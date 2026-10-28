/* The Look up page's dictionary work (#313), without its drawing: a text (a word or sentence typed with the
 * keyboard, or an earlier line of history.csv), a cursor in it, the overlay's own lookup at the cursor, and a card
 * of the entry shown, saved to the Anki queue.
 *
 * The lookup is the overlay's, unchanged: core/client.c vjo_overlay_lookup_at, with the overlay's dictionaries in
 * their order (config.ini local_dictionaries, local_dictionary_results: the other dictionaries' entries for the
 * word stacked below it), its ranking and deinflection, over the same memory as the Shell's (shell/worker.c): one
 * 64 KiB block, the dictionary session (its open files and caches) at its start and the lookup's arena after it,
 * here a static block of the app's (vjo_lookup_block_bytes). The text is the overlay's sentence: a line as the
 * overlay shows it (non_japanese_filter, then trimmed: core/history_lines.c's line history keeps it so), a typed
 * text trimmed only. The cursor moves as the overlay's does (core/text_rows.h vjo_text_row_key): Left/Right by the
 * matched word in the Whole word highlight, a character in Character mode, the stick a character.
 *
 * A card is the overlay's record (core/anki_queue.h, version 4) without a picture (settings/app_queue.h): the shown
 * entry and the other dictionaries' entries below it, while the record stays within VJO_QUEUE_TEXT_MAX; its sentence
 * the line, or the typed text when it is longer than the word (a typed word alone has none); the game, title ID and
 * platform the line's (a typed search has none); MiscInfo the game and the line's local date (a typed search: the
 * date it was saved); the time, to the minute, when it was saved. Mining works with Card mining OFF: it is the
 * page's own explicit press.
 *
 * The main thread only; nothing here allocates: the static blocks are below (docs/vita-memory.md 5.4). */
#ifndef VJO_SETTINGS_LOOKUP_H
#define VJO_SETTINGS_LOOKUP_H
#include <stddef.h>
#include <stdint.h>
#include "../core/anki.h"
#include "../core/client.h"
#include "../core/config.h"
#include "../core/local_dict.h"

#define VJO_LOOKUP_TEXT_MAX VJO_LOCAL_MAX_TEXT /* bytes of text: the lookup's own limit */

/* What a card from the text records besides the entry. */
typedef struct {
    int typed;             /* a typed search: none of the rest */
    char game[128];        /* the line's game, its name (history.csv name) */
    char title_id[16];
    char platform[8];      /* "vita" | "psp" */
    char engine[8];        /* "vocr" | "hook": how the line was read */
    char model[48];        /* the OCR model that read it ("" for a hook's line) */
    uint64_t time_us;      /* the line's time, UTC microseconds; 0 unknown */
} VjoLookupSource;

/* The settings the lookups use (config.ini as the app loads it): the dictionaries, their order and count, the
 * filter, the cursor's rules. A change of dictionaries closes the session's files. */
void vjo_lookup_configure(const VjoConfig *cfg);
/* Closes the dictionary session's files (the page is left: an install may replace them). */
void vjo_lookup_close(void);

/* The text to look words up in: typed (trimmed) or a line (filtered as the overlay shows it, then trimmed), cut at
 * VJO_LOOKUP_TEXT_MAX bytes on a character. The cursor goes to its first selectable character, and is looked up
 * (vjo_lookup_result). Returns 0, or -1 when nothing is left of it (empty: no lookup). */
int vjo_lookup_set_text(const char *text, int typed);
const char *vjo_lookup_text(void);
size_t vjo_lookup_text_len(void);
/* The cursor and its lookup. */
size_t vjo_lookup_cursor(void);
/* Looks the word at byte `cursor` up (a character's start; else the start of the character it is in), the shown
 * entry's other dictionaries' entries stacked below it. Returns VJO_OK or the lookup's error (vjo_lookup_error).
 * Each lookup reuses the arena: the previous result is gone. */
int vjo_lookup_at(size_t cursor);
/* The cursor's step (as the overlay's arrows): dx -1/1, stick (the left stick: a character, never round the text's
 * ends). Moves and looks up; returns 1 when the cursor moved. */
int vjo_lookup_step(int dx, int stick);
/* The cursor's word has been looked up (the stick moves the cursor first, its lookup comes once it rests). */
int vjo_lookup_fresh(void);
/* The last lookup: its entries (n_entries 0: no word at the cursor), its error's text ("" for none); while the
 * cursor's word is not looked up yet, none. */
const VjoOverlayData *vjo_lookup_result(void);
const char *vjo_lookup_error(void);
/* The span the page highlights: the matched word in the Whole word highlight with an entry, else the cursor's
 * character. [*start, *end) in the text's bytes. */
void vjo_lookup_highlight(size_t *start, size_t *end);

/* Saves the shown entry as a card (settings/app_queue.h), what it says in msg. Returns VJO_APP_QUEUE_*, or
 * VJO_APP_QUEUE_FAILED - 10 when there is no entry to save. */
int vjo_lookup_mine(const VjoLookupSource *source, char *msg, size_t cap);
/* The card's note without saving it, in the card arena (for the tests): 0, or -1 (no entry, no memory). */
int vjo_lookup_note(const VjoLookupSource *source, VjoAnkiNote *note, char *misc, size_t misc_cap);

#endif
