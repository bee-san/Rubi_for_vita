/* The job policy of core/vocr_ocr.c (VjoVocrPolicy) without weights: the
 * stable-text wait on a screen that types out, holds still, toggles an icon or
 * never holds still; a zeroed policy reading exactly as API 19 did; the
 * retraction of a capture the Shell was told of; and the blank-line check's ink
 * statistics. Each strip read is one frame of the game: the clock moves on by
 * 16,667 us (60 fps) per read, as the display hook's next frame would. */
#include "acutest.h"
#include "net_posix.h"
#include "vocr_ocr.h"
#include "vocr.h"
#include "vocr_lm.h"
#include "../../core/fbgrey.h"
#include "../../include/vjo_vocr_sizes.h"
#include "vocr_lines.h"
#include "vocr_lines_refine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 400
#define H 100
#define FRAME_US 16667u
#define LOG_MAX 512

typedef struct {
    unsigned y, n, flags, frame;
    uint64_t t0; /* the clock when the read began */
} Read;

static struct {
    uint64_t clock;
    unsigned frame;
    unsigned chars;        /* characters of the full line */
    unsigned type_every;   /* frames per new character; 0: all there from the start */
    unsigned blink_every;  /* the "next" icon changes every this many frames; 0: no icon */
    unsigned icon_states;  /* 0: it blinks (on, off); n: it bobs through n positions */
    unsigned icon_levels;  /* n: it goes through n grey levels instead (80, 140, 200, ...) */
    unsigned icon_y;       /* its first row (0: 30, beside the line's rows); 24 rows */
    unsigned pause_at;     /* typing stops at this frame (0: never) ... */
    unsigned resume_for;   /* ... and goes on for this many frames once the first capture is published */
    unsigned resumed_at;   /* the frame it went on (set by strip) */
    unsigned change_after; /* change_frame is set this many frames after the first capture is published */
    unsigned alt_row, alt_from; /* row alt_row shows row_b from frame alt_from on, row_a before (0: none) */
    unsigned char row_a[W], row_b[W];
    unsigned open_read;    /* reads done when the model file was first asked for */
    int noise;             /* every frame differs: never holds still */
    unsigned change_frame; /* from this frame on the line is another one (the player advanced); 0: never */
    unsigned text_from;    /* the line shows from this frame (an empty box before) */
    unsigned clear_at;     /* from this frame the box is empty again; 0: never */
    unsigned sleeps;       /* the policy's sleep calls */
    int three_lines;       /* three lines of 24 rows (6-30, 37-61, 68-92) instead of one (30-54) */
    int two_lines;         /* the first two of them */
    unsigned text_blink;   /* the text shows for this many frames, then hides as long (0: it stays) */
    int frozen;            /* the clock never moves (reads and sleeps take no time) */
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
    if (!scr.opens) scr.open_read = scr.n_log;
    scr.opens++;
    return -1;
}

/* Characters shown at this frame. */
static unsigned shown(unsigned frame)
{
    unsigned typed = frame;
    if (!scr.type_every) return scr.chars;
    if (scr.pause_at && typed > scr.pause_at) {
        typed = scr.pause_at;
        if (scr.resumed_at && frame > scr.resumed_at)
            typed += frame - scr.resumed_at < scr.resume_for ? frame - scr.resumed_at : scr.resume_for;
    }
    typed /= scr.type_every;
    return typed < scr.chars ? typed : scr.chars;
}

static unsigned char pixel(unsigned x, unsigned y, unsigned frame)
{
    unsigned char v = 20;
    int text_on = frame >= scr.text_from && !(scr.clear_at && frame >= scr.clear_at) &&
                  !(scr.text_blink && frame / scr.text_blink % 2);
    if (scr.alt_row && y == scr.alt_row) return (frame >= scr.alt_from ? scr.row_b : scr.row_a)[x];
    int on_line = scr.three_lines || scr.two_lines
                  ? (y >= 6 && y < 30) || (y >= 37 && y < 61) || (scr.three_lines && y >= 68 && y < 92)
                  : y >= 30 && y < 54;
    if (on_line && text_on) {
        unsigned c = x >= 20 ? (x - 20) / 12 : 99, k = x >= 20 ? (x - 20) % 12 : 99;
        int other = scr.change_frame && frame >= scr.change_frame;
        if (c < shown(frame) && (k < 2 || (other && k == 6) || (k == 4 && (y & 3) == 0))) v = 235;
    }
    if (scr.blink_every && x >= 370 && x < 386) {
        unsigned state = frame / scr.blink_every, iy = scr.icon_y ? scr.icon_y : 30;
        if (scr.icon_levels) {
            if (y >= iy && y < iy + 24) v = (unsigned char)(80 + 60 * (state % scr.icon_levels));
        } else if (!scr.icon_states) {
            if (y >= iy && y < iy + 24 && state % 2) v = 200;
        } else if (y >= iy + 2 + state % scr.icon_states * 3 && y < iy + 14 + state % scr.icon_states * 3) {
            v = 200;
        }
    }
    if (scr.noise) v = (unsigned char)(v ^ (frame * 37u & 0x1Fu));
    return v;
}

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    uint64_t t0 = scr.clock;
    (void)ud;
    if (y + n > H) return VJO_E_SOURCE;
    if (!scr.frozen) scr.clock += FRAME_US;
    scr.frame++;
    if (scr.n_log < LOG_MAX) scr.log[scr.n_log++] = (Read){y, n, flags, scr.frame, t0};
    if (flags & VJO_STRIP_LAST) { /* the first capture is published: the game goes on */
        if (scr.resume_for && !scr.resumed_at) scr.resumed_at = scr.frame;
        if (scr.change_after && !scr.change_frame) scr.change_frame = scr.frame + scr.change_after;
    }
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < W; x++) dst[(size_t)r * stride + x] = pixel(x, y + r, scr.frame);
    return (int)n;
}

/* The policy's sleep: the game shows frames meanwhile. */
static void sleep_us(uint32_t us)
{
    if (!scr.frozen) scr.clock += us;
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
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
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
        /* The wait held the rows still for the span already: after the first
         * pass only the finder's own second pass reads from row 0, no re-read
         * of the verification, before the weights are asked for. */
        unsigned from_zero = 0;
        for (unsigned i = first; i < scr.open_read; i++) from_zero += scr.log[i].y == 0;
        TEST_CHECK(scr.opens == 1 && from_zero == 2);
        TEST_MSG("%u reads from row 0 between the first pass and the weights", from_zero);
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
    VjoVocrPolicy pol = {.settle_us = 150000};
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
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
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
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_max_us = 400000, .settle_first = 1};
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
    VjoVocrPolicy pol = {.settle_us = 150000};
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
    /* That read is the wait's (not the retry's own first pass): the first from
     * row 0 after the tear, FIRST without LAST, and followed by the wait's
     * unflagged reads, at least settle_us of them, before the retry's first pass. */
    unsigned last1 = 0, plain = 0, next = 0;
    while (last1 < scr.n_log && !(scr.log[last1].flags & VJO_STRIP_LAST)) last1++;
    TEST_ASSERT(last1 + 2 < scr.n_log);
    TEST_CHECK(scr.log[last1 + 1].y == 0 && !scr.log[last1 + 1].flags && scr.log[last1 + 1].frame >= scr.change_frame);
    TEST_CHECK(retract == last1 + 2 && scr.log[retract].y == 0 && scr.log[retract].flags == VJO_STRIP_FIRST);
    for (next = retract + 1; next < scr.n_log && !(scr.log[next].flags & VJO_STRIP_FIRST); next++) plain++;
    TEST_ASSERT(next < scr.n_log);
    TEST_CHECK(plain >= 2 && scr.log[next].t0 - scr.log[retract].t0 >= 150000);
    TEST_MSG("the tear at read %u, the retraction at %u, then %u unflagged reads (%llu us) before the retry's first pass",
             last1 + 1, retract, plain, (unsigned long long)(scr.log[next].t0 - scr.log[retract].t0));
}

/* The re-read that ends the verification STARTS settle_us after the job's
 * first pass did: the rows are known to hold still from its start only, so a
 * pass that merely ends that late does not count. Without a sleep the passes
 * follow each other and the first one due is the last; with one, the job
 * sleeps until it is due. Either way no pass more is read. */
