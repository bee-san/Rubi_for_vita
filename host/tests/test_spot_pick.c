/* Scan screen's picker (core/spot_pick.h): the map the game worker makes of a scan's set, and the Shell's
 * lookups in it. The d-pad and the stick go to the nearest block that way (ties, edges, a single block, no
 * block), every block that can be picked is reachable by them from the start, a touch picks the block under the
 * finger, the cursor starts on the pick, vertical blocks are drawn but never picked, at most VJO_SPOT_SHOWN blocks
 * are shown (the pick among them), and each block's rectangle is the region it is read as (the margin, clipped to
 * the screen, a PSP frame scaled to it). The Shell uses only a map whose indices are its blocks', and the
 * overlay's frames always use the blocks they draw. */
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
    /* from the dialogue: up is the name plate right above it, not the HUD label further up (nor the vertical
     * column); right and left none (no block reaches further that way than the wide box: the menu ends inside it,
     * the name plate starts where it starts, the HUD label is further up than left); down none */
    TEST_CHECK(vjo_spot_step(&m, 2, VJO_RSEL_UP) == 1);
    TEST_CHECK(vjo_spot_step(&m, 2, VJO_RSEL_RIGHT) == 2);
    TEST_CHECK(vjo_spot_step(&m, 2, VJO_RSEL_LEFT) == 2 && vjo_spot_step(&m, 2, VJO_RSEL_DOWN) == 2);
    /* from the name plate: up the HUD label, down the dialogue, right the menu (the dialogue starts where the
     * name plate starts: not to its right), left none */
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_UP) == 0 && vjo_spot_step(&m, 1, VJO_RSEL_DOWN) == 2);
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_RIGHT) == 3 && vjo_spot_step(&m, 1, VJO_RSEL_LEFT) == 1);
    /* from the menu: down the dialogue; left the name plate (670 against the wide box's 650 further left and
     * 100 across, twice); up and right none (the HUD label is further left than up) */
    TEST_CHECK(vjo_spot_step(&m, 3, VJO_RSEL_LEFT) == 1 && vjo_spot_step(&m, 3, VJO_RSEL_RIGHT) == 3);
    TEST_CHECK(vjo_spot_step(&m, 3, VJO_RSEL_DOWN) == 2 && vjo_spot_step(&m, 3, VJO_RSEL_UP) == 3);
    /* from the HUD label: right the menu (further right than down), not the dialogue (further down than right) */
    TEST_CHECK(vjo_spot_step(&m, 0, VJO_RSEL_RIGHT) == 3 && vjo_spot_step(&m, 0, VJO_RSEL_DOWN) == 1);
    /* the vertical column is never a destination */
    for (int i = 0; i < m.count; i++)
        for (int d = 0; d < 4; d++)
            TEST_CHECK(m.next[i][d] != 4 || i == 4);
    /* the repo's sample frame (host/fixtures/textspot): a HUD line at the top and the dialogue: left and right
     * stay, up and down go between the two */
    clear();
    block(0, 12, 10, 376, 28, 0);
    block(1, 43, 324, 562, 135, 0);
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_LEFT) == 1 && vjo_spot_step(&m, 1, VJO_RSEL_RIGHT) == 1);
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_UP) == 0 && vjo_spot_step(&m, 0, VJO_RSEL_DOWN) == 1);
    TEST_CHECK(vjo_spot_step(&m, 0, VJO_RSEL_LEFT) == 0 && vjo_spot_step(&m, 0, VJO_RSEL_RIGHT) == 0);
    /* a tie: two blocks at the same distance below (each as far down as across): the first in reading order */
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

/* The blocks the arrows reach from the start, a bit each (as the overlay can: only through next[][]). */
static unsigned reachable(const VjoSpotMap *m)
{
    unsigned seen = m->start >= 0 ? 1u << m->start : 0u, before;
    do {
        before = seen;
        for (int i = 0; i < m->count; i++)
            if (seen >> i & 1u)
                for (int d = 0; d < 4; d++)
                    seen |= 1u << m->next[i][d];
    } while (seen != before);
    return seen;
}

/* Every block that can be picked is reachable, every arrow leads to one of the map's blocks, never a vertical one
 * (unless it stays where it is). */
