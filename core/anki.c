#include "anki.h"

#include <string.h>

#include "base64.h"
#include "json.h"
#include "pitch.h"
#include "port.h"
#include "utf.h"

/* A computer that is off: give up quickly (the default is ~20 s). */
#define CONNECT_TIMEOUT_US (3 * 1000 * 1000)

/* AnkiConnect's error messages, matched by prefix: the classic add-on's
 * (its createNote), then its rewrite's (2026: notes.validate, resolve). */
#define ERR_DUPLICATE "cannot create note because it is a duplicate"
#define ERR_DUPLICATE_V2 "A matching note exists."
#define ERR_NO_MODEL "model was not found: "
#define ERR_NO_MODEL_V2 "Notetype not found: "
#define ERR_NO_DECK "deck was not found: "
#define ERR_NO_DECK_V2 "Deck not found: "
/* The rewrite's addNote names no reason: canAddNotesWithErrorDetail has it. */
#define ERR_VALIDATION_V2 "One or more notes failed validation."

/* Kiku v2.1.0's field names (youyoumu/kiku b7fb283, packages/note/src/lib/types.ts,
 * the same in Kiku_v2.1.0.apkg; docs/anki.md). */
const char *const vjo_kiku_field[VJO_KIKU_FIELDS] = {
    [VJO_KIKU_EXPRESSION] = "Expression",
    [VJO_KIKU_EXPRESSION_FURIGANA] = "ExpressionFurigana",
    [VJO_KIKU_EXPRESSION_READING] = "ExpressionReading",
    [VJO_KIKU_EXPRESSION_AUDIO] = "ExpressionAudio",
    [VJO_KIKU_MAIN_DEFINITION] = "MainDefinition",
    [VJO_KIKU_SENTENCE] = "Sentence",
    [VJO_KIKU_SENTENCE_FURIGANA] = "SentenceFurigana",
    [VJO_KIKU_PICTURE] = "Picture",
    [VJO_KIKU_GLOSSARY] = "Glossary",
    [VJO_KIKU_PITCH_POSITION] = "PitchPosition",
    [VJO_KIKU_PITCH_CATEGORIES] = "PitchCategories",
    [VJO_KIKU_FREQUENCY] = "Frequency",
    [VJO_KIKU_FREQ_SORT] = "FreqSort",
    [VJO_KIKU_MISC_INFO] = "MiscInfo",
};

