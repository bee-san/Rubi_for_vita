#include "sentence_reading.h"

#include <string.h>

#include "port.h"
#include "textjoin.h"
#include "utf.h"

#define WORD_MAX 64 /* code points of a word, its dictionary form or its reading: a longer one gets none */
#define WORD_BYTES (WORD_MAX * 4 + 1)
#define FIT_STEPS 4096 /* tries of a reading's fit before it counts as undecided (as core/anki.c's furigana) */
#define WORD_RUBIES 32 /* readings of one word (its kanji runs) */

void vjo_reading_rules_default(VjoReadingRules *r)
{
    r->ratio = VJO_READINGS_RATIO;
    r->part_ratio = VJO_READINGS_PART_RATIO;
    r->rare = VJO_READINGS_RARE;
    r->counters = r->variants = r->anchor = r->compounds = r->parts = r->particles = r->phrases = r->shorter =
        r->names = r->inline_readings = 1;
}

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

/* A reading: kana only (a kanji character's article, or a name without one, has its spelling for one). */
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

/* A character that takes a reading: a kanji, or a digit (５分[ごふん], １本[いっぽん]: a number and its counter as
 * one entry read as one). */
static int base(uint32_t c)
{
    return vjo_is_kanji(c) || (c >= '0' && c <= '9') || (c >= 0xFF10 && c <= 0xFF19);
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
    if (!base(f->w->c[i])) {
        if (j < f->r->n && same_kana(f->w->c[i], f->r->c[j]))
            fit(f, i + 1, j + 1);
        return;
    }
    while (e < f->w->n && base(f->w->c[e]))
        e++;
    for (int k = j + (e - i); k <= f->r->n && f->found < 2; k++) {
        f->end[i] = k;
        fit(f, e, k);
    }
}

/* Kanji whose dictionary-form reading changes with conjugation, as core/furigana.c's vjo_furigana_rubies reads them
 * (its table, which the Shell links: kept the same here, in the app's own code, so that the Shell's bytes are not
 * changed), with 来なさい's き besides: the reading before the n bytes of kana that follow (来ない: こ; 良かった: よ;
 * 行って: い), or NULL (the dictionary form's). */
static const char *kuru(const char *next, size_t n)
{
    /* 来 read く (来る): き before た/て/ま/ち/なさ (来た, 来ます, 来ちゃう, 来なさい), こ before な/ず/さ/ら/い/よ
     * and れ except in れば (来ない, 来させる, 来られる, 来い, 来よう, 来れる); else く (来る, 来れば) */
    static const char *const ki[] = {"た", "て", "ま", "ち", "なさ"};
    static const char *const ko[] = {"な", "ず", "さ", "ら", "い", "よ"};
    for (unsigned k = 0; k < sizeof(ki) / sizeof(ki[0]); k++)
        if (vjo_starts_withn(next, n, ki[k]))
            return "き";
    for (unsigned k = 0; k < sizeof(ko) / sizeof(ko[0]); k++)
        if (vjo_starts_withn(next, n, ko[k]))
            return "こ";
    if (vjo_starts_withn(next, n, "れ") && !vjo_starts_withn(next, n, "れば"))
        return "こ";
    return NULL;
}

/* 良い/好い read いい: only the plain form is いい; 良かった, 良く, 良さ are よ. */
static const char *ii(const char *next, size_t n)
{
    return vjo_starts_withn(next, n, "い") ? NULL : "よ";
}

/* 行く read ゆく: its past and て forms are いった, いって. */
static const char *yuku(const char *next, size_t n)
{
    return vjo_starts_withn(next, n, "っ") ? "い" : NULL;
}

static const char *conjugated(const char *base, size_t base_len, const char *reading, size_t reading_len,
                              const char *next, size_t n)
{
    static const struct {
        const char *base, *reading;
        const char *(*conjugated)(const char *next, size_t n);
    } irregular[] = {
        {"来", "く", kuru},
        {"良", "い", ii},
        {"好", "い", ii},
        {"行", "ゆ", yuku},
    };
    for (unsigned k = 0; k < sizeof(irregular) / sizeof(irregular[0]); k++)
        if (base_len == strlen(irregular[k].base) && !memcmp(base, irregular[k].base, base_len) &&
            reading_len == strlen(irregular[k].reading) && !memcmp(reading, irregular[k].reading, reading_len))
            return irregular[k].conjugated(next, n);
    return NULL;
}

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

