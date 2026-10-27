/* API 53: the text spotter in the actual game worker (game_ocr/main.c) and core adapter (core/vocr_ocr.c), with
 * the committed random VTSP version-4 blob as its pinned weights (host/fixtures/spotter, test_spot_pins.h), the
 * real spotter runtime and textspot7's post-pass, and the resident test's synthetic recognizer with a 4 KiB arena
 * (test_spot_models.h) and spies for the recognizer and GRAD (so a job's lines tell which finder found them). A
 * request with VJO_GAME_OCR_S_SPOT reads the spotter's weights through the kernel's broker (VJO_GAME_OCR_OPEN_SPOT)
 * into a block of their own (VjoGameVocrSpot), checked, kept for the next job, given back by a job without the
 * spotter; it reads the game's whole frame through the kernel's rows (VJO_ROWS_FRAME) beside the region's; any
 * failure (missing, wrong, no block, no room for its scratch) reads with GRAD. No inference, accuracy or console
 * claim: random weights, and spies. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "vocr_textspot6.h"
#include "vocr_textspot7.h"
#include "test_resident_weights.h"
#include "../../game_ocr/main.c"

#ifndef VJO_SOURCE_DIR
#error "VJO_SOURCE_DIR: the source tree (its host/fixtures)"
#endif
#define FW 960u
#define FH 544u
#define RX 54u
#define RY 380u
#define W 852u
#define H 60u /* the text box's first line: a region where the spotter's part is pages over GRAD's scratch */
#define TB_H 135u
static unsigned char frame[FW * FH]; /* the game's frame: the text box at (RX, RY) on a pattern */
static unsigned frame_rows_asked, frame_rows_fail, frame_rows_armed_busy, region_x = RX, region_y = RY, region_w = W,
    region_h = H;
