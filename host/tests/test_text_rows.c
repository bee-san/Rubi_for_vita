/* The overlay's arrow keys over the header's displayed rows (#81 requests 1
 * and 2), on layout fixtures from host/tests/layout_double.h: rows of 10
 * full-width cells (24 px in a 240 px box), soft wraps, hard line breaks,
 * mixed full-width/narrow advances, kinsoku hanging punctuation, CR, UTF-16
 * surrogate pairs, unavailable layouts. A differential check then compares
 * every key from every character of generated texts with the rules restated
 * over the laid-out cells (ref_step/ref_vertical). */
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

/* Left/Right in Character mode: returns the cursor, *rc the row result. */
static size_t step(const char *s, size_t cursor, int dx, int jp, int *rc)
{
    VjoTextRows r = layout_rows(&lay);
    size_t out = cursor;
    *rc = vjo_text_row_step(s, strlen(s), &r, cursor, cursor + 1, dx, jp, &out);
    return out;
}

static void wrapped_line_rows_and_wrap_at_row_ends(void)
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
    TEST_CHECK(step(s, 27, 1, 1, &rc) == 0 && rc == VJO_ROW_WRAPPED);  /* Right from the row's last */
    TEST_CHECK(step(s, 0, 1, 1, &rc) == 3 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 0, -1, 1, &rc) == 27 && rc == VJO_ROW_WRAPPED); /* Left from its first */
    TEST_CHECK(step(s, 30, -1, 1, &rc) == 39 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 39, 1, 1, &rc) == 30 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 33, -1, 0, &rc) == 30 && rc == VJO_ROW_MOVED);
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
    TEST_CHECK(step(s, 13, 1, 1, &rc) == 10 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 10, -1, 1, &rc) == 13 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 9, 1, 0, &rc) == 10 && rc == VJO_ROW_MOVED);
    TEST_CHECK(step(s, 13, 1, 0, &rc) == 0 && rc == VJO_ROW_WRAPPED); /* the line break is skipped */
}

static void punctuation_and_hanging_kinsoku(void)
{
    /* 。 may not start a row: it hangs past the edge of row 0 (x 240). */
    const char *s = "あいうえおかきくけこ。さし";
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.line[10] == 0 && lay.x[10] == 240.0f);
    TEST_CHECK(step(s, 27, 1, 1, &rc) == 0 && rc == VJO_ROW_WRAPPED);   /* Japanese-only skips 。 */
    TEST_CHECK(step(s, 0, -1, 1, &rc) == 27 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 27, 1, 0, &rc) == 30 && rc == VJO_ROW_MOVED);    /* off: 。 is a character */
    TEST_CHECK(step(s, 30, 1, 0, &rc) == 0 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 0, -1, 0, &rc) == 30 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(up_down(s, 30, 1, 0) == 36);                             /* 。 (240..264) -> し */
    const char *t = "あいうえおかきくけ、さしす"; /* 、 fits as row 0's 10th cell */
    TEST_ASSERT(layout_text(&lay, t, W, JA, NARROW) == 2);
    TEST_CHECK(step(t, 24, 1, 1, &rc) == 0 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(t, 24, 1, 0, &rc) == 27 && rc == VJO_ROW_MOVED);
    /* Spaces (also U+3000) and line breaks never hold the cursor. */
    const char *u = "A B\xe3\x80\x80猫\n犬";
    TEST_ASSERT(layout_text(&lay, u, W, JA, NARROW) == 2);
    TEST_CHECK(step(u, 0, 1, 0, &rc) == 2 && step(u, 2, 1, 0, &rc) == 6);
    TEST_CHECK(step(u, 6, 1, 0, &rc) == 0 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(u, 1, 1, 0, &rc) == 2); /* from a blank: the next character */
    /* DEL and C1 controls are not visible characters either. */
    const char *v = "A\x7f" "B\xc2\x85" "C";
    TEST_ASSERT(layout_text(&lay, v, W, JA, NARROW) == 1);
    TEST_CHECK(step(v, 0, 1, 0, &rc) == 2 && step(v, 2, 1, 0, &rc) == 5);
    TEST_CHECK(step(v, 5, 1, 0, &rc) == 0 && rc == VJO_ROW_WRAPPED);
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
    TEST_CHECK(step(s, 5, -1, 1, &rc) == 12 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(step(s, 0, 1, 1, &rc) == 0 && rc == VJO_ROW_STAY); /* 猫 is alone on its row */
    TEST_CHECK(step(s, 3, 1, 0, &rc) == 0 && rc == VJO_ROW_WRAPPED); /* a cursor on CR: its row's */
    TEST_CHECK(lay.bad_queries == 0);
}

