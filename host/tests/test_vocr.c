/* vita-vn-ocr backend (core/vocr_ocr.c) without its weights: sizing (no copy
 * of the region: one strip and its row checksums), the strips read through the
 * source, the line finder on both polarities, a screen that changes while it is
 * read, and every refusal before or while the model loads. With
 * VJO_VOCR_MODEL_DIR set, the pinned table is also checked against the real
 * release files. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "platform_posix.h"
#include "vocr.h"
#include "vocr_lm.h"
#include "vocr_ocr.h"
#include "vocr_models.h"
#include "vocr_lines.h"
#include "../../core/fbgrey.h"
#include "../../core/config.h"
#include "../../include/vjo_ledger_budget.h"
#include "../../include/vjo_vocr_sizes.h"
#include "test_tier_pins.h"
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
    /* the models accepted by name (the host tools) and each size's own (test_tier_pins.h), each once */
    static const char *const by_name[] = {VJO_VOCR3_H15_NAME, VJO_VOCR3_FL10_NAME, "H15_w8.vocr", "FL10_w8.vocr",
                                          "F20_w8.vocr", VJO_VOCR4_W20_NAME};
    unsigned expected = 6;
    for (unsigned i = 0; i < 3; i++) {
        unsigned known = 0;
        for (unsigned k = 0; k < 6; k++) known |= !strcmp(test_tier_pins[i].name, by_name[k]);
        expected += !known;
    }
    TEST_CHECK_(n == expected && all != NULL, "%u pinned models, %u expected", n, expected);
    for (unsigned k = 0; k < 6; k++) TEST_CHECK_(vjo_vocr_model(by_name[k]) != NULL, "%s", by_name[k]);
    for (unsigned i = 0; i < n; i++) TEST_CHECK(all[i].finder_mode == VOCR_LINES_TEXTBOX_RECOVERY);
    /* the three sizes' models (test_tier_pins.h: their releases' exact sizes and W8I arenas) */
    for (unsigned i = 0; i < 3; i++) {
        const VjoVocrModel *m = vjo_vocr_model(test_tier_pins[i].name);
        TEST_CHECK_(m && m->bytes == test_tier_pins[i].bytes && m->arena_bytes == test_tier_pins[i].arena &&
                    !strcmp(vjo_vocr_tier_model((int)i + 1), test_tier_pins[i].name), "%s", test_tier_pins[i].name);
    }
    /* every pin within the weights block the game worker allows: the Large size's, the largest */
    for (unsigned i = 0; i < n; i++) TEST_CHECK_(all[i].bytes <= VJO_VOCR_MAX_BYTES, "%s", all[i].name);
    TEST_CHECK(VJO_VOCR_MAX_BYTES == test_tier_pins[2].bytes && VJO_VOCR_MAX_BYTES == VJO_VOCR_TIER3_BYTES);
    /* the cascade's larger model: F20's shape, so F20's size and arena */
    const VjoVocrModel *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME), *f20 = vjo_vocr_model("F20_w8.vocr");
    TEST_ASSERT(w20 != NULL && f20 != NULL);
    TEST_CHECK(w20->bytes == f20->bytes && w20->arena_bytes == f20->arena_bytes && memcmp(w20->sha256, f20->sha256, 32));
    const VjoVocrModel *def = vjo_vocr_model(VJO_VOCR_DEFAULT_MODEL);
    TEST_ASSERT(def != NULL);
    TEST_CHECK(def->bytes == test_tier_pins[1].bytes && def->arena_bytes == test_tier_pins[1].arena &&
               !strcmp(def->name, VJO_VOCR_TIER2_NAME));
    TEST_CHECK(vjo_vocr_model("H15_w8.vocr")->bytes == 1155968 && vjo_vocr_model(VJO_VOCR3_H15_NAME)->bytes == 1155968);
    TEST_CHECK(vjo_vocr_model("FL10_w8.vocr")->bytes == 657664);
    TEST_CHECK(vjo_vocr_model(VJO_VOCR3_FL10_NAME)->bytes == 657664);
    TEST_CHECK(!vjo_vocr_model("H15_f32.vocr") && !vjo_vocr_model("") && !vjo_vocr_model(NULL));
    /* weights + max(finder, recognizer arena) + the runtime's two structs: no grey
     * copy of the region any more (API 19), at any size */
    size_t full = vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 960, 544);
    size_t parts = (size_t)test_tier_pins[1].bytes + test_tier_pins[1].arena;
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
    /* The VOCR2 trial was removed: its model file is no longer one the worker runs. */
    TEST_CHECK(!vjo_vocr_model("VOCR2_FL10_w8.vocr"));
    TEST_CHECK(!vjo_vocr_workspace_bytes("VOCR2_FL10_w8.vocr", W, H));
}