static unsigned char spot_blob[VJO_VOCR_SPOT_BYTES];
static unsigned char job_memory[1024 * 1024] __attribute__((aligned(64)));
static unsigned char weights_memory[TEST_FL10_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char spot_memory[64 * 1024] __attribute__((aligned(64)));
static unsigned char capture_mem_block[64 * 1024] __attribute__((aligned(64)));
static unsigned char line_mem_block[FW * FH + 4096] __attribute__((aligned(64)));
static int64_t available;
static unsigned live_bytes;
static struct { int live, fail; unsigned allocs, frees, size; } blocks[16]; /* by uid */
static uint64_t clock_value, mtime, spot_mtime;
static int file_open_now, spot_missing, spot_corrupt, spot_size_delta;
static uint32_t opened_which;
static unsigned opens, spot_opens, spot_reads, model_reads, grad_calls, runs;
static VjoGameOcrRequest current;

/* ---- the Vita SDK and kernel transport, as the worker sees them ---- */
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ out->size_user = (int)(available - live_bytes); return 0; }
static SceUID uid_of(const char *name)
{
    return !strcmp(name, "VjoGameVocr") ? 9 : !strcmp(name, "VjoGameVocrWeights") ? 10 :
           !strcmp(name, "VjoGameVocrCapture") ? 11 : !strcmp(name, "VjoGameVocrSpot") ? 14 :
           !strcmp(name, "VjoGameVocrLine") ? 15 : -1;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    SceUID uid = uid_of(name);
    (void)opts;
    TEST_CHECK(!(size & 4095));
    TEST_ASSERT(uid > 0);
    TEST_CHECK(!blocks[uid].live); /* never two of one */
    /* the spotter's and the weights' blocks are USER_RW only */
    if (uid == 14 || uid == 10) TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    blocks[uid].allocs++;
    if (blocks[uid].fail || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) return (int)0x80024302u;
    blocks[uid].live = 1;
    blocks[uid].size = size;
    live_bytes += size;
    return uid;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_ASSERT(uid > 0 && uid < 16 && blocks[uid].live);
    *base = uid == 9 ? (void *)job_memory : uid == 10 ? (void *)weights_memory : uid == 11 ? (void *)capture_mem_block
          : uid == 14 ? (void *)spot_memory : (void *)line_mem_block;
    TEST_CHECK(blocks[uid].size <= (uid == 9 ? sizeof(job_memory) : uid == 10 ? sizeof(weights_memory)
                                    : uid == 11 ? sizeof(capture_mem_block) : uid == 14 ? sizeof(spot_memory)
                                    : sizeof(line_mem_block)));
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    TEST_ASSERT(uid > 0 && uid < 16 && blocks[uid].live);
    blocks[uid].live = 0;
    blocks[uid].frees++;
    live_bytes -= blocks[uid].size;
    return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu,
                             void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return 0; }
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelDelayThread(unsigned us) { clock_value += us; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return (int)0x80010013u; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return 0; }
int vjoOcrTake(VjoGameOcrRequest *out) { (void)out; return 1; }
int vjoOcrCancelled(uint32_t seq) { (void)seq; return 0; }
int vjoOcrComplete(const VjoGameOcrResult *out) { (void)out; return 0; }
/* The kernel's rows: the region's grey, straight into the worker's capture block; API 53, the whole frame's
 * (VJO_ROWS_FRAME), for a request with the spotter only, into its part of the workspace. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->seq == 12 && req->n);
    if (req->flags & VJO_ROWS_FRAME) {
        TEST_CHECK(req->flags == VJO_ROWS_FRAME && (current.speed & VJO_GAME_OCR_S_SPOT) && req->y < FH &&
                   req->n <= FH - req->y && req->dst_stride >= FW && blocks[9].live);
        frame_rows_asked += req->n;
        if (frame_rows_fail) return (int)0x80010005u; /* a native error: the job's (VJO_E_SOURCE) */
        if (frame_rows_armed_busy) { /* the hook's copy outlived the call: it may still write dst */
            info->armed = 1;
            return VJO_ERR_BUSY;
        }
        for (unsigned r = 0; r < req->n; r++)
            memcpy((unsigned char *)dst + (size_t)r * req->dst_stride, frame + (size_t)(req->y + r) * FW, FW);
    } else {
        TEST_CHECK(req->y < region_h && req->n <= region_h - req->y && blocks[11].live);
        for (unsigned r = 0; r < req->n; r++)
            memcpy((unsigned char *)dst + (size_t)r * req->dst_stride,
                   frame + (size_t)(region_y + req->y + r) * FW + region_x, region_w);
    }
    info->fb_width = FW; info->fb_height = FH; info->fb_pitch = FW;
    return (int)req->n;
}
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { return -1; }
/* The broker: the request's model (the synthetic FL10) or, for a request with the spotter, its pinned weights. */
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    uint32_t size;
    TEST_CHECK(seq == 12 && !file_open_now);
    TEST_ASSERT(which == VJO_GAME_OCR_OPEN_MODEL || which == VJO_GAME_OCR_OPEN_SPOT);
    if (which == VJO_GAME_OCR_OPEN_SPOT) {
        TEST_CHECK(current.speed & VJO_GAME_OCR_S_SPOT); /* the kernel opens it for such a request only */
        spot_opens++;
        if (spot_missing) return (int)0x80010002u;
        size = VJO_VOCR_SPOT_BYTES + (uint32_t)spot_size_delta;
        *info = (VjoGameOcrModelInfo){sizeof(*info), size, 0, 0, spot_mtime};
    } else {
        opens++;
        size = TEST_FL10_BYTES;
        *info = (VjoGameOcrModelInfo){sizeof(*info), size, 0, 0, mtime};
    }
    opened_which = which;
    file_open_now = 1;
    return (int)size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && file_open_now && n && n <= VJO_GAME_OCR_MODEL_CHUNK);
    for (uint32_t i = 0; i < n; i++)
        ((unsigned char *)dst)[i] = opened_which == VJO_GAME_OCR_OPEN_SPOT
                                    ? spot_blob[off + i] ^ (spot_corrupt && off + i == 200 ? 1 : 0)
                                    : test_weight_byte(off + i, TEST_FL10_SEED);
    if (opened_which == VJO_GAME_OCR_OPEN_SPOT) spot_reads++;
    else model_reads++;
    return (int)n;
}
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src)
{ (void)seq; (void)off; (void)bytes; (void)src; TEST_CHECK(0); return -1; }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { (void)seq; (void)bytes; TEST_CHECK(0); return -1; }
int vjoOcrModelClose(uint32_t seq) { TEST_CHECK(seq == 12); file_open_now = 0; return 0; }

