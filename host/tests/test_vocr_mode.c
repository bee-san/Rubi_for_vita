/* The actual core adapter with a finder spy, no weights and no inference: each
 * model selects its finder mode (the vertical model, #99, the column finder and
 * its flags), the region arrives in grey strips through the source (no copy of
 * the whole region), and the workspace and capture block are the sizes the job
 * reports. */
#include "acutest.h"
#include "vocr_ocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "vocr_vertical.h"
#include "../../include/vjo_vocr_sizes.h"
#include <stdlib.h>
#include <string.h>
#include "vocr_lines_fix.h"
#include "platform_posix.h"

#ifndef VJO_VOCR_GATE_TEST_DIR
#define VJO_VOCR_GATE_TEST_DIR "" /* the text-presence gate's weights (cmake/VocrGate.cmake), "" without them */
#endif

#define WIDTH 40u
#define HEIGHT 12u
#define MAX_SIDE 64u
/* the region the source gives: WIDTH x HEIGHT, or another (set_region) */
static unsigned rw = WIDTH, rh = HEIGHT;
static unsigned row_reads[MAX_SIDE], strips, finder_calls, column_calls, model_opens, first_flags, last_flags;
static int selected_mode, emit_line;
static unsigned selected_fix;
/* API 50: each call of the fixed finder's fix, and the lines it gives with a fix (fixed_lines < 0: as without) */
static unsigned fix_seq[4], fix_calls;
static int fixed_lines = -1;
static const char *selected_model;
static const unsigned char grey = (unsigned char)((77u * 30 + 150u * 100 + 29u * 150) >> 8);

static int strip(void *user, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)user;
    TEST_CHECK(y + n <= rh && stride >= rw);
    strips++;
    if (flags & VJO_STRIP_FIRST) first_flags++;
    if (flags & VJO_STRIP_LAST) last_flags++;
    for (unsigned r = 0; r < n; r++) {
        row_reads[y + r]++;
        memset(dst + (size_t)r * stride, grey, rw);
    }
    return (int)n;
}

static int spy_finder(const VocrLinesRows *src, void *arena, size_t bytes, VocrLineSet *out);

int vjo_test_find_lines_rows(const VocrLinesRows *src, int finder, void *arena, size_t bytes, VocrLineSet *out)
{
    finder_calls++; selected_mode = finder;
    return spy_finder(src, arena, bytes, out);
}

/* The vertical model's (vocr_vertical.h): its flags are the model's finder_mode's low byte; the
 * arena holds both finders'. */
int vjo_test_find_columns_rows(const VocrLinesRows *src, unsigned flags, void *arena, size_t bytes, VocrLineSet *out)
{
    column_calls++; selected_mode = (int)(VJO_VOCR_FINDER_COLUMNS | flags);
    TEST_CHECK(bytes >= vocr_columns_arena_bytes(rw, rh));
    return spy_finder(src, arena, bytes, out);
}

static int spy_finder(const VocrLinesRows *src, void *arena, size_t bytes, VocrLineSet *out)
{
    TEST_CHECK(src->width == rw && src->height == rh && src->bpp == VOCR_LINES_GREY);
    TEST_CHECK(arena && bytes >= vocr_lines_arena_bytes(rw, rh));
    /* the finder reads the region through the row callback, two passes */
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned y = 0; y < rh; y++) {
            const unsigned char *row = src->row(src->user, y);
            TEST_ASSERT(row != NULL);
            for (unsigned x = 0; x < rw; x++) TEST_CHECK(row[x] == grey);
        }
    memset(out, 0, sizeof(*out));
    if (emit_line == 1) {
        out->count = 1;
        out->lines[0] = (VocrLine){2, 2, 20, 6, 0};
    } else if (emit_line == 2) { /* two rows 8 x 6 px over the same span: what auto's rule takes for a column */
        out->count = 2;
        out->lines[0] = (VocrLine){2, 2, 8, 6, 0};
        out->lines[1] = (VocrLine){2, 20, 8, 6, 0};
    }
    return VOCR_LINES_OK;
}

