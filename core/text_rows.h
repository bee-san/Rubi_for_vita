/* Cursor movement over the header's displayed rows: Up/Down to the row above
 * or below, soft-wrapped rows included (#81 request 1), and Left/Right in
 * reading order across rows and line breaks, stopping at the text's first and
 * last selectable characters (the owner, 2026-10-16, item 1; it replaces #81
 * request 2's wrap at each row's ends, test.3's item G). Only a text whose
 * selectable characters are all on one displayed row still wraps round its
 * ends, as #81 asked. Rows come from the real renderer through the touch hit
 * test's callback (text_hit.h): each rendered character's start x and
 * laid-out line, by UTF-16 index (CR has no unit, a surrogate pair is queried
 * at its first unit). Lines must not decrease in reading order, as in any
 * left-to-right layout. Nothing is allocated or estimated; a failed or
 * out-of-range query means "not laid out yet". */
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

/* Left/Right in reading order, whatever row the next character is on.
 * direction > 0: the first selectable character at or after byte `from`
 * (> cursor: the next character, or the end of the matched word, on a later
 * row too); direction < 0: the last selectable character before the cursor.
 * Past the text's last (first) one the cursor stays (STAY), except that with
 * `wrap` a text whose selectable characters are all on one displayed row goes
 * round to its first (last): VJO_ROW_WRAPPED. Only that end asks the layout,
 * for the rows of the text's first and last selectable characters (two
 * queries); without it (rows NULL, not laid out yet, or a position paf does
 * not give) the line breaks stand for the rows, a text with none between them
 * taken as one row, and the result is VJO_ROW_UNAVAILABLE (with *out as that
 * rule has it). *out is written only on a move. */
int vjo_text_row_step(const char *text, size_t len, const VjoTextRows *rows, size_t cursor,
                      size_t from, int direction, int japanese_only, int wrap, size_t *out);

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

/* Left/Right in both highlight modes, as the overlay's arrows and the left
 * stick move. Whole word: Right skips the matched source span [cursor,
 * matched_end), wherever it ends, and records where it started; Left retraces
 * up to 16 of those (a wrap too), else steps one character back and forgets
 * them. Character mode (and a pending or failed lookup) passes matched_end 0:
 * one character. The step is vjo_text_row_step's (rows may be NULL), round a
 * one-row text's ends only with `wrap`. *row_result: its VJO_ROW_*, MOVED
 * for a retrace. */
typedef struct {
    uint32_t previous[16];
    unsigned count;
} VjoWordNavigation;
size_t vjo_word_step(VjoWordNavigation *history, const char *text, size_t len, size_t cursor,
                     size_t matched_end, int direction, int japanese_only, const VjoTextRows *rows,
                     int wrap, int *row_result);

/* The overlay's cursor keys over the header, the d-pad's arrows and the left
 * stick's four directions alike: Left/Right (dx) by vjo_word_step, which
 * drops Up/Down's column, else Up/Down (dy) by vjo_text_row_vertical; Up/Down
 * forget Right's history, so Left then retraces only the Rights made since
 * (across rows). One history and one column for both. `stick`: the left
 * stick's step (#138, core/stick_cursor.h), which never goes round a one-row
 * text's ends itself, as a held stick stops at the ends of the Rubi app's
 * lists (its Left still retraces a d-pad Right that went round), and is not
 * counted: the counts are the arrows', for the overlay's close log. Returns the
 * VJO_ROW_* result; *out is the new cursor (the old one if it stays). */
typedef struct {
    VjoWordNavigation words;
    VjoRowGoal goal;
    int row_moves, wraps, no_layout;
} VjoRowNav;
int vjo_text_row_key(VjoRowNav *nav, const char *text, size_t len, const VjoTextRows *rows, size_t cursor,
                     size_t matched_end, int dx, int dy, int japanese_only, int stick, size_t *out);
#endif
