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
 *   arrows do: up/down a row, keeping the d-pad's column; left/right in reading
 *   order across the rows, but a character a step whatever word is shown (the
 *   owner, 2026-10-20: the d-pad goes word by word, the stick character by
 *   character); its rights are in the d-pad's history, its left never retraces;
 *   a diagonal by its dominant axis; the word starting at the cursor looked up
 *   only once the cursor rests (#156). At a one-row text's ends it stops instead
 *   of wrapping, as a held stick stops at the ends of the Rubi app's lists.
 * - Taps go through the real hit test (core/text_hit.c), which with Japanese
 *   characters only OFF can select an ideographic space no step lands on.
 *
 * shell/overlay.cpp input_overlay is paf code no host target builds: nav_frame
 * restates its calls around the real helpers (core/text_rows.c,
 * core/stick_cursor.h). The texts are synthetic. */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "layout_double.h"
#include "stick_cursor.h"
#include "text_hit.h"
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

/* A tap on rendered character k of the laid-out text (just inside its cell's start), as
 * shell/overlay.cpp input_header_touch takes it: the real hit test (core/text_hit.c) over the
 * header's rows, here the double's, 30 px a row; a hit on another character than the cursor's
 * moves the cursor there, starts a new path (no history) and is looked up at once. 1: a hit. */
static int tap(Nav *n, unsigned k)
{
    VjoTextHitLayout hit = {0.0f, 0.0f, lay.width, 30.0f * (float)lay.lines, 30.0f * (float)lay.lines, 0.0f, JA,
                            lay.lines, layout_position, &lay};
    size_t byte = n->len;
    if (!vjo_text_hit(n->s, n->len, &hit, lay.x[k] + 1.0f, 30.0f * (float)lay.line[k] + 15.0f, n->jp, &byte))
        return 0;
    if (byte != n->cursor) {
        n->cursor = byte;
        n->nav.words.count = 0; /* a tap starts a new navigation path */
        n->st.due = 0;          /* post_cursor_now */
        post(n);
        n->immediate++;
    }
    return 1;
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
/* the same with です。 shown at で, to the text's end: the d-pad's Right from で goes round */
static const Span ONE_TO_END[] = {{0, 3}, {3, 6}, {6, 12}, {12, 21}};
/* a second line with no Japanese: 猫0 が3 好6 き9 で12 す15 \n18 | O19 K20, len 21 */
static const char ONE_JP[] = "猫が好きです\nOK";
/* the owner's example (2026-10-20), with the bundled dictionary's match at every character:
 * 昨0 日3 す6 き9 焼12 き15 を18 食21 べ24 ま27 し30 た33, len 36 */
static const char OWNER[] = "昨日すき焼きを食べました";
static const Span OWNER_WORDS[] = {{0, 6},   {3, 6},   {6, 18},  {9, 12},  {12, 18}, {15, 18},
                                   {18, 21}, {21, 36}, {24, 27}, {27, 36}, {30, 36}, {33, 36}};
/* 昨0 日3 、6 す9 き12 焼15 き18 を21 食24 べ27 ま30 し33 た36 。39, len 42 */
static const char OWNER_P[] = "昨日、すき焼きを食べました。";
static const Span OWNER_P_WORDS[] = {{0, 6},   {3, 6},   {9, 21},  {12, 15}, {15, 21}, {18, 21}, {21, 24},
                                     {24, 39}, {27, 30}, {30, 39}, {33, 39}, {36, 39}};
/* an ideographic space (U+3000), which a tap may select but no arrow or stick step lands on, two rows:
 * 昨0 日3 U+3000 6 す9 き12 焼15 き18 を21 食24 べ27 | ま30 し33 た36, len 39; no entry at the space */
static const char SPACE[] = "昨日\xe3\x80\x80すき焼きを食べました";
static const Span SPACE_WORDS[] = {{0, 6},   {3, 6},   {9, 21},  {12, 15}, {15, 21}, {18, 21}, {21, 24},
                                   {24, 39}, {27, 30}, {30, 39}, {33, 39}, {36, 39}};

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
    /* the stick's right from the shown word goes a character, to べ (the owner, 2026-10-20) */
    n = nav_open(CROSS, W, WORDS(CROSS_WORDS), 12, 1, 1);
    flick(&n, 255, MID);
    TEST_CHECK_(n.cursor == 15 && n.deferred == 1, "the stick's right from たべ/る: %zu", n.cursor);
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

/* ---- item 3: the stick moves as the d-pad does, its left/right a character at a time ---- */

/* From every selectable character of a fixture, each direction: one d-pad press
 * and one flick of the stick go where their rules say, restated over the laid-out
 * cells. Up/down: the same row step for both. Left/right: the d-pad's Left/Right
 * (in Whole word mode its Right skips the shown word; round a one-row text's
 * ends), the stick's the next or previous selectable character whatever is shown,
 * never round the ends (the owner, 2026-10-20). The d-pad's word is looked up at
 * once, the stick's once the cursor rests, once. */
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
            size_t end = match_end(&a), from = c + 1, want, stick_want;
            int wrapped = 0, stick_wrapped;
            if (end > c && end <= len && (end == len || vjo_utf8_cursor_step(s, len, end, 0) == end))
                from = end;
            want = d < 2 ? ref_read_step(&lay, c, d ? from : c + 1, ARROW[d][0], jp, 1, &wrapped)
                         : ref_vertical(&lay, c, ARROW[d][1], jp, -1.0f);
            stick_want = d < 2 ? ref_read_step(&lay, c, c + 1, ARROW[d][0], jp, 0, &stick_wrapped) : want;
            press(&a, d);
            TEST_CHECK_(a.cursor == want, "%s jp %d word %d: the d-pad's %s from %zu: %zu, expected %zu", name, jp,
                        word, NAMES[d], c, a.cursor, want);
            TEST_CHECK(a.immediate == (a.cursor != c) && a.deferred == 0);
            nav_frame(&b, 0, 0, STICK[d][0], STICK[d][1], HZ60);
            TEST_CHECK(b.immediate == 0 && b.deferred == 0);
            hold(&b, MID, MID, 20 * HZ60);
            TEST_CHECK_(b.cursor == stick_want, "%s jp %d word %d: the stick's %s from %zu: %zu, expected %zu", name,
                        jp, word, NAMES[d], c, b.cursor, stick_want);
            TEST_CHECK_(b.deferred == (b.cursor != c) && b.immediate == 0 && (b.cursor == c || b.shown == b.cursor),
                        "%s: the stick's %s from %zu: %d deferred lookups", name, NAMES[d], c, b.deferred);
            /* where they differ: the d-pad's wrap, or its Right past a word longer than a character */
            TEST_CHECK(b.cursor == a.cursor || wrapped || (d == 1 && word && from > c + 1));
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
        for (int jp = 0; jp <= 1; jp++) {
            checks += same_as_the_dpad("the owner's example", OWNER, W, WORDS(OWNER_WORDS), jp, word);
            checks += same_as_the_dpad("punctuated", OWNER_P, 120.0f, WORDS(OWNER_P_WORDS), jp, word);
        }
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
    /* Whole word: one character a step from the first, the shown 好き not skipped (no lookup on the way) */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    static const size_t words[] = {6, 9, 12, 15, 22, 25, 28, 31, 34, 37};
    seen = 0;
    for (uint64_t at = 0; at < 3000000u; at += HZ60) {
        size_t was = n.cursor;
        nav_frame(&n, 0, 0, 255, MID, HZ60);
        if (n.cursor != was) {
            seen++;
            TEST_CHECK_(seen < 10 && n.cursor == words[seen], "word step %zu at %zu", seen, n.cursor);
        }
    }
    TEST_CHECK(n.cursor == 37 && n.deferred == 1 && seen == 9);
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

/* The stick and the d-pad share Right's history (the owner, 2026-10-20, for the stick's own
 * steps): the stick's rights, a character each, are recorded as any Right, and the d-pad's Left
 * retraces them and then the d-pad's words; the stick's left steps a character back, never
 * retracing a word, and the Right that started where it lands leaves the history. */
static void the_stick_and_the_dpad_share_their_steps(void)
{
    Nav n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1); /* 好き shown */
    flick(&n, 255, MID);
    TEST_CHECK_(n.cursor == 9 && n.deferred == 1, "the stick's right from 好き: %zu, expected き (9)", n.cursor);
    TEST_CHECK(n.nav.words.count == 1);
    flick(&n, 0, MID);
    TEST_CHECK_(n.cursor == 6 && n.nav.words.count == 0, "the stick's left after its right: %zu", n.cursor);
    /* two d-pad Rights (words), then flicks left: a character each, not the d-pad's words */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    press(&n, 1);
    press(&n, 1);
    TEST_CHECK(n.cursor == 22 && n.nav.words.count == 2);
    flick(&n, 0, MID);
    TEST_CHECK_(n.cursor == 15 && n.nav.words.count == 2,
                "a flick left after the d-pad's Rights: %zu, expected す (15), a character back", n.cursor);
    flick(&n, 0, MID);
    TEST_CHECK(n.cursor == 12 && n.nav.words.count == 1); /* で, where the second Right started */
    press(&n, 0);
    TEST_CHECK_(n.cursor == 6, "the d-pad's Left: %zu, expected 好 (6), the first Right retraced", n.cursor);
    /* inside a word the d-pad skipped, the d-pad's Left goes back to its start */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    press(&n, 1);
    press(&n, 1);
    flick(&n, 0, MID);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 12, "the d-pad's Left from す: %zu, expected です's で (12)", n.cursor);
    /* two quick flicks right (one character each), then the d-pad's Lefts retrace them */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    nav_frame(&n, 0, 0, 255, MID, HZ60);
    hold(&n, MID, MID, 3 * HZ60);
    nav_frame(&n, 0, 0, 255, MID, HZ60);
    TEST_CHECK(n.cursor == 12 && n.deferred == 0);
    hold(&n, MID, MID, 20 * HZ60);
    press(&n, 0);
    TEST_CHECK_(n.cursor == 9, "Left after two flicks: %zu, expected 9", n.cursor);
    press(&n, 0);
    TEST_CHECK(n.cursor == 6);
    /* Up/Down forget it, for both */
    n = nav_open(TWO, W, WORDS(TWO_WORDS), 6, 1, 1);
    flick(&n, 255, MID);
    flick(&n, MID, 255);
    TEST_CHECK(n.cursor == 31 && n.nav.words.count == 0);
}