static int nan_position(void *ctx, uint32_t unit, float *x, uint32_t *line)
{
    int rc = layout_position(ctx, unit, x, line);
    if (unit == 4)
        *x = NAN;
    return rc;
}

static void unavailable_layout_changes_nothing(void)
{
    const char *s = "猫\r\n𠮷野家";
    VjoRowGoal goal = {0, 0};
    size_t out = 99, len = strlen(s);
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    VjoTextRows r = layout_rows(&lay);
    lay.fail_unit = 4; /* 野, a selectable cell */
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, 0, 1, 1, &goal, &out) == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 5, 6, 1, 1, &out) == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(out == 99 && goal.at == 0);
    lay.fail_unit = 1; /* the line break: it only ends 猫's cell early */
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, 0, 1, 1, &goal, &out) == VJO_ROW_MOVED && out == 5);
    lay.fail_unit = -1;
    r.lines = 1; /* a position on a line the layout does not have */
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, 0, 1, 1, &goal, &out) == VJO_ROW_UNAVAILABLE);
    r = layout_rows(&lay);
    r.position = nan_position;
    TEST_CHECK(vjo_text_row_step(s, len, &r, 5, 6, 1, 1, &out) == VJO_ROW_UNAVAILABLE);
    r = layout_rows(&lay);
    r.terminal_width = 0;
    TEST_CHECK(vjo_text_row_step(s, len, &r, 5, 6, 1, 1, &out) == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_text_row_vertical(s, len, NULL, 0, 1, 1, &goal, &out) == VJO_ROW_UNAVAILABLE);
    r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_vertical(s, len, &r, len, 1, 1, &goal, &out) == VJO_ROW_STAY);
    TEST_CHECK(vjo_text_row_step("", 0, &r, 0, 1, 1, 1, &out) == VJO_ROW_STAY);
    TEST_CHECK(vjo_text_row_step(s, len, &r, 0, 1, 0, 1, &out) == VJO_ROW_STAY);
    TEST_CHECK(out == 5);
    /* Without a layout the whole text is one row: the arrows still move. */
    VjoWordNavigation h = {{0}, 0};
    int rc;
    TEST_CHECK(vjo_word_step(&h, s, len, 12, 0, 1, 1, NULL, &rc) == 0 && rc == VJO_ROW_UNAVAILABLE);
    TEST_CHECK(vjo_word_step(&h, s, len, 0, 0, 1, 1, NULL, &rc) == 5 && rc == VJO_ROW_UNAVAILABLE);
    lay.fail_unit = 4;
    r = layout_rows(&lay);
    h.count = 0;
    TEST_CHECK(vjo_word_step(&h, s, len, 5, 0, 1, 1, &r, &rc) == 9 && rc == VJO_ROW_UNAVAILABLE);
}

