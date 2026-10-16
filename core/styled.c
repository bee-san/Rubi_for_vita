#include "styled.h"

#include <string.h>

#include "port.h"
#include "utf.h"

int vjo_font_px(int font_size)
{
    return (font_size * 3 + 1) / 2;
}

int vjo_styled_init(VjoStyled *s, VjoArena *a, uint32_t cap_units, int cap_spans)
{
    memset(s, 0, sizeof(*s));
    s->text = (uint16_t *)vjo_arena_alloc(a, sizeof(uint16_t) * (cap_units + 1));
    s->spans = (VjoSpan *)vjo_arena_alloc(a, sizeof(VjoSpan) * (size_t)cap_spans);
    if (!s->text || !s->spans)
        return -1;
    s->cap = cap_units;
    s->cap_spans = cap_spans;
    s->text[0] = 0;
    return 0;
}

void vjo_styled_add(VjoStyled *s, const char *utf8, size_t n, uint32_t rgb, int px)
{
    size_t i = 0;
    uint32_t start = s->len;
    while (i < n) {
        uint32_t cp = vjo_utf8_next(utf8, n, &i);
        int units = vjo_utf16_units(cp);
        if (cp == '\r')
            continue;
        if (s->len + (uint32_t)units > s->cap) {
            s->truncated = 1;
            break;
        }
        if (units == 2) {
            cp -= 0x10000;
            s->text[s->len++] = (uint16_t)(0xD800 | (cp >> 10));
            s->text[s->len++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            s->text[s->len++] = (uint16_t)cp;
        }
    }
    s->text[s->len] = 0;
    if (s->len == start)
        return;
    if (s->n_spans >= s->cap_spans) {
        s->truncated = 1;
        return;
    }
    s->spans[s->n_spans].start = start;
    s->spans[s->n_spans].len = s->len - start;
    s->spans[s->n_spans].rgb = rgb;
    s->spans[s->n_spans].px = px;
    s->n_spans++;
}

void vjo_styled_puts(VjoStyled *s, const char *utf8, uint32_t rgb, int px)
{
    vjo_styled_add(s, utf8, strlen(utf8), rgb, px);
}

int vjo_styled_insert(VjoStyled *s, uint32_t at, uint16_t unit)
{
    if (!at || at > s->len || s->len >= s->cap)
        return 0;
    memmove(s->text + at + 1, s->text + at, (s->len - at + 1) * sizeof(uint16_t)); /* with the NUL */
    s->text[at] = unit;
    s->len++;
    for (int i = 0; i < s->n_spans; i++) {
        VjoSpan *sp = &s->spans[i];
        if (sp->start >= at)
            sp->start++;
        else if (sp->start + sp->len >= at)
            sp->len++;
    }
    return 1;
}

uint32_t vjo_utf16_index(const char *utf8, size_t off)
{
    size_t len = strlen(utf8), i = 0;
    uint32_t u = 0;
    if (off > len)
        off = len;
    while (i < off) {
        uint32_t cp = vjo_utf8_next(utf8, len, &i);
        if (cp != '\r')
            u += (uint32_t)vjo_utf16_units(cp);
    }
    return u;
}

void vjo_styled_header(VjoStyled *s, const VjoEntryList *l, int ja_px)
{
    if (l && l->header)
        vjo_styled_puts(s, l->header, VJO_RGB_TEXT, ja_px);
}

int vjo_entry_header_range(const VjoEntryList *l, int entry, uint32_t *start, uint32_t *len)
{
    const VjoEntry *e;
    if (!l || entry < 0 || entry >= l->n_entries)
        return 0;
    e = &l->entries[entry];
    if (e->hl_start < 0)
        return 0;
    *start = vjo_utf16_index(l->header, (size_t)e->hl_start);
    *len = vjo_utf16_index(l->header, (size_t)e->hl_end) - *start;
    return *len > 0;
}

#define CLIP_MARK "…" /* where a definition too long for the body is cut */

/* The entry's last meaning: its definition, a blank line, then its kanji lines
 * as written, each kanji at ja_px (see styled.h). */
static void entry_text(VjoStyled *s, const char *text, int ja_px, int en_px)
{
    size_t len = strlen(text), k = vjo_kanji_block(text), d = k, gap;
    if (k < len)
        while (d && text[d - 1] == '\n') /* the definition ends before the newline(s) its kanji lines follow */
            d--;
    gap = d && k < len ? 2 : 0; /* "\n\n": a blank line between a definition and its kanji lines */
    /* the kanji lines, the blank line and the mark */
    uint32_t cap = s->cap, after = vjo_utf16_index(text + k, len - k) + (uint32_t)gap + 1;
    int truncated = s->truncated;
    vjo_styled_puts(s, "\n", VJO_RGB_DIM, en_px);
    /* The definition in the room the kanji lines and the mark leave:
     * vjo_styled_add stops at a whole character. */
    s->cap = cap > after ? cap - after : 0;
    vjo_styled_add(s, text, d, VJO_RGB_DIM, en_px);
    s->cap = cap;
    if (s->truncated != truncated) { /* clipped: marked, not truncated */
        s->truncated = truncated;
        vjo_styled_puts(s, CLIP_MARK, VJO_RGB_DIM, en_px);
    }
    vjo_styled_add(s, "\n\n", gap, VJO_RGB_DIM, en_px); /* the blank line before the lines */
    while (k < len) {
        const char *eq = strchr(text + k, '='), *end = strchr(eq, '\n'); /* vjo_kanji_block: right after the kanji */
        size_t next = end ? (size_t)(end - text) + 1 : len;
        vjo_styled_add(s, text + k, (size_t)(eq - (text + k)), VJO_RGB_TEXT, ja_px);
        vjo_styled_add(s, eq, next - (size_t)(eq - text), VJO_RGB_DIM, en_px); /* "=meaning, meaning" and its newline */
        k = next;
    }
}

void vjo_styled_entry(VjoStyled *s, const VjoEntryList *l, int selected, int ja_px, int en_px, int in_anki,
                      int show_rank)
{
    const VjoVocab *v;
    char rank[VJO_RANK_BADGE_MAX];
    if (!l || selected < 0 || selected >= l->n_entries)
        return;
    v = l->entries[selected].vocab;
    vjo_styled_puts(s, v->spelling, VJO_RGB_HIGHLIGHT, ja_px);
    if (strcmp(v->spelling, v->reading) != 0) {
        vjo_styled_puts(s, " (", VJO_RGB_READING, ja_px);
        vjo_styled_puts(s, v->reading, VJO_RGB_READING, ja_px);
        vjo_styled_puts(s, ")", VJO_RGB_READING, ja_px);
    }
    if (show_rank)
        vjo_styled_puts(s, vjo_rank_badge(rank, v->rank, l->source), VJO_RGB_RANK, en_px);
    if (in_anki)
        vjo_styled_puts(s, " " VJO_ANKI_MARK, VJO_RGB_OK, en_px);
    for (int i = 0; i < v->n_meanings; i++) {
        if (i == v->n_meanings - 1) {
            entry_text(s, v->meanings[i], ja_px, en_px);
            break;
        }
        vjo_styled_puts(s, "\n", VJO_RGB_DIM, en_px);
        vjo_styled_puts(s, v->meanings[i], VJO_RGB_DIM, en_px);
    }
}

void vjo_styled_dict_page(VjoStyled *s, int page, int pages, int en_px)
{
    char line[48];
    if (pages <= 1 || page < 0 || page >= pages)
        return;
    vjo_snprintf(line, sizeof(line), "L/R Dictionary %d of %d", page + 1, pages);
    vjo_styled_puts(s, line, VJO_RGB_RANK, en_px);
}
