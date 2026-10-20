/* The overlay's arrow keys over the header's displayed rows, on layout
 * fixtures from host/tests/layout_double.h: rows of 10 full-width cells (24 px
 * in a 240 px box), soft wraps, hard line breaks, mixed full-width/narrow
 * advances, kinsoku hanging punctuation, CR, UTF-16 surrogate pairs,
 * unavailable layouts. Up/Down go to the row above or below (#81 request 1);
 * Left/Right go in reading order across rows and stop at the text's ends,
 * except on a text whose selectable characters are all on one row, which the
 * d-pad goes round (the owner, 2026-10-16, item 1; it replaces #81 request 2's
 * wrap at each row's ends). A differential check then compares every key from
 * every character of generated texts with the rules restated over the laid-out
 * cells (ref_read_step/ref_vertical), and the left stick's left/right, a
 * character at a time whatever word is shown (the owner, 2026-10-20), with the
 * same rule without the wrap. host/tests/test_cursor_nav.c drives the
 * same code as the overlay's frame, with the d-pad and the left stick. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "acutest.h"
#include "text_rows.h"
#include "layout_double.h"

#define W 240.0f /* 10 full-width cells */
#define JA 24.0f
#define NARROW 12.0f

static Layout lay;

static int vertical(const char *s, size_t cursor, int dy, int jp, VjoRowGoal *goal, size_t *out)
{
    VjoTextRows r = layout_rows(&lay);
    return vjo_text_row_vertical(s, strlen(s), &r, cursor, dy, jp, goal, out);
}

/* Up/Down with no remembered column. */
static size_t up_down(const char *s, size_t cursor, int dy, int jp)
{
    VjoRowGoal goal = {0, 0};
    size_t out = cursor;
    int rc = vertical(s, cursor, dy, jp, &goal, &out);
    TEST_CHECK(rc == (out == cursor ? VJO_ROW_STAY : VJO_ROW_MOVED));
    return out;
}

/* Left/Right in Character mode, as the d-pad (wrap) or the stick (no wrap)
 * steps: returns the cursor, *rc the row result. */
static size_t step_as(const char *s, size_t cursor, int dx, int jp, int wrap, int *rc)
{
    VjoTextRows r = layout_rows(&lay);
    size_t out = cursor;
    *rc = vjo_text_row_step(s, strlen(s), &r, cursor, cursor + 1, dx, jp, wrap, &out);
    return out;
}

static size_t step(const char *s, size_t cursor, int dx, int jp, int *rc)
{
    return step_as(s, cursor, dx, jp, 1, rc);
}

static void wrapped_line_rows_and_their_ends(void)
{
    /* 14 kana: a soft wrap after the 10th (byte 3k is kana k). */
    const char *s = "あいうえおかきくけこさしすせ";
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.line[9] == 0 && lay.line[10] == 1);
    TEST_CHECK(up_down(s, 6, 1, 1) == 36);  /* う (column 2) -> す */
    TEST_CHECK(up_down(s, 36, -1, 1) == 6); /* and back */
    TEST_CHECK(up_down(s, 27, 1, 1) == 39); /* こ (column 9) -> せ, the short row's last */
    TEST_CHECK(up_down(s, 39, -1, 1) == 9); /* せ's own column 3: え */
    TEST_CHECK(up_down(s, 0, -1, 1) == 0);  /* no row above */
    TEST_CHECK(up_down(s, 39, 1, 1) == 39); /* none below */
    unsigned calls = lay.calls;
    TEST_CHECK(step(s, 27, 1, 1, &rc) == 30 && rc == VJO_ROW_MOVED);  /* Right from the row's last: the next row's first */
    TEST_CHECK(step(s, 0, 1, 1, &rc) == 3 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 30, -1, 1, &rc) == 27 && rc == VJO_ROW_MOVED); /* Left from a row's first: the row above's last */
    TEST_CHECK(step(s, 33, -1, 0, &rc) == 30 && rc == VJO_ROW_MOVED);
    TEST_CHECK(lay.calls == calls); /* in reading order: no position asked */
    /* the text's first and last: on two rows, nowhere to go (test.3's item G) */
    TEST_CHECK(step(s, 0, -1, 1, &rc) == 0 && rc == VJO_ROW_STAY);
    TEST_CHECK(step(s, 39, 1, 1, &rc) == 39 && rc == VJO_ROW_STAY);
    TEST_CHECK(lay.calls == calls + 4); /* each end: the first's and the last's rows */
    TEST_CHECK(step_as(s, 39, 1, 1, 0, &rc) == 39 && rc == VJO_ROW_STAY && lay.calls == calls + 4);
    TEST_CHECK(lay.bad_queries == 0);
}

/* A cell ends at the next character's start on its own row only: a
 * one-cell row's end is never the next row's first start. */
static void a_cell_never_ends_on_the_next_row(void)
{
    /* 24 px box: 猫 alone on row 0 (0..24), then narrow A-D (6 px) on row 1. */
    const char *s = "猫ABCD";
    TEST_ASSERT(layout_text(&lay, s, 24.0f, JA, 6.0f) == 2);
    TEST_CHECK(lay.line[1] == 1 && lay.x[1] == 0.0f && lay.x[2] == 6.0f);
    TEST_CHECK(up_down(s, 0, 1, 0) == 4); /* 猫's centre 12 ends B (6..12): B, not A */
    TEST_CHECK(up_down(s, 6, -1, 0) == 0);
}

static void column_kept_across_a_short_row(void)
{
    /* Hard breaks: 10 kana, 2 kana, 10 kana. け is column 8 (x 192..216). */
    const char *s = "あいうえおかきくけこ\nさし\nたちつてとなにぬねの";
    VjoRowGoal goal = {0, 0};
    size_t at = 24, out = 0;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 3);
    TEST_ASSERT(vertical(s, at, 1, 1, &goal, &out) == VJO_ROW_MOVED);
    TEST_CHECK(out == 34 && goal.at == 35 && goal.x == 204.0f); /* し, the short row's last */
    TEST_ASSERT(vertical(s, out, 1, 1, &goal, &out) == VJO_ROW_MOVED);
    TEST_CHECK(out == 62); /* ね: the column, not し's */
    TEST_ASSERT(vertical(s, out, -1, 1, &goal, &out) == VJO_ROW_MOVED && out == 34);
    TEST_ASSERT(vertical(s, out, -1, 1, &goal, &out) == VJO_ROW_MOVED && out == 24);
    /* Another cursor (a tap, Left/Right) or a forgotten column: the cell's own. */
    TEST_CHECK(up_down(s, 34, 1, 1) == 41); /* し's centre 36: ち */
    goal.at = 0;
    TEST_ASSERT(vertical(s, 34, 1, 1, &goal, &out) == VJO_ROW_MOVED && out == 41);
    goal = (VjoRowGoal){204.0f, 0}; /* a column for another cursor is not used */
    TEST_ASSERT(vertical(s, 34, 1, 1, &goal, &out) == VJO_ROW_MOVED && out == 41);
}

