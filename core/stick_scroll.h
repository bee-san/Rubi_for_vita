/* Scrolling a list with the left stick, as VitaShell does (TheOfficialFloW/
 * VitaShell utils.c readPad, browser.c): the stick pushed more than
 * VJO_STICK_THRESHOLD from its centre counts as up or down; it moves one row
 * as it crosses, then nothing for nine frames, then one row every frame while
 * it stays out ("hold2": a count that reaches VJO_STICK_HOLD_FRAMES and stays
 * there). Up wins over down, as there; the stick's other axis is unused.
 *
 * One call per frame (the Overlay app waits for vblank: 60 a second), no SDK
 * calls: host/tests/test_stick_scroll.c drives it frame by frame. */
#ifndef VJO_STICK_SCROLL_H
#define VJO_STICK_SCROLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_STICK_CENTER 128       /* VitaShell ANALOG_CENTER */
#define VJO_STICK_THRESHOLD 64     /* VitaShell ANALOG_THRESHOLD */
#define VJO_STICK_HOLD_FRAMES 10   /* VitaShell readPad's hold2_count limit */

typedef struct {
    uint8_t old_up, old_down;     /* the previous frame's state, as VitaShell's old_pad */
    uint8_t count_up, count_down; /* frames held, as hold2_count */
} VjoStickScroll;

/* One axis of VitaShell's hold2 for one direction: fires on the press, then
 * every frame once held for VJO_STICK_HOLD_FRAMES. */
static inline int vjo_stick_hold2(int current, uint8_t *old, uint8_t *count)
{
    int fire = current && !*old;
    if (current) {
        if (*count >= VJO_STICK_HOLD_FRAMES) {
            fire = 1;
            *count = VJO_STICK_HOLD_FRAMES;
        }
        (*count)++;
    } else {
        *count = 0;
    }
    *old = (uint8_t)current;
    return fire;
}

/* The frame's left-stick y (0 top, 255 bottom): -1 to move up a row, 1 down,
 * 0 not this frame. */
static inline int vjo_stick_scroll(VjoStickScroll *s, uint8_t ly)
{
    int up = ly < VJO_STICK_CENTER - VJO_STICK_THRESHOLD;
    int down = !up && ly > VJO_STICK_CENTER + VJO_STICK_THRESHOLD;
    int fire_up = vjo_stick_hold2(up, &s->old_up, &s->count_up);
    int fire_down = vjo_stick_hold2(down, &s->old_down, &s->count_down);
    return fire_up ? -1 : fire_down ? 1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif
