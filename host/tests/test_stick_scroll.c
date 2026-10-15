/* The left stick's list scrolling (core/stick_scroll.h) against VitaShell's
 * readPad, by the clock: at 60 Hz the push's row, nine quiet frames, then a row
 * every frame, as VitaShell counts them; at 30 Hz (the Overlay app's likely
 * rate as a system application) the same 166.7 ms pause and 60 rows a second,
 * two rows a frame; and the same at 20, 40 and 50 Hz. */
#include "acutest.h"
#include "stick_scroll.h"

#define HZ60 16667u /* the frame interval, us */
#define HZ30 33333u

/* A stick and its clock (it starts away from 0, as the console's does). */
typedef struct {
    VjoStickScroll s;
    uint64_t now;
} Rig;
static Rig rig(void)
{
    Rig r = {{0}, 5000000u};
    return r;
}
/* One frame frame_us after the last at ly: the rows moved. */
static int frame(Rig *r, uint8_t ly, uint64_t frame_us)
{
    r->now += frame_us;
    return vjo_stick_scroll(&r->s, ly, r->now);
}
/* Rows moved over n frames; *first: the frame (1-based) of the first move. */
static int moves(Rig *r, uint8_t ly, int n, uint64_t frame_us, int *first)
{
    int total = 0;
    *first = 0;
    for (int f = 1; f <= n; f++) {
        int m = frame(r, ly, frame_us);
        if (m && !*first) *first = f;
        total += m;
    }
    return total;
}

static void threshold(void)
{
    Rig r = rig();
    int first;
    /* 64 from the centre is still the centre; 65 is a push */
    TEST_CHECK(moves(&r, 64, 30, HZ60, &first) == 0 && moves(&r, 192, 30, HZ60, &first) == 0);
    TEST_CHECK(frame(&r, 128, HZ60) == 0);
    TEST_CHECK(frame(&r, 63, HZ60) == -1);
    TEST_CHECK(frame(&r, 128, HZ60) == 0);
    TEST_CHECK(frame(&r, 193, HZ60) == 1);
    TEST_CHECK(frame(&r, 128, HZ60) == 0);
    /* how far it is pushed changes nothing */
    TEST_CHECK(frame(&r, 0, HZ60) == -1 && frame(&r, 0, HZ60) == 0);
}

/* At 60 Hz, VitaShell's hold2 frame for frame: a row on the push (frame 1),
 * none on frames 2-10, then one on every frame from 11 on. */
static void held_at_60_hz_moves_like_vitashell(void)
{
    Rig r = rig();
    int fired[40];
    for (int f = 0; f < 40; f++)
        fired[f] = frame(&r, 255, HZ60);
    TEST_CHECK(fired[0] == 1);
    for (int f = 1; f < 10; f++)
        TEST_CHECK_(fired[f] == 0, "frame %d: %d", f + 1, fired[f]);
    for (int f = 10; f < 40; f++)
        TEST_CHECK_(fired[f] == 1, "frame %d: %d", f + 1, fired[f]);
    /* released and pushed again: the same again */
    frame(&r, 128, HZ60);
    int first;
    TEST_CHECK(moves(&r, 255, 10, HZ60, &first) == 1 && first == 1);
    TEST_CHECK(moves(&r, 255, 5, HZ60, &first) == 5 && first == 1);
    /* the clock's rounding: ten frames of exactly 1/60 s are 166,666 us here */
    Rig e = rig();
    uint64_t start = e.now;
    vjo_stick_scroll(&e.s, 255, start);
    int at10 = 0;
    for (int f = 1; f <= 10; f++)
        at10 = vjo_stick_scroll(&e.s, 255, start + (uint64_t)f * 1000000u / 60u);
    TEST_CHECK_(at10 == 1, "the eleventh frame of exactly 60 Hz: %d", at10);
}

/* At 30 Hz the same time: the push's row, quiet frames 2-5, a row at frame 6
 * (166.7 ms), then two rows a frame. */
static void held_at_30_hz_keeps_the_time(void)
{
    Rig r = rig();
    int fired[30];
    for (int f = 0; f < 30; f++)
        fired[f] = frame(&r, 255, HZ30);
    TEST_CHECK(fired[0] == 1);
    for (int f = 1; f < 5; f++)
        TEST_CHECK_(fired[f] == 0, "frame %d: %d", f + 1, fired[f]);
    TEST_CHECK_(fired[5] == 1, "frame 6 (166.7 ms): %d", fired[5]);
    for (int f = 6; f < 30; f++)
        TEST_CHECK_(fired[f] == 2, "frame %d: %d", f + 1, fired[f]);
    /* up: the same, negative */
    Rig u = rig();
    int first;
    TEST_CHECK(moves(&u, 0, 6, HZ30, &first) == -2 && first == 1);
    TEST_CHECK(frame(&u, 0, HZ30) == -2);
}

