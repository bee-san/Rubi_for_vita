/* API 32, the Anki screenshot's black bars (core/shottrim.h) on synthetic frames: a letterbox, a
 * pillarbox, both, none; near-black content at an edge; an all-black frame; noisy bars; Adrenaline's
 * layouts of a PSP game's 480 x 272 picture in the Vita's 960 x 544 frame (its screen sizes 1.0x-2.0x,
 * nearest and blended edges); bars that frame no centred picture; the guess from the first rows and
 * when it is exact; and, on random frames, that every pixel outside the picture kept is black. The
 * game worker's use of it (the passes, the reads, the JPEG) is host/tests/test_game_shot.c's. */
#include "acutest.h"
#include "shottrim.h"

#include <stdio.h>
#include <string.h>

#define W 960
#define H 544
static uint32_t frame[W * H]; /* A8B8G8R8 words, R in the low byte */

static uint32_t rgb(unsigned r, unsigned g, unsigned b) { return 0xFF000000u | b << 16 | g << 8 | r; }
static void fill(uint32_t w, uint32_t h, uint32_t c)
{
    for (uint32_t i = 0; i < w * h; i++) frame[i] = c;
}
/* A picture that is never black: a gradient, and darker text-like strokes (each channel 40 or more). */
static void picture(uint32_t fw, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            unsigned v = 60 + (x * 150 / (w ? w : 1) + y * 40 / (h ? h : 1)) % 190;
            if ((x / 3) % 7 < 2 && (y / 5) % 4 < 3) v = 40;
            frame[(y0 + y) * fw + x0 + x] = rgb(v, v - 20, 255 - v);
        }
}
static void rect(uint32_t fw, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) frame[y * fw + x] = c;
}

/* The frame scanned as the game worker reads it, in strips of `strip` rows from the top. */
static VjoTrimScan scan_frame(uint32_t w, uint32_t h, uint32_t strip)
{
    VjoTrimScan s;
    vjo_trim_begin(&s, w, h);
    for (uint32_t y = 0; y < h; y += strip)
        vjo_trim_rows(&s, (const uint8_t *)(frame + (size_t)y * w), h - y < strip ? h - y : strip, w * 4u);
    return s;
}
static int same_scan(const VjoTrimScan *a, const VjoTrimScan *b)
{
    return a->w == b->w && a->h == b->h && a->rows == b->rows && a->top == b->top && a->last == b->last &&
           a->left == b->left && a->right == b->right && a->color == b->color && a->content == b->content &&
           a->uniform == b->uniform;
}
static int decide(uint32_t w, uint32_t h, VjoTrimRect *r)
{
    VjoTrimScan s = scan_frame(w, h, 64);
    return vjo_trim_decide(&s, r);
}
#define CHECK_RECT(r, X, Y, WW, HH)                                                                        \
    TEST_CHECK_((r).x == (X) && (r).y == (Y) && (r).w == (WW) && (r).h == (HH), "picture %ux%u at %u,%u, want %ux%u at %u,%u", \
                (r).w, (r).h, (r).x, (r).y, (unsigned)(WW), (unsigned)(HH), (unsigned)(X), (unsigned)(Y))

/* The guess after the rows the worker reads first: strips of 64 until a content row, or more than 3/10
 * of the frame's rows all black (game_ocr/main.c shot_pass). */
static int guess(uint32_t w, uint32_t h, VjoTrimRect *r, int *exact, uint32_t *rows_read)
{
    VjoTrimScan s;
    vjo_trim_begin(&s, w, h);
    while (!s.content && s.top * 10u <= h * 3u && s.rows < h) {
        uint32_t n = h - s.rows < 64u ? h - s.rows : 64u;
        vjo_trim_rows(&s, (const uint8_t *)(frame + (size_t)s.rows * w), n, w * 4u);
    }
    *rows_read = s.rows;
    return vjo_trim_guess(&s, r, exact);
}

