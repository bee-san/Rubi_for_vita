#include "text_rows.h"

#include <string.h>

#include "utf.h"

typedef struct {
    const char *s;
    size_t len, i, begin;      /* the next byte; the current character's first byte */
    uint32_t cp, unit, units;  /* its code point and UTF-16 index; units so far */
} Walk;

/* The next rendered character. CR is not rendered and has no unit (styled.c). */
static int walk(Walk *w)
{
    while (w->i < w->len) {
        w->begin = w->i;
        w->cp = vjo_utf8_next(w->s, w->len, &w->i);
        if (w->cp == '\r')
            continue;
        w->unit = w->units;
        w->units += (uint32_t)vjo_utf16_units(w->cp);
        return 1;
    }
    return 0;
}

/* A position the layout reported, on one of its lines, with a finite x. */
static int query(const VjoTextRows *r, uint32_t unit, float *x, uint32_t *line)
{
    return r->position(r->ctx, unit, x, line) >= 0 && *line < r->lines && *x - *x == 0.0f;
}

static int rows_ok(const char *s, const VjoTextRows *r)
{
    return s && r && r->position && r->lines && r->terminal_width > 0.0f;
}

int vjo_text_selectable(uint32_t cp, size_t bytes, int japanese_only)
{
    /* Japanese letters are all at U+3005 or above: canonical UTF-8 is 3 or
     * 4 bytes (the decoder also accepts overlong forms; never select those). */
    if (japanese_only)
        return vjo_is_japanese_letter(cp) && bytes == (cp < 0x10000 ? 3u : 4u);
    return cp > 0x20 && (cp < 0x7F || cp > 0x9F) && cp != 0x3000;
}

/* Where the cell just walked (x, line) ends: the next character's start on
 * the same line, else x + terminal_width (a row's last cell, or a next
 * character whose position is unknown). The walk itself is not advanced. */
static float cell_end(const Walk *at, const VjoTextRows *r, float x, uint32_t line)
{
    Walk w = *at;
    float nx;
    uint32_t nl;
    return walk(&w) && query(r, w.unit, &nx, &nl) && nl == line && nx >= x ? nx : x + r->terminal_width;
}

/* The cell holding byte `at`: its start x, line and end. */
static int cell(const char *s, size_t len, const VjoTextRows *r, size_t at, float *x, float *end, uint32_t *line)
{
    Walk w = {s, len, 0, 0, 0, 0, 0};
    while (walk(&w))
        if (at < w.i) {
            if (!query(r, w.unit, x, line))
                return 0;
            *end = cell_end(&w, r, *x, *line);
            return 1;
        }
    return 0;
}

int vjo_text_row_step(const char *s, size_t len, const VjoTextRows *r, size_t cursor,
                      size_t from, int direction, int japanese_only, size_t *out)
{
    Walk w = {s, len, 0, 0, 0, 0, 0};
    size_t first = len, last = len, before = len, before_row = len, next = len, to;
    uint32_t row, line, next_line = 0;
    float x, end;
    int rc = VJO_ROW_MOVED;
    if (!s || cursor >= len || !direction)
        return VJO_ROW_STAY;
    if (!rows_ok(s, r) || !cell(s, len, r, cursor, &x, &end, &row))
        return VJO_ROW_UNAVAILABLE;
    /* Only selectable characters are queried; the scan ends after the row
     * once the first candidate at `from` is known. */
    while (walk(&w)) {
        if (!vjo_text_selectable(w.cp, w.i - w.begin, japanese_only))
            continue;
        if (!query(r, w.unit, &x, &line))
            return VJO_ROW_UNAVAILABLE;
        if (line == row) {
            if (first == len)
                first = w.begin;
            last = w.begin;
        }
        if (w.begin < cursor) {
            before = w.begin;
            if (line == row)
                before_row = w.begin;
        } else if (w.begin >= from && next == len) {
            next = w.begin;
            next_line = line;
        }
        if (line > row && next < len)
            break;
    }
    if (direction > 0) {
        to = next;
        if ((next == len || next_line != row) && first < len) {
            to = first;
            rc = VJO_ROW_WRAPPED;
        }
    } else {
        to = before_row;
        if (before_row == len) {
            to = last < len ? last : before;
            rc = last < len ? VJO_ROW_WRAPPED : VJO_ROW_MOVED;
        }
    }
    if (to >= len || to == cursor)
        return VJO_ROW_STAY;
    *out = to;
    return rc;
}

