/* The Rubi app's Stats tab (#312): reading statistics from ux0:data/VitaJPOverlay/history.csv
 * (docs/ocr-history.md), counted as the overlay's Triangle view counts them (core/stats.h):
 * - Today, all games: characters, reading time and characters/hour, the daily goal's bar and the streak;
 * - Daily goal: off, characters (2,000 to 50,000) or minutes (15 to 120), Left/Right on its row, saved in the
 *   app's own settings file (app.ini, never config.ini: the overlay does not read it);
 * - Last 14 days: a bar of characters a day, the chosen day's time below it;
 * - This year: a heatmap of characters a day (weeks from Sunday, as GameSentenceMiner's);
 * - All games, then each game: characters today and in all, reading time, characters/hour, cards mined (the Anki
 *   queue's records, by their title ID), cards per 10,000 characters and the last day read; newest first.
 * The count runs on its own thread (CPU3, as every thread of the app) and is kept in
 * ux0:data/VitaJPOverlay/stats_cache.bin, so an append to history.csv is read from where the last count stopped.
 * history.csv and the cards are only read, never written. */
#ifndef VJO_SETTINGS_STATS_PAGE_H
#define VJO_SETTINGS_STATS_PAGE_H
#include <stdint.h>
#include "../core/arena.h"
#include "../core/buttons.h"

#define VJO_STATS_HISTORY_PATH "ux0:data/VitaJPOverlay/history.csv"
#define VJO_STATS_CACHE_PATH "ux0:data/VitaJPOverlay/stats_cache.bin"

/* main.c's drawing helpers, so the page looks like the others. */
typedef struct {
    void (*text)(int x, int y, unsigned color, float pixels, const char *s);
    int (*text_width)(float pixels, const char *s);
    void (*wrapped)(int x, int y, int width, unsigned color, float pixels, int line_height, int lines,
                    const char *s);
    void (*card)(int y, int height, int focus);
    void (*badge)(int x, int y, int width, int height, const char *label, int active);
    void (*footer)(int y, int height, int text_y, int lines, const char *message, int error, const char *controls);
    void (*header)(void);
    void (*rect)(int x, int y, int width, int height, unsigned color);
    unsigned white, dim, accent, off, track, selected;
    const VjoButtons *buttons;
} VjoStatsUi;

/* The page is shown (Select or its tab): reads the goal and the reading-time rule, and counts what is new. */
void vjo_stats_page_show(VjoArena *scratch);
/* One frame: the buttons pressed and a new touch (x, y; -1 when none), then drawing. */
void vjo_stats_page_frame(const VjoStatsUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch);
/* The left stick's move for the next frame (rows; core/stick_scroll.h). */
void vjo_stats_page_stick(int move);
/* Leaving the app: a count in progress stops, keeping what it counted, and its thread is joined. */
void vjo_stats_page_quit(void);

#endif
