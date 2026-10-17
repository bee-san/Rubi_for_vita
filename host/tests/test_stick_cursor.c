/* The left stick over the overlay's recognized text (#138, core/stick_cursor.h),
 * by the clock at 20 to 60 Hz and on irregular frames:
 *
 * - the speed: the push's step, the lists' 166.7 ms pause, then steps that come
 *   12.5 ms sooner each time from 125 ms down to the cap, 50 ms: never more than
 *   one step in a frame, never more than 21 in any second, 20 a second held;
 * - the axes: the farther one counts, the one held keeps counting until let go;
 * - the deferred lookup (the debounce): a stick move makes the word due; it is
 *   looked up once, when the cursor has rested stick_lookup_ms and the stick no
 *   longer drives it (let go, or held against the text's end); the d-pad looks up
 *   at once and drops a due one. The overlay's frame (shell/overlay.cpp
 *   input_overlay: the d-pad, then the stick's step, then its lookup) is replayed
 *   here over a cursor on a text;
 * - the stick's step along the text (core/text_rows.c vjo_text_row_key with
 *   `stick`, the arrows' Left/Right): in reading order across lines, the
 *   matched word skipped, never round the ends. host/tests/test_cursor_nav.c
 *   compares the stick's four directions with the d-pad's on layouts.
 *
 * The texts are synthetic. */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "stick_cursor.h"
#include "text_rows.h"
#include "utf.h"

#define HZ60 16667u /* frame intervals, us */
#define HZ30 33333u
#define REST 200000u /* stick_lookup_ms's default, us */

#define RIGHT 255
#define LEFT 0
#define MID 128

/* ---- the stick alone ---- */

typedef struct {
    VjoStickCursor s;
    uint64_t now;
} Stick;

static Stick stick(void)
{
    Stick k;
    memset(&k, 0, sizeof(k));
    k.now = 5000000u; /* the console's clock is far from 0 */
    return k;
}

static int frame(Stick *k, uint8_t lx, uint8_t ly, uint64_t dt)
{
    k->now += dt;
    return vjo_stick_cursor(&k->s, lx, ly, k->now);
}

/* Holds the stick right for `us` at frames of dt: the steps' times (from the
 * push) into t (at most cap), their count returned. vjo_stick_cursor returns
 * one direction, so a frame takes at most one step whatever its length. */
static int hold(Stick *k, uint64_t us, uint64_t dt, uint64_t *t, int cap)
{
    uint64_t start = k->now, frames = 0;
    int n = 0;
    for (uint64_t at = 0; at < us; at += dt) {
        int d = frame(k, RIGHT, MID, frames++ ? dt : 0);
        if (d) {
            TEST_CHECK(d == 1);
            if (n < cap) t[n] = k->now - start;
            n++;
        }
    }
    return n;
}

static void the_threshold_is_the_lists(void)
{
    Stick k = stick();
    /* 64 from the centre is still the centre, either way and either axis; 65 is a push */
    for (int i = 0; i < 30; i++)
        TEST_CHECK(frame(&k, 64, 192, HZ60) == 0 && frame(&k, 192, 64, HZ60) == 0);
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    TEST_CHECK(frame(&k, 193, MID, HZ60) == 1);
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    TEST_CHECK(frame(&k, 63, MID, HZ60) == -1);
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    TEST_CHECK(frame(&k, MID, 63, HZ60) == -2);
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    TEST_CHECK(frame(&k, MID, 193, HZ60) == 2);
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    /* how far it is pushed beyond that changes nothing: the push's step, then the pause */
    TEST_CHECK(frame(&k, 255, MID, HZ60) == 1 && frame(&k, 255, MID, HZ60) == 0 && frame(&k, 200, MID, HZ60) == 0);
}

/* At 60 Hz: the push's step, 166.7 ms, then 125, 112.5, 100, 87.5, 75 and 62.5 ms,
 * then every 50 ms (the cap); each step on the first frame due (slack 1 ms). */
static void held_it_accelerates_to_the_cap(void)
{
    static const uint64_t gaps[] = {166667, 125000, 112500, 100000, 87500, 75000, 62500, 50000, 50000, 50000};
    Stick k = stick();
    uint64_t t[400], due = 0;
    int n = hold(&k, 10000000u, HZ60, t, 400);
    TEST_CHECK(t[0] == 0);
    for (int i = 1; i < n && i < 400; i++) {
        due += i <= 10 ? gaps[i - 1] : 50000u;
        TEST_CHECK_(t[i] + VJO_STICK_SLACK_US >= due && t[i] < due + HZ60, "step %d at %llu us, due %llu", i,
                    (unsigned long long)t[i], (unsigned long long)due);
    }
    /* 13 steps in the first second, then 20 a second: 193 in 10 s */
    int first = 0;
    for (int i = 0; i < n && i < 400; i++) first += t[i] < 1000000u;
    TEST_CHECK_(first == 13, "%d steps in the first second", first);
    TEST_CHECK_(n == 193, "%d steps in 10 s", n);
    /* the clock's rounding: ten frames of exactly 1/60 s read 166,666 us; the
     * slack makes the tenth the first repeat, as in the Rubi app's lists */
    Stick e = stick();
    uint64_t start = e.now;
    vjo_stick_cursor(&e.s, RIGHT, MID, start);
    int at10 = 0;
    for (int f = 1; f <= 10; f++)
        at10 = vjo_stick_cursor(&e.s, RIGHT, MID, start + (uint64_t)f * 1000000u / 60u);
    TEST_CHECK_(at10 == 1, "the tenth frame of exactly 60 Hz: %d", at10);
}

/* The speed cap, by the clock at any frame rate: held 10 s at 60, 50, 40, 30, 25
 * and 20 Hz, and on irregular frames of 1-60 ms, never more than 21 steps in any
 * second (from any step on); held past the ramp, 20 a second. */
