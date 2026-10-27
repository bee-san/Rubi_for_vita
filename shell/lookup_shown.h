/* The overlay's press-to-screen figure (#360; the debug build's only: its close line,
 * docs/release-variants.md). A lookup the overlay asks for (a cursor move, a tap, a dictionary
 * page, a line history step: worker.c post_local_lookup) records its cursor_seq and the time in
 * the view; the control thread publishes a lookup's result with its cursor_seq and the time its
 * job published it (job_done: worker.c view_publish); the paf thread's render that draws that
 * result for the cursor it shows takes the figure, once, under the view lock (overlay.cpp render:
 * a few instructions a lookup, two loads a render, and no log line until the overlay closes: an
 * append on the paf thread would stutter the game). The time is the process time's low 32 bits
 * in us, so a difference is right across their wrap (71 minutes). It ends at the render that sets
 * the text: paf lays it out in the frame or two after (LAYOUT_RETRY_FRAMES), unmeasured. */
#ifndef VJO_LOOKUP_SHOWN_H
#define VJO_LOOKUP_SHOWN_H

#include <psp2/kernel/processmgr.h>
#include <stdint.h>

#include "shell.h"

#if VJO_DEBUG
/* Since the overlay opened: the lookups shown, the sums of the ms from their post to the render that
 * showed them and from their result's publication (job_done) to it, and the largest of the first. */
typedef struct {
    uint32_t n, sum_ms, max_ms, after_ms;
} VjoShown;

/* Under the view lock, in a render: when the view shows the result of the lookup the overlay asked
 * for, with the cursor it asked for (cursor_matches), the figure is taken and the request forgotten.
 * 1 when one was taken. */
static inline int vjo_shown_take(VjoShown *s, VjoView *v, int cursor_matches)
{
    uint32_t now, ms;
    if (!v->ask_seq || v->ask_seq != v->shown_seq || !cursor_matches)
        return 0;
    now = (uint32_t)sceKernelGetProcessTimeWide();
    ms = (now - v->ask_us) / 1000u;
    s->n++;
    s->sum_ms += ms;
    if (ms > s->max_ms)
        s->max_ms = ms;
    s->after_ms += (now - v->done_us) / 1000u;
    v->ask_seq = 0;
    return 1;
}

static inline uint32_t vjo_shown_mean(uint32_t sum, uint32_t n)
{
    return n ? sum / n : 0u;
}

/* The figure in the overlay's close line (overlay.cpp close_page), after its counts and timings: the
 * format's part and its four numbers. The whole line is at most 380 B with its '\n' (218 B of text and
 * 15 numbers of 10 or 11 characters), longer than vjo_log's 254 B, so close_page formats it itself. */
#define VJO_SHOWN_FORMAT ", lookups shown %u: mean %u ms (%u ms after the result), max %u ms"
#define VJO_SHOWN_ARGS(s)                                                                                  \
    (unsigned)(s)->n, (unsigned)vjo_shown_mean((s)->sum_ms, (s)->n), (unsigned)vjo_shown_mean((s)->after_ms, (s)->n), \
        (unsigned)(s)->max_ms
#endif

#endif
