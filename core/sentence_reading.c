#include "sentence_reading.h"

#include <string.h>

#include "port.h"
#include "textjoin.h"
#include "utf.h"

#define WORD_MAX 64 /* code points of a word, its dictionary form or its reading: a longer one gets none */
#define WORD_BYTES (WORD_MAX * 4 + 1)
#define FIT_STEPS 4096 /* tries of a reading's fit before it counts as undecided (as core/anki.c's furigana) */
#define WORD_RUBIES 32 /* readings of one word (its kanji runs) */

static const VjoReadingRules default_rules = {VJO_READINGS_RATIO, VJO_READINGS_PART_RATIO, 1, 1, 1, 1};

/* ---------------- a word's reading on its text ---------------- */

typedef struct {
    uint32_t c[WORD_MAX];
    uint32_t off[WORD_MAX + 1]; /* byte offsets */
    int n;
} Cps;

static int split(const char *s, size_t len, Cps *o)
{
    size_t i = 0;
    o->n = 0;
    while (i < len) {
        if (o->n == WORD_MAX)
            return -1;
        o->off[o->n] = (uint32_t)i;
        o->c[o->n++] = vjo_utf8_next(s, len, &i);
    }
    o->off[o->n] = (uint32_t)len;
    return 0;
}

/* Katakana as hiragana, for comparing kana (ヵヶ, U+30F5 and U+30F6, are kanji here: vjo_is_kanji). */
static uint32_t fold(uint32_t c)
{
    return c >= 0x30A1 && c <= 0x30F4 ? c - 0x60 : c;
}

static int vowel(uint32_t c)
{
    c = fold(c);
    return c == 0x3042 || c == 0x3044 || c == 0x3046 || c == 0x3048 || c == 0x304A || /* あいうえお */
           c == 0x3041 || c == 0x3043 || c == 0x3045 || c == 0x3047 || c == 0x3049;   /* ぁぃぅぇぉ */
}

/* A word's kana against its reading's: katakana as hiragana, ー as the vowel it lengthens (ゲーム, げえむ). */
static int same_kana(uint32_t a, uint32_t b)
{
    a = fold(a);
    b = fold(b);
    return a == b || (a == 0x30FC && vowel(b)) || (b == 0x30FC && vowel(a));
}

/* A reading: kana only (a kanji character's article has its spelling for one: no reading). */
static int kana_only(const Cps *r)
{
    if (!r->n)
        return 0;
    for (int i = 0; i < r->n; i++) {
        uint32_t c = fold(r->c[i]);
        if (!(c >= 0x3041 && c <= 0x3096) && c != 0x30FC && c != 0x309D && c != 0x309E)
            return 0;
    }
    return 1;
}

static int has_kanji(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n)
        if (vjo_is_kanji(vjo_utf8_next(s, n, &i)))
            return 1;
    return 0;
}

/* The ways the dictionary form w fits its reading r from w[i] and r[j], counted in found (up to 2), the first
 * one's runs in best: kana must read as itself; a run of kanji takes at least one character of the reading a
 * kanji (as Yomitan's segmentizeFurigana). Two ways (物の怪, もののけ: も|のけ or もの|け) or too many tries
 * leave it undecided. */
typedef struct {
    const Cps *w, *r;
    int steps, found;
    int end[WORD_MAX];  /* a kanji run starting at i: where its reading ends in r */
    int best[WORD_MAX]; /* the same, of the first way it fits */
} Fit;

static void fit(Fit *f, int i, int j)
{
    int e = i;
    if (f->found > 1)
        return;
    if (++f->steps > FIT_STEPS) {
        f->found = 2;
        return;
    }
    if (i == f->w->n) {
        if (j == f->r->n && !f->found++)
            memcpy(f->best, f->end, sizeof(f->end));
        return;
    }
    if (!vjo_is_kanji(f->w->c[i])) {
        if (j < f->r->n && same_kana(f->w->c[i], f->r->c[j]))
            fit(f, i + 1, j + 1);
        return;
    }
    while (e < f->w->n && vjo_is_kanji(f->w->c[e]))
        e++;
    for (int k = j + (e - i); k <= f->r->n && f->found < 2; k++) {
        f->end[i] = k;
        fit(f, e, k);
    }
}

