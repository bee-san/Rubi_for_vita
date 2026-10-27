/* The actual game worker (game_ocr/main.c) and core adapter (core/vocr_ocr.c)
 * with synthetic pinned weights (test_resident_models.h), so the resident
 * weights' lifecycle runs where the release files are absent (CI): read and
 * SHA-256 checked once, kept while the file's size and modification time stay
 * the same, released on change, failure, model change, refused preflight,
 * inactive game and stop. The line finder and recognizer are spies: this
 * makes no inference, accuracy or console-timing claim. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "test_resident_weights.h"
#include "../../game_ocr/main.c"
#include <bearssl_hash.h>

#define W 200u
#define H 60u
static unsigned char job_memory[1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_MAX_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char pixels[W*H*4];
static unsigned char capture_mem_block[64*1024] __attribute__((aligned(64)));
static int capture_live;
/* API 52, the pause: the worker's frame block, and the kernel's frame copies (VJO_ROWS_FRAME) */
static unsigned char frame_mem_block[64*1024] __attribute__((aligned(64)));
static int frame_live, frame_alloc_failure, frame_refuse, frame_busy_armed, frame_gone_first;
static unsigned frame_block, frame_allocs, frame_calls, strip_calls, spin; /* spin: a spinner moves at each frame */
static int frame_cancel; /* the Shell cancels while the copy waits for its frame */
static unsigned frame_refuse_us; /* a refused copy's time in the game's display call (the cap's: past 100 ms) */
static unsigned capture_block, capture_allocs;
static int64_t available;
static unsigned live_bytes, peak_bytes, job_block, weights_block;
static int job_live, weights_live, job_alloc_failure, weights_alloc_failure, weights_free_failure;
static unsigned weights_refusals; /* the next weights blocks refused (#324: after a passed check) */
static unsigned job_allocs, job_frees, weights_allocs, weights_frees;
static int registered_rc, quit_after_delay, cancel_now, cancel_at_open, cancel_after_reads;
static unsigned quit_delays; /* the worker loop quits after this many sleeps (0: quit_after_delay decides) */
static const char *served;        /* the model the kernel would open for this job */
static int open_failure, size_delta, stat_failure, corrupt;
static uint64_t mtime;
static unsigned opens, closes, reads, bytes_read, loads, delays;
static int file_open_now;
static uint64_t clock_value;
static unsigned spy_lines = 1;
/* API 52 (#358): the finder's boxes instead (spy_boxes_n of them), and the recognizer's text for each line */
static VocrLine spy_boxes[VJO_GAME_OCR_LINES];
static unsigned spy_boxes_n;
static const char *spy_text = "猫";
static unsigned spy_runs;        /* the recognizer's runs: a line from the line cache is not run */
static int cancel_at_job_free;   /* #358: cancelled as the job's memory is freed, after its lines were read */
static int free_query_error;
static uint32_t opened_which;

/* ---- the Vita SDK and kernel transport, as the worker sees them ---- */
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ out->size_user = (int)(available - live_bytes); return free_query_error; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    if (!strcmp(name, "VjoGameVocrFrame")) { /* API 52: on the ladder (USER_RW, CDRAM, phycont) */
        TEST_CHECK(!frame_live && !(size & 4095));
        if (frame_alloc_failure || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) return -1;
        TEST_CHECK(size <= sizeof(frame_mem_block));
        frame_live = 1; frame_block = size; frame_allocs++;
        live_bytes += size; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return 13;
    }
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(size & 4095));
    if (!strcmp(name, "VjoGameVocrCapture")) {
        TEST_CHECK(!capture_live && size <= sizeof(capture_mem_block));
        if (job_alloc_failure) return -1;
        capture_live = 1; capture_block = size; capture_allocs++;
        live_bytes += size; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return 11;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) {
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory)); /* never two weight blocks */
        weights_allocs++;
        if (weights_alloc_failure) return -1;
        if (weights_refusals) { weights_refusals--; return -1; }
        weights_live = 1; weights_block = size;
    } else {
        TEST_CHECK(!strcmp(name, "VjoGameVocr") && !job_live && size <= sizeof(job_memory));
        job_allocs++;
        if (job_alloc_failure) return -1;
        job_live = 1; job_block = size;
    }
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return !strcmp(name, "VjoGameVocrWeights") ? 10 : 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem_block; return 0; }
    if (uid == 13) { TEST_CHECK(frame_live); *base = frame_mem_block; return 0; }
    TEST_CHECK(uid == 10 ? weights_live : uid == 9 && job_live);
    *base = uid == 10 ? (void *)weights_memory : (void *)job_memory; return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(capture_live); capture_live = 0; live_bytes -= capture_block; return 0; }
    if (uid == 13) { TEST_CHECK(frame_live); frame_live = 0; live_bytes -= frame_block; return 0; }
    if (uid == 10) {
        TEST_CHECK(weights_live);
        if (weights_free_failure) return -1;
        weights_live = 0; weights_frees++; live_bytes -= weights_block; return 0;
    }
    TEST_CHECK(uid == 9 && job_live);
    if (cancel_at_job_free) cancel_now = 1;
    job_live = 0; job_frees++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return 0; }

/* ocr-speed (API 22): the thread run time and the ARM clock; see test_game_vocr.c for the tested ones. */
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelDelayThread(unsigned us)
{
    delays++; clock_value += us;
    if (quit_after_delay || (quit_delays && !--quit_delays)) quitting = 1;
    return 0;
}
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return (int)0x80010013u; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return registered_rc; }
/* take_wait_us: the kernel's idle vjoOcrTake waits this long for a job before
 * answering 1 (100 ms since the hand-off waits; 0: an older kernel, at once);
 * take_job: the first call claims a job (an empty request: the worker refuses it
 * at once and completes it); take_quit: the worker stops after this many calls. */
static unsigned take_calls, take_wait_us, take_quit;
static int take_job, take_perm; /* take_perm: every take answers PERM (the game stopped being the active one) */
int vjoOcrTake(VjoGameOcrRequest *out)
{
    take_calls++;
    if (take_quit && take_calls >= take_quit) quitting = 1;
    if (take_job && take_calls == 1) { memset(out, 0, sizeof(*out)); return 0; }
    clock_value += take_wait_us;
    return take_perm ? VJO_ERR_PERM : 1;
}
int vjoOcrCancelled(uint32_t seq)
{ return cancel_now || (cancel_after_reads && reads >= (unsigned)cancel_after_reads); }
static unsigned complete_failures; /* the next completions the kernel refuses (BUSY) */
int vjoOcrComplete(const VjoGameOcrResult *out)
{
    TEST_CHECK(!job_live && !capture_live && (!frame_live || hook_owns_capture));
    if (complete_failures) { complete_failures--; return VJO_ERR_BUSY; }
    return 0;
}
/* A spinner beside the text, rows 40-49, columns 150-169: a new shade at every frame the game shows. */
static void spinner_moves(void)
{
    if (!spin) return;
    spin++;
    for (unsigned y = 40; y < 50; y++)
        for (unsigned x = 150; x < 170; x++) {
            unsigned char *q = pixels + (y * W + x) * 4;
            q[0] = q[1] = q[2] = (unsigned char)(spin * 37u + 11u);
        }
}
/* The kernel's rows: grey of the pixels, straight into the worker's capture block. Each call is one frame of the
 * game (16,667 us). API 52: a frame copy (VJO_ROWS_FRAME) is every row of the region from one frame, into the
 * worker's frame block, in one call. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    if (req->flags & VJO_ROWS_FRAME) {
        frame_calls++;
        TEST_CHECK(req->seq == 12 && !req->y && req->n == H && req->flags == (VJO_ROWS_SIG | VJO_ROWS_LAST | VJO_ROWS_FRAME));
        TEST_CHECK(req->mode == VJO_GAME_OCR_SRC_HOOK && req->dst_stride == W && frame_live && capture_live);
        TEST_CHECK(dst == (void *)frame_mem_block); /* the kernel checks the request's VJO_GAME_OCR_S_PAUSE */
        info->armed = 1;
        if (frame_cancel) { cancel_now = 1; info->armed = 0; return VJO_ERR_CANCELLED; }
        if (frame_busy_armed) return VJO_ERR_BUSY; /* its copy outlived the call */
        if (frame_refuse) { info->armed = frame_refuse != VJO_ERR_UNSUPPORTED; info->copy_us = frame_refuse_us; return frame_refuse; }
        clock_value += 16667u;
        spinner_moves();
        for (unsigned r = 0; r < H; r++)
            for (unsigned x = 0; x < W; x++) {
                const unsigned char *q = pixels + (r * W + x) * 4;
                ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*q[0] + 150u*q[1] + 29u*q[2]) >> 8);
            }
        info->fb_width = W; info->fb_height = H; info->fb_pitch = W;
        info->copy_us = 1234; /* the game's wait in its display call */
        return (int)req->n;
    }
    strip_calls++;
    if (frame_gone_first) TEST_CHECK(!frame_live); /* a refused copy's block goes back before the job reads on */
    clock_value += 16667u;
    spinner_moves();
    TEST_CHECK(req->seq == 12 && req->y < H && req->n && req->n <= H - req->y && capture_live);
    for (unsigned r = 0; r < req->n; r++)
        for (unsigned x = 0; x < W; x++) {
            const unsigned char *p = pixels + ((req->y + r) * W + x) * 4;
            ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
        }
    info->fb_width = W; info->fb_height = H; info->fb_pitch = W;
    return (int)req->n;
}
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { return -1; }

static const VjoVocrModel *served_model(void) { return vjo_vocr_model(served); }
static uint32_t served_seed(void) { return !strcmp(served, "FL10_w8.vocr") ? TEST_FL10_SEED : TEST_H15_SEED; }
static VjoGameOcrRequest current; /* the claimed request, as the kernel holds it */
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    TEST_CHECK(seq == 12 && !file_open_now);
    /* The kernel opens one of the request's two pinned names, never a path. */
    TEST_ASSERT(which == VJO_GAME_OCR_OPEN_MODEL || which == VJO_GAME_OCR_OPEN_FALLBACK);
    served = which == VJO_GAME_OCR_OPEN_FALLBACK ? current.fallback : current.model;
    TEST_ASSERT(served[0] && vjo_vocr_model(served));
    opened_which = which;
    opens++;
    /* The Shell cancels while the kernel opens the file (it answers CANCELLED). */
    if (cancel_at_open) { cancel_now = 1; return VJO_ERR_CANCELLED; }
    if (open_failure) return open_failure;
    file_open_now = 1;
    uint32_t size = served_model()->bytes + (uint32_t)size_delta;
    *info = (VjoGameOcrModelInfo){sizeof(*info), size, stat_failure, 0, stat_failure ? 0 : mtime};
    return (int)size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && file_open_now && n && n <= VJO_GAME_OCR_MODEL_CHUNK);
    reads++;
    for (uint32_t i = 0; i < n; i++)
        ((unsigned char *)dst)[i] = test_weight_byte(off + i, served_seed()) ^ (corrupt && off + i == 777 ? 1 : 0);
    bytes_read += n;
    return (int)n;
}
/* API 23: a shot's file; this test's jobs are OCR jobs (test_game_shot.c has the shot's) */
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src)
{ (void)seq; (void)off; (void)bytes; (void)src; TEST_CHECK(0); return -1; }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { (void)seq; (void)bytes; TEST_CHECK(0); return -1; }
int vjoOcrModelClose(uint32_t seq) { TEST_CHECK(seq == 12); if (file_open_now) closes++; file_open_now = 0; return 0; }

/* ---- the runtime: spies, each checking it got the verified resident bytes ---- */
int vocr_find_lines_rows(const VocrLinesRows *src, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    memset(out, 0, sizeof(*out));
    out->count = spy_boxes_n ? spy_boxes_n : spy_lines;
    for (unsigned i = 0; i < out->count; i++)
        out->lines[i] = spy_boxes_n ? spy_boxes[i] : (VocrLine){10, 2 + 3*i, 180 - 10*i, 3, 0};
    return VOCR_LINES_OK;
}
/* API 50: the job's finder is the fixed one (vita-vn-ocr #129); its spy records the flags and finds as above */
static unsigned spy_line_fix = ~0u;
int vocr_find_lines_fix_rows(const VocrLinesRows *src, int mode, unsigned fix, void *arena, size_t bytes,
                             VocrLineSet *out)
{
    spy_line_fix = fix;
    return vocr_find_lines_rows(src, mode, arena, bytes, out);
}
int vocr_load(vocr_model *m, const void *blob, size_t bytes)
{
    const VjoVocrModel *pin = served_model();
    loads++;
    TEST_CHECK(blob == (const void *)weights_memory && bytes == pin->bytes && weights_live);
    for (uint32_t i = 0; i < bytes; i += 997)
        if (((const unsigned char *)blob)[i] != test_weight_byte(i, served_seed())) { TEST_CHECK(!"resident bytes changed"); break; }
    memset(m, 0, sizeof(*m)); m->blob = blob; m->bytes = bytes;
    return VOCR_OK;
}
size_t vocr_arena_bytes(const vocr_model *m, const vocr_opts *o) { return served_model()->arena_bytes; }
int vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes) { return VOCR_OK; }
/* Frames as the runtime counts them: resized width / 8; 1 ms per frame. */
int vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
{
    snprintf(text, cap, "%s", spy_text); extra->width = img->width; extra->frames = img->width / 8;
    spy_runs++;
    clock_value += 1000u * extra->frames; return VOCR_OK;
}