static int starts_with(const char *s, const char *prefix)
{
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

static int is_duplicate(const char *e)
{
    return starts_with(e, ERR_DUPLICATE) || starts_with(e, ERR_DUPLICATE_V2);
}

static int is_no_deck(const char *e)
{
    return starts_with(e, ERR_NO_DECK) || starts_with(e, ERR_NO_DECK_V2);
}

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
/* ヵヶ: kanji to the bases here (vjo_is_kanji), kana to Kiku's parser (not Han script) */
#define KANA_TO_KIKU(c) ((c) == 0x30F5 || (c) == 0x30F6)

typedef struct {
    uint32_t w[FURI_MAX], r[FURI_MAX]; /* the word and its reading, katakana as hiragana */
    size_t wo[FURI_MAX + 1], ro[FURI_MAX + 1]; /* byte offsets */
    int nw, nr, steps;
    int end[FURI_MAX]; /* a kanji run starting at i: where its reading ends in r */
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

/* Does the word from i fit the reading from j? Kana must match; a run of
 * kanji takes one or more of the reading's characters, the fewest first. */
static int furi_fit(Furi *f, int i, int j)
{
    int e = i;
    if (++f->steps > FURI_STEPS)
        return 0;
    if (i == f->nw)
        return j == f->nr;
    if (!vjo_is_kanji(f->w[i]))
        return j < f->nr && f->w[i] == f->r[j] && furi_fit(f, i + 1, j + 1);
    while (e < f->nw && vjo_is_kanji(f->w[e]))
        e++;
    for (int k = j + 1; k <= f->nr; k++) {
        f->end[i] = k;
        if (furi_fit(f, e, k))
            return 1;
    }
    return 0;
}

/* "食[た]べる", "お 茶[ちゃ]", "真[ま]っ 白[しろ]": a space before a base that
 * follows other text, or holds a character Kiku's parser does not count as a
 * kanji (ヵヶ), so Anki's furigana filter and Kiku both read exactly that run. */
void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading)
{
    Furi f;
    size_t n = strlen(spelling), i = 0;
    int any = 0, mixed = 0, fit;
    while (i < n) {
        uint32_t c = vjo_utf8_next(spelling, n, &i);
        any |= vjo_is_kanji(c);
        mixed |= !vjo_is_kanji(c) || KANA_TO_KIKU(c);
    }
    if (!any || !reading || !*reading || !strcmp(spelling, reading)) {
        html_escape(b, spelling, n);
        return;
    }
    f.steps = 0;
    fit = (f.nw = furi_split(spelling, f.w, f.wo)) > 0 && (f.nr = furi_split(reading, f.r, f.ro)) > 0 &&
          furi_fit(&f, 0, 0);
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
            space |= KANA_TO_KIKU(f.w[e]);
        if (space)
            vjo_buf_putc(b, ' ');
        html_escape(b, spelling + f.wo[i], f.wo[e] - f.wo[i]);
        vjo_buf_putc(b, '[');
        html_escape(b, reading + f.ro[j], f.ro[f.end[i]] - f.ro[j]);
        vjo_buf_putc(b, ']');
        j = f.end[i];
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

/* One dictionary's entry as Yomitan writes it into a glossary, which Kiku
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

/* The header with every word's furigana into out->sentence_furigana,
 * with entry target's range (if the furigana takes it). Returns 0, or -1 on
 * OOM. */
static int sentence_furigana(VjoArena *a, const VjoEntryList *l, int target, VjoAnkiNote *out)
{
    const char *h = l->header;
    size_t len = strlen(h);
    int cap = (int)(len / 2) + 1; /* a kanji is 3 or 4 bytes */
    VjoRuby *r = (VjoRuby *)vjo_arena_alloc(a, sizeof(VjoRuby) * (size_t)cap);
    int nr;
    VjoBuf b;
    if (!r)
        return -1;
    nr = vjo_entries_rubies(l, r, cap);
    vjo_buf_init(&b, a);
    if (vjo_entry_in_text(l, target)) {
        size_t hs = (size_t)l->entries[target].hl_start, he = (size_t)l->entries[target].hl_end;
        vjo_furigana_render(&b, h, 0, hs, r, nr, 0);
        out->sf_hl_start = (int)b.len;
        vjo_furigana_render(&b, h, hs, he, r, nr, 0);
        out->sf_hl_end = (int)b.len;
        vjo_furigana_render(&b, h, he, len, r, nr, 0);
    } else {
        vjo_furigana_render(&b, h, 0, len, r, nr, 0);
    }
    if (b.oom)
        return -1;
    out->sentence_furigana = vjo_buf_cstr(&b);
    return 0;
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
    out->source = l->source;
    out->furigana = v->ruby ? dup(a, v->ruby) : NULL;
    out->sf_hl_start = out->sf_hl_end = -1;
    if (sentence_furigana(a, l, entry, out) < 0) {
        out->sentence_furigana = NULL; /* added without it */
        out->sf_hl_start = out->sf_hl_end = -1;
    }
    m = (char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(v->n_meanings ? v->n_meanings : 1));
    p = (char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(v->n_pos ? v->n_pos : 1));
    if (!out->spelling || !out->reading || !out->sentence || !m || !p)
        return -1;
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

/* ---------------- requests ---------------- */

static size_t body_len(const VjoAnkiBody *b)
{
    return b->prefix_len + (b->picture_len ? vjo_base64_len(b->picture_len) : 0) + b->suffix_len;
}

#define VERSION_REQUEST "{\"action\":\"version\",\"version\":6}"
#define CREATE_DECK_REQUEST "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"" VJO_ANKI_DECK "\"}}"

/* The note's target and its duplicate check: the Kiku deck only. */
#define TARGET "\"deckName\":\"" VJO_ANKI_DECK "\",\"modelName\":\"" VJO_ANKI_NOTE_TYPE "\""
#define OPTIONS                                                                                               \
    ",\"options\":{\"allowDuplicate\":false,\"duplicateScope\":\"deck\",\"duplicateScopeOptions\":{"           \
    "\"deckName\":\"" VJO_ANKI_DECK "\",\"checkChildren\":false,\"checkAllModels\":false}}"

/* canAddNotesWithErrorDetail for the n words, each in Expression. */
static char *can_add_request(VjoArena *a, const char *const *words, int n)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"action\":\"canAddNotesWithErrorDetail\",\"version\":6,\"params\":{\"notes\":[");
    for (int i = 0; i < n; i++) {
        vjo_buf_puts(&b, i ? ",{" TARGET ",\"fields\":{\"Expression\":" : "{" TARGET ",\"fields\":{\"Expression\":");
        vjo_json_write_string(&b, words[i]);
        vjo_buf_puts(&b, "}" OPTIONS "}");
    }
    vjo_buf_puts(&b, "]}}");
    return b.oom ? NULL : vjo_buf_cstr(&b);
}

char *vjo_anki_can_add_request(VjoArena *a, const VjoEntryList *l, int n)
{
    const char **words;
    if (!l)
        return NULL;
    if (n > l->n_entries)
        n = l->n_entries;
    words = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(n > 0 ? n : 1));
    if (!words)
        return NULL;
    for (int i = 0; i < n; i++)
        words[i] = l->entries[i].vocab->spelling;
    return can_add_request(a, words, n);
}

