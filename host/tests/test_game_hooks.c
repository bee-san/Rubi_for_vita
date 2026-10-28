/* Native hooks in the game worker's job path (game_ocr/main.c, docs/native-hooks.md): which text a job
 * shows, OCR's or a hook stream's, through the actual worker and core adapter with the resident test's
 * synthetic pinned weights and spy runtime (test_game_resident.c), and a fake of game_ocr/hooks.h whose
 * streams the tests set. A job a stream answers reads the region's first capture pass and nothing else
 * (no model, no finding, no recognition); a job OCR reads may show an agreeing stream's text; with
 * nothing installed a job takes no snapshot. The spies make no inference, accuracy or timing claim. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "test_resident_weights.h"
#define VJO_TEST_HOOK_CACHE 1
#define VJO_TEST_HOOK_LINE 1
#define VJO_TEST_SC3 1
#include "../../game_ocr/main.c"

#define W 200u
#define H 150u /* three strips of the first pass: 64 + 64 + 22 rows */
static unsigned char job_memory[1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_MAX_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char capture_mem_block[64*1024] __attribute__((aligned(64)));
static unsigned char lm_memory[VJO_VOCR_LM1_BYTES + 4096] __attribute__((aligned(64))); /* a resident LM's block */
static int lm_live; static unsigned lm_block;
/* API 55, the pause: the worker's frame block (VjoGameVocrFrame) and the kernel's frame copies into it */
static unsigned char frame_mem_block[64*1024] __attribute__((aligned(64)));
static int frame_live;
static unsigned frame_block, frame_copies;
static unsigned char pixels[W*H*4];
static int capture_live, job_live, weights_live, shot_allocs, probe_allocs;
static unsigned capture_block, capture_allocs, job_block, weights_block, live_bytes;
static int64_t available;
static uint64_t clock_value;
static int file_open_now, cancel_now, rows_error;
static unsigned opens, finds, runs;
static const char *spy_text = "猫"; /* what the spy recognizer reads on every line */

static uint8_t saved_cache[VJO_HOOK_CACHE_BYTES], fake_build[32];
static unsigned cache_loads, cache_saves, build_reads;
static int saved_cache_size, cache_load_error, cache_save_error, build_error;
static unsigned cache_busy, cache_closes;
static int cache_pending, cache_cancel, fail_capture_free, fail_weights_free;
int vjo_hooks_build_id(uint8_t out[32])
{
    build_reads++;
    memcpy(out, fake_build, sizeof(fake_build));
    return !build_error;
}
int vjoOcrHookCache(uint32_t seq, uint32_t operation, void *data)
{
    TEST_CHECK(seq == 12 && !file_open_now);
    cache_pending = 1;
    if (cache_cancel) cancel_now = 1;
    if (cache_busy) { cache_busy--; return VJO_ERR_BUSY; }
    if (operation == VJO_GAME_OCR_HOOK_CACHE_LOAD) {
        cache_loads++;
        if (cache_load_error) return cache_load_error;
        if (saved_cache_size > 0) memcpy(data, saved_cache, sizeof(saved_cache));
        return saved_cache_size;
    }
    TEST_CHECK(operation == VJO_GAME_OCR_HOOK_CACHE_SAVE);
    cache_saves++;
    if (cache_save_error) return cache_save_error;
    memcpy(saved_cache, data, sizeof(saved_cache));
    saved_cache_size = sizeof(saved_cache);
    return 0;
}

/* An SC3 site's charset (game_ocr/hooks.c vjo_hooks_sc3_want, vjo_hooks_sc3_set): the pin the fake engine wants (NULL:
 * none), what it was given, and whether it keeps it; the file the fake broker opens for it (VJO_GAME_OCR_OPEN_SC3). */
static const VjoSc3Pin *sc3_want;
static const uint8_t *sc3_set_file;
static uint32_t sc3_set_bytes;
static unsigned sc3_sets, sc3_opens;
static int sc3_accept, sc3_open_rc; /* sc3_open_rc: what Open answers (the file's size, or an error) */
static unsigned char sc3_mem[16384] __attribute__((aligned(64)));
static int sc3_live;
static unsigned sc3_block;
/* #368's audit: the charset's read answers BUSY while sc3_read_busy (the kernel's file worker stalled) or fails with
 * sc3_read_error; its close answers BUSY sc3_close_busy times; its block's base cannot be read while sc3_base_fail
 * and its free fails sc3_free_fail times; sc3_allocs counts the blocks taken, sc3_double those taken while one is
 * live */
static int sc3_read_busy, sc3_read_error, sc3_base_fail;
static unsigned sc3_close_busy, sc3_free_fail, sc3_allocs, sc3_double;
const VjoSc3Pin *vjo_hooks_sc3_want(void) { return sc3_set_file ? NULL : sc3_want; }
int vjo_hooks_sc3_set(const uint8_t *file, uint32_t bytes)
{
    sc3_sets++;
    sc3_set_file = NULL;
    if (!file || !sc3_accept) return 0;
    TEST_CHECK(file == sc3_mem && sc3_live && bytes == sc3_want->bytes);
    for (uint32_t i = 0; i < bytes; i++) TEST_CHECK_(file[i] == (uint8_t)(i * 7u), "byte %u", (unsigned)i);
    sc3_set_file = file;
    sc3_set_bytes = bytes;
    return 1;
}

/* ---- the fake hook engine (game_ocr/hooks.h) ---- */
typedef struct {
    uint32_t kind, seq, key; /* key: VjoHookSnap.key (another caller's when its CALL slot was replaced) */
    const char *text; /* UTF-8, NULL: no text */
    int busy;         /* a game thread is writing it: the snapshot fails */
    int filtered_empty; /* raw text exists, but the decoder/filter produces no text */
    const VjoLunaSite *luna; /* only Luna entries carry the game's extraction/filter script */
} FakeStream;
static const VjoLunaSite fake_luna_site = {0};
static FakeStream fs[VJO_HOOK_STREAMS + 1u];
static uint32_t fake_mode, fake_installed, fake_refused;
static unsigned snaps, texts;
/* vjo_hooks_keep's calls (audit-hooks F2), and what vjo_hooks_call_stats answers */
static unsigned keeps, kept_ids[8];
static uint32_t kept_keys[8];
static uint32_t fake_call_dropped, fake_call_replaced;
/* At the snap_at-th snapshot, stream bump_id gets bump_text (its seq moves on), as a game thread would. */
static unsigned bump_at;
static uint32_t bump_id;
static const char *bump_text;

/* vjo_hooks_start and vjo_hooks_stop: game_ocr/main.c's host macros (no game, no hooks installed). */
void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    *mode = fake_mode;
    *installed = fake_installed;
    *refused = fake_refused;
}
static uint32_t fake_verified; /* bit id - 1: a Rubi verified table site's stream */
int vjo_hooks_verified(uint32_t id)
{
    return id >= 1u && id <= 32u && (fake_verified >> (id - 1u) & 1u);
}
void vjo_hooks_keep(uint32_t id, uint32_t key)
{
    if (keeps < sizeof(kept_ids) / sizeof(kept_ids[0])) {
        kept_ids[keeps] = id;
        kept_keys[keeps] = key;
    }
    keeps++;
}
void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced)
{
    *dropped = fake_call_dropped;
    *replaced = fake_call_replaced;
}
int vjo_hooks_snap(uint32_t id, VjoHookSnap *out)
{
    snaps++;
    if (bump_at && snaps == bump_at) {
        fs[bump_id].text = bump_text;
        fs[bump_id].seq++;
    }
    if (!id || id > VJO_HOOK_STREAMS || !fs[id].text || !fs[id].text[0] || fs[id].busy) return 0;
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->kind = fs[id].kind;
    out->seq = fs[id].seq;
    out->key = fs[id].key;
    out->hits = 1;
    out->encoding = VJO_HOOK_ENC_UTF8;
    out->site = fs[id].luna;
    out->n = (uint32_t)strlen(fs[id].text) < VJO_HOOK_RAW_BYTES ? (uint32_t)strlen(fs[id].text) : VJO_HOOK_RAW_BYTES;
    memcpy(out->raw, &fs[id].text, sizeof(fs[id].text)); /* the fake's raw bytes: the text's address */
    return 1;
}
/* The hook watch's lock-free read (API 41): never refused, a busy stream included. */
static unsigned seq_reads;
uint32_t vjo_hooks_seq(uint32_t id, uint32_t *key, uint32_t *kind, uint32_t *bytes)
{
    seq_reads++;
    if (!id || id > VJO_HOOK_STREAMS || !fs[id].text) return 0;
    *key = fs[id].key;
    *kind = fs[id].kind;
    *bytes = (uint32_t)strlen(fs[id].text);
    return fs[id].seq;
}
/* vjoOcrHookLine (API 41): what the watch reported, and what the kernel answers */
static unsigned hook_lines;
static uint32_t hook_line_seq, hook_line_bytes;
static int hook_line_rc;
int vjoOcrHookLine(uint32_t seq, uint32_t bytes)
{
    hook_lines++;
    if (hook_line_rc) return hook_line_rc;
    hook_line_seq = seq;
    hook_line_bytes = bytes;
    return 0;
}
size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    const char *t;
    size_t n;
    texts++;
    if (fs[s->id].filtered_empty) { out[0] = '\0'; return 0; }
    memcpy(&t, s->raw, sizeof(t));
    n = strlen(t);
    if (n >= cap) { /* cut at the last whole character, as vjo_hook_decode does */
        n = cap - 1u;
        while (n && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, t, n);
    out[n] = '\0';
    return n;
}

/* ---- the Vita SDK and kernel transport, as the worker sees them ---- */
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
/* 1: the free USER memory in whole MiB, rounded down, as a console reports it (#324: あおかな on a PCH-2000) */
static int free_mib;
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = (int)(available - live_bytes);
    if (free_mib && out->size_user > 0) out->size_user -= out->size_user % (1024 * 1024);
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    (void)opts;
    TEST_CHECK(!(size & 4095));
    if (!strcmp(name, "VjoGameShot")) { shot_allocs++; return -1; }
    if (!strcmp(name, "VjoGameVocrProbe")) { probe_allocs++; return -1; }
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    if (!strcmp(name, "VjoGameVocrFrame")) { /* API 55 */
        TEST_CHECK(!frame_live && size <= sizeof(frame_mem_block));
        frame_live = 1; frame_block = size; live_bytes += size;
        return 14;
    }
    if (!strcmp(name, "VjoGameSc3")) {
        sc3_allocs++;
        sc3_double += sc3_live != 0;
        TEST_CHECK(!sc3_live && size <= sizeof(sc3_mem));
        sc3_live = 1; sc3_block = size; live_bytes += size;
        return 12;
    }
    if (!strcmp(name, "VjoGameVocrCapture")) {
        TEST_CHECK(!capture_live && size <= sizeof(capture_mem_block));
        capture_live = 1; capture_block = size; capture_allocs++; live_bytes += size;
        return 11;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) {
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory));
        weights_live = 1; weights_block = size; live_bytes += size;
        return 10;
    }
    if (!strcmp(name, "VjoGameVocrLM")) {
        TEST_CHECK(!lm_live && size <= sizeof(lm_memory));
        lm_live = 1; lm_block = size; live_bytes += size;
        return 13;
    }
    TEST_CHECK(!strcmp(name, "VjoGameVocr") && !job_live && size <= sizeof(job_memory));
    job_live = 1; job_block = size; live_bytes += size;
    return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem_block; return 0; }
    if (uid == 14) { TEST_CHECK(frame_live); *base = frame_mem_block; return 0; }
    if (uid == 12) { TEST_CHECK(sc3_live); if (sc3_base_fail) return -1; *base = sc3_mem; return 0; }
    if (uid == 13) { TEST_CHECK(lm_live); *base = lm_memory; return 0; }
    TEST_CHECK(uid == 10 ? weights_live : uid == 9 && job_live);
    *base = uid == 10 ? (void *)weights_memory : (void *)job_memory;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 14) { TEST_CHECK(frame_live); frame_live = 0; live_bytes -= frame_block; return 0; }
    if (uid == 11) {
        TEST_CHECK(capture_live);
        if (fail_capture_free) { fail_capture_free--; return -1; }
        capture_live = 0; live_bytes -= capture_block; return 0;
    }
    if (uid == 10) {
        TEST_CHECK(weights_live);
        if (fail_weights_free) { fail_weights_free--; return -1; }
        weights_live = 0; live_bytes -= weights_block; return 0;
    }
    if (uid == 13) { TEST_CHECK(lm_live); lm_live = 0; live_bytes -= lm_block; return 0; }
    if (uid == 12) {
        TEST_CHECK(sc3_live && !sc3_set_file); /* never while the engine holds it */
        if (sc3_free_fail) { sc3_free_fail--; return -1; }
        sc3_live = 0; live_bytes -= sc3_block; return 0;
    }
    TEST_CHECK(uid == 9 && job_live); job_live = 0; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
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
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_now; }
int vjoOcrComplete(const VjoGameOcrResult *out) { return 0; }
/* The kernel's rows, recorded: each call's first row, count and flags. */
#define MAX_ROWS_CALLS 256u
static struct { uint32_t y, n, flags; } rows_log[MAX_ROWS_CALLS];
static unsigned rows_calls;
/* API 49: a trusted answer's capture without rows (VJO_ROWS_HOOK), and what the kernel answers it */
static unsigned hook_publishes;
static int hook_publish_rc;
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    if (req->flags & VJO_ROWS_FRAME) { /* API 55: the whole region from one frame, into the frame block */
        TEST_CHECK(req->seq == 12 && !req->y && req->n == H && req->dst_stride == W && frame_live &&
                   dst == (void *)frame_mem_block && req->flags == (VJO_ROWS_SIG | VJO_ROWS_LAST | VJO_ROWS_FRAME));
        frame_copies++;
        for (unsigned r = 0; r < H; r++)
            for (unsigned x = 0; x < W; x++) {
                const unsigned char *p = pixels + (r * W + x) * 4;
                ((unsigned char *)dst)[r * W + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
            }
        info->armed = 1;
        info->fb_width = W; info->fb_height = H; info->fb_pitch = W;
        info->copy_us = 777; /* the game's wait in its display call */
        return (int)H;
    }
    if (req->flags & VJO_ROWS_HOOK) {
        TEST_CHECK(req->seq == 12 && req->size == sizeof(*req) && req->flags == VJO_ROWS_HOOK && !req->n && !req->y &&
                   !dst && !req->dst_stride && req->mode == VJO_GAME_OCR_SRC_HOOK && info && !capture_live);
        hook_publishes++;
        return hook_publish_rc;
    }
    TEST_CHECK(req->seq == 12 && req->y < H && req->n && req->n <= H - req->y && req->n <= VJO_GAME_OCR_ROWS_MAX);
    TEST_CHECK(capture_live && (unsigned char *)dst >= capture_mem_block &&
               (unsigned char *)dst + (req->n - 1u) * req->dst_stride + W <= capture_mem_block + capture_block);
    if (rows_calls < MAX_ROWS_CALLS) rows_log[rows_calls] = (typeof(rows_log[0])){req->y, req->n, req->flags};
    rows_calls++;
    if (rows_error) return rows_error;
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
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src) { return -1; }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { return -1; }
static int sc3_file_open;
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    if (which == VJO_GAME_OCR_OPEN_SC3) { /* the charset: the kernel names the file by the game's own title */
        TEST_CHECK(seq == 12 && !file_open_now && sc3_live);
        sc3_opens++;
        file_open_now = sc3_file_open = sc3_open_rc > 0;
        *info = (VjoGameOcrModelInfo){sizeof(*info), sc3_open_rc > 0 ? (uint32_t)sc3_open_rc : 0u, 0, 0, 0};
        return sc3_open_rc;
    }
    TEST_CHECK(seq == 12 && !file_open_now && which == VJO_GAME_OCR_OPEN_MODEL);
    opens++;
    file_open_now = 1;
    *info = (VjoGameOcrModelInfo){sizeof(*info), TEST_FL10_BYTES, 0, 0, 0x12345678};
    return (int)TEST_FL10_BYTES;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && file_open_now && n && n <= VJO_GAME_OCR_MODEL_CHUNK);
    if (sc3_file_open) {
        TEST_CHECK(off + n <= (uint32_t)sc3_open_rc && (unsigned char *)dst >= sc3_mem &&
                   (unsigned char *)dst + n <= sc3_mem + sc3_block);
        if (sc3_read_busy) return VJO_ERR_BUSY;
        if (sc3_read_error) return sc3_read_error;
        for (uint32_t i = 0; i < n; i++) ((unsigned char *)dst)[i] = (unsigned char)((off + i) * 7u);
        return (int)n;
    }
    for (uint32_t i = 0; i < n; i++) ((unsigned char *)dst)[i] = test_weight_byte(off + i, TEST_FL10_SEED);
    return (int)n;
}
int vjoOcrModelClose(uint32_t seq)
{
    TEST_CHECK(seq == 12);
    if (sc3_file_open && sc3_close_busy) { sc3_close_busy--; return VJO_ERR_BUSY; }
    if (cache_pending) { cache_closes++; cache_pending = 0; }
    file_open_now = sc3_file_open = 0;
    return 0;
}

