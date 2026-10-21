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

#endif