static void the_cap_holds_at_any_frame_rate(void)
{
    static const uint64_t rates[] = {16667, 20000, 25000, 33333, 40000, 50000, 0};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        Stick k = stick();
        uint64_t t[400], start = k.now, at = 0;
        uint32_t seed = 12345;
        int n = 0;
        frame(&k, RIGHT, MID, 0);
        t[n++] = 0;
        while (at < 10000000u) {
            uint64_t dt = rates[r];
            if (!dt) { /* irregular: 1-60 ms (one over 50 ms counts 50) */
                seed = seed * 1103515245u + 12345u;
                dt = 1000u + (seed >> 8) % 59000u;
            }
            at += dt;
            int d = frame(&k, RIGHT, MID, dt);
            if (d) {
                TEST_CHECK(d == 1);
                if (n < 400) t[n] = k.now - start;
                n++;
            }
        }
        int worst = 0;
        for (int i = 0; i < n && i < 400; i++) {
            int in = 0;
            for (int j = i; j < n && j < 400 && t[j] < t[i] + 1000000u; j++) in++;
            if (in > worst) worst = in;
        }
        TEST_CHECK_(worst <= 21, "%llu us frames: %d steps in one second", (unsigned long long)rates[r], worst);
        /* held after the ramp (from 1 s on): 20 a second, less what frames over 50 ms give up */
        int late = 0;
        for (int i = 0; i < n && i < 400; i++) late += t[i] >= 1000000u && t[i] < 10000000u;
        TEST_CHECK_(late <= 181 && late >= (rates[r] ? 178 : 165), "%llu us frames: %d steps from 1 to 10 s",
                    (unsigned long long)rates[r], late);
    }
}

/* A stalled frame (the overlay held up for 2 s) counts one fastest step's time:
 * during the pause it moves nothing, after the ramp one step, not 40; a clock
 * going back counts nothing. */
static void a_stalled_frame_moves_at_most_once(void)
{
    Stick k = stick();
    TEST_CHECK(frame(&k, RIGHT, MID, 0) == 1);
    TEST_CHECK(frame(&k, RIGHT, MID, 2000000u) == 0); /* 50 ms of the 166.7 ms pause */
    TEST_CHECK(frame(&k, RIGHT, MID, 2000000u) == 0); /* 100 ms */
    TEST_CHECK(frame(&k, RIGHT, MID, 2000000u) == 0); /* 150 ms */
    TEST_CHECK(frame(&k, RIGHT, MID, 2000000u) == 1); /* 200 ms: the first repeat */
    for (int i = 0; i < 120; i++) /* 2 s at 60 Hz: past the ramp */
        frame(&k, RIGHT, MID, HZ60);
    int moved = 0;
    for (int i = 0; i < 10; i++)
        moved += frame(&k, RIGHT, MID, 2000000u) != 0;
    TEST_CHECK_(moved == 10, "ten 2 s frames: %d steps", moved);
    k.now -= 100000u;
    TEST_CHECK(vjo_stick_cursor(&k.s, RIGHT, MID, k.now) == 0);
    /* and it took nothing from the next step's time: held past the ramp, one
     * step every three 60 Hz frames before it and after it */
    Stick b = stick();
    uint64_t t[64];
    TEST_CHECK(hold(&b, 2000000u, HZ60, t, 64) > 20);
    int before = 0, after = 0;
    for (int f = 0; f < 30; f++)
        before += frame(&b, RIGHT, MID, HZ60) != 0;
    b.now -= 100000u;
    TEST_CHECK(vjo_stick_cursor(&b.s, RIGHT, MID, b.now) == 0);
    for (int f = 0; f < 30; f++)
        after += frame(&b, RIGHT, MID, HZ60) != 0;
    TEST_CHECK_(before == 10 && after == 10, "30 frames: %d steps before the clock went back, %d after", before,
                after);
    VjoStickCursor fresh;
    memset(&fresh, 0, sizeof(fresh));
    TEST_CHECK(vjo_stick_cursor(&fresh, LEFT, MID, 123456789u) == -1); /* the push whatever the clock read */
}

/* Let go and pushed again, or straight the other way: afresh, the push's step
 * and the pause again. */
static void a_new_push_starts_afresh(void)
{
    Stick k = stick();
    uint64_t t[64];
    TEST_CHECK(hold(&k, 1000000u, HZ60, t, 64) == 13);
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    TEST_CHECK(hold(&k, 300000u, HZ60, t, 64) == 2 && t[1] + VJO_STICK_SLACK_US >= 166667u);
    TEST_CHECK(frame(&k, LEFT, MID, HZ60) == -1); /* straight to the other way */
    int n = 0;
    for (int i = 0; i < 9; i++)
        n += frame(&k, LEFT, MID, HZ60) != 0;
    TEST_CHECK(n == 0 && frame(&k, LEFT, MID, HZ60) == -1); /* the 10th frame: 166.7 ms */
}

/* The farther axis counts; the one held keeps counting while it stays pushed,
 * so a stick rolled from right through up-right to up changes once, afresh. */
static void the_axis_held_stays_until_let_go(void)
{
    Stick k = stick();
    TEST_CHECK(frame(&k, 250, 140, HZ60) == 1);  /* right, a little down */
    TEST_CHECK(frame(&k, 220, 20, HZ60) == 0);   /* rolled up-right, y now farther: still right */
    TEST_CHECK(frame(&k, 200, 0, HZ60) == 0);    /* still right (x 72 out) */
    int n = 0;
    for (int i = 0; i < 9; i++) n += frame(&k, 200, 0, HZ60) == 1;
    TEST_CHECK_(n == 1, "still held right: its repeat (%d)", n);
    TEST_CHECK(frame(&k, 180, 0, HZ60) == -2);   /* x let back inside: up, afresh, its push's step */
    TEST_CHECK(frame(&k, 250, 0, HZ60) == 0);    /* x out again, but up is held */
    TEST_CHECK(frame(&k, MID, 255, HZ60) == 2);  /* straight to down */
    TEST_CHECK(frame(&k, 0, 255, HZ60) == 0);    /* x as far: down is held */
    TEST_CHECK(frame(&k, MID, MID, HZ60) == 0);
    TEST_CHECK(frame(&k, 0, 0, HZ60) == -1);     /* a tie from rest: along the text */
}

