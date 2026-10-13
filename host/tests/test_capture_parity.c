/* API 18 against API 19 (#74): the recognizer is given byte-identical input.
 *
 * API 18 copied the region of the game's frame as A8B8G8R8 into a kernel
 * buffer, and the game worker turned it into a region-sized grey plane for the
 * finder and the recognizer. API 19 has the display hook write grey rows into
 * the worker's own strip, a strip at a time, and no copy of the region exists.
 * Both pipelines here are the real code (parity_api18.c: 8228c3c's kernel
 * conversion and core, verbatim; parity_api19.c: today's kernel/capture.c and
 * core), run to vocr_run, whose input is recorded: every line's size, polarity
 * and grey bytes, and with the real weights (VJO_VOCR_MODEL_DIR) the tensor
 * vocr_preprocess makes of it. Frames are built in each SceDisplay format the
 * kernel accepts (A8B8G8R8, A1B5G5R5, A2B10G10R10).
 *
 * Procedural frames always run. VJO_PARITY_LIST names a list of eval crops
 * (tools/capture_parity.py writes it from the linefind dev manifests):
 * one "RGBA_FILE WIDTH HEIGHT X0 Y0 X1 Y1" line per region. */
#include "acutest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vocr.h"
#include "parity.h"
#include "../../include/vjo_vocr_sizes.h"

#ifndef VJO_VOCR_MODEL_DIR
#define VJO_VOCR_MODEL_DIR ""
#endif

#define FB_A8B8G8R8 0x00000000u
#define FB_A2B10G10R10 0x60800000u
#define FB_A1B5G5R5 0x50000000u
static const uint32_t formats[] = {FB_A8B8G8R8, FB_A1B5G5R5, FB_A2B10G10R10};
static const char *const format_names[] = {"A8B8G8R8", "A1B5G5R5", "A2B10G10R10"};
#define N_FORMATS 3u

const void *parity_model;
static vocr_model real_model;
static unsigned char *real_blob;

static int grow(unsigned char **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;
    size_t n = *cap ? *cap : 65536;
    while (n < need) n *= 2;
    unsigned char *p = realloc(*buf, n);
    if (!p) return -1;
    *buf = p;
    *cap = n;
    return 0;
}

void parity_reset(ParityRun *r)
{
    unsigned char *bytes = r->bytes, *tensor = r->tensor;
    size_t cap = r->cap, tcap = r->tensor_cap;
    memset(r, 0, sizeof(*r));
    r->bytes = bytes; r->cap = cap;
    r->tensor = tensor; r->tensor_cap = tcap;
}

void parity_record(ParityRun *run, const void *image)
{
    const vocr_image *img = image;
    size_t n = (size_t)img->width * img->height;
    if (!run) return;
    if (run->lines < PARITY_MAX_LINES)
        run->line[run->lines] = (ParityLine){img->width, img->height, img->format};
    run->lines++;
    if (grow(&run->bytes, &run->cap, run->used + n)) { run->rc = -999; return; }
    for (unsigned y = 0; y < img->height; y++)
        memcpy(run->bytes + run->used + (size_t)y * img->width, img->pixels + (size_t)y * img->stride, img->width);
    run->used += n;
    if (parity_model) {
        unsigned rw = 0, pw = 0;
        if (vocr_preprocess(parity_model, img, NULL, 0, &rw, &pw) != VOCR_OK) { run->rc = -998; return; }
        size_t t = (size_t)real_model.height * pw;
        if (grow(&run->tensor, &run->tensor_cap, run->tensor_used + t) ||
            vocr_preprocess(parity_model, img, run->tensor + run->tensor_used, t, &rw, &pw) != VOCR_OK) {
            run->rc = -998;
            return;
        }
        run->tensor_used += t;
    }
}

/* ---- frames ---- */
static uint32_t rng = 0x74u;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