/* core/vocr_models.c names exactly the models the worker pins, at the same
 * sizes, each with a generation (the local engine's name in the Rubi app)
 * and a shape; the default is the Normal size's (test_tier_pins.h). */
static void test_model_metadata_matches_the_pins(void)
{
    unsigned n = 0, infos = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n);
    const VjoVocrModelInfo *info = vjo_vocr_model_infos(&infos);
    char label[24];
    TEST_CHECK(infos == n && info != NULL);
    for (unsigned i = 0; i < n; i++) {
        const VjoVocrModelInfo *m = vjo_vocr_model_info(all[i].name);
        TEST_CHECK_(m != NULL && m->bytes == all[i].bytes, "%s", all[i].name);
        TEST_CHECK_(m && !strncmp(m->generation, "VOCR", 4) && m->generation[4] >= '1' && m->generation[4] <= '9' &&
                    m->shape[0], "%s", all[i].name);
        /* a file named VOCRn_... belongs to generation VOCRn */
        if (m && !strncmp(all[i].name, "VOCR", 4)) {
            size_t g = strlen(m->generation);
            TEST_CHECK_(!strncmp(all[i].name, m->generation, g) && all[i].name[g] == '_', "%s", all[i].name);
        }
    }
    for (unsigned i = 0; i < infos; i++) TEST_CHECK_(vjo_vocr_model(info[i].name) != NULL, "%s", info[i].name);
    const VjoVocrModelInfo *def = vjo_vocr_model_info(VJO_VOCR_DEFAULT_MODEL);
    TEST_ASSERT(def != NULL);
    for (unsigned i = 0; i < 3; i++) { /* each size's label, generation-shape, as the Rubi app shows it */
        const VjoVocrModelInfo *m = vjo_vocr_model_info(test_tier_pins[i].name);
        TEST_CHECK_(m && !strcmp(m->generation, test_tier_pins[i].generation) &&
                    vjo_vocr_model_label(m, label, sizeof(label)) == (int)strlen(test_tier_pins[i].label) &&
                    !strcmp(label, test_tier_pins[i].label), "%s: %s", test_tier_pins[i].name, label);
    }
    size_t def_len = strlen(test_tier_pins[1].label);
    TEST_CHECK(!strcmp(def->generation, test_tier_pins[1].generation));
    TEST_CHECK(vjo_vocr_model_label(vjo_vocr_model_info(VJO_VOCR3_FL10_NAME), label, sizeof(label)) == 10 &&
               !strcmp(label, "VOCR3-FL10"));
    TEST_CHECK(vjo_vocr_model_label(vjo_vocr_model_info("FL10_w8.vocr"), label, sizeof(label)) > 0 &&
               !strcmp(label, "VOCR1-FL10"));
    TEST_CHECK(vjo_vocr_model_label(def, label, def_len) < 0 && !label[0]); /* does not fit: nothing */
    TEST_CHECK(vjo_vocr_model_label(def, label, def_len + 1) == (int)def_len && !strcmp(label, test_tier_pins[1].label));
    TEST_CHECK(vjo_vocr_model_label(NULL, label, sizeof(label)) < 0 && !label[0]);
    TEST_CHECK(vjo_vocr_model_label(def, NULL, 8) < 0 && vjo_vocr_model_label(def, label, 0) < 0);
    TEST_CHECK(!vjo_vocr_model_info("VOCR2_FL10_w8.vocr") && !vjo_vocr_model_info("other.vocr") &&
               !vjo_vocr_model_info("") && !vjo_vocr_model_info(NULL));
    TEST_CHECK(vjo_vocr_model_infos(NULL) == info);
}

