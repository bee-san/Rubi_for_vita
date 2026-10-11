/* Exercise the same recoloring helper used by the native header. This text
 * double caches its drawing until Invalidate(), as a native renderer may do;
 * it is not a substitute for visual verification on ScePaf hardware. */
#include <algorithm>
#include <vector>
#include "acutest.h"
#include "../../shell/header_highlight.h"
extern "C" {
#include "styled.h"
#include "utf.h"
}

enum { COLOR = 0, NORMAL = 1, HIGHLIGHT = 2 };
struct Text {
    std::vector<int> styles, drawing;
    bool ready = true;
    int calls = 0, invalidations = 0, fail_style = 0;
    bool fail_invalidate = false;
    explicit Text(uint32_t len) : styles(len, NORMAL), drawing(styles) {}
    int GetLength(uint32_t &len) {
        len = (uint32_t)styles.size();
        return ready ? 0 : -1;
    }
    int SetStyleAttribute(int attribute, uint32_t start, uint32_t len, int color) {
        TEST_CHECK(attribute == COLOR);
        ++calls;
        if (calls == fail_style || start > styles.size() || len > styles.size() - start)
            return -1;
        std::fill(styles.begin() + start, styles.begin() + start + len, color);
        return 0;
    }
    int Invalidate() {
        ++invalidations;
        if (fail_invalidate)
            return -1;
        drawing = styles;
        return 0;
    }
};
struct Applied {
    uint32_t start = 0, len = 0;
    bool dirty = false;
};
static bool recolor(Text *text, Applied &a, uint32_t start, uint32_t len)
{
    return vjo_header_recolor(text, COLOR, a.start, a.len, a.dirty,
                              start, len, (int)NORMAL, (int)HIGHLIGHT);
}
static void check_drawing(const Text &text, uint32_t start, uint32_t len)
{
    for (uint32_t i = 0; i < text.drawing.size(); ++i)
        TEST_CHECK(text.drawing[i] == (i >= start && i - start < len ? HIGHLIGHT : NORMAL));
}

static void character_to_word_and_back(void)
{
    Text text(10);
    Applied a;
    TEST_ASSERT(recolor(&text, a, 2, 1)); /* pending lookup */
    check_drawing(text, 2, 1);
    /* Style writes alone leave the displayed character cached. */
    TEST_ASSERT(text.SetStyleAttribute(COLOR, 2, 4, HIGHLIGHT) == 0);
    check_drawing(text, 2, 1);
    TEST_ASSERT(recolor(&text, a, 2, 4)); /* successful whole-word lookup */
    check_drawing(text, 2, 4);
    TEST_CHECK(a.start == 2 && a.len == 4 && !a.dirty);
    int calls = text.calls, invalidations = text.invalidations;
    TEST_ASSERT(recolor(&text, a, 2, 4));
    TEST_CHECK(text.calls == calls && text.invalidations == invalidations);
    TEST_ASSERT(recolor(&text, a, 7, 1)); /* next cursor / unsuccessful lookup */
    check_drawing(text, 7, 1);
    TEST_ASSERT(recolor(&text, a, 7, 2));
    check_drawing(text, 7, 2);
    TEST_ASSERT(recolor(&text, a, 7, 1)); /* Character mode */
    check_drawing(text, 7, 1);
    TEST_ASSERT(recolor(&text, a, 99, 0)); /* no selectable character */
    check_drawing(text, 0, 0);
    TEST_CHECK(a.start == 0 && a.len == 0);
}

static void layout_and_style_failures(void)
{
    Text text(10);
    Applied a;
    text.ready = false;
    TEST_CHECK(!recolor(&text, a, 2, 4));
    TEST_CHECK(text.calls == 0 && text.invalidations == 0 && a.len == 0);
    text.ready = true;
    TEST_ASSERT(recolor(&text, a, 2, 1));
    /* Failed clearing or failed coloring must not commit the requested range. */
    for (int step = 1; step <= 2; ++step) {
        text.fail_style = text.calls + step;
        TEST_CHECK(!recolor(&text, a, 2, 4));
        TEST_CHECK(a.start == 2 && a.len == 1 && a.dirty);
        text.fail_style = 0;
        TEST_ASSERT(recolor(&text, a, 2, 4));
        check_drawing(text, 2, 4);
        TEST_ASSERT(recolor(&text, a, 2, 1));
    }
    text.fail_invalidate = true;
    TEST_CHECK(!recolor(&text, a, 2, 4));
    TEST_CHECK(a.start == 2 && a.len == 1 && a.dirty);
    check_drawing(text, 2, 1);
    text.fail_invalidate = false;
    TEST_ASSERT(recolor(&text, a, 2, 4));
    check_drawing(text, 2, 4);
}

static void failed_update_superseded(void)
{
    Text text(10);
    Applied a;
    TEST_ASSERT(recolor(&text, a, 2, 1));
    text.fail_invalidate = true;
    TEST_CHECK(!recolor(&text, a, 2, 4)); /* styles changed, drawing did not */
    text.fail_invalidate = false;
    TEST_ASSERT(recolor(&text, a, 2, 1)); /* switch mode before the retry */
    check_drawing(text, 2, 1); /* no leftover yellow from the failed word */
    TEST_CHECK(!a.dirty);
}

static void utf16_source_span(void)
{
    const char *source = "😀\r食べました。";
    VjoEntry entry = {};
    entry.hl_start = 5; /* raw UTF-8 bytes, after emoji and CR */
    entry.hl_end = 20; /* all five source characters of the inflected verb */
    VjoEntryList list = {source, &entry, 1};
    uint32_t start = 0, len = 0, cursor_start = 0, cursor_len = 0;
    TEST_ASSERT(vjo_entry_header_range(&list, 0, &start, &len));
    TEST_CHECK(start == 2 && len == 5); /* emoji uses two UTF-16 units; CR omitted */
    vjo_utf8_cursor_range(source, 23, 5, &cursor_start, &cursor_len);
    TEST_CHECK(cursor_start == 2 && cursor_len == 1);
    Text text(8);
    Applied a;
    TEST_ASSERT(recolor(&text, a, cursor_start, cursor_len));
    TEST_ASSERT(recolor(&text, a, start, len));
    check_drawing(text, 2, 5);
}

static void invalid_range(void)
{
    Text text(10);
    Applied a;
    TEST_ASSERT(recolor(&text, a, 2, 1));
    int calls = text.calls;
    TEST_CHECK(!recolor(&text, a, UINT32_MAX, 2));
    TEST_CHECK(!recolor(&text, a, 9, UINT32_MAX));
    TEST_CHECK(!recolor((Text *)NULL, a, 2, 4));
    TEST_CHECK(text.calls == calls && a.len == 1 && !a.dirty);
    check_drawing(text, 2, 1);
}

TEST_LIST = {
    {"character_to_word_and_back", character_to_word_and_back},
    {"layout_and_style_failures", layout_and_style_failures},
    {"failed_update_superseded", failed_update_superseded},
    {"utf16_source_span", utf16_source_span},
    {"invalid_range", invalid_range},
    {NULL, NULL}
};