static void test_the_last_reread_starts_the_span_after_the_first_pass(void)
{
    for (int sleeping = 0; sleeping < 2; sleeping++) {
        VjoVocrPolicy pol = {.settle_us = 150000, .sleep = sleeping ? sleep_us : NULL};
        VjoVocrStats st;
        uint64_t first = 0, last = 0;
        reset();
        TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL && scr.opens == 1 && st.settled && st.tries == 1);
        for (unsigned i = 0; i < scr.open_read; i++) {
            if (scr.log[i].y == 0 && (scr.log[i].flags & VJO_STRIP_FIRST) && !first) first = scr.log[i].t0;
            if (scr.log[i].y == 0) last = scr.log[i].t0;
        }
        TEST_CHECK(first && last - first >= 150000 && last - first < 150000 + 2u * FRAME_US);
        TEST_MSG("sleep %d: the last re-read from row 0 starts %llu us after the first pass", sleeping,
                 (unsigned long long)(last - first));
    }
}

/* Waiting first on a screen that changes once during the wait: the wait ends
 * on the first pass that STARTS settle_us after the start of the pass that
 * read the changed rows, never on one that only ends that late. */
static void test_the_wait_ends_on_the_first_pass_due(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
    VjoVocrStats st;
    uint64_t seen = 0, done = 0;
    unsigned first = 0;
    reset();
    scr.change_frame = 6;
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL && st.settled && st.settle_changes > 0 && st.tries == 1);
    while (first < scr.n_log && !(scr.log[first].flags & VJO_STRIP_FIRST)) first++;
    TEST_ASSERT(first < scr.n_log);
    for (unsigned i = 0; i < first; i++) {
        if (scr.log[i].y != 0) continue;
        if (scr.log[i].frame >= scr.change_frame && !seen) seen = scr.log[i].t0; /* reads every changed row */
        done = scr.log[i].t0;                                                   /* the wait's last pass */
    }
    TEST_CHECK(seen && done - seen >= 150000 && done - seen < 150000 + 2u * FRAME_US);
    TEST_MSG("the wait's last pass starts %llu us after the one that read the change", (unsigned long long)(done - seen));
}

/* The waiting budget is the job's, not each wait's: the first wait settles
 * after about 370 ms of a 600 ms budget; typing resumes once the capture is
 * published (7 more characters, 350 ms), so the retry's wait runs out of the
 * rest instead of settling, and the job waits no more than its budget (and the
 * pass under way) in all. */
static void test_the_waiting_budget_is_the_jobs(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_max_us = 600000, .settle_first = 1};
    VjoVocrStats st;
    reset();
    scr.type_every = 3;
    scr.pause_at = 12;   /* 4 characters, then still */
    scr.resume_for = 21; /* 7 more once the first capture is published */
    (void)run(&pol, &st);
    TEST_CHECK(scr.resumed_at > 0 && st.tries >= 2 && st.settle_timeout);
    TEST_CHECK(st.settle_us >= 600000 && st.settle_us <= 600000 + 3u * FRAME_US);
    TEST_MSG("waited %u us in all (budget 600000), timeout %u, tries %u", (unsigned)st.settle_us, st.settle_timeout,
             st.tries);
}

/* An icon that goes through three grey levels: each of its rows changes twice
 * (to the second and the third level), then only goes back to levels it showed,
 * also to the one two back, so the wait ends without waiting for it. */
static void test_a_three_state_icon_settles(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
    VjoVocrStats st;
    reset();
    scr.blink_every = 4;
    scr.icon_levels = 3;
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.settled && !st.settle_timeout && st.tries == 1 && !st.tears && st.settle_us < 600000);
    TEST_CHECK(st.cycling_rows == 24 && st.cycles > 0 && st.settle_changes <= 2u * 24u);
    TEST_MSG("waited %u us; %u changes, %u returns on %u rows", (unsigned)st.settle_us, st.settle_changes, st.cycles,
             st.cycling_rows);
}

/* What the job learned stays learned when it retries: an icon below the line,
 * blinking, is taken for a cycling icon in the first wait; the player then
 * advances the text, and the retry's wait knows the icon's rows (they are
 * counted once in the job) instead of learning them again. */
static void test_a_retry_keeps_what_the_job_learned(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
    VjoVocrStats st;
    reset();
    scr.blink_every = 8;
    scr.icon_y = 70;      /* rows of its own, below the line */
    scr.change_after = 1; /* the text changes right after the first capture */
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.tries == 2 && st.tears > 0 && st.settled && !st.settle_timeout && st.cycles > 0);
    TEST_CHECK(st.cycling_rows == 24);
    TEST_MSG("tries %u tears %u; %u returns on %u rows, %u changes", st.tries, st.tears, st.cycles, st.cycling_rows,
             st.settle_changes);
}

/* FNV-1a of one region row (the job's row checksum), and the 16 bits its
 * ring of earlier states keeps. */
static uint32_t row_sum(const unsigned char *row)
{
    return vjo_fnv1a(VJO_FNV_INIT, row, W);
}

static uint16_t folded(uint32_t sum)
{
    return (uint16_t)(sum ^ (sum >> 16));
}

/* row_a: the background; row_b: it with 3 bytes changed so that its checksum
 * differs but folds to the same 16 bits (172,985 tries). */
static void find_fold_collision(void)
{
    uint32_t a, b = 0;
    memset(scr.row_a, 20, W);
    memcpy(scr.row_b, scr.row_a, W);
    a = row_sum(scr.row_a);
    for (uint32_t v = 0; v < (1u << 24); v++) {
        scr.row_b[100] = (unsigned char)v;
        scr.row_b[101] = (unsigned char)(v >> 8);
        scr.row_b[102] = (unsigned char)(v >> 16);
        b = row_sum(scr.row_b);
        if (b != a && folded(b) == folded(a)) break;
    }
    TEST_ASSERT(b != a && folded(b) == folded(a));
}

/* Without the wait every change is one, exactly as in API 19: a row (outside
 * the line) whose new checksum folds to the same 16 bits as its old one is a
 * change, not a return to an earlier state. */
static void test_without_the_wait_a_folded_match_is_a_change(void)
{
    VjoVocrPolicy zero;
    VjoVocrStats st;
    uint32_t a, b;
    memset(&zero, 0, sizeof(zero));
    reset();
    find_fold_collision();
    a = row_sum(scr.row_a);
    b = row_sum(scr.row_b);
    scr.alt_row = 70;
    scr.alt_from = 4; /* the finder's second pass */
    TEST_CHECK(run(&zero, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.tears == 1 && !st.cycles && !st.cycling_rows && st.tries == 2);
    TEST_MSG("row 70: %08X -> %08X (both fold to %04X): tears %u, returns %u, tries %u", (unsigned)a, (unsigned)b,
             folded(a), st.tears, st.cycles, st.tries);
}

/* Host tools see each line's ink (VjoVocrPolicy.line) with the check off too,
 * and then nothing is skipped: the same lines are recognized as without a
 * callback. Needs the release weights. */
static unsigned seen_lines, seen_blank;
static void count_line(void *ud, const VjoVocrLineInfo *info)
{
    (void)ud;
    seen_lines++;
    seen_blank += info->blank != 0;
}

static void *test_alloc(void *ud, size_t bytes)
{
    void *m = NULL;
    (void)ud;
    return posix_memalign(&m, 16, bytes) ? NULL : m;
}

static int test_release(void *ud)
{
    (void)ud;
    return 0;
}

static void test_the_line_callback_skips_nothing_with_the_check_off(void)
{
    const char *dir = VJO_VOCR_MODEL_DIR;
    VjoVocrPolicy off = {.blank_edges = 20000, .line = count_line}, on = off;
    VjoVocrStats st;
    VjoPlatform p;
    PosixPlatform pp = {0};
    char path[600], text[128];
    FILE *f;
    snprintf(path, sizeof(path), "%s/FL10_w8.vocr", dir);
    if (!dir[0] || !(f = fopen(path, "rb"))) {
        TEST_SKIP("no FL10_w8.vocr in VJO_VOCR_MODEL_DIR");
        return;
    }
    fclose(f);
    posix_platform_init(&pp, &p);
    on.blank = 1; /* the same edges bound with the check on: every line is skipped */
    for (int check = 0; check < 2; check++) {
        VjoVocrResident res = {test_alloc, test_release, NULL, NULL, NULL, 0, 0, 0};
        reset();
        seen_lines = seen_blank = 0;
        int rc = vjo_vocr_ocr_policy(&p, dir, "FL10_w8.vocr", &im, &res, ws_mem, vjo_vocr_job_bytes("FL10_w8.vocr", W, H),
                                     capture, vjo_vocr_capture_bytes(W, H), check ? &on : &off, text, sizeof(text), &st);
        TEST_CHECK(rc == VJO_OK && st.lines == 1 && seen_lines == 1);
        if (check) TEST_CHECK(st.blank == 1 && !st.recognized && seen_blank == 1 && !text[0]);
        else TEST_CHECK(!st.blank && st.recognized == 1 && !seen_blank && st.model_loaded);
        TEST_MSG("check %d: rc %d, %u lines, %u skipped, %u recognized ('%s')", check, rc, st.lines, st.blank,
                 st.recognized, text);
        free(res.data);
    }
}

/* With a sleep, the verification does not read passes too early to count: the
 * same result from fewer strips (the display hook copies fewer rows), and no
 * later. */
static void test_sleep_skips_early_passes(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000};
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
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1, .sleep = sleep_us};
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
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
    VjoVocrStats st;
    reset();
    scr.three_lines = 1;
    scr.text_from = 10;
    scr.clear_at = 25; /* during the finder's second pass after the wait (frames 23-26) */
    TEST_CHECK(run(&pol, &st) == VJO_OK);
    TEST_CHECK(st.tries == 2 && st.tears > 0 && st.lines == 0 && st.cycling_rows == VJO_VOCR_CYCLE_ROWS && !scr.opens);
    TEST_MSG("tries %u tears %u lines %u, cycling rows %u", st.tries, st.tears, st.lines, st.cycling_rows);
}

