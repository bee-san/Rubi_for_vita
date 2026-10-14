/* Every stage of an OCR job and of the Shell's lookups, on the real sources
 * (stage_bench.h, docs/performance.md). Each stage runs its setup once, one
 * untimed warm-up iteration, then `iters` timed iterations of identical work,
 * so the Cortex-A9 lane can difference two runs with different counts and keep
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
#include "local_dict.h"
#include "port.h"
#include "textfilter.h"
#include "utf.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
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
static int stage_capture(const BenchIn *in, unsigned iters, BenchOut *out)
{
    const void *fb;
    unsigned rows = strip_rows(in);
    int sig = (int)(in->mode & 1u);
    uint32_t bpp = vjo_fb_bpp(in->fmt);
    uint64_t t0 = 0;
    if (!region_ok(in) || !bpp) return -1;
    fb = frame_in(in, in->fmt);
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        if (sig) bench_capture_sig_begin(in->w, in->h);
        for (uint32_t y = 0; y < in->h; y += rows) {
            uint32_t n = in->h - y < rows ? in->h - y : rows;
            int rc = bench_grey_rows((uintptr_t)fb, in->fb_w, in->fmt, in->fb_w, in->fb_h, in->x, in->y, in->w,
                                     in->h, y, n, strip, in->w, sig);
            if (rc != (int)n) return -2;
            if (it == 0) out->check = fnv64(out->check, strip, (size_t)n * in->w);
        }
    }
    out->ns = now_ns(in) - t0;
    if (sig) out->check = fnv64(out->check, &(uint32_t){bench_capture_sig_hash()}, 4);
    out->ops = (in->h + rows - 1) / rows;
    out->c[0] = out->ops;
    out->c[1] = in->h;
    out->c[2] = (uint64_t)in->w * in->h;
    out->c[3] = (uint64_t)in->w * in->h * bpp;
    out->c[4] = (uint64_t)in->w * in->h;
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
    return vocr_find_lines_rows(&rows, finder_mode(in), finder_arena, need, set);
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
    return (int)n;
}

static void *tall_line(void *ud, size_t bytes)
{
    (void)ud;
    return bytes <= sizeof(line_block) ? line_block : NULL;
}

/* The weights as a file in memory, as the game reads them through the kernel. */
static int mem_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    const BenchIn *in = (const BenchIn *)ctx;
    if (off > in->model_bytes || n > in->model_bytes - off) return -1;
    memcpy(dst, in->model + off, n);
    return 0;
}
static void mem_close(void *ctx) { (void)ctx; }
static const BenchIn *mem_in;
static int mem_open(void *ud, const char *path, VjoFile *f)
{
    (void)ud; (void)path;
    *f = (VjoFile){(void *)mem_in, mem_in->model_bytes, mem_read, mem_close, 1};
    return 0;
}

/* mode 0: weights resident (unchanged file, as every job after a game's
 * first); mode 1: one-shot, read and SHA-256 checked for every job (the
 * first job of a game: weights=read). */
