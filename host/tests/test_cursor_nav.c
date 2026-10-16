/* The overlay's cursor over a recognized text of several lines, with the d-pad
 * and the left stick (owner, 2026-10-16 21:33Z, items 1 and 3), on layout
 * fixtures (host/tests/layout_double.h: rows of ten full-width cells, 24 px in
 * a 240 px box): two lines, three lines, a line of one word, an empty line, a
 * line with nothing Japanese, one line soft-wrapped onto two rows, a word that
 * runs on after a line break, and a text on one row.
 *
 * - The d-pad (item 1): Right from a line's last word goes to the next line's
 *   first word, and Left from a line's first word to the previous line's last
 *   character (after a Right, back to the word it came from: the retrace),
 *   across soft-wrapped and hard-broken rows alike. At the text's first and
 *   last selectable character the cursor stays (test.3's item G: no wrap to the
 *   far end), except on a text whose selectable characters are all on one row,
 *   which keeps #81's wrap.
 * - The stick (item 3): its four directions move the cursor as the d-pad's four
 *   arrows do: up/down a row, keeping the d-pad's column; left/right the d-pad's
 *   steps, the shown word skipped and Rights retraced; a diagonal by its
 *   dominant axis; the word looked up only once the cursor rests (#156). At a
 *   one-row text's ends it stops instead of wrapping, as a held stick stops at
 *   the ends of the Overlay app's lists.
 *
 * shell/overlay.cpp input_overlay is paf code no host target builds: nav_frame
 * restates its calls around the real helpers (core/text_rows.c,
 * core/stick_cursor.h). The texts are synthetic. */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "layout_double.h"
#include "stick_cursor.h"
#include "text_rows.h"
#include "utf.h"

#define W 240.0f /* 10 full-width cells */
#define JA 24.0f
#define NARROW 12.0f
#define HZ60 16667u
#define REST 200000u /* stick_lookup_ms's default, us */
#define MID 128

static Layout lay;

/* A word of the fake dictionary: a lookup at `start` matches [start, end). */
typedef struct {
    size_t start, end;
} Span;

/* The overlay over one text: s_nav, s_stick, s_cursor_byte, and what the
 * worker shows (its cursor and the match there, as render() copies them). */
typedef struct {
    const char *s;
    size_t len;
    VjoTextRows rows;
    int laid;                /* the header is laid out (header_rows) */
    int jp, word;            /* Japanese-only; Whole word (else Character) */
    const Span *words;
    int n_words;
    VjoRowNav nav;
    VjoStickCursor st;
    uint64_t now;
    size_t cursor;
    size_t shown, shown_end; /* the worker's cursor, its match's end (0: none) */
    int immediate, deferred; /* lookups: the d-pad's at once, the stick's once it rests */
} Nav;

/* The cursor posted and its lookup shown (the worker's 35-112 ms are not
 * modelled: the user waits for each d-pad lookup). render() then copies it;
 * outside Whole word mode it drops Right's history. */
static void post(Nav *n)
{
    n->shown = n->cursor;
    n->shown_end = 0;
    for (int i = 0; i < n->n_words; i++)
        if (n->words[i].start == n->cursor)
            n->shown_end = n->words[i].end;
    if (!n->word)
        n->nav.words.count = 0;
}

/* render()'s s_match_end: the shown match's end when it starts at the cursor (Whole word). */
static size_t match_end(const Nav *n)
{
    return n->word && n->shown == n->cursor ? n->shown_end : 0;
}

/* The overlay open on s at cursor, its word shown (the open's lookup). */
static Nav nav_open(const char *s, float width, const Span *words, int n_words, size_t cursor, int jp, int word)
{
    Nav n;
    memset(&n, 0, sizeof(n));
    layout_text(&lay, s, width, JA, NARROW);
    n.s = s;
    n.len = strlen(s);
    n.rows = layout_rows(&lay);
    n.laid = 1;
    n.jp = jp;
    n.word = word;
    n.words = words;
    n.n_words = n_words;
    n.now = 5000000u;
    n.st.rest_us = REST;
    n.cursor = cursor;
    post(&n);
    return n;
}

/* One frame of shell/overlay.cpp input_overlay over the header: the d-pad's
 * arrow pressed in it (dx Left/Right, dy Up/Down: -1, 0 or 1) and the left
 * stick at (lx, ly), dt after the last frame. The stick's step, when no arrow
 * takes the frame, is an arrow's for vjo_text_row_key (`stick` set). */