/* The documented limit (VJO_VOCR_CYCLE_ROWS), pinned so that a change to it is
 * deliberate: the same with a box of two lines, 48 rows, fewer than an icon may
 * take: they are taken for an icon's and the job does not read again; the
 * finder's second pass read the empty box, so the job has fewer lines than the
 * screen it published (here none). */
static void test_a_two_line_box_clearing_back_is_taken_for_an_icon(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_first = 1};
    VjoVocrStats st;
    unsigned published = 0;
    int rc;
    reset();
    scr.two_lines = 1;
    scr.text_from = 10;
    scr.clear_at = 25;
    rc = run(&pol, &st);
    for (unsigned i = 0; i < scr.n_log; i++) published += (scr.log[i].flags & VJO_STRIP_LAST) != 0;
    TEST_CHECK((rc == VJO_OK || rc == VJO_E_OCR_MODEL) && published == 1);
    TEST_CHECK(st.tries == 1 && !st.tears && st.lines < 2 && st.cycling_rows == 48);
    TEST_MSG("rc %d tries %u tears %u lines %u, cycling rows %u", rc, st.tries, st.tears, st.lines, st.cycling_rows);
}

/* Three lines of text that flash (8 frames shown, 8 hidden) while the job
 * waits: their 72 rows go back and forth like an icon's, but at most 64 rows
 * of a job are taken for an icon's; the other 8 are changes, so the screen
 * never holds still and the wait runs out. */
static void test_at_most_64_rows_are_taken_for_an_icon(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000, .settle_max_us = 400000, .settle_first = 1};
    VjoVocrStats st;
    reset();
    scr.three_lines = 1;
    scr.text_blink = 8;
    (void)run(&pol, &st);
    TEST_CHECK(st.cycling_rows == VJO_VOCR_CYCLE_ROWS && st.settle_timeout && !st.settled);
    TEST_MSG("cycling rows %u, timeout %u, waited %u us", st.cycling_rows, st.settle_timeout, (unsigned)st.settle_us);
}

/* With the wait on, a row whose new checksum folds to the 16 bits of its
 * current one is a change too: it is not the current state (the sums differ)
 * and not a return (the ring's copies of that fold are the current state's). */
static uint32_t row_sum(const unsigned char *row);
static uint16_t folded(uint32_t sum);
static void find_fold_collision(void);
static void test_with_the_wait_a_folded_match_is_a_change(void)
{
    VjoVocrPolicy pol = {.settle_us = 150000};
    VjoVocrStats st;
    reset();
    find_fold_collision();
    scr.alt_row = 70;
    scr.alt_from = 4; /* the finder's second pass: read, not waited for */
    TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
    TEST_CHECK(st.tears == 1 && !st.cycles && !st.cycling_rows && st.tries == 2 && st.settled);
    TEST_MSG("tears %u, returns %u on %u rows, tries %u", st.tears, st.cycles, st.cycling_rows, st.tries);
}

/* A clock that never moves (a broken platform clock, a sleep that returns at
 * once): the wait cannot measure a span, and it ends after
 * VJO_VOCR_SETTLE_READS strips as if its time had run out. */
static void test_a_frozen_clock_ends_the_wait(void)
{
    for (int first = 0; first < 2; first++) {
        VjoVocrPolicy pol = {.settle_us = 150000, .settle_max_us = 400000, .settle_first = first, .sleep = sleep_us};
        VjoVocrStats st;
        reset();
        scr.frozen = 1;
        TEST_CHECK(run(&pol, &st) == VJO_E_OCR_MODEL);
        TEST_CHECK(st.settle_timeout && !st.settled && st.strips >= VJO_VOCR_SETTLE_READS &&
                   st.strips < VJO_VOCR_SETTLE_READS + 16u && st.lines == 1);
        TEST_MSG("settle_first %d: %u strips, timeout %u", first, st.strips, st.settle_timeout);
    }
}

/* A 2x upscale with bilinear filtering, as a PSP game's 480x272 frame reaches
 * the Vita's screen: each output pixel 3/4 of its nearer and 1/4 of its farther
 * source neighbour, in each direction (edges clamped). */
static void upscale2(const unsigned char *src, unsigned w, unsigned h, unsigned char *dst)
{
    for (unsigned y = 0; y < 2 * h; y++) {
        unsigned y0 = y / 2, y1 = y % 2 ? (y0 + 1 < h ? y0 + 1 : y0) : (y0 ? y0 - 1 : 0);
        for (unsigned x = 0; x < 2 * w; x++) {
            unsigned x0 = x / 2, x1 = x % 2 ? (x0 + 1 < w ? x0 + 1 : x0) : (x0 ? x0 - 1 : 0);
            unsigned v = 9u * src[y0 * w + x0] + 3u * src[y0 * w + x1] + 3u * src[y1 * w + x0] + src[y1 * w + x1];
            dst[y * 2 * w + x] = (unsigned char)((v + 8u) / 16u);
        }
    }
}

/* The largest change of grey level within `span` pixels, across or down. */
static unsigned max_step_within(const unsigned char *px, unsigned w, unsigned h, unsigned span)
{
    unsigned most = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            for (unsigned s = 1; s <= span; s++) {
                unsigned v = px[y * w + x];
                if (x + s < w) { unsigned o = px[y * w + x + s], d = v > o ? v - o : o - v; if (d > most) most = d; }
                if (y + s < h) { unsigned o = px[(y + s) * w + x], d = v > o ? v - o : o - v; if (d > most) most = d; }
            }
    return most;
}

static unsigned max_step(const unsigned char *px, unsigned w, unsigned h)
{
    return max_step_within(px, w, h, 1);
}

/* A blur of about sigma 1 ([1 4 6 4 1] / 16 across, then down; edges clamped). */
static void soften(unsigned char *px, unsigned w, unsigned h)
{
    static unsigned char tmp[40 * 300];
    static const unsigned k[5] = {1, 4, 6, 4, 1};
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
                unsigned v = 0;
                for (int i = -2; i <= 2; i++) {
                    int a = pass ? (int)y + i : (int)x + i, n = pass ? (int)h : (int)w;
                    a = a < 0 ? 0 : a >= n ? n - 1 : a;
                    v += k[i + 2] * (pass ? px[(unsigned)a * w + x] : px[y * w + (unsigned)a]);
                }
                tmp[y * w + x] = (unsigned char)((v + 8u) / 16u);
            }
        memcpy(px, tmp, (size_t)w * h);
    }
}

/* A platform that refuses one file by name (a missing cascade model). */
static PosixPlatform cascade_pp;
static int (*posix_open)(void *ud, const char *path, VjoFile *out);
static const char *refused_name;
static int refusing_open(void *ud, const char *path, VjoFile *out)
{
    const char *base = strrchr(path, '/');
    if (refused_name && !strcmp(base ? base + 1 : path, refused_name)) return -1;
    return posix_open(ud, path, out);
}

/* Resident memory the test gives back: ud is the resident itself. */
static int free_weights(void *ud)
{
    free(((VjoVocrResident *)ud)->data);
    return 0;
}

static int free_lm(void *ud)
{
    free(((VjoVocrLmResident *)ud)->data);
    return 0;
}

static int have_files(const char *dir, const char *const *names, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        char path[600];
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (!dir[0] || !(f = fopen(path, "rb"))) return 0;
        fclose(f);
    }
    return 1;
}

