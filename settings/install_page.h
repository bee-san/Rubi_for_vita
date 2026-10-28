/* The Rubi app's Install page (docs/one-vpk-install.md, "The app: an Install page"): the state of
 * the overlay on the card, the plan of the chosen action with its exact edits and the recovery text,
 * then, on Enter, the action on its own thread with a progress bar, and what it did. It never
 * restarts the Vita and never repeats anything by itself (settings/install.c). */
#ifndef VJO_SETTINGS_INSTALL_PAGE_H
#define VJO_SETTINGS_INSTALL_PAGE_H
#include <stdint.h>
#include "page_ui.h"
#include "text_layout.h"

/* At the app's start: scans the card. 1 when the app should open on this page (the overlay is not
 * installed, another build's, damaged, interrupted, or cannot be installed and says why). */
int vjo_install_page_start(void);
/* The page is shown (Select, its tab): scans again unless an action runs. */
void vjo_install_page_show(void);
/* One frame: the buttons pressed and a new touch (-1 when none), then drawing. */
void vjo_install_page_frame(const VjoPageUi *ui, uint32_t pressed, int touch_x, int touch_y);
/* The left stick's move for the next frame (scrolls the plan). */
void vjo_install_page_stick(int move);
/* An action runs: the app neither leaves nor changes page then. */
int vjo_install_page_busy(void);
/* Leaving the app: waits for a running action to end (it cannot be stopped part way). */
void vjo_install_page_quit(void);
/* The app carries an overlay (its payload): the banner names this page instead of install-vita.sh. */
int vjo_install_page_has_payload(void);
/* How many runs have finished (Install, Uninstall, Put it back, Keep): main.c reads its banner
 * (app_check.h) again when this changes, so that it says what is on the card now. */
int vjo_install_page_runs(void);
#endif