static int is_media(int field)
{
    return field == VJO_KIKU_PICTURE || field == VJO_KIKU_EXPRESSION_AUDIO;
}

char *vjo_anki_field_value(VjoArena *a, int field, const VjoAnkiNote *n)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (field) {
    case VJO_KIKU_EXPRESSION: html_escape(&b, n->spelling, strlen(n->spelling)); break;
    case VJO_KIKU_EXPRESSION_READING: html_escape(&b, n->reading, strlen(n->reading)); break;
    case VJO_KIKU_EXPRESSION_FURIGANA:
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
    case VJO_KIKU_MAIN_DEFINITION:
    case VJO_KIKU_GLOSSARY:
        /* Glossary: every dictionary's entry, the shown one first (Kiku
         * leaves out the one MainDefinition has); none for a single one. */
        if (!n->n_meanings || (field == VJO_KIKU_GLOSSARY && !n->n_more))
            break;
        vjo_buf_puts(&b, GLOSSARY_OPEN);
        gloss_item(&b, n->dictionary, n->meanings, n->n_meanings);
        for (int k = 0; field == VJO_KIKU_GLOSSARY && k < n->n_more; k++)
            gloss_item(&b, n->more[k].dictionary, n->more[k].meanings, n->more[k].n_meanings);
        vjo_buf_puts(&b, GLOSSARY_CLOSE);
        break;
    case VJO_KIKU_SENTENCE: vjo_anki_sentence(&b, n->sentence, n->hl_start, n->hl_end); break;
    case VJO_KIKU_FREQ_SORT:
        if (n->rank != VJO_NO_RANK)
            vjo_buf_printf(&b, "%d", n->rank);
        break;
    case VJO_KIKU_SENTENCE_FURIGANA:
        if (n->sentence_furigana)
            vjo_anki_sentence(&b, n->sentence_furigana, n->sf_hl_start, n->sf_hl_end);
        break;
    case VJO_KIKU_PITCH_POSITION: vjo_anki_pitch_position(&b, n->pitch, n->n_pitch); break;
    case VJO_KIKU_PITCH_CATEGORIES:
        vjo_anki_pitch_categories(&b, n->reading, n->pitch, n->n_pitch, n->pos, n->n_pos);
        break;
    case VJO_KIKU_FREQUENCY:
        if (n->rank != VJO_NO_RANK && n->source) {
            vjo_buf_puts(&b, "<ul style=\"text-align: left;\"><li>");
            html_escape(&b, n->source, strlen(n->source));
            vjo_buf_printf(&b, ": %d</li></ul>", n->rank);
        }
        break;
    case VJO_KIKU_MISC_INFO:
        if (n->misc)
            html_escape(&b, n->misc, strlen(n->misc));
        break;
    default: break;
    }
    return vjo_buf_cstr(&b);
}

/* "audio"/"picture":[{"filename":..,"fields":[field], then the source */
static void put_media(VjoBuf *b, const char *kind, const char *name, int field)
{
    vjo_buf_printf(b, ",\"%s\":[{\"filename\":", kind);
    vjo_json_write_string(b, name);
    vjo_buf_printf(b, ",\"fields\":[\"%s\"],", vjo_kiku_field[field]);
}