/* ---- the runtime: spies ---- */
static unsigned spy_lines = 1; /* the lines the spy finder finds (0: a region without text) */
static int spy_reads_rows; /* 1: the spy reads every row of the region once, as a finder's pass does */
int vocr_find_lines_rows(const VocrLinesRows *src, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    finds++;
    for (unsigned y = 0; spy_reads_rows && y < src->height; y++)
        if (!src->row(src->user, y)) return VOCR_LINES_E_SOURCE;
    memset(out, 0, sizeof(*out));
    out->count = spy_lines;
    out->lines[0] = (VocrLine){10, 2, 180, 3, 0};
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
    memset(m, 0, sizeof(*m)); m->blob = blob; m->bytes = bytes;
    return VOCR_OK;
}
size_t vocr_arena_bytes(const vocr_model *m, const vocr_opts *o) { return vjo_vocr_model("FL10_w8.vocr")->arena_bytes; }
int vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes) { return VOCR_OK; }
int vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
{
    runs++;
    snprintf(text, cap, "%s", spy_text);
    extra->width = img->width; extra->frames = img->width / 8;
    return VOCR_OK;
}

/* ---- helpers ---- */
static VjoGameOcrRequest request(uint32_t pick)
{
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=W, .fb_height=H, .width=W, .height=H};
    snprintf(r.model, sizeof(r.model), "FL10_w8.vocr");
    snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
    r.hook_pick = pick;
    return r;
}
static void reset(void)
{
    if (weights_live) TEST_CHECK(!drop_weights());
    if (lm_live || lm_res.data) TEST_CHECK(!drop_lm());
    lm_res.lm = NULL; lm_res.data = NULL; lm_res.capacity = 0; lm_res.size = lm_res.stamp = 0; lm_uid = -1;
    spy_reads_rows = 0;
    if (sc3_uid >= 0 && sc3_live) { /* an SC3 charset a test left: given back as the worker's stop does */
        sc3_set_file = NULL;
        TEST_CHECK(!game_memblock_free(GL_SC3, VJO_GITEM_SC3, sc3_uid, sc3_bytes_held));
    }
    worker = workspace_uid = weights_uid = capture_uid = line_uid = -1;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    model_owned = 0; quitting = active_seq = 0; blocked = 0;
    memset(&hk, 0, sizeof(hk));
    memset(fs, 0, sizeof(fs));
    fake_mode = VJO_TEXT_SOURCE_AUTO; fake_installed = 1u << 16 | 4u; fake_refused = 2; fake_verified = 0;
    snaps = texts = bump_at = 0; bump_id = 0; bump_text = NULL;
    seq_reads = hook_lines = 0; hook_line_seq = hook_line_bytes = 0; hook_line_rc = 0;
    keeps = 0; memset(kept_ids, 0, sizeof(kept_ids)); memset(kept_keys, 0, sizeof(kept_keys)); fake_call_dropped = fake_call_replaced = 0;
    capture_live = job_live = weights_live = 0; capture_block = capture_allocs = 0; live_bytes = 0;
    shot_allocs = probe_allocs = 0;
    available = 64*1024*1024; clock_value = 0; free_mib = 0;
    frame_live = 0; frame_block = frame_copies = 0; /* API 55, and the worker's frame state */
    frame_uid = -1; frame_base = NULL; frame_bytes_held = 0; memset(&pz, 0, sizeof(pz)); pause_capped = 0;
    file_open_now = cancel_now = rows_error = 0;
    opens = finds = runs = 0; rows_calls = 0; hook_publishes = 0; hook_publish_rc = 0;
    spy_text = "猫";
    spy_lines = 1;
    cache_loads = cache_saves = build_reads = 0;
    saved_cache_size = cache_load_error = cache_save_error = build_error = 0;
    cache_busy = cache_closes = 0; cache_pending = cache_cancel = fail_capture_free = fail_weights_free = 0;
    memset(saved_cache, 0, sizeof(saved_cache));
    memset(fake_build, 0xa7, sizeof(fake_build));
    memset(pixels, 0, sizeof(pixels));
    sc3_uid = -1; sc3_bytes_held = 0; sc3_tries = 0;
    sc3_want = NULL; sc3_set_file = NULL; sc3_set_bytes = 0; sc3_sets = sc3_opens = 0; sc3_accept = 1; sc3_open_rc = 0;
    sc3_live = 0; sc3_block = 0; sc3_file_open = 0;
    sc3_read_busy = sc3_read_error = sc3_base_fail = 0; sc3_close_busy = sc3_free_fail = sc3_allocs = sc3_double = 0;
}
/* One job; between jobs the line cache is emptied so the spy's text is read again. */
static int run(uint32_t pick, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(pick);
    vjo_game_ocr_cache_clear();
    rows_calls = 0;
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out->cleanup_status);
    TEST_CHECK(out->hook_mode == fake_mode && out->hook_installed == fake_installed && out->hook_refused == fake_refused);
    return rc;
}
static void stream(uint32_t id, uint32_t kind, uint32_t seq, const char *text)
{
    fs[id].kind = kind;
    fs[id].seq = seq;
    fs[id].text = text;
}
/* One job whose request carries speed (API 43: VJO_GAME_OCR_S_TRUST), as run() does. */
static int run_speed(uint32_t pick, uint32_t speed, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(pick);
    r.speed = speed;
    vjo_game_ocr_cache_clear();
    rows_calls = 0;
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out->cleanup_status);
    return rc;
}
/* The job read exactly one first pass over the region: from row 0 in strips, each continuing it
 * (VJO_ROWS_SIG), the last ending it (VJO_ROWS_LAST), and nothing more. */
static int first_pass_only(void)
{
    unsigned rows = vjo_vocr_strip_rows(W, H), y = 0, i;
    for (i = 0; i < rows_calls && i < MAX_ROWS_CALLS; i++) {
        uint32_t want = VJO_ROWS_SIG | (y + rows_log[i].n == H ? VJO_ROWS_LAST : 0u);
        if (rows_log[i].y != y || rows_log[i].n != (H - y < rows ? H - y : rows) || rows_log[i].flags != want) return 0;
        y += rows_log[i].n;
    }
    return y == H && i == (H + rows - 1u) / rows;
}

static const char LINE1[] = "「明日の朝、駅前の公園でもう一度会おうよ」";
static const char OCR1[] = "明日の朝、駅前の公園でもう一度会おうよ";
static const char LINE2[] = "「そんなこと急に言われても、困るんだけどな」";
static const char OCR2[] = "そんなこと急に言われても、困るんだけとな";
static const char LINE3[] = "「だったら今夜のうちに準備しておけばいいじゃない」";
static const char OCR3[] = "だったら今夜のうちに準備しておけばいいじゃない";

/* ---- tests ---- */
static void nothing_installed_is_ocr_as_before(void)
{
    VjoGameOcrResult out;
    reset();
    fake_installed = 0; fake_refused = 0; fake_mode = VJO_TEXT_SOURCE_OCR;
    stream(9, VJO_HOOK_KIND_CALL, 1, LINE1); /* never looked at */
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1));
    TEST_CHECK(!snaps && !texts && opens == 1 && runs == 1);
    TEST_CHECK(!out.text_from && !out.hook_flags && !out.hook_score && !out.hook_streams);
    TEST_CHECK(!out.hook_mode && !out.hook_installed && !out.hook_refused);
    /* nor with refused sites only: nothing hooked is nothing installed */
    fake_refused = 3; fake_mode = VJO_TEXT_SOURCE_AUTO;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1) && !snaps && !out.text_from);
    TEST_CHECK(out.hook_mode == VJO_TEXT_SOURCE_AUTO && out.hook_refused == 3);
}

static void agreeing_stream_replaces_ocr_text(void)
{
    VjoGameOcrResult out;
    reset();
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1);
    stream(10, VJO_HOOK_KIND_CALL, 6, "セーブしますか？");
    stream(2, VJO_HOOK_KIND_SITE, 3, LINE3);
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(!strcmp(out.text, LINE1), "%s", out.text);
    TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), OCR1)); /* OCR's text after it, for the log */
    TEST_CHECK(out.text_from == 9 && out.hook_flags == VJO_HOOK_R_AGREED && out.hook_score == 1000);
    TEST_CHECK(out.hook_streams == (1u << 8 | 1u << 9 | 1u << 1));
    TEST_CHECK(opens == 1 && runs == 1 && out.recognized == 1); /* OCR ran */
    /* every byte after the log's text is zero */
    size_t n = strlen(out.text), m = strlen(out.text + n + 1);
    for (size_t i = n + m + 2; i < sizeof(out.text); i++) if (out.text[i]) { TEST_CHECK(!"stale byte"); break; }
    /* OCR reads something else: its text stays */
    spy_text = "全然関係のない画面の文字がここに表示されている";
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, spy_text));
    TEST_CHECK(!out.text_from && !out.hook_flags && out.hook_score < VJO_HOOK_AGREE && out.hook_score > 0);
}

static void locked_stream_answers_with_the_first_pass_only(void)
{
    VjoGameOcrResult out;
    reset();
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1);
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && out.hook_flags == VJO_HOOK_R_AGREED);
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
    spy_text = OCR2;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED) && hk.sel.locked == 9);
    TEST_CHECK(opens == 2 && runs == 2 && finds == 2);
    /* new text in the locked stream: no model, no finding, no recognition, one first pass */
    stream(9, VJO_HOOK_KIND_CALL, 7, LINE3);
    spy_text = "読まれないはずの文字";
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(!strcmp(out.text, LINE3), "%s", out.text);
    TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), LINE3)); /* the stream's own text */
    TEST_CHECK(out.text_from == 9 && out.hook_flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(out.hook_score == 0 && out.hook_streams == 1u << 8);
    TEST_CHECK(opens == 2 && runs == 2 && finds == 2);
    TEST_CHECK_(first_pass_only(), "%u rows calls", rows_calls);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_HOOK && out.strip_rows == vjo_vocr_strip_rows(W, H));
    TEST_CHECK(out.captures == rows_calls && !out.lines && !out.recognized && out.weights == VJO_GAME_OCR_WEIGHTS_NONE);
    TEST_CHECK(capture_allocs == 3 && capture_block == PAGE_ROUND(vjo_vocr_strip_rows(W, H) * W));
    TEST_CHECK(out.capture_bytes == capture_block && out.capture_memory == VJO_GAME_OCR_MEM_USER);
    TEST_CHECK(out.reserve_bytes == GAME_RESERVE && out.need_workspace == capture_block && out.free_user > 0);
    /* the first pass's PASS and SIG steps are in the log, as for OCR */
    unsigned pass = 0, sig = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        pass += out.step[i].id == VJO_GSTEP_PASS && !out.step[i].rc;
        sig += out.step[i].id == VJO_GSTEP_SIG;
    }
    TEST_CHECK(pass == 1 && sig == 1);
    /* nothing new since: OCR reads again, and the stream still agrees */
    spy_text = OCR3;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && opens == 3 && runs == 3);
    TEST_CHECK(out.text_from == 9 && out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED));
    /* a locked stream busy as the job begins: OCR, and no miss */
    fs[9].seq++;
    fs[9].busy = 1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && opens == 4 && !out.text_from && !strcmp(out.text, OCR3));
    TEST_CHECK(hk.sel.locked == 9 && hk.sel.misses == 0 && !out.hook_streams);
}

/* API 41, the hook watch (hooks_watch, between jobs): once a source answers without recognition, a newer
 * line in it is reported once (vjoOcrHookLine: its sequence and raw length), read without the slot's lock;
 * the job's own snapshot, other streams, a caller that took the source's slot, and a report within
 * HOOK_LINE_GAP_US of the last are not; a refused report is tried again. */