/* The readings of the kanji of the text as written (s, sb's bytes) from its dictionary form w read r (Yomitan's
 * distributeFuriganaInflected): the text is written as w up to where its inflection starts (the stem); each of
 * its kanji runs is a run of w there and reads as it, conjugated (来[こ]ない). A dictionary form whose reading
 * fits it in two ways reads as a whole, only uninflected (物の怪[もののけ]). The count, or -1 when a kanji run
 * of the text gets no reading (out then holds nothing usable). Readings point into rb or are static. */
/* 何 read なに as a word of its own: なん before the sounds of た, だ and な rows that follow it in speech (何だ,
 * 何で, 何の, 何と, 何て, 何なの), なに before anything else (何が, 何を, 何も); before a kanji (a counter: 何人,
 * 何回) it is not decided. 1 なん, 0 なに, -1 undecided. */
static int reads_nan(const char *next, size_t n)
{
    static const char *const before_nan[] = {"た", "て", "と", "だ", "で", "ど", "な", "の"};
    size_t i = 0;
    if (!n)
        return 0;
    if (vjo_is_kanji(vjo_utf8_next(next, n, &i)))
        return -1;
    for (unsigned k = 0; k < sizeof(before_nan) / sizeof(before_nan[0]); k++)
        if (vjo_starts_withn(next, n, before_nan[k]))
            return 1;
    return 0;
}

static int place(const char *sb, const Cps *s, const Cps *w, const char *rb, const Cps *r, const char *after,
                 size_t after_len, VjoRuby *out, int cap)
{
    Fit f;
    int stem = 0, n = 0, rs[WORD_MAX];
    memset(&f, 0, sizeof(f));
    f.w = w;
    f.r = r;
    fit(&f, 0, 0);
    while (stem < s->n && stem < w->n && fold(s->c[stem]) == fold(w->c[stem]))
        stem++;
    if (f.found != 1) {
        if (stem != s->n || stem != w->n || !cap)
            return -1;
        out[0].start = s->off[0];
        out[0].end = s->off[s->n];
        out[0].reading = rb;
        out[0].reading_len = r->off[r->n];
        return 1;
    }
    for (int i = 0, j = 0; i < w->n;) {
        if (!vjo_is_kanji(w->c[i])) {
            i++;
            j++;
            continue;
        }
        rs[i] = j;
        j = f.best[i];
        while (i < w->n && vjo_is_kanji(w->c[i]))
            i++;
    }
    for (int i = 0; i < s->n;) {
        int e = i, we = i;
        const char *reading, *conj, *nb;
        size_t len, next, nn;
        if (!vjo_is_kanji(s->c[i])) {
            i++;
            continue;
        }
        while (e < s->n && vjo_is_kanji(s->c[e]))
            e++;
        while (we < w->n && vjo_is_kanji(w->c[we]))
            we++;
        /* (before the stem's end the two are the same characters, so w's run starts at i too) */
        if (e > stem || we != e || n == cap)
            return -1;
        reading = rb + r->off[rs[i]];
        len = r->off[f.best[i]] - r->off[rs[i]];
        next = s->off[e];
        /* what follows the run: the word's own kana, else the text after the word */
        nb = e < s->n ? sb + next : after;
        nn = e < s->n ? s->off[s->n] - next : after_len;
        conj = vjo_furigana_conjugated(sb + s->off[i], s->off[e] - s->off[i], reading, len, nb, nn);
        if (!conj && s->off[e] - s->off[i] == 3 && !memcmp(sb + s->off[i], "\xE4\xBD\x95", 3) /* 何 */ && len == 6 &&
            !memcmp(reading, "\xE3\x81\xAA\xE3\x81\xAB", 6) /* なに */) {
            int k = reads_nan(nb, nn);
            if (k < 0)
                return -1;
            if (k)
                conj = "\xE3\x81\xAA\xE3\x82\x93"; /* なん */
        }
        out[n].start = s->off[i];
        out[n].end = s->off[e];
        out[n].reading = conj ? conj : reading;
        out[n].reading_len = conj ? strlen(conj) : len;
        n++;
        i = e;
    }
    return n;
}

