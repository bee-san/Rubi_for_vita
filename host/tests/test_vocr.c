/* vita-vn-ocr backend (core/vocr_ocr.c) without its weights: sizing (no copy
 * of the region: one strip and its row checksums), the strips read through the
 * source, the line finder on both polarities, a screen that changes while it is
 * read, and every refusal before or while the model loads. With
 * VJO_VOCR_MODEL_DIR set, the pinned table is also checked against the real
 * release files. */
#include "acutest.h"
#include "net_posix.h"
#include "vocr.h"
#include "vocr_ocr.h"
#include "vocr_lines.h"
#include "../../core/fbgrey.h"
#include "../../include/vjo_vocr_sizes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 400
#define H 100
static unsigned char pixels[W * H * 4];
static unsigned reads[H], strips_read, change_at;
static int fail_rows, cancel_now, opens, change_every;

/* Grey strips of the RGBA pixels. change_at: from that strip on the screen is
 * different (the player advanced the text); change_every: it never settles. */
static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud; (void)flags;
    if (fail_rows) return fail_rows;
    if (y + n > H) return VJO_E_SOURCE;
    strips_read++;
    for (unsigned r = 0; r < n; r++) {
        reads[y + r]++;
        for (unsigned x = 0; x < W; x++) {
            const unsigned char *p = pixels + ((size_t)(y + r) * W + x) * 4;
            unsigned g = vjo_grey(p[0], p[1], p[2]);
            if (change_at && strips_read >= change_at) g ^= 0x40;
            if (change_every) g ^= (strips_read * 37u) & 0x7Fu; /* every read differs */
            dst[(size_t)r * stride + x] = (unsigned char)g;
        }
    }
    return (int)n;
}
static int cancelled(void *ud) { (void)ud; return cancel_now; }
static VjoGreySource im = {NULL, W, H, strip, cancelled, NULL};
static unsigned char capture[64 * 1024];

static void fill(unsigned char v)
{
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = pixels[i+1] = pixels[i+2] = v;
        pixels[i+3] = 255;
    }
}

/* Glyph-like vertical strokes (2 px wide every 6 px) across one text line. */
static void line_of_strokes(unsigned y0, unsigned y1, unsigned char v)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = 20; x < W - 20; x += 6)
            for (unsigned k = 0; k < 2; k++) {
                unsigned char *p = pixels + ((size_t)y * W + x + k) * 4;
                p[0] = p[1] = p[2] = v;
            }
}

static void reset(void)
{
    memset(reads, 0, sizeof(reads));
    strips_read = change_at = 0;
    fail_rows = cancel_now = opens = change_every = 0;
}
#define CAP capture, vjo_vocr_capture_bytes(W, H)

static int no_file(void *ud, const char *path, VjoFile *out)
{
    (void)ud; (void)path; (void)out;
    opens++;
    return -1;
}

static void *workspace(const char *model, size_t *bytes)
{
    void *p = NULL;
    *bytes = vjo_vocr_workspace_bytes(model, W, H);
    TEST_ASSERT(*bytes > 0);
    TEST_ASSERT(posix_memalign(&p, 64, *bytes) == 0);
    return p;
}