/* No bars: the picture fills the frame. Decided by the first strip: the top edge and a side's reached. */
static void a_full_picture_has_no_bars(void)
{
    VjoTrimRect r;
    int exact;
    uint32_t rows;
    picture(W, 0, 0, W, H);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_NONE);
    CHECK_RECT(r, 0, 0, W, H);
    TEST_CHECK(guess(W, H, &r, &exact, &rows) == VJO_TRIM_NONE && exact && rows == 64);
    CHECK_RECT(r, 0, 0, W, H);
}

/* A letterbox (60 black rows above and below), a 4:3 pillarbox (117 columns each side), both (a PSP
 * game's 480 x 272 shown 1x): each axis trimmed by its narrower bar less 2 px, rounded down to even;
 * the guess from the first content rows is the same, but not exact (rows below could change it). */
static void letterbox_pillarbox_and_both(void)
{
    static const struct { uint32_t x, y, w, h, cx, cy, cw, ch; } cases[] = {
        {0, 60, W, H - 120, 0, 58, W, H - 116},       /* letterbox */
        {117, 0, W - 234, H, 114, 0, W - 228, H},     /* pillarbox: (117 - 2) & ~1 = 114 */
        {240, 136, 480, 272, 238, 134, 484, 276},     /* both: a PSP game 1x */
        {0, 9, W, H - 18, 0, 6, W, H - 12},           /* a 9-row letterbox: (9 - 2) & ~1 = 6 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoTrimRect r, g;
        int exact;
        uint32_t rows;
        fill(W, H, rgb(0, 0, 0));
        picture(W, cases[i].x, cases[i].y, cases[i].w, cases[i].h);
        TEST_CHECK_(decide(W, H, &r) == VJO_TRIM_TRIMMED, "case %u", i);
        CHECK_RECT(r, cases[i].cx, cases[i].cy, cases[i].cw, cases[i].ch);
        TEST_CHECK_(guess(W, H, &g, &exact, &rows) == VJO_TRIM_TRIMMED && !exact, "case %u: guess", i);
        CHECK_RECT(g, cases[i].cx, cases[i].cy, cases[i].cw, cases[i].ch);
        TEST_CHECK_(rows == (cases[i].y / 64u + 1u) * 64u, "case %u: %u rows read for the guess", i, rows);
    }
}

/* Adrenaline's screen sizes for a PSP game (its v2 changelog: 2.0x, 1.75x, 1.5x, 1.25x, 1.0x; since v6.3
 * any size), the 480 x 272 picture centred in the 960 x 544 frame: every smaller size trimmed to the picture
 * and 2 px of its bars; 2.0x untouched. With a blended edge (a bilinear filter's: the picture's outermost
 * pixels dark) the bars end where a pixel first reaches 16, and the 2 px kept hold the blend. */
static void adrenaline_layouts(void)
{
    static const struct { const char *name; uint32_t w, h; } sizes[] = {
        {"1.0x", 480, 272}, {"1.25x", 600, 340}, {"1.5x", 720, 408}, {"1.75x", 840, 476}, {"2.0x", 960, 544}};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (unsigned blend = 0; blend < 3; blend++) {
            uint32_t x0 = (W - sizes[i].w) / 2, y0 = (H - sizes[i].h) / 2, bar_x = x0, bar_y = y0;
            VjoTrimRect r;
            fill(W, H, rgb(0, 0, 0));
            picture(W, x0, y0, sizes[i].w, sizes[i].h);
            if (blend) { /* the picture's edge blended with the black around it: 10 (still black) or 20 (not) */
                unsigned v = blend == 1 ? 10 : 20;
                uint32_t c = rgb(v, v, v);
                rect(W, x0, y0, sizes[i].w, 1, c);
                rect(W, x0, y0 + sizes[i].h - 1, sizes[i].w, 1, c);
                rect(W, x0, y0, 1, sizes[i].h, c);
                rect(W, x0 + sizes[i].w - 1, y0, 1, sizes[i].h, c);
                if (blend == 1) bar_x++, bar_y++;
            }
            int kind = decide(W, H, &r);
            if (sizes[i].w == W && blend != 1) {
                TEST_CHECK_(kind == VJO_TRIM_NONE, "%s blend %u: %d", sizes[i].name, blend, kind);
                CHECK_RECT(r, 0, 0, W, H);
                continue;
            }
            if (sizes[i].w == W) { /* a 1-px "bar" of blend: none */
                TEST_CHECK_(kind == VJO_TRIM_NONE, "%s blend %u: %d", sizes[i].name, blend, kind);
                continue;
            }
            uint32_t cx = (bar_x - 2u) & ~1u, cy = (bar_y - 2u) & ~1u;
            TEST_CHECK_(kind == VJO_TRIM_TRIMMED, "%s blend %u: %d", sizes[i].name, blend, kind);
            CHECK_RECT(r, cx, cy, W - 2 * cx, H - 2 * cy);
            /* the picture whole inside it, its blended edge too */
            TEST_CHECK_(r.x <= x0 && r.y <= y0 && r.x + r.w >= x0 + sizes[i].w && r.y + r.h >= y0 + sizes[i].h,
                        "%s blend %u: the picture is cut", sizes[i].name, blend);
        }
}

/* Black is every channel under 16: a picture's near-black pixels at an edge (15) are bar; one that
 * reaches 16 in any channel is content. A side whose bar has one content pixel is no bar, and with
 * the other side's then alone the frame frames no centred picture: kept whole. */
static void near_black_content_at_an_edge(void)
{
    VjoTrimRect r;
    /* a pillarbox; in rows 200-299 a dark object reaches the left edge */
    static const struct { uint32_t c; int kind; } objects[] = {
        {0x000F0F0Fu, VJO_TRIM_TRIMMED}, /* 15, 15, 15: black */
        {0x00101010u, VJO_TRIM_KEPT},    /* 16, 16, 16: content */
        {0x00000010u, VJO_TRIM_KEPT},    /* R 16 alone */
        {0x00100000u, VJO_TRIM_KEPT},    /* B 16 alone */
        {0x80000000u, VJO_TRIM_TRIMMED}, /* alpha only: black */
    };
    for (unsigned i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        fill(W, H, rgb(0, 0, 0));
        picture(W, 117, 0, W - 234, H);
        rect(W, 0, 200, 140, 100, objects[i].c | 0xFF000000u * !(objects[i].c >> 24));
        TEST_CHECK_(decide(W, H, &r) == objects[i].kind, "object 0x%08X", objects[i].c);
        if (objects[i].kind == VJO_TRIM_KEPT) CHECK_RECT(r, 0, 0, W, H);
        else CHECK_RECT(r, 114, 0, W - 228, H);
    }
    /* one pixel of content in a letterbox's bottom bar: that bar ends there (3 rows), no pair: kept whole */
    fill(W, H, rgb(0, 0, 0));
    picture(W, 0, 60, W, H - 120);
    frame[(H - 3) * W + 500] = rgb(0, 16, 0);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_KEPT);
    CHECK_RECT(r, 0, 0, W, H);
}

/* An all-black frame (a fade, a black screen) is kept whole; its guess is exact once more than 3/10 of
 * its rows are black (no letterbox is that tall), after 192 of 544 rows; exactly black is one colour. */
static void an_all_black_frame_is_kept_whole(void)
{
    VjoTrimRect r;
    int exact;
    uint32_t rows;
    VjoTrimScan s;
    fill(W, H, rgb(0, 0, 0));
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_KEPT);
    CHECK_RECT(r, 0, 0, W, H);
    TEST_CHECK(guess(W, H, &r, &exact, &rows) == VJO_TRIM_KEPT && exact && rows == 192);
    CHECK_RECT(r, 0, 0, W, H);
    s = scan_frame(W, H, 64);
    TEST_CHECK(!s.content && s.uniform && s.color == 0 && s.top == H);
    /* a black of another value, still one colour; one pixel apart: no more */
    fill(W, H, rgb(3, 2, 1));
    s = scan_frame(W, H, 64);
    TEST_CHECK(!s.content && s.uniform && s.color == 0x00010203u);
    frame[100 * W + 7] = rgb(3, 2, 2);
    s = scan_frame(W, H, 64);
    TEST_CHECK(!s.content && !s.uniform);
}

