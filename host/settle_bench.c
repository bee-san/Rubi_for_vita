/* What the stable-text wait (core/vocr_ocr.h VjoVocrPolicy.settle_us) costs and
 * buys, on a simulated game screen: every strip read is one frame of the game
 * (the display hook copies the rows from the next frame it shows), at 60 or 30
 * frames per second. For each region size and screen (still; still but not yet
 * settled when the job is asked; text typing out; a blinking icon) it runs the
 * job without weights (it stops where the model would load, so the number is
 * the capture and the wait, not recognition) and prints one JSON line: frames
 * read and simulated ms until the lines would be recognized, the wait's passes
 * and whether the line was read whole. Host estimate: the console's frame
 * pacing and strip time are unmeasured (#81).
 *
 *   vjo-settle-bench [SETTLE_MS]
 *
 * The stutter workstream (docs/stutter.md): every strip is also a frame whose
 * display call the game loses to a copy, so the bench counts them, their rows
 * and the frames the job spans, with the wait's passes paced
 * (VjoVocrPolicy.settle_pace_us):
 *
 *   vjo-settle-bench --pace [SETTLE_MS]    the paces 0, 50, 75, 100, 150 and 200 ms on CLANNAD's
 *                                          852 x 135 box: still, a box that animates for 1 s after
 *                                          its line (first-ocr.md's 13 rows through new states),
 *                                          typing, a blinking icon; asked settled and not
 *   vjo-settle-bench --pauses [PACE_MS [FRAME_US]]  text-stability.md's sweep at a pace: the longest
 *                                          typewriter pause still read whole (FRAME_US: a 60 fps frame's
 *                                          time, 16,683 for a 59.94 Hz console; default 1 s / 60)
 *   vjo-settle-bench --replay [SETTLE_MS]  one job per stdin line "W H FPS FIRST ANIM_MS PACE_MS"
 *                                          (FIRST 1: not settled when asked; ANIM_MS: the box's
 *                                          rows change to new states until then, then hold still),
 *                                          one JSON line each (tools/stutter_model.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vocr_ocr.h"

typedef struct {
    unsigned w, h, frame_us, frame, type_every, chars, blink_every, flagged_at, first_frame, pause, at_last;
    uint64_t clock;
    /* the stutter scenarios: rows [anim_y, anim_y + anim_rows) take a new state every frame until anim_us */
    unsigned anim_rows, anim_y;
    uint64_t anim_us;
    /* every strip call: one frame of the game with a copy in its display call */
    unsigned calls, rows_max, sleeps;
    unsigned long rows_copied;
} Sim;

static Sim sim;
static unsigned char cap[64 * 1024];
static unsigned char ws[512 * 1024] __attribute__((aligned(64)));

static uint64_t now(void *ud)
{
    (void)ud;
    return sim.clock;
}

/* The worker's sleep (sceKernelDelayThread): the game shows frames meanwhile. */
static void sleep_us(uint32_t us)
{
    sim.clock += us;
    sim.frame += us / sim.frame_us;
    sim.sleeps++;
}

static int no_file(void *ud, const char *path, VjoFile *out)
{
    (void)ud; (void)path; (void)out;
    return -1;
}

static unsigned shown(unsigned frame)
{
    if (sim.pause) /* 12 shown when the job is asked, the rest from frame pause, one per 3 frames */
        return frame < sim.pause ? 12u : 12u + (frame - sim.pause) / 3u + 1u < sim.chars
                                           ? 12u + (frame - sim.pause) / 3u + 1u : sim.chars;
    return !sim.type_every ? sim.chars : frame / sim.type_every < sim.chars ? frame / sim.type_every : sim.chars;
}