/* The owner's example (2026-10-20 08:37Z): "lets say i have a sentence like 昨日すき焼きを食べました. I
 * go to word 食べました. I want to go to ました." The d-pad goes there word by word; then the stick:
 * right once is べ, twice ま, each looked up from that character once the cursor rests (the
 * worker's lookup: the longest match starting there, here the bundled dictionary's matches, which
 * host/tests/test_word_lookup.cpp looks up), and ました is shown. Then on, a character a step, to
 * the last; the d-pad's Right from ました, whose match reaches the text's end, goes nowhere on two
 * rows and round to the first on one. The stick held through the punctuated sentence lands on every
 * selectable character, 、 and 。 too with Japanese-only off, and is looked up once, where it stops. */
static void the_owners_example_a_character_at_a_time(void)
{
    for (int width = 0; width < 2; width++)
        for (int jp = 0; jp <= 1; jp++) {
            float w = width ? W : 920.0f; /* 240 px: two rows, ま the first's last; 920 px: one row */
            TEST_CASE_("%.0f px, Japanese-only %d", w, jp);
            Nav n = nav_open(OWNER, w, WORDS(OWNER_WORDS), 0, jp, 1);
            TEST_ASSERT(lay.lines == (width ? 2u : 1u));
            static const int words[] = {6, 18, 21, -1};
            presses(&n, 1, words);
            TEST_CHECK(n.shown == 21 && n.shown_end == 36 && n.immediate == 3); /* 食べました */
            flick(&n, 255, MID);
            TEST_CHECK_(n.cursor == 24 && n.deferred == 1 && n.shown == 24 && n.shown_end == 27,
                        "right once: %zu, its lookup from %zu to %zu", n.cursor, n.shown, n.shown_end);
            flick(&n, 255, MID);
            TEST_CHECK_(n.cursor == 27 && n.deferred == 2 && n.shown == 27 && n.shown_end == 36,
                        "twice: %zu, its lookup from %zu to %zu (ました)", n.cursor, n.shown, n.shown_end);
            /* the d-pad's Right from ました: nowhere on two rows, round to 昨 on one */
            Nav d = n;
            press(&d, 1);
            TEST_CHECK(width ? d.cursor == 27 && d.nav.wraps == 0 : d.cursor == 0 && d.nav.wraps == 1);
            /* the stick on to た, the last, where it stays (on one row too) */
            static const size_t on[] = {30, 33, 33};
            for (int i = 0; i < 3; i++) {
                flick(&n, 255, MID);
                TEST_CHECK_(n.cursor == on[i], "on %d: %zu", i, n.cursor);
            }
            TEST_CHECK(n.deferred == 4 && n.nav.wraps == 0 && n.immediate == 3);
            /* and back: the stick's left a character a step to 食, the d-pad's Left then retraces its words */
            for (int i = 0; i < 4; i++)
                flick(&n, 0, MID);
            TEST_CHECK(n.cursor == 21 && n.nav.words.count == 3);
            static const int back[] = {18, 6, 0, -1};
            presses(&n, 0, back);
            /* held through the punctuated sentence: every selectable character in order, looked up
             * once, at the last, where it stops */
            n = nav_open(OWNER_P, w, WORDS(OWNER_P_WORDS), 0, jp, 1);
            const size_t jp_order[] = {0, 3, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36};
            const size_t all_order[] = {0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39};
            const size_t *order = jp ? jp_order : all_order;
            size_t steps = jp ? 11 : 13, seen = 0;
            for (uint64_t at = 0; at < 3000000u; at += HZ60) {
                size_t was = n.cursor;
                nav_frame(&n, 0, 0, 255, MID, HZ60);
                if (n.cursor != was) {
                    seen++;
                    TEST_CHECK_(seen <= steps && n.cursor == order[seen], "held, step %zu at %zu", seen, n.cursor);
                }
            }
            TEST_CHECK_(seen == steps && n.cursor == order[steps] && n.deferred == 1 && n.shown == order[steps],
                        "held: %zu steps to %zu, %d lookups", seen, n.cursor, n.deferred);
        }
}