/* The readings of text (n bytes) as the word spelling read reading: -1 when they do not fit (or a part is
 * longer than WORD_MAX), else their count (0: no kanji). */
static int word_rubies(const char *text, size_t n, const char *after, size_t after_len, const char *spelling,
                       const char *reading, VjoRuby *out, int cap)
{
    Cps s, w, r;
    if (!spelling || !reading || !has_kanji(text, n))
        return 0;
    if (split(text, n, &s) || split(spelling, strlen(spelling), &w) || split(reading, strlen(reading), &r) ||
        !kana_only(&r))
        return -1;
    return place(text, &s, &w, reading, &r, after, after_len, out, cap);
}

int vjo_word_rubies(const char *surface, size_t n, const char *spelling, const char *reading, VjoRuby *out, int cap)
{
    int k = word_rubies(surface, n, "", 0, spelling, reading, out, cap);
    return k < 0 ? 0 : k;
}

/* Two words' readings of the same text read it the same: the same kanji runs, the same kana. */
static int same_rubies(const VjoRuby *a, int na, const VjoRuby *b, int nb)
{
    if (na != nb)
        return 0;
    for (int k = 0; k < na; k++) {
        size_t i = 0, j = 0;
        if (a[k].start != b[k].start || a[k].end != b[k].end)
            return 0;
        while (i < a[k].reading_len && j < b[k].reading_len)
            if (fold(vjo_utf8_next(a[k].reading, a[k].reading_len, &i)) !=
                fold(vjo_utf8_next(b[k].reading, b[k].reading_len, &j)))
                return 0;
        if (i < a[k].reading_len || j < b[k].reading_len)
            return 0;
    }
    return 1;
}

/* A spoken shortening of a reading: the reading less some of its う, ー or っ (ほんとに of ほんとうに, だいじょぶ
 * of だいじょうぶ), which the word's kanji spelling does not write (it is written in kana). */
static int shortening(const char *full, const char *short_)
{
    size_t nf = strlen(full), ns = strlen(short_), i = 0, j = 0;
    if (ns >= nf)
        return 0;
    while (i < nf) {
        uint32_t c = fold(vjo_utf8_next(full, nf, &i));
        size_t k = j;
        if (j < ns && fold(vjo_utf8_next(short_, ns, &k)) == c)
            j = k;
        else if (c != 0x3046 && c != 0x30FC && c != 0x3063) /* う ー っ */
            return 0;
    }
    return j == ns;
}

/* ---------------- the sentence ---------------- */

/* core/local_dict.c's boundary(): a lookup never matches across one, so a stretch between two without a kanji
 * is never looked up, and the parse after one is the same whatever came before it. */
static int boundary(uint32_t cp)
{
    return cp <= 0x20 || cp == 0x3000 || cp == 0x3001 || cp == 0x3002 || (cp >= 0x3008 && cp <= 0x3011) ||
           cp == 0xff01 || cp == 0xff1f || cp == '!' || cp == '?' || cp == ',' || cp == '.';
}

/* A digit, a full-width one or a kanji numeral: a number, which changes a counter's reading after it (三本,
 * 一回) and its own (一回: いっ). */
static int numeral(uint32_t c)
{
    static const uint16_t kanji[] = {0x3007, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D,
                                     0x5341, 0x767E, 0x5343, 0x4E07, 0x5104, 0x5146, 0x96F6}; /* 〇一二三四五六七八九十百千万億兆零 */
    if ((c >= '0' && c <= '9') || (c >= 0xFF10 && c <= 0xFF19))
        return 1;
    for (unsigned k = 0; k < sizeof(kanji) / sizeof(kanji[0]); k++)
        if (c == kanji[k])
            return 1;
    return 0;
}