static int all_reachable(const VjoSpotMap *m)
{
    unsigned seen = reachable(m), want = 0;
    for (int i = 0; i < m->count; i++) {
        if (!(m->vertical >> i & 1u))
            want |= 1u << i;
        for (int d = 0; d < 4; d++)
            if (m->next[i][d] >= m->count || (m->next[i][d] != i && (m->vertical >> m->next[i][d] & 1u)))
                return 0;
    }
    return m->start < 0 ? want == 0 : (seen & want) == want;
}

/* The repo's grid frame (tools/spot_frames.py; the spotter's 20 blocks on it, host/fixtures/textspot): tall columns
 * beside stacks of lines, their regions overlapping by the margin. With the regions as the geometry, down from a
 * stack's first line went to the column beside it and three lines were never reached (#228's audit, N1). */
static const uint16_t grid[20][5] = {
    {11, 43, 106, 478, 0}, {131, 43, 114, 39, 0}, {131, 131, 114, 39, 0}, {131, 219, 114, 39, 0},
    {131, 307, 114, 39, 0}, {131, 395, 114, 39, 0}, {131, 483, 114, 39, 0}, {259, 43, 114, 478, 0},
    {379, 43, 122, 478, 0}, {523, 43, 114, 215, 0}, {523, 307, 114, 39, 0}, {523, 395, 114, 39, 0},
    {523, 483, 114, 39, 0}, {651, 43, 114, 39, 0}, {651, 131, 114, 39, 0}, {651, 219, 114, 39, 0},
    {651, 307, 114, 39, 0}, {651, 395, 114, 39, 0}, {651, 483, 114, 39, 0}, {779, 43, 114, 478, 0}};

static uint32_t lcg_state;
static unsigned rnd(unsigned n) /* 0..n-1, a fixed sequence */
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 8) % n;
}

static void every_block_is_reachable_by_the_arrows(void)
{
    VjoSpotMap m;
    /* the grid: all 16 shown reachable from the pick (8); down from the first line of the stack is the next line,
     * up from the stack's fourth line the third, not the column beside them */
    clear();
    for (unsigned i = 0; i < 20; i++)
        block(i, grid[i][0], grid[i][1], grid[i][2], grid[i][3], grid[i][4]);
    set.pick = 8;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.count == VJO_SPOT_SHOWN && m.start == 8 && all_reachable(&m));
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_DOWN) == 2 && vjo_spot_step(&m, 4, VJO_RSEL_UP) == 3);
    TEST_CHECK(vjo_spot_step(&m, 1, VJO_RSEL_RIGHT) == 7 && vjo_spot_step(&m, 8, VJO_RSEL_LEFT) == 7);
    /* one block inside another, and the same block twice: no arrow leads from one to the other by geometry */
    clear();
    block(0, 100, 100, 600, 300, 0);
    block(1, 300, 200, 100, 40, 0);
    set.pick = 0;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(all_reachable(&m));
    set.pick = 1;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(all_reachable(&m));
    clear();
    block(0, 200, 300, 500, 100, 0);
    block(1, 200, 300, 500, 100, 0);
    block(2, 50, 50, 100, 30, 0);
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(all_reachable(&m));
    /* 2,000 layouts of 2 to 20 blocks: lines, tall columns, wide bands, vertical ones, a duplicate, three frame
     * sizes */
    lcg_state = 228;
    for (int t = 0; t < 2000; t++) {
        static const unsigned size[3][2] = {{960, 544}, {480, 272}, {725, 544}};
        unsigned n = 2 + rnd(19), z = rnd(3), fw = size[z][0], fh = size[z][1];
        clear();
        for (unsigned i = 0; i < n; i++) {
            unsigned kind = rnd(10), w, h;
            if (kind == 0) {
                w = 20 + rnd(fw / 5);
                h = fh / 3 + rnd(fh - fh / 3);
            } else if (kind == 1) {
                w = fw / 4 + rnd(fw - fw / 4);
                h = 20 + rnd(fh / 5);
            } else {
                w = 1 + rnd(fw / 2);
                h = 1 + rnd(fh / 4);
            }
            if (i && rnd(20) == 0)
                set.block[i] = set.block[i - 1];
            else
                block(i, (int)rnd(fw - w + 1), (int)rnd(fh - h + 1), (int)w, (int)h, rnd(10) == 0);
            if (set.block_count <= i)
                set.block_count = i + 1;
        }
        set.pick = (int32_t)rnd(n + 1) - 1;
        vjo_spot_map(&set, fw, fh, &m);
        TEST_CHECK(all_reachable(&m) && vjo_spot_map_ok(&m));
        TEST_MSG("layout %d: %u blocks at %ux%u, start %d, reached %04x", t, n, fw, fh, m.start, reachable(&m));
    }
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
    set.pick = 40; /* past the shown ones: it takes the last place, and the cursor starts on it */
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.count == VJO_SPOT_SHOWN && m.start == VJO_SPOT_SHOWN - 1 && vjo_spot_map_ok(&m));
    for (int i = 0; i < m.count; i++)
        for (int d = 0; d < 4; d++)
            TEST_CHECK(m.next[i][d] < VJO_SPOT_SHOWN);
    /* block 40 (x 0, y 340) is in the last place; block 15 (x 840, y 68) and 16 (row 2) are not shown */
    TEST_CHECK(m.rect[15][0] == 0 && m.rect[15][1] == 340 - 8 && vjo_spot_hit(&m, 50, 340 + 15) == 15);
    TEST_CHECK(vjo_spot_hit(&m, 850, 68 + 15) == -1 && vjo_spot_hit(&m, 50, 2 * 68 + 15) == -1);
    set.pick = 9;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.start == 9 && m.rect[15][0] == 840 - 8 && m.rect[15][1] == 68 - 8);
    /* a pick that is no block of the set: the first shown */
    set.pick = (int32_t)VJO_SPOT_MAX;
    vjo_spot_map(&set, 960, 544, &m);
    TEST_CHECK(m.start == 0 && m.rect[15][0] == 840 - 8);
    /* the transport holds the whole set and the map */
    TEST_CHECK(sizeof(VjoSpotScan) == 1552 + 196);
}