/* Noisy bars (a JPEG's or a scaler's: every channel 0-15) are bars as clean ones are, one colour they are
 * not. A black band over 3/10 of the frame at its top, then content (a text box on a black screen): kept
 * whole, and the guess says so as soon as the band is taller than 3/10. */
static void noisy_bars_and_a_tall_black_band(void)
{
    VjoTrimRect r;
    int exact;
    uint32_t rows, v = 7;
    VjoTrimScan s;
    for (uint32_t i = 0; i < W * H; i++) {
        v = v * 1103515245u + 12345u;
        frame[i] = 0xFF000000u | ((v >> 8) & 0x000F0F0Fu);
    }
    picture(W, 240, 136, 480, 272);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_TRIMMED);
    CHECK_RECT(r, 238, 134, 484, 276);
    s = scan_frame(W, H, 64);
    TEST_CHECK(!s.uniform);
    /* the black screen with a text box from row 380 */
    fill(W, H, rgb(0, 0, 0));
    picture(W, 20, 380, W - 40, 150);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_KEPT);
    CHECK_RECT(r, 0, 0, W, H);
    TEST_CHECK(guess(W, H, &r, &exact, &rows) == VJO_TRIM_KEPT && exact && rows == 192);
    s = scan_frame(W, H, 64);
    TEST_CHECK(s.uniform && s.top == 380);
    /* the band ends in the strip that has the first content row, past 3/10 (170 of 544 rows): exact too */
    fill(W, H, rgb(0, 0, 0));
    picture(W, 0, 170, W, 300);
    TEST_CHECK(guess(W, H, &r, &exact, &rows) == VJO_TRIM_KEPT && exact && rows == 192);
    CHECK_RECT(r, 0, 0, W, H);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_KEPT);
    /* 163 rows (not past 3/10): a guess, a letterbox with a bottom bar as tall, not exact */
    fill(W, H, rgb(0, 0, 0));
    picture(W, 0, 163, W, 218);
    TEST_CHECK(guess(W, H, &r, &exact, &rows) == VJO_TRIM_TRIMMED && !exact && rows == 192);
    CHECK_RECT(r, 0, 160, W, H - 320);
}

