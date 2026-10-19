/* Vertical text (vita-vn-ocr #99; core/vocr_ocr.h VJO_VOCR_FINDER_COLUMNS) through the actual core
 * adapter, with real vocr_run and vocr_lm_decode calls on tiny weights blobs and a tiny LM the test
 * builds and pins itself (test_vertical_models.h; vita-vn-ocr runtime/tests' builders, as
 * test_vocr_parallel.c's), so it runs where the release files are absent (CI):
 *  - a job on a model of columns gives the columns' texts right to left, '\n' between them: each one is
 *    what vocr_run reads (greedy, or decoded with the LM) from the column turned 90 degrees
 *    anticlockwise (vocr_column_rotate) with the column's polarity; the columns are the column finder's
 *    (vocr_find_columns, P0 and P1) on the whole region; each line's box and stats are the column's on the
 *    screen and its pixels (the line callback) the turned crop. Light and dark text; a region inside one
 *    strip, and one whose columns are taller than a strip (read through the line buffer);
 *  - with helpers and with the line cache the same text; a second job reads every column from the cache;
 *  - the rotation buffer: a column larger than VJO_VOCR_ROTATE_BYTES is left out, never read, and counted,
 *    and the columns beside it are read;
 *  - the steps for rows never run on columns: no short-line probe, refinement or cascade, whatever the
 *    policy asks (and no workspace for them);
 *  - off: a job on the model of rows gives today's job exactly. Its text, greedy text, frames, every
 *    line's stats, the workspace and the strips read hash to the figures this file's off part gives when
 *    built against main 1de7b67 (before vertical text; VJO_TEST_OFF_ONLY, which leaves the rest out), on a
 *    5-line region and on host/fixtures/short_line/textbox.pgm (#194's box), greedy and with the LM.
 * This makes no claim about accuracy: the tiny models read nothing real. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "platform_posix.h"
#include "vocr.h"
#include "vocr_lm.h"
#include "vocr_lines.h"
#include "vocr_ocr.h"
#ifndef VJO_TEST_OFF_ONLY
#include "vocr_vertical.h"
#endif
#include "../../core/fbgrey.h"
#include <bearssl_hash.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_pin_model(unsigned which, uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes);
void test_pin_lm(uint32_t bytes, const unsigned char sha256[32]);
#ifndef VJO_TEST_OFF_ONLY
uint32_t vjo_vocr_test_line_length(unsigned w, unsigned h, int cols);
void vjo_vocr_test_gate_columns(int on);
#endif
#ifndef VJO_VOCR_GATE_TEST_DIR
#define VJO_VOCR_GATE_TEST_DIR "" /* the text-presence gate's weights (cmake/VocrGate.cmake), "" without them */
#endif

#define M_ROWS "TINY_w8.vocr"
#define M_COLS "TINYV_w8.vocr"
#define M_COLS1 "TINYV1_w8.vocr"
#define M_COLSD "TINYVD_w8.vocr" /* P1 with the finder's own dark flags (no VOCR_COLUMNS_NO_DARK) */
#define M_COLSG "TINYVG_w8.vocr" /* M_COLS with scratch room for the text-presence gate's arena */

/* ------------------------------------------------------------ the tiny blobs
 * (vita-vn-ocr runtime/tests/test_vocr.c's builder: height 8, stride 4) */
static unsigned char blob[1 << 16] __attribute__((aligned(16)));
static size_t blob_pos;
static uint32_t lcg = 12345u;
enum { CLASSES = 12 };

