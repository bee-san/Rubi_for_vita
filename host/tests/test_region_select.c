/* The region selector's state machine (core/region_select.c), driven frame by
 * frame as shell/overlay.cpp drives it: the cursor, the stick's dead zone,
 * speed curve and acceleration, the d-pad's single steps and repeat, the two
 * corners, enter and cancel under either button assignment, touch drags, the
 * screen's edges and the saved region. */
#include <string.h>

#include "acutest.h"
#include "buttons.h"
#include "region_select.h"
#include "regions.h"

#define FRAME_US 16667 /* 60 fps */
#define C 128          /* stick centre */

/* The console's buttons for the rig: × enters unless a test assigns ○. */
static VjoButtons btn;
#define ENTER (btn.enter)
#define CANCEL (btn.cancel)

typedef struct {
    VjoRegionSelect s;
    VjoRselInput in;
    uint32_t prev; /* buttons held at the previous frame */
} Rig;

static void rig_begin(Rig *r, uint32_t held)
{
    memset(r, 0, sizeof(*r));
    r->in.lx = r->in.ly = C;
    r->in.now_us = 5000000;
    if (!btn.enter)
        btn = vjo_buttons(VJO_BUTTON_ASSIGN_CROSS);
    r->in.enter = btn.enter;
    r->in.cancel = btn.cancel;
    r->in.held = r->prev = held; /* as the overlay: buttons held on entry are not presses */
    vjo_rsel_begin(&r->s, r->in.now_us);
}

/* One frame `us` after the previous, with `held` held. */
static int frame(Rig *r, uint32_t held, int us)
{
    r->in.now_us += us;
    r->in.held = held;
    r->in.pressed = held & ~r->prev;
    r->prev = held;
    return vjo_rsel_update(&r->s, &r->in);
}

/* n frames; returns the last event other than NONE. */
static int frames(Rig *r, uint32_t held, int n, int us)
{
    int last = VJO_RSEL_NONE;
    for (int i = 0; i < n; i++) {
        int ev = frame(r, held, us);
        if (ev != VJO_RSEL_NONE)
            last = ev;
    }
    return last;
}

static void stick(Rig *r, int lx, int ly)
{
    r->in.lx = (uint8_t)lx;
    r->in.ly = (uint8_t)ly;
}

static void touch(Rig *r, int down, int x, int y)
{
    r->in.touch = down;
    r->in.tx = x;
    r->in.ty = y;
}

static int cx(Rig *r)
{
    int x, y;
    vjo_rsel_cursor(&r->s, &x, &y);
    return x;
}

static int cy(Rig *r)
{
    int x, y;
    vjo_rsel_cursor(&r->s, &x, &y);
    return y;
}

static int box_is(Rig *r, int x, int y, int w, int h)
{
    int bx, by, bw, bh;
    if (!vjo_rsel_box(&r->s, &bx, &by, &bw, &bh)) {
        TEST_MSG("no box; want %d,%d %dx%d", x, y, w, h);
        return 0;
    }
    TEST_MSG("box %d,%d %dx%d; want %d,%d %dx%d", bx, by, bw, bh, x, y, w, h);
    return bx == x && by == y && bw == w && bh == h;
}

/* Puts the cursor on (x, y) with single d-pad presses (at most a screen's
 * worth each way, so a broken d-pad fails instead of hanging). */
static void dpad_to(Rig *r, int x, int y)
{
    for (int n = 0; cx(r) != x && n < VJO_RSEL_W; n++) {
        frame(r, cx(r) < x ? VJO_RSEL_RIGHT : VJO_RSEL_LEFT, FRAME_US);
        frame(r, 0, FRAME_US);
    }
    for (int n = 0; cy(r) != y && n < VJO_RSEL_H; n++) {
        frame(r, cy(r) < y ? VJO_RSEL_DOWN : VJO_RSEL_UP, FRAME_US);
        frame(r, 0, FRAME_US);
    }
    TEST_CHECK_(cx(r) == x && cy(r) == y, "d-pad to %d,%d reached %d,%d", x, y, cx(r), cy(r));
}