static void hook_watch_reports_a_new_line(void)
{
    VjoGameOcrResult out;
    reset();
    /* nothing learned: nothing watched */
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1);
    hooks_watch();
    TEST_CHECK(!hook_lines && !seq_reads);
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && !hk.sel.interval);
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
    hooks_watch();
    TEST_CHECK(!hook_lines);
    spy_text = OCR2;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.locked == 9 && hk.watch_seen == 6);
    /* the line the job read is no news */
    hooks_watch();
    TEST_CHECK(!hook_lines && seq_reads == 1);
    /* the game copies the next line: reported once, its sequence and length; a slot a game thread is
     * writing does not stop the watch, which takes no lock */
    stream(9, VJO_HOOK_KIND_CALL, 7, LINE3);
    fs[9].busy = 1;
    hooks_watch();
    TEST_CHECK(hook_lines == 1 && hook_line_seq == 7 && hook_line_bytes == strlen(LINE3));
    fs[9].busy = 0;
    hooks_watch();
    TEST_CHECK(hook_lines == 1);
    /* the job the Shell starts for it answers from the stream, without recognition */
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && !strcmp(out.text, LINE3));
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR) && hk.watch_seen == 7);
    hooks_watch();
    TEST_CHECK(hook_lines == 1);
    /* another stream's text is no line of the dialogue */
    clock_value += HOOK_LINE_GAP_US;
    stream(10, VJO_HOOK_KIND_CALL, 8, "セーブしますか？");
    hooks_watch();
    TEST_CHECK(hook_lines == 1);
    /* within HOOK_LINE_GAP_US of the last report: the next round after it */
    hk.watch_sent = now_wide();
    stream(9, VJO_HOOK_KIND_CALL, 9, LINE2);
    hooks_watch();
    TEST_CHECK(hook_lines == 1);
    clock_value += HOOK_LINE_GAP_US;
    hooks_watch();
    TEST_CHECK(hook_lines == 2 && hook_line_seq == 9);
    /* refused (another app in front, a job claimed): tried again at the next round */
    clock_value += HOOK_LINE_GAP_US;
    stream(9, VJO_HOOK_KIND_CALL, 11, LINE1);
    hook_line_rc = VJO_ERR_PERM;
    hooks_watch();
    TEST_CHECK(hook_lines == 3 && hook_line_seq == 9 && hk.watch_seen == 9);
    hook_line_rc = 0;
    hooks_watch();
    TEST_CHECK(hook_lines == 4 && hook_line_seq == 11 && hk.watch_seen == 11);
    /* another caller took the source's slot: its text is not the source's line */
    clock_value += HOOK_LINE_GAP_US;
    fs[9].key = 0x4444;
    fs[9].seq = 12;
    hooks_watch();
    TEST_CHECK(hook_lines == 4);
}

/* Luna's sites answer without recognition from their first line: each new line of one is reported. */
static void hook_watch_reports_luna_lines(void)
{
    VjoGameOcrResult out;
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 1, "はい"); fs[2].luna = &fake_luna_site;
    hooks_watch();
    TEST_CHECK(!hook_lines); /* no site has held text in a job yet */
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 2);
    TEST_CHECK(hk.luna_seen == VJO_HOOK_STREAM_BIT(2) && !hk.sel.interval);
    hooks_watch();
    TEST_CHECK(!hook_lines);
    stream(2, VJO_HOOK_KIND_SITE, 3, "うん");
    hooks_watch();
    TEST_CHECK(hook_lines == 1 && hook_line_seq == 3 && hook_line_bytes == strlen("うん"));
}

/* A locked CALL stream whose slot another caller took (audit-hooks F2: the slot's key is the new caller's,
 * VjoHookSnap.key, which hooks_snapshot passes as VjoHookCand.key): the selection unlocks it at once, so the
 * new caller's first text is checked by OCR, never shown without it (hooks-fix-text's F3(c)). */
static void replaced_slot_unlocks(void)
{
    VjoGameOcrResult out;
    reset();
    fs[9].key = 0x1230u; /* the caller's return address minus segment 0's base */
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1);
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9);
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
    spy_text = OCR2;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.locked == 9 && hk.sel.locked_key == 0x1230u);
    TEST_CHECK(hk.cand[0].id == 9 && hk.cand[0].key == 0x1230u);
    /* the same key: the locked stream's new text answers without OCR */
    stream(9, VJO_HOOK_KIND_CALL, 7, LINE3);
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR) && runs == 2);
    /* another caller took slot 9: its key differs, its text is new; OCR reads the screen, no lock */
    fs[9].key = 0x2340u;
    stream(9, VJO_HOOK_KIND_CALL, 8, "「別の呼び出し元の文字列ですよね」");
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && runs == 3);
    TEST_CHECK(hk.cand[0].key == 0x2340u);
    TEST_CHECK_(!(out.hook_flags & (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR)), "flags %08X", (unsigned)out.hook_flags);
    TEST_CHECK(hk.sel.locked == 0 && !out.text_from && !strcmp(out.text, OCR1));
}

static void failed_answer_changes_nothing(void)
{
    VjoGameOcrResult out;
    reset();
    hk.sel = (VjoHookSelect){.locked = 9, .locked_seq = 5, .streak_id = 9, .streak = 2,
                               .interval = 8, .locked_encoding = VJO_HOOK_ENC_UTF8};
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE1);
    rows_error = VJO_ERR_ARG; /* every rows call refused: the pass cannot be read */
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_SOURCE);
    TEST_CHECK(!out.text[0] && !out.text_from && !out.hook_flags && out.hook_streams == 1u << 8);
    TEST_CHECK(hk.sel.locked == 9 && hk.sel.locked_seq == 5 && !opens);
    /* cancelled while it reads the pass */
    rows_error = 0;
    cancel_now = 1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_CANCELLED && !out.text[0] && !out.text_from);
    TEST_CHECK(hk.sel.locked_seq == 5);
    /* too little memory for one strip beside the reserve */
    cancel_now = 0;
    available = GAME_RESERVE + 4096;
    unsigned allocs = capture_allocs;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && !out.text_from && capture_allocs == allocs);
    TEST_CHECK(out.reserve_bytes == GAME_RESERVE && out.need_workspace == PAGE_ROUND(vjo_vocr_strip_rows(W, H) * W));
    /* the next job answers as the failed ones would have */
    available = 64*1024*1024;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, LINE1));
    TEST_CHECK(out.text_from == 9 && out.hook_flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR) && hk.sel.locked_seq == 6);
    /* a failed OCR job leaves the streak as it was too */
    hk.sel = (VjoHookSelect){0};
    spy_text = OCR1;
    cancel_now = 0;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.streak == 1);
    available = 1024; /* the memory check refuses OCR */
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && hk.sel.streak == 1 && !out.text_from);
}

static void pinned_and_ocr_only(void)
{
    VjoGameOcrResult out;
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 4, "えっ！？　本当に…？");
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1);
    spy_text = OCR1;
    /* pinned with text: that text, normalised as OCR's is, without OCR */
    TEST_CHECK(run(2, &out) == 0 && !opens && first_pass_only());
    TEST_CHECK_(!strcmp(out.text, "えっ!?　本当に…?"), "%s", out.text); /* core/textnorm.h keeps spaces */
    TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), "えっ！？　本当に…？"));
    TEST_CHECK(out.text_from == 2 && out.hook_flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    TEST_CHECK(!memcmp(&hk.sel, &(VjoHookSelect){0}, sizeof(hk.sel)));
    /* pinned without text: OCR, its text, even though stream 9 agrees */
    TEST_CHECK(run(5, &out) == 0 && opens == 1 && !strcmp(out.text, OCR1) && !out.text_from);
    TEST_CHECK(!out.hook_flags && out.hook_score == 1000);
    /* OCR only */
    TEST_CHECK(run(VJO_HOOK_PICK_OCR, &out) == 0 && opens == 2 && !strcmp(out.text, OCR1) && !out.text_from);
    TEST_CHECK(out.hook_score == 1000 && !memcmp(&hk.sel, &(VjoHookSelect){0}, sizeof(hk.sel)));
    /* a pick that is none of them: refused as an unknown request value is, before any snapshot */
    unsigned before = snaps;
    TEST_CHECK(run(VJO_HOOK_STREAMS + 1u, &out) == VJO_E_SOURCE && opens == 2 && snaps == before);
    TEST_CHECK(run(0x80, &out) == VJO_E_SOURCE && snaps == before && !out.text_from && !out.hook_streams);
}

static void self_test_jobs_are_ocr(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset();
    hk.sel = (VjoHookSelect){.locked = 9, .locked_seq = 5, .streak_id = 9, .streak = 2,
                               .interval = 8, .locked_encoding = VJO_HOOK_ENC_UTF8};
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE1);
    spy_text = OCR1;
    r = request(9); /* even pinned */
    r.speed = VJO_GAME_OCR_S_NOCACHE;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && opens == 1 && !strcmp(out.text, OCR1) && !out.text_from);
    TEST_CHECK(out.hook_score == 1000 && hk.sel.locked_seq == 5 && hk.sel.streak == 2);
}

static void shot_and_probe_take_no_snapshot(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=W, .fb_height=H, .width=W, .height=H,
                           .flags=VJO_GAME_OCR_F_SHOT};
    reset();
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE1);
    snprintf(r.shot, sizeof(r.shot), "123-456");
    vjo_game_ocr_run_request(&r, &out); /* its block is refused here: the shot's own tests run it */
    TEST_CHECK(shot_allocs > 0 && !snaps && !out.text_from && out.hook_installed == fake_installed);
#if VJO_DEBUG
    r = request(VJO_HOOK_PICK_AUTO);
    r.flags = VJO_GAME_OCR_F_PROBE;
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(probe_allocs > 0 && !snaps && !out.text_from);
#endif
}

static void shown_text_is_whole(void)
{
    static char longer[2048], cut[1024];
    VjoGameOcrResult out;
    reset();
    /* 300 kana (900 B): longer than a stream's comparison copy (HOOK_TEXT_CAP) */
    longer[0] = '\0';
    for (unsigned i = 0; i < 300; i++) strcat(longer, i % 3 ? "か" : "き");
    TEST_CHECK(strlen(longer) == 900 && strlen(longer) >= HOOK_TEXT_CAP);
    stream(9, VJO_HOOK_KIND_CALL, 5, longer);
    spy_text = longer;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9);
    TEST_CHECK(!strcmp(out.text, longer) && !strcmp(vjo_text_raw(out.text, sizeof(out.text)), longer));
    /* the stream moved on between the snapshot and the show: the job's copy, cut at a character */
    hk.sel = (VjoHookSelect){0};
    bump_at = VJO_HOOK_STREAMS + 2u; /* the snapshot snaps every ID, then stream 9 again; the show's is next */
    bump_id = 9;
    bump_text = LINE2;
    snaps = 0;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9);
    memcpy(cut, longer, HOOK_TEXT_CAP - 1u);
    cut[HOOK_TEXT_CAP - 1u] = '\0';
    TEST_CHECK_(!strcmp(out.text, cut), "%u B", (unsigned)strlen(out.text));
}

static void full_pool_keeps_the_pick_and_the_newest(void)
{
    static char texts_of[VJO_HOOK_STREAMS + 1u][512];
    VjoGameOcrResult out;
    reset();
    /* every stream 399 B, stream 15 the oldest and locked; the pool holds five and a cut sixth */
    for (uint32_t id = 1; id <= VJO_HOOK_STREAMS; id++) {
        for (unsigned i = 0; i < 133; i++) { /* 133 of one kana, U+3041 + id: 399 B */
            texts_of[id][3 * i] = (char)0xE3;
            texts_of[id][3 * i + 1] = (char)0x81;
            texts_of[id][3 * i + 2] = (char)(0x81 + id);
        }
        texts_of[id][399] = '\0';
        stream(id, id <= VJO_HOOK_SITE_STREAMS ? VJO_HOOK_KIND_SITE : VJO_HOOK_KIND_CALL, 100 + id, texts_of[id]);
        fs[id].key = id * 0x100u; /* each fake caller/site has its own stable source identity */
    }
    fs[15].seq = 1;
    hk.sel = (VjoHookSelect){.locked = 15, .locked_seq = 1, .streak_id = 15, .streak = 2, .locked_key = 0xf00u,
                               .interval = 8, .locked_encoding = VJO_HOOK_ENC_UTF8};
    fs[15].seq = 2; /* new text: it answers */
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 15);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(out.hook_streams == (1u << VJO_HOOK_STREAMS) - 1u); /* every stream holds text */
    TEST_CHECK(hk.n == 6 && hk.cand[0].id == 15);
    for (unsigned i = 1; i < hk.n; i++) TEST_CHECK(hk.cand[i].id == VJO_HOOK_STREAMS + 1u - i); /* the newest */
    TEST_CHECK(hk.cand[5].n < 399 && hk.cand[4].n == 399);
}

/* audit-hooks F2: the stream that agreed with OCR (text_from, AGREED) and a pinned pick keep their CALL slot
 * (vjo_hooks_keep); nothing else is kept; CALLS_FULL is set while a new caller found every CALL slot taken
 * (vjo_hooks_call_stats: dropped or replaced), in a result with text, never in a failed one. */
static void keep_and_calls_full(void)
{
    VjoGameOcrResult out;
    reset();
    fs[9].key = 0x1230u;
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1);
    stream(12, VJO_HOOK_KIND_CALL, 6, "セーブしますか？");
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && out.hook_flags == VJO_HOOK_R_AGREED);
    TEST_CHECK(keeps == 1 && kept_ids[0] == 9 && kept_keys[0] == 0x1230u); /* the key the job's snapshot saw */
    /* OCR reads something else: nothing agrees, nothing kept */
    keeps = 0;
    spy_text = "全然関係のない画面の文字がここに表示されている";
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !out.text_from && !keeps);
    /* OCR only: nothing kept */
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_OCR, &out) == 0 && !out.text_from && !keeps);
    /* pinned: the pick is kept, with text or without */
    TEST_CHECK(run(12, &out) == 0 && out.text_from == 12 && (out.hook_flags & VJO_HOOK_R_PINNED));
    TEST_CHECK(keeps == 1 && kept_ids[0] == 12 && kept_keys[0] == 0); /* pinned: whichever caller holds it */
    keeps = 0;
    TEST_CHECK(run(15, &out) == 0 && !out.text_from && keeps == 1 && kept_ids[0] == 15);
    TEST_CHECK(!(out.hook_flags & VJO_HOOK_R_CALLS_FULL));
    /* a new caller was dropped, or replaced a slot: CALLS_FULL on every result with text */
    hk.sel = (VjoHookSelect){0};
    keeps = 0;
    fake_call_dropped = 1;
    spy_text = "全然関係のない画面の文字がここに表示されている";
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.hook_flags == VJO_HOOK_R_CALLS_FULL && !keeps);
    fake_call_dropped = 0;
    fake_call_replaced = 3;
    spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_CALLS_FULL) && keeps == 1 && kept_ids[0] == 9);
    /* a failed job: no flags, nothing kept */
    keeps = 0;
    available = 1024;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && !out.hook_flags && !out.text_from && !keeps);
    available = 64*1024*1024;
    /* nothing installed: neither */
    fake_installed = 0;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !out.hook_flags && !keeps);
}

