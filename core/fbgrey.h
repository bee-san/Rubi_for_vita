/* Game framebuffer pixel formats and the one grey conversion every capture path
 * uses. Header-only, integer-only and free of the SDK, so the kernel (no FPU),
 * the game worker and the host tests share it.
 *
 * Grey is (77 R + 150 G + 29 B) >> 8: the luma vita-vn-ocr's line finder and
 * recognizer compute from RGBA themselves (runtime/vocr_lines.h, vocr.h), so a
 * grey copy changes nothing they see. The 16- and 10-bit formats are expanded to
 * 8 bits per channel first, exactly as the kernel's capture did before API 19
 * (kernel/capture.c convert_row), so their grey is unchanged too; the kernel's
 * one-pass A1B5G5R5 rows (vjo_a1b5g5r5_to_grey) fold that expansion into the
 * weights and give the same bytes. */
#ifndef VJO_FBGREY_H
#define VJO_FBGREY_H

#include <stdint.h>

/* SceDisplayFrameBuf.pixelformat values a game can submit. Only A8B8G8R8 is in
 * the SDK [SDK: Display-Reference]; A2B10G10R10 is in vita-headers
 * (psp2common/display.h); the 16-bit A1B5G5R5 (BGRA5551, R in the low bits) is
 * what vita-udcd-uvc (src/main.c:42) and Vitc read from the PSP emulator. A
 * value outside these is refused with its raw value in the log: no RGB565 value
 * is documented for SceDisplay. */
#define VJO_FB_A8B8G8R8 0x00000000u
#define VJO_FB_A2B10G10R10 0x60800000u
#define VJO_FB_A1B5G5R5 0x50000000u

/* Bytes per pixel of a supported format, 0 for any other. */
static inline unsigned vjo_fb_bpp(uint32_t fmt)
{
    return fmt == VJO_FB_A8B8G8R8 || fmt == VJO_FB_A2B10G10R10 ? 4u : fmt == VJO_FB_A1B5G5R5 ? 2u : 0u;
}

static inline unsigned vjo_grey(unsigned r, unsigned g, unsigned b)
{
    return (77u * r + 150u * g + 29u * b) >> 8;
}

/* The same grey of one A8B8G8R8 word (R in the low byte) with two multiplies
 * and no per-channel extraction. (v & 0x00FF00FF) is R + B * 2^16; times
 * 29 + 77 * 2^16 it is 29 R + (77 R + 29 B) * 2^16 (the 77 B * 2^32 term
 * leaves the 32-bit word). (v & 0xFF00) * 150 * 2^8 is 150 G * 2^16. The sum
 * is 29 R + (77 R + 150 G + 29 B) * 2^16: the bracket is at most 256 * 255 =
 * 65,280 and 29 R at most 7,395, so nothing wraps or carries into bit 16, and
 * bits 24-31 are exactly (77 R + 150 G + 29 B) >> 8 (host/tests/test_fbgrey.c
 * checks every R, G, B). The alpha byte never takes part. */
#define VJO_GREY_RB 0x004D001Du /* 29 + (77 << 16) */
#define VJO_GREY_G8 0x00009600u /* 150 << 8 */
static inline unsigned vjo_grey_abgr(uint32_t v)
{
    return ((v & 0x00FF00FFu) * VJO_GREY_RB + (v & 0x0000FF00u) * VJO_GREY_G8) >> 24;
}

/* The 16-bit format straight to grey, with the kernel's expansion
 * (vjo_fb_to_abgr: each 5-bit channel c5 << 3) folded into the weights: R5 +
 * B5 * 2^16 times 232 + 616 * 2^16 and G5 * 2^5 times 1200 * 2^11 give the same
 * sum at bits 16-31 (at most 2,048 * 31 = 63,488; the low part at most
 * 232 * 31 = 7,192). It is the PSP emulator's format (Adrenaline). */
static inline unsigned vjo_grey_a1b5g5r5(uint32_t v)
{
    return ((((v & 0x1Fu) | ((v & 0x7C00u) << 6)) * 0x026800E8u) + (v & 0x03E0u) * 0x00258000u) >> 24;
}

/* n pixels of src (fmt) as A8B8G8R8 words (R in the low byte). src and dst may
 * not overlap. */
static inline void vjo_fb_to_abgr(uint32_t *dst, const void *src, unsigned n, uint32_t fmt)
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

/* vjo_fb_to_abgr in place (the kernel's one scratch row, kernel/capture.c): buf holds n pixels of fmt from its
 * first byte (n * vjo_fb_bpp(fmt) bytes) and ends as the n A8B8G8R8 words vjo_fb_to_abgr gives. A 16-bit row
 * grows (2 bytes a pixel to 4), so it runs from its last pixel down: pixel i's word, bytes 4i to 4i + 3, lies
 * over 16-bit pixels 2i and 2i + 1, read already (2i >= i). Its 16-bit loads may alias the words it stores
 * (vjo_fb_u16): they must not be moved past a store. A8B8G8R8 is left as it is. */
typedef uint16_t vjo_fb_u16 __attribute__((may_alias));
static inline void vjo_fb_to_abgr_inplace(uint32_t *buf, unsigned n, uint32_t fmt)
{
    unsigned i;
    if (fmt == VJO_FB_A2B10G10R10) {
        for (i = 0; i < n; i++) {
            uint32_t v = buf[i];
            uint32_t r = (v >> 2) & 0xFFu, g = (v >> 12) & 0xFFu, b = (v >> 22) & 0xFFu;
            buf[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    } else if (fmt == VJO_FB_A1B5G5R5) {
        const vjo_fb_u16 *s = (const vjo_fb_u16 *)buf;
        for (i = n; i-- > 0;) {
            uint32_t v = s[i];
            uint32_t r = (v & 0x1Fu) << 3, g = ((v >> 5) & 0x1Fu) << 3, b = ((v >> 10) & 0x1Fu) << 3;
            buf[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

/* n A8B8G8R8 pixels as grey bytes. dst may be src's first byte (in place, as the kernel's rows are): byte i is
 * written after word i is read, and lies in word i / 4, read already. */
static inline void vjo_abgr_to_grey(unsigned char *dst, const uint32_t *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        dst[i] = (unsigned char)vjo_grey_abgr(src[i]);
}

/* n A1B5G5R5 pixels as grey bytes: the same bytes as vjo_fb_to_abgr then
 * vjo_abgr_to_grey, in one pass. dst may be src's first byte (in place: byte i
 * lies in pixel i / 2, read already). */
static inline void vjo_a1b5g5r5_to_grey(unsigned char *dst, const uint16_t *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        dst[i] = (unsigned char)vjo_grey_a1b5g5r5(src[i]);
}

/* FNV-1a, 32 bits, continuing h: the row checksums the worker compares between
 * passes to see whether the screen changed while it was read. */
#define VJO_FNV_INIT 2166136261u
static inline uint32_t vjo_fnv1a(uint32_t h, const unsigned char *p, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

#endif