static void mixed_widths_use_measured_cells(void)
{
    /* Narrow A-J (12 px) then 猫犬, a hard break, five kana. */
    const char *s = "ABCDEFGHIJ猫犬\nあいうえお";
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.x[10] == 120.0f && lay.x[11] == 144.0f);
    TEST_CHECK(up_down(s, 4, 1, 0) == 23);   /* E (48..60) -> う (48..72) */
    TEST_CHECK(up_down(s, 13, 1, 0) == 29);  /* 犬 (144..168) -> お, the nearest */
    TEST_CHECK(up_down(s, 23, -1, 0) == 4);  /* う's centre 60, the E|F edge: the earlier */
    TEST_CHECK(up_down(s, 26, -1, 0) == 6);  /* え's centre 84: G is 72..84, H 84..96: G */
    TEST_CHECK(up_down(s, 29, -1, 0) == 8);  /* お's centre 108 ends I (96..108): the earlier */
    /* Japanese-only: the Latin cells cannot hold the cursor. */
    TEST_CHECK(up_down(s, 23, -1, 1) == 10); /* -> 猫, the nearest Japanese cell */
    TEST_CHECK(up_down(s, 10, 1, 1) == 29);  /* 猫's centre 132 -> お */
    TEST_CHECK(step(s, 13, 1, 1, &rc) == 17 && rc == VJO_ROW_MOVED);  /* 犬 -> あ, over the line break */
    TEST_CHECK(step(s, 17, -1, 1, &rc) == 13 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 10, -1, 1, &rc) == 10 && rc == VJO_ROW_STAY);  /* 猫, the first Japanese letter */
    TEST_CHECK(step(s, 29, 1, 1, &rc) == 29 && rc == VJO_ROW_STAY);   /* お, the last */
    TEST_CHECK(step(s, 9, 1, 0, &rc) == 10 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 13, 1, 0, &rc) == 17 && rc == VJO_ROW_MOVED);  /* the line break is skipped */
    TEST_CHECK(step(s, 0, -1, 0, &rc) == 0 && rc == VJO_ROW_STAY);
}

static void punctuation_and_hanging_kinsoku(void)
{
    /* 。 may not start a row: it hangs past the edge of row 0 (x 240). */
    const char *s = "あいうえおかきくけこ。さし";
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.line[10] == 0 && lay.x[10] == 240.0f);
    TEST_CHECK(step(s, 27, 1, 1, &rc) == 33 && rc == VJO_ROW_MOVED);   /* Japanese-only skips 。 */
    TEST_CHECK(step(s, 33, -1, 1, &rc) == 27 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 0, -1, 1, &rc) == 0 && rc == VJO_ROW_STAY);
    TEST_CHECK(step(s, 27, 1, 0, &rc) == 30 && rc == VJO_ROW_MOVED);   /* off: 。 is a character */
    TEST_CHECK(step(s, 30, 1, 0, &rc) == 33 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 33, -1, 0, &rc) == 30 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 36, 1, 0, &rc) == 36 && rc == VJO_ROW_STAY);
    TEST_CHECK(up_down(s, 30, 1, 0) == 36);                             /* 。 (240..264) -> し */
    const char *t = "あいうえおかきくけ、さしす"; /* 、 fits as row 0's 10th cell */
    TEST_ASSERT(layout_text(&lay, t, W, JA, NARROW) == 2);
    TEST_CHECK(step(t, 24, 1, 1, &rc) == 30 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(t, 24, 1, 0, &rc) == 27 && rc == VJO_ROW_MOVED);
    /* Spaces (also U+3000) and line breaks never hold the cursor. */
    const char *u = "A B\xe3\x80\x80猫\n犬";
    TEST_ASSERT(layout_text(&lay, u, W, JA, NARROW) == 2);
    TEST_CHECK(step(u, 0, 1, 0, &rc) == 2 && step(u, 2, 1, 0, &rc) == 6);
    TEST_CHECK(step(u, 6, 1, 0, &rc) == 10 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(u, 10, -1, 0, &rc) == 6 && step(u, 6, -1, 0, &rc) == 2);
    TEST_CHECK(step(u, 10, 1, 0, &rc) == 10 && rc == VJO_ROW_STAY);
    TEST_CHECK(step(u, 1, 1, 0, &rc) == 2); /* from a blank: the next character */
    TEST_CHECK(step(u, 1, -1, 0, &rc) == 0);
    /* DEL and C1 controls are not visible characters either; one row: round it. */
    const char *v = "A\x7f" "B\xc2\x85" "C";
    TEST_ASSERT(layout_text(&lay, v, W, JA, NARROW) == 1);
    TEST_CHECK(step(v, 0, 1, 0, &rc) == 2 && step(v, 2, 1, 0, &rc) == 5);
    TEST_CHECK(step(v, 5, 1, 0, &rc) == 0 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(v, 0, -1, 0, &rc) == 5 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step_as(v, 5, 1, 0, 0, &rc) == 5 && rc == VJO_ROW_STAY); /* the stick: no wrap */
    /* Japanese-only never selects an overlong encoding of a Japanese letter
     * (U+3042 in four bytes), as vjo_utf8_japanese_cursor_step does not. */
    const char *o = "猫\xf0\x83\x81\x82" "犬";
    TEST_ASSERT(layout_text(&lay, o, W, JA, NARROW) == 1);
    TEST_CHECK(lay.cp[1] == 0x3042 && lay.bytes[1] == 4);
    TEST_CHECK(step(o, 0, 1, 1, &rc) == 7 && step(o, 7, -1, 1, &rc) == 0);
    TEST_CHECK(step(o, 0, 1, 0, &rc) == 3); /* off: any visible character */
}

static void cr_newline_and_surrogate_pairs(void)
{
    /* 猫 \r \n 𠮷 野 家: CR has no unit, 𠮷 takes two (queried at the first). */
    const char *s = "猫\r\n𠮷野家";
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.unit[2] == 2 && lay.unit[3] == 4); /* 𠮷 at units 2-3, 野 at 4 */
    TEST_CHECK(up_down(s, 0, 1, 1) == 5);
    TEST_CHECK(up_down(s, 12, -1, 1) == 0);
    TEST_CHECK(step(s, 5, 1, 1, &rc) == 9 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 5, -1, 1, &rc) == 0 && rc == VJO_ROW_MOVED);  /* over CR LF */
    TEST_CHECK(step(s, 0, 1, 1, &rc) == 5 && rc == VJO_ROW_MOVED);   /* 猫, alone on its row */
    TEST_CHECK(step(s, 3, 1, 0, &rc) == 5 && rc == VJO_ROW_MOVED);   /* a cursor on CR: the next character */
    TEST_CHECK(step(s, 3, -1, 0, &rc) == 0 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 12, 1, 1, &rc) == 12 && rc == VJO_ROW_STAY);  /* 家, the text's last */
    TEST_CHECK(step(s, 0, -1, 1, &rc) == 0 && rc == VJO_ROW_STAY);
    TEST_CHECK(lay.bad_queries == 0);
}

/* A line break before the first selectable character or after the last one
 * does not make a second row of them: such a text still goes round, with the
 * layout and without it (an empty first or last line). */