/* Held, the list moves as many rows by the same time at any frame rate: at
 * 20, 30, 40 and 50 Hz never more than a row off the 60 Hz count at each of
 * their frames, for 5 s. */
static void the_speed_does_not_depend_on_the_frame_rate(void)
{
    static const uint64_t rates[] = {50000u, 33333u, 25000u, 20000u};
    for (int i = 0; i < 4; i++) {
        Rig fast = rig(), slow = rig();
        int fast_rows = frame(&fast, 255, HZ60), slow_rows = frame(&slow, 255, rates[i]);
        uint64_t fast_t = 0, slow_t = 0;
        int worst = 0;
        while (slow_t < 5000000u) {
            slow_rows += frame(&slow, 255, rates[i]);
            slow_t += rates[i];
            while (fast_t + HZ60 <= slow_t) {
                fast_rows += frame(&fast, 255, HZ60);
                fast_t += HZ60;
            }
            int off = slow_rows > fast_rows ? slow_rows - fast_rows : fast_rows - slow_rows;
            if (off > worst) worst = off;
        }
        TEST_CHECK_(worst <= 1, "%llu us frames: %d rows apart at worst from 60 Hz's", (unsigned long long)rates[i],
                    worst);
        /* about 5 s: the push's row, then 60 a second after 1/6 s */
        int want = 2 + (int)((slow_t + VJO_STICK_SLACK_US - VJO_STICK_HOLD_US) / VJO_STICK_ROW_US);
        TEST_CHECK_(slow_rows >= want - 1 && slow_rows <= want, "%llu us frames: %d rows in %llu us, want %d",
                    (unsigned long long)rates[i], slow_rows, (unsigned long long)slow_t, want);
    }
}

/* Held for an hour, it keeps the speed: a row every frame at 60 Hz, two at
 * 30 Hz (one in about a thousand frames gives one, as 33,333 us is one short
 * of two rows), and never more than three in a frame. */
static void held_for_an_hour(void)
{
    static const uint64_t rates[] = {HZ60, HZ30};
    for (int i = 0; i < 2; i++) {
        Rig r = rig();
        long total = frame(&r, 255, rates[i]);
        int most = 0, quiet = 0;
        long frames = 3600L * 1000000L / (long)rates[i];
        for (long f = 2; f <= frames; f++) {
            int m = frame(&r, 255, rates[i]);
            total += m;
            if (m > most) most = m;
            if (f > 11 && m == 0) quiet++;
        }
        long want = 1 + (3600L * 1000000L - 166667L) / 16667L;
        TEST_CHECK_(total >= want - 10 && total <= want + 10, "%llu us frames: %ld rows in an hour, want about %ld",
                    (unsigned long long)rates[i], total, want);
        TEST_CHECK_(most <= 2 && quiet == 0, "%llu us frames: at most %d rows a frame, %d quiet frames",
                    (unsigned long long)rates[i], most, quiet);
    }
}

/* A frame that took 2 s (the app stalled) moves at most three rows; a clock
 * that goes back moves nothing; and the push's row comes at once whatever the
 * clock read before. */
static void a_stalled_frame_moves_at_most_three_rows(void)
{
    Rig r = rig();
    int first;
    TEST_CHECK(moves(&r, 255, 15, HZ60, &first) == 6); /* frame 1, then 11-15 */
    TEST_CHECK_(frame(&r, 255, 2000000u) == 3, "a 2 s frame");
    TEST_CHECK(frame(&r, 255, HZ60) == 1);
    r.now -= 100000u;
    TEST_CHECK(vjo_stick_scroll(&r.s, 255, r.now) == 0);
    TEST_CHECK(frame(&r, 255, HZ60) == 1);
    /* during the pause too: a long frame moves no row before its time */
    Rig p = rig();
    TEST_CHECK(frame(&p, 255, HZ60) == 1);
    TEST_CHECK(frame(&p, 255, 2000000u) == 0 && frame(&p, 255, 2000000u) == 0); /* 100 ms counted */
    TEST_CHECK(frame(&p, 255, 2000000u) == 0);                                 /* 150 ms */
    TEST_CHECK(frame(&p, 255, 2000000u) == 3); /* 200 ms: the repeats due at 166.7, 183.3 and 200 ms */
    VjoStickScroll fresh = {0};
    TEST_CHECK(vjo_stick_scroll(&fresh, 0, 123456789u) == -1);
}

