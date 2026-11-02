/* MAGES/5pb. SC3 script text (docs/native-hooks.md, "SC3 text"): the strings of STEINS;GATE (PCSG00147), STEINS;GATE
 * 比翼恋理のだーりん (PCSG00146), STEINS;GATE 線形拘束のフェノグラム (PCSG00250), ROBOTICS;NOTES ELITE (PCSG00352),
 * CHAOS;CHILD (PCSG00555) and STEINS;GATE 0 (PCSG00673), which their page function takes in r0.
 * Their bytes are no character encoding: a glyph is a big-endian 16-bit 0x8000 | index into the game's own font,
 * whose characters only a table of that game names (its charset, VjoSc3Pin); a byte below 0x80 is a control; 0xFF
 * ends the string. 0x00 is a line break and the low byte of many glyphs, so a string is read through its end token,
 * never to a NUL. Pure C, no allocation, no platform calls, no library: the game worker (game_ocr/hooks.c) measures a
 * hit with vjo_sc3_span in the game's thread and decodes it with vjo_sc3_decode at job time; host tests link it as is.
 *
 * The tokens, as the farm's decoder reads them (bee-san/psandvita scripts/sc3_codec.py) and as each game's page
 * parser walks them (PCSG00146 FUN_8102a118, PCSG00352 0x8100e1a6, PCSG00555 0x8102ebe6):
 *   0x80-0xFE, then one byte  a glyph: ((b0 & 0x7F) << 8 | b1), an index into the charset
 *   0xFF                      the end of the string
 *   0x0C, 0x11, 0x12, 0x13    a control with two argument bytes (margins, colours)
 *   0x04, 0x15                a control with an SC3 expression after it: tokens up to a 0x00 byte, each two bytes,
 *                             or with bit 7 set 2 + ((t & 0x60) >> 5) bytes
 *   any other byte < 0x80     a one-byte control: 0x00 line break, 0x01 speaker name, 0x02 its end (the line's
 *                             body), 0x09 ruby base, 0x0A its reading, 0x0B ruby end; the others draw nothing
 * The older adv6 engine of STEINS;GATE (PCSG00147, page parser 0x8102d576) reads two controls otherwise
 * (VJO_SC3_ADV6): 0x04 is a colour with one argument byte, not an expression, and 0x14 has two argument bytes. Read
 * as an expression, its 0x04 swallows the glyphs after it: 04 02 8244 822a 04 00 lost 「機関」 (the farm's run
 * paired-vita-pcsg00147-hf-20261027, event 21). */
#ifndef VJO_HOOK_SC3_H
#define VJO_HOOK_SC3_H

#include <stddef.h>
#include <stdint.h>

/* A game's charset file, ux0:data/VitaJPOverlay/hooks/<title>.sc3 (tools/sc3_charset.py): a 32-byte header
 * ("vjosc3" 0x00 0x01, the title ID in 12 bytes, the main module's NID and the glyph count, little-endian, 4 zero
 * bytes), then each glyph's character as a little-endian 16-bit code point (0: a glyph the table cannot name). */
#define VJO_SC3_HEADER 32u
#define VJO_SC3_MAX_BYTES 16384u /* the kernel's broker opens no larger one (VJO_GAME_OCR_OPEN_SC3) */

/* The charset file a build accepts for a title: its size and SHA-256 (core/hook_sc3_pins.inc, which
 * tools/sc3_charset.py writes beside the files); the worker checks both (game_ocr/hooks.c vjo_hooks_sc3_set). */
typedef struct {
    char title[12];      /* nine title-ID characters, NUL padded */
    uint32_t bytes;      /* the file's size: VJO_SC3_HEADER + 2 x its glyphs */
    uint8_t sha256[32];
} VjoSc3Pin;

/* The pin of title_id's charset, or NULL. */
const VjoSc3Pin *vjo_sc3_pin(const char *title_id);

/* Every pin (the host tests check them against the files): their number, the pins in *out. */
unsigned vjo_sc3_pins(const VjoSc3Pin **out);

/* The token rules (the dialect argument below): the adv7 engine's and its successors' (every SC3 title but one), or
 * STEINS;GATE's adv6 (VjoPlaceVerified.read VJO_PLACE_READ_SC3_ADV6). */
#define VJO_SC3_ADV7 0u
#define VJO_SC3_ADV6 1u

/* Hook time: the bytes of the SC3 string at p (n readable) through its 0xFF end token, or 0 when there is none
 * within them (or a token is cut by n). Reads only p[0..n). */
unsigned vjo_sc3_span(const uint8_t *p, unsigned n, unsigned dialect);

/* Job time: n bytes of an SC3 string -> NUL-terminated UTF-8 in dst (cap bytes, cap >= 1), up to its end token.
 * table: count little-endian 16-bit code points (a charset file's, after its header). A glyph is its character;
 * one the table cannot name (0, or an index past count) is dropped, never guessed. 0x00 is a line break; a
 * speaker's name (0x01 .. 0x02) is a line of its own; ruby keeps its base and drops its reading (0x0A .. 0x0B), as
 * Rubi's other filters do; every other control is dropped. A token cut by n ends the text; a character that does
 * not fit ends it at the last whole one. Returns the bytes before the NUL. */
size_t vjo_sc3_decode(const uint8_t *table, unsigned count, const uint8_t *src, size_t n, char *dst, size_t cap,
                      unsigned dialect);

#endif
