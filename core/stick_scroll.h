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
    uint8_t latched;              /* vjo_stick_latch: at rest until let back inside the threshold */
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
    if (s->latched) {
        if (!up && !down) s->latched = 0;
        up = down = 0; /* at rest until then: the next push is a push */
    }
    int fire_up = vjo_stick_hold2(up, &s->old_up, &s->count_up);
    int fire_down = vjo_stick_hold2(down, &s->old_down, &s->count_down);
    return fire_up ? -1 : fire_down ? 1 : 0;
}

/* A page change or the system keyboard, after this frame's vjo_stick_scroll:
 * a stick still pushed moves nothing until it is let back inside the
 * threshold, as a held button is no new press. A stick at rest is not
 * affected: its next push is a push. */
static inline void vjo_stick_latch(VjoStickScroll *s)
{
    if (s->old_up || s->old_down) s->latched = 1;
}

/* The selection of a list of n rows after the stick's move (vjo_stick_scroll's
 * -1, 0 or 1): one row, but never past the first or the last, as VitaShell's
 * lists stop there (browser.c, settings.c), also where the d-pad wraps round.
 * A held stick runs to the end and stays there. */
static inline int vjo_stick_step(int selected, int n, int move)
{
    int next = selected + move;
    if (next > n - 1) next = n - 1;
    if (next < 0) next = 0;
    return next;
}

#ifdef __cplusplus
}
#endif

#endif