/* ---- helpers ---- */
static VjoGameOcrRequest request_with(const char *name, const char *fallback)
{
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=W, .fb_height=H, .width=W, .height=H};
    snprintf(r.model, sizeof(r.model), "%s", name);
    snprintf(r.fallback, sizeof(r.fallback), "%s", fallback);
    snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
    served = name;
    current = r;
    return r;
}
static VjoGameOcrRequest request(const char *name) { return request_with(name, ""); }
static void reset(void)
{
    if (weights_live) { weights_free_failure = 0; TEST_CHECK(!drop_weights()); }
    worker = workspace_uid = weights_uid = capture_uid = line_uid = -1;
    capture_live = 0; capture_block = capture_allocs = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = stat_error = 0;
    available = 64*1024*1024; live_bytes = peak_bytes = 0;
    job_live = weights_live = job_alloc_failure = weights_alloc_failure = weights_free_failure = 0;
    weights_refusals = 0;
    job_allocs = job_frees = weights_allocs = weights_frees = 0;
    registered_rc = quit_after_delay = cancel_now = cancel_at_open = cancel_after_reads = cancel_at_job_free = 0;
    take_calls = take_wait_us = take_quit = 0; take_job = take_perm = 0; complete_failures = 0;
    quit_delays = 0;
    open_failure = size_delta = stat_failure = corrupt = 0; mtime = 0x12345678;
    opens = closes = reads = bytes_read = loads = delays = 0; file_open_now = 0;
    spy_lines = 1; free_query_error = 0;
    clock_value = 0;
    memset(pixels, 0, sizeof(pixels));
    frame_uid = -1; frame_base = NULL; frame_bytes_held = 0; hook_owns_capture = 0;
    memset(&pz, 0, sizeof(pz));
    frame_live = frame_alloc_failure = frame_refuse = frame_busy_armed = frame_gone_first = frame_cancel = 0;
    frame_block = frame_allocs = frame_calls = strip_calls = spin = 0;
    frame_refuse_us = 0; pause_capped = 0;
}
static int run_with(const char *name, const char *fallback, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request_with(name, fallback);
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out->cleanup_status);
    TEST_CHECK(weights_live == (resident.model != NULL));
    TEST_CHECK(out->resident_bytes == (weights_live ? weights_block : 0));
    return rc;
}
static int run(const char *name, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(name);
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !file_open_now && !model_owned && !out->cleanup_status);
    /* Between jobs a weights block is held only for verified weights. */
    TEST_CHECK(weights_live == (resident.model != NULL));
    TEST_CHECK(out->resident_bytes == (weights_live ? weights_block : 0));
    return rc;
}

/* ---- tests ---- */
static void synthetic_pins_match_their_bytes(void)
{
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned k = 0; k < 2; k++) {
        const VjoVocrModel *m = vjo_vocr_model(names[k]);
        uint32_t seed = k ? TEST_H15_SEED : TEST_FL10_SEED;
        br_sha256_context h; unsigned char digest[32], byte;
        br_sha256_init(&h);
        for (uint32_t i = 0; i < m->bytes; i++) { byte = test_weight_byte(i, seed); br_sha256_update(&h, &byte, 1); }
        br_sha256_out(&h, digest);
        TEST_CHECK(!memcmp(digest, m->sha256, 32));
    }
}
static void weights_are_read_once_and_kept(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t n = TEST_FL10_BYTES, pieces = (n + VJO_GAME_OCR_MODEL_CHUNK - 1) / VJO_GAME_OCR_MODEL_CHUNK;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == n && reads == pieces);
    TEST_CHECK(weights_allocs == 1 && weights_live && out.resident_bytes == PAGE_ROUND(n));
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    for (unsigned i = 0; i < 50; i++) {
        TEST_CHECK(!run("FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    }
    /* Each job checks the file's size and time (Open/Close), none reads it again. */
    TEST_CHECK(bytes_read == n && reads == pieces && opens == 51 && closes == 51 && loads == 51);
    TEST_CHECK(weights_allocs == 1 && !weights_frees && job_allocs == 51 && job_frees == 51);
    TEST_CHECK(!delays); /* answered model calls never make the worker sleep */
}
static void changed_file_is_read_again(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    mtime++; /* replaced, same size */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(bytes_read == 2*TEST_FL10_BYTES && weights_allocs == 1 && !weights_frees); /* same block reused */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    size_delta = 1; /* a different length is never this model: refused, weights released */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && !out.text[0]);
    TEST_CHECK(!weights_live && weights_frees == 1 && !out.resident_bytes && bytes_read == 2*TEST_FL10_BYTES);
    size_delta = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ && weights_allocs == 2);
}
static void unknown_modification_time_reads_every_job(void)
{
    reset(); VjoGameOcrResult out; stat_failure = (int)0x80010016u;
    for (unsigned i = 0; i < 3; i++) {
        TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
        TEST_CHECK(out.stat_error == stat_failure);
    }
    TEST_CHECK(bytes_read == 3*TEST_FL10_BYTES && weights_allocs == 1);
    stat_failure = 0; /* known again: the next job keys on it, then reuses */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ && !out.stat_error);
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
}
static void missing_or_corrupt_file_releases_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    loads = 0;
    open_failure = (int)0x80010002u; /* deleted while the game runs */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && out.io_error == open_failure);
    TEST_CHECK(!weights_live && !out.resident_bytes);
    open_failure = 0; corrupt = 1; mtime++;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && !weights_live && !loads);
    corrupt = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && loads == 1);
    corrupt = 1; /* same size and time: the verified copy is used, the card is not read */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
}
static void cancellation_keeps_verified_and_drops_partial_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out));
    cancel_at_open = 1; /* cancelled while checking the file: proves nothing about it */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && weights_live && resident.model);
    TEST_CHECK(out.resident_bytes == weights_block && !out.text[0]);
    cancel_at_open = cancel_now = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    mtime++; cancel_after_reads = (int)reads + 2; /* cancelled half-way through a new read */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && !weights_live && !resident.model);
    cancel_after_reads = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}
static void model_change_releases_the_old_weights_first(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_block == PAGE_ROUND(TEST_FL10_BYTES));
    peak_bytes = live_bytes;
    TEST_CHECK(!run("H15_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && weights_block == PAGE_ROUND(TEST_H15_BYTES));
    /* The alloc stub asserts one weights block at a time; the peak is H15's job. */
    TEST_CHECK(peak_bytes == PAGE_ROUND(TEST_H15_BYTES) + PAGE_ROUND(vjo_vocr_job_bytes("H15_w8.vocr", W, H)) +
                             PAGE_ROUND(vjo_vocr_capture_bytes(W, H)));
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_frees == 2 && weights_block == PAGE_ROUND(TEST_FL10_BYTES));
}
static void peak_is_the_old_single_block_within_a_page(void)
{
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned k = 0; k < 2; k++) {
        reset(); VjoGameOcrResult out;
        TEST_CHECK(!run(names[k], &out));
        uint32_t old_block = PAGE_ROUND(vjo_vocr_workspace_bytes(names[k], W, H));
        TEST_CHECK(peak_bytes <= old_block + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)) + 4096u);
        TEST_MSG("%s: peak %u, one-block %u", names[k], peak_bytes, old_block);
        TEST_CHECK(live_bytes == PAGE_ROUND(vjo_vocr_model(names[k])->bytes)); /* kept between jobs */
    }
}
static void preflight_counts_resident_weights_and_refusal_releases_them(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t job = PAGE_ROUND(vjo_vocr_job_bytes("FL10_w8.vocr", W, H)) + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)),
                   weights = PAGE_ROUND(TEST_FL10_BYTES);
    available = GAME_RESERVE + job + weights - 1; /* the first job also needs the weights */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs && !weights_allocs);
    available = GAME_RESERVE + job + weights;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    /* Resident weights are not counted twice: the reserve and the job fit exactly. */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    available--; /* the game took a byte: the job no longer fits beside its reserve */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !weights_live && weights_frees == 1);
    TEST_CHECK(job_allocs == 2 && !out.resident_bytes); /* refused before allocating */
    weights_alloc_failure = 1; available = 64*1024*1024; /* the weights block itself is refused */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !weights_live && !out.text[0]);
    weights_alloc_failure = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}
