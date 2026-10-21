/* Recognized text normalised for display and lookup (textnorm.h). Built for
 * size (-Os): the game worker runs it on the text it returns. */
#include "textnorm.h"

#include <stdint.h>
#include <string.h>

#define CP_KANJI_ONE 0x4E00u  /* 一 */
#define CP_LONG_VOWEL 0x30FCu /* ー */
#define CP_MIDDLE_DOT 0x30FBu /* ・ */
#define CP_ELLIPSIS 0x2026u   /* … */
#define CP_DASH 0x2015u       /* ― */
#define CP_SMALL_TSU 0x30C3u  /* ッ */
#define CP_SMALL_KE 0x30F6u   /* ヶ */

/* One code point at s: ASCII or a three-byte sequence, the only ones a rule
 * reads (every mark, kana and common kanji); anything else is one byte of
 * 0xFFFD and is kept as it is. Never reads past the NUL. */
static uint32_t decode(const char *str, size_t *n)
{
    const unsigned char *s = (const unsigned char *)str;
    *n = 1;
    if (s[0] < 0x80) return s[0];
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *n = 3;
        return (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3Fu);
    }
    return 0xFFFD;
}

static size_t encode3(uint32_t c, char *out)
{
    out[0] = (char)(0xE0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

static int is_hira(uint32_t c)
{
    return (c >= 0x3041 && c <= 0x3096) || c == 0x309D || c == 0x309E;
}

static int is_kata(uint32_t c)
{
    return (c >= 0x30A1 && c <= 0x30FA) || c == CP_LONG_VOWEL || c == 0x30FD || c == 0x30FE;
}

static int is_kanji(uint32_t c)
{
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) ||
           c == 0x3005 || c == 0x3006; /* 々〆 */
}

/* ぁぃぅぇぉっゃゅょゎ and their katakana (0x60 higher). Not the counters
 * ゕゖヵヶ: no long vowel comes before one, and 一 often does (一ヶ月). */
static int is_small(uint32_t c)
{
    static const unsigned char small[] = {0x01, 0x03, 0x05, 0x07, 0x09, 0x23, 0x43, 0x45, 0x47, 0x4E};
    if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60;
    if (c < 0x3041 || c > 0x3096) return 0;
    for (size_t i = 0; i < sizeof(small); i++)
        if (c - 0x3040 == small[i]) return 1;
    return 0;
}

/* What may follow a long vowel written after a hiragana: a small kana, ー, a
 * tilde, !, ?, an ellipsis, a closing quote. Not the end of the line: a line
 * wraps mid-word, and もう一 / 度 is far more common there than すごー. */
static int ends_elongation(uint32_t c)
{
    return is_small(c) || c == CP_LONG_VOWEL || c == '~' || c == '!' || c == '?' || c == CP_ELLIPSIS ||
           c == 0x2025 /* ‥ */ || c == 0x300D /* 」 */ || c == 0x300F /* 』 */;
}

/* — – ‐ ‑ ‒ − ─ ━ */
static int is_dash(uint32_t c)
{
    return (c >= 0x2010 && c <= 0x2014) || c == 0x2212 || c == 0x2500 || c == 0x2501;
}

/* A numeral (〇一二三四五六七八九十百千万, 0-9, ０-９), or 何 数 幾. */
static int is_count(uint32_t c)
{
    static const uint16_t num[] = {0x3007, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B,
                                   0x4E5D, 0x5341, 0x767E, 0x5343, 0x4E07, 0x4F55, 0x6570, 0x5E7E};
    if ((c >= '0' && c <= '9') || (c >= 0xFF10 && c <= 0xFF19)) return 1;
    for (size_t i = 0; i < sizeof(num) / sizeof(num[0]); i++)
        if (c == num[i]) return 1;
    return 0;
}

/* Pass 1: the forms (width, tildes, dashes, ellipses), and ッ before a counter made ヶ (textnorm.h: before
 * the 一/ー rule of pass 2, on the text as read). Writes at w <= r. */
static void fold_forms(char *s, VjoTextNorm *k)
{
    size_t r = 0, w = 0, n, m;
    uint32_t prev = 0; /* the code point read before c */
    while (s[r]) {
        uint32_t c = decode(s + r, &n), next;
        unsigned dots = 0;
        if (c == CP_SMALL_TSU && ((next = decode(s + r + n, &m)) == 0x6708 || next == 0x5E74 ||  /* 月 年 */
                                  (is_count(prev) && (next == 0x56FD || next == 0x6761 ||        /* 国 条 */
                                                      (next == 0x6240 && prev != CP_KANJI_ONE))))) { /* 所 */
            r += n;
            w += encode3(CP_SMALL_KE, s + w);
            k->counter++;
            prev = CP_SMALL_KE;
            continue;
        }
        if (c == '.' || c == 0xFF0E || (c == CP_MIDDLE_DOT && decode(s + r + n, &m) == CP_MIDDLE_DOT)) {
            /* A run of dots: an ellipsis for every three (of ・, at least one), the rest as dots. */
            uint32_t d = c == CP_MIDDLE_DOT ? c : '.';
            unsigned e;
            while ((c = decode(s + r, &n)) == d || (d == '.' && c == 0xFF0E)) {
                k->width += c == 0xFF0E;
                r += n;
                dots++;
            }
            e = d == '.' || dots >= 3 ? dots / 3 : 1;
            k->ellipsis += e;
            while (e--) w += encode3(CP_ELLIPSIS, s + w);
            if (d == '.')
                for (unsigned i = 0; i < dots % 3; i++) s[w++] = '.';
            prev = 0;
            continue;
        }
        r += n;
        prev = c;
        if (c >= 0xFF01 && c <= 0xFF5E) {
            s[w++] = (char)(c - 0xFEE0);
            k->width++;
        } else if (c == 0x301C || c == 0x223C) {
            s[w++] = '~';
            k->tilde++;
        } else if (is_dash(c)) {
            w += encode3(CP_DASH, s + w);
            k->dash++;
        } else {
            memmove(s + w, s + r - n, n);
            w += n;
        }
    }
    s[w] = '\0';
}

/* The first code point after a run of 一 and ー starting at s (0 at the end of
 * the line). */
static uint32_t after_run(const char *s)
{
    size_t n;
    uint32_t c;
    while ((c = decode(s, &n)) == CP_KANJI_ONE || c == CP_LONG_VOWEL) s += n;
    return c == '\n' ? 0 : c;
}

/* Pass 2: ー or 一 by context. Both are three bytes: the text keeps its size. */
static void long_vowels(char *s, VjoTextNorm *k)
{
    uint32_t prev = 0;
    size_t r = 0, n, m;
    while (s[r]) {
        uint32_t c = decode(s + r, &n), next;
        if (c == CP_KANJI_ONE && (is_kata(prev) || is_hira(prev))) {
            next = after_run(s + r);
            /* after katakana it stays before a kanji, a big hiragana or a counter
             * ヵ ヶ (ハネムーン一ヶ月) */
            if (is_kata(prev) ? !is_kanji(next) && !(is_hira(next) && !is_small(next)) && next != 0x30F5 &&
                                    next != 0x30F6
                              : ends_elongation(next)) {
                /* the whole run: "コ一一" is "コーー" */
                while (decode(s + r, &n) == CP_KANJI_ONE) {
                    encode3(CP_LONG_VOWEL, s + r);
                    k->long_vowel++;
                    r += n;
                }
                prev = CP_LONG_VOWEL;
                continue;
            }
        } else if (c == CP_LONG_VOWEL && is_kanji(prev) && prev != CP_KANJI_ONE &&
                   is_kanji(next = decode(s + r + n, &m)) && next != CP_KANJI_ONE) {
            encode3(CP_KANJI_ONE, s + r);
            k->kanji_one++;
            c = CP_KANJI_ONE;
        }
        prev = c == '\n' ? 0 : c;
        r += n;
    }
}

/* Pass 3: a box's border or scene art read as a line of text, "ーニーーーーニーニー…": three or
 * more code points, each one of ー ニ 一 二 ― - ~ ・ (the forms passes 1 and 2 leave), at least two
 * different ones, and ー or ニ (katakana) among them. Such a line goes with its line break; no line
 * is changed in part. */
static void garbage_lines(char *s, VjoTextNorm *k)
{
    /* bit i of seen: set[i] is in the line; bit 8: something else is */
    static const uint16_t set[] = {CP_LONG_VOWEL, 0x30CB /* ニ */, CP_KANJI_ONE, 0x4E8C /* 二 */, CP_DASH, '-', '~',
                                   CP_MIDDLE_DOT};
    size_t r = 0, w = 0, e, n;
    while (s[r]) {
        unsigned seen = 0, len = 0, i;
        for (e = r; s[e] && s[e] != '\n'; e += n, len++) {
            uint32_t c = decode(s + e, &n);
            for (i = 0; i < 8 && set[i] != c; i++) {
            }
            seen |= 1u << i;
        }
        if (len >= 3 && seen < 0x100 && (seen & (seen - 1)) && (seen & 3)) {
            k->garbage++;
            if (!s[e] && w) w--; /* the last line: the break before it goes */
            r = e + !!s[e];      /* any other: the break after it */
            continue;
        }
        e += !!s[e];
        memmove(s + w, s + r, e - r);
        w += e - r;
        r = e;
    }
    s[w] = '\0';
}

unsigned vjo_text_normalize_as(char *s, unsigned flags, VjoTextNorm *counts)
{
    VjoTextNorm k;
    memset(&k, 0, sizeof(k));
    if (s) {
        fold_forms(s, &k);
        long_vowels(s, &k);
        if (!(flags & VJO_TEXTNORM_KEEP_BORDERS)) garbage_lines(s, &k);
    }
    if (counts) *counts = k;
    return k.width + k.tilde + k.dash + k.ellipsis + k.long_vowel + k.kanji_one + k.counter + k.garbage;
}

unsigned vjo_text_normalize_keep(char *buf, size_t cap, const char *raw, unsigned flags, VjoTextNorm *counts)
{
    size_t n = 0, keep, m, rn;
    unsigned changes;
    if (!buf || cap < 2) return vjo_text_normalize_as(NULL, flags, counts);
    while (n + 1 < cap && buf[n]) n++;
    buf[n] = '\0';
    if (n + 2 > cap) { /* no room for a copy: the text fills the buffer */
        changes = vjo_text_normalize_as(buf, flags, counts);
        m = strlen(buf);
        memset(buf + m, 0, cap - m);
        return changes;
    }
    if (!raw) raw = buf;
    rn = strlen(raw);
    /* The copy goes after the text first; it is cut to the room left, at the
     * start of a character. */
    keep = rn + n + 2 <= cap ? rn : cap - n - 2;
    while (keep && keep < rn && ((unsigned char)raw[keep] & 0xC0) == 0x80) keep--;
    memmove(buf + n + 1, raw, keep);
    buf[n + 1 + keep] = '\0';
    changes = vjo_text_normalize_as(buf, flags, counts);
    m = strlen(buf);
    memmove(buf + m + 1, buf + n + 1, keep + 1);
    memset(buf + m + 1 + keep + 1, 0, cap - (m + 1 + keep + 1));
    return changes;
}
