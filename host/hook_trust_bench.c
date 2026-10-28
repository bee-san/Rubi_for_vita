/* The trusted-hook bench (docs/native-hooks.md, "Trusted hooks"): a game session replayed through the actual game
 * worker (game_ocr/main.c), core adapter (core/vocr_ocr.c) and vita-vn-ocr runtime with the pinned models, one job
 * per page, as the Shell's background read of each new hooked line (API 41). A fake of game_ocr/hooks.h holds one
 * stream, which takes each page's hook text in turn; the kernel's rows are the page's region, grey; the game's free
 * memory is a figure the run sets, less the worker's own blocks. tools/hook_trust_bench.py writes the pages and
 * reads the jobs.
 *
 *   vjo-hook-trust-bench MODEL_DIR KIND TRUST FREE_MIB [press] < pages.bin > jobs.jsonl
 *     KIND   luna (a ported Luna site) | verified (a Rubi verified site) | call (a string copy's caller) |
 *            lag (a caller one page behind the screen: a source that must never be trusted)
 *     TRUST  1: VJO_GAME_OCR_S_TRUST in every job (hook_trust = on); 0: Rubi 0.9.7's selection
 *     press  every job a press's (#413: no stable-text wait, as in Only when I press L + R; #413's audit F2), not the
 *            background read's
 *     pages.bin: "VJTB" then per page: u32 width, u32 height, u32 text bytes, the hook's UTF-8 text, width x height
 *     RGB bytes (made grey here as the kernel does, (77 R + 150 G + 29 B) >> 8).
 *
 * One JSON line per job: whether it ran the OCR path, what it recognized, the worker's blocks at their peak and after
 * it, its reserve, its host time, its capture strips (a game frame each), its CTC frames, and how close the text it
 * showed is to the page's hook text (core/hook_select.h's similarity). Host execution: no console timing, battery or
 * CPU3 figure comes from it (docs/offline-testing.md). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <psp2/host_stubs.h>
#define VJO_TEST_HOOK_CACHE 1 /* this file's vjoOcrHookCache: no cache, every run learns afresh */
#define VJO_TEST_HOOK_LINE 1
#include "../game_ocr/main.c"

/* ---- the session ---- */
static const char *model_dir;
static int kind_luna, kind_verified, kind_lag;
static const unsigned char *grey;   /* the page's region */
static uint32_t width, height;
static const char *hook_text;       /* the stream's text now (lag: the page before's) */
static uint32_t hook_seq;
static uint64_t sim_us;             /* the game's clock: host time plus the frames the rows calls waited */

static uint64_t host_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ---- the fake hook engine (game_ocr/hooks.h): one stream, ID 2 (a site) or 9 (a CALL caller) ---- */
static const VjoLunaSite fake_luna_site = {0};
static uint32_t stream_id(void) { return kind_luna || kind_verified ? 2u : 9u; }
void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    *mode = VJO_TEXT_SOURCE_AUTO;
    *installed = kind_luna || kind_verified ? 1u << 16 | 8u : 8u;
    *refused = 0;
}
int vjo_hooks_verified(uint32_t id) { return kind_verified && id == 2u; }
void vjo_hooks_keep(uint32_t id, uint32_t key) { (void)id; (void)key; }
int vjo_hooks_psp_poll(void) { return 0; } /* #318's PSP text poll: no PSP emulator here (psp_title 0) */
void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced) { *dropped = *replaced = 0; }
int vjo_hooks_build_id(uint8_t out[32]) { memset(out, 0xa7, 32); return 1; }
int vjo_hooks_snap(uint32_t id, VjoHookSnap *out)
{
    if (id != stream_id() || !hook_text || !hook_text[0]) return 0;
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->kind = id <= VJO_HOOK_SITE_STREAMS ? VJO_HOOK_KIND_SITE : VJO_HOOK_KIND_CALL;
    out->seq = hook_seq;
    out->key = 0x5910Fu;
    out->hits = 1;
    out->encoding = VJO_HOOK_ENC_UTF8;
    out->site = kind_luna ? &fake_luna_site : NULL;
    out->n = (uint32_t)strlen(hook_text) < VJO_HOOK_RAW_BYTES ? (uint32_t)strlen(hook_text) : VJO_HOOK_RAW_BYTES;
    memcpy(out->raw, &hook_text, sizeof(hook_text)); /* the fake's raw bytes: the text's address */
    return 1;
}
uint32_t vjo_hooks_seq(uint32_t id, uint32_t *key, uint32_t *kind, uint32_t *bytes)
{
    if (id != stream_id() || !hook_text) return 0;
    *key = 0x5910Fu;
    *kind = id <= VJO_HOOK_SITE_STREAMS ? VJO_HOOK_KIND_SITE : VJO_HOOK_KIND_CALL;
    *bytes = (uint32_t)strlen(hook_text);
    return hook_seq;
}
size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    const char *t;
    size_t n;
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
int vjoOcrHookCache(uint32_t seq, uint32_t operation, void *data) { (void)seq; (void)operation; (void)data; return -1; }
int vjoOcrHookLine(uint32_t seq, uint32_t bytes) { (void)seq; (void)bytes; return -1; }

