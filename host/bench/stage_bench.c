/* Every stage of an OCR job and of the Shell's lookups, on the real sources
 * (stage_bench.h, docs/performance.md). Each stage runs its setup once, one
 * untimed warm-up iteration, then `iters` timed iterations of identical work
 * (scene_update_typing's flip a different cell each time: within 6%), so the
 * Cortex-A9 lane can difference two runs with different counts and keep
 * exactly the per-iteration instructions. Only string.h functions are called
 * here: on that lane this file is built by the Vita compiler and linked with
 * the plugins' own libraries. */
#include <string.h>

#include "stage_bench.h"

#include <bearssl_hash.h>

#include "anki.h"
#include "anki_queue.h"
#include "arena.h"
#include "client.h"
#include "config.h"
#include "fbgrey.h"
#include "furigana.h"
#include "history_lines.h"
#include "jpegsw.h"
#include "local_dict.h"
#include "port.h"
#include "textfilter.h"
#include "utf.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "vocr_lines_fix.h"
#include "vocr_ocr.h"
#include "../../kernel/scene.h"

#define MAX_W 960u
#define MAX_H 544u
#define ROWS_MAX 64u
#define TEXTS_MAX 128u
#define LOOKUP_ARENA (64u * 1024u) /* the Shell's local result arena (shell/worker.c) */

/* Host instrumentation buffers (a game frame, its grey plane, the job's
 * memory); none of this is production memory. */
static uint32_t fb32[MAX_W * MAX_H];
static uint16_t fb16[MAX_W * MAX_H];
static unsigned char grey[MAX_W * MAX_H];
static unsigned char strip[ROWS_MAX * MAX_W];
static unsigned char workspace[2u << 20] __attribute__((aligned(64)));
static unsigned char capture_block[64u * 1024u] __attribute__((aligned(64)));
static unsigned char line_block[VJO_VOCR_LINE_MAX_BYTES] __attribute__((aligned(64)));
static unsigned char weights[2u << 20] __attribute__((aligned(64)));
static unsigned char finder_arena[1u << 20] __attribute__((aligned(16)));
static unsigned char lookup_mem[LOOKUP_ARENA] __attribute__((aligned(16)));
static unsigned char big_mem[4u << 20] __attribute__((aligned(16)));
static unsigned char scratch_mem[256u * 1024u] __attribute__((aligned(16)));
static char texts_mem[64u * 1024u];

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    if (!h) h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * UINT64_C(1099511628211);
    return h;
}

static uint64_t fnv64s(uint64_t h, const char *s)
{
    return fnv64(h, s, strlen(s) + 1);
}

static uint64_t now_ns(const BenchIn *in)
{
    return in->clock_ns ? in->clock_ns() : 0;
}

/* ---- inputs ---- */

static int region_ok(const BenchIn *in)
{
    return in->rgba && in->fb_w && in->fb_h && in->fb_w <= MAX_W && in->fb_h <= MAX_H && in->w && in->h &&
           in->x + in->w <= in->fb_w && in->y + in->h <= in->fb_h;
}

/* The frame in SceDisplay format fmt (the inverse of the kernel's expansion,
 * as test_capture_parity.c builds its frames). */
static const void *frame_in(const BenchIn *in, uint32_t fmt)
{
    size_t n = (size_t)in->fb_w * in->fb_h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = in->rgba + 4 * i;
        uint32_t r = p[0], g = p[1], b = p[2];
        if (fmt == VJO_FB_A8B8G8R8)
            fb32[i] = 0xFF000000u | b << 16 | g << 8 | r;
        else if (fmt == VJO_FB_A2B10G10R10)
            fb32[i] = 0xC0000000u | (b << 2 | b >> 6) << 20 | (g << 2 | g >> 6) << 10 | (r << 2 | r >> 6);
        else
            fb16[i] = (uint16_t)(0x8000u | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
    }
    return fmt == VJO_FB_A1B5G5R5 ? (const void *)fb16 : (const void *)fb32;
}

/* The region's grey plane, as the hook's rows give it (A8B8G8R8 frame). */
static void grey_plane(const BenchIn *in)
{
    for (uint32_t y = 0; y < in->h; y++) {
        const uint8_t *p = in->rgba + 4 * ((size_t)(in->y + y) * in->fb_w + in->x);
        for (uint32_t x = 0; x < in->w; x++, p += 4)
            grey[(size_t)y * in->w + x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
    }
}

/* in->text: records separated by '\x1e' (a record may hold '\n'). */
static unsigned split_texts(const BenchIn *in, const char **out, unsigned cap)
{
    size_t len = in->text ? strlen(in->text) : 0;
    unsigned n = 0;
    if (!len || len >= sizeof(texts_mem)) return 0;
    memcpy(texts_mem, in->text, len + 1);
    for (char *s = texts_mem; *s && n < cap;) {
        char *e = strchr(s, '\x1e');
        if (e) *e = 0;
        if (*s) out[n++] = s;
        if (!e) break;
        s = e + 1;
    }
    return n;
}

/* ---- capture: the display hook's grey rows (kernel/capture.c) ---- */

static unsigned strip_rows(const BenchIn *in)
{
    unsigned rows = in->strip_rows ? in->strip_rows : vjo_vocr_strip_rows(in->w, in->h);
    return rows > ROWS_MAX ? ROWS_MAX : rows ? rows : 1u;
}

/* One pass over the region, a rows call (one display frame) per strip.
 * mode bit 0: the first pass, which also builds the capture's signature. */
/* mode 1: the first pass (the signature too); mode 2 (API 23): an Anki
 * screenshot's colour rows, 4 bytes a pixel, 64 rows a strip. */
static int stage_capture(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static unsigned char colour[64 * 960 * 4];
    const void *fb;
    int rgba = (in->mode & 3u) == 2u, sig = !rgba && (in->mode & 1u);
    unsigned rows = rgba ? 64u : strip_rows(in), px = rgba ? 4u : 1u;
    unsigned char *dst = rgba ? colour : strip;
    uint32_t bpp = vjo_fb_bpp(in->fmt);
    uint64_t t0 = 0;
    if (!region_ok(in) || !bpp || (rgba && in->w > 960)) return -1;
    fb = frame_in(in, in->fmt);
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        if (sig) bench_capture_sig_begin(in->w, in->h);
        for (uint32_t y = 0; y < in->h; y += rows) {
            uint32_t n = in->h - y < rows ? in->h - y : rows;
            int rc = bench_grey_rows((uintptr_t)fb, in->fb_w, in->fmt, in->fb_w, in->fb_h, in->x, in->y, in->w,
                                     in->h, y, n, dst, in->w * px, rgba ? 2 : sig);
            if (rc != (int)n) return -2;
            if (it == 0) out->check = fnv64(out->check, dst, (size_t)n * in->w * px);
        }
    }
    out->ns = now_ns(in) - t0;
    if (sig) out->check = fnv64(out->check, &(uint32_t){bench_capture_sig_hash()}, 4);
    out->ops = (in->h + rows - 1) / rows;
    out->c[0] = out->ops;
    out->c[1] = in->h;
    out->c[2] = (uint64_t)in->w * in->h;
    out->c[3] = (uint64_t)in->w * in->h * bpp;
    out->c[4] = (uint64_t)in->w * in->h * px;
    out->c[5] = rows;
    return 0;
}

