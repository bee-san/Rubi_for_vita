/* The text box suggested on first use (#358, core/region_suggest.c): which of a job's lines are the
 * dialogue, the band it suggests (the frame's width, from half a line above the dialogue's first line to
 * the frame's bottom), its normalization as region.ini keeps a region, and each line's kana count. */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "region_suggest.h"
#include "regions.h"
#include "vocr_ocr.h" /* vjo_vocr_suggest: a job's stats */

#define W 960
#define H 544

/* A line at x, y of w x h px whose text holds `kana` kana and kanji. */
#define L(x, y, w, h, kana) ((VjoSuggestLine){(uint16_t)(x), (uint16_t)(y), (uint16_t)(w), (uint16_t)(h), (uint16_t)(kana)})

/* The pixels the kernel reads for a normalized rect (kernel/capture.c: the origin floored, the far edge
 * ceiled) on a fw x fh frame. */
static void pixels(const VjoRect *r, unsigned fw, unsigned fh, unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1)
{
    *x0 = (unsigned)((uint64_t)r->x * fw / 65536u);
    *y0 = (unsigned)((uint64_t)r->y * fh / 65536u);
    *x1 = (unsigned)(((uint64_t)(r->x + r->w) * fw + 65535u) / 65536u);
    *y1 = (unsigned)(((uint64_t)(r->y + r->h) * fh + 65535u) / 65536u);
}

/* The suggested band's top in frame pixels, or -1 for none; every band is the frame's width down to its
 * bottom, and a region region.ini keeps as it is. */
static int band_top(const VjoSuggestLine *l, unsigned n, unsigned fw, unsigned fh)
{
    VjoRect r;
    unsigned x0, y0, x1, y1;
    char line[80];
    VjoRect back;
    int ok = vjo_region_suggest(l, n, fw, fh, &r), len;
    if (!ok) {
        TEST_CHECK_(!r.x && !r.y && !r.w && !r.h, "no box: zeroed (%u,%u,%u,%u)", r.x, r.y, r.w, r.h);
        return -1;
    }
    pixels(&r, fw, fh, &x0, &y0, &x1, &y1);
    TEST_CHECK_(r.x == 0 && r.w == 65535 && x0 == 0 && x1 == fw, "the frame's width: %u,%u -> %u..%u", r.x, r.w, x0, x1);
    TEST_CHECK_(y1 == fh && r.h && (uint32_t)r.y + r.h <= 65536u, "down to the bottom: %u,%u -> %u..%u", r.y, r.h, y0, y1);
    len = vjo_region_format(line, sizeof(line), "PCSG00415", &r);
    TEST_CHECK(len > 0 && vjo_region_parse(line, (size_t)len, "PCSG00415", &back) == VJO_REGION_GAME &&
               !memcmp(&back, &r, sizeof(r)));
    return (int)y0;
}

static void counts_kana_and_kanji(void)
{
    TEST_CHECK(vjo_suggest_kana("") == 0);
    TEST_CHECK(vjo_suggest_kana(NULL) == 0);
    TEST_CHECK(vjo_suggest_kana("Auto Skip 123 !?") == 0);
    TEST_CHECK(vjo_suggest_kana("ひらがな") == 4);
    TEST_CHECK(vjo_suggest_kana("カタカナー・") == 6); /* U+30A0-30FF, the long vowel and the middle dot too */
    TEST_CHECK(vjo_suggest_kana("漢字") == 2);
    /* the brackets and full-width punctuation are not, nor half-width katakana, nor Hangul */
    TEST_CHECK(vjo_suggest_kana("「時間は流転し、」。！？") == 6);
    TEST_CHECK(vjo_suggest_kana("ｶﾀｶﾅ") == 0);
    TEST_CHECK(vjo_suggest_kana("한국어") == 0);
    TEST_CHECK(vjo_suggest_kana("Ａｕｔｏ") == 0);
    /* a sequence cut short counts nothing and stops nothing after it */
    TEST_CHECK(vjo_suggest_kana("\xE3\x81") == 0);
    TEST_CHECK(vjo_suggest_kana("\xE3\x81あ") == 1);
    TEST_CHECK(vjo_suggest_kana("\xE3あい\xF0\x9F\x98\x80う") == 3); /* an emoji is four bytes, not counted */
    {
        static char many[3 * 70000 + 1];
        for (int i = 0; i < 70000; i++)
            memcpy(many + 3 * i, "あ", 3);
        many[3 * 70000] = 0;
        TEST_CHECK(vjo_suggest_kana(many) == 65535); /* at most */
    }
}