static void prime_hook_cache_trusted(uint32_t interval, uint32_t trusted)
{
    VjoHookLearned learned = {0, 0x1234u, VJO_HOOK_ENC_UTF8, interval, trusted};
    VjoHookCacheKey key = {0};
    memcpy(key.title, "PCSG00415", 10);
    memcpy(hk.cache_key.title, key.title, sizeof(key.title));
    memcpy(key.build, fake_build, sizeof(key.build));
    TEST_ASSERT(hook_revision(key.definition));
    key.mode = fake_mode;
    TEST_ASSERT(vjo_hook_cache_put(saved_cache, 0, &key, &learned) == 1);
    saved_cache_size = sizeof(saved_cache);
}

static void prime_hook_cache(uint32_t interval) { prime_hook_cache_trusted(interval, 0); }

static void restart_hook_selection(void)
{
    memset(&hk.sel, 0, sizeof(hk.sel));
    memset(&hk.cached, 0, sizeof(hk.cached));
    hk.cache_attempted = hk.cache_ready = hk.build_known = 0;
}

static void luna_never_loads_ocr_or_generic_learning(void)
{
    VjoGameOcrResult out;
    VjoHookSelect empty = {0};
    reset(); prime_hook_cache(32); /* even an available generic cache must wait until it is needed */
    stream(2, VJO_HOOK_KIND_SITE, 1, "はい"); fs[2].luna = &fake_luna_site;
    stream(9, VJO_HOOK_KIND_CALL, 100, LINE1);
    for (unsigned i = 0; i < 72; i++) { /* 36 fresh readings plus their unchanged reopens */
        fs[2].seq = i / 2u + 1u;
        fs[2].text = i / 2u % 2u ? "うん" : "はい";
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
        TEST_CHECK(!strcmp(out.text, fs[2].text) && out.text_from == 2);
        TEST_CHECK(out.hook_flags == (VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR) && !out.hook_score);
        TEST_CHECK(first_pass_only() && !out.recognized && out.weights == VJO_GAME_OCR_WEIGHTS_NONE);
        TEST_CHECK(!opens && !finds && !runs && !keeps);
        TEST_CHECK(!cache_loads && !cache_saves && !build_reads && !hk.cache_attempted);
        TEST_CHECK(!memcmp(&hk.sel, &empty, sizeof(empty)));
    }
}

static void luna_preserves_generic_state_and_falls_back_when_unavailable(void)
{
    VjoGameOcrResult out;
    VjoHookSelect was;
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 1, LINE3); fs[2].luna = &fake_luna_site;
    fs[2].filtered_empty = 1; /* a real raw Luna snapshot cannot bypass OCR if its filter discards it */
    stream(9, VJO_HOOK_KIND_CALL, 2, LINE1); spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && runs == 1);
    TEST_CHECK(out.hook_streams == (VJO_HOOK_STREAM_BIT(2) | VJO_HOOK_STREAM_BIT(9)));
    TEST_CHECK(hk.n == 1 && hk.cand[0].id == 9 && out.hook_flags == VJO_HOOK_R_AGREED);
    TEST_CHECK(hk.sel.streak == 1 && !hk.sel.interval);
    /* A busy Luna stream is unavailable too: the second regular line still needs OCR to learn. */
    fs[2].filtered_empty = 0; fs[2].busy = 1;
    stream(9, VJO_HOOK_KIND_CALL, 3, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && runs == 2);
    TEST_CHECK(hk.sel.locked == 9 && hk.sel.interval == 8);
    hk.sel.unchecked = 7; /* preserve an overdue generic check while the trusted script answers */
    was = hk.sel;
    fs[2].busy = 0;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 2);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(runs == 2 && !memcmp(&hk.sel, &was, sizeof(was)));
    fs[2].text = NULL;
    stream(9, VJO_HOOK_KIND_CALL, 4, LINE3); spy_text = OCR3;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 9 && runs == 3);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED));
    TEST_CHECK(hk.sel.interval == 16 && !hk.sel.unchecked);
}

static void luna_survives_pool_saturation_and_uses_newest_script(void)
{
    static char long_text[400];
    VjoGameOcrResult out;
    VjoHookSelect was;
    reset();
    for (unsigned i = 0; i < 133; i++) memcpy(long_text + 3 * i, "あ", 3);
    long_text[399] = '\0';
    for (uint32_t id = 1; id <= VJO_HOOK_STREAMS; id++) {
        stream(id, id <= VJO_HOOK_SITE_STREAMS ? VJO_HOOK_KIND_SITE : VJO_HOOK_KIND_CALL,
               100 + id, long_text);
        fs[id].key = id * 0x100u;
    }
    fs[2].seq = 1; fs[2].luna = &fake_luna_site; /* oldest text, behind enough CALL text to fill the pool */
    hk.sel = (VjoHookSelect){.locked = 15, .locked_seq = 115, .locked_key = 0xf00u,
                              .locked_encoding = VJO_HOOK_ENC_UTF8, .interval = 8, .unchecked = 7};
    was = hk.sel;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 2 && !runs && !opens);
    TEST_CHECK(hk.cand[0].id == 2 && hk.cand[0].n == 399 && !strcmp(out.text, long_text));
    TEST_CHECK(!memcmp(&hk.sel, &was, sizeof(was)));
    fs[4].luna = &fake_luna_site; fs[4].seq = 2; fs[4].text = LINE1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 4 && !strcmp(out.text, LINE1));
    fs[2].seq = 2; /* equal sequence: stable lowest stream ID, independent of generic preference */
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 2);
    fs[2].seq = UINT32_MAX; fs[4].seq = 1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 4);
    TEST_CHECK(!runs && !opens && !memcmp(&hk.sel, &was, sizeof(was)));
}

static void luna_respects_explicit_picks_and_self_tests(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 1, "はい"); fs[2].luna = &fake_luna_site;
    stream(9, VJO_HOOK_KIND_CALL, 2, LINE1); spy_text = OCR1;
    TEST_ASSERT(run(9, &out) == 0 && out.text_from == 9 && !opens && !runs);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR));
    TEST_ASSERT(run(2, &out) == 0 && out.text_from == 2 && !opens && !runs);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    TEST_ASSERT(run(VJO_HOOK_PICK_OCR, &out) == 0 && !out.text_from && runs == 1);
    TEST_CHECK(!strcmp(out.text, OCR1) && !(out.hook_flags & VJO_HOOK_R_NO_OCR));
    for (unsigned fallback = 0; fallback < 2; fallback++) {
        r = request(VJO_HOOK_PICK_AUTO);
        if (fallback) r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK;
        else r.speed = VJO_GAME_OCR_S_NOCACHE;
        vjo_game_ocr_cache_clear();
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && !out.text_from);
        TEST_CHECK(runs == fallback + 2u && !strcmp(out.text, OCR1));
    }
}

static void luna_capture_failure_preserves_state(void)
{
    VjoGameOcrResult out;
    VjoHookSelect was;
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 1, "はい"); fs[2].luna = &fake_luna_site;
    hk.sel = (VjoHookSelect){.locked = 9, .locked_seq = 5, .locked_key = 0x1234u,
                              .locked_encoding = VJO_HOOK_ENC_UTF8, .interval = 32, .unchecked = 31};
    was = hk.sel;
    rows_error = VJO_ERR_ARG;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_SOURCE);
    TEST_CHECK(!out.text[0] && !out.text_from && !out.hook_flags && !opens && !runs);
    TEST_CHECK(!memcmp(&hk.sel, &was, sizeof(was)));
    rows_error = 0; available = GAME_RESERVE + 4096;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM);
    TEST_CHECK(!out.text[0] && !out.text_from && !out.hook_flags && !memcmp(&hk.sel, &was, sizeof(was)));
    available = 64*1024*1024;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 2 && !strcmp(out.text, "はい"));
    TEST_CHECK(first_pass_only() && !opens && !runs && !cache_loads && !cache_saves);
    TEST_CHECK(!memcmp(&hk.sel, &was, sizeof(was)));
}

static void remembered_hook_worker_cadence(void)
{
    VjoGameOcrResult out;
    reset();
    prime_hook_cache(32);
    /* A stable caller is slot15 this launch; the saved record contains no temporary stream ID. */
    fs[15].key = 0x1234;
    stream(15, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && runs == 1 && out.text_from == 15);
    TEST_CHECK(hk.sel.interval == 32 && !hk.sel.unchecked && !hk.sel.confirm);
    TEST_CHECK(build_reads == 1 && cache_loads == 1 && !cache_saves);
    for (unsigned i = 1; i <= 32; i++) {
        const char *line = i & 1u ? LINE2 : LINE3;
        stream(15, VJO_HOOK_KIND_CALL, i + 1, line); spy_text = line;
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
        TEST_CHECK_(runs == (i == 32 ? 2u : 1u), "reading %u, runs %u", i, runs);
    }
    TEST_CHECK(hk.sel.interval == 32 && !hk.sel.unchecked && !cache_saves && cache_loads == 1);
    /* Reopening unchanged dialogue takes OCR's ordinary path, without advancing the learned counter. */
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && runs == 3 && !hk.sel.unchecked);
    TEST_CHECK(!cache_saves && cache_loads == 1);
    /* Restart: one bad OCR result retains the source; the next fresh agreement resets to 8 and saves once. With
     * hook_trust = off (these jobs carry no VJO_GAME_OCR_S_TRUST) that is 0.9.7's, write for write (#338's audit F1:
     * API 49 saved the source at 16 at the first miss, a write 0.9.7 never made, and restored it at 16). */
    restart_hook_selection();
    stream(15, VJO_HOOK_KIND_CALL, 34, LINE1); spy_text = "ここには全く違った文章が表示されています";
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !out.text_from);
    TEST_CHECK(hk.sel.interval == 32 && hk.sel.misses == 1 && !hk.sel.strong && !cache_saves);
    TEST_CHECK(hk.cached.interval == 32 && hk.cached.key == 0x1234u && !hk.cached.trusted);
    stream(15, VJO_HOOK_KIND_CALL, 35, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 15);
    TEST_CHECK(hk.sel.interval == 8 && !hk.sel.misses && cache_saves == 1);
}

/* #338's audit F1, the other half: with hook_trust = on a strong source at 32 is saved trusted (a write of its own when
 * a check's close agreement makes it strong, and one when a miss takes it back), and comes back trusted; its interval is
 * saved as it is (32, not API 49's 16 for one not trusted). A launch with hook_trust = off after one with it on saves
 * the source untrusted once and then as 0.9.7 would. */
static void remembered_trust_is_its_own_bit(void)
{
    VjoGameOcrResult out;
    reset();
    prime_hook_cache_trusted(32, 1);
    fs[15].key = 0x1234;
    stream(15, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = OCR1;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 15 && !runs);
    TEST_CHECK(hk.sel.strong && hk.sel.interval == 32 && cache_loads == 1 && !cache_saves);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    /* the same cache, trust off: its confirmation OCR, then the bit cleared in one write, then none */
    restart_hook_selection();
    stream(15, VJO_HOOK_KIND_CALL, 2, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 15 && runs == 1);
    TEST_CHECK(cache_saves == 1 && hk.cached.interval == 32 && !hk.cached.trusted);
    stream(15, VJO_HOOK_KIND_CALL, 3, LINE3); spy_text = LINE3;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 15 && cache_saves == 1);
    /* trust on again: not trusted until a close agreement at 32 (its confirmation), which saves it trusted */
    restart_hook_selection();
    stream(15, VJO_HOOK_KIND_CALL, 4, LINE1); spy_text = OCR1;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && runs == 2);
    TEST_CHECK(hk.sel.strong && cache_saves == 2 && hk.cached.interval == 32 && hk.cached.trusted);
    /* trusted, it answers without OCR and is not compared again (no miss can take its trust back: only its slot
     * changing hands, or hook_trust = off) */
    stream(15, VJO_HOOK_KIND_CALL, 5, LINE2); spy_text = "ここには全く違った文章が表示されています";
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && runs == 2 && out.text_from == 15);
    TEST_CHECK(cache_saves == 2);
    /* a source saved at 32 untrusted, trust on: a startup disagreement keeps it at 32 with no write (API 49 had saved
     * it at 16 and restored it there), and the next agreement takes it to 8, one write: 0.9.7's cadence */
    reset();
    prime_hook_cache_trusted(32, 0);
    fs[15].key = 0x1234;
    stream(15, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = "ここには全く違った文章が表示されています";
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && runs == 1 && !out.text_from);
    TEST_CHECK(hk.sel.interval == 32 && hk.sel.misses == 1 && !hk.sel.strong && !cache_saves);
    stream(15, VJO_HOOK_KIND_CALL, 2, LINE2); spy_text = OCR2;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 15);
    TEST_CHECK(hk.sel.interval == 8 && cache_saves == 1 && hk.cached.interval == 8 && !hk.cached.trusted);
}

static void hook_cache_worker_failures(void)
{
    VjoGameOcrResult out;
    reset(); prime_hook_cache(32);
    fs[9].key = 0x1234; stream(9, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = OCR1;
    rows_error = VJO_ERR_CANCELLED;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) != 0);
    TEST_CHECK(hk.sel.interval == 32 && hk.sel.confirm && !cache_saves);
    rows_error = 0;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.interval == 32 && !hk.sel.confirm);
    TEST_CHECK(!cache_saves); /* failed startup consumed neither comparison nor persistence */
    /* The complete executable changed, with the same title ID: no reuse of the learned32 interval. */
    restart_hook_selection(); fake_build[31] ^= 1;
    stream(9, VJO_HOOK_KIND_CALL, 2, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !hk.sel.interval && hk.sel.streak == 1);
    stream(9, VJO_HOOK_KIND_CALL, 3, LINE3); spy_text = OCR3;
    cache_save_error = -1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.interval == 8 && cache_saves == 1);
    stream(9, VJO_HOOK_KIND_CALL, 4, LINE1);
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && cache_saves == 1); /* no write per reading on failure */
    /* A cache read IO error is ignored, and never overwrites other games' data. */
    reset(); prime_hook_cache(32); cache_load_error = -1;
    fs[9].key = 0x1234; stream(9, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !hk.sel.interval);
    stream(9, VJO_HOOK_KIND_CALL, 2, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.interval == 8 && !cache_saves);
}

