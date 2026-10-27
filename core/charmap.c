/* The char map: the places of a recognized text's characters on the screen (core/charmap.h). */
#include "charmap.h"

#include <string.h>

/* strnlen, which C99 and SceShell's libc lack */
static size_t blen(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void wr16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)(v >> 8 & 0xFFu);
}

uint32_t vjo_cmap_fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static const unsigned char *char_at(const VjoCmap *m, unsigned k)
{
    return m->p + VJO_CMAP_HEADER + 6u * k;
}

static const unsigned char *line_at(const VjoCmap *m, unsigned i)
{
    return m->p + VJO_CMAP_HEADER + 6u * m->chars + 8u * i;
}

size_t vjo_cmap_bytes(const VjoCmap *m)
{
    return m->p ? VJO_CMAP_HEADER + 6u * m->chars + 8u * m->lines : 0u;
}

/* The block at p (room bytes there) when it is whole and made for the sentence sent of len bytes: its lines tile
 * its characters in order, and every offset is inside the sentence and larger than the one before. */
static int parse(const unsigned char *p, size_t room, const char *sent, size_t len, VjoCmap *m)
{
    unsigned lines, chars, i, at = 0;
    uint32_t prev = 0;
    m->p = NULL;
    m->lines = m->chars = 0;
    if (room < VJO_CMAP_HEADER || p[0] != 'C' || p[1] != 'M' || p[2] != VJO_CMAP_VERSION) return 0;
    lines = p[3];
    chars = rd16(p + 4);
    if (!lines || lines > VJO_CMAP_LINES || !chars || chars > VJO_CMAP_CHARS || rd16(p + 6) != len ||
        (rd16(p + 8) | (uint32_t)rd16(p + 10) << 16) != vjo_cmap_fnv(sent, len) ||
        VJO_CMAP_HEADER + 6u * chars + 8u * lines > room)
        return 0;
    m->p = p;
    m->lines = lines;
    m->chars = chars;
    for (i = 0; i < lines; i++) {
        const unsigned char *l = line_at(m, i);
        if (rd16(l + 4) != at || !rd16(l + 6) || rd16(l + 6) > chars - at || rd16(l) > rd16(l + 2)) break;
        at += rd16(l + 6);
    }
    for (unsigned k = 0; i == lines && at == chars && k < chars; k++) {
        const unsigned char *c = char_at(m, k);
        uint32_t off = rd16(c);
        if (off >= len || (k && off <= prev) || rd16(c + 2) > rd16(c + 4)) break;
        prev = off;
        if (k + 1 == chars) return 1;
    }
    m->p = NULL;
    m->lines = m->chars = 0;
    return 0;
}

int vjo_cmap_find(const char *buf, size_t cap, size_t len, VjoCmap *m)
{
    m->p = NULL;
    m->lines = m->chars = 0;
    if (!buf || len + 1u >= cap) return 0;
    return parse((const unsigned char *)buf + len + 1, cap - len - 1, buf, len, m);
}

size_t vjo_cmap_carry(char *dst, size_t dst_cap, size_t len, const char *src, size_t src_cap)
{
    VjoCmap m;
    size_t tlen, at, n;
    if (!dst || len + 1u >= dst_cap) return 0;
    n = dst_cap - len - 1 < VJO_CMAP_HEADER ? dst_cap - len - 1 : VJO_CMAP_HEADER;
    memset(dst + len + 1, 0, n); /* no block unless one is copied below */
    if (!src || !src_cap) return 0;
    tlen = blen(src, src_cap);
    if (tlen + 1 >= src_cap) return 0;
    at = tlen + 1 + blen(src + tlen + 1, src_cap - tlen - 1) + 1; /* after the text's and its copy's NULs */
    if (at >= src_cap || !parse((const unsigned char *)src + at, src_cap - at, dst, len, &m)) return 0;
    n = vjo_cmap_bytes(&m);
    if (n > dst_cap - len - 1) return 0;
    memcpy(dst + len + 1, src + at, n);
    return n;
}

void vjo_cmap_line(const VjoCmap *m, unsigned i, unsigned *y0, unsigned *y1, unsigned *first, unsigned *count)
{
    const unsigned char *l = line_at(m, i);
    *y0 = rd16(l);
    *y1 = rd16(l + 2);
    *first = rd16(l + 4);
    *count = rd16(l + 6);
}

