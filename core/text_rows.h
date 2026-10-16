/* Cursor movement by the header's displayed rows (#81 requests 1 and 2):
 * Up/Down to the row above or below, soft-wrapped rows included, and
 * Left/Right wrapping at a row's ends. Rows come from the real renderer
 * through the touch hit test's callback (text_hit.h): each rendered
 * character's start x and laid-out line, by UTF-16 index (CR has no unit, a
 * surrogate pair is queried at its first unit). Lines must not decrease in
 * reading order, as in any left-to-right layout. Nothing is allocated or
 * estimated; a failed or out-of-range query means "not laid out yet". */
#ifndef VJO_TEXT_ROWS_H
#define VJO_TEXT_ROWS_H
#include <stddef.h>
#include <stdint.h>
#include "text_hit.h"

typedef struct {
    VjoCharPosition position;
    void *ctx;
    uint32_t lines;       /* laid-out lines (GetLineCount) */
    float terminal_width; /* a row's last cell has no next start: the Japanese point size */
} VjoTextRows;

enum { VJO_ROW_UNAVAILABLE = -1, VJO_ROW_STAY = 0, VJO_ROW_MOVED = 1, VJO_ROW_WRAPPED = 2 };

/* A character that can hold the cursor: a Japanese letter (as
 * vjo_utf8_japanese_cursor_step selects) when japanese_only, otherwise any
 * visible one; spaces (also U+3000), line breaks and controls never.
 * bytes: its UTF-8 length. */
int vjo_text_selectable(uint32_t cp, size_t bytes, int japanese_only);

/* Left/Right on the cursor's row. direction > 0: the first selectable
 * character at or after byte `from` (> cursor: the next character, or the
 * end of the matched word) if it is on the cursor's row, else the row's
 * first one (VJO_ROW_WRAPPED). direction < 0: the row's selectable character
 * before the cursor, else the row's last (wrapped). A row without any (the
 * cursor on a blank) steps to the nearest selectable character instead.
 * STAY: nowhere else to go. *out is written only on a move. */
int vjo_text_row_step(const char *text, size_t len, const VjoTextRows *rows, size_t cursor,
                      size_t from, int direction, int japanese_only, size_t *out);

/* Up/Down's column: kept while the cursor stays where they left it, so
 * repeated moves keep it across a short row. at: 1 + that cursor; 0 (as
 * zero-initialized) means no column. */
typedef struct {
    float x;
    uint32_t at;
} VjoRowGoal;

/* Up (direction < 0) / Down: the nearest row above/below that has a
 * selectable character, and on it the one whose cell is nearest the goal
 * column (inside it, else the closest edge; the earlier on a tie). Without
 * a goal for this cursor, the column is the centre of the cursor's cell.
 * A move records the column and *out in *goal. STAY: no such row. */
int vjo_text_row_vertical(const char *text, size_t len, const VjoTextRows *rows, size_t cursor,
                          int direction, int japanese_only, VjoRowGoal *goal, size_t *out);

/* Left/Right in both highlight modes, as the overlay's arrows move. Whole
 * word: Right skips the matched source span [cursor, matched_end) and
 * records where it started; Left retraces up to 16 of those (a wrap too),
 * else steps one character back and forgets them. Character mode (and a
 * pending or failed lookup) passes matched_end 0: one character. With rows
 * the cursor stays on its row (vjo_text_row_step); without them (NULL, or
 * not laid out yet) it steps through the text as before. *row_result: the
 * row step's VJO_ROW_*, MOVED for a retrace, UNAVAILABLE for a step through
 * the text. */
typedef struct {
    uint32_t previous[16];
    unsigned count;
} VjoWordNavigation;
size_t vjo_word_step(VjoWordNavigation *history, const char *text, size_t len, size_t cursor,
                     size_t matched_end, int direction, int japanese_only, const VjoTextRows *rows,
                     int *row_result);

/* The overlay's arrow keys over the header: Left/Right (dx) by
 * vjo_word_step, which drops Up/Down's column, else Up/Down (dy) by
 * vjo_text_row_vertical; a row change forgets Right's history, so Left
 * retraces the new row's words only. Returns the VJO_ROW_* result; *out is
 * the new cursor (the old one if it stays). The counts are for the
 * overlay's close log. */
typedef struct {
    VjoWordNavigation words;
    VjoRowGoal goal;
    int row_moves, wraps, no_layout;
} VjoRowNav;
int vjo_text_row_key(VjoRowNav *nav, const char *text, size_t len, const VjoTextRows *rows, size_t cursor,
                     size_t matched_end, int dx, int dy, int japanese_only, size_t *out);

/* The left stick along the text (#138, core/stick_cursor.h): direction > 0
 * the first selectable character at or after matched_end (the matched word
 * skipped, as vjo_word_step skips it) or after the cursor, direction < 0 the
 * last one before the cursor; in reading order, across the displayed rows,
 * and never round the text's ends: at its first or last selectable character
 * the cursor stays (returned as it is). No history: it is the d-pad's. */
size_t vjo_text_read_step(const char *text, size_t len, size_t cursor, size_t matched_end, int direction,
                          int japanese_only);
#endif