/* The overlay opens with the stick pushed (or the region selector gives it
 * back): nothing until it is let go; then a push is a push. */
static void a_latched_stick_waits_to_be_let_go(void)
{
    Stick k = stick();
    vjo_stick_cursor_latch(&k.s);
    for (int i = 0; i < 60; i++)
        TEST_CHECK(frame(&k, RIGHT, MID, HZ60) == 0);
    TEST_CHECK(frame(&k, MID, 190, HZ60) == 0); /* let go (inside the threshold) */
    TEST_CHECK(frame(&k, RIGHT, MID, HZ60) == 1);
    /* latched at rest: the next push is a push */
    Stick r = stick();
    vjo_stick_cursor_latch(&r.s);
    TEST_CHECK(frame(&r, MID, MID, HZ60) == 0 && frame(&r, MID, 0, HZ60) == -2);
}

/* ---- the overlay's frame, replayed over a cursor on positions 0..n-1 ---- */

typedef struct {
    Stick k;
    int cursor, n;
    int worker;                  /* the worker's cursor (VjoView.cursor_byte): the last post it accepted */
    int refuse;                  /* the worker refuses posts (Cancel's command pending, or closing) */
    int immediate, deferred;     /* lookups posted: the d-pad's at once, the stick's once it rests */
    uint64_t deferred_at;        /* the last deferred lookup's time */
    int deferred_cursor;         /* and its cursor */
    uint64_t moved_at;           /* the stick's last move */
} Overlay;

static Overlay overlay(int n, uint32_t rest_us)
{
    Overlay o;
    memset(&o, 0, sizeof(o));
    o.k = stick();
    o.n = n;
    o.k.s.rest_us = rest_us; /* render() copies stick_lookup_ms there */
    o.deferred_cursor = -1;
    return o;
}

/* vjo_post_cursor: accepted, the worker's cursor is the overlay's at once */
static void ov_post(Overlay *o)
{
    if (!o->refuse)
        o->worker = o->cursor;
}

/* One frame as shell/overlay.cpp input_overlay runs it, with its helpers: the
 * stick's state first; an arrow pressed (dpad -1/1: Left/Right, here without
 * the row's wrap) takes the frame (vjo_stick_cursor_step) and posts at once,
 * dropping the stick's due word (post_cursor_now); else the stick's step, an
 * arrow's (its left/right a direction), without a post; render(0) after a
 * move (vjo_stick_cursor_shown); then the stick's lookup if the cursor rests,
 * posted. Up/down: a single row, nowhere to go. */
static void ov_frame(Overlay *o, uint8_t lx, uint8_t ly, int dpad, uint64_t dt)
{
    o->k.now += dt;
    int direction = dpad, vertical = 0, to = o->cursor;
    int stick = vjo_stick_cursor_step(vjo_stick_cursor(&o->k.s, lx, ly, o->k.now), &direction, &vertical);
    if (direction)
        to = o->cursor + direction;
    if (to < 0 || to >= o->n)
        to = o->cursor; /* the text's ends: the stick never goes round them */
    if (stick)
        vjo_stick_cursor_moved(&o->k.s, to != o->cursor, o->k.now);
    if (to != o->cursor) {
        o->cursor = to;
        if (stick) {
            o->moved_at = o->k.now;
        } else {
            o->k.s.due = 0; /* post_cursor_now */
            ov_post(o);
            o->immediate++;
        }
        vjo_stick_cursor_shown(&o->k.s, o->worker == o->cursor); /* render(0) */
    }
    if (vjo_stick_cursor_lookup(&o->k.s, o->k.now)) {
        o->deferred++;
        o->deferred_at = o->k.now;
        o->deferred_cursor = o->cursor;
        ov_post(o);
    }
}

/* frames of dt for us with the stick at (lx, ly) */
static void ov_run(Overlay *o, uint8_t lx, uint8_t ly, uint64_t us, uint64_t dt)
{
    for (uint64_t at = 0; at < us; at += dt)
        ov_frame(o, lx, ly, 0, dt);
}

/* A flick (one frame pushed): one step, and its lookup once, on the first frame
 * the cursor has rested 200 ms, not before; at 60 and 30 Hz. */
static void a_flick_looks_up_once_after_the_rest(void)
{
    static const uint64_t rates[] = {HZ60, HZ30};
    for (int r = 0; r < 2; r++) {
        Overlay o = overlay(40, REST);
        ov_frame(&o, RIGHT, MID, 0, rates[r]);
        TEST_CHECK(o.cursor == 1 && o.k.s.due);
        uint64_t moved = o.moved_at;
        while (o.k.now + rates[r] + VJO_STICK_SLACK_US < moved + REST) { /* the frames before the rest, its slack */
            ov_frame(&o, MID, MID, 0, rates[r]);
            TEST_CHECK(o.deferred == 0);
        }
        ov_run(&o, MID, MID, 3 * rates[r], rates[r]);
        TEST_CHECK(o.deferred == 1 && o.deferred_cursor == 1);
        TEST_CHECK_(o.deferred_at + VJO_STICK_SLACK_US >= moved + REST && o.deferred_at < moved + REST + rates[r],
                    "%llu us frames: looked up %llu us after the move", (unsigned long long)rates[r],
                    (unsigned long long)(o.deferred_at - moved));
        ov_run(&o, MID, MID, 2000000u, rates[r]);
        TEST_CHECK(o.deferred == 1 && o.immediate == 0); /* once */
        TEST_CHECK(o.k.s.moves == 1 && o.k.s.lookups == 1);
    }
}

/* Held across a long line: the cursor moves 13 times in the first second and
 * nothing is looked up while it moves; let go, one lookup 200 ms after its last
 * move, at the cursor where it stopped. Without the deferral each of those
 * steps would have been a lookup, as each d-pad step is. */