static void starts_centred_without_a_box(void)
{
    Rig r;
    int x, y, w, h;
    rig_begin(&r, 0);
    TEST_CHECK(cx(&r) == 480 && cy(&r) == 272);
    TEST_CHECK(!vjo_rsel_box(&r.s, &x, &y, &w, &h));
    TEST_CHECK(frames(&r, 0, 120, FRAME_US) == VJO_RSEL_NONE);
    TEST_CHECK(cx(&r) == 480 && cy(&r) == 272);
}

static void dead_zone_ignores_idle_drift(void)
{
    static const int idle[][2] = {{C + 1, C - 3}, {C + 32, C - 32}, {C - 32, C + 32}, {C + 20, C}};
    Rig r;
    for (unsigned i = 0; i < sizeof(idle) / sizeof(idle[0]); i++) {
        rig_begin(&r, 0);
        stick(&r, idle[i][0], idle[i][1]);
        frames(&r, 0, 300, FRAME_US); /* 5 s */
        TEST_CHECK_(cx(&r) == 480 && cy(&r) == 272, "stick %d,%d moved to %d,%d", idle[i][0], idle[i][1],
                    cx(&r), cy(&r));
    }
    rig_begin(&r, 0);
    stick(&r, C + 33, C); /* just past the dead zone: slow, but moving */
    frames(&r, 0, 60, FRAME_US);
    TEST_CHECK_(cx(&r) > 480 + 20 && cx(&r) < 480 + 40, "x %d after 1 s at the dead zone's edge", cx(&r));
    TEST_CHECK(cy(&r) == 272);
}

static void stick_curve_and_acceleration(void)
{
    Rig r;
    int x0, first, late;
    /* a small push stays slow: about 35-38 px/s */
    rig_begin(&r, 0);
    stick(&r, C + 41, C);
    frames(&r, 0, 60, FRAME_US);
    TEST_CHECK_(cx(&r) - 480 >= 30 && cx(&r) - 480 <= 45, "small push: %d px in 1 s", cx(&r) - 480);
    /* full deflection: 600 px/s, rising to 1,200 px/s over 0.6 s; about 181 px in the first 0.25 s */
    rig_begin(&r, 0);
    dpad_to(&r, 0, 272);
    stick(&r, 255, C);
    x0 = cx(&r);
    frames(&r, 0, 6, FRAME_US);
    first = cx(&r) - x0;
    frames(&r, 0, 9, FRAME_US);
    TEST_CHECK_(cx(&r) - x0 >= 170 && cx(&r) - x0 <= 195, "full push: %d px in 0.25 s", cx(&r) - x0);
    frames(&r, 0, 21, FRAME_US); /* to 0.6 s */
    x0 = cx(&r);
    frames(&r, 0, 6, FRAME_US);
    late = cx(&r) - x0;
    TEST_CHECK_(late > first * 17 / 10, "speeds up while held: %d px then %d px per 0.1 s", first, late);
    TEST_CHECK_(late >= 115 && late <= 125, "%d px per 0.1 s at full speed (1,200 px/s)", late);
    /* back to the centre ends the acceleration */
    stick(&r, C, C);
    frame(&r, 0, FRAME_US);
    dpad_to(&r, 0, 272);
    stick(&r, 255, C);
    x0 = cx(&r);
    frames(&r, 0, 6, FRAME_US);
    TEST_CHECK_(cx(&r) - x0 == first, "after a release the first 0.1 s is %d px again (%d)", first, cx(&r) - x0);
    /* both directions of both axes reach the same speed */
    rig_begin(&r, 0);
    stick(&r, 0, C);
    frames(&r, 0, 6, FRAME_US);
    TEST_CHECK_(480 - cx(&r) >= first - 1 && 480 - cx(&r) <= first + 1, "left %d px, right %d px", 480 - cx(&r), first);
    rig_begin(&r, 0);
    stick(&r, C, 0);
    frames(&r, 0, 6, FRAME_US);
    TEST_CHECK_(272 - cy(&r) >= first - 1 && 272 - cy(&r) <= first + 1, "up %d px", 272 - cy(&r));
    rig_begin(&r, 0);
    stick(&r, C, 255);
    frames(&r, 0, 6, FRAME_US);
    TEST_CHECK_(cy(&r) - 272 >= first - 1 && cy(&r) - 272 <= first + 1, "down %d px", cy(&r) - 272);
    /* a diagonal moves both axes */
    rig_begin(&r, 0);
    stick(&r, 255, 255);
    frames(&r, 0, 6, FRAME_US);
    TEST_CHECK(cx(&r) - 480 == first && cy(&r) - 272 == first);
}

