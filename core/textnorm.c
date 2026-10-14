/* Recognized text normalised for display and lookup (textnorm.h). Built for
 * size: it runs in the Shell (SceShell's pages, AGENTS.md rule 1). */
#include "textnorm.h"

#include <stdint.h>
#include <string.h>

#define CP_KANJI_ONE 0x4E00u  /* 一 */
#define CP_LONG_VOWEL 0x30FCu /* ー */
#define CP_MIDDLE_DOT 0x30FBu /* ・ */
#define CP_ELLIPSIS 0x2026u   /* … */
#define CP_DASH 0x2015u       /* ― */

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

/* ぁぃぅぇぉっゃゅょゎゕゖ and their katakana (0x60 higher). */
static int is_small(uint32_t c)
{
    static const unsigned char small[] = {0x01, 0x03, 0x05, 0x07, 0x09, 0x23, 0x43, 0x45, 0x47, 0x4E, 0x55, 0x56};
    if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60;
    if (c < 0x3041 || c > 0x3096) return 0;
    for (size_t i = 0; i < sizeof(small); i++)
        if (c - 0x3040 == small[i]) return 1;
    return 0;
}

/* What may follow a long vowel written after a hiragana: a small kana, ー, a
 * tilde, !, ?, an ellipsis, a closing quote, the end of the line. */
static int ends_elongation(uint32_t c)
{
    return !c || is_small(c) || c == CP_LONG_VOWEL || c == '~' || c == '!' || c == '?' || c == CP_ELLIPSIS ||
           c == 0x2025 /* ‥ */ || c == 0x300D /* 」 */ || c == 0x300F /* 』 */;
}

/* — – ‐ ‑ ‒ − ─ ━ */
static int is_dash(uint32_t c)
{
    return (c >= 0x2010 && c <= 0x2014) || c == 0x2212 || c == 0x2500 || c == 0x2501;
}

/* Pass 1: the forms (width, tildes, dashes, ellipses). Writes at w <= r. */
static void fold_forms(char *s, VjoTextNorm *k)
{
    size_t r = 0, w = 0, n, m;
    while (s[r]) {
        uint32_t c = decode(s + r, &n);
        unsigned dots = 0;
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
            continue;
        }
        r += n;
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
            if (is_kata(prev) ? !is_kanji(next) && !(is_hira(next) && !is_small(next)) : ends_elongation(next)) {
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

unsigned vjo_text_normalize(char *s, VjoTextNorm *counts)
{
    VjoTextNorm k;
    memset(&k, 0, sizeof(k));
    if (s) {
        fold_forms(s, &k);
        long_vowels(s, &k);
    }
    if (counts) *counts = k;
    return k.width + k.tilde + k.dash + k.ellipsis + k.long_vowel + k.kanji_one;
}

unsigned vjo_text_normalize_keep(char *buf, size_t cap, VjoTextNorm *counts)
{
    size_t n = 0, keep, m;
    unsigned changes;
    if (!buf || cap < 2) return vjo_text_normalize(NULL, counts);
    while (n + 1 < cap && buf[n]) n++;
    buf[n] = '\0';
    if (n + 2 > cap) { /* no room for a copy: the text fills the buffer */
        changes = vjo_text_normalize(buf, counts);
        m = strlen(buf);
        memset(buf + m, 0, cap - m);
        return changes;
    }
    /* The copy goes after the text first; it is cut to the room left, at the
     * start of a character. */
    keep = 2 * n + 2 <= cap ? n : cap - n - 2;
    while (keep && keep < n && ((unsigned char)buf[keep] & 0xC0) == 0x80) keep--;
    memcpy(buf + n + 1, buf, keep);
    buf[n + 1 + keep] = '\0';
    changes = vjo_text_normalize(buf, counts);
    m = strlen(buf);
    memmove(buf + m + 1, buf + n + 1, keep + 1);
    memset(buf + m + 1 + keep + 1, 0, cap - (m + 1 + keep + 1));
    return changes;
}