/* ---- the change detection: the hook's signature, the worker's tracker ---- */

static uint16_t norm(uint32_t v, uint32_t full)
{
    return (uint16_t)(((uint64_t)v << 16) / full);
}

static int stage_scene_sig(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static SceneSig sig;
    const void *fb;
    uint64_t t0 = 0;
    if (!region_ok(in) || !vjo_fb_bpp(in->fmt)) return -1;
    fb = frame_in(in, in->fmt);
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        if (bench_region_sig((uintptr_t)fb, in->fb_w, in->fmt, in->fb_w, in->fb_h, norm(in->x, in->fb_w),
                             norm(in->y, in->fb_h), norm(in->w, in->fb_w), norm(in->h, in->fb_h), &sig))
            return -2;
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64(0, sig.hash, sizeof(sig.hash));
    out->ops = 1;
    out->c[0] = sig.w;
    out->c[1] = sig.h;
    out->c[2] = sig.cols;
    return 0;
}

/* mode 0: the region shows the same screen (the common case between pages);
 * mode 1: one cell changes on every signature (text typing out). Setup runs 64
 * signatures (4 s at the kernel worker's 64 ms) and checks the tracker's
 * state; each timed iteration is one more scene_update. */
static int stage_scene_update(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static SceneTracker t;
    static SceneSig sig;
    static SceneAcc acc;
    int64_t now = 1000000;
    uint64_t t0 = 0, events = 0;
    if (!region_ok(in)) return -1;
    memset(&t, 0, sizeof(t));
    scene_reset(&t);
    scene_acc_begin(&acc, &sig, in->w, in->h);
    for (unsigned k = 0; k < 64; k++, now += 64000) {
        if (in->mode & 1u) sig.hash[k % SCENE_CELLS] ^= 0x9E3779B9u;
        events += scene_update(&t, &sig, now, 0, 0) != SCENE_NONE;
    }
    out->check = fnv64(fnv64(0, &t.id, sizeof(t.id)), &t.stable, sizeof(t.stable));
    out->check = fnv64(out->check, &events, sizeof(events));
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        if (in->mode & 1u) sig.hash[it % SCENE_CELLS] ^= 0x9E3779B9u;
        scene_update(&t, &sig, now, 0, 0);
        now += 64000;
    }
    out->ns = now_ns(in) - t0;
    out->ops = 1;
    out->c[0] = events;
    return 0;
}

/* ---- the line finder (vita-vn-ocr runtime/vocr_lines_rows.c) ---- */

typedef struct {
    const BenchIn *in;
    uint64_t calls;
} RowSrc;

static const unsigned char *plane_row(void *user, unsigned y)
{
    RowSrc *s = (RowSrc *)user;
    s->calls++;
    return y < s->in->h ? grey + (size_t)y * s->in->w : NULL;
}

static int finder_mode(const BenchIn *in)
{
    const VjoVocrModel *m = in->model_name ? vjo_vocr_model(in->model_name) : NULL;
    return m ? (int)m->finder_mode : VOCR_LINES_GRAD;
}

static int find(const BenchIn *in, RowSrc *src, VocrLineSet *set)
{
    VocrLinesRows rows = {in->w, in->h, VOCR_LINES_GREY, plane_row, src};
    size_t need = vocr_lines_arena_bytes(in->w, in->h);
    if (need > sizeof(finder_arena)) return -1;
    /* API 50: the shipped default, ocr_line_fix = on (VOCR_FIX_DEFAULT; core/vocr_ocr.c's call) */
    return vocr_find_lines_fix_rows(&rows, finder_mode(in), VOCR_FIX_DEFAULT, finder_arena, need, set);
}

static int stage_find(const BenchIn *in, unsigned iters, BenchOut *out)
{
    VocrLineSet set;
    RowSrc src = {in, 0};
    uint64_t t0 = 0, calls = 0;
    if (!region_ok(in)) return -1;
    grey_plane(in);
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) { t0 = now_ns(in); calls = src.calls; }
        if (find(in, &src, &set)) return -2;
    }
    out->ns = now_ns(in) - t0;
    for (unsigned i = 0; i < set.count; i++) out->check = fnv64(out->check, &set.lines[i], sizeof(set.lines[i]));
    out->ops = 1;
    out->c[0] = set.count;
    out->c[1] = iters ? (src.calls - calls) / iters : 0;
    out->c[2] = vocr_lines_arena_bytes(in->w, in->h);
    return 0;
}

/* ---- recognition (vita-vn-ocr runtime/vocr.c), resident weights ---- */

static vocr_model rmodel;
static vocr_ctx rctx;

static int load_model(const BenchIn *in, size_t *arena)
{
    vocr_opts opts;
    if (!in->model || !in->model_bytes || in->model_bytes > sizeof(weights)) return -1;
    memcpy(weights, in->model, in->model_bytes);
    if (vocr_load(&rmodel, weights, in->model_bytes) != VOCR_OK) return -1;
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    *arena = vocr_arena_bytes(&rmodel, &opts);
    if (!*arena || *arena > sizeof(workspace)) return -1;
    return vocr_init(&rctx, &rmodel, &opts, workspace, *arena) == VOCR_OK ? 0 : -1;
}