/* Bars that frame no centred picture: one bar alone (a dark sky), two too unequal (the narrower under
 * 3/4 of the wider), one over 3/10 of the side (a small logo), a pair beside an axis that is neither:
 * kept whole. Under 8 px both: no bars. The limits themselves. */
static void bars_that_frame_no_centred_picture(void)
{
    static const struct { const char *name; uint32_t top, bottom, left, right; int kind; uint32_t cx, cy; } cases[] = {
        {"one bar alone (top)", 100, 0, 0, 0, VJO_TRIM_KEPT, 0, 0},
        {"one bar alone (left)", 0, 0, 50, 0, VJO_TRIM_KEPT, 0, 0},
        {"top 60, bottom 40: under 3/4", 60, 40, 0, 0, VJO_TRIM_KEPT, 0, 0},
        {"top 60, bottom 45: 3/4", 60, 45, 0, 0, VJO_TRIM_TRIMMED, 0, 42},
        {"left 29, right 50 (a vignette)", 0, 0, 29, 50, VJO_TRIM_KEPT, 0, 0},
        {"a centred logo: bars over 3/10", 222, 222, 330, 330, VJO_TRIM_KEPT, 0, 0},
        {"sides of 288 (3/10 of 960)", 0, 0, 288, 288, VJO_TRIM_TRIMMED, 286, 0},
        {"sides of 289", 0, 0, 289, 289, VJO_TRIM_KEPT, 0, 0},
        {"top and bottom of 163 (3/10 of 544: 163.2)", 163, 163, 0, 0, VJO_TRIM_TRIMMED, 0, 160},
        {"top and bottom of 164", 164, 164, 0, 0, VJO_TRIM_KEPT, 0, 0},
        {"7 px all round: no bars", 7, 7, 7, 7, VJO_TRIM_NONE, 0, 0},
        {"8 px all round", 8, 8, 8, 8, VJO_TRIM_TRIMMED, 6, 6},
        {"sides of 7 and 9", 0, 0, 7, 9, VJO_TRIM_KEPT, 0, 0},
        {"a pillarbox and a bottom bar alone", 0, 21, 74, 69, VJO_TRIM_KEPT, 0, 0},
        {"a pillarbox and 7 rows above, 5 below", 7, 5, 120, 120, VJO_TRIM_TRIMMED, 118, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoTrimRect r;
        fill(W, H, rgb(0, 0, 0));
        picture(W, cases[i].left, cases[i].top, W - cases[i].left - cases[i].right, H - cases[i].top - cases[i].bottom);
        TEST_CHECK_(decide(W, H, &r) == cases[i].kind, "%s", cases[i].name);
        CHECK_RECT(r, cases[i].cx, cases[i].cy, W - 2 * cases[i].cx, H - 2 * cases[i].cy);
    }
}

/* The guess from the first rows: a pillarbox's first strip gives it; one whose lower rows reach the edges
 * is guessed wrong (the decision once every row is scanned differs: the game worker reads it again); a
 * frame shown smaller than the Vita's (480 x 272, a game that renders so) has bars of its own size. */
static void the_guess_and_other_frame_sizes(void)
{
    VjoTrimRect r, g;
    int exact;
    uint32_t rows;
    fill(W, H, rgb(0, 0, 0));
    picture(W, 117, 0, W - 234, H);
    picture(W, 0, 300, W, 40); /* lower rows across the whole width */
    TEST_CHECK(guess(W, H, &g, &exact, &rows) == VJO_TRIM_TRIMMED && !exact && rows == 64);
    CHECK_RECT(g, 114, 0, W - 228, H);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_NONE);
    CHECK_RECT(r, 0, 0, W, H);
    /* a top bar guessed with a bottom bar as wide; the bottom's narrower (pairing): the narrower decides */
    fill(W, H, rgb(0, 0, 0));
    picture(W, 0, 60, W, H - 60 - 50);
    TEST_CHECK(guess(W, H, &g, &exact, &rows) == VJO_TRIM_TRIMMED && !exact && rows == 64);
    CHECK_RECT(g, 0, 58, W, H - 116);
    TEST_CHECK(decide(W, H, &r) == VJO_TRIM_TRIMMED);
    CHECK_RECT(r, 0, 48, W, H - 96);
    /* 480 x 272: bars of 3/10 of it at most (81 rows, 144 columns) */
    fill(480, 272, rgb(0, 0, 0));
    picture(480, 60, 34, 360, 204);
    TEST_CHECK(decide(480, 272, &r) == VJO_TRIM_TRIMMED);
    CHECK_RECT(r, 58, 32, 364, 208);
    fill(480, 272, rgb(0, 0, 0));
    picture(480, 0, 82, 480, 108);
    TEST_CHECK(decide(480, 272, &r) == VJO_TRIM_KEPT);
}

