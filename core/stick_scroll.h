/* Scrolling a list with the left stick, as VitaShell does (TheOfficialFloW/
 * VitaShell utils.c readPad, browser.c): the stick pushed more than
 * VJO_STICK_THRESHOLD from its centre counts as up or down; it moves one row
 * as it crosses, then, held, nothing for VJO_STICK_HOLD_US and then a row
 * every VJO_STICK_ROW_US while it stays out. VitaShell counts frames ("hold2":
 * nine quiet frames, then a row every frame); at its 60 Hz that is 166.7 ms,
 * then 60 rows a second. The Rubi app likely draws at 30 Hz as a system
 * application (libvita2d's vita2d_end_drawing waits for a vblank there and the
 * loop for another: an inference, not measured; the app logs its frame
 * interval once at start), so the stick goes by the clock instead of the
 * frame: held, it moves two rows a frame at 30 Hz, one at 60 Hz, at the same
 * speed. Up wins over down, as there; the stick's other axis is unused.
 *
 * One call per frame, with the frame's clock (sceKernelGetProcessTimeWide's
 * microseconds), and no SDK calls: host/tests/test_stick_scroll.c drives it
 * at 20 to 60 Hz. */
#ifndef VJO_STICK_SCROLL_H
#define VJO_STICK_SCROLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_STICK_CENTER 128       /* VitaShell ANALOG_CENTER */
#define VJO_STICK_THRESHOLD 64     /* VitaShell ANALOG_THRESHOLD */
#define VJO_STICK_HOLD_US 166667   /* VitaShell's ten frames at 60 Hz (hold2_count's limit) before it repeats */
#define VJO_STICK_ROW_US 16667     /* then a row a frame at 60 Hz: 60 rows a second */
/* A row is due within this much of its time. The clock counts whole
 * microseconds, so ten frames at exactly 60 Hz read 166,666 or 166,667 us,
 * and five at 30 Hz 166,665: none of them is a reason to wait a frame more. */
#define VJO_STICK_SLACK_US 1000
/* A frame's time counts at most this much (as core/region_select.h's step): a
 * stalled frame moves the list at most three rows. */
#define VJO_STICK_MAX_STEP_US 50000

typedef struct {
    int8_t dir;         /* -1 up, 1 down: pushed at the previous call; 0 at rest */
    uint8_t latched;    /* vjo_stick_latch: at rest until let back inside the threshold */
    int32_t credit_us;  /* time held since the push or the last row, less the slack's early rows */
    int32_t wait_us;    /* the time from the push or the last row to the next row */
    uint64_t last_us;   /* the previous call's clock */
} VjoStickScroll;

/* The frame's left-stick y (0 top, 255 bottom) at now_us: the rows to move,
 * negative up, 0 none; at most three either way. */
static inline int vjo_stick_scroll(VjoStickScroll *s, uint8_t ly, uint64_t now_us)
{
    int dir = ly < VJO_STICK_CENTER - VJO_STICK_THRESHOLD ? -1 : ly > VJO_STICK_CENTER + VJO_STICK_THRESHOLD ? 1 : 0;
    uint64_t step = now_us > s->last_us ? now_us - s->last_us : 0; /* a clock going back counts nothing */
    s->last_us = now_us;
    if (step > VJO_STICK_MAX_STEP_US)
        step = VJO_STICK_MAX_STEP_US;
    if (s->latched) {
        if (!dir) s->latched = 0;
        dir = 0; /* at rest until then: the next push is a push */
    }
    if (dir != s->dir) {
        /* pushed from rest, let go, or straight to the other way: afresh */
        s->dir = (int8_t)dir;
        s->credit_us = 0;
        s->wait_us = VJO_STICK_HOLD_US;
        return dir; /* the push's row */
    }
    if (!dir)
        return 0;
    int rows = 0;
    s->credit_us += (int32_t)step;
    while (s->credit_us + VJO_STICK_SLACK_US >= s->wait_us) {
        s->credit_us -= s->wait_us;
        s->wait_us = VJO_STICK_ROW_US;
        rows++;
    }
    return dir * rows;
}

/* A page change or the system keyboard, after this frame's vjo_stick_scroll:
 * a stick still pushed moves nothing until it is let back inside the
 * threshold, as a held button is no new press. A stick at rest is not
 * affected: its next push is a push. */
static inline void vjo_stick_latch(VjoStickScroll *s)
{
    if (s->dir) s->latched = 1;
}

/* The selection of a list of n rows after the stick's move (vjo_stick_scroll's
 * rows): never past the first or the last, as VitaShell's lists stop there
 * (browser.c, settings.c), also where the d-pad wraps round. A held stick runs
 * to the end and stays there. */
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
