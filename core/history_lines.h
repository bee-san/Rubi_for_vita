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

/* Reading time (docs/ocr-history.md#reading-time): a record's line is read from its own time to the next
 * record of the same game's, credited to it when the next one comes. config.ini's reading_time picks the
 * rule, once, as the Shell starts (vjo_reading_rule):
 *   adaptive (the default; GameSentenceMiner's adaptive cap, without its sessions): each line at most 2.5
 *     times what the reader's pace would take for it, 2 s to 5 minutes, whatever the gap: a pause longer
 *     than that (AFK, a suspend, standby, the next day) gives the line its cap and no more. The pace is
 *     the 75th percentile of the time a character took in this game's lines of 5 or more characters that
 *     took 1 s to 5 minutes, as a running estimate: 3/32 of itself up after a slower line, 1/32 down
 *     after a faster one; 1.2 s a character until it has learned (`pace` 0); kept between 2 ms and 4.19 s a
 *     character (VJO_READING_PACE_MIN to 16 bits: below 32 units 1/32 of it is 0, so it could never move again).
 *   15s (Rubi 0.9.7 and older): only an interval (0, 15 s] counts, whole; a longer one gets nothing.
 * Another game's record, an invalid time or a clock set back breaks the interval. timed_chars belongs to
 * the earlier line; untimed lines never inflate the speed. Stored with the total (and the pace with it)
 * so incremental scans and restarts retain the interval at their boundary.
 *
 * Skip mode (docs/reading-time.md#skip-mode, #602; GameSentenceMiner's burst detection, in records): a line of
 * VJO_READING_SKIM_CHARS or more whose next record came less than VJO_READING_SKIM_GAP_US later was skimmed, not
 * read. VJO_READING_SKIM_LINES such lines in a row (shorter skimmed lines between them neither count nor end the
 * run) are a skip: their characters, their time and their timed characters come out of the counters again, in
 * both rules, and so does every skimmed line after them, of any length, until a line is not skimmed (it is read:
 * the one the skip landed on) or the interval breaks. Fewer in a row stay counted. history.csv keeps every line. */
enum { VJO_READING_ADAPTIVE, VJO_READING_GAPS_15S };
#define VJO_READING_SKIM_GAP_US 1000000u /* 12 characters in under 1 s: over 43,200 an hour */
#define VJO_READING_SKIM_CHARS 12u       /* GameSentenceMiner's rapid replacement: lines of 12 or more */
#define VJO_READING_SKIM_LINES 3u
#define VJO_READING_SKIM_SHIFT 29        /* VjoReadingTime.skim: the run's lines from this bit */
#define VJO_READING_PACE_UNIT_SHIFT 6         /* the pace's unit: 64 us a character (16 bits: to 4.19 s) */
#define VJO_READING_PACE_START 18750u         /* 1.2 s a character, 3,000 an hour: GameSentenceMiner's 3 s cap */
#define VJO_READING_PACE_MIN 32u              /* 2 ms a character: the smallest pace whose 1/32 step is not 0 */
#define VJO_READING_CAP_FLOOR_US 2000000u     /* stats_util.ADAPTIVE_FLOOR_SECONDS */
#define VJO_READING_CAP_CEILING_US 300000000u /* stats_util.ABSOLUTE_CEILING: 5 minutes */
#define VJO_READING_GAP_15S_US 15000000u
extern uint32_t vjo_reading_rule; /* VJO_READING_*: config.ini's reading_time as the Shell started */
typedef struct {
    uint64_t active_us, timed_chars, last_us;
    uint32_t last_game;
    /* last_chars was 32 bits until 0.9.7 (a line has at most 4,095: VJO_GAME_OCR_TEXT_BYTES); the pace took
     * its high half, which was zero: a cache of 0.9.7 reads as the same counters with no pace learned */
    uint16_t last_chars;
    uint16_t pace; /* adaptive: the pace, in 64 us a character (0: not learned yet) */
    /* skip mode: the skimmed lines of the run so far (VJO_READING_SKIM_LINES: a skip) from bit
     * VJO_READING_SKIM_SHIFT, and below it the microseconds credited to them while the run is shorter than a skip
     * (under 2 s); skim_chars: their characters (at most 2 x 4,095) */
    uint32_t skim;
    uint16_t skim_chars;
    /* the characters the last record added to the total: its line's, less any of a skip it showed (negative
     * then); not kept between records (a cache's is the last record's) */
    int16_t counted;
} VjoReadingTime;
/* One selected game's daily counters. UTC bounds represent local midnight on the Vita.
 * Empty uuid retains the unfiltered total used by offline tools. */
typedef struct {
    char uuid[24];
    uint64_t start_us, end_us, chars, active_us, timed_chars;
} VjoReadingScope;
void vjo_reading_scope_add(VjoReadingTime *t, VjoReadingScope *scope,
                           uint64_t us, uint32_t game, uint32_t chars);
/* API 49: the newest line, added at `us`, grew to `chars` characters (its record replaced, at the same time): the
 * characters it gains, also in the scope's day, and 0 when that line was not counted (another game, or not the
 * newest). No time is credited: the gap before it is the same. */
uint32_t vjo_reading_scope_grow(VjoReadingTime *t, VjoReadingScope *scope, uint64_t us, uint32_t chars);
/* The characters of the lines of buf's records, less those of skips (VjoReadingTime.counted): negative when a window
 * holds less than the skip it ends took back of lines counted before it. */
int32_t vjo_history_count_scope(char *buf, size_t len, int whole, int cut,
    size_t *first, size_t *last, VjoReadingTime *time, VjoReadingScope *scope);
/* Adds the record at `us` (its game, its line's characters) and credits the record before it: returns the
 * microseconds credited to that line, 0 when none were (a break, another game, a time that is not later). */
uint32_t vjo_reading_time_add(VjoReadingTime *t, uint64_t us, uint32_t game, uint32_t chars);
/* Like vjo_history_chars, also updating reading time in file order, across window boundaries. */
int32_t vjo_history_count(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last,
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
