#include <math.h>
#include <string.h>
#include "acutest.h"
#include "text_hit.h"

typedef struct { uint32_t unit, line; float x; } Cell;
typedef struct { const Cell *cells; size_t count; int fail; unsigned calls; } Renderer;
static int position(void *ctx, uint32_t unit, float *x, uint32_t *line)
{
    Renderer *r = ctx;
    r->calls++;
    if (r->fail) return -1;
    for (size_t i = 0; i < r->count; i++) if (r->cells[i].unit == unit) {
        *x = r->cells[i].x; *line = r->cells[i].line; return 0;
    }
    /* Querying the low surrogate or a CR unit is a production error. */
    TEST_CHECK_(0, "unexpected rendered index %u", unit);
    return -1;
}
static VjoTextHitLayout layout(Renderer *r, unsigned lines)
{
    return (VjoTextHitLayout){20, 12, 920, 48, (float)lines * 24, 0, 24,
        lines, position, r};
}
static void proportional_wrapped_scrolled_and_clipped(void)
{
    const char *s = "A猫犬鳥魚";
    const Cell cells[] = {{0,0,0}, {1,0,12}, {2,0,36}, {3,1,0}, {4,1,24}};
    Renderer r = {cells, 5, 0, 0}; VjoTextHitLayout l = layout(&r, 2);
    size_t byte = 999;
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 34, 14, 1, &byte));
    TEST_CHECK(byte == 1); /* measured Latin advance is12, not24 */
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 57, 14, 1, &byte));
    TEST_CHECK(byte == 4);
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 21, 37, 1, &byte));
    TEST_CHECK(byte == 7);
    l.scroll = 24; l.clip_height = 24;
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 45, 13, 1, &byte));
    TEST_CHECK(byte == 10);
    TEST_CHECK(!vjo_text_hit(s, strlen(s), &l, 45, 36, 1, &byte));
    TEST_CHECK(!vjo_text_hit(s, strlen(s), &l, 19, 13, 1, &byte));
    TEST_CHECK(!vjo_text_hit(s, strlen(s), &l, 940, 13, 1, &byte));
    TEST_CHECK(!vjo_text_hit(s, strlen(s), &l, 900, 13, 1, &byte)); /* blank trailing area */
}
static void restricted_cells_never_jump_and_surrogates_keep_source_offsets(void)
{
    const char *s = "😀\r\n𠮷猫";
    const Cell cells[] = {{0,0,0}, {2,0,24}, {3,1,0}, {5,1,24}};
    Renderer r = {cells, 4, 0, 0}; VjoTextHitLayout l = layout(&r, 2);
    size_t byte = 999;
    TEST_CHECK(!vjo_text_hit(s, strlen(s), &l, 21, 13, 1, &byte));
    TEST_CHECK(byte == 999); /* emoji must not snap to the following Han */
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 21, 13, 0, &byte));
    TEST_CHECK(byte == 0);
    TEST_CHECK(!vjo_text_hit(s, strlen(s), &l, 45, 13, 0, &byte)); /* newline */
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 21, 37, 1, &byte));
    TEST_CHECK(byte == 6); /* CR has no glyph; supplementary Han starts UTF16unit3 */
    TEST_ASSERT(vjo_text_hit(s, strlen(s), &l, 45, 37, 1, &byte));
    TEST_CHECK(byte == 10);
    const char *latin = "A 猫";
    const Cell other[] = {{0,0,0}, {1,0,12}, {2,0,24}};
    r = (Renderer){other, 3, 0, 0}; l = layout(&r, 1);
    TEST_CHECK(!vjo_text_hit(latin, strlen(latin), &l, 21, 13, 1, &byte));
    TEST_CHECK(!vjo_text_hit(latin, strlen(latin), &l, 33, 13, 0, &byte));
    TEST_ASSERT(vjo_text_hit(latin, strlen(latin), &l, 45, 13, 1, &byte));
    TEST_CHECK(byte == 2);
}
static void unavailable_and_invalid_geometry_are_noops(void)
{
    const Cell cell[] = {{0,0,0}};
    Renderer r = {cell, 1, 1, 0}; VjoTextHitLayout l = layout(&r, 1);
    size_t byte = 123;
    TEST_CHECK(!vjo_text_hit("猫", 3, &l, 21, 13, 1, &byte));
    TEST_CHECK(byte == 123);
    r.fail = 0;
    TEST_CHECK(!vjo_text_hit("", 0, &l, 21, 13, 1, &byte));
    TEST_CHECK(!vjo_text_hit("猫", 3, &l, NAN, 13, 1, &byte));
    TEST_CHECK(!vjo_text_hit("猫", 3, &l, 21, NAN, 1, &byte));
    l.lines = 0;
    TEST_CHECK(!vjo_text_hit("猫", 3, &l, 21, 13, 1, &byte));
    l.lines = 1; l.content_height = 0;
    TEST_CHECK(!vjo_text_hit("猫", 3, &l, 21, 13, 1, &byte));
    TEST_CHECK(byte == 123);
}
TEST_LIST = {
    {"measured_positions_wrap_scroll_clip_and_blank_space", proportional_wrapped_scrolled_and_clipped},
    {"Japanese_restriction_CR_and_supplementary_positions", restricted_cells_never_jump_and_surrogates_keep_source_offsets},
    {"unavailable_layout_and_invalid_geometry_leave_selection_unchanged", unavailable_and_invalid_geometry_are_noops},
    {NULL, NULL}
};