/* Frames shorter than 27 rows (no Vita frame is; the kernel reports what the game shows): 3/10 of the height
 * is then under a bar's 8 rows, and a black band at the top that is over 3/10 but under 8 rows is no bar, as
 * the rule says, not a tall band: the guess is the rule's decision (audit-blackbars). */
static void tiny_frames_follow_the_rule(void)
{
    static const struct { uint32_t h, top, bottom, side; int kind; } cases[] = {
        {8, 3, 3, 200, VJO_TRIM_TRIMMED},   /* a 3-row band, 3/10 of 8 is 2.4: no bar; the pillarbox trimmed */
        {8, 0, 0, 200, VJO_TRIM_TRIMMED},
        {20, 7, 7, 120, VJO_TRIM_TRIMMED},
        {20, 7, 0, 0, VJO_TRIM_NONE},       /* bars under 8 rows only */
        {26, 8, 8, 0, VJO_TRIM_KEPT},       /* 8 rows are a bar, over 3/10 of 26 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoTrimRect g, r;
        int exact;
        uint32_t rows, h = cases[i].h;
        fill(W, h, rgb(0, 0, 0));
        picture(W, cases[i].side, cases[i].top, W - 2 * cases[i].side, h - cases[i].top - cases[i].bottom);
        int kind = decide(W, h, &r), gk = guess(W, h, &g, &exact, &rows);
        TEST_CHECK_(kind == cases[i].kind, "case %u: %d", i, kind);
        /* the whole frame in its first strip: the guess is exact and is the decision */
        TEST_CHECK_(rows == h && exact && gk == kind && g.x == r.x && g.y == r.y && g.w == r.w && g.h == r.h,
                    "case %u: the guess %ux%u at %u,%u (exact %d, kind %d), the decision %ux%u at %u,%u", i, g.w, g.h, g.x,
                    g.y, exact, gk, r.w, r.h, r.x, r.y);
        /* read 4 rows at a time instead: the guess after the first content row is never an exact refusal of a band
         * under 8 rows; exact only when it is the decision */
        VjoTrimScan sc;
        vjo_trim_begin(&sc, W, h);
        while (!sc.content && sc.top * 10u <= h * 3u && sc.rows < h)
            vjo_trim_rows(&sc, (const uint8_t *)(frame + (size_t)sc.rows * W), h - sc.rows < 4u ? h - sc.rows : 4u, W * 4u);
        gk = vjo_trim_guess(&sc, &g, &exact);
        TEST_CHECK_(!exact || (gk == kind && g.x == r.x && g.y == r.y && g.w == r.w && g.h == r.h),
                    "case %u, 4-row strips: an exact guess %ux%u at %u,%u (kind %d) that is not the decision %ux%u at %u,%u", i,
                    g.w, g.h, g.x, g.y, gk, r.w, r.h, r.x, r.y);
    }
}