int vjo_anki_add_request(VjoArena *a, const VjoAnkiNote *n, const VjoAnkiMedia *m, VjoAnkiBody *out)
{
    const char *v[VJO_KIKU_FIELDS];
    int first = 1, audio = m->audio_url && m->audio_name, pic = m->picture_len && m->picture_name;
    VjoBuf b;
    memset(out, 0, sizeof(*out));
    /* Values first, so the request below grows in place. */
    for (int f = 0; f < VJO_KIKU_FIELDS; f++) {
        v[f] = NULL;
        if (!is_media(f) && !(v[f] = vjo_anki_field_value(a, f, n)))
            return -1;
        if (v[f] && !*v[f])
            v[f] = NULL; /* nothing to add (no rank, no meanings) */
    }
    /* A media field is sent empty: the rewritten AnkiConnect (2026) puts the
     * <img>/[sound:] only into fields the request names. */
    if (audio)
        v[VJO_KIKU_EXPRESSION_AUDIO] = "";
    if (pic)
        v[VJO_KIKU_PICTURE] = "";
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET ",\"fields\":{");
    for (int f = 0; f < VJO_KIKU_FIELDS; f++) {
        if (!v[f])
            continue;
        vjo_buf_printf(&b, first ? "\"%s\":" : ",\"%s\":", vjo_kiku_field[f]);
        first = 0;
        vjo_json_write_string(&b, v[f]);
    }
    vjo_buf_puts(&b, "},\"tags\":[\"" VJO_ANKI_TAG "\"");
    if (n->queue_tag) {
        vjo_buf_putc(&b, ',');
        vjo_json_write_string(&b, n->queue_tag);
    }
    vjo_buf_puts(&b, "]" OPTIONS);
    if (audio) {
        put_media(&b, "audio", m->audio_name, VJO_KIKU_EXPRESSION_AUDIO);
        vjo_buf_puts(&b, "\"url\":");
        vjo_json_write_string(&b, m->audio_url);
        vjo_buf_puts(&b, "}]");
    }
    if (pic) {
        put_media(&b, "picture", m->picture_name, VJO_KIKU_PICTURE);
        vjo_buf_puts(&b, "\"data\":\"");
        out->picture = m->picture;
        out->picture_len = m->picture_len;
        out->suffix = "\"}]}}}";
    } else {
        vjo_buf_puts(&b, "}}}");
        out->suffix = "";
    }
    if (b.oom)
        return -1;
    out->prefix = vjo_buf_cstr(&b);
    if (!out->prefix)
        return -1;
    out->prefix_len = b.len;
    out->suffix_len = strlen(out->suffix);
    return 0;
}

void vjo_anki_media_name(char *out, size_t cap, uint64_t unix_ms, const char *ext)
{
    vjo_snprintf(out, cap, "vitajp_%u%03u.%s", (unsigned)(unix_ms / 1000u), (unsigned)(unix_ms % 1000u), ext);
}

/* ---------------- word audio ---------------- */

#define AUDIO_CONNECT_TIMEOUT_US (3 * 1000 * 1000)
#define AUDIO_IO_TIMEOUT_US (5 * 1000 * 1000) /* a stalled source: not the default 20 s */
#define AUDIO_NAME_CAP 40

int vjo_anki_audio_enabled(const VjoConfig *cfg)
{
    return cfg->anki_audio_url[0] != '\0';
}

static void url_encode(VjoBuf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            vjo_buf_putc(b, (char)c);
        } else {
            vjo_buf_putc(b, '%');
            vjo_buf_putc(b, hex[c >> 4]);
            vjo_buf_putc(b, hex[c & 15]);
        }
    }
}

char *vjo_anki_audio_path(VjoArena *a, const char *path_template, const char *term, const char *reading)
{
    const char *t = path_template;
    VjoBuf b;
    if (!reading || !*reading)
        reading = term;
    vjo_buf_init(&b, a);
    if (*t != '/')
        vjo_buf_putc(&b, '/');
    while (*t) {
        if (!strncmp(t, "{term}", 6)) {
            url_encode(&b, term);
            t += 6;
        } else if (!strncmp(t, "{reading}", 9)) {
            url_encode(&b, reading);
            t += 9;
        } else if (!strncmp(t, "{language}", 10)) {
            vjo_buf_puts(&b, "ja");
            t += 10;
        } else {
            vjo_buf_putc(&b, *t++);
        }
    }
    return b.oom ? NULL : vjo_buf_cstr(&b);
}

static int is_http_url(const char *s)
{
    return starts_with(s, "https://") || starts_with(s, "http://");
}