/* The confidence cascade with the release weights (VOCR3-FL10 reads, VOCR4-W20
 * reads again, the 1 MB LM). A line under the threshold (forced here: every
 * line) shows exactly what the larger model reads alone, with its confidence;
 * the greedy text kept for the log is the first model's; the larger model's
 * weights are read once and then resident. No doubtful line: they are not even
 * opened. Past the time ceiling, a doubtful line is counted and left. Without
 * the LM the confidence is still there and the larger model reads greedily. A
 * missing file: the job reads as without the cascade and says why. */
static void test_the_cascade_reads_doubtful_lines_again(void)
{
    static const char *const files[] = {VJO_VOCR3_FL10_NAME, VJO_VOCR_CASCADE_NAME, VJO_VOCR_LM2_NAME};
    const char *dir = VJO_VOCR_MODEL_DIR;
    const VjoVocrModel *fl10 = vjo_vocr_model(VJO_VOCR3_FL10_NAME), *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME);
    const VjoVocrLm *lm = vjo_vocr_lm_tier(2);
    VjoPlatform p;
    VjoVocrStats st, alone_st;
    char text[256], raw[256], alone[256];
    unsigned char *ws = NULL;
    size_t ws_bytes;
    if (!have_files(dir, files, 3)) { /* before any check: acutest refuses a skip after one */
        TEST_SKIP("no VOCR3-FL10, VOCR4-W20 and 1000k LM in VJO_VOCR_MODEL_DIR");
        return;
    }
    TEST_ASSERT(fl10 && w20 && lm);
    posix_platform_init(&cascade_pp, &p);
    posix_open = p.file_open;
    p.file_open = refusing_open;
    refused_name = NULL;
    ws_bytes = vjo_vocr_job_bytes(w20->name, W, H) + vjo_vocr_lm_job_bytes(fl10->name) +
               vjo_vocr_cascade_job_bytes(w20->name);
    TEST_ASSERT(posix_memalign((void **)&ws, 64, ws_bytes) == 0);
    VjoVocrResident res = {test_alloc, free_weights, NULL, NULL, NULL, 0, 0, 0}, cres = res, wres = res;
    VjoVocrLmResident lres = {test_alloc, free_lm, NULL, NULL, NULL, 0, 0, 0};
    res.ud = &res;
    cres.ud = &cres;
    wres.ud = &wres;
    lres.ud = &lres;
    VjoVocrPolicy pol = {.lm = lm, .lm_res = &lres, .raw = raw, .raw_cap = sizeof(raw), .cascade = w20,
                         .cascade_res = &cres, .cascade_us = 60000000u, .cascade_conf = 1001};
    VjoVocrPolicy w20_pol = {.lm = lm, .lm_res = &lres};
    /* every line read again: the text is VOCR4-W20's own, with its confidence */
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(st.lines == 1 && st.recognized == 1 && st.cascade == w20 && st.cascade_lines == 1 && !st.cascade_rc &&
               st.cascade_loaded && !st.cascade_skipped && cres.model == w20 && st.lm == lm);
    TEST_CHECK((st.line[0].flags & VJO_VOCR_LINE_CASCADE) && (st.line[0].flags & VJO_VOCR_LINE_LM));
    unsigned shown_conf = st.line[0].flags & VJO_VOCR_LINE_CONF;
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, w20->name, &im, &wres, ws, ws_bytes, capture, vjo_vocr_capture_bytes(W, H),
                                    &w20_pol, alone, sizeof(alone), &alone_st) == VJO_OK);
    TEST_CHECK(!strcmp(alone, text) && (alone_st.line[0].flags & VJO_VOCR_LINE_CONF) == shown_conf &&
               !(alone_st.line[0].flags & VJO_VOCR_LINE_CASCADE));
    TEST_MSG("shown '%s' (conf %u); W20 alone '%s' (conf %u); FL10's greedy '%s'", text, shown_conf, alone,
             alone_st.line[0].flags & VJO_VOCR_LINE_CONF, raw);
    /* the next job: its weights resident, not read again */
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(st.cascade_cached && !st.cascade_loaded && st.cascade_lines == 1 && !strcmp(alone, text));
    /* the threshold: a line exactly at it is not doubtful, one under it is */
    {
        VjoVocrPolicy plain = {.lm = lm, .lm_res = &lres};
        unsigned conf;
        reset();
        TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                        vjo_vocr_capture_bytes(W, H), &plain, text, sizeof(text), &st) == VJO_OK);
        conf = st.line[0].flags & VJO_VOCR_LINE_CONF;
        TEST_ASSERT(conf > 0 && conf <= 1000);
        pol.cascade_conf = conf; /* at it: kept */
        reset();
        TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                        vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
        TEST_CHECK(!st.cascade_lines && !(st.line[0].flags & VJO_VOCR_LINE_CASCADE));
        pol.cascade_conf = conf + 1; /* just above it: read again */
        reset();
        TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                        vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
        TEST_CHECK(st.cascade_lines == 1 && (st.line[0].flags & VJO_VOCR_LINE_CASCADE));
        TEST_MSG("FL10's confidence on the line: %u", conf);
        pol.cascade_conf = 1001;
    }
    /* no doubtful line: the larger model's file is not even opened */
    TEST_CHECK(!vjo_vocr_resident_drop(&cres) && !cres.data);
    pol.cascade_conf = 1; /* a confidence of 0 only */
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
    if ((st.line[0].flags & VJO_VOCR_LINE_CONF) >= 1)
        TEST_CHECK(!st.cascade_lines && !st.cascade_loaded && !st.cascade_cached && !cres.data && !st.cascade &&
                   !(st.line[0].flags & VJO_VOCR_LINE_CASCADE) && !strcmp(text, raw) == !(st.line[0].flags & VJO_VOCR_LINE_CHANGED));
    /* past the time ceiling: counted, left, and nothing read for it */
    pol.cascade_conf = 1001;
    pol.cascade_us = 1;
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(st.cascade_skipped == 1 && !st.cascade_lines && !cres.data && !(st.line[0].flags & VJO_VOCR_LINE_CASCADE));
    /* without the LM: the confidence still comes, the larger model reads greedily */
    VjoVocrPolicy greedy = {.cascade = w20, .cascade_res = &cres, .cascade_us = 60000000u, .cascade_conf = 1001};
    VjoVocrPolicy w20_greedy = {0};
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &greedy, text, sizeof(text), &st) == VJO_OK);
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, w20->name, &im, &wres, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &w20_greedy, alone, sizeof(alone), &alone_st) == VJO_OK);
    TEST_CHECK(st.cascade_lines == 1 && !st.lm && !(st.line[0].flags & VJO_VOCR_LINE_LM) &&
               (st.line[0].flags & VJO_VOCR_LINE_CONF) != VJO_VOCR_LINE_CONF_NONE && !strcmp(text, alone));
    TEST_MSG("greedy: shown '%s', W20 alone '%s'", text, alone);
    /* the larger model missing: the job reads with the first one and says why */
    TEST_CHECK(!vjo_vocr_resident_drop(&cres));
    refused_name = VJO_VOCR_CASCADE_NAME;
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                    vjo_vocr_capture_bytes(W, H), &greedy, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(st.cascade_rc == VJO_E_OCR_MODEL && !st.cascade_lines && !st.cascade && st.recognized == 1 && !cres.data);
    refused_name = NULL;
    /* a workspace without the cascade's room: the LM still, the cascade not */
    reset();
    TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws,
                                    vjo_vocr_job_bytes(fl10->name, W, H) + vjo_vocr_lm_job_bytes(fl10->name), capture,
                                    vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(st.cascade_rc == VJO_E_OOM && st.lm == lm && !st.cascade_lines);
    TEST_CHECK(!vjo_vocr_resident_drop(&res) && !vjo_vocr_resident_drop(&wres) && !vjo_vocr_lm_drop(&lres) &&
               !vjo_vocr_resident_drop(&cres));
    free(ws);
}

/* The runtime's calls, wrapped at link time (CMakeLists.txt: --wrap): each goes
 * through to the real one. With wrap.on a test makes the calls take time on the
 * fake clock, makes the job's model read a line as empty or its decoder fail, and
 * counts them. The job's model is the first context a job runs (its first line
 * comes before any re-read), the cascade's the other; the same for the decoders. */
