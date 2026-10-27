/* API 54: the text spotter in the actual game worker (game_ocr/main.c) and core adapter (core/vocr_ocr.c), with
 * the committed random VTSP version-4 blob as its pinned weights (host/fixtures/spotter, test_spot_pins.h), the
 * real spotter runtime, and the resident test's synthetic recognizer with a 4 KiB arena (test_spot_models.h) and
 * spies for the recognizer and GRAD (so a job's lines tell which finder found them). A request with
 * VJO_GAME_OCR_S_SPOT reads the spotter's weights through the kernel's broker (VJO_GAME_OCR_OPEN_SPOT) into a block
 * of their own (VjoGameVocrSpot), checked, kept for the next job, given back by a job without the spotter; any
 * failure (missing, wrong, no block, no room for its scratch) reads with GRAD. No inference, accuracy or console
 * claim: random weights, and spies. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "vocr_textspot6.h"
#include "test_resident_weights.h"
#include "../../game_ocr/main.c"

#ifndef VJO_SOURCE_DIR
#error "VJO_SOURCE_DIR: the source tree (its host/fixtures)"
#endif
#define W 852u
#define H 60u /* the text box's first line: a region where the spotter's arena is pages over GRAD's */
#define TB_H 135u
static unsigned char grey[W * H];
static unsigned char spot_blob[VJO_VOCR_SPOT_BYTES];
static unsigned char job_memory[1024 * 1024] __attribute__((aligned(64)));
static unsigned char weights_memory[TEST_FL10_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char spot_memory[64 * 1024] __attribute__((aligned(64)));
static unsigned char capture_mem_block[64 * 1024] __attribute__((aligned(64)));
static unsigned char line_mem_block[W * H + 4096] __attribute__((aligned(64)));
static int64_t available;
static unsigned live_bytes;
static struct { int live, fail; unsigned allocs, frees, size; } blocks[16]; /* by uid */
static uint64_t clock_value, mtime, spot_mtime;
static int file_open_now, spot_missing, spot_corrupt, spot_size_delta;
static uint32_t opened_which;
static unsigned opens, spot_opens, spot_reads, model_reads, grad_calls, runs;
static int grad_vertical; /* GRAD's spy returns rows auto's rule takes for vertical text (6 narrow rows stacked) */
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
/* The kernel's rows: the frame's grey, straight into the worker's capture block. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->seq == 12 && req->y < H && req->n && req->n <= H - req->y && blocks[11].live);
    for (unsigned r = 0; r < req->n; r++) memcpy((unsigned char *)dst + (size_t)r * req->dst_stride, grey + (req->y + r) * W, W);
    info->fb_width = 960; info->fb_height = 544; info->fb_pitch = 960;
    return (int)req->n;
}
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { return -1; }
/* The broker: the request's model (the synthetic FL10) or, for a request with the spotter, its pinned weights. */
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    uint32_t size;
    TEST_CHECK(seq == 12 && !file_open_now);
    /* (the vertical model: auto's columns try, test_spot_models.h's synthetic VOCR5-V with FL10's bytes) */
    TEST_ASSERT(which == VJO_GAME_OCR_OPEN_MODEL || which == VJO_GAME_OCR_OPEN_SPOT || which == VJO_GAME_OCR_OPEN_VERTICAL);
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
    if (grad_vertical) { /* auto's rule (vjo_vocr_rows_vertical): 5 or more narrow rows over one span */
        out->count = 6;
        for (unsigned k = 0; k < 6; k++) out->lines[k] = (VocrLine){100, 10 * k, 30, 9, 0};
        return VOCR_LINES_OK;
    }
    out->count = 1;
    out->lines[0] = (VocrLine){10, 40, 400, 30, 0};
    return VOCR_LINES_OK;
}
/* API 50: the job's GRAD is the fixed finder (vita-vn-ocr #129, VjoVocrPolicy.line_fix): the same spy */
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
    memcpy(grey, tb, sizeof(grey));
}
/* The runtime's own horizontal boxes on the frame, read halved (a region of a 960 x 544 frame), at most 16. */
static unsigned runtime_lines(void)
{
    static unsigned char arena[256 * 1024];
    vocr_textspot6 m;
    VocrTextspot6Set s;
    VocrLinesImage im = {grey, W, H, W, VOCR_LINES_GREY};
    unsigned n = 0;
    TEST_ASSERT(vocr_textspot6_load(&m, spot_blob, sizeof(spot_blob)) == VOCR_LINES_OK);
    size_t a = vocr_textspot6_arena_bytes_flags(&m, W, H, VOCR_TEXTSPOT6_SCALE2);
    TEST_ASSERT(a && a <= sizeof(arena));
    TEST_ASSERT(vocr_textspot6_find(&m, &im, VOCR_TEXTSPOT6_SCALE2, arena, a, &s) == VOCR_LINES_OK);
    for (unsigned i = 0; i < s.count; i++) n += !s.boxes[i].vertical;
    return n < VJO_VOCR_MAX_LINES ? n : VJO_VOCR_MAX_LINES;
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
    opens = spot_opens = spot_reads = model_reads = grad_calls = runs = 0;
    grad_vertical = 0;
    clock_value = 0;
}
static VjoGameOcrRequest request(uint32_t speed, uint32_t flags)
{
    VjoGameOcrRequest r = {.size = sizeof(r), .seq = 12, .capture_seq = 11, .flags = flags, .speed = speed,
                           .fb_width = 960, .fb_height = 544, .x = 54, .y = 380, .width = W, .height = H};
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
/* The runtime's boxes of each orientation on the frame, read halved (the spotter's hand-over counts them all). */
static void spot_counts(unsigned *vertical, unsigned *horizontal)
{
    static unsigned char arena[256 * 1024];
    vocr_textspot6 m;
    VocrTextspot6Set set;
    VocrLinesImage im = {grey, W, H, W, VOCR_LINES_GREY};
    TEST_ASSERT(vocr_textspot6_load(&m, spot_blob, sizeof(spot_blob)) == VOCR_LINES_OK);
    size_t a = vocr_textspot6_arena_bytes_flags(&m, W, H, VOCR_TEXTSPOT6_SCALE2);
    TEST_ASSERT(a && a <= sizeof(arena));
    TEST_ASSERT(vocr_textspot6_find(&m, &im, VOCR_TEXTSPOT6_SCALE2, arena, a, &set) == VOCR_LINES_OK);
    *vertical = *horizontal = 0;
    for (unsigned i = 0; i < set.count; i++) {
        if (set.boxes[i].vertical) (*vertical)++;
        else (*horizontal)++;
    }
}
static unsigned find_steps(const VjoGameOcrResult *out, unsigned kind)
{
    unsigned n = 0;
    for (uint32_t i = 0; i < out->n_steps; i++) n += out->step[i].id == VJO_GSTEP_FIND && out->step[i].kind == kind;
    return n;
}

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
    /* its scratch: the job's workspace with the spotter's part (larger than this pin's 4 KiB recognizer arena) */
    size_t spot = vjo_vocr_spot_bytes(&spot_res, W, H, 960, 544);
    TEST_CHECK(spot && out.workspace_bytes == vjo_vocr_job_bytes_spot("FL10_w8.vocr", W, H, spot));
    TEST_CHECK(out.workspace_bytes > vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    s = step_of(&out, VJO_GSTEP_SPOT);
    TEST_ASSERT(s != NULL);
    TEST_CHECK(s->kind == 1 && s->arg == want && !s->rc && (s->bytes & VJO_GLM_READ) && !(s->bytes & VJO_GLM_RESIDENT));
    TEST_CHECK((s->bytes & 0x3FFFFFFFu) == spot && (s->sum & 0x7FFFu) >= want && s->us > 0);
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
    /* a job without the spotter gives its block back, reads with GRAD and logs no spotter step */
    TEST_CHECK(!run(0, &out) && grad_calls == 1 && out.lines == 1 && !step_of(&out, VJO_GSTEP_SPOT));
    TEST_CHECK(!blocks[14].live && blocks[14].frees == 1 && !spot_res.verified && spot_opens == 3);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H) && !vjo_ledger_current(&g_game_ledger, GL_SPOT));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