static void held_across_a_line_looks_up_only_where_it_stops(void)
{
    Overlay o = overlay(100, REST);
    ov_run(&o, RIGHT, MID, 1000000u, HZ60);
    TEST_CHECK_(o.cursor == 13 && o.deferred == 0, "cursor %d, %d lookups", o.cursor, o.deferred);
    ov_run(&o, RIGHT, MID, 1000000u, HZ60);
    TEST_CHECK_(o.cursor == 33 && o.deferred == 0, "cursor %d, %d lookups", o.cursor, o.deferred);
    uint64_t moved = o.moved_at;
    ov_run(&o, MID, MID, 1000000u, HZ60);
    TEST_CHECK(o.deferred == 1 && o.deferred_cursor == 33);
    TEST_CHECK(o.deferred_at + VJO_STICK_SLACK_US >= moved + REST && o.deferred_at < moved + REST + HZ60);
    TEST_CHECK(o.k.s.moves == 33 && o.k.s.lookups == 1);
}

/* Held against the text's end: the cursor stops at the last position (never
 * round to the first), and its word is looked up 200 ms after the last move
 * although the stick is still held; once. */
static void held_against_the_end_looks_up_while_held(void)
{
    Overlay o = overlay(5, REST);
    ov_run(&o, RIGHT, MID, 3000000u, HZ60);
    TEST_CHECK(o.cursor == 4 && o.deferred == 1 && o.deferred_cursor == 4);
    TEST_CHECK_(o.deferred_at + VJO_STICK_SLACK_US >= o.moved_at + REST && o.deferred_at < o.moved_at + REST + 50000u + HZ60,
                "looked up %llu us after the last move", (unsigned long long)(o.deferred_at - o.moved_at));
    ov_run(&o, RIGHT, MID, 3000000u, HZ60);
    TEST_CHECK(o.deferred == 1 && o.k.s.moves == 4);
    /* and back to the start: the same there */
    ov_run(&o, LEFT, MID, 3000000u, HZ60);
    TEST_CHECK(o.cursor == 0 && o.deferred == 2 && o.deferred_cursor == 0);
}

/* Flicks closer together than the rest: one lookup, 200 ms after the last. */
static void quick_flicks_look_up_once(void)
{
    Overlay o = overlay(40, REST);
    for (int f = 0; f < 5; f++) {
        ov_frame(&o, RIGHT, MID, 0, HZ60);
        ov_run(&o, MID, MID, 150000u, HZ60);
    }
    TEST_CHECK(o.cursor == 5 && o.deferred == 0);
    uint64_t moved = o.moved_at;
    ov_run(&o, MID, MID, 500000u, HZ60);
    TEST_CHECK(o.deferred == 1 && o.deferred_cursor == 5 && o.deferred_at + VJO_STICK_SLACK_US >= moved + REST &&
               o.deferred_at < moved + REST + HZ60);
}

/* The owner's range, 150-250 ms, and 0: with 0 the word is looked up as soon as
 * the stick stops driving the cursor (let go), never while it moves. */
static void the_rest_is_the_setting(void)
{
    static const uint32_t rests[] = {150000u, 250000u, 1000000u};
    for (int i = 0; i < 3; i++) {
        Overlay o = overlay(40, rests[i]);
        ov_frame(&o, RIGHT, MID, 0, HZ60);
        ov_run(&o, MID, MID, 1500000u, HZ60);
        TEST_CHECK_(o.deferred == 1 && o.deferred_at + VJO_STICK_SLACK_US >= o.moved_at + rests[i] &&
                    o.deferred_at < o.moved_at + rests[i] + HZ60, "rest %u us: %d lookup(s), %llu us after", rests[i],
                    o.deferred, (unsigned long long)(o.deferred_at - o.moved_at));
    }
    Overlay z = overlay(40, 0);
    ov_run(&z, RIGHT, MID, 1000000u, HZ60);
    TEST_CHECK(z.cursor == 13 && z.deferred == 0);
    ov_frame(&z, MID, MID, 0, HZ60);
    TEST_CHECK(z.deferred == 1 && z.deferred_cursor == 13);
    /* a flick with 0: looked up in the frame the stick is let go */
    ov_frame(&z, LEFT, MID, 0, HZ60);
    TEST_CHECK(z.cursor == 12 && z.deferred == 1);
    ov_frame(&z, MID, MID, 0, HZ60);
    TEST_CHECK(z.deferred == 2 && z.deferred_cursor == 12);
}

/* Held, the cursor is moving, not resting, even through the 166.7 ms pause
 * after the push, which is longer than the owner's shortest rest, 150 ms: no
 * lookup until it is let go (or stopped by an end). */
static void a_held_stick_is_not_a_rest(void)
{
    Overlay o = overlay(100, 150000u);
    ov_run(&o, RIGHT, MID, 160000u, HZ60); /* the push, into the pause */
    TEST_CHECK(o.cursor == 1 && o.deferred == 0);
    ov_run(&o, RIGHT, MID, 1840000u, HZ60);
    TEST_CHECK_(o.cursor > 20 && o.deferred == 0, "cursor %d, %d lookups", o.cursor, o.deferred);
    ov_run(&o, MID, MID, 500000u, HZ60);
    TEST_CHECK(o.deferred == 1 && o.deferred_cursor == o.cursor && o.deferred_at + VJO_STICK_SLACK_US >= o.moved_at + 150000u);
}

/* The d-pad keeps its step and its lookup at once, and drops the stick's due
 * one: the stick's never comes, until the stick moves the cursor again. */
