#include "anki.h"

#include <string.h>

#include "furigana.h"
#include "pitch.h"
#include "port.h"
#include "textjoin.h"
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

/* ---- the word's furigana, as Yomitan's {furigana-plain} ---- */

#define FURI_MAX 48     /* code points of a word or its reading distributed (longer: the whole word) */
#define FURI_STEPS 4096 /* tries of the distribution before the whole word takes the reading */
/* ヵヶ: kanji to the bases here (vjo_is_kanji), kana to a ruby parser that reads Han script */
#define KANA_TO_RUBY(c) ((c) == 0x30F5 || (c) == 0x30F6)

typedef struct {
    uint32_t w[FURI_MAX], r[FURI_MAX]; /* the word and its reading, katakana as hiragana */
    size_t wo[FURI_MAX + 1], ro[FURI_MAX + 1]; /* byte offsets */
    int nw, nr, steps, found;
    int end[FURI_MAX];  /* a kanji run starting at i: where its reading ends in r */
    int best[FURI_MAX]; /* the same, of the first way it fits */
} Furi;

/* s's code points into cp (katakana as hiragana: readings are written in
 * hiragana) and their byte offsets; -1 when it has more than FURI_MAX. */
static int furi_split(const char *s, uint32_t *cp, size_t *off)
{
    size_t n = strlen(s), i = 0;
    int k = 0;
    while (i < n) {
        uint32_t c;
        if (k == FURI_MAX)
            return -1;
        off[k] = i;
        c = vjo_utf8_next(s, n, &i);
        cp[k++] = c >= 0x30A1 && c <= 0x30F6 && !vjo_is_kanji(c) ? c - 0x60 : c;
    }
    off[k] = n;
    return k;
}

/* The ways the word from i fits the reading from j, counted in found (up to
 * 2), the first one's runs in best: kana must match; a run of kanji takes one
 * or more of the reading's characters. Two ways (物の怪, もののけ: も|け or
 * もの|け) or too many tries leave it undecided: the whole word, as Yomitan. */
static void furi_fit(Furi *f, int i, int j)
{
    int e = i;
    if (f->found > 1)
        return;
    if (++f->steps > FURI_STEPS) {
        f->found = 2;
        return;
    }
    if (i == f->nw) {
        if (j == f->nr && !f->found++)
            memcpy(f->best, f->end, sizeof(f->end));
        return;
    }
    if (!vjo_is_kanji(f->w[i])) {
        if (j < f->nr && f->w[i] == f->r[j])
            furi_fit(f, i + 1, j + 1);
        return;
    }
    while (e < f->nw && vjo_is_kanji(f->w[e]))
        e++;
    for (int k = j + 1; k <= f->nr && f->found < 2; k++) {
        f->end[i] = k;
        furi_fit(f, e, k);
    }
}

/* "食[た]べる", "お 茶[ちゃ]", "真[ま]っ 白[しろ]": a space before a base that
 * follows other text, or holds a character a ruby parser may not count as a
 * kanji (ヵヶ), so Anki's furigana filter (Lapis's {{furigana:}}) reads exactly that run. */
void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading)
{
    Furi f;
    size_t n = strlen(spelling), i = 0;
    int any = 0, mixed = 0, fit;
    while (i < n) {
        uint32_t c = vjo_utf8_next(spelling, n, &i);
        any |= vjo_is_kanji(c);
        mixed |= !vjo_is_kanji(c) || KANA_TO_RUBY(c);
    }
    if (!any || !reading || !*reading || !strcmp(spelling, reading)) {
        html_escape(b, spelling, n);
        return;
    }
    f.steps = f.found = 0;
    if ((f.nw = furi_split(spelling, f.w, f.wo)) > 0 && (f.nr = furi_split(reading, f.r, f.ro)) > 0)
        furi_fit(&f, 0, 0);
    fit = f.found == 1;
    if (!fit) { /* the whole word takes the reading */
        if (mixed)
            vjo_buf_putc(b, ' ');
        html_escape(b, spelling, n);
        vjo_buf_putc(b, '[');
        html_escape(b, reading, strlen(reading));
        vjo_buf_putc(b, ']');
        return;
    }
    for (int i = 0, j = 0; i < f.nw;) {
        int e = i, space = i > 0;
        if (!vjo_is_kanji(f.w[i])) {
            html_escape(b, spelling + f.wo[i], f.wo[i + 1] - f.wo[i]);
            i++;
            j++;
            continue;
        }
        for (; e < f.nw && vjo_is_kanji(f.w[e]); e++)
            space |= KANA_TO_RUBY(f.w[e]);
        if (space)
            vjo_buf_putc(b, ' ');
        html_escape(b, spelling + f.wo[i], f.wo[e] - f.wo[i]);
        vjo_buf_putc(b, '[');
        html_escape(b, reading + f.ro[j], f.ro[f.best[i]] - f.ro[j]);
        vjo_buf_putc(b, ']');
        j = f.best[i];
        i = e;
    }
}