static int stage_recognize(const BenchIn *in, unsigned iters, BenchOut *out)
{
    VocrLineSet set;
    RowSrc src = {in, 0};
    size_t arena = 0;
    uint64_t t0 = 0, frames = 0, bytes = 0;
    if (!region_ok(in) || load_model(in, &arena)) return -1;
    grey_plane(in);
    if (find(in, &src, &set)) return -2;
    for (unsigned it = 0; it <= iters; it++) {
        size_t used = 0;
        if (it == 1) t0 = now_ns(in);
        frames = bytes = 0;
        for (unsigned i = 0; i < set.count; i++) {
            const VocrLine *ln = &set.lines[i];
            vocr_image img = {grey + (size_t)ln->y * in->w + ln->x, ln->w, ln->h, in->w,
                              VOCR_GREY8 | (ln->dark ? VOCR_INVERT : 0)};
            vocr_extra extra;
            char *t = out->text + used;
            memset(&extra, 0, sizeof(extra));
            if (vocr_run(&rctx, &img, t, sizeof(out->text) - used - 1, &extra) != VOCR_OK) return -3;
            frames += extra.frames;
            bytes += strlen(t);
            used += strlen(t);
            out->text[used++] = '\n';
            out->text[used] = 0;
        }
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64s(0, out->text);
    out->ops = set.count;
    out->c[0] = set.count;
    out->c[1] = frames;
    out->c[2] = bytes;
    out->c[3] = arena;
    out->c[4] = rmodel.params;
    return 0;
}

/* ---- the whole game-worker job (core/vocr_ocr.c) ---- */

/* The job's clock, simulated: the display hook answers each strip with the
 * game's next frame (1/fps s), and the worker's sleep (sceKernelDelayThread) lets
 * that much time pass; recognition takes none of it. So the stable-text wait
 * reads exactly the passes it would at that frame rate, and the cascade's time
 * ceiling never binds (docs/performance.md says so). */
static uint64_t sim_us;
static uint32_t sim_frame_us = 16667u;

typedef struct {
    const BenchIn *in;
    uint64_t strips, rows;
} StripSrc;

/* The hook's rows, as already measured by the capture stage: a copy from the
 * grey plane stands in for them here. */
static int plane_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    StripSrc *s = (StripSrc *)ud;
    (void)flags;
    if (y >= s->in->h || n > s->in->h - y) return VJO_E_SOURCE;
    for (unsigned r = 0; r < n; r++) memcpy(dst + (size_t)r * stride, grey + (size_t)(y + r) * s->in->w, s->in->w);
    s->strips++;
    s->rows += n;
    sim_us += sim_frame_us;
    return (int)n;
}

static uint64_t sim_now(void *ud)
{
    (void)ud;
    return sim_us;
}

static void sim_sleep(uint32_t us)
{
    sim_us += us;
}

static void *tall_line(void *ud, size_t bytes)
{
    (void)ud;
    return bytes <= sizeof(line_block) ? line_block : NULL;
}

/* The weights, the LM and the cascade's weights as files in memory, as the game
 * reads them through the kernel's model broker. */
typedef struct {
    const uint8_t *data;
    uint32_t bytes;
} MemFile;
static MemFile mem_files[3];
static int mem_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    const MemFile *f = (const MemFile *)ctx;
    if (off > f->bytes || n > f->bytes - off) return -1;
    memcpy(dst, f->data + off, n);
    return 0;
}
static void mem_close(void *ctx) { (void)ctx; }
static const BenchIn *mem_in;
static int ends_with(const char *path, const char *name)
{
    size_t p = strlen(path), n = name ? strlen(name) : 0;
    return n && p >= n && !strcmp(path + p - n, name) && (p == n || path[p - n - 1] == '/');
}
static int mem_open(void *ud, const char *path, VjoFile *f)
{
    const char *names[3] = {mem_in->model_name, mem_in->lm_name, mem_in->cascade_name};
    (void)ud;
    for (unsigned i = 0; i < 3; i++)
        if (mem_files[i].data && ends_with(path, names[i])) {
            *f = (VjoFile){&mem_files[i], mem_files[i].bytes, mem_read, mem_close, 1};
            return 0;
        }
    return -1;
}

/* The resident blocks' memory (the game's USER_RW blocks), static here. */
static unsigned char lm_mem[1u << 20] __attribute__((aligned(64)));
static unsigned char cascade_mem[2u << 20] __attribute__((aligned(64)));
static void *weights_alloc(void *ud, size_t bytes) { (void)ud; return bytes <= sizeof(weights) ? weights : NULL; }
static void *lm_alloc(void *ud, size_t bytes) { (void)ud; return bytes <= sizeof(lm_mem) ? lm_mem : NULL; }
static void *cascade_alloc(void *ud, size_t bytes) { (void)ud; return bytes <= sizeof(cascade_mem) ? cascade_mem : NULL; }
static int no_release(void *ud) { (void)ud; return 0; }

/* mode: the game worker's job policy (game_ocr/main.c vjo_game_ocr_run_request,
 * with the config defaults of core/config.c), in bits:
 *   1: a game's first job: the weights (and the LM) read and SHA-256 checked in
 *      every iteration, as at a game's first job; else resident, as every later
 *      job with the files unchanged;
 *   2: beam + LM decoding with in->lm (ocr_lm = 1000k: tier 2);
 *   4: the confidence cascade with in->cascade (VOCR4-W20, ocr_cascade_ms = 2000,
 *      lines under VJO_VOCR_CASCADE_CONF read again);
 *   8: the stable-text wait (ocr_settle_ms = 150, ocr_settle_max_ms = 2000), on
 *      the simulated clock at in->fps;
 *  16: the finder's boxes refined (ocr_refine_lines: a PSP game by default).
 * 2 | 4 | 8 is a Vita game's job on main's defaults; 0 is API 19's job. */