static void the_dpad_looks_up_at_once(void)
{
    Overlay o = overlay(40, REST);
    ov_frame(&o, RIGHT, MID, 0, HZ60); /* the stick: cursor 1, due */
    ov_run(&o, MID, MID, 100000u, HZ60);
    ov_frame(&o, MID, MID, 1, HZ60);   /* Right: cursor 2, looked up in this frame */
    TEST_CHECK(o.cursor == 2 && o.immediate == 1 && !o.k.s.due);
    ov_run(&o, MID, MID, 1000000u, HZ60);
    TEST_CHECK(o.deferred == 0 && o.immediate == 1);
    for (int i = 0; i < 4; i++) {      /* four presses, four lookups at once, none deferred */
        ov_frame(&o, MID, MID, i < 2 ? 1 : -1, HZ60);
        ov_frame(&o, MID, MID, 0, HZ60);
    }
    TEST_CHECK(o.cursor == 2 && o.immediate == 5 && o.deferred == 0);
    ov_frame(&o, LEFT, MID, 0, HZ60);  /* the stick again: due again */
    ov_run(&o, MID, MID, 500000u, HZ60);
    TEST_CHECK(o.cursor == 1 && o.deferred == 1 && o.deferred_cursor == 1);
    /* a d-pad press during a held stick takes that frame (the stick's step due in
     * it is dropped) and looks up at once; the stick's word is looked up once it rests */
    ov_run(&o, RIGHT, MID, 400000u, HZ60);
    for (int f = 0; f < 60; f++) { /* up to the frame in which the stick's next step is due */
        Overlay probe = o;
        ov_frame(&probe, RIGHT, MID, 0, HZ60);
        if (probe.cursor != o.cursor)
            break;
        ov_frame(&o, RIGHT, MID, 0, HZ60);
    }
    int before = o.cursor;
    ov_frame(&o, RIGHT, MID, 1, HZ60);
    TEST_CHECK_(o.immediate == 6 && o.deferred == 1 && o.cursor == before + 1, "cursor %d -> %d, %d at once",
                before, o.cursor, o.immediate);
    ov_run(&o, RIGHT, MID, 200000u, HZ60);
    ov_run(&o, MID, MID, 500000u, HZ60);
    TEST_CHECK(o.deferred == 2 && o.deferred_cursor == o.cursor);
}

/* Up/down: a step that cannot leave the row (one row) moves nothing and makes
 * nothing due; a due word is looked up while the stick is held against it. */
static void a_blocked_step_is_a_rest(void)
{
    Overlay o = overlay(40, REST);
    ov_run(&o, MID, 0, 1000000u, HZ60);
    TEST_CHECK(o.cursor == 0 && o.deferred == 0 && !o.k.s.due && o.k.s.moves == 0);
    ov_frame(&o, RIGHT, MID, 0, HZ60);
    ov_frame(&o, MID, MID, 0, HZ60);
    ov_run(&o, MID, 0, 1000000u, HZ60); /* up, held: the cursor rests */
    TEST_CHECK(o.deferred == 1 && o.deferred_cursor == 1);
}

/* The clock's low 32 bits wrap every 71.6 minutes of SceShell's process time,
 * which can fall in an open overlay: held across the wrap, the steps keep their
 * times exactly as far from it, and a rest across it is looked up on time. */
static void the_clocks_wrap_changes_nothing(void)
{
    static const uint64_t starts[] = {5000000u, 0xFFFFFFFFull - 3000000u, 0x1FFFFFFFFull - 166000u,
                                      0x2FFFFFFFFull - 49999u};
    uint64_t ref[256];
    int nref = 0;
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        Stick k = stick();
        k.now = starts[i];
        uint64_t t[256];
        int n = hold(&k, 6000000u, HZ60, t, 256);
        if (!i) {
            memcpy(ref, t, sizeof(ref));
            nref = n;
        }
        TEST_CHECK_(n == nref, "from %llx: %d steps in 6 s, %d from 5 s", (unsigned long long)starts[i], n, nref);
        for (int s = 0; s < n && s < 256; s++)
            TEST_CHECK_(t[s] == ref[s], "from %llx: step %d at %llu us, %llu from 5 s", (unsigned long long)starts[i], s,
                        (unsigned long long)t[s], (unsigned long long)ref[s]);
        /* let go across the wrap: looked up on the first frame 200 ms after the last move */
        Overlay o = overlay(400, REST);
        o.k.now = (starts[i] | 0xFFFFFFFFull) - 100000u; /* the wrap 100 ms after the move */
        ov_frame(&o, RIGHT, MID, 0, 0);
        TEST_CHECK(o.cursor == 1);
        ov_run(&o, MID, MID, 400000u, HZ60);
        TEST_CHECK_(o.deferred == 1 && o.deferred_at + VJO_STICK_SLACK_US >= o.moved_at + REST && o.deferred_at < o.moved_at + REST + HZ60,
                    "from %llx: %d lookup(s), %lld us after the move", (unsigned long long)starts[i], o.deferred,
                    (long long)(o.deferred_at - o.moved_at));
    }
}

/* The rest is counted with the steps' 1 ms slack for the microsecond clock
 * (#156's audit N3): six frames at exactly 30 Hz read 199,998 us, and are a
 * 200 ms rest. A frame 1,001 us short of it is not one. */
static void the_rest_has_the_steps_slack(void)
{
    VjoStickCursor s;
    memset(&s, 0, sizeof(s));
    s.rest_us = REST;
    uint64_t t = 7000000u;
    vjo_stick_cursor_moved(&s, 1, t);
    TEST_CHECK(!vjo_stick_cursor_lookup(&s, t + REST - VJO_STICK_SLACK_US - 1));
    TEST_CHECK(s.due && vjo_stick_cursor_lookup(&s, t + REST - VJO_STICK_SLACK_US) && !s.due);
    /* at exactly 30 Hz: the sixth frame after the move (1,000,000 / 30 us each, rounded down) */
    Overlay o = overlay(40, REST);
    uint64_t start = o.k.now = 9000000u;
    ov_frame(&o, RIGHT, MID, 0, 0);
    TEST_CHECK(o.cursor == 1);
    for (int f = 1; f <= 6; f++) {
        o.k.now = start + (uint64_t)f * 1000000u / 30u;
        ov_frame(&o, MID, MID, 0, 0);
        TEST_CHECK_(o.deferred == (f == 6), "frame %d at 30 Hz: %d lookup(s)", f, o.deferred);
    }
}

