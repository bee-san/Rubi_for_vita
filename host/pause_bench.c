/* What the pause for the frame (config.ini ocr_pause, API 55; docs/game-pause.md) costs the game and buys the read,
 * on a simulated game screen, through the real job (core/vocr_ocr.c with vita-vn-ocr's real line finder; no weights:
 * a job that reaches the recognizer stops there, VJO_E_OCR_MODEL -115, so the figures are the capture and the wait,
 * not recognition). Every strip a job reads strip by strip is one frame of the game: the display hook copies those
 * rows inside that frame's display call (kernel/game_ocr.c). A paused job's first pass is one display call that
 * copies the whole region (VJO_ROWS_FRAME); every later read is the worker's copy, no frame. Per job it prints one
 * JSON line: rc (-120 VJO_E_SCREEN_CHANGED: nothing shown), the display calls the job copied rows in, the rows copied
 * in all and in the longest call, and the simulated time to the recognizer (or the job's end) at 60 fps.
 *
 * The game's wait, est.: the longest call's pixels (rows x width) times the display hook's cost per pixel, from
 * docs/performance.md's `capture_abgr_sig` row (A8B8G8R8 with the capture's signature: 0.72-1.94 ms at 444 MHz and
 * 0.96-2.59 ms at 333 MHz for a strip of 52 x 896 pixels, the Vita compiler's instructions counted under qemu-arm and
 * the A9 cost model's range) and its `capture_a2b10` row for A2B10G10R10 (1.34-3.17 and 1.78-4.22 ms). Host model:
 * the console's frame pacing and copy time are unmeasured; a debug build's step log (`gstep pause`, its us) and
 * kernel.txt (`the frame's N rows in one display call, T us`) measure them on a console.
 *
 *   vjo-pause-bench */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vocr_ocr.h"

#define MAX_W 960u
#define MAX_H 544u
#define FRAME_US 16667u
#define STRIP_PX (52.0 * 896.0)

typedef struct {
    unsigned w, h, frame, chars, type_every, icon, icon_every, varying, settle_first, paused;
    unsigned frozen_at, display_calls, rows_copied, longest;
    uint64_t clock;
} Sim;

static Sim sim;
static unsigned char cap[64 * 1024];
static unsigned char ws[2 * 1024 * 1024] __attribute__((aligned(64)));
static unsigned char copy[MAX_W * MAX_H];

static uint64_t now(void *ud)
{
    (void)ud;
    return sim.clock;
}

static void sleep_us(uint32_t us)
{
    sim.clock += us;
    sim.frame += us / FRAME_US;
}

static int no_file(void *ud, const char *path, VjoFile *out)
{
    (void)ud; (void)path; (void)out;
    return -1;
}

static unsigned shown(unsigned frame)
{
    if (!sim.type_every) return sim.chars;
    return frame / sim.type_every < sim.chars ? frame / sim.type_every : sim.chars;
}

/* The icon's state at a frame: 1 the "next" mark blinking (on or off); 2 a spinner of 20 shades, a new one at every
 * frame (varying: 1 to 4 frames a shade); 0 none. */
static unsigned icon_state(unsigned frame)
{
    if (sim.icon == 1) return frame / sim.icon_every % 2u;
    if (sim.icon == 2 && !sim.varying) return frame % 20u;
    if (sim.icon == 2) { /* a shade lasts 1, 2, 3 or 4 frames in turn: 10 frames a round of 4 shades */
        unsigned f = frame % 50u, s = 0, len = 1;
        while (f >= len) { f -= len; s++; len = s % 4u + 1u; }
        return s % 20u;
    }
    return 0;
}

/* A text box: a line of glyph-like strokes, 12 px a character, in its middle rows; the icon right of it. */
static unsigned char pixel(unsigned x, unsigned y, unsigned frame)
{
    unsigned top = sim.h / 2u - 12u;
    if (sim.icon && x >= sim.w - 30u && x < sim.w - 6u && y >= top && y < top + 24u) {
        unsigned s = icon_state(frame);
        return (unsigned char)(sim.icon == 1 ? (s ? 200u : 20u) : 40u + s * 37u % 200u);
    }
    if (y < top || y >= top + 24u || x < 20u || x + 40u >= sim.w) return 20;
    return (x - 20u) / 12u < shown(frame) && (x - 20u) % 12u < 2u ? 235 : 20;
}