#define JOB_FIRST 1u
#define JOB_LM 2u
#define JOB_CASCADE 4u
#define JOB_SETTLE 8u
#define JOB_REFINE 16u
static int stage_job(const BenchIn *in, unsigned iters, BenchOut *out)
{
    VjoPlatform p;
    static VjoVocrResident res, cres;
    static VjoVocrLmResident lres;
    static VjoVocrStats st;
    VjoVocrPolicy pol;
    static char raw[VJO_OCR_TEXT_CAP];
    const VjoVocrModel *mi = in->model_name ? vjo_vocr_model(in->model_name) : NULL;
    const VjoVocrModel *ci = in->cascade_name ? vjo_vocr_model(in->cascade_name) : NULL;
    const VjoVocrLm *lm = in->lm_name ? vjo_vocr_lm(in->lm_name) : NULL;
    StripSrc ss = {in, 0, 0};
    VjoGreySource src = {&ss, in->w, in->h, plane_strip, NULL, tall_line, NULL};
    size_t ws, cap = vjo_vocr_capture_bytes(in->w, in->h);
    unsigned mode = in->mode;
    uint64_t t0 = 0, strips0 = 0, rows0 = 0, sim0 = 0;
    if (!region_ok(in) || !mi || !in->model || in->model_bytes != mi->bytes) return -1;
    if ((mode & JOB_LM) && (!lm || !in->lm || in->lm_bytes != lm->bytes)) return -1;
    if ((mode & JOB_CASCADE) && (!ci || !in->cascade || in->cascade_bytes != ci->bytes)) return -1;
    memset(&pol, 0, sizeof(pol));
    pol.line_fix = VOCR_FIX_DEFAULT; /* API 50: main's default, ocr_line_fix = on */
    ws = vjo_vocr_job_bytes(mi->name, in->w, in->h);
    if (mode & JOB_REFINE) {
        pol.refine = VJO_VOCR_REFINE_ALL;
        ws += vjo_vocr_refine_bytes(in->w, in->h);
    }
    if (mode & (JOB_LM | JOB_CASCADE)) /* game_ocr/main.c extra_job_bytes, the greedy text aside */
        ws += vjo_vocr_lm_job_bytes(mi->name) + (mode & JOB_CASCADE ? vjo_vocr_cascade_job_bytes(ci->name) : 0);
    if (!ws || ws > sizeof(workspace) || !cap || cap > sizeof(capture_block)) return -1;
    if (mode & JOB_LM) {
        pol.lm = lm;
        pol.lm_res = &lres;
        pol.raw = raw;
        pol.raw_cap = sizeof(raw);
    }
    if (mode & JOB_CASCADE) {
        pol.cascade = ci;
        pol.cascade_res = &cres;
        pol.cascade_us = 2000000u;
        pol.cascade_conf = 0; /* game_ocr/main.c VJO_GAME_CASCADE_CONF: the core's VJO_VOCR_CASCADE_CONF */
    }
    if (mode & JOB_SETTLE) {
        pol.settle_us = 150000u;
        pol.settle_max_us = 2000000u;
        pol.sleep = sim_sleep;
    }
    sim_frame_us = 1000000u / (in->fps ? in->fps : 60u);
    grey_plane(in);
    memset(&p, 0, sizeof(p));
    p.file_open = mem_open;
    p.now_us = sim_now;
    mem_in = in;
    mem_files[0] = (MemFile){in->model, in->model_bytes};
    mem_files[1] = (MemFile){mode & JOB_LM ? in->lm : NULL, in->lm_bytes};
    mem_files[2] = (MemFile){mode & JOB_CASCADE ? in->cascade : NULL, in->cascade_bytes};
    memset(&res, 0, sizeof(res));
    memset(&cres, 0, sizeof(cres));
    memset(&lres, 0, sizeof(lres));
    res.alloc = weights_alloc;
    res.release = no_release;
    cres.alloc = cascade_alloc;
    cres.release = no_release;
    lres.alloc = lm_alloc;
    lres.release = no_release;
    for (unsigned it = 0; it <= iters; it++) {
        int rc;
        if (it == 1) { t0 = now_ns(in); strips0 = ss.strips; rows0 = ss.rows; sim0 = sim_us; }
        if (mode & JOB_FIRST) { /* a game's first job: nothing resident */
            vjo_vocr_resident_drop(&res);
            vjo_vocr_resident_drop(&cres);
            vjo_vocr_lm_drop(&lres);
        }
        rc = vjo_vocr_ocr_policy(&p, "bench", mi->name, &src, &res, workspace, ws, capture_block, cap, &pol, out->text,
                                 sizeof(out->text), &st);
        if (rc) return -2;
        /* with lines, the LM must have decoded them and the cascade been usable */
        if ((mode & JOB_LM) && st.lines && (st.lm != lm || st.lm_rc)) return -3;
        if ((mode & JOB_CASCADE) && st.lines && st.cascade_rc) return -4;
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64s(0, out->text);
    if (mode & JOB_LM) out->check = fnv64s(out->check, raw);
    out->ops = 1;
    out->c[0] = iters ? (ss.strips - strips0) / iters : 0;
    out->c[1] = st.lines;
    out->c[2] = st.frames;
    out->c[3] = st.tears;
    out->c[4] = st.tries;
    out->c[5] = st.tall;
    out->c[6] = ws;
    out->c[7] = cap;
    out->c[8] = iters ? (ss.rows - rows0) / iters : 0;
    out->c[9] = st.strip_rows;
    out->c[10] = st.model_loaded;
    out->c[11] = st.model_cached;
    out->c[12] = st.settle_passes;
    out->c[13] = st.lm_lines;
    out->c[14] = st.lm_changed;
    out->c[15] = st.cascade_lines;
    out->c[16] = st.cascade_frames;
    out->c[17] = st.lm_loaded;
    out->c[18] = iters ? (sim_us - sim0) / iters / 1000u : 0; /* simulated ms per job: the display frames */
    out->c[19] = st.refine_lines;
    return 0;
}

static int stage_sha256(const BenchIn *in, unsigned iters, BenchOut *out)
{
    br_sha256_context h;
    unsigned char digest[32];
    uint64_t t0 = 0;
    if (!in->model || !in->model_bytes) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        br_sha256_init(&h);
        for (uint32_t off = 0; off < in->model_bytes; off += 32768u) {
            uint32_t n = in->model_bytes - off < 32768u ? in->model_bytes - off : 32768u;
            br_sha256_update(&h, in->model + off, n);
        }
        br_sha256_out(&h, digest);
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64(0, digest, sizeof(digest));
    out->ops = 1;
    out->c[0] = in->model_bytes;
    return 0;
}

/* ---- the Shell: the recognized text (shell/worker.c run_vocr) ---- */

static int stage_postproc(const BenchIn *in, unsigned iters, BenchOut *out)
{
    const char *texts[TEXTS_MAX];
    unsigned n = split_texts(in, texts, TEXTS_MAX);
    uint64_t t0 = 0, bytes_in = 0, bytes_out = 0;
    VjoArena a;
    if (!n) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        bytes_in = bytes_out = 0;
        for (unsigned i = 0; i < n; i++) {
            char *f, *s;
            size_t len;
            vjo_arena_init(&a, scratch_mem, sizeof(scratch_mem));
            f = vjo_filter_lines(&a, texts[i], VJO_FILTER_LINES);
            if (!f) return -2;
            s = vjo_java_trim(f);
            len = strlen(s);
            bytes_in += strlen(texts[i]);
            bytes_out += len + vjo_utf8_japanese_cursor_step(s, len, 0, 0);
            if (it == 0) out->check = fnv64s(out->check, s);
        }
    }
    out->ns = now_ns(in) - t0;
    out->ops = n;
    out->c[0] = n;
    out->c[1] = bytes_in;
    out->c[2] = bytes_out;
    return 0;
}

