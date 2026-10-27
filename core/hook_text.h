/* Text a native hook copied out of a game, decoded to UTF-8 (docs/native-hooks.md).
 * Plain C, no allocation, no platform calls: the game worker uses it at job time,
 * in its own thread, never inside a hooked game function; host tests link it as is. */
#ifndef VJO_HOOK_TEXT_H
#define VJO_HOOK_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* The encoding of a hook's raw bytes. AUTO (the CALL source's string copies): valid
 * UTF-8 with at least one non-ASCII character is UTF-8, else CP932. SC3: MAGES' script
 * text, glyph indices into a game's own charset (core/hook_sc3.h), which only
 * game_ocr/hooks.c decodes (it holds the charset): vjo_hook_decode gives no text for it. */
enum {
    VJO_HOOK_ENC_AUTO = 0,
    VJO_HOOK_ENC_UTF8 = 1,
    VJO_HOOK_ENC_UTF16LE = 2,
    VJO_HOOK_ENC_CP932 = 3,
    VJO_HOOK_ENC_SC3 = 4,
    VJO_HOOK_ENC_COUNT
};

/* Decodes n raw bytes (stopping at the first NUL unit: one 0 byte, or for UTF-16LE
 * one 0x0000 unit) into NUL-terminated UTF-8 in dst (cap bytes, cap >= 1). A byte
 * sequence that is not valid in the encoding is dropped (never copied through), a
 * character that does not fit ends the text at the last whole character. CP932's
 * single byte 0xA0, which Shift_JIS leaves undefined, is VJO_HOOK_CP932_A0, as in
 * Windows' cp932 (and so in Luna's filters): a filter may make it what its game
 * draws there, and vjo_hook_clean drops it otherwise. Returns the bytes written
 * before the NUL; *used, when not NULL, gets the encoding that was decoded (AUTO
 * resolved to UTF8 or CP932). */
#define VJO_HOOK_CP932_A0 0xF8F0u /* a private-use code point (UTF-8 EF A3 B0) */
size_t vjo_hook_decode(unsigned encoding, const uint8_t *src, size_t n, char *dst, size_t cap,
                       unsigned *used);

/* Job time, AFTER any game-specific filter: remove common backslash controls
 * from decoded UTF-8 in place. Numeric \s and \w commands lose their decimal
 * argument; literal \n/\r/\t become whitespace, \c/\k/\p/\f are dropped.
 * Unknown escapes and doubled backslashes stay literal. Nonprinting ASCII
 * controls are dropped, except real tab/CR/LF, and so is VJO_HOOK_CP932_A0 (the
 * decoder's 0xA0 that no filter mapped). At most n bytes are read, stopping
 * at NUL; text has room for n + 1 bytes. Returns the new length and terminates it.
 * No allocation, and the result never grows. */
size_t vjo_hook_clean(char *text, size_t n);

/* Japanese characters (hiragana, katakana, CJK ideographs, the CJK symbols and
 * fullwidth forms blocks, the iteration and prolonged sound marks) in n bytes of
 * UTF-8; invalid sequences count as none. */
unsigned vjo_hook_japanese(const char *utf8, size_t n);

/* A cheap test for the CALL source's hook path (a game thread, inside the hooked
 * string function; its copies are UTF-8 or CP932, never UTF-16): may these n raw
 * bytes hold Japanese text? 1 when a byte >= 0x81 is among the first min(n, 8)
 * bytes (a CP932 lead byte or a UTF-8 lead/continuation byte), else 0. No
 * decoding: the worker decodes and judges later. */
static inline int vjo_hook_maybe_text(const uint8_t *p, size_t n)
{
    size_t k = n < 8 ? n : 8;
    for (size_t i = 0; i < k; i++)
        if (p[i] >= 0x81) return 1;
    return 0;
}

#endif
