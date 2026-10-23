/* The game worker's char map writer (core/charmap_build.h). */
#include "charmap_build.h"

#include <string.h>

#include "utf.h"
#include "vocr_charbox.h"

static size_t blen(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static int has_japanese(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n)
        if (vjo_is_japanese_char(vjo_utf8_next(s, n, &i))) return 1;
    return 0;
}

/* Line j of text: its first byte and its content's length (core/textfilter.c: the \r of \r\n is the
 * separator's when lines are filtered; a copy keeps it). Returns the next line's start, or NULL after the last. */
static const char *next_line(const char *p, int filter_lines, size_t *content)
{
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    if (filter_lines && nl && n && p[n - 1] == '\r') n--;
    *content = n;
    return nl ? nl + 1 : NULL;
}

unsigned vjo_cmap_sentence(const char *text, int filter_lines, unsigned char kept[VJO_CMAP_LINES], size_t *lead,
                           size_t *len, uint32_t *fnv)
{
    const char *p = text, *q;
    size_t joined = 0, ld = 0, tr = 0, content;
    unsigned lines = 0, n_kept = 0;
    int leading = 1;
    uint32_t h = 2166136261u;
    memset(kept, 0, VJO_CMAP_LINES);
    /* pass 1: the kept lines, the joined text's length and its leading bytes <= 0x20 (vjo_java_trim) */
    for (q = p; q; lines++) {
        const char *s = q;
        q = next_line(s, filter_lines, &content);
        if (filter_lines && !has_japanese(s, content)) continue;
        if (lines < VJO_CMAP_LINES) kept[lines] = 1;
        if (n_kept++) { /* the '\n' between two kept lines */
            if (leading) ld++;
            joined++;
        }
        for (size_t i = 0; i < content; i++) {
            if (leading && (unsigned char)s[i] <= 0x20) ld++;
            else leading = 0;
        }
        joined += content;
    }
    /* pass 2: the trailing bytes <= 0x20, then the sentence's FNV-1a */
    if (ld < joined) {
        size_t at = 0;
        int nonblank_seen_at = 0;
        size_t last_nonblank = 0;
        unsigned k = 0;
        for (q = p; q; ) {
            const char *s = q;
            q = next_line(s, filter_lines, &content);
            if (filter_lines && !has_japanese(s, content)) continue;
            if (k++) at++; /* the separator: <= 0x20 */
            for (size_t i = 0; i < content; i++, at++)
                if ((unsigned char)s[i] > 0x20) {
                    last_nonblank = at;
                    nonblank_seen_at = 1;
                }
        }
        tr = nonblank_seen_at ? joined - last_nonblank - 1 : 0;
        at = 0;
        k = 0;
        for (q = p; q; ) {
            const char *s = q;
            q = next_line(s, filter_lines, &content);
            if (filter_lines && !has_japanese(s, content)) continue;
            if (k++) {
                if (at >= ld && at < joined - tr) {
                    h ^= (unsigned char)'\n';
                    h *= 16777619u;
                }
                at++;
            }
            for (size_t i = 0; i < content; i++, at++)
                if (at >= ld && at < joined - tr) {
                    h ^= (unsigned char)s[i];
                    h *= 16777619u;
                }
        }
    }
    *lead = ld < joined ? ld : joined;
    *len = ld < joined ? joined - ld - tr : 0;
    *fnv = h;
    return lines;
}

static unsigned decode_line(VjoCmapScratch *t, const char *s, size_t n)
{
    size_t i = 0;
    unsigned m = 0;
    while (i < n && m < VJO_CMAP_LINE_CPS) {
        t->off[m] = (uint16_t)i;
        t->cp[m++] = vjo_utf8_next(s, n, &i);
    }
    return m;
}

/* The equal characters of the alignment of src onto the line's m code points (into t->map). */
static unsigned aligned_equal(VjoCmapScratch *t, const VjoCmapSource *src, unsigned m)
{
    unsigned eq = 0, n = src->n < VJO_CMAP_SRC_CPS ? src->n : VJO_CMAP_SRC_CPS;
    for (unsigned i = 0; i < n; i++) t->src[i] = src->cp[i];
    if (vocr_charbox_align(t->src, n, t->cp, m, t->map) != 0) return 0;
    for (unsigned k = 0; k < m; k++)
        eq += t->map[k] >= 0 && t->src[t->map[k]] == t->cp[k];
    return eq;
}

size_t vjo_cmap_build(char *buf, size_t cap, int filter_lines, const VjoCmapSource *src, unsigned n_src,
                      unsigned fb_w, unsigned fb_h, VjoCmapScratch *t)
{
    unsigned char kept[VJO_CMAP_LINES];
    size_t tlen, rlen, at, lead, slen, joff = 0, content;
    uint32_t fnv;
    unsigned n_kept = 0, next_src = 0, j = 0;
    VjoCmapWriter w;
    const char *q;
    if (!buf || !cap || !src || !n_src || !t) return 0;
    tlen = blen(buf, cap);
    if (!tlen || tlen + 1 >= cap) return 0;
    rlen = blen(buf + tlen + 1, cap - tlen - 1);
    at = tlen + 1 + rlen + 1;
    if (at >= cap) return 0;
    vjo_cmap_sentence(buf, filter_lines, kept, &lead, &slen, &fnv);
    for (unsigned k = 0; k < VJO_CMAP_LINES; k++) n_kept += kept[k];
    if (!slen || !n_kept || vjo_cmap_begin(&w, buf, cap, at, slen, fnv, fb_w, fb_h, n_kept) < 0) return 0;
    for (q = buf; q && j < VJO_CMAP_LINES; j++) {
        const char *s = q;
        unsigned m, match = n_src, prev_b = 0;
        int have_prev = 0;
        q = next_line(s, filter_lines, &content);
        if (!kept[j]) continue;
        m = decode_line(t, s, content);
        /* the next recognized line of at most four whose characters align with at least half of this line's */
        for (unsigned i = next_src; m && i < n_src && i < next_src + 4u; i++)
            if (2u * aligned_equal(t, &src[i], m) >= m) {
                match = i;
                break;
            }
        if (match == n_src) {
            vjo_cmap_add_line(&w, 0, 0);
        } else {
            const VjoCmapSource *L = &src[match];
            next_src = match + 1;
            aligned_equal(t, L, m); /* t->map for this pair */
            vjo_cmap_add_line(&w, L->y0, L->y1);
            for (unsigned k = 0; k < m && !w.failed; k++) {
                size_t pos = joff + t->off[k];
                unsigned a, b;
                if (t->map[k] >= 0) {
                    a = L->x + L->a[t->map[k]];
                    b = L->x + L->b[t->map[k]];
                    if (b < a) b = a;
                } else { /* inserted: a zero-width cell at its neighbour */
                    unsigned nb = prev_b;
                    if (!have_prev)
                        for (unsigned k2 = k + 1; k2 < m; k2++)
                            if (t->map[k2] >= 0) {
                                nb = L->x + L->a[t->map[k2]];
                                break;
                            }
                    a = b = nb;
                }
                prev_b = b;
                have_prev = 1;
                if (pos < lead || pos >= lead + slen) continue; /* trimmed */
                vjo_cmap_add_char(&w, (uint32_t)(pos - lead), a, b);
            }
        }
        joff += content + 1u; /* and the '\n' before the next kept line */
    }
    return vjo_cmap_end(&w);
}