static void crosses_the_screen_in_under_a_second(void)
{
    Rig r;
    int n;
    rig_begin(&r, 0);
    stick(&r, 0, C);
    for (n = 0; cx(&r) > 0 && n < 600; n++)
        frame(&r, 0, FRAME_US);
    TEST_CHECK_(n <= 36, "centre to the left edge (480 px): %d frames", n);
    stick(&r, C, C);
    frame(&r, 0, FRAME_US);
    stick(&r, 255, C);
    for (n = 0; cx(&r) < VJO_RSEL_W - 1 && n < 600; n++)
        frame(&r, 0, FRAME_US);
    TEST_CHECK_(n <= 60, "the screen's width (960 px): %d frames", n);
    stick(&r, C, C);
    frame(&r, 0, FRAME_US);
    stick(&r, C, 0);
    for (n = 0; cy(&r) > 0 && n < 600; n++)
        frame(&r, 0, FRAME_US);
    TEST_CHECK_(n <= 36, "centre to the top edge (272 px): %d frames", n);
}

static void speed_does_not_depend_on_the_frame_rate(void)
{
    /* 100, 60, 50 and 25 fps, one second each */
    static const int rates[] = {10000, 16667, 20000, 40000}, counts[] = {100, 60, 50, 25};
    int moved[4];
    for (int i = 0; i < 4; i++) {
        Rig r;
        rig_begin(&r, 0);
        dpad_to(&r, 0, 272);
        stick(&r, C + 72, C);
        frames(&r, 0, counts[i], rates[i]);
        moved[i] = cx(&r);
    }
    for (int i = 1; i < 4; i++)
        TEST_CHECK_(moved[i] - moved[0] <= 3 && moved[0] - moved[i] <= 3, "1 s at %d fps: %d px, at 100 fps: %d px",
                    counts[i], moved[i], moved[0]);
}

static void a_stalled_frame_moves_little(void)
{
    Rig r;
    rig_begin(&r, 0);
    stick(&r, 255, C);
    frame(&r, 0, 2000000); /* the paf thread stalled for 2 s */
    TEST_CHECK_(cx(&r) - 480 > 0 && cx(&r) - 480 <= 40, "%d px after a 2 s frame", cx(&r) - 480);
    stick(&r, C, C);
    frame(&r, 0, -5000); /* a clock going back moves nothing */
    TEST_CHECK(cx(&r) - 480 <= 40);
}

static void dpad_single_steps_and_repeat(void)
{
    Rig r;
    rig_begin(&r, 0);
    frame(&r, VJO_RSEL_RIGHT, 10000);
    TEST_CHECK(cx(&r) == 481 && cy(&r) == 272);
    frame(&r, 0, 10000);
    frame(&r, VJO_RSEL_RIGHT, 10000);
    TEST_CHECK(cx(&r) == 482);
    frames(&r, VJO_RSEL_RIGHT, 30, 10000); /* held 0.3 s: no repeat yet */
    TEST_CHECK_(cx(&r) == 482, "x %d", cx(&r));
    frames(&r, VJO_RSEL_RIGHT, 100, 10000); /* 1.3 s: 30 px/s for 1 s */
    TEST_CHECK_(cx(&r) == 482 + 30, "x %d", cx(&r));
    frames(&r, VJO_RSEL_RIGHT, 100, 10000); /* 2.3 s: then 300 px/s */
    TEST_CHECK_(cx(&r) == 482 + 30 + 300, "x %d", cx(&r));
    frame(&r, 0, 10000);
    frame(&r, VJO_RSEL_LEFT, 10000);
    TEST_CHECK(cx(&r) == 811);
    frame(&r, VJO_RSEL_UP, 10000);
    TEST_CHECK(cy(&r) == 271 && cx(&r) == 811);
    frame(&r, 0, 10000);
    frame(&r, VJO_RSEL_DOWN, 10000);
    frame(&r, 0, 10000);
    frame(&r, VJO_RSEL_DOWN, 10000);
    TEST_CHECK(cy(&r) == 273);
    /* two directions at once move both axes */
    frame(&r, 0, 10000);
    frame(&r, VJO_RSEL_LEFT | VJO_RSEL_UP, 10000);
    TEST_CHECK(cx(&r) == 810 && cy(&r) == 272);
    /* a direction held since before the selection began does nothing until pressed again */
    rig_begin(&r, VJO_RSEL_RIGHT);
    frames(&r, VJO_RSEL_RIGHT, 120, 10000);
    TEST_CHECK(cx(&r) == 480);
    frame(&r, 0, 10000);
    frame(&r, VJO_RSEL_RIGHT, 10000);
    TEST_CHECK(cx(&r) == 481);
}

