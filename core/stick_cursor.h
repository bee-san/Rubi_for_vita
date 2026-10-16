/* The left stick over the open overlay's recognized text (#138): it moves the
 * cursor as the d-pad does, faster the longer it is held, and the word at the
 * cursor is looked up only once the cursor rests there.
 *
 * Pushed more than VJO_STICK_THRESHOLD from its centre (core/stick_scroll.h:
 * the Overlay app's lists, #107/#113/#129), the stick moves the cursor one
 * step at once; held, the next step comes after VJO_STICK_HOLD_US (the lists'
 * 166.7 ms), then after VJO_STICK_CURSOR_FIRST_US, each later one
 * VJO_STICK_CURSOR_ACCEL_US sooner than the one before (the acceleration),
 * never sooner than VJO_STICK_CURSOR_FAST_US after the last: at most 20 steps
 * a second (the cap; held, 13 steps in the first second, then 20 a second).
 * How far it is pushed beyond the threshold does not change the speed, as in
 * the lists. Left/right step along the text, up/down a displayed row
 * (shell/overlay.cpp: vjo_text_read_step, which never goes round the text's
 * ends, and vjo_text_row_key; a held stick stops at the first and last
 * character, as the lists stop at their ends). The axis pushed farther counts,
 * and the one held keeps counting until it is let back inside the threshold,
 * so a stick rolled round from one axis to the other changes once.
 *
 * By the clock (sceKernelGetProcessTimeWide's microseconds), as the lists
 * since #129: the same steps by the same time at any frame rate. A frame's
 * time counts at most one fastest step's, so a frame moves the cursor at most
 * once and a stalled frame never makes it jump.
 *
 * The lookup: a stick move only makes the word at the cursor due. It is looked
 * up (vjo_stick_cursor_lookup returns 1, once) when the cursor has not moved
 * for rest_us (config.ini stick_lookup_ms, 200 ms by default) and the stick
 * no longer drives it: let go, or held against the text's end. The d-pad and
 * taps still look up at once, and drop a due lookup (due = 0); an arrow pressed
 * takes its frame, the stick's step due in it dropped.
 *
 * One call of each per frame, no allocation and no SDK calls:
 * host/tests/test_stick_cursor.c drives it at 20 to 60 Hz. */
#ifndef VJO_STICK_CURSOR_H
#define VJO_STICK_CURSOR_H

#include <stdint.h>

#include "stick_scroll.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_STICK_CURSOR_FIRST_US 125000 /* the first repeat after the pause: 8 a second */
#define VJO_STICK_CURSOR_ACCEL_US 12500  /* each repeat this much sooner than the one before */
#define VJO_STICK_CURSOR_FAST_US 50000   /* the cap: never sooner than this after the last, 20 a second */
#define VJO_STICK_CURSOR_REST_MS 200     /* config.ini stick_lookup_ms's default */

typedef struct {
    int8_t dir;          /* the direction held: -1/1 left/right, -2/2 up/down; 0 at rest */
    uint8_t latched;     /* at rest until let back inside the threshold (vjo_stick_cursor_latch) */
    uint8_t due;         /* the stick moved the cursor: its word awaits its lookup */
    uint8_t blocked;     /* the held stick's last step left the cursor where it was (an end) */
    int32_t credit_us;   /* time held since the push or the last step */
    int32_t wait_us;     /* from the push or the last step to the next */
    uint32_t last_us;    /* the previous frame's clock, its low 32 bits (differences wrap safely) */
    uint32_t moved_us;   /* when the stick last moved the cursor, the same way */
    uint32_t rest_us;    /* stick_lookup_ms: how long the cursor rests before its word is looked up */
    int moves, lookups;  /* the stick's cursor moves and their lookups (the overlay's close line) */
} VjoStickCursor;

static inline int vjo_stick_away(int v)
{
    v -= VJO_STICK_CENTER;
    return v < 0 ? -v : v;
}

/* The direction an axis is pushed: 0 inside the threshold, else -unit or unit. */
static inline int vjo_stick_axis(int v, int unit)
{
    return v > VJO_STICK_CENTER + VJO_STICK_THRESHOLD ? unit : v < VJO_STICK_CENTER - VJO_STICK_THRESHOLD ? -unit : 0;
}

/* The frame's left stick (lx 0 left .. 255 right, ly 0 top .. 255 bottom) at
 * now_us: the step to take now, as the direction it is held (-1/1 left/right,
 * -2/2 up/down), or 0 for none. Only the clock's low 32 bits are used: the
 * differences are right across its wrap (every 71.6 minutes). */
