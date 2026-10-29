/* main.c's drawing helpers for the pages it does not draw itself (settings/anki_page.c, lookup_page.c,
 * install_page.c), so that they look like its own: its text, cards, badges, status box and header, its colours and the
 * console's buttons. One table for every page (main.c page_ui). */
#ifndef VJO_SETTINGS_PAGE_UI_H
#define VJO_SETTINGS_PAGE_UI_H
#include "../core/buttons.h"

typedef struct {
    void (*text)(int x, int y, unsigned color, float pixels, const char *s);
    int (*text_width)(float pixels, const char *s); /* (a VjoTextMeasure, settings/text_layout.h) */
    /* s in at most `lines` rows of `width` pixels, the last cut with "..." */
    void (*wrapped)(int x, int y, int width, unsigned color, float pixels, int line_height, int lines,
                    const char *s);
    void (*rect)(int x, int y, int width, int height, unsigned color);
    void (*card)(int y, int height, int focus);
    void (*badge)(int x, int y, int width, int height, const char *label, int active);
    /* the status box (its first baseline text_y, `lines` lines) and the control hints below it */
    void (*footer)(int y, int height, int text_y, int lines, const char *message, int error, const char *controls);
    void (*hints)(const char *controls); /* the control hints' row alone, at the bottom */
    void (*header)(void);
    void (*clip)(int x0, int y0, int x1, int y1); /* clipping to that rectangle; x1 <= x0: none */
    unsigned white, dim, accent, off;
    unsigned track;            /* an unfocused card's colour (a scroll bar's track, an empty bar) */
    unsigned selected;         /* a focused card's colour */
    const VjoButtons *buttons; /* the console's enter and cancel buttons */
    unsigned highlight, reading, rank, definition; /* Look up: theme-specific dictionary text */
} VjoPageUi;

/* A scroll bar: its track at x, y (w wide, room tall) and its thumb, where the part from..from + shown of total is
 * (rows or pixels; the caller draws it only when total is more than the room shows). */
static inline void vjo_page_scroll_bar(const VjoPageUi *ui, int x, int y, int w, int room, int from, int shown,
                                       int total)
{
    ui->rect(x, y, w, room, ui->track);
    ui->rect(x, y + room * from / total, w, room * shown / total, ui->dim);
}

#endif
