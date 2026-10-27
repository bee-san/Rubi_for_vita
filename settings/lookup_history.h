/* The Look up page's earlier lines (#313): the games of history.csv and one game's last VJO_LOOKUP_LINES lines,
 * newest first, read from the file's end back (core/history_read.h: the Shell's own records and rules), a window
 * at a time, as many windows a frame as fit the time it is given (vjo_lookup_history_step): the page draws on, and
 * shows the lines found so far, while a big file is read. history.csv has the lines of Read text: Automatically
 * (docs/ocr-history.md); the app only reads it.
 *
 * A scan reads back at most VJO_LOOKUP_SCAN_MAX bytes from the end: the games listed are those of that part (the
 * most recent first), and a game's lines are its newest ones there. The selected game is the newest record's
 * unless one is chosen (vjo_lookup_history_select, which reads that game's lines again from the end). A line is
 * kept as the overlay's line history keeps one (core/history_lines.c vjo_history_scan): filtered as the overlay
 * shows a text (config.ini non_japanese_filter) and trimmed, none empty, none the same as the one kept after it.
 * Each line keeps its place in the file (a few bytes) and its start for the list; its whole text is read again when
 * it is opened (vjo_lookup_history_line).
 *
 * The memory is static: one window of VJO_LOOKUP_WINDOW bytes (and the byte before it) and the tables below. The
 * main thread only. */
#ifndef VJO_SETTINGS_LOOKUP_HISTORY_H
#define VJO_SETTINGS_LOOKUP_HISTORY_H
#include <stddef.h>
#include <stdint.h>
#include "lookup.h"

#define VJO_LOOKUP_HISTORY_PATH "ux0:data/VitaJPOverlay/history.csv"
#define VJO_LOOKUP_LINES 200
#define VJO_LOOKUP_GAMES 32
#define VJO_LOOKUP_PREVIEW 120               /* bytes of a line's start that the list keeps */
#define VJO_LOOKUP_WINDOW (64u * 1024u)
#define VJO_LOOKUP_SCAN_MAX (8u * 1024u * 1024u)

typedef struct {
    char uuid[24];  /* "vita-PCSG00415": history.csv's first column */
    char name[128]; /* its name column, the newest record's */
} VjoLookupGame;

typedef struct {
    uint64_t offset;  /* the record's start in history.csv */
    uint32_t bytes;   /* its bytes, to the next record's start */
    uint64_t time_us; /* its time, UTC microseconds; 0 when the Shell could not read one */
    char preview[VJO_LOOKUP_PREVIEW + 1]; /* the line's start: filtered and trimmed, its line breaks as spaces */
} VjoLookupLine;

enum {
    VJO_LOOKUP_HISTORY_NONE,    /* no history.csv (nothing recorded yet), or no record in it */
    VJO_LOOKUP_HISTORY_READING, /* a scan is reading it */
    VJO_LOOKUP_HISTORY_READ,    /* the scan is done */
    VJO_LOOKUP_HISTORY_FAILED   /* it cannot be read (vjo_lookup_history_error) */
};

/* Starts a scan of the file as it is now, unless one of the same file (its size) is done or under way. The game is
 * the one chosen (vjo_lookup_history_select) while the file still has it, else the newest record's. `filter`:
 * config.ini non_japanese_filter. */
void vjo_lookup_history_open(int filter);
/* The scan's next windows, for about budget_us microseconds (at least one window). 1 while it has more. */
int vjo_lookup_history_step(uint32_t budget_us);
int vjo_lookup_history_state(void);
int vjo_lookup_history_error(void); /* the SDK's error of a failed read (0x80010002: no file) */
uint64_t vjo_lookup_history_size(void);
uint64_t vjo_lookup_history_read(void); /* the bytes the scan has read back from the end */

int vjo_lookup_history_games(const VjoLookupGame **games);
int vjo_lookup_history_game(void); /* the selected game, -1 none */
void vjo_lookup_history_select(int game);
int vjo_lookup_history_lines(const VjoLookupLine **lines);

/* Line i's whole text (as history.csv has it: the caller filters it, vjo_lookup_set_text) into text (cap bytes)
 * and what a card from it records into *src. 0, or -1 when the file no longer has that record there (it changed:
 * open the history again). */
int vjo_lookup_history_line(int i, char *text, size_t cap, VjoLookupSource *src);

#endif