static void inactive_game_and_stop_release_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    registered_rc = VJO_ERR_BUSY; quit_delays = 60; /* another claim pending, 6 s: the active game's, keep them */
    worker_main(0, NULL);
    TEST_CHECK(weights_live && !blocked);
    /* ocr-first: not the active game for a moment (a system dialog, the Shell placing the
     * game again after a resume) keeps them; INACTIVE_DROP_US of it (process time) gives
     * them back to the game (suspended, backgrounded or disabled). */
    quitting = 0; quit_after_delay = 0; registered_rc = VJO_ERR_NO_GAME;
    quit_delays = 10; /* 1 s */
    worker_main(0, NULL);
    TEST_CHECK(weights_live && resident.model && !blocked);
    quitting = 0; registered_rc = 0; quit_delays = 1; /* active again: the time starts over */
    worker_main(0, NULL);
    quitting = 0; registered_rc = VJO_ERR_NO_GAME; quit_delays = 45; /* 4.5 s */
    worker_main(0, NULL);
    TEST_CHECK(weights_live && resident.model && !blocked);
    quitting = 0; quit_delays = 10; /* 5.5 s in all */
    worker_main(0, NULL);
    TEST_CHECK(!weights_live && !resident.model && !blocked);
    quitting = 0; registered_rc = 0; quit_after_delay = 0; quit_delays = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !resident.model);
}
/* The worker's loop keeps its pace whatever the kernel does: after an idle
 * vjoOcrTake that answered at once (an older kernel) it sleeps its 100 ms, so it
 * never spins; after one the kernel held for its 100 ms wait it asks again at
 * once (the wait is the pace, and a job submitted meanwhile is claimed when it
 * comes, not at the next poll); after a job it asks again at once. */
static void worker_loop_keeps_its_pace(void)
{
    reset(); take_quit = 5; /* an older kernel */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 5 && delays == 5 && clock_value >= 5 * 100000u);
    TEST_MSG("%u takes, %u sleeps", take_calls, delays);
    reset(); take_quit = 5; take_wait_us = 100000; /* the kernel waits */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 5 && delays == 0 && clock_value >= 5 * 100000u && clock_value < 6 * 100000u);
    TEST_MSG("%u takes, %u sleeps", take_calls, delays);
    reset(); take_quit = 3; take_wait_us = 100000; take_job = 1; /* a job, then idle */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 0 && !claim_outstanding);
    reset(); take_quit = 3; take_job = 1; /* a job on an older kernel: the next take at once, then its sleeps */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 2);
    /* a job whose completion the kernel refuses: the claim is retried after 50 ms, never at once */
    reset(); take_quit = 2; take_wait_us = 100000; take_job = 1; complete_failures = 1;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 2 && delays == 2 && !claim_outstanding);
    TEST_MSG("%u takes, %u sleeps", take_calls, delays);
    /* a take the kernel held and then refused (the game stopped being the active one during the
     * wait): the next round at once, as after any held take; refused at once: the 100 ms sleep */
    reset(); take_quit = 3; take_wait_us = 100000; take_perm = 1;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 0);
    reset(); take_quit = 3; take_perm = 1;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 3);
    /* a wait cut short (a job submitted and cancelled before it was taken): the 100 ms sleep */
    reset(); take_quit = 2; take_wait_us = 20000;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 2 && delays == 2);
    reset();
}

/* ocr-first (docs/first-ocr.md): a job that finds no line (a title screen, a box
 * between two lines) reads the weights the session's first job with text would
 * otherwise read while the user waits; that job then finds them resident, and
 * another job without a line opens nothing. The self-test's forced fallback on a
 * screen without text reads nothing and keeps the configured model resident;
 * with text it reads the fallback in its place, never two. A cancelled warm-up
 * keeps no partial bytes, and a missing file fails no job that read no line. */
static uint32_t load_step_arg(const VjoGameOcrResult *out)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == VJO_GSTEP_LOAD) return out->step[i].arg | 0x100u;
    return 0;
}
static void a_job_without_lines_warms_the_weights(void)
{
    reset(); VjoGameOcrResult out;
    spy_lines = 0;
    TEST_CHECK(!run("H15_w8.vocr", &out) && !out.lines && !out.text[0]);
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && weights_live && bytes_read == TEST_H15_BYTES && !loads);
    TEST_CHECK(load_step_arg(&out) == (0x100u | 1u)); /* the LOAD step, arg 1: the warm-up's */
    spy_lines = 1;
    TEST_CHECK(!run("H15_w8.vocr", &out) && !strcmp(out.text, "猫") && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    TEST_CHECK(bytes_read == TEST_H15_BYTES && loads == 1 && load_step_arg(&out) == 0x100u);
    unsigned o = opens;
    spy_lines = 0;
    TEST_CHECK(!run("H15_w8.vocr", &out) && opens == o && bytes_read == TEST_H15_BYTES && !load_step_arg(&out));
    /* the self-test's fallback stage on a screen without text: H15 stays, FL10 is not read */
    VjoGameOcrRequest r = request_with("H15_w8.vocr", "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK; current = r;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.fallback && !out.lines);
    TEST_CHECK(resident.model == vjo_vocr_model("H15_w8.vocr") && weights_live && !weights_frees);
    TEST_CHECK(bytes_read == TEST_H15_BYTES && out.weights == VJO_GAME_OCR_WEIGHTS_NONE && !job_live && !capture_live);
    /* with text it reads FL10 in H15's place: never two blocks (the alloc stub checks) */
    spy_lines = 1;
    r = request_with("H15_w8.vocr", "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK; current = r;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.fallback && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(resident.model == vjo_vocr_model("FL10_w8.vocr") && weights_frees == 1);
    TEST_CHECK(bytes_read == TEST_H15_BYTES + TEST_FL10_BYTES);
    /* cancelled half-way: nothing partial is kept */
    reset(); spy_lines = 0; cancel_after_reads = 3;
    TEST_CHECK(run("H15_w8.vocr", &out) == VJO_E_CANCELLED && !weights_live && !resident.model);
    /* a missing model file: the job without a line succeeds, the error is reported */
    reset(); spy_lines = 0; open_failure = (int)0x80010002u;
    TEST_CHECK(!run("H15_w8.vocr", &out) && !weights_live && out.io_error == open_failure && !out.text[0]);
    open_failure = 0;
    TEST_CHECK(!run("H15_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}

/* The job's other model's weights (its fallback's, or the model's when the
 * fallback runs) stay while the job fits beside them, until it reads its own
 * (ocr-first), and go before the job's own blocks when it does not (the memory
 * check counted them as released): never two weight blocks either way. */
static int step_index(const VjoGameOcrResult *out, unsigned id, unsigned arg)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == id && out->step[i].arg == arg) return (int)i;
    return -1;
}
static void another_models_weights_go_when_the_job_needs_them(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = PAGE_ROUND(vjo_vocr_job_bytes("H15_w8.vocr", W, H)) + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)) +
                         PAGE_ROUND(TEST_H15_BYTES);
    available = GAME_RESERVE + h15 - 1; /* FL10 runs: H15 does not fit */
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
    TEST_CHECK(resident.model == vjo_vocr_model("FL10_w8.vocr"));
    /* Room for H15 beside FL10's block: FL10 stays until H15 is read (a screen without a
     * line here: the warm-up reads it), after the job's own blocks. */
    available = 64*1024*1024; spy_lines = 0;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback && weights_frees == 1);
    TEST_CHECK(resident.model == vjo_vocr_model("H15_w8.vocr") && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    int freed = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS),
        workspace = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_WORKSPACE);
    TEST_CHECK(freed > workspace && workspace >= 0);
    TEST_MSG("ample: weights freed at step %d, workspace allocated at step %d", freed, workspace);
    /* back to FL10 (H15 no longer fits), then room for H15 beside FL10's block, to the byte:
     * FL10 stays until H15 is read */
    available = GAME_RESERVE + h15 - 1; spy_lines = 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback && weights_frees == 2);
    available = GAME_RESERVE + h15 + PAGE_ROUND(TEST_FL10_BYTES); /* FL10 is live: free = available - FL10 */
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback && weights_frees == 3);
    freed = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS);
    workspace = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_WORKSPACE);
    TEST_CHECK(freed > workspace && workspace >= 0);
    TEST_MSG("exact: weights freed at step %d, workspace allocated at step %d", freed, workspace);
    /* one byte less: FL10's block goes before the job's own blocks */
    available = GAME_RESERVE + h15 - 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback && weights_frees == 4);
    available = GAME_RESERVE + h15 + PAGE_ROUND(TEST_FL10_BYTES) - 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback && weights_frees == 5);
    TEST_CHECK(out.release_bytes == PAGE_ROUND(TEST_FL10_BYTES) && resident.model == vjo_vocr_model("H15_w8.vocr"));
    freed = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS);
    workspace = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_WORKSPACE);
    TEST_CHECK(freed >= 0 && freed < workspace);
    TEST_MSG("tight: weights freed at step %d, workspace allocated at step %d", freed, workspace);
}

