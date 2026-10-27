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
#include "../../game_ocr/main.c"

#define W 200u
#define H 150u /* three strips of the first pass: 64 + 64 + 22 rows */
static unsigned char job_memory[1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_MAX_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char capture_mem_block[64*1024] __attribute__((aligned(64)));
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
/* PSP text's poll (game_ocr/hooks.h): psp_polls counts the calls; at a poll, psp_installs (nonzero) becomes
 * what vjo_hooks_counts reports installed, as the real poll's profile coming does (it returns 1 then). */
static unsigned psp_polls;
static uint32_t psp_installs;
int vjo_hooks_psp_poll(void)
{
    psp_polls++;
    if (fake_installed == psp_installs) return 0;
    fake_installed = psp_installs;
    return 1;
}
uint32_t vjo_hooks_psp_status(void)
{
    return 0;
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
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out) { out->size_user = (int)(available - live_bytes); return 0; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    (void)opts;
    TEST_CHECK(!(size & 4095));
    if (!strcmp(name, "VjoGameShot")) { shot_allocs++; return -1; }
    if (!strcmp(name, "VjoGameVocrProbe")) { probe_allocs++; return -1; }
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
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
    TEST_CHECK(!strcmp(name, "VjoGameVocr") && !job_live && size <= sizeof(job_memory));
    job_live = 1; job_block = size; live_bytes += size;
    return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem_block; return 0; }
    TEST_CHECK(uid == 10 ? weights_live : uid == 9 && job_live);
    *base = uid == 10 ? (void *)weights_memory : (void *)job_memory;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
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
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    TEST_CHECK(seq == 12 && !file_open_now && which == VJO_GAME_OCR_OPEN_MODEL);
    opens++;
    file_open_now = 1;
    *info = (VjoGameOcrModelInfo){sizeof(*info), TEST_FL10_BYTES, 0, 0, 0x12345678};
    return (int)TEST_FL10_BYTES;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && file_open_now && n && n <= VJO_GAME_OCR_MODEL_CHUNK);
    for (uint32_t i = 0; i < n; i++) ((unsigned char *)dst)[i] = test_weight_byte(off + i, TEST_FL10_SEED);
    return (int)n;
}
int vjoOcrModelClose(uint32_t seq)
{
    TEST_CHECK(seq == 12);
    if (cache_pending) { cache_closes++; cache_pending = 0; }
    file_open_now = 0;
    return 0;
}

/* ---- the runtime: spies ---- */
static unsigned spy_lines = 1; /* the lines the spy finder finds (0: a region without text) */
int vocr_find_lines_rows(const VocrLinesRows *src, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    finds++;
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
    available = 64*1024*1024; clock_value = 0;
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

static void prime_hook_cache(uint32_t interval)
{
    VjoHookLearned learned = {0, 0x1234u, VJO_HOOK_ENC_UTF8, interval};
    VjoHookCacheKey key = {0};
    memcpy(key.title, "PCSG00415", 10);
    memcpy(hk.cache_key.title, key.title, sizeof(key.title));
    memcpy(key.build, fake_build, sizeof(key.build));
    TEST_ASSERT(hook_revision(key.definition));
    key.mode = fake_mode;
    TEST_ASSERT(vjo_hook_cache_put(saved_cache, 0, &key, &learned) == 1);
    saved_cache_size = sizeof(saved_cache);
}

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
    /* Restart: one bad OCR result retains the source, saved once as no longer trusted (API 49: the trusted interval
     * is saved only for a strong source, so a restart now confirms it); the next fresh agreement resets to 8 and
     * saves once more. */
    restart_hook_selection();
    stream(15, VJO_HOOK_KIND_CALL, 34, LINE1); spy_text = "ここには全く違った文章が表示されています";
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !out.text_from);
    TEST_CHECK(hk.sel.interval == 32 && hk.sel.misses == 1 && !hk.sel.strong && cache_saves == 1);
    TEST_CHECK(hk.cached.interval == 16 && hk.cached.key == 0x1234u);
    stream(15, VJO_HOOK_KIND_CALL, 35, LINE2); spy_text = OCR2;
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 15);
    TEST_CHECK(hk.sel.interval == 8 && !hk.sel.misses && cache_saves == 2);
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
    TEST_CHECK(cache_saves == 1); /* API 49: saved as no longer trusted */
    stream(9, VJO_HOOK_KIND_CALL, 3, LINE3); spy_text = OCR3; fail_capture_free = 1;
    r = request(VJO_HOOK_PICK_AUTO); vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(out.cleanup_status && blocked && cache_saves == 1 && hk.sel.interval == 32 && hk.sel.misses == 1);
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