static void local_config(const BenchIn *in, VjoConfig *cfg)
{
    vjo_config_defaults(cfg);
    cfg->dictionary = VJO_DICT_LOCAL;
    cfg->local_dictionary_results = 1;
    vjo_snprintf(cfg->local_dictionary_dir, sizeof(cfg->local_dictionary_dir), "%s", in->dict_dir);
    vjo_snprintf(cfg->local_dictionaries, sizeof(cfg->local_dictionaries), "plaintext.vjdict");
}

/* The local word lookup (core/client.c, core/local_dict.c) with the bundled
 * dictionary. mode 0: the first word of each text (what Auto OCR prepares);
 * mode 1: every selectable letter (Right in Character mode, or a tap). */
static int stage_lookup(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static VjoConfig cfg;
    const char *texts[TEXTS_MAX];
    unsigned n = split_texts(in, texts, TEXTS_MAX);
    VjoPlatform p;
    uint64_t t0 = 0, lookups = 0, entries = 0, peak = 0;
    uint64_t opens, reads, bytes, repeats, pages, opens0 = 0, reads0 = 0, bytes0 = 0, repeats0 = 0, pages0 = 0;
    if (!n || !in->dict_dir || !in->file_open) return -1;
    local_config(in, &cfg);
    memset(&p, 0, sizeof(p));
    p.file_open = in->file_open;
    bench_io_mark();
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) {
            t0 = now_ns(in);
            bench_io_counts(&opens0, &reads0, &bytes0, &repeats0, &pages0);
        }
        lookups = entries = 0;
        for (unsigned i = 0; i < n; i++) {
            size_t len = strlen(texts[i]);
            size_t cursor = vjo_utf8_japanese_cursor_step(texts[i], len, 0, 0);
            bench_io_text();
            while (cursor < len) {
                VjoArena a;
                VjoOverlayData d;
                size_t next;
                int rc;
                vjo_arena_init(&a, lookup_mem, sizeof(lookup_mem));
                memset(&d, 0, sizeof(d));
                d.ocr_text = d.filtered = d.sentence = texts[i];
                rc = vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d);
                lookups++;
                if (a.peak > peak) peak = a.peak;
                if (!rc && d.list.n_entries) {
                    entries++;
                    if (it == 0) {
                        const VjoEntry *e = &d.list.entries[0];
                        out->check = fnv64s(out->check, e->text);
                        out->check = fnv64(out->check, &e->hl_start, sizeof(e->hl_start));
                        out->check = fnv64(out->check, &e->hl_end, sizeof(e->hl_end));
                    }
                } else if (it == 0) {
                    out->check = fnv64(out->check, &rc, sizeof(rc));
                }
                if (!(in->mode & 1u)) break;
                next = vjo_utf8_cursor_step(texts[i], len, cursor, 1);
                next = next > cursor ? vjo_utf8_japanese_cursor_step(texts[i], len, next, 0) : len;
                if (next <= cursor) break;
                cursor = next;
            }
        }
    }
    out->ns = now_ns(in) - t0;
    bench_io_counts(&opens, &reads, &bytes, &repeats, &pages);
    out->ops = lookups;
    out->c[0] = lookups;
    out->c[1] = entries;
    if (iters) {
        out->c[2] = (opens - opens0) / iters;
        out->c[3] = (reads - reads0) / iters;
        out->c[4] = (bytes - bytes0) / iters;
        out->c[5] = (repeats - repeats0) / iters;
        out->c[6] = (pages - pages0) / iters;
    }
    out->c[7] = peak;
    return 0;
}

/* config.ini, as every overlay open reads it (shell/files.c vjo_config_load). */
/* Settings that differ from every default, parsed once untimed: docs/config.example.ini
 * holds the defaults, so without these a parser that read nothing would keep the
 * checksum (audit-overlay-perf-1b N2). */
static const char nondefault_config[] =
    "local_dictionary_results = 3\n"
    "japanese_only_selection = on\n"
    "highlight_mode = character\n"
    "stick_lookup_ms = 350\n"
    "font_size_ja = 30\n"
    "font_size_en = 18\n"
    "ocr_mode = on_press\n"
    "vocr_tier = small\n"
    "ocr_settle_ms = 0\n"
    "ocr_settle_max_ms = 500\n"
    "ocr_settle_press_ms = 300\n"
    "ocr_cascade_ms = 0\n"
    "ocr_lm = off\n"
    "ocr_refine_lines = on\n"
    "ocr_skip_blank_lines = on\n"
    "ocr_threads = 1\n"
    "ocr_boost_clock = on\n"
    "ocr_vertical = on\n"
    "log_file = off\n"
    "anki_mining = on\n"
    "anki_screenshot = off\n"
    "ocr_text_gate = on\n"
    "hook_sites = off\n"
    "anki_screenshot_trim_bars = off\n"
    "reading_time = 15s\n"
    "hook_trust = off\n"
    "ocr_line_fix = all\n"
    "overlay_on_text = on\n" /* API 46 */
    "ocr_pause = on\n";

static int stage_config(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static VjoConfig cfg, alt;
    const char *text = in->config_text ? in->config_text : vjo_config_default_text();
    size_t len = strlen(text);
    uint64_t t0 = 0;
    vjo_config_defaults(&alt);
    vjo_config_parse(&alt, nondefault_config, sizeof(nondefault_config) - 1);
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, text, len);
    }
    out->ns = now_ns(in) - t0;
    /* Every parsed setting: VjoConfig is ints and char arrays only (no pointer,
     * no padding), laid out alike by both compilers, and zeroed by
     * vjo_config_defaults first, so its bytes are the output. */
    out->check = fnv64(fnv64(0, &cfg, sizeof(cfg)), &alt, sizeof(alt));
    out->ops = 1;
    out->c[0] = len;
    out->c[1] = (uint64_t)cfg.n_warnings;
    out->c[2] = sizeof(cfg);
    out->c[3] = (uint64_t)alt.n_warnings; /* 0: every non-default setting was accepted */
    return 0;
}