/* API 21: each model tier's pinned model is one the runtime runs, smaller to
 * larger, and the estimates the Rubi app shows for it come from the runtime
 * itself: its job bytes are the game worker's check at a full screen (page-rounded
 * weights, workspace and capture block, without the LM, cascade or refinement),
 * its need adds the tier's reserve; the time grows with the size. The three
 * tables (include/vjo_vocr_sizes.h, core/config.c, core/vocr_models.c) agree;
 * tools/tests/test_vocr_defaults.py checks them against tools/model_tiers.json. */
static void test_tier_metadata_matches_the_runtime(void)
{
#define PAGE(x) (((size_t)(x) + 4095u) & ~(size_t)4095u)
    static const char *const names[] = {"", VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME};
    static const uint32_t bytes[] = {0, VJO_VOCR_TIER1_BYTES, VJO_VOCR_TIER2_BYTES, VJO_VOCR_TIER3_BYTES};
    static const uint32_t reserve[] = {0, VJO_GAME_RESERVE_SMALL, VJO_GAME_RESERVE_NORMAL, VJO_GAME_RESERVE_LARGE};
    static const char *const ids[] = {"", "small", "normal", "large"};
    const VjoVocrModelInfo *prev = NULL;
    TEST_CHECK(VJO_VOCR_TIERS == 3u && VJO_VOCR_TIERS_ALL == 7u);
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
        const VjoVocrTierInfo *tier = vjo_vocr_tier_info(t);
        const VjoVocrModel *m = tier ? vjo_vocr_model(tier->model) : NULL;
        const VjoVocrModelInfo *mi = tier ? vjo_vocr_model_info(tier->model) : NULL;
        TEST_ASSERT(tier && m && mi);
        TEST_CHECK_(!strcmp(tier->model, names[t]) && !strcmp(vjo_vocr_tier_model((int)t), names[t]) &&
                    m->bytes == bytes[t] && mi->bytes == bytes[t], "tier %u: %s", t, tier->model);
        TEST_CHECK(!strcmp(tier->id, ids[t]) && !strcmp(vjo_vocr_tier_id((int)t), ids[t]) &&
                   vjo_vocr_tier_parse(ids[t]) == (int)t && tier->reserve == reserve[t] && tier->name[0] &&
                   tier->tagline[0]);
        /* The console's (32-bit) job: the arena (or the finder's at 960x544, the larger) and the
         * runtime's two structs, 3,760 B there (the #81 self-test's workspace records: VOCR3-FL10
         * 128,592 = 124,832 + 3,760, VOCR3-H15 at the full screen 186,704 = 182,944 + 3,760); a
         * 64-bit host's structs are larger, so its own job may be a page more. */
        size_t finder = vocr_lines_arena_bytes(960, 544), arena = m->arena_bytes > finder ? m->arena_bytes : finder;
        size_t vita_structs = 3760u, structs = vjo_vocr_job_bytes(m->name, 960, 544) - ((arena + 15u) & ~(size_t)15u);
        TEST_CHECK_(sizeof(void *) == 4 ? structs == vita_structs : structs >= vita_structs, "structs %zu B", structs);
        size_t job = PAGE(m->bytes) + PAGE(((arena + 15u) & ~(size_t)15u) + vita_structs) +
                     PAGE(vjo_vocr_capture_bytes(960, 544));
        TEST_CHECK_(mi->job_bytes == job, "%s: metadata %u B, the runtime's check %zu B", m->name, mi->job_bytes, job);
        TEST_CHECK(vjo_vocr_tier_need(t) == mi->job_bytes + tier->reserve);
        /* the shape's estimate: the same for every model of a shape, larger for a larger tier, unless
         * that tier kept the model of the tier below it (the same model, the same figures) */
        TEST_CHECK(mi->box_ms > 0 && (!prev || mi == prev ||
                                      (mi->box_ms > prev->box_ms && mi->job_bytes > prev->job_bytes && mi->bytes > prev->bytes)));
        prev = mi;
    }
    TEST_CHECK(!vjo_vocr_tier_info(0) && !vjo_vocr_tier_info(4) && !vjo_vocr_tier_need(0) && !vjo_vocr_tier_need(4));
    /* each tier's model's generation is its release's (test_tier_pins.h) */
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++)
        TEST_CHECK(!strcmp(vjo_vocr_model_info(vjo_vocr_tier_info(t)->model)->generation, test_tier_pins[t - 1].generation));
    /* models of one shape share its estimates (the v0.2.0 ones and VOCR3's, F20 and VOCR4-W20) */
    TEST_CHECK(vjo_vocr_model_info("H15_w8.vocr")->box_ms == vjo_vocr_model_info(VJO_VOCR3_H15_NAME)->box_ms &&
               vjo_vocr_model_info("F20_w8.vocr")->job_bytes == vjo_vocr_model_info(VJO_VOCR4_W20_NAME)->job_bytes &&
               vjo_vocr_model_info("FL10_w8.vocr")->job_bytes == vjo_vocr_model_info(VJO_VOCR3_FL10_NAME)->job_bytes);