/* A game that leaves too little for any model (アマツツミ: 2 MiB free on a console): OCR is refused, and a
 * Rubi verified site's newest text answers unconfirmed, with one first pass beside the hook-only reserve and
 * no recognition; nothing is learned. A Luna site or CALL caller, an OCR-only pick, or too little even for
 * the pass: the refusal as before. With room for OCR nothing of this runs. */
static void verified_site_answers_when_ocr_has_no_memory(void)
{
    VjoGameOcrResult out;
    for (unsigned borrowed_filter = 0; borrowed_filter < 2; borrowed_filter++) {
        reset();
        available = 2 * 1024 * 1024;
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

        /* too little even for the pass beside the smaller reserve: refused, no text */
        stream(1, VJO_HOOK_KIND_SITE, 6, LINE3);
        available = VJO_GAME_RESERVE_HOOK_ONLY + 4096;
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == VJO_E_OOM && !out.text_from && !out.text[0]);
        /* an OCR-only pick: the refusal */
        available = 2 * 1024 * 1024;
        TEST_CHECK(run(VJO_HOOK_PICK_OCR, &out) == VJO_E_OOM && !out.text_from);
        /* room for OCR: recognition as before, never the unconfirmed answer */
        available = 64 * 1024 * 1024;
        TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && runs >= 1 && !(out.hook_flags & VJO_HOOK_R_UNCONFIRMED));
    }
}

/* ---- API 43: trusted hooks (VJO_GAME_OCR_S_TRUST) through the worker's job path ---- */

/* A Rubi verified site answers from its first line without OCR: no model opened, no line found or recognized,
 * and (API 49) no pixel read: its capture is the trusted line's scene (VJO_ROWS_HOOK), with no block, in a game
 * that leaves 3 MiB (under the OCR reserve: without the bit, 0.9.7's path refuses OCR and answers unconfirmed only
 * after its memory check) and in one with room. A kernel that refuses that capture (the region shows another
 * screen) gets API 43's first pass beside the hook-only reserve. */
static void trusted_verified_site_answers_without_ocr(void)
{
    VjoGameOcrResult out;
    const uint32_t trusted = VJO_HOOK_R_TRUSTED | VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR;
    for (unsigned roomy = 0; roomy < 2; roomy++) {
        reset();
        available = roomy ? 64 * 1024 * 1024 : 3 * 1024 * 1024;
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
         * verified site then answers unconfirmed) */
        stream(2, VJO_HOOK_KIND_SITE, 8, LINE1);
        spy_text = OCR1;
        TEST_ASSERT(run_speed(VJO_HOOK_PICK_AUTO, 0, &out) == 0 && out.text_from == 2);
        if (roomy) TEST_CHECK(runs == 1 && out.hook_flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_SITE));
        else TEST_CHECK(!runs && out.hook_flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    }
}

/* A Luna site in a game short of memory: 0.9.7's answer keeps the 4 MiB OCR reserve for its pass and is refused;
 * a trusted answer needs no memory at all (API 49: no pass) and shows the line, and when the kernel refuses its
 * capture without pixels, its pass keeps the hook-only reserve (refused in a game without room even for that). */
