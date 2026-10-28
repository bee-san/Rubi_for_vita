/* The Rubi app's Look up page (#313): look a word up and mine it later, beside the suspended game.
 *
 * - Search: the keyboard (Triangle, or Enter or a tap on the text box while it is empty) takes a word or a sentence
 *   of up to 128 characters, which the overlay's own lookup reads (settings/lookup.h): the entry of the word at the
 *   cursor, the other dictionaries' entries for it below. Left/Right move the cursor by the matched word (a
 *   character in the Character highlight), the left stick a character, a tap to the character; Up/Down and the right
 *   stick scroll the entry. Enter saves the entry as an Anki card, without a picture.
 * - Earlier lines: a game's last lines in history.csv, newest first (settings/lookup_history.h), the most recent
 *   game's first; Square goes to the next game. Enter (or a tap) opens a line with the word cursor of Search; Enter
 *   then saves its word as a card, with the line as its sentence and the line's game; Cancel goes back to the list.
 * - L and R go from one to the other.
 *
 * It draws through main.c's helpers, so it looks like the other pages, and starts its own frames as the Anki page
 * does. The keyboard runs inside a frame: vjo_lookup_page_frame returns 1 then, and main.c treats the buttons and
 * touch still down as no new press. Nothing is allocated: the memory is static (settings/lookup.c,
 * settings/lookup_history.c, the page's rows here). The main thread only. */
#ifndef VJO_SETTINGS_LOOKUP_PAGE_H
#define VJO_SETTINGS_LOOKUP_PAGE_H
#include <stddef.h>
#include <stdint.h>
#include "../core/arena.h"
#include "page_ui.h"

/* One frame's input, from main.c's loop. */
typedef struct {
    uint32_t pressed, held;  /* buttons pressed this frame (not down the frame before); down now */
    uint8_t lx, ly, rx, ry;  /* the sticks */
    uint64_t now_us;         /* the process clock */
    int touch_x, touch_y;    /* a new touch, -1 none */
    int stick_rows;          /* main.c's left stick rows (core/stick_scroll.h): the list's */
} VjoLookupInput;

/* The page is shown (Select or its tab): config.ini read again (the dictionaries, the filter, the cursor's rules),
 * history.csv's size checked (a scan again when it changed), a stick still pushed latched. */
void vjo_lookup_page_show(VjoArena *scratch);
/* One frame: input, a part of history.csv's scan, the drawing. 1 when the keyboard ran in it. */
int vjo_lookup_page_frame(const VjoPageUi *ui, const VjoLookupInput *in);
/* Cancel is the page's (an earlier line is open: back to the list), not the app's exit. */
int vjo_lookup_page_holds_cancel(void);
/* The page is left (another page): the dictionaries' files are closed, so an install can replace them. */
void vjo_lookup_page_leave(void);
/* The app ends. */
void vjo_lookup_page_quit(void);

#endif
