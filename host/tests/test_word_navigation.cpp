#include <cstring>
#include "acutest.h"
#include "../../shell/word_navigation.h"
#include "layout_double.h"

static void matched_source_and_back(void)
{
    const char *text = "食べました、猫です。";
    size_t len = std::strlen(text), cursor = 0;
    VjoWordNavigation history = {};
    cursor = vjo_word_cursor_step(history, text, len, cursor, 15, 1, 1);
    TEST_CHECK(cursor == 18 && history.count == 1); /* original inflection + comma */
    cursor = vjo_word_cursor_step(history, text, len, cursor, 21, 1, 1);
    TEST_CHECK(cursor == 21 && history.count == 2);
    cursor = vjo_word_cursor_step(history, text, len, cursor, 0, -1, 1);
    TEST_CHECK(cursor == 18 && history.count == 1); /* even while a lookup is pending */
    cursor = vjo_word_cursor_step(history, text, len, cursor, 0, -1, 1);
    TEST_CHECK(cursor == 0 && history.count == 0);
}

/* Without a layout (the first frames after opening, or GetCharInfo
 * failing) the whole text is one row: the end wraps to the start. */
static void unlaid_text_is_one_row(void)
{
    const char *text = "猫犬。ABC";
    size_t len = std::strlen(text);
    VjoWordNavigation history = {};
    int rc = 0;
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 0, 0, 1, 1, NULL, &rc) == 3 && rc == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, 6, 1, 1) == 0); /* only punctuation/Latin after */
    TEST_CHECK(history.count == 2);
    history.count = 0;
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, len, 1, 0) == 0);   /* the match reaches the end */
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, 6, 1, 0) == 6);     /* 。 is a character when off */
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, 5, 1, 1) == 0);     /* bad UTF-8 end: one character */
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, len + 1, 1, 1) == 0);
    history.count = 0;
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 0, 0, -1, 1) == 3 && history.count == 0); /* wraps back */
    TEST_CHECK(vjo_word_cursor_step(history, "", 0, 0, 0, 1, 1) == 0);
    TEST_CHECK(vjo_word_cursor_step(history, text, len, len, 0, 1, 1) == len); /* no Japanese: nothing moves */
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 0, 0, 0, 1, NULL, &rc) == 0 && rc == VJO_ROW_STAY);
}

static void supplementary_and_history_bound(void)
{
    const char *text = "𠮷野家、猫";
    VjoWordNavigation history = {};
    size_t next = vjo_word_cursor_step(history, text, std::strlen(text), 0, 10, 1, 1);
    TEST_CHECK(next == 13);
    TEST_CHECK(vjo_word_cursor_step(history, text, std::strlen(text), next, 0, -1, 1) == 0);
    char letters[40];
    std::memset(letters, 'a', sizeof(letters));
    for (size_t i = 0; i < 30; ++i)
        TEST_CHECK(vjo_word_cursor_step(history, letters, sizeof(letters), i, i + 1, 1, 0) == i + 1);
    TEST_CHECK(history.count == 16 && history.previous[0] == 14);
    for (size_t i = 30; i > 14; --i)
        TEST_CHECK(vjo_word_cursor_step(history, letters, sizeof(letters), i, 0, -1, 0) == i - 1);
    TEST_CHECK(history.count == 0);
}

/* With the header's rows: Right wraps from the row's last word, Left
 * retraces it, and history from a wrap is undone exactly. */
static void row_wrap_and_retrace(void)
{
    static Layout l;
    /* 10 cells per row: 食べました、猫です。 | 犬も好き */
    const char *text = "食べました、猫です。犬も好き";
    size_t len = std::strlen(text);
    TEST_ASSERT(layout_text(&l, text, 240.0f, 24.0f, 12.0f) == 2);
    VjoTextRows rows = layout_rows(&l);
    VjoWordNavigation history = {};
    int rc = 0;
    size_t cursor = vjo_word_cursor_step(history, text, len, 0, 15, 1, 1, &rows, &rc);
    TEST_CHECK(cursor == 18 && rc == VJO_ROW_MOVED); /* 食べました, then 、 is skipped */
    cursor = vjo_word_cursor_step(history, text, len, cursor, 21, 1, 1, &rows, &rc);
    TEST_CHECK(cursor == 21 && rc == VJO_ROW_MOVED);
    cursor = vjo_word_cursor_step(history, text, len, cursor, 27, 1, 1, &rows, &rc); /* です; 。; row 1 */
    TEST_CHECK(cursor == 0 && rc == VJO_ROW_WRAPPED && history.count == 3);
    cursor = vjo_word_cursor_step(history, text, len, cursor, 0, -1, 1, &rows, &rc);
    TEST_CHECK(cursor == 21 && rc == VJO_ROW_MOVED); /* the wrap undone */
    cursor = vjo_word_cursor_step(history, text, len, cursor, 0, -1, 1, &rows, &rc);
    cursor = vjo_word_cursor_step(history, text, len, cursor, 0, -1, 1, &rows, &rc);
    TEST_CHECK(cursor == 0 && history.count == 0);
    cursor = vjo_word_cursor_step(history, text, len, cursor, 0, -1, 1, &rows, &rc);
    TEST_CHECK(cursor == 24 && rc == VJO_ROW_WRAPPED); /* す, the row's last letter */
    /* A word that fills the rest of its row: nowhere else to go. */
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 30, len, 1, 1, &rows, &rc) == 30 && rc == VJO_ROW_STAY);
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 30, 33, 1, 1, &rows, &rc) == 33);
    TEST_CHECK(l.bad_queries == 0);
}

/* L or R tapped alone pages dictionaries; the L + R chord never does. */
static void shoulder_taps(void)
{
    const uint32_t L = 0x100, R = 0x200, SELECT = 0x1, CROSS = 0x4000;
    uint32_t armed = 0;
    TEST_CHECK(vjo_shoulder_tap(armed, L, L, L, R) == 0 && armed == L);
    TEST_CHECK(vjo_shoulder_tap(armed, L, 0, L, R) == 0);      /* held */
    TEST_CHECK(vjo_shoulder_tap(armed, 0, 0, L, R) == -1 && armed == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, R, R, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 1);
    /* L then R (the close chord), R then L, both at once: nothing. */
    TEST_CHECK(vjo_shoulder_tap(armed, L, L, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L | R, R, L, R) == 0 && armed == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, R, 0, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, R, R, L, R) == 0 && vjo_shoulder_tap(armed, L | R, L, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L, 0, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L | R, L | R, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    /* Select + L (a trigger), or another button during the tap: nothing. */
    TEST_CHECK(vjo_shoulder_tap(armed, SELECT | L, L, L, R) == 0 && armed == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L, L, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L | CROSS, CROSS, L, R) == 0 && armed == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L, 0, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, L, L, L, R) == 0);
    TEST_CHECK(vjo_shoulder_tap(armed, CROSS, CROSS, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
    /* Held while the overlay opened: its press is never seen, so neither is a tap. */
    armed = 0;
    TEST_CHECK(vjo_shoulder_tap(armed, L, 0, L, R) == 0 && vjo_shoulder_tap(armed, 0, 0, L, R) == 0);
}

TEST_LIST = {
    {"matched_inflection_and_backward_history", matched_source_and_back},
    {"unlaid_text_is_one_row", unlaid_text_is_one_row},
    {"supplementary_Han_and_bounded_history", supplementary_and_history_bound},
    {"row_wrap_and_retrace", row_wrap_and_retrace},
    {"shoulder_taps", shoulder_taps},
    {NULL, NULL}
};
