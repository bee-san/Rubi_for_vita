/* The Rubi app's Anki page: card mining on/off, the cards' screenshots
 * on/off (anki_screenshot, API 23), with automatic black-bar cropping, what the export makes of each card
 * (#311: Card type, Game tag and Sentence readings, app.ini's anki_card_type,
 * anki_game_tag and anki_sentence_readings), what it does with a word mined
 * before (#314: Duplicates, app.ini's anki_duplicates: tag, skip or keep), the
 * retained, disabled known-word importer (#521), and the export of the queued cards
 * (or all of them; "N queued, M already mined") as Lapis notes in a new .apkg file under
 * ux0:data/VitaJPOverlay/anki/ (settings/anki_export.h), which Anki imports
 * on any computer or phone; a card's screenshot goes into its Picture field.
 * Nothing is sent anywhere: the app uses no network. */
#ifndef VJO_SETTINGS_ANKI_PAGE_H
#define VJO_SETTINGS_ANKI_PAGE_H
#include <stddef.h>
#include <stdint.h>
#include "../core/arena.h"
#include "../core/config.h"
#include "anki_export.h"
#include "page_ui.h"

/* The category is shown: reloads the settings and counts the cards. */
void vjo_anki_page_show(VjoArena *scratch);
/* One frame of the page: the buttons pressed and a new touch (x, y; -1 when
 * none), then drawing. */
void vjo_anki_page_frame(const VjoPageUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch);
/* The left stick's move for the next frame (rows: negative up, positive down,
 * 0 none; core/stick_scroll.h): it stops at the first and last row, where
 * Up/Down wrap round. */
void vjo_anki_page_stick(int move);
/* An export or the known words' import is running (the page's worker: 64 KiB stack, CPU3, the page's 2 MiB arena
 * VJO_EXPORT_MEM). */
int vjo_anki_page_busy(void);
/* Cancel belongs to the Actions page or a running job (cancel stops it). */
int vjo_anki_page_holds_cancel(void);
/* The actions subpage goes back to Anki, consuming this Back. A running job retains Cancel to stop it. */
int vjo_anki_page_back(void);
const char *vjo_anki_page_title(void);
/* Leaving the app: cancels and joins a running export or import (the queue is left as it was). */
void vjo_anki_page_quit(void);

/* The app's lines in log.txt (main.c and the pages include them from here). */
#include "applog.h"

#endif