static void a_line_break_at_an_end_is_not_a_row(void)
{
    static const char *const texts[] = {"\n猫犬鳥", "猫犬鳥\n", "\n\n猫犬鳥\n", "A\n猫犬鳥"};
    for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        const char *s = texts[i];
        size_t len = strlen(s), cat = strstr(s, "猫") - s, bird = strstr(s, "鳥") - s, out = len;
        layout_text(&lay, s, W, JA, NARROW);
        VjoTextRows r = layout_rows(&lay);
        TEST_CHECK_(vjo_text_row_step(s, len, &r, bird, bird + 1, 1, 1, 1, &out) == VJO_ROW_WRAPPED && out == cat,
                    "text %u: Right from 鳥: %zu", i, out);
        TEST_CHECK(vjo_text_row_step(s, len, &r, cat, cat + 1, -1, 1, 1, &out) == VJO_ROW_WRAPPED && out == bird);
        out = len;
        TEST_CHECK_(vjo_text_row_step(s, len, NULL, bird, bird + 1, 1, 1, 1, &out) == VJO_ROW_UNAVAILABLE &&
                    out == cat, "text %u without a layout: %zu", i, out);
        TEST_CHECK(vjo_text_row_step(s, len, NULL, cat, cat + 1, -1, 1, 1, &out) == VJO_ROW_UNAVAILABLE &&
                   out == bird);
    }
    /* off, "A\n猫犬鳥" has A on its own row: no wrap, either way */
    const char *s = texts[3];
    size_t len = strlen(s), out = 99;
    layout_text(&lay, s, W, JA, NARROW);
    VjoTextRows r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 8, 9, 1, 0, 1, &out) == VJO_ROW_STAY && out == 99);
    TEST_CHECK(vjo_text_row_step(s, len, NULL, 8, 9, 1, 0, 1, &out) == VJO_ROW_UNAVAILABLE && out == 99);
    TEST_CHECK(vjo_text_row_step(s, len, NULL, 0, 1, -1, 0, 1, &out) == VJO_ROW_UNAVAILABLE && out == 99);
    /* one letter (or a cursor on the only one Right could go round to): nowhere to go, nothing asked */
    const char *one = "猫。";
    layout_text(&lay, one, W, JA, NARROW);
    r = layout_rows(&lay);
    unsigned calls = lay.calls;
    TEST_CHECK(vjo_text_row_step(one, 6, &r, 0, 1, 1, 1, 1, &out) == VJO_ROW_STAY && out == 99);
    TEST_CHECK(vjo_text_row_step(one, 6, &r, 0, 1, -1, 1, 1, &out) == VJO_ROW_STAY && out == 99);
    TEST_CHECK(vjo_text_row_step(one, 6, &r, 3, 4, -1, 1, 1, &out) == VJO_ROW_MOVED && out == 0); /* from 。 */
    TEST_CHECK(lay.calls == calls);
}

/* x is NaN for 猫, the text's first character. */
static int nan_position(void *ctx, uint32_t unit, float *x, uint32_t *line)
{
    int rc = layout_position(ctx, unit, x, line);
    if (unit == 0)
        *x = NAN;
    return rc;
}

static void unavailable_layout_changes_nothing(void)
{
    const char *s = "猫\r\n𠮷野家"; /* units: 猫 0, LF 1, 𠮷 2-3, 野 4, 家 5 */
    VjoRowGoal goal = {0, 0};
    size_t out = 99, len = strlen(s);
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    VjoTextRows r = layout_rows(&lay);
    lay.fail_unit = 4; /* 野, a selectable cell */
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, 0, 1, 1, &goal, &out) == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(out == 99 && goal.at == 0);
    /* Left/Right in reading order ask no position; at the text's ends only its
     * first's and last's (猫 and 家 here), and without them the line breaks decide */
    unsigned calls = lay.calls;
    TEST_CHECK(vjo_text_row_step(s, len, &r, 5, 6, 1, 1, 1, &out) == VJO_ROW_MOVED && out == 9 && lay.calls == calls);
    out = 99;
    TEST_CHECK(vjo_text_row_step(s, len, &r, 12, 13, 1, 1, 1, &out) == VJO_ROW_STAY && out == 99);
    TEST_CHECK(lay.calls == calls + 2);
    lay.fail_unit = 5; /* 家, the text's last */
    TEST_CHECK(vjo_text_row_step(s, len, &r, 12, 13, 1, 1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 99);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 0, 1, -1, 1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 99);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 12, 13, 1, 1, 0, &out) == VJO_ROW_STAY && out == 99); /* the stick */
    lay.fail_unit = 1; /* the line break: it only ends 猫's cell early */
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, 0, 1, 1, &goal, &out) == VJO_ROW_MOVED && out == 5);
    lay.fail_unit = -1;
    r.lines = 1; /* a position on a line the layout does not have */
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, 0, 1, 1, &goal, &out) == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 12, 13, 1, 1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 5);
    r = layout_rows(&lay);
    r.position = nan_position;
    TEST_CHECK(vjo_text_row_step(s, len, &r, 12, 13, 1, 1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 5);
    r = layout_rows(&lay);
    r.terminal_width = 0;
    TEST_CHECK(vjo_text_row_step(s, len, &r, 12, 13, 1, 1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 5);
    TEST_CHECK(vjo_text_row_vertical(s, len, NULL, 0, 1, 1, &goal, &out) == VJO_ROW_UNAVAILABLE);
    r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, len, 1, 1, &goal, &out) == VJO_ROW_STAY);
    TEST_CHECK(vjo_text_row_step("", 0, &r, 0, 1, 1, 1, 1, &out) == VJO_ROW_STAY);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 0, 1, 0, 1, 1, &out) == VJO_ROW_STAY);
    TEST_CHECK(vjo_text_row_step(s, len, &r, len, len + 1, 1, 1, 1, &out) == VJO_ROW_STAY);
    TEST_CHECK(vjo_text_row_step(NULL, len, &r, 0, 1, 1, 1, 1, &out) == VJO_ROW_STAY);
    TEST_CHECK(out == 5);
    /* Without a layout the line breaks stand for the rows: the arrows still
     * move, stop at the ends of a text with one, and go round one without. */
    VjoWordNavigation h = {{0}, 0};
    int rc;
    TEST_CHECK(vjo_word_step(&h, s, len, 12, 0, 1, 1, NULL, 1, &rc) == 12 && rc == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_word_step(&h, s, len, 0, 0, 1, 1, NULL, 1, &rc) == 5 && rc == VJO_ROW_MOVED && h.count == 1);
    h.count = 0;
    TEST_CHECK(vjo_word_step(&h, s, len, 0, 0, -1, 1, NULL, 1, &rc) == 0 && rc == VJO_ROW_UNAVAILABLE);
    const char *t = "猫犬鳥";
    TEST_CHECK(vjo_word_step(&h, t, 9, 6, 0, 1, 1, NULL, 1, &rc) == 0 && rc == VJO_ROW_UNAVAILABLE && h.count == 1);
    TEST_CHECK(vjo_word_step(&h, t, 9, 0, 0, -1, 1, NULL, 1, &rc) == 6 && rc == VJO_ROW_MOVED && h.count == 0);
    TEST_CHECK(vjo_word_step(&h, t, 9, 0, 0, -1, 1, NULL, 1, &rc) == 6 && rc == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_word_step(&h, t, 9, 6, 0, 1, 1, NULL, 0, &rc) == 6 && rc == VJO_ROW_STAY); /* the stick */
}

