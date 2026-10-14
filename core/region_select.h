/* Drawing the OCR region with the left stick and the d-pad as well as by
 * touch (#81 request 3). A cursor moves over the screen: Cross sets the first
 * corner, the box then follows the cursor to the opposite corner, and Cross
 * saves it. Circle takes the first corner back, or, with none, leaves without
 * saving. A touch drag sets the first corner where it starts and moves the
 * cursor with the finger, so the stick and the d-pad can finish its opposite
 * corner after it lifts.
 *
 * Screen pixels, no allocation and no SDK calls: the overlay
 * (shell/overlay.cpp) feeds it each frame's input and draws what it reports;
 * the host tests drive it frame by frame (host/tests/test_region_select.c). */
#ifndef VJO_REGION_SELECT_H
#define VJO_REGION_SELECT_H

#include <stdint.h>

#include "../include/vjo_api.h"

#ifdef __cplusplus
extern "C" {
#endif
#define VJO_RSEL_W 960 /* the screen, pixels */
#define VJO_RSEL_H 544
/* The smallest box saved: the capture's own minimum (kernel/capture.c widens
 * a narrower or shorter region to 32 px), so the size shown is the size read. */
#define VJO_RSEL_MIN 32

/* Buttons: the SCE_CTRL_* bits of psp2common/ctrl.h (the overlay asserts it). */
#define VJO_RSEL_UP     0x0010u
#define VJO_RSEL_RIGHT  0x0020u
#define VJO_RSEL_DOWN   0x0040u
#define VJO_RSEL_LEFT   0x0080u
#define VJO_RSEL_CIRCLE 0x2000u
#define VJO_RSEL_CROSS  0x4000u

/* The left stick moves nothing within VJO_RSEL_DEAD of its centre (the idle
 * stick read 1-3 from it in #81's console logs). Beyond, its speed grows with
 * the square of the deflection, from VJO_RSEL_SLOW px/s at the dead zone's
 * edge to VJO_RSEL_FAST at full deflection, and while it stays out of the dead
 * zone up to VJO_RSEL_ACCEL times that within VJO_RSEL_RAMP_MS (scaled by the
 * deflection, so a small push stays slow): 1,200 px/s at full deflection. */
#define VJO_RSEL_DEAD 32
#define VJO_RSEL_SLOW 30
#define VJO_RSEL_FAST 600
#define VJO_RSEL_ACCEL 2
#define VJO_RSEL_RAMP_MS 600
/* The d-pad moves one pixel per press. Held, it repeats after
 * VJO_RSEL_REPEAT_MS at VJO_RSEL_REPEAT px/s, and one second later at
 * VJO_RSEL_REPEAT_FAST px/s. */
#define VJO_RSEL_REPEAT_MS 300
#define VJO_RSEL_REPEAT 30
#define VJO_RSEL_REPEAT_FAST 300
/* A frame's time step counts at most this much: a stalled frame never makes
 * the cursor jump. */
#define VJO_RSEL_MAX_STEP_US 50000

typedef struct {
    uint32_t held, pressed; /* VJO_RSEL_* held now, and newly pressed */
    uint8_t lx, ly;         /* the left stick, 128 = centre */
    int touch;              /* a front touch is down at (tx, ty), screen pixels */
    int tx, ty;
    int64_t now_us;
} VjoRselInput;

/* What an update did. */
enum {
    VJO_RSEL_NONE = 0, /* at most the cursor moved */
    VJO_RSEL_CORNER,   /* the first corner was set, by Cross or a new touch */
    VJO_RSEL_BACK,     /* Circle took the first corner back */
    VJO_RSEL_SMALL,    /* Cross with a box under VJO_RSEL_MIN either way: not saved */
    VJO_RSEL_SAVE,     /* Cross: save vjo_rsel_box's box and leave */
    VJO_RSEL_CANCEL    /* Circle with no first corner: leave, nothing saved */
};

typedef struct {
    int32_t x, y;           /* the cursor, 1/256 px */
    int ax, ay;             /* the first corner, px, while anchored */
    int anchored;
    int touching;           /* 1: a drag moves the cursor; 2: a touch ignored until it lifts */
    int64_t last_us;        /* the previous update */
    uint32_t stick_us;      /* time the stick has been out of its dead zone */
    uint32_t pad_on;        /* d-pad directions pressed and still held (VJO_RSEL_UP << i) */
    uint32_t pad_us[4];     /* time each has been held: up, right, down, left */
    uint32_t pad_steps[4];  /* pixels each has moved since its press */
} VjoRegionSelect;

/* Starts a selection: the cursor at the screen's centre, no first corner. */
void vjo_rsel_begin(VjoRegionSelect *s, int64_t now_us);
/* One frame: touch, then the d-pad and the stick (not during a drag), then
 * Circle and Cross. Returns VJO_RSEL_*. */
int vjo_rsel_update(VjoRegionSelect *s, const VjoRselInput *in);
/* The cursor's pixel. */
void vjo_rsel_cursor(const VjoRegionSelect *s, int *x, int *y);
/* The box from the first corner to the cursor, both pixels inside it (the
 * cursor on the corner is a 1 x 1 box). Returns 0 when there is no corner. */
int vjo_rsel_box(const VjoRegionSelect *s, int *x, int *y, int *w, int *h);
/* A box of screen pixels as a saved region (0..65535 each way): the origin
 * rounded up and the size down, so kernel/capture.c's floor/ceil give back
 * exactly this box at 960 x 544 (before its even-x alignment). */
VjoRect vjo_rsel_rect(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
