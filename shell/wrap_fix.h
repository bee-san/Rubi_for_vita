/* The overlay's paf texts are wrapped by paf (WordWrap and Kinsoku), which
 * breaks Japanese anywhere. Once a text is laid out, vjo_wrap_fix finds the
 * first line paf started where core/linebreak.h forbids it (inside a run of
 * kanji, katakana or kana, between kanji and its kana, before a closing mark),
 * and gives the last place the line before may break instead; the caller
 * puts a hard break ('\n') there and sets the text again, one break a
 * frame, until no such line is left (owner, 2026-10-16, item 4: "we
 * shouldn't split a word down the middle either"). A line with no allowed
 * place (one word wider than the box) keeps paf's break. Only the text's own
 * layout is read (GetLineCount, GetCharInfo), never estimated, so the
 * console's glyph widths decide where lines end. Templates, so that the host
 * tests run this code on a model layout (host/tests/test_wrap_fix.cpp). */
#ifndef VJO_WRAP_FIX_H
#define VJO_WRAP_FIX_H

#include <stdint.h>

extern "C" {
#include "../core/linebreak.h"
}

#define VJO_WRAP_WAIT 0xFFFFFFFFu /* not laid out yet, or still the text before the last break */
#define VJO_WRAP_BREAKS 16        /* breaks put into one text, at most */
#define VJO_WRAP_SETTLE 2         /* frames from setting a text to reading its layout */
#define VJO_WRAP_TRIES 6          /* frames more to wait for a layout */

/* The laid-out line of unit i; -1 when there is none yet. */
template <class Info, class Text>
static inline int64_t vjo_wrap_line(Text *t, uint32_t i)
{
    Info ci;
    return t->GetCharInfo(i, ci) < 0 ? -1 : (int64_t)ci.line;
}

/* The unit to start a line at instead of the first one paf started inside a
 * word: 0 when there is none, VJO_WRAP_WAIT while the layout is not this
 * text's (no line count, or no position for its last unit: a layout of the
 * text before the last '\n' was put in has one unit fewer). Lines grow with
 * the units. A layout read too early can only end a line early: every place
 * returned is one core/linebreak.h allows in this text. */
template <class Info, class Text>
uint32_t vjo_wrap_fix(Text *t, const uint16_t *u, uint32_t n)
{
    uint32_t lines = 0, start = 0;
    if (!t || !n)
        return 0;
    if (t->GetLineCount(lines) < 0 || !lines || vjo_wrap_line<Info>(t, n - 1) < 0)
        return VJO_WRAP_WAIT;
    for (uint32_t line = 1; line < lines; line++) {
        uint32_t lo = start, hi = n, b; /* the line's first unit: the first at or past it */
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int64_t l = vjo_wrap_line<Info>(t, mid);
            if (l < 0)
                return VJO_WRAP_WAIT;
            if (l >= (int64_t)line)
                hi = mid;
            else
                lo = mid + 1;
        }
        if (lo <= start || lo >= n)
            return VJO_WRAP_WAIT;
        /* a hard break, a space or a place a line may start: no word is cut (the
         * last test reads a few units before lo only); else the line before
         * ends at its last such place */
        if (u[lo - 1] != '\n' && u[lo] != '\n' && u[lo] != ' ' && u[lo] != 0x3000 &&
            vjo_break_last16(u, n, lo - 1, lo) != lo && (b = vjo_break_last16(u, n, start, lo)) > start)
            return b;
        start = lo;
    }
    return 0;
}

/* One text's re-wrap, a step a frame: armed with VJO_WRAP_SETTLE frames when
 * the text is set. last: the unit of the '\n' put in last. */
struct VjoWrap {
    int frames, tries, count;
    uint32_t last;
};

static inline void vjo_wrap_arm(VjoWrap *w)
{
    w->frames = VJO_WRAP_SETTLE;
    w->tries = w->count = 0;
}

/* This frame's step: the unit to put a '\n' before (the caller puts it in,
 * sets the text again and calls vjo_wrap_put), else 0. room: units free. */
template <class Info, class Text>
uint32_t vjo_wrap_step(VjoWrap *w, Text *t, const uint16_t *u, uint32_t n, uint32_t room)
{
    uint32_t b;
    if (!w->frames || --w->frames)
        return 0;
    b = vjo_wrap_fix<Info>(t, u, n);
    if (b == VJO_WRAP_WAIT) {
        w->frames = ++w->tries <= VJO_WRAP_TRIES; /* try again next frame */
        return 0;
    }
    return room && w->count < VJO_WRAP_BREAKS ? b : 0;
}

static inline void vjo_wrap_put(VjoWrap *w, uint32_t b)
{
    w->frames = VJO_WRAP_SETTLE;
    w->tries = 0;
    w->count++;
    w->last = b;
}

/* The recognized text's own units against the shown ones once breaks are
 * put in (overlay.cpp's header: the cursor, taps and the highlight count the
 * text's units). brk: the text's units a break was put before, ascending. */
static inline uint32_t vjo_wrap_shown(const uint32_t *brk, int n, uint32_t unit)
{
    uint32_t shown = unit;
    for (int i = 0; i < n && brk[i] <= unit; i++)
        shown++;
    return shown;
}

/* Records a break put before shown unit `at` as the text's unit, in order. */
static inline void vjo_wrap_add(uint32_t *brk, int *n, uint32_t at)
{
    int i = 0;
    while (i < *n && brk[i] + (uint32_t)i < at) /* break i is shown at brk[i] + i */
        i++;
    for (int k = *n; k > i; k--)
        brk[k] = brk[k - 1];
    brk[i] = at - (uint32_t)i;
    ++*n;
}

#endif