static struct {
    int on;
    const void *model_ctx, *decoder_ctx;
    uint32_t run_us, cascade_run_us;
    unsigned runs, cascade_runs, decodes, cascade_decodes, inits;
    int empty_run; /* this run of the job's model (0, 1, ...) reads nothing; -1: none */
    const char *cascade_text; /* the cascade's runs read this (it reads nothing on the synthetic lines) */
    int decode_rc; /* nonzero: each decode of the job's model writes "X" and returns it */
    long beta[8];  /* the per-character bonus each decoder was planned with */
} wrap;

static void wrap_reset(void)
{
    memset(&wrap, 0, sizeof(wrap));
    wrap.empty_run = -1;
    wrap.on = 1;
}

int __real_vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t text_cap, vocr_extra *extra);
int __wrap_vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t text_cap, vocr_extra *extra);
int __wrap_vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t text_cap, vocr_extra *extra)
{
    int rc = __real_vocr_run(c, img, text, text_cap, extra);
    if (!wrap.on) return rc;
    if (!wrap.model_ctx) wrap.model_ctx = c;
    if (c == wrap.model_ctx) {
        if (rc == VOCR_OK && (int)wrap.runs == wrap.empty_run && text_cap) text[0] = '\0';
        wrap.runs++;
        scr.clock += wrap.run_us;
    } else {
        if (rc == VOCR_OK && wrap.cascade_text && strlen(wrap.cascade_text) < text_cap)
            memcpy(text, wrap.cascade_text, strlen(wrap.cascade_text) + 1);
        wrap.cascade_runs++;
        scr.clock += wrap.cascade_run_us;
    }
    return rc;
}

int __real_vocr_lm_decode(vocr_lm_ctx *c, const vocr_extra *ex, char *text, size_t text_cap, unsigned *chars);
int __wrap_vocr_lm_decode(vocr_lm_ctx *c, const vocr_extra *ex, char *text, size_t text_cap, unsigned *chars);
int __wrap_vocr_lm_decode(vocr_lm_ctx *c, const vocr_extra *ex, char *text, size_t text_cap, unsigned *chars)
{
    if (wrap.on) {
        if (!wrap.decoder_ctx) wrap.decoder_ctx = c;
        if (c != wrap.decoder_ctx) {
            wrap.cascade_decodes++;
        } else {
            wrap.decodes++;
            if (wrap.decode_rc) { /* a decoder that wrote part of a line, then failed */
                if (text_cap > 1) {
                    text[0] = 'X';
                    text[1] = '\0';
                }
                return wrap.decode_rc;
            }
        }
    }
    return __real_vocr_lm_decode(c, ex, text, text_cap, chars);
}

int __real_vocr_lm_init(vocr_lm_ctx *c, const vocr_model *m, const vocr_lm *lm, const vocr_lm_opts *o, void *ws,
                        size_t bytes);
int __wrap_vocr_lm_init(vocr_lm_ctx *c, const vocr_model *m, const vocr_lm *lm, const vocr_lm_opts *o, void *ws,
                        size_t bytes);
int __wrap_vocr_lm_init(vocr_lm_ctx *c, const vocr_model *m, const vocr_lm *lm, const vocr_lm_opts *o, void *ws,
                        size_t bytes)
{
    if (wrap.on) {
        if (wrap.inits < sizeof(wrap.beta) / sizeof(wrap.beta[0])) wrap.beta[wrap.inits] = (long)o->beta;
        wrap.inits++;
    }
    return __real_vocr_lm_init(c, m, lm, o, ws, bytes);
}

static unsigned newlines(const char *s)
{
    unsigned n = 0;
    for (; *s; s++) n += *s == '\n';
    return n;
}

/* The cascade's time ceiling, on the fake clock (frozen: only the wrapped runs
 * move it). Three equal lines, each read in 100 ms by the job's model (FL10) and
 * in 250 ms by VOCR4-W20 (the shapes' cost ratio, 235/94 = 2.5); every line is
 * doubtful. Before re-reading a line the job counts the time spent, the re-read
 * (this line's time x the ratio) and the lines still to read (the time per length
 * so far): at line 0, 100 + 250 + 200 = 550 ms. A ceiling of 550 ms re-reads line
 * 0 and no other (line 1: 450 + 250 + 100 = 800 ms); 549.999 ms, none. A re-read
 * that takes 500 ms raises the ratio to 5, so under 1,100 ms line 1 is left
 * (700 + 500 + 100 ms), which the shapes' ratio (1,050 ms) would re-read. The
 * job's own times leave the re-reads out (recognize_us and each line's us: its
 * frames are the job's model's); they are counted apart, with W20's frames. */
static void test_the_cascade_ceiling_counts_spent_reread_and_rest(void)
{
    static const char *const files[] = {VJO_VOCR3_FL10_NAME, VJO_VOCR_CASCADE_NAME};
    static const struct {
        uint32_t ceiling, cascade_run_us;
        unsigned lines, skipped;
    } cases[] = {{549999u, 250000u, 0, 3}, {550000u, 250000u, 1, 2}, {1100000u, 500000u, 1, 2}};
    const char *dir = VJO_VOCR_MODEL_DIR;
    const VjoVocrModel *fl10 = vjo_vocr_model(VJO_VOCR3_FL10_NAME), *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME);
    VjoPlatform p;
    VjoVocrStats st;
    static char text[1024];
    unsigned char *ws = NULL;
    size_t ws_bytes;
    if (!have_files(dir, files, 2)) {
        TEST_SKIP("no VOCR3-FL10 and VOCR4-W20 in VJO_VOCR_MODEL_DIR");
        return;
    }
    TEST_ASSERT(fl10 && w20);
    posix_platform_init(&cascade_pp, &p);
    p.now_us = now;
    ws_bytes = vjo_vocr_job_bytes(w20->name, W, H) + vjo_vocr_lm_job_bytes(fl10->name) +
               vjo_vocr_cascade_job_bytes(w20->name);
    TEST_ASSERT(posix_memalign((void **)&ws, 64, ws_bytes) == 0);
    VjoVocrResident res = {test_alloc, free_weights, NULL, NULL, NULL, 0, 0, 0}, cres = res;
    res.ud = &res;
    cres.ud = &cres;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoVocrPolicy pol = {.cascade = w20, .cascade_res = &cres, .cascade_us = cases[i].ceiling,
                             .cascade_conf = 1001};
        reset();
        scr.three_lines = 1;
        scr.frozen = 1;
        wrap_reset();
        wrap.run_us = 100000u;
        wrap.cascade_run_us = cases[i].cascade_run_us;
        TEST_ASSERT(vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture,
                                        vjo_vocr_capture_bytes(W, H), &pol, text, sizeof(text), &st) == VJO_OK);
        TEST_ASSERT(st.lines == 3 && st.recognized == 3);
        for (unsigned k = 1; k < 3; k++) /* three equal lines: the arithmetic above */
            TEST_ASSERT(st.line[k].width == st.line[0].width && st.line[k].height == st.line[0].height);
        TEST_CHECK(st.cascade_lines == cases[i].lines && st.cascade_skipped == cases[i].skipped &&
                   wrap.cascade_runs == cases[i].lines);
        TEST_CHECK(!(st.line[0].flags & VJO_VOCR_LINE_CASCADE) == !cases[i].lines &&
                   !(st.line[1].flags & VJO_VOCR_LINE_CASCADE) && !(st.line[2].flags & VJO_VOCR_LINE_CASCADE));
        TEST_CHECK(st.recognize_us == 300000u && st.cascade_us == cases[i].lines * cases[i].cascade_run_us);
        for (unsigned k = 0; k < 3; k++) TEST_CHECK(st.line[k].us == 100000u);
        TEST_CHECK(!cases[i].lines == !st.cascade_frames);
        TEST_MSG("ceiling %u us: %u read again, %u left; recognize %u us, cascade %u us (%u frames)",
                 (unsigned)cases[i].ceiling, st.cascade_lines, st.cascade_skipped, (unsigned)st.recognize_us,
                 (unsigned)st.cascade_us, st.cascade_frames);
    }
    wrap.on = 0;
    TEST_CHECK(!vjo_vocr_resident_drop(&res) && !vjo_vocr_resident_drop(&cres));
    free(ws);
}

/* The LM's paths, with the wrapped runtime and the release weights: the larger
 * model's re-read is decoded with the LM too; a line read as empty is not
 * decoded; a decode that fails leaves the greedy line (one that does not fit
 * keeps the LM on, any other error turns it off for the job); the greedy text
 * kept for the log has the shown text's line breaks; each tier is planned with
 * its own per-character bonus (500k 3000, 1000k 3500). */