/* A visual novel's screen: a picture's junk, a speaker's name above the box, two lines of dialogue and the
 * box's buttons below it, as the job on the whole frame reads them. */
static void dialogue_above_the_buttons(void)
{
    VjoSuggestLine l[] = {
        L(300, 40, 120, 60, 0),   /* a picture's edge read as text */
        L(118, 330, 96, 24, 3),   /* the speaker's name: 3 kana, its own tag above the box */
        L(118, 391, 722, 32, 28), /* the dialogue */
        L(118, 432, 345, 31, 12),
        L(700, 512, 230, 22, 0),  /* AUTO SKIP LOG: no kana */
    };
    /* the median of 31 and 32 is the lower, 31: 16 px above the first line */
    TEST_CHECK_(band_top(l, 5, W, H) == 391 - 16, "%d", band_top(l, 5, W, H));
    /* a name as close above the dialogue as its next line (a name line in the box) is in it: its median 31 */
    l[1] = L(118, 350, 96, 30, 3);
    TEST_CHECK_(band_top(l, 5, W, H) == 350 - 16, "%d", band_top(l, 5, W, H));
}

static void the_group_with_the_most_kana_not_the_lowest(void)
{
    const VjoSuggestLine l[] = {
        L(60, 380, 800, 36, 24), L(60, 422, 760, 36, 22), /* the dialogue */
        L(80, 505, 90, 24, 3), L(200, 505, 90, 24, 3),    /* セーブ ロード: 6 in all, lower */
    };
    TEST_CHECK(band_top(l, 4, W, H) == 380 - 18);
    /* alone, buttons side by side are a group each, too few to be dialogue */
    TEST_CHECK(band_top(l + 2, 2, W, H) == -1);
}

static void a_tie_goes_to_the_upper_group(void)
{
    const VjoSuggestLine l[] = {L(100, 100, 400, 30, 10), L(100, 400, 400, 30, 10)};
    TEST_CHECK(band_top(l, 2, W, H) == 100 - 15);
}

static void nothing_like_dialogue_no_box(void)
{
    VjoRect r = {1, 2, 3, 4};
    const VjoSuggestLine latin[] = {L(100, 400, 600, 32, 0), L(100, 440, 600, 32, 1)};
    const VjoSuggestLine few[] = {L(100, 400, 120, 32, 5)}; /* one line of 5: under the box's 6 */
    const VjoSuggestLine split[] = {L(100, 100, 120, 32, 3), L(100, 300, 120, 32, 3)}; /* 3 and 3, apart */
    TEST_CHECK(band_top(latin, 2, W, H) == -1);
    TEST_CHECK(band_top(few, 1, W, H) == -1);
    TEST_CHECK(band_top(split, 2, W, H) == -1);
    TEST_CHECK(band_top(NULL, 0, W, H) == -1);
    TEST_CHECK(band_top(few, 0, W, H) == -1);
    TEST_CHECK(!vjo_region_suggest(latin, 2, 0, H, &r) && !r.w);
    TEST_CHECK(!vjo_region_suggest(latin, 2, W, 0, &r) && !r.w);
    TEST_CHECK(!vjo_region_suggest(latin, 2, 70000u, H, &r) && !r.w);
}

/* Lines of dialogue's height only: 12-90 px of 544, in a frame of any size. */
static void dialogue_heights_only(void)
{
    const VjoSuggestLine tiny[] = {L(100, 400, 600, 11, 20)}, small[] = {L(100, 400, 600, 12, 20)};
    const VjoSuggestLine tall[] = {L(100, 300, 600, 91, 20)}, big[] = {L(100, 300, 600, 90, 20)};
    TEST_CHECK(band_top(tiny, 1, W, H) == -1);
    TEST_CHECK(band_top(small, 1, W, H) == 400 - 6);
    TEST_CHECK(band_top(tall, 1, W, H) == -1);
    TEST_CHECK(band_top(big, 1, W, H) == 300 - 45);
    /* a PSP game's own 480 x 272: 6-45 px */
    {
        const VjoSuggestLine p[] = {L(50, 200, 300, 5, 20), L(50, 210, 300, 6, 20)};
        TEST_CHECK(band_top(p, 1, 480, 272) == -1);
        TEST_CHECK(band_top(p + 1, 1, 480, 272) == 210 - 3);
    }
}

