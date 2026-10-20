/* The characters of a recognized line as a Japanese reading tracker counts
 * them: the history's `characters` column (docs/ocr-history.md), which
 * tools/reading_stats.py also computes for the rows of files written before it.
 *
 * A character is a code point that is a Japanese letter (the letters of the
 * Han, Hiragana and Katakana scripts in core/utf.c's table: kanji, kana, ー, the
 * iteration marks, half-width katakana; the set the "Japanese characters only"
 * cursor stops on) or an ASCII letter or digit (the normalised line holds the
 * full-width forms ０-９Ａ-Ｚａ-ｚ as ASCII). Punctuation, brackets, symbols,
 * spaces and line breaks count nothing (「」。、…！？・―〜♪ and U+3000 too), and
 * a character counts once whatever its UTF-8 bytes or UTF-16 units. A byte that
 * does not start a well-formed, shortest-form sequence of one of them counts
 * nothing, as the U+FFFD a strict decoder makes of it would not.
 *
 * Inline: the Shell's history writer counts in the loop it already runs over
 * the text, and pays only the test (vjo_utf8_next and vjo_text_selectable are in
 * the Shell already). */
#ifndef VJO_READING_CHARS_H
#define VJO_READING_CHARS_H

#include <stddef.h>
#include <stdint.h>
#include "text_rows.h" /* vjo_text_selectable */
#include "utf.h"

/* Whether the code point cp, which vjo_utf8_next decoded from `bytes` bytes,
 * counts. One byte is ASCII, or an invalid byte (U+FFFD). A Japanese letter is at
 * U+3005 or above: 3 or 4 bytes, never an overlong form (the decoder takes
 * those), which vjo_text_selectable checks. */
static inline uint32_t vjo_reading_char(uint32_t cp, size_t bytes)
{
    return bytes > 1 ? (uint32_t)vjo_text_selectable(cp, bytes, 1)
                     : (uint32_t)(((cp | 0x20u) - 'a' < 26u) | (cp - '0' < 10u));
}

/* The characters of the len bytes at s. */
static inline uint32_t vjo_reading_characters(const char *s, size_t len)
{
    uint32_t n = 0;
    for (size_t i = 0, at = 0; at < len; at = i) {
        uint32_t cp = vjo_utf8_next(s, len, &i);
        n += vjo_reading_char(cp, i - at);
    }
    return n;
}

#endif