/* A line of glyph-like strokes, 12 px a character, in the box's middle rows. */
static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    unsigned top = sim.h / 2 - 12;
    (void)ud;
    sim.clock += sim.frame_us;
    sim.frame++;
    sim.calls++;
    sim.rows_copied += n;
    if (n > sim.rows_max) sim.rows_max = n;
    if ((flags & VJO_STRIP_FIRST) && !y && !sim.first_frame) sim.first_frame = sim.frame;
    if (flags & VJO_STRIP_LAST) sim.at_last = sim.frame; /* the published capture's (the last one) */
    for (unsigned r = 0; r < n; r++) {
        unsigned row = y + r;
        unsigned char *d = dst + (size_t)r * stride;
        memset(d, 20, sim.w);
        if (sim.anim_rows && row >= sim.anim_y && row < sim.anim_y + sim.anim_rows) {
            /* the box's animation: a mark that moves on every frame (new states, never an earlier
             * one), then stops where it is */
            uint64_t t = sim.clock < sim.anim_us ? sim.clock : sim.anim_us;
            unsigned x = 20u + (unsigned)((t / sim.frame_us) * 7u % (sim.w - 60u));
            for (unsigned k = 0; k < 24u; k++) d[x + k] = 180;
        }
        if (row < top || row >= top + 24) continue;
        for (unsigned x = 20; x + 2 < sim.w; x++) {
            unsigned c = (x - 20) / 12, k = (x - 20) % 12;
            if (c < shown(sim.frame) && k < 2) d[x] = 235;
        }
        if (sim.blink_every && (sim.frame / sim.blink_every) % 2)
            for (unsigned x = sim.w - 30; x < sim.w - 14; x++) d[x] = 200;
    }
    return (int)n;
}

typedef struct {
    unsigned type_every, blink_every, pause;
    unsigned anim_rows, anim_ms;
    unsigned pace_ms;
} Screen;

static unsigned frame_us_override; /* --pauses' third argument: the console's 16,683 us (59.94 Hz), say */

static int job(unsigned w, unsigned h, unsigned fps, unsigned settle_ms, int first, const Screen *sc, VjoVocrStats *st,
               char *text, size_t cap_text)
{
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    VjoVocrPolicy pol = {.settle_us = settle_ms * 1000u, .settle_first = first, .sleep = sleep_us,
                         .settle_pace_us = sc->pace_ms * 1000u};
    VjoGreySource im = {NULL, w, h, strip, NULL, NULL};
    memset(&sim, 0, sizeof(sim));
    sim.w = w;
    sim.h = h;
    sim.frame_us = frame_us_override ? frame_us_override * 60u / fps : 1000000u / fps;
    sim.chars = (w - 40) / 12 < 25 ? (w - 40) / 12 : 25;
    sim.type_every = sc->type_every;
    sim.blink_every = sc->blink_every;
    sim.pause = sc->pause;
    sim.anim_rows = sc->anim_rows;
    sim.anim_y = h > sc->anim_rows + 4u ? h - sc->anim_rows - 4u : 0u; /* the box's lower edge, below the line */
    sim.anim_us = (uint64_t)sc->anim_ms * 1000u;
    p.file_open = no_file;
    p.now_us = now;
    return vjo_vocr_ocr_policy(&p, "none", "FL10_w8.vocr", &im, &res, ws, vjo_vocr_job_bytes("FL10_w8.vocr", w, h), cap,
                               vjo_vocr_capture_bytes(w, h), settle_ms ? &pol : NULL, text, cap_text, st);
}

static void run(const char *screen, unsigned w, unsigned h, unsigned fps, unsigned settle_ms, int first,
                unsigned type_every, unsigned blink_every, unsigned pause)
{
    Screen sc = {type_every, blink_every, pause, 0, 0, 0};
    VjoVocrStats st;
    char text[64];
    int rc = job(w, h, fps, settle_ms, first, &sc, &st, text, sizeof(text));
    printf("{\"screen\": \"%s\", \"region\": \"%ux%u\", \"strips_per_pass\": %u, \"fps\": %u, \"settle_ms\": %u, "
           "\"wait_first\": %d, \"rc\": %d, \"frames\": %u, \"ms\": %u, \"first_pass_frame\": %u, "
           "\"typed_by_frame\": %u, \"chars_at_first_pass\": %u, \"of\": %u, \"settle_passes\": %u, \"settle_ms_spent\": %u, "
           "\"settled\": %u, \"timeout\": %u, \"tries\": %u, \"cycles\": %u, \"strips\": %u, "
           "\"chars_at_published_capture\": %u}\n",
           screen, w, h, (h + st.strip_rows - 1) / st.strip_rows, fps, settle_ms, first, rc, sim.frame,
           (unsigned)(sim.clock / 1000u), sim.first_frame, type_every * sim.chars, shown(sim.first_frame), sim.chars,
           st.settle_passes, (unsigned)(st.settle_us / 1000u), st.settled, st.settle_timeout, st.tries, st.cycles, st.strips,
           shown(sim.at_last));
}