static void nav_frame(Nav *n, int dx, int dy, uint8_t lx, uint8_t ly, uint64_t dt)
{
    int direction = dx, vertical = dy, stick;
    size_t to = n->cursor;
    n->now += dt;
    stick = vjo_stick_cursor_step(vjo_stick_cursor(&n->st, lx, ly, n->now), &direction, &vertical);
    if (direction || vertical)
        vjo_text_row_key(&n->nav, n->s, n->len, n->laid ? &n->rows : NULL, n->cursor, match_end(n), direction,
                         vertical, n->jp, stick != 0, &to);
    if (stick)
        vjo_stick_cursor_moved(&n->st, to != n->cursor, n->now);
    if (to != n->cursor) {
        n->cursor = to;
        if (!stick) {
            n->st.due = 0; /* post_cursor_now */
            post(n);
            n->immediate++;
        } else if (!n->word)
            n->nav.words.count = 0; /* render(0) */
        vjo_stick_cursor_shown(&n->st, n->shown == n->cursor);
    }
    if (vjo_stick_cursor_lookup(&n->st, n->now)) {
        post(n);
        n->deferred++;
    }
}

/* The four directions: left, right, up, down; the d-pad's arrows and the stick pushed all the way. */
static const int ARROW[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
static const uint8_t STICK[4][2] = {{0, MID}, {255, MID}, {MID, 0}, {MID, 255}};
static const char *const NAMES[4] = {"left", "right", "up", "down"};

/* A d-pad press, then a frame with nothing pressed. */
static void press(Nav *n, int d)
{
    nav_frame(n, ARROW[d][0], ARROW[d][1], MID, MID, HZ60);
    nav_frame(n, 0, 0, MID, MID, HZ60);
}

/* The stick held for us at 60 Hz. */
static void hold(Nav *n, uint8_t lx, uint8_t ly, uint64_t us)
{
    for (uint64_t at = 0; at < us; at += HZ60)
        nav_frame(n, 0, 0, lx, ly, HZ60);
}

/* A flick: one frame pushed, then let go long enough for the rest (333 ms). */
static void flick(Nav *n, uint8_t lx, uint8_t ly)
{
    nav_frame(n, 0, 0, lx, ly, HZ60);
    hold(n, MID, MID, 20 * HZ60);
}

/* ---- the fixtures (byte offsets in the comments; jp: Japanese-only) ---- */

/* 猫0 が3 好6 き9 で12 す15 。18 \n21 | 犬22 も25 好28 き31 で34 す37 。40, len 43 */
static const char TWO[] = "猫が好きです。\n犬も好きです。";
static const Span TWO_WORDS[] = {{0, 3}, {3, 6}, {6, 12}, {12, 18}, {22, 25}, {25, 28}, {28, 34}, {34, 40}};
/* 猫0 で3 す6 \n9 | 犬10 で13 す16 \n19 | 鳥20 で23 す26, len 29 */
static const char THREE[] = "猫です\n犬です\n鳥です";
static const Span THREE_WORDS[] = {{0, 3}, {3, 9}, {10, 13}, {13, 19}, {20, 23}, {23, 29}};
/* 猫0 が3 好6 き9 で12 す15 \n18 | 犬19 \n22 | そ23 う26 で29 す32 ね35, len 38 */
static const char ONEWORD[] = "猫が好きです\n犬\nそうですね";
static const Span ONEWORD_WORDS[] = {{0, 3}, {3, 6}, {6, 12}, {12, 18}, {19, 22}, {23, 29}, {29, 35}, {35, 38}};
/* 猫0 が3 好6 き9 \n12 | \n13 | 犬14 も17 好20 き23, len 26 */
static const char EMPTY[] = "猫が好き\n\n犬も好き";
static const Span EMPTY_WORDS[] = {{0, 3}, {3, 6}, {6, 12}, {14, 17}, {17, 20}, {20, 26}};
/* 猫0 で3 す6 \n9 | O10 K11 ?12 (space)13 1 14 2 15 3 16 \n17 | 犬18 で21 す24, len 27 */
static const char LATIN[] = "猫です\nOK? 123\n犬です";
static const Span LATIN_WORDS[] = {{0, 3}, {3, 9}, {18, 21}, {21, 27}};
/* one recognized line on two rows: 猫0 が3 好6 き9 で12 す15 。18 犬21 も24 好27 | き30 で33 す36 。39, len 42 */
static const char SOFT[] = "猫が好きです。犬も好きです。";
static const Span SOFT_WORDS[] = {{0, 3}, {3, 6}, {6, 12}, {12, 18}, {21, 24}, {24, 27}, {27, 33}, {33, 39}};
/* a word run on after a line break (たべ / る, looked up whole): わ0 た3 し6 を9 た12 べ15 \n18 | る19 か22 な25, len 28 */
static const char CROSS[] = "わたしをたべ\nるかな";
static const Span CROSS_WORDS[] = {{0, 9}, {9, 12}, {12, 22}, {22, 28}};
/* one row: 猫0 が3 好6 き9 で12 す15 。18, len 21 */
static const char ONE[] = "猫が好きです。";
static const Span ONE_WORDS[] = {{0, 3}, {3, 6}, {6, 12}, {12, 18}};
/* a second line with no Japanese: 猫0 が3 好6 き9 で12 す15 \n18 | O19 K20, len 21 */
static const char ONE_JP[] = "猫が好きです\nOK";

#define WORDS(a) a, (int)(sizeof(a) / sizeof(a[0]))

/* Right/Left presses from `from`, the cursor after each against want[] (-1 ends). */
static void presses(Nav *n, int d, const int *want)
{
    for (int i = 0; want[i] >= 0; i++) {
        size_t at = n->cursor;
        press(n, d);
        TEST_CHECK_(n->cursor == (size_t)want[i], "%s from %zu: %zu, expected %d", NAMES[d], at, n->cursor, want[i]);
    }
}

/* ---- item 1: the d-pad ---- */

static void two_lines_right_and_left_cross_the_break(void)
{
    Nav n = nav_open(TWO, W, WORDS(TWO_WORDS), 15, 1, 0);
    TEST_ASSERT(lay.lines == 2 && lay.line[6] == 0 && lay.line[8] == 1);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 22, "Right from line 1's last: %zu, expected line 2's first (22)", n.cursor);
    TEST_CHECK(n.immediate == 1 && n.shown == n.cursor && n.nav.wraps == 0);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 15, "Left from line 2's first: %zu, expected line 1's last (15)", n.cursor);
    /* the whole text's ends: the cursor stays (test.3's item G) */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 37, 1, 0);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 37 && n.immediate == 0, "Right from the last: %zu", n.cursor);
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 0, 1, 0);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 0 && n.immediate == 0, "Left from the first: %zu (item G: no wrap to the far right)",
                n.cursor);
    TEST_CHECK(n.nav.wraps == 0);
    /* Whole word: です skipped with its 。, onto line 2; Left retraces it */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 12, 1, 1);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 22, "Right from です: %zu, expected 犬 (22)", n.cursor);
    press(&n, 0);
    TEST_CHECK(n.cursor == 12);
    press(&n, 0);
    TEST_CHECK(n.cursor == 9); /* no history: one character back */
    /* every word of both lines, then nothing; Left retraces each, then stops at the first */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 0, 1, 1);
    static const int right[] = {3, 6, 12, 22, 25, 28, 34, 34, -1}, left[] = {28, 25, 22, 12, 6, 3, 0, 0, -1};
    presses(&n, 1, right);
    presses(&n, 0, left);
    TEST_CHECK(n.nav.wraps == 0 && n.nav.words.count == 0);
    /* Up/Down as before: the column kept, nothing above the first row or below the last */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 0);
    static const int down[] = {28, 28, -1}, up[] = {6, 6, -1};
    presses(&n, 3, down);
    presses(&n, 2, up);
    TEST_CHECK(n.nav.row_moves == 2);
}