static void failed_release_blocks_until_stop_retries(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out));
    weights_free_failure = 1; size_delta = 1; /* the refusal must release them, and cannot */
    VjoGameOcrRequest r = request("FL10_w8.vocr");
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && blocked && weights_live);
    TEST_CHECK(out.cleanup_status && !out.text[0] && !resident.model);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && weights_live);
    weights_free_failure = 0; size_delta = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_start()); /* a restarted worker loads them again */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
}

static void memory_check_is_reported_also_when_refused(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t job = PAGE_ROUND(vjo_vocr_job_bytes("FL10_w8.vocr", W, H)) + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)),
                   weights = PAGE_ROUND(TEST_FL10_BYTES);
    available = GAME_RESERVE + job + weights - 4096;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs);
    TEST_CHECK(out.free_user == (int32_t)available && out.reserve_bytes == GAME_RESERVE);
    TEST_CHECK(out.need_workspace == job && out.need_weights == weights);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H)); /* was 0 */
    available = 64*1024*1024;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.free_user == (int32_t)available && out.need_weights == weights);
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.need_weights == 0); /* resident: only the job */
    TEST_CHECK(out.free_user == (int32_t)(available - weights));
    free_query_error = (int)0x80020001u; /* the query itself failed: refused, its error reported */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && out.free_user == free_query_error && !weights_live);
    free_query_error = 0;
    VjoGameOcrRequest bad = request("FL10_w8.vocr"); bad.width = 961; /* ends before the check */
    TEST_CHECK(vjo_game_ocr_run_request(&bad, &out) == VJO_E_SOURCE && !out.reserve_bytes && !out.free_user);
}
static void frames_and_lines_are_reported(void)
{
    reset(); VjoGameOcrResult out;
    spy_lines = VJO_GAME_OCR_LINES;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.lines == VJO_GAME_OCR_LINES);
    TEST_CHECK(out.recognized == VJO_GAME_OCR_LINES);
    uint32_t frames = 0;
    for (unsigned i = 0; i < out.recognized; i++) {
        TEST_CHECK(out.line[i].width == 180 - 10*i && out.line[i].height == 3);
        TEST_CHECK(out.line[i].frames == (180 - 10*i) / 8 && out.line[i].us >= 1000u * out.line[i].frames);
        frames += out.line[i].frames;
    }
    TEST_CHECK(out.frames == frames && out.recognize_us >= 1000u * frames);
    TEST_MSG("frames %u recognize_us %u", out.frames, out.recognize_us);
    spy_lines = 0; /* nothing found: no frames, no lines, the file is not opened */
    unsigned before = opens;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && !out.frames && !out.recognized && opens == before);
}

/* API 52 (#358): a job of rows on the whole frame returns the box its lines suggest (core/region_suggest.h), its rows
 * at the bottom of the 960 x 544 screen; the same screen again, its lines from the line cache, the same box; a job on
 * part of the frame, one whose lines hold no dialogue and a failed one (cancelled as it reads, or after its lines were
 * read) return none. */
static void a_whole_frame_job_suggests_a_box(void)
{
    VjoGameOcrResult out;
    reset();
    spy_boxes[0] = (VocrLine){10, 30, 150, 8, 0};
    spy_boxes[1] = (VocrLine){10, 40, 100, 8, 0};
    spy_boxes_n = 2;
    spy_text = "ひらがなカタカナ";
    /* the lines' median height 8: 4 px above the first, 26 of the frame's 60 rows: 235 of the screen's 544, so the
     * band's 309 rows */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.recognized == 2);
    TEST_CHECK_(out.suggest_rows == 309, "%u", out.suggest_rows);
    /* the same screen again: both lines from the line cache, their kana counted from the cached text (core/vocr_ocr.c
     * line_stat): the same box */
    {
        const unsigned before = spy_runs;
        TEST_CHECK(!run("FL10_w8.vocr", &out) && out.recognized == 2);
        TEST_CHECK_(spy_runs == before, "%u lines recognized again", spy_runs - before);
        TEST_CHECK_(out.suggest_rows == 309, "%u", out.suggest_rows);
    }
    /* part of the frame: none */
    VjoGameOcrRequest part = request("FL10_w8.vocr");
    part.fb_width = W + 40;
    TEST_CHECK(!vjo_game_ocr_run_request(&part, &out) && out.recognized == 2 && !out.suggest_rows);
    part = request("FL10_w8.vocr");
    part.y = 1; part.height = H - 1; part.fb_height = H;
    TEST_CHECK(!vjo_game_ocr_run_request(&part, &out) && !out.suggest_rows);
    /* no kana: none (the line cache emptied: the same pixels would give the cached text, as the worker does) */
    memset(&line_cache, 0, sizeof(line_cache));
    spy_text = "AUTO";
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.recognized == 2 && !out.suggest_rows);
    /* a failed job, cancelled as it reads: none, and no text */
    memset(&line_cache, 0, sizeof(line_cache));
    spy_text = "ひらがなカタカナ";
    cancel_after_reads = 1;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && !out.text[0] && !out.suggest_rows);
    cancel_after_reads = 0;
    /* cancelled after its lines were read, as its memory is freed (the cancel the job's end sees): none, no text */
    memset(&line_cache, 0, sizeof(line_cache));
    cancel_at_job_free = 1;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && !out.text[0] && !out.suggest_rows);
    TEST_CHECK(spy_runs > 0 && cancel_now);
    cancel_at_job_free = cancel_now = 0;
    spy_boxes_n = 0;
    spy_text = "猫";
}

