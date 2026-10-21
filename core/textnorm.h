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
 *    which a wrapped line often is ("もう一 / 度");
 *  - counters: the small tsu ッ, which the recognizer reads for the small ke ヶ
 *    in some fonts (CLANNAD's 「九ヶ月です」 as "ルッ月です」", vita-vn-ocr #109),
 *    becomes ヶ before 月 or 年 whatever comes before it, before 国 or 条 after
 *    a numeral (〇一二三四五六七八九十百千万, 0-9) or 何 数 幾, and before 所
 *    after one of those other than 一 (一ッ所, ひとつところ, stays). ッ月 and ッ年
 *    occur nowhere in the 700 M characters of text vita-vn-ocr counted; ッ before
 *    日, 村 or 町 stays (三ッ日月, place names). It runs before the 一/ー rule, so
 *    "ハネムーン一ッ月" keeps its 一 before the counter. vita-vn-ocr's
 *    runtime/vocr_post.c vocr_post_counters() is the same rule
 *    (docs/counters.md there has the counts);
 *  - a line made only of a box's border or scene art read as text goes, with
 *    its line break: three or more code points, each one of ー ニ 一 二 ― - ~ ・
 *    (the forms the rules above leave), at least two different ones, and ー or
 *    ニ among them ("ーニーーーーニーニー…"). The kanji numerals ("一二三"), a
 *    pause ("――――", "ーーー"), a word ("ニート") and a line with anything else
 *    on it ("「ーー」", "ニーー!") stay. Valid text is very rarely such a line
 *    (docs/vita-vn-ocr.md), so the rule follows config.ini's non_japanese_filter:
 *    lines, the default, drops it; none keeps it (VJO_TEXTNORM_KEEP_BORDERS).
 *
 * Nothing else changes: whitespace, small kana (but the counter ッ above),
 * dakuten and lookalike kanji or kana are left as the recognizer read them
 * (those are recognition errors, not forms; the counter's is the one a text
 * rule can fix, as no text holds ッ there). The text never grows, so it is normalised in place. Lines (\n) are
 * normalised separately: a rule never looks across one, and a line is dropped
 * whole or not at all. The game worker runs it on every recognized text
 * (vjo_text_normalize_keep), so no consumer (overlay, subtitles, Anki, lookup)
 * sees the recognizer's forms, and SceShell runs none of it. */
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
    unsigned garbage;    /* lines of a border read as text dropped */
} VjoTextNorm;

/* The rule that may be left out: the border rule, the only one that drops a
 * line. The game worker leaves it out when config.ini says non_japanese_filter =
 * none (include/vjo_game_ocr.h VJO_GAME_OCR_S_DROP_BORDERS). */
#define VJO_TEXTNORM_KEEP_BORDERS 1u

/* Normalises the NUL-terminated UTF-8 text s in place (it never grows) and
 * returns the number of changes, each counted in *counts when it is not NULL;
 * a line dropped is one change. Invalid UTF-8 bytes are kept as they are.
 * flags: 0, every rule, or VJO_TEXTNORM_KEEP_BORDERS. */
unsigned vjo_text_normalize_as(char *s, unsigned flags, VjoTextNorm *counts);

/* Every rule. */
static inline unsigned vjo_text_normalize(char *s, VjoTextNorm *counts)
{
    return vjo_text_normalize_as(s, 0u, counts);
}

/* The same for the text in buf (cap bytes, NUL-terminated within them), which
 * also keeps raw (or, when raw is NULL, the text as it was) after the normalised
 * text's NUL, so one buffer carries both (the game worker returns its result
 * this way, the recognizer's own greedy text as raw, and the Shell logs it:
 * vjo_text_raw). A copy that does not fit is cut at a character. Every byte
 * after it is zeroed. raw may not overlap buf. Returns the changes. */
unsigned vjo_text_normalize_keep(char *buf, size_t cap, const char *raw, unsigned flags, VjoTextNorm *counts);

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
