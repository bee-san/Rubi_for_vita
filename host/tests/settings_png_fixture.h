/* Synthetic PNG icons for the Rubi app's host tests (test_settings_layout.c,
 * test_settings_games.c): 8-bit palette or RGBA images with a pattern, and a
 * tEXt chunk naming the game they belong to. The image data is compressed
 * with zlib when the test has it (VJO_TEST_ZLIB) and is otherwise written as
 * stored deflate blocks; rows alternate the Sub and Up filters, as encoders'
 * adaptive filtering does. fx_png_decode turns such a file back into RGBA, as
 * libvita2d's libpng loader does (inflate, unfilter, palette expansion), so
 * the harness's loads cost what a decode costs. Host-only. */
#ifndef VJO_TEST_SETTINGS_PNG_FIXTURE_H
#define VJO_TEST_SETTINGS_PNG_FIXTURE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef VJO_TEST_ZLIB
#include <zlib.h>
#endif

static inline uint32_t fx_crc(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static inline void fx_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static inline uint32_t fx_get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* One chunk at out: length, type, data, CRC; returns its size. */
static inline size_t fx_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t n)
{
    fx_be32(out, n);
    memcpy(out + 4, type, 4);
    if (n) memmove(out + 8, data, n);
    fx_be32(out + 8 + n, fx_crc(out + 4, 4 + (size_t)n));
    return 12 + (size_t)n;
}

/* A zlib stream of raw[0, n) into out (cap bytes); its size, 0 if it does not fit. */
static inline size_t fx_zlib(uint8_t *out, size_t cap, const uint8_t *raw, size_t n)
{
#ifdef VJO_TEST_ZLIB
    uLongf size = (uLongf)cap;
    return compress2(out, &size, raw, (uLong)n, 9) == Z_OK ? (size_t)size : 0;
#else
    size_t at = 2, done = 0;
    uint32_t a = 1, b = 0;
    if (cap < 6) return 0;
    out[0] = 0x78; out[1] = 0x01;
    do {
        size_t len = n - done > 65535 ? 65535 : n - done;
        if (at + 5 + len + 4 > cap) return 0;
        out[at] = done + len == n;
        out[at + 1] = (uint8_t)len; out[at + 2] = (uint8_t)(len >> 8);
        out[at + 3] = (uint8_t)~len; out[at + 4] = (uint8_t)(~len >> 8);
        memcpy(out + at + 5, raw + done, len);
        at += 5 + len;
        done += len;
    } while (done < n);
    for (size_t i = 0; i < n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    fx_be32(out + at, b << 16 | a);
    return at + 4;
#endif
}

/* A w x h icon (palette: 8-bit indexed, else RGBA) whose pattern depends on
 * seed, tagged "Comment" = id when id is not NULL. Its size, 0 if > cap. */
static inline size_t fx_png(uint8_t *out, size_t cap, unsigned w, unsigned h, int palette, unsigned seed, const char *id)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    unsigned bpp = palette ? 1 : 4;
    size_t stride = 1 + (size_t)w * bpp, raw_size = stride * h;
    uint8_t *raw = calloc(raw_size, 1), *row = malloc((size_t)w * bpp), *prev = calloc((size_t)w * bpp, 1);
    uint8_t ihdr[13], plte[768], text[64];
    size_t at = 0, n;
    if (!raw || !row || !prev || cap < 8 + 25 + 12 + 780 + 76 + 12) { free(raw); free(row); free(prev); return 0; }
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++)
            for (unsigned c = 0; c < bpp; c++) {
                uint32_t v = (x * 7u + y * 3u + seed * 13u + c * 41u) ^ ((x * y) >> 4) ^ ((x ^ seed) & 15u);
                row[x * bpp + c] = (uint8_t)(c == 3 ? 255 : v);
            }
        uint8_t *f = raw + y * stride;
        f[0] = y & 1 ? 2 : 1; /* Up, Sub */
        for (size_t i = 0; i < (size_t)w * bpp; i++)
            f[1 + i] = (uint8_t)(row[i] - (f[0] == 1 ? (i >= bpp ? row[i - bpp] : 0) : prev[i]));
        memcpy(prev, row, (size_t)w * bpp);
    }
    memcpy(out, signature, 8);
    at = 8;
    fx_be32(ihdr, w); fx_be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = palette ? 3 : 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    at += fx_chunk(out + at, "IHDR", ihdr, 13);
    if (palette) {
        for (int i = 0; i < 256; i++) {
            plte[i * 3] = (uint8_t)i; plte[i * 3 + 1] = (uint8_t)(255 - i); plte[i * 3 + 2] = (uint8_t)(i * 5 + seed);
        }
        at += fx_chunk(out + at, "PLTE", plte, 768);
    }
    if (id) {
        n = strlen(id) < 40 ? strlen(id) : 40;
        memcpy(text, "Comment", 8);
        memcpy(text + 8, id, n);
        at += fx_chunk(out + at, "tEXt", text, (uint32_t)(8 + n));
    }
    n = fx_zlib(out + at + 8, cap - at - 8 - 12 - 12, raw, raw_size);
    free(raw); free(row); free(prev);
    if (!n) return 0;
    at += fx_chunk(out + at, "IDAT", out + at + 8, (uint32_t)n);
    at += fx_chunk(out + at, "IEND", NULL, 0);
    return at;
}

