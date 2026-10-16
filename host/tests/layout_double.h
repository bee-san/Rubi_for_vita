/* A renderer double for the header's layout (host tests only): lays UTF-8
 * text out as the overlay's ScePaf Text does, as far as the cursor keys and
 * taps can tell. Each rendered character (CR has no glyph and no UTF-16
 * unit) gets a start x and a laid-out line; a full-width character advances
 * by `ja`, a narrow one (ASCII, half-width kana) by `narrow`. Lines break
 * after '\n' and wrap at `width` (word wrap of Japanese text: any character
 * may start a line), except that characters that may not start a line under
 * kinsoku (closing brackets, 、。, small kana, ー, !?) hang past the edge, as
 * paf's kinsoku lets them. The query callback is text_hit.h's VjoCharPosition:
 * a query of a low surrogate, of a CR or past the text counts as a bad query.
 *
 * ref_step/ref_vertical restate the cursor keys' rules over the laid-out
 * cells directly (no scan, no early exit), for differential checks. */
#ifndef VJO_LAYOUT_DOUBLE_H
#define VJO_LAYOUT_DOUBLE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "text_rows.h"
#include "utf.h"

#define LAYOUT_MAX 2048

typedef struct {
    const char *s;
    size_t len;
    float width, ja, narrow;
    unsigned n;                      /* rendered characters */
    uint32_t unit[LAYOUT_MAX];       /* each one's UTF-16 index */
    size_t begin[LAYOUT_MAX];        /* its first byte */
    size_t bytes[LAYOUT_MAX];        /* its UTF-8 length */
    uint32_t cp[LAYOUT_MAX];
    float x[LAYOUT_MAX];
    uint32_t line[LAYOUT_MAX];
    uint32_t lines;
    int fail_unit;                   /* this unit's query fails (-1: none) */
    unsigned calls, bad_queries;
} Layout;

static inline int layout_no_line_start(uint32_t cp)
{
    static const uint32_t no_start[] = {0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x300D, 0x300F, 0xFF09, 0x3011,
                                        0xFF01, 0xFF1F, 0x30FC, 0x2026, 0x3063, 0x30C3, 0x3083, 0x3085,
                                        0x3087, 0x30E3, 0x30E5, 0x30E7, 0x3041, 0x3043, 0x3045, 0x3047,
                                        0x3049, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, '!', '?', ')', ',', '.'};
    for (size_t i = 0; i < sizeof(no_start) / sizeof(no_start[0]); i++)
        if (cp == no_start[i])
            return 1;
    return 0;
}

static inline int layout_narrow(uint32_t cp)
{
    return cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F);
}

/* Lays s out (it must outlive the layout); returns the line count. */
static inline uint32_t layout_text(Layout *l, const char *s, float width, float ja, float narrow)
{
    size_t i = 0, len = strlen(s);
    uint32_t unit = 0, line = 0;
    float x = 0;
    memset(l, 0, sizeof(*l));
    l->s = s;
    l->len = len;
    l->width = width;
    l->ja = ja;
    l->narrow = narrow;
    l->fail_unit = -1;
    while (i < len && l->n < LAYOUT_MAX) {
        size_t b = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        float adv = cp == '\n' ? 0 : layout_narrow(cp) ? narrow : ja;
        if (cp == '\r')
            continue;
        if (x > 0 && x + adv > width && !layout_no_line_start(cp)) {
            line++;
            x = 0;
        }
        l->unit[l->n] = unit;
        l->begin[l->n] = b;
        l->bytes[l->n] = i - b;
        l->cp[l->n] = cp;
        l->x[l->n] = x;
        l->line[l->n] = line;
        l->n++;
        unit += (uint32_t)vjo_utf16_units(cp);
        x += adv;
        if (cp == '\n') {
            line++;
            x = 0;
        }
    }
    l->lines = line + 1;
    return l->lines;
}

static inline int layout_position(void *ctx, uint32_t index16, float *x, uint32_t *line)
{
    Layout *l = (Layout *)ctx;
    l->calls++;
    if ((int)index16 == l->fail_unit)
        return -1;
    for (unsigned k = 0; k < l->n; k++)
        if (l->unit[k] == index16) {
            *x = l->x[k];
            *line = l->line[k];
            return 0;
        }
    l->bad_queries++; /* a low surrogate, a CR or past the text */
    return -1;
}

static inline VjoTextRows layout_rows(Layout *l)
{
    VjoTextRows r;
    r.position = layout_position;
    r.ctx = l;
    r.lines = l->lines;
    r.terminal_width = l->ja;
    return r;
}

