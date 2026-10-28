/* core/fbgrey.h's word-at-a-time grey (vjo_grey_abgr, vjo_grey_a1b5g5r5 and
 * their row forms) gives exactly the bytes of the per-channel conversion it
 * replaced (the reference below is that code, verbatim from 4a7ae4d): for every
 * R, G, B, every alpha pattern, every A1B5G5R5 value and every top-8-bit
 * A2B10G10R10 channel with each of the low bits (that format still expands to
 * A8B8G8R8 first). Exhaustive, so it also runs on the Vita's CPU under qemu-arm
 * (tools/arm_host_tests.sh). */
#include "acutest.h"
#include <stdint.h>
#include <string.h>

#include "fbgrey.h"

/* ---- the reference: core/fbgrey.h at 4a7ae4d ---- */
static unsigned ref_grey(unsigned r, unsigned g, unsigned b)
{
    return (77u * r + 150u * g + 29u * b) >> 8;
}

static void ref_fb_to_abgr(uint32_t *dst, const void *src, unsigned n, uint32_t fmt)
{
    unsigned i;
    if (fmt == VJO_FB_A2B10G10R10) {
        const uint32_t *s = (const uint32_t *)src;
        for (i = 0; i < n; i++) {
            uint32_t v = s[i];
            uint32_t r = (v >> 2) & 0xFFu, g = (v >> 12) & 0xFFu, b = (v >> 22) & 0xFFu;
            dst[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    } else if (fmt == VJO_FB_A1B5G5R5) {
        const uint16_t *s = (const uint16_t *)src;
        for (i = 0; i < n; i++) {
            uint32_t v = s[i];
            uint32_t r = (v & 0x1Fu) << 3, g = ((v >> 5) & 0x1Fu) << 3, b = ((v >> 10) & 0x1Fu) << 3;
            dst[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    } else {
        const uint32_t *s = (const uint32_t *)src;
        for (i = 0; i < n; i++) dst[i] = s[i];
    }
}

static void ref_abgr_to_grey(unsigned char *dst, const uint32_t *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        uint32_t v = src[i];
        dst[i] = (unsigned char)ref_grey(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu);
    }
}

/* ---- the checks ---- */

#define ROW 4096u

static uint32_t words[ROW], abgr[ROW];
static uint16_t halves[ROW];
static unsigned char got[ROW], want[ROW];

/* Row form of one format (the kernel's path for it) against the reference's two
 * passes; n pixels in words/halves. */
static int row_matches(uint32_t fmt, unsigned n)
{
    const void *src = fmt == VJO_FB_A1B5G5R5 ? (const void *)halves : (const void *)words;
    ref_fb_to_abgr(abgr, src, n, fmt);
    ref_abgr_to_grey(want, abgr, n);
    memset(got, 0xA5, sizeof(got));
    if (fmt == VJO_FB_A1B5G5R5) {
        vjo_a1b5g5r5_to_grey(got, halves, n);
    } else {
        static uint32_t mine[ROW];
        vjo_fb_to_abgr(mine, src, n, fmt);
        vjo_abgr_to_grey(got, mine, n);
    }
    return !memcmp(got, want, n) && got[n] == 0xA5; /* nothing written past n */
}

static void every_rgb_in_a8b8g8r8(void)
{
    static const uint32_t alphas[] = {0x00u, 0xFFu, 0x80u, 0x7Fu};
    unsigned bad = 0;
    for (uint32_t rgb = 0; rgb < (1u << 24); rgb++) {
        uint32_t r = rgb & 0xFFu, g = (rgb >> 8) & 0xFFu, b = rgb >> 16;
        uint32_t v = alphas[rgb & 3u] << 24 | rgb;
        bad += vjo_grey_abgr(v) != ref_grey(r, g, b);
        bad += vjo_grey(r, g, b) != ref_grey(r, g, b);
    }
    TEST_CHECK(bad == 0);
    TEST_MSG("%u mismatches", bad);
}

static void every_value_in_a1b5g5r5(void)
{
    unsigned bad = 0;
    for (uint32_t v = 0; v < 65536u; v++) {
        uint32_t e;
        uint16_t h = (uint16_t)v;
        ref_fb_to_abgr(&e, &h, 1, VJO_FB_A1B5G5R5);
        bad += vjo_grey_a1b5g5r5(v) != ref_grey(e & 0xFFu, (e >> 8) & 0xFFu, (e >> 16) & 0xFFu);
    }
    TEST_CHECK(bad == 0);
    TEST_MSG("%u mismatches", bad);
}

static void every_channel_in_a2b10g10r10(void)
{
    unsigned bad = 0;
    /* Every top-8-bit R, G, B; the two low bits of each channel and the 2-bit
     * alpha cycle through all their values across the sweep. The kernel expands
     * it to A8B8G8R8 (vjo_fb_to_abgr, unchanged) and takes vjo_grey_abgr. */
    for (uint32_t rgb = 0; rgb < (1u << 24); rgb++) {
        uint32_t r = rgb & 0xFFu, g = (rgb >> 8) & 0xFFu, b = rgb >> 16, k = (rgb * 2654435761u) >> 24;
        uint32_t v = (k >> 6) << 30 | (b << 2 | (k & 3u)) << 20 | (g << 2 | ((k >> 2) & 3u)) << 10 |
                     (r << 2 | ((k >> 4) & 3u));
        uint32_t e, mine;
        ref_fb_to_abgr(&e, &v, 1, VJO_FB_A2B10G10R10);
        vjo_fb_to_abgr(&mine, &v, 1, VJO_FB_A2B10G10R10);
        bad += vjo_grey_abgr(mine) != ref_grey(e & 0xFFu, (e >> 8) & 0xFFu, (e >> 16) & 0xFFu);
    }
    TEST_CHECK(bad == 0);
    TEST_MSG("%u mismatches", bad);
}

/* The row functions, at every length up to a 960-pixel row and a ragged tail,
 * on pseudo-random pixels of each format. */
static void rows_match_the_two_pass_conversion(void)
{
    uint32_t x = 0x12345678u;
    for (unsigned i = 0; i < ROW; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        words[i] = x;
        halves[i] = (uint16_t)(x >> 7);
    }
    for (unsigned n = 0; n <= 961; n += n < 70 ? 1 : 37) {
        TEST_CHECK(row_matches(VJO_FB_A8B8G8R8, n));
        TEST_CHECK(row_matches(VJO_FB_A2B10G10R10, n));
        TEST_CHECK(row_matches(VJO_FB_A1B5G5R5, n));
        TEST_MSG("n = %u", n);
    }
    TEST_CHECK(row_matches(VJO_FB_A8B8G8R8, ROW - 1));
    TEST_CHECK(row_matches(VJO_FB_A2B10G10R10, ROW - 1));
    TEST_CHECK(row_matches(VJO_FB_A1B5G5R5, ROW - 1));
    /* vjo_abgr_to_grey itself, the form the signature rows take. */
    ref_abgr_to_grey(want, words, ROW);
    vjo_abgr_to_grey(got, words, ROW);
    TEST_CHECK(!memcmp(got, want, ROW));
}

/* The kernel's one scratch row (kernel/capture.c): vjo_fb_to_abgr_inplace gives vjo_fb_to_abgr's words (and the
 * reference's) in the source's own buffer, for each format at every length to a 960-pixel row (VJO_MAX_W), odd ones
 * included, and a 4,095-pixel one, and writes nothing past the row's words; the grey rows in place
 * (vjo_abgr_to_grey from those words, vjo_a1b5g5r5_to_grey from the 16-bit pixels) give the bytes they give
 * apart. */
#define GUARD 0x5AA5C33Cu
static uint32_t inplace[ROW + 4];

static int inplace_matches(uint32_t fmt, unsigned n)
{
    const void *src = fmt == VJO_FB_A1B5G5R5 ? (const void *)halves : (const void *)words;
    unsigned bytes = n * vjo_fb_bpp(fmt), bad = 0;
    static unsigned char grey[ROW];
    ref_fb_to_abgr(abgr, src, n, fmt);
    for (unsigned i = 0; i < ROW + 4; i++) inplace[i] = GUARD;
    memcpy(inplace, src, bytes);
    vjo_fb_to_abgr_inplace(inplace, n, fmt);
    bad += memcmp(inplace, abgr, (size_t)n * 4) != 0;
    for (unsigned i = n; i < ROW + 4; i++) bad += inplace[i] != GUARD; /* nothing past the row */
    /* the grey row in place, from the words just made: the bytes vjo_abgr_to_grey gives apart */
    vjo_abgr_to_grey(grey, abgr, n);
    vjo_abgr_to_grey((unsigned char *)inplace, inplace, n);
    bad += memcmp(inplace, grey, n) != 0;
    if (fmt == VJO_FB_A1B5G5R5) { /* the one-pass 16-bit grey row in place */
        vjo_a1b5g5r5_to_grey(grey, halves, n);
        memcpy(inplace, halves, (size_t)n * 2);
        vjo_a1b5g5r5_to_grey((unsigned char *)inplace, (const uint16_t *)inplace, n);
        bad += memcmp(inplace, grey, n) != 0;
    }
    return bad == 0;
}

static void rows_convert_in_place(void)
{
    static const uint32_t formats[] = {VJO_FB_A8B8G8R8, VJO_FB_A2B10G10R10, VJO_FB_A1B5G5R5};
    uint32_t x = 0x9E3779B9u;
    for (unsigned i = 0; i < ROW; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        words[i] = x;
        halves[i] = (uint16_t)(x >> 11);
    }
    for (unsigned f = 0; f < 3; f++) {
        for (unsigned n = 0; n <= 961; n++)
            if (!inplace_matches(formats[f], n)) {
                TEST_CHECK_(0, "format 0x%08X, %u pixels", (unsigned)formats[f], n);
                break;
            }
        TEST_CHECK_(inplace_matches(formats[f], ROW - 1), "format 0x%08X, %u pixels", (unsigned)formats[f], ROW - 1);
    }
    /* every 16-bit value in place, 16 rows of 4,096: pixel 0 shares its word's bytes with pixel 1 */
    for (uint32_t c = 0; c < 16u; c++) {
        for (uint32_t i = 0; i < ROW; i++) halves[i] = (uint16_t)(c * ROW + i);
        TEST_CHECK_(inplace_matches(VJO_FB_A1B5G5R5, ROW), "16-bit values %u..%u", c * ROW, c * ROW + ROW - 1);
    }
}

TEST_LIST = {
    {"rows_convert_in_place", rows_convert_in_place},
    {"every_rgb_in_a8b8g8r8", every_rgb_in_a8b8g8r8},
    {"every_value_in_a1b5g5r5", every_value_in_a1b5g5r5},
    {"every_channel_in_a2b10g10r10", every_channel_in_a2b10g10r10},
    {"rows_match_the_two_pass_conversion", rows_match_the_two_pass_conversion},
    {NULL, NULL},
};
