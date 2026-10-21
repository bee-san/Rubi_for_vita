/* Recognized text, normalised for display and dictionary lookup.
 *
 * The local recognizer's vocabulary holds the half- and full-width forms of
 * the same marks (! and ！, ~ and ～, . and ．), several dashes and both the
 * long-vowel mark ー and the kanji 一, which look alike. The overlay shows and
 * looks up one form of each, the one vita-vn-ocr's scorer (eval/metrics.py
 * norm) reads as the same text, so a sentence on the console compares with the
 * evaluation directly:
 *
 *  - width: the full-width ASCII forms U+FF01-U+FF5E become ASCII, as NFKC does
 *    (！ -> !, １ -> 1, Ａ -> A, ～ -> ~);
 *  - tildes: 〜 (U+301C) and ∼ (U+223C) become ~;
 *  - dashes: — – ‐ ‑ ‒ − ─ ━ become ―, the Japanese dash; the ASCII hyphen stays;
 *  - ellipses: "..." becomes …, and a run of two or more ・ becomes … for every
 *    three of them (at least one); a lone ・ (in a name) stays;
 *  - ー or 一 by context: 一 becomes ー after a katakana (or ー) when katakana,
 *    a small kana, a mark or the end of the line follows ("コ一ヒ一"), and after
 *    a hiragana when a small kana, ー, ~, !, ?, an ellipsis, 」 or 』 follows
 *    ("だよ一っ"); ー between two kanji becomes 一 ("統ー的"). 一 before a kanji,
 *    a hiragana or a counter ヶ ヵ stays ("コーヒー一杯", "ケーキ一つ",
 *    "ハネムーン一ヶ月"), and so does one after a hiragana at the end of a line,
 *    which a wrapped line often is ("もう一 / 度").
 *  - counters: the small tsu ッ, which the recognizer reads for the small ke ヶ
 *    in some fonts (CLANNAD's 「九ヶ月です」 as "ルッ月です」", vita-vn-ocr #109),
 *    becomes ヶ before 月 or 年 whatever comes before it, before 国 or 条 after
 *    a numeral (〇一二三四五六七八九十百千万, 0-9) or 何 数 幾, and before 所
 *    after one of those other than 一 (一ッ所, ひとつところ, stays). ッ月 and ッ年
 *    occur nowhere in the 700 M characters of text vita-vn-ocr counted; ッ before
 *    日, 村 or 町 stays (三ッ日月, place names). It runs before the 一/ー rule, so
 *    "ハネムーン一ッ月" keeps its 一 before the counter. vita-vn-ocr's
 *    runtime/vocr_post.c vocr_post_counters() is the same rule
 *    (docs/counters.md there has the counts).
 *
 * Nothing else changes: whitespace, small kana (but the counter ッ above),
 * dakuten and lookalike kanji or kana are left as the recognizer read them
 * (those are recognition errors, not forms; the counter's is the one a text
 * rule can fix, as no text holds ッ there). The text never grows, so it is normalised in place. Lines (\n) are
 * normalised separately: a rule never looks across one. The game worker runs it
 * on every recognized text (vjo_text_normalize_keep), so no consumer (overlay,
 * subtitles, Anki, lookup) sees the recognizer's forms, and SceShell runs none
 * of it. */
#ifndef VJO_TEXTNORM_H
#define VJO_TEXTNORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned width;      /* full-width ASCII forms folded */
    unsigned tilde;      /* 〜 ∼ */
    unsigned dash;       /* dashes made ― */
    unsigned ellipsis;   /* "..." and ・ runs made … */
    unsigned long_vowel; /* 一 made ー */
    unsigned kanji_one;  /* ー made 一 */
    unsigned counter;    /* ッ made ヶ before a counter */
} VjoTextNorm;

/* Normalises the NUL-terminated UTF-8 text s in place (it never grows) and
 * returns the number of changes, each counted in *counts when it is not NULL.
 * Invalid UTF-8 bytes are kept as they are. */
unsigned vjo_text_normalize(char *s, VjoTextNorm *counts);

/* The same for the text in buf (cap bytes, NUL-terminated within them), which
 * also keeps raw (or, when raw is NULL, the text as it was) after the normalised
 * text's NUL, so one buffer carries both (the game worker returns its result
 * this way, the recognizer's own greedy text as raw, and the Shell logs it:
 * vjo_text_raw). A copy that does not fit is cut at a character. Every byte
 * after it is zeroed. raw may not overlap buf. Returns the changes. */
unsigned vjo_text_normalize_keep(char *buf, size_t cap, const char *raw, VjoTextNorm *counts);

/* The text kept by vjo_text_normalize_keep in buf (cap bytes), or "" when there
 * is none. */
static inline const char *vjo_text_raw(const char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap && buf[n]) n++;
    return n + 1 < cap ? buf + n + 1 : "";
}

#ifdef __cplusplus
}
#endif
#endif