static void three_lines_walk_to_the_ends_and_stop(void)
{
    Nav n = nav_open(THREE, W, WORDS(THREE_WORDS), 0, 1, 0);
    TEST_ASSERT(lay.lines == 3);
    static const int right[] = {3, 6, 10, 13, 16, 20, 23, 26, 26, -1};
    static const int left[] = {23, 20, 16, 13, 10, 6, 3, 0, 0, -1};
    presses(&n, 1, right);
    presses(&n, 0, left);
    TEST_CHECK(n.nav.wraps == 0);
    /* Whole word: です and its line break skipped, onto the next line's first word */
    n = nav_open(THREE, W, WORDS(THREE_WORDS), 0, 1, 1);
    static const int words[] = {3, 10, 13, 20, 23, 23, -1}, back[] = {20, 13, 10, 3, 0, 0, -1};
    presses(&n, 1, words);
    presses(&n, 0, back);
    /* Up/Down through the three lines in one column */
    n = nav_open(THREE, W, WORDS(THREE_WORDS), 3, 1, 0);
    static const int down[] = {13, 23, 23, -1}, up[] = {13, 3, 3, -1};
    presses(&n, 3, down);
    presses(&n, 2, up);
}

static void a_line_of_one_word(void)
{
    Nav n = nav_open(ONEWORD, W, WORDS(ONEWORD_WORDS), 15, 1, 0);
    TEST_ASSERT(lay.lines == 3);
    static const int right[] = {19, 23, -1}, left[] = {19, 15, -1};
    presses(&n, 1, right);
    presses(&n, 0, left);
    n = nav_open(ONEWORD, W, WORDS(ONEWORD_WORDS), 12, 1, 1);
    static const int words[] = {19, 23, 29, -1}, back[] = {23, 19, 12, -1};
    presses(&n, 1, words);
    presses(&n, 0, back);
    /* Up/Down through the one-word row keep ね's column (cell 4) on both sides */
    n = nav_open(ONEWORD, W, WORDS(ONEWORD_WORDS), 35, 1, 0);
    static const int up[] = {19, 12, -1}, down[] = {19, 35, -1};
    presses(&n, 2, up);
    presses(&n, 3, down);
}