static void hook_cache_cleanup_ownership(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset(); prime_hook_cache(32); cache_busy = 2;
    fs[9].key = 0x1234; stream(9, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.interval == 32);
    TEST_CHECK(cache_closes == 1 && !cache_pending && !model_owned);
    /* A completed read whose user copy failed can still own staged broker data; ModelClose drains it. */
    restart_hook_selection(); cache_load_error = VJO_ERR_COPY;
    stream(9, VJO_HOOK_KIND_CALL, 2, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !hk.sel.interval);
    TEST_CHECK(cache_closes == 2 && !cache_pending && !model_owned && !cache_saves);
    /* Cancel during a BUSY cache operation: no leak, no consumed startup comparison, no persistence. */
    restart_hook_selection(); cache_load_error = 0; cache_busy = 1; cache_cancel = 1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_CANCELLED);
    TEST_CHECK(!cache_pending && !model_owned && !cache_saves);
    /* A failed primary capture cleanup rolls back learning and never starts a cache write. */
    reset(); prime_hook_cache(32);
    fs[9].key = 0x1234; stream(9, VJO_HOOK_KIND_CALL, 1, LINE1); spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    stream(9, VJO_HOOK_KIND_CALL, 2, LINE2); spy_text = "この画面は異なる文章を表示しています";
    hk.sel.confirm = 1; /* force a comparable disagreement check */
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && hk.sel.misses == 1);
    TEST_CHECK(!cache_saves); /* hook_trust = off: no write at a first miss, as in 0.9.7 (#338's audit F1) */
    stream(9, VJO_HOOK_KIND_CALL, 3, LINE3); spy_text = OCR3; fail_capture_free = 1;
    r = request(VJO_HOOK_PICK_AUTO); vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(out.cleanup_status && blocked && !cache_saves && hk.sel.interval == 32 && hk.sel.misses == 1);
    TEST_CHECK(!cleanup()); /* fixture releases the retained block before the next reset */
}

/* API 36: the border rule (core/textnorm.h) on the text a job returns, OCR's or a pinned stream's, only with
 * VJO_GAME_OCR_S_DROP_BORDERS (the Shell's non_japanese_filter = lines); the norm step's kind counts the lines,
 * and it is logged when they were all the text. The recognizer's (or the stream's) text stays after the NUL. */
static unsigned norm_step(const VjoGameOcrResult *out, unsigned *kind, unsigned *arg)
{
    unsigned n = 0;
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == VJO_GSTEP_NORM) { n++; *kind = out->step[i].kind; *arg = out->step[i].arg; }
    return n;
}
static void border_lines_follow_the_request(void)
{
    static const char border[] = "ーニーーーーニーニーーーー";
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    unsigned kind = 9, arg = 9;
    reset();
    spy_text = border; /* the region's one line */
    r = request(VJO_HOOK_PICK_OCR);
    r.speed = VJO_GAME_OCR_S_DROP_BORDERS;
    vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && opens == 1 && !out.text[0] && !out.text_from);
    TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), border));
    TEST_CHECK(norm_step(&out, &kind, &arg) == 1 && kind == 1 && arg == 1);
    r.speed = 0; /* non_japanese_filter = none: the line stays */
    vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && !strcmp(out.text, border));
    TEST_CHECK(norm_step(&out, &kind, &arg) == 1 && kind == 0 && arg == 0);
    spy_text = "ーニー、本当"; /* a line with anything else on it stays */
    r.speed = VJO_GAME_OCR_S_DROP_BORDERS;
    vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && !strcmp(out.text, spy_text));
    TEST_CHECK(norm_step(&out, &kind, &arg) == 1 && kind == 0);
    /* a pinned stream's text, normalised as OCR's is */
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 4, "本当に！\nーニーーーニー");
    r = request(2);
    r.speed = VJO_GAME_OCR_S_DROP_BORDERS;
    vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && !opens && out.text_from == 2);
    TEST_CHECK_(!strcmp(out.text, "本当に!"), "%s", out.text);
    TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), "本当に！\nーニーーーニー"));
    r.speed = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && out.text_from == 2);
    TEST_CHECK_(!strcmp(out.text, "本当に!\nーニーーーニー"), "%s", out.text);
}

/* API 36: the border rule on a stream that agrees with OCR and replaces its text (Auto: hook_show with OCR's
 * text kept after it), with and without VJO_GAME_OCR_S_DROP_BORDERS (audit-postproc S1). */
static void border_lines_on_an_agreeing_stream(void)
{
    static const char streamed[] = "「明日の朝、駅前の公園でもう一度会おうよ」\nーニーーーニー";
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    for (int bit = 1; bit >= 0; bit--) {
        reset();
        stream(9, VJO_HOOK_KIND_CALL, 5, streamed);
        spy_text = OCR1;
        r = request(VJO_HOOK_PICK_AUTO);
        r.speed = bit ? VJO_GAME_OCR_S_DROP_BORDERS : 0u;
        vjo_game_ocr_cache_clear();
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && out.text_from == 9);
        TEST_CHECK_(!strcmp(out.text, bit ? LINE1 : streamed), "bit %d: %s", bit, out.text);
        TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), OCR1)); /* OCR's text for the log */
    }
}

/* A game that leaves too little for any model: OCR is refused, and a
 * Rubi verified site's newest text answers unconfirmed, with one first pass beside the hook-only reserve and
 * no recognition; nothing is learned. A Luna site or CALL caller, an OCR-only pick, or too little even for
 * the pass: the refusal as before. With room for OCR nothing of this runs. */
static void verified_site_answers_when_ocr_has_no_memory(void)
{
    VjoGameOcrResult out;
    for (unsigned borrowed_filter = 0; borrowed_filter < 2; borrowed_filter++) {
        reset();
        available = VJO_GAME_RESERVE_HOOK_ONLY + 64 * 1024;
        stream(1, VJO_HOOK_KIND_SITE, 5, LINE1);
        stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && !out.text_from && !out.hook_flags);
        TEST_CHECK(capture_allocs == 0 && runs == 0);

        fake_verified = 1u; /* stream 1 */
        /* PCSG01198's verified entry borrows Luna's filter; it still needs the low-memory path. */
        fs[1].luna = borrowed_filter ? &fake_luna_site : NULL;
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0);
        TEST_CHECK_(!strcmp(out.text, LINE1), "%s", out.text);
        TEST_CHECK(out.text_from == 1 && out.hook_flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
        TEST_CHECK(out.hook_streams == (1u | 1u << 8) && runs == 0 && !out.lines);
        TEST_CHECK_(first_pass_only(), "%u rows calls", rows_calls);
        TEST_CHECK(out.source == VJO_GAME_OCR_SRC_HOOK && capture_allocs == 1);
        TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_HOOK_ONLY && out.need_workspace == capture_block);
        TEST_CHECK(!hk.sel.locked && !hk.sel.interval && !hk.sel.streak); /* nothing learned without OCR */

        /* too little even for the pass beside the reserve: refused, no text */
        stream(1, VJO_HOOK_KIND_SITE, 6, LINE3);
        available = VJO_GAME_RESERVE_HOOK_ONLY + 4096;
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && !out.text_from && !out.text[0]);
        /* an OCR-only pick: the refusal */
        available = VJO_GAME_RESERVE_HOOK_ONLY + 64 * 1024;
        TEST_CHECK(run(VJO_HOOK_PICK_OCR, &out) == VJO_E_OOM && !out.text_from);
        /* room for OCR: recognition as before, never the unconfirmed answer */
        available = 64 * 1024 * 1024;
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && runs >= 1 && !(out.hook_flags & VJO_HOOK_R_UNCONFIRMED));
    }
}

/* #280's audit F1: with every size's reserve 1 MiB, a game at アマツツミ's measured 2 MiB (PCSG01198, 2026-10-23)
 * passes the model's check, and on a console that reports its free memory in whole MiB (#324) the weights are refused
 * once the job's blocks are taken (2 MiB less those blocks reads 1 MiB, the reserve). No smaller model is left, so no
 * OCR can run in this game now, as when the check fails: with hook_trust = off a Rubi verified site's newest text
 * answers unconfirmed there too, after the try's blocks go back (its ALLOC steps leave the log, its passed check
 * stays), with one more pass beside the hook-only reserve. A CALL caller alone keeps the refusal; with the exact
 * figure OCR runs and agrees; a trusted site answers before any check, as before. */
static void verified_site_answers_when_its_weights_are_refused(void)
{
    VjoGameOcrResult out;
    for (unsigned exact = 0; exact < 2; exact++) {
        reset();
        free_mib = !exact;
        available = 2 * 1024 * 1024;
        fake_verified = 1u; /* stream 1: a Rubi verified site */
        stream(1, VJO_HOOK_KIND_SITE, 5, LINE1);
        spy_text = OCR1;
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
        TEST_CHECK_(!strcmp(out.text, LINE1) && out.text_from == 1, "%s from %u", out.text, (unsigned)out.text_from);
        if (exact) { /* room for the weights too: OCR reads, and the hook agrees */
            TEST_CHECK(runs == 1 && out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_SITE));
            TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE && weights_live);
            continue;
        }
        TEST_CHECK_(out.hook_flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE) && !runs &&
                        !out.lines && !out.recognized,
                    "flags 0x%x, %u runs, %u lines", (unsigned)out.hook_flags, runs, (unsigned)out.lines);
        TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_HOOK_ONLY && out.need_workspace == capture_block);
        TEST_CHECK(capture_allocs == 2 && !weights_live && !job_live && !capture_live); /* the try's, then the pass's */
        TEST_CHECK(!hk.sel.locked && !hk.sel.interval && !hk.sel.streak); /* nothing learned without OCR */
        unsigned passed = 0, workspace = 0;
        for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
            passed += out.step[i].id == VJO_GSTEP_CHECK && !out.step[i].rc;
            workspace += out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_WORKSPACE;
        }
        TEST_CHECK_(passed == 1 && !workspace, "%u passed checks, %u workspace ALLOC steps", passed, workspace);
        /* audit-b F1 and F2: an LM an earlier job left resident (verified, so cleanup() keeps it), which the check
         * counts as held, and a try that reads the region before its weights are refused (as a console's finder
         * does). The answer comes as above, and the LM goes back to the game first, its FREE step kept (the try did
         * not take it); the try's capture is the job's (one PASS step, its strips counted beside the pass's). */
        reset();
        free_mib = 1;
        fake_verified = 1u;
        stream(1, VJO_HOOK_KIND_SITE, 8, LINE2);
        spy_reads_rows = 1;
        {
            const VjoVocrLm *lm1 = vjo_vocr_lm_tier(1);
            TEST_ASSERT(lm1 != NULL);
            lm_res.data = lm_alloc(NULL, lm1->bytes); /* the earlier job's verified LM */
            TEST_ASSERT(lm_res.data != NULL && lm_live);
            lm_res.lm = lm1; lm_res.capacity = lm_res.size = lm1->bytes; lm_res.stamp = 1;
            available = 2 * 1024 * 1024 + (int64_t)lm_block; /* the check sees 2 MiB beside the LM */
        }
        VjoGameOcrRequest q = request(VJO_HOOK_PICK_AUTO);
        q.flags = 1u << VJO_GAME_OCR_LM_SHIFT; /* the LM's first tier allowed: the resident one */
        vjo_game_ocr_cache_clear();
        rows_calls = 0;
        TEST_ASSERT(vjo_game_ocr_run_request(&q, &out) == 0);
        TEST_CHECK_(!strcmp(out.text, LINE2) && out.hook_flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE),
                    "%s, flags 0x%x", out.text, (unsigned)out.hook_flags);
        TEST_CHECK_(!lm_live && !lm_res.lm && !weights_live && !job_live && !capture_live,
                    "lm %d weights %d job %d capture %d live after the answer", lm_live, weights_live, job_live,
                    capture_live);
        unsigned lm_frees = 0, passes = 0;
        for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
            lm_frees += out.step[i].id == VJO_GSTEP_FREE && out.step[i].arg == VJO_GITEM_LM;
            passes += out.step[i].id == VJO_GSTEP_PASS;
        }
        const unsigned strips = (H + vjo_vocr_strip_rows(W, H) - 1u) / vjo_vocr_strip_rows(W, H);
        TEST_CHECK_(lm_frees == 1 && passes == 1 && out.captures >= 2u * strips && out.capture_us > 0,
                    "%u LM FREE steps, %u PASS steps, %u captures (a pass is %u strips), %u us", lm_frees, passes,
                    (unsigned)out.captures, strips, (unsigned)out.capture_us);
        /* a CALL caller alone: the refusal, as before */
        reset();
        free_mib = 1;
        available = 2 * 1024 * 1024;
        stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && !out.text_from && !out.text[0] && !runs);
        /* hook_trust = on: the trusted site answers before any check, with no block */
        reset();
        free_mib = 1;
        available = 2 * 1024 * 1024;
        fake_verified = 1u;
        stream(1, VJO_HOOK_KIND_SITE, 7, LINE3);
        TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !strcmp(out.text, LINE3));
        TEST_CHECK(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR) && !capture_allocs);
    }
}

/* #355's audit N7: the same path with the pause (ocr_pause = on). The try took its frame block with its check, and its
 * weights were refused before its first pass (the core reads its weights first), so no frame was copied and the game
 * never waited; the verified site's answer then ends the job early (#500's path). Its pause step is still logged, before
 * the try's blocks go back (UNUSED: asked and held, never copied; us 1, no wait), as on every other path, and the frame
 * block goes back with them. Before, that path logged no pause step at all. */