/* The scan's state is the rows', whatever strips they come in (the hook's 64, the kernel strip's 16, one
 * at a time), and rows past the frame's last are not read. */
static void any_strips_give_the_same_scan(void)
{
    fill(W, H, rgb(0, 0, 0));
    picture(W, 180, 102, 600, 340);
    frame[300 * W + 150] = rgb(90, 90, 90); /* content in the left bar, one row */
    VjoTrimScan a = scan_frame(W, H, 64), b = scan_frame(W, H, 16), c = scan_frame(W, H, 1), d = scan_frame(W, H, 37);
    TEST_CHECK(same_scan(&a, &b) && same_scan(&a, &c) && same_scan(&a, &d));
    TEST_CHECK(a.rows == H && a.top == 102 && a.last == 441 && a.left == 150 && a.right == 180);
    VjoTrimScan e;
    vjo_trim_begin(&e, W, H);
    vjo_trim_rows(&e, (const uint8_t *)frame, H + 100, W * 4u); /* clamped to the frame */
    TEST_CHECK(e.rows == H && same_scan(&a, &e));
}

/* Random frames (bars of any widths, near-black and black pixels in the picture, noise in the bars): the
 * picture kept is centred, at even offsets, at least 2/5 of each side when trimmed, and every pixel
 * outside it is black; each trimmed side keeps at least 2 black rows or columns beside the picture. */
