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
 *    a hiragana when a small kana, ー, ~, !, ?, an ellipsis, 」, 』 or the end of
 *    the line follows ("だよ一っ"); ー between two kanji becomes 一 ("統ー的").
 *    一 before a kanji or a hiragana stays, as in "コーヒー一杯" and "ケーキ一つ".
 *
 * Nothing else changes: whitespace, small kana, dakuten and lookalike kanji or
 * kana are left as the recognizer read them (those are recognition errors, not
 * forms). The text never grows, so it is normalised in place. Lines (\n) are
 * normalised separately: a rule never looks across one. */
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
} VjoTextNorm;

/* Normalises the NUL-terminated UTF-8 text s in place (it never grows) and
 * returns the number of changes, each counted in *counts when it is not NULL.
 * Invalid UTF-8 bytes are kept as they are. */
unsigned vjo_text_normalize(char *s, VjoTextNorm *counts);

#ifdef __cplusplus
}
#endif
#endif