static void whole_words_cross_rows(void)
{
    /* Row 0: 猫 が 好 き で す 。 犬 も 好 | row 1: き で す 。 (好き crosses). */
    const char *s = "猫が好きです。犬も好きです。";
    size_t len = strlen(s), cursor = 0;
    static const size_t ends[][2] = {{0, 3}, {3, 6}, {6, 12}, {12, 18}, {21, 24}, {24, 27}, {27, 33}, {33, 39}};
    const size_t n = sizeof(ends) / sizeof(ends[0]);
    VjoWordNavigation h = {{0}, 0};
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.line[9] == 0 && lay.line[10] == 1);
    VjoTextRows r = layout_rows(&lay);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT(cursor == ends[i][0]);
        cursor = vjo_word_step(&h, s, len, cursor, ends[i][1], 1, 1, &r, 1, &rc);
        TEST_CHECK(rc == (i + 1 < n ? VJO_ROW_MOVED : VJO_ROW_STAY));
    }
    /* 好き ends on row 1: Right went on after its tail there (で), then stopped at the text's last word */
    TEST_CHECK(cursor == 33 && h.count == 7);
    for (size_t i = n - 1; i-- > 0;) {
        cursor = vjo_word_step(&h, s, len, cursor, 0, -1, 1, &r, 1, &rc);
        TEST_CHECK(cursor == ends[i][0] && rc == VJO_ROW_MOVED); /* Left retraces, across the rows */
    }
    TEST_CHECK(h.count == 0);
    cursor = vjo_word_step(&h, s, len, 0, 0, -1, 1, &r, 1, &rc);
    TEST_CHECK(cursor == 0 && rc == VJO_ROW_STAY); /* no history, the text's first: nowhere */
    TEST_CHECK(vjo_word_step(&h, s, len, 30, 0, -1, 1, &r, 1, &rc) == 27 && rc == VJO_ROW_MOVED); /* row 1's first */
    TEST_CHECK(vjo_word_step(&h, s, len, 27, 0, -1, 1, &r, 1, &rc) == 24 && h.count == 0);
    /* Row 1: で's match runs to the end, 。 is not Japanese: the end, nowhere to go. */
    TEST_CHECK(vjo_word_step(&h, s, len, 30, 33, 1, 1, &r, 1, &rc) == 33);
    TEST_CHECK(vjo_word_step(&h, s, len, 33, 39, 1, 1, &r, 1, &rc) == 33 && rc == VJO_ROW_STAY);
    TEST_CHECK(vjo_word_step(&h, s, len, 33, len, 1, 0, &r, 1, &rc) == 33 && rc == VJO_ROW_STAY);
    TEST_CHECK(vjo_word_step(&h, s, len, 27, len, 1, 0, &r, 1, &rc) == 27 && rc == VJO_ROW_STAY);
    /* A pending or failed lookup, or a match end inside a character: one character. */
    h.count = 0;
    TEST_CHECK(vjo_word_step(&h, s, len, 6, 0, 1, 1, &r, 1, &rc) == 9);
    TEST_CHECK(vjo_word_step(&h, s, len, 6, 10, 1, 1, &r, 1, &rc) == 9);
    TEST_CHECK(vjo_word_step(&h, s, len, 6, len + 1, 1, 1, &r, 1, &rc) == 9);
    TEST_CHECK(h.count == 3);
    /* A match whose end lies on a later row, after a line break (a word looked
     * up whole across it): Right lands after its tail there, never on that
     * row's first character or round to the text's start. */
    const char *t = "わたしをたべ\nるかな"; /* た 12, べ 15, LF 18, る 19, か 22 */
    TEST_ASSERT(layout_text(&lay, t, W, JA, NARROW) == 2);
    r = layout_rows(&lay);
    h.count = 0;
    TEST_CHECK(vjo_word_step(&h, t, strlen(t), 12, 22, 1, 1, &r, 1, &rc) == 22 && rc == VJO_ROW_MOVED);
    TEST_CHECK(vjo_word_step(&h, t, strlen(t), 22, 0, -1, 1, &r, 1, &rc) == 12 && rc == VJO_ROW_MOVED);
    TEST_CHECK(vjo_word_step(&h, t, strlen(t), 12, 25, 1, 1, &r, 0, &rc) == 25); /* without the wrap the same */
}

/* Left always moves: a history entry at the cursor itself (none of the
 * overlay's paths leaves one) is dropped, and a Left step forgets the rest. */
static void left_never_retraces_onto_the_cursor(void)
{
    const char *s = "あいうえお";
    size_t len = strlen(s);
    VjoWordNavigation h = {{0, 6}, 2};
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 1);
    VjoTextRows r = layout_rows(&lay);
    TEST_CHECK(vjo_word_step(&h, s, len, 6, 0, -1, 1, &r, 1, &rc) == 3 && rc == VJO_ROW_MOVED);
    TEST_CHECK(h.count == 0); /* the step back forgot 0 too */
    TEST_CHECK(vjo_word_step(&h, s, len, 3, 0, -1, 1, &r, 1, &rc) == 0 && h.count == 0);
    h = (VjoWordNavigation){{99}, 1}; /* past the text */
    TEST_CHECK(vjo_word_step(&h, s, len, 0, 0, -1, 1, &r, 1, &rc) == 12 && rc == VJO_ROW_WRAPPED && h.count == 0);
    h = (VjoWordNavigation){{99}, 1};
    TEST_CHECK(vjo_word_step(&h, s, len, 0, 0, -1, 1, &r, 0, &rc) == 0 && rc == VJO_ROW_STAY && h.count == 1);
}