const char *vjo_anki_meaning_source(const char *meaning, char *name, size_t cap)
{
    const char *nl = strchr(meaning, '\n');
    size_t n;
    name[0] = '\0';
    /* tools/convert_dictionary.py: "[title]\n" + the definition; a title may
     * hold brackets itself ("Jitendex.org [2026-05-05]"). */
    if (meaning[0] != '[' || !nl || nl - meaning < 3 || nl[-1] != ']')
        return meaning;
    n = (size_t)(nl - meaning) - 2;
    if (n >= cap)
        n = cap - 1;
    memcpy(name, meaning + 1, n);
    name[n] = '\0';
    vjo_utf8_trim_partial(name);
    return nl + 1;
}

/* One dictionary's entry as Yomitan writes it into a glossary, which Lapis
 * pages by data-dictionary:
 * <li data-dictionary="NAME"><i>(NAME)</i> <span>...</span></li>; several
 * meanings in <ul>. NAME: dict, else the first meaning's own. */
static void gloss_item(VjoBuf *b, const char *dict, const char *const *meanings, int n)
{
    char name[96], own[96];
    if (n <= 0)
        return;
    vjo_anki_meaning_source(meanings[0], name, sizeof(name));
    if (dict && *dict)
        vjo_snprintf(name, sizeof(name), "%s", dict);
    vjo_buf_puts(b, "<li");
    if (*name) {
        vjo_buf_puts(b, " data-dictionary=\"");
        html_escape(b, name, strlen(name));
        vjo_buf_puts(b, "\"><i>(");
        html_escape(b, name, strlen(name));
        vjo_buf_puts(b, ")</i> ");
    } else {
        vjo_buf_putc(b, '>');
    }
    vjo_buf_puts(b, n > 1 ? "<span><ul>" : "<span>");
    for (int i = 0; i < n; i++) {
        const char *t = vjo_anki_meaning_source(meanings[i], own, sizeof(own));
        if (n > 1)
            vjo_buf_puts(b, "<li>");
        html_text(b, t, strlen(t));
        if (n > 1)
            vjo_buf_puts(b, "</li>");
    }
    vjo_buf_puts(b, n > 1 ? "</ul></span></li>" : "</span></li>");
}

#define GLOSSARY_OPEN "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
#define GLOSSARY_CLOSE "</ol></div>"

void vjo_anki_pitch_position(VjoBuf *b, const int *pitch, int n)
{
    if (n <= 0)
        return;
    if (n > 1)
        vjo_buf_puts(b, "<ol>");
    for (int i = 0; i < n; i++)
        vjo_buf_printf(b, "%s<span style=\"display:inline;\"><span>[</span><span>%d</span><span>]</span></span>%s",
                       n > 1 ? "<li>" : "", pitch[i], n > 1 ? "</li>" : "");
    if (n > 1)
        vjo_buf_puts(b, "</ol>");
}

void vjo_anki_pitch_categories(VjoBuf *b, const char *reading, const int *pitch, int n, const char *const *pos,
                               int n_pos)
{
    const char *seen[VJO_MAX_PITCH];
    int n_seen = 0;
    for (int i = 0; i < n && n_seen < VJO_MAX_PITCH; i++) {
        const char *c = vjo_pitch_category(reading, pitch[i], pos, n_pos);
        int dup = 0;
        for (int k = 0; k < n_seen && !dup; k++)
            dup = seen[k] == c;
        if (dup)
            continue;
        if (n_seen)
            vjo_buf_putc(b, ',');
        vjo_buf_puts(b, c);
        seen[n_seen++] = c;
    }
}

