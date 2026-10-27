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
 *   vjo-settle-bench [SETTLE_MS] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vocr_ocr.h"

typedef struct {
    unsigned w, h, frame_us, frame, type_every, chars, blink_every, flagged_at, first_frame, pause, at_last;
    uint64_t clock;
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
    if ((flags & VJO_STRIP_FIRST) && !y && !sim.first_frame) sim.first_frame = sim.frame;
    if (flags & VJO_STRIP_LAST) sim.at_last = sim.frame; /* the published capture's (the last one) */
    for (unsigned r = 0; r < n; r++) {
        unsigned row = y + r;
        unsigned char *d = dst + (size_t)r * stride;
        memset(d, 20, sim.w);
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

static void run(const char *screen, unsigned w, unsigned h, unsigned fps, unsigned settle_ms, int first,
                unsigned type_every, unsigned blink_every, unsigned pause)
{
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    VjoVocrPolicy pol = {.settle_us = settle_ms * 1000u, .settle_first = first, .sleep = sleep_us};
    VjoGreySource im = {NULL, w, h, strip, NULL, NULL, NULL};
    VjoVocrStats st;
    char text[64];
    int rc;
    memset(&sim, 0, sizeof(sim));
    sim.w = w;
    sim.h = h;
    sim.frame_us = 1000000u / fps;
    sim.chars = (w - 40) / 12 < 25 ? (w - 40) / 12 : 25;
    sim.type_every = type_every;
    sim.blink_every = blink_every;
    sim.pause = pause;
    p.file_open = no_file;
    p.now_us = now;
    rc = vjo_vocr_ocr_policy(&p, "none", "FL10_w8.vocr", &im, &res, ws, vjo_vocr_job_bytes("FL10_w8.vocr", w, h), cap,
                             vjo_vocr_capture_bytes(w, h), settle_ms ? &pol : NULL, text, sizeof(text), &st);
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

int main(int argc, char **argv)
{
    static const unsigned regions[][2] = {{400, 100}, {917, 125}, {760, 160}, {960, 544}};
    unsigned settle = argc > 1 ? (unsigned)atoi(argv[1]) : 150u;
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