static void test_the_lm_paths(void)
{
    static const char *const files[] = {VJO_VOCR3_FL10_NAME, VJO_VOCR_CASCADE_NAME, VJO_VOCR_LM1_NAME,
                                        VJO_VOCR_LM2_NAME};
    const char *dir = VJO_VOCR_MODEL_DIR;
    const VjoVocrModel *fl10 = vjo_vocr_model(VJO_VOCR3_FL10_NAME), *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME);
    const VjoVocrLm *lm1 = vjo_vocr_lm_tier(1), *lm2 = vjo_vocr_lm_tier(2);
    VjoPlatform p;
    VjoVocrStats st;
    static char text[1024], raw[1024];
    unsigned char *ws = NULL;
    size_t ws_bytes;
    if (!have_files(dir, files, 4)) {
        TEST_SKIP("no VOCR3-FL10, VOCR4-W20 and the 500k and 1000k LMs in VJO_VOCR_MODEL_DIR");
        return;
    }
    TEST_ASSERT(fl10 && w20 && lm1 && lm2 && lm1->beta != lm2->beta);
    posix_platform_init(&cascade_pp, &p);
    p.file_open = posix_open ? posix_open : p.file_open;
    ws_bytes = vjo_vocr_job_bytes(w20->name, W, H) + vjo_vocr_lm_job_bytes(fl10->name) +
               vjo_vocr_cascade_job_bytes(w20->name);
    TEST_ASSERT(posix_memalign((void **)&ws, 64, ws_bytes) == 0);
    VjoVocrResident res = {test_alloc, free_weights, NULL, NULL, NULL, 0, 0, 0}, cres = res;
    VjoVocrLmResident lres = {test_alloc, free_lm, NULL, NULL, NULL, 0, 0, 0};
    res.ud = &res;
    cres.ud = &cres;
    lres.ud = &lres;
    VjoVocrPolicy casc = {.lm = lm2, .lm_res = &lres, .raw = raw, .raw_cap = sizeof(raw), .cascade = w20,
                          .cascade_res = &cres, .cascade_us = 60000000u, .cascade_conf = 1001};
    VjoVocrPolicy plain = {.lm = lm2, .lm_res = &lres, .raw = raw, .raw_cap = sizeof(raw)};
#define RUN(pol) vjo_vocr_ocr_policy(&p, dir, fl10->name, &im, &res, ws, ws_bytes, capture, \
                                     vjo_vocr_capture_bytes(W, H), (pol), text, sizeof(text), &st)
    /* one line read again (W20 reading "\xE3\x81\x82"): decoded by the job's model's decoder
     * and by the cascade's */
    reset();
    wrap_reset();
    wrap.cascade_text = "\xE3\x81\x82";
    TEST_ASSERT(RUN(&casc) == VJO_OK);
    TEST_CHECK(st.cascade_lines == 1 && wrap.cascade_runs == 1 && wrap.decodes == 1 && wrap.cascade_decodes == 1);
    TEST_CHECK(wrap.inits == 2 && wrap.beta[0] == lm2->beta && wrap.beta[1] == lm2->beta);
    TEST_MSG("%u + %u decodes, %u decoders planned", wrap.decodes, wrap.cascade_decodes, wrap.inits);
    /* W20 reads the synthetic line as nothing where FL10 read text: the line leaves the text
     * shown (its greedy reading the log), and the job counts it */
    reset();
    wrap_reset();
    TEST_ASSERT(RUN(&casc) == VJO_OK);
    TEST_CHECK(st.cascade_lines == 1 && !text[0] && !raw[0] && st.cascade_blanked == 1);
    TEST_MSG("shown '%s', greedy kept '%s', %u blanked", text, raw, st.cascade_blanked);
    /* three lines, the second read as empty: two decoded, two shown, two logged */
    reset();
    scr.three_lines = 1;
    wrap_reset();
    wrap.empty_run = 1;
    TEST_ASSERT(RUN(&plain) == VJO_OK);
    TEST_CHECK(st.recognized == 3 && wrap.decodes == 2 && st.lm_lines == 2 && newlines(text) == 1 &&
               newlines(raw) == 1);
    /* three lines: the greedy text kept has the shown text's line breaks */
    reset();
    scr.three_lines = 1;
    wrap_reset();
    TEST_ASSERT(RUN(&plain) == VJO_OK);
    TEST_CHECK(st.lm_lines == 3 && newlines(text) == 2 && newlines(raw) == 2);
    TEST_MSG("shown '%s', greedy '%s'", text, raw);
    /* every decode too long for its room: the greedy lines stay, the LM stays on */
    reset();
    scr.three_lines = 1;
    wrap_reset();
    wrap.decode_rc = VOCR_E_TEXT;
    TEST_ASSERT(RUN(&plain) == VJO_OK);
    TEST_CHECK(!strcmp(text, raw) && !strchr(text, 'X') && wrap.decodes == 3 && !st.lm_lines && !st.lm_rc);
    /* another decoder error: the greedy line stays and the job decodes no more */
    reset();
    scr.three_lines = 1;
    wrap_reset();
    wrap.decode_rc = VOCR_E_ARG;
    TEST_ASSERT(RUN(&plain) == VJO_OK);
    TEST_CHECK(!strcmp(text, raw) && !strchr(text, 'X') && wrap.decodes == 1 && !st.lm_lines &&
               st.lm_rc == VJO_E_OCR_INFERENCE);
    /* the 500k tier: planned with its own bonus */
    TEST_CHECK(!vjo_vocr_lm_drop(&lres) && !lres.data);
    VjoVocrPolicy small = {.lm = lm1, .lm_res = &lres};
    reset();
    wrap_reset();
    TEST_ASSERT(RUN(&small) == VJO_OK);
    TEST_CHECK(st.lm == lm1 && wrap.inits == 1 && wrap.beta[0] == lm1->beta);
#undef RUN
    wrap.on = 0;
    TEST_CHECK(!vjo_vocr_resident_drop(&res) && !vjo_vocr_resident_drop(&cres) && !vjo_vocr_lm_drop(&lres));
    free(ws);
}

/* ---- the finder's boxes refined (VjoVocrPolicy.refine) ---- */
#define RW 480u
#define RH 300u
static unsigned char refine_rgba[RH * RW * 4], refine_grey[RH * RW];
static unsigned refine_w, refine_h;
static VocrLine parts_got[VJO_VOCR_MAX_LINES];
static unsigned parts_n;
static uint32_t refine_rng = 0x5eed1234u;

static unsigned refine_below(unsigned n)
{
    refine_rng ^= refine_rng << 13;
    refine_rng ^= refine_rng >> 17;
    refine_rng ^= refine_rng << 5;
    return n ? refine_rng % n : 0u;
}

static void refine_px(unsigned x, unsigned y, unsigned v)
{
    unsigned char *q = refine_rgba + ((size_t)y * RW + x) * 4;
    q[0] = q[1] = q[2] = (unsigned char)v;
    q[3] = 255;
    refine_grey[(size_t)y * RW + x] = (unsigned char)vjo_grey(v, v, v);
}

/* A procedural dialogue region (as vita-vn-ocr's own refinement test draws
 * them): a plain or gradient ground, a frame or a panel, rows of glyph-like
 * strokes (some set close together), cursor-like blobs and portrait-like slabs. */
static void refine_draw(unsigned w, unsigned h)
{
    unsigned x, y, i, lines = 1u + refine_below(5), boxes = refine_below(3), blobs = refine_below(3);
    unsigned paper = refine_below(256), ink = (paper + 96u + refine_below(64)) & 255u;
    int gradient = refine_below(3) == 0;
    refine_w = w;
    refine_h = h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) refine_px(x, y, gradient ? (paper + x * 64u / w + y) & 255u : paper);
    for (i = 0; i < boxes; i++) {
        unsigned x0 = refine_below(w), y0 = refine_below(h), bw = refine_below(w - x0) + 1u, bh = refine_below(h - y0) + 1u;
        unsigned v = refine_below(256), fill = refine_below(2);
        for (y = y0; y < y0 + bh; y++)
            for (x = x0; x < x0 + bw; x++)
                if (fill || y == y0 || y + 1u == y0 + bh || x == x0 || x + 1u == x0 + bw) refine_px(x, y, v);
    }
    for (i = 0; i < lines; i++) {
        unsigned lh = 8u + refine_below(30), y0 = refine_below(h), x0 = refine_below(w), x1, pitch = 4u + refine_below(20);
        unsigned v = refine_below(3) ? ink : refine_below(256);
        if (y0 + lh > h) lh = h - y0;
        x1 = x0 + refine_below(w - x0 + 1u);
        for (y = y0; y < y0 + lh; y++)
            for (x = x0; x < x1; x++) {
                unsigned cx = (x - x0) % pitch, cy = y - y0;
                if (cx < pitch * 3u / 4u && (cx % 4u < 2u || cy % 7u < 2u || (cx + cy) % 9u == 0u)) refine_px(x, y, v);
            }
    }
    for (i = 0; i < blobs; i++) {
        unsigned bw = 3u + refine_below(30), bh = 3u + refine_below(h), x0 = refine_below(w), y0 = refine_below(h);
        unsigned v = refine_below(256);
        for (y = y0; y < y0 + bh && y < h; y++)
            for (x = x0; x < x0 + bw && x < w; x++) refine_px(x, y, v);
    }
}