static void dpad_after_the_stick_lands_on_whole_pixels(void)
{
    Rig r;
    int x;
    rig_begin(&r, 0);
    stick(&r, C + 40, C);
    frames(&r, 0, 7, FRAME_US); /* a fraction of a pixel past a whole one */
    stick(&r, C, C);
    TEST_CHECK((r.s.x & 255) != 128);
    x = cx(&r);
    frame(&r, VJO_RSEL_LEFT, FRAME_US);
    TEST_CHECK_((r.s.x & 255) == 128 && cx(&r) == x - 1, "on the pixel's centre: %d/256 of %d", r.s.x & 255, cx(&r));
    frame(&r, 0, FRAME_US);
    frame(&r, VJO_RSEL_RIGHT, FRAME_US);
    TEST_CHECK_(cx(&r) == x, "one step left and one right: %d, was %d", cx(&r), x);
}

static void clamped_to_the_screen(void)
{
    Rig r;
    rig_begin(&r, 0);
    stick(&r, 0, 0);
    frames(&r, 0, 120, FRAME_US);
    TEST_CHECK(cx(&r) == 0 && cy(&r) == 0);
    stick(&r, C, C);
    frame(&r, VJO_RSEL_LEFT | VJO_RSEL_UP, FRAME_US);
    TEST_CHECK(cx(&r) == 0 && cy(&r) == 0);
    stick(&r, 255, 255);
    frames(&r, 0, 120, FRAME_US);
    TEST_CHECK(cx(&r) == VJO_RSEL_W - 1 && cy(&r) == VJO_RSEL_H - 1);
    stick(&r, C, C);
    frames(&r, VJO_RSEL_RIGHT | VJO_RSEL_DOWN, 300, FRAME_US); /* held repeat into the edge */
    TEST_CHECK(cx(&r) == VJO_RSEL_W - 1 && cy(&r) == VJO_RSEL_H - 1);
    /* a full-screen box from corner to corner */
    frames(&r, 0, 1, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_CORNER);
    stick(&r, 0, 0);
    frames(&r, 0, 120, FRAME_US);
    TEST_CHECK(box_is(&r, 0, 0, VJO_RSEL_W, VJO_RSEL_H));
    /* touches outside the panel's range are pinned to the edge */
    rig_begin(&r, 0);
    touch(&r, 1, -50, 9999);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_CORNER);
    TEST_CHECK(cx(&r) == 0 && cy(&r) == VJO_RSEL_H - 1);
    touch(&r, 1, 5000, -1);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(box_is(&r, 0, 0, VJO_RSEL_W, VJO_RSEL_H));
}

static void two_corners_with_enter(void)
{
    Rig r;
    int x, y, w, h;
    rig_begin(&r, 0);
    dpad_to(&r, 48, 417);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_CORNER);
    TEST_CHECK(box_is(&r, 48, 417, 1, 1));
    /* enter again at once (a double press) saves nothing */
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SMALL);
    TEST_CHECK(box_is(&r, 48, 417, 1, 1));
    /* the box follows the cursor */
    dpad_to(&r, 48 + 30, 417 + 31);
    TEST_CHECK(box_is(&r, 48, 417, 31, 32));
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SMALL); /* 31 wide */
    dpad_to(&r, 48 + 31, 417 + 30);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SMALL); /* 31 high */
    dpad_to(&r, 48 + 31, 417 + 31);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE); /* 32 x 32: the minimum */
    TEST_CHECK(box_is(&r, 48, 417, 32, 32));
    /* a CLANNAD-sized box drawn from its bottom-right corner: 883 x 112 at 48,417 */
    rig_begin(&r, 0);
    dpad_to(&r, 930, 528);
    frame(&r, ENTER, FRAME_US);
    stick(&r, 0, 0);
    for (int n = 0; (cx(&r) > 48 + 40 || cy(&r) > 417 + 40) && n < 600; n++)
        frame(&r, 0, FRAME_US);
    stick(&r, C, C);
    dpad_to(&r, 48, 417);
    TEST_CHECK(box_is(&r, 48, 417, 883, 112)); /* the cursor above and left of the corner */
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE);
    TEST_ASSERT(vjo_rsel_box(&r.s, &x, &y, &w, &h));
    TEST_CHECK(x == 48 && y == 417 && w == 883 && h == 112);
}