static void row_keys_forget_the_row_they_leave(void)
{
    const char *s = "猫が好きです。犬も好きです。";
    size_t len = strlen(s), out = 0;
    VjoRowNav nav;
    memset(&nav, 0, sizeof(nav));
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    VjoTextRows r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 0, 3, 1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 3);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 3, 6, 1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 6);
    TEST_CHECK(nav.words.count == 2);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 6, 12, 0, 1, 1, 0, &out) == VJO_ROW_MOVED);
    TEST_CHECK(out == 36 && nav.words.count == 0 && nav.row_moves == 1); /* 好 (48..72) -> す (48..72) */
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 36, 0, -1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 33);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 33, 0, -1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 30);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 30, 0, -1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 27);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 27, 0, 0, 1, 1, 0, &out) == VJO_ROW_MOVED && out == 36);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 36, 0, 0, 1, 1, 0, &out) == VJO_ROW_STAY && out == 36);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, NULL, 36, 0, 0, -1, 1, 0, &out) == VJO_ROW_UNAVAILABLE && out == 36);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 36, 0, 1, 0, 1, 0, &out) == VJO_ROW_STAY && out == 36);
    TEST_CHECK(nav.row_moves == 2 && nav.wraps == 0 && nav.no_layout == 1);
    /* The stick's steps (`stick`) take the same path and are not counted; its left/right go a
     * character at a time (the owner, 2026-10-20): from 好 with 好き shown, き, not past the word. */
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 36, 0, 0, -1, 1, 1, &out) == VJO_ROW_MOVED && out == 6);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, NULL, 6, 0, 0, 1, 1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 6);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 6, 12, 1, 0, 1, 1, &out) == VJO_ROW_MOVED && out == 9);
    TEST_CHECK(nav.words.count == 1); /* the stick's Right is in the history too */
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 9, 0, -1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 6);
    TEST_CHECK(nav.row_moves == 2 && nav.wraps == 0 && nav.no_layout == 1);
    /* One row: the arrows go round its ends (counted), the stick does not, and its left
     * never retraces, a wrap included: at the first character it stays; the d-pad's Left
     * then undoes the wrap. */
    const char *o = "猫が好きです。";
    memset(&nav, 0, sizeof(nav));
    TEST_ASSERT(layout_text(&lay, o, W, JA, NARROW) == 1);
    r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_key(&nav, o, strlen(o), &r, 15, 0, 1, 0, 1, 1, &out) == VJO_ROW_STAY && out == 15);
    TEST_CHECK(vjo_text_row_key(&nav, o, strlen(o), &r, 15, 0, 1, 0, 1, 0, &out) == VJO_ROW_WRAPPED && out == 0);
    TEST_CHECK(vjo_text_row_key(&nav, o, strlen(o), &r, 0, 0, -1, 0, 1, 1, &out) == VJO_ROW_STAY && out == 0);
    TEST_CHECK(nav.wraps == 1 && nav.words.count == 1);
    TEST_CHECK(vjo_text_row_key(&nav, o, strlen(o), &r, 0, 0, -1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 15);
    TEST_CHECK(nav.wraps == 1 && nav.words.count == 0); /* the d-pad's Left retraced the wrap */
    TEST_CHECK(vjo_text_row_key(&nav, o, strlen(o), NULL, 15, 0, 1, 0, 1, 0, &out) == VJO_ROW_UNAVAILABLE && out == 0);
    TEST_CHECK(nav.wraps == 1 && nav.no_layout == 1);
    /* Left/Right take Up/Down's column afresh, even back on the same cell:
     * け (column 8) Down to し keeps 204; Right goes on to た, Left retraces to
     * し; Down then goes by し's own centre (36) to ち, not ね. */
    const char *t = "あいうえおかきくけこ\nさし\nたちつてとなにぬねの";
    size_t tlen = strlen(t);
    memset(&nav, 0, sizeof(nav));
    TEST_ASSERT(layout_text(&lay, t, W, JA, NARROW) == 3);
    r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 24, 0, 0, 1, 1, 0, &out) == VJO_ROW_MOVED && out == 34);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 34, 0, 1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 38);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 38, 0, -1, 0, 1, 0, &out) == VJO_ROW_MOVED && out == 34);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 34, 0, 0, 1, 1, 0, &out) == VJO_ROW_MOVED && out == 41);
}

/* ---- every key from every character of generated texts ---- */