/* The rendered character holding byte b, or -1. */
static inline int layout_at(const Layout *l, size_t b)
{
    for (unsigned k = 0; k < l->n; k++)
        if (b >= l->begin[k] && b < l->begin[k] + l->bytes[k])
            return (int)k;
    for (unsigned k = 0; k < l->n; k++) /* a CR: the character after it */
        if (l->begin[k] > b)
            return (int)k;
    return -1;
}

static inline int layout_selectable(const Layout *l, unsigned k, int japanese_only)
{
    return vjo_text_selectable(l->cp[k], l->bytes[k], japanese_only);
}

static inline float layout_end(const Layout *l, unsigned k)
{
    return k + 1 < l->n && l->line[k + 1] == l->line[k] && l->x[k + 1] >= l->x[k] ? l->x[k + 1] : l->x[k] + l->ja;
}

/* Left/Right on the cursor's row (vjo_text_row_step's rule); returns the
 * new cursor, or `cursor` to stay; *wrapped tells a wrap. */
static inline size_t ref_step(const Layout *l, size_t cursor, size_t from, int direction, int japanese_only, int *wrapped)
{
    int c = layout_at(l, cursor), first = -1, last = -1, before = -1, before_row = -1, next = -1;
    uint32_t row = l->line[c];
    *wrapped = 0;
    for (unsigned k = 0; k < l->n; k++) {
        if (!layout_selectable(l, k, japanese_only))
            continue;
        if (l->line[k] == row) {
            if (first < 0)
                first = (int)k;
            last = (int)k;
        }
        if (l->begin[k] < cursor) {
            before = (int)k;
            if (l->line[k] == row)
                before_row = (int)k;
        } else if (l->begin[k] >= from && next < 0)
            next = (int)k;
    }
    int to;
    if (direction > 0) {
        to = next >= 0 && l->line[next] == row ? next : first >= 0 ? first : next;
        *wrapped = !(next >= 0 && l->line[next] == row) && first >= 0;
    } else {
        to = before_row >= 0 ? before_row : last >= 0 ? last : before;
        *wrapped = before_row < 0 && last >= 0;
    }
    if (to < 0 || l->begin[to] == cursor) {
        *wrapped = 0;
        return cursor;
    }
    return l->begin[to];
}

/* Left/Right in reading order across the rows (the owner's 2026-10-16 rule,
 * test.3's item G): direction > 0 the first selectable character at or after
 * `from`, direction < 0 the last one before the cursor, whatever row it is on.
 * Past the text's end that way the cursor stays, unless `wrap` is set and every
 * selectable character is on one row: then that row's first (Right) or last
 * (Left), *wrapped set. Returns the new cursor, or `cursor` to stay. */
static inline size_t ref_read_step(const Layout *l, size_t cursor, size_t from, int direction, int japanese_only,
                                   int wrap, int *wrapped)
{
    int first = -1, last = -1, before = -1, next = -1, to;
    *wrapped = 0;
    for (unsigned k = 0; k < l->n; k++) {
        if (!layout_selectable(l, k, japanese_only))
            continue;
        if (first < 0)
            first = (int)k;
        last = (int)k;
        if (l->begin[k] < cursor)
            before = (int)k;
        else if (l->begin[k] >= from && next < 0)
            next = (int)k;
    }
    to = direction > 0 ? next : before;
    if (to < 0 && wrap && first >= 0 && l->line[first] == l->line[last]) {
        to = direction > 0 ? first : last;
        *wrapped = 1;
    }
    if (to < 0 || l->begin[to] == cursor) {
        *wrapped = 0;
        return cursor;
    }
    return l->begin[to];
}

/* Up/Down (vjo_text_row_vertical's rule); goal < 0: the cursor cell's
 * centre. Returns the new cursor or `cursor` to stay. */
static inline size_t ref_vertical(const Layout *l, size_t cursor, int direction, int japanese_only, float goal)
{
    int c = layout_at(l, cursor), best = -1;
    long target = -1;
    uint32_t row = l->line[c];
    float bd = 0;
    if (goal < 0)
        goal = (l->x[c] + layout_end(l, (unsigned)c)) / 2;
    for (unsigned k = 0; k < l->n; k++)
        if (layout_selectable(l, k, japanese_only) && (direction < 0 ? l->line[k] < row : l->line[k] > row) &&
            (target < 0 || (direction < 0 ? (long)l->line[k] > target : (long)l->line[k] < target)))
            target = (long)l->line[k];
    if (target < 0)
        return cursor;
    for (unsigned k = 0; k < l->n; k++) {
        if (!layout_selectable(l, k, japanese_only) || (long)l->line[k] != target)
            continue;
        float x = l->x[k], e = layout_end(l, k);
        float d = goal < x ? x - goal : goal > e ? goal - e : 0;
        if (best < 0 || d < bd) {
            best = (int)k;
            bd = d;
        }
    }
    return l->begin[best];
}
#endif