/* ---- the recognizer and GRAD: spies (the spotter is the real runtime's) ---- */
int test_vocr_find_lines_rows(const VocrLinesRows *src, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    (void)src; (void)mode; (void)arena; (void)bytes;
    grad_calls++;
    memset(out, 0, sizeof(*out));
    out->count = 1;
    out->lines[0] = (VocrLine){10, 40, 400, 30, 0};
    return VOCR_LINES_OK;
}
/* API 50: the job's finder is the fixed one (vita-vn-ocr #129): the same spy */
int test_vocr_find_lines_fix_rows(const VocrLinesRows *src, int mode, unsigned fix, void *arena, size_t bytes,
                                  VocrLineSet *out)
{
    (void)fix;
    return test_vocr_find_lines_rows(src, mode, arena, bytes, out);
}
int test_vocr_load(vocr_model *m, const void *blob, size_t bytes)
{
    TEST_CHECK(blob == (const void *)weights_memory && bytes == TEST_FL10_BYTES);
    memset(m, 0, sizeof(*m));
    m->blob = blob;
    m->bytes = bytes;
    return VOCR_OK;
}
size_t test_vocr_arena_bytes(const vocr_model *m, const vocr_opts *o) { (void)m; (void)o; return 4096; }
int test_vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes)
{ (void)c; (void)m; (void)o; (void)arena; (void)bytes; return VOCR_OK; }
int test_vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
{
    (void)c;
    runs++;
    snprintf(text, cap, "猫");
    extra->width = img->width;
    extra->frames = img->width / 8;
    return VOCR_OK;
}