/* Bytes the memory check counts for a model's job and weights (page-rounded). */
static uint32_t capture_of(void) { return PAGE_ROUND(vjo_vocr_capture_bytes(W, H)); }
/* The job's need: its workspace and its capture block (API 19). */
static uint32_t job_of(const char *name) { return PAGE_ROUND(vjo_vocr_job_bytes(name, W, H)) + capture_of(); }
static uint32_t weights_of(const char *name) { return PAGE_ROUND(vjo_vocr_model(name)->bytes); }

static void fallback_runs_only_when_the_model_does_not_fit(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr");
    const uint32_t fl10 = job_of("FL10_w8.vocr") + weights_of("FL10_w8.vocr");
    TEST_ASSERT(fl10 < h15);
    /* Room for H15: it runs, the fallback is not used. */
    available = GAME_RESERVE + h15;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(!out.fallback && opened_which == VJO_GAME_OCR_OPEN_MODEL && resident.model == vjo_vocr_model("H15_w8.vocr"));
    TEST_CHECK(out.need_workspace == job_of("H15_w8.vocr") && out.need_weights == weights_of("H15_w8.vocr"));
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
    /* One byte short for H15 but room for FL10: FL10 runs in the same job. */
    reset(); available = GAME_RESERVE + h15 - 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(out.fallback == 1 && opened_which == VJO_GAME_OCR_OPEN_FALLBACK);
    TEST_CHECK(out.model_need_workspace == job_of("H15_w8.vocr") && out.model_need_weights == weights_of("H15_w8.vocr"));
    TEST_CHECK(out.need_workspace == job_of("FL10_w8.vocr") && out.need_weights == weights_of("FL10_w8.vocr"));
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    TEST_CHECK(resident.model == vjo_vocr_model("FL10_w8.vocr") && weights_block == weights_of("FL10_w8.vocr"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == TEST_FL10_BYTES);
    /* No room for FL10 either: refused, with FL10's numbers (the smallest check). */
    reset(); available = GAME_RESERVE + fl10 - 1;
    TEST_CHECK(run_with("H15_w8.vocr", "FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs && !weights_allocs);
    TEST_CHECK(out.fallback == 1 && out.need_weights == weights_of("FL10_w8.vocr") && !out.text[0]);
}
static void resident_fallback_is_kept_and_upgraded_when_memory_returns(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr");
    available = GAME_RESERVE + h15 - 1; /* the game's free memory without the worker's blocks */
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
    /* Same memory: H15 still does not fit even counting FL10's block as
     * released, so FL10 stays resident and is not read again. */
    for (unsigned i = 0; i < 5; i++) {
        TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT && out.need_weights == 0);
        TEST_CHECK(out.model_release_bytes == weights_of("FL10_w8.vocr"));
    }
    TEST_CHECK(bytes_read == TEST_FL10_BYTES && weights_allocs == 1 && !weights_frees);
    /* The game frees memory: H15 now fits once FL10's block is released, so the
     * job releases FL10 first (never two weight blocks) and reads H15. */
    available += 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback);
    TEST_CHECK(out.release_bytes == weights_of("FL10_w8.vocr") && opened_which == VJO_GAME_OCR_OPEN_MODEL);
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && resident.model == vjo_vocr_model("H15_w8.vocr"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == TEST_FL10_BYTES + TEST_H15_BYTES);
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback); /* stays on H15 */
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    /* Memory shrinks again: H15 resident no longer fits beside the job; FL10
     * fits once H15's block is released: switch down within one job. */
    available -= 1 + weights_of("FL10_w8.vocr");
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
    TEST_CHECK(out.release_bytes == weights_of("H15_w8.vocr") && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    TEST_CHECK(weights_frees == 2 && weights_allocs == 3);
}
static void fallback_must_be_another_smaller_pinned_model(void)
{
    reset(); VjoGameOcrResult out;
    available = GAME_RESERVE + job_of("FL10_w8.vocr") + weights_of("FL10_w8.vocr");
    /* FL10 configured with a larger "fallback": ignored, FL10 runs. */
    TEST_CHECK(!run_with("FL10_w8.vocr", "H15_w8.vocr", &out) && !out.fallback);
    TEST_CHECK(opened_which == VJO_GAME_OCR_OPEN_MODEL && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    /* An unknown, the same or no fallback: the configured model only. */
    const char *ignored[] = {"F99_w8.vocr", "H15_w8.vocr", ""};
    for (unsigned i = 0; i < 3; i++) {
        reset(); available = GAME_RESERVE + job_of("FL10_w8.vocr") + weights_of("FL10_w8.vocr");
        TEST_CHECK(run_with("H15_w8.vocr", ignored[i], &out) == VJO_E_OOM && !out.fallback && !job_allocs);
        TEST_CHECK(out.need_weights == weights_of("H15_w8.vocr"));
    }
    /* An unterminated fallback field is a wire error before anything else. */
    reset();
    VjoGameOcrRequest bad = request_with("H15_w8.vocr", "FL10_w8.vocr");
    memset(bad.fallback, 'x', sizeof(bad.fallback));
    TEST_CHECK(vjo_game_ocr_run_request(&bad, &out) == VJO_E_SOURCE && !out.reserve_bytes && !job_allocs);
}
static void fallback_file_failure_and_other_resident_model(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr");
    available = GAME_RESERVE + h15 - 1;
    open_failure = (int)0x80010002u; /* FL10 is missing too: the fallback's own file error */
    TEST_CHECK(run_with("H15_w8.vocr", "FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && out.fallback);
    TEST_CHECK(opened_which == VJO_GAME_OCR_OPEN_FALLBACK && out.io_error == open_failure && !weights_live);
    open_failure = 0;
    /* Weights of a model this job can use neither as model nor fallback are released first. */
    reset(); available = 64*1024*1024;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    TEST_CHECK(!run_with("H15_w8.vocr", "", &out) && resident.model == vjo_vocr_model("H15_w8.vocr"));
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && !out.release_bytes);
}


/* ---- API 52: the pause for the frame (VJO_GAME_OCR_S_PAUSE; docs/game-pause.md) ---- */
/* The request a job with the stable-text wait (150 ms, at most 200 ms) sends, with or without the pause. */
static VjoGameOcrRequest paused(int pause)
{
    VjoGameOcrRequest r = request("FL10_w8.vocr");
    r.flags |= 15u << VJO_GAME_OCR_SETTLE_SHIFT | 4u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    if (pause) r.speed |= VJO_GAME_OCR_S_PAUSE;
    current = r;
    return r;
}
static const VjoGameOcrStep *pause_step(const VjoGameOcrResult *out)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == VJO_GSTEP_PAUSE) return &out->step[i];
    return NULL;
}
/* A spinner in the region that never comes back to a shade it showed: read strip by strip, every re-read the
 * stable-text wait asks for tears the region and the job shows nothing (VJO_E_SCREEN_CHANGED: the report of #348).
 * Paused, the region is one frame, copied in one call into the frame block; the job reads it there, waits for
 * nothing, shows its text, and gives the block back. */
static void the_pause_reads_one_frame_beside_a_spinner(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset(); spin = 1;
    r = paused(0);
    (void)vjo_game_ocr_run_request(&r, &out); /* its last try reads once, the wait's budget spent (no re-read) */
    TEST_CHECK(out.tears > 0 && strip_calls > 3 && !frame_calls && !frame_allocs && !pause_step(&out));
    TEST_MSG("strip by strip: %u strips, %u tears, rc %d", strip_calls, out.tears, out.rc);
    reset(); spin = 1;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !strcmp(out.text, "猫"));
    TEST_CHECK(frame_calls == 1 && !strip_calls && !out.tears && out.captures == 1); /* one frame, nothing else */
    TEST_CHECK(frame_allocs == 1 && !frame_live && frame_block == PAGE_ROUND(W * H));
    const VjoGameOcrStep *ps = pause_step(&out);
    TEST_CHECK(ps && ps->kind == 1 && ps->arg == H && !ps->rc && ps->bytes == PAGE_ROUND(W * H) &&
               ps->sum == (VJO_GPAUSE_FROZEN | VJO_GAME_OCR_MEM_USER << 8) && ps->us == 1234);
    /* the block's own ALLOC and FREE are not logged: the PAUSE step has them */
    TEST_CHECK(step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_FRAME) < 0 && step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_FRAME) < 0);
    TEST_CHECK(out.need_workspace == job_of("FL10_w8.vocr") + PAGE_ROUND(W * H)); /* the check counted it */
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_FRAME) == 0 && vjo_ledger_peak(&g_game_ledger, GL_FRAME) == PAGE_ROUND(W * H));
    /* the next job, without the pause, reads strip by strip again */
    spin = 0; strip_calls = 0;
    r = paused(0);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && strip_calls >= 1 && frame_calls == 1 && !pause_step(&out));
}
/* A copy the kernel refuses (a game it does not pause, the watchdog's switch: UNSUPPORTED) or cuts at its cap
 * (TIMEOUT): the frame block goes back to the game at once, the job reads strip by strip as without the pause, and
 * the step says why. */