static void cancel_takes_the_corner_back_then_cancels_with(int assign)
{
    btn = vjo_buttons(assign);
    Rig r;
    int x, y, w, h;
    rig_begin(&r, 0);
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_CANCEL);
    rig_begin(&r, 0);
    frame(&r, ENTER, FRAME_US);
    dpad_to(&r, 600, 400);
    TEST_CHECK(box_is(&r, 480, 272, 121, 129));
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_BACK);
    TEST_CHECK(!vjo_rsel_box(&r.s, &x, &y, &w, &h));
    TEST_CHECK(cx(&r) == 600 && cy(&r) == 400); /* the cursor stays for a new first corner */
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_CORNER);
    TEST_CHECK(box_is(&r, 600, 400, 1, 1));
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_BACK);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_CANCEL);
    /* enter and cancel in the same frame: cancel wins, nothing is saved */
    rig_begin(&r, 0);
    frame(&r, ENTER, FRAME_US);
    dpad_to(&r, 600, 400);
    TEST_CHECK(frame(&r, ENTER | CANCEL, FRAME_US) == VJO_RSEL_BACK);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER | CANCEL, FRAME_US) == VJO_RSEL_CANCEL);
}

static void cancel_takes_the_corner_back_then_cancels(void)
{
    cancel_takes_the_corner_back_then_cancels_with(VJO_BUTTON_ASSIGN_CROSS);
    TEST_CHECK(ENTER == VJO_BUTTON_CROSS && CANCEL == VJO_BUTTON_CIRCLE);
}

/* A console set to enter with ○ (Japan's default): ○ sets and saves corners,
 * × takes them back and leaves; the same presses as the × console's swapped. */
static void circle_enters_on_a_circle_console(void)
{
    Rig r;
    cancel_takes_the_corner_back_then_cancels_with(VJO_BUTTON_ASSIGN_CIRCLE);
    TEST_CHECK(ENTER == VJO_BUTTON_CIRCLE && CANCEL == VJO_BUTTON_CROSS);
    rig_begin(&r, 0);
    TEST_CHECK(frame(&r, VJO_BUTTON_CROSS, FRAME_US) == VJO_RSEL_CANCEL);
    rig_begin(&r, 0);
    TEST_CHECK(frame(&r, VJO_BUTTON_CIRCLE, FRAME_US) == VJO_RSEL_CORNER);
    dpad_to(&r, 600, 400);
    TEST_CHECK(frame(&r, VJO_BUTTON_CIRCLE, FRAME_US) == VJO_RSEL_SAVE);
    btn = vjo_buttons(VJO_BUTTON_ASSIGN_CROSS);
}

static void touch_draws_and_the_stick_finishes(void)
{
    Rig r;
    int x, y, w, h;
    rig_begin(&r, 0);
    touch(&r, 1, 100, 400);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_CORNER);
    TEST_CHECK(box_is(&r, 100, 400, 1, 1));
    touch(&r, 1, 700, 500);
    stick(&r, 0, 0); /* the finger has the cursor: the stick and the d-pad wait */
    TEST_CHECK(frame(&r, VJO_RSEL_LEFT, FRAME_US) == VJO_RSEL_NONE);
    frames(&r, 0, 30, FRAME_US);
    TEST_CHECK(box_is(&r, 100, 400, 601, 101));
    /* lifted: the stick and the d-pad move the opposite corner */
    stick(&r, C, C);
    touch(&r, 0, 0, 0);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_NONE);
    TEST_CHECK(box_is(&r, 100, 400, 601, 101));
    dpad_to(&r, 703, 498);
    TEST_CHECK(box_is(&r, 100, 400, 604, 99));
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE);
    TEST_ASSERT(vjo_rsel_box(&r.s, &x, &y, &w, &h));
    TEST_CHECK(x == 100 && y == 400 && w == 604 && h == 99);
    /* a new drag replaces the box, from where it starts */
    rig_begin(&r, 0);
    touch(&r, 1, 10, 20);
    frame(&r, 0, FRAME_US);
    touch(&r, 1, 300, 200);
    frame(&r, 0, FRAME_US);
    touch(&r, 0, 0, 0);
    frame(&r, 0, FRAME_US);
    touch(&r, 1, 900, 500);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_CORNER);
    TEST_CHECK(box_is(&r, 900, 500, 1, 1));
    /* enter during a drag saves the box drawn so far, as before */
    touch(&r, 1, 500, 300);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE);
    TEST_CHECK(box_is(&r, 500, 300, 401, 201));
}