/* The readings of the kanji of the text as written (s, sb's bytes) from its dictionary form w read r (Yomitan's
 * distributeFuriganaInflected): the text is written as w up to where its inflection starts (the stem); each of
 * its kanji runs is a run of w there and reads as it, conjugated (来[こ]ない; the kana after the word, after,
 * decide 何's). A dictionary form whose reading fits it in two ways reads as a whole, only uninflected
 * (物の怪[もののけ]). The count, or -1 when a kanji run of the text gets no reading. Readings point into rb or are
 * static. */
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
        /* (no way at all: the reading is not the word's; two: read whole, as the text is the word) */
        if (!f.found || stem != s->n || stem != w->n || !cap)
            return -1;
        out[0].start = s->off[0];
        out[0].end = s->off[s->n];
        out[0].reading = rb;
        out[0].reading_len = r->off[r->n];
        return 1;
    }
    for (int i = 0, j = 0; i < w->n;) {
        if (!base(w->c[i])) {
            i++;
            j++;
            continue;
        }
        rs[i] = j;
        j = f.best[i];
        while (i < w->n && base(w->c[i]))
            i++;
    }
    for (int i = 0; i < s->n;) {
        int e = i, we = i;
        const char *reading, *conj, *nb;
        size_t len, next, nn;
        if (!base(s->c[i])) {
            i++;
            continue;
        }
        while (e < s->n && base(s->c[e]))
            e++;
        while (we < w->n && base(w->c[we]))
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
        conj = conjugated(sb + s->off[i], s->off[e] - s->off[i], reading, len, nb, nn);
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

/* The readings of text (n bytes, followed by after) as the word spelling read reading: -1 when they do not fit
 * (or a part is longer than WORD_MAX), else their count (0: no kanji). */
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

int vjo_word_rubies(const char *surface, size_t n, const char *after, size_t after_len, const char *spelling,
                    const char *reading, VjoRuby *out, int cap)
{
    int k = word_rubies(surface, n, after ? after : "", after ? after_len : 0, spelling, reading, out, cap);
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
 * of だいじょうぶ), which the word's kanji spelling does not write. */
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

/* ---------------- names: VNDB's romanized readings ---------------- */

/* Hepburn romaji (VNDB's: long vowels doubled, ou, ii; n before a consonant; a hyphen before an honorific) as
 * hiragana into out (cap bytes): its length, or -1 for anything else (a foreign name, a letter it does not know). */
static int romaji_kana(const char *s, size_t n, char *out, size_t cap)
{
    static const char *const table[] = {
        /* three letters, then two, then one: the longest first */
        "kya\0きゃ", "kyu\0きゅ", "kyo\0きょ", "sha\0しゃ", "shu\0しゅ", "sho\0しょ", "shi\0し", "chi\0ち",
        "cha\0ちゃ", "chu\0ちゅ", "cho\0ちょ", "tsu\0つ", "nya\0にゃ", "nyu\0にゅ", "nyo\0にょ", "hya\0ひゃ",
        "hyu\0ひゅ", "hyo\0ひょ", "mya\0みゃ", "myu\0みゅ", "myo\0みょ", "rya\0りゃ", "ryu\0りゅ", "ryo\0りょ",
        "gya\0ぎゃ", "gyu\0ぎゅ", "gyo\0ぎょ", "bya\0びゃ", "byu\0びゅ", "byo\0びょ", "pya\0ぴゃ", "pyu\0ぴゅ",
        "pyo\0ぴょ", "ka\0か", "ki\0き", "ku\0く", "ke\0け", "ko\0こ", "sa\0さ", "su\0す", "se\0せ", "so\0そ",
        "ta\0た", "te\0て", "to\0と", "na\0な", "ni\0に", "nu\0ぬ", "ne\0ね", "no\0の", "ha\0は", "hi\0ひ",
        "fu\0ふ", "he\0へ", "ho\0ほ", "ma\0ま", "mi\0み", "mu\0む", "me\0め", "mo\0も", "ya\0や", "yu\0ゆ",
        "yo\0よ", "ra\0ら", "ri\0り", "ru\0る", "re\0れ", "ro\0ろ", "wa\0わ", "wo\0を", "ga\0が", "gi\0ぎ",
        "gu\0ぐ", "ge\0げ", "go\0ご", "za\0ざ", "ji\0じ", "zu\0ず", "ze\0ぜ", "zo\0ぞ", "ja\0じゃ", "ju\0じゅ",
        "jo\0じょ", "da\0だ", "de\0で", "do\0ど", "ba\0ば", "bi\0び", "bu\0ぶ", "be\0べ", "bo\0ぼ", "pa\0ぱ",
        "pi\0ぴ", "pu\0ぷ", "pe\0ぺ", "po\0ぽ", "a\0あ", "i\0い", "u\0う", "e\0え", "o\0お"};
    size_t i = 0, used = 0;
    while (i < n) {
        char c = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
        const char *kana = NULL;
        size_t take = 0;
        char d = i + 1 < n ? (char)(s[i + 1] | 32) : 0;
        if (c == 'n' && (s[i + 1 < n ? i + 1 : i] == '\'' && i + 1 < n)) {
            kana = "ん"; /* n' before a vowel: Shin'ichi */
            take = 2;
        } else if (c == 'n' && (!d || !strchr("aiueoy", d))) {
            kana = "ん";
            take = 1;
        } else if (c != 'n' && i + 1 < n && (s[i + 1] | 32) == c && strchr("kstpcgzdbhfjmrw", c)) {
            kana = "っ"; /* a doubled consonant */
            take = 1;
        } else if (c == 't' && i + 2 < n && (s[i + 1] | 32) == 'c' && (s[i + 2] | 32) == 'h') {
            kana = "っ"; /* tch */
            take = 1;
        } else {
            for (unsigned k = 0; k < sizeof(table) / sizeof(table[0]) && !kana; k++) {
                size_t m = strlen(table[k]);
                size_t j = 0;
                while (j < m && i + j < n && ((s[i + j] >= 'A' && s[i + j] <= 'Z') ? s[i + j] + 32 : s[i + j]) == table[k][j])
                    j++;
                if (j == m) {
                    kana = table[k] + m + 1;
                    take = m;
                }
            }
        }
        if (!kana)
            return -1;
        if (used + strlen(kana) >= cap)
            return -1;
        memcpy(out + used, kana, strlen(kana));
        used += strlen(kana);
        i += take;
    }
    out[used] = 0;
    return (int)used;
}

/* Kana as compared with a romanized reading: katakana as hiragana, ぢ づ を as じ ず お (Hepburn writes them so). */
static uint32_t name_kana(uint32_t c)
{
    c = fold(c);
    return c == 0x3062 ? 0x3058 : c == 0x3065 ? 0x305A : c == 0x3092 ? 0x304A : c;
}

static int same_name_reading(const char *a, size_t na, const char *b, size_t nb)
{
    size_t i = 0, j = 0;
    while (i < na && j < nb)
        if (name_kana(vjo_utf8_next(a, na, &i)) != name_kana(vjo_utf8_next(b, nb, &j)))
            return 0;
    return i == na && j == nb;
}

/* A name entry's definition (VNDB's: each character "Romaji Name (漢字 名)", then its game and its page) against
 * the word's reading of text (n bytes, the whole of it): 0 when some character's part written as the text is read
 * so (or none is: an alias, a name of a role), 1 when every one that is reads otherwise. */
static int name_disagrees(const char *def, const char *text, size_t n, const char *reading)
{
    const char *line = def;
    int seen = 0, agree = 0;
    while (*line) {
        const char *end = strchr(line, '\n'), *open, *close;
        if (!end)
            end = line + strlen(line);
        open = memchr(line, '(', (size_t)(end - line));
        close = open ? memchr(open, ')', (size_t)(end - open)) : NULL;
        if (open && close && open > line && close == end - 1) {
            /* the parts: romaji split at spaces and hyphens, the original at spaces */
            const char *rp[8], *kp[8];
            size_t rl[8], kl[8];
            int nr = 0, nk = 0;
            const char *q = line, *rend = open - 1;
            while (q < rend && nr < 8) {
                const char *b = q;
                while (q < rend && *q != ' ' && *q != '-')
                    q++;
                rp[nr] = b;
                rl[nr++] = (size_t)(q - b);
                while (q < rend && (*q == ' ' || *q == '-'))
                    q++;
            }
            q = open + 1;
            while (q < close && nk < 8) {
                const char *b = q;
                while (q < close && *q != ' ')
                    q++;
                kp[nk] = b;
                kl[nk++] = (size_t)(q - b);
                while (q < close && *q == ' ')
                    q++;
            }
            for (int k = 0; nr == nk && k < nk; k++) {
                char kana[WORD_BYTES];
                int m;
                if (kl[k] != n || memcmp(kp[k], text, n) || (m = romaji_kana(rp[k], rl[k], kana, sizeof(kana))) < 0)
                    continue;
                seen = 1;
                agree |= same_name_reading(kana, (size_t)m, reading, strlen(reading));
            }
        }
        line = *end ? end + 1 : end;
    }
    return seen && !agree;
}

#ifdef VJO_HOST
int vjo_reading_romaji_kana(const char *s, size_t n, char *out, size_t cap)
{
    return romaji_kana(s, n, out, cap);
}

int vjo_reading_name_disagrees(const char *def, const char *text, size_t n, const char *reading)
{
    return name_disagrees(def, text, n, reading);
}
#endif

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

/* The character that ends s[0, at) (0 for at 0). */
static uint32_t char_before(const char *s, size_t at)
{
    size_t i;
    if (!at)
        return 0;
    i = at - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    return vjo_utf8_next(s, at, &i);
}

static int katakana(uint32_t c)
{
    return c >= 0x30A1 && c <= 0x30FC && !vjo_is_kanji(c);
}

typedef struct {
    VjoArena *keep, *scratch;
    const VjoPlatform *p;
    VjoConfig *cfg; /* the caller's, the single-result lookup */
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

/* The caller's Cancel (VjoReadingStats.stopped), asked before each lookup: VJO_E_CANCELLED, else 0. */
static int stopping(const Parse *ps)
{
    return ps->st->stopped && ps->st->stopped(ps->st->stopped_ud) ? VJO_E_CANCELLED : 0;
}

/* A failure of the parse: counted, unless it is the caller's Cancel. */
static int failed(Parse *ps, int rc)
{
    if (rc != VJO_E_CANCELLED)
        ps->st->failed++;
    return rc;
}

/* 1 and the entry, 0 with no match at cursor, < 0 on failure. */
static int entry_at(Parse *ps, const char *text, size_t cursor, Entry *e)
{
    size_t mark = vjo_arena_mark(ps->scratch), len = strlen(text), at = cursor;
    VjoDictResult res;
    VjoErr err;
    const VjoVocab *v;
    int rc, units = 0;
    if ((rc = stopping(ps)) < 0)
        return rc;
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
    v = &res.vocab[res.tokens[0].vocab];
    e->end = at;
    e->rank = v->rank;
    e->conjugation = res.tokens[0].conjugation;
    e->ok = copy_word(e->spelling, v->spelling) && copy_word(e->reading, v->reading);
    vjo_snprintf(e->dictionary, sizeof(e->dictionary), "%s", v->dictionary ? v->dictionary : "");
    vjo_arena_release(ps->scratch, mark);
    return 1;
}

/* Rare: unranked, or ranked past rules.rare. */
static int rare(const Parse *ps, int rank)
{
    return !rank || rank > ps->rules->rare;
}

/* An entry is among the top ones beside the chosen one (rank best): Jiten's rank within ratio of it, or the
 * chosen one has none, or ratio 0 (every entry). */
static int as_frequent(int rank, int best, int ratio)
{
    if (!best || !ratio)
        return 1;
    return rank && (long long)rank <= (long long)ratio * best;
}

/* mine's readings inside w[o, o + n), as offsets into it: how many, into rm */
static int inside(const VjoRuby *r, int k, size_t o, size_t n, VjoRuby *rm)
{
    int nm = 0;
    for (int j = 0; j < k; j++)
        if (r[j].start >= o && r[j].end <= o + n) {
            rm[nm] = r[j];
            rm[nm].start -= o;
            rm[nm++].end -= o;
        }
    return nm;
}

/* Does another entry of text[o, o + n) of the word w (its readings r, k of them; mine its entry), as frequent as
 * mine within ratio (0: any entry), read its kanji otherwise than the word does? after: what follows the text.
 * 1 yes, 0 no, < 0 a failed lookup (VJO_E_TOO_LARGE: the entries could not all be read). */
static int others_disagree(Parse *ps, const char *w, size_t o, size_t n, const char *after, size_t after_len,
                           const Entry *mine, const VjoRuby *r, int k, int ratio)
{
    size_t mark = vjo_arena_mark(ps->scratch), units = 0, i = 0;
    const char *text = w + o;
    char *copy = vjo_arena_strndup(ps->scratch, text, n);
    VjoRuby rm[WORD_RUBIES];
    VjoDictResult res;
    VjoErr err;
    int rc, verdict = 0, nm = inside(r, k, o, n, rm);
    if (!copy)
        return VJO_E_OOM;
    while (i < n)
        units += (size_t)vjo_utf16_units(vjo_utf8_next(text, n, &i));
    if ((rc = stopping(ps)) < 0) {
        vjo_arena_release(ps->scratch, mark);
        return rc;
    }
    memset(&err, 0, sizeof(err));
    ps->st->lookups++;
    rc = vjo_local_lookup(ps->scratch, ps->p, ps->cfg, copy, &res, &err);
    if (rc) {
        vjo_arena_release(ps->scratch, mark);
        return rc;
    }
    for (int t = 0; t < res.n_tokens && !verdict; t++) {
        const VjoVocab *v = &res.vocab[res.tokens[t].vocab];
        VjoRuby ro[WORD_RUBIES];
        int ko;
        if (res.tokens[t].pos16 != 0 || (size_t)res.tokens[t].len16 != units)
            continue;
        /* (another dictionary's unranked entry beside the word's, such as a character's name, is as frequent) */
        if (!as_frequent(v->rank, mine->rank, ratio) &&
            !(!v->rank && strcmp(v->dictionary ? v->dictionary : "", mine->dictionary)))
            continue;
        if ((!strcmp(v->spelling, mine->spelling) && !strcmp(v->reading, mine->reading)) ||
            (ps->rules->variants && shortening(mine->reading, v->reading)))
            continue;
        ko = word_rubies(text, n, after, after_len, v->spelling, v->reading, ro, WORD_RUBIES);
        if (ko > 0 && !same_rubies(rm, nm, ro, ko))
            verdict = 1;
        /* another dictionary's entry of the text whose reading is no kana: a name (VNDB's names have none) whose
         * characters' romanized names read it otherwise (一樹: Kazuki, the word いちじゅ) */
        else if (ko < 0 && ps->rules->names && strcmp(v->dictionary ? v->dictionary : "", mine->dictionary) && !o &&
                 v->n_meanings && name_disagrees(v->meanings[0], text, n, mine->reading))
            verdict = 2;
    }
    vjo_arena_release(ps->scratch, mark);
    return verdict;
}

/* Does a part of the word w, w[o, o + n), as the overlay looks it up on its own (its entry at its start), overrule
 * it: an entry part_ratio times as frequent as mine (or mine unranked), over at least the part's first run of
 * kanji, that reads its kanji otherwise? 1 yes, 0 no, < 0 a failed lookup. */
static int part_overrules(Parse *ps, const char *w, size_t o, size_t n, const char *after, size_t after_len,
                          const Entry *mine, const VjoRuby *r, int k)
{
    size_t mark = vjo_arena_mark(ps->scratch), run = 0;
    char *copy = vjo_arena_strndup(ps->scratch, w + o, n);
    VjoRuby rm[WORD_RUBIES], ro[WORD_RUBIES];
    Entry e;
    int got, nm, ko, verdict = 0;
    if (!copy)
        return VJO_E_OOM;
    got = entry_at(ps, copy, 0, &e);
    vjo_arena_release(ps->scratch, mark);
    if (got <= 0)
        return got;
    while (run < n) { /* the part's first run of kanji ends here */
        size_t q = run;
        if (!vjo_is_kanji(vjo_utf8_next(w + o, n, &q)))
            break;
        run = q;
    }
    if (!e.ok || !e.rank || (mine->rank && (long long)mine->rank <= (long long)ps->rules->part_ratio * e.rank) ||
        e.end < run)
        return 0;
    nm = inside(r, k, o, e.end, rm);
    ko = word_rubies(w + o, e.end, e.end < n ? w + o + e.end : after, e.end < n ? n - e.end : after_len, e.spelling,
                     e.reading, ro, WORD_RUBIES);
    if (ko > 0 && !same_rubies(rm, nm, ro, ko))
        verdict = 1;
    return verdict;
}

/* The word of a phrase entry, text[0, n) (followed by after: its particle), whose readings there are r (k of them):
 * does another word of that text read it otherwise? The word's own entry is the one that reads it as the phrase
 * does (通り どおり, rank 2007); another word is an entry of another Jiten rank, as frequent within ratio (通り とおり,
 * 1755). Readings of the same rank are one word's (言う: いう and ゆう, both 25), and an unranked one says nothing
 * (限り: きり). 1 yes, 0 no, < 0 a failed lookup. */
static int particle_word_disagrees(Parse *ps, const char *text, size_t n, const char *after, size_t after_len,
                                   const VjoRuby *r, int k)
{
    size_t mark = vjo_arena_mark(ps->scratch), units = 0, i = 0;
    char *copy = vjo_arena_strndup(ps->scratch, text, n);
    VjoRuby rm[WORD_RUBIES], ro[WORD_RUBIES];
    VjoDictResult res;
    VjoErr err;
    int rc, own = 0, verdict = 0, nm = inside(r, k, 0, n, rm), ko;
    if (!copy)
        return VJO_E_OOM;
    while (i < n)
        units += (size_t)vjo_utf16_units(vjo_utf8_next(text, n, &i));
    if ((rc = stopping(ps)) < 0) {
        vjo_arena_release(ps->scratch, mark);
        return rc;
    }
    memset(&err, 0, sizeof(err));
    ps->st->lookups++;
    rc = vjo_local_lookup(ps->scratch, ps->p, ps->cfg, copy, &res, &err);
    if (rc) {
        vjo_arena_release(ps->scratch, mark);
        return rc;
    }
    for (int pass = 0; pass < 2 && !verdict; pass++) /* the word's own entry first, then the others */
        for (int t = 0; t < res.n_tokens && !verdict && !(pass == 0 && own); t++) {
            const VjoVocab *v = &res.vocab[res.tokens[t].vocab];
            if (res.tokens[t].pos16 != 0 || (size_t)res.tokens[t].len16 != units || !v->rank)
                continue;
            ko = word_rubies(text, n, after, after_len, v->spelling, v->reading, ro, WORD_RUBIES);
            if (ko <= 0)
                continue;
            if (pass == 0 && same_rubies(rm, nm, ro, ko))
                own = v->rank;
            else if (pass == 1 && own && v->rank != own && as_frequent(v->rank, own, ps->rules->ratio) &&
                     !same_rubies(rm, nm, ro, ko))
                verdict = 1;
        }
    vjo_arena_release(ps->scratch, mark);
    return verdict;
}

/* The kanji characters of s[0, n). */
static int kanji_count(const char *s, size_t n)
{
    size_t i = 0;
    int k = 0;
    while (i < n)
        k += vjo_is_kanji(vjo_utf8_next(s, n, &i));
    return k;
}

/* A particle a whole entry may end with after one kanji (彼の, 俺が, 外に, 誰が, 私か): the lookup's longest match
 * can take a rare phrase entry over the common word and its particle. */
static int particle(const char *s, size_t n)
{
    static const char *const kana[] = {"の", "が", "に", "を", "は", "と", "で", "も", "へ", "か", "や"};
    for (unsigned k = 0; k < sizeof(kana) / sizeof(kana[0]); k++)
        if (n == strlen(kana[k]) && !memcmp(s, kana[k], n))
            return 1;
    return 0;
}

/* A particle a match from kana may start with (は行って, が目を, と書かれた, と一緒に). */
static int particle_char(uint32_t c)
{
    static const uint16_t p[] = {0x306F, 0x304C, 0x3092, 0x306B, 0x3068, 0x306E, 0x3067, 0x3082, 0x3078, 0x3084};
    for (unsigned k = 0; k < sizeof(p) / sizeof(p[0]); k++) /* は が を に と の で も へ や */
        if (c == p[k])
            return 1;
    return 0;
}

/* A phrase's first word: one kanji, a particle, then another kanji (木の下: このした, of 木 き; 目を開く) */
static int phrase_head(const char *text, size_t re, size_t n)
{
    size_t q = re, at;
    uint32_t c;
    if (kanji_count(text, re) != 1 || re >= n)
        return 0;
    at = q;
    c = vjo_utf8_next(text, n, &q);
    (void)at;
    return particle_char(c) && q < n && vjo_is_kanji(first_char(text, q, n));
}

/* One matched word, text[0, n) (with kanji), followed by after; e its entry. (A single kanji in a compound the lookup
 * split gets none before this: parse().) Its readings into out (at most cap), copied into keep. Returns how many
 * (0: none, counted in st), or < 0 on failure. */
static int word(Parse *ps, const char *text, size_t n, const char *after, size_t after_len, const Entry *e,
                VjoRuby *out, int cap)
{
    const VjoReadingRules *ru = ps->rules;
    VjoRuby r[WORD_RUBIES];
    int k, rc;
    unsigned *why = &ps->st->ambiguous;
    size_t i = 0;
    ps->st->words++;
    if (!e->ok || (k = word_rubies(text, n, after, after_len, e->spelling, e->reading, r, WORD_RUBIES)) <= 0) {
        ps->st->unplaced++;
        if (ps->st->trace)
            ps->st->trace(ps->st->trace_ud, "unplaced", text, n, e->spelling, e->reading);
        return 0;
    }
    /* A: another entry of the same text, about as frequent, reads its kanji otherwise */
    rc = others_disagree(ps, text, 0, n, after, after_len, e, r, k, ru->ratio);
    /* A on the word of an unranked entry that ends in a particle after kana (通りに, read どおりに: the longest match
     * took a rare phrase over the word and its particle): another word of that text reads it otherwise (通り after a
     * verb is とおり): particle_word_disagrees. A word of kanji alone before the particle is B's (彼の, 一緒に). */
    if (!rc && ru->particles && !e->rank && !e->conjugation && n > 3 && particle(text + n - 3, 3) &&
        has_kanji(text, n - 3) && !vjo_is_kanji(char_before(text, n - 3)))
        rc = particle_word_disagrees(ps, text, n - 3, text + n - 3, 3, r, k);
    /* B, C: each run of kanji of a longer word, as a word of its own (B: the run alone; C: a later run with the
     * kana after it), far more frequent than the word, reads it otherwise (今日 of 今日は, 解明 of 解明して, 開ける
     * of 目を開ける) */
    while (!rc && i < n) {
        size_t rs = i, re, ce, j = i;
        if (!vjo_is_kanji(vjo_utf8_next(text, n, &j))) {
            i = j;
            continue;
        }
        re = j;
        while (re < n) {
            size_t q = re;
            if (!vjo_is_kanji(vjo_utf8_next(text, n, &q)))
                break;
            re = q;
        }
        ce = re;
        while (ce < n) {
            size_t q = ce;
            if (vjo_is_kanji(vjo_utf8_next(text, n, &q)))
                break;
            ce = q;
        }
        /* B: a run of two kanji or more (今日 of 今日は, 解明 of 解明して, 自然 of 自然に), or one kanji with a
         * particle after it, the whole entry (彼 of 彼の: かの, 外 of 外に: ほかに); a single kanji of a verb or a
         * compound word reads otherwise on its own (立 of 立ち上がる) */
        if (ru->parts && (rs > 0 || re < n) &&
            (kanji_count(text + rs, re - rs) > 1 || (rs == 0 && !e->conjugation && particle(text + re, n - re)) ||
             (rs == 0 && phrase_head(text, re, n)))) {
            rc = part_overrules(ps, text, rs, re - rs, text + re, n - re, e, r, k);
            why = &ps->st->part;
        }
        if (!rc && ru->phrases && rs > 0 && ce > re) {
            rc = part_overrules(ps, text, rs, ce - rs, ce < n ? text + ce : after, ce < n ? n - ce : after_len, e, r,
                                k);
            why = &ps->st->phrase;
        }
        i = re;
    }
    /* D: a rare inflected match, a few characters shorter (its kanji kept), is a common word that reads its kanji
     * otherwise (扱い of 扱いだ read as 扱ぐ's past, 深い of 深いた as 深く's) */
    if (!rc && ru->shorter && e->conjugation && rare(ps, e->rank)) {
        /* (a character shorter, then two, three: 同じ of 同じタイ, read as 同じる's 同じたい) */
        size_t sub = n, last = 0, q = 0;
        while (q < n) { /* where the word's last kanji ends */
            if (vjo_is_kanji(vjo_utf8_next(text, n, &q)))
                last = q;
        }
        for (int tries = 0; !rc && tries < 3; tries++) {
            sub--;
            while (sub > 0 && ((unsigned char)text[sub] & 0xC0) == 0x80)
                sub--;
            if (sub <= last)
                break;
            rc = part_overrules(ps, text, 0, sub, text + sub, n - sub, e, r, k);
            why = &ps->st->shorter;
        }
    }
    if (rc == VJO_E_TOO_LARGE) {
        ps->st->unchecked++;
        return 0;
    }
    if (rc < 0)
        return rc;
    if (rc) {
        static const char *const names[] = {"ambiguous", "compound", "part", "phrase", "shorter", "name"};
        unsigned *counters[] = {&ps->st->ambiguous, &ps->st->compound, &ps->st->part, &ps->st->phrase,
                                &ps->st->shorter, &ps->st->name};
        if (rc == 2)
            why = &ps->st->name;
        (*why)++;
        for (unsigned j = 0; ps->st->trace && j < sizeof(names) / sizeof(names[0]); j++)
            if (counters[j] == why)
                ps->st->trace(ps->st->trace_ud, names[j], text, n, e->spelling, e->reading);
        return 0;
    }
    if (ps->st->trace)
        ps->st->trace(ps->st->trace_ud, "read", text, n, e->spelling, e->reading);
    if (k > cap)
        return VJO_E_TOO_LARGE;
    for (int j = 0; j < k; j++) {
        out[j] = r[j];
        if (!(out[j].reading = vjo_arena_strndup(ps->keep, r[j].reading, r[j].reading_len)))
            return VJO_E_OOM;
    }
    ps->st->read++;
    return k;
}

static int hiragana(uint32_t c)
{
    return c >= 0x3041 && c <= 0x309F;
}

static int kana(uint32_t c)
{
    return hiragana(c) || katakana(c);
}

/* Kana a noun's particle or copula starts with (私は, 嘘だ, 変な, 猫じゃ, 嘘っ, 今まで, 今さ, 今くらい): what follows a
 * single kanji matched alone when it is a word of its own. */
static int follower(uint32_t c)
{
    static const uint16_t f[] = {0x304C, 0x306E, 0x3092, 0x306B, 0x306F, 0x3078, 0x3068, 0x3067, 0x3082, 0x3084,
                                 0x304B, 0x306D, 0x3088, 0x3060, 0x306A, 0x3058, 0x305E, 0x3063, 0x3055,
                                 0x307E, 0x307B, 0x304F, 0x3050, 0x3070};
    /* が の を に は へ と で も や か ね よ だ な じ ぞ っ さ ま ほ く ぐ ば (not わ: 終わっ) */
    for (unsigned k = 0; k < sizeof(f) / sizeof(f[0]); k++)
        if (c == f[k])
            return 1;
    return 0;
}

/* A number and 日 as one entry: a date's day or a count of days (１日: ついたち, いちにち), not told apart. */
static int days(const char *s, size_t n)
{
    return n > 3 && !memcmp(s + n - 3, "\xE6\x97\xA5", 3) /* 日 */ && all_numerals(s, n - 3);
}

static size_t chars(const char *s, size_t n)
{
    size_t i = 0, k = 0;
    while (i < n) {
        vjo_utf8_next(s, n, &i);
        k++;
    }
    return k;
}

static int digit(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 0xFF10 && c <= 0xFF19);
}

/* No reading in brackets after the word (s: the text after it), or one that is the word's last reading r. */
static int inline_agrees(const char *s, size_t n, const VjoRuby *r)
{
    size_t i = 0, start, end;
    uint32_t c = n ? vjo_utf8_next(s, n, &i) : 0;
    int kana_only = 1;
    if (c != '(' && c != 0xFF08)
        return 1;
    start = i;
    for (;;) {
        size_t at = i;
        if (i >= n || i - start > 64)
            return 1; /* no closing bracket near: not a reading */
        c = vjo_utf8_next(s, n, &i);
        if (c == ')' || c == 0xFF09) {
            end = at;
            break;
        }
        kana_only &= kana(c) || c == 0x30FC;
    }
    if (!kana_only || end == start)
        return 1;
    i = start;
    {
        size_t j = 0;
        while (i < end && j < r->reading_len)
            if (fold(vjo_utf8_next(s, end, &i)) != fold(vjo_utf8_next(r->reading, r->reading_len, &j)))
                return 0;
        return i == end && j == r->reading_len;
    }
}

#define MAX_DEPTH 4 /* parse()'s recursion: the text before a split compound's word */

/* The words of text[from, to) (text: NUL-terminated at `to`, so that no lookup reads past it; follow: the
 * character after it in the sentence, 0 for none), their readings appended to out (*n of cap so far). 0, or < 0
 * on failure. */
static int parse(Parse *ps, const char *text, size_t from, size_t to, uint32_t follow, VjoRuby *out, int cap, int *n,
                 int depth)
{
    const VjoReadingRules *rules = ps->rules;
    size_t pos = from;
    /* a run of numbers just before: where it ends (0: none), its first reading and its words with one */
    size_t number_end = 0;
    int number_first = 0;
    unsigned number_read = 0;
    while (pos < to) {
        size_t seg = pos, i;
        int kanji = 0;
        uint32_t cp;
        while (seg < to) {
            i = seg;
            cp = vjo_utf8_next(text, to, &i);
            if (boundary(cp))
                break;
            kanji |= vjo_is_kanji(cp);
            seg = i;
        }
        if (seg == pos) { /* a boundary */
            vjo_utf8_next(text, to, &pos);
            number_end = 0;
            continue;
        }
        if (!kanji) { /* nothing here takes a reading */
            pos = seg;
            number_end = 0;
            continue;
        }
        while (pos < seg) {
            Entry e;
            size_t end;
            int got = entry_at(ps, text, pos, &e), before = *n, k = 0, rc;
            if (got < 0)
                return failed(ps, got);
            if (got) {
                uint32_t c0 = first_char(text, pos, to);
                end = e.end > seg ? seg : e.end;
                /* A match of kana alone (はあっ in はあっという間に) gives way to the lookup one character on when
                 * that reaches further, over kanji: its kana take no reading either way. */
                if (rules->anchor && kana(c0) && !has_kanji(text + pos, end - pos)) {
                    size_t q = pos;
                    Entry a;
                    vjo_utf8_next(text, to, &q);
                    if (q < end && has_kanji(text + q, seg - q)) {
                        if ((got = entry_at(ps, text, q, &a)) < 0)
                            return failed(ps, got);
                        if (got && a.end > end && has_kanji(text + q, (a.end > seg ? seg : a.end) - q)) {
                            pos = q;
                            e = a;
                            end = a.end > seg ? seg : a.end;
                            ps->st->anchored++;
                        }
                        got = 1;
                    }
                }
                /* A match from kana with kanji after it gives way to the lookup at its first kanji: after a
                 * particle when that reaches as far (ガ目 of が目を: 目を逸らす, ト書 of と書かれた), else when it
                 * is rare and that reaches further (ラ変 of ら変わった: 変わった; not an honorific's word: お上, おかみ)
                 * or is part_ratio times as frequent (この家 of この家は: 家) */
                c0 = first_char(text, pos, to);
                if (rules->anchor && kana(c0) && has_kanji(text + pos, end - pos)) {
                    size_t q = pos, at = pos;
                    Entry a;
                    while (q < end) {
                        at = q;
                        if (vjo_is_kanji(vjo_utf8_next(text, end, &q)))
                            break;
                    }
                    if ((got = entry_at(ps, text, at, &a)) < 0)
                        return failed(ps, got);
                    if (got && a.end >= end &&
                        ((particle_char(c0) && at == pos + 3) ||
                         (rare(ps, e.rank) && a.rank &&
                          ((a.end > end && c0 != 0x304A && c0 != 0x3054) /* (not after お, ご: お上) */ || !e.rank ||
                           (long long)a.rank * rules->part_ratio <= e.rank)))) {
                        pos = at;
                        e = a;
                        end = a.end > seg ? seg : a.end;
                        ps->st->anchored++;
                    }
                    got = 1;
                }
                /* A match that ends inside a run of kanji whose later kanji starts a longer word part_ratio times as
                 * frequent (夢の世 of 夢の世界: 世界; 初任 of 初任務: 任務) took that word's first kanji: the text before
                 * that kanji is parsed on its own, and the word from there (以前 of 以前述べた stays: 前述 is rarer). */
                if (rules->anchor && depth < MAX_DEPTH && end < seg && vjo_is_kanji(first_char(text, end, to)) &&
                    vjo_is_kanji(char_before(text, end))) {
                    size_t q = pos;
                    vjo_utf8_next(text, end, &q);
                    while (q < end) {
                        size_t at = q;
                        Entry a;
                        int got2;
                        if (!vjo_is_kanji(vjo_utf8_next(text, end, &q)))
                            continue;
                        if ((got2 = entry_at(ps, text, at, &a)) < 0)
                            return failed(ps, got2);
                        if (got2 && a.end > end && a.rank &&
                            (!e.rank || (long long)a.rank * rules->part_ratio <= e.rank)) {
                            size_t mark = vjo_arena_mark(ps->scratch);
                            char *head = vjo_arena_strndup(ps->scratch, text, at);
                            if (!head)
                                return VJO_E_OOM;
                            ps->st->anchored++;
                            rc = parse(ps, head, pos, at, first_char(text, at, to), out, cap, n, depth + 1);
                            vjo_arena_release(ps->scratch, mark);
                            if (rc < 0)
                                return rc;
                            pos = at;
                            e = a;
                            end = a.end > seg ? seg : a.end;
                            before = *n;
                            number_end = 0;
                            break;
                        }
                    }
                }
            } else {
                end = pos;
                cp = vjo_utf8_next(text, to, &end);
                ps->st->unmatched += (unsigned)vjo_is_kanji(cp);
            }
            if (got && end > pos && has_kanji(text + pos, end - pos)) {
                uint32_t first = first_char(text, pos, to), prev = char_before(text, pos);
                uint32_t last = char_before(text, end), next = end < to ? first_char(text, end, to) : follow;
                int bound = (vjo_is_kanji(first) && (vjo_is_kanji(prev) || katakana(prev))) ||
                            (vjo_is_kanji(last) && vjo_is_kanji(next));
                int lone = vjo_is_kanji(first) && chars(text + pos, end - pos) == 1;
                ps->st->words++;
                if (bound && rules->compounds && kanji_count(text + pos, end - pos) == 1) {
                    /* a single kanji in a compound the lookup split: its reading as a word says little (中 of
                     * 授業中, 君 of 春原君, 箱 of マッチ箱: ばこ) */
                    ps->st->compound++;
                    if (ps->st->trace)
                        ps->st->trace(ps->st->trace_ud, "compound", text + pos, end - pos, e.spelling, e.reading);
                } else if (lone && hiragana(next) && (rare(ps, e.rank) || !follower(next)) && rules->shorter) {
                    /* a single kanji matched alone before kana that is no particle or copula (or rare): the lookup did
                     * not find the word's inflection (働 of 働かな: どう, 下 of 下さーい: した, 行 of 行こっ) */
                    ps->st->shorter++;
                    if (ps->st->trace)
                        ps->st->trace(ps->st->trace_ud, "shorter", text + pos, end - pos, e.spelling, e.reading);
                } else if (rules->counters &&
                           (days(text + pos, end - pos) || (digit(prev) && digit(first)) ||
                            (digit(last) && digit(next)) ||
                            (numeral(prev) && !numeral(first) && pos > number_end))) {
                    /* a number and 日 (a date's day or a count of days), a match that starts or ends inside a number
                     * (第２ of 第２１, ５分 of ４５分), a word after a number another match ends with (人 of もう１人) */
                    ps->st->counters++;
                    if (ps->st->trace)
                        ps->st->trace(ps->st->trace_ud, "counters", text + pos, end - pos, e.spelling, e.reading);
                } else {
                    ps->st->words--; /* word() counts it */
                    k = word(ps, text + pos, end - pos, text + end, to - end, &e, out + *n, cap - *n);
                }
                if (k < 0) {
                    if (k == VJO_E_TOO_LARGE && *n == cap)
                        k = 0; /* no room for more: the rest stays without */
                    else
                        return failed(ps, k);
                }
                /* the text's own reading in brackets after the word (嚇(おど)し, 賓客(ひんきゃく)): the word's last
                 * reading must be it */
                if (k > 0 && rules->inline_readings && out[*n + k - 1].end == end - pos &&
                    !inline_agrees(text + end, to - end, &out[*n + k - 1])) {
                    ps->st->read--;
                    ps->st->inline_readings++;
                    if (ps->st->trace)
                        ps->st->trace(ps->st->trace_ud, "inline", text + pos, end - pos, e.spelling, e.reading);
                    k = 0;
                }
                for (int j = 0; j < k; j++)
                    out[*n + j].start += pos, out[*n + j].end += pos;
                *n += k;
            }
            /* a number's counter, and the number: no reading (三本: さんぼん, 一回: いっかい) */
            if (all_numerals(text + pos, end - pos)) {
                if (number_end != pos) {
                    number_first = before;
                    number_read = 0;
                }
                number_read += (unsigned)(k > 0);
                number_end = end;
            } else {
                size_t q = pos;
                if (rules->counters && number_end && number_end == pos && vjo_is_kanji(vjo_utf8_next(text, end, &q))) {
                    unsigned dropped = number_read + (unsigned)(k > 0);
                    ps->st->read -= dropped;
                    ps->st->counters += dropped;
                    *n = number_first;
                }
                number_end = 0;
            }
            pos = end;
        }
    }
    return 0;
}

int vjo_sentence_rubies(VjoArena *keep, VjoArena *scratch, const VjoPlatform *p, const VjoConfig *cfg,
                        const VjoReadingRules *rules, const char *text, size_t from, size_t to, VjoRuby *out,
                        int cap, VjoReadingStats *st)
{
    size_t base = vjo_arena_mark(scratch), keep_base = vjo_arena_mark(keep);
    VjoReadingRules defaults;
    VjoReadingStats unused;
    Parse ps;
    char *copy;
    int n = 0, rc;
    memset(&unused, 0, sizeof(unused));
    if (!rules) {
        vjo_reading_rules_default(&defaults);
        rules = &defaults;
    }
    ps.keep = keep;
    ps.scratch = scratch;
    ps.p = p;
    ps.rules = rules;
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

    rc = parse(&ps, copy, from, to, first_char(text, to, strlen(text)), out, cap, &n, 0);
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
    if (hl) { /* a highlight inside a character takes the characters it touches (<b> and </b> both written) */
        while (o[0] > 0 && ((unsigned char)t[o[0]] & 0xC0) == 0x80)
            o[0]--;
        while ((size_t)o[1] < len && ((unsigned char)t[o[1]] & 0xC0) == 0x80)
            o[1]++;
    }
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
        k = vjo_word_rubies(t + o[0], (size_t)(o[1] - o[0]), t + o[1], len - (size_t)o[1], note->spelling,
                            note->reading, r + n, cap - n);
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
