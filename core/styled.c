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

/* A neighbour's row, dimmed: the last row of the line before (at most 128 units: a textbox's row is
 * about 40, and the text after it keeps its own) and a break, or a break and the first row of the
 * line after. */
static void faded_row(VjoStyled *s, const char *t, int after, int px)
{
    const char *nl;
    uint32_t cap = s->cap;
    if (!t || !*t)
        return;
    if (after) {
        vjo_styled_add(s, "\n", 1, VJO_RGB_RANK, px);
    } else {
        while ((nl = strchr(t, '\n')))
            t = nl + 1;
        if (cap > s->len + 128)
            s->cap = s->len + 128; /* vjo_styled_add stops at a whole character */
    }
    nl = strchr(t, '\n');
    vjo_styled_add(s, t, nl ? (size_t)(nl - t) : strlen(t), VJO_RGB_RANK, px);
    s->cap = cap;
    if (!after)
        vjo_styled_add(s, "\n", 1, VJO_RGB_RANK, px);
}

uint32_t vjo_styled_history(VjoStyled *s, const char *before, const char *text, const char *after, int ja_px)
{
    uint32_t lead;
    faded_row(s, before, 0, ja_px);
    lead = s->len;
    vjo_styled_puts(s, text, VJO_RGB_TEXT, ja_px);
    faded_row(s, after, 1, ja_px);
    return lead;
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

#include "conjugation.inc"
static const char conjugation_prefix[] VJO_DATA_TABLE(conjugation_prefix) = "\nConjugation: ";
static const char conjugation_arrow[] VJO_DATA_TABLE(conjugation_arrow) = " → ";

/* A vocabulary's headword, reading and, with mark, the badge, the in-Anki mark and the conjugation
 * (the shown entry's own: NULL for a stacked one), then its meanings. */
static void vocab_text(VjoStyled *s, const VjoVocab *v, const VjoEntryList *l, const VjoEntry *e, int ja_px,
                       int en_px, int in_anki, int show_rank)
{
    char rank[VJO_RANK_BADGE_MAX];
    vjo_styled_puts(s, v->spelling, VJO_RGB_HIGHLIGHT, ja_px);
    if (strcmp(v->spelling, v->reading) != 0) {
        vjo_styled_puts(s, " (", VJO_RGB_READING, ja_px);
        vjo_styled_puts(s, v->reading, VJO_RGB_READING, ja_px);
        vjo_styled_puts(s, ")", VJO_RGB_READING, ja_px);
    }
    if (e) {
        if (show_rank)
            vjo_styled_puts(s, vjo_rank_badge(rank, v->rank, l->source), VJO_RGB_RANK, en_px);
        if (in_anki)
            vjo_styled_puts(s, " " VJO_ANKI_MARK, VJO_RGB_OK, en_px);
        uint32_t reasons = e->conjugation;
        if (reasons) {
            vjo_styled_puts(s, conjugation_prefix, VJO_RGB_READING, en_px);
            for (; reasons; reasons >>= 6) {
                unsigned reason = reasons & 63u;
                vjo_styled_puts(s, reason && reason < sizeof(conjugation_offsets) / sizeof(*conjugation_offsets)
                    ? conjugation_labels + conjugation_offsets[reason] : "…", VJO_RGB_READING, en_px);
                if (reasons >> 6) vjo_styled_puts(s, conjugation_arrow, VJO_RGB_READING, en_px);
            }
        }
    }
    for (int i = 0; i < v->n_meanings; i++) {
        if (i == v->n_meanings - 1) {
            entry_text(s, v->meanings[i], ja_px, en_px);
            break;
        }
        vjo_styled_puts(s, "\n", VJO_RGB_DIM, en_px);
        vjo_styled_puts(s, v->meanings[i], VJO_RGB_DIM, en_px);
    }
}

/* UTF-16 units a stacked vocabulary takes at most: its blank line, headword, reading and meanings (each
 * after a newline), and a clip mark. */
static uint32_t stacked_units(const VjoVocab *v)
{
    uint32_t n = 3 + vjo_utf16_index(v->spelling, strlen(v->spelling)) + vjo_utf16_index(v->reading, strlen(v->reading)) + 3;
    for (int i = 0; i < v->n_meanings; i++)
        n += vjo_utf16_index(v->meanings[i], strlen(v->meanings[i])) + 3;
    return n;
}

void vjo_styled_entry(VjoStyled *s, const VjoEntryList *l, int selected, int ja_px, int en_px, int in_anki,
                      int show_rank)
{
    uint32_t cap, room = 0;
    if (!l || selected < 0 || selected >= l->n_entries)
        return;
    /* the stacked entries below keep their room: a long entry is clipped instead */
    cap = s->cap;
    for (int i = 0; i < l->n_more && selected == 0; i++)
        room += stacked_units(l->more[i]);
    s->cap = cap > s->len + room ? cap - room : s->len;
    vocab_text(s, l->entries[selected].vocab, l, &l->entries[selected], ja_px, en_px, in_anki, show_rank);
    s->cap = cap;
    for (int i = 0; i < l->n_more && selected == 0; i++) {
        const VjoVocab *v = l->more[i];
        vjo_styled_puts(s, "\n\n", VJO_RGB_DIM, en_px); /* its definition names its dictionary ("[title]") */
        vocab_text(s, v, l, NULL, ja_px, en_px, 0, 0);
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