static float frand(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (float)((lcg >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t put(const void *data, size_t n)
{
    uint32_t off = (uint32_t)blob_pos;
    memcpy(blob + blob_pos, data, n);
    blob_pos = (blob_pos + n + 15u) & ~(size_t)15u;
    return off;
}
static uint32_t put_floats(unsigned n, float scale)
{
    uint32_t off = (uint32_t)blob_pos;
    for (unsigned i = 0; i < n; i++) {
        float v = frand() * scale;
        memcpy(blob + blob_pos + 4u * i, &v, sizeof v);
    }
    blob_pos = (blob_pos + 4u * n + 15u) & ~(size_t)15u;
    return off;
}
static uint32_t crc32_ref(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void op_rec(unsigned i, unsigned type, unsigned flags, unsigned kh, unsigned kw, unsigned sh, unsigned sw,
                   unsigned ph, unsigned pw, unsigned cin, unsigned cout, unsigned hin, unsigned hout, uint32_t w_off,
                   uint32_t w_bytes, uint32_t b_off, uint32_t s_off, unsigned k, unsigned cout_pad)
{
    unsigned char *r = blob + 64 + 64 * i;
    memset(r, 0, 64);
    r[0] = (unsigned char)type; r[1] = (unsigned char)flags; r[2] = (unsigned char)kh; r[3] = (unsigned char)kw;
    r[4] = (unsigned char)sh; r[5] = (unsigned char)sw; r[6] = (unsigned char)ph; r[7] = (unsigned char)pw;
    wr16(r + 8, cin); wr16(r + 10, cout); wr16(r + 12, hin); wr16(r + 14, hout);
    wr32(r + 16, w_off); wr32(r + 20, w_bytes); wr32(r + 24, b_off); wr32(r + 28, s_off);
    wr16(r + 32, k); wr16(r + 34, cout_pad);
}
static void mat_op(unsigned i, unsigned type, unsigned flags, unsigned kh, unsigned cin, unsigned cout, unsigned hin,
                   unsigned hout)
{
    unsigned k = kh * cin, cout_pad = (cout + 7u) & ~7u, n = cout_pad * k;
    uint32_t w_off = (uint32_t)blob_pos, s_off, b_off = 0;
    for (unsigned j = 0; j < n; j++) blob[blob_pos + j] = (unsigned char)(signed char)(int)(frand() * 127.0f);
    blob_pos = (blob_pos + n + 15u) & ~(size_t)15u;
    s_off = (uint32_t)blob_pos;
    for (unsigned j = 0; j < cout_pad; j++) {
        float sc = 0.01f + 0.004f * (float)(j % 5);
        memcpy(blob + blob_pos + 4u * j, &sc, sizeof sc);
    }
    blob_pos = (blob_pos + 4u * cout_pad + 15u) & ~(size_t)15u;
    if (flags & 4) b_off = put_floats(cout_pad, 0.1f);
    op_rec(i, type, flags | 8, kh, 1, 1, 1, 0, 0, cin, cout, hin, hout, w_off, n, b_off, s_off, k, cout_pad);
}
/* classes: 0 blank, 1-11 'a'..'k' (the tiny LM knows a-d) */
static size_t build_blob(uint32_t seed)
{
    unsigned char vocab[2 * (CLASSES + 1) + CLASSES];
    unsigned pos = 0, n_ops = 7;
    uint32_t off;
    memset(blob, 0, sizeof blob);
    blob_pos = 64 + 64 * n_ops;
    lcg = seed;
    off = put_floats(9 * 8, 0.6f);
    op_rec(0, 1, 1 | 4, 3, 3, 2, 2, 1, 1, 1, 8, 8, 4, off, 9 * 8 * 4, put_floats(8, 0.1f), 0, 9, 8);
    off = put_floats(9 * 8, 0.6f);
    op_rec(1, 2, 1 | 4, 3, 3, 2, 2, 1, 1, 8, 8, 4, 2, off, 9 * 8 * 4, put_floats(8, 0.1f), 0, 9, 8);
    mat_op(2, 3, 1 | 4, 1, 8, 16, 2, 2);
    mat_op(3, 3, 1 | 4 | 16, 2, 16, 16, 2, 1);
    off = put_floats(5 * 16, 0.6f);
    op_rec(4, 2, 1 | 4, 1, 5, 1, 1, 0, 2, 16, 16, 1, 1, off, 5 * 16 * 4, put_floats(16, 0.1f), 0, 5, 16);
    mat_op(5, 3, 1 | 2 | 4, 1, 16, 16, 1, 1);
    mat_op(6, 4, 4, 1, 16, CLASSES, 1, 1);
    wr16(vocab, 0);
    wr16(vocab + 2, 0);
    for (unsigned i = 1; i < CLASSES; i++) {
        vocab[2 * (CLASSES + 1) + pos++] = (unsigned char)('a' + i - 1);
        wr16(vocab + 2 * (i + 1), pos);
    }
    off = put(vocab, 2 * (CLASSES + 1) + pos);
    memcpy(blob, "VOCR", 4);
    wr16(blob + 4, 1); wr16(blob + 6, 64); wr32(blob + 8, (uint32_t)blob_pos); wr32(blob + 12, 1u);
    wr16(blob + 16, 8); wr16(blob + 18, 256); wr16(blob + 20, 4); wr16(blob + 22, n_ops);
    wr32(blob + 24, CLASSES); wr32(blob + 28, 64); wr32(blob + 32, off); wr32(blob + 36, pos);
    wr32(blob + 40, crc32_ref(blob + 64, blob_pos - 64));
    return blob_pos;
}

/* ------------------------------------------------------------ the tiny LM
 * (vita-vn-ocr runtime/tests/test_vocr_lm.c's: order 3; tokens 0 <unk>, 1 <s>,
 * 2 </s>, 3-6 a b c d; bigrams (<s> a) (a b) (a c) (b c); trigram (a b c)) */
static unsigned char lmb[4096] __attribute__((aligned(16)));
static size_t lpos;
static uint32_t lput(const void *d, size_t n)
{
    uint32_t off = (uint32_t)lpos;
    memcpy(lmb + lpos, d, n);
    lpos = (lpos + n + 15u) & ~(size_t)15u;
    return off;
}
static void lrow(unsigned k, uint32_t n, uint32_t ot, uint32_t op, uint32_t ob, uint32_t oc, uint32_t ocp, uint32_t ocb)
{
    unsigned char *r = lmb + 64 + 32 * (k - 1);
    wr32(r, n); wr32(r + 4, ot); wr32(r + 8, op); wr32(r + 12, ob); wr32(r + 16, oc); wr32(r + 20, ocp); wr32(r + 24, ocb);
}
static size_t build_lm(void)
{
    static const uint32_t cps[4] = {0x61, 0x62, 0x63, 0x64};
    static const uint8_t p1[7] = {10, 20, 4, 2, 3, 3, 5}, b1[7] = {0, 3, 0, 1, 2, 0, 0};
    static const uint32_t c1[8] = {0, 0, 1, 1, 3, 4, 4, 4};
    static const uint16_t t2[4] = {3, 4, 5, 5};
    static const uint8_t p2[4] = {1, 2, 3, 1}, b2[4] = {0, 1, 0, 0};
    static const uint32_t c2[5] = {0, 0, 1, 1, 1};
    static const uint16_t t3[1] = {5};
    static const uint8_t p3[1] = {7};
    int16_t cb1[256], cbb1[256], cb2[256], cbb2[256], cb3[256];
    uint32_t o[16];
    memset(lmb, 0, sizeof lmb);
    for (unsigned i = 0; i < 256; i++) {
        cb1[i] = (int16_t)(-64 * (int)i);
        cb2[i] = (int16_t)(-50 * (int)i);
        cb3[i] = (int16_t)(-10 * (int)i);
        cbb1[i] = cbb2[i] = 0;
    }
    cbb1[1] = -100; cbb1[2] = -200; cbb1[3] = 50; cbb2[1] = -300;
    lpos = 64 + 6 * 32;
    o[0] = lput(p1, 7); o[1] = lput(b1, 7); o[2] = lput(c1, sizeof c1); o[3] = lput(cb1, sizeof cb1);
    o[4] = lput(cbb1, sizeof cbb1); o[5] = lput(t2, sizeof t2); o[6] = lput(p2, 4); o[7] = lput(b2, 4);
    o[8] = lput(c2, sizeof c2); o[9] = lput(cb2, sizeof cb2); o[10] = lput(cbb2, sizeof cbb2); o[11] = lput(t3, sizeof t3);
    o[12] = lput(p3, 1); o[13] = lput(cb3, sizeof cb3); o[14] = lput(cps, sizeof cps);
    lrow(1, 7, 0, o[0], o[1], o[2], o[3], o[4]);
    lrow(2, 4, o[5], o[6], o[7], o[8], o[9], o[10]);
    lrow(3, 1, o[11], o[12], 0, 0, o[13], 0);
    memcpy(lmb, "VOLM", 4);
    wr32(lmb + 4, 1); wr32(lmb + 8, (uint32_t)lpos); wr32(lmb + 12, 3); wr32(lmb + 16, 7); wr32(lmb + 28, o[14]);
    wr32(lmb + 24, crc32_ref(lmb + 64, lpos - 64));
    return lpos;
}

static char model_dir[256]; /* the product joins it and a name in 256 bytes (core/vocr_ocr.c join_path) */
/* The tiny models parsed, for the reference readings (the job reads its own copy of the file). */
static unsigned char ref_blob[2][1 << 16] __attribute__((aligned(16)));
static size_t ref_bytes[2];
static unsigned char ref_lm[4096] __attribute__((aligned(16)));
static size_t ref_lm_bytes;

static void write_file(const char *name, const void *data, size_t n, unsigned char sha[32])
{
    char path[300];
    br_sha256_context h;
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", model_dir, name);
    f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, n, f) == n);
    fclose(f);
    br_sha256_init(&h);
    br_sha256_update(&h, data, n);
    br_sha256_out(&h, sha);
}

/* The blobs and the LM in files, pinned in the tables: the model of rows is seed 12345's blob (the
 * vertical models seed 777's: one blob under both of their names, each pinned). */
static void setup_files(void)
{
    static int done;
    static const uint32_t seeds[2] = {12345u, 777u};
    unsigned char sha[32];
    vocr_model m;
    vocr_lm lm;
    vocr_opts o;
    size_t n;
    if (done) return;
    TEST_ASSERT_(vjo_test_tmpdir_new(model_dir, sizeof model_dir) != NULL && strlen(model_dir) + 20 < 256,
                 "no models' directory in %s with room for the product's 256-byte model path", vjo_test_tmpdir_path);
    for (unsigned i = 0; i < 2; i++) {
        n = build_blob(seeds[i]);
        TEST_ASSERT(vocr_load(&m, blob, n) == VOCR_OK);
        memset(&o, 0, sizeof o);
        o.mode = VOCR_MODE_W8I;
        memcpy(ref_blob[i], blob, n);
        ref_bytes[i] = n;
        if (!i) {
            write_file(M_ROWS, blob, n, sha);
            test_pin_model(0, (uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o));
        }
#ifndef VJO_TEST_OFF_ONLY
        else {
            write_file(M_COLS, blob, n, sha);
            test_pin_model(1, (uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o));
            write_file(M_COLS1, blob, n, sha);
            test_pin_model(2, (uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o));
            write_file(M_COLSD, blob, n, sha);
            test_pin_model(3, (uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o));
            write_file(M_COLSG, blob, n, sha); /* 256 KiB more arena: the gate's fits beside the finder's */
            test_pin_model(4, (uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o) + 262144u);
        }
#endif
    }
    n = build_lm();
    TEST_ASSERT(vocr_lm_load(&lm, lmb, n) == VOCR_OK);
    memcpy(ref_lm, lmb, n);
    ref_lm_bytes = n;
    write_file("TINY_lm.vlm", lmb, n, sha);
    test_pin_lm((uint32_t)n, sha);
    done = 1;
}

/* ------------------------------------------------------------ regions */
#define RW 300u
#define RH 220u
#define PIX_MAX (300u * 544u)    /* the largest region drawn or read here, in pixels */
static unsigned char pixels[PIX_MAX * 4];
static unsigned rw, rh;          /* the region in pixels (PIX_MAX at most) */
static unsigned strips_read;

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud; (void)flags;
    if (y + n > rh) return VJO_E_SOURCE;
    strips_read++;
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < rw; x++) {
            const unsigned char *p = pixels + ((size_t)(y + r) * rw + x) * 4;
            dst[(size_t)r * stride + x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
        }
    return (int)n;
}
static unsigned char *line_mem;
static void *line_buffer(void *ud, size_t bytes)
{
    (void)ud;
    if (!line_mem) line_mem = malloc(VJO_VOCR_LINE_MAX_BYTES);
    return bytes <= VJO_VOCR_LINE_MAX_BYTES ? line_mem : NULL;
}
static VjoGreySource im = {NULL, 0, 0, strip, NULL, line_buffer};
static unsigned char capture[64 * 1024];

static void fill(unsigned w, unsigned h, unsigned char bg)
{
    TEST_ASSERT((size_t)w * h <= PIX_MAX);
    rw = w;
    rh = h;
    im.width = w;
    im.height = h;
    for (size_t i = 0; i < (size_t)w * h * 4; i += 4) {
        pixels[i] = pixels[i + 1] = pixels[i + 2] = bg;
        pixels[i + 3] = 255;
    }
}
static void dot(unsigned x, unsigned y, unsigned char v)
{
    unsigned char *p;
    if (x >= rw || y >= rh) return;
    p = pixels + ((size_t)y * rw + x) * 4;
    p[0] = p[1] = p[2] = v;
}

/* LINES lines of glyph-like strokes, each with its own spacing and length (test_vocr_parallel.c's). */
#define LINES 5
static void draw_rows(void)
{
    fill(420, 200, 20);
    for (unsigned l = 0; l < LINES; l++) {
        unsigned y0 = 12 + l * 38, step = 5 + l, len = 420 - 30 - 40 * l;
        for (unsigned y = y0; y < y0 + 24; y++)
            for (unsigned x = 15; x < len; x += step)
                for (unsigned k = 0; k < 2 + (x / 7 + l) % 2; k++)
                    dot(x + k, y, (unsigned char)(200 + 7 * ((x / 11 + y) % 5)));
    }
}

/* One row of tall strokes across a 960 x 170 region: a line of about 940 x 66 px, larger than a column's
 * rotation buffer and taller than a strip (read through the line buffer): a row line no column bound may
 * touch (audit-vertical-rubi M7). */
static void draw_big_row(void)
{
    fill(960, 170, 20);
    for (unsigned y = 50; y < 110; y++)
        for (unsigned x = 15; x < 945; x += 6)
            for (unsigned k = 0; k < 2 + (x / 7) % 2; k++)
                dot(x + k, y, (unsigned char)(200 + 7 * ((x / 11 + y) % 5)));
}

#ifndef VJO_TEST_OFF_ONLY
/* A column of `glyphs` glyph cells of `cell` px from (x0, y0) down, `pitch` apart: each cell holds
 * horizontal bars every few rows across most of it (so that every pixel column of the cell has ink a few
 * rows apart, as text has) and a vertical stroke or two, its shapes seeded by (seed, glyph). */
static void column(unsigned x0, unsigned y0, unsigned glyphs, unsigned cell, unsigned pitch, unsigned seed,
                   unsigned char ink)
{
    for (unsigned g = 0; g < glyphs; g++) {
        unsigned s = seed * 131u + g * 29u, cy = y0 + g * pitch;
        for (unsigned b = 0; b < cell; b += 3u + (s + b) % 3u) { /* the bars */
            unsigned from = (s * 7u + b) % (cell / 4u + 1u), to = cell - (s * 3u + b * 5u) % (cell / 4u + 1u);
            for (unsigned x = from; x < to; x++) dot(x0 + x, cy + b, ink);
        }
        for (unsigned v = 0; v < 1u + s % 2u; v++) { /* the strokes */
            unsigned x = (s * 5u + v * 9u) % cell;
            for (unsigned y = (s + v) % (cell / 3u + 1u); y < cell; y++) {
                dot(x0 + x, cy + y, ink);
                dot(x0 + x + 1u, cy + y, ink);
            }
        }
    }
}

/* `n` columns, right to left in reading order, from x0 leftwards by `gap` + the cell. */
static void draw_columns(unsigned w, unsigned h, unsigned n, const unsigned *glyphs, unsigned cell, unsigned pitch,
                         unsigned char bg, unsigned char ink)
{
    fill(w, h, bg);
    for (unsigned c = 0; c < n; c++)
        column(w - 20u - (c + 1u) * (cell + 18u), 8, glyphs[c], cell, pitch, c + 1u, ink);
}
#endif

/* ------------------------------------------------------------ jobs */
static void *res_mem, *cres_mem, *lm_mem;
static void *alloc16(void **mem, size_t n)
{
    free(*mem);
    if (posix_memalign(mem, 16, n ? n : 16u)) *mem = NULL;
    return *mem;
}
static void *res_alloc(void *ud, size_t n) { (void)ud; return alloc16(&res_mem, n); }
static int res_release(void *ud) { (void)ud; free(res_mem); res_mem = NULL; return 0; }
static void *cres_alloc(void *ud, size_t n) { (void)ud; return alloc16(&cres_mem, n); }
static int cres_release(void *ud) { (void)ud; free(cres_mem); cres_mem = NULL; return 0; }
static void *lm_alloc(void *ud, size_t n) { (void)ud; return alloc16(&lm_mem, n); }
static int lm_release(void *ud) { (void)ud; free(lm_mem); lm_mem = NULL; return 0; }
static VjoVocrResident res = {res_alloc, res_release, NULL, NULL, NULL, 0, 0, 0};
static VjoVocrResident cres = {cres_alloc, cres_release, NULL, NULL, NULL, 0, 0, 0}; /* a cascade's */
static VjoVocrLmResident lres = {lm_alloc, lm_release, NULL, NULL, NULL, 0, 0, 0};

/* What the line callback saw, in order. */
#define MAXL VJO_VOCR_MAX_LINES
typedef struct {
    unsigned n;
    unsigned x[MAXL], y[MAXL], w[MAXL], h[MAXL], dark[MAXL];
    uint64_t pix[MAXL];  /* FNV-1a of the line's pixels as the callback gives them */
    char text[MAXL][128];
} Seen;
static Seen seen;
#ifndef VJO_TEST_OFF_ONLY
static unsigned seen_ph; /* 1: the callback's pixels are a column's turned crop (h bytes x w rows) */
#endif

static uint64_t fnv(uint64_t k, const void *p, size_t n)
{
    const unsigned char *b = p;
    if (!k) k = 0xcbf29ce484222325ull;
    while (n--) { k ^= *b++; k *= 0x100000001b3ull; }
    return k;
}

static void on_line(void *ud, const VjoVocrLineInfo *info)
{
    unsigned i = seen.n, pw = info->w, ph = info->h;
    uint64_t k = 0;
    (void)ud;
    if (i >= MAXL) return;
    seen.x[i] = info->x; seen.y[i] = info->y; seen.w[i] = info->w; seen.h[i] = info->h; seen.dark[i] = info->dark;
#ifndef VJO_TEST_OFF_ONLY
    if (seen_ph) { pw = info->h; ph = info->w; } /* a column: its turned crop, w rows of h bytes */
#endif
    for (unsigned r = 0; r < ph; r++) k = fnv(k, info->pixels + (size_t)r * info->stride, pw);
    seen.pix[i] = k;
    snprintf(seen.text[i], sizeof(seen.text[i]), "%s", info->text);
    seen.n++;
}

typedef struct {
    int lm, line, helpers, cache, refine, short_lines, cascade, gate;
} JobOpts;

/* A pool of real threads (VjoVocrPool): one per line a helper runs, joined. */
typedef struct {
    pthread_t t[1 + VJO_VOCR_HELPERS_MAX];
    void (*fn[1 + VJO_VOCR_HELPERS_MAX])(void *);
    void *arg[1 + VJO_VOCR_HELPERS_MAX];
    unsigned runs;
} Pool;
static Pool pool_state;
static void *pool_thread(void *p)
{
    unsigned slot = (unsigned)(uintptr_t)p;
    pool_state.fn[slot](pool_state.arg[slot]);
    return NULL;
}
static int pool_run(void *ud, unsigned slot, void (*fn)(void *), void *arg)
{
    (void)ud;
    pool_state.fn[slot] = fn;
    pool_state.arg[slot] = arg;
    pool_state.runs++;
    return pthread_create(&pool_state.t[slot], NULL, pool_thread, (void *)(uintptr_t)slot) ? -1 : 0;
}
static int pool_join(void *ud, unsigned slot)
{
    (void)ud;
    return pthread_join(pool_state.t[slot], NULL) ? -1 : 0;
}
static VjoVocrCache cache;

static int job(const char *model, const JobOpts *o, char *text, size_t cap, char *raw, size_t raw_cap,
               VjoVocrStats *st)
{
    VjoPlatform platform;
    PosixPlatform pp = {0};
    VjoVocrPool pool = {0, pool_run, pool_join, NULL, NULL};
    VjoVocrPolicy pol;
    size_t ws_bytes;
    void *ws = NULL;
    int rc;
    posix_platform_init(&pp, &platform);
    memset(&pol, 0, sizeof pol);
    memset(&seen, 0, sizeof seen);
    if (o->line) pol.line = on_line;
    if (o->lm) {
        pol.lm = vjo_vocr_lm_tier(1);
        pol.lm_res = &lres;
        pol.raw = raw;
        pol.raw_cap = raw_cap;
    }
    if (o->helpers) {
        pool.helpers = (unsigned)o->helpers;
        pol.pool = &pool;
    }
    if (o->cache) {
        pol.cache = &cache;
        pol.cache_variant = (uint32_t)o->lm;
    }
    pol.refine = o->refine ? VJO_VOCR_REFINE_ALL : 0u;
    pol.short_lines = o->short_lines;
#ifndef VJO_TEST_OFF_ONLY
    static VjoVocrGateResident gate_res; /* its weights: from the models' directory when a gate test put them there
                                           * (put_gate_file); without them the gate cannot decide */
    if (o->gate) {
        memset(&gate_res, 0, sizeof gate_res);
        pol.gate = 1;
        pol.gate_res = &gate_res;
    }
#endif
    if (o->cascade) { /* the model of rows reads doubtful lines again: never a model of columns' */
        pol.cascade = vjo_vocr_model(M_ROWS);
        pol.cascade_res = &cres;
        pol.cascade_us = 4000000u;
        pol.cascade_conf = 1001u;
    }
    /* the job's whole workspace, as the game worker gives it: the job, the refinement's arena, the LM and
     * cascade extras, the helpers' line slots (each part only where asked) */
    ws_bytes = vjo_vocr_job_bytes(model, rw, rh) + (o->refine ? vjo_vocr_refine_bytes(rw, rh) : 0) +
               (o->lm || o->cascade ? vjo_vocr_lm_job_bytes(model) : 0) +
               (o->cascade ? vjo_vocr_cascade_job_bytes(M_ROWS) : 0) +
               (o->helpers ? vjo_vocr_helpers_bytes(model, (unsigned)o->helpers) : 0);
    TEST_ASSERT(ws_bytes && posix_memalign(&ws, 16, ws_bytes) == 0);
    memset(ws, 0xA5, ws_bytes);
    strips_read = 0;
    rc = vjo_vocr_ocr_policy(&platform, model_dir, model, &im, &res, ws, ws_bytes, capture, sizeof capture, &pol,
                             text, cap, st);
    free(ws);
    return rc;
}

/* A hash of everything a job gives that does not depend on time: its text, its greedy text, its counts and
 * every line's stats (crop, frames, flags, cache: not the helper that ran it), its workspace and the strips
 * it read. */
static uint64_t job_hash(int rc, const char *text, const char *raw, const VjoVocrStats *st)
{
    uint64_t k = fnv(0, &rc, sizeof rc);
    const unsigned counts[] = {st->lines, st->dark, st->truncated, st->frames, st->recognized, st->blank,
                               st->lm_lines, st->lm_changed, st->strips, st->tears, st->tries, st->tall,
                               st->tall_skipped, (unsigned)st->line_bytes, st->region_sum,
                               (unsigned)st->workspace, (unsigned)st->capture, (unsigned)st->scratch_bytes,
                               st->strip_rows, st->refine_boxes, st->refine_lines, st->cascade_lines,
                               st->cached, st->model_loaded, st->model_cached};
    k = fnv(k, text, strlen(text) + 1);
    if (raw) k = fnv(k, raw, strlen(raw) + 1);
    k = fnv(k, counts, sizeof counts);
    for (unsigned i = 0; i < st->recognized && i < VJO_VOCR_MAX_LINES; i++) {
        const VjoVocrLineStat *l = &st->line[i];
        const unsigned f[] = {l->width, l->height, l->frames, l->flags, l->cached};
        k = fnv(k, f, sizeof f);
    }
    return k;
}

/* ------------------------------------------------------------ off: today's job of rows */
/* The figures main 1de7b67's core gives (this file with -DVJO_TEST_OFF_ONLY, built against that tree and
 * the runtime it pins, vita-vn-ocr d214a05; and on this tree's runtime, the same): each job's hash. */
static const struct {
    const char *what;
    uint64_t hash;
} golden[] = {
    {"5 lines greedy", 0xcd35c9240432349bull},
    {"5 lines lm", 0xe6e8a12f12f952b9ull},
    {"5 lines lm, 2 helpers", 0xe5d5add21606ed7bull},
    {"short line greedy", 0x94ef9049dc5ba6c1ull},
    {"short line lm, refined, short lines", 0xbb7cd64c103607fbull},
    {"one big row greedy", 0x6d6657eecf01de6dull},
};

static int load_pgm(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned w, h, maxv;
    if (!f) return -1;
    if (fscanf(f, "P5 %u %u %u", &w, &h, &maxv) != 3 || maxv != 255 || fgetc(f) == EOF || !w || !h ||
        (size_t)w * h > PIX_MAX) {
        fclose(f);
        return -1;
    }
    rw = w;
    rh = h;
    im.width = w;
    im.height = h;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return -1; }
        pixels[4 * i] = pixels[4 * i + 1] = pixels[4 * i + 2] = (unsigned char)c;
        pixels[4 * i + 3] = 255;
    }
    fclose(f);
    return 0;
}