/* RGB888 scene into a frame of format fmt with pitch pixels per row. */
static void *encode(const unsigned char *rgb, unsigned w, unsigned h, unsigned pitch, uint32_t fmt)
{
    unsigned bpp = fmt == FB_A1B5G5R5 ? 2 : 4;
    unsigned char *fb = calloc((size_t)pitch * h, bpp);
    if (!fb) return NULL;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            const unsigned char *s = rgb + ((size_t)y * w + x) * 3;
            unsigned r = s[0], g = s[1], b = s[2];
            if (fmt == FB_A1B5G5R5) {
                uint16_t v = (uint16_t)(0x8000u | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
                memcpy(fb + ((size_t)y * pitch + x) * 2, &v, 2);
            } else if (fmt == FB_A2B10G10R10) {
                uint32_t v = 3u << 30 | (b << 2 | b >> 6) << 20 | (g << 2 | g >> 6) << 10 | (r << 2 | r >> 6);
                memcpy(fb + ((size_t)y * pitch + x) * 4, &v, 4);
            } else {
                uint32_t v = 0xFF000000u | b << 16 | g << 8 | r;
                memcpy(fb + ((size_t)y * pitch + x) * 4, &v, 4);
            }
        }
    return fb;
}

static void fill(unsigned char *rgb, unsigned w, unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                 unsigned r, unsigned g, unsigned b)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) {
            unsigned char *d = rgb + ((size_t)y * w + x) * 3;
            d[0] = (unsigned char)r; d[1] = (unsigned char)g; d[2] = (unsigned char)b;
        }
}

/* A line of glyph-like cells (random strokes) of height gh, starting at x0, y0. */
static void glyphs(unsigned char *rgb, unsigned w, unsigned x0, unsigned y0, unsigned x1, unsigned gh,
                   unsigned r, unsigned g, unsigned b)
{
    for (unsigned x = x0; x + gh <= x1; x += gh + gh / 6) {
        unsigned strokes = 3 + rnd(4), t = gh / 10 ? gh / 10 : 1;
        for (unsigned k = 0; k < strokes; k++) {
            if (rnd(2)) { unsigned yy = y0 + rnd(gh - t); fill(rgb, w, x, yy, x + gh, yy + t, r, g, b); }
            else { unsigned xx = x + rnd(gh - t); fill(rgb, w, xx, y0, xx + t, y0 + gh, r, g, b); }
        }
    }
}

/* A VN-like screen: a noisy gradient, a dark textbox with light lines, a light
 * box with dark lines, and a large title line (taller than a strip). */
static unsigned char *scene(unsigned w, unsigned h)
{
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return NULL;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned char *d = rgb + ((size_t)y * w + x) * 3;
            d[0] = (unsigned char)(60 + y * 120 / h + rnd(9));
            d[1] = (unsigned char)(90 + x * 80 / w + rnd(9));
            d[2] = (unsigned char)(140 + rnd(9));
        }
    unsigned s = w / 480; /* 1 at 480x272, 2 at 960x544 */
    /* the dialogue box: three lines of 22 px (x s) light text, one yellowish */
    fill(rgb, w, 20 * s, 190 * s, w - 20 * s, h - 6 * s, 18, 20, 34);
    glyphs(rgb, w, 32 * s, 198 * s, w - 40 * s, 11 * s, 240, 240, 240);
    glyphs(rgb, w, 32 * s, 216 * s, w - 120 * s, 11 * s, 250, 236, 150);
    glyphs(rgb, w, 32 * s, 234 * s, w - 200 * s, 11 * s, 228, 228, 236);
    /* a light box with dark text */
    fill(rgb, w, 300 * s, 20 * s, 460 * s, 70 * s, 236, 232, 220);
    glyphs(rgb, w, 308 * s, 28 * s, 452 * s, 10 * s, 30, 26, 24);
    glyphs(rgb, w, 308 * s, 46 * s, 420 * s, 10 * s, 40, 36, 34);
    /* the title: one 46 px (x s) line on a dark band */
    fill(rgb, w, 10 * s, 96 * s, 250 * s, 150 * s, 10, 10, 16);
    glyphs(rgb, w, 16 * s, 100 * s, 240 * s, 46 * s, 250, 250, 250);
    return rgb;
}

/* ---- comparison ---- */
typedef struct {
    unsigned regions, lines, dark, tall, max_h;
    unsigned long long bytes, tensor_bytes;
    unsigned differences;
} Totals;