void vjo_cmap_get(const VjoCmap *m, unsigned k, uint32_t *off, unsigned *line, unsigned *x0, unsigned *x1)
{
    const unsigned char *c = char_at(m, k);
    unsigned i = 0;
    while (i + 1 < m->lines && k >= rd16(line_at(m, i) + 4) + rd16(line_at(m, i) + 6)) i++;
    *off = rd16(c);
    *line = i;
    *x0 = rd16(c + 2);
    *x1 = rd16(c + 4);
}

int vjo_cmap_index(const VjoCmap *m, uint32_t off)
{
    unsigned lo = 0, hi = m->p ? m->chars : 0u;
    while (lo < hi) { /* the offsets increase */
        unsigned mid = (lo + hi) / 2u;
        uint32_t v = rd16(char_at(m, mid));
        if (v == off) return (int)mid;
        if (v < off) lo = mid + 1u;
        else hi = mid;
    }
    return -1;
}

/* How far v is outside [a, b): 0 inside. */
static int outside(int v, int a, int b)
{
    return v < a ? a - v : v >= b ? v - b + 1 : 0;
}

int vjo_cmap_hit(const VjoCmap *m, int x, int y)
{
    int best = -1, best_d = VJO_CMAP_SLACK + 1;
    for (unsigned i = 0; i < m->lines; i++) {
        unsigned y0, y1, first, count;
        vjo_cmap_line(m, i, &y0, &y1, &first, &count);
        int dy = outside(y, (int)y0, (int)y1);
        for (unsigned k = first; dy < best_d && k < first + count; k++) {
            const unsigned char *c = char_at(m, k);
            int d = dy + outside(x, (int)rd16(c + 2), (int)rd16(c + 4));
            if (rd16(c + 4) > rd16(c + 2) && d < best_d) {
                best_d = d;
                best = (int)k;
            }
        }
    }
    return best;
}

int vjo_cmap_span(const VjoCmap *m, uint32_t start, uint32_t end, VjoCmapRect *r, int max)
{
    int n = 0;
    for (unsigned i = 0; i < m->lines && n < max; i++) {
        unsigned y0, y1, first, count, x0 = 0, x1 = 0, any = 0;
        vjo_cmap_line(m, i, &y0, &y1, &first, &count);
        for (unsigned k = first; k < first + count; k++) {
            const unsigned char *c = char_at(m, k);
            uint32_t off = rd16(c);
            if (off < start || off >= end) continue;
            if (!any++) x0 = rd16(c + 2);
            x1 = rd16(c + 4); /* the cells follow each other along the line */
        }
        if (any && x1 > x0 && y1 > y0) {
            r[n].x = (int)x0;
            r[n].y = (int)y0;
            r[n].w = (int)(x1 - x0);
            r[n].h = (int)(y1 - y0);
            n++;
        }
    }
    return n;
}

int vjo_cmap_vertical(const VjoCmap *m, int k, int dir)
{
    uint32_t off;
    unsigned line, x0, x1;
    if (!m->p || k < 0 || (unsigned)k >= m->chars || !dir) return -1;
    vjo_cmap_get(m, (unsigned)k, &off, &line, &x0, &x1);
    int cx = (int)(x0 + x1);
    /* the nearest line that way with a character that has a cell, then its character nearest in x */
    for (int i = (int)line + dir; i >= 0 && i < (int)m->lines; i += dir) {
        unsigned y0, y1, first, count;
        int best = -1, best_d = 0;
        vjo_cmap_line(m, (unsigned)i, &y0, &y1, &first, &count);
        for (unsigned j = first; j < first + count; j++) {
            const unsigned char *c = char_at(m, j);
            int d = (int)(rd16(c + 2) + rd16(c + 4)) - cx;
            if (d < 0) d = -d;
            if (rd16(c + 4) > rd16(c + 2) && (best < 0 || d < best_d)) {
                best = (int)j;
                best_d = d;
            }
        }
        if (best >= 0) return best;
    }
    return -1;
}