/* API 50: the job's finder is the fixed one (vita-vn-ocr #129, runtime/vocr_lines_fix.h); fix 0 is the frozen
 * finder's lines, which the spy above stands for */
int vjo_test_find_lines_fix_rows(const VocrLinesRows *src, int finder, unsigned fix, void *arena, size_t bytes,
                                 VocrLineSet *out)
{
    int rc;
    selected_fix = fix;
    if (fix_calls < sizeof(fix_seq) / sizeof(fix_seq[0])) fix_seq[fix_calls] = fix;
    fix_calls++;
    rc = vjo_test_find_lines_rows(src, finder, arena, bytes, out);
    if (!rc && fix && fixed_lines >= 0) { /* the fixed rows: fixed_lines rows, 8 x 6 px, 18 px apart */
        out->count = (unsigned)fixed_lines;
        for (unsigned i = 0; i < out->count; i++) out->lines[i] = (VocrLine){2, 2 + 18 * i, 8, 6, 0};
    }
    return rc;
}

static int absent_model(void *user, const char *path, VjoFile *out)
{
    (void)user; (void)out;
    TEST_CHECK(!strncmp(path, "absent/", 7) && !strcmp(path + 7, selected_model));
    model_opens++;
    return -1;
}

static void reset(int line)
{
    memset(row_reads, 0, sizeof(row_reads));
    strips = finder_calls = column_calls = model_opens = first_flags = last_flags = 0; selected_mode = -1;
    emit_line = line;
    selected_fix = ~0u;
    fix_calls = 0;
    memset(fix_seq, 0xFF, sizeof(fix_seq));
    fixed_lines = -1;
}

static void test_each_model_dispatches_without_extra_capture_or_workspace(void)
{
    unsigned count;
    const VjoVocrModel *models = vjo_vocr_models(&count);
    VjoPlatform platform = {0}; platform.file_open = absent_model;
    VjoGreySource image = {NULL, WIDTH, HEIGHT, strip, NULL, NULL, NULL};
    unsigned char capture[4096];
    TEST_CHECK(vjo_vocr_strip_rows(WIDTH, HEIGHT) == HEIGHT && vjo_vocr_capture_bytes(WIDTH, HEIGHT) <= sizeof(capture));
    for (unsigned i = 0; i < count; i++) {
        selected_model = models[i].name;
        size_t size = vjo_vocr_workspace_bytes(models[i].name, WIDTH, HEIGHT);
        size_t cap = vjo_vocr_capture_bytes(WIDTH, HEIGHT);
        void *workspace = NULL;
        TEST_ASSERT(posix_memalign(&workspace, 16, size) == 0);
        VjoVocrStats stats; char text[16] = "old";
        reset(0);
        TEST_CHECK(vjo_vocr_ocr(&platform, "absent", models[i].name, &image, workspace, size, capture, cap,
                                text, sizeof(text), &stats) == VJO_OK);
        /* the model's own finder (every pinned model of rows uses textbox recovery, 3; the vertical
         * model the column finder, P1 with the dark flags left to the job: VALLEYS | NO_DARK) */
        int expected = (int)models[i].finder_mode, cols = !strcmp(models[i].name, VJO_VOCR_VERTICAL_NAME);
        TEST_CHECK_(expected == (cols ? (int)(VJO_VOCR_FINDER_COLUMNS | VOCR_COLUMNS_VALLEYS | VOCR_COLUMNS_NO_DARK)
                                      : VOCR_LINES_TEXTBOX_RECOVERY) &&
                    finder_calls == (unsigned)!cols && column_calls == (unsigned)cols && selected_mode == expected &&
                    !model_opens && stats.columns == (unsigned)cols, "%s", models[i].name);
        TEST_CHECK(cols ? selected_fix == ~0u : selected_fix == 0u); /* API 19's job (no policy): the frozen
                                                                    * finder's lines */
        TEST_CHECK(stats.workspace == size && stats.capture == cap && !stats.model_loaded && !text[0]);
        /* One strip holds the region: read once (the first pass, whole: FIRST and LAST),
         * then once more to see that it did not change; the finder's passes hit the strip. */
        TEST_CHECK(strips == 2 && first_flags == 1 && last_flags == 1 && stats.strips == 2 && !stats.tears);
        for (unsigned y = 0; y < rh; y++) TEST_CHECK(row_reads[y] == 2);
        /* A detected line must request this exact artifact and refuse a missing
         * file without publishing text or changing the arena. */
        reset(1);
        strcpy(text, "old");
        TEST_CHECK(vjo_vocr_ocr(&platform, "absent", models[i].name, &image, workspace, size, capture, cap,
                                text, sizeof(text), &stats) == VJO_E_OCR_MODEL);
        TEST_CHECK(finder_calls + column_calls == 1 && selected_mode == expected && model_opens == 1);
        TEST_CHECK(stats.workspace == size && !stats.model_loaded && !text[0]);
        /* Too small a capture block is refused before anything is read. */
        reset(0);
        TEST_CHECK(vjo_vocr_ocr(&platform, "absent", models[i].name, &image, workspace, size, capture, cap - 1,
                                text, sizeof(text), &stats) == VJO_E_OOM && !strips);
        free(workspace);
    }
}