static void an_empty_line_and_a_line_without_japanese(void)
{
    Nav n = nav_open(EMPTY, W, WORDS(EMPTY_WORDS), 9, 1, 0);
    TEST_ASSERT(lay.lines == 3 && lay.line[4] == 0 && lay.line[5] == 1 && lay.line[6] == 2);
    static const int right[] = {14, -1}, left[] = {9, -1};
    presses(&n, 1, right);
    presses(&n, 0, left);
    n = nav_open(EMPTY, W, WORDS(EMPTY_WORDS), 6, 1, 1); /* 好き */
    static const int words[] = {14, 17, 20, 20, -1};
    presses(&n, 1, words);
    n = nav_open(EMPTY, W, WORDS(EMPTY_WORDS), 6, 1, 0);
    static const int down[] = {20, 20, -1}, up[] = {6, 6, -1};
    presses(&n, 3, down);
    presses(&n, 2, up);
    /* Japanese-only: the Latin line holds nothing selectable and is passed over */
    n = nav_open(LATIN, W, WORDS(LATIN_WORDS), 6, 1, 0);
    TEST_ASSERT(lay.lines == 3);
    static const int jright[] = {18, -1}, jleft[] = {6, -1};
    presses(&n, 1, jright);
    presses(&n, 0, jleft);
    n = nav_open(LATIN, W, WORDS(LATIN_WORDS), 3, 1, 0);
    static const int jdown[] = {21, -1};
    presses(&n, 3, jdown);
    /* off: its characters are visited, its space and the line breaks never */
    n = nav_open(LATIN, W, WORDS(LATIN_WORDS), 6, 0, 0);
    static const int oright[] = {10, 11, 12, 14, 15, 16, 18, -1}, oleft[] = {16, 15, 14, 12, 11, 10, 6, -1};
    presses(&n, 1, oright);
    presses(&n, 0, oleft);
}

/* One recognized line wrapped onto two rows: its rows are crossed as a line break is. */
static void soft_wrapped_rows_are_crossed_too(void)
{
    Nav n = nav_open(SOFT, W, WORDS(SOFT_WORDS), 27, 1, 0);
    TEST_ASSERT(lay.lines == 2 && lay.line[9] == 0 && lay.line[10] == 1);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 30, "Right from row 0's last: %zu, expected row 1's first (30)", n.cursor);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 27, "Left from row 1's first: %zu, expected row 0's last (27)", n.cursor);
    n = nav_open(SOFT, W, WORDS(SOFT_WORDS), 36, 1, 0);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 36, "Right from the last: %zu", n.cursor);
    n = nav_open(SOFT, W, WORDS(SOFT_WORDS), 0, 1, 0);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 0, "Left from the first: %zu", n.cursor);
    /* Whole word: 好き runs on to row 1; Right lands after its tail there, and Left comes back */
    n = nav_open(SOFT, W, WORDS(SOFT_WORDS), 27, 1, 1);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 33, "Right from 好き: %zu, expected で after its tail (33)", n.cursor);
    press(&n, 0);
    TEST_CHECK(n.cursor == 27);
}