static inline int vjo_stick_cursor(VjoStickCursor *s, uint8_t lx, uint8_t ly, uint64_t now_us)
{
    int hx = vjo_stick_axis(lx, 1), hy = vjo_stick_axis(ly, 2), dir;
    int32_t step = (int32_t)((uint32_t)now_us - s->last_us);
    s->last_us = (uint32_t)now_us;
    if (step < 0)
        step = 0; /* a clock going back counts nothing */
    if (step > VJO_STICK_CURSOR_FAST_US)
        step = VJO_STICK_CURSOR_FAST_US;
    /* still pushed the way it is held; else the axis pushed farther (along the text on a tie) */
    dir = s->dir && (s->dir == hx || s->dir == hy) ? s->dir
        : !hy || (hx && vjo_stick_away(lx) >= vjo_stick_away(ly)) ? hx : hy;
    if (s->latched) {
        if (!dir)
            s->latched = 0;
        dir = 0;
    }
    if (dir != s->dir) {
        /* pushed from rest, let go, or to another direction: afresh */
        s->dir = (int8_t)dir;
        s->credit_us = 0;
        s->wait_us = VJO_STICK_HOLD_US;
        return dir; /* the push's step */
    }
    if (!dir)
        return 0;
    s->credit_us += step;
    if (s->credit_us + VJO_STICK_SLACK_US < s->wait_us)
        return 0;
    s->credit_us -= s->wait_us;
    /* the next wait: after the pause the first repeat's, then each 12.5 ms less, down to the cap */
    s->wait_us = s->wait_us > VJO_STICK_CURSOR_FIRST_US ? VJO_STICK_CURSOR_FIRST_US
               : s->wait_us > VJO_STICK_CURSOR_FAST_US + VJO_STICK_CURSOR_ACCEL_US
               ? s->wait_us - VJO_STICK_CURSOR_ACCEL_US : VJO_STICK_CURSOR_FAST_US;
    return dir;
}

/* After a step of the stick's was tried (the overlay tries each one, but for
 * one in a frame an arrow is pressed in: the arrow takes the frame and drops
 * the due lookup): moved, the cursor changed (else the stick holds it against
 * an end, or a row it cannot leave). */
static inline void vjo_stick_cursor_moved(VjoStickCursor *s, int moved, uint64_t now_us)
{
    s->blocked = !moved;
    if (moved) {
        s->due = 1;
        s->moved_us = (uint32_t)now_us;
        s->moves++;
    }
}

/* Each frame, after the stick's step: 1, once, when the word the stick left the
 * cursor on is to be looked up now: the cursor has not moved for rest_us (at
 * most 1 s, less the steps' 1 ms slack for the microsecond clock: six frames at
 * exactly 30 Hz read 199,998 us) and the stick no longer drives it. */
static inline int vjo_stick_cursor_lookup(VjoStickCursor *s, uint64_t now_us)
{
    if (!s->due || (s->dir && !s->blocked) ||
        (int32_t)((uint32_t)now_us - s->moved_us) + VJO_STICK_SLACK_US < (int32_t)s->rest_us)
        return 0;
    s->due = 0;
    s->lookups++;
    return 1;
}

/* The overlay's frame over the text (shell/overlay.cpp input_overlay): stick is
 * vjo_stick_cursor's step, direction and *vertical the arrow pressed (-1/1, 0
 * none). An arrow takes the frame: the stick's step due in it is dropped (0).
 * Else the stick's up/down are a row step, as Up/Down go (*vertical), and its
 * left/right a step along the text. Returns the stick's step that is taken
 * (nonzero: no lookup now). */
static inline int vjo_stick_cursor_step(int stick, int direction, int *vertical)
{
    if (direction || *vertical)
        return 0;
    if (stick == 2 || stick == -2)
        *vertical = stick / 2;
    return stick;
}

/* The overlay's cursor is the worker's (matches: VjoView.cursor_byte, under the
 * view lock): its word is shown, or its lookup is on its way. A word of the
 * stick's due there is dropped: the stick came back to it before the rest. */
static inline void vjo_stick_cursor_shown(VjoStickCursor *s, int matches)
{
    if (matches)
        s->due = 0;
}

/* New text in the overlay (a refresh replaced it, or an open takes the worker's
 * cursor): a due word was in the text it replaces, and a stick still pushed
 * moves nothing until it is let go, as a page change latches the app's lists. */
static inline void vjo_stick_cursor_new_text(VjoStickCursor *s)
{
    s->due = 0;
    s->latched = 1;
}

/* The overlay's close (close_page), under the view lock: 1 when the next open
 * may keep the overlay's cursor (with its text): it is the worker's
 * (worker_cursor: VjoView.cursor_byte, the last one the worker accepted) and no
 * word of the stick's is due. Else 0: the next open takes the worker's cursor,
 * as for new text. A post the worker refused leaves them apart: while Cancel's
 * VJO_CMD_CLOSED waits for the control thread (it wakes every 50 ms) or the
 * overlay is closing, vjo_post_cursor posts nothing, and a deferred lookup has
 * cleared its due word by then. */
static inline int vjo_stick_cursor_kept(const VjoStickCursor *s, uint32_t cursor, uint32_t worker_cursor)
{
    return !s->due && cursor == worker_cursor;
}

/* The overlay opened, or the region selector gave the stick back: a stick
 * still pushed moves nothing until it is let back inside the threshold. */
static inline void vjo_stick_cursor_latch(VjoStickCursor *s)
{
    s->latched = 1;
}

#ifdef __cplusplus
}
#endif

#endif
