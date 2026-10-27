#include "region_suggest.h"

#include <string.h>

#include "vocr_ocr.h" /* vjo_vocr_suggest: a job's recognized lines */

uint16_t vjo_suggest_kana(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t n = 0;
    if (!p)
        return 0;
    while (*p) {
        unsigned c = p[0], len = c < 0x80u ? 1u : (c & 0xE0u) == 0xC0u ? 2u : (c & 0xF0u) == 0xE0u ? 3u
                                              : (c & 0xF8u) == 0xF0u ? 4u : 1u;
        unsigned k = 1;
        while (k < len && (p[k] & 0xC0u) == 0x80u)
            k++;
        if (k < len) { /* a sequence cut short: its lead byte alone, as an invalid one */
            p++;
            continue;
        }
        if (len == 3u) {
            uint32_t cp = (c & 0x0Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu);
            n += (cp >= 0x3040u && cp <= 0x30FFu) || (cp >= 0x4E00u && cp <= 0x9FAFu);
        }
        p += len;
    }
    return (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
}

/* b, the line below a, joins a's group: the gap between them is at most the taller one's height, their
 * heights are within twice each other, and they overlap across or start within align px of each other. */
static int joins(const VjoSuggestLine *a, const VjoSuggestLine *b, int align)
{
    int gap = (int)b->y - (int)a->y - (int)a->h;
    int tall = a->h > b->h ? a->h : b->h;
    int dx = (int)b->x - (int)a->x;
    int right = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    int left = a->x > b->x ? a->x : b->x;
    if (gap > tall || 2 * b->h < a->h || 2 * a->h < b->h)
        return 0;
    return (dx <= align && dx >= -align) || right > left;
}

/* A pixel coordinate of a frame `size` px across in region.ini's 0..65535: the origin rounded up and a size
 * down, so the kernel's floor and ceil (kernel/capture.c) give back these pixels (core/region_select.c). */
static uint16_t norm(uint32_t v, uint32_t size, int up)
{
    uint32_t n = (v * 65536u + (up ? size - 1u : 0u)) / size; /* v <= size <= 65535: no overflow */
    return (uint16_t)(n > 65535u ? 65535u : n);
}

int vjo_region_suggest(const VjoSuggestLine *line, unsigned n, unsigned fb_w, unsigned fb_h, VjoRect *out)
{
    VjoSuggestLine c[VJO_SUGGEST_LINES];
    uint16_t h[VJO_SUGGEST_LINES];
    unsigned m = 0, start = 0, kana = 0, best = 0, best_n = 0, best_kana = 0, med, margin, top, min_h;
    memset(out, 0, sizeof(*out));
    if (!line || !fb_w || !fb_h || fb_w > 65535u || fb_h > 65535u)
        return 0;
    if (n > VJO_SUGGEST_LINES)
        n = VJO_SUGGEST_LINES;
    /* the lines that count, top to bottom (a tie in the order given) */
    for (unsigned i = 0; i < n; i++) {
        const VjoSuggestLine *l = &line[i];
        unsigned j;
        if (l->kana < VJO_SUGGEST_LINE_KANA || !l->w || (uint32_t)l->x + l->w > fb_w ||
            (uint32_t)l->y + l->h > fb_h || (uint32_t)l->h * 136u < 3u * fb_h || /* 12 px of 544 */
            (uint32_t)l->h * 272u > 45u * fb_h)                                  /* 90 px of 544 */
            continue;
        for (j = m; j && c[j - 1].y > l->y; j--)
            c[j] = c[j - 1];
        c[j] = *l;
        m++;
    }
    /* their groups; the one with the most kana and kanji */
    for (unsigned i = 0; i <= m; i++) {
        if (i == m || (i && !joins(&c[i - 1], &c[i], (int)(fb_w / 24u)))) { /* 40 px of 960 */
            if (kana > best_kana) {
                best = start;
                best_n = i - start;
                best_kana = kana;
            }
            start = i;
            kana = 0;
        }
        if (i < m)
            kana += c[i].kana;
    }
    if (best_kana < VJO_SUGGEST_BOX_KANA)
        return 0;
    /* half its lines' median height (the lower middle one of an even count) above its first line */
    for (unsigned i = 0; i < best_n; i++) {
        unsigned j;
        for (j = i; j && h[j - 1] > c[best + i].h; j--)
            h[j] = h[j - 1];
        h[j] = c[best + i].h;
    }
    med = h[(best_n - 1u) / 2u];
    margin = (med + 1u) / 2u;
    top = c[best].y > margin ? c[best].y - margin : 0u;
    /* at least the region selector's 32 of 544 px tall (core/region_select.h VJO_RSEL_MIN) */
    min_h = (32u * fb_h + 543u) / 544u;
    if (fb_h - top < min_h)
        top = fb_h > min_h ? fb_h - min_h : 0u;
    out->x = 0;
    out->w = 65535u;
    out->y = norm(top, fb_h, 1);
    out->h = norm(fb_h - top, fb_h, 0);
    return 1;
}

void vjo_region_screen(const VjoRect *r, uint16_t box[4])
{
    if (!r->w || !r->h) {
        box[0] = box[1] = box[2] = box[3] = 0;
        return;
    }
    box[0] = (uint16_t)((uint32_t)r->x * 960u >> 16);
    box[1] = (uint16_t)((uint32_t)r->y * 544u >> 16);
    box[2] = (uint16_t)((((uint32_t)r->x + r->w) * 960u + 65535u) >> 16);
    box[3] = (uint16_t)((((uint32_t)r->y + r->h) * 544u + 65535u) >> 16);
}

int vjo_vocr_suggest(const VjoVocrStats *st, unsigned fb_w, unsigned fb_h, uint16_t box[4])
{
    VjoSuggestLine l[VJO_SUGGEST_LINES];
    VjoRect r;
    unsigned n = st->recognized < VJO_SUGGEST_LINES ? st->recognized : VJO_SUGGEST_LINES;
    int ok = 0;
    memset(&r, 0, sizeof(r));
    if (!st->columns) {
        for (unsigned i = 0; i < n; i++) {
            const VjoVocrLineStat *ls = &st->line[i];
            l[i] = (VjoSuggestLine){ls->x, ls->y, ls->width, ls->height, ls->kana};
        }
        ok = vjo_region_suggest(l, n, fb_w, fb_h, &r);
    }
    vjo_region_screen(&r, box);
    return ok;
}