int vjo_text_row_vertical(const char *s, size_t len, const VjoTextRows *r, size_t cursor,
                          int direction, int japanese_only, VjoRowGoal *keep, size_t *out)
{
    Walk w = {s, len, 0, 0, 0, 0, 0};
    size_t best = len;
    uint32_t row, line, bline = 0;
    float x, end, goal, bd = 0;
    if (!s || cursor >= len || !direction)
        return VJO_ROW_STAY;
    if (!rows_ok(s, r) || !cell(s, len, r, cursor, &x, &end, &row))
        return VJO_ROW_UNAVAILABLE;
    goal = keep->at == cursor + 1 ? keep->x : (x + end) / 2.0f;
    /* In reading order: the rows above end at the cursor's row, the one
     * below at the next row after it. A nearer row restarts the choice. */
    while (walk(&w)) {
        if (!vjo_text_selectable(w.cp, w.i - w.begin, japanese_only))
            continue;
        if (!query(r, w.unit, &x, &line))
            return VJO_ROW_UNAVAILABLE;
        if (direction < 0 ? line >= row : best < len && line > bline)
            break;
        if (direction < 0 || line > row) {
            float e = cell_end(&w, r, x, line);
            float d = goal < x ? x - goal : goal > e ? goal - e : 0.0f;
            if (best == len || line > bline || d < bd) {
                best = w.begin;
                bline = line;
                bd = d;
            }
        }
    }
    if (best == len)
        return VJO_ROW_STAY;
    keep->x = goal;
    keep->at = (uint32_t)best + 1;
    *out = best;
    return VJO_ROW_MOVED;
}

/* No layout: the whole text as one row (the arrows still move). */
static int flat_position(void *ctx, uint32_t unit, float *x, uint32_t *line)
{
    (void)ctx;
    *x = (float)unit;
    *line = 0;
    return 0;
}

size_t vjo_word_step(VjoWordNavigation *h, const char *s, size_t len, size_t cursor,
                     size_t matched_end, int direction, int japanese_only, const VjoTextRows *r,
                     int *row_result)
{
    static const VjoTextRows flat = {flat_position, NULL, 1, 1.0f};
    size_t next = cursor, from = cursor + 1;
    int rc = VJO_ROW_STAY;
    if (s && cursor < len && direction) {
        /* The history only holds where Right started since the last tap, row
         * change or new text, so Left undoes it exactly, a wrap included. */
        if (direction < 0 && h->count && h->previous[h->count - 1] != cursor && h->previous[h->count - 1] < len) {
            next = h->previous[--h->count];
            rc = VJO_ROW_MOVED;
        } else {
            if (direction > 0 && matched_end > cursor && matched_end <= len &&
                (matched_end == len || vjo_utf8_cursor_step(s, len, matched_end, 0) == matched_end))
                from = matched_end;
            rc = r ? vjo_text_row_step(s, len, r, cursor, from, direction, japanese_only, &next) : VJO_ROW_UNAVAILABLE;
            if (rc < 0)
                vjo_text_row_step(s, len, &flat, cursor, from, direction, japanese_only, &next);
            if (next != cursor && direction < 0)
                h->count = 0;
            else if (next != cursor) {
                if (h->count == 16)
                    memmove(h->previous, h->previous + 1, 15 * sizeof(h->previous[0]));
                h->count -= h->count == 16;
                h->previous[h->count++] = (uint32_t)cursor;
            }
        }
    }
    if (row_result)
        *row_result = rc;
    return next;
}

size_t vjo_text_read_step(const char *s, size_t len, size_t cursor, size_t matched_end, int direction,
                          int japanese_only)
{
    /* vjo_word_step without rows steps through the text as one row; only its
     * wrap round the ends is refused (and its history is a throwaway one). */
    VjoWordNavigation once;
    size_t to;
    once.count = 0;
    to = vjo_word_step(&once, s, len, cursor, matched_end, direction, japanese_only, NULL, NULL);
    return (direction > 0 ? to > cursor : to < cursor) ? to : cursor;
}

int vjo_text_row_key(VjoRowNav *nav, const char *s, size_t len, const VjoTextRows *r, size_t cursor,
                     size_t matched_end, int dx, int dy, int japanese_only, size_t *out)
{
    int rc;
    *out = cursor;
    if (dx) {
        nav->goal.at = 0; /* Up/Down take their column afresh */
        *out = vjo_word_step(&nav->words, s, len, cursor, matched_end, dx, japanese_only, r, &rc);
    }
    else if ((rc = vjo_text_row_vertical(s, len, r, cursor, dy, japanese_only, &nav->goal, out)) > 0) {
        nav->words.count = 0;
        nav->row_moves++;
    }
    nav->wraps += rc == VJO_ROW_WRAPPED;
    nav->no_layout += rc < 0;
    return rc;
}