static int all_numerals(const char *s, size_t n)
{
    size_t i = 0;
    if (!n)
        return 0;
    while (i < n)
        if (!numeral(vjo_utf8_next(s, n, &i)))
            return 0;
    return 1;
}

static uint32_t first_char(const char *s, size_t at, size_t len)
{
    return at < len ? vjo_utf8_next(s, len, &at) : 0;
}

/* The character that ends s[0, at) (at > 0, a character boundary). */
static uint32_t char_before(const char *s, size_t at)
{
    size_t i = at - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    return vjo_utf8_next(s, at, &i);
}

/* Where the run of kanji that ends text[from, end) starts, when it starts after from; 0 otherwise. */
static size_t kanji_run_start(const char *s, size_t from, size_t end)
{
    size_t i = from, start = 0;
    while (i < end) {
        size_t at = i;
        if (vjo_is_kanji(vjo_utf8_next(s, end, &i))) {
            if (!start)
                start = at;
        } else {
            start = 0;
        }
    }
    return start > from ? start : 0;
}

typedef struct {
    VjoArena *keep, *scratch;
    const VjoPlatform *p;
    VjoConfig *cfg; /* the caller's, local_dictionary_results 1 */
    const VjoReadingRules *rules;
    VjoReadingStats *st;
} Parse;

/* The overlay's entry for a word: the lookup at the cursor (page 0, its span and conjugation). */
typedef struct {
    char spelling[WORD_BYTES], reading[WORD_BYTES];
    char dictionary[VJO_LOCAL_TITLE_SIZE]; /* its file's title, "" without one */
    int rank, ok; /* ok: both fit WORD_BYTES */
    uint32_t conjugation;
    size_t end; /* the span's end in the text */
} Entry;

static int copy_word(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= WORD_BYTES)
        return 0;
    memcpy(dst, src ? src : "", n + 1);
    return 1;
}

/* 1 and the entry, 0 with no match at cursor, < 0 on failure. */
static int entry_at(Parse *ps, const char *text, size_t cursor, Entry *e)
{
    size_t mark = vjo_arena_mark(ps->scratch), len = strlen(text), at = cursor;
    VjoDictResult res;
    VjoErr err;
    int rc, units = 0;
    memset(&err, 0, sizeof(err));
    ps->st->lookups++;
    rc = vjo_local_lookup_at(ps->scratch, ps->p, ps->cfg, text, cursor, &res, &err);
    if (rc) {
        vjo_arena_release(ps->scratch, mark);
        return rc;
    }
    if (!res.n_vocab || !res.n_tokens || res.tokens[0].len16 <= 0) {
        vjo_arena_release(ps->scratch, mark);
        return 0;
    }
    while (at < len && units < res.tokens[0].len16)
        units += vjo_utf16_units(vjo_utf8_next(text, len, &at));
    e->end = at;
    e->rank = res.vocab[res.tokens[0].vocab].rank;
    e->conjugation = res.tokens[0].conjugation;
    e->ok = copy_word(e->spelling, res.vocab[res.tokens[0].vocab].spelling) &&
            copy_word(e->reading, res.vocab[res.tokens[0].vocab].reading);
    vjo_snprintf(e->dictionary, sizeof(e->dictionary), "%s",
                 res.vocab[res.tokens[0].vocab].dictionary ? res.vocab[res.tokens[0].vocab].dictionary : "");
    vjo_arena_release(ps->scratch, mark);
    return 1;
}

/* An entry is among the top ones beside the chosen one (rank best): Jiten's rank within ratio of it, or the
 * chosen one has none (an unranked entry beside a ranked one is not). */
static int as_frequent(int rank, int best, int ratio)
{
    if (!best || !ratio)
        return 1;
    return rank && (long long)rank <= (long long)ratio * best;
}

/* Does an entry for the whole of text[0, n), as frequent as one ranked best, read its kanji otherwise than
 * mine (nm of them)? 1 yes, 0 no, VJO_E_TOO_LARGE when they cannot all be read, < 0 on failure. With best < 0:
 * the most frequent entries of the text, against mine, which must be rarer than them by part_ratio. */