/* Formatting one Auto OCR job's log lines: a copy of the Shell's own format
 * strings (shell/worker.c vocr_run_one, log_vocr_measurements, log_text_lines,
 * log_lookup; shell/selftest.c vjo_gstep_format for the game worker's steps),
 * with synthetic values of the committed sample's job (its 896x156 region, the
 * bench's own workspace and capture sizes, round free sizes and times; no value
 * from a console log), in the order a job on main's defaults logs
 * them. It is a replica (the Shell's code calls the SDK), so it measures the
 * formatting only: the file system's cost per line is the console's
 * (docs/performance.md, console calibration). The checksum is the formatted
 * bytes. */
static int stage_log(const BenchIn *in, unsigned iters, BenchOut *out)
{
    /* a resident job's steps on main's defaults (game_ocr/main.c vjo_game_ocr_run_request) */
    static const char *const steps[] = {"mem",  "check",     "alloc", "alloc",   "pass", "sig",  "find", "settle",
                                        "load", "recognize", "lm",    "cascade", "norm", "free", "free"};
    char line[256];
    uint64_t t0 = 0, bytes = 0, h = 0;
    unsigned lines = 0;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        bytes = lines = 0;
#define LOGF(...)                                                                                                      \
    do {                                                                                                               \
        int k_ = vjo_snprintf(line, sizeof(line) - 1, __VA_ARGS__);                                                    \
        if (k_ > (int)sizeof(line) - 2) k_ = (int)sizeof(line) - 2;                                                    \
        if (k_ >= 0) {                                                                                                 \
            line[k_++] = '\n';                                                                                         \
            bytes += (uint64_t)k_;                                                                                     \
            if (it == 0) h = fnv64(h, line, (size_t)k_);                                                               \
        }                                                                                                              \
        lines++;                                                                                                       \
    } while (0)
        LOGF("job start (%s)%s", "auto: screen settled", "");
        LOGF("game vocr rc=%d %s%s: %ux%u at %u,%u of %ux%u fmt 0x%08X, lines=%u USER workspace=%u weights=%s "
             "resident=%u, find=%u ms load=%u ms recognize=%u ms frames=%u io=0x%08X stat=0x%08X cleanup=%d",
             0, "VOCR3_H15_w8.vocr", "", 896u, 156u, 32u, 362u, 960u, 544u, 0u, 2u, 186704u, "resident", 1159168u,
             100u, 1u, 546u, 108u, 0u, 0u, 0);
        LOGF("game vocr capture: %s into %s, %u B block, %u-row strips, %u strips in %u ms, %u rows changed, sum 0x%08X",
             "hook", "USER", 65536u, 64u, 13u, 217u, 0u, 0x12345678u);
        LOGF("game vocr memory: free USER=%d, needs workspace %u + weights %u + reserve %u: check %s, headroom %d",
             90000000, 610304u, 0u, 4194304u, "passed", 85195392);
        LOGF("game vocr speed: %u frames in %u us, %u us per frame", 108u, 546480u, 5060u);
        LOGF("%s %ux%u %uf %u.%ums c%u%s%s", "game vocr lines:", 860u, 32u, 54u, 273u, 2u, 900u, "", "");
        for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
            LOGF("gstep %s %s kind=%s arg=%s rc=0x%08X bytes=%u sum=0x%08X main=%d cdram=%d phycont=%d us=%u", "job",
                 steps[s], s == 2 || s == 3 || s >= 13 ? "user" : "-",
                 s == 2 || s == 13 ? "capture" : s == 3 || s == 14 ? "workspace" : "0", 0u, 65536u, 0u, 90000000,
                 12000000, 27000000, 1000u * s + 17u);
        LOGF("game vocr text %u: %s", 0u, "\"朝ごはん、もう冷\"");
        LOGF("result arenas Paf allocation: free=%u requested=%u reserve=%u success=%d", 2600000u, 65536u, 1572864u, 1);
        LOGF("local dictionary workspace: %u bytes, %d %u-byte results, %u-byte connection pool", 65536u, 1, 65536u, 0u);
        LOGF("lookup %s at %u: %d chars, dictionary %s, page %d/%d, %u reads %u B, rc=%d, %u ms; \"%s\" -> %s%s%s%s%s%s%s%s",
             "first word", 0u, 2, "1/1", 1, 1, 15u, 61440u, 0, 73u, "朝ごはん、もう冷", "朝御飯", " (", "あさごはん", ")",
             " [", "Bee's Ultimate Dictionary", "]", " #59004");
        LOGF("subtitle text ready in %d ms", 1300);
        LOGF("job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d", 0, 0, 1350, 53552u, 1, 1);
#undef LOGF
    }
    out->ns = now_ns(in) - t0;
    out->check = h;
    out->ops = lines;
    out->c[0] = lines;
    out->c[1] = bytes;
    return 0;
}

/* Entries for each text's first word, for the Anki and furigana stages. */
static unsigned first_entries(const BenchIn *in, VjoConfig *cfg, VjoOverlayData *d, unsigned cap)
{
    const char *texts[TEXTS_MAX];
    unsigned n = split_texts(in, texts, TEXTS_MAX < cap ? TEXTS_MAX : cap), k = 0;
    VjoPlatform p;
    VjoArena a;
    if (!n || !in->dict_dir || !in->file_open) return 0;
    local_config(in, cfg);
    memset(&p, 0, sizeof(p));
    p.file_open = in->file_open;
    vjo_arena_init(&a, big_mem, sizeof(big_mem));
    for (unsigned i = 0; i < n; i++) {
        size_t len = strlen(texts[i]);
        memset(&d[k], 0, sizeof(d[k]));
        d[k].ocr_text = d[k].filtered = d[k].sentence = texts[i];
        if (!vjo_overlay_lookup_at(&a, &p, cfg, vjo_utf8_japanese_cursor_step(texts[i], len, 0, 0), &d[k]) &&
            d[k].list.n_entries)
            k++;
    }
    return k;
}

/* × in the overlay: the card's note and its queue record (core/anki.c,
 * core/anki_queue.c), text only as the build ships it. */