static void test_models_and_sizes(void)
{
    unsigned n = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n);
    TEST_CHECK(n == 3 + VJO_VOCR2_ENABLED && all != NULL);
    for (unsigned i = 0; i < 3; i++) TEST_CHECK(all[i].finder_mode == VOCR_LINES_GRAD);
    const VjoVocrModel *h15 = vjo_vocr_model(VJO_VOCR_DEFAULT_MODEL);
    TEST_ASSERT(h15 != NULL);
    TEST_CHECK(h15->bytes == 1155968 && h15->arena_bytes == 182944);
    TEST_CHECK(vjo_vocr_model("FL10_w8.vocr")->bytes == 657664);
    TEST_CHECK(!vjo_vocr_model("H15_f32.vocr") && !vjo_vocr_model("") && !vjo_vocr_model(NULL));
    /* weights + max(finder, recognizer arena) + the runtime's two structs: no grey
     * copy of the region any more (API 19), at any size */
    size_t full = vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 960, 544);
    size_t parts = 1155968 + 182944;
    TEST_CHECK(full >= parts && full - parts < sizeof(vocr_model) + sizeof(vocr_ctx) + 64);
    TEST_MSG("960x544 workspace %zu, parts %zu", full, parts);
    TEST_CHECK(vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 917, 125) == full); /* the arenas dominate */
    /* The capture block: one strip of grey rows and a checksum and a bit per row,
     * under 64 KiB at every region size, a full frame included. */
    static const unsigned sizes[][2] = {{960, 544}, {917, 125}, {760, 130}, {480, 272}, {1, 1}, {64, 544}};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned w = sizes[i][0], h = sizes[i][1], rows = vjo_vocr_strip_rows(w, h);
        size_t cap = vjo_vocr_capture_bytes(w, h);
        TEST_CHECK(rows >= 1 && rows <= VJO_VOCR_STRIP_MAX_ROWS && rows <= h && (size_t)rows * w <= VJO_VOCR_STRIP_BYTES);
        TEST_CHECK(cap < 64u * 1024u && ((cap + 4095u) & ~(size_t)4095u) <= 64u * 1024u);
        TEST_MSG("%ux%u: %u-row strips, capture block %zu B", w, h, rows, cap);
    }
    TEST_CHECK(vjo_vocr_strip_rows(960, 544) == 59 && vjo_vocr_strip_rows(917, 125) == 62);
    TEST_CHECK(!vjo_vocr_capture_bytes(961, 10) && !vjo_vocr_capture_bytes(10, 545) && !vjo_vocr_capture_bytes(0, 1));
    /* Resident form: the job is the one-shot workspace without the weights. */
    for (unsigned i = 0; i < n; i++) {
        size_t job = vjo_vocr_job_bytes(all[i].name, 917, 125);
        TEST_CHECK(job && job + ((all[i].bytes + 15u) & ~15u) == vjo_vocr_workspace_bytes(all[i].name, 917, 125));
    }
    TEST_CHECK(!vjo_vocr_job_bytes("other.vocr", 100, 10) && !vjo_vocr_job_bytes(VJO_VOCR_DEFAULT_MODEL, 961, 10));
    TEST_CHECK(vjo_vocr_workspace_bytes("FL10_w8.vocr", 960, 544) < full);
    /* the region's size no longer counts: the arenas and weights dominate */
    TEST_CHECK(vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 880, 144) == full);
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 961, 10));
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 10, 545));
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 0, 10));
    TEST_CHECK(!vjo_vocr_workspace_bytes("other.vocr", 100, 10));
#if VJO_VOCR2_ENABLED
    const VjoVocrModel *old = vjo_vocr_model("FL10_w8.vocr");
    const VjoVocrModel *next = vjo_vocr_model(VJO_VOCR2_ARTIFACT_NAME);
    TEST_ASSERT(next != NULL);
    TEST_CHECK(next->finder_mode == VOCR_LINES_TEXTBOX && next->finder_mode == 2);
    TEST_CHECK(next->bytes == old->bytes && next->arena_bytes == old->arena_bytes);
    TEST_CHECK(memcmp(next->sha256, old->sha256, sizeof(old->sha256)) != 0);
    TEST_CHECK(!strcmp(VJO_VOCR2_MODEL_NAME, VJO_VOCR2_ARTIFACT_NAME));
    TEST_CHECK(vjo_vocr_workspace_bytes(next->name, 960, 544) == vjo_vocr_workspace_bytes(old->name, 960, 544));
    TEST_CHECK(vjo_vocr_workspace_bytes(next->name, 761, 160) == vjo_vocr_workspace_bytes(old->name, 761, 160));
#else
    TEST_CHECK(!vjo_vocr_model(VJO_VOCR2_ARTIFACT_NAME));
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR2_ARTIFACT_NAME, W, H));
#endif
}

static void test_empty_region_reads_no_model(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64] = "old";
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;
    reset();
    fill(16);
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(!text[0] && st.lines == 0 && !st.model_loaded && opens == 0);
    TEST_CHECK(st.workspace == bytes && st.capture == vjo_vocr_capture_bytes(W, H));
    TEST_CHECK(st.strip_rows == 64 && st.strip_rows == vjo_vocr_strip_rows(W, H));
    /* The finder's two passes (both polarities), each over two 64-row strips:
     * the first pass's strips follow each other ([0,64) [64,100)), a later
     * strip stays inside the region ([0,64) [36,100)). No line: nothing more. */
    for (unsigned y = 0; y < H; y++) TEST_CHECK(reads[y] == (y >= 36 && y < 64 ? 3u : 2u));
    TEST_MSG("reads: row0 %u row63 %u row64 %u row99 %u, strips %u tries %u", reads[0], reads[63], reads[64],
             reads[99], st.strips, st.tries);
    TEST_CHECK(st.strips == 4 && !st.tears && st.tries == 1);
    free(ws);
}

static void test_lines_both_polarities(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64] = "old";
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;

    reset(); /* light text on a dark box */
    fill(20);
    line_of_strokes(20, 44, 235);
    line_of_strokes(60, 84, 235);
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 2 && st.dark == 0 && opens == 1 && !text[0]);
    TEST_MSG("lines %u dark %u", st.lines, st.dark);

    reset(); /* dark text on a light box: the overlay's bright-pixel finder rejects it */
    fill(235);
    line_of_strokes(20, 44, 30);
    line_of_strokes(60, 84, 30);
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 2 && st.dark == 2);
    TEST_MSG("lines %u dark %u", st.lines, st.dark);
    free(ws);
}