/* The model's file is absent; the text-presence gate's weights come from VJO_VOCR_GATE_TEST_DIR when present. */
static PosixPlatform posix;
static VjoPlatform posix_plat;
static int gate_or_absent(void *user, const char *path, VjoFile *out)
{
    char real[512];
    (void)user;
    if (!strcmp(path, "absent/" VJO_VOCR_GATE_NAME) && VJO_VOCR_GATE_TEST_DIR[0]) {
        snprintf(real, sizeof(real), "%s/%s", VJO_VOCR_GATE_TEST_DIR, VJO_VOCR_GATE_NAME);
        return posix_plat.file_open(posix_plat.ud, real, out);
    }
    model_opens++;
    return -1;
}
static void *heap_alloc(void *ud, size_t n) { (void)ud; return malloc(n); }

static int policy_job(unsigned w, unsigned h, int check, unsigned fix, int gate, VjoVocrStats *st)
{
    static VjoVocrGateResident gate_res;
    VjoPlatform platform = {0};
    VjoGreySource image = {NULL, w, h, strip, NULL, NULL, NULL};
    VjoVocrResident res = {heap_alloc, NULL, NULL, NULL, NULL, 0, 0, 0};
    VjoVocrPolicy pol;
    static unsigned char capture[64 * 1024];
    char text[16];
    void *ws = NULL;
    size_t size = vjo_vocr_workspace_bytes(VJO_VOCR5_N_NAME, w, h);
    int rc;
    platform.file_open = gate_or_absent;
    rw = w;
    rh = h;
    memset(&pol, 0, sizeof(pol));
    pol.vertical_check = check;
    pol.line_fix = fix;
    if (gate) {
        memset(&gate_res, 0, sizeof(gate_res));
        pol.gate = 1;
        pol.gate_res = &gate_res;
    }
    TEST_ASSERT(size && posix_memalign(&ws, 16, size) == 0);
    rc = vjo_vocr_ocr_policy(&platform, "absent", VJO_VOCR5_N_NAME, &image, &res, ws, size, capture,
                             vjo_vocr_capture_bytes(w, h), &pol, text, sizeof(text), st);
    free(ws);
    rw = WIDTH;
    rh = HEIGHT;
    return rc;
}

/* API 50 beside API 47's auto (VjoVocrPolicy.vertical_check): auto's rule was chosen on the frozen finder's rows, so
 * a region at least as tall as wide is checked on them; where they do not look vertical, its rows are found again
 * with the fixes, and those are the rows read. A wider region is found once, with the fixes; without the check, or
 * without the fixes, every region is found once. The text-presence gate takes each row once, in the first pass
 * (audit-vocr6-finder-j J2: the second pass dropped, the frozen check on every region, the gate fed twice). */