static void trusted_luna_site_in_a_tight_game(void)
{
    VjoGameOcrResult out;
    reset();
    available = 3 * 1024 * 1024;
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
    reset(); prime_hook_cache(VJO_HOOK_TRUST_INTERVAL);
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

/* PSP text (core/psp_text.h, game_ocr/hooks.c vjo_hooks_psp_poll): in the PSP emulator's process each OCR job
 * polls before its snapshot, and a profile that came at that poll is the job's own (what vjo_hooks_counts reports
 * is read again). Its stream 1 is a regular hook's: OCR must agree, twice to lock, then it answers with the first
 * pass only; no learned-source cache in that process. A native game, a shot and the self-test's probe never poll. */
static void psp_text_polls_first_and_must_agree(void)
{
    VjoGameOcrResult out;
    reset();
    prime_hook_cache(32); /* a saved record the PSP process must never load */
    fake_installed = 0; fake_refused = 0; fake_mode = VJO_TEXT_SOURCE_HOOKS;
    psp_polls = 0; psp_installs = 1u << 16;
    fs[1].key = 0x08898920u;
    stream(1, VJO_HOOK_KIND_SITE, 1, LINE1);
    spy_text = OCR1;
    /* a native Vita game: no poll, nothing installed, OCR */
    psp_title = 0;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && !psp_polls && !snaps && !out.text_from && !out.hook_installed);
    /* the PSP emulator: the poll first, then the stream as this job's candidate, shown once OCR agrees */
    psp_title = 1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && psp_polls == 1 && out.hook_installed == 1u << 16);
    TEST_CHECK_(out.text_from == 1 && (out.hook_flags & VJO_HOOK_R_AGREED) && (out.hook_flags & VJO_HOOK_R_SITE) &&
                !(out.hook_flags & VJO_HOOK_R_NO_OCR) && runs == 2, "from %u flags 0x%x runs %u", out.text_from,
                out.hook_flags, runs);
    TEST_CHECK(!strcmp(out.text, LINE1));
    TEST_CHECK(!cache_loads && !build_reads); /* no learned source in the PSP emulator */
    /* a second agreeing line locks; the third is answered without recognition */
    stream(1, VJO_HOOK_KIND_SITE, 2, LINE2); spy_text = OCR2;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 1 && (out.hook_flags & VJO_HOOK_R_LOCKED));
    stream(1, VJO_HOOK_KIND_SITE, 3, LINE3); spy_text = "読まれないはずの文字";
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 1 && (out.hook_flags & VJO_HOOK_R_NO_OCR));
    TEST_CHECK(!strcmp(out.text, LINE3) && runs == 3 && first_pass_only());
    TEST_CHECK(psp_polls == 3 && !cache_loads && !cache_saves);
    /* the game leaves (Adrenaline's XMB): the poll says so, the job is OCR's with no snapshot */
    psp_installs = 0; snaps = 0; spy_text = OCR1;
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && psp_polls == 4 && !out.hook_installed && !snaps &&
               !out.text_from && !strcmp(out.text, OCR1));
    /* the game again (a profile comes back): nothing learned carries over; OCR must agree again */
    psp_installs = 1u << 16;
    stream(1, VJO_HOOK_KIND_SITE, 5, LINE3); spy_text = "全然関係のない画面の文字がここに表示されている";
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && psp_polls == 5 && out.hook_installed == 1u << 16);
    TEST_CHECK(!out.text_from && runs == 5 && !hk.sel.interval && !hk.sel.locked);
    /* a shot and the self-test's probe job never poll */
    {
        VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=W, .fb_height=H, .width=W,
                               .height=H, .flags=VJO_GAME_OCR_F_SHOT};
        snprintf(r.shot, sizeof(r.shot), "123-456");
        vjo_game_ocr_run_request(&r, &out);
#if VJO_DEBUG
        r = request(VJO_HOOK_PICK_AUTO);
        r.flags = VJO_GAME_OCR_F_PROBE;
        vjo_game_ocr_run_request(&r, &out);
#endif
        TEST_CHECK(psp_polls == 5);
    }
    psp_title = 0; psp_installs = 0;
}

/* API 46 (overlay_on_text, VJO_GAME_OCR_S_CHARS): a stream that answers before recognition (here a Luna site) is
 * still the job's text, but the job recognizes too, for the characters' places: OCR's text follows it for the log,
 * and its flags lose VJO_HOOK_R_NO_OCR. A locked regular stream's answer, the same. With no memory for OCR, the
 * stream that answered shows without places, as without the bit (its flags with VJO_HOOK_R_NO_OCR again). */