static void groups_by_gap_height_and_alignment(void)
{
    /* a gap of the taller line's height joins; one pixel more splits (the upper group has more kana) */
    const VjoSuggestLine joined[] = {L(100, 300, 600, 30, 4), L(100, 360, 600, 30, 4)};
    const VjoSuggestLine apart[] = {L(100, 300, 600, 30, 5), L(100, 361, 600, 30, 4)};
    TEST_CHECK(band_top(joined, 2, W, H) == 300 - 15);
    TEST_CHECK(band_top(apart, 2, W, H) == -1); /* 5 and 4: neither has 6 */
    /* heights within twice each other join; more apart split */
    {
        const VjoSuggestLine twice[] = {L(100, 300, 600, 20, 4), L(100, 330, 600, 40, 4)};
        const VjoSuggestLine more[] = {L(100, 300, 600, 20, 4), L(100, 330, 600, 41, 4)};
        TEST_CHECK(band_top(twice, 2, W, H) == 300 - 10); /* the median of 20 and 40: 20 */
        TEST_CHECK(band_top(more, 2, W, H) == -1);
    }
    /* lines that start within 40 px of each other, or overlap across, join; others split */
    {
        const VjoSuggestLine near[] = {L(100, 300, 100, 30, 4), L(140, 340, 100, 30, 4)};
        const VjoSuggestLine overlap[] = {L(100, 300, 100, 30, 4), L(199, 340, 100, 30, 4)};
        const VjoSuggestLine beside[] = {L(100, 300, 100, 30, 4), L(200, 340, 100, 30, 4)};
        TEST_CHECK(band_top(near, 2, W, H) == 285);
        TEST_CHECK(band_top(overlap, 2, W, H) == 285);
        TEST_CHECK(band_top(beside, 2, W, H) == -1);
    }
    /* a line overlapping the one above it (a gap below 0) joins */
    {
        const VjoSuggestLine o[] = {L(100, 300, 600, 30, 3), L(100, 320, 600, 30, 3)};
        TEST_CHECK(band_top(o, 2, W, H) == 285);
    }
}

static void any_order_the_same_box(void)
{
    const VjoSuggestLine a[] = {L(118, 432, 345, 31, 12), L(700, 512, 230, 22, 0), L(118, 391, 722, 32, 28)};
    const VjoSuggestLine b[] = {L(118, 391, 722, 32, 28), L(118, 432, 345, 31, 12), L(700, 512, 230, 22, 0)};
    VjoRect ra, rb;
    TEST_CHECK(vjo_region_suggest(a, 3, W, H, &ra) && vjo_region_suggest(b, 3, W, H, &rb));
    TEST_CHECK(!memcmp(&ra, &rb, sizeof(ra)));
}

static void the_median_height_and_the_screen_edges(void)
{
    /* three lines: the middle height; the margin rounded up */
    const VjoSuggestLine three[] = {L(60, 200, 600, 25, 9), L(60, 230, 600, 41, 9), L(60, 275, 600, 33, 9)};
    TEST_CHECK(band_top(three, 3, W, H) == 200 - 17);
    /* near the top: from the frame's first row */
    {
        const VjoSuggestLine top[] = {L(60, 10, 600, 30, 9)};
        TEST_CHECK(band_top(top, 1, W, H) == 0);
    }
    /* at the bottom: at least 32 px tall, the region selector's smallest box */
    {
        const VjoSuggestLine low[] = {L(60, 528, 600, 16, 9)};
        TEST_CHECK(band_top(low, 1, W, H) == H - 32);
    }
    /* lines that leave the frame are not taken */
    {
        const VjoSuggestLine out[] = {L(900, 300, 100, 30, 9), L(60, 530, 600, 30, 9)};
        TEST_CHECK(band_top(out, 2, W, H) == -1);
    }
}

static void at_most_sixteen_lines(void)
{
    VjoSuggestLine l[20];
    for (int i = 0; i < 20; i++)
        l[i] = L(60, 20 + 25 * i, 300, 20, i < 16 ? 0 : 30); /* the dialogue after the 16th */
    TEST_CHECK(band_top(l, 20, W, H) == -1);
    l[15].kana = 6;
    TEST_CHECK(band_top(l, 20, W, H) == 20 + 25 * 15 - 10);
}

