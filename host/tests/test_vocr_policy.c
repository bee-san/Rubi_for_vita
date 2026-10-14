/* The job policy of core/vocr_ocr.c (VjoVocrPolicy) without weights: the
 * stable-text wait on a screen that types out, holds still, toggles an icon or
 * never holds still; a zeroed policy reading exactly as API 19 did; the
 * retraction of a capture the Shell was told of; and the blank-line check's ink
 * statistics. Each strip read is one frame of the game: the clock moves on by
 * 16,667 us (60 fps) per read, as the display hook's next frame would. */
#include "acutest.h"
#include "vocr_ocr.h"
#include "../../core/fbgrey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 400
#define H 100
#define FRAME_US 16667u
#define LOG_MAX 512

typedef struct {
    unsigned y, n, flags, frame;
} Read;

static struct {
    uint64_t clock;
    unsigned frame;
    unsigned chars;        /* characters of the full line */
    unsigned type_every;   /* frames per new character; 0: all there from the start */
    unsigned blink_every;  /* the "next" icon changes every this many frames; 0: no icon */
    unsigned icon_states;  /* 0: it blinks (on, off); n: it bobs through n positions */
    int noise;             /* every frame differs: never holds still */
    unsigned change_frame; /* from this frame on the line is another one (the player advanced); 0: never */
    unsigned text_from;    /* the line shows from this frame (an empty box before) */
    unsigned clear_at;     /* from this frame the box is empty again; 0: never */
    unsigned sleeps;       /* the policy's sleep calls */
    int three_lines;       /* three lines of 24 rows (6-30, 37-61, 68-92) instead of one (30-54) */
    Read log[LOG_MAX];
    unsigned n_log;
    int opens;
} scr;

static uint64_t now(void *ud)
{
    (void)ud;
    return scr.clock;
}

static int no_file(void *ud, const char *path, VjoFile *out)
{
    (void)ud; (void)path; (void)out;
    scr.opens++;
    return -1;
}

/* Characters shown at this frame. */
static unsigned shown(unsigned frame)
{
    return !scr.type_every ? scr.chars : frame / scr.type_every < scr.chars ? frame / scr.type_every : scr.chars;
}

static unsigned char pixel(unsigned x, unsigned y, unsigned frame)
{
    unsigned char v = 20;
    int text_on = frame >= scr.text_from && !(scr.clear_at && frame >= scr.clear_at);
    int on_line = scr.three_lines ? (y >= 6 && y < 30) || (y >= 37 && y < 61) || (y >= 68 && y < 92) : y >= 30 && y < 54;
    if (on_line && text_on) {
        unsigned c = x >= 20 ? (x - 20) / 12 : 99, k = x >= 20 ? (x - 20) % 12 : 99;
        int other = scr.change_frame && frame >= scr.change_frame;
        if (c < shown(frame) && (k < 2 || (other && k == 6) || (k == 4 && (y & 3) == 0))) v = 235;
    }
    if (scr.blink_every && x >= 370 && x < 386) {
        unsigned state = frame / scr.blink_every;
        if (!scr.icon_states && y >= 30 && y < 54 && state % 2) v = 200;
        if (scr.icon_states && y >= 32 + state % scr.icon_states * 3 && y < 44 + state % scr.icon_states * 3) v = 200;
    }
    if (scr.noise) v = (unsigned char)(v ^ (frame * 37u & 0x1Fu));
    return v;
}

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud;
    if (y + n > H) return VJO_E_SOURCE;
    scr.clock += FRAME_US;
    scr.frame++;
    if (scr.n_log < LOG_MAX) scr.log[scr.n_log++] = (Read){y, n, flags, scr.frame};
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < W; x++) dst[(size_t)r * stride + x] = pixel(x, y + r, scr.frame);
    return (int)n;
}

/* The policy's sleep: the game shows frames meanwhile. */
static void sleep_us(uint32_t us)
{
    scr.clock += us;
    scr.frame += us / FRAME_US;
    scr.sleeps++;
}

static VjoGreySource im = {NULL, W, H, strip, NULL, NULL};
static unsigned char capture[64 * 1024];
static unsigned char ws_mem[512 * 1024] __attribute__((aligned(64)));