/* Inflates a zlib stream (zlib's own, or stored blocks only). 0 on success. */
static inline int fx_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t want)
{
#ifdef VJO_TEST_ZLIB
    uLongf size = (uLongf)want;
    return uncompress(out, &size, in, (uLong)n) == Z_OK && size == want ? 0 : -1;
#else
    size_t at = 2, done = 0;
    if (n < 6 || (in[0] & 15) != 8) return -1;
    for (;;) {
        if (at + 5 > n || (in[at] & 6)) return -1; /* stored blocks only */
        size_t len = in[at + 1] | (size_t)in[at + 2] << 8;
        int last = in[at] & 1;
        if (at + 5 + len > n || done + len > want) return -1;
        memcpy(out + done, in + at + 5, len);
        at += 5 + len;
        done += len;
        if (last) return done == want ? 0 : -1;
    }
#endif
}

static inline uint8_t fx_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

/* An 8-bit palette or RGBA PNG of the size checked by the caller, as RGBA
 * into rgba (w * h * 4 bytes, cleared first as libvita2d clears a new
 * texture). Its tEXt "Comment" is copied to id (cap bytes). 0 on success. */
static inline int fx_png_decode(const uint8_t *png, size_t size, uint8_t *rgba, char *id, size_t cap)
{
    unsigned w = 0, h = 0, type = 0, bpp;
    uint8_t plte[768] = {0};
    uint8_t *idat = NULL, *raw = NULL;
    size_t idat_size = 0, at = 8;
    int rc = -1;
    if (id && cap) id[0] = 0;
    while (at + 12 <= size) {
        uint32_t n = fx_get32(png + at);
        const uint8_t *t = png + at + 4, *d = png + at + 8;
        if (n > size - at - 12) break;
        if (!memcmp(t, "IHDR", 4)) { w = fx_get32(d); h = fx_get32(d + 4); type = d[9]; }
        else if (!memcmp(t, "PLTE", 4)) memcpy(plte, d, n < 768 ? n : 768);
        else if (!memcmp(t, "tEXt", 4) && id && n > 8 && !memcmp(d, "Comment", 8)) {
            size_t k = n - 8 < cap - 1 ? n - 8 : cap - 1;
            memcpy(id, d + 8, k);
            id[k] = 0;
        } else if (!memcmp(t, "IDAT", 4)) {
            uint8_t *grown = realloc(idat, idat_size + n + 1);
            if (!grown) goto done;
            idat = grown;
            memcpy(idat + idat_size, d, n);
            idat_size += n;
        } else if (!memcmp(t, "IEND", 4)) break;
        at += 12 + (size_t)n;
    }
    if (!w || !h || (type != 3 && type != 6)) goto done;
    bpp = type == 3 ? 1 : 4;
    size_t stride = 1 + (size_t)w * bpp;
    memset(rgba, 0, (size_t)w * h * 4);
    raw = malloc(stride * h);
    if (!raw || fx_inflate(idat, idat_size, raw, stride * h)) goto done;
    for (unsigned y = 0; y < h; y++) {
        uint8_t *r = raw + y * stride + 1, *up = y ? raw + (y - 1) * stride + 1 : NULL;
        for (size_t i = 0; i < (size_t)w * bpp; i++) {
            int a = i >= bpp ? r[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0;
            switch (r[-1]) {
            case 1: r[i] = (uint8_t)(r[i] + a); break;
            case 2: r[i] = (uint8_t)(r[i] + b); break;
            case 3: r[i] = (uint8_t)(r[i] + ((a + b) >> 1)); break;
            case 4: r[i] = (uint8_t)(r[i] + fx_paeth(a, b, c)); break;
            default: break;
            }
        }
        uint8_t *o = rgba + (size_t)y * w * 4;
        for (unsigned x = 0; x < w; x++)
            if (bpp == 1) {
                const uint8_t *p = plte + r[x] * 3;
                o[x * 4] = p[0]; o[x * 4 + 1] = p[1]; o[x * 4 + 2] = p[2]; o[x * 4 + 3] = 255;
            } else memcpy(o + x * 4, r + x * 4, 4);
    }
    rc = 0;
done:
    free(idat);
    free(raw);
    return rc;
}
#endif