static void verified_site_after_refused_weights_logs_the_pause(void)
{
    VjoGameOcrResult out;
    reset();
    free_mib = 1;
    available = 2 * 1024 * 1024;
    fake_verified = 1u;
    stream(1, VJO_HOOK_KIND_SITE, 5, LINE1);
    spy_text = OCR1;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_PAUSE, &out) == 0);
    TEST_CHECK_(!strcmp(out.text, LINE1) && out.text_from == 1 && !runs, "%s from %u, %u runs", out.text,
                (unsigned)out.text_from, runs);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    TEST_CHECK(!frame_copies && frame_block && !frame_live && !weights_live &&
               vjo_ledger_current(&g_game_ledger, GL_FRAME) == 0);
    unsigned pauses = 0;
    const VjoGameOcrStep *ps = NULL;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out.step[i].id == VJO_GSTEP_PAUSE) { pauses++; ps = &out.step[i]; }
    TEST_CHECK_(pauses == 1 && ps && !ps->kind && !ps->arg && !ps->rc && ps->bytes == frame_block && ps->us == 1 &&
                    ps->sum == (VJO_GPAUSE_UNUSED | VJO_GAME_OCR_MEM_USER << 8),
                "%u pause steps; kind %u rows %u rc %d bytes %u us %u sum 0x%X", pauses, ps ? ps->kind : 9u,
                ps ? ps->arg : 0u, ps ? (int)ps->rc : 0, ps ? ps->bytes : 0u, ps ? ps->us : 0u, ps ? ps->sum : 0u);
}

/* ---- API 43: trusted hooks (VJO_GAME_OCR_S_TRUST) through the worker's job path ---- */

/* A Rubi verified site answers from its first line without OCR: no model opened, no line found or recognized,
 * and (API 49) no pixel read: its capture is the trusted line's scene (VJO_ROWS_HOOK), with no block, in a game
 * that leaves room for the hook-only pass and none for OCR (1 MiB + 64 KiB: without the bit, 0.9.7's path refuses
 * OCR and answers unconfirmed only after its memory check), in one that leaves 3 MiB (あおかな's: under the 4 MiB
 * OCR reserve before PR #280, room for OCR beside its 1 MiB now) and in one with room. A kernel that refuses that
 * capture (API 51: only while the region has no screen yet; API 49 refused it whenever the region showed another
 * scene) gets API 43's first pass beside the hook-only reserve. */
static void trusted_verified_site_answers_without_ocr(void)
{
    VjoGameOcrResult out;
    const uint32_t trusted = VJO_HOOK_R_TRUSTED | VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR;
    static const int64_t free_bytes[] = {VJO_GAME_RESERVE_HOOK_ONLY + 64 * 1024, 3 * 1024 * 1024, 64 * 1024 * 1024};
    for (unsigned roomy = 0; roomy < 3; roomy++) { /* 0: no room for OCR; 1 and 2: OCR fits */
        reset();
        available = free_bytes[roomy];
        fake_verified = 1u << 1; /* stream 2 */
        stream(2, VJO_HOOK_KIND_SITE, 5, LINE1);
        stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
        for (unsigned line = 0; line < 3; line++) {
            if (line == 1) stream(2, VJO_HOOK_KIND_SITE, 7, LINE3);
            /* line 2: the same text again (the overlay reopened on a new scene): answered again */
            TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0);
            TEST_CHECK_(!strcmp(out.text, line ? LINE3 : LINE1), "%s", out.text);
            TEST_CHECK(out.text_from == 2 && out.hook_flags == trusted && !out.hook_score);
            TEST_CHECK(!rows_calls && hook_publishes == line + 1u && !capture_allocs);
            TEST_CHECK(!out.recognized && !out.lines && out.weights == VJO_GAME_OCR_WEIGHTS_NONE && !out.need_workspace);
            TEST_CHECK(!opens && !finds && !runs);
            TEST_CHECK(!hk.sel.interval && !hk.sel.streak); /* nothing learned without OCR */
        }
        /* the kernel refuses the capture without pixels: the first pass, beside the hook-only reserve */
        hook_publish_rc = VJO_ERR_FORMAT;
        TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 2);
        TEST_CHECK(first_pass_only() && hook_publishes == 4 && capture_allocs == 1 && !runs);
        TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_HOOK_ONLY && out.need_workspace == capture_block);
        hook_publish_rc = 0;
        /* without the bit (hook_trust = off): 0.9.7's selection, OCR first (refused in the tight game, where the
         * verified site then answers unconfirmed; with 3 MiB free it runs beside the 1 MiB reserve and agrees) */
        stream(2, VJO_HOOK_KIND_SITE, 8, LINE1);
        spy_text = OCR1;
        TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == 0 && out.text_from == 2);
        if (roomy) TEST_CHECK(runs == 1 && out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_SITE));
        else TEST_CHECK(!runs && out.hook_flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
        TEST_CHECK(out.reserve_bytes == (roomy ? VJO_GAME_RESERVE : VJO_GAME_RESERVE_HOOK_ONLY));
        TEST_MSG("%lld B free: %u runs, flags 0x%x, reserve %u", (long long)available, runs, (unsigned)out.hook_flags,
                 (unsigned)out.reserve_bytes);
    }
}

/* A Luna site in a game short of memory: 0.9.7's answer keeps the OCR reserve for its pass (1 MiB since PR #280,
 * 4 MiB before) and is refused without room for it beside its strip; a trusted answer needs no memory at all (API 49:
 * no pass; API 51: on another scene too) and shows the line, and when the kernel refuses its capture without pixels
 * (a region without a screen yet), its pass keeps the hook-only reserve (refused in a game without room even for
 * that). With 3 MiB free (あおかな's, under the 4 MiB reserve before PR #280), 0.9.7's answer shows the line too. */
static void trusted_luna_site_in_a_tight_game(void)
{
    VjoGameOcrResult out;
    reset();
    available = VJO_GAME_RESERVE + 4096; /* under the reserve and the pass's strip */
    stream(2, VJO_HOOK_KIND_SITE, 1, LINE2); fs[2].luna = &fake_luna_site;
    TEST_CHECK(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == VJO_E_OOM && !out.text[0] && !capture_allocs);
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE);
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !strcmp(out.text, LINE2));
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR) && !opens && !runs);
    TEST_CHECK(!capture_allocs && hook_publishes == 1 && !rows_calls);
    available = 0; /* no room at all: the line all the same */
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !strcmp(out.text, LINE2));
    hook_publish_rc = VJO_ERR_FORMAT;
    available = 3 * 1024 * 1024;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !strcmp(out.text, LINE2));
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_HOOK_ONLY && capture_allocs == 1);
    /* too little even for its one pass beside that reserve: refused, as any job */
    available = VJO_GAME_RESERVE_HOOK_ONLY + 4096;
    TEST_CHECK(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == VJO_E_OOM && !out.text_from);
    /* without the bit, in a game with 3 MiB free: its pass fits beside the 1 MiB reserve, and the line is shown
     * with no OCR (a Luna site is never compared, #274) */
    hook_publish_rc = 0;
    available = 3 * 1024 * 1024;
    const unsigned allocs = capture_allocs;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == 0 && !strcmp(out.text, LINE2));
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE && capture_allocs == allocs + 1u && !opens && !runs);
    TEST_CHECK(out.hook_flags & VJO_HOOK_R_NO_OCR);
}

/* The weights an earlier OCR job left resident (the title screen before the first hooked line) go back to the
 * game once trusted answers have run TRUST_DROP_ANSWERS (8) jobs in a row; an OCR job between them starts the run
 * again, so a site whose text is now and then unavailable does not make each OCR job read them again. A job that
 * finds no line after a trusted answer reads none ahead (no warm-up). */
static void trusted_answer_gives_back_the_weights(void)
{
    VjoGameOcrResult out;
    reset();
    spy_text = OCR1;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !out.text_from && runs == 1);
    TEST_CHECK(weights_live && out.resident_bytes > 0 && opens == 1);
    fake_verified = 1u << 1;
    for (unsigned i = 1; i <= TRUST_DROP_ANSWERS; i++) {
        if (i == 4) { /* the site's stream busy at the snapshot: OCR (the resident weights, no read), the run again */
            fs[2].busy = 1;
            TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !out.text_from);
            TEST_CHECK(weights_live && opens == 2 && hk.trusted_run == 0);
            fs[2].busy = 0;
        }
        stream(2, VJO_HOOK_KIND_SITE, i, LINE1);
        TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 2);
        TEST_CHECK_(weights_live && out.resident_bytes > 0, "answer %u: kept", i);
    }
    TEST_CHECK(hk.trusted_run == TRUST_DROP_ANSWERS - 3u);
    for (unsigned i = 0; i < 3; i++) {
        stream(2, VJO_HOOK_KIND_SITE, 20 + i, LINE2);
        TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 2);
        TEST_CHECK_(weights_live == (i < 2), "answer %u of the run's last three", i);
    }
    TEST_CHECK(!weights_live && !out.resident_bytes && runs == 2 && opens == 2);
    /* the site's stream busy again: OCR, which finds no line here; no warm-up reads the weights back */
    fs[2].busy = 1;
    spy_lines = 0;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !out.text_from && !out.lines);
    TEST_CHECK(!weights_live && opens == 2);
    /* without the bit the warm-up reads them as 0.9.7's did */
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == 0 && !out.lines);
    TEST_CHECK(weights_live && opens == 3);
    /* the give-back fails (the weights' block cannot be freed): the line all the same, and no retry until OCR runs */
    fs[2].busy = 0;
    for (unsigned i = 1; i <= TRUST_DROP_ANSWERS + 2u; i++) {
        stream(2, VJO_HOOK_KIND_SITE, 40 + i, i & 1u ? LINE1 : LINE2);
        if (i == TRUST_DROP_ANSWERS) fail_weights_free = 1;
        TEST_ASSERT_(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 2, "answer %u", i);
    }
    TEST_CHECK_(!fail_weights_free && hk.trusted_run == TRUST_DROP_ANSWERS + 1u, "injected %d, run %u",
                fail_weights_free, hk.trusted_run);
    if (weights_live) { weights_live = 0; live_bytes -= weights_block; } /* the fixture's own release */
    resident.data = NULL; resident.model = NULL; weights_uid = -1;
}

/* A CALL caller agrees with OCR on lines 1 and 2 (its lock), 10 and 26 (the checks at 8 and 16) and is then
 * trusted: 100 fresh lines and 5 jobs on the last one again (a screen change without a new line) take 4 OCR runs,
 * against 0.9.7's 11 (also lines 58 and 90, and each repeat). Its slot taken by another caller: OCR again. */
static void trusted_call_source_after_four_agreements(void)
{
    static char lines[100][96], ocr[100][96];
    VjoGameOcrResult out;
    unsigned runs_with[2];
    for (unsigned i = 0; i < 100; i++) {
        snprintf(ocr[i], sizeof(ocr[i]), "会話の新しいページ番号%uの台詞です", i + 1);
        snprintf(lines[i], sizeof(lines[i]), "「%s」", ocr[i]);
    }
    for (int trust = 0; trust < 2; trust++) {
        reset();
        for (unsigned i = 0; i < 105; i++) {
            if (i < 100) {
                stream(9, VJO_HOOK_KIND_CALL, i + 1, lines[i]);
                spy_text = ocr[i];
            }
            TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, trust ? VJO_GAME_OCR_S_TRUST : 0, &out) == 0);
            TEST_CHECK_(out.text_from == 9 && !strcmp(out.text, lines[i < 100 ? i : 99]), "line %u: %s", i + 1, out.text);
            if (trust && i >= 26)
                TEST_CHECK(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR) &&
                           !out.need_workspace && !rows_calls && !out.recognized);
        }
        runs_with[trust] = runs;
    }
    TEST_CHECK_(runs_with[1] == 4 && runs_with[0] == 11, "trust %u, 0.9.7 %u", runs_with[1], runs_with[0]);
    /* the source's slot now holds another caller's text (its key): not the source; OCR reads the screen */
    fs[9].key = 0x4444;
    stream(9, VJO_HOOK_KIND_CALL, 500, LINE1);
    spy_text = "全然関係のない画面の文字がここに表示されている";
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && runs == 5);
    TEST_CHECK(!out.text_from && !strcmp(out.text, spy_text) && !hk.sel.interval); /* unlocked */
}

/* #366's audit-b, F1: two CALL slots holding one caller (a race could give it a second, which keeps the text it was
 * taken with while the caller writes the other). Learned on stream 9 (with trust: trusted; without: locked), the
 * second slot appears on stream 14 with an old line, and the caller's next lines land in stream 9; then the other way
 * round, learned on 14, with the caller's lines landing in a second slot on 9. Every job shows the current line, from
 * the slot the caller writes (the newest), never the second slot's old one. Before, the job took the last of the
 * slots in ID order: with trust, the old line on every job, without OCR. Each case also runs with the streams' clock
 * near 2^32: the old line at 0xFFFFFFF9, the caller's next lines wrapping past 0 (#366's audit-c, N2: an unsigned
 * comparison took the old line from line 48 on). */
static uint32_t clock_after(uint32_t base, uint32_t n) /* the clock's n-th value after base (0 skipped, as its own) */
{
    uint32_t v = base + n;
    return base && v < base ? v + 1u : v;
}
static void second_slot_of_the_learned_caller(void)
{
    static char lines[60][96], ocr[60][96];
    static const char *const varied[10] = {
        "今日は朝から雨が降っていて外に出られない", "駅前の喫茶店で待ち合わせることにしようか",
        "それは本当に君が自分で決めたことなのか", "図書館の奥にある古い本棚を調べてみよう",
        "明日の試験のために今夜は早く寝ることにする", "窓の外で猫が静かにこちらを見つめていた",
        "先生の話を最後まで聞いてから判断しなさい", "海沿いの道を自転車で走るのが好きだった",
        "誰にも言えない秘密をひとつだけ持っている", "夕焼けの空が赤く染まって綺麗だったね"};
    VjoGameOcrResult out;
    for (unsigned i = 0; i < 60; i++) {
        if (i < 40) snprintf(ocr[i], sizeof(ocr[i]), "会話の新しいページ番号%uの台詞です", i + 1);
        else snprintf(ocr[i], sizeof(ocr[i]), "%s", varied[i % 10]); /* unlike each other and the old line */
        snprintf(lines[i], sizeof(lines[i]), "「%s」", ocr[i]);
    }
    for (unsigned k = 0; k < 8; k++) { /* every case: across the wrap or not, trusted or not, below or above */
        const int wrap = k >> 2, trust = k >> 1 & 1, below = k & 1;
        const uint32_t learned = below ? 14 : 9, second = below ? 9 : 14, writes = below ? second : learned;
        const uint32_t speed = trust ? VJO_GAME_OCR_S_TRUST : 0, base = wrap ? 0xFFFFFFD0u : 0u;
        TEST_CASE_("%s, the second slot %s%s", trust ? "trusted" : "hook_trust = off", below ? "below" : "above",
                   wrap ? ", the clock wrapping" : "");
        reset();
        fs[learned].key = 0x5910u;
        for (unsigned i = 0; i < 40; i++) {
            stream(learned, VJO_HOOK_KIND_CALL, clock_after(base, i + 1), lines[i]);
            spy_text = ocr[i];
            TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, speed, &out) == 0 && out.text_from == learned);
        }
        TEST_ASSERT(hk.sel.interval && hk.sel.locked == learned);
        if (trust) TEST_ASSERT(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
        fs[second].key = 0x5910u; /* the second slot: the caller's old line (above), or its next ones (below) */
        stream(second, VJO_HOOK_KIND_CALL, clock_after(base, 41), lines[40]);
        for (unsigned i = 41; i < 60; i++) {
            stream(writes, VJO_HOOK_KIND_CALL, clock_after(base, i + 1), lines[i]);
            spy_text = ocr[i];
            TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, speed, &out) == 0);
            TEST_CHECK_(out.text_from == writes && !strcmp(out.text, lines[i]),
                        "line %u: stream %u (%u), flags %#x: %s", i + 1, (unsigned)out.text_from, (unsigned)writes,
                        (unsigned)out.hook_flags, out.text);
            if (trust)
                TEST_CHECK(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
        }
        TEST_CHECK_(hk.sel.interval && hk.sel.locked == writes, "locked %u (%u), interval %u",
                    (unsigned)hk.sel.locked, (unsigned)writes, (unsigned)hk.sel.interval);
    }
}

