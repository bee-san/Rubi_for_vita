/* The Rubi app's Cards page (#315), opened from the Anki page's Cards row: the queue's cards newest
 * first (the word, its reading, the game and the date, a camera mark for a card with a picture, a
 * mark for a word mined before: the export's duplicate), ▲▼ and the stick to move, L/R a page;
 * Left/Right show the exported cards instead (read only: their notes may be in Anki already, and an
 * edit would be a new note).
 * Enter opens a card: its sentence with the word marked, the first lines of its definition and its
 * picture, decoded when the card opens and scaled to fit (settings/card_picture.c); L/R the card
 * before or after it. A queued card can be deleted, its sentence fixed with the keyboard, or its
 * picture dropped (settings/cards.h has the order of every step); Triangle on the list empties
 * anki_queue/deleted/. Delete, Drop picture and Empty deleted ask for a second press.
 *
 * Every read and change runs on the Anki page's worker (vjo_anki_job_start), never on the main
 * thread, which keeps drawing; the list's table (VJO_CARDS_MAX rows of 128 B) and the two card views
 * are static, the rest of a job's memory the worker's arena. */
#ifndef VJO_SETTINGS_CARDS_PAGE_H
#define VJO_SETTINGS_CARDS_PAGE_H
#include <stdint.h>
#include "anki_page.h"

/* Opens the page on the queued cards and reads them. */
void vjo_cards_page_open(void);
int vjo_cards_page_is_open(void);
/* The Anki page shown again with the Cards page open: its list read again (the card selected kept). */
void vjo_cards_page_reread(void);
/* One frame of the open page: input, then drawing. 1 when the keyboard ran (main.c then takes no
 * button or touch until it is let go). */
int vjo_cards_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y);
/* The left stick's move for the next frame (core/stick_scroll.h): it stops at the first and last card. */
void vjo_cards_page_stick(int move);
/* The page's job has ended and its worker is joined (settings/anki_page.c): its results are taken. */
void vjo_cards_page_job_done(void);
/* A job of the page is the worker's (the Anki page then lets cancel stop it only while it reads). */
int vjo_cards_page_job_running(void);
/* Leaving the app: the picture's texture goes. */
void vjo_cards_page_quit(void);
#endif
