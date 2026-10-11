#include "anki.h"

#include <string.h>

#include "port.h"
#include "utf.h"

/* ---------------- field values ---------------- */

static void html_escape(VjoBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': vjo_buf_puts(b, "&amp;"); break;
        case '<': vjo_buf_puts(b, "&lt;"); break;
        case '>': vjo_buf_puts(b, "&gt;"); break;
        case '"': vjo_buf_puts(b, "&quot;"); break;
        default: vjo_buf_putc(b, s[i]); break;
        }
    }
}

/* Escaped, with line breaks as <br> (CR dropped). */
static void html_text(VjoBuf *b, const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '\n' || s[i] == '\r') {
            html_escape(b, s + start, i - start);
            if (i < n && s[i] == '\n')
                vjo_buf_puts(b, "<br>");
            start = i + 1;
        }
    }
}

void vjo_anki_sentence(VjoBuf *b, const char *text, int hl_start, int hl_end)
{
    size_t n = strlen(text);
    if (hl_start < 0 || hl_end <= hl_start || (size_t)hl_end > n) {
        html_text(b, text, n);
        return;
    }
    html_text(b, text, (size_t)hl_start);
    vjo_buf_puts(b, "<b>");
    html_text(b, text + hl_start, (size_t)(hl_end - hl_start));
    vjo_buf_puts(b, "</b>");
    html_text(b, text + hl_end, n - (size_t)hl_end);
}

static int is_kanji(uint32_t cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x3134F) || cp == 0x3005 /* 々 */;
}

static int has_kanji(const char *s)
{
    size_t n = strlen(s), i = 0;
    while (i < n)
        if (is_kanji(vjo_utf8_next(s, n, &i)))
            return 1;
    return 0;
}

void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading)
{
    html_escape(b, spelling, strlen(spelling));
    if (reading && *reading && strcmp(spelling, reading) != 0 && has_kanji(spelling)) {
        vjo_buf_putc(b, '[');
        html_escape(b, reading, strlen(reading));
        vjo_buf_putc(b, ']');
    }
}

void vjo_anki_definition(VjoBuf *b, const char *const *meanings, int n)
{
    if (n <= 0)
        return;
    vjo_buf_puts(b, "<ol>");
    for (int i = 0; i < n; i++) {
        vjo_buf_puts(b, "<li>");
        html_escape(b, meanings[i], strlen(meanings[i]));
        vjo_buf_puts(b, "</li>");
    }
    vjo_buf_puts(b, "</ol>");
}

static char *dup(VjoArena *a, const char *s)
{
    return vjo_arena_strndup(a, s ? s : "", s ? strlen(s) : 0);
}

int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out)
{
    const VjoEntry *e;
    const VjoVocab *v;
    char **m;
    if (!out || !l || !l->entries || entry < 0 || entry >= l->n_entries ||
        !l->entries[entry].vocab || l->entries[entry].vocab->n_meanings < 0)
        return -1;
    memset(out, 0, sizeof(*out));
    e = &l->entries[entry];
    v = e->vocab;
    out->spelling = dup(a, v->spelling);
    out->reading = dup(a, v->reading);
    out->sentence = dup(a, l->header);
    out->hl_start = e->hl_start;
    out->hl_end = e->hl_end;
    out->n_meanings = 0;
    m = (char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(v->n_meanings ? v->n_meanings : 1));
    if (!out->spelling || !out->reading || !out->sentence || !m)
        return -1;
    for (int i = 0; i < v->n_meanings; i++)
        if (!(m[i] = dup(a, v->meanings[i])))
            return -1;
    out->meanings = (const char **)m;
    out->n_meanings = v->n_meanings;
    return 0;
}
