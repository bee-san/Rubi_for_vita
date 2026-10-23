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

static const unsigned char *line_at(const VjoCmap *m, unsigned i)
{
    return m->p + VJO_CMAP_HEADER + 8u * i;
}

static const unsigned char *char_at(const VjoCmap *m, unsigned k)
{
    return m->p + VJO_CMAP_HEADER + 8u * m->lines + 6u * k;
}

size_t vjo_cmap_bytes(const VjoCmap *m)
{
    return m->p ? VJO_CMAP_HEADER + 8u * m->lines + 6u * m->chars : 0u;
}

/* The block at p (room bytes there) when it is whole and made for the sentence sent of len bytes. */
static int parse(const unsigned char *p, size_t room, const char *sent, size_t len, VjoCmap *m)
{
    unsigned lines, chars, i, at = 0;
    memset(m, 0, sizeof(*m));
    if (room < VJO_CMAP_HEADER || p[0] != 'C' || p[1] != 'M' || p[2] != VJO_CMAP_VERSION) return 0;
    lines = p[3];
    chars = rd16(p + 4);
    if (!lines || lines > VJO_CMAP_LINES || !chars || chars > VJO_CMAP_CHARS || rd16(p + 6) != len ||
        (rd16(p + 8) | (uint32_t)rd16(p + 10) << 16) != vjo_cmap_fnv(sent, len) || !rd16(p + 12) || !rd16(p + 14) ||
        VJO_CMAP_HEADER + 8u * lines + 6u * chars > room)
        return 0;
    m->p = p;
    m->lines = lines;
    m->chars = chars;
    m->fb_w = rd16(p + 12);
    m->fb_h = rd16(p + 14);
    /* the line table tiles the characters in order, every offset inside the sentence and increasing */
    for (i = 0; i < lines; i++) {
        const unsigned char *l = line_at(m, i);
        if (rd16(l + 4) != at || rd16(l + 6) > chars - at || rd16(l) > rd16(l + 2)) break;
        at += rd16(l + 6);
    }
    if (i == lines && at == chars) {
        uint32_t prev = 0;
        for (i = 0; i < chars; i++) {
            const unsigned char *c = char_at(m, i);
            uint32_t off = rd16(c);
            if (off >= len || (i && off <= prev) || rd16(c + 2) > rd16(c + 4)) break;
            prev = off;
        }
        if (i == chars) return 1;
    }
    memset(m, 0, sizeof(*m));
    return 0;
}

int vjo_cmap_find(const char *buf, size_t cap, size_t len, VjoCmap *m)
{
    memset(m, 0, sizeof(*m));
    if (!buf || len + 1u >= cap) return 0;
    return parse((const unsigned char *)buf + len + 1, cap - len - 1, buf, len, m);
}

size_t vjo_cmap_carry(char *dst, size_t dst_cap, size_t len, const char *src, size_t src_cap)
{
    VjoCmap m;
    size_t tlen, rlen, at, n;
    if (!dst || len + 1u >= dst_cap) return 0;
    n = dst_cap - len - 1 < VJO_CMAP_HEADER ? dst_cap - len - 1 : VJO_CMAP_HEADER;
    memset(dst + len + 1, 0, n); /* no block unless one is copied below */
    if (!src || !src_cap) return 0;
    tlen = blen(src, src_cap);
    if (tlen + 1 >= src_cap) return 0;
    rlen = blen(src + tlen + 1, src_cap - tlen - 1);
    at = tlen + 1 + rlen + 1;
    if (at >= src_cap || !parse((const unsigned char *)src + at, src_cap - at, dst, len, &m)) return 0;
    n = vjo_cmap_bytes(&m);
    if (n > dst_cap - len - 1) return 0;
    memcpy(dst + len + 1, src + at, n);
    return n;
}