static void write_file(const char *path, size_t n, unsigned char v)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    for (size_t i = 0; i < n; i++) fputc(v, f);
    fclose(f);
}

static void test_wrong_or_corrupt_weights(void)
{
    VjoPlatform p;
    PosixPlatform pp = {0};
    VjoVocrStats st;
    char dir[] = "/tmp/vjo-vocr-XXXXXX", path[128], text[64];
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    posix_platform_init(&pp, &p);
    TEST_ASSERT(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/%s", dir, VJO_VOCR_DEFAULT_MODEL);
    fill(20);
    line_of_strokes(30, 54, 235);
    write_file(path, 1000, 0); /* wrong size */
    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, dir, VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    write_file(path, 1155968, 0x5A); /* right size, wrong SHA-256 */
    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, dir, VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 1 && !st.model_loaded && !text[0]);
    unlink(path);
    rmdir(dir);
    free(ws);
}

static void test_refusals(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64] = "old";
    size_t bytes;
    unsigned char *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;
    fill(20);
    line_of_strokes(30, 54, 235);

    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes - 1, CAP, text, sizeof(text), &st) ==
               VJO_E_OOM);
    TEST_CHECK(!text[0] && st.workspace == bytes && reads[0] == 0); /* refused before any read */
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, capture, vjo_vocr_capture_bytes(W, H) - 1,
                            text, sizeof(text), &st) == VJO_E_OOM && reads[0] == 0);
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws + 8, bytes - 8, CAP, text, sizeof(text), &st) ==
               VJO_E_OOM); /* unaligned */
    TEST_CHECK(vjo_vocr_ocr(&p, "d", "other.vocr", &im, ws, bytes, CAP, text, sizeof(text), &st) == VJO_E_OCR_MODEL);
    VjoGreySource big = im;
    big.width = VJO_OCR_MAX_WIDTH + 1;
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &big, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_REGION);
    reset();
    cancel_now = 1;
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_CANCELLED);
    reset();
    fail_rows = (int)0xBFFFFFFCu; /* the source's own error comes back */
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               (int)0xBFFFFFFCu);
    TEST_CHECK(vjo_vocr_ocr(NULL, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_SOURCE);
    free(ws);
}

/* The screen changes while it is read: the row checksums differ, the job starts
 * over, and gives up with VJO_E_SCREEN_CHANGED when it never settles. */
static void test_changed_screen_is_read_again(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64];
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;
    fill(20);
    line_of_strokes(30, 54, 235);
    reset();
    change_at = 2; /* the check read differs from the first */
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL); /* read again, then the (missing) model */
    /* 100 rows: two strips; the second pass's first strip saw the change */
    TEST_CHECK(st.tries == 2 && st.tears == vjo_vocr_strip_rows(W, H) && st.lines == 1);
    reset();
    change_every = 1;
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_SCREEN_CHANGED);
    TEST_CHECK(st.tries == VJO_VOCR_TRIES && !text[0] && !opens);
    free(ws);
}

/* A region taller than a strip: two passes over every row (the finder's own),
 * each strip read once per pass, the first pass's strips in order with the last
 * one marked; a 960x544 frame never needs more than its capture block. */
static int full_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    static unsigned next_first;
    unsigned *count = ud;
    if (flags & VJO_STRIP_FIRST) { TEST_CHECK(y == next_first); next_first = y + n; if (flags & VJO_STRIP_LAST) { TEST_CHECK(y + n == 544); next_first = 0; } }
    TEST_CHECK(y + n <= 544 && n <= 59 && stride == 960);
    for (unsigned r = 0; r < n; r++) {
        memset(dst + (size_t)r * stride, 16, 960);
        if ((y + r) % 100 >= 40 && (y + r) % 100 < 64)
            for (unsigned x = 40; x < 900; x += 6) memset(dst + (size_t)r * stride + x, 235, 2);
    }
    count[0]++;
    if (flags & VJO_STRIP_LAST) count[1]++;
    return (int)n;
}
static void test_full_frame_in_strips(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64];
    unsigned counts[2] = {0, 0};
    size_t bytes = vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 960, 544);
    void *ws = NULL;
    VjoGreySource full = {counts, 960, 544, full_strip, NULL, NULL};
    TEST_ASSERT(posix_memalign(&ws, 64, bytes) == 0);
    p.file_open = no_file;
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &full, ws, bytes, capture,
                            vjo_vocr_capture_bytes(960, 544), text, sizeof(text), &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 5 && st.strip_rows == 59 && counts[1] == 1 && !st.tears);
    TEST_CHECK(st.strips >= 2u * 10u && st.strips == counts[0]); /* ceil(544 / 59) = 10 strips a pass */
    TEST_MSG("960x544: %u lines, %u strips of %u rows, capture block %zu B", st.lines, st.strips, st.strip_rows,
             vjo_vocr_capture_bytes(960, 544));
    free(ws);
}

