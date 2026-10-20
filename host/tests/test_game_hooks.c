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

/* ---- the fake hook engine (game_ocr/hooks.h) ---- */
typedef struct {
    uint32_t kind, seq;
    const char *text; /* UTF-8, NULL: no text */
    int busy;         /* a game thread is writing it: the snapshot fails */
} FakeStream;
static FakeStream fs[VJO_HOOK_STREAMS + 1u];
static uint32_t fake_mode, fake_installed, fake_refused;
static unsigned snaps, texts;
/* At the snap_at-th snapshot, stream bump_id gets bump_text (its seq moves on), as a game thread would. */
static unsigned bump_at;
static uint32_t bump_id;
static const char *bump_text;

void vjo_hooks_start(const char *title_id, unsigned mode, int psp) { (void)title_id; (void)mode; (void)psp; }
int vjo_hooks_stop(void) { return 0; }
void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    *mode = fake_mode;
    *installed = fake_installed;
    *refused = fake_refused;
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
    out->hits = 1;
    out->encoding = VJO_HOOK_ENC_UTF8;
    out->n = (uint32_t)strlen(fs[id].text) < VJO_HOOK_RAW_BYTES ? (uint32_t)strlen(fs[id].text) : VJO_HOOK_RAW_BYTES;
    memcpy(out->raw, &fs[id].text, sizeof(fs[id].text)); /* the fake's raw bytes: the text's address */
    return 1;
}
size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    const char *t;
    size_t n;
    texts++;
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
    if (uid == 11) { TEST_CHECK(capture_live); capture_live = 0; live_bytes -= capture_block; return 0; }
    if (uid == 10) { TEST_CHECK(weights_live); weights_live = 0; live_bytes -= weights_block; return 0; }
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
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
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
int vjoOcrModelClose(uint32_t seq) { TEST_CHECK(seq == 12); file_open_now = 0; return 0; }

/* ---- the runtime: spies ---- */
int vocr_find_lines_rows(const VocrLinesRows *src, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    finds++;
    memset(out, 0, sizeof(*out));
    out->count = 1;
    out->lines[0] = (VocrLine){10, 2, 180, 3, 0};
    return VOCR_LINES_OK;
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
    fake_mode = VJO_TEXT_SOURCE_AUTO; fake_installed = 1u << 16 | 4u; fake_refused = 2;
    snaps = texts = bump_at = 0; bump_id = 0; bump_text = NULL;
    capture_live = job_live = weights_live = 0; capture_block = capture_allocs = 0; live_bytes = 0;
    shot_allocs = probe_allocs = 0;
    available = 64*1024*1024; clock_value = 0;
    file_open_now = cancel_now = rows_error = 0;
    opens = finds = runs = 0; rows_calls = 0;
    spy_text = "猫";
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

static void failed_answer_changes_nothing(void)
{
    VjoGameOcrResult out;
    reset();
    hk.sel = (VjoHookSelect){9, 5, 9, 2, 0};
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
    hk.sel = (VjoHookSelect){9, 5, 9, 2, 0};
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
    }
    fs[15].seq = 1;
    hk.sel = (VjoHookSelect){15, 1, 15, 2, 0};
    fs[15].seq = 2; /* new text: it answers */
    TEST_CHECK(run(VJO_HOOK_PICK_AUTO, &out) == 0 && out.text_from == 15);
    TEST_CHECK(out.hook_flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(out.hook_streams == (1u << VJO_HOOK_STREAMS) - 1u); /* every stream holds text */
    TEST_CHECK(hk.n == 6 && hk.cand[0].id == 15);
    for (unsigned i = 1; i < hk.n; i++) TEST_CHECK(hk.cand[i].id == VJO_HOOK_STREAMS + 1u - i); /* the newest */
    TEST_CHECK(hk.cand[5].n < 399 && hk.cand[4].n == 399);
}

TEST_LIST = {
    {"nothing installed: OCR as before, no snapshot", nothing_installed_is_ocr_as_before},
    {"an agreeing stream's text replaces OCR's", agreeing_stream_replaces_ocr_text},
    {"a locked stream with new text answers with the first pass only", locked_stream_answers_with_the_first_pass_only},
    {"a job that fails changes nothing", failed_answer_changes_nothing},
    {"a pinned stream, OCR only, an unknown pick", pinned_and_ocr_only},
    {"the self-test's jobs are OCR's", self_test_jobs_are_ocr},
    {"a shot and the probe take no snapshot", shot_and_probe_take_no_snapshot},
    {"the shown text is the stream's whole text", shown_text_is_whole},
    {"a full pool keeps the pick's stream and the newest", full_pool_keeps_the_pick_and_the_newest},
    {NULL, NULL}
};