static void reset(void)
{
    memset(&scr, 0, sizeof(scr));
    scr.clock = 1000000;
    scr.chars = 25;
}

static int run(const VjoVocrPolicy *pol, VjoVocrStats *st)
{
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    char text[128];
    size_t job = vjo_vocr_job_bytes("FL10_w8.vocr", W, H);
    TEST_ASSERT(job && job <= sizeof(ws_mem));
    p.file_open = no_file;
    p.now_us = now;
    return vjo_vocr_ocr_policy(&p, "missing", "FL10_w8.vocr", &im, &res, ws_mem, job, capture,
                               vjo_vocr_capture_bytes(W, H), pol, text, sizeof(text), st);
}

/* The first read that starts a first pass and ends it (VJO_STRIP_LAST): the
 * capture the Shell is told of. */
static int published_at(unsigned *frame)
{
    for (unsigned i = 0; i < scr.n_log; i++)
        if (scr.log[i].flags & VJO_STRIP_LAST) {
            *frame = scr.log[i].frame;
            return (int)i;
        }
    return -1;
}

/* A zeroed policy, or none, reads exactly as API 19's job: the same strips,
 * in the same order, with the same flags, and the same result. */
static void test_zero_policy_is_api19(void)
{
    VjoVocrStats a, b, c;
    Read first[LOG_MAX];
    unsigned n;
    VjoVocrPolicy zero;
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    char text[128];
    memset(&zero, 0, sizeof(zero));
    reset();
    p.file_open = no_file;
    p.now_us = now;
    TEST_CHECK(vjo_vocr_ocr_resident(&p, "missing", "FL10_w8.vocr", &im, &res, ws_mem,
                                     vjo_vocr_job_bytes("FL10_w8.vocr", W, H), capture, vjo_vocr_capture_bytes(W, H),
                                     text, sizeof(text), &a) == VJO_E_OCR_MODEL);
    memcpy(first, scr.log, sizeof(first));
    n = scr.n_log;
    reset();
    TEST_CHECK(run(NULL, &b) == VJO_E_OCR_MODEL);
    TEST_CHECK(scr.n_log == n && !memcmp(scr.log, first, n * sizeof(Read)));
    reset();
    TEST_CHECK(run(&zero, &c) == VJO_E_OCR_MODEL);
    TEST_CHECK(scr.n_log == n && !memcmp(scr.log, first, n * sizeof(Read)));
    TEST_CHECK(a.lines == 1 && b.lines == 1 && c.lines == 1 && a.strips == b.strips && b.strips == c.strips);
    TEST_CHECK(a.region_sum == b.region_sum && b.region_sum == c.region_sum && a.tries == 1);
    TEST_CHECK(!b.settle_passes && !b.settle_us && !b.settled && !b.blank && !c.settle_passes);
    TEST_MSG("%u reads; region sum 0x%08X", n, (unsigned)a.region_sum);
}

/* Text that types out (a character every 3 frames, 25 of them): waiting first,
 * the job reads nothing as a first pass (no capture for the Shell, no lines)
 * until every row has held still for 150 ms, i.e. 9 frames after the last
 * character; its lines are the whole line's. */
static void test_typing_is_read_once_whole(void)
{
    VjoVocrPolicy pol = {150000, 0, 1, 0, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    unsigned at = 0, typed = 25 * 3;
    reset();
    scr.type_every = 3;
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL); /* the lines were found; no weights here */
    TEST_ASSERT(published_at(&at) >= 0);
    TEST_CHECK(at > typed + 150000 / FRAME_US); /* the first pass begins after the wait */
    TEST_CHECK(st.settled && !st.settle_timeout && st.settle_changes > 0 && st.lines == 1 && st.tries == 1);
    TEST_CHECK(st.settle_us >= 150000 && st.settle_passes >= 2 && !st.tears);
    {
        unsigned first = 0;
        while (first < scr.n_log && !(scr.log[first].flags & VJO_STRIP_FIRST)) first++;
        TEST_CHECK(first < scr.n_log && first > 0 && scr.log[first].y == 0);
        TEST_CHECK(scr.log[first].frame > typed + 150000 / FRAME_US); /* nothing flagged while it typed */
    }
    TEST_MSG("typed by frame %u, first pass ends at frame %u, waited %u us in %u passes, %u row changes", typed, at,
             (unsigned)st.settle_us, st.settle_passes, st.settle_changes);
    /* Without the wait the same screen, read at once, is a line half typed. */
    VjoVocrStats now_st;
    reset();
    scr.type_every = 3;
    (void)run(NULL, &now_st);
    TEST_CHECK(published_at(&at) >= 0 && at < typed);
    TEST_MSG("no wait: first pass at frame %u, while %u of %u characters show", at, shown(at), scr.chars);
}