static void test_auto_checks_tall_regions_on_the_frozen_rows(void)
{
    VjoVocrStats st;
    int rc;
    posix_platform_init(&posix, &posix_plat);
    selected_model = VJO_VOCR5_N_NAME;
    /* a wide region: one pass, with the fixes */
    reset(1);
    rc = policy_job(WIDTH, HEIGHT, 1, VOCR_FIX_DEFAULT, 0, &st);
    TEST_CHECK_(rc == VJO_E_OCR_MODEL && fix_calls == 1 && fix_seq[0] == VOCR_FIX_DEFAULT && st.lines == 1,
                "rc %d, %u calls, fix %u, %u lines", rc, fix_calls, fix_seq[0], st.lines);
    /* a tall region whose frozen row is no column: checked frozen, then its fixed rows (two) are the ones read */
    reset(1);
    fixed_lines = 2;
    rc = policy_job(32, 48, 1, VOCR_FIX_DEFAULT, 0, &st);
    TEST_CHECK_(rc == VJO_E_OCR_MODEL && fix_calls == 2 && fix_seq[0] == 0u && fix_seq[1] == VOCR_FIX_DEFAULT &&
                    st.lines == 2 && model_opens == 1,
                "rc %d, %u calls (fix %u, %u), %u lines", rc, fix_calls, fix_seq[0], fix_seq[1], st.lines);
    /* a tall region whose frozen rows look like a column: vertical text, no second pass, no weights opened */
    reset(2);
    fixed_lines = 1;
    rc = policy_job(32, 48, 1, VOCR_FIX_DEFAULT, 0, &st);
    TEST_CHECK_(rc == VJO_E_VERTICAL && fix_calls == 1 && fix_seq[0] == 0u && !model_opens,
                "rc %d, %u calls, fix %u", rc, fix_calls, fix_seq[0]);
    /* without the check, or without the fixes: one pass, as asked */
    reset(1);
    fixed_lines = 2;
    rc = policy_job(32, 48, 0, VOCR_FIX_DEFAULT, 0, &st);
    TEST_CHECK_(rc == VJO_E_OCR_MODEL && fix_calls == 1 && fix_seq[0] == VOCR_FIX_DEFAULT && st.lines == 2,
                "no check: rc %d, %u calls, fix %u", rc, fix_calls, fix_seq[0]);
    reset(1);
    rc = policy_job(32, 48, 1, 0u, 0, &st);
    TEST_CHECK_(rc == VJO_E_OCR_MODEL && fix_calls == 1 && fix_seq[0] == 0u, "no fix: rc %d, %u calls", rc, fix_calls);
    /* with the text-presence gate: it takes each of the region's rows once, though the finder ran twice */
    if (!VJO_VOCR_GATE_TEST_DIR[0]) {
        TEST_SKIP("no text-presence gate weights (VJO_VOCR_GATE_TEST_DIR)");
        return;
    }
    reset(1);
    fixed_lines = 2;
    rc = policy_job(32, 48, 1, VOCR_FIX_DEFAULT, 1, &st);
    TEST_CHECK_(fix_calls == 2 && fix_seq[0] == 0u && fix_seq[1] == VOCR_FIX_DEFAULT && !st.gate_rc && st.gate_ran &&
                    st.gate_rows == 48,
                "gate: rc %d, %u calls, gate rc %d ran %u rows %u", rc, fix_calls, st.gate_rc, st.gate_ran, st.gate_rows);
}

TEST_LIST = {
    {"model dispatch: strips, one check read, one unchanged workspace and capture block", test_each_model_dispatches_without_extra_capture_or_workspace},
    {"API 50 with auto: a tall region is checked on the frozen rows, then read with the fixes", test_auto_checks_tall_regions_on_the_frozen_rows},
    {NULL, NULL}
};
