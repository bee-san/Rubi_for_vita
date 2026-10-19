/* Scan screen (API 30, config.ini ocr_scan_screen): the overlay's region drawing starts on the text spotter's
 * blocks (vita-vn-ocr #104) instead of an empty screen. The d-pad and the left stick go to the nearest block that
 * way, a touch picks the block under the finger, enter reads the picked one (its rectangle becomes the game's
 * region, as a box drawn by hand does, and is read at once) and cancel goes back to drawing the box by hand.
 * Vertical blocks are drawn but cannot be picked (no vertical text in this build).
 *
 * The game worker makes the map (vjo_spot_map, include/vjo_spot.h VjoSpotMap) with the scan, so the Shell keeps
 * a copy and looks things up in it: vjo_spot_hit and vjo_spot_step below are all its code for it
 * (shell/overlay.cpp). Screen pixels, no allocation and no SDK calls; host/tests/test_spot_pick.c drives both. */
#ifndef VJO_SPOT_PICK_H
#define VJO_SPOT_PICK_H

#include <stdint.h>

#include "../include/vjo_spot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The map of a scan's set r, whose frame was fb_w x fb_h pixels (game_ocr/main.c run_spot): each shown block
 * scaled to the 960 x 544 screen (its edges rounded outward), grown by VJO_SPOT_MARGIN and clipped; the start;
 * and from each block, for each of the d-pad's four directions, the nearest block that can be picked whose centre
 * lies that way: the least gap that way plus twice the gap across (each 0 where the two overlap), the first in
 * reading order on a tie, the block itself when there is none. r NULL or a zero size: an empty map. */
void vjo_spot_map(const VjoSpotResult *r, unsigned fb_w, unsigned fb_h, VjoSpotMap *m);

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

#ifdef __cplusplus
}
#endif

#endif