/* A screen that holds still: the job finds its lines at once (no wait first)
 * and re-reads the region until 150 ms of reads agree, then recognizes. */
static void test_still_screen_waits_only_the_span(void)
{
    VjoVocrPolicy pol = {150000, 0, 0, 0, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    unsigned at = 0;
    reset();
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(published_at(&at) == 1 && at == 2); /* the first pass is the job's first two strips */
    TEST_CHECK(st.settled && !st.settle_timeout && !st.settle_changes && st.settle_passes >= 1 && st.tries == 1);
    TEST_CHECK(st.settle_us > 0 && st.settle_us < 150000 + 4u * FRAME_US);
    TEST_MSG("%u extra passes, %u us", st.settle_passes, (unsigned)st.settle_us);
}

/* A "next" icon blinking every 8 frames beside a still line: its rows only go
 * back and forth between two states, so the wait ends; they are counted. */
static void test_blinking_icon_does_not_block(void)
{
    VjoVocrPolicy pol = {150000, 0, 1, 0, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    reset();
    scr.blink_every = 8;
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.settled && !st.settle_timeout && st.cycles > 0 && st.cycling_rows > 0 && st.cycling_rows <= 24);
    TEST_CHECK(st.settle_us < 1000000 && st.tries == 1 && !st.tears);
    TEST_MSG("%u returns on %u rows, waited %u us", st.cycles, st.cycling_rows, (unsigned)st.settle_us);
}

/* A region that never holds still (a video behind the text): the wait gives up
 * after settle_max_us, once for the job, which then reads as before. */
static void test_never_still_times_out_once(void)
{
    VjoVocrPolicy pol = {150000, 400000, 1, 0, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    reset();
    scr.noise = 1;
    TEST_CHECK(run(&pol, &st) == VJO_E_SCREEN_CHANGED);
    TEST_CHECK(!st.settled && st.settle_timeout && st.tries == VJO_VOCR_TRIES);
    TEST_CHECK(st.settle_us >= 400000 && st.settle_us < 400000 + 3u * FRAME_US);
    TEST_MSG("waited %u us", (unsigned)st.settle_us);
}

/* The player advances the text after the job's first pass was published: the
 * job voids that capture before it waits (a first-pass read of row 0 that does
 * not end it), then publishes the new screen once it holds still. */
static void test_changed_screen_retracts_its_capture(void)
{
    VjoVocrPolicy pol = {150000, 0, 0, 0, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    unsigned lasts = 0, retract = 0;
    reset();
    scr.change_frame = 3; /* after the first pass (frames 1-2) */
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.tries == 2 && st.tears > 0 && st.settled && st.lines == 1);
    for (unsigned i = 0; i < scr.n_log; i++) {
        if (scr.log[i].flags & VJO_STRIP_LAST) lasts++;
        if (scr.log[i].flags == VJO_STRIP_FIRST && scr.log[i].y == 0 && lasts == 1 && !retract) retract = i;
    }
    TEST_CHECK(lasts == 2 && retract > 0);
    TEST_MSG("retracted at read %u, %u publications", retract, lasts);
}

/* With a sleep, the verification does not read passes too early to count: the
 * same result from fewer strips (the display hook copies fewer rows), and no
 * later. */
static void test_sleep_skips_early_passes(void)
{
    VjoVocrPolicy pol = {150000, 0, 0, 0, 0, NULL, NULL, NULL};
    VjoVocrStats busy, idle;
    unsigned reads_busy, at_busy, at_idle;
    reset();
    TEST_CHECK(run(&pol, &busy) == VJO_E_OCR_MODEL && busy.settled);
    reads_busy = scr.n_log;
    at_busy = scr.frame;
    reset();
    pol.sleep = sleep_us;
    TEST_CHECK(run(&pol, &idle) == VJO_E_OCR_MODEL && idle.settled && !idle.settle_timeout && scr.sleeps >= 1);
    at_idle = scr.frame;
    TEST_CHECK(scr.n_log < reads_busy && idle.strips < busy.strips && at_idle <= at_busy + 1);
    TEST_MSG("%u strips without sleeping, %u with (%u sleeps); recognition from frame %u / %u", busy.strips,
             idle.strips, scr.sleeps, at_busy, at_idle);
}

/* An icon bobbing through four positions: once it has shown them all, its
 * rows only go back to recent states, so the wait ends. */
static void test_bobbing_icon_settles(void)
{
    VjoVocrPolicy pol = {150000, 0, 1, 0, 0, NULL, NULL, sleep_us};
    VjoVocrStats st;
    reset();
    scr.blink_every = 4;
    scr.icon_states = 4;
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.settled && !st.settle_timeout && st.cycles > 0 && st.cycling_rows > 0 && st.cycling_rows <= 24);
    TEST_CHECK(st.tries == 1 && !st.tears && st.settle_us < 1000000);
    TEST_MSG("%u returns on %u rows, waited %u us", st.cycles, st.cycling_rows, (unsigned)st.settle_us);
}

/* A box of three lines was empty while the job waited, then the page showed
 * and held still; the player advances and the box goes back to empty while
 * the lines are read. Its rows go back to a state the job saw, as an icon's
 * would, but 72 rows are more than an icon's 64: the change is a change, and
 * the retry reads the empty box. */
static void test_a_box_clearing_back_is_a_change(void)
{
    VjoVocrPolicy pol = {150000, 0, 1, 0, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    reset();
    scr.three_lines = 1;
    scr.text_from = 10;
    scr.clear_at = 25; /* during the finder's second pass after the wait (frames 23-26) */
    TEST_CHECK(run(&pol, &st) == VJO_OK);
    TEST_CHECK(st.tries == 2 && st.tears > 0 && st.lines == 0 && st.cycling_rows == VJO_VOCR_CYCLE_ROWS && !scr.opens);
    TEST_MSG("tries %u tears %u lines %u, cycling rows %u", st.tries, st.tears, st.lines, st.cycling_rows);
}

/* The ink statistics: an even area or a smooth ramp holds no text-like ink;
 * strokes do, also small and also dark on light; under 24 levels of contrast
 * nothing is text. */
static void test_ink_and_blank(void)
{
    static unsigned char px[40 * 300];
    VjoVocrInk ink;
    memset(px, 90, sizeof(px));
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 1 && ink.hi == ink.lo && ink.edges == 0);
    for (unsigned y = 0; y < 40; y++) /* a ramp: a smooth picture or the box's gradient */
        for (unsigned x = 0; x < 300; x++) px[y * 300 + x] = (unsigned char)(20 + x * 200 / 300);
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 1 && ink.hi - ink.lo > 150 && ink.edges == 0);
    memset(px, 20, sizeof(px)); /* strokes */
    for (unsigned y = 8; y < 32; y++)
        for (unsigned x = 10; x < 290; x += 12) px[y * 300 + x] = px[y * 300 + x + 1] = 235;
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 0 && ink.edges >= 400);
    TEST_MSG("strokes: contrast %u, %u edges per 10,000", ink.hi - ink.lo, ink.edges);
    for (size_t i = 0; i < sizeof(px); i++) px[i] = (unsigned char)(255 - px[i]); /* dark on light */
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 0);
    memset(px, 20, sizeof(px)); /* one small mark: "。" alone in its box */
    for (unsigned y = 16; y < 24; y++)
        for (unsigned x = 16; x < 24; x++) px[y * 40 + x] = (y == 16 || y == 23 || x == 16 || x == 23) ? 230 : 20;
    TEST_CHECK(vjo_vocr_ink(px, 40, 40, 40, &ink) == 0 && ink.hi == ink.lo); /* under 2% of the pixels */
    TEST_MSG("a mark in a 40x40 box: %u edges per 10,000", ink.edges);
    memset(px, 100, sizeof(px)); /* strokes of 20 levels: under the contrast floor */
    for (unsigned y = 8; y < 32; y++)
        for (unsigned x = 10; x < 290; x += 12) px[y * 300 + x] = 120;
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 1 && ink.edge == VJO_VOCR_EDGE_MIN && !ink.edges);
    TEST_CHECK(vjo_vocr_ink(NULL, 300, 300, 40, &ink) == 0 && vjo_vocr_ink(px, 300, 0, 40, &ink) == 0);
    TEST_CHECK(vjo_vocr_ink(px, 961, 961, 1, &ink) == 0); /* wider than any region: never called blank */
}