/* A plain region of one row of strokes, for the tests that need just a line. */
static void refine_clear(unsigned w, unsigned h)
{
    refine_w = w;
    refine_h = h;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) refine_px(x, y, 20);
}

static void refine_text(unsigned y0, unsigned rows, unsigned x0, unsigned x1)
{
    for (unsigned y = y0; y < y0 + rows; y++)
        for (unsigned x = x0; x < x1; x++) {
            unsigned k = (x - x0) % 13, r = y - y0;
            if (k < 2 || (k < 10 && (r == rows / 2 || r == 1)) || (k == 6 && r % 3 == 0)) refine_px(x, y, 235);
        }
}

static unsigned char refine_line_mem[RH * RW];
static void *refine_line_buffer(void *ud, size_t bytes)
{
    (void)ud;
    return bytes <= sizeof(refine_line_mem) ? refine_line_mem : NULL;
}

static void refine_record(void *ud, const VjoVocrLineInfo *info)
{
    (void)ud;
    if (parts_n < VJO_VOCR_MAX_LINES) parts_got[parts_n] = (VocrLine){info->x, info->y, info->w, info->h, info->dark};
    parts_n++;
}

/* The job's lines with refinement steps `flags` (every line skipped as blank:
 * no weights needed), and the runtime's own whole-set refinement of the
 * in-memory finder's lines on the same pixels. Returns the job's rc. */
static int refine_job(unsigned flags, VocrLineSet *whole, unsigned *found, VjoVocrStats *st)
{
    static unsigned char arena[600 * 1024] __attribute__((aligned(16)));
    VjoRgbaImage rgba = {refine_rgba, refine_w, refine_h, (size_t)RW * 4};
    VjoGreySource src;
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    VjoVocrPolicy pol = {.blank = 1, .blank_edges = 20000, .line = refine_record, .refine = flags};
    VocrLinesImage gim = {refine_grey, refine_w, refine_h, RW, VOCR_LINES_GREY};
    size_t job = vjo_vocr_job_bytes("FL10_w8.vocr", refine_w, refine_h) + vjo_vocr_refine_bytes(refine_w, refine_h);
    unsigned char *ws = NULL;
    char text[64];
    int rc;
    vjo_grey_source_rgba(&rgba, &src);
    src.line_buffer = refine_line_buffer;
    p.file_open = no_file;
    p.now_us = now;
    TEST_ASSERT(posix_memalign((void **)&ws, 64, job) == 0);
    parts_n = 0;
    rc = vjo_vocr_ocr_policy(&p, "missing", "FL10_w8.vocr", &src, &res, ws, job, capture,
                             vjo_vocr_capture_bytes(refine_w, refine_h), &pol, text, sizeof(text), st);
    free(ws);
    TEST_ASSERT(vocr_lines_arena_bytes(refine_w, refine_h) <= sizeof(arena));
    TEST_CHECK(vocr_find_lines(&gim, VOCR_LINES_GRAD, arena, sizeof(arena), whole) == VOCR_LINES_OK);
    *found = whole->count;
    if (flags) TEST_CHECK(vocr_refine_lines(&gim, flags, arena, sizeof(arena), whole, NULL) == VOCR_LINES_OK);
    return rc;
}

static int same_lines(const VocrLineSet *a, const VocrLine *b, unsigned nb)
{
    if (a->count != nb) return 0;
    for (unsigned i = 0; i < nb; i++)
        if (memcmp(&a->lines[i], &b[i], sizeof(b[i]))) return 0;
    return 1;
}

/* Each box refined on the pixels the job holds for it (its strip, the box made
 * strip-relative; or a tall line's own block) gives exactly the lines the
 * runtime's whole-set refinement gives on the whole region, over 300 procedural
 * regions of one strip and of several (boxes across strips, taller than one);
 * and it does refine (boxes tightened, split). Off, the lines are the finder's,
 * exactly, and nothing is read for it. */
static void test_refined_boxes_match_the_whole_set_refinement(void)
{
    unsigned regions = 0, changed = 0, split = 0, tall = 0, kept = 0, mismatch = 0;
    for (unsigned t = 0; t < 300; t++) {
        VocrLineSet whole, plain;
        VjoVocrStats st;
        unsigned found = 0, w = 64u + refine_below(RW - 63u), h = 24u + refine_below(RH - 23u);
        refine_draw(w, h);
        TEST_CHECK(refine_job(0, &plain, &found, &st) == VJO_OK);
        if (!found) continue;
        regions++;
        TEST_CHECK(same_lines(&plain, parts_got, parts_n) && !st.refine_boxes && !st.refine_rows);
        TEST_CHECK(refine_job(VJO_VOCR_REFINE_ALL, &whole, &found, &st) == VJO_OK);
        tall += st.tall;
        if (st.refine_kept) { /* a split past 16 lines: the box kept (the whole set caps it otherwise) */
            kept++;
            continue;
        }
        if (!same_lines(&whole, parts_got, parts_n)) {
            mismatch++;
            TEST_MSG("region %u (%ux%u): %u lines refined per box, %u by the whole set", t, w, h, parts_n, whole.count);
        }
        TEST_CHECK(st.refine_lines == parts_n && st.lines == found && !st.refine_rc);
        changed += !same_lines(&plain, parts_got, parts_n);
        split += st.refine_split;
    }
    TEST_CHECK(!mismatch && regions >= 200 && changed >= 30 && split >= 5);
    TEST_MSG("%u regions with lines: %u refined differently from the finder, %u boxes split, %u tall lines, "
             "%u kept for the line cap, %u mismatches", regions, changed, split, tall, kept, mismatch);
    if (getenv("REFINE_DEBUG"))
        printf("refine: %u regions with lines, %u refined differently, %u split, %u tall lines, %u kept, %u mismatches\n",
               regions, changed, split, tall, kept, mismatch);
}