/* A match that ends after a line break (a word looked up whole across it, as
 * overlay-cross-line-lookup's lookups give): Right lands on the first
 * selectable character at or after the match's end, on the next line, never
 * on that line's first character or round to the text's start; Left retraces. */
static void a_match_ending_after_a_line_break(void)
{
    Nav n = nav_open(CROSS, W, WORDS(CROSS_WORDS), 12, 1, 1);
    TEST_ASSERT(lay.lines == 2 && lay.line[6] == 0 && lay.line[7] == 1);
    press(&n, 1);
    TEST_CHECK_(n.cursor == 22, "Right from たべ/る: %zu, expected か (22)", n.cursor);
    press(&n, 0);
    TEST_CHECK(n.cursor == 12);
    n = nav_open(CROSS, W, WORDS(CROSS_WORDS), 9, 1, 1);
    static const int words[] = {12, 22, 22, -1};
    presses(&n, 1, words);
    /* the stick's Right from the shown word skips it the same way */
    n = nav_open(CROSS, W, WORDS(CROSS_WORDS), 12, 1, 1);
    flick(&n, 255, MID);
    TEST_CHECK_(n.cursor == 22 && n.deferred == 1, "the stick's right from たべ/る: %zu", n.cursor);
}

/* A text whose selectable characters are on one row keeps #81's wrap with
 * the d-pad; the stick stops at its ends. */
static void one_row_keeps_its_wrap(void)
{
    Nav n = nav_open(ONE, W, WORDS(ONE_WORDS), 15, 1, 0);
    TEST_ASSERT(lay.lines == 1);
    press(&n, 1);
    TEST_CHECK(n.cursor == 0 && n.nav.wraps == 1);
    press(&n, 0);
    TEST_CHECK(n.cursor == 15 && n.nav.wraps == 2);
    n = nav_open(ONE, W, WORDS(ONE_WORDS), 12, 1, 1);
    press(&n, 1);
    TEST_CHECK(n.cursor == 0); /* です and the 。 after it: round to the first word */
    press(&n, 0);
    TEST_CHECK(n.cursor == 12); /* the wrap retraced */
    n = nav_open(ONE, W, WORDS(ONE_WORDS), 15, 1, 0);
    flick(&n, 255, MID);
    TEST_CHECK(n.cursor == 15 && n.deferred == 0 && n.st.moves == 0);
    n = nav_open(ONE, W, WORDS(ONE_WORDS), 0, 1, 0);
    flick(&n, 0, MID);
    TEST_CHECK(n.cursor == 0 && n.deferred == 0);
    /* a second line with nothing Japanese: with Japanese-only one row of selectable characters, which wraps */
    n = nav_open(ONE_JP, W, NULL, 0, 15, 1, 0);
    TEST_ASSERT(lay.lines == 2);
    press(&n, 1);
    TEST_CHECK(n.cursor == 0 && n.nav.wraps == 1);
    /* off: two rows, crossed, and the end stops */
    n = nav_open(ONE_JP, W, NULL, 0, 15, 0, 0);
    static const int right[] = {19, 20, 20, -1};
    presses(&n, 1, right);
    TEST_CHECK(n.nav.wraps == 0);
}

/* Before the header is laid out (or when paf gives a character no position),
 * the line breaks stand for the rows: the ends of a text with one stop, the
 * ends of a text without one wrap. The press counts as one without a layout
 * only when it needed one. */