/* Held into the text's end (blocked: its word is looked up while held there),
 * then rolled straight to the other way and held: the cursor moves again, and
 * nothing is looked up while it does, whatever the rest (0 included), until it
 * stops at the other end. */
static void blocked_then_moving_again_is_no_rest(void)
{
    static const uint32_t rests[] = {0u, 50000u, 100000u, 150000u, REST};
    for (unsigned i = 0; i < sizeof(rests) / sizeof(rests[0]); i++) {
        Overlay o = overlay(6, rests[i]);
        ov_run(&o, RIGHT, MID, 3000000u, HZ60);
        TEST_CHECK(o.cursor == 5 && o.deferred == 1 && o.deferred_cursor == 5);
        int moving = 1;
        for (int f = 0; f < 180; f++) { /* 3 s held left: back to 0, then blocked there */
            ov_frame(&o, LEFT, MID, 0, HZ60);
            if (o.deferred > 1 && moving) {
                TEST_CHECK_(o.deferred_cursor == 0, "rest %u us: looked up at %d while moving left", rests[i],
                            o.deferred_cursor);
                moving = 0;
            }
        }
        TEST_CHECK_(o.cursor == 0 && o.deferred == 2 && o.deferred_cursor == 0, "rest %u us: %d lookups, the last at %d",
                    rests[i], o.deferred, o.deferred_cursor);
    }
}

/* The frame's decision (vjo_stick_cursor_step): an arrow pressed takes it, the
 * stick's step dropped; else the stick's four directions are the arrows': its
 * up/down a row step (vertical) and its left/right a Left/Right (direction). */
static void an_arrow_takes_the_frame(void)
{
    static const int sticks[] = {0, 1, -1, 2, -2};
    for (unsigned i = 0; i < sizeof(sticks) / sizeof(sticks[0]); i++) {
        int st = sticks[i];
        for (int arrow = -1; arrow <= 1; arrow++) {
            int direction = arrow, vertical = 0;
            TEST_CHECK(vjo_stick_cursor_step(st, &direction, &vertical) == (arrow ? 0 : st));
            TEST_CHECK(vertical == (!arrow && (st == 2 || st == -2) ? st / 2 : 0));
            TEST_CHECK(direction == (arrow ? arrow : st == 1 || st == -1 ? st : 0));
            if (!arrow)
                continue;
            direction = 0;
            vertical = arrow; /* Up/Down pressed */
            TEST_CHECK(vjo_stick_cursor_step(st, &direction, &vertical) == 0 && vertical == arrow && !direction);
        }
    }
}

/* The stick away from the shown word and back to it before the rest: nothing
 * to look up (vjo_stick_cursor_shown); the word, its dictionary page included,
 * stays as it is (#156's audit N2). A word the worker has not shown is looked
 * up as before. */
static void back_on_the_shown_word_is_not_looked_up_again(void)
{
    Overlay o = overlay(40, REST);
    ov_frame(&o, MID, MID, 1, HZ60); /* Right: 1, looked up at once */
    TEST_CHECK(o.worker == 1 && o.immediate == 1);
    ov_frame(&o, RIGHT, MID, 0, HZ60);
    TEST_CHECK(o.cursor == 2 && o.k.s.due);
    ov_frame(&o, MID, MID, 0, HZ60);
    ov_frame(&o, LEFT, MID, 0, HZ60);
    TEST_CHECK(o.cursor == 1 && !o.k.s.due);
    ov_run(&o, MID, MID, 1000000u, HZ60);
    TEST_CHECK(o.deferred == 0 && o.worker == 1);
    ov_frame(&o, LEFT, MID, 0, HZ60); /* 0: not the worker's */
    ov_run(&o, MID, MID, 1000000u, HZ60);
    TEST_CHECK(o.deferred == 1 && o.worker == 0 && o.deferred_cursor == 0);
}

/* New text (a quiet refresh replaced it, or an open took the worker's
 * cursor): the due word goes with the old text, and a stick still held moves
 * nothing until it is let go (#156's audit N10). */
static void new_text_drops_the_due_word_and_latches(void)
{
    Overlay o = overlay(40, REST);
    ov_run(&o, RIGHT, MID, 500000u, HZ60);
    TEST_CHECK(o.cursor > 3 && o.k.s.due);
    vjo_stick_cursor_new_text(&o.k.s); /* render: new_text */
    o.cursor = o.worker = 0;           /* the new text's first word, the worker's */
    ov_run(&o, RIGHT, MID, 1000000u, HZ60);
    TEST_CHECK(o.cursor == 0 && o.deferred == 0 && !o.k.s.due);
    ov_frame(&o, MID, MID, 0, HZ60);
    ov_frame(&o, RIGHT, MID, 0, HZ60);
    TEST_CHECK(o.cursor == 1 && o.k.s.due);
}

/* The close (vjo_stick_cursor_kept): the next open keeps the overlay's cursor
 * only when it is the worker's with nothing due. #156's audit B1: Cancel posts
 * VJO_CMD_CLOSED, which the control thread handles up to 50 ms later; a rest
 * that ends meanwhile looks the word up, the worker refuses the post, and
 * nothing is due any more: the cursors differ, so the next open takes the
 * worker's (whose word it shows) instead of keeping one no lookup will come for. */