/* A workspace without the refinement's room: the boxes as found, and why. */
static void test_refinement_without_room_reads_the_boxes_as_found(void)
{
    VjoRgbaImage rgba = {refine_rgba, RW, 60, (size_t)RW * 4};
    VjoGreySource src;
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    VjoVocrPolicy pol = {.blank = 1, .blank_edges = 20000, .line = refine_record, .refine = VJO_VOCR_REFINE_ALL};
    VjoVocrStats st;
    char text[64];
    refine_clear(RW, 60);
    refine_text(18, 22, 30, 420);
    vjo_grey_source_rgba(&rgba, &src);
    p.file_open = no_file;
    p.now_us = now;
    parts_n = 0;
    TEST_CHECK(vjo_vocr_ocr_policy(&p, "missing", "FL10_w8.vocr", &src, &res, ws_mem, vjo_vocr_job_bytes("FL10_w8.vocr", RW, 60),
                                   capture, vjo_vocr_capture_bytes(RW, 60), &pol, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(st.refine_rc == VJO_E_OOM && !st.refine_boxes && parts_n == st.lines && st.lines == 1);
    TEST_CHECK(vjo_vocr_refine_bytes(RW, 60) > 0 && vjo_vocr_refine_bytes(0, 60) == 0 &&
               vjo_vocr_refine_bytes(961, 60) == 0);
}

/* The ink statistics: an even area or a smooth ramp holds no text-like ink;
 * strokes do, also small, dark on light, and soft (shown 2x with bilinear
 * filtering, or blurred); under 24 levels of contrast nothing is text. */
static void test_ink_and_blank(void)
{
    static unsigned char px[40 * 300], small[20 * 150];
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
    /* PSP text: anti-aliased strokes at 480x272, on the Vita's screen 2x with
     * bilinear filtering: no step between neighbouring pixels reaches half the
     * contrast any more (the rule of one-pixel steps at contrast / 2 saw no
     * edge at all), yet it is text */
    memset(small, 20, sizeof(small));
    for (unsigned x = 5; x < 145; x += 6) {
        for (unsigned y = 4; y < 16; y++) {
            small[y * 150 + x] = 235;
            small[y * 150 + x - 1] = small[y * 150 + x + 1] = 127;
        }
        for (unsigned d = 0; d < 3; d++) small[3 * 150 + x - 1 + d] = small[16 * 150 + x - 1 + d] = 127;
    }
    upscale2(small, 150, 20, px);
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 0 && ink.edges >= 400);
    TEST_CHECK(max_step(px, 300, 40) < (unsigned)(ink.hi - ink.lo) / 2u);
    TEST_MSG("PSP strokes shown 2x: contrast %u, largest step %u, edge %u, %u edges per 10,000", ink.hi - ink.lo,
             max_step(px, 300, 40), ink.edge, ink.edges);
    /* anti-aliased strokes blurred (sigma about 1: a soft font, a filter): the same */
    memset(px, 20, sizeof(px));
    for (unsigned y = 8; y < 32; y++)
        for (unsigned x = 10; x < 290; x += 12) {
            px[y * 300 + x] = px[y * 300 + x + 1] = 235;
            px[y * 300 + x - 1] = px[y * 300 + x + 2] = 127;
        }
    soften(px, 300, 40);
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 0 && ink.edges >= 400);
    TEST_CHECK(max_step(px, 300, 40) < (unsigned)(ink.hi - ink.lo) / 2u);
    TEST_MSG("blurred strokes: contrast %u, largest step %u, edge %u, %u edges per 10,000", ink.hi - ink.lo,
             max_step(px, 300, 40), ink.edge, ink.edges);
    /* strokes that rise over six pixels, across and down (a sixth of the contrast
     * a pixel): within two pixels a third, so an edge at a quarter of the contrast
     * and none at a half; text (a soft, large font scaled down) */
    {
        static const unsigned sixths[24] = {0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0};
        for (unsigned y = 0; y < 40; y++) {
            unsigned fy = y < 4 || y >= 36 ? 0 : y < 10 ? y - 3 : y >= 30 ? 36 - y : 6;
            for (unsigned x = 0; x < 300; x++)
                px[y * 300 + x] = (unsigned char)(20 + (420u * sixths[x % 24] * fy + 36u) / 72u);
        }
        TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 0 && ink.edges >= 400);
        TEST_CHECK(max_step_within(px, 300, 40, VJO_VOCR_INK_SPAN) < (unsigned)(ink.hi - ink.lo) / 2u);
        TEST_MSG("ramped strokes: contrast %u, largest change within %u pixels %u, edge %u, %u edges per 10,000",
                 ink.hi - ink.lo, VJO_VOCR_INK_SPAN, max_step_within(px, 300, 40, VJO_VOCR_INK_SPAN), ink.edge,
                 ink.edges);
    }
    for (unsigned y = 0; y < 40; y++) /* a smooth wave, eight times as steep as the ramp: no edges */
        for (unsigned x = 0; x < 300; x++) {
            unsigned t = x % 74;
            px[y * 300 + x] = (unsigned char)(20 + (t < 37 ? t : 74 - t) * 200 / 37);
        }
    soften(px, 300, 40);
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 1 && ink.hi - ink.lo > 150 && !ink.edges);
    TEST_MSG("a smooth wave: contrast %u, edge %u, %u edges per 10,000", ink.hi - ink.lo, ink.edge, ink.edges);
    memset(px, 20, sizeof(px)); /* one small mark: "。" alone in its box */
    for (unsigned y = 16; y < 24; y++)
        for (unsigned x = 16; x < 24; x++) px[y * 40 + x] = (y == 16 || y == 23 || x == 16 || x == 23) ? 230 : 20;
    TEST_CHECK(vjo_vocr_ink(px, 40, 40, 40, &ink) == 0 && ink.hi == ink.lo); /* under 2% of the pixels */
    TEST_MSG("a mark in a 40x40 box: %u edges per 10,000", ink.edges);
    memset(px, 100, sizeof(px)); /* strokes of 20 levels: under the contrast floor */
    for (unsigned y = 8; y < 32; y++)
        for (unsigned x = 10; x < 290; x += 12) px[y * 300 + x] = 120;
    TEST_CHECK(vjo_vocr_ink(px, 300, 300, 40, &ink) == 1 && ink.edge == VJO_VOCR_EDGE_MIN && !ink.edges);
    /* exact counts: one bright column in a 97 x 10 crop (too few pixels to move
     * the percentiles: edge 24): across, the two pixels left of it (one and two
     * steps away) and the column itself, 3 a row; 30 * 10,000 / 970 = 309.3 */
    memset(px, 20, 97 * 10);
    for (unsigned y = 0; y < 10; y++) px[y * 97 + 50] = 235;
    TEST_CHECK(vjo_vocr_ink(px, 97, 97, 10, &ink) == 0 && ink.edge == VJO_VOCR_EDGE_MIN && ink.edges == 309);
    TEST_MSG("one column: edge %u, %u edges per 10,000", ink.edge, ink.edges);
    for (unsigned y = 0; y < 10; y++) px[y * 97 + 50] = 20; /* one dot: 3 across, 3 down (rows 3-5) */
    px[5 * 97 + 50] = 235;
    TEST_CHECK(vjo_vocr_ink(px, 97, 97, 10, &ink) == 0 && ink.edges == 61); /* 6 * 10,000 / 970 = 61.9: a mark */
    TEST_MSG("one dot: %u edges per 10,000", ink.edges);
    {   /* more pixels than any line: never called blank, nothing measured */
        unsigned char *big = calloc(960u * 4400u, 1);
        TEST_ASSERT(big != NULL);
        TEST_CHECK(vjo_vocr_ink(big, 960, 960, 4400, &ink) == 0 && !ink.edges && !ink.hi && !ink.edge);
        TEST_CHECK(vjo_vocr_ink(big, 960, 960, 4369, &ink) == 1); /* 4,194,240 pixels: measured, even */
        free(big);
    }
    TEST_CHECK(vjo_vocr_ink(NULL, 300, 300, 40, &ink) == 0 && vjo_vocr_ink(px, 300, 0, 40, &ink) == 0);
    TEST_CHECK(vjo_vocr_ink(px, 961, 961, 1, &ink) == 0); /* wider than any region: never called blank */
}

/* In a job: lines the check skips are not recognized, and a region of skipped
 * lines opens no weights; without the check the same line opens them. */
static void test_blank_lines_open_no_weights(void)
{
    VjoVocrPolicy all = {.blank = 1, .blank_edges = 20000}, real = {.blank = 1};
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
    {"a_two_line_box_clearing_back_is_taken_for_an_icon", test_a_two_line_box_clearing_back_is_taken_for_an_icon},
    {"at_most_64_rows_are_taken_for_an_icon", test_at_most_64_rows_are_taken_for_an_icon},
    {"with_the_wait_a_folded_match_is_a_change", test_with_the_wait_a_folded_match_is_a_change},
    {"a_frozen_clock_ends_the_wait", test_a_frozen_clock_ends_the_wait},
    {"the_last_reread_starts_the_span_after_the_first_pass", test_the_last_reread_starts_the_span_after_the_first_pass},
    {"the_wait_ends_on_the_first_pass_due", test_the_wait_ends_on_the_first_pass_due},
    {"the_waiting_budget_is_the_jobs_not_each_waits", test_the_waiting_budget_is_the_jobs},
    {"a_three_state_icon_settles", test_a_three_state_icon_settles},
    {"a_retry_keeps_what_the_job_learned", test_a_retry_keeps_what_the_job_learned},
    {"without_the_wait_a_folded_match_is_a_change", test_without_the_wait_a_folded_match_is_a_change},
    {"the_line_callback_skips_nothing_with_the_check_off", test_the_line_callback_skips_nothing_with_the_check_off},
    {"the_cascade_reads_doubtful_lines_again", test_the_cascade_reads_doubtful_lines_again},
    {"the_cascade_ceiling_counts_the_time_spent_the_reread_and_the_rest", test_the_cascade_ceiling_counts_spent_reread_and_rest},
    {"the_lm_decodes_rereads_skips_empty_lines_and_keeps_greedy_on_failure", test_the_lm_paths},
    {"refined_boxes_match_the_whole_set_refinement", test_refined_boxes_match_the_whole_set_refinement},
    {"refinement_without_room_reads_the_boxes_as_found", test_refinement_without_room_reads_the_boxes_as_found},
    {"ink_statistics_and_the_blank_rule", test_ink_and_blank},
    {"blank_lines_are_skipped_and_open_no_weights", test_blank_lines_open_no_weights},
    {NULL, NULL}
};
