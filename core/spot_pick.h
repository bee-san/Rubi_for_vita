/* Scan screen (API 30, config.ini ocr_scan_screen): the overlay's region drawing starts on the text spotter's
 * blocks (vita-vn-ocr #104) instead of an empty screen. The d-pad and the left stick go to the nearest block that
 * way, a touch picks the block under the finger, enter reads the picked one (its rectangle becomes the game's
 * region, as a box drawn by hand does, and is read at once) and cancel goes back to drawing the box by hand.
 * Vertical blocks are drawn but cannot be picked (no vertical text in this build).
 *
 * The game worker makes the map (vjo_spot_map, include/vjo_spot.h VjoSpotMap) with the scan, so the Shell keeps
 * a copy and looks things up in it: the inline functions below are all its code for it (shell/worker.c checks the
 * map, shell/overlay.cpp runs a frame). Screen pixels, no allocation and no SDK calls; host/tests/test_spot_pick.c
 * drives them all. */
#ifndef VJO_SPOT_PICK_H
#define VJO_SPOT_PICK_H

#include <stdint.h>

#include "../include/vjo_spot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The map of a scan's set r, whose frame was fb_w x fb_h pixels (game_ocr/main.c run_spot): the first
 * VJO_SPOT_SHOWN blocks in reading order, the last place the spotter's pick's when it is not among them; each
 * scaled to the 960 x 544 screen (its edges rounded outward), grown by VJO_SPOT_MARGIN and clipped; the start;
 * and from each block, for each of the d-pad's four directions, the nearest block that can be picked that lies
 * that way, on the blocks' own rectangles (without the margin): its centre beyond the block's centre on that
 * axis, level with the block (their spans across overlap) or at least as far along as across, and reaching
 * further that way than the block; the nearest by the least gap that way (where the two overlap that way, how
 * much further it reaches) plus twice the gap across, the first in reading order on a tie, the block itself when
 * there is none. Then every block that can be picked is made reachable from the start by the arrows
 * (core/spot_pick.c arrows). r NULL or a zero size: an empty map. */
void vjo_spot_map(const VjoSpotResult *r, unsigned fb_w, unsigned fb_h, VjoSpotMap *m);

/* 1 when every index in m is one of its blocks (count at most VJO_SPOT_SHOWN, start below it, every neighbour
 * below it), as every map vjo_spot_map makes is. The map comes from the game's process: shell/worker.c publishes
 * no other, so the lookups below and the overlay's m->rect[sel] stay in the map. */
static inline int vjo_spot_map_ok(const VjoSpotMap *m)
{
    const uint8_t *n = (const uint8_t *)m->next;
    if (m->count > VJO_SPOT_SHOWN || m->start >= m->count)
        return 0;
    for (int i = 0; i < 4 * m->count; i++)
        if (n[i] >= m->count)
            return 0;
    return 1;
}

/* The first block in reading order that can be picked under the screen pixel (x, y); -1 for none. */
static inline int vjo_spot_hit(const VjoSpotMap *m, int x, int y)
{
    for (int i = 0; i < m->count; i++) {
        const int16_t *r = m->rect[i];
        if (!(m->vertical >> i & 1u) && x >= r[0] && x - r[0] < r[2] && y >= r[1] && y - r[1] < r[3])
            return i;
    }
    return -1;
}

/* From block cur, the d-pad's arrow dir (one of VJO_RSEL_UP, _RIGHT, _DOWN, _LEFT: 0x10 << d) leads to. */
static inline int vjo_spot_step(const VjoSpotMap *m, int cur, unsigned dir)
{
    return cur >= 0 && cur < m->count ? m->next[cur][__builtin_ctz(dir) - 4] : cur;
}

/* The overlay's scan screen (shell/overlay.cpp input_scan): state 1 waits for the scan's result (seq:
 * g_view.spot.seq when the scan was asked for), 2 picks among its blocks (sel); 0 draws by hand. */
typedef struct {
    uint32_t seq;
    int state, sel;
} VjoSpotUi;

/* What the overlay does with a frame (vjo_spot_frame). */
enum {
    VJO_SPOT_KEEP,   /* nothing */
    VJO_SPOT_SHOW,   /* a result: draw its blocks, the cursor on sel */
    VJO_SPOT_MOVE,   /* draw the blocks, the cursor on sel */
    VJO_SPOT_READ,   /* read block sel (VJO_CMD_SCAN_PICK) */
    VJO_SPOT_MANUAL, /* draw by hand */
    VJO_SPOT_HAND    /* draw by hand, with this frame's touch (the drag starts there) */
};

/* One frame of the scan screen: seq and m the view's, copied together; hit the block under a new touch
 * (vjo_spot_hit, -1 for none) or -2 without one; enter and cancel pressed; dir the d-pad's arrow (VJO_RSEL_*,
 * 0 for none). Every result that arrives is drawn afresh with the cursor on its start, the session's first or a
 * later one (a scan asked for while another ran ends in the same session), so a touch, enter and the arrows use
 * the blocks drawn. A result with no block to start on, or cancel, draws by hand; so does a touch while it
 * waits, or outside every block. */
static inline int vjo_spot_frame(VjoSpotUi *u, uint32_t seq, const VjoSpotMap *m, int hit, int enter, int cancel,
                                 unsigned dir)
{
    int sel;
    if (seq != u->seq) {
        u->seq = seq;
        u->sel = m->start;
        if (u->sel < 0 || u->sel >= m->count)
            return VJO_SPOT_MANUAL;
        u->state = 2;
        return VJO_SPOT_SHOW;
    }
    if (u->state == 1)
        return hit != -2 ? VJO_SPOT_HAND : cancel ? VJO_SPOT_MANUAL : VJO_SPOT_KEEP;
    if (hit == -1)
        return VJO_SPOT_HAND;
    if (hit >= 0)
        u->sel = hit;
    if (hit >= 0 || enter)
        return VJO_SPOT_READ;
    if (cancel)
        return VJO_SPOT_MANUAL;
    if (dir && (sel = vjo_spot_step(m, u->sel, dir & -dir)) != u->sel) {
        u->sel = sel;
        return VJO_SPOT_MOVE;
    }
    return VJO_SPOT_KEEP;
}

#ifdef __cplusplus
}
#endif

#endif