static void a_refused_post_is_not_kept_at_the_close(void)
{
    /* the B1 sequence: a flick, Cancel 11 frames (183 ms) later, the rest ends before the close */
    Overlay o = overlay(40, REST);
    ov_frame(&o, RIGHT, MID, 0, HZ60);
    ov_run(&o, MID, MID, 180000u, HZ60);
    TEST_CHECK(o.k.s.due && o.deferred == 0);
    o.refuse = 1; /* VJO_CMD_CLOSED pending */
    ov_run(&o, MID, MID, 50000u, HZ60);
    TEST_CHECK(o.deferred == 1 && !o.k.s.due && o.worker == 0 && o.cursor == 1);
    TEST_CHECK(!vjo_stick_cursor_kept(&o.k.s, (uint32_t)o.cursor, (uint32_t)o.worker));
    /* the same with the post accepted: kept */
    Overlay a = overlay(40, REST);
    ov_frame(&a, RIGHT, MID, 0, HZ60);
    ov_run(&a, MID, MID, 300000u, HZ60);
    TEST_CHECK(a.deferred == 1 && a.worker == 1);
    TEST_CHECK(vjo_stick_cursor_kept(&a.k.s, (uint32_t)a.cursor, (uint32_t)a.worker));
    /* closed with the word due: not kept, whatever the worker's cursor */
    Overlay d = overlay(40, REST);
    ov_frame(&d, RIGHT, MID, 0, HZ60);
    TEST_CHECK(d.k.s.due && !vjo_stick_cursor_kept(&d.k.s, (uint32_t)d.cursor, (uint32_t)d.worker));
    TEST_CHECK(!vjo_stick_cursor_kept(&d.k.s, 1, 1));
    /* a d-pad press refused (main's window: pressed just after Cancel): not kept either */
    Overlay p = overlay(40, REST);
    p.refuse = 1;
    ov_frame(&p, MID, MID, 1, HZ60);
    TEST_CHECK(p.cursor == 1 && p.worker == 0 && !vjo_stick_cursor_kept(&p.k.s, 1, 0));
}

/* ---- the stick's step along the text ---- */

/* The stick's left/right as the overlay takes it (owner, 2026-10-16, item 3):
 * an arrow's for vjo_text_row_key with `stick` set, which never goes round
 * the text's ends; here on a fresh path (no Right to retrace: Left one
 * character back) and without a layout, which a stick's Left/Right never
 * asks for. Nothing goes into the arrows' counts. */
static size_t read_step(const char *s, size_t len, size_t cursor, size_t matched_end, int direction, int jp)
{
    VjoRowNav nav;
    size_t out = cursor;
    memset(&nav, 0, sizeof(nav));
    vjo_text_row_key(&nav, s, len, NULL, cursor, matched_end, direction, 0, jp, 1, &out);
    TEST_CHECK(!nav.row_moves && !nav.wraps && !nav.no_layout);
    return out;
}

/* The selectable characters' first bytes in reading order. */
static int selectable(const char *s, int japanese_only, size_t *at, int cap)
{
    size_t i = 0, len = strlen(s);
    int n = 0;
    while (i < len) {
        size_t b = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (cp != '\r' && vjo_text_selectable(cp, i - b, japanese_only) && n < cap)
            at[n++] = b;
    }
    return n;
}

static void read_steps_in_reading_order(void)
{
    /* two lines (a hard break) and a third after a space; Latin and punctuation between */
    const char *s = "猫が好き。\nRPGの話 です、ね\r\n𠮷野家";
    size_t len = strlen(s), at[64];
    for (int jp = 0; jp < 2; jp++) {
        int n = selectable(s, jp, at, 64);
        TEST_ASSERT(n > 6);
        /* Right visits each selectable character once, in order, across the lines, and stays at the last */
        size_t c = at[0];
        for (int i = 1; i < n; i++) {
            c = read_step(s, len, c, 0, 1, jp);
            TEST_CHECK_(c == at[i], "japanese_only %d: step %d at %zu, want %zu", jp, i, c, at[i]);
        }
        TEST_CHECK(read_step(s, len, c, 0, 1, jp) == c); /* never round to the first */
        for (int i = n - 2; i >= 0; i--) {
            c = read_step(s, len, c, 0, -1, jp);
            TEST_CHECK(c == at[i]);
        }
        TEST_CHECK(read_step(s, len, c, 0, -1, jp) == c); /* nor round to the last */
    }
    /* Japanese-only skips the Latin word, the punctuation and the spaces */
    TEST_CHECK(read_step(s, len, 9, 0, 1, 1) == 19);  /* き -> の (after "。\nRPG") */
    TEST_CHECK(read_step(s, len, 9, 0, 1, 0) == 12);  /* off: き -> 。 */
    TEST_CHECK(read_step(s, len, 12, 0, 1, 0) == 16); /* 。 -> R: never the line break */
    TEST_CHECK(read_step(s, len, 22, 0, 1, 0) == 26); /* 話 -> で: never the space */
    TEST_CHECK(read_step(s, len, 35, 0, 1, 1) == 40); /* ね -> 𠮷: over CR LF */
    TEST_CHECK(read_step(s, len, 40, 0, -1, 1) == 35);
}

/* Whole word: the matched word is skipped on the step from its start (as the
 * d-pad's Right), a bad end is one character; Left never uses a match. */
static void read_steps_skip_the_matched_word(void)
{
    /* 今 0, 日 3, は 6, 天 9, 気 12, が 15, い 18, い 21, 。 24, 散 27, 歩 30 */
    const char *s = "今日は天気がいい。散歩";
    size_t len = strlen(s);
    TEST_CHECK(len == 33);
    TEST_CHECK(read_step(s, len, 0, 6, 1, 1) == 6);     /* 今日 -> は */
    TEST_CHECK(read_step(s, len, 9, 15, 1, 1) == 15);   /* 天気 -> が */
    TEST_CHECK(read_step(s, len, 15, 24, 1, 1) == 27);  /* がいい -> 散 (the 。 skipped) */
    TEST_CHECK(read_step(s, len, 15, 24, 1, 0) == 24);  /* off: -> 。 */
    TEST_CHECK(read_step(s, len, 0, 4, 1, 1) == 3);     /* not a character boundary: one character */
    TEST_CHECK(read_step(s, len, 0, 0, 1, 1) == 3);     /* no match: one character */
    TEST_CHECK(read_step(s, len, 30, len, 1, 1) == 30); /* the match reaches the end: stays */
    TEST_CHECK(read_step(s, len, 27, len, 1, 1) == 27);
    TEST_CHECK(read_step(s, len, 9, 15, -1, 1) == 6);   /* Left: one character back */
    TEST_CHECK(read_step(s, len, 0, 0, -1, 1) == 0);
    TEST_CHECK(read_step(s, len, 3, 0, 0, 1) == 3);     /* no direction */
    TEST_CHECK(read_step("", 0, 0, 0, 1, 1) == 0);
    TEST_CHECK(read_step("ABC", 3, 3, 0, 1, 1) == 3);   /* no Japanese: the cursor is len */
    TEST_CHECK(read_step("ABC", 3, 3, 0, -1, 1) == 3);
}