/* ---- helpers ---- */
static void load_fixtures(void)
{
    char head[16];
    static unsigned char tb[W * TB_H];
    FILE *f = fopen(VJO_SOURCE_DIR "/host/fixtures/spotter/" VJO_VOCR_SPOT_NAME, "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(spot_blob, 1, sizeof(spot_blob), f) == sizeof(spot_blob));
    fclose(f);
    f = fopen(VJO_SOURCE_DIR "/host/fixtures/short_line/textbox.pgm", "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(head, 1, 15, f) == 15 && !memcmp(head, "P5\n852 135\n255\n", 15));
    TEST_ASSERT(fread(tb, 1, sizeof(tb), f) == sizeof(tb));
    fclose(f);
    for (unsigned y = 0; y < FH; y++)
        for (unsigned x = 0; x < FW; x++) frame[y * FW + x] = (unsigned char)((x * 3u + y * 5u) & 0x7Fu);
    for (unsigned y = 0; y < TB_H; y++) memcpy(frame + (size_t)(RY + y) * FW + RX, tb + (size_t)y * W, W);
}
/* The runtime's own lines for the region: the spotter on the frame in memory (its dark flags), its horizontal boxes
 * in its order through textspot7's post-pass (decode D) on the region in memory, at most 16. */
static unsigned runtime_lines(void)
{
    static unsigned char arena[256 * 1024], post[8 * 1024];
    static VocrT7Box fb[VOCR_T7_MAX_BOXES], rb[VOCR_T7_MAX_BOXES];
    vocr_textspot6 m;
    VocrTextspot6Set s;
    VocrLinesImage im = {frame, FW, FH, FW, VOCR_LINES_GREY};
    VocrLinesImage reg = {frame + (size_t)region_y * FW + region_x, region_w, region_h, FW, VOCR_LINES_GREY};
    unsigned n = 0, m_out = 0;
    TEST_ASSERT(vocr_textspot6_load(&m, spot_blob, sizeof(spot_blob)) == VOCR_LINES_OK);
    size_t a = vocr_textspot6_frame_arena_bytes(&m, FW, FH);
    TEST_ASSERT(a && a <= sizeof(arena));
    TEST_ASSERT(vocr_textspot6_find(&m, &im, 0, arena, a, &s) == VOCR_LINES_OK);
    for (unsigned i = 0; i < s.count; i++)
        if (!s.boxes[i].vertical)
            fb[n++] = (VocrT7Box){s.boxes[i].x, s.boxes[i].y, s.boxes[i].w, s.boxes[i].h, s.boxes[i].conf,
                                  s.boxes[i].dark};
    TEST_ASSERT(vocr_textspot7_post(fb, n, (int)region_x, (int)region_y, (int)(region_x + region_w),
                                    (int)(region_y + region_h), &reg, VJO_VOCR_SPOT_POST, post, sizeof(post), rb,
                                    &m_out) == VOCR_T7_OK);
    return m_out < VJO_VOCR_MAX_LINES ? m_out : VJO_VOCR_MAX_LINES;
}
static void reset(void)
{
    if (blocks[14].live || spot_res.data) TEST_CHECK(!drop_spot());
    if (blocks[10].live || resident.data) TEST_CHECK(!drop_weights());
    worker = workspace_uid = weights_uid = capture_uid = line_uid = lm_uid = cascade_uid = spot_uid = -1;
    memset(&resident, 0, sizeof(resident));
    resident.alloc = weights_alloc;
    resident.release = weights_release;
    memset(&spot_res, 0, sizeof(spot_res));
    spot_res.alloc = spot_alloc;
    spot_res.release = spot_release;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = stat_error = 0;
    memset(blocks, 0, sizeof(blocks));
    available = 64 * 1024 * 1024;
    live_bytes = 0;
    mtime = 0x1234; spot_mtime = 0x5678;
    file_open_now = spot_missing = spot_corrupt = spot_size_delta = 0;
    frame_rows_asked = frame_rows_fail = frame_rows_armed_busy = 0;
    hook_owns_frame = hook_owns_capture = hook_owns_workspace = 0;
    region_x = RX; region_y = RY; region_w = W; region_h = H;
    opens = spot_opens = spot_reads = model_reads = grad_calls = runs = 0;
    clock_value = 0;
}
static VjoGameOcrRequest request(uint32_t speed, uint32_t flags)
{
    VjoGameOcrRequest r = {.size = sizeof(r), .seq = 12, .capture_seq = 11, .flags = flags, .speed = speed,
                           .fb_width = FW, .fb_height = FH, .x = region_x, .y = region_y, .width = region_w,
                           .height = region_h};
    snprintf(r.model, sizeof(r.model), "FL10_w8.vocr");
    snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
    current = r;
    return r;
}
static int run_flags(uint32_t speed, uint32_t flags, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(speed, flags);
    int rc = vjo_game_ocr_run_request(&r, out);
    /* between jobs: no job block, no file, and a spotter block only for verified weights */
    TEST_CHECK(!blocks[9].live && !blocks[11].live && !file_open_now && !model_owned && !out->cleanup_status);
    TEST_CHECK(blocks[14].live == (spot_res.verified != 0));
    return rc;
}
static int run(uint32_t speed, VjoGameOcrResult *out) { return run_flags(speed, 0, out); }
static const VjoGameOcrStep *step_of(const VjoGameOcrResult *out, unsigned id)
{
    for (uint32_t i = 0; i < out->n_steps; i++)
        if (out->step[i].id == id) return &out->step[i];
    return NULL;
}
static uint32_t spot_block(void) { return PAGE_ROUND(vjo_vocr_spot_block_bytes()); }

/* ---- tests ---- */
static void a_spot_job_finds_its_lines_with_the_spotter(void)
{
    VjoGameOcrResult out;
    const VjoGameOcrStep *s;
    load_fixtures();
    reset();
    unsigned want = runtime_lines();
    TEST_ASSERT(want > 0);
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && out.text[0]);
    TEST_CHECK_(out.lines == want && !grad_calls && runs == want, "%u lines, the runtime's %u", out.lines, want);
    TEST_CHECK(spot_opens == 1 && spot_reads > 0 && blocks[14].allocs == 1 && blocks[14].size == spot_block());
    TEST_CHECK(spot_res.verified && vjo_ledger_current(&g_game_ledger, GL_SPOT) == spot_block());
    /* the whole frame was read through the kernel's frame rows: its one pass, then the kept boxes' rows */
    TEST_CHECK_(frame_rows_asked >= FH, "%u frame rows", frame_rows_asked);
    /* its scratch: the job's workspace with the spotter's part (larger than this pin's 4 KiB recognizer arena) */
    size_t spot = vjo_vocr_spot_bytes(&spot_res, W, H, FW, FH);
    TEST_CHECK(spot && out.workspace_bytes == vjo_vocr_job_bytes_spot("FL10_w8.vocr", W, H, spot));
    TEST_CHECK(out.workspace_bytes > vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    s = step_of(&out, VJO_GSTEP_SPOT);
    TEST_ASSERT(s != NULL);
    TEST_CHECK(s->kind == 1 && s->arg == want && !s->rc && (s->bytes & VJO_GLM_READ) && !(s->bytes & VJO_GLM_RESIDENT));
    TEST_CHECK((s->bytes & 0x3FFFFFFFu) == spot && (s->sum & 0x7Fu) >= want && ((s->sum >> 8) & 0xFFu) >= 10 &&
               s->us > 0);
    /* the next job: the weights kept (opened, not read), the same block */
    unsigned reads = spot_reads;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && out.lines == want && !grad_calls);
    s = step_of(&out, VJO_GSTEP_SPOT);
    TEST_ASSERT(s != NULL);
    TEST_CHECK(s->kind == 1 && (s->bytes & VJO_GLM_RESIDENT) && !(s->bytes & VJO_GLM_READ));
    TEST_CHECK(spot_reads == reads && spot_opens == 2 && blocks[14].allocs == 1 && !blocks[14].frees);
    /* the file replaced: read again into the same block */
    spot_mtime++;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && (step_of(&out, VJO_GSTEP_SPOT)->bytes & VJO_GLM_READ));
    TEST_CHECK(spot_reads > reads && blocks[14].allocs == 1);
    /* a job without the spotter gives its block back, reads with GRAD, reads no frame row and logs no spotter step */
    frame_rows_asked = 0;
    TEST_CHECK(!run(0, &out) && grad_calls == 1 && out.lines == 1 && !step_of(&out, VJO_GSTEP_SPOT));
    TEST_CHECK(!blocks[14].live && blocks[14].frees == 1 && !spot_res.verified && spot_opens == 3 && !frame_rows_asked);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H) && !vjo_ledger_current(&g_game_ledger, GL_SPOT));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* A region that is the whole frame is the spotter's frame: no frame rows of their own, its lines the runtime's. */