void vjo_anki_misc_info(char *out, size_t cap, const char *game, const VjoAnkiDate *when)
{
    int has_game = game && *game;
    if (!when)
        vjo_snprintf(out, cap, "%s", has_game ? game : "");
    else if (has_game)
        vjo_snprintf(out, cap, "%s (%04d-%02d-%02d %02d:%02d)", game, when->year, when->month, when->day,
                     when->hour, when->minute);
    else
        vjo_snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", when->year, when->month, when->day, when->hour,
                     when->minute);
    vjo_utf8_trim_partial(out);
}

static char *dup(VjoArena *a, const char *s)
{
    return vjo_arena_strndup(a, s ? s : "", s ? strlen(s) : 0);
}

/* The header with every word's furigana into out->sentence_furigana, with
 * entry target's range (if the furigana takes it). No lookup gives readings
 * per word any more (only the removed online dictionaries did), so it is the
 * header itself with the word's own range: what rendering the readings of a
 * local lookup, which has none, always made. */
static void sentence_furigana(const VjoEntryList *l, int target, VjoAnkiNote *out)
{
    out->sentence_furigana = out->sentence;
    if (vjo_entry_in_text(l, target)) {
        out->sf_hl_start = l->entries[target].hl_start;
        out->sf_hl_end = l->entries[target].hl_end;
    }
}

int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out)
{
    const VjoEntry *e;
    const VjoVocab *v;
    char **m, **p;
    if (!l || entry < 0 || entry >= l->n_entries)
        return -1;
    memset(out, 0, sizeof(*out));
    e = &l->entries[entry];
    v = e->vocab;
    out->spelling = dup(a, v->spelling);
    out->reading = dup(a, v->reading);
    out->sentence = dup(a, l->header);
    out->rank = v->rank;
    out->hl_start = e->hl_start;
    out->hl_end = e->hl_end;
    /* The frequency list's name is the card's own copy too (#528): it is in the lookup's memory, which the next
     * lookup writes over, and the Shell names a card's record before its screenshot and writes it after. */
    if (l->source && !(out->source = dup(a, l->source)))
        return -1;
    out->sf_hl_start = out->sf_hl_end = -1;
    /* The entry's dictionary: its file's title (Bee's Ultimate Dictionary), else
     * the name its definition's first line gives ("[JMdict Plain]"), else none. */
    {
        char name[96];
        const char *d = v->dictionary;
        if (!d)
            d = (vjo_anki_meaning_source(v->n_meanings ? v->meanings[0] : "", name, sizeof(name)), name);
        if (*d && !(out->dictionary = dup(a, d)))
            return -1;
    }
    m = (char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(v->n_meanings ? v->n_meanings : 1));
    p = (char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(v->n_pos ? v->n_pos : 1));
    if (!out->spelling || !out->reading || !out->sentence || !m || !p)
        return -1;
    sentence_furigana(l, entry, out);
    for (int i = 0; i < v->n_meanings; i++)
        if (!(m[i] = dup(a, v->meanings[i])))
            return -1;
    out->meanings = (const char **)m;
    out->n_meanings = v->n_meanings;
    for (int i = 0; i < v->n_pos; i++)
        if (!(p[i] = dup(a, v->pos[i])))
            return -1;
    out->pos = (const char **)p;
    out->n_pos = v->n_pos;
    out->n_pitch = v->n_pitch;
    memcpy(out->pitch, v->pitch, sizeof(out->pitch));
    return 0;
}

/* A card's Sentence into b (not yet written): the sentence the recognized
 * lines make (core/textjoin.h: a break the game's wrapping put inside it
 * goes, the word's range follows; a name's line and a new sentence's keep
 * theirs), joined in a copy before b's block. The queue keeps the text as
 * recognized, so a card queued before the join comes joined too. -1 on OOM. */
static int joined_sentence(VjoBuf *b, VjoArena *a, const char *text, int hl_start, int hl_end)
{
    int o[2] = {hl_start, hl_end};
    char *t = vjo_arena_strndup(a, text, strlen(text));
    if (!t)
        return -1;
    vjo_text_join(t, o, 2);
    vjo_anki_sentence(b, t, o[0], o[1]);
    return 0;
}