static uint32_t seed = 12345;
static uint32_t lcg(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

static void generated_texts_follow_the_rules(void)
{
    static const char *const pieces[] = {"あ", "猫", "犬", "、", "。", "」", "ー", "っ", "A", "b", "7", " ", "\n",
                                         "\r", "𠮷", "ｶ", "\xe3\x80\x80", "「", "x", "語"};
    static char text[4096];
    static const float widths[] = {240.0f, 120.0f, 920.0f, 30.0f};
    unsigned checks = 0, moves = 0, wraps = 0, rows_total = 0, one_row = 0;
    for (int t = 0; t < 120; t++) {
        size_t n = 0, pieces_n = 2 + lcg() % (t % 3 ? 90 : 12); /* a third of them short, often on one row */
        for (size_t p = 0; p < pieces_n; p++) {
            const char *piece = pieces[lcg() % (sizeof(pieces) / sizeof(pieces[0]))];
            size_t k = strlen(piece);
            if (n + k >= sizeof(text)) break;
            memcpy(text + n, piece, k);
            n += k;
        }
        text[n] = 0;
        float width = widths[t % 4];
        rows_total += layout_text(&lay, text, width, JA, NARROW);
        VjoTextRows r = layout_rows(&lay);
        size_t ends[2][2] = {{n, n}, {n, n}}; /* per jp: the text's first and last selectable characters */
        for (unsigned k = 0; k < lay.n; k++)
            for (int jp = 0; jp <= 1; jp++)
                if (layout_selectable(&lay, k, jp)) {
                    if (ends[jp][0] == n)
                        ends[jp][0] = lay.begin[k];
                    ends[jp][1] = lay.begin[k];
                }
        for (unsigned k = 0; k < lay.n; k++) {
            size_t c = lay.begin[k];
            for (int jp = 0; jp <= 1; jp++) {
                for (int d = -1; d <= 1; d += 2) {
                    int wrapped, rc;
                    for (int wrap = 0; wrap <= 1; wrap++) {
                        unsigned calls = lay.calls;
                        size_t want = ref_read_step(&lay, c, c + 1, d, jp, wrap, &wrapped), out = c;
                        rc = vjo_text_row_step(text, n, &r, c, c + 1, d, jp, wrap, &out);
                        TEST_CHECK_(out == want && rc == (want == c ? VJO_ROW_STAY : wrapped ? VJO_ROW_WRAPPED : VJO_ROW_MOVED),
                                    "text %d byte %zu jp %d dx %d wrap %d: %zu rc %d, expected %zu wrapped %d", t, c, jp,
                                    d, wrap, out, rc, want, wrapped);
                        /* positions asked: none for a move in reading order; at an end, the first's and
                         * the last's rows, only when it could go round to another character */
                        size_t round = ends[jp][d > 0 ? 0 : 1];
                        unsigned asked = want != c && !wrapped ? 0u : wrap && round < n && round != c ? 2u : 0u;
                        TEST_CHECK_(lay.calls - calls == asked, "text %d byte %zu dx %d wrap %d: %u queries, expected %u",
                                    t, c, d, wrap, lay.calls - calls, asked);
                        /* and without a layout the same, but where the rows decide: by the line breaks */
                        out = c;
                        rc = vjo_text_row_step(text, n, NULL, c, c + 1, d, jp, wrap, &out);
                        TEST_CHECK_(want == c || wrapped ? 1 : out == want && rc == VJO_ROW_MOVED,
                                    "text %d byte %zu dx %d without a layout: %zu", t, c, d, out);
                    }
                    /* a whole word: Right from the end of the next selectable character */
                    size_t from = k + 2 < lay.n ? lay.begin[k + 2] : n;
                    size_t want = ref_read_step(&lay, c, from, 1, jp, 1, &wrapped), out = c;
                    rc = vjo_text_row_step(text, n, &r, c, from, 1, jp, 1, &out);
                    TEST_CHECK_(out == want, "text %d byte %zu from %zu: %zu, expected %zu", t, c, from, out, want);
                    VjoRowGoal goal = {0, 0};
                    want = ref_vertical(&lay, c, d, jp, -1.0f);
                    out = c;
                    rc = vjo_text_row_vertical(text, n, &r, c, d, jp, &goal, &out);
                    TEST_CHECK_(out == want && rc == (want == c ? VJO_ROW_STAY : VJO_ROW_MOVED),
                                "text %d byte %zu jp %d dy %d: %zu, expected %zu", t, c, jp, d, out, want);
                    goal = (VjoRowGoal){(float)(lcg() % 1000), (uint32_t)c + 1}; /* a remembered column */
                    want = ref_vertical(&lay, c, d, jp, goal.x);
                    out = c;
                    vjo_text_row_vertical(text, n, &r, c, d, jp, &goal, &out);
                    TEST_CHECK_(out == want, "text %d byte %zu column: %zu, expected %zu", t, c, out, want);
                    checks += 6;
                }
            }
        }
        /* From the text's first selectable character, Right visits each one
         * once in reading order, across every row, and then stays at the last
         * (or goes round to the first, on one row); Left the same way back. */
        for (int jp = 0; jp <= 1; jp++) {
            unsigned sel[LAYOUT_MAX], ns = 0;
            for (unsigned k = 0; k < lay.n; k++)
                if (layout_selectable(&lay, k, jp))
                    sel[ns++] = k;
            if (!ns)
                continue;
            int one = lay.line[sel[0]] == lay.line[sel[ns - 1]];
            one_row += one && jp;
            for (int d = -1; d <= 1; d += 2) {
                size_t at = lay.begin[d > 0 ? sel[0] : sel[ns - 1]], next;
                int rc = VJO_ROW_STAY;
                for (unsigned i = 1; i < ns; i++) {
                    next = at;
                    rc = vjo_text_row_step(text, n, &r, at, at + 1, d, jp, 1, &next);
                    TEST_CHECK_(rc == VJO_ROW_MOVED && next == lay.begin[d > 0 ? sel[i] : sel[ns - 1 - i]],
                                "text %d jp %d dx %d: step %u at %zu", t, jp, d, i, next);
                    at = next;
                    moves++;
                }
                next = at;
                rc = vjo_text_row_step(text, n, &r, at, at + 1, d, jp, 1, &next);
                TEST_CHECK_(one && ns > 1 ? rc == VJO_ROW_WRAPPED && next == lay.begin[d > 0 ? sel[0] : sel[ns - 1]]
                                          : rc == VJO_ROW_STAY && next == at,
                            "text %d jp %d dx %d: past the end (%s): rc %d at %zu", t, jp, d, one ? "one row" : "rows",
                            rc, next);
                wraps += rc == VJO_ROW_WRAPPED;
            }
        }
        TEST_CHECK(lay.bad_queries == 0);
    }
    TEST_CHECK(one_row > 10 && wraps > 20); /* the one-row texts' wrap is exercised */
    printf("\n  %u key checks on 120 generated texts (%u rows; %u texts with their Japanese on one row), "
           "%u Right and Left steps through them, %u wraps\n", checks, rows_total, one_row, moves, wraps);
}

/* One key of `nav` from cursor (dx: -1/1 Left/Right, the stick's when `stick`), on the laid-out
 * text: the cursor it leaves, its VJO_ROW_* result in *rc. */
static size_t key(VjoRowNav *nav, const char *s, size_t cursor, size_t matched_end, int dx, int jp, int stick, int *rc)
{
    VjoTextRows r = layout_rows(&lay);
    size_t out = cursor;
    *rc = vjo_text_row_key(nav, s, strlen(s), &r, cursor, matched_end, dx, 0, jp, stick, &out);
    return out;
}

/* The left stick's left/right go a character at a time in either highlight mode (the owner,
 * 2026-10-20: "I go to word 食べました. I want to go to ました"): on 昨日すき焼きを食べました with
 * 食べました shown, the stick's right is べ, then ま, not past the word as the d-pad's Right goes,
 * and never round the text's ends. Its rights are recorded as any Right; its left steps a
 * character back, never retracing a Right, and the Right that started where it lands leaves the
 * history, so the d-pad's Left retraces what is left: the stick's characters, then the d-pad's
 * words. On one row (920 px), two (240 px: row 0 ends at ま) and three (120 px), Japanese-only on
 * and off. Bytes: 昨0 日3 す6 き9 焼12 き15 を18 食21 べ24 ま27 し30 た33, len 36; the match ends
 * are the bundled dictionary's at each character (host/tests/test_word_lookup.cpp looks them up). */
static void the_stick_steps_a_character_at_a_time(void)
{
    const char *s = "昨日すき焼きを食べました";
    static const float widths[] = {920.0f, W, 120.0f};
    static const size_t end_at[] = {6, 6, 18, 12, 18, 18, 21, 36, 27, 36, 36, 36}; /* by character */
    for (int w = 0; w < 3; w++)
        for (int jp = 0; jp <= 1; jp++) {
            VjoRowNav nav;
            int rc;
            size_t c = 0;
            TEST_CASE_("%.0f px, Japanese-only %d", widths[w], jp);
            memset(&nav, 0, sizeof(nav));
            TEST_ASSERT(layout_text(&lay, s, widths[w], JA, NARROW) == (uint32_t)(w + 1));
            /* the d-pad, word by word: 昨日, すき焼き, を, to 食べました */
            static const size_t words[] = {6, 18, 21};
            for (int i = 0; i < 3; i++) {
                c = key(&nav, s, c, end_at[c / 3], 1, jp, 0, &rc);
                TEST_CHECK(c == words[i] && rc == VJO_ROW_MOVED);
            }
            TEST_CHECK(nav.words.count == 3);
            /* the stick, character by character, whatever is shown: べ, ま, し, た, and the last stays */
            static const size_t chars[] = {24, 27, 30, 33, 33};
            for (int i = 0; i < 5; i++) {
                size_t was = c;
                c = key(&nav, s, c, end_at[c / 3], 1, jp, 1, &rc);
                TEST_CHECK_(c == chars[i] && rc == (i < 4 ? VJO_ROW_MOVED : VJO_ROW_STAY),
                            "the stick's right from %zu (shown to %zu): %zu rc %d, expected %zu", was, end_at[was / 3],
                            c, rc, chars[i]);
            }
            TEST_CHECK(nav.words.count == 7 && nav.words.previous[3] == 21 && nav.words.previous[6] == 30);
            /* the d-pad's Left retraces the stick's characters, then the d-pad's words; at the text's
             * first, one row goes round, more stop */
            static const size_t back[] = {30, 27, 24, 21, 18, 6, 0};
            for (int i = 0; i < 7; i++) {
                c = key(&nav, s, c, 0, -1, jp, 0, &rc);
                TEST_CHECK_(c == back[i] && rc == VJO_ROW_MOVED, "the d-pad's Left %d: %zu, expected %zu", i, c, back[i]);
            }
            c = key(&nav, s, c, end_at[0], -1, jp, 0, &rc);
            TEST_CHECK(w == 0 ? c == 33 && rc == VJO_ROW_WRAPPED : c == 0 && rc == VJO_ROW_STAY);
            TEST_CHECK(nav.row_moves == 0 && nav.wraps == (w == 0) && nav.no_layout == 0);
            /* again to ま (d-pad words, two stick characters), then the stick's left: a character a
             * step back, each landing on where a Right started taking it out of the history; the
             * d-pad's Left from inside すき焼き goes to the word's start, where its Right started */
            memset(&nav, 0, sizeof(nav));
            c = 0;
            for (int i = 0; i < 5; i++)
                c = key(&nav, s, c, end_at[c / 3], 1, jp, i >= 3, &rc);
            TEST_CHECK(c == 27 && nav.words.count == 5);
            static const struct { size_t at; unsigned count; } left[] = {{24, 4}, {21, 3}, {18, 2}, {15, 2}};
            for (int i = 0; i < 4; i++) {
                c = key(&nav, s, c, end_at[c / 3], -1, jp, 1, &rc);
                TEST_CHECK_(c == left[i].at && rc == VJO_ROW_MOVED && nav.words.count == left[i].count,
                            "the stick's left %d: %zu with %u in the history, expected %zu with %u", i, c,
                            nav.words.count, left[i].at, left[i].count);
            }
            c = key(&nav, s, c, 0, -1, jp, 0, &rc);
            TEST_CHECK_(c == 6 && rc == VJO_ROW_MOVED && nav.words.count == 1, "the d-pad's Left from き: %zu", c);
            c = key(&nav, s, c, end_at[2], -1, jp, 1, &rc);
            TEST_CHECK(c == 3 && nav.words.count == 1); /* 日: 昨日's Right started before it */
            c = key(&nav, s, c, end_at[1], -1, jp, 1, &rc);
            TEST_CHECK(c == 0 && nav.words.count == 0);
            c = key(&nav, s, c, end_at[0], -1, jp, 1, &rc);
            TEST_CHECK(c == 0 && rc == VJO_ROW_STAY && nav.words.count == 0); /* never round, on one row too */
            TEST_CHECK(nav.row_moves == 0 && nav.wraps == 0 && nav.no_layout == 0);
        }
    /* Punctuation (Japanese-only decides), the ends and a row's end: 昨0 日3 、6 す9 き12 焼15 き18
     * を21 食24 べ27 ま30 し33 た36 。39, len 42; at 240 px row 0 ends at べ, at 120 px at き (12). */
    const char *p = "昨日、すき焼きを食べました。";
    for (int w = 0; w < 3; w++)
        for (int jp = 0; jp <= 1; jp++) {
            VjoRowNav nav;
            int rc;
            TEST_CASE_("punctuation, %.0f px, Japanese-only %d", widths[w], jp);
            memset(&nav, 0, sizeof(nav));
            layout_text(&lay, p, widths[w], JA, NARROW);
            TEST_CHECK(key(&nav, p, 3, 6, 1, jp, 1, &rc) == (jp ? 9u : 6u));   /* 日 shown: 、 off, す on */
            TEST_CHECK(key(&nav, p, 9, 21, -1, jp, 1, &rc) == (jp ? 3u : 6u)); /* back the same way */
            TEST_CHECK(key(&nav, p, 27, 30, 1, jp, 1, &rc) == 30u && key(&nav, p, 30, 39, -1, jp, 1, &rc) == 27u);
            TEST_CHECK(key(&nav, p, 9, 21, 1, jp, 1, &rc) == 12u && key(&nav, p, 12, 15, 1, jp, 1, &rc) == 15u);
            size_t last = jp ? 36 : 39;
            TEST_CHECK(key(&nav, p, 36, 39, 1, jp, 1, &rc) == last);           /* た: 。 off, the last on */
            TEST_CHECK(key(&nav, p, last, 0, 1, jp, 1, &rc) == last && rc == VJO_ROW_STAY);
            TEST_CHECK(key(&nav, p, 0, 6, -1, jp, 1, &rc) == 0 && rc == VJO_ROW_STAY);
            TEST_CHECK(nav.row_moves == 0 && nav.wraps == 0 && nav.no_layout == 0);
        }
}

/* The stick's left/right from every selectable character of generated texts, against the rule
 * restated over the cells (ref_read_step without the wrap): the next or previous selectable
 * character in reading order whatever match end is passed (none, a word's, inside a character, past
 * the text), Japanese-only on and off, with and without a layout (the stick's left/right asks for no
 * position). Its right adds where it started to the history (the oldest of 16 dropped), as any
 * Right; its left takes out the history's newest entry when that is at or after where it lands, and
 * leaves the rest; a step that stays changes nothing. The arrows' counts stay. */
static void the_sticks_steps_in_generated_texts(void)
{
    static const char *const pieces[] = {"あ", "猫", "犬", "、", "。", "」", "ー", "っ", "A", "b", "7", " ", "\n",
                                         "\r", "𠮷", "ｶ", "\xe3\x80\x80", "「", "x", "語"};
    static char text[4096];
    static const float widths[] = {240.0f, 120.0f, 920.0f, 30.0f};
    unsigned checks = 0, pushed = 0, popped = 0, kept = 0;
    seed = 4242;
    for (int t = 0; t < 120; t++) {
        size_t n = 0, pieces_n = 2 + lcg() % (t % 3 ? 90 : 12);
        for (size_t q = 0; q < pieces_n; q++) {
            const char *piece = pieces[lcg() % (sizeof(pieces) / sizeof(pieces[0]))];
            size_t k = strlen(piece);
            if (n + k >= sizeof(text)) break;
            memcpy(text + n, piece, k);
            n += k;
        }
        text[n] = 0;
        layout_text(&lay, text, widths[t % 4], JA, NARROW);
        VjoTextRows r = layout_rows(&lay);
        for (unsigned k = 0; k < lay.n; k++)
            for (int jp = 0; jp <= 1; jp++) {
                if (!layout_selectable(&lay, k, jp))
                    continue; /* the cursor is always on a selectable character */
                size_t c = lay.begin[k];
                for (int d = -1; d <= 1; d += 2)
                    for (int laid = 0; laid <= 1; laid++) {
                        int wrapped, rc;
                        size_t want = ref_read_step(&lay, c, c + 1, d, jp, 0, &wrapped), out = c;
                        /* a history as the overlay's paths leave one: earlier selectable characters in
                         * order, often ending at the one just before the cursor (a character's Right) */
                        VjoRowNav nav;
                        memset(&nav, 0, sizeof(nav));
                        int before_c = -1;
                        for (unsigned j = 0; j < k; j++)
                            if (layout_selectable(&lay, j, jp)) {
                                before_c = (int)j;
                                if (lcg() % 3 == 0 && nav.words.count < 16)
                                    nav.words.previous[nav.words.count++] = (uint32_t)lay.begin[j];
                            }
                        if (before_c >= 0 && lcg() % 2 && nav.words.count < 16 &&
                            (!nav.words.count || nav.words.previous[nav.words.count - 1] != lay.begin[before_c]))
                            nav.words.previous[nav.words.count++] = (uint32_t)lay.begin[before_c];
                        VjoWordNavigation h0 = nav.words;
                        uint32_t pick = lcg() % 5;
                        size_t matched = pick == 0 ? 0 : pick == 1 ? n + 1 : c + 1 + lcg() % (n - c);
                        unsigned calls = lay.calls;
                        rc = vjo_text_row_key(&nav, text, n, laid ? &r : NULL, c, matched, d, 0, jp, 1, &out);
                        TEST_CHECK_(out == want && rc == (want == c ? VJO_ROW_STAY : VJO_ROW_MOVED),
                                    "text %d byte %zu jp %d dx %d laid %d match %zu: %zu rc %d, expected %zu", t, c, jp,
                                    d, laid, matched, out, rc, want);
                        TEST_CHECK_(lay.calls == calls, "text %d byte %zu dx %d: %u queries", t, c, d,
                                    lay.calls - calls);
                        TEST_CHECK(!nav.row_moves && !nav.wraps && !nav.no_layout && nav.goal.at == 0);
                        VjoWordNavigation h1 = h0;
                        if (want != c && d > 0) {
                            if (h1.count == 16) {
                                memmove(h1.previous, h1.previous + 1, 15 * sizeof(h1.previous[0]));
                                h1.count--;
                            }
                            h1.previous[h1.count++] = (uint32_t)c;
                            pushed++;
                        } else if (want != c && h1.count && h1.previous[h1.count - 1] >= want) {
                            h1.count--;
                            popped++;
                        } else
                            kept++;
                        TEST_CHECK_(nav.words.count == h1.count &&
                                    !memcmp(nav.words.previous, h1.previous, h1.count * sizeof(h1.previous[0])),
                                    "text %d byte %zu dx %d: %u in the history, expected %u", t, c, d, nav.words.count,
                                    h1.count);
                        checks++;
                    }
            }
        TEST_CHECK(lay.bad_queries == 0);
    }
    TEST_CHECK(pushed > 1000 && popped > 300 && kept > 300);
    printf("\n  %u stick steps on 120 generated texts: %u recorded, %u taken out of the history, %u kept it\n",
           checks, pushed, popped, kept);
}

/* Sessions of mixed keys over generated multi-row texts, as the overlay's paths take them: the
 * d-pad's Left/Right with a shown word's end (any of the text's character boundaries after the
 * cursor), the stick's left/right, Up/Down, a tap (render's s_nav.words.count = 0) and Character
 * mode (the same, and no match end). After every key Right's history is strictly increasing and
 * all of it lies before the cursor (on a text of several rows nothing goes round, so the stick's
 * left only ever takes out entries at or after where it lands, the newest); the stick's every step
 * is the next or previous selectable character; the d-pad's Left lands on the history's newest
 * entry when there is one. */
static void mixed_sessions_keep_the_history_in_order(void)
{
    static const char *const pieces[] = {"あ", "猫", "犬", "、", "。", "ー", "っ", "A", "7", " ", "\n", "𠮷", "語", "き"};
    static char text[2048];
    unsigned sessions = 0, keys = 0, stick_keys = 0, retraced = 0;
    seed = 777;
    for (int t = 0; t < 200; t++) {
        size_t n = 0, pieces_n = 20 + lcg() % 60;
        for (size_t q = 0; q < pieces_n; q++) {
            const char *piece = pieces[lcg() % (sizeof(pieces) / sizeof(pieces[0]))];
            size_t k = strlen(piece);
            if (n + k >= sizeof(text)) break;
            memcpy(text + n, piece, k);
            n += k;
        }
        text[n] = 0;
        layout_text(&lay, text, t % 2 ? 120.0f : W, JA, NARROW);
        VjoTextRows r = layout_rows(&lay);
        for (int jp = 0; jp <= 1; jp++) {
            unsigned sel[LAYOUT_MAX], ns = 0;
            for (unsigned k = 0; k < lay.n; k++)
                if (layout_selectable(&lay, k, jp))
                    sel[ns++] = k;
            if (ns < 2 || lay.line[sel[0]] == lay.line[sel[ns - 1]])
                continue; /* a one-row text goes round with the d-pad: its history is not in order */
            VjoRowNav nav;
            memset(&nav, 0, sizeof(nav));
            size_t c = lay.begin[sel[0]];
            int word = 1;
            sessions++;
            for (int step = 0; step < 300; step++) {
                uint32_t pick = lcg() % 20;
                size_t out = c, end = 0;
                int wrapped;
                if (pick < 6) { /* the d-pad's Right, with a shown word's end */
                    if (word && lcg() % 4) {
                        size_t e = c;
                        for (int k = 1 + (int)(lcg() % 4); k-- > 0 && e < n;)
                            vjo_utf8_next(text, n, &e);
                        end = e;
                    }
                    vjo_text_row_key(&nav, text, n, &r, c, end, 1, 0, jp, 0, &out);
                } else if (pick < 9) { /* the d-pad's Left */
                    unsigned before = nav.words.count;
                    size_t want = before && nav.words.previous[before - 1] != c ? nav.words.previous[before - 1] : 0;
                    vjo_text_row_key(&nav, text, n, &r, c, 0, -1, 0, jp, 0, &out);
                    if (before) {
                        TEST_CHECK_(out == want, "text %d jp %d: the d-pad's Left from %zu: %zu, the history's %zu", t,
                                    jp, c, out, want);
                        retraced++;
                    }
                } else if (pick < 15) { /* the stick's right or left */
                    int d = pick < 12 ? 1 : -1;
                    size_t want = ref_read_step(&lay, c, c + 1, d, jp, 0, &wrapped);
                    vjo_text_row_key(&nav, text, n, &r, c, word ? n : 0, d, 0, jp, 1, &out);
                    TEST_CHECK_(out == want, "text %d jp %d: the stick's %+d from %zu: %zu, expected %zu", t, jp, d, c,
                                out, want);
                    stick_keys++;
                } else if (pick < 18) { /* Up/Down */
                    vjo_text_row_key(&nav, text, n, &r, c, 0, 0, pick < 17 ? -1 : 1, jp, lcg() % 2, &out);
                } else if (pick < 19) { /* a tap on a selectable character */
                    out = lay.begin[sel[lcg() % ns]];
                    nav.words.count = 0;
                } else { /* Character mode and back (render clears the history) */
                    word = !word;
                    nav.words.count = 0;
                }
                c = out;
                keys++;
                for (unsigned i = 0; i < nav.words.count; i++)
                    TEST_CHECK_(nav.words.previous[i] < c && (!i || nav.words.previous[i - 1] < nav.words.previous[i]),
                                "text %d jp %d step %d: history[%u] = %u, cursor %zu", t, jp, step, i,
                                nav.words.previous[i], c);
                TEST_CHECK(layout_selectable(&lay, (unsigned)layout_at(&lay, c), jp));
            }
            TEST_CHECK(!nav.wraps && lay.bad_queries == 0);
        }
    }
    TEST_CHECK(sessions > 200 && retraced > 5000);
    printf("\n  %u mixed sessions, %u keys (%u the stick's, %u Lefts retracing): the history stays in order\n",
           sessions, keys, stick_keys, retraced);
}

TEST_LIST = {
    {"wrapped_line_rows_and_their_ends", wrapped_line_rows_and_their_ends},
    {"a_cell_never_ends_on_the_next_row", a_cell_never_ends_on_the_next_row},
    {"column_kept_across_a_short_row", column_kept_across_a_short_row},
    {"mixed_widths_use_measured_cells", mixed_widths_use_measured_cells},
    {"punctuation_and_hanging_kinsoku", punctuation_and_hanging_kinsoku},
    {"cr_newline_and_surrogate_pairs", cr_newline_and_surrogate_pairs},
    {"a_line_break_at_an_end_is_not_a_row", a_line_break_at_an_end_is_not_a_row},
    {"unavailable_layout_changes_nothing", unavailable_layout_changes_nothing},
    {"whole_words_cross_rows", whole_words_cross_rows},
    {"left_never_retraces_onto_the_cursor", left_never_retraces_onto_the_cursor},
    {"row_keys_forget_the_row_they_leave", row_keys_forget_the_row_they_leave},
    {"generated_texts_follow_the_rules", generated_texts_follow_the_rules},
    {"the_stick_steps_a_character_at_a_time", the_stick_steps_a_character_at_a_time},
    {"the_sticks_steps_in_generated_texts", the_sticks_steps_in_generated_texts},
    {"mixed_sessions_keep_the_history_in_order", mixed_sessions_keep_the_history_in_order},
    {NULL, NULL}};