/* ---- the game's memory: real blocks, each named, the peak of all of them ---- */
#define BLOCKS 16
static struct { void *base; uint32_t size; } block[BLOCKS];
static int64_t game_free;            /* the game's free USER bytes without the worker's blocks */
static uint64_t live_bytes, peak_bytes;
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return sim_us + host_us(); }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    int64_t f = game_free - (int64_t)live_bytes;
    out->size_user = f > INT32_MAX ? INT32_MAX : (int)f;
    out->size_cdram = out->size_phycont = 0;
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    (void)name; (void)opts;
    if (type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) return (int)0x80024302u; /* USER only here */
    if ((int64_t)size > game_free - (int64_t)live_bytes) return (int)0x80024302u;
    for (int i = 1; i < BLOCKS; i++) {
        if (block[i].base) continue;
        if (posix_memalign(&block[i].base, 64, size)) return (int)0x80024302u;
        block[i].size = size;
        live_bytes += size;
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return 100 + i;
    }
    return (int)0x80024302u;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    int i = uid - 100;
    if (i < 1 || i >= BLOCKS || !block[i].base) return -1;
    *base = block[i].base;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    int i = uid - 100;
    if (i < 1 || i >= BLOCKS || !block[i].base) return -1;
    free(block[i].base);
    block[i].base = NULL;
    live_bytes -= block[i].size;
    return 0;
}