/* Every step from every character of generated texts against the rule restated:
 * the next or previous selectable character in byte order, else the cursor. */
static void read_steps_against_the_rule(void)
{
    static const char *const parts[] = {"あ", "漢", "ー", "A", " ", "。", "\n", "\r", "、", "𠮷", "ｶ", "7", "　"};
    uint32_t seed = 7;
    for (int t = 0; t < 200; t++) {
        char s[256] = "";
        size_t at[128];
        for (int k = 0; k < 40; k++) {
            seed = seed * 1103515245u + 12345u;
            strcat(s, parts[(seed >> 8) % (sizeof(parts) / sizeof(parts[0]))]);
        }
        size_t len = strlen(s);
        for (int jp = 0; jp < 2; jp++) {
            int n = selectable(s, jp, at, 128);
            for (int i = 0; i < n; i++) {
                size_t right = i + 1 < n ? at[i + 1] : at[i], left = i ? at[i - 1] : at[i];
                TEST_CHECK(read_step(s, len, at[i], 0, 1, jp) == right);
                TEST_CHECK(read_step(s, len, at[i], 0, -1, jp) == left);
            }
        }
    }
}

/* The overlay's stick over a real step function: held right through a text it
 * visits each Japanese letter once in order, stops at the last, and is looked up
 * once there, 200 ms after it arrived, although the stick is still held. */
static void held_through_a_text(void)
{
    const char *s = "猫が好きです。今日は天気がいいので、散歩に行きました。";
    size_t len = strlen(s), at[64];
    int n = selectable(s, 1, at, 64), visited = 1, lookups = 0;
    Stick k = stick();
    VjoRowNav nav; /* the overlay's s_nav: one path for the d-pad and the stick */
    size_t c = at[0], last = c;
    memset(&nav, 0, sizeof(nav));
    k.s.rest_us = REST;
    uint64_t moved = 0, looked = 0;
    for (int f = 0; f < 600; f++) { /* 10 s at 60 Hz */
        k.now += HZ60;
        int direction = 0, vertical = 0;
        int dir = vjo_stick_cursor_step(vjo_stick_cursor(&k.s, RIGHT, MID, k.now), &direction, &vertical);
        if (dir) {
            size_t to = c;
            TEST_CHECK(direction == dir && !vertical);
            vjo_text_row_key(&nav, s, len, NULL, c, 0, direction, vertical, 1, 1, &to);
            vjo_stick_cursor_moved(&k.s, to != c, k.now);
            if (to != c) {
                TEST_CHECK(visited < n && to == at[visited]);
                visited++;
                moved = k.now;
            }
            c = to;
        }
        if (vjo_stick_cursor_lookup(&k.s, k.now)) {
            lookups++;
            looked = k.now;
            last = c;
        }
    }
    TEST_CHECK_(visited == n && c == at[n - 1], "%d of %d letters", visited, n);
    TEST_CHECK(lookups == 1 && last == at[n - 1] && looked + VJO_STICK_SLACK_US >= moved + REST && looked < moved + REST + 50000u + HZ60);
}

TEST_LIST = {
    {"the_threshold_is_the_lists", the_threshold_is_the_lists},
    {"held_it_accelerates_to_the_cap", held_it_accelerates_to_the_cap},
    {"the_cap_holds_at_any_frame_rate", the_cap_holds_at_any_frame_rate},
    {"a_stalled_frame_moves_at_most_once", a_stalled_frame_moves_at_most_once},
    {"a_new_push_starts_afresh", a_new_push_starts_afresh},
    {"the_axis_held_stays_until_let_go", the_axis_held_stays_until_let_go},
    {"a_latched_stick_waits_to_be_let_go", a_latched_stick_waits_to_be_let_go},
    {"a_flick_looks_up_once_after_the_rest", a_flick_looks_up_once_after_the_rest},
    {"held_across_a_line_looks_up_only_where_it_stops", held_across_a_line_looks_up_only_where_it_stops},
    {"held_against_the_end_looks_up_while_held", held_against_the_end_looks_up_while_held},
    {"quick_flicks_look_up_once", quick_flicks_look_up_once},
    {"the_rest_is_the_setting", the_rest_is_the_setting},
    {"a_held_stick_is_not_a_rest", a_held_stick_is_not_a_rest},
    {"the_dpad_looks_up_at_once", the_dpad_looks_up_at_once},
    {"a_blocked_step_is_a_rest", a_blocked_step_is_a_rest},
    {"the_clocks_wrap_changes_nothing", the_clocks_wrap_changes_nothing},
    {"the_rest_has_the_steps_slack", the_rest_has_the_steps_slack},
    {"blocked_then_moving_again_is_no_rest", blocked_then_moving_again_is_no_rest},
    {"an_arrow_takes_the_frame", an_arrow_takes_the_frame},
    {"back_on_the_shown_word_is_not_looked_up_again", back_on_the_shown_word_is_not_looked_up_again},
    {"new_text_drops_the_due_word_and_latches", new_text_drops_the_due_word_and_latches},
    {"a_refused_post_is_not_kept_at_the_close", a_refused_post_is_not_kept_at_the_close},
    {"read_steps_in_reading_order", read_steps_in_reading_order},
    {"read_steps_skip_the_matched_word", read_steps_skip_the_matched_word},
    {"read_steps_against_the_rule", read_steps_against_the_rule},
    {"held_through_a_text", held_through_a_text},
    {NULL, NULL}
};