/* A finished buffer's text, its block (the arena's last allocation) cut to it. A VjoBuf grows by
 * doubling where it has room and to the exact size near the arena's end, so its capacity depends
 * on where it was built; cut to its text, a field value takes the same bytes wherever the export
 * builds it (#117's fix for the old send, kept for core/lapis.c's notes). NULL on OOM. */
static char *buf_done(VjoBuf *b)
{
    char *s = vjo_buf_cstr(b);
    if (s && vjo_arena_extend_last(b->arena, b->data, b->cap, b->len + 1) == 0)
        b->cap = b->len + 1;
    return s;
}

char *vjo_anki_field_value(VjoArena *a, int field, const VjoAnkiNote *n)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (field) {
    case VJO_LAPIS_EXPRESSION: html_escape(&b, n->spelling, strlen(n->spelling)); break;
    case VJO_LAPIS_EXPRESSION_READING: html_escape(&b, n->reading, strlen(n->reading)); break;
    case VJO_LAPIS_EXPRESSION_FURIGANA:
        if (n->furigana) {
            /* the dictionary's ruby ("来[く]る"), in Anki's syntax */
            char *f = vjo_furigana_align(a, n->spelling, strlen(n->spelling), n->furigana);
            vjo_buf_init(&b, a);
            if (f)
                html_escape(&b, f + (*f == ' '), strlen(f + (*f == ' ')));
        }
        if (!b.len)
            vjo_anki_furigana(&b, n->spelling, n->reading);
        break;
    case VJO_LAPIS_MAIN_DEFINITION:
    case VJO_LAPIS_GLOSSARY:
        /* Glossary: every dictionary's entry, the shown one first (Lapis
         * hides the ones MainDefinition has); none for a single one. */
        if (!n->n_meanings || (field == VJO_LAPIS_GLOSSARY && !n->n_more))
            break;
        vjo_buf_puts(&b, GLOSSARY_OPEN);
        gloss_item(&b, n->dictionary, n->meanings, n->n_meanings);
        for (int k = 0; field == VJO_LAPIS_GLOSSARY && k < n->n_more; k++)
            gloss_item(&b, n->more[k].dictionary, n->more[k].meanings, n->more[k].n_meanings);
        vjo_buf_puts(&b, GLOSSARY_CLOSE);
        break;
    case VJO_LAPIS_SENTENCE:
        if (joined_sentence(&b, a, n->sentence, n->hl_start, n->hl_end))
            return NULL;
        break;
    case VJO_LAPIS_FREQ_SORT:
        /* {frequency-harmonic-rank}: Yomitan writes 9999999 for a word with
         * no frequency (default-anki-field-templates.handlebars), so sorting
         * by FreqSort puts it last, as Yomitan's own Lapis cards. */
        vjo_buf_printf(&b, "%d", n->rank != VJO_NO_RANK ? n->rank : 9999999);
        break;
    case VJO_LAPIS_SENTENCE_FURIGANA:
        /* Lapis shows {{furigana:SentenceFurigana}}, else {{furigana:Sentence}}:
         * filled only when it has readings the sentence has not (Lapis's
         * README asks for it empty otherwise). */
        if (n->sentence_furigana && strcmp(n->sentence_furigana, n->sentence) &&
            joined_sentence(&b, a, n->sentence_furigana, n->sf_hl_start, n->sf_hl_end))
            return NULL;
        break;
    case VJO_LAPIS_PITCH_POSITION: vjo_anki_pitch_position(&b, n->pitch, n->n_pitch); break;
    case VJO_LAPIS_PITCH_CATEGORIES:
        vjo_anki_pitch_categories(&b, n->reading, n->pitch, n->n_pitch, n->pos, n->n_pos);
        break;
    case VJO_LAPIS_FREQUENCY:
        if (n->rank != VJO_NO_RANK && n->source) {
            vjo_buf_puts(&b, "<ul style=\"text-align: left;\"><li>");
            html_escape(&b, n->source, strlen(n->source));
            vjo_buf_printf(&b, ": %d</li></ul>", n->rank);
        }
        break;
    case VJO_LAPIS_MISC_INFO:
        if (n->misc)
            html_escape(&b, n->misc, strlen(n->misc));
        break;
    default: break;
    }
    return buf_done(&b);
}