/* Up then straight to down: down starts afresh, with its own pause. */
static void up_then_down(void)
{
    Rig r = rig();
    int first;
    TEST_CHECK(moves(&r, 0, 20, HZ60, &first) == -11 && first == 1);
    TEST_CHECK(moves(&r, 255, 10, HZ60, &first) == 1 && first == 1);
    TEST_CHECK(moves(&r, 255, 3, HZ60, &first) == 3);
    Rig h = rig();
    TEST_CHECK(moves(&h, 0, 10, HZ30, &first) == -10 && first == 1); /* 1, 1 at frame 6, then 2 a frame */
    TEST_CHECK(moves(&h, 255, 5, HZ30, &first) == 1 && first == 1);
}

/* The stick's move on a list stops at its first and last row (VitaShell's
 * lists do not wrap); a held stick runs to the end and stays there, also when
 * it moves two rows a frame. */
static void stops_at_the_ends(void)
{
    TEST_CHECK(vjo_stick_step(0, 4, -1) == 0 && vjo_stick_step(3, 4, 1) == 3);
    TEST_CHECK(vjo_stick_step(1, 4, -1) == 0 && vjo_stick_step(2, 4, 1) == 3);
    TEST_CHECK(vjo_stick_step(2, 4, 0) == 2 && vjo_stick_step(0, 1, 1) == 0);
    TEST_CHECK(vjo_stick_step(2, 4, 3) == 3 && vjo_stick_step(1, 4, -2) == 0 && vjo_stick_step(1, 6, 2) == 3);
    Rig r = rig();
    int sel = 0, path[30];
    for (int f = 0; f < 30; f++)
        path[f] = sel = vjo_stick_step(sel, 4, frame(&r, 255, HZ60));
    /* one row on the push, nine quiet frames, then a row a frame to the last */
    TEST_CHECK_(path[0] == 1 && path[9] == 1 && path[10] == 2 && path[11] == 3, "%d %d %d %d", path[0], path[9],
                path[10], path[11]);
    for (int f = 11; f < 30; f++)
        TEST_CHECK_(path[f] == 3, "frame %d: %d", f + 1, path[f]);
    frame(&r, 128, HZ60);
    for (int f = 0; f < 30; f++)
        sel = vjo_stick_step(sel, 4, frame(&r, 0, HZ60));
    TEST_CHECK(sel == 0);
    /* at 30 Hz: the push, the first repeat at 166.7 ms, then two a frame to the last of 8 */
    Rig h = rig();
    sel = 0;
    for (int f = 0; f < 15; f++)
        path[f] = sel = vjo_stick_step(sel, 8, frame(&h, 255, HZ30));
    TEST_CHECK_(path[0] == 1 && path[4] == 1 && path[5] == 2 && path[6] == 4 && path[7] == 6 && path[8] == 7 &&
                path[14] == 7, "%d %d %d %d %d %d %d", path[0], path[4], path[5], path[6], path[7], path[8], path[14]);
}

/* A page change or the keyboard latches a pushed stick: still pushed, it
 * moves nothing until let back inside the threshold, and then the next push
 * is a push again. A stick at rest is not latched. */
static void latched_until_let_go(void)
{
    Rig r = rig();
    int first;
    TEST_CHECK(moves(&r, 255, 15, HZ60, &first) == 6); /* frame 1, then 11-15 */
    vjo_stick_latch(&r.s);
    TEST_CHECK(moves(&r, 255, 30, HZ60, &first) == 0);
    vjo_stick_latch(&r.s); /* again while latched: still latched */
    TEST_CHECK(moves(&r, 0, 30, HZ60, &first) == 0 && moves(&r, 200, 5, HZ60, &first) == 0);
    TEST_CHECK(frame(&r, 128, HZ60) == 0); /* let go */
    TEST_CHECK(moves(&r, 255, 11, HZ60, &first) == 2 && first == 1);
    TEST_CHECK(frame(&r, 128, HZ60) == 0);
    vjo_stick_latch(&r.s); /* at rest: no latch */
    TEST_CHECK(frame(&r, 0, HZ60) == -1);
}

TEST_LIST = {
    {"threshold: past 64 from the centre, any distance", threshold},
    {"held at 60 Hz: a move, nine quiet frames, then every frame", held_at_60_hz_moves_like_vitashell},
    {"held at 30 Hz: the same 166.7 ms, then two rows a frame", held_at_30_hz_keeps_the_time},
    {"the speed does not depend on the frame rate (20-60 Hz)", the_speed_does_not_depend_on_the_frame_rate},
    {"held for an hour: the speed holds, nothing wraps", held_for_an_hour},
    {"a stalled frame moves at most three rows", a_stalled_frame_moves_at_most_three_rows},
    {"up then down starts afresh", up_then_down},
    {"a list's first and last row stop the stick", stops_at_the_ends},
    {"latched: nothing until let go, then a push again", latched_until_let_go},
    {NULL, NULL}
};
