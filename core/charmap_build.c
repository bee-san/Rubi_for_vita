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

/* The characters of recognized lines s..s+k-1, one after the other, into t->src (their lines and places in
 * t->line and t->at); their count. */
static unsigned gather(VjoCmapScratch *t, const VjoCmapSource *src, unsigned s, unsigned k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < k; i++)
        for (unsigned j = 0; j < src[s + i].n && j <= 0xFFu && n < VJO_CMAP_SRC_CPS; j++) {
            t->src[n] = src[s + i].cp[j];
            t->line[n] = (uint8_t)i;
            t->at[n++] = (uint8_t)j;
        }
    return n;
}

/* The equal characters of the alignment of t->src's n onto the text line's m code points (into t->map). */
static unsigned aligned_equal(VjoCmapScratch *t, unsigned n, unsigned m)
{
    unsigned eq = 0;
    if (!n || vocr_charbox_align(t->src, n, t->cp, m, t->map) != 0) return 0;
    for (unsigned k = 0; k < m; k++)
        eq += t->map[k] >= 0 && t->src[t->map[k]] == t->cp[k];
    return eq;
}

/* frame px to the space's: the frame fills it as it is shown */
static unsigned scale(unsigned v, unsigned from, unsigned to)
{
    return (unsigned)(((uint32_t)v * to + from / 2u) / from);
}

size_t vjo_cmap_build(char *buf, size_t cap, int filter_lines, const VjoCmapSource *src, unsigned n_src,
                      unsigned fb_w, unsigned fb_h, unsigned w, unsigned h, VjoCmapScratch *t)
{
    unsigned char kept[VJO_CMAP_LINES];
    size_t tlen, rlen, at, lead, slen, joff = 0, content;
    uint32_t fnv;
    unsigned next_src = 0, j = 0;
    VjoCmapWriter wr;
    const char *q;
    if (!buf || !cap || !src || !n_src || !t || !fb_w || !fb_h || fb_w > 0xFFFFu || fb_h > 0xFFFFu) return 0;
    tlen = blen(buf, cap);
    if (!tlen || tlen + 1 >= cap) return 0;
    rlen = blen(buf + tlen + 1, cap - tlen - 1);
    at = tlen + 1 + rlen + 1;
    if (at >= cap) return 0;
    vjo_cmap_sentence(buf, filter_lines, kept, &lead, &slen, &fnv);
    if (!slen || vjo_cmap_begin(&wr, buf, cap, at, slen, fnv, w, h) < 0) return 0;
    for (q = buf; q && j < VJO_CMAP_LINES; j++) {
        const char *s = q;
        unsigned m, best = 0, bs = 0, bk = 0;
        q = next_line(s, filter_lines, &content);
        if (!kept[j]) continue;
        m = decode_line(t, s, content);
        /* the run of recognized lines this line is on: it starts at one of the next four, and is one to
         * VJO_CMAP_SPAN lines long (a hook's line the game wrapped); the most equal characters, the first such
         * (a run on which every character aligns ends the search) */
        for (unsigned s0 = next_src; m && best < m && s0 < n_src && s0 < next_src + 4u; s0++)
            for (unsigned k = 1; best < m && k <= VJO_CMAP_SPAN && s0 + k <= n_src; k++) {
                unsigned eq = aligned_equal(t, gather(t, src, s0, k), m);
                if (eq > best) {
                    best = eq;
                    bs = s0;
                    bk = k;
                }
            }
        if (best && 2u * best >= m) {
            unsigned cur = n_src, prev_b = 0, line = 0; /* cur: the recognized line of the row being written */
            next_src = bs + bk;
            aligned_equal(t, gather(t, src, bs, bk), m); /* t->map for this run */
            for (unsigned k = 0; k < m && !wr.failed; k++) {
                size_t pos = joff + t->off[k];
                int i = t->map[k];
                unsigned a, b;
                if (i >= 0) { /* the cell of the recognized character it aligns to */
                    line = bs + t->line[i];
                    a = scale(src[line].x + src[line].a[t->at[i]], fb_w, w);
                    b = scale(src[line].x + src[line].b[t->at[i]], fb_w, w);
                    if (b < a) b = a;
                } else if (cur < n_src) { /* not read by the recognizer: a zero-width cell after the last one */
                    a = b = prev_b;
                } else { /* ... or, before any, at the next one's left edge */
                    for (unsigned k2 = k + 1; i < 0 && k2 < m; k2++) i = t->map[k2];
                    if (i < 0) break;
                    line = bs + t->line[i];
                    a = b = scale(src[line].x + src[line].a[t->at[i]], fb_w, w);
                }
                prev_b = b;
                if (pos < lead || pos >= lead + slen) continue; /* trimmed */
                if (line != cur) /* a new row: the recognized line this character is on */
                    vjo_cmap_add_line(&wr, scale(src[line].y0, fb_h, h), scale(src[line].y1, fb_h, h));
                cur = line;
                vjo_cmap_add_char(&wr, (uint32_t)(pos - lead), a, b);
            }
        }
        joff += content + 1u; /* and the '\n' before the next kept line */
    }
    return vjo_cmap_end(&wr);
}