/* Right's history as the stick's left needs it on a text of several rows: increasing, all before the cursor. */
static int history_in_order(const Nav *n)
{
    for (unsigned i = 0; i < n->nav.words.count; i++)
        if (n->nav.words.previous[i] >= n->cursor || (i && n->nav.words.previous[i - 1] >= n->nav.words.previous[i]))
            return 0;
    return 1;
}

/* Three edges of the stick's left (audit-stick-chars F1, N2 and its mutant M10):
 * - With Japanese characters only OFF, the default, a tap may select an ideographic space, where no
 *   arrow or stick step lands (core/text_hit.c takes any character above U+0020, vjo_text_selectable
 *   never U+3000). The d-pad's Right from it records it; the stick's left from す lands before it, on
 *   日, and takes it out of the history as it is after where the stick lands, so the d-pad's Left goes
 *   on back to 昨, never forward to the space. With ON the space takes no tap.
 * - On a text on one row the d-pad's Right from its last word goes round to its first, which leaves
 *   where it started after the cursor (the one case the history is not in order). The stick's right
 *   and left there take out only the stick's own entry: the d-pad's Left still undoes the wrap, back to
 *   で, rather than going round to the text's last character.
 * - A stick's left that stays (at the text's first character) still takes Up/Down's column afresh, as
 *   any Left/Right does: Down from 猫 goes by 猫's own cell, to あ, not by the column Up came with, い's. */