/* The hook watch (API 41) watches a verified site once a trusted job read it, so its next line starts the read. */
static void hook_watch_reports_trusted_verified_lines(void)
{
    VjoGameOcrResult out;
    reset();
    fake_verified = 1u << 2; /* stream 3 */
    stream(3, VJO_HOOK_KIND_SITE, 4, LINE1);
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == 0); /* without the bit: not watched */
    stream(3, VJO_HOOK_KIND_SITE, 5, LINE2);
    hooks_watch();
    TEST_CHECK(!hook_lines);
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 3);
    stream(3, VJO_HOOK_KIND_SITE, 6, LINE3);
    hooks_watch();
    TEST_CHECK(hook_lines == 1 && hook_line_seq == 6);
}

/* ---- API 49: no wait for a trusted line (docs/native-hooks.md) ---- */

static int runs_beyond_title(void) { return runs > 1u; } /* OCR beyond the title screen's one job */

/* With trust on (the last user job's VJO_GAME_OCR_S_TRUST), a trusted site's line is reported at once and flagged
 * (VJO_HOOK_LINE_TRUSTED): from its first line, which no job has read; the next at once (no gap); the same text
 * again, from another watched stream, is no new line, nor is a write its decoder leaves empty; a stream being
 * written, or a job in the way, is read at the next round. The watch's pace (vjoOcrTake's wait) is HOOK_WATCH_US
 * while a trusted source is watched. An unverified Rubi site is never watched. hook_trust = off: API 41's watch. */
static void trusted_watch_reports_at_once(void)
{
    VjoGameOcrResult out;
    reset();
    fake_verified = 1u << 2; /* stream 3 */
    TEST_CHECK(hooks_watch() == 0 && !hook_lines);
    spy_text = OCR1;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !out.text_from); /* a title screen */
    TEST_CHECK(hk.trust_on && hooks_watch() == HOOK_WATCH_US && !hook_lines); /* a table site installed: its pace */
    /* the site's first line */
    stream(3, VJO_HOOK_KIND_SITE, 4, LINE1);
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US);
    TEST_CHECK(hook_lines == 1 && hook_line_seq == 4 && hook_line_bytes == ((uint32_t)strlen(LINE1) | VJO_HOOK_LINE_TRUSTED));
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 1); /* no news */
    /* the next line at once: no gap */
    stream(3, VJO_HOOK_KIND_SITE, 5, LINE2);
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 2 && hook_line_seq == 5);
    /* a Luna site carrying the same line: read, but no new line */
    stream(1, VJO_HOOK_KIND_SITE, 6, LINE2); fs[1].luna = &fake_luna_site;
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 2 && hk.watch_seen == 6);
    stream(1, VJO_HOOK_KIND_SITE, 7, LINE3); /* its own next line is one */
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 3 && hook_line_seq == 7);
    /* a write the decoder leaves empty (a filtered name): none */
    stream(3, VJO_HOOK_KIND_SITE, 8, "名前"); fs[3].filtered_empty = 1;
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 3 && hk.watch_seen == 8);
    fs[3].filtered_empty = 0;
    /* a stream a game thread is writing: the next round */
    stream(3, VJO_HOOK_KIND_SITE, 9, LINE1); fs[3].busy = 1;
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 3 && hk.watch_seen == 8);
    fs[3].busy = 0;
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 4 && hook_line_seq == 9);
    /* a job in the way (BUSY): the next round */
    stream(3, VJO_HOOK_KIND_SITE, 10, LINE2); hook_line_rc = VJO_ERR_BUSY;
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 5 && hk.watch_seen == 9);
    hook_line_rc = 0;
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 6 && hook_line_seq == 10);
    /* an unverified Rubi site (no Luna script): never trusted, not watched */
    stream(4, VJO_HOOK_KIND_SITE, 11, LINE3);
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 6);
    /* the job the Shell starts for the line: the trusted answer, its capture without pixels */
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 3);
    TEST_CHECK(!strcmp(out.text, LINE2) && hook_publishes == 1 && !rows_calls && !runs_beyond_title());
    /* hook_trust = off: a job without the bit, then API 41's watch: unflagged, paced, the 100 ms take */
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == 0 && !hk.trust_on);
    stream(1, VJO_HOOK_KIND_SITE, 12, LINE1);
    clock_value += HOOK_LINE_GAP_US;
    TEST_CHECK(hooks_watch() == 0 && hook_lines == 7 && hook_line_bytes == (uint32_t)strlen(LINE1));
    stream(1, VJO_HOOK_KIND_SITE, 13, LINE2);
    TEST_CHECK(hooks_watch() == 0 && hook_lines == 7); /* within the gap */
}

/* The learned source: reported flagged once vjo_hook_select_trusted would answer from it (four agreements, the last
 * a strong one), unflagged before; one restored as trusted (saved at the trusted interval, API 49) is watched by its
 * identity before a job has found its slot, and its first job answers without OCR. */
static void trusted_watch_learned_source(void)
{
    VjoGameOcrResult out;
    reset();
    spy_text = OCR1;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && hk.trust_on && hk.installed);
    hk.sel = (VjoHookSelect){0};
    hk.sel.locked = 9; hk.sel.locked_key = 0x9999; hk.sel.locked_encoding = VJO_HOOK_ENC_UTF8; hk.sel.interval = 16;
    hk.installed = 4; fake_installed = 4; /* string copies only, no table site */
    stream(9, VJO_HOOK_KIND_CALL, 5, LINE1); fs[9].key = 0x9999;
    TEST_CHECK(hooks_watch() == 0 && hook_lines == 1 && hook_line_bytes == (uint32_t)strlen(LINE1)); /* not trusted */
    hk.sel.interval = VJO_HOOK_TRUST_INTERVAL; hk.sel.strong = 1;
    stream(9, VJO_HOOK_KIND_CALL, 6, LINE2);
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 2);
    TEST_CHECK(hook_line_bytes == ((uint32_t)strlen(LINE2) | VJO_HOOK_LINE_TRUSTED));
    hk.sel.misses = 1; /* a pending disagreement: no longer trusted */
    stream(9, VJO_HOOK_KIND_CALL, 7, LINE3);
    clock_value += HOOK_LINE_GAP_US;
    TEST_CHECK(hooks_watch() == 0 && hook_lines == 3 && hook_line_bytes == (uint32_t)strlen(LINE3));
    /* restored as trusted from the cache (the next start of the game): no slot yet (locked 0), found by its key */
    reset(); prime_hook_cache_trusted(VJO_HOOK_TRUST_INTERVAL, 1);
    fake_installed = 4; /* string copies only: the pace is the trusted source's */
    spy_text = OCR1;
    stream(10, VJO_HOOK_KIND_CALL, 1, "セーブしますか？"); fs[10].key = 0x5555; /* the title screen's menu */
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !out.text_from && runs == 1);
    TEST_CHECK(cache_loads == 1 && hk.sel.strong && hk.sel.interval == VJO_HOOK_TRUST_INTERVAL && !hk.sel.locked);
    stream(11, VJO_HOOK_KIND_CALL, 8, LINE1); fs[11].key = 0x1234;
    stream(10, VJO_HOOK_KIND_CALL, 9, LINE3); /* another caller: no line of it */
    TEST_CHECK(hooks_watch() == HOOK_WATCH_US && hook_lines == 1 && hook_line_seq == 8);
    TEST_CHECK(hook_line_bytes == ((uint32_t)strlen(LINE1) | VJO_HOOK_LINE_TRUSTED));
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && out.text_from == 11);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR) && runs == 1);
    TEST_CHECK(hk.sel.locked == 11 && hook_publishes == 1 && !rows_calls);
}

/* API 50: the line finder's fixes follow the request's speed bits (VJO_GAME_OCR_S_LINE_FIX: VOCR_FIX_DEFAULT;
 * _JUNK: VOCR_FIX_JUNK; _VALLEY: VOCR_FIX_VALLEY); without them the finder is the frozen one (fix 0). */
/* An SC3 site's charset (hooks_sc3_load): read at the first job of a game with hooks, before its snapshot, into a
 * block of its own beside the hook-only reserve, through the kernel's SC3 open; the engine gets it and the block
 * stays (its ALLOC step once, no FREE) until the worker stops. A refused file, a missing one or a short one: the
 * block goes back and nothing is tried again; a timed-out open is tried again at the next jobs, three tries in all;
 * no memory: nothing is tried until a job has it. The job's own text and model error are as without it. Nothing is
 * read with no SC3 site, nor in a game without hooks. */
static void sc3_charset_loads_once(void)
{
    static const VjoSc3Pin pin = {"PCSG00146", 5364u, {0}};
    VjoGameOcrResult out;
    unsigned allocs = 0, frees = 0;
    /* read, kept, and the job's trusted line answers with it */
    reset();
    sc3_want = &pin;
    sc3_open_rc = (int)pin.bytes;
    fake_verified = 1u;
    stream(1, VJO_HOOK_KIND_SITE, 5, LINE1);
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0 && !strcmp(out.text, LINE1));
    TEST_CHECK(sc3_opens == 1 && sc3_sets == 1 && sc3_set_file == sc3_mem && sc3_set_bytes == pin.bytes);
    TEST_CHECK(sc3_live && sc3_block == 8192u && sc3_uid == 12 && !out.io_error);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_SC3) == 8192u);
    for (unsigned i = 0; i < out.n_steps; i++) {
        allocs += out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_SC3;
        frees += out.step[i].id == VJO_GSTEP_FREE && out.step[i].arg == VJO_GITEM_SC3;
    }
    TEST_CHECK(allocs == 1 && !frees && blocks_kept() == 1);
    /* the next jobs read nothing again */
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0);
    TEST_CHECK(sc3_opens == 1 && sc3_sets == 1 && sc3_live);
    /* #382/#387's audit N6: a stop whose free of the charset fails says so and keeps it recorded; the next frees it */
    sc3_free_fail = 1;
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && sc3_live && sc3_uid == 12 && !sc3_set_file);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_SC3) == 8192u);
    /* the worker's stop gives it back, after the engine forgets it (each stop: so 3 sets in all) */
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK && !sc3_live && sc3_uid < 0 && sc3_sets == 3 && !sc3_set_file);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_SC3) == 0);
    /* refused by the engine (not the pinned file), missing (an error), short: the block goes back, no retry */
    for (unsigned k = 0; k < 3; k++) {
        reset();
        sc3_want = &pin;
        sc3_accept = k != 0;
        sc3_open_rc = k == 0 ? (int)pin.bytes : k == 1 ? (int)0x80010002u : (int)pin.bytes - 1;
        spy_text = OCR1;
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1) && !out.io_error);
        TEST_CHECK_(sc3_opens == 1 && sc3_sets == (k == 0) && !sc3_live && sc3_uid < 0, "case %u", k);
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && sc3_opens == 1);
    }
    /* a timed-out open: tried again at the next job, three tries in all */
    reset();
    sc3_want = &pin;
    sc3_open_rc = VJO_ERR_TIMEOUT;
    spy_text = OCR1;
    for (unsigned k = 1; k <= 4; k++) {
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !out.io_error);
        TEST_CHECK_(sc3_opens == (k < 3 ? k : 3) && !sc3_live, "job %u", k);
    }
    reset();
    sc3_want = &pin;
    sc3_open_rc = VJO_ERR_TIMEOUT;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && sc3_opens == 1);
    sc3_open_rc = (int)pin.bytes;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && sc3_opens == 2 && sc3_live && sc3_set_file == sc3_mem);
    /* no room beside the hook-only reserve: no block, no open, until a job has it */
    reset();
    sc3_want = &pin;
    sc3_open_rc = (int)pin.bytes;
    available = VJO_GAME_RESERVE_HOOK_ONLY + 4096;
    stream(1, VJO_HOOK_KIND_SITE, 5, LINE1);
    fake_verified = 1u;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0);
    TEST_CHECK(!sc3_opens && !sc3_live && !sc3_tries);
    available = 64 * 1024 * 1024;
    TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, VJO_GAME_OCR_S_TRUST, &out) == 0);
    TEST_CHECK(sc3_opens == 1 && sc3_live);
    /* no SC3 site, or no hooks at all: nothing */
    reset();
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !sc3_opens && !sc3_live);
    reset();
    sc3_want = &pin;
    fake_installed = 0; fake_refused = 0; fake_mode = VJO_TEXT_SOURCE_OCR;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !sc3_opens && !sc3_live);
}

/* #368's audit F1: a charset read that timed out (the kernel's file worker stalled for the read's MODEL_WAIT_US) is
 * tried again at a later job, as an open that timed out is, three tries in all; a read that failed otherwise is not.
 * Meanwhile OCR reads the game, and the job's I/O error is its own (none). */
