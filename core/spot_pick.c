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

/* The scores of the arrows (cost below): a block not quite that way (outside the cone, or reaching no further that
 * way than the block it is from), the repair's arrow already in use, and a block not that way at all. */
#define FAR (1 << 14)
#define USED (1 << 15)
#define BACK (1 << 16)

/* Block b from block a in direction d (0 up, 1 right, 2 down, 3 left), on the blocks' own rectangles (x, y, w, h):
 * the least gap that way, or where the two overlap that way how much further b reaches, plus twice the gap across;
 * FAR added unless b lies that way (level with a, or at least as far along as across) and reaches further that way
 * than a, BACK unless b's centre is beyond a's. */
static int32_t cost(const int16_t *a, const int16_t *b, int d)
{
    int k = d & 1 ? 0 : 1, s = d == 1 || d == 2 ? 1 : -1; /* the axis along the move (0 x, 1 y), its sign */
    /* the centres' offsets along and across (doubled: no rounding), the gaps, how much further b reaches */
    int along = (2 * b[k] + b[k + 2] - 2 * a[k] - a[k + 2]) * s;
    int across = 2 * b[1 - k] + b[3 - k] - 2 * a[1 - k] - a[3 - k];
    int ga = gap(a[1 - k], a[3 - k], b[1 - k], b[3 - k]), g = gap(a[k], a[k + 2], b[k], b[k + 2]);
    int lead = s > 0 ? b[k] + b[k + 2] - a[k] - a[k + 2] : a[k] - b[k];
    return (along <= 0 ? BACK : 0) + ((ga && along < (across < 0 ? -across : across)) || lead <= 0 ? FAR : 0) +
           (g ? g : lead > 0 ? lead : 0) + 2 * ga;
}

/* The blocks the arrows reach from the start, a bit each. */
static unsigned reached(const VjoSpotMap *m)
{
    unsigned seen = m->start >= 0 ? 1u << m->start : 0u, before;
    do {
        before = seen;
        for (int c = 0; c < m->count; c++)
            if (seen >> c & 1u)
                for (int d = 0; d < 4; d++)
                    seen |= 1u << m->next[c][d];
    } while (seen != before);
    return seen;
}

/* The arrows: from each block, for each direction, the nearest pickable block that way (the least cost under FAR;
 * the first in reading order on a tie; the block itself for none). Then every pickable block is made reachable from
 * the start: while one is not, the cheapest arrow of a reachable block towards it is taken (a dead end before one
 * in use), a dead end pointed at it, an arrow in use routed through it (r -> u -> the arrow's old block, on u's same
 * arrow). Each step makes one more block reachable and keeps every block that was: at most VJO_SPOT_SHOWN steps. */
static void arrows(VjoSpotMap *m, const int16_t (*own)[4])
{
    int n = m->count;
    for (int c = 0; c < n; c++)
        for (int d = 0; d < 4; d++) {
            int best = c;
            int32_t best_cost = FAR, k;
            for (int i = 0; i < n; i++)
                if (i != c && pickable(m, i) && (k = cost(own[c], own[i], d)) < best_cost) {
                    best_cost = k;
                    best = i;
                }
            m->next[c][d] = (uint8_t)best;
        }
    if (m->start < 0)
        return;
    for (;;) {
        unsigned seen = reached(m);
        int32_t best_cost = 0x7FFFFFFF, k;
        int bu = -1, br = 0, bd = 0;
        for (int u = 0; u < n; u++)
            if (pickable(m, u) && !(seen >> u & 1u))
                for (int r = 0; r < n; r++)
                    if (seen >> r & 1u)
                        for (int d = 0; d < 4; d++)
                            if ((k = cost(own[r], own[u], d) + (m->next[r][d] != r ? USED : 0)) < best_cost) {
                                best_cost = k;
                                bu = u;
                                br = r;
                                bd = d;
                            }
        if (bu < 0)
            return;
        if (m->next[br][bd] != br)
            m->next[bu][bd] = m->next[br][bd];
        m->next[br][bd] = (uint8_t)bu;
    }
}

void vjo_spot_map(const VjoSpotResult *r, unsigned fb_w, unsigned fb_h, VjoSpotMap *m)
{
    unsigned n = r && fb_w && fb_h ? r->block_count : 0;
    /* the pick (a block of the set, or none) and its place among the shown */
    int pick = n && r->pick >= 0 && (unsigned)r->pick < n && r->pick < (int)VJO_SPOT_MAX ? (int)r->pick : -1;
    int16_t own[VJO_SPOT_SHOWN][4]; /* each block's own rectangle on the screen: the arrows' geometry */
    memset(m, 0, sizeof(*m));
    m->start = -1;
    if (n > VJO_SPOT_SHOWN)
        n = VJO_SPOT_SHOWN;
    m->count = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) {
        /* a pick past the shown blocks takes the last place: the dialogue is often the last in reading order */
        const VjoSpotBlock *b = &r->block[i + 1 == n && pick >= (int)n ? (unsigned)pick : i];
        /* the frame's pixels on the screen's, edges outward */
        int x0 = (int)((unsigned)b->x * VJO_RSEL_W / fb_w), y0 = (int)((unsigned)b->y * VJO_RSEL_H / fb_h);
        int x1 = (int)((((unsigned)b->x + b->w) * VJO_RSEL_W + fb_w - 1) / fb_w);
        int y1 = (int)((((unsigned)b->y + b->h) * VJO_RSEL_H + fb_h - 1) / fb_h);
        /* its own rectangle, clipped to the screen */
        int ox0 = x0 < VJO_RSEL_W ? x0 : VJO_RSEL_W, oy0 = y0 < VJO_RSEL_H ? y0 : VJO_RSEL_H;
        int ox1 = x1 < VJO_RSEL_W ? x1 : VJO_RSEL_W, oy1 = y1 < VJO_RSEL_H ? y1 : VJO_RSEL_H;
        own[i][0] = (int16_t)ox0;
        own[i][1] = (int16_t)oy0;
        own[i][2] = (int16_t)(ox1 > ox0 ? ox1 - ox0 : 1);
        own[i][3] = (int16_t)(oy1 > oy0 ? oy1 - oy0 : 1);
        /* the region it is read as: grown by the margin, clipped */
        x0 -= VJO_SPOT_MARGIN;
        y0 -= VJO_SPOT_MARGIN;
        x1 += VJO_SPOT_MARGIN;
        y1 += VJO_SPOT_MARGIN;
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
    if (pick >= (int)n)
        pick = (int)n - 1;
    if (pickable(m, pick))
        m->start = (int8_t)pick;
    arrows(m, (const int16_t (*)[4])own);
}