/* The pinned sizes and arenas match the release files (when they are here). */
static void test_release_files(void)
{
    const char *dir = VJO_VOCR_MODEL_DIR;
    unsigned n = 0, checked = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n);
    if (!dir[0]) {
        TEST_SKIP("VJO_VOCR_MODEL_DIR not set");
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        char path[512];
        void *blob = NULL;
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", dir, all[i].name);
        f = fopen(path, "rb");
        if (!f) continue;
        TEST_ASSERT(posix_memalign(&blob, 16, all[i].bytes + 1) == 0);
        TEST_CHECK(fread(blob, 1, all[i].bytes + 1, f) == all[i].bytes);
        fclose(f);
        vocr_model m;
        vocr_opts o;
        memset(&o, 0, sizeof(o));
        o.mode = VOCR_MODE_W8I;
        TEST_CHECK(vocr_load(&m, blob, all[i].bytes) == VOCR_OK);
        TEST_CHECK(vocr_check_crc(blob, all[i].bytes) == VOCR_OK);
        TEST_CHECK(vocr_arena_bytes(&m, &o) == all[i].arena_bytes);
        TEST_MSG("%s: arena %zu, pinned %u", all[i].name, vocr_arena_bytes(&m, &o), all[i].arena_bytes);
        free(blob);
        checked++;
    }
    TEST_CHECK(checked > 0);
    TEST_MSG("no release file in %s", dir);
}

/* The device default is H15 (bee-san/vita-vn-ocr v0.2.0 H15_w8.vocr, its
 * published sha256), FL10 its automatic fallback, and nothing pinned exceeds
 * the console budget ceiling (docs/vita-memory-budget.md in vita-vn-ocr). */
static void test_default_model_and_ceiling(void)
{
    static const unsigned char h15_v020[32] = {
        0x31,0x6e,0xbc,0x37,0x7e,0xc3,0x45,0x20,0x44,0x09,0x91,0xc5,0x08,0x51,0x99,0xd8,
        0xe7,0x40,0x31,0xf0,0x1a,0x86,0x79,0x38,0xbe,0x90,0xa6,0x0c,0x1e,0x3f,0x03,0xcd};
    VjoConfig c;
    unsigned n = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n), *h15, *fl10;
    vjo_config_defaults(&c);
    TEST_CHECK(!strcmp(c.vocr_model, "H15_w8.vocr") && !strcmp(c.vocr_fallback_model, "FL10_w8.vocr"));
    TEST_CHECK(!strcmp(VJO_VOCR_DEFAULT_MODEL, c.vocr_model));
    h15 = vjo_vocr_model(c.vocr_model);
    fl10 = vjo_vocr_model(c.vocr_fallback_model);
    TEST_ASSERT(h15 && fl10);
    TEST_CHECK(!memcmp(h15->sha256, h15_v020, 32) && h15->bytes == 1155968 && h15->arena_bytes == 182944);
    /* The fallback needs less game memory than the default for every region. */
    TEST_CHECK(vjo_vocr_workspace_bytes(fl10->name, 960, 544) < vjo_vocr_workspace_bytes(h15->name, 960, 544));
    TEST_CHECK(vjo_vocr_workspace_bytes(fl10->name, 1, 1) < vjo_vocr_workspace_bytes(h15->name, 1, 1));
    for (unsigned i = 0; i < n; i++) {
        TEST_CHECK(all[i].bytes <= 1600000u && all[i].arena_bytes <= 262144u);
        TEST_MSG("%s: %u B weights, %u B arena", all[i].name, all[i].bytes, all[i].arena_bytes);
    }
}

TEST_LIST = {
    {"default_is_h15_with_fl10_fallback_within_the_budget_ceiling", test_default_model_and_ceiling},
    {"models_and_workspace_sizes", test_models_and_sizes},
    {"empty_region_reads_strips_and_no_model", test_empty_region_reads_no_model},
    {"finds_light_and_dark_text_lines", test_lines_both_polarities},
    {"wrong_size_or_hash_is_refused", test_wrong_or_corrupt_weights},
    {"refusals_publish_no_text", test_refusals},
    {"a_changed_screen_is_read_again_then_refused", test_changed_screen_is_read_again},
    {"a_full_frame_goes_through_strips", test_full_frame_in_strips},
    {"pinned_table_matches_release_files", test_release_files},
    {NULL, NULL}
};