/* One paced job: the strips (game frames with a copy in their display call), their rows, the frames
 * the job spans until its lines would be recognized, and that time. */
static void run_paced(const char *screen, unsigned w, unsigned h, unsigned fps, unsigned settle_ms, int first,
                      const Screen *sc)
{
    VjoVocrStats st;
    char text[64];
    int rc = job(w, h, fps, settle_ms, first, sc, &st, text, sizeof(text));
    printf("{\"screen\": \"%s\", \"region\": \"%ux%u\", \"fps\": %u, \"settle_ms\": %u, \"pace_ms\": %u, "
           "\"wait_first\": %d, \"anim_ms\": %u, \"rc\": %d, \"ms\": %u, \"frames\": %u, \"strips\": %u, "
           "\"rows_max\": %u, \"rows_copied\": %lu, \"settle_passes\": %u, \"settle_naps\": %u, "
           "\"settle_ms_spent\": %u, \"settled\": %u, \"timeout\": %u, \"tries\": %u, \"chars_at_published_capture\": %u, "
           "\"of\": %u}\n",
           screen, w, h, fps, settle_ms, sc->pace_ms, first, sc->anim_ms, rc, (unsigned)(sim.clock / 1000u), sim.frame,
           sim.calls, sim.rows_max, sim.rows_copied, st.settle_passes, st.settle_naps, (unsigned)(st.settle_us / 1000u),
           st.settled, st.settle_timeout, st.tries, shown(sim.at_last), sim.chars);
}

static int pace_sweep(unsigned settle)
{
    static const unsigned paces[] = {0, 50, 75, 100, 150, 200};
    const unsigned w = 852, h = 135; /* CLANNAD's region (docs/auto-vs-press.md) */
    for (unsigned i = 0; i < sizeof(paces) / sizeof(paces[0]); i++) {
        unsigned p = paces[i];
        Screen still = {0, 0, 0, 0, 0, p}, anim = {0, 0, 0, 13, 1000, p}, typing = {3, 0, 0, 0, 0, p},
               blink = {0, 8, 0, 0, 0, p};
        run_paced("still", w, h, 60, settle, 0, &still);
        run_paced("still, not settled when asked", w, h, 60, settle, 1, &still);
        run_paced("box animates 1 s after its line, asked as settled", w, h, 60, settle, 0, &anim);
        run_paced("box animates 1 s after its line, not settled when asked", w, h, 60, settle, 1, &anim);
        run_paced("typing, 1 char / 3 frames", w, h, 60, settle, 1, &typing);
        run_paced("blinking icon, 8 frames", w, h, 60, settle, 1, &blink);
    }
    return 0;
}

/* Jobs from stdin: "W H FPS FIRST ANIM_MS PACE_MS" each (tools/stutter_model.py writes them from a
 * collect's job lines); the animated band is first-ocr.md's 13 rows. */
static int replay(unsigned settle)
{
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned w, h, fps, first, anim, pace;
        if (sscanf(line, "%u %u %u %u %u %u", &w, &h, &fps, &first, &anim, &pace) != 6) continue;
        if (!w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT || h < 40 || w < 80 || fps < 10 || fps > 120) {
            fprintf(stderr, "vjo-settle-bench: replay: line out of range: %s", line);
            return 2;
        }
        Screen sc = {0, 0, 0, anim ? 13u : 0u, anim, pace};
        run_paced("replay", w, h, fps, settle, (int)first, &sc);
    }
    return 0;
}