static int others_disagree(Parse *ps, const char *text, size_t n, const char *after, size_t after_len,
                           const Entry *mine, const VjoRuby *rm, int nm, int best, int ratio)
{
    size_t mark = vjo_arena_mark(ps->scratch), units = 0, i = 0;
    char *copy = vjo_arena_strndup(ps->scratch, text, n);
    VjoDictResult res;
    VjoErr err;
    int rc, verdict = 0, top = 0;
    if (!copy)
        return VJO_E_OOM;
    while (i < n)
        units += (size_t)vjo_utf16_units(vjo_utf8_next(text, n, &i));
    memset(&err, 0, sizeof(err));
    ps->st->lookups++;
    rc = vjo_local_lookup(ps->scratch, ps->p, ps->cfg, copy, &res, &err);
    if (rc) {
        vjo_arena_release(ps->scratch, mark);
        return rc;
    }
    if (best < 0) { /* the part's most frequent entry, ranked */
        for (int t = 0; t < res.n_tokens; t++) {
            int rank = res.vocab[res.tokens[t].vocab].rank;
            if (res.tokens[t].pos16 == 0 && (size_t)res.tokens[t].len16 == units && rank && (!top || rank < top))
                top = rank;
        }
        if (!top || (mine->rank && (long long)mine->rank <= (long long)ps->rules->part_ratio * top)) {
            vjo_arena_release(ps->scratch, mark);
            return 0;
        }
        best = top;
    }
    for (int t = 0; t < res.n_tokens && !verdict; t++) {
        const VjoVocab *v = &res.vocab[res.tokens[t].vocab];
        VjoRuby ro[WORD_RUBIES];
        int k;
        /* (another dictionary's unranked entry, a character's name in VNDB's, is as frequent as any) */
        if (res.tokens[t].pos16 != 0 || (size_t)res.tokens[t].len16 != units ||
            !(as_frequent(v->rank, best, ratio) ||
              (!v->rank && strcmp(v->dictionary ? v->dictionary : "", mine->dictionary))) ||
            (!strcmp(v->spelling, mine->spelling) && !strcmp(v->reading, mine->reading)) ||
            (ps->rules->variants && shortening(mine->reading, v->reading)))
            continue;
        k = word_rubies(text, n, after, after_len, v->spelling, v->reading, ro, WORD_RUBIES);
        if (k > 0 && !same_rubies(rm, nm, ro, k))
            verdict = 1;
    }
    vjo_arena_release(ps->scratch, mark);
    return verdict;
}

/* A whole entry ending in kana after its last kanji (今日は): the part up to its last kanji (今日), or 0 when
 * it ends otherwise. */
static size_t kanji_part(const char *s, size_t n)
{
    size_t i = 0, last = 0;
    int kana_tail = 0;
    while (i < n) {
        uint32_t c = vjo_utf8_next(s, n, &i);
        if (vjo_is_kanji(c)) {
            last = i;
            kana_tail = 1;
        } else if (!(c >= 0x3041 && c <= 0x3096)) {
            kana_tail = 0; /* anything but hiragana after it (katakana, a letter) */
        }
    }
    return last && last < n && kana_tail ? last : 0;
}

/* One matched word, text[0, n) (with kanji), the overlay's entry e: its readings into out (at most cap),
 * readings copied into keep. Returns how many (0: none, counted in st), or < 0 on failure. */