static void whole_words_stay_on_their_row(void)
{
    /* Row 0: 猫 が 好 き で す 。 犬 も 好 | row 1: き で す 。 (好き crosses). */
    const char *s = "猫が好きです。犬も好きです。";
    size_t len = strlen(s), cursor = 0;
    static const size_t ends[][2] = {{0, 3}, {3, 6}, {6, 12}, {12, 18}, {21, 24}, {24, 27}, {27, 33}};
    VjoWordNavigation h = {{0}, 0};
    int rc;
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    TEST_CHECK(lay.line[9] == 0 && lay.line[10] == 1);
    VjoTextRows r = layout_rows(&lay);
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        TEST_ASSERT(cursor == ends[i][0]);
        cursor = vjo_word_step(&h, s, len, cursor, ends[i][1], 1, 1, &r, &rc);
        TEST_CHECK(rc == (i + 1 < sizeof(ends) / sizeof(ends[0]) ? VJO_ROW_MOVED : VJO_ROW_WRAPPED));
    }
    TEST_CHECK(cursor == 0 && h.count == 7); /* 好き ends on row 1: back to the row's first word */
    for (size_t i = sizeof(ends) / sizeof(ends[0]); i-- > 0;) {
        cursor = vjo_word_step(&h, s, len, cursor, 0, -1, 1, &r, &rc);
        TEST_CHECK(cursor == ends[i][0] && rc == VJO_ROW_MOVED); /* Left retraces, the wrap first */
    }
    TEST_CHECK(h.count == 0);
    cursor = vjo_word_step(&h, s, len, 0, 0, -1, 1, &r, &rc);
    TEST_CHECK(cursor == 27 && rc == VJO_ROW_WRAPPED); /* no history: the row's last character */
    TEST_CHECK(vjo_word_step(&h, s, len, 27, 0, -1, 1, &r, &rc) == 24 && h.count == 0);
    /* Row 1: で's match runs to the end, 。 is not Japanese: wrap. */
    TEST_CHECK(vjo_word_step(&h, s, len, 30, 33, 1, 1, &r, &rc) == 33);
    TEST_CHECK(vjo_word_step(&h, s, len, 33, 39, 1, 1, &r, &rc) == 30 && rc == VJO_ROW_WRAPPED);
    TEST_CHECK(vjo_word_step(&h, s, len, 33, len, 1, 0, &r, &rc) == 30 && rc == VJO_ROW_WRAPPED);
    /* A pending or failed lookup, or a match end inside a character: one character. */
    h.count = 0;
    TEST_CHECK(vjo_word_step(&h, s, len, 6, 0, 1, 1, &r, &rc) == 9);
    TEST_CHECK(vjo_word_step(&h, s, len, 6, 10, 1, 1, &r, &rc) == 9);
    TEST_CHECK(vjo_word_step(&h, s, len, 6, len + 1, 1, 1, &r, &rc) == 9);
    TEST_CHECK(h.count == 3);
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
    TEST_CHECK(vjo_word_step(&h, s, len, 6, 0, -1, 1, &r, &rc) == 3 && rc == VJO_ROW_MOVED);
    TEST_CHECK(h.count == 0); /* the step back forgot 0 too */
    TEST_CHECK(vjo_word_step(&h, s, len, 3, 0, -1, 1, &r, &rc) == 0 && h.count == 0);
    h = (VjoWordNavigation){{99}, 1}; /* past the text */
    TEST_CHECK(vjo_word_step(&h, s, len, 0, 0, -1, 1, &r, &rc) == 12 && rc == VJO_ROW_WRAPPED && h.count == 0);
}