/* text-stability.md's replay, at a pace: a job asked with 12 of 25 characters shown, the typing
 * resuming after P frames (P = 1-40), at 60 and 30 fps, asked as settled and not, on CLANNAD's
 * 852 x 135 box and the bench's 917 x 125: the longest pause still read whole and the shortest
 * read as the 12 shown. */
static int pause_sweep(unsigned settle, unsigned pace)
{
    static const unsigned regions[][2] = {{852, 135}, {917, 125}};
    for (unsigned i = 0; i < 2; i++)
        for (unsigned fps = 60; fps >= 30; fps -= 30)
            for (int first = 0; first < 2; first++) {
                unsigned whole_up_to = 0, part_from = 0;
                for (unsigned p = 1; p <= 40; p++) {
                    Screen sc = {0, 0, p, 0, 0, pace};
                    VjoVocrStats st;
                    char text[64];
                    unsigned chars;
                    (void)job(regions[i][0], regions[i][1], fps, settle, first, &sc, &st, text, sizeof(text));
                    chars = shown(sim.at_last);
                    if (chars == sim.chars && !part_from) whole_up_to = p;
                    if (chars == 12u && !part_from) part_from = p;
                }
                printf("{\"region\": \"%ux%u\", \"fps\": %u, \"settle_ms\": %u, \"pace_ms\": %u, \"wait_first\": %d, "
                       "\"whole_up_to_frames\": %u, \"part_from_frames\": %u}\n",
                       regions[i][0], regions[i][1], fps, settle, pace, first, whole_up_to, part_from);
            }
    return 0;
}

int main(int argc, char **argv)
{
    static const unsigned regions[][2] = {{400, 100}, {917, 125}, {760, 160}, {960, 544}};
    unsigned settle;
    if (argc > 1 && !strcmp(argv[1], "--pace")) return pace_sweep(argc > 2 ? (unsigned)atoi(argv[2]) : 150u);
    if (argc > 1 && !strcmp(argv[1], "--replay")) return replay(argc > 2 ? (unsigned)atoi(argv[2]) : 150u);
    if (argc > 1 && !strcmp(argv[1], "--pauses")) {
        frame_us_override = argc > 3 ? (unsigned)atoi(argv[3]) : 0u; /* a 60 fps frame; 30 fps: twice it */
        return pause_sweep(150u, argc > 2 ? (unsigned)atoi(argv[2]) : 0u);
    }
    settle = argc > 1 ? (unsigned)atoi(argv[1]) : 150u;
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        for (unsigned fps = 60; fps >= 30; fps -= 30) {
            unsigned w = regions[i][0], h = regions[i][1];
            run("still", w, h, fps, 0, 0, 0, 0, 0);
            run("still", w, h, fps, settle, 0, 0, 0, 0);
            run("still, not settled when asked", w, h, fps, settle, 1, 0, 0, 0);
            run("typing, 1 char / 3 frames", w, h, fps, 0, 0, 3, 0, 0);
            run("typing, 1 char / 3 frames", w, h, fps, settle, 1, 3, 0, 0);
            run("typing, 1 char / 3 frames, asked as settled", w, h, fps, settle, 0, 3, 0, 0);
            run("blinking icon, 8 frames", w, h, fps, settle, 1, 0, 8, 0);
            /* the kernel took a pause in the typing for the end (or missed the last mark) */
            run("12/25 shown, typing resumes after 8 frames", w, h, fps, 0, 0, 0, 0, 8);
            run("12/25 shown, typing resumes after 8 frames", w, h, fps, settle, 0, 0, 0, 8);
            run("12/25 shown, typing resumes after 24 frames", w, h, fps, 0, 0, 0, 0, 24);
            run("12/25 shown, typing resumes after 24 frames", w, h, fps, settle, 0, 0, 0, 24);
        }
    return 0;
}