static void a_refused_pause_reads_as_before(void)
{
    const int why[] = {VJO_ERR_UNSUPPORTED, VJO_ERR_TIMEOUT, VJO_ERR_COPY};
    for (unsigned i = 0; i < 3; i++) {
        VjoGameOcrResult out;
        VjoGameOcrRequest r;
        reset();
        frame_refuse = why[i]; frame_gone_first = 1;
        r = paused(1);
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !strcmp(out.text, "猫"));
        TEST_CHECK(frame_calls == 1 && strip_calls >= 1 && frame_allocs == 1 && !frame_live);
        const VjoGameOcrStep *ps = pause_step(&out);
        TEST_CHECK(ps && !ps->kind && ps->rc == why[i] && (ps->sum & 0xFFu) == VJO_GPAUSE_REFUSED && !ps->arg);
        TEST_MSG("refusal %u: kind %u rc 0x%08X sum %u", i, ps ? ps->kind : 9u, ps ? (unsigned)ps->rc : 0u, ps ? ps->sum : 9u);
    }
}
/* No room for the frame block beside the job (its memory check, with its reserve), or a refused allocation: the job
 * runs as without the pause, with the same model; the pause never costs a model, LM or cascade. */
static void without_room_the_job_reads_as_before(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset();
    TEST_CHECK(!run("FL10_w8.vocr", &out)); /* the weights resident from here */
    available = live_bytes + GAME_RESERVE + job_of("FL10_w8.vocr") + PAGE_ROUND(W * H) - 4096;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !strcmp(out.text, "猫"));
    TEST_CHECK(!frame_allocs && !frame_calls && strip_calls >= 1 && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    TEST_CHECK(out.need_workspace == job_of("FL10_w8.vocr") && !out.fallback);
    const VjoGameOcrStep *ps = pause_step(&out);
    TEST_CHECK(ps && !ps->kind && ps->rc == VJO_E_OOM && ps->sum == VJO_GPAUSE_NO_ROOM && !ps->bytes);
    /* room for it: paused */
    available += 4096;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && frame_calls == 1 && (ps = pause_step(&out)) && ps->kind == 1);
    /* the allocation refused after a passed check: as without room */
    available = 64*1024*1024; frame_alloc_failure = 1; frame_calls = 0;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !frame_calls && (ps = pause_step(&out)) &&
               ps->sum == VJO_GPAUSE_NO_ROOM && ps->rc == VJO_E_OOM);
}
/* A job cancelled while its copy waits for a frame ends cancelled with the block given back; a copy that outlived its
 * call (the hook may still write the block) ends the job, keeps the block for the game's life and takes no more jobs,
 * as for the capture block. */
static void a_cancelled_or_stuck_copy(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset(); frame_cancel = 1;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !out.text[0] && !frame_live && !strip_calls);
    TEST_CHECK(!blocked && !out.cleanup_status);
    reset(); frame_busy_armed = 1;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) != VJO_OK && !out.text[0] && !strip_calls);
    TEST_CHECK(blocked && hook_owns_capture && frame_live); /* never freed under the hook */
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && frame_calls == 1);
    hook_owns_capture = 0; frame_live = 0; frame_uid = -1; capture_live = 0; capture_uid = -1; blocked = 0;
}

/* #324: the check passed, yet the model's weights were refused (the console's free memory in whole MiB): the job gives
 * its blocks back, the frame block too, and checks again from the next smaller model. That is a new check, so a new
 * try of the pause: its own frame block and its own copy (never the first try's, given back), the game's waits for
 * both copies in the step; the job's captures are the last try's, as its strips would be. */
static void weights_refused_after_the_check_the_smaller_models_try_copies_its_own_frame(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset(); spin = 1;
    r = paused(1);
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    served = "H15_w8.vocr";
    current = r;
    weights_refusals = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !strcmp(out.text, "猫"));
    TEST_CHECK(out.fallback == 1 && weights_allocs == 2 && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    /* test/combined: main's #382 keeps the job's capture across the retry: each try's frame copy is one of its
     * captures */
    TEST_CHECK_(frame_calls == 2 && !strip_calls && !out.tears && out.captures == 2,
                "%u frame copies, %u strips, %u tears, %u captures", frame_calls, strip_calls, out.tears, out.captures);
    TEST_CHECK(frame_allocs == 2 && !frame_live && !capture_live && !job_live && !out.cleanup_status);
    const VjoGameOcrStep *ps = pause_step(&out);
    TEST_CHECK_(ps && ps->kind == 1 && ps->arg == H && !ps->rc && ps->bytes == PAGE_ROUND(W * H) &&
                ps->sum == (VJO_GPAUSE_FROZEN | VJO_GAME_OCR_MEM_USER << 8) && ps->us == 2u * 1234u,
                "pause step: kind %u, rows %u, rc %d, sum 0x%X, us %u", ps ? ps->kind : 9u, ps ? ps->arg : 0u,
                ps ? (int)ps->rc : 0, ps ? ps->sum : 0u, ps ? ps->us : 0u);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_FRAME) == 0);
}

