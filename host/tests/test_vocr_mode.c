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

#define WIDTH 40u
#define HEIGHT 12u
static unsigned row_reads[HEIGHT], strips, finder_calls, column_calls, model_opens, first_flags, last_flags;
static int selected_mode, emit_line;
static unsigned selected_fix;
static const char *selected_model;
static const unsigned char grey = (unsigned char)((77u * 30 + 150u * 100 + 29u * 150) >> 8);

static int strip(void *user, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)user;
    TEST_CHECK(y + n <= HEIGHT && stride >= WIDTH);
    strips++;
    if (flags & VJO_STRIP_FIRST) first_flags++;
    if (flags & VJO_STRIP_LAST) last_flags++;
    for (unsigned r = 0; r < n; r++) {
        row_reads[y + r]++;
        memset(dst + (size_t)r * stride, grey, WIDTH);
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
    TEST_CHECK(bytes >= vocr_columns_arena_bytes(WIDTH, HEIGHT));
    return spy_finder(src, arena, bytes, out);
}

static int spy_finder(const VocrLinesRows *src, void *arena, size_t bytes, VocrLineSet *out)
{
    TEST_CHECK(src->width == WIDTH && src->height == HEIGHT && src->bpp == VOCR_LINES_GREY);
    TEST_CHECK(arena && bytes >= vocr_lines_arena_bytes(WIDTH, HEIGHT));
    /* the finder reads the region through the row callback, two passes */
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned y = 0; y < HEIGHT; y++) {
            const unsigned char *row = src->row(src->user, y);
            TEST_ASSERT(row != NULL);
            for (unsigned x = 0; x < WIDTH; x++) TEST_CHECK(row[x] == grey);
        }
    memset(out, 0, sizeof(*out));
    if (emit_line) {
        out->count = 1;
        out->lines[0] = (VocrLine){2, 2, 20, 6, 0};
    }
    return VOCR_LINES_OK;
}

/* API 44: the job's finder is the fixed one (vita-vn-ocr #129, runtime/vocr_lines_fix.h); fix 0 is the frozen
 * finder's lines, which the spy above stands for */
int vjo_test_find_lines_fix_rows(const VocrLinesRows *src, int finder, unsigned fix, void *arena, size_t bytes,
                                 VocrLineSet *out)
{
    selected_fix = fix;
    return vjo_test_find_lines_rows(src, finder, arena, bytes, out);
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
}

static void test_each_model_dispatches_without_extra_capture_or_workspace(void)
{
    unsigned count;
    const VjoVocrModel *models = vjo_vocr_models(&count);
    VjoPlatform platform = {0}; platform.file_open = absent_model;
    VjoGreySource image = {NULL, WIDTH, HEIGHT, strip, NULL, NULL};
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
        for (unsigned y = 0; y < HEIGHT; y++) TEST_CHECK(row_reads[y] == 2);
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

TEST_LIST = {
    {"model dispatch: strips, one check read, one unchanged workspace and capture block", test_each_model_dispatches_without_extra_capture_or_workspace},
    {NULL, NULL}
};