static void a_missing_or_wrong_spotter_reads_with_grad(void)
{
    VjoGameOcrResult out;
    const VjoGameOcrStep *s;
    load_fixtures();
    /* missing: GRAD, no block, the job's own model read as always */
    reset();
    spot_missing = 1;
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && out.text[0] && grad_calls == 1 && out.lines == 1);
    TEST_CHECK(!blocks[14].allocs && opens == 1 && model_reads > 0 && !out.io_error);
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
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && grad_calls == 1 && out.lines == 1);
    TEST_CHECK(step_of(&out, VJO_GSTEP_SPOT)->rc == VJO_E_OOM && !blocks[14].live && blocks[14].frees == 1);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H) && !out.fallback);
    /* a page more: the spotter's scratch fits, and it finds the lines */
    reset();
    available = GAME_RESERVE + grad + capture + weights + spot_block() + (with - grad);
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT, &out) && !grad_calls && step_of(&out, VJO_GSTEP_SPOT)->kind == 1);
    TEST_CHECK(blocks[14].live && out.release_bytes == 0);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

static void its_refinement_follows_its_own_bit(void)
{
    VjoGameOcrResult out;
    const VjoGameOcrStep *r;
    load_fixtures();
    reset();
    unsigned want = runtime_lines();
    /* GRAD's refinement asked for (bits 12-13 on), the spotter's not: its boxes as found, no refine step */
    TEST_CHECK(!run_flags(VJO_GAME_OCR_S_SPOT, VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT, &out));
    TEST_CHECK(out.lines == want && !step_of(&out, VJO_GSTEP_REFINE));
    /* the spotter's on (VJO_GAME_OCR_S_SPOT_REFINE): every box refined, its step's kind ON */
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT | VJO_GAME_OCR_S_SPOT_REFINE, &out));
    r = step_of(&out, VJO_GSTEP_REFINE);
    TEST_ASSERT(r != NULL);
    TEST_CHECK(r->kind == VJO_GAME_OCR_REFINE_ON && (r->bytes >> 16) == want);
    /* the bit alone, without the spotter: GRAD's job, as without it */
    TEST_CHECK(!run(VJO_GAME_OCR_S_SPOT_REFINE, &out) && grad_calls == 1 && !step_of(&out, VJO_GSTEP_REFINE));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
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