static void without_a_layout_the_line_breaks_decide(void)
{
    Nav n = nav_open(TWO, W, WORDS(TWO_WORDS), 15, 1, 0);
    n.laid = 0;
    press(&n, 1);
    TEST_CHECK(n.cursor == 22 && n.nav.no_layout == 0);
    n.cursor = 37;
    press(&n, 1);
    TEST_CHECK_(n.cursor == 37 && n.nav.no_layout == 1, "Right from the last, no layout: %zu (%d)", n.cursor,
                n.nav.no_layout);
    n.cursor = 0;
    press(&n, 0);
    TEST_CHECK(n.cursor == 0 && n.nav.no_layout == 2);
    press(&n, 3); /* Down: no layout, no move */
    TEST_CHECK(n.cursor == 0 && n.nav.no_layout == 3);
    n = nav_open(ONE, W, WORDS(ONE_WORDS), 15, 1, 0);
    n.laid = 0;
    press(&n, 1);
    TEST_CHECK(n.cursor == 0 && n.nav.no_layout == 1);
    /* a position paf does not give: the same */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 37, 1, 0);
    lay.fail_unit = 0; /* 猫, the text's first */
    press(&n, 1);
    TEST_CHECK(n.cursor == 37 && n.nav.no_layout == 1);
    lay.fail_unit = -1;
    /* the stick is never counted */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 37, 1, 0);
    n.laid = 0;
    flick(&n, 255, MID);
    flick(&n, MID, 255);
    TEST_CHECK(n.cursor == 37 && n.nav.no_layout == 0);
}

/* ---- item 3: the stick moves as the d-pad does ---- */

/* From every selectable character of a fixture, each direction: one d-pad press
 * and one flick of the stick go to the same character, the rule's (restated over
 * the laid-out cells), except at a one-row text's ends, where the d-pad wraps and
 * the stick stays. The d-pad's word is looked up at once, the stick's once the
 * cursor rests, once. */
static unsigned same_as_the_dpad(const char *name, const char *s, float width, const Span *words, int n_words,
                                 int jp, int word)
{
    unsigned checks = 0;
    layout_text(&lay, s, width, JA, NARROW);
    unsigned n_chars = lay.n;
    for (unsigned k = 0; k < n_chars; k++) {
        layout_text(&lay, s, width, JA, NARROW);
        if (!layout_selectable(&lay, k, jp))
            continue;
        size_t c = lay.begin[k], len = strlen(s);
        for (int d = 0; d < 4; d++) {
            Nav a = nav_open(s, width, words, n_words, c, jp, word);
            Nav b = nav_open(s, width, words, n_words, c, jp, word);
            size_t end = match_end(&a), from = c + 1, want;
            int wrapped = 0;
            if (end > c && end <= len && (end == len || vjo_utf8_cursor_step(s, len, end, 0) == end))
                from = end;
            want = d < 2 ? ref_read_step(&lay, c, d ? from : c + 1, ARROW[d][0], jp, 1, &wrapped)
                         : ref_vertical(&lay, c, ARROW[d][1], jp, -1.0f);
            press(&a, d);
            TEST_CHECK_(a.cursor == want, "%s jp %d word %d: the d-pad's %s from %zu: %zu, expected %zu", name, jp,
                        word, NAMES[d], c, a.cursor, want);
            TEST_CHECK(a.immediate == (a.cursor != c) && a.deferred == 0);
            nav_frame(&b, 0, 0, STICK[d][0], STICK[d][1], HZ60);
            TEST_CHECK(b.immediate == 0 && b.deferred == 0);
            hold(&b, MID, MID, 20 * HZ60);
            size_t stick_want = wrapped ? c : want;
            TEST_CHECK_(b.cursor == stick_want, "%s jp %d word %d: the stick's %s from %zu: %zu, expected %zu", name,
                        jp, word, NAMES[d], c, b.cursor, stick_want);
            TEST_CHECK_(b.deferred == (b.cursor != c) && b.immediate == 0 && (b.cursor == c || b.shown == b.cursor),
                        "%s: the stick's %s from %zu: %d deferred lookups", name, NAMES[d], c, b.deferred);
            checks += 2;
        }
    }
    return checks;
}

static void the_stick_moves_as_the_dpad_does(void)
{
    unsigned checks = 0;
    for (int word = 0; word <= 1; word++) {
        checks += same_as_the_dpad("two lines", TWO, W, WORDS(TWO_WORDS), 1, word);
        checks += same_as_the_dpad("three lines", THREE, W, WORDS(THREE_WORDS), 1, word);
        checks += same_as_the_dpad("a one-word line", ONEWORD, W, WORDS(ONEWORD_WORDS), 1, word);
        checks += same_as_the_dpad("an empty line", EMPTY, W, WORDS(EMPTY_WORDS), 1, word);
        for (int jp = 0; jp <= 1; jp++)
            checks += same_as_the_dpad("no Japanese line", LATIN, W, WORDS(LATIN_WORDS), jp, word);
        checks += same_as_the_dpad("soft-wrapped", SOFT, W, WORDS(SOFT_WORDS), 1, word);
        checks += same_as_the_dpad("a word after a break", CROSS, W, WORDS(CROSS_WORDS), 1, word);
        checks += same_as_the_dpad("one row", ONE, W, WORDS(ONE_WORDS), 1, word);
        checks += same_as_the_dpad("narrow box", THREE, 48.0f, WORDS(THREE_WORDS), 1, word);
    }
    printf("\n  %u d-pad and stick moves compared\n", checks);
}

