/* Scan screen's picker (core/spot_pick.h): the map the game worker makes of a scan's set, and the Shell's two
 * lookups in it. The d-pad and the stick go to the nearest block that way (ties, edges, a single block, no
 * block), a touch picks the block under the finger, the cursor starts on the pick, vertical blocks are drawn
 * but never picked, at most VJO_SPOT_SHOWN blocks are shown, and each block's rectangle is the region it is read
 * as (the margin, clipped to the screen, a PSP frame scaled to it). */
#include "acutest.h"
#include <string.h>

#include "spot_pick.h"
#include "region_select.h"

static VjoSpotResult set;

/* Block i of the set: frame pixels, vertical or not. */
static void block(unsigned i, int x, int y, int w, int h, int vertical)
{
    set.block[i] = (VjoSpotBlock){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, (uint8_t)vertical, (uint8_t)i, 1, 0};
    if (set.block_count <= i) set.block_count = i + 1;
}

static void clear(void)
{
    memset(&set, 0, sizeof(set));
    set.pick = -1;
}

/* A VN screen: a HUD label top left (0), a name plate (1) and the dialogue (2, the pick), a menu at the right of
 * the screen's middle (3) and a vertical column (4). */
static void vn_screen(void)
{
    clear();
    block(0, 20, 10, 300, 30, 0);
    block(1, 50, 330, 80, 36, 0);
    block(2, 50, 380, 860, 120, 0);
    block(3, 700, 200, 200, 80, 0);
    block(4, 400, 100, 40, 200, 1);
    set.pick = 2;
}

static void rects_are_the_regions_read(void)
{
    VjoSpotMap m;
    vn_screen();
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.count == 5 && m.start == 2 && m.vertical == 1u << 4);
    /* grown by the margin on every side, clipped to the screen (block 0 at y 10: its top at 2) */
    TEST_CHECK(m.rect[0][0] == 12 && m.rect[0][1] == 2 && m.rect[0][2] == 316 && m.rect[0][3] == 46);
    TEST_CHECK(m.rect[2][0] == 42 && m.rect[2][1] == 372 && m.rect[2][2] == 876 && m.rect[2][3] == 136);
    /* one at the screen's corner: clipped */
    clear();
    block(0, 0, 0, 960, 544, 0);
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.rect[0][0] == 0 && m.rect[0][1] == 0 && m.rect[0][2] == 960 && m.rect[0][3] == 544);
    clear();
    block(0, 955, 540, 5, 4, 0);
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.rect[0][0] == 947 && m.rect[0][1] == 532 && m.rect[0][0] + m.rect[0][2] == 960 &&
               m.rect[0][1] + m.rect[0][3] == 544);
    /* a PSP game's 480 x 272 frame: twice the size on the screen, then the margin */
    clear();
    block(0, 25, 190, 400, 60, 0);
    vjo_spot_map(&set, 480, 272, &m);
    TEST_CHECK(m.rect[0][0] == 42 && m.rect[0][1] == 372 && m.rect[0][2] == 816 && m.rect[0][3] == 136);
    /* another size (720 x 408): edges outward, so the region holds the whole block */
    clear();
    block(0, 101, 101, 101, 101, 0);
    vjo_spot_map(&set, 720, 408, &m);
    TEST_CHECK(m.rect[0][0] == 134 - 8 && m.rect[0][1] == 134 - 8);
    TEST_CHECK(m.rect[0][0] + m.rect[0][2] == 270 + 8 && m.rect[0][1] + m.rect[0][3] == 270 + 8);
}

static void the_cursor_starts_on_the_pick(void)
{
    VjoSpotMap m;
    vn_screen();
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.start == 2);
    /* a vertical pick: the first block that can be picked */
    set.pick = 4;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.start == 0);
    /* no pick (-1), or one past the shown blocks: the same */
    set.pick = -1;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.start == 0);
    /* only vertical blocks: none to start on, so the overlay draws by hand */
    clear();
    block(0, 800, 50, 40, 300, 1);
    block(1, 740, 50, 40, 300, 1);
    set.pick = 0;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.count == 2 && m.start == -1 && vjo_spot_hit(&m, 810, 100) == -1);
    /* no block, a failed scan's zeroed set, no set at all */
    clear();
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.count == 0 && m.start == -1);
    vjo_spot_map(NULL, 960, 544, &m);
    TEST_CHECK(m.count == 0 && m.start == -1);
    vn_screen();
    vjo_spot_map(&set, 0, 544, &m);
    TEST_CHECK(m.count == 0 && m.start == -1);
}