int vjo_cmap_popup_top(const VjoCmap *m, const VjoCmapRect *word, int h, int gap)
{
    int below = word->y + word->h + gap, above = word->y - gap - h, top = VJO_SCREEN_H, bottom = 0;
    for (unsigned i = 0; m->p && i < m->lines; i++) { /* the game's text: from its first line's top to its last's foot */
        const unsigned char *l = line_at(m, i);
        if ((int)rd16(l) < top) top = (int)rd16(l);
        if ((int)rd16(l + 2) > bottom) bottom = (int)rd16(l + 2);
    }
    if (top < bottom && bottom + gap + h <= VJO_SCREEN_H) return bottom + gap;
    if (top < bottom && top - gap - h >= 0) return top - gap - h;
    if (below + h <= VJO_SCREEN_H) return below;
    if (above >= 0) return above;
    /* no room on either side: at the edge farther from the word */
    return word->y + word->h / 2 < VJO_SCREEN_H / 2 ? VJO_SCREEN_H - h : 0;
}

/* ---- writing ---- */

int vjo_cmap_begin(VjoCmapWriter *wr, char *buf, size_t cap, size_t at, size_t len, uint32_t fnv, unsigned w,
                   unsigned h)
{
    size_t room;
    memset(wr, 0, sizeof(*wr));
    if (!buf || at > cap || len > 0xFFFFu || !w || !h || w > 0xFFFFu || h > 0xFFFFu) return -1;
    room = cap - at;
    if (room < VJO_CMAP_HEADER + 6u + 8u) return -1;
    wr->p = (unsigned char *)buf + at;
    wr->cap = room;
    wr->max_chars = (unsigned)((room - VJO_CMAP_HEADER - 8u * VJO_CMAP_LINES) / 6u);
    if (room < VJO_CMAP_HEADER + 8u * VJO_CMAP_LINES + 6u) /* room for a few lines only: as many characters */
        wr->max_chars = (unsigned)((room - VJO_CMAP_HEADER - 8u) / 6u);
    if (wr->max_chars > VJO_CMAP_CHARS) wr->max_chars = VJO_CMAP_CHARS;
    memset(wr->p, 0, VJO_CMAP_HEADER);
    wr->p[0] = 'C';
    wr->p[1] = 'M';
    wr->p[2] = (unsigned char)VJO_CMAP_VERSION;
    wr16(wr->p + 6, (unsigned)len);
    wr16(wr->p + 8, fnv & 0xFFFFu);
    wr16(wr->p + 10, fnv >> 16);
    wr16(wr->p + 12, w);
    wr16(wr->p + 14, h);
    return 0;
}

int vjo_cmap_add_line(VjoCmapWriter *wr, unsigned y0, unsigned y1)
{
    if (wr->lines && !wr->line[wr->lines - 1u][3]) wr->lines--; /* the last line got no character: reuse it */
    if (!wr->p || wr->lines >= VJO_CMAP_LINES || y0 > y1 || y1 > 0xFFFFu) {
        wr->failed = 1;
        return -1;
    }
    wr->line[wr->lines][0] = (uint16_t)y0;
    wr->line[wr->lines][1] = (uint16_t)y1;
    wr->line[wr->lines][2] = (uint16_t)wr->chars;
    wr->line[wr->lines++][3] = 0;
    return 0;
}

int vjo_cmap_add_char(VjoCmapWriter *wr, uint32_t off, unsigned x0, unsigned x1)
{
    unsigned char *c;
    if (!wr->p || !wr->lines || wr->chars >= wr->max_chars || off > 0xFFFFu || x0 > x1 || x1 > 0xFFFFu) {
        wr->failed = 1;
        return -1;
    }
    c = wr->p + VJO_CMAP_HEADER + 6u * wr->chars++;
    wr16(c, off);
    wr16(c + 2, x0);
    wr16(c + 4, x1);
    wr->line[wr->lines - 1u][3]++;
    return 0;
}

size_t vjo_cmap_end(VjoCmapWriter *wr)
{
    size_t bytes;
    unsigned char *l;
    if (!wr->p) return 0;
    if (wr->lines && !wr->line[wr->lines - 1u][3]) wr->lines--;
    bytes = VJO_CMAP_HEADER + 6u * wr->chars + 8u * wr->lines;
    if (!wr->chars || bytes > wr->cap) {
        memset(wr->p, 0, VJO_CMAP_HEADER);
        wr->p = NULL;
        return 0;
    }
    l = wr->p + VJO_CMAP_HEADER + 6u * wr->chars;
    for (unsigned i = 0; i < wr->lines; i++)
        for (unsigned j = 0; j < 4; j++) wr16(l + 8u * i + 2u * j, wr->line[i][j]);
    wr->p[3] = (unsigned char)wr->lines;
    wr16(wr->p + 4, wr->chars);
    return bytes;
}