/* Held, the stick walks the text in each direction as repeated presses do, by
 * #156's clock, and stops at its ends: each step the next character in reading
 * order across the lines, no lookup while it moves, one lookup where it stops
 * (held against the end: while still held). Up/down: a row a step, the column
 * kept, through the short row. */
static void the_stick_held_crosses_lines_and_stops(void)
{
    Nav n = nav_open(THREE, W, WORDS(THREE_WORDS), 0, 1, 0);
    static const size_t order[] = {0, 3, 6, 10, 13, 16, 20, 23, 26};
    size_t seen = 0;
    for (uint64_t at = 0; at < 3000000u; at += HZ60) {
        size_t was = n.cursor;
        nav_frame(&n, 0, 0, 255, MID, HZ60);
        if (n.cursor != was) {
            seen++;
            TEST_CHECK_(seen < 9 && n.cursor == order[seen], "step %zu at %zu", seen, n.cursor);
        }
    }
    TEST_CHECK_(n.cursor == 26 && seen == 8, "held right: at %zu after %zu steps", n.cursor, seen);
    TEST_CHECK(n.deferred == 1 && n.shown == 26 && n.immediate == 0);
    hold(&n, 0, MID, 3000000u);
    TEST_CHECK_(n.cursor == 0 && n.deferred == 2 && n.shown == 0, "held left: at %zu, %d lookups", n.cursor,
                n.deferred);
    /* down and up through the rows in で's column, then held against the last row */
    n = nav_open(ONEWORD, W, WORDS(ONEWORD_WORDS), 35, 1, 0);
    hold(&n, MID, MID, HZ60);
    hold(&n, MID, 0, 2000000u);
    TEST_CHECK_(n.cursor == 12 && n.deferred == 1 && n.shown == 12, "held up from ね: %zu", n.cursor);
    hold(&n, MID, MID, HZ60);
    hold(&n, MID, 255, 2000000u);
    TEST_CHECK_(n.cursor == 35 && n.deferred == 2, "held down from で: %zu (the column kept)", n.cursor);
    TEST_CHECK(n.nav.row_moves == 0 && n.st.moves == 4);
    /* Whole word: the shown word skipped on the first step, then one character a step (no lookup on the way) */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    static const size_t words[] = {6, 12, 15, 22, 25, 28, 31, 34, 37};
    seen = 0;
    for (uint64_t at = 0; at < 3000000u; at += HZ60) {
        size_t was = n.cursor;
        nav_frame(&n, 0, 0, 255, MID, HZ60);
        if (n.cursor != was) {
            seen++;
            TEST_CHECK_(seen < 9 && n.cursor == words[seen], "word step %zu at %zu", seen, n.cursor);
        }
    }
    TEST_CHECK(n.cursor == 37 && n.deferred == 1);
}

/* A diagonal moves as its dominant axis's arrow (the farther from the centre,
 * 0-128); a tie goes along the text. */
static void diagonals_take_the_dominant_axis(void)
{
    static const struct {
        uint8_t lx, ly;
        int d;
    } pushes[] = {{255, 40, 1},  {255, 216, 1}, {0, 40, 0},  {0, 216, 0}, {40, 0, 2},    {216, 0, 2},
                  {40, 255, 3},  {216, 255, 3}, {230, 60, 1}, {60, 230, 3}, {255, 0, 2},  /* y 128 against x 127 */
                  {0, 255, 0},   /* x 128 against y 127 */
                  {255, 255, 1}, {0, 0, 0},     {255, 1, 1}};  /* ties: along the text */
    for (unsigned i = 0; i < sizeof(pushes) / sizeof(pushes[0]); i++) {
        Nav a = nav_open(THREE, W, WORDS(THREE_WORDS), 13, 1, 0); /* で of line 2 */
        Nav b = nav_open(THREE, W, WORDS(THREE_WORDS), 13, 1, 0);
        press(&a, pushes[i].d);
        flick(&b, pushes[i].lx, pushes[i].ly);
        TEST_CHECK_(b.cursor == a.cursor && a.cursor != 13, "(%d, %d): %zu, the d-pad's %s %zu", pushes[i].lx,
                    pushes[i].ly, b.cursor, NAMES[pushes[i].d], a.cursor);
        TEST_CHECK(b.deferred == 1 && b.immediate == 0);
    }
}

