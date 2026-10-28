/* history.csv's records, read in the Rubi app as the Shell reads them (core/history_lines.c), for the app's pages
 * that read the file: the Look up page's earlier lines (#313) and the reading streak's seed (settings/streak_seed.c,
 * core/streak_scan.c). docs/ocr-history.md has the
 * format. No I/O and no allocation: a caller reads the file a window at a time, as shell/history.c does.
 *
 * A window is buf[0, len): bytes of the file and, unless `whole` (the window starts at the file's first byte),
 * buf[0] is the byte before the window, which only says whether the window's first byte starts a record. A record
 * starts at a '"' at the start of a line (after a '\n', or as the file's first byte) whose next byte is neither '"'
 * nor ',' (inside a field every '"' is doubled, a field's closing '"' is followed by ',' or a line break, and the
 * header line starts "uuid,"), and it runs to the next record's start. These are core/history_lines.c's own
 * boundaries and rules, which its functions keep private (they are the Shell's, whose code must not change):
 * host/tests/test_history_read.c checks these against them on random files. */
#ifndef VJO_HISTORY_READ_H
#define VJO_HISTORY_READ_H

#include <stddef.h>
#include <stdint.h>

/* The columns by position (docs/ocr-history.md "The format and older files"): every version keeps each column in
 * its place; 0.8.0 wrote the 41 up to line_metrics, 0.9.0 added characters, a later one line_source. A file keeps
 * the header it began with, so its rows can have more cells than its header names: read by position. */
enum {
    VJO_HISTORY_COL_UUID = 0,     /* "vita-PCSG00415": the platform and the title ID */
    VJO_HISTORY_COL_ID = 1,       /* given_identifier: "vjo-<time us>-<offset>" */
    VJO_HISTORY_COL_NAME = 2,     /* the game's title, else its ID */
    VJO_HISTORY_COL_LINE = 3,     /* the text, its wrapped lines kept */
    VJO_HISTORY_COL_TIME = 4,     /* UTC Unix seconds, six fractional digits */
    VJO_HISTORY_COL_PLATFORM = 5, /* "vita" | "psp" */
    VJO_HISTORY_COL_TITLE_ID = 6,
    VJO_HISTORY_COL_ENGINE = 7,   /* "vocr", or "hook" for a native text hook's line */
    VJO_HISTORY_COL_MODEL = 8,
    VJO_HISTORY_COL_RAW_LINE = 9,
    VJO_HISTORY_COL_LINE_METRICS = 40,
    VJO_HISTORY_COL_CHARACTERS = 41,
    VJO_HISTORY_COL_LINE_SOURCE = 42,
    VJO_HISTORY_COLUMNS = 43      /* Rubi's columns today */
};
/* The cells a row keeps: Rubi's 43 and a few more a later version may append. */
#define VJO_HISTORY_CELLS 48

typedef struct {
    /* The first four cells (always there: a record without them is damaged), unescaped and NUL-terminated. */
    char *uuid, *id, *name, *line;
    /* The time cell as the Shell reads it: up to 12 digits, '.', exactly six digits and a ',' after them, in
     * microseconds; 0 for anything else (an invalid time, which does not make the record damaged). */
    uint64_t time_us;
    /* Every cell parsed, in order: cell[0..3] are the four above, cell[4] the time's text, then as many more as
     * vjo_history_row's `want` asks for and the record has; the rest NULL. Each one unescaped ("" read as ") and
     * NUL-terminated in place; len[k] is cell k's length in bytes, which is the one to count with: a damaged file
     * can hold zeros (a power loss), and the Shell counts a line by its length (core/history_lines.c). */
    int cells;
    char *cell[VJO_HISTORY_CELLS];
    uint32_t len[VJO_HISTORY_CELLS];
} VjoHistoryRow;

/* 1 when a record starts at buf[i] of the window buf[0, len) (above): core/history_lines.c's record_starts, and
 * never at i 0 of a window that is not `whole` (buf[0] is the byte before it). */
int vjo_history_record_start(const char *buf, size_t i, size_t len, int whole);
/* The first record start at or after `from`, or len when there is none. */
size_t vjo_history_next_start(const char *buf, size_t from, size_t len, int whole);
/* The last record start before `before`, or len when there is none: from the window's end (before = len) back, the
 * records newest first, each running to the start found before it (core/history_lines.c vjo_history_scan's walk). */
size_t vjo_history_prev_start(const char *buf, size_t before, size_t len, int whole);

/* The record in [start, end): start a record's start, end the next record's (or the window's end). Its cells into
 * row, the first four and the time always, then the cells after them until `want` (4 to VJO_HISTORY_CELLS) cells
 * are parsed or the record ends; a cell the window's end cuts is not kept. Returns 0, or -1 for a damaged record,
 * by the Shell's rule (core/history_lines.c record_line): its first four cells must each be quoted and followed by a
 * ',', and the fourth's ',' by a digit (a record a power loss cut, run into the next boot's append, has another
 * field's '"' there); the Shell counts no line of such a record and breaks the reading time at it.
 * Unescapes in place: it writes only inside [start, end), so find a record's end before parsing it, as both walks
 * above do (they read nothing inside a record found before). */
int vjo_history_row(char *start, char *end, int want, VjoHistoryRow *row);

/* The game's 32-bit FNV-1a over a uuid cell's len bytes (VjoHistoryRow.len[0]), as core/history_lines.c hashes a
 * record's uuid for the reading time: the `game` of vjo_reading_time_add and vjo_reading_scope_add (whose scope's
 * uuid it hashes the same way, by its strlen). */
uint32_t vjo_history_game(const char *uuid, size_t len);

/* The 0-based position of column `name` in a header line (the file's first line, header[0, len), its line break
 * included: its names are separated by ',' up to it), or -1 when the header has no such name (a cut header, an
 * older file) or the buffer's end cuts that name before a ',' or the line break. */
int vjo_history_column(const char *header, size_t len, const char *name);

#endif