static void sc3_timed_out_read_is_tried_again(void)
{
    static const VjoSc3Pin pin = {"PCSG00146", 5364u, {0}};
    VjoGameOcrResult out;
    reset();
    sc3_want = &pin;
    sc3_open_rc = (int)pin.bytes;
    sc3_read_busy = 1;
    spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1));
    TEST_CHECK_(sc3_opens == 1 && !sc3_live && sc3_uid < 0 && !sc3_set_file && !out.io_error,
                "the timed-out read: %u opens, block %d, io_error 0x%08X", sc3_opens, sc3_live, (unsigned)out.io_error);
    sc3_read_busy = 0;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(sc3_opens == 2 && sc3_live && sc3_set_file == sc3_mem,
                "a timed-out read is tried again at the next job (opens %u, charset set %d)", sc3_opens,
                sc3_set_file != NULL);
    /* reads that keep timing out: three tries in all */
    reset();
    sc3_want = &pin;
    sc3_open_rc = (int)pin.bytes;
    sc3_read_busy = 1;
    spy_text = OCR1;
    for (unsigned k = 1; k <= 4; k++) {
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1) && !out.io_error);
        TEST_CHECK_(sc3_opens == (k < 3 ? k : 3) && !sc3_live, "job %u: %u opens", k, sc3_opens);
    }
    /* a read that failed otherwise (the kernel's error, not a timeout): once a session */
    reset();
    sc3_want = &pin;
    sc3_open_rc = (int)pin.bytes;
    sc3_read_error = -456;
    spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !out.io_error);
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(sc3_opens == 1 && !sc3_live, "a failed read: %u opens", sc3_opens);
}

/* #368's audit F2: the charset is read, but its close does not finish within MODEL_WAIT_US (the kernel's file worker
 * stalled) and finishes later, at the job's cleanup. Until then the broker is the charset's, so the job's own model
 * cannot open: the job fails, and its I/O error says why, the close's timeout. Before, it said 0, which the Shell
 * takes for a missing or damaged model file (shell/worker.c file_bad): it left that model size out for the rest of
 * the game's activation. With a remembered hook (the hook cache), the cache is not asked for while the broker is held:
 * the kernel refuses it then (kernel/game_ocr.c hook_cache_locked: VJO_ERR_ARG, another file's operation), and it
 * would not be asked again this session (hk.cache_attempted); the fake's invariant (no cache call while a file is
 * open) stands for that refusal. The next job reads the cache, and the game as before. */
static void sc3_failed_close_is_the_job_s_error(void)
{
    static const VjoSc3Pin pin = {"PCSG00146", 5364u, {0}};
    VjoGameOcrResult out;
    for (int cached = 0; cached <= 1; cached++) {
        reset();
        if (cached) prime_hook_cache(32);
        sc3_want = &pin;
        sc3_open_rc = (int)pin.bytes;
        sc3_close_busy = 2500; /* about 2,000 polls in the load's close, the rest in the job's cleanup */
        spy_text = OCR1;
        int rc = run(VJO_HOOK_PICK_AUTO, &out);
        TEST_CHECK_(rc == VJO_E_OCR_MODEL && out.io_error == VJO_ERR_TIMEOUT && !opens,
                    "%s: a job that failed because the charset's close timed out reports it (rc %d, io_error 0x%08X)",
                    cached ? "remembered hook" : "no cache", rc, (unsigned)out.io_error);
        TEST_CHECK(sc3_set_file == sc3_mem && sc3_live && sc3_opens == 1); /* the charset itself was read */
        TEST_CHECK_(!cache_loads && !hk.cache_attempted, "%u cache loads while the broker was held", cache_loads);
        TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1) && !out.io_error);
        TEST_CHECK_(opens == 1 && cache_loads == (unsigned)cached && sc3_opens == 1,
                    "the next job: %u model opens, %u cache loads", opens, cache_loads);
    }
}

/* #368's audit N1: a charset block that could not be given back (its base unreadable and its free refused, or a
 * retried try's free refused) stays recorded, and the next try frees it before it takes another: a block is never
 * taken over a live one, whose uid would be lost (never freed, not even at the stop). While that free fails, nothing
 * is taken and no try is counted. */
static void sc3_block_not_given_back_is_freed_before_another(void)
{
    static const VjoSc3Pin pin = {"PCSG00146", 5364u, {0}};
    VjoGameOcrResult out;
    /* the block's base cannot be read, and its free fails once */
    reset();
    sc3_want = &pin;
    sc3_open_rc = (int)pin.bytes;
    sc3_base_fail = 1;
    sc3_free_fail = 1;
    spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !strcmp(out.text, OCR1));
    TEST_CHECK(sc3_uid == 12 && sc3_live && sc3_allocs == 1 && !sc3_opens);
    sc3_base_fail = 0;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(!sc3_double && sc3_allocs == 2 && sc3_opens == 1 && sc3_live && sc3_set_file == sc3_mem,
                "no second block taken while the first is still held (%u taken, %u over a live one; %u opens)",
                sc3_allocs, sc3_double, sc3_opens);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_SC3) == 8192u);
    /* a timed-out open whose block's free fails twice: the next job takes nothing while it fails, then reads */
    reset();
    sc3_want = &pin;
    sc3_open_rc = VJO_ERR_TIMEOUT;
    sc3_free_fail = 2;
    spy_text = OCR1;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK(sc3_uid == 12 && sc3_live && sc3_opens == 1 && sc3_tries == 1);
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(sc3_allocs == 1 && sc3_opens == 1 && sc3_live && sc3_tries == 1, "free refused: %u blocks, %u opens",
                sc3_allocs, sc3_opens);
    sc3_open_rc = (int)pin.bytes;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0);
    TEST_CHECK_(!sc3_double && sc3_allocs == 2 && sc3_opens == 2 && sc3_set_file == sc3_mem,
                "then: %u blocks, %u over a live one, %u opens", sc3_allocs, sc3_double, sc3_opens);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_SC3) == 8192u);
}

static void line_fix_follows_the_request(void)
{
    static const struct { uint32_t speed; unsigned fix; } cases[] = {
        {0u, 0u}, {VJO_GAME_OCR_S_LINE_FIX, VOCR_FIX_DEFAULT},
        {VJO_GAME_OCR_S_LINE_FIX | VJO_GAME_OCR_S_LINE_JUNK, VOCR_FIX_DEFAULT | VOCR_FIX_JUNK},
        {VJO_GAME_OCR_S_LINE_FIX | VJO_GAME_OCR_S_LINE_JUNK | VJO_GAME_OCR_S_LINE_VALLEY,
         VOCR_FIX_DEFAULT | VOCR_FIX_JUNK | VOCR_FIX_VALLEY},
        {VJO_GAME_OCR_S_LINE_VALLEY, VOCR_FIX_VALLEY}};
    VjoGameOcrResult out;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        VjoGameOcrRequest r = request(VJO_HOOK_PICK_OCR);
        r.speed = cases[i].speed;
        spy_line_fix = ~0u;
        vjo_game_ocr_cache_clear();
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == 0 && finds == 1);
        TEST_CHECK_(spy_line_fix == cases[i].fix, "speed 0x%08X: fix %u", (unsigned)cases[i].speed, spy_line_fix);
    }
}

/* #413's audit F2 in the worker (#491's audit F1): a job whose request has no settle time (a press since #413) may
 * read a line still typing out, and its text agreeing with the start of the locked caller's line only is no evidence
 * (no miss; the check stays due, and the whole line then counts); a request with the wait (the background read's:
 * 150 ms within 2 s, on a settled screen or, SETTLE_FIRST, an unsettled one; or the wait alone, its limit the
 * default) compares it as before: one miss. So the worker's no-wait test (game_ocr/main.c, vjo_hook_select_after's
 * unsettled) is pinned: wired to 0, to 1 or to another field of the request, a case fails. */
static int run_flags(uint32_t flags, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(VJO_HOOK_PICK_AUTO);
    r.flags = flags;
    vjo_game_ocr_cache_clear();
    rows_calls = 0;
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out->cleanup_status);
    return rc;
}
static void a_press_reads_a_line_typing_out(void)
{
    static const uint32_t wait = 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    /* a press; the background read's on a settled screen, and on an unsettled one; the wait with the default limit */
    static const uint32_t flags[4] = {0, wait, wait | VJO_GAME_OCR_F_SETTLE_FIRST, 15u << VJO_GAME_OCR_SETTLE_SHIFT};
    VjoGameOcrResult out;
    for (unsigned q = 0; q < 4; q++) {
        reset();
        stream(9, VJO_HOOK_KIND_CALL, 5, LINE1); spy_text = OCR1;
        TEST_ASSERT(run_flags(wait, &out) == 0 && out.text_from == 9);
        stream(9, VJO_HOOK_KIND_CALL, 6, LINE2); spy_text = OCR2;
        TEST_ASSERT(run_flags(wait, &out) == 0 && hk.sel.locked == 9 && hk.sel.interval == 8);
        hk.sel.unchecked = hk.sel.interval - 1u; /* the next new line is the scheduled check */
        stream(9, VJO_HOOK_KIND_CALL, 7, LINE3); spy_text = "だったら今夜の"; /* its start, 7 of 23 */
        TEST_ASSERT(run_flags(flags[q], &out) == 0);
        TEST_CHECK_(!out.text_from && !strcmp(out.text, spy_text) && !(out.hook_flags & VJO_HOOK_R_UNLOCK),
                    "request 0x%08x: text_from %u, flags 0x%x", (unsigned)flags[q], out.text_from, out.hook_flags);
        if (!q) {
            TEST_CHECK_(!hk.sel.misses && !hk.sel.confirm && hk.sel.typing, "press: misses %u", hk.sel.misses);
            spy_text = OCR3; /* the same line, typed out: it agrees, and the check counts */
            TEST_ASSERT(run_flags(0, &out) == 0);
            TEST_CHECK(out.text_from == 9 && !hk.sel.misses && hk.sel.interval == 16 && !hk.sel.typing);
        } else {
            TEST_CHECK_(hk.sel.misses == 1 && hk.sel.confirm && !hk.sel.typing, "request 0x%08x: misses %u",
                        (unsigned)flags[q], hk.sel.misses);
        }
    }
}

TEST_LIST = {
    {"a_press_reads_a_line_typing_out", a_press_reads_a_line_typing_out},
    {"an SC3 site's charset: read once, kept until the stop; refused, missing, short or no memory: given back",
     sc3_charset_loads_once},
    {"#368's audit: a charset read that timed out is tried again, as an open is; another failed read is not",
     sc3_timed_out_read_is_tried_again},
    {"#368's audit: a charset close that failed is the job's error; the hook cache waits for the broker",
     sc3_failed_close_is_the_job_s_error},
    {"#368's audit: a charset block not given back is freed before another is taken, never lost",
     sc3_block_not_given_back_is_freed_before_another},
    {"API 43: a trusted verified site answers without OCR, also short of memory", trusted_verified_site_answers_without_ocr},
    {"API 43: a trusted Luna site keeps the hook-only reserve", trusted_luna_site_in_a_tight_game},
    {"API 43: a trusted answer gives the weights back; no warm-up after it", trusted_answer_gives_back_the_weights},
    {"API 43: a CALL source is trusted after four agreements; OCR resumes when it is gone", trusted_call_source_after_four_agreements},
    {"#366's audit-b: of two slots holding the learned caller, the one it writes (trusted or not, either order)",
     second_slot_of_the_learned_caller},
    {"API 43: the hook watch reports a trusted verified site's lines", hook_watch_reports_trusted_verified_lines},
    {"API 49: a trusted line is reported at once, from its first, once per text; the watch's pace", trusted_watch_reports_at_once},
    {"API 49: the learned source is reported flagged once trusted, restored ones by identity", trusted_watch_learned_source},
    {"API 50: the line finder's fixes follow the request", line_fix_follows_the_request},
    {"OCR refused for memory: a verified site answers unconfirmed", verified_site_answers_when_ocr_has_no_memory},
    {"#280's audit F1: a verified site answers when the weights are refused after the check (アマツツミ's 2 MiB, "
     "whole MiB)", verified_site_answers_when_its_weights_are_refused},
    {"API 55, #355's audit N7: that path with the pause logs the pause step", verified_site_after_refused_weights_logs_the_pause},
    {"Luna: no initial, periodic or repeated OCR and no generic cache IO", luna_never_loads_ocr_or_generic_learning},
    {"Luna: preserve generic learning and resume it when unavailable", luna_preserves_generic_state_and_falls_back_when_unavailable},
    {"Luna: pool saturation, newest script, stable ties and sequence wrap", luna_survives_pool_saturation_and_uses_newest_script},
    {"Luna: manual picks, OCR-only and self-test overrides", luna_respects_explicit_picks_and_self_tests},
    {"Luna: failed capture preserves generic state", luna_capture_failure_preserves_state},
    {"the border rule on a stream that replaces OCR's text (API 36)", border_lines_on_an_agreeing_stream},
    {"the border rule follows the request (API 36), on OCR's and a stream's text", border_lines_follow_the_request},
    {"hook cache cleanup: BUSY, copy failure, cancellation, failed primary free", hook_cache_cleanup_ownership},
    {"remembered hook: startup confirmation, exact32 cadence, recovery", remembered_hook_worker_cadence},
    {"remembered hook: its trust saved apart from its interval (hook_trust on, then off, then on)", remembered_trust_is_its_own_bit},
    {"hook cache worker: failed job, exact build invalidation, IO failures", hook_cache_worker_failures},
    {"nothing installed: OCR as before, no snapshot", nothing_installed_is_ocr_as_before},
    {"an agreeing stream's text replaces OCR's", agreeing_stream_replaces_ocr_text},
    {"a locked stream with new text answers with the first pass only", locked_stream_answers_with_the_first_pass_only},
    {"the hook watch reports a learned source's new line once (API 41)", hook_watch_reports_a_new_line},
    {"the hook watch reports a Luna site's new lines (API 41)", hook_watch_reports_luna_lines},
    {"a locked CALL stream whose slot another caller took unlocks (VjoHookCand.key)", replaced_slot_unlocks},
    {"a job that fails changes nothing", failed_answer_changes_nothing},
    {"a pinned stream, OCR only, an unknown pick", pinned_and_ocr_only},
    {"the self-test's jobs are OCR's", self_test_jobs_are_ocr},
    {"a shot and the probe take no snapshot", shot_and_probe_take_no_snapshot},
    {"the shown text is the stream's whole text", shown_text_is_whole},
    {"a full pool keeps the pick's stream and the newest", full_pool_keeps_the_pick_and_the_newest},
    {"an agreeing or pinned stream keeps its CALL slot; CALLS_FULL", keep_and_calls_full},
    {NULL, NULL}
};