static void the_sticks_left_at_the_edges(void)
{
    for (int jp = 0; jp <= 1; jp++) {
        TEST_CASE_("a tapped space, Japanese-only %d", jp);
        Nav n = nav_open(SPACE, W, WORDS(SPACE_WORDS), 0, jp, 1);
        TEST_ASSERT(lay.lines == 2 && lay.cp[2] == 0x3000 && lay.begin[2] == 6);
        if (jp) {
            TEST_CHECK(!tap(&n, 2) && n.cursor == 0 && n.immediate == 0); /* ON: no hit on the space */
        } else {
            TEST_CHECK(tap(&n, 2) && n.cursor == 6 && n.shown == 6 && n.shown_end == 0);
            press(&n, 1);
            TEST_CHECK_(n.cursor == 9 && n.nav.words.count == 1 && n.nav.words.previous[0] == 6,
                        "the d-pad's Right from the space: %zu, %u in the history", n.cursor, n.nav.words.count);
            flick(&n, 0, MID);
            TEST_CHECK_(n.cursor == 3 && n.nav.words.count == 0 && history_in_order(&n),
                        "the stick's left from す: %zu with %u in the history, expected 日 (3) with none", n.cursor,
                        n.nav.words.count);
            press(&n, 0);
            TEST_CHECK_(n.cursor == 0, "the d-pad's Left from 日: %zu, expected 昨 (0), never the space (6)", n.cursor);
        }
        TEST_CASE_("one row, Japanese-only %d", jp);
        n = nav_open(ONE, W, WORDS(ONE_TO_END), 12, jp, 1); /* です。 shown at で, to the text's end */
        TEST_ASSERT(lay.lines == 1);
        press(&n, 1);
        TEST_CHECK(n.cursor == 0 && n.nav.wraps == 1 && n.nav.words.count == 1 && n.nav.words.previous[0] == 12);
        flick(&n, 255, MID);
        TEST_CHECK(n.cursor == 3 && n.nav.words.count == 2);
        flick(&n, 0, MID);
        TEST_CHECK_(n.cursor == 0 && n.nav.words.count == 1 && n.nav.words.previous[0] == 12,
                    "the stick's left back to 猫: %zu with %u in the history, expected [12]", n.cursor,
                    n.nav.words.count);
        press(&n, 0);
        TEST_CHECK_(n.cursor == 12 && n.nav.wraps == 1,
                    "the d-pad's Left: %zu, expected で (12), the wrap undone, not round to the last", n.cursor);
        TEST_CASE_("the column, Japanese-only %d", jp);
        n = nav_open("猫\nあいう", W, NULL, 0, 7, jp, 1); /* 猫0 \n3 | あ4 い7 う10 */
        TEST_ASSERT(lay.lines == 2);
        press(&n, 2);
        TEST_CHECK(n.cursor == 0 && n.nav.goal.at == 1); /* Up from い: 猫, い's column kept */
        flick(&n, 0, MID);
        TEST_CHECK(n.cursor == 0 && n.st.moves == 0 && n.nav.goal.at == 0); /* stays, the column dropped */
        press(&n, 3);
        TEST_CHECK_(n.cursor == 4, "Down after the stick's left at the first: %zu, expected あ (4) by 猫's own cell",
                    n.cursor);
    }
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

/* ---- line history (docs/line-history.md) ----
 * An earlier line is shown between the last row of the line before it and the first row of the line
 * after it, dimmed (core/styled.c vjo_styled_history), in the one header the layout lays out: the
 * cursor's units start `lead` units in (overlay.cpp header_shown). Up with no row of the text above
 * the cursor, and Down with none below on an earlier line, are what the overlay turns into this
 * game's line before or after it (input_overlay: a d-pad press whose vertical step stays); the dimmed
 * rows hold no character of the text, so the cursor never goes into them, and Left/Right stop at the
 * text's own ends. */
typedef struct {
    Layout *l;
    uint32_t lead;
} Shown;
static int shown_position(void *ctx, uint32_t unit, float *x, uint32_t *line)
{
    Shown *s = (Shown *)ctx;
    return layout_position(s->l, s->lead + unit, x, line);
}

/* One d-pad press over the header of an earlier line (history: its place, 0 the current text): the new
 * cursor, and the line step the overlay posts (-1 earlier, 1 later, 0 none). */
static size_t history_press(const char *text, VjoTextRows *rows, VjoRowNav *nav, size_t cursor, int dx, int dy,
                            int history, int *step)
{
    size_t to = cursor;
    int rc = vjo_text_row_key(nav, text, strlen(text), rows, cursor, 0, dx, dy, 0, 0, &to);
    *step = rc == VJO_ROW_STAY && !dx && (dy < 0 || history) ? dy : 0; /* VJO_CMD_EARLIER / VJO_CMD_LATER */
    return to;
}

static void up_and_down_past_an_earlier_lines_rows(void)
{
    /* the line before's last row, the earlier line (two rows), the line after's first row:
     * 二行目\n | 犬0 も3 好6 き9 \n12 | 鳥13 も16 \n | 三行目 (the text's bytes) */
    static const char header[] = "二行目\n犬も好き\n鳥も\n三行目";
    static const char text[] = "犬も好き\n鳥も";
    Shown sh = {&lay, 4}; /* "二行目\n": 4 units */
    VjoTextRows rows;
    VjoRowNav nav;
    int step;
    size_t at;
    memset(&nav, 0, sizeof(nav));
    TEST_ASSERT(layout_text(&lay, header, W, JA, NARROW) == 4);
    rows = layout_rows(&lay);
    rows.position = shown_position;
    rows.ctx = &sh;
    /* Up from the text's first row: no row of it above (the dimmed row has none): the line before */
    at = history_press(text, &rows, &nav, 3, 0, -1, 2, &step);
    TEST_CHECK(at == 3 && step == -1);
    /* Down from its first row: its second row, as on any text */
    at = history_press(text, &rows, &nav, 3, 0, 1, 2, &step);
    TEST_CHECK(at == 16 && step == 0);
    /* Down from its last row: the line after (an earlier line only) */
    at = history_press(text, &rows, &nav, 16, 0, 1, 2, &step);
    TEST_CHECK(at == 16 && step == 1);
    at = history_press(text, &rows, &nav, 16, 0, 1, 0, &step);
    TEST_CHECK(at == 16 && step == 0); /* the current text: Down stays, as before line history */
    /* Up from its last row: its first row */
    at = history_press(text, &rows, &nav, 16, 0, -1, 2, &step);
    TEST_CHECK(at == 3 && step == 0);
    /* Left/Right stop at the text's own ends: never in a dimmed row, never a line step */
    at = history_press(text, &rows, &nav, 0, -1, 0, 2, &step);
    TEST_CHECK(at == 0 && step == 0);
    at = history_press(text, &rows, &nav, 16, 1, 0, 2, &step);
    TEST_CHECK(at == 16 && step == 0);
    at = history_press(text, &rows, &nav, 9, 1, 0, 2, &step);
    TEST_CHECK(at == 13 && step == 0); /* across the text's own break */
    TEST_CHECK(!lay.bad_queries);
    /* the current text on one row (no neighbours): Up is the line before, as the owner asked */
    static const char one[] = "昨日すき焼きを食べました";
    sh.lead = 0;
    TEST_ASSERT(layout_text(&lay, one, 2 * W, JA, NARROW) == 1);
    rows = layout_rows(&lay);
    rows.position = shown_position;
    rows.ctx = &sh;
    at = history_press(one, &rows, &nav, 6, 0, -1, 0, &step);
    TEST_CHECK(at == 6 && step == -1);
    /* not laid out yet: no step (the overlay waits for the layout) */
    at = history_press(one, NULL, &nav, 6, 0, -1, 0, &step);
    TEST_CHECK(at == 6 && step == 0);
    TEST_CHECK(!lay.bad_queries);
}

/* ---- the source badge (core/styled.c vjo_styled_history: "[HOOK]" or "[OCR]" before the current text) ----
 * The badge is laid out on the text's first row, before it, in the one header: the cursor's units start
 * after it (lead), as after an earlier line's dimmed row. The cursor never goes into it, Up on the text's
 * top row is still the line before (no row of the text above), a tap on it selects nothing, and the
 * text's own characters are where taps and the arrows find them. */
static void the_source_badge_is_never_the_cursors(void)
{
    /* the ASCII badges (6 and 5 narrow cells) and an icon (source_badge = icons: U+E1BB, one full-width cell) */
    static const struct { const char *header; uint32_t lead; float width; } headers[] = {
        {"[HOOK]猫が好き。犬も好きだ", 6, 6 * NARROW}, {"[OCR]猫が好き。犬も好きだ", 5, 5 * NARROW},
        {"\xEE\x86\xBB猫が好き。犬も好きだ", 1, JA}};
    static const char text[] = "猫が好き。犬も好きだ"; /* 猫0 が3 好6 き9 。12 犬15 も18 好21 き24 だ27 */
    for (unsigned b = 0; b < sizeof(headers) / sizeof(headers[0]); b++) {
        Shown sh = {&lay, headers[b].lead};
        VjoTextRows rows;
        VjoRowNav nav;
        int step;
        size_t at, byte = 99;
        TEST_CASE(headers[b].header);
        memset(&nav, 0, sizeof(nav));
        /* 10 cells a row: the badge (3, 2.5 or 1 full-width) and 7, 7 or 9 of the text's 10 */
        TEST_ASSERT(layout_text(&lay, headers[b].header, W, JA, NARROW) == 2);
        rows = layout_rows(&lay);
        rows.position = shown_position;
        rows.ctx = &sh;
        /* Left from the text's first character: it stays (no badge cell to go to), no line step */
        at = history_press(text, &rows, &nav, 0, -1, 0, 0, &step);
        TEST_CHECK(at == 0 && step == 0);
        /* Right walks the text's characters only */
        at = history_press(text, &rows, &nav, 0, 1, 0, 0, &step);
        TEST_CHECK(at == 3 && step == 0);
        /* Up on the top row: the line before (line history), never the badge */
        at = history_press(text, &rows, &nav, 3, 0, -1, 0, &step);
        TEST_CHECK(at == 3 && step == -1);
        /* Down: the text's second row, as without a badge; Up comes back to the first */
        at = history_press(text, &rows, &nav, 3, 0, 1, 0, &step);
        TEST_CHECK(at > 12 && step == 0);
        at = history_press(text, &rows, &nav, at, 0, -1, 0, &step);
        TEST_CHECK(at <= 12 && step == 0);
        /* taps (overlay.cpp input_header_touch): on the badge, nothing; on 猫, its first byte */
        VjoTextHitLayout hit = {0, 0, W, 4 * JA, 2 * JA, 0, JA, 2, shown_position, &sh};
        TEST_CHECK(!vjo_text_hit(text, strlen(text), &hit, 1.0f, 2.0f, 0, &byte) && byte == 99);
        TEST_CHECK(!vjo_text_hit(text, strlen(text), &hit, headers[b].width - 1.0f, 2.0f, 0, &byte));
        TEST_CHECK(vjo_text_hit(text, strlen(text), &hit, headers[b].width + 1.0f, 2.0f, 0, &byte) && byte == 0);
        TEST_CHECK(!lay.bad_queries);
    }
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
    {"the_owners_example_a_character_at_a_time", the_owners_example_a_character_at_a_time},
    {"the_sticks_left_at_the_edges", the_sticks_left_at_the_edges},
    {"the_close_line_counts_are_the_arrows", the_close_line_counts_are_the_arrows},
    {"up_and_down_past_an_earlier_lines_rows", up_and_down_past_an_earlier_lines_rows},
    {"the_source_badge_is_never_the_cursors", the_source_badge_is_never_the_cursors},
    {NULL, NULL}};