static void test_off_is_todays_job(void)
{
    static const JobOpts opts[] = {{0}, {.lm = 1}, {.lm = 1, .helpers = 2}, {0}, {.lm = 1, .refine = 1, .short_lines = 1},
                                   {0}};
    char text[VJO_OCR_TEXT_CAP], raw[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
    int bad = 0;
    setup_files();
    for (unsigned i = 0; i < sizeof golden / sizeof golden[0]; i++) {
        int rc;
        uint64_t h;
        if (i < 3) draw_rows();
        else if (i < 5) TEST_ASSERT(load_pgm(VJO_SOURCE_DIR "/host/fixtures/short_line/textbox.pgm") == 0);
        else draw_big_row();
        raw[0] = '\0';
        rc = job(M_ROWS, &opts[i], text, sizeof text, opts[i].lm ? raw : NULL, sizeof raw, &st);
        h = job_hash(rc, text, opts[i].lm ? raw : NULL, &st);
        printf("off %-36s hash 0x%016llx rc=%d lines=%u frames=%u workspace=%zu text=\"%s\"\n", golden[i].what,
               (unsigned long long)h, rc, st.lines, st.frames, st.workspace, text);
#ifndef VJO_TEST_OFF_ONLY
        TEST_CHECK_(h == golden[i].hash, "%s: hash 0x%016llx, main's 0x%016llx", golden[i].what, (unsigned long long)h,
                    (unsigned long long)golden[i].hash);
        TEST_CHECK(!st.columns && !st.rotate_skipped && rc == VJO_OK && st.lines >= 1);
        if (i == 5) { /* the case holds a row line larger than the rotation buffer, read whole */
            unsigned big = 0;
            for (unsigned k = 0; k < st.recognized && k < VJO_VOCR_MAX_LINES; k++)
                big += (size_t)st.line[k].width * st.line[k].height > VJO_VOCR_ROTATE_BYTES;
            TEST_CHECK_(big >= 1 && st.tall >= 1 && !st.tall_skipped, "%u lines over %u B, %u tall", big,
                        (unsigned)VJO_VOCR_ROTATE_BYTES, st.tall);
        }
#endif
        bad += rc != VJO_OK;
    }
    TEST_CHECK(!bad);
}

#ifndef VJO_TEST_OFF_ONLY
/* ------------------------------------------------------------ columns */
/* The reference: the column finder on the whole region (in memory), each column cropped, turned
 * (vocr_column_rotate) and read by vocr_run with the column's polarity, greedy or decoded with the LM;
 * the texts joined as the job joins lines (a column read as empty adds no separator). */
typedef struct {
    VocrLineSet set;
    char text[MAXL][256];
    uint64_t pix[MAXL];
    unsigned frames[MAXL];
    char joined[VJO_OCR_TEXT_CAP];
} Ref;

static void reference(unsigned flags, int with_lm, Ref *ref)
{
    static unsigned char grey[PIX_MAX], rot[VJO_VOCR_ROTATE_BYTES], arena[1 << 20] __attribute__((aligned(16)));
    static unsigned char lm_ws[VJO_VOCR_LM_WS_BYTES] __attribute__((aligned(16)));
    VocrLinesImage gim = {grey, rw, rh, rw, VOCR_LINES_GREY};
    vocr_model m;
    vocr_ctx ctx;
    vocr_opts o;
    vocr_lm lm;
    vocr_lm_ctx lc;
    vocr_lm_opts lo;
    size_t used = 0;
    memset(ref, 0, sizeof *ref);
    for (size_t i = 0; i < (size_t)rw * rh; i++)
        grey[i] = (unsigned char)vjo_grey(pixels[4 * i], pixels[4 * i + 1], pixels[4 * i + 2]);
    TEST_ASSERT(vocr_columns_arena_bytes(rw, rh) <= sizeof arena);
    TEST_ASSERT(vocr_find_columns(&gim, flags, arena, sizeof arena, &ref->set) == VOCR_LINES_OK);
    TEST_ASSERT(vocr_load(&m, ref_blob[1], ref_bytes[1]) == VOCR_OK);
    memset(&o, 0, sizeof o);
    o.mode = VOCR_MODE_W8I;
    TEST_ASSERT(vocr_arena_bytes(&m, &o) <= sizeof arena);
    TEST_ASSERT(vocr_init(&ctx, &m, &o, arena, vocr_arena_bytes(&m, &o)) == VOCR_OK);
    if (with_lm) { /* core/vocr_ocr.c lm_opts: the decoder's settings for every model */
        TEST_ASSERT(vocr_lm_load(&lm, ref_lm, ref_lm_bytes) == VOCR_OK);
        memset(&lo, 0, sizeof lo);
        lo.beam = 8; lo.topk = VJO_VOCR_LM_TOPK; lo.alpha = 500; lo.beta = 3000; lo.gate = 8000;
        lo.flags = VOCR_LM_EOS; lo.bracket = 8000;
        TEST_ASSERT(vocr_lm_workspace_bytes(&m, &lm, &lo) <= sizeof lm_ws);
        TEST_ASSERT(vocr_lm_init(&lc, &m, &lm, &lo, lm_ws, vocr_lm_workspace_bytes(&m, &lm, &lo)) == VOCR_OK);
    }
    for (unsigned i = 0; i < ref->set.count; i++) {
        const VocrLine *c = &ref->set.lines[i];
        static float tv[VJO_VOCR_LM_FRAMES * VJO_VOCR_LM_TOPK], bl[VJO_VOCR_LM_FRAMES];
        static uint16_t tc[VJO_VOCR_LM_FRAMES * VJO_VOCR_LM_TOPK];
        vocr_image img;
        vocr_extra ex;
        char greedy[256];
        uint64_t k = 0;
        if ((size_t)c->w * c->h > sizeof rot) continue; /* the job leaves it out too (no reading) */
        TEST_ASSERT(vocr_column_rotate(grey + (size_t)c->y * rw + c->x, rw, VOCR_LINES_GREY, c->w, c->h, rot, c->h) ==
                    VOCR_LINES_OK);
        for (unsigned r = 0; r < c->w; r++) k = fnv(k, rot + (size_t)r * c->h, c->h);
        ref->pix[i] = k;
        img.pixels = rot;
        img.width = c->h;  /* the turned column: as wide as the column is tall */
        img.height = c->w;
        img.stride = c->h;
        img.format = VOCR_GREY8 | (c->dark ? VOCR_INVERT : 0);
        memset(&ex, 0, sizeof ex);
        if (with_lm) {
            ex.topk = VJO_VOCR_LM_TOPK; ex.topk_logit = tv; ex.topk_class = tc; ex.blank_logit = bl;
            ex.cap_frames = VJO_VOCR_LM_FRAMES;
        }
        TEST_ASSERT(vocr_run(&ctx, &img, greedy, sizeof greedy, &ex) == VOCR_OK);
        ref->frames[i] = ex.frames;
        if (with_lm && greedy[0]) TEST_ASSERT(vocr_lm_decode(&lc, &ex, ref->text[i], sizeof ref->text[i], NULL) == VOCR_OK);
        else snprintf(ref->text[i], sizeof ref->text[i], "%s", greedy);
        if (ref->text[i][0]) {
            size_t n = strlen(ref->text[i]);
            if (used) ref->joined[used++] = '\n';
            memcpy(ref->joined + used, ref->text[i], n + 1);
            used += n;
        }
    }
}

/* The job's lines (the callback) are the reference's columns, in the finder's order (right to left), each
 * with its box, polarity, turned pixels and reading; the stats are the columns'. */
static void check_columns(const Ref *ref, const char *text, const VjoVocrStats *st, const char *what)
{
    TEST_CHECK_(!strcmp(text, ref->joined), "%s: text \"%s\", the columns' \"%s\"", what, text, ref->joined);
    TEST_CHECK_(st->columns == 1 && st->lines == ref->set.count && st->recognized == ref->set.count &&
                !st->rotate_skipped && !st->truncated, "%s: %u columns found, %u recognized, %u expected", what,
                st->lines, st->recognized, ref->set.count);
    TEST_CHECK_(seen.n == ref->set.count, "%s: %u lines seen", what, seen.n);
    for (unsigned i = 0; i < seen.n && i < ref->set.count; i++) {
        const VocrLine *c = &ref->set.lines[i];
        TEST_CHECK_(seen.x[i] == c->x && seen.y[i] == c->y && seen.w[i] == c->w && seen.h[i] == c->h &&
                    seen.dark[i] == c->dark && seen.pix[i] == ref->pix[i] && !strcmp(seen.text[i], ref->text[i]),
                    "%s: column %u at %u,%u %ux%u \"%s\", the reference's at %u,%u %ux%u \"%s\"", what, i, seen.x[i],
                    seen.y[i], seen.w[i], seen.h[i], seen.text[i], c->x, c->y, c->w, c->h, ref->text[i]);
        if (i) TEST_CHECK_(seen.x[i] + seen.w[i] < seen.x[i - 1] + seen.w[i - 1], "%s: column %u not left of %u", what,
                           i, i - 1);
        TEST_CHECK(st->line[i].width == c->w && st->line[i].height == c->h && st->line[i].frames == ref->frames[i]);
    }
}

static void test_columns_read_right_to_left(void)
{
    static const unsigned glyphs[4] = {6, 4, 7, 3}, short_glyphs[4] = {2, 1, 2, 2};
    static const struct { unsigned char bg, ink; } pol[2] = {{20, 225}, {235, 25}}; /* light text, dark text */
    static const char *const models_of[3] = {M_COLS, M_COLS1, M_COLSD};
    char text[VJO_OCR_TEXT_CAP], raw[VJO_OCR_TEXT_CAP], own_dark[VJO_OCR_TEXT_CAP] = "";
    VjoVocrStats st;
    Ref ref;
    unsigned distinct = 0, strips_no_dark = 0;
    setup_files();
    for (unsigned p = 0; p < 2; p++)
        for (unsigned tall = 0; tall <= 1; tall++)
            for (unsigned lm = 0; lm <= 1; lm++)
                for (unsigned variant = 0; variant <= 2; variant++) { /* P0, P1 (NO_DARK); P1, the finder's dark */
                    const char *model = models_of[variant];
                    JobOpts o = {.lm = (int)lm, .line = 1};
                    char what[96];
                    snprintf(what, sizeof what, "%s text, %s columns, %s, P%u%s", p ? "dark" : "light",
                             tall ? "tall" : "short", lm ? "LM" : "greedy", variant ? 1u : 0u,
                             variant == 2 ? " (finder's dark)" : "");
                    if (tall) draw_columns(RW, RH, 4, glyphs, 20, 26, pol[p].bg, pol[p].ink);
                    else draw_columns(200, 60, 4, short_glyphs, 20, 26, pol[p].bg, pol[p].ink);
                    reference(variant ? VOCR_COLUMNS_VALLEYS : 0u, (int)lm, &ref); /* the finder's own dark flags */
                    TEST_ASSERT_(ref.set.count == 4, "%s: the finder found %u columns, 4 drawn", what, ref.set.count);
                    for (unsigned i = 0; i < 4; i++) TEST_CHECK(ref.set.lines[i].dark == p);
                    seen_ph = 1;
                    TEST_CHECK_(job(model, &o, text, sizeof text, raw, sizeof raw, &st) == VJO_OK, "%s", what);
                    check_columns(&ref, text, &st, what);
                    /* the columns taller than a strip went through the line buffer, the short ones not */
                    TEST_CHECK_(tall ? st.tall == 4 && !st.tall_skipped : !st.tall, "%s: %u tall", what, st.tall);
                    /* the dark text's columns counted: by the finder, or by the job on the pixels it fetched */
                    TEST_CHECK_(st.dark == 4u * p, "%s: %u dark", what, st.dark);
                    /* NO_DARK: the same columns and text from fewer strip reads (the finder read no column's
                     * rows for its dark flag; each column spans every strip of a tall region) */
                    if (variant == 1) {
                        strips_no_dark = st.strips;
                        snprintf(own_dark, sizeof own_dark, "%s", text);
                    } else if (variant == 2) {
                        TEST_CHECK_(!strcmp(text, own_dark), "%s: \"%s\" against NO_DARK's \"%s\"", what, text, own_dark);
                        TEST_CHECK_(tall ? strips_no_dark < st.strips : strips_no_dark <= st.strips,
                                    "%s: %u strips, %u with NO_DARK", what, st.strips, strips_no_dark);
                        if (tall && !lm) printf("strips read: %u with NO_DARK, %u with the finder's dark flags\n",
                                                strips_no_dark, st.strips);
                    }
                    for (unsigned i = 1; i < 4; i++) distinct += strcmp(ref.text[i], ref.text[i - 1]) != 0;
                    printf("%-40s \"%s\"\n", what, text);
                }
    seen_ph = 0;
    /* the order is tested only if the columns' readings differ */
    TEST_CHECK_(distinct > 8, "%u neighbouring columns read differently", distinct);
}

/* Helpers and the line cache give the serial job's text; a second job on the same screen reads every
 * column from the cache (no recognition), a changed column alone is read again. */
static void test_helpers_and_cache(void)
{
    static const unsigned glyphs[4] = {6, 4, 7, 3};
    char text[VJO_OCR_TEXT_CAP], serial[VJO_OCR_TEXT_CAP], raw[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
    setup_files();
    draw_columns(RW, RH, 4, glyphs, 20, 26, 20, 225);
    for (int lm = 0; lm <= 1; lm++) {
        JobOpts o = {.lm = lm};
        TEST_ASSERT(job(M_COLS, &o, serial, sizeof serial, raw, sizeof raw, &st) == VJO_OK && st.recognized == 4);
        for (int h = 1; h <= 2; h++) {
            o.helpers = h;
            pool_state.runs = 0;
            TEST_CHECK(job(M_COLS, &o, text, sizeof text, raw, sizeof raw, &st) == VJO_OK);
            TEST_CHECK_(!strcmp(text, serial) && st.helpers == (unsigned)h && st.dispatched >= 1 &&
                        pool_state.runs == st.dispatched, "lm %d, %d helpers: \"%s\", serial \"%s\"", lm, h, text, serial);
        }
        o.helpers = 0;
        o.cache = 1;
        memset(&cache, 0, sizeof cache);
        TEST_CHECK(job(M_COLS, &o, text, sizeof text, raw, sizeof raw, &st) == VJO_OK && !strcmp(text, serial) &&
                   !st.cached && st.frames > 0);
        TEST_CHECK(job(M_COLS, &o, text, sizeof text, raw, sizeof raw, &st) == VJO_OK && !strcmp(text, serial) &&
                   st.cached == 4 && st.frames == 0);
        column(RW - 20u - 2u * 38u, 8, 4, 20, 26, 9u, 225); /* column 2's glyphs change */
        TEST_CHECK(job(M_COLS, &o, text, sizeof text, raw, sizeof raw, &st) == VJO_OK && st.cached == 3 && st.frames > 0);
        draw_columns(RW, RH, 4, glyphs, 20, 26, 20, 225);
    }
}

/* The rotation buffer's bound: a column whose crop holds more than VJO_VOCR_ROTATE_BYTES is left out, never
 * read (not even through the line buffer), and counted; the column beside it is read as before. The region is
 * the full 544 px tall: a column of 104 px of ink (110 padded) is 58,960 B, of 90 px 51,456 B. */
static void test_rotation_buffer_bound(void)
{
    static const unsigned glyphs[1] = {6};
    char text[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
    JobOpts o = {.line = 1};
    Ref ref;
    setup_files();
    for (unsigned wide = 0; wide <= 1; wide++) {
        unsigned big_w = wide ? 104u : 90u;
        /* a column of 64-px cells at the right (furigana is a column under 55 % of the median width), and at
         * the left a band of bars every 4 rows, as tall as the region */
        draw_columns(260, 544, 1, glyphs, 64, 72, 20, 225);
        for (unsigned y = 6; y < 538; y += 4)
            for (unsigned x = 8; x < 8 + big_w; x++) dot(x, y, 225);
        reference(0, 0, &ref);
        TEST_ASSERT_(ref.set.count == 2, "wide %u: %u columns found", wide, ref.set.count);
        const VocrLine *b = &ref.set.lines[1];
        size_t bytes = (size_t)b->w * b->h;
        TEST_ASSERT_(wide ? bytes > VJO_VOCR_ROTATE_BYTES : bytes <= VJO_VOCR_ROTATE_BYTES,
                     "the band's column %ux%u: %zu B", b->w, b->h, bytes);
        seen_ph = 1;
        TEST_ASSERT(job(M_COLS, &o, text, sizeof text, NULL, 0, &st) == VJO_OK);
        seen_ph = 0;
        TEST_CHECK_(st.lines == 2 && st.rotate_skipped == wide && st.recognized == 2u - wide && seen.n == 2u - wide &&
                    seen.x[0] == ref.set.lines[0].x, "wide %u: %u lines, %u skipped, %u read", wide, st.lines,
                    st.rotate_skipped, st.recognized);
        TEST_CHECK_(!strcmp(text, wide ? ref.text[0] : ref.joined), "wide %u: \"%s\"", wide, text);
        /* the column at the right went through the line buffer; the skipped one was never read */
        TEST_CHECK_(st.tall == 2u - wide && st.line_bytes == (size_t)(wide ? ref.set.lines[0].w * ref.set.lines[0].h
                                                                             : bytes),
                    "wide %u: %u tall, %zu B the largest", wide, st.tall, st.line_bytes);
    }
}

/* The steps for rows never run on columns, whatever the policy asks: the short-line probe, the
 * refinement and the cascade (and no workspace is laid out for the last two). */
static void test_row_steps_do_not_run(void)
{
    static const unsigned glyphs[4] = {6, 4, 7, 3};
    char text[VJO_OCR_TEXT_CAP], plain[VJO_OCR_TEXT_CAP], raw[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
    JobOpts base = {.lm = 1};
    JobOpts all = {.lm = 1, .refine = 1, .short_lines = 1, .cascade = 1};
    setup_files();
    draw_columns(RW, RH, 4, glyphs, 20, 26, 20, 225);
    TEST_ASSERT(job(M_COLS, &base, plain, sizeof plain, raw, sizeof raw, &st) == VJO_OK);
    size_t plain_ws = st.workspace;
    TEST_ASSERT(job(M_COLS, &all, text, sizeof text, raw, sizeof raw, &st) == VJO_OK);
    TEST_CHECK_(!strcmp(text, plain) && st.lines == 4 && !st.refine_boxes && !st.refine_rc && !st.cascade &&
                !st.cascade_lines && !st.cascade_rc && !st.cascade_loaded && st.workspace == plain_ws,
                "\"%s\" against \"%s\"; workspace %zu against %zu", text, plain, st.workspace, plain_ws);
    /* the job of columns' own workspace: the larger of the finders' arenas (or the recognizer's), and the
     * rotation buffer after the runtime's structs */
    {
        size_t lines = vocr_lines_arena_bytes(RW, RH), cols = vocr_columns_arena_bytes(RW, RH);
        size_t a_rows = vjo_vocr_model(M_ROWS)->arena_bytes, a_cols = vjo_vocr_model(M_COLS)->arena_bytes;
        size_t s_rows = (lines > a_rows ? lines : a_rows), s_cols = cols > lines ? cols : lines;
        s_cols = s_cols > a_cols ? s_cols : a_cols;
        TEST_CHECK(vjo_vocr_job_bytes(M_COLS, RW, RH) == vjo_vocr_job_bytes(M_ROWS, RW, RH) +
                                                             ((s_cols + 15u) & ~(size_t)15u) -
                                                             ((s_rows + 15u) & ~(size_t)15u) +
                                                             ((VJO_VOCR_ROTATE_BYTES + 15u) & ~(size_t)15u));
    }
}

/* The cascade's time estimate counts a line's length in the recognizer's terms: a row's width at a height of 32
 * (main's formula, unchanged), a column's height at a width of 32, as it is read turned (audit-vertical-rubi M8). */
static void test_line_lengths(void)
{
    TEST_CHECK(vjo_vocr_test_line_length(640, 32, 0) == 641u && vjo_vocr_test_line_length(320, 40, 0) == 257u);
    TEST_CHECK(vjo_vocr_test_line_length(36, 290, 1) == 290u * 32u / 36u + 1u && vjo_vocr_test_line_length(32, 640, 1) == 641u);
    TEST_CHECK(vjo_vocr_test_line_length(36, 290, 0) == 36u * 32u / 290u + 1u); /* the same box as a row */
}

/* The text-presence gate (API 26) scores row boxes, and two guards keep it off columns (API 29): run_job sets it
 * up only for a model of rows (its weights read, its arena placed), and recognize begins it only on rows. Each test
 * below fails when its guard is gone (audit-vertical-rubi S2). Both put the gate's weights in the models' directory
 * when cmake/VocrGate.cmake got them, and run columns on M_COLSG, whose arena has room for the gate's. */
static int put_gate_file(void)
{
    char path[512];
    static unsigned char gate[VJO_VOCR_GATE_BYTES + 1];
    unsigned char sha[32];
    size_t n;
    FILE *f;
    if (!VJO_VOCR_GATE_TEST_DIR[0]) return 0;
    snprintf(path, sizeof path, "%s/%s", VJO_VOCR_GATE_TEST_DIR, VJO_VOCR_GATE_NAME);
    TEST_ASSERT_((f = fopen(path, "rb")) != NULL, "%s", path);
    n = fread(gate, 1, sizeof gate, f);
    fclose(f);
    TEST_ASSERT(n == VJO_VOCR_GATE_BYTES);
    write_file(VJO_VOCR_GATE_NAME, gate, n, sha);
    return 1;
}

static void drop_gate_file(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", model_dir, VJO_VOCR_GATE_NAME);
    remove(path);
}

/* run_job's guard: a job of columns with the gate asked for never reads its weights or places its arena, with
 * the file present or not, and reads as with the gate off. Rows with the same policy try it. */
static void test_gate_never_set_up_for_columns(void)
{
    static const unsigned glyphs[4] = {6, 4, 7, 3};
    char text[VJO_OCR_TEXT_CAP], plain[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
    JobOpts off = {0}, on = {.gate = 1};
    setup_files();
    for (int with_file = 0; with_file < 2; with_file++) {
        if (with_file && !put_gate_file()) break; /* without the weights the file-less pass covers the guard */
        draw_rows();
        TEST_CHECK(job(M_ROWS, &on, text, sizeof text, NULL, 0, &st) == VJO_OK && st.lines == LINES);
        TEST_CHECK_(with_file ? st.gate_read == 1 : st.gate_rc == VJO_E_OCR_MODEL && !st.gate_read,
                    "rows: gate read %u rc %d", st.gate_read, st.gate_rc); /* the policy reaches the gate for rows */
        draw_columns(RW, RH, 4, glyphs, 20, 26, 20, 225);
        TEST_ASSERT(job(M_COLSG, &off, plain, sizeof plain, NULL, 0, &st) == VJO_OK && st.lines == 4);
        TEST_CHECK(job(M_COLSG, &on, text, sizeof text, NULL, 0, &st) == VJO_OK);
        TEST_CHECK_(!strcmp(text, plain) && st.lines == 4 && !st.gate_read && !st.gate_rc && !st.gate_ran &&
                    !st.gate_rows && !st.gate_stopped, "file %d: \"%s\" against \"%s\", gate read %u rc %d ran %u",
                    with_file, text, plain, st.gate_read, st.gate_rc, st.gate_ran);
    }
    drop_gate_file();
}

/* recognize's guard: with the gate set up for a job of columns (vjo_vocr_test_gate_columns: as without
 * run_job's guard) and its real weights, the job still never begins it on the columns. */
static void test_gate_never_runs_on_columns(void)
{
    static const unsigned glyphs[4] = {6, 4, 7, 3};
    char text[VJO_OCR_TEXT_CAP], plain[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
    JobOpts off = {0}, on = {.gate = 1};
    int rc;
    setup_files();
    if (!put_gate_file()) {
        TEST_SKIP("no text-presence gate weights (cmake/VocrGate.cmake could not get them)");
        return;
    }
    draw_columns(RW, RH, 4, glyphs, 20, 26, 20, 225);
    TEST_ASSERT(job(M_COLSG, &off, plain, sizeof plain, NULL, 0, &st) == VJO_OK && st.lines == 4);
    vjo_vocr_test_gate_columns(1);
    rc = job(M_COLSG, &on, text, sizeof text, NULL, 0, &st);
    vjo_vocr_test_gate_columns(0);
    drop_gate_file();
    /* set up: its weights read and verified, its arena placed (gate_rc 0); never begun: no row, no score */
    TEST_CHECK_(rc == VJO_OK && st.gate_read == 1 && !st.gate_rc, "rc %d, gate read %u rc %d", rc, st.gate_read,
                st.gate_rc);
    TEST_CHECK_(!strcmp(text, plain) && st.lines == 4 && !st.gate_ran && !st.gate_rows && !st.gate_stopped,
                "\"%s\" against \"%s\", gate ran %u rows %u stopped %u", text, plain, st.gate_ran, st.gate_rows,
                st.gate_stopped);
}

/* A region without ink gives no column: the job reads nothing and opens no model file. */
static void test_empty_region_opens_no_model(void)
{
    char text[VJO_OCR_TEXT_CAP] = "old";
    VjoVocrStats st;
    JobOpts o = {0};
    setup_files();
    vjo_vocr_resident_drop(&res);
    fill(RW, RH, 20);
    int rc = job(M_COLS, &o, text, sizeof text, NULL, 0, &st);
    TEST_CHECK_(rc == VJO_OK && !text[0] && !st.lines && !st.model_loaded && !st.model_cached && st.columns == 1,
                "rc %d, %u columns, text \"%s\"", rc, st.lines, text);
}
#endif /* VJO_TEST_OFF_ONLY */

TEST_LIST = {
    {"off: a job of rows is today's job exactly (main 1de7b67's figures)", test_off_is_todays_job},
#ifndef VJO_TEST_OFF_ONLY
    {"columns: right to left, each read turned, light and dark, short and tall, greedy and LM, P0 and P1",
     test_columns_read_right_to_left},
    {"columns: helpers and the line cache give the serial job's text", test_helpers_and_cache},
    {"columns: one larger than the rotation buffer is left out, never read, and counted", test_rotation_buffer_bound},
    {"columns: the steps for rows never run, whatever the policy asks", test_row_steps_do_not_run},
    {"columns: a region without ink gives no column and opens no model", test_empty_region_opens_no_model},
    {"line lengths: a row's as before, a column's turned (the cascade's estimate)", test_line_lengths},
    {"the text-presence gate is never set up for columns (run_job's guard)", test_gate_never_set_up_for_columns},
    {"the text-presence gate never runs on columns (recognize's guard)", test_gate_never_runs_on_columns},
#endif
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("vocr-vertical")) return 2;
    return acutest_main(argc, argv);
}