/* Every box it gives, for any lines, is the frame's width down to its bottom and a region region.ini keeps. */
static void every_box_is_a_region(void)
{
    uint32_t seed = 12345u;
    unsigned boxes = 0;
    for (int t = 0; t < 20000; t++) {
        VjoSuggestLine l[16];
        unsigned n = (seed = seed * 1103515245u + 12345u) >> 28, fw = 200u + (seed >> 22) % 900u, fh;
        seed = seed * 1103515245u + 12345u;
        fh = 100u + (seed >> 22) % 500u;
        for (unsigned i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            l[i] = L((seed >> 8) % fw, (seed >> 16) % fh, 1 + (seed >> 4) % fw, 1 + (seed >> 12) % 100, (seed >> 20) % 12);
        }
        boxes += band_top(l, n, fw, fh) >= 0;
    }
    TEST_CHECK_(boxes > 100, "%u boxes", boxes);
}

/* The box as the result carries it (vjo_region_screen): the pixels the kernel reads for it on the 960 x 544 screen,
 * whatever the frame it was found on; nothing for an empty rect. */
static void the_box_on_the_screen(void)
{
    uint16_t b[4];
    const VjoSuggestLine l[] = {L(118, 391, 722, 32, 28), L(118, 432, 345, 31, 12)};
    VjoRect r;
    TEST_ASSERT(vjo_region_suggest(l, 2, W, H, &r));
    vjo_region_screen(&r, b);
    TEST_CHECK_(b[0] == 0 && b[1] == 391 - 16 && b[2] == 960 && b[3] == 544, "%u,%u,%u,%u", b[0], b[1], b[2], b[3]);
    /* a PSP game's own 480 x 272 frame: twice its pixels on the screen */
    {
        const VjoSuggestLine p[] = {L(59, 195, 361, 16, 28), L(59, 216, 172, 16, 12)};
        TEST_ASSERT(vjo_region_suggest(p, 2, 480, 272, &r));
        vjo_region_screen(&r, b);
        TEST_CHECK_(b[0] == 0 && b[1] == 2 * (195 - 8) && b[2] == 960 && b[3] == 544, "%u,%u,%u,%u", b[0], b[1], b[2],
                    b[3]);
    }
    r = (VjoRect){10, 10, 0, 100};
    vjo_region_screen(&r, b);
    TEST_CHECK(!b[0] && !b[1] && !b[2] && !b[3]);
    r = (VjoRect){10, 10, 100, 0};
    vjo_region_screen(&r, b);
    TEST_CHECK(!b[0] && !b[1] && !b[2] && !b[3]);
}

/* From a job's stats (the game worker's and host/page_ocr.c's call): its recognized lines' boxes and kana; nothing
 * for a job of columns. */
static void from_a_jobs_stats(void)
{
    static VjoVocrStats st;
    uint16_t b[4] = {1, 2, 3, 4};
    memset(&st, 0, sizeof(st));
    st.recognized = 2;
    st.line[0] = (VjoVocrLineStat){.width = 722, .height = 32, .kana = 28, .x = 118, .y = 391};
    st.line[1] = (VjoVocrLineStat){.width = 345, .height = 31, .kana = 12, .x = 118, .y = 432};
    TEST_CHECK(vjo_vocr_suggest(&st, W, H, b) && b[0] == 0 && b[1] == 375 && b[2] == 960 && b[3] == 544);
    st.columns = 1;
    TEST_CHECK(!vjo_vocr_suggest(&st, W, H, b) && !b[0] && !b[1] && !b[2] && !b[3]);
    st.columns = 0;
    st.recognized = 0;
    b[2] = 9;
    TEST_CHECK(!vjo_vocr_suggest(&st, W, H, b) && !b[2]);
    /* recognized is never taken past the stats' lines */
    st.recognized = 40;
    TEST_CHECK(vjo_vocr_suggest(&st, W, H, b) && b[1] == 375);
}

TEST_LIST = {
    {"counts kana and kanji", counts_kana_and_kanji},
    {"the dialogue above the buttons, the name left out", dialogue_above_the_buttons},
    {"the group with the most kana, not the lowest", the_group_with_the_most_kana_not_the_lowest},
    {"a tie goes to the upper group", a_tie_goes_to_the_upper_group},
    {"nothing like dialogue: no box", nothing_like_dialogue_no_box},
    {"dialogue heights only, in any frame", dialogue_heights_only},
    {"groups by gap, height and alignment", groups_by_gap_height_and_alignment},
    {"any order: the same box", any_order_the_same_box},
    {"the median height and the screen's edges", the_median_height_and_the_screen_edges},
    {"at most sixteen lines", at_most_sixteen_lines},
    {"every box is a region", every_box_is_a_region},
    {"the box on the screen", the_box_on_the_screen},
    {"from a job's stats", from_a_jobs_stats},
    {NULL, NULL}
};