static int compare(const ParityFrame *f, const char *model, const char *what, Totals *t)
{
    static ParityRun a, b;
    parity_api18(f, model, &a);
    parity_api19(f, model, &b);
    int same = a.rc == b.rc && a.crop_x == b.crop_x && a.crop_y == b.crop_y && a.crop_w == b.crop_w &&
               a.crop_h == b.crop_h && a.lines == b.lines && a.lines <= PARITY_MAX_LINES && a.used == b.used &&
               !memcmp(a.bytes, b.bytes, a.used) && a.tensor_used == b.tensor_used &&
               !memcmp(a.tensor, b.tensor, a.tensor_used);
    for (unsigned i = 0; same && i < a.lines; i++)
        same = a.line[i].w == b.line[i].w && a.line[i].h == b.line[i].h && a.line[i].format == b.line[i].format;
    t->regions++;
    if (!same) {
        t->differences++;
        TEST_CHECK_(0, "%s: API 18 rc %d crop %u,%u %ux%u %u lines %zu B; API 19 rc %d crop %u,%u %ux%u %u lines %zu B",
                    what, a.rc, a.crop_x, a.crop_y, a.crop_w, a.crop_h, a.lines, a.used, b.rc, b.crop_x, b.crop_y,
                    b.crop_w, b.crop_h, b.lines, b.used);
        return 0;
    }
    TEST_CHECK_(a.rc == 0, "%s: rc %d", what, a.rc);
    t->lines += a.lines;
    t->bytes += a.used;
    t->tensor_bytes += a.tensor_used;
    t->tall += b.tall;
    for (unsigned i = 0; i < a.lines; i++) {
        t->dark += (a.line[i].format & VOCR_INVERT) ? 1u : 0u;
        if (a.line[i].h > t->max_h) t->max_h = a.line[i].h;
    }
    return 1;
}

static void report(const char *set, const Totals *t)
{
    fprintf(stderr, "parity %s: %u regions, %u lines (%u dark, %u taller than a strip, tallest %u rows), %llu B "
            "of recognizer input%s%llu B of preprocessed tensors, %u differences\n", set, t->regions, t->lines,
            t->dark, t->tall, t->max_h, t->bytes, parity_model ? ", " : " (no weights: tensors not compared), ",
            t->tensor_bytes, t->differences);
    TEST_MSG("%s: %u regions, %u lines, %llu bytes", set, t->regions, t->lines, t->bytes);
}

/* Pixels to 1/65536 of the frame, as the Shell stores a region (a full width is 65535). */
static uint16_t norm(unsigned v, unsigned size)
{
    uint32_t n = (uint32_t)v * 65536u / size;
    return (uint16_t)(n > 65535u ? 65535u : n);
}

static const char *const models[] = {"H15_w8.vocr", "FL10_w8.vocr",
#if VJO_VOCR2_ENABLED
                                     VJO_VOCR2_ARTIFACT_NAME,
#endif
};

static void load_weights(void)
{
    char path[512];
    FILE *f;
    long n;
    if (parity_model || !VJO_VOCR_MODEL_DIR[0]) return;
    snprintf(path, sizeof(path), "%s/H15_w8.vocr", VJO_VOCR_MODEL_DIR);
    if (!(f = fopen(path, "rb"))) return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && !posix_memalign((void **)&real_blob, 64, (size_t)n) && fread(real_blob, 1, (size_t)n, f) == (size_t)n &&
        vocr_load(&real_model, real_blob, (size_t)n) == VOCR_OK)
        parity_model = &real_model;
    fclose(f);
}