/* ---- threads: a helper's line runs at its start (test_game_vocr.c's model) ---- */
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu,
                             void *opt)
{
    static SceUID next = 40;
    (void)n; (void)entry; (void)p; (void)stack; (void)a; (void)cpu; (void)opt;
    return ++next;
}
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return uid > 40 ? helper_entry(size, args), 0 : 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { (void)uid; (void)status; (void)timeout; return 0; }
int sceKernelDeleteThread(SceUID uid) { (void)uid; return 0; }
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; info->runClocks = host_us(); return 0; }
SceUID sceKernelGetThreadId(void) { return 20; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelDelayThread(unsigned us) { sim_us += us; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { (void)pid; snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { (void)pid; (void)param; (void)out; (void)cap; return -1; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { (void)path; (void)flags; (void)mode; return (int)0x80010013u; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return 0; }
int vjoOcrTake(VjoGameOcrRequest *out) { (void)out; return 1; }
int vjoOcrCancelled(uint32_t seq) { (void)seq; return 0; }
int vjoOcrComplete(const VjoGameOcrResult *out) { (void)out; return 0; }
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src) { (void)seq; (void)off; (void)bytes; (void)src; return -1; }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { (void)seq; (void)bytes; return -1; }

/* ---- the kernel's rows: the page's region, one game frame (16.7 ms) per call ---- */
static unsigned rows_calls;
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    if (req->flags & VJO_ROWS_HOOK) return 0; /* API 49: a trusted answer's capture, published without pixels */
    if (req->y >= height || !req->n || req->n > height - req->y) return VJO_ERR_ARG;
    rows_calls++;
    sim_us += 16667u;
    for (unsigned r = 0; r < req->n; r++)
        memcpy((unsigned char *)dst + (size_t)r * req->dst_stride, grey + (size_t)(req->y + r) * width, width);
    info->fb_width = width; info->fb_height = height; info->fb_pitch = width; info->fb_format = 0;
    info->wait_us = 16667u;
    info->armed = 1;
    return (int)req->n;
}

/* ---- the kernel's model broker: the pinned files in MODEL_DIR, by the request's `which` ---- */
static FILE *model;
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    char path[1024];
    const char *name = NULL;
    (void)seq;
    if (model) return VJO_ERR_BUSY;
    if (which == VJO_GAME_OCR_OPEN_GATE) name = VJO_VOCR_GATE_NAME;
    else if (which >= VJO_GAME_OCR_OPEN_TIER && which < VJO_GAME_OCR_OPEN_TIER + VJO_VOCR_TIERS)
        name = tier_names[which - VJO_GAME_OCR_OPEN_TIER];
    else if (which >= VJO_GAME_OCR_OPEN_LM && which < VJO_GAME_OCR_OPEN_LM + VJO_VOCR_LM_TIERS)
        name = vjo_vocr_lm_tier(which - VJO_GAME_OCR_OPEN_LM + 1u)->name;
    if (!name) return -123;
    snprintf(path, sizeof(path), "%s/%s", model_dir, name);
    if (!(model = fopen(path, "rb"))) return -123;
    fseek(model, 0, SEEK_END);
    int size = (int)ftell(model);
    if (info) *info = (VjoGameOcrModelInfo){sizeof(*info), (uint32_t)size, 0, 0, 0x1234567};
    return size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t n, void *out)
{
    (void)seq;
    if (!model || fseek(model, (long)offset, SEEK_SET)) return -456;
    return (int)fread(out, 1, n, model);
}
int vjoOcrModelClose(uint32_t seq) { (void)seq; if (model) fclose(model); model = NULL; return 0; }

/* ---- the run ---- */
static int read_u32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }

static void json_text(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char **argv)
{
    static VjoGameOcrResult out;
    char magic[4];
    int trust, press = argc == 6 && !strcmp(argv[5], "press");
    char *prev = NULL;
    if (argc != 5 && !press) {
        fprintf(stderr, "usage: vjo-hook-trust-bench MODEL_DIR luna|verified|call|lag 0|1 FREE_MIB [press] < pages.bin\n");
        return 2;
    }
    model_dir = argv[1];
    kind_luna = !strcmp(argv[2], "luna");
    kind_verified = !strcmp(argv[2], "verified");
    kind_lag = !strcmp(argv[2], "lag");
    if (!kind_luna && !kind_verified && !kind_lag && strcmp(argv[2], "call")) return 2;
    trust = atoi(argv[3]);
    game_free = (int64_t)atoi(argv[4]) * 1024 * 1024;
    if (fread(magic, 1, 4, stdin) != 4 || memcmp(magic, "VJTB", 4)) return 2;
    ledger_init();
    worker = workspace_uid = weights_uid = capture_uid = line_uid = lm_uid = cascade_uid = -1;
    for (unsigned page = 0;; page++) {
        uint32_t w, h, tn;
        char *text;
        unsigned char *pixels;
        if (!read_u32(stdin, &w)) break;
        if (!read_u32(stdin, &h) || !read_u32(stdin, &tn) || !w || !h || w > VJO_OCR_MAX_WIDTH ||
            h > VJO_OCR_MAX_HEIGHT || tn > 65536) return 2;
        text = malloc(tn + 1u);
        pixels = malloc((size_t)w * h);
        unsigned char *rgb = malloc((size_t)w * h * 3u);
        if (!text || !pixels || !rgb || fread(text, 1, tn, stdin) != tn ||
            fread(rgb, 1, (size_t)w * h * 3u, stdin) != (size_t)w * h * 3u)
            return 2;
        for (size_t i = 0; i < (size_t)w * h; i++) /* the kernel's grey (core/fbgrey.h) */
            pixels[i] = (unsigned char)((77u * rgb[3 * i] + 150u * rgb[3 * i + 1] + 29u * rgb[3 * i + 2]) >> 8);
        free(rgb);
        text[tn] = '\0';
        /* the new line reaches the stream (the lag source: the line before it) */
        hook_text = kind_lag ? prev : text;
        hook_seq = page + 1u;
        grey = pixels; width = w; height = h;
        /* the Shell's background job for it (shell/worker.c vocr_run_one, the defaults): Normal with Small below it,
         * the 1 MB LM, the cascade within 2 s, PSP-only refinement, the 150 ms wait (the screen settled; press: none,
         * a press's since #413), two helpers, 444 MHz, the border rule, the head gate and the two-level head */
        VjoGameOcrRequest r = {.size = sizeof(r), .seq = page + 12u, .capture_seq = page + 11u, .fb_width = w,
                               .fb_height = h, .width = w, .height = h, .tiers = 3u};
        snprintf(r.model, sizeof(r.model), "%s", tier_names[VJO_VOCR_TIER_NORMAL - 1u]);
        snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
        r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT |
                  VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT |
                  (press ? 0u : 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT);
        r.speed = 2u | VJO_GAME_OCR_S_BOOST | VJO_GAME_OCR_S_DROP_BORDERS | VJO_GAME_OCR_S_HEAD_GATE |
                  VJO_GAME_OCR_S_TWO_LEVEL | (trust ? VJO_GAME_OCR_S_TRUST : 0u); /* ocr_head_gate = on: both */
        r.hook_pick = VJO_HOOK_PICK_AUTO;
        rows_calls = 0;
        peak_bytes = live_bytes;
        uint64_t t0 = host_us();
        int rc = vjo_game_ocr_run_request(&r, &out);
        uint64_t us = host_us() - t0;
        unsigned ocr = !(out.hook_flags & VJO_HOOK_R_NO_OCR) && rc != VJO_E_OOM;
        size_t shown = text_len(out.text, sizeof(out.text));
        unsigned sim = vjo_hook_similarity(text, tn, out.text, shown);
        unsigned comparable = vjo_hook_similarity(text, tn, text, tn) != 0; /* 4 characters or more */
        printf("{\"page\": %u, \"rc\": %d, \"from\": %u, \"flags\": %u, \"ocr\": %u, \"lines\": %u, \"recognized\": %u, "
               "\"frames\": %u, \"strips\": %u, \"weights\": %u, \"peak\": %llu, \"after\": %llu, \"resident\": %u, "
               "\"reserve\": %u, \"need\": %u, \"us\": %llu, \"find_us\": %u, \"recognize_us\": %u, \"load_us\": %u, "
               "\"score\": %u, \"sim\": %u, \"cmp\": %u, \"shown\": ",
               page, rc, out.text_from, out.hook_flags, ocr, out.lines, out.recognized, out.frames, out.captures,
               out.weights, (unsigned long long)peak_bytes, (unsigned long long)live_bytes, out.resident_bytes,
               out.reserve_bytes, out.need_workspace + out.need_weights, (unsigned long long)us, out.find_us,
               out.recognize_us, out.load_us, out.hook_score, sim, comparable);
        json_text(out.text);
        printf("}\n");
        free(prev);
        prev = text;
        free(pixels);
        grey = NULL;
    }
    free(prev);
    drop_resident();
    return 0;
}