/* The Shell publishes a map only when every index in it is one of its blocks: every map vjo_spot_map makes is
 * such a map; one with its count, start or a neighbour out of its blocks is not. */
static void the_shell_uses_only_a_map_within_its_blocks(void)
{
    VjoSpotMap m, good;
    vn_screen();
    vjo_spot_map(&set, 960, 544, &good);
    TEST_CHECK(vjo_spot_map_ok(&good));
    clear();
    vjo_spot_map(&set, 960, 544, &m); /* no block */
    TEST_CHECK(vjo_spot_map_ok(&m));
    memset(&m, 0, sizeof(m)); /* a failed scan's zeroed text: refused, and the Shell's empty map is the same */
    TEST_CHECK(!vjo_spot_map_ok(&m));
    m = good;
    m.count = VJO_SPOT_SHOWN + 1;
    TEST_CHECK(!vjo_spot_map_ok(&m));
    m.count = 255;
    TEST_CHECK(!vjo_spot_map_ok(&m));
    m = good;
    m.start = 5;
    TEST_CHECK(!vjo_spot_map_ok(&m));
    m.start = -1; /* none: allowed (the overlay draws by hand) */
    TEST_CHECK(vjo_spot_map_ok(&m));
    for (int i = 0; i < good.count; i++)
        for (int d = 0; d < 4; d++) {
            m = good;
            m.next[i][d] = good.count;
            TEST_CHECK(!vjo_spot_map_ok(&m));
            m.next[i][d] = 255;
            TEST_CHECK(!vjo_spot_map_ok(&m));
        }
    /* the neighbours past the count are not looked at (vjo_spot_map leaves them 0) */
    m = good;
    m.next[good.count][0] = 200;
    TEST_CHECK(vjo_spot_map_ok(&m));
}

/* The overlay's frames (vjo_spot_frame): waiting, the result, the arrows, a touch, enter and cancel; and every
 * result that arrives is drawn afresh, so what enter reads is what is drawn even when a second scan's result
 * lands in the same session (a scan asked for while another ran). */