#undef PAGE
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
    char dir[PATH_MAX], path[PATH_MAX + 64], text[64];
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    posix_platform_init(&pp, &p);
    TEST_ASSERT(vjo_test_tmpdir_new(dir, sizeof(dir)) != NULL);
    snprintf(path, sizeof(path), "%s/%s", dir, VJO_VOCR_DEFAULT_MODEL);
    fill(20);
    line_of_strokes(30, 54, 235);
    write_file(path, 1000, 0); /* wrong size */
    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, dir, VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, CAP, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    write_file(path, vjo_vocr_model(VJO_VOCR_DEFAULT_MODEL)->bytes, 0x5A); /* right size, wrong SHA-256 */
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

/* The pinned sizes and arenas match the release files (when they are here); and
 * what the overlay sizes by class (any class count: VOCR5's 4,987 and 5,631, VOCR3's
 * 3,501 and 5,355) holds for each: a class's text is at most 4 bytes (a line's
 * greedy text is sized at 4 bytes a frame, core/vocr_ocr.c LM_LINE_TEXT), the top-K
 * keeps class numbers in 16 bits, and the LM decoder's workspace, which grows with
 * the classes (the class map), fits VJO_VOCR_LM_WS_BYTES with every LM here. */
static void test_release_files(void)
{
    const char *dir = VJO_VOCR_MODEL_DIR;
    unsigned n = 0, checked = 0, lm_checked = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n);
    void *lm_blob[VJO_VOCR_LM_TIERS] = {0};
    vocr_lm lm[VJO_VOCR_LM_TIERS];
    if (!dir[0]) {
        TEST_SKIP("VJO_VOCR_MODEL_DIR not set");
        return;
    }
    for (unsigned t = 0; t < VJO_VOCR_LM_TIERS; t++) {
        const VjoVocrLm *pin = vjo_vocr_lm_tier(t + 1u);
        char path[512];
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", dir, pin->name);
        if (!(f = fopen(path, "rb"))) continue;
        TEST_ASSERT(posix_memalign(&lm_blob[t], 16, pin->bytes + 1) == 0);
        TEST_CHECK(fread(lm_blob[t], 1, pin->bytes + 1, f) == pin->bytes);
        fclose(f);
        if (vocr_lm_load(&lm[t], lm_blob[t], pin->bytes) != VOCR_OK) {
            TEST_CHECK_(0, "%s: vocr_lm_load", pin->name);
            free(lm_blob[t]);
            lm_blob[t] = NULL;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        char path[512];
        void *blob = NULL;
        FILE *f;
        unsigned longest = 0;
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
        for (unsigned c = 1; c < m.n_classes; c++) {
            unsigned len;
            (void)vocr_class_text(&m, c, &len);
            if (len > longest) longest = len;
        }
        TEST_CHECK_(m.n_classes > 1 && m.n_classes <= 65536u && longest >= 1 && longest <= 4,
                    "%s: %u classes, the longest %u B", all[i].name, m.n_classes, longest);
        for (unsigned t = 0; t < VJO_VOCR_LM_TIERS; t++) {
            vocr_lm_opts lo;
            size_t need;
            if (!lm_blob[t]) continue;
            memset(&lo, 0, sizeof(lo)); /* core/vocr_ocr.c lm_opts */
            lo.beam = 8; lo.topk = VJO_VOCR_LM_TOPK; lo.alpha = 500; lo.beta = vjo_vocr_lm_tier(t + 1u)->beta;
            lo.gate = 8000; lo.flags = VOCR_LM_EOS; lo.bracket = 8000;
            need = vocr_lm_workspace_bytes(&m, &lm[t], &lo);
            TEST_CHECK_(need > 0 && need <= VJO_VOCR_LM_WS_BYTES, "%s with %s: LM workspace %zu B", all[i].name,
                        vjo_vocr_lm_tier(t + 1u)->name, need);
            lm_checked++;
        }
        free(blob);
        checked++;
    }
    for (unsigned t = 0; t < VJO_VOCR_LM_TIERS; t++) free(lm_blob[t]);
    TEST_CHECK(checked > 0);
    TEST_MSG("no release file in %s", dir);
    TEST_MSG("%u models, %u model x LM workspaces checked", checked, lm_checked);
}

