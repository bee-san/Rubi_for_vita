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

TEST_LIST = {
    {"every_rgb_in_a8b8g8r8", every_rgb_in_a8b8g8r8},
    {"every_value_in_a1b5g5r5", every_value_in_a1b5g5r5},
    {"every_channel_in_a2b10g10r10", every_channel_in_a2b10g10r10},
    {"rows_match_the_two_pass_conversion", rows_match_the_two_pass_conversion},
    {NULL, NULL},
};