static int stage_anki(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static VjoOverlayData d[TEXTS_MAX];
    static VjoConfig cfg;
    unsigned n = first_entries(in, &cfg, d, TEXTS_MAX);
    uint64_t t0 = 0, bytes = 0;
    if (!n) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        bytes = 0;
        for (unsigned i = 0; i < n; i++) {
            VjoArena a;
            VjoAnkiNote note;
            char *json;
            vjo_arena_init(&a, scratch_mem, sizeof(scratch_mem));
            if (vjo_anki_note_from_entry(&a, &d[i].list, 0, &note)) return -2;
            json = vjo_queue_encode(&a, &note, 0);
            if (!json) return -3;
            bytes += strlen(json);
            if (it == 0) out->check = fnv64s(out->check, json); /* the record itself, every byte */
        }
    }
    out->ns = now_ns(in) - t0;
    out->ops = n;
    out->c[0] = n;
    out->c[1] = bytes;
    return 0;
}

/* A word's furigana: its surface aligned with its reading (core/furigana.c), as
 * an Anki card's ExpressionFurigana is made from a queued record that carries its
 * dictionary's ruby (the removed online dictionaries gave one). */
static int stage_furigana(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static VjoOverlayData d[TEXTS_MAX];
    static VjoConfig cfg;
    unsigned n = first_entries(in, &cfg, d, TEXTS_MAX);
    uint64_t t0 = 0, bytes = 0;
    if (!n) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        bytes = 0;
        for (unsigned i = 0; i < n; i++) {
            const VjoVocab *v = d[i].list.entries[0].vocab;
            VjoArena a;
            char *r;
            vjo_arena_init(&a, scratch_mem, sizeof(scratch_mem));
            r = vjo_furigana_align(&a, v->spelling, strlen(v->spelling), v->reading);
            if (!r) return -2;
            bytes += strlen(r);
            if (it == 0) out->check = fnv64s(out->check, r);
        }
    }
    out->ns = now_ns(in) - t0;
    out->ops = n;
    out->c[0] = n;
    out->c[1] = bytes;
    return 0;
}

/* ---- API 23: the Anki screenshot's JPEG (core/jpegsw.c, in the game worker) ----
 * The whole frame at the shot's first quality, 70, band by band as the worker
 * encodes its strips (game_ocr/main.c run_shot), into an arena of the worker's
 * block: the encoder's state and the JPEG. */
static int stage_jpeg(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static uint8_t mem[4096 + 16 + 192 * 1024 + 16];
    VjoArena a;
    VjoBuf b = {0};
    uint64_t t0 = 0;
    if (!in->rgba || !in->fb_w || in->fb_w > 960 || !in->fb_h || in->fb_h > 544) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        VjoJpeg *e;
        if (it == 1) t0 = now_ns(in);
        vjo_arena_init(&a, mem, sizeof(mem));
        if (!(e = vjo_jpeg_begin(&a, in->fb_w, in->fb_h, 70, &b))) return -2;
        for (uint32_t y = 0; y < in->fb_h; y += VJO_JPEG_BAND) {
            uint32_t n = in->fb_h - y < VJO_JPEG_BAND ? in->fb_h - y : VJO_JPEG_BAND;
            if (vjo_jpeg_rows(e, in->rgba + (size_t)y * in->fb_w * 4u, n, in->fb_w * 4u) < 0) return -3;
        }
        if (vjo_jpeg_end(e) < 0) return -4;
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64(0, b.data, b.len);
    out->ops = 1;
    out->c[0] = b.len;
    out->c[1] = in->fb_w;
    out->c[2] = in->fb_h;
    out->c[3] = a.peak;
    return 0;
}

/* ---- the reading total (docs/ocr-history.md#total-characters-read; shell/history.c) ---- */

/* history.csv of TOTAL_RECORDS records, as the Shell's writer writes them (shell/history.c
 * vjo_history_append) for a resident job of the texts in turn: each text its line and its raw line, the
 * OCR facts' 30 cells at a resident job's sizes, one metrics entry a row, the characters and the source. */
#define TOTAL_RECORDS 2000u
static char history_mem[TOTAL_RECORDS * 1024u];
static char total_window[VJO_HISTORY_WINDOW + 1];

static size_t put_field(char *p, const char *s)
{
    size_t n = 0;
    p[n++] = '"';
    for (; *s; s++) {
        if (*s == '"') p[n++] = '"';
        p[n++] = *s;
    }
    p[n++] = '"';
    return n;
}

static size_t history_image(const BenchIn *in, unsigned *records)
{
    static const char header[] =
        "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,"
        "scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,"
        "fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,"
        "recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,"
        "capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,characters,line_source\n";
    const char *texts[TEXTS_MAX];
    unsigned n = split_texts(in, texts, TEXTS_MAX), k;
    size_t len = strlen(header);
    if (!n) return 0;
    memcpy(history_mem, header, len);
    for (k = 0; k < TOTAL_RECORDS; k++) {
        const char *text = texts[k % n];
        unsigned rows = 1;
        uint64_t t = 1792220400250123ull + (uint64_t)k * 7000000u;
        for (const char *c = text; *c; c++) rows += *c == '\n';
        if (len + 2 * strlen(text) * 2 + 600 > sizeof(history_mem)) break;
        len += (size_t)vjo_snprintf(history_mem + len, 64, "\"vita-PCSG00415\",\"vjo-%llu-%u\",",
                                    (unsigned long long)t, (unsigned)len);
        len += put_field(history_mem + len, "CLANNAD");
        history_mem[len++] = ',';
        len += put_field(history_mem + len, text);
        len += (size_t)vjo_snprintf(history_mem + len, 96, ",%llu.%06u,\"vita\",\"PCSG00415\",\"vocr\",\"VOCR5_N_w8.vocr\",",
                                    (unsigned long long)(t / 1000000u), (unsigned)(t % 1000000u));
        len += put_field(history_mem + len, text); /* the raw line: the recognizer's text, as long */
        len += (size_t)vjo_snprintf(history_mem + len, 256,
                                    ",17,%u,%llu,550000,%u,32,362,896,156,960,544,960,0,%u,%u,%u,98000,0,%u,88000,6,0,"
                                    "%u,1,0,65536,64,610304,1159168,0,\"",
                                    k + 23, (unsigned long long)(9876543210ull + k * 7000000ull), k + 42, rows, rows,
                                    rows * 54u, rows * 273000u, 1234567890u + k);
        for (unsigned r = 0; r < rows; r++)
            len += (size_t)vjo_snprintf(history_mem + len, 48, "%s%u:32:54:%u:273000", r ? "|" : "", 860u - 40u * r,
                                        900u + r);
        len += (size_t)vjo_snprintf(history_mem + len, 32, "\",%u,\"ocr\"\n",
                                    (unsigned)vjo_line_characters(text, strlen(text)));
    }
    *records = k;
    return len;
}

