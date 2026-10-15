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

TEST_LIST = {
    {"threshold: past 64 from the centre, any distance", threshold},
    {"held: a move, nine quiet frames, then every frame", held_moves_like_vitashell},
    {"up then down starts afresh", up_then_down},
    {NULL, NULL}
};
