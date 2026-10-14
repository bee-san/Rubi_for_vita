/* The Overlay app's Anki page: card mining on/off, AnkiConnect's address
 * (typed with the system keyboard: IP[:port], or auto to search the
 * network), a connection test and "send the queued cards now". The cards
 * are Kiku notes in the Kiku deck (core/anki.h); nothing else is set up. */
#ifndef VJO_SETTINGS_ANKI_PAGE_H
#define VJO_SETTINGS_ANKI_PAGE_H
#include <stddef.h>
#include <stdint.h>
#include "../core/arena.h"
#include "../core/config.h"

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
} VjoAnkiUi;

/* The page is shown (Select or its tab): reloads the settings and counts the queue. */
void vjo_anki_page_show(VjoArena *scratch);
/* One frame of the page: the buttons pressed and a new touch (x, y; -1 when
 * none), then drawing. */
void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch);
/* The keyboard is open: main.c leaves every button to it. */
int vjo_anki_page_modal(void);
/* A test or a send is running: Circle cancels it instead of leaving the app. */
int vjo_anki_page_busy(void);
/* Leaving the app: cancels and joins the running action, ends the network. */
void vjo_anki_page_quit(void);

#endif
