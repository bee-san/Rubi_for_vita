/* Line history (the owner, 2026-10-20: "Mine from history"): in the overlay, Up from the
 * text's top row shows this game's earlier lines, one at a time, as history.csv keeps them
 * (docs/line-history.md). history.csv has only Read text: Automatically's lines (docs/ocr-history.md),
 * so line history is that mode's: nothing else records a line for it.
 *
 * This file reads them out of the file's last bytes: no allocation of its own, no I/O
 * (shell/history.c reads them). */
#ifndef VJO_HISTORY_LINES_H
#define VJO_HISTORY_LINES_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

/* One visit (from the first Up to the return to the current text) keeps at most this many earlier
 * lines, and only as many as fit its block (VJO_BUDGET_PAF_HISTORY: this table, the current text and
 * the lines), from the last VJO_HISTORY_WINDOW bytes of history.csv (read into the control thread's
 * 24 KiB scratch, with the byte before them). */
#define VJO_HISTORY_LINES 32
#define VJO_HISTORY_WINDOW 24512u

enum { VJO_LINE_UNKNOWN, VJO_LINE_OCR, VJO_LINE_HOOK };

typedef struct {
    const char *line[VJO_HISTORY_LINES + 2]; /* [0] the current text, [k] the k-th earlier line; NULL past
                                              * line[n] (zeroed by the caller, kept so by the scan) */
    int n;                                   /* earlier lines: line[1..n] */
    int at;                                  /* the line shown: 0 the current text */
    uint8_t origin[VJO_HISTORY_LINES + 2]; /* saved engine; unknown for older/incomplete records */
} VjoHistory;

/* Adds the earlier lines of game `uuid` (history.csv's first column with the quote and comma after it:
 * "vita-PCSG00415\",") in buf[0, len),
 * the file's last bytes, newest first, to h (h->line[0] set): each record's line (its fourth column,
 * unescaped in place in buf), less its rows without Japanese when filter is VJO_FILTER_LINES
 * (core/textfilter.h, as the overlay shows a text) and trimmed, copied into `lines`. A line that is
 * empty then, or the same as the line after it (the one kept last), is not kept; an incomplete record
 * (one being appended, or cut by the window's start) is skipped, and so are the records newer than the
 * current text's newest one (h->line[0]; a refresh's line the visit's opening dropped). buf[0] is the byte before the window,
 * or the file's first byte when `whole` (the window is the whole file). Stops once h or `lines` is
 * full: the lines kept are always the newest, without a gap. */
void vjo_history_scan(VjoHistory *h, VjoArena *lines, char *buf, size_t len, int whole, const char *uuid, int filter);

/* The characters a line counts in history.csv's `characters` column (core/reading_chars.h): the writer's
 * count, and the total's. */
uint32_t vjo_line_characters(const char *s, size_t len);

/* Active reading: only intervals (0, 15 s] between adjacent records of the same game.
 * timed_chars belongs to the earlier line; untimed lines never inflate the speed. Stored with
 * the total so incremental scans and restarts retain the interval at their boundary. */
typedef struct {
    uint64_t active_us, timed_chars, last_us;
    uint32_t last_game, last_chars;
} VjoReadingTime;
/* One selected game's daily counters. UTC bounds represent local midnight on the Vita.
 * Empty uuid retains the unfiltered total used by offline tools. */
typedef struct {
    char uuid[24];
    uint64_t start_us, end_us, chars, active_us, timed_chars;
} VjoReadingScope;
void vjo_reading_scope_add(VjoReadingTime *t, VjoReadingScope *scope,
                           uint64_t us, uint32_t game, uint32_t chars);
uint32_t vjo_history_count_scope(char *buf, size_t len, int whole, int cut,
    size_t *first, size_t *last, VjoReadingTime *time, VjoReadingScope *scope);
void vjo_reading_time_add(VjoReadingTime *t, uint64_t us, uint32_t game, uint32_t chars);
/* Like vjo_history_chars, also updating reading time in file order, across window boundaries. */
uint32_t vjo_history_count(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last,
                           VjoReadingTime *time);

/* The total (docs/ocr-history.md#total-characters-read): the characters of the lines of the records that
 * start in buf[0, len), a window of history.csv read with the byte before it (buf[0], or the file's first
 * byte when `whole`), every game's, as vjo_history_scan finds records and reads their lines (unescaped in
 * place: buf is changed). With `cut` the newest record is left out: the window's end may cut it. *first
 * and *last: where the oldest and the newest record start (len: none). */
uint32_t vjo_history_chars(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last);

/* n in decimal with a comma between every three digits (1,234,567) into buf, which has room for them:
 * 27 bytes for any n (20 digits and 6 commas). Returns buf. The Triangle view's total. */
char *vjo_history_number(char *buf, size_t cap, uint64_t n);

#endif