/* A Triangle press's count: the whole file in 24,512 B windows read with the byte before each into the
 * control thread's scratch (the memcpy here stands in for the read), each from the newest record start of
 * the one before, as vjo_history_total_step takes them. Per record. */
static int stage_total(const BenchIn *in, unsigned iters, BenchOut *out)
{
    unsigned records = 0, windows = 0;
    size_t len = history_image(in, &records);
    uint64_t t0 = 0, chars = 0;
    if (!len) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        size_t at = 0;
        if (it == 1) t0 = now_ns(in);
        chars = 0;
        windows = 0;
        for (;;) {
            size_t to = len - at > VJO_HISTORY_WINDOW ? at + VJO_HISTORY_WINDOW : len, base = at ? at - 1 : 0;
            size_t first, last;
            int cut = to < len;
            memcpy(total_window, history_mem + base, to - base);
            chars += vjo_history_chars(total_window, to - base, !at, cut, &first, &last);
            windows++;
            if (!cut) break;
            at = last < to - base && base + last > at ? base + last : to;
        }
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64(fnv64(0, &chars, sizeof(chars)), &windows, sizeof(windows));
    out->ops = records;
    out->c[0] = records;
    out->c[1] = len;
    out->c[2] = windows;
    out->c[3] = chars;
    return 0;
}

/* Line history's read (shell/history.c vjo_history_load, core/history_lines.c vjo_history_scan): the first
 * Up past the overlay's top row reads history.csv's last 24,512 B and the byte before them into the control
 * thread's scratch and keeps this game's lines, newest first (the current text: the newest record's). */
static int stage_history_scan(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static uint8_t lines_mem[16384];
    static VjoHistory h;
    unsigned records = 0;
    size_t len = history_image(in, &records), from = len > VJO_HISTORY_WINDOW ? len - VJO_HISTORY_WINDOW - 1 : 0;
    uint64_t t0 = 0, hash = 0;
    if (!len) return -1;
    for (unsigned it = 0; it <= iters; it++) {
        VjoArena lines;
        if (it == 1) t0 = now_ns(in);
        memcpy(total_window, history_mem + from, len - from);
        memset(&h, 0, sizeof(h));
        h.line[0] = "";
        vjo_arena_init(&lines, lines_mem, sizeof(lines_mem));
        vjo_history_scan(&h, &lines, total_window, len - from, !from, "vita-PCSG00415\",", VJO_FILTER_LINES);
        if (it == 0)
            for (int k = 1; k <= h.n; k++) hash = fnv64s(hash, h.line[k]);
    }
    out->ns = now_ns(in) - t0;
    out->check = hash;
    out->ops = 1;
    out->c[0] = (uint64_t)h.n;
    out->c[1] = len - from;
    return 0;
}

/* The Triangle view's total with its commas (vjo_history_number), at every press: per number. */
static int stage_total_number(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static const uint64_t numbers[] = {7u, 12345u, 1234567u, 98765432109ull};
    char buf[32];
    uint64_t t0 = 0, h = 0, bytes = 0;
    (void)in;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        bytes = 0;
        for (unsigned i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
            const char *t = vjo_history_number(buf, sizeof(buf), numbers[i]);
            bytes += strlen(t);
            if (it == 0) h = fnv64s(h, t);
        }
    }
    out->ns = now_ns(in) - t0;
    out->check = h;
    out->ops = sizeof(numbers) / sizeof(numbers[0]);
    out->c[0] = out->ops;
    out->c[1] = bytes;
    return 0;
}

/* ---- the table ---- */

typedef struct {
    const char *name;
    int (*run)(const BenchIn *, unsigned, BenchOut *);
    const char *counters[BENCH_COUNTERS + 1];
} Stage;

static const Stage stages[] = {
    {"capture", stage_capture, {"strips", "rows", "pixels", "fb_bytes_read", "grey_bytes_written", "strip_rows"}},
    {"scene_sig", stage_scene_sig, {"sig_w", "sig_h", "grid_cols"}},
    {"scene_update", stage_scene_update, {"events"}},
    {"find", stage_find, {"lines", "row_reads", "arena_bytes"}},
    {"recognize", stage_recognize, {"lines", "frames", "text_bytes", "arena_bytes", "params"}},
    {"job", stage_job, {"strips", "lines", "frames", "tears", "tries", "tall", "workspace_bytes", "capture_bytes",
                        "rows_read", "strip_rows", "model_loaded", "model_cached", "settle_passes", "lm_lines",
                        "lm_changed", "cascade_lines", "cascade_frames", "lm_loaded", "display_ms", "refine_lines"}},
    {"sha256", stage_sha256, {"bytes"}},
    {"postproc", stage_postproc, {"texts", "bytes_in", "bytes_out"}},
    {"lookup", stage_lookup, {"lookups", "entries", "opens", "reads", "read_bytes", "repeat_reads", "pages",
                              "arena_peak"}},
    {"config", stage_config, {"bytes", "warnings", "struct_bytes", "nondefault_warnings"}},
    {"log", stage_log, {"lines", "bytes"}},
    {"anki", stage_anki, {"cards", "json_bytes"}},
    {"furigana", stage_furigana, {"words", "bytes"}},
    {"jpeg", stage_jpeg, {"jpeg_bytes", "w", "h", "arena_peak"}},
    {"total", stage_total, {"records", "bytes", "windows", "characters"}},
    {"total_number", stage_total_number, {"numbers", "bytes"}},
    {"history_scan", stage_history_scan, {"lines", "bytes"}},
};
#define N_STAGES (sizeof(stages) / sizeof(stages[0]))

static const char *const *names_list(void)
{
    static const char *names[N_STAGES + 1];
    for (unsigned i = 0; i < N_STAGES; i++) names[i] = stages[i].name;
    return names;
}

const char *const *bench_stages(void)
{
    return names_list();
}

const char *const *bench_counter_names(const char *name)
{
    for (unsigned i = 0; i < N_STAGES; i++)
        if (!strcmp(stages[i].name, name)) return stages[i].counters;
    return NULL;
}

int bench_stage(const char *name, const BenchIn *in, unsigned iters, BenchOut *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < N_STAGES; i++)
        if (!strcmp(stages[i].name, name)) return stages[i].run(in, iters, out);
    return -100;
}