/* {"type": "audioSourceList", "audioSources": [{"name": ..., "url": ...}, ...]} */
int vjo_anki_audio_parse(VjoArena *a, const char *json, size_t len, const char **url)
{
    VjoJson j;
    int list;
    *url = NULL;
    if (vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT))
        return VJO_E_PARSE;
    list = vjo_json_obj_get(&j, 0, "audioSources");
    if (!vjo_json_is_type(&j, list, JSMN_ARRAY))
        return VJO_E_PARSE;
    for (int k = 0; k < j.t[list].size; k++) {
        int u = vjo_json_obj_get(&j, vjo_json_arr_get(&j, list, k), "url");
        const char *s;
        if (!vjo_json_is_type(&j, u, JSMN_STRING))
            continue;
        if (!(s = vjo_json_str(a, &j, u)))
            return VJO_E_OOM;
        if (is_http_url(s)) {
            *url = s;
            break;
        }
    }
    return VJO_OK;
}

const char *vjo_anki_audio_ext(const char *url)
{
    static const char *const known[] = {"mp3", "ogg", "opus", "oga", "m4a", "aac", "wav", "flac", "webm"};
    const char *path = strstr(url, "://"), *ext = NULL;
    char e[8];
    size_t n = 0;
    path = path ? path + 3 : url;
    while (*path && *path != '/') /* past the host */
        path++;
    for (; *path && *path != '?' && *path != '#'; path++) {
        if (*path == '/')
            ext = NULL;
        else if (*path == '.')
            ext = path + 1;
    }
    if (!ext)
        return "mp3";
    while (ext + n < path && n < sizeof(e) - 1) {
        char c = ext[n];
        e[n++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    e[n] = '\0';
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (ext + n == path && !strcmp(e, known[i]))
            return known[i];
    return "mp3";
}

/* ---------------- replies ---------------- */

/* {"result": ..., "error": ...}: VJO_OK with the result token, or
 * VJO_E_ANKI_DUPLICATE / VJO_E_ANKI with the error in *error. */
static int parse_reply(VjoArena *a, const char *json, size_t len, VjoJson *j, int *result, const char **error)
{
    int r, e;
    *error = NULL;
    if (vjo_json_parse(a, json, len, j) < 0 || !vjo_json_is_type(j, 0, JSMN_OBJECT) ||
        vjo_json_skip(j, 0) != j->n)
        return VJO_E_PARSE;
    r = vjo_json_obj_get(j, 0, "result");
    e = vjo_json_obj_get(j, 0, "error");
    if (r < 0 || e < 0)
        return VJO_E_PARSE; /* not AnkiConnect */
    if (!(vjo_json_is_type(j, e, JSMN_PRIMITIVE) && j->t[e].end - j->t[e].start == 4 &&
          !memcmp(json + j->t[e].start, "null", 4))) {
        if (!vjo_json_is_type(j, e, JSMN_STRING))
            return VJO_E_PARSE;
        if (!(*error = vjo_json_str(a, j, e)))
            return VJO_E_OOM;
        return is_duplicate(*error) ? VJO_E_ANKI_DUPLICATE : VJO_E_ANKI;
    }
    *result = r;
    return VJO_OK;
}

/* canAddNotesWithErrorDetail: [{"canAdd": bool, "error"?: string}, ...];
 * marks[k] = a duplicate, reasons[k] (if not NULL) = the error, NULL if none. */
static int parse_can_add(VjoArena *a, const char *json, size_t len, uint8_t *marks, const char **reasons, int n,
                         const char **error)
{
    VjoJson j;
    int r, rc = parse_reply(a, json, len, &j, &r, error), i, k;
    if (rc)
        return rc;
    if (!vjo_json_is_type(&j, r, JSMN_ARRAY) || j.t[r].size != n)
        return VJO_E_PARSE;
    for (k = 0, i = r + 1; k < n; k++, i = vjo_json_skip(&j, i)) {
        int can_add, e = vjo_json_obj_get(&j, i, "error");
        if (vjo_json_bool(&j, vjo_json_obj_get(&j, i, "canAdd"), &can_add) < 0)
            return VJO_E_PARSE;
        const char *why = !can_add && vjo_json_is_type(&j, e, JSMN_STRING) ? vjo_json_str(a, &j, e) : NULL;
        marks[k] = is_duplicate(why);
        if (reasons)
            reasons[k] = why;
    }
    return VJO_OK;
}

/* ---------------- calls ---------------- */

typedef struct {
    const VjoAnkiBody *b;
    char *chunk; /* base64 of up to CHUNK_IN bytes */
} BodyWriter;

#define CHUNK_IN 3072 /* a multiple of 3: only the last chunk is padded */

static int write_anki_body(void *ud, VjoConn *c)
{
    BodyWriter *w = (BodyWriter *)ud;
    const VjoAnkiBody *b = w->b;
    int rc = vjo_conn_send_all(c, b->prefix, b->prefix_len);
    for (size_t off = 0; rc == VJO_OK && off < b->picture_len; off += CHUNK_IN) {
        size_t n = b->picture_len - off < CHUNK_IN ? b->picture_len - off : CHUNK_IN;
        vjo_base64_encode(b->picture + off, n, w->chunk);
        rc = vjo_conn_send_all(c, w->chunk, vjo_base64_len(n));
    }
    if (rc == VJO_OK && b->suffix_len)
        rc = vjo_conn_send_all(c, b->suffix, b->suffix_len);
    return rc;
}

/* POST / to AnkiConnect; the reply is in resp. */
static int anki_post(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiBody *body,
                     VjoHttpResponse *resp, VjoErr *err)
{
    VjoHttpRequest req;
    BodyWriter w;
    memset(err, 0, sizeof(*err));
    w.b = body;
    w.chunk = NULL;
    if (body->picture_len && !(w.chunk = (char *)vjo_arena_alloc(a, vjo_base64_len(CHUNK_IN))))
        return err->rc = VJO_E_OOM;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.host = host;
    req.path = "/";
    req.content_type = "application/json";
    req.body_len = body_len(body);
    req.write_body = write_anki_body;
    req.ud = &w;
    req.connect_timeout_us = CONNECT_TIMEOUT_US;
    return vjo_http_request(a, p, port, 0, &req, VJO_ANKI_MAX_RESPONSE, resp, err);
}

static void text_body(VjoAnkiBody *b, const char *json)
{
    memset(b, 0, sizeof(*b));
    b->prefix = json;
    b->prefix_len = strlen(json);
}

/* One action; its result token in *result. */
static int call(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiBody *body, VjoJson *j,
                int *result, VjoErr *err)
{
    VjoHttpResponse resp;
    const char *error;
    if (anki_post(a, p, host, port, body, &resp, err))
        return err->rc;
    err->rc = parse_reply(a, resp.body, resp.body_len, j, result, &error);
    err->detail = error;
    return err->rc;
}

int vjo_anki_call(VjoArena *a, const VjoPlatform *p, const char *host, int port, const char *json, VjoJson *j,
                  int *result, VjoErr *err)
{
    VjoAnkiBody b;
    text_body(&b, json);
    return call(a, p, host, port, &b, j, result, err);
}

int vjo_anki_probe(VjoArena *a, const VjoPlatform *p, const char *host, int port, VjoErr *err)
{
    VjoAnkiBody b;
    VjoJson j;
    int r;
    long v;
    size_t mark = vjo_arena_mark(a);
    text_body(&b, VERSION_REQUEST);
    if (call(a, p, host, port, &b, &j, &r, err) == VJO_OK && (vjo_json_int(&j, r, &v) < 0 || v != 6))
        err->rc = VJO_E_PARSE;
    vjo_arena_release(a, mark);
    return err->rc;
}

int vjo_anki_check(VjoArena *a, const VjoPlatform *p, const char *host, int port, const char *request,
                   uint8_t *marks, int n, VjoErr *err)
{
    VjoAnkiBody b;
    VjoHttpResponse resp;
    const char *error;
    text_body(&b, request);
    if (anki_post(a, p, host, port, &b, &resp, err))
        return err->rc;
    err->rc = parse_can_add(a, resp.body, resp.body_len, marks, NULL, n, &error);
    err->detail = error;
    return err->rc;
}

/* Note/deck IDs exceed a Vita long. Validate the decimal token without truncation. */
static int positive_id(const VjoJson *j, int t)
{
    int len;
    if (!vjo_json_is_type(j, t, JSMN_PRIMITIVE))
        return 0;
    len = j->t[t].end - j->t[t].start;
    if (len < 1 || len > 19 || j->js[j->t[t].start] < '1' || j->js[j->t[t].start] > '9')
        return 0;
    for (int i = j->t[t].start + 1; i < j->t[t].end; i++)
        if (j->js[i] < '0' || j->js[i] > '9')
            return 0;
    return 1;
}

static int call_id(VjoArena *a, const VjoPlatform *p, const char *host, int port,
                    const VjoAnkiBody *body, VjoErr *err)
{
    VjoJson j;
    int r;
    if (call(a, p, host, port, body, &j, &r, err) == VJO_OK && !positive_id(&j, r))
        err->rc = VJO_E_PARSE;
    return err->rc;
}

int vjo_anki_find_queued(VjoArena *a, const VjoPlatform *p, const char *host, int port,
                         const char *tag, int *found, VjoErr *err)
{
    VjoBuf b;
    VjoAnkiBody body;
    VjoJson j;
    int r;
    size_t mark = vjo_arena_mark(a);
    *found = 0;
    memset(err, 0, sizeof(*err));
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"action\":\"findNotes\",\"version\":6,\"params\":{\"query\":");
    /* Tags are generated locally from hex IDs; JSON escaping still applies. */
    {
        VjoBuf query;
        char *q;
        vjo_buf_init(&query, a);
        vjo_buf_puts(&query, "tag:");
        vjo_buf_puts(&query, tag);
        q = vjo_buf_cstr(&query);
        if (!q)
            return err->rc = VJO_E_OOM;
        vjo_json_write_string(&b, q);
    }
    vjo_buf_puts(&b, "}}");
    if (b.oom || !vjo_buf_cstr(&b))
        return err->rc = VJO_E_OOM;
    text_body(&body, (char *)b.data);
    if (call(a, p, host, port, &body, &j, &r, err))
        return err->rc;
    if (!vjo_json_is_type(&j, r, JSMN_ARRAY))
        return err->rc = VJO_E_PARSE;
    for (int i = 0, t = r + 1; i < j.t[r].size; i++, t = vjo_json_skip(&j, t))
        if (!positive_id(&j, t))
            return err->rc = VJO_E_PARSE;
    *found = j.t[r].size > 0;
    vjo_arena_release(a, mark);
    return VJO_OK;
}

/* The rewritten AnkiConnect refused the note without a reason: asks for
 * it, so a duplicate is VJO_E_ANKI_DUPLICATE (its ✓) and anything else
 * says what failed. Keeps err as it is when the question fails too. */
static int why_refused(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiNote *n,
                       VjoErr *err)
{
    VjoAnkiBody b;
    VjoHttpResponse resp;
    VjoErr e;
    uint8_t dup = 0;
    const char *reason = NULL, *error, *words[1] = {n->spelling};
    char *request = can_add_request(a, words, 1);
    if (!request)
        return err->rc;
    memset(&e, 0, sizeof(e));
    text_body(&b, request);
    if (anki_post(a, p, host, port, &b, &resp, &e) ||
        parse_can_add(a, resp.body, resp.body_len, &dup, &reason, 1, &error))
        return err->rc;
    if (dup)
        return err->rc = VJO_E_ANKI_DUPLICATE;
    if (reason)
        err->detail = reason;
    return err->rc;
}

int vjo_anki_add(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiNote *n,
                 const VjoAnkiMedia *m, VjoErr *err)
{
    VjoAnkiBody note, deck;
    memset(err, 0, sizeof(*err));
    if (vjo_anki_add_request(a, n, m, &note) < 0)
        return err->rc = VJO_E_OOM;
    if (call_id(a, p, host, port, &note, err) == VJO_E_ANKI && starts_with(err->detail, ERR_VALIDATION_V2))
        return why_refused(a, p, host, port, n, err);
    if (err->rc != VJO_E_ANKI || !is_no_deck(err->detail))
        return err->rc;
    /* The first note of a new deck. */
    text_body(&deck, CREATE_DECK_REQUEST);
    if (call_id(a, p, host, port, &deck, err))
        return err->rc;
    return call_id(a, p, host, port, &note, err);
}

static int audio_lookup(VjoArena *a, const VjoPlatform *p, const char *audio_url, const VjoAnkiNote *n,
                        const char **url, VjoErr *err)
{
    VjoHttpRequest req;
    VjoHttpResponse resp;
    const char *path;
    char host[VJO_HOST_MAX];
    int port, tls;
    if (vjo_anki_audio_endpoint(audio_url, host, &port, &tls, &path) < 0)
        return err->rc = VJO_E_PARSE;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.host = host;
    req.connect_timeout_us = AUDIO_CONNECT_TIMEOUT_US;
    req.io_timeout_us = AUDIO_IO_TIMEOUT_US;
    if (!(req.path = vjo_anki_audio_path(a, path, n->spelling, n->reading)))
        return err->rc = VJO_E_OOM;
    if (vjo_http_request(a, p, port, tls, &req, VJO_ANKI_AUDIO_MAX_RESPONSE, &resp, err))
        return err->rc;
    return err->rc = vjo_anki_audio_parse(a, resp.body, resp.body_len, url);
}

int vjo_anki_find_audio(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const VjoAnkiNote *n,
                        uint64_t unix_ms, VjoAnkiMedia *m, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a), len;
    const char *url = NULL;
    char *keep;
    memset(err, 0, sizeof(*err));
    m->audio_url = m->audio_name = NULL;
    if (audio_lookup(a, p, cfg->anki_audio_url, n, &url, err) || !url) {
        vjo_arena_release(a, mark);
        return err->rc;
    }
    /* Keep just the URL: the reply and its tokens go back to the arena.
     * The copy lands at or below the URL, so memmove. */
    len = strlen(url);
    vjo_arena_release(a, mark);
    if (!(keep = (char *)vjo_arena_alloc(a, len + 1 + AUDIO_NAME_CAP)))
        return err->rc = VJO_E_OOM;
    memmove(keep, url, len + 1);
    vjo_anki_media_name(keep + len + 1, AUDIO_NAME_CAP, unix_ms, vjo_anki_audio_ext(keep));
    m->audio_url = keep;
    m->audio_name = keep + len + 1;
    return VJO_OK;
}