static void a_whole_frame_region_is_read_once(void)
{
    VjoGameOcrResult out;
    load_fixtures();
    reset();
    region_x = region_y = 0;
    region_w = FW;
    region_h = FH;
    unsigned want = runtime_lines();
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && !grad_calls && out.lines == want && !frame_rows_asked);
    TEST_CHECK(step_of(&out, VJO_GSTEP_SPOT)->kind == 1 && !((step_of(&out, VJO_GSTEP_SPOT)->sum >> 8) & 0xFFu));
    TEST_CHECK(out.workspace_bytes ==
               vjo_vocr_job_bytes_spot("FL10_w8.vocr", FW, FH, vjo_vocr_spot_bytes(&spot_res, FW, FH, FW, FH)));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

static void a_missing_or_wrong_spotter_reads_with_grad(void)
{
    VjoGameOcrResult out;
    const VjoGameOcrStep *s;
    load_fixtures();
    /* missing: GRAD, no block, no frame row, the job's own model read as always */
    reset();
    spot_missing = 1;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && out.text[0] && grad_calls == 1 && out.lines == 1);
    TEST_CHECK(!blocks[14].allocs && opens == 1 && model_reads > 0 && !out.io_error && !frame_rows_asked);
    s = step_of(&out, VJO_GSTEP_SPOT);
    TEST_ASSERT(s != NULL);
    TEST_CHECK(!s->kind && !s->arg && s->rc == VJO_E_OCR_MODEL && !(s->bytes & (VJO_GLM_READ | VJO_GLM_RESIDENT)));
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    /* another size: never the pin */
    reset();
    spot_size_delta = 4;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && grad_calls == 1 && !blocks[14].allocs);
    TEST_CHECK(step_of(&out, VJO_GSTEP_SPOT)->rc == VJO_E_OCR_MODEL);
    /* another hash: read, refused, its block given back */
    reset();
    spot_corrupt = 1;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && grad_calls == 1 && blocks[14].allocs == 1 && !blocks[14].live);
    TEST_CHECK(step_of(&out, VJO_GSTEP_SPOT)->rc == VJO_E_OCR_MODEL && !spot_res.verified);
    /* verified, then the card's file goes bad: the next job reads with GRAD and gives the block back */
    reset();
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && !grad_calls && blocks[14].live);
    spot_mtime++;
    spot_corrupt = 1;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && grad_calls == 1 && !blocks[14].live);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

