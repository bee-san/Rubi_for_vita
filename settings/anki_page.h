/* The Rubi app's Anki page: card mining on/off, the cards' screenshots
 * on/off (anki_screenshot, API 23) and their black bars trimmed or kept
 * (anki_screenshot_trim_bars, API 32), what the export makes of each card
 * (#311: Card type, Game tag and Sentence readings, app.ini's anki_card_type,
 * anki_game_tag and anki_sentence_readings), what it does with a word mined
 * before (#314: Duplicates, app.ini's anki_duplicates: tag, skip or keep), the
 * known words' import (#314 stage C), and the export of the queued cards
 * (or all of them; "N queued, M already mined") as Lapis notes in a new .apkg file under
 * ux0:data/VitaJPOverlay/anki/ (settings/anki_export.h), which Anki imports
 * on any computer or phone; a card's screenshot goes into its Picture field.
 * Nothing is sent anywhere: the app uses no network. */
#ifndef VJO_SETTINGS_ANKI_PAGE_H
#define VJO_SETTINGS_ANKI_PAGE_H
#include <stddef.h>
#include <stdint.h>
#include "../core/arena.h"
#include "../core/buttons.h"
#include "../core/config.h"
#include "anki_export.h"

/* main.c's drawing helpers, so the page looks like the others. */
typedef struct {
    void (*text)(int x, int y, unsigned color, float pixels, const char *s);
    void (*wrapped)(int x, int y, int width, unsigned color, float pixels, int line_height, int lines,
                    const char *s);
    void (*card)(int y, int height, int focus);
    void (*badge)(int x, int y, int width, int height, const char *label, int active);
    void (*footer)(int y, int height, int text_y, int lines, const char *message, int error, const char *controls);
    void (*header)(void);
    unsigned white, dim, accent, off;
    unsigned track; /* the scroll bar's track, a card's colour (API 32: the five rows scroll) */
    const VjoButtons *buttons; /* the console's enter and cancel buttons */
} VjoAnkiUi;

/* The page is shown (Select or its tab): reloads the settings and counts the cards. */
void vjo_anki_page_show(VjoArena *scratch);
/* One frame of the page: the buttons pressed and a new touch (x, y; -1 when
 * none), then drawing. */
void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch);
/* The left stick's move for the next frame (rows: negative up, positive down,
 * 0 none; core/stick_scroll.h): it stops at the first and last row, where
 * Up/Down wrap round. */
void vjo_anki_page_stick(int move);
/* An export or the known words' import is running (the page's worker: 64 KiB stack, CPU3, the page's 2 MiB arena
 * VJO_EXPORT_MEM). */
int vjo_anki_page_busy(void);
/* Cancel is the page's, not the app's exit: a job runs (cancel stops it). */
int vjo_anki_page_holds_cancel(void);
/* Leaving the app: cancels and joins a running export or import (the queue is left as it was). */
void vjo_anki_page_quit(void);

/* The app's lines in log.txt, "Rubi app: " and the text (settings/applog.c,
 * below the Shell's 1 MiB rotation): written while config.ini's log_file is
 * on, which main.c sets at start and the page again when it is shown. Between
 * vjo_app_log_batch_begin and _end, in one function of one thread, that
 * thread's lines go out in one append, at the end. */
void vjo_log_enable(int on);
void vjo_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vjo_app_log_batch_begin(void);
void vjo_app_log_batch_end(void);

#endif
