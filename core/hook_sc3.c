/* MAGES/5pb. SC3 script text (core/hook_sc3.h, docs/native-hooks.md "SC3 text"). Pure C.
 *
 * Every loop advances by a whole token, at least one byte, and stops at n: no input makes it read past n, write
 * past cap or loop. */
#include "hook_sc3.h"

#include <string.h>

#include "port.h" /* VJO_DATA_TABLE: the pins */

/* The charsets this build accepts (tools/sc3_charset.py writes the pins with the files): in the worker's data
 * segment, as Rubi's verified entries are (core/hook_place.c). */
#include "hook_sc3_pins.inc"
#define PINS_N (sizeof(sc3_pins) / sizeof(sc3_pins[0]))

const VjoSc3Pin *vjo_sc3_pin(const char *title_id)
{
    if (!title_id || title_id[9]) return NULL;
    for (unsigned i = 0; i < PINS_N; i++)
        if (!strncmp(sc3_pins[i].title, title_id, 9)) return &sc3_pins[i];
    return NULL;
}

unsigned vjo_sc3_pins(const VjoSc3Pin **out)
{
    *out = sc3_pins;
    return PINS_N;
}

/* The length of the token at p[i] (i < n), or 0 when n cuts it. */
static size_t token(const uint8_t *p, size_t n, size_t i)
{
    unsigned c = p[i];
    size_t k = i + 1u;
    if (c >= 0x80u && c != 0xFFu) {
        k++; /* a glyph's second byte */
    } else if (c == 0x0Cu || (c >= 0x11u && c <= 0x13u)) {
        k += 2u;
    } else if (c == 0x04u || c == 0x15u) { /* an SC3 expression, through the 0x00 that ends it */
        for (;;) {
            unsigned t;
            if (k >= n) return 0;
            t = p[k];
            if (!t) break;
            k += t & 0x80u ? 2u + ((t & 0x60u) >> 5) : 2u;
        }
        k++;
    }
    return k <= n ? k - i : 0;
}

unsigned vjo_sc3_span(const uint8_t *p, unsigned n)
{
    size_t i = 0, k;
    if (!p) return 0;
    while (i < n) {
        if (p[i] == 0xFFu) return (unsigned)i + 1u;
        if (!(k = token(p, n, i))) return 0;
        i += k;
    }
    return 0;
}

size_t vjo_sc3_decode(const uint8_t *table, unsigned count, const uint8_t *src, size_t n, char *dst, size_t cap)
{
    size_t i = 0, w = 0, k;
    int name = 0, reading = 0;
    if (!dst || !cap) return 0;
    if (!src) n = 0;
    while (i < n && src[i] != 0xFFu && (k = token(src, n, i))) {
        unsigned c = src[i];
        uint32_t cp = 0;
        if (c >= 0x80u) {
            unsigned g = (c & 0x7Fu) << 8 | src[i + 1];
            if (!reading && table && g < count) cp = table[2u * g] | (uint32_t)table[2u * g + 1u] << 8;
            if (cp >= 0xD800u && cp <= 0xDFFFu) cp = 0; /* a surrogate is no character (the tool writes none) */
        } else if (c == 0x00u) {
            cp = '\n';
        } else if (c == 0x01u) { /* the speaker's name, then its own line */
            name = 1;
        } else if (c == 0x02u) {
            if (name) cp = '\n';
            name = 0;
        } else if (c == 0x0Au) { /* ruby: the base is drawn, the reading above it is dropped */
            reading = 1;
        } else if (c == 0x0Bu) {
            reading = 0;
        }
        if (cp) {
            unsigned len = cp < 0x80u ? 1u : cp < 0x800u ? 2u : 3u;
            if (len > cap - 1u - w) break; /* ends at the last whole character */
            if (len == 1u) {
                dst[w++] = (char)cp;
            } else if (len == 2u) {
                dst[w++] = (char)(0xC0u | cp >> 6);
                dst[w++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                dst[w++] = (char)(0xE0u | cp >> 12);
                dst[w++] = (char)(0x80u | (cp >> 6 & 0x3Fu));
                dst[w++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
        i += k;
    }
    dst[w] = 0;
    return w;
}