static void no_memory_for_the_spotter_reads_with_grad(void)
{
    VjoGameOcrResult out;
    load_fixtures();
    /* its block refused by the system */
    reset();
    blocks[14].fail = 1;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && grad_calls == 1 && blocks[14].allocs == 1);
    TEST_CHECK(step_of(&out, VJO_GSTEP_SPOT)->rc == VJO_E_OOM && !spot_res.verified && !spot_res.data);
    /* room for the job with GRAD, a page short of its scratch with the spotter: GRAD, and the block given back
     * after the check (which counted it as the job's to give back: the tier is never stepped down for it) */
    reset();
    const uint32_t capture = PAGE_ROUND(vjo_vocr_capture_bytes(W, H));
    const uint32_t grad = PAGE_ROUND(vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    TEST_ASSERT(!run(VJO_GAME_OCR_S_SPOT, &out) && spot_res.verified); /* the spotter's sizes, read once */
    const uint32_t with = PAGE_ROUND(out.workspace_bytes), weights = PAGE_ROUND(TEST_FL10_BYTES);
    TEST_ASSERT(with > grad);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !blocks[14].live && !blocks[10].live);
    reset();
    available = GAME_RESERVE + grad + capture + weights + spot_block() + (with - grad) - 4096;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && grad_calls == 1 && out.lines == 1 && !frame_rows_asked);
    TEST_CHECK(step_of(&out, VJO_GSTEP_SPOT)->rc == VJO_E_OOM && !blocks[14].live && blocks[14].frees == 1);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H) && !out.fallback);
    /* a page more: the spotter's scratch fits, and it finds the lines */
    reset();
    available = GAME_RESERVE + grad + capture + weights + spot_block() + (with - grad);
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && !grad_calls && step_of(&out, VJO_GSTEP_SPOT)->kind == 1);
    TEST_CHECK(blocks[14].live && out.release_bytes == 0);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* GRAD's refinement is GRAD's: a job whose spotter finds the lines reads its boxes as the post-pass gave them (no
 * refine step), whatever the refinement's mode; the mode applies to GRAD's lines as before. */