static const unsigned char *frozen(void *ud)
{
    (void)ud;
    return sim.frozen_at ? copy : NULL;
}

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud;
    if (sim.paused && !sim.frozen_at && (flags & VJO_STRIP_FIRST) && !y) {
        /* the pause: the whole region from the next frame, in its one display call */
        sim.clock += FRAME_US;
        sim.frame++;
        sim.frozen_at = sim.frame;
        sim.display_calls++;
        sim.rows_copied += sim.h;
        if (sim.h > sim.longest) sim.longest = sim.h;
        for (unsigned r = 0; r < sim.h; r++)
            for (unsigned x = 0; x < sim.w; x++) copy[(size_t)r * sim.w + x] = pixel(x, r, sim.frame);
    }
    if (sim.frozen_at) {
        for (unsigned r = 0; r < n; r++) memcpy(dst + (size_t)r * stride, copy + (size_t)(y + r) * sim.w, sim.w);
        return (int)n;
    }
    sim.clock += FRAME_US;
    sim.frame++;
    sim.display_calls++;
    sim.rows_copied += n;
    if (n > sim.longest) sim.longest = n;
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < sim.w; x++) dst[(size_t)r * stride + x] = pixel(x, y + r, sim.frame);
    return (int)n;
}

static void run(const char *screen, unsigned w, unsigned h, unsigned icon, unsigned icon_every, unsigned varying,
                unsigned type_every, unsigned first, unsigned paused)
{
    VjoPlatform p = {0};
    VjoVocrResident res = {0};
    /* Read text: Automatically's job: the 150 ms wait within 2 s (config.ini ocr_settle_ms, ocr_settle_max_ms) */
    VjoVocrPolicy pol = {.settle_us = 150000u, .settle_max_us = 2000000u, .settle_first = (int)first, .sleep = sleep_us};
    VjoGreySource im = {NULL, w, h, strip, NULL, NULL, paused ? frozen : NULL};
    VjoVocrStats st;
    char text[64];
    double px;
    int rc;
    memset(&sim, 0, sizeof(sim));
    sim.w = w;
    sim.h = h;
    sim.chars = (w - 60u) / 12u < 25u ? (w - 60u) / 12u : 25u;
    sim.icon = icon;
    sim.icon_every = icon_every;
    sim.varying = varying;
    sim.type_every = type_every;
    sim.paused = paused;
    p.file_open = no_file;
    p.now_us = now;
    rc = vjo_vocr_ocr_policy(&p, "none", "FL10_w8.vocr", &im, &res, ws, vjo_vocr_job_bytes("FL10_w8.vocr", w, h), cap,
                             vjo_vocr_capture_bytes(w, h), &pol, text, sizeof(text), &st);
    px = (double)sim.longest * w;
    printf("{\"screen\": \"%s\", \"region\": \"%ux%u\", \"asked\": \"%s\", \"paused\": %u, \"rc\": %d, \"tears\": %u, "
           "\"display_calls\": %u, \"rows_copied\": %u, \"longest_call_rows\": %u, \"ms_to_recognizer\": %u, "
           "\"wait_ms_abgr_444\": [%.1f, %.1f], \"wait_ms_abgr_333\": [%.1f, %.1f], \"wait_ms_a2b10_333\": [%.1f, %.1f], "
           "\"copy_ms_abgr_333\": [%.1f, %.1f]}\n",
           screen, w, h, first ? "not settled" : "settled", paused, rc, st.tears, sim.display_calls, sim.rows_copied,
           sim.longest, (unsigned)(sim.clock / 1000u),
           px * 0.72 / STRIP_PX, px * 1.94 / STRIP_PX, px * 0.96 / STRIP_PX, px * 2.59 / STRIP_PX,
           px * 1.78 / STRIP_PX, px * 4.22 / STRIP_PX,
           (double)sim.rows_copied * w * 0.96 / STRIP_PX, (double)sim.rows_copied * w * 2.59 / STRIP_PX);
}

int main(void)
{
    static const unsigned regions[][2] = {{852, 135}, {917, 126}, {960, 544}};
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        for (unsigned paused = 0; paused < 2; paused++) {
            unsigned w = regions[i][0], h = regions[i][1];
            run("still", w, h, 0, 0, 0, 0, 0, paused);
            run("still", w, h, 0, 0, 0, 0, 1, paused);
            run("blinking next mark, 8 frames", w, h, 1, 8, 0, 0, 0, paused);
            run("spinner, 20 shades, a new one each frame", w, h, 2, 1, 0, 0, 0, paused);
            run("spinner, 20 shades, a new one each frame", w, h, 2, 1, 0, 0, 1, paused);
            run("spinner, 20 shades, 1-4 frames a shade", w, h, 2, 1, 1, 0, 0, paused);
            run("typing, 1 char / 3 frames", w, h, 0, 0, 0, 3, 1, paused);
        }
    return 0;
}
