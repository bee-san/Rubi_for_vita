#ifndef VJO_UTF_H
#define VJO_UTF_H

#include <stddef.h>
#include <stdint.h>

/* Decodes one UTF-8 code point at s[*i] (advancing *i). Invalid bytes decode
 * as U+FFFD and consume one byte. */
uint32_t vjo_utf8_next(const char *s, size_t len, size_t *i);
/* Encodes cp into out (>= 4 bytes); returns byte count. */
int vjo_utf8_encode(uint32_t cp, char *out);
/* UTF-16 code units needed for code point cp. */
static inline int vjo_utf16_units(uint32_t cp) { return cp >= 0x10000 ? 2 : 1; }

/* A character cursor selects a whole decoded codepoint, never the end of
 * the text. direction < 0 / > 0 steps backward / forward; 0 normalizes an
 * arbitrary byte offset. Invalid bytes follow vjo_utf8_next's one-byte
 * replacement behavior. Empty text returns 0. */
size_t vjo_utf8_cursor_step(const char *s, size_t len, size_t offset, int direction);
/* The selected character's range in the styled header's UTF-16 units.
 * CR is omitted by that renderer and therefore has a zero-length range.
 * Returns 0 for empty text or an omitted CR. */
int vjo_utf8_cursor_range(const char *s, size_t len, size_t offset, uint32_t *start, uint32_t *units);

/* ASCII case-insensitive equality, and whether s starts with prefix. */
int vjo_ieq(const char *a, const char *b);
int vjo_ieq_prefix(const char *s, const char *prefix);

/* Java String.trim(): strips chars <= U+0020 from both ends, in place;
 * returns the (possibly advanced) start pointer. */
char *vjo_java_trim(char *s);

/* Japanese characters: [぀-ゟ゠-ヿ一-龯] */
static inline int vjo_is_japanese_char(uint32_t cp)
{
    return (cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
           (cp >= 0x4E00 && cp <= 0x9FAF);
}

#endif
