/* The black bars around a game's picture (core/shottrim.h): a scan of the frame's rows as the game
 * worker reads them, and the picture between the bars. No allocation and no copy: a few words of
 * state; a black row read whole, a content row only from each end to its content. */
#include "shottrim.h"

#include <stddef.h>

#define BLACK(p) (!((p) & VJO_TRIM_BLACK_MASK))
#define RGB(p) ((p) & 0x00FFFFFFu)

void vjo_trim_begin(VjoTrimScan *s, uint32_t w, uint32_t h)
{
    s->w = w;
    s->h = h;
    s->rows = s->top = s->last = s->color = 0;
    s->left = s->right = w;
    s->content = 0;
    s->above = s->uniform = 1;
}

/* The black pixels from p on, a pixel d apart (1 or -1), before the first content pixel, at most n; *same
 * is cleared unless all are `color`. A run of `color` (an emulator's bars) costs one compare a pixel. */
static uint32_t black_run(const uint32_t *p, int d, uint32_t n, uint32_t color, uint32_t *same)
{
    uint32_t i = 0;
    for (; i < n && RGB(*p) == color; i++, p += d)
        ;
    if (i < n && BLACK(*p)) {
        *same = 0;
        for (; i < n && BLACK(*p); i++, p += d)
            ;
    }
    return i;
}

void vjo_trim_rows(VjoTrimScan *s, const uint8_t *rgba, uint32_t n, uint32_t stride)
{
    const uint32_t w = s->w;
    if (n > s->h - s->rows)
        n = s->h - s->rows;
    for (uint32_t i = 0; i < n; i++, s->rows++) {
        const uint32_t *p = (const uint32_t *)(const void *)(rgba + (size_t)i * stride);
        uint32_t x, r, same = 1;
        if (!s->rows) /* the colour runs are measured against: row 0's first pixel's, if black (else none) */
            s->color = BLACK(p[0]) ? RGB(p[0]) : 0xFFFFFFFFu;
        /* from the left to the row's first content pixel (a black row: all of it) */
        x = black_run(p, 1, w, s->color, &same);
        if (x < w) {
            s->content = 1;
            s->last = s->rows;
            if (x < s->left)
                s->left = x;
            /* from the right, no further than the fewest yet: p[x] is content, so this stops by it */
            r = black_run(p + w - 1, -1, s->right, s->color, &same);
            if (r < s->right)
                s->right = r;
        } else if (!s->content) { /* above the picture */
            s->top++;
            s->above &= (uint8_t)same;
        }
        s->uniform &= (uint8_t)same;
    }
}

/* An axis' two bars a and b on a side of `side` pixels: 0 (no bars), the picture's offset (a
 * pair), or -1 (bars that frame no centred picture). */
static int axis(uint32_t a, uint32_t b, uint32_t side)
{
    uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
    if (hi < VJO_TRIM_MIN_BAR)
        return 0;
    if (lo < VJO_TRIM_MIN_BAR || hi * 10u > side * 3u || lo * 4u < hi * 3u) /* (sides of at most 4,096) */
        return -1;
    return (int)((lo - VJO_TRIM_MARGIN) & ~1u);
}

static void whole(const VjoTrimScan *s, VjoTrimRect *r)
{
    r->x = r->y = 0;
    r->w = s->w;
    r->h = s->h;
}

static int decide(const VjoTrimScan *s, uint32_t bottom, VjoTrimRect *r)
{
    int v = axis(s->top, bottom, s->h), x = axis(s->left, s->right, s->w);
    whole(s, r);
    if (!s->content || v < 0 || x < 0)
        return VJO_TRIM_KEPT;
    if (!v && !x)
        return VJO_TRIM_NONE;
    r->x = (uint32_t)x;
    r->y = (uint32_t)v;
    r->w = s->w - 2u * (uint32_t)x;
    r->h = s->h - 2u * (uint32_t)v;
    return VJO_TRIM_TRIMMED;
}

int vjo_trim_decide(const VjoTrimScan *s, VjoTrimRect *r)
{
    return decide(s, s->h - 1u - s->last, r);
}

int vjo_trim_guess(const VjoTrimScan *s, VjoTrimRect *r, int *exact)
{
    int kind;
    /* every row scanned (a frame within one strip): the decision itself */
    *exact = 1;
    if (s->rows == s->h)
        return vjo_trim_decide(s, r);
    /* a black band over 3/10 of the frame at its top that is a bar (8 rows or more; under 8 rows it is none, which
     * only a frame under 27 rows can have), with a content row below it or none yet: kept, whatever is below */
    if (s->top >= VJO_TRIM_MIN_BAR && (!s->content || s->top * 10u > s->h * 3u)) {
        whole(s, r);
        return VJO_TRIM_KEPT;
    }
    /* a content row reaching the top and a side edge: neither axis can be a pair (none is trimmed), whatever is below */
    *exact = s->content && s->top < VJO_TRIM_MIN_BAR && (s->left < VJO_TRIM_MIN_BAR || s->right < VJO_TRIM_MIN_BAR);
    kind = decide(s, s->top < VJO_TRIM_MIN_BAR ? 0u : s->top, r);
    /* side bars under no top bar whose black is not all one colour are mostly a dark picture's edges (an
     * emulator's bars are one colour): guess the whole frame; the rows below decide, as for any guess */
    if (!*exact && s->top < VJO_TRIM_MIN_BAR && !s->uniform) {
        whole(s, r);
        kind = VJO_TRIM_KEPT;
    }
    return kind;
}
