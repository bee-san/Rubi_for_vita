/* The host tests' word navigation: core/text_rows.h's word step with C++ defaults, and the L/R tap rule
 * the overlay's dictionary pages had until #289 (the overlay itself calls vjo_text_row_key). Moved here
 * from shell/, which included it for nothing (cleanup P1, #418). */
#ifndef VJO_WORD_NAVIGATION_H
#define VJO_WORD_NAVIGATION_H

extern "C" {
#include "../../core/text_rows.h"
#include "../../core/utf.h"
}

/* Store positions, not definitions or dictionary pointers. Right skips the
 * matched source span; Left retraces up to 16 recent forward selections.
 * Without a match/history, keep the existing character navigation. Both go
 * in reading order across the header's rows and stop at the text's ends; with
 * `wrap` (the d-pad's, the default) a text on one displayed row goes round
 * its ends (vjo_word_step, core/text_rows.h). */
inline size_t vjo_word_cursor_step(VjoWordNavigation &history, const char *text,
                                  size_t len, size_t cursor, size_t matched_end,
                                  int direction, int japanese_only,
                                  const VjoTextRows *rows = NULL, int *row_result = NULL, int wrap = 1)
{
    return vjo_word_step(&history, text, len, cursor, matched_end, direction, japanese_only, rows, wrap, row_result);
}

/* L or R alone: pressed with no other player button held, released before
 * any other is pressed. Returns -1 (l) or +1 (r) on that release, else 0;
 * armed holds the shoulder being tapped. The L + R chord in either order,
 * Select + L/R and a shoulder already held when the overlay opened (no press
 * is seen) never count. */
inline int vjo_shoulder_tap(uint32_t &armed, uint32_t held, uint32_t pressed, uint32_t l, uint32_t r)
{
    uint32_t shoulder = pressed & (l | r);
    int tap = 0;
    if (shoulder)
        armed = held == shoulder && shoulder != (l | r) ? shoulder : 0;
    else if (armed && held != armed) {
        if (!held)
            tap = armed == r ? 1 : -1;
        armed = 0;
    }
    return tap;
}

#endif