static void the_overlay_s_frames_use_the_blocks_drawn(void)
{
    VjoSpotMap a, b, none;
    VjoSpotUi u;
    vn_screen();
    vjo_spot_map(&set, 960, 544, &a); /* 5 blocks, the start the dialogue (2) */
    clear();
    block(0, 100, 400, 700, 100, 0);
    vjo_spot_map(&set, 960, 544, &b); /* another screen: 1 block */
    clear();
    vjo_spot_map(&set, 960, 544, &none);
    /* waiting (seq 7 when the scan was asked for): nothing, until a touch (drawn by hand from there) or cancel */
    u = (VjoSpotUi){7, 1, 0};
    TEST_CHECK(vjo_spot_frame(&u, 7, &none, -2, 0, 0, 0) == VJO_SPOT_KEEP && u.state == 1);
    TEST_CHECK(vjo_spot_frame(&u, 7, &none, -2, 0, 0, VJO_RSEL_UP) == VJO_SPOT_KEEP);
    TEST_CHECK(vjo_spot_frame(&u, 7, &none, -1, 0, 0, 0) == VJO_SPOT_HAND);
    TEST_CHECK(vjo_spot_frame(&u, 7, &none, -2, 0, 1, 0) == VJO_SPOT_MANUAL);
    /* a result with no block to start on: drawn by hand */
    TEST_CHECK(vjo_spot_frame(&u, 8, &none, -2, 0, 0, 0) == VJO_SPOT_MANUAL);
    /* the result: shown, the cursor on its start; the arrows move (at an edge: nothing) */
    u = (VjoSpotUi){7, 1, 0};
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, 0) == VJO_SPOT_SHOW && u.state == 2 && u.sel == 2);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, 0) == VJO_SPOT_KEEP && u.sel == 2);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, VJO_RSEL_DOWN) == VJO_SPOT_KEEP && u.sel == 2);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, VJO_RSEL_UP) == VJO_SPOT_MOVE && u.sel == 1);
    /* two arrows at once: the first of up, right, down and left (here down) */
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, VJO_RSEL_DOWN | VJO_RSEL_LEFT) == VJO_SPOT_MOVE && u.sel == 2);
    /* enter reads the block the cursor is on; a touch reads the block under it; outside every block: by hand */
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 1, 0, 0) == VJO_SPOT_READ && u.sel == 2);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, 1, 0, 0, 0) == VJO_SPOT_READ && u.sel == 1);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -1, 1, 0, 0) == VJO_SPOT_HAND);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 1, 0) == VJO_SPOT_MANUAL);
    /* the audit's case (#228, F3): the cursor on block 3 of the first result, then a second scan's result, of
     * another screen, lands: it is drawn afresh with the cursor on its own start, and enter reads that block */
    u = (VjoSpotUi){7, 1, 0};
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, 0) == VJO_SPOT_SHOW);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, VJO_RSEL_UP) == VJO_SPOT_MOVE && u.sel == 1);
    TEST_CHECK(vjo_spot_frame(&u, 8, &a, -2, 0, 0, VJO_RSEL_RIGHT) == VJO_SPOT_MOVE && u.sel == 3);
    TEST_CHECK(vjo_spot_frame(&u, 9, &b, -2, 1, 0, 0) == VJO_SPOT_SHOW && u.sel == 0 && u.sel < b.count);
    TEST_CHECK(vjo_spot_frame(&u, 9, &b, -2, 0, 0, VJO_RSEL_LEFT) == VJO_SPOT_KEEP && u.sel == 0);
    TEST_CHECK(vjo_spot_frame(&u, 9, &b, -2, 1, 0, 0) == VJO_SPOT_READ && u.sel == 0);
    /* and a later result with no block: drawn by hand, not a pick out of the blocks drawn before */
    TEST_CHECK(vjo_spot_frame(&u, 10, &none, -2, 1, 0, 0) == VJO_SPOT_MANUAL);
}

TEST_LIST = {
    {"scan screen: each block's rectangle is the region it is read as", rects_are_the_regions_read},
    {"scan screen: the cursor starts on the pick, else the first block that can be picked", the_cursor_starts_on_the_pick},
    {"scan screen: the d-pad and the stick go to the nearest block that way", the_dpad_goes_to_the_nearest_block_that_way},
    {"scan screen: every block that can be picked is reachable by the arrows", every_block_is_reachable_by_the_arrows},
    {"scan screen: a touch picks the block under the finger", a_touch_picks_the_block_under_the_finger},
    {"scan screen: at most 16 blocks are shown, of a truncated 64-box set", at_most_sixteen_blocks_are_shown},
    {"scan screen: the Shell uses only a map within its blocks", the_shell_uses_only_a_map_within_its_blocks},
    {"scan screen: the overlay's frames use the blocks drawn", the_overlay_s_frames_use_the_blocks_drawn},
    {NULL, NULL}
};