static void procedural_frames(void)
{
    static const unsigned sizes[][3] = {{960, 544, 960}, {960, 544, 1024}, {480, 272, 512}};
    Totals t;
    memset(&t, 0, sizeof(t));
    load_weights();
    for (unsigned s = 0; s < 3; s++) {
        unsigned w = sizes[s][0], h = sizes[s][1], k = w / 480;
        unsigned char *rgb;
        rng = 0x74u + s;
        rgb = scene(w, h);
        TEST_ASSERT(rgb != NULL);
        /* regions in pixels: the dialogue box, a tight textbox, the light box, the
         * title, a tiny one (grown to 32x32), an odd x at the right edge, the frame */
        const unsigned regions[][4] = {
            {20 * k, 190 * k, w - 20 * k, h - 6 * k}, {28 * k, 196 * k, w - 36 * k, 250 * k},
            {300 * k, 20 * k, 460 * k, 70 * k},      {10 * k, 96 * k, 250 * k, 150 * k},
            {200 * k, 120 * k, 205 * k, 124 * k},    {w - 151, 191 * k, w, h - 1},
            {0, 0, 0, 0},
        };
        for (unsigned fi = 0; fi < N_FORMATS; fi++) {
            void *fb = encode(rgb, w, h, sizes[s][2], formats[fi]);
            TEST_ASSERT(fb != NULL);
            for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++)
                for (unsigned m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
                    char what[160];
                    ParityFrame f = {fb, w, h, sizes[s][2], formats[fi], 0, 0, 0, 0};
                    if (regions[r][2]) {
                        f.rx = norm(regions[r][0], w); f.ry = norm(regions[r][1], h);
                        f.rw = norm(regions[r][2] - regions[r][0], w); f.rh = norm(regions[r][3] - regions[r][1], h);
                    }
                    snprintf(what, sizeof(what), "%ux%u pitch %u %s region %u %s", w, h, sizes[s][2],
                             format_names[fi], r, models[m]);
                    compare(&f, models[m], what, &t);
                }
            free(fb);
        }
        free(rgb);
    }
    report("procedural", &t);
    TEST_CHECK(t.lines > 100 && t.dark > 0 && t.tall > 0 && !t.differences);
}

/* The eval crops tools/capture_parity.py lists (linefind dev manifests only). */
static void eval_crops(void)
{
    const char *list = getenv("VJO_PARITY_LIST");
    char path[1024];
    unsigned w, h, x0, y0, x1, y1;
    Totals t[N_FORMATS];
    FILE *f;
    if (!list || !list[0]) { TEST_MSG("VJO_PARITY_LIST unset: eval crops skipped"); return; }
    TEST_ASSERT((f = fopen(list, "r")) != NULL);
    memset(t, 0, sizeof(t));
    load_weights();
    while (fscanf(f, "%1023s %u %u %u %u %u %u", path, &w, &h, &x0, &y0, &x1, &y1) == 7) {
        FILE *img = fopen(path, "rb");
        unsigned char *rgba = malloc((size_t)w * h * 4), *rgb = malloc((size_t)w * h * 3);
        TEST_ASSERT(img && rgba && rgb && w <= 960 && h <= 544 && x1 > x0 && y1 > y0 && x1 <= w && y1 <= h);
        TEST_ASSERT(fread(rgba, 1, (size_t)w * h * 4, img) == (size_t)w * h * 4);
        fclose(img);
        for (size_t i = 0; i < (size_t)w * h; i++) memcpy(rgb + i * 3, rgba + i * 4, 3);
        for (unsigned fi = 0; fi < N_FORMATS; fi++) {
            void *fb = encode(rgb, w, h, w, formats[fi]);
            ParityFrame fr = {fb, w, h, w, formats[fi], norm(x0, w), norm(y0, h), norm(x1 - x0, w), norm(y1 - y0, h)};
            char what[1200];
            TEST_ASSERT(fb != NULL);
            snprintf(what, sizeof(what), "%s %s region %u,%u-%u,%u", path, format_names[fi], x0, y0, x1, y1);
            compare(&fr, "H15_w8.vocr", what, &t[fi]);
            free(fb);
        }
        free(rgb);
        free(rgba);
    }
    fclose(f);
    for (unsigned fi = 0; fi < N_FORMATS; fi++) {
        char set[64];
        snprintf(set, sizeof(set), "eval %s", format_names[fi]);
        report(set, &t[fi]);
        TEST_CHECK(t[fi].regions > 0 && !t[fi].differences);
    }
}

TEST_LIST = {
    {"procedural_frames_give_identical_recognizer_input", procedural_frames},
    {"eval_crops_give_identical_recognizer_input", eval_crops},
    {NULL, NULL}
};