static void row_keys_forget_the_row_they_leave(void)
{
    const char *s = "猫が好きです。犬も好きです。";
    size_t len = strlen(s), out = 0;
    VjoRowNav nav;
    memset(&nav, 0, sizeof(nav));
    TEST_ASSERT(layout_text(&lay, s, W, JA, NARROW) == 2);
    VjoTextRows r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 0, 3, 1, 0, 1, &out) == VJO_ROW_MOVED && out == 3);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 3, 6, 1, 0, 1, &out) == VJO_ROW_MOVED && out == 6);
    TEST_CHECK(nav.words.count == 2);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 6, 12, 0, 1, 1, &out) == VJO_ROW_MOVED);
    TEST_CHECK(out == 36 && nav.words.count == 0 && nav.row_moves == 1); /* 好 (48..72) -> す (48..72) */
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 36, 0, -1, 0, 1, &out) == VJO_ROW_MOVED && out == 33);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 33, 0, -1, 0, 1, &out) == VJO_ROW_MOVED && out == 30);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 30, 0, -1, 0, 1, &out) == VJO_ROW_WRAPPED && out == 36);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, &r, 36, 0, 0, 1, 1, &out) == VJO_ROW_STAY && out == 36);
    TEST_CHECK(vjo_text_row_key(&nav, s, len, NULL, 36, 0, 0, -1, 1, &out) == VJO_ROW_UNAVAILABLE && out == 36);
    TEST_CHECK(nav.row_moves == 1 && nav.wraps == 1 && nav.no_layout == 1);
    /* Left/Right take Up/Down's column afresh, even back on the same cell:
     * け (column 8) Down to し keeps 204; Right wraps to さ, Left retraces to
     * し; Down then goes by し's own centre (36) to ち, not ね. */
    const char *t = "あいうえおかきくけこ\nさし\nたちつてとなにぬねの";
    size_t tlen = strlen(t);
    memset(&nav, 0, sizeof(nav));
    TEST_ASSERT(layout_text(&lay, t, W, JA, NARROW) == 3);
    r = layout_rows(&lay);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 24, 0, 0, 1, 1, &out) == VJO_ROW_MOVED && out == 34);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 34, 0, 1, 0, 1, &out) == VJO_ROW_WRAPPED && out == 31);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 31, 0, -1, 0, 1, &out) == VJO_ROW_MOVED && out == 34);
    TEST_CHECK(vjo_text_row_key(&nav, t, tlen, &r, 34, 0, 0, 1, 1, &out) == VJO_ROW_MOVED && out == 41);
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
    unsigned checks = 0, moves = 0, wraps = 0, rows_total = 0;
    for (int t = 0; t < 120; t++) {
        size_t n = 0, pieces_n = 2 + lcg() % 90;
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
        for (unsigned k = 0; k < lay.n; k++) {
            size_t c = lay.begin[k];
            for (int jp = 0; jp <= 1; jp++) {
                for (int d = -1; d <= 1; d += 2) {
                    int wrapped, rc;
                    size_t want = ref_step(&lay, c, c + 1, d, jp, &wrapped), out = c;
                    rc = vjo_text_row_step(text, n, &r, c, c + 1, d, jp, &out);
                    TEST_CHECK_(out == want && rc == (want == c ? VJO_ROW_STAY : wrapped ? VJO_ROW_WRAPPED : VJO_ROW_MOVED),
                                "text %d byte %zu jp %d dx %d: %zu rc %d, expected %zu wrapped %d", t, c, jp, d, out, rc,
                                want, wrapped);
                    /* a whole word: Right from the end of the next selectable character */
                    size_t from = k + 2 < lay.n ? lay.begin[k + 2] : n;
                    want = ref_step(&lay, c, from, 1, jp, &wrapped);
                    out = c;
                    rc = vjo_text_row_step(text, n, &r, c, from, 1, jp, &out);
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
                    checks += 4;
                }
                /* Right from a selectable character comes back to it after
                 * visiting each selectable character of its row once. */
                if (layout_selectable(&lay, k, jp)) {
                    unsigned row_cells = 0, steps = 0;
                    for (unsigned j = 0; j < lay.n; j++)
                        row_cells += lay.line[j] == lay.line[k] && layout_selectable(&lay, j, jp);
                    size_t at = c;
                    int rc;
                    do {
                        size_t next = c;
                        rc = vjo_text_row_step(text, n, &r, at, at + 1, 1, jp, &next);
                        TEST_ASSERT(rc >= 0);
                        if (rc == VJO_ROW_STAY) break;
                        TEST_CHECK(lay.line[layout_at(&lay, next)] == lay.line[k]);
                        moves++;
                        wraps += rc == VJO_ROW_WRAPPED;
                        at = next;
                    } while (++steps <= row_cells && at != c);
                    TEST_CHECK_(at == c && (rc == VJO_ROW_STAY ? row_cells == 1 : steps == row_cells),
                                "text %d byte %zu: back after %u of %u", t, c, steps, row_cells);
                }
            }
        }
        TEST_CHECK(lay.bad_queries == 0);
    }
    printf("\n  %u key checks on 120 generated texts (%u rows), %u Right steps around rows, %u wraps\n",
           checks, rows_total, moves, wraps);
}

TEST_LIST = {
    {"wrapped_line_rows_and_wrap_at_row_ends", wrapped_line_rows_and_wrap_at_row_ends},
    {"a_cell_never_ends_on_the_next_row", a_cell_never_ends_on_the_next_row},
    {"column_kept_across_a_short_row", column_kept_across_a_short_row},
    {"mixed_widths_use_measured_cells", mixed_widths_use_measured_cells},
    {"punctuation_and_hanging_kinsoku", punctuation_and_hanging_kinsoku},
    {"cr_newline_and_surrogate_pairs", cr_newline_and_surrogate_pairs},
    {"unavailable_layout_changes_nothing", unavailable_layout_changes_nothing},
    {"whole_words_stay_on_their_row", whole_words_stay_on_their_row},
    {"left_never_retraces_onto_the_cursor", left_never_retraces_onto_the_cursor},
    {"row_keys_forget_the_row_they_leave", row_keys_forget_the_row_they_leave},
    {"generated_texts_follow_the_rules", generated_texts_follow_the_rules},
    {NULL, NULL}};