/* The device default is the Normal size (test_tier_pins.h: VOCR5-N, bee-san/vita-vn-ocr
 * v0.3.0's VOCR5_N_w8.vocr), Small VOCR5-S and Large VOCR5-L; each within its size's ceilings
 * (the owner's int8 weights, 2026-10-14 18:09Z: Small < 1 MB, Normal <= 1.6 MB, Large
 * <= 2.5 MB; the arenas of vita-vn-ocr docs/model-tiers.md 1.3: 128 KiB, 256 KiB,
 * 1.25 MiB), and a larger size larger. VOCR3's and v0.2.0's models stay accepted
 * by name (the host tools): VOCR3 has the size and arena of the v0.2.0 H15 and
 * FL10 it was retrained from (so a job needs the same memory); and nothing pinned
 * exceeds the Large size's ceilings. */
static void test_default_model_and_ceiling(void)
{
    static const unsigned char vocr3_h15[32] = {
        0xcb,0x3c,0x61,0xa8,0xf9,0x1f,0x9a,0xee,0xb3,0xef,0xcc,0x0e,0x79,0x31,0x3f,0x66,
        0x41,0x12,0xf2,0x5c,0x4b,0xd5,0x58,0x65,0xba,0xe9,0xdf,0x52,0x52,0x2b,0x36,0x0c};
    static const unsigned char vocr3_fl10[32] = {
        0x87,0xfb,0x4d,0x0c,0x18,0x41,0x6f,0x5f,0x37,0x71,0xce,0xcd,0x27,0x4c,0x8d,0xb3,
        0xcf,0x69,0xb8,0xb4,0xa4,0xee,0x9f,0x20,0x4e,0xde,0xbb,0x4b,0x82,0x07,0x91,0x54};
    static const unsigned char h15_v020[32] = {
        0x31,0x6e,0xbc,0x37,0x7e,0xc3,0x45,0x20,0x44,0x09,0x91,0xc5,0x08,0x51,0x99,0xd8,
        0xe7,0x40,0x31,0xf0,0x1a,0x86,0x79,0x38,0xbe,0x90,0xa6,0x0c,0x1e,0x3f,0x03,0xcd};
    /* tools/model_tiers.json weights_max_bytes and arena_max_bytes, by size */
    static const uint32_t weights_max[] = {0, 999999u, 1600000u, 2500000u};
    static const uint32_t arena_max[] = {0, 131072u, 262144u, 1310720u};
    VjoConfig c;
    unsigned n = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n), *h15, *fl10, *old_h15, *old_fl10, *prev = NULL;
    vjo_config_defaults(&c);
    /* API 21: the default tier, Normal; each tier's model the table's */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && VJO_VOCR_TIER_DEFAULT == VJO_VOCR_TIER_NORMAL);
    TEST_CHECK(!strcmp(VJO_VOCR_DEFAULT_MODEL, vjo_vocr_tier_model(c.vocr_tier)) &&
               !strcmp(test_tier_pins[1].name, vjo_vocr_tier_model(VJO_VOCR_TIER_NORMAL)) &&
               !strcmp(test_tier_pins[0].name, vjo_vocr_tier_model(VJO_VOCR_TIER_SMALL)) &&
               !strcmp(test_tier_pins[2].name, vjo_vocr_tier_model(VJO_VOCR_TIER_LARGE)));
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
        const VjoVocrModel *m = vjo_vocr_model(vjo_vocr_tier_model((int)t));
        TEST_ASSERT(m != NULL);
        TEST_CHECK_(m->bytes <= weights_max[t] && m->arena_bytes <= arena_max[t], "tier %u: %s %u B, arena %u B", t,
                    m->name, m->bytes, m->arena_bytes);
        /* larger than the size below's, or that size's own model (a size may keep it) */
        TEST_CHECK(!prev || m == prev || (m->bytes > prev->bytes && m->arena_bytes >= prev->arena_bytes));
        prev = m;
    }
    h15 = vjo_vocr_model(VJO_VOCR3_H15_NAME);
    fl10 = vjo_vocr_model(VJO_VOCR3_FL10_NAME);
    old_h15 = vjo_vocr_model("H15_w8.vocr");
    old_fl10 = vjo_vocr_model("FL10_w8.vocr");
    TEST_ASSERT(h15 && fl10 && old_h15 && old_fl10);
    TEST_CHECK(!memcmp(h15->sha256, vocr3_h15, 32) && h15->bytes == 1155968 && h15->arena_bytes == 182944);
    TEST_CHECK(!memcmp(fl10->sha256, vocr3_fl10, 32) && fl10->bytes == 657664 && fl10->arena_bytes == 124832);
    TEST_CHECK(!memcmp(old_h15->sha256, h15_v020, 32));
    /* the same shapes: equal sizes, arenas, finders and job memory at every region */
    TEST_CHECK(h15->bytes == old_h15->bytes && h15->arena_bytes == old_h15->arena_bytes &&
               h15->finder_mode == old_h15->finder_mode && memcmp(h15->sha256, old_h15->sha256, 32));
    TEST_CHECK(fl10->bytes == old_fl10->bytes && fl10->arena_bytes == old_fl10->arena_bytes &&
               fl10->finder_mode == old_fl10->finder_mode && memcmp(fl10->sha256, old_fl10->sha256, 32));
    TEST_CHECK(vjo_vocr_job_bytes(h15->name, 960, 544) == vjo_vocr_job_bytes(old_h15->name, 960, 544));
    TEST_CHECK(vjo_vocr_job_bytes(fl10->name, 917, 125) == vjo_vocr_job_bytes(old_fl10->name, 917, 125));
    TEST_CHECK(vjo_vocr_workspace_bytes(h15->name, 1, 1) == vjo_vocr_workspace_bytes(old_h15->name, 1, 1));
    /* A smaller size needs less game memory than a larger one for every region (the same, with the
     * same model). */
    for (unsigned t = 2; t <= VJO_VOCR_TIERS; t++) {
        const char *a = vjo_vocr_tier_model((int)t - 1), *b = vjo_vocr_tier_model((int)t);
        if (!strcmp(a, b)) continue;
        TEST_CHECK(vjo_vocr_workspace_bytes(a, 960, 544) < vjo_vocr_workspace_bytes(b, 960, 544));
        TEST_CHECK(vjo_vocr_workspace_bytes(a, 1, 1) < vjo_vocr_workspace_bytes(b, 1, 1));
    }
    for (unsigned i = 0; i < n; i++) {
        TEST_CHECK(all[i].bytes <= weights_max[3] && all[i].arena_bytes <= arena_max[3]);
        TEST_MSG("%s: %u B weights, %u B arena", all[i].name, all[i].bytes, all[i].arena_bytes);
    }
}