static void random_frames_never_lose_content(void)
{
    uint32_t v = 12345, trimmed = 0;
    for (unsigned n = 0; n < 400; n++) {
        uint32_t w = n % 3 ? W : 480, h = n % 3 ? H : 272, t, b, l, rr;
        /* bars mostly a near pair up to 7/20 of the side (some no pair, some too wide), now and then any */
        v = v * 1103515245u + 12345u; t = (v >> 8) % (h * 7 / 20);
        v = v * 1103515245u + 12345u; b = n % 4 ? t + (v >> 8) % 9 - 4 : (v >> 8) % (h / 2);
        v = v * 1103515245u + 12345u; l = (v >> 8) % (w * 7 / 20);
        v = v * 1103515245u + 12345u; rr = n % 5 ? l + (v >> 8) % 9 - 4 : (v >> 8) % (w / 2);
        if ((int32_t)b < 0 || b >= h - t) b = 0;
        if ((int32_t)rr < 0 || rr >= w - l) rr = 0;
        for (uint32_t i = 0; i < w * h; i++) { /* noisy black */
            v = v * 1103515245u + 12345u;
            frame[i] = 0xFF000000u | ((v >> 8) & 0x00070707u);
        }
        picture(w, l, t, w - l - rr, h - t - b);
        for (unsigned k = 0; k < 50; k++) { /* black and near-black pixels inside the picture */
            v = v * 1103515245u + 12345u;
            uint32_t x = l + (v >> 8) % (w - l - rr), y = t + (v >> 20) % (h - t - b);
            frame[y * w + x] = k & 1 ? rgb(15, 15, 15) : rgb(0, 0, 0);
        }
        VjoTrimRect r;
        int kind = decide(w, h, &r);
        TEST_CHECK_(r.x + r.w <= w && r.y + r.h <= h && r.x * 2 + r.w == w && r.y * 2 + r.h == h && !(r.x & 1) && !(r.y & 1),
                    "frame %u: %ux%u at %u,%u", n, r.w, r.h, r.x, r.y);
        if (kind != VJO_TRIM_TRIMMED) {
            TEST_CHECK_(r.x == 0 && r.y == 0, "frame %u: kept, but %u,%u", n, r.x, r.y);
            continue;
        }
        trimmed++;
        TEST_CHECK_(r.w * 5 >= w * 2 && r.h * 5 >= h * 2, "frame %u: %ux%u is under 2/5", n, r.w, r.h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                int inside = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
                int margin = inside && ((r.x && (x < r.x + 2 || x >= r.x + r.w - 2)) ||
                                        (r.y && (y < r.y + 2 || y >= r.y + r.h - 2)));
                if ((!inside || margin) && (frame[y * w + x] & VJO_TRIM_BLACK_MASK)) {
                    TEST_CHECK_(0, "frame %u: content at %u,%u %s the picture %ux%u at %u,%u", n, x, y,
                                inside ? "in the margin of" : "outside", r.w, r.h, r.x, r.y);
                    y = h;
                    break;
                }
            }
    }
    TEST_CHECK(trimmed > 150);
    printf("measure: %u of 400 random frames trimmed\n", trimmed);
}

TEST_LIST = {
    {"API 32: a full picture has no bars, decided by the first rows", a_full_picture_has_no_bars},
    {"API 32: a letterbox, a pillarbox and both", letterbox_pillarbox_and_both},
    {"API 32: Adrenaline's layouts of a PSP game, edges sharp and blended", adrenaline_layouts},
    {"API 32: near-black content at an edge", near_black_content_at_an_edge},
    {"API 32: an all-black frame is kept whole", an_all_black_frame_is_kept_whole},
    {"API 32: noisy bars, and a tall black band at the top", noisy_bars_and_a_tall_black_band},
    {"API 32: bars that frame no centred picture, and the limits", bars_that_frame_no_centred_picture},
    {"API 32: the guess from the first rows, and frames of another size", the_guess_and_other_frame_sizes},
    {"API 32: any strips give the same scan", any_strips_give_the_same_scan},
    {"API 32: frames shorter than 27 rows follow the rule", tiny_frames_follow_the_rule},
    {"API 32: random frames never lose content", random_frames_never_lose_content},
    {NULL, NULL}};
