/* The left stick's list scrolling (core/stick_scroll.h) frame by frame against
 * VitaShell's readPad: the threshold, the first move, the nine quiet frames,
 * then one move a frame, and up and down taking turns. */
#include "acutest.h"
#include "stick_scroll.h"

/* Moves over n frames with the stick at ly; *first: the frame (1-based) of the first. */
static int moves(VjoStickScroll *s, uint8_t ly, int n, int want, int *first)
{
    int count = 0;
    *first = 0;
    for (int f = 1; f <= n; f++)
        if (vjo_stick_scroll(s, ly) == want && count++ == 0)
            *first = f;
    return count;
}

static void threshold(void)
{
    VjoStickScroll s = {0};
    int first;
    /* 64 from the centre is still the centre; 65 is a push */
    TEST_CHECK(moves(&s, 64, 30, -1, &first) == 0 && moves(&s, 192, 30, 1, &first) == 0);
    TEST_CHECK(vjo_stick_scroll(&s, 128) == 0);
    TEST_CHECK(vjo_stick_scroll(&s, 63) == -1);
    TEST_CHECK(vjo_stick_scroll(&s, 128) == 0);
    TEST_CHECK(vjo_stick_scroll(&s, 193) == 1);
    TEST_CHECK(vjo_stick_scroll(&s, 128) == 0);
    /* how far it is pushed changes nothing */
    TEST_CHECK(vjo_stick_scroll(&s, 0) == -1 && vjo_stick_scroll(&s, 0) == 0);
}

/* VitaShell's hold2: a move on the push (frame 1), none on frames 2-10, then
 * one on every frame from 11 on. */
static void held_moves_like_vitashell(void)
{
    VjoStickScroll s = {0};
    int fired[40];
    for (int f = 0; f < 40; f++)
        fired[f] = vjo_stick_scroll(&s, 255);
    TEST_CHECK(fired[0] == 1);
    for (int f = 1; f < 10; f++)
        TEST_CHECK_(fired[f] == 0, "frame %d", f + 1);
    for (int f = 10; f < 40; f++)
        TEST_CHECK_(fired[f] == 1, "frame %d", f + 1);
    /* released and pushed again: the same again */
    vjo_stick_scroll(&s, 128);
    int first;
    TEST_CHECK(moves(&s, 255, 10, 1, &first) == 1 && first == 1);
    TEST_CHECK(moves(&s, 255, 5, 1, &first) == 5 && first == 1);
}

/* Up then straight to down: down starts afresh, with its own pause. */
static void up_then_down(void)
{
    VjoStickScroll s = {0};
    int first;
    TEST_CHECK(moves(&s, 0, 20, -1, &first) == 11 && first == 1);
    TEST_CHECK(moves(&s, 255, 10, 1, &first) == 1 && first == 1);
    TEST_CHECK(moves(&s, 255, 3, 1, &first) == 3);
}

/* The stick's move on a list stops at its first and last row (VitaShell's
 * lists do not wrap); a held stick runs to the end and stays there. */
static void stops_at_the_ends(void)
{
    TEST_CHECK(vjo_stick_step(0, 4, -1) == 0 && vjo_stick_step(3, 4, 1) == 3);
    TEST_CHECK(vjo_stick_step(1, 4, -1) == 0 && vjo_stick_step(2, 4, 1) == 3);
    TEST_CHECK(vjo_stick_step(2, 4, 0) == 2 && vjo_stick_step(0, 1, 1) == 0);
    VjoStickScroll s = {0};
    int sel = 0, path[30];
    for (int f = 0; f < 30; f++)
        path[f] = sel = vjo_stick_step(sel, 4, vjo_stick_scroll(&s, 255));
    /* one row on the push, nine quiet frames, then a row a frame to the last */
    TEST_CHECK_(path[0] == 1 && path[9] == 1 && path[10] == 2 && path[11] == 3, "%d %d %d %d", path[0], path[9],
                path[10], path[11]);
    for (int f = 11; f < 30; f++)
        TEST_CHECK_(path[f] == 3, "frame %d: %d", f + 1, path[f]);
    vjo_stick_scroll(&s, 128);
    for (int f = 0; f < 30; f++)
        sel = vjo_stick_step(sel, 4, vjo_stick_scroll(&s, 0));
    TEST_CHECK(sel == 0);
}

/* Held for minutes, it keeps moving a row every frame: the count stays at its
 * limit and never wraps round (a uint8_t would, every 256 frames). */
static void held_for_long(void)
{
    VjoStickScroll s = {0};
    int quiet = 0, first_quiet = 0;
    for (int f = 1; f <= 1200; f++)
        if (vjo_stick_scroll(&s, 255) != 1 && f > 10 && quiet++ == 0) first_quiet = f;
    TEST_CHECK_(quiet == 0, "%d quiet frames after the pause, the first at frame %d", quiet, first_quiet);
}

/* A page change or the keyboard latches a pushed stick: still pushed, it
 * moves nothing until let back inside the threshold, and then the next push
 * is a push again. A stick at rest is not latched. */
static void latched_until_let_go(void)
{
    VjoStickScroll s = {0};
    int first;
    TEST_CHECK(moves(&s, 255, 15, 1, &first) == 6); /* frame 1, then 11-15 */
    vjo_stick_latch(&s);
    TEST_CHECK(moves(&s, 255, 30, 1, &first) == 0);
    vjo_stick_latch(&s); /* again while latched: still latched */
    TEST_CHECK(moves(&s, 0, 30, -1, &first) == 0 && moves(&s, 200, 5, 1, &first) == 0);
    TEST_CHECK(vjo_stick_scroll(&s, 128) == 0); /* let go */
    TEST_CHECK(moves(&s, 255, 11, 1, &first) == 2 && first == 1);
    TEST_CHECK(vjo_stick_scroll(&s, 128) == 0);
    vjo_stick_latch(&s); /* at rest: no latch */
    TEST_CHECK(vjo_stick_scroll(&s, 0) == -1);
}

TEST_LIST = {
    {"threshold: past 64 from the centre, any distance", threshold},
    {"held: a move, nine quiet frames, then every frame", held_moves_like_vitashell},
    {"held for long: every frame, the count never wraps", held_for_long},
    {"up then down starts afresh", up_then_down},
    {"a list's first and last row stop the stick", stops_at_the_ends},
    {"latched: nothing until let go, then a push again", latched_until_let_go},
    {NULL, NULL}
};
