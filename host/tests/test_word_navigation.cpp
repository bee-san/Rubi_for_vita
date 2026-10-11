#include <cstring>
#include "acutest.h"
#include "../../shell/word_navigation.h"

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
static void unmatched_and_end(void)
{
    const char *text = "猫犬。ABC";
    size_t len = std::strlen(text);
    VjoWordNavigation history = {};
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 0, 0, 1, 1) == 3);
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, 6, 1, 1) == 3);
    TEST_CHECK(history.count == 1); /* trailing punctuation must not reselect the tail */
    history.count = 0;
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, len, 1, 0) == 3);
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, 6, 1, 0) == 6);
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, 5, 1, 1) == 3); /* bad UTF-8 end */
    TEST_CHECK(vjo_word_cursor_step(history, text, len, 3, len + 1, 1, 1) == 3);
    TEST_CHECK(vjo_word_cursor_step(history, "", 0, 0, 0, 1, 1) == 0);
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
TEST_LIST = {
    {"matched_inflection_and_backward_history", matched_source_and_back},
    {"unmatched_invalid_range_and_sentence_end", unmatched_and_end},
    {"supplementary_Han_and_bounded_history", supplementary_and_history_bound},
    {NULL, NULL}
};