/* The overlay's one-off box (Cancel): a drag's lift is enter, so the box is read as soon as it is drawn; a tap's
 * 1 x 1 box is refused as enter refuses it, and the stick and enter still finish a box; a drag cancelled while the
 * finger is down reads nothing as it lifts. */
static void the_one_off_box_reads_on_lift(void)
{
    Rig r;
    int x, y, w, h;
    rig_begin(&r, 0);
    r.s.once = 1;
    touch(&r, 1, 100, 400);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_CORNER);
    touch(&r, 1, 300, 480);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_NONE);
    touch(&r, 0, 0, 0);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_SAVE);
    TEST_ASSERT(vjo_rsel_box(&r.s, &x, &y, &w, &h));
    TEST_CHECK(x == 100 && y == 400 && w == 201 && h == 81);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_NONE); /* one lift, one read */
    /* a tap: too small, the corner kept; the d-pad and enter finish it */
    rig_begin(&r, 0);
    r.s.once = 1;
    touch(&r, 1, 10, 20);
    frame(&r, 0, FRAME_US);
    touch(&r, 0, 0, 0);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_SMALL);
    dpad_to(&r, 60, 70);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE && box_is(&r, 10, 20, 51, 51));
    /* cancel during the drag: its lift reads nothing */
    rig_begin(&r, 0);
    r.s.once = 1;
    touch(&r, 1, 200, 200);
    frame(&r, 0, FRAME_US);
    touch(&r, 1, 400, 300);
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_BACK);
    touch(&r, 0, 0, 0);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_NONE);
}

static void cancel_during_a_drag_waits_for_the_finger(void)
{
    Rig r;
    rig_begin(&r, 0);
    touch(&r, 1, 200, 200);
    frame(&r, 0, FRAME_US);
    touch(&r, 1, 400, 300);
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_BACK);
    /* the finger still down neither starts a new corner nor moves the cursor, and its lift reads no box */
    touch(&r, 1, 450, 350);
    TEST_CHECK(frames(&r, 0, 30, FRAME_US) == VJO_RSEL_NONE);
    TEST_CHECK(cx(&r) == 400 && cy(&r) == 300);
    /* ...but the d-pad does */
    frame(&r, VJO_RSEL_RIGHT, FRAME_US);
    TEST_CHECK(cx(&r) == 401);
    touch(&r, 0, 0, 0);
    frame(&r, 0, FRAME_US);
    touch(&r, 1, 50, 60);
    TEST_CHECK(frame(&r, 0, FRAME_US) == VJO_RSEL_CORNER);
    TEST_CHECK(box_is(&r, 50, 60, 1, 1));
}