static void the_spotters_boxes_are_not_refined(void)
{
    VjoGameOcrResult out;
    load_fixtures();
    reset();
    unsigned want = runtime_lines();
    TEST_CHECK(!run_flags(VJO_GAME_OCR_S_SPOT, VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT, &out));
    TEST_CHECK(out.lines == want && !grad_calls && !step_of(&out, VJO_GSTEP_REFINE));
    spot_missing = 1; /* GRAD's path: refined */
    TEST_CHECK(!run_flags(VJO_GAME_OCR_S_SPOT, VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT, &out));
    TEST_CHECK(grad_calls == 1 && step_of(&out, VJO_GSTEP_REFINE) != NULL);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* A frame row the kernel cannot give is the job's error, as a region row's is: no GRAD reading after it. */
static void a_frame_row_that_cannot_be_read_is_the_jobs_error(void)
{
    VjoGameOcrResult out;
    load_fixtures();
    reset();
    frame_rows_fail = 1;
    TEST_CHECK(run(VJO_GAME_OCR_S_SPOT, &out) == VJO_E_SOURCE && !grad_calls && !runs && frame_rows_asked);
    TEST_CHECK(!out.text[0] && blocks[14].live && spot_res.verified); /* its weights stay for the next job */
    frame_rows_fail = 0;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && !grad_calls && out.lines == runtime_lines());
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* A frame-row copy that outlived its call (BUSY with the request armed): the display hook may still write the
 * frame's strip, which lies in the job's workspace, so the workspace is never freed (it goes with the process) and
 * the worker takes no more jobs, as for the region's capture block; the capture block itself is freed. */
static void a_frame_copy_that_outlives_its_call_keeps_the_workspace(void)
{
    VjoGameOcrResult out;
    load_fixtures();
    reset();
    frame_rows_armed_busy = 1;
    VjoGameOcrRequest r = request(VJO_GAME_OCR_S_SPOT, 0);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) != VJO_OK && !out.text[0]);
    TEST_CHECK(blocked && hook_owns_frame && !hook_owns_capture && blocks[9].live && !blocks[11].live && !grad_calls);
    TEST_CHECK(!file_open_now && !model_owned);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && blocks[9].live); /* never freed: the hook may still write it */
    blocks[9].live = 0; /* (the process's exit takes it) */
    live_bytes -= blocks[9].size;
    hook_owns_frame = 0;
    blocked = 0;
}

static void the_worker_stop_and_a_refused_job_give_the_block_back(void)
{
    VjoGameOcrResult out;
    load_fixtures();
    reset();
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && blocks[14].live);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !blocks[14].live && !spot_res.verified);
    /* a job the game has no memory for: everything resident goes back to the game, the spotter's too */
    reset();
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && blocks[14].live && blocks[10].live);
    available = GAME_RESERVE + 64 * 1024;
    TEST_CHECK(run(VJO_GAME_OCR_S_SPOT, &out) == VJO_E_OOM && !blocks[14].live && !blocks[10].live);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

TEST_LIST = {
    {"a job with the spotter reads its boxes, keeps its weights and gives them back", a_spot_job_finds_its_lines_with_the_spotter},
    {"a region that is the whole frame is the spotter's frame", a_whole_frame_region_is_read_once},
    {"a missing or wrong spotter reads with GRAD", a_missing_or_wrong_spotter_reads_with_grad},
    {"no memory for the spotter reads with GRAD", no_memory_for_the_spotter_reads_with_grad},
    {"the spotter's boxes are not refined", the_spotters_boxes_are_not_refined},
    {"a frame row that cannot be read is the job's error", a_frame_row_that_cannot_be_read_is_the_jobs_error},
    {"a frame copy that outlives its call keeps the workspace", a_frame_copy_that_outlives_its_call_keeps_the_workspace},
    {"the worker's stop and a refused job give the spotter's block back", the_worker_stop_and_a_refused_job_give_the_block_back},
    {NULL, NULL}
};