void vjo_cmap_line(const VjoCmap *m, unsigned i, unsigned *y0, unsigned *y1)
{
    const unsigned char *l = line_at(m, i);
    *y0 = rd16(l);
    *y1 = rd16(l + 2);
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

int vjo_cmap_sx(const VjoCmap *m, unsigned x)
{
    return (int)(((uint32_t)x * VJO_SCREEN_W + m->fb_w / 2u) / m->fb_w);
}

int vjo_cmap_sy(const VjoCmap *m, unsigned y)
{
    return (int)(((uint32_t)y * VJO_SCREEN_H + m->fb_h / 2u) / m->fb_h);
}

int vjo_cmap_hit(const VjoCmap *m, int x, int y)
{
    for (unsigned k = 0; m->p && k < m->chars; k++) {
        uint32_t off;
        unsigned line, x0, x1, y0, y1;
        vjo_cmap_get(m, k, &off, &line, &x0, &x1);
        vjo_cmap_line(m, line, &y0, &y1);
        if (x1 > x0 && x >= vjo_cmap_sx(m, x0) && x < vjo_cmap_sx(m, x1) && y >= vjo_cmap_sy(m, y0) &&
            y < vjo_cmap_sy(m, y1))
            return (int)k;
    }
    return -1;
}

int vjo_cmap_span(const VjoCmap *m, uint32_t start, uint32_t end, VjoCmapRect *r, int max)
{
    int n = 0;
    unsigned cur = ~0u, lx0 = 0, lx1 = 0;
    for (unsigned k = 0; m->p && k <= m->chars; k++) {
        uint32_t off = 0;
        unsigned line = ~0u, x0 = 0, x1 = 0;
        if (k < m->chars) vjo_cmap_get(m, k, &off, &line, &x0, &x1);
        if (k == m->chars || off >= end || line != cur) { /* close the line's rectangle */
            if (cur != ~0u && n < max) {
                unsigned y0, y1;
                vjo_cmap_line(m, cur, &y0, &y1);
                r[n].x = vjo_cmap_sx(m, lx0);
                r[n].y = vjo_cmap_sy(m, y0);
                r[n].w = vjo_cmap_sx(m, lx1) - r[n].x;
                r[n].h = vjo_cmap_sy(m, y1) - r[n].y;
                if (r[n].w > 0 && r[n].h > 0) n++;
            }
            cur = ~0u;
            if (k == m->chars || off >= end) break;
        }
        if (off < start) continue;
        if (cur == ~0u) {
            cur = line;
            lx0 = x0;
        }
        lx1 = x1;
    }
    return n;
}

int vjo_cmap_vertical(const VjoCmap *m, int k, int dir)
{
    uint32_t off;
    unsigned line, x0, x1, best_line = 0;
    int best = -1, cx, best_d = 0;
    if (!m->p || k < 0 || (unsigned)k >= m->chars || !dir) return -1;
    vjo_cmap_get(m, (unsigned)k, &off, &line, &x0, &x1);
    cx = (int)(x0 + x1);
    for (unsigned j = 0; j < m->chars; j++) {
        unsigned l2, a, b;
        int d;
        vjo_cmap_get(m, j, &off, &l2, &a, &b);
        if (b <= a || (dir < 0 ? l2 >= line : l2 <= line)) continue;
        d = (int)(a + b) - cx;
        if (d < 0) d = -d;
        /* the nearest line first, then the nearest centre */
        if (best < 0 || (dir < 0 ? l2 > best_line : l2 < best_line) || (l2 == best_line && d < best_d)) {
            best = (int)j;
            best_line = l2;
            best_d = d;
        }
    }
    return best;
}

int vjo_cmap_popup_top(const VjoCmapRect *word, int h, int gap)
{
    int below = word->y + word->h + gap, above = word->y - gap - h;
    if (below + h <= VJO_SCREEN_H) return below;
    if (above >= 0) return above;
    /* no room on either side: at the edge farther from the word */
    return word->y + word->h / 2 < VJO_SCREEN_H / 2 ? VJO_SCREEN_H - h : 0;
}

/* ---- writing ---- */

int vjo_cmap_begin(VjoCmapWriter *w, char *buf, size_t cap, size_t at, size_t len, uint32_t fnv,
                   unsigned fb_w, unsigned fb_h, unsigned lines)
{
    size_t room;
    memset(w, 0, sizeof(*w));
    if (!buf || at > cap || !lines || lines > VJO_CMAP_LINES || len > 0xFFFFu || !fb_w || !fb_h ||
        fb_w > 0xFFFFu || fb_h > 0xFFFFu)
        return -1;
    room = cap - at;
    if (room < VJO_CMAP_HEADER + 8u * lines) return -1;
    w->p = (unsigned char *)buf + at;
    w->cap = room;
    w->lines = 0;
    w->max_chars = (unsigned)((room - VJO_CMAP_HEADER - 8u * lines) / 6u);
    if (w->max_chars > VJO_CMAP_CHARS) w->max_chars = VJO_CMAP_CHARS;
    memset(w->p, 0, VJO_CMAP_HEADER + 8u * lines);
    w->p[0] = 'C';
    w->p[1] = 'M';
    w->p[2] = (unsigned char)VJO_CMAP_VERSION;
    w->p[3] = (unsigned char)lines;
    wr16(w->p + 6, (unsigned)len);
    wr16(w->p + 8, fnv & 0xFFFFu);
    wr16(w->p + 10, fnv >> 16);
    wr16(w->p + 12, fb_w);
    wr16(w->p + 14, fb_h);
    return 0;
}

int vjo_cmap_add_line(VjoCmapWriter *w, unsigned y0, unsigned y1)
{
    unsigned char *l;
    if (!w->p || w->lines >= w->p[3] || y0 > y1 || y1 > 0xFFFFu) {
        w->failed = 1;
        return -1;
    }
    l = w->p + VJO_CMAP_HEADER + 8u * w->lines++;
    wr16(l, y0);
    wr16(l + 2, y1);
    wr16(l + 4, w->chars);
    wr16(l + 6, 0);
    return 0;
}

int vjo_cmap_add_char(VjoCmapWriter *w, uint32_t off, unsigned x0, unsigned x1)
{
    unsigned char *c, *l;
    if (!w->p || !w->lines || w->chars >= w->max_chars || off > 0xFFFFu || x0 > x1 || x1 > 0xFFFFu) {
        w->failed = 1;
        return -1;
    }
    c = w->p + VJO_CMAP_HEADER + 8u * w->p[3] + 6u * w->chars++;
    wr16(c, off);
    wr16(c + 2, x0);
    wr16(c + 4, x1);
    l = w->p + VJO_CMAP_HEADER + 8u * (w->lines - 1u);
    wr16(l + 6, rd16(l + 6) + 1u);
    return 0;
}

size_t vjo_cmap_end(VjoCmapWriter *w)
{
    size_t bytes;
    if (!w->p) return 0;
    while (w->lines < w->p[3]) /* lines never added hold no character, after the last one */
        if (vjo_cmap_add_line(w, 0, 0) < 0) break;
    if (!w->chars) {
        memset(w->p, 0, VJO_CMAP_HEADER + 8u * w->p[3]);
        w->p = NULL;
        return 0;
    }
    wr16(w->p + 4, w->chars);
    bytes = VJO_CMAP_HEADER + 8u * w->p[3] + 6u * w->chars;
    return bytes;
}