static void saved_region_round_trips_region_ini(void)
{
    static const int boxes[][4] = {
        {0, 0, 960, 544}, {48, 417, 883, 112}, {928, 512, 32, 32}, {0, 0, 32, 32}, {1, 1, 959, 543}, {479, 271, 1, 1},
    };
    for (unsigned i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
        char line[80];
        VjoRect r = vjo_rsel_rect(boxes[i][0], boxes[i][1], boxes[i][2], boxes[i][3]), back;
        int n = vjo_region_format(line, sizeof(line), "PCSG00415", &r);
        TEST_ASSERT(n > 0);
        TEST_CHECK_(vjo_region_parse(line, (size_t)n, "PCSG00415", &back) == VJO_REGION_GAME &&
                    !memcmp(&back, &r, sizeof(r)), "%s", line);
    }
    /* every box on the screen is a region region.ini accepts: non-empty, inside 0..65536 */
    for (int x = 0; x < VJO_RSEL_W; x++)
        for (int w = 1; w <= VJO_RSEL_W - x; w++) {
            VjoRect r = vjo_rsel_rect(x, 0, w, 1);
            if (!r.w || (uint32_t)r.x + r.w > 65536u) {
                TEST_CHECK_(0, "x %d w %d -> %u,%u", x, w, r.x, r.w);
                return;
            }
        }
    for (int y = 0; y < VJO_RSEL_H; y++)
        for (int h = 1; h <= VJO_RSEL_H - y; h++) {
            VjoRect r = vjo_rsel_rect(0, y, 1, h);
            if (!r.h || (uint32_t)r.y + r.h > 65536u) {
                TEST_CHECK_(0, "y %d h %d -> %u,%u", y, h, r.y, r.h);
                return;
            }
        }
    /* out-of-range input is pinned to the screen */
    {
        VjoRect a = vjo_rsel_rect(-10, -10, 5000, 5000), b = vjo_rsel_rect(0, 0, 960, 544);
        TEST_CHECK(!memcmp(&a, &b, sizeof(a)) && a.w == 65535 && a.h == 65535);
    }
    /* A CLANNAD-sized box, 883 x 112 at 48,417. The old conversion floored both
     * origins (3276,50236), which the kernel reads back as 47 (46 after its even-x
     * alignment) and 416, a pixel above and left of the box drawn; rounding the
     * origin up reads back 48 and 417. */
    {
        VjoRect c = vjo_rsel_rect(48, 417, 883, 112);
        TEST_CHECK_(c.x == 3277 && c.y == 50237 && c.w == 60279 && c.h == 13492, "%u,%u,%u,%u", c.x, c.y, c.w, c.h);
    }
}

/* #358: the suggested box drawn from the start (vjo_rsel_preset): the screen's bottom `rows` rows across its width, its
 * first corner at the top left, the cursor on the bottom right pixel, so that enter saves the box as it is and the stick
 * or the d-pad move its bottom right corner; cancel takes the corner back as for any box. Rows from the game's process
 * that are 0 or more than the screen's are refused, the selection as it was; a band too short is drawn and enter
 * refuses it as any box. */
static void a_suggested_box_is_drawn_and_saved_as_it_is(void)
{
    Rig r;
    rig_begin(&r, 0);
    TEST_CHECK(vjo_rsel_preset(&r.s, 170)); /* the game worker's band: 374 down to the bottom */
    TEST_CHECK(box_is(&r, 0, 374, 960, 170) && cx(&r) == 959 && cy(&r) == 543);
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE);
    /* the stick moves the bottom right corner; enter saves the box as it is then */
    rig_begin(&r, 0);
    TEST_CHECK(vjo_rsel_preset(&r.s, 170));
    dpad_to(&r, 700, 500);
    TEST_CHECK(box_is(&r, 0, 374, 701, 127));
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE);
    /* cancel takes its corner back: drawing by hand from the cursor; cancel again leaves */
    rig_begin(&r, 0);
    TEST_CHECK(vjo_rsel_preset(&r.s, 170));
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_BACK);
    {
        int x, y, w, h;
        TEST_CHECK(!vjo_rsel_box(&r.s, &x, &y, &w, &h) && cx(&r) == 959 && cy(&r) == 543);
    }
    frame(&r, 0, FRAME_US);
    TEST_CHECK(frame(&r, CANCEL, FRAME_US) == VJO_RSEL_CANCEL);
    /* the shortest band enter saves, 32 rows; 31 is drawn and refused by enter (VJO_RSEL_SMALL); the whole screen */
    rig_begin(&r, 0);
    TEST_CHECK(vjo_rsel_preset(&r.s, 32) && box_is(&r, 0, 512, 960, 32));
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SAVE);
    rig_begin(&r, 0);
    TEST_CHECK(vjo_rsel_preset(&r.s, 31) && box_is(&r, 0, 513, 960, 31));
    TEST_CHECK(frame(&r, ENTER, FRAME_US) == VJO_RSEL_SMALL);
    rig_begin(&r, 0);
    TEST_CHECK(vjo_rsel_preset(&r.s, 544) && box_is(&r, 0, 0, 960, 544));
    /* refused, the selection as it was: none, more than the screen, the Shell's word for a box taken */
    {
        static const uint32_t refused[] = {0, 545, 1023, VJO_RSEL_TAKEN};
        for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            rig_begin(&r, 0);
            TEST_CHECK_(!vjo_rsel_preset(&r.s, refused[i]) && !r.s.anchored && cx(&r) == VJO_RSEL_W / 2 &&
                        cy(&r) == VJO_RSEL_H / 2, "rows %u kept the selection as it was", refused[i]);
        }
    }
}