/* audit-vocr6-spotter2-rubi F1: ocr_vertical = on has no spotter. On's job is GRAD's rows and on's check (#379's
 * measured rule): the spotter's weights are not opened, no block is held or taken, no spot step is logged, and the
 * job (here GRAD's one horizontal row, read) is the one without the spotter's bit, byte for byte in its text and its
 * workspace. */
static void on_reads_without_the_spotter(void)
{
    VjoGameOcrResult with, without;
    const uint32_t on = (uint32_t)VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    load_fixtures();
    reset();
    TEST_CHECK(run(on, &without) == VJO_OK);
    const unsigned grad_without = grad_calls, block_on = blocks[9].size;
    reset();
    TEST_CHECK(run(on | VJO_GAME_OCR_S_SPOT, &with) == VJO_OK);
    TEST_CHECK_(!spot_opens && !spot_reads && !blocks[14].allocs && !blocks[14].live, "spot opens %u, block allocs %u",
                spot_opens, blocks[14].allocs);
    TEST_CHECK(!step_of(&with, VJO_GSTEP_SPOT) && !step_of(&without, VJO_GSTEP_SPOT));
    TEST_CHECK_(with.workspace_bytes == without.workspace_bytes && blocks[9].size == block_on,
                "workspace %u against %u, its block %u against %u", with.workspace_bytes, without.workspace_bytes,
                blocks[9].size, block_on);
    TEST_CHECK(!strcmp(with.text, without.text) && with.text[0] && grad_calls == grad_without && with.lines == 1);
    /* a block an earlier job of rows kept is given back by on's job (the spotter has no part in it) */
    reset();
    TEST_CHECK(run(VJO_GAME_OCR_S_SPOT, &with) == VJO_OK && blocks[14].live);
    TEST_CHECK(run(on | VJO_GAME_OCR_S_SPOT, &with) == VJO_OK && !blocks[14].live && !spot_res.verified);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* audit-vocr6-spotter2-rubi F2, F3: auto on a region whose spotter boxes are mostly vertical (the fixture blob on
 * this region) and whose GRAD rows auto's rule takes for vertical text. The rows' try hands over (the spotter's
 * VJO_E_VERTICAL), auto's check sends the job to its columns try, which reads the region's columns with the vertical
 * model (synthetic) and never runs the spotter: its workspace is the one without the spotter's bit, the spotter's
 * block is kept for the next job of rows, and the job's spot step says why GRAD's rows decided (VJO_E_VERTICAL, the
 * weights read in this job). The next job of rows reads none of them again. */
static void autos_columns_try_keeps_the_block_and_says_why(void)
{
    VjoGameOcrResult with, without;
    const uint32_t autov = (uint32_t)VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    unsigned v = 0, h = 0;
    load_fixtures();
    spot_counts(&v, &h);
    TEST_ASSERT_(v > h, "the fixture's boxes on this region: %u vertical, %u horizontal", v, h);
    reset();
    grad_vertical = 1;
    TEST_CHECK(run(autov, &without) == VJO_OK);
    TEST_CHECK(find_steps(&without, 2) == 1); /* auto's hand-over to the columns' try (GRAD's rows) */
    const unsigned block_without = blocks[9].size; /* the workspace block the columns' try allocated */
    reset();
    grad_vertical = 1;
    TEST_CHECK(run(autov | VJO_GAME_OCR_S_SPOT, &with) == VJO_OK);
    TEST_CHECK(find_steps(&with, 2) == 1 && spot_opens == 1 && spot_reads > 0);
    TEST_CHECK_(with.workspace_bytes == without.workspace_bytes && blocks[9].size == block_without,
                "the columns' try's workspace %u against %u, its block %u against %u", with.workspace_bytes,
                without.workspace_bytes, blocks[9].size, block_without);
    const VjoGameOcrStep *s = step_of(&with, VJO_GSTEP_SPOT);
    TEST_CHECK_(s && !s->kind && !s->arg && s->rc == VJO_E_VERTICAL && (s->bytes & VJO_GLM_READ),
                "spot step: kind %u rc %d bytes 0x%X", s ? s->kind : 0u, s ? (int)s->rc : 0, s ? (unsigned)s->bytes : 0u);
    TEST_CHECK(blocks[14].live && spot_res.verified); /* kept for the next job of rows */
    /* with the spotter's refinement asked for too: the columns' try takes no refinement arena for it */
    grad_vertical = 1;
    TEST_CHECK(run(autov | VJO_GAME_OCR_S_SPOT | VJO_GAME_OCR_S_SPOT_REFINE, &with) == VJO_OK);
    TEST_CHECK_(find_steps(&with, 2) == 1 && blocks[9].size == block_without,
                "with its refinement: the columns' try's block %u against %u", blocks[9].size, block_without);
    TEST_CHECK(spot_opens == 2); /* each job's rows' try opens the file (its size and time); no columns' try does */
    const unsigned reads = spot_reads;
    grad_vertical = 0; /* GRAD's rows now horizontal: the next job reads rows */
    TEST_CHECK(run(autov | VJO_GAME_OCR_S_SPOT, &with) == VJO_OK);
    TEST_CHECK_(spot_reads == reads && spot_opens == 3, "the next job: %u reads (kept: %u), %u opens", spot_reads,
                reads, spot_opens);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

TEST_LIST = {
    {"a job with the spotter reads its boxes, keeps its weights and gives them back", a_spot_job_finds_its_lines_with_the_spotter},
    {"a missing or wrong spotter reads with GRAD", a_missing_or_wrong_spotter_reads_with_grad},
    {"no memory for the spotter reads with GRAD", no_memory_for_the_spotter_reads_with_grad},
    {"the spotter's boxes are refined by its own bit", its_refinement_follows_its_own_bit},
    {"the worker's stop and a refused job give the spotter's block back", the_worker_stop_and_a_refused_job_give_the_block_back},
    {"ocr_vertical = on reads without the spotter", on_reads_without_the_spotter},
    {"auto's columns try keeps the spotter's block, takes none of its scratch and says why", autos_columns_try_keeps_the_block_and_says_why},
    {NULL, NULL}
};