/* The stick and the d-pad share Right's history: either's Left retraces either's Rights. */
static void the_stick_and_the_dpad_share_their_steps(void)
{
    Nav n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1); /* 好き shown */
    flick(&n, 255, MID);
    TEST_CHECK(n.cursor == 12 && n.deferred == 1); /* 好き skipped, です looked up once it rests */
    flick(&n, 0, MID);
    TEST_CHECK_(n.cursor == 6, "the stick's left after its right: %zu, expected 好 (6), as the d-pad's Left", n.cursor);
    /* two d-pad Rights, two flicks left */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    press(&n, 1);
    press(&n, 1);
    TEST_CHECK(n.cursor == 22);
    flick(&n, 0, MID);
    TEST_CHECK_(n.cursor == 12, "a flick left after the d-pad's Rights: %zu, expected 12", n.cursor);
    flick(&n, 0, MID);
    TEST_CHECK(n.cursor == 6);
    /* two quick flicks right (12 not looked up yet: one character), then the d-pad's Lefts */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    nav_frame(&n, 0, 0, 255, MID, HZ60);
    hold(&n, MID, MID, 3 * HZ60);
    nav_frame(&n, 0, 0, 255, MID, HZ60);
    TEST_CHECK(n.cursor == 15 && n.deferred == 0);
    hold(&n, MID, MID, 20 * HZ60);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 12, "Left after two flicks: %zu, expected 12", n.cursor);
    press(&n, 0);
    TEST_CHECK(n.cursor == 6);
    /* Up/Down forget it, for both */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    flick(&n, 255, MID);
    flick(&n, MID, 255);
    TEST_CHECK(n.cursor == 34 && n.nav.words.count == 0);
}

/* The overlay's close line counts the arrows' row moves, wraps and presses
 * without a layout; the stick's moves are its own (#156's B2). */
static void the_close_line_counts_are_the_arrows(void)
{
    Nav n = nav_open(THREE, W, WORDS(THREE_WORDS), 3, 1, 0);
    flick(&n, MID, 255);
    flick(&n, MID, 255);
    flick(&n, MID, 0);
    TEST_CHECK(n.cursor == 13 && n.nav.row_moves == 0 && n.st.moves == 3);
    press(&n, 3);
    press(&n, 2);
    TEST_CHECK(n.nav.row_moves == 2);
    n = nav_open(ONE, W, WORDS(ONE_WORDS), 15, 1, 0);
    flick(&n, 255, MID);
    TEST_CHECK(n.nav.wraps == 0);
    press(&n, 1);
    TEST_CHECK(n.nav.wraps == 1);
}

TEST_LIST = {
    {"two_lines_right_and_left_cross_the_break", two_lines_right_and_left_cross_the_break},
    {"three_lines_walk_to_the_ends_and_stop", three_lines_walk_to_the_ends_and_stop},
    {"a_line_of_one_word", a_line_of_one_word},
    {"an_empty_line_and_a_line_without_japanese", an_empty_line_and_a_line_without_japanese},
    {"soft_wrapped_rows_are_crossed_too", soft_wrapped_rows_are_crossed_too},
    {"a_match_ending_after_a_line_break", a_match_ending_after_a_line_break},
    {"one_row_keeps_its_wrap", one_row_keeps_its_wrap},
    {"without_a_layout_the_line_breaks_decide", without_a_layout_the_line_breaks_decide},
    {"the_stick_moves_as_the_dpad_does", the_stick_moves_as_the_dpad_does},
    {"the_stick_held_crosses_lines_and_stops", the_stick_held_crosses_lines_and_stops},
    {"diagonals_take_the_dominant_axis", diagonals_take_the_dominant_axis},
    {"the_stick_and_the_dpad_share_their_steps", the_stick_and_the_dpad_share_their_steps},
    {"the_close_line_counts_are_the_arrows", the_close_line_counts_are_the_arrows},
    {NULL, NULL}};