/* #358: the Shell's word for the suggested box (shell/shell.h VjoView.suggest): the newest job's box is offered once in
 * an activation of the game; none after it, nor for a game with a region (VJO_RSEL_TAKEN), until the control thread
 * stores 0 as the game next starts or resumes (shell/worker.c push_region). */
static void a_suggested_box_is_offered_once_an_activation(void)
{
    uint32_t word = 0;
    TEST_CHECK(vjo_rsel_take(&word) == 0 && word == 0); /* none yet */
    vjo_rsel_publish(&word, 169);
    vjo_rsel_publish(&word, 0); /* the newest screen holds no dialogue: none for it */
    TEST_CHECK(vjo_rsel_take(&word) == 0 && word == 0);
    vjo_rsel_publish(&word, 200);
    vjo_rsel_publish(&word, 169); /* the newest screen's */
    TEST_CHECK(vjo_rsel_take(&word) == 169 && word == VJO_RSEL_TAKEN);
    TEST_CHECK(vjo_rsel_take(&word) == 0); /* once */
    vjo_rsel_publish(&word, 170);         /* later jobs': none after it */
    vjo_rsel_publish(&word, 0);
    TEST_CHECK(word == VJO_RSEL_TAKEN && vjo_rsel_take(&word) == 0);
    word = 0; /* the game resumes */
    vjo_rsel_publish(&word, 170);
    TEST_CHECK(vjo_rsel_take(&word) == 170 && word == VJO_RSEL_TAKEN);
    /* rows from the game's process that make no band are never offered, nor taken */
    word = 0;
    vjo_rsel_publish(&word, 545);
    TEST_CHECK(vjo_rsel_take(&word) == 0 && word == 545);
    vjo_rsel_publish(&word, 544);
    TEST_CHECK(vjo_rsel_take(&word) == 544);
    word = 0;
    vjo_rsel_publish(&word, VJO_RSEL_TAKEN); /* the word for taken: none, as if taken */
    TEST_CHECK(vjo_rsel_take(&word) == 0 && word == VJO_RSEL_TAKEN);
}

TEST_LIST = {
    {"starts centred, without a box", starts_centred_without_a_box},
    {"dead zone ignores idle drift", dead_zone_ignores_idle_drift},
    {"stick: square curve and acceleration", stick_curve_and_acceleration},
    {"stick crosses the screen in under a second", crosses_the_screen_in_under_a_second},
    {"stick speed does not depend on the frame rate", speed_does_not_depend_on_the_frame_rate},
    {"a stalled frame moves little", a_stalled_frame_moves_little},
    {"d-pad: single steps, then repeat", dpad_single_steps_and_repeat},
    {"d-pad after the stick lands on whole pixels", dpad_after_the_stick_lands_on_whole_pixels},
    {"clamped to the screen", clamped_to_the_screen},
    {"two corners with enter, minimum size", two_corners_with_enter},
    {"cancel takes the corner back, then cancels", cancel_takes_the_corner_back_then_cancels},
    {"○ enters on a console set to ○", circle_enters_on_a_circle_console},
    {"touch draws, the stick finishes", touch_draws_and_the_stick_finishes},
    {"the one-off box is read as the drag lifts", the_one_off_box_reads_on_lift},
    {"cancel during a drag waits for the finger", cancel_during_a_drag_waits_for_the_finger},
    {"saved region round-trips region.ini", saved_region_round_trips_region_ini},
    {"a suggested box is drawn and saved as it is (#358)", a_suggested_box_is_drawn_and_saved_as_it_is},
    {"a suggested box is offered once an activation (#358)", a_suggested_box_is_offered_once_an_activation},
    {NULL, NULL}
};