static void the_dpad_goes_to_the_nearest_block_that_way(void)
{
    VjoSpotMap m;
    vn_screen();
    vjo_spot_map(&set, 960, 544, &m);
    /* from the dialogue: up is the name plate right above it (their regions touch), not the HUD label further
     * up (nor the vertical column); right the menu (its centre right of the dialogue's); left the name plate
     * too (its centre is left of the wide box's); down: none */
    TEST_CHECK(vjo_spot_step(&m, 2, VJO_RSEL_UP) == 1);
    TEST_CHECK(vjo_spot_step(&m, 2, VJO_RSEL_RIGHT) == 3);
    TEST_CHECK(vjo_spot_step(&m, 2, VJO_RSEL_LEFT) == 1 && vjo_spot_step(&m, 2, VJO_RSEL_DOWN) == 2);
    /* from the name plate: up the HUD label, down the dialogue, right the dialogue (the wide box overlaps it
     * along, the menu is 554 px away) */
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_UP) == 0 && vjo_spot_step(&m, 1, VJO_RSEL_DOWN) == 2);
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_RIGHT) == 2 && vjo_spot_step(&m, 1, VJO_RSEL_LEFT) == 1);
    /* from the menu: down the dialogue, left the dialogue too (it lies under the menu's whole width: 84 px
     * across against the name plate's 554 px along), up the HUD label, right none */
    TEST_CHECK(vjo_spot_step(&m, 3, VJO_RSEL_LEFT) == 2 && vjo_spot_step(&m, 3, VJO_RSEL_RIGHT) == 3);
    TEST_CHECK(vjo_spot_step(&m, 3, VJO_RSEL_DOWN) == 2 && vjo_spot_step(&m, 3, VJO_RSEL_UP) == 0);
    /* the vertical column is never a destination */
    for (int i = 0; i < m.count; i++)
        for (int d = 0; d < 4; d++)
            TEST_CHECK(m.next[i][d] != 4 || i == 4);
    /* a tie: two blocks at the same distance below: the first in reading order */
    clear();
    block(0, 400, 100, 160, 40, 0);
    block(1, 200, 300, 160, 40, 0);
    block(2, 600, 300, 160, 40, 0);
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(vjo_spot_step(&m, 0, VJO_RSEL_DOWN) == 1);
    /* a single block: every arrow stays on it */
    clear();
    block(0, 100, 100, 300, 40, 0);
    vjo_spot_map(&set, 960, 544, &m);
    for (unsigned d = VJO_RSEL_UP; d <= VJO_RSEL_LEFT; d <<= 1)
        TEST_CHECK(vjo_spot_step(&m, 0, d) == 0);
    /* no block, or a cursor that is none: it stays where it is */
    clear();
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(vjo_spot_step(&m, -1, VJO_RSEL_DOWN) == -1 && vjo_spot_step(&m, 3, VJO_RSEL_DOWN) == 3);
}

static void a_touch_picks_the_block_under_the_finger(void)
{
    VjoSpotMap m;
    vn_screen();
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(vjo_spot_hit(&m, 480, 440) == 2);
    TEST_CHECK(vjo_spot_hit(&m, 60, 340) == 1);
    /* the margin counts: 5 px above the dialogue's own top */
    TEST_CHECK(vjo_spot_hit(&m, 480, 375) == 2);
    /* the vertical column is not picked; nowhere: none */
    TEST_CHECK(vjo_spot_hit(&m, 420, 200) == -1 && vjo_spot_hit(&m, 600, 60) == -1);
    /* the rectangle's edges: inside on its first pixel, outside one past its last */
    TEST_CHECK(vjo_spot_hit(&m, m.rect[3][0], m.rect[3][1]) == 3);
    TEST_CHECK(vjo_spot_hit(&m, m.rect[3][0] + m.rect[3][2], m.rect[3][1]) == -1);
    /* two that overlap: the first in reading order */
    clear();
    block(0, 100, 100, 300, 100, 0);
    block(1, 200, 150, 300, 100, 0);
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(vjo_spot_hit(&m, 250, 170) == 0 && vjo_spot_hit(&m, 450, 230) == 1);
}

static void at_most_sixteen_blocks_are_shown(void)
{
    VjoSpotMap m;
    clear();
    for (unsigned i = 0; i < VJO_SPOT_MAX; i++)
        block(i, (int)(i % 8) * 120, (int)(i / 8) * 68, 100, 30, 0);
    set.count = VJO_SPOT_MAX;
    set.truncated = 1;
    set.pick = 40; /* beyond the shown ones: the first shown */
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.count == VJO_SPOT_SHOWN && m.start == 0);
    for (int i = 0; i < m.count; i++)
        for (int d = 0; d < 4; d++)
            TEST_CHECK(m.next[i][d] < VJO_SPOT_SHOWN);
    /* block 16 (row 2) is not shown: nothing is hit there */
    TEST_CHECK(vjo_spot_hit(&m, 50, 2 * 68 + 15) == -1);
    set.pick = 9;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.start == 9);
    /* the transport holds the whole set and the map */
    TEST_CHECK(sizeof(VjoSpotScan) == 1552 + 196);
}

TEST_LIST = {
    {"scan screen: each block's rectangle is the region it is read as", rects_are_the_regions_read},
    {"scan screen: the cursor starts on the pick, else the first block that can be picked", the_cursor_starts_on_the_pick},
    {"scan screen: the d-pad and the stick go to the nearest block that way", the_dpad_goes_to_the_nearest_block_that_way},
    {"scan screen: a touch picks the block under the finger", a_touch_picks_the_block_under_the_finger},
    {"scan screen: at most 16 blocks are shown, of a truncated 64-box set", at_most_sixteen_blocks_are_shown},
    {NULL, NULL}
};