/* In a job: lines the check skips are not recognized, and a region of skipped
 * lines opens no weights; without the check the same line opens them. */
static void test_blank_lines_open_no_weights(void)
{
    VjoVocrPolicy all = {0, 0, 0, 1, 20000, NULL, NULL, NULL}, real = {0, 0, 0, 1, 0, NULL, NULL, NULL};
    VjoVocrStats st;
    reset();
    TEST_CHECK(run(&all, &st) == VJO_OK && st.lines == 1 && st.blank == 1 && !scr.opens && !st.recognized);
    TEST_CHECK(st.blank_line[0].index == 0 && st.blank_line[0].width > 200 && st.blank_line[0].ink.edges > 0);
    reset();
    TEST_CHECK(run(&real, &st) == VJO_E_OCR_MODEL && st.lines == 1 && !st.blank && scr.opens == 1);
}

/* The capture block stays within its 64 KiB at every region size, and keeps at
 * least two earlier states a row; a dialogue box keeps sixteen. */
static void test_capture_block_sizes(void)
{
    size_t most = 0;
    for (unsigned w = 1; w <= 960; w += (w < 64 ? 1 : 7))
        for (unsigned h = 1; h <= 544; h += (h < 64 ? 1 : 13)) {
            size_t b = vjo_vocr_capture_bytes(w, h);
            unsigned s = vjo_vocr_state_slots(w, h);
            TEST_CHECK(b && b <= VJO_VOCR_CAPTURE_LIMIT && s >= 2 && s <= VJO_VOCR_STATES_MAX);
            if (b > most) most = b;
        }
    TEST_CHECK(vjo_vocr_capture_bytes(960, 544) <= VJO_VOCR_CAPTURE_LIMIT && vjo_vocr_capture_bytes(896, 544) <= VJO_VOCR_CAPTURE_LIMIT);
    TEST_CHECK(vjo_vocr_state_slots(917, 125) == 16 && vjo_vocr_state_slots(960, 544) >= 2);
    TEST_MSG("largest capture block %zu B; 960x544 keeps %u states", most, vjo_vocr_state_slots(960, 544));
}

TEST_LIST = {
    {"the_capture_block_stays_within_64_KiB_at_every_size", test_capture_block_sizes},
    {"a_zero_policy_reads_exactly_as_api19", test_zero_policy_is_api19},
    {"typing_text_is_read_once_whole", test_typing_is_read_once_whole},
    {"a_still_screen_waits_only_the_span", test_still_screen_waits_only_the_span},
    {"a_blinking_icon_does_not_block_the_wait", test_blinking_icon_does_not_block},
    {"a_screen_never_still_times_out_once", test_never_still_times_out_once},
    {"a_changed_screen_retracts_its_capture_then_publishes", test_changed_screen_retracts_its_capture},
    {"with_a_sleep_the_verification_reads_fewer_passes", test_sleep_skips_early_passes},
    {"a_bobbing_icon_settles_once_it_has_shown_its_positions", test_bobbing_icon_settles},
    {"a_box_clearing_back_to_empty_is_a_change", test_a_box_clearing_back_is_a_change},
    {"ink_statistics_and_the_blank_rule", test_ink_and_blank},
    {"blank_lines_are_skipped_and_open_no_weights", test_blank_lines_open_no_weights},
    {NULL, NULL}
};
