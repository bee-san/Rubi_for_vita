#include "spot_pick.h"

#include <string.h>

#include "region_select.h" /* VJO_RSEL_W, _H: the screen */

static int pickable(const VjoSpotMap *m, int i)
{
    return i >= 0 && i < m->count && !(m->vertical >> i & 1u);
}

/* The gap between [a, a + an) and [b, b + bn); 0 where they overlap. */
static int gap(int a, int an, int b, int bn)
{
    return b >= a + an ? b - a - an : a >= b + bn ? a - b - bn : 0;
}

/* From block c, the nearest pickable one in direction d (0 up, 1 right, 2 down, 3 left); c for none. */
static int nearest(const VjoSpotMap *m, int c, int d)
{
    int k = d & 1 ? 0 : 1, s = d == 1 || d == 2 ? 1 : -1; /* the axis along the move (0 x, 1 y), its sign */
    int best = c, best_cost = 0x7FFFFFFF;
    const int16_t *a = m->rect[c];
    for (int i = 0; i < m->count; i++) {
        const int16_t *b = m->rect[i];
        int cost;
        /* its centre that way (doubled: no rounding); not c itself, whose centre is its own */
        if (!pickable(m, i) || (2 * b[k] + b[k + 2] - 2 * a[k] - a[k + 2]) * s <= 0)
            continue;
        cost = gap(a[k], a[k + 2], b[k], b[k + 2]) + 2 * gap(a[1 - k], a[3 - k], b[1 - k], b[3 - k]);
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    return best;
}

void vjo_spot_map(const VjoSpotResult *r, unsigned fb_w, unsigned fb_h, VjoSpotMap *m)
{
    unsigned n = r && fb_w && fb_h ? r->block_count : 0;
    memset(m, 0, sizeof(*m));
    m->start = -1;
    if (n > VJO_SPOT_SHOWN)
        n = VJO_SPOT_SHOWN;
    m->count = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) {
        const VjoSpotBlock *b = &r->block[i];
        /* the frame's pixels on the screen's, edges outward, then the margin, clipped */
        int x0 = (int)((unsigned)b->x * VJO_RSEL_W / fb_w) - VJO_SPOT_MARGIN;
        int y0 = (int)((unsigned)b->y * VJO_RSEL_H / fb_h) - VJO_SPOT_MARGIN;
        int x1 = (int)((((unsigned)b->x + b->w) * VJO_RSEL_W + fb_w - 1) / fb_w) + VJO_SPOT_MARGIN;
        int y1 = (int)((((unsigned)b->y + b->h) * VJO_RSEL_H + fb_h - 1) / fb_h) + VJO_SPOT_MARGIN;
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > VJO_RSEL_W)
            x1 = VJO_RSEL_W;
        if (y1 > VJO_RSEL_H)
            y1 = VJO_RSEL_H;
        m->rect[i][0] = (int16_t)x0;
        m->rect[i][1] = (int16_t)y0;
        m->rect[i][2] = (int16_t)(x1 > x0 ? x1 - x0 : 1);
        m->rect[i][3] = (int16_t)(y1 > y0 ? y1 - y0 : 1);
        if (b->vertical)
            m->vertical |= (uint16_t)(1u << i);
    }
    /* the start: the spotter's pick, or when it cannot be picked the first block that can */
    for (int i = (int)n - 1; i >= 0; i--)
        if (pickable(m, i))
            m->start = (int8_t)i;
    if (n && pickable(m, r->pick))
        m->start = (int8_t)r->pick;
    for (unsigned i = 0; i < n; i++)
        for (int d = 0; d < 4; d++)
            m->next[i][d] = (uint8_t)nearest(m, (int)i, d);
}