static int stage_job(const BenchIn *in, unsigned iters, BenchOut *out)
{
    VjoPlatform p;
    VjoVocrResident res;
    VjoVocrStats st;
    const VjoVocrModel *mi = in->model_name ? vjo_vocr_model(in->model_name) : NULL;
    StripSrc ss = {in, 0, 0};
    VjoGreySource src = {&ss, in->w, in->h, plane_strip, NULL, tall_line};
    size_t ws, cap = vjo_vocr_capture_bytes(in->w, in->h);
    int one_shot = (int)(in->mode & 1u);
    uint64_t t0 = 0, strips0 = 0, rows0 = 0;
    if (!region_ok(in) || !mi || !in->model || in->model_bytes != mi->bytes) return -1;
    ws = one_shot ? vjo_vocr_workspace_bytes(mi->name, in->w, in->h) : vjo_vocr_job_bytes(mi->name, in->w, in->h);
    if (!ws || ws > sizeof(workspace) || !cap || cap > sizeof(capture_block)) return -1;
    grey_plane(in);
    memset(&p, 0, sizeof(p));
    p.file_open = mem_open;
    mem_in = in;
    memset(&res, 0, sizeof(res));
    if (!one_shot) {
        memcpy(weights, in->model, in->model_bytes);
        res.model = mi;
        res.data = weights;
        res.capacity = in->model_bytes;
        res.size = in->model_bytes;
        res.stamp = 1;
    }
    for (unsigned it = 0; it <= iters; it++) {
        int rc;
        if (it == 1) { t0 = now_ns(in); strips0 = ss.strips; rows0 = ss.rows; }
        rc = one_shot ? vjo_vocr_ocr(&p, "bench", mi->name, &src, workspace, ws, capture_block, cap, out->text,
                                     sizeof(out->text), &st)
                      : vjo_vocr_ocr_resident(&p, "bench", mi->name, &src, &res, workspace, ws, capture_block, cap,
                                              out->text, sizeof(out->text), &st);
        if (rc) return -2;
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64s(0, out->text);
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
static int stage_config(const BenchIn *in, unsigned iters, BenchOut *out)
{
    static VjoConfig cfg;
    const char *text = in->config_text ? in->config_text : vjo_config_default_text();
    size_t len = strlen(text);
    uint64_t t0 = 0;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, text, len);
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64(0, &cfg.ocr_backend, sizeof(cfg.ocr_backend));
    out->check = fnv64s(out->check, cfg.vocr_model);
    out->ops = 1;
    out->c[0] = len;
    out->c[1] = (uint64_t)cfg.n_warnings;
    return 0;
}

/* Formatting a job's log lines (shell/worker.c's own format strings, with a
 * typical job's values). Only the formatting: the file writes are counted by
 * tools/bench_stages.py. */
static int stage_log(const BenchIn *in, unsigned iters, BenchOut *out)
{
    char line[256];
    uint64_t t0 = 0, bytes = 0;
    unsigned lines = 0;
    for (unsigned it = 0; it <= iters; it++) {
        if (it == 1) t0 = now_ns(in);
        bytes = lines = 0;
#define LOGF(...) do { int k_ = vjo_snprintf(line, sizeof(line) - 1, __VA_ARGS__); bytes += k_ > 0 ? (uint64_t)k_ + 1 : 0; lines++; } while (0)
        LOGF("job start (%s)%s", "auto: screen settled", "");
        LOGF("game vocr rc=%d %s%s: %ux%u at %u,%u of %ux%u fmt 0x%08X, lines=%u USER workspace=%u weights=%s resident=%u, "
             "find=%u ms load=%u ms recognize=%u ms frames=%u io=0x%08X stat=0x%08X cleanup=%d",
             0, "VOCR3_H15_w8.vocr", "", 917u, 125u, 22u, 400u, 960u, 544u, 0u, 3u, 188416u, "resident", 1159168u,
             41u, 2u, 610u, 240u, 0u, 0u, 0);
        LOGF("game vocr capture: %s into %s, %u B block, %u-row strips, %u strips in %u ms, %u rows changed, sum 0x%08X",
             "hook", "USER", 61440u, 62u, 8u, 140u, 0u, 0x1234ABCDu);
        LOGF("game vocr memory: free USER=%d, needs workspace %u + weights %u + reserve %u: check %s, headroom %d",
             91226112, 249856u, 0u, 4194304u, "passed", 86781952);
        LOGF("game vocr speed: %u frames in %u us, %u us per frame", 240u, 610000u, 2541u);
        LOGF("game vocr lines: %ux%u %uf %u.%ums %ux%u %uf %u.%ums %ux%u %uf %u.%ums", 860u, 32u, 80u, 210u, 3u, 820u,
             32u, 76u, 199u, 1u, 610u, 32u, 84u, 201u, 7u);
        for (int s = 0; s < 11; s++)
            LOGF("job step %d %s kind=%u arg=%u rc=0x%08X bytes=%u sum=0x%08X main=%d cdram=%d phycont=%d us=%u", s,
                 "alloc", 1u, 2u, 0u, 61440u, 0u, 91226112, 117440512, 0, 1234u + (unsigned)s);
        LOGF("job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d", 0, 0, 912, 41216u, 1, 1);
        LOGF("lookup %s at %u: %d chars, dictionary %s, page %d/%d, %u reads %u B, rc=%d, %u ms; \"%s\" -> %s%s%s%s%s%s%s",
             "first word", 0u, 2, "1/1", 1, 1, 15u, 61440u, 0, 12u, "朝ごはん、もう冷", "朝御飯", " (", "あさごはん", ")",
             " [", "Daijirin4 Plain", "]");
#undef LOGF
    }
    out->ns = now_ns(in) - t0;
    out->check = fnv64(0, &bytes, sizeof(bytes));
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
            if (vjo_anki_note_from_entry(&a, &cfg, &d[i].list, 0, &note)) return -2;
            json = vjo_queue_encode(&a, &note, 0);
            if (!json) return -3;
            bytes += strlen(json);
            if (it == 0) out->check = fnv64(out->check, &bytes, sizeof(bytes));
        }
    }
    out->ns = now_ns(in) - t0;
    out->ops = n;
    out->c[0] = n;
    out->c[1] = bytes;
    return 0;
}

/* The subtitle strip's readings (jiten/jpdb only): each word's surface aligned
 * with its reading (core/furigana.c). */
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
                        "rows_read", "strip_rows", "model_loaded", "model_cached"}},
    {"sha256", stage_sha256, {"bytes"}},
    {"postproc", stage_postproc, {"texts", "bytes_in", "bytes_out"}},
    {"lookup", stage_lookup, {"lookups", "entries", "opens", "reads", "read_bytes", "repeat_reads", "pages",
                              "arena_peak"}},
    {"config", stage_config, {"bytes", "warnings"}},
    {"log", stage_log, {"lines", "bytes"}},
    {"anki", stage_anki, {"cards", "json_bytes"}},
    {"furigana", stage_furigana, {"words", "bytes"}},
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