static int word(Parse *ps, const char *text, size_t n, const char *after, size_t after_len, int bound,
                const Entry *e, VjoRuby *out, int cap)
{
    VjoRuby r[WORD_RUBIES];
    int k, rc;
    size_t part;
    ps->st->words++;
    if (!e->ok || (k = word_rubies(text, n, after, after_len, e->spelling, e->reading, r, WORD_RUBIES)) <= 0) {
        ps->st->unplaced++;
        return 0;
    }
    /* the top entries disagree: another as frequent reads the kanji otherwise; in a compound the lookup split
     * (the word goes on a run of kanji or katakana: 授業中, 春原君) its own readings as a word do not count, so
     * every entry does (中: なか, ちゅう, じゅう) */
    rc = others_disagree(ps, text, n, after, after_len, e, r, k, e->rank, bound && ps->rules->compounds ? 0 : ps->rules->ratio);
    /* a whole entry ending in kana whose kanji part, far more frequent, reads otherwise (今日は, 今日) */
    if (!rc && ps->rules->part_ratio > 0 && !e->conjugation && (part = kanji_part(text, n)) > 0) {
        int kp = 0;
        while (kp < k && r[kp].end <= part)
            kp++;
        if (kp == k || r[kp].start >= part) {
            rc = others_disagree(ps, text, part, text + part, n - part, e, r, kp, -1, ps->rules->ratio);
            if (rc == 1) {
                ps->st->part++;
                return 0;
            }
        }
    }
    if (rc == VJO_E_TOO_LARGE) {
        ps->st->unchecked++;
        return 0;
    }
    if (rc < 0)
        return rc;
    if (rc) {
        ps->st->ambiguous++;
        return 0;
    }
    if (k > cap)
        return VJO_E_TOO_LARGE;
    for (int i = 0; i < k; i++) {
        out[i] = r[i];
        if (!(out[i].reading = vjo_arena_strndup(ps->keep, r[i].reading, r[i].reading_len)))
            return VJO_E_OOM;
    }
    ps->st->read++;
    return k;
}

