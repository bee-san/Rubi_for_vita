/* The actual core adapter with a finder spy, no weights and no inference. */
#include "acutest.h"
#include "vocr_ocr.h"
#include "vocr_lines.h"
#include "../../include/vjo_vocr_sizes.h"
#include <stdlib.h>
#include <string.h>

#define WIDTH 40u
#define HEIGHT 12u
static unsigned row_reads[HEIGHT], next_row, finder_calls, model_opens;
static int selected_mode, emit_line;
static const char *selected_model;

static int read_rows(void *user, unsigned y, unsigned n, unsigned char *dst)
{
    (void)user;
    TEST_CHECK(y == next_row && y + n <= HEIGHT);
    for (unsigned r = 0; r < n; r++) {
        row_reads[y + r]++;
        for (unsigned x = 0; x < WIDTH; x++) {
            *dst++ = 30; *dst++ = 100; *dst++ = 150; *dst++ = 255;
        }
    }
    next_row += n;
    return (int)n;
}

int vjo_test_find_lines(const VocrLinesImage *image, int finder, void *arena,
                        size_t bytes, VocrLineSet *out)
{
    finder_calls++; selected_mode = finder;
    TEST_CHECK(next_row == HEIGHT && image->width == WIDTH && image->height == HEIGHT);
    TEST_CHECK(image->bpp == VOCR_LINES_GREY && image->stride == WIDTH);
    TEST_CHECK(arena && bytes >= vocr_lines_arena_bytes(WIDTH, HEIGHT));
    unsigned char grey = (unsigned char)((77u * 30 + 150u * 100 + 29u * 150) >> 8);
    for (unsigned i = 0; i < WIDTH * HEIGHT; i++) TEST_CHECK(image->pixels[i] == grey);
    memset(out, 0, sizeof(*out));
    if (emit_line) {
        out->count = 1;
        out->lines[0] = (VocrLine){2, 2, 20, 6, 0};
    }
    return VOCR_LINES_OK;
}

static int absent_model(void *user, const char *path, VjoFile *out)
{
    (void)user; (void)out;
    TEST_CHECK(!strncmp(path, "absent/", 7) && !strcmp(path + 7, selected_model));
    model_opens++;
    return -1;
}

static void test_each_model_dispatches_without_extra_capture_or_workspace(void)
{
    unsigned count;
    const VjoVocrModel *models = vjo_vocr_models(&count);
    VjoPlatform platform = {0}; platform.file_open = absent_model;
    VjoOcrImage image = {NULL, WIDTH, HEIGHT, read_rows, NULL};
    for (unsigned i = 0; i < count; i++) {
        memset(row_reads, 0, sizeof(row_reads));
        next_row = finder_calls = model_opens = 0; selected_mode = -1; emit_line = 0;
        selected_model = models[i].name;
        size_t size = vjo_vocr_workspace_bytes(models[i].name, WIDTH, HEIGHT);
        void *workspace = NULL;
        TEST_ASSERT(posix_memalign(&workspace, 16, size) == 0);
        VjoVocrStats stats; char text[16] = "old";
        TEST_CHECK(vjo_vocr_ocr(&platform, "absent", models[i].name, &image, workspace,
                    size, text, sizeof(text), &stats) == VJO_OK);
        int expected = !strcmp(models[i].name, VJO_VOCR2_ARTIFACT_NAME) ? 2 : 0;
        TEST_CHECK(finder_calls == 1 && selected_mode == expected && !model_opens);
        TEST_CHECK(stats.workspace == size && !stats.model_loaded && !text[0]);
        for (unsigned y = 0; y < HEIGHT; y++) TEST_CHECK(row_reads[y] == 1);
        if (expected == 2)
            TEST_CHECK(size == vjo_vocr_workspace_bytes("FL10_w8.vocr", WIDTH, HEIGHT));
        /* A detected line must request this exact artifact and refuse a missing
         * file without publishing text, re-reading capture or changing the arena. */
        memset(row_reads, 0, sizeof(row_reads));
        next_row = finder_calls = model_opens = 0; selected_mode = -1; emit_line = 1;
        strcpy(text, "old");
        TEST_CHECK(vjo_vocr_ocr(&platform, "absent", models[i].name, &image, workspace,
                    size, text, sizeof(text), &stats) == VJO_E_OCR_MODEL);
        TEST_CHECK(finder_calls == 1 && selected_mode == expected && model_opens == 1);
        TEST_CHECK(stats.workspace == size && !stats.model_loaded && !text[0]);
        for (unsigned y = 0; y < HEIGHT; y++) TEST_CHECK(row_reads[y] == 1);
        free(workspace);
    }
}

TEST_LIST = {
    {"model dispatch reads each row once and uses one unchanged workspace", test_each_model_dispatches_without_extra_capture_or_workspace},
    {NULL, NULL}
};