const char *vjo_anki_audio_err_text(VjoArena *a, const VjoErr *err)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK: return "no recording";
    case VJO_E_NET: return "audio source offline";
    case VJO_E_OOM: return "not enough memory";
    case VJO_E_TOO_LARGE: return "reply too large";
    case VJO_E_PARSE: return "not an audio source list";
    case VJO_E_HTTP: return "bad HTTP reply, or the word is too long";
    case VJO_E_STATUS: vjo_buf_printf(&b, "HTTP %d", err->http_status); break;
    case VJO_E_TLS: vjo_buf_printf(&b, "TLS error %d", err->tls_error); break;
    default: vjo_buf_printf(&b, "unexpected reply %d", err->rc); break;
    }
    return vjo_buf_cstr(&b);
}

const char *vjo_anki_err_text(VjoArena *a, const VjoErr *err)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK:
        return "";
    case VJO_E_NET:
        return "Anki offline";
    case VJO_E_NOT_FOUND:
        return "Anki not found: test the connection in the Overlay app";
    case VJO_E_OOM:
        return "Anki: not enough memory";
    case VJO_E_ANKI_DUPLICATE:
        return "Already in Anki";
    case VJO_E_STATUS:
        vjo_buf_printf(&b, "Anki refused the request (HTTP %d)", err->http_status);
        break;
    case VJO_E_ANKI:
        if (starts_with(err->detail, ERR_NO_MODEL) || starts_with(err->detail, ERR_NO_MODEL_V2))
            return "Kiku note type not found in Anki: import Kiku (docs/anki.md)";
        vjo_buf_puts(&b, "Anki: ");
        vjo_buf_puts(&b, err->detail ? err->detail : "error");
        break;
    default:
        vjo_buf_printf(&b, "Anki: unexpected response (%d). Is AnkiConnect running?", err->rc);
        break;
    }
    return vjo_buf_cstr(&b);
}

int vjo_anki_host_stale(int rc)
{
    return rc != VJO_OK && rc != VJO_E_ANKI && rc != VJO_E_ANKI_DUPLICATE && rc != VJO_E_OOM;
}

/* ---------------- network search ---------------- */

int vjo_anki_scan_candidates(uint32_t ip, uint32_t mask, uint32_t *out, int cap)
{
    uint32_t m = mask | 0xFFFFFF00u, net, bcast; /* at most a /24 */
    int n = 0;
    if (~m & (~m + 1)) /* not a contiguous mask */
        m = 0xFFFFFF00u;
    net = ip & m;
    bcast = net | ~m;
    for (uint32_t h = net + 1; h < bcast && n < cap; h++)
        if (h != ip)
            out[n++] = h;
    return n;
}