/* The cascade's first re-read is estimated from the job's own time per line by
 * the two models' weights (core/vocr_ocr.c cost_ratio): the next size up of each
 * size, and VOCR4-W20 beside v0.2.0's FL10 and H15 for a request by name. */
static void test_cascade_ratio_follows_the_weights(void)
{
    const char *s = VJO_VOCR_TIER1_NAME, *nn = VJO_VOCR_TIER2_NAME, *l = VJO_VOCR_TIER3_NAME;
    TEST_CHECK(vjo_vocr_cascade_ratio(nn, s) == (uint32_t)((uint64_t)VJO_VOCR_TIER2_BYTES * 256u / VJO_VOCR_TIER1_BYTES));
    TEST_CHECK(vjo_vocr_cascade_ratio(l, nn) == (uint32_t)((uint64_t)VJO_VOCR_TIER3_BYTES * 256u / VJO_VOCR_TIER2_BYTES));
    /* a larger size reads slower (VOCR5-L beside VOCR5-N: 2.09x, eval/vocr5_protocol.json's estimates 1.94-2.48x);
     * a size that kept the model of the size below it is no cascade of it (game_ocr/main.c cascade_of) */
    TEST_CHECK(vjo_vocr_cascade_ratio(nn, s) > 256u);
    TEST_CHECK(!strcmp(l, nn) ? vjo_vocr_cascade_ratio(l, nn) == 256u : vjo_vocr_cascade_ratio(l, nn) > 256u);
    /* the request by name's: VOCR4-W20 beside FL10 (vita-vn-ocr's 235 / 94 ms, 2.5) and H15 (1.42) */
    TEST_CHECK(vjo_vocr_cascade_ratio(VJO_VOCR_CASCADE_NAME, "FL10_w8.vocr") == 606u &&
               vjo_vocr_cascade_ratio(VJO_VOCR_CASCADE_NAME, "H15_w8.vocr") == 345u);
    TEST_CHECK(!vjo_vocr_cascade_ratio("other.vocr", s) && !vjo_vocr_cascade_ratio(nn, NULL));
}

TEST_LIST = {
    {"default_is_vocr5_n_each_size_within_its_ceiling_vocr3_and_v020_kept", test_default_model_and_ceiling},
    {"the_cascade_ratio_follows_the_weights", test_cascade_ratio_follows_the_weights},
    {"models_and_workspace_sizes", test_models_and_sizes},
    {"model_metadata_names_every_pinned_model_with_its_generation", test_model_metadata_matches_the_pins},
    {"model_tiers_pin_runtime_models_and_their_estimates_come_from_the_runtime", test_tier_metadata_matches_the_runtime},
    {"empty_region_reads_strips_and_no_model", test_empty_region_reads_no_model},
    {"finds_light_and_dark_text_lines", test_lines_both_polarities},
    {"wrong_size_or_hash_is_refused", test_wrong_or_corrupt_weights},
    {"refusals_publish_no_text", test_refusals},
    {"a_changed_screen_is_read_again_then_refused", test_changed_screen_is_read_again},
    {"a_full_frame_goes_through_strips", test_full_frame_in_strips},
    {"pinned_table_matches_release_files", test_release_files},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("vocr")) return 2; /* the model files' directory in it, removed at the end */
    return acutest_main(argc, argv);
}