int vjo_sentence_rubies(VjoArena *keep, VjoArena *scratch, const VjoPlatform *p, const VjoConfig *cfg,
                        const VjoReadingRules *rules, const char *text, size_t from, size_t to, VjoRuby *out,
                        int cap, VjoReadingStats *st)
{
    size_t base = vjo_arena_mark(scratch), keep_base = vjo_arena_mark(keep), pos = from;
    VjoReadingStats unused;
    Parse ps;
    char *copy;
    int n = 0, rc = 0;
    /* a run of numbers just before: where it ends (0: none), its first reading and its words with one */
    size_t number_end = 0;
    int number_first = 0;
    unsigned number_read = 0;
    memset(&unused, 0, sizeof(unused));
    ps.keep = keep;
    ps.scratch = scratch;
    ps.p = p;
    ps.rules = rules ? rules : &default_rules;
    ps.st = st ? st : &unused;
    if (from > to || to > strlen(text))
        return VJO_E_PARSE;
    /* the lookups see nothing after `to`; the session (p->local_session) is the caller's */
    copy = vjo_arena_strndup(scratch, text, to);
    ps.cfg = (VjoConfig *)vjo_arena_alloc(scratch, sizeof(*ps.cfg));
    if (!copy || !ps.cfg) {
        vjo_arena_release(scratch, base);
        return VJO_E_OOM;
    }
    memcpy(ps.cfg, cfg, sizeof(*ps.cfg));
    ps.cfg->local_dictionary_results = 1;
    while (pos < to && rc >= 0) {
        size_t seg = pos, i;
        int kanji = 0;
        uint32_t cp;
        while (seg < to) {
            i = seg;
            cp = vjo_utf8_next(copy, to, &i);
            if (boundary(cp))
                break;
            kanji |= vjo_is_kanji(cp);
            seg = i;
        }
        if (seg == pos) { /* a boundary */
            vjo_utf8_next(copy, to, &pos);
            number_end = 0;
            continue;
        }
        if (!kanji) { /* nothing here takes a reading */
            pos = seg;
            number_end = 0;
            continue;
        }
        while (pos < seg && rc >= 0) {
            Entry e;
            size_t end;
            int got = entry_at(&ps, copy, pos, &e), before = n, k = 0;
            if (got < 0) {
                ps.st->failed++;
                rc = got;
                break;
            }
            if (got) {
                end = e.end > seg ? seg : e.end;
                /* A match from kana that ends inside a run of kanji (と一 of と一緒に: ト一) splits a word the
                 * lookup at the run's start may hold whole (一緒に): that one, if it reaches further. */
                if (ps.rules->anchor && end < seg && !vjo_is_kanji(first_char(copy, pos, to))) {
                    size_t run = kanji_run_start(copy, pos, end);
                    if (run && vjo_is_kanji(first_char(copy, end, to))) {
                        Entry a;
                        int got2 = entry_at(&ps, copy, run, &a);
                        if (got2 < 0) {
                            ps.st->failed++;
                            rc = got2;
                            break;
                        }
                        if (got2 && a.end > end) {
                            pos = run; /* the kana before it: no reading */
                            e = a;
                            end = a.end > seg ? seg : a.end;
                            ps.st->anchored++;
                        }
                    }
                }
            } else {
                end = pos;
                cp = vjo_utf8_next(copy, to, &end);
                ps.st->unmatched += (unsigned)vjo_is_kanji(cp);
            }
            if (got && end > pos && has_kanji(copy + pos, end - pos)) {
                uint32_t first = first_char(copy, pos, to), prev = pos > from ? char_before(copy, pos) : 0;
                int bound = vjo_is_kanji(first) && (vjo_is_kanji(prev) || (prev >= 0x30A1 && prev <= 0x30FC));
                k = word(&ps, copy + pos, end - pos, copy + end, to - end, bound, &e, out + n, cap - n);
                if (k < 0) {
                    if (k == VJO_E_TOO_LARGE && n == cap) {
                        k = 0; /* no room for more: the rest stays without */
                    } else {
                        ps.st->failed++;
                        rc = k;
                        break;
                    }
                }
                for (int j = 0; j < k; j++)
                    out[n + j].start += pos, out[n + j].end += pos;
                n += k;
            }
            /* a number's counter, and the number: no reading (三本: さんぼん, 一回: いっかい) */
            if (all_numerals(copy + pos, end - pos)) {
                if (number_end != pos) {
                    number_first = before;
                    number_read = 0;
                }
                number_read += (unsigned)(k > 0);
                number_end = end;
            } else {
                size_t q = pos;
                if (ps.rules->counters && number_end && number_end == pos &&
                    vjo_is_kanji(vjo_utf8_next(copy, end, &q))) {
                    unsigned dropped = number_read + (unsigned)(k > 0);
                    ps.st->read -= dropped;
                    ps.st->counters += dropped;
                    n = number_first;
                }
                number_end = 0;
            }
            pos = end;
        }
    }
    vjo_arena_release(scratch, base);
    if (rc < 0) {
        vjo_arena_release(keep, keep_base);
        return rc;
    }
    return n;
}

/* ---------------- the field ---------------- */

/* Text as HTML, its '[' and ']' as entities (Anki's furigana filter then reads only the readings written here),
 * a line break as <br> (a CR dropped). */
static void html_text(VjoBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': vjo_buf_puts(b, "&amp;"); break;
        case '<': vjo_buf_puts(b, "&lt;"); break;
        case '>': vjo_buf_puts(b, "&gt;"); break;
        case '"': vjo_buf_puts(b, "&quot;"); break;
        case '[': vjo_buf_puts(b, "&#91;"); break;
        case ']': vjo_buf_puts(b, "&#93;"); break;
        case '\n': vjo_buf_puts(b, "<br>"); break;
        case '\r': break;
        default: vjo_buf_putc(b, s[i]); break;
        }
    }
}

