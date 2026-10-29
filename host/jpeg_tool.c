/* vjo-jpeg: the game worker's JPEG encoder (core/jpegsw.c) on the host, for its
 * tests (host/tests/test_jpegsw.py, which decodes the output with Pillow) and to
 * measure the Anki screenshot's sizes on real frames.
 *
 *   vjo-jpeg IN.rgba WIDTH HEIGHT QUALITY OUT.jpg [ARENA_BYTES]
 *
 * IN.rgba is WIDTH x HEIGHT pixels of R, G, B, A bytes. ARENA_BYTES bounds the
 * encoder's memory (its state, a band and the JPEG), as the game worker's block
 * does: past it the encode fails (exit status 3), as a JPEG over its cap does. */
#include <stdio.h>
#include <stdlib.h>

#include "jpegsw.h"

typedef struct {
    const uint8_t *px;
    uint32_t w, h;
} Image;

static int rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    const Image *im = (const Image *)ud;
    for (uint32_t r = 0; r < n; r++)
        for (uint32_t i = 0; i < im->w * 4u; i++)
            dst[r * im->w * 4u + i] = im->px[((size_t)(row + r) * im->w * 4u) + i];
    return (int)n;
}

/* The buffers go back on every way out: LeakSanitizer reads a process's end (#528's ASan run of jpeg_encoder). */
static int done(int rc, uint8_t *px, uint8_t *mem)
{
    free(px);
    free(mem);
    return rc;
}

int main(int argc, char **argv)
{
    Image im;
    VjoArena a;
    VjoBuf out = {0};
    uint8_t *px = NULL, *mem = NULL;
    size_t bytes, cap;
    FILE *f;
    int quality;
    if (argc < 6)
        return 2;
    im.w = (uint32_t)strtoul(argv[2], NULL, 10);
    im.h = (uint32_t)strtoul(argv[3], NULL, 10);
    quality = atoi(argv[4]);
    bytes = (size_t)im.w * im.h * 4u;
    cap = argc > 6 ? strtoul(argv[6], NULL, 10) : 16u * 1024u * 1024u;
    if (!(px = (uint8_t *)malloc(bytes ? bytes : 1)) || !(mem = (uint8_t *)malloc(cap)) || !(f = fopen(argv[1], "rb")))
        return done(2, px, mem);
    if (fread(px, 1, bytes, f) != bytes) {
        fclose(f);
        return done(2, px, mem);
    }
    fclose(f);
    im.px = px;
    vjo_arena_init(&a, mem, cap);
    if (vjo_jpeg_encode(&a, im.w, im.h, im.w * 4u, rows, &im, quality, &out) < 0) {
        fprintf(stderr, "vjo-jpeg: encode failed (%s)\n", out.oom ? "out of memory" : "bad size");
        return done(3, px, mem);
    }
    if (!(f = fopen(argv[5], "wb")))
        return done(2, px, mem);
    if (fwrite(out.data, 1, out.len, f) != out.len) {
        fclose(f);
        return done(2, px, mem);
    }
    if (fclose(f))
        return done(2, px, mem);
    printf("%u %u\n", (unsigned)out.len, (unsigned)a.peak);
    return done(0, px, mem);
}