static void chars_recognize_under_a_hook_answer(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    reset();
    stream(2, VJO_HOOK_KIND_SITE, 1, "はい"); fs[2].luna = &fake_luna_site;
    spy_text = OCR1;
    r = request(VJO_HOOK_PICK_AUTO);
    r.speed = VJO_GAME_OCR_S_CHARS;
    vjo_game_ocr_cache_clear();
    rows_calls = 0;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK_(!strcmp(out.text, "はい") && out.text_from == 2, "%s from %u", out.text, (unsigned)out.text_from);
    TEST_CHECK_(out.hook_flags == VJO_HOOK_R_SITE, "flags %08X", (unsigned)out.hook_flags);
    TEST_CHECK(runs == 1 && out.recognized == 1 && !first_pass_only());
    TEST_CHECK(!strcmp(vjo_text_raw(out.text, sizeof(out.text)), OCR1)); /* OCR's text after it, for the log */
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out.cleanup_status);
    /* the same job without the bit: the first pass only, as before */
    TEST_ASSERT(run(VJO_HOOK_PICK_AUTO, &out) == 0 && runs == 1 && first_pass_only());
    TEST_CHECK(out.text_from == 2 && out.hook_flags == (VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR));
    /* no memory for OCR: the stream that answered, without places */
    available = 2 * 1024 * 1024;
    vjo_game_ocr_cache_clear();
    rows_calls = 0;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!strcmp(out.text, "はい") && out.text_from == 2 && runs == 1);
    TEST_CHECK_(out.hook_flags == (VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR), "flags %08X", (unsigned)out.hook_flags);
    TEST_CHECK(first_pass_only() && out.reserve_bytes == VJO_GAME_RESERVE_HOOK_ONLY);
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out.cleanup_status);
}

TEST_LIST = {
    {"API 43: a trusted verified site answers without OCR, also short of memory", trusted_verified_site_answers_without_ocr},
    {"API 43: a trusted Luna site keeps the hook-only reserve", trusted_luna_site_in_a_tight_game},
    {"API 43: a trusted answer gives the weights back; no warm-up after it", trusted_answer_gives_back_the_weights},
    {"API 43: a CALL source is trusted after four agreements; OCR resumes when it is gone", trusted_call_source_after_four_agreements},
    {"API 43: the hook watch reports a trusted verified site's lines", hook_watch_reports_trusted_verified_lines},
    {"API 49: a trusted line is reported at once, from its first, once per text; the watch's pace", trusted_watch_reports_at_once},
    {"API 49: the learned source is reported flagged once trusted, restored ones by identity", trusted_watch_learned_source},
    {"API 50: the line finder's fixes follow the request", line_fix_follows_the_request},
    {"PSP text: the job polls first; its stream must agree with OCR; no cache there", psp_text_polls_first_and_must_agree},
    {"OCR refused for memory: a verified site answers unconfirmed", verified_site_answers_when_ocr_has_no_memory},
    {"Luna: no initial, periodic or repeated OCR and no generic cache IO", luna_never_loads_ocr_or_generic_learning},
    {"Luna: preserve generic learning and resume it when unavailable", luna_preserves_generic_state_and_falls_back_when_unavailable},
    {"Luna: pool saturation, newest script, stable ties and sequence wrap", luna_survives_pool_saturation_and_uses_newest_script},
    {"Luna: manual picks, OCR-only and self-test overrides", luna_respects_explicit_picks_and_self_tests},
    {"Luna: failed capture preserves generic state", luna_capture_failure_preserves_state},
    {"the border rule on a stream that replaces OCR's text (API 36)", border_lines_on_an_agreeing_stream},
    {"the border rule follows the request (API 36), on OCR's and a stream's text", border_lines_follow_the_request},
    {"hook cache cleanup: BUSY, copy failure, cancellation, failed primary free", hook_cache_cleanup_ownership},
    {"remembered hook: startup confirmation, exact32 cadence, recovery", remembered_hook_worker_cadence},
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
    {"overlay_on_text: a hook's answer is still recognized for its places (API 46)", chars_recognize_under_a_hook_answer},
    {NULL, NULL}
};