void vjo_furigana_html(VjoBuf *b, const char *t, size_t len, const VjoRuby *r, int nr, int hl_start, int hl_end)
{
    int hl = hl_start >= 0 && hl_end > hl_start && (size_t)hl_end <= len, k = 0;
    size_t i = 0;
    for (;;) {
        if (hl && i == (size_t)hl_end)
            vjo_buf_puts(b, "</b>");
        if (hl && i == (size_t)hl_start)
            vjo_buf_puts(b, "<b>");
        if (i >= len)
            break;
        while (k < nr && r[k].start < i)
            k++;
        if (k < nr && r[k].start == i && r[k].end > i && r[k].end <= len && r[k].reading_len &&
            !(hl && i < (size_t)hl_start && r[k].end > (size_t)hl_start) &&
            !(hl && i < (size_t)hl_end && r[k].end > (size_t)hl_end) && !memchr(t + i, '\n', r[k].end - i)) {
            if (b->len && b->data[b->len - 1] != '>')
                vjo_buf_putc(b, ' '); /* (a space of the text's own before it: Anki's filter takes only this one) */
            html_text(b, t + i, r[k].end - i);
            vjo_buf_putc(b, '[');
            html_text(b, r[k].reading, r[k].reading_len);
            vjo_buf_putc(b, ']');
            i = r[k++].end;
            continue;
        }
        {
            size_t j = i;
            vjo_utf8_next(t, len, &j);
            html_text(b, t + i, j - i);
            i = j;
        }
    }
}

char *vjo_sentence_furigana(VjoArena *a, VjoArena *scratch, const VjoPlatform *p, const VjoConfig *cfg,
                            const VjoReadingRules *rules, const VjoAnkiNote *note, VjoReadingStats *st, int *rc)
{
    size_t mark = vjo_arena_mark(scratch), len, kanji = 0, i = 0, keep_size;
    int o[2], n = 0, k, cap, hl;
    char *t, *out = NULL;
    VjoRuby *r;
    VjoArena keep;
    void *keep_mem;
    *rc = 0;
    if (!note->sentence) {
        *rc = VJO_E_PARSE;
        return NULL;
    }
    /* the sentence as the Sentence field joins it, the word's range following it */
    o[0] = note->hl_start;
    o[1] = note->hl_end;
    if (!(t = vjo_arena_strndup(scratch, note->sentence, strlen(note->sentence)))) {
        *rc = VJO_E_OOM;
        return NULL;
    }
    len = vjo_text_join(t, o, 2);
    hl = o[0] >= 0 && o[1] > o[0] && (size_t)o[1] <= len;
    while (i < len)
        kanji += (size_t)vjo_is_kanji(vjo_utf8_next(t, len, &i));
    if (!kanji) {
        vjo_arena_release(scratch, mark);
        return vjo_arena_strndup(a, "", 0);
    }
    cap = (int)kanji;
    keep_size = 4 * len + 1024; /* the readings: kana, at most a few a kanji */
    r = (VjoRuby *)vjo_arena_alloc(scratch, sizeof(*r) * (size_t)cap);
    keep_mem = vjo_arena_alloc(scratch, keep_size);
    if (!r || !keep_mem) {
        vjo_arena_release(scratch, mark);
        *rc = VJO_E_OOM;
        return NULL;
    }
    vjo_arena_init(&keep, keep_mem, keep_size);
    k = vjo_sentence_rubies(&keep, scratch, p, cfg, rules, t, 0, hl ? (size_t)o[0] : len, r, cap, st);
    if (k >= 0)
        n += k;
    if (k >= 0 && hl) { /* the mined word: the card's own reading */
        k = vjo_word_rubies(t + o[0], (size_t)(o[1] - o[0]), note->spelling, note->reading, r + n, cap - n);
        for (int j = 0; j < k; j++)
            r[n + j].start += (size_t)o[0], r[n + j].end += (size_t)o[0];
        n += k;
        k = vjo_sentence_rubies(&keep, scratch, p, cfg, rules, t, (size_t)o[1], len, r + n, cap - n, st);
        if (k >= 0)
            n += k;
    }
    if (k < 0) {
        *rc = k;
    } else if (!n) {
        out = vjo_arena_strndup(a, "", 0);
    } else {
        VjoBuf b;
        vjo_buf_init(&b, a);
        vjo_furigana_html(&b, t, len, r, n, hl ? o[0] : -1, hl ? o[1] : -1);
        out = vjo_buf_cstr(&b);
    }
    if (!out && !*rc)
        *rc = VJO_E_OOM;
    vjo_arena_release(scratch, mark);
    return out;
}
