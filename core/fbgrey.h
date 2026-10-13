/* Game framebuffer pixel formats and the one grey conversion every capture path
 * uses. Header-only, integer-only and free of the SDK, so the kernel (no FPU),
 * the game worker and the host tests share it.
 *
 * Grey is (77 R + 150 G + 29 B) >> 8: the luma vita-vn-ocr's line finder and
 * recognizer compute from RGBA themselves (runtime/vocr_lines.h, vocr.h), so a
 * grey copy changes nothing they see. The 16- and 10-bit formats are expanded to
 * 8 bits per channel first, exactly as the kernel's capture did before API 19
 * (kernel/capture.c convert_row), so their grey is unchanged too. */
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

/* n A8B8G8R8 pixels as grey bytes. */
static inline void vjo_abgr_to_grey(unsigned char *dst, const uint32_t *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        uint32_t v = src[i];
        dst[i] = (unsigned char)vjo_grey(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu);
    }
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