/* The audit's F2: a copy that reached the cap (VJO_ERR_TIMEOUT after VJO_GAME_OCR_PAUSE_MAX_US in the game's display
 * call) made the game wait for nothing, and would at every reading: the game is not paused again until it next
 * starts. Later jobs ask for no copy and take no block, and their step says why (CAPPED). A frame that never came
 * (TIMEOUT with no wait) changes nothing. */
static void a_copy_that_reaches_the_cap_turns_the_pause_off_for_the_game(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    const VjoGameOcrStep *ps;
    reset();
    frame_refuse = VJO_ERR_TIMEOUT; /* no frame came: no wait */
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && frame_calls == 1 && !pause_capped);
    frame_refuse_us = VJO_GAME_OCR_PAUSE_MAX_US + 900u; /* the cap: the game waited 100.9 ms */
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !strcmp(out.text, "猫") && frame_calls == 2);
    TEST_CHECK(pause_capped && !frame_live && strip_calls >= 1);
    ps = pause_step(&out);
    TEST_CHECK(ps && !ps->kind && ps->rc == VJO_ERR_TIMEOUT && (ps->sum & 0xFFu) == VJO_GPAUSE_REFUSED &&
               ps->us == VJO_GAME_OCR_PAUSE_MAX_US + 900u);
    frame_refuse = 0; frame_refuse_us = 0;
    const unsigned allocs = frame_allocs;
    r = paused(1);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !strcmp(out.text, "猫"));
    TEST_CHECK(frame_calls == 2 && frame_allocs == allocs && out.need_workspace == job_of("FL10_w8.vocr"));
    ps = pause_step(&out);
    TEST_CHECK_(ps && !ps->kind && ps->rc == VJO_ERR_TIMEOUT && ps->sum == VJO_GPAUSE_CAPPED && !ps->bytes && ps->us == 1,
                "capped: kind %u rc 0x%08X sum %u bytes %u us %u", ps ? ps->kind : 9u, ps ? (unsigned)ps->rc : 0u,
                ps ? ps->sum : 9u, ps ? ps->bytes : 9u, ps ? ps->us : 9u);
}
/* The audit's F1: the frame block takes free memory only. Another model's weights resident (FL10), a job of H15 with
 * two helpers, and free memory where H15's job has room for its frame block or its helpers, not both: without the
 * pause both helpers fit, and FL10 stays until H15 is read; with it, the same, with fewer helpers. (Before, the
 * helpers counted FL10's block as free, and the frame block made the job give it back before its own blocks.) */
static void the_frame_block_never_makes_the_job_give_back_another_models_weights(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr"), f = PAGE_ROUND(W * H);
    const uint32_t e2 = (uint32_t)helpers_need("H15_w8.vocr", vjo_vocr_job_bytes("H15_w8.vocr", W, H), 2);
    TEST_ASSERT(e2 > 0 && f > 0);
    for (int pause = 0; pause < 2; pause++) {
        reset();
        available = GAME_RESERVE + h15 - 1; /* FL10 runs: H15 does not fit */
        TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
        TEST_ASSERT(resident.model == vjo_vocr_model("FL10_w8.vocr"));
        available = (int64_t)live_bytes + GAME_RESERVE + h15 + (f > e2 ? f : e2); /* free beside FL10's block */
        r = paused(pause);
        snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
        snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
        r.speed |= 2u; /* two helpers */
        served = "H15_w8.vocr";
        current = r;
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_OK && !out.fallback && !strcmp(out.text, "猫"));
        const int freed = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS),
                  workspace = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_WORKSPACE);
        TEST_CHECK_(workspace >= 0 && freed > workspace, "pause %d: FL10 freed at step %d, the workspace taken at %d",
                    pause, freed, workspace);
        TEST_CHECK(resident.model == vjo_vocr_model("H15_w8.vocr") && !frame_live);
        if (pause)
            TEST_CHECK_(frame_calls == 1 && out.need_workspace >= job_of("H15_w8.vocr") + f &&
                        out.need_workspace < job_of("H15_w8.vocr") + f + e2, "paused: need %u (job %u, frame %u, helpers %u)",
                        out.need_workspace, job_of("H15_w8.vocr"), f, e2);
        else
            TEST_CHECK(!frame_calls && out.need_workspace == job_of("H15_w8.vocr") + e2);
    }
}

TEST_LIST = {
    {"API 52: the pause reads one frame beside a spinner that tears every strip-by-strip read", the_pause_reads_one_frame_beside_a_spinner},
    {"API 52: a refused or cut pause gives its block back and reads as before", a_refused_pause_reads_as_before},
    {"API 52: without room for its block the job reads as before, with the same model", without_room_the_job_reads_as_before},
    {"API 52: a cancelled copy, and one that outlived its call", a_cancelled_or_stuck_copy},
    {"API 52, #324: weights refused after the check: the smaller model's try copies its own frame",
     weights_refused_after_the_check_the_smaller_models_try_copies_its_own_frame},
    {"API 52: a copy that reaches the cap turns the pause off for the game", a_copy_that_reaches_the_cap_turns_the_pause_off_for_the_game},
    {"API 52: the frame block never makes the job give back another model's weights",
     the_frame_block_never_makes_the_job_give_back_another_models_weights},
    {"a job without lines warms the weights; the forced fallback keeps the model", a_job_without_lines_warms_the_weights},
    {"another model's weights go when the job needs their memory", another_models_weights_go_when_the_job_needs_them},
    {"the fallback runs only when the configured model does not fit", fallback_runs_only_when_the_model_does_not_fit},
    {"a resident fallback is kept, and upgraded or downgraded within one job", resident_fallback_is_kept_and_upgraded_when_memory_returns},
    {"a fallback must be another, smaller pinned model", fallback_must_be_another_smaller_pinned_model},
    {"the fallback's own file failure, and another resident model", fallback_file_failure_and_other_resident_model},
    {"the memory check is reported, also when it refuses the job", memory_check_is_reported_also_when_refused},
    {"CTC frames and each line's crop, frames and time are reported", frames_and_lines_are_reported},
    {"a whole-frame job suggests a box, cached lines too; part of the frame, no dialogue, a failure none (API 52)",
     a_whole_frame_job_suggests_a_box},
    {"synthetic pins match their generated bytes", synthetic_pins_match_their_bytes},
    {"weights are read and hashed once, then kept for 50 jobs", weights_are_read_once_and_kept},
    {"a changed modification time is read again; a changed size is refused", changed_file_is_read_again},
    {"an unknown modification time reads the weights for every job", unknown_modification_time_reads_every_job},
    {"a missing or corrupt file releases the resident weights", missing_or_corrupt_file_releases_weights},
    {"cancellation keeps verified weights and drops a partial read", cancellation_keeps_verified_and_drops_partial_weights},
    {"a model change releases the old weights before allocating the new", model_change_releases_the_old_weights_first},
    {"the job peak stays within a page of the old single block", peak_is_the_old_single_block_within_a_page},
    {"the preflight counts resident weights once; a refusal releases them", preflight_counts_resident_weights_and_refusal_releases_them},
    {"an inactive game and the worker stop release the weights", inactive_game_and_stop_release_weights},
    {"a failed release blocks the worker until stop retries it", failed_release_blocks_until_stop_retries},
    {"the worker's loop keeps its pace with a waiting kernel and an older one", worker_loop_keeps_its_pace},
    {NULL, NULL}
};
