/* Where the overlay's own wrapping may break a line (owner, 2026-10-16, item
 * 4: "VNs normally don't split a word down the middle. We shouldn't either"):
 * between phrases, and by Japanese line-breaking rules (kinsoku). Used where
 * text is wrapped for the screen: the Shell's paf texts (the recognized text,
 * the popup, the subtitle strip: shell/wrap_fix.h moves a break paf put where
 * this forbids it back to the last place allowed here) and the Rubi app's
 * rows (settings/text_layout.c).
 *
 * A line may break before a character, after the one before it, when:
 *   - neither forbids it: no line starts with a closing mark, a small kana,
 *     ー, 々, 〜, ・, a dash or a pause (、。，．！？」』）】〕〉》…‥ and the
 *     like, and the recognized text's ASCII ! ? ~), none ends with an opening
 *     one (「『（【〔〈《); a space only ends a line, and no line breaks at a
 *     '\n' (it is a hard break already);
 *   - and it is after a space, a closing mark or a symbol (♪, ※, ①), or
 *     before an opening mark or a symbol;
 *   - or it is where a Japanese phrase starts after kana: a kanji or
 *     katakana after hiragana (彼女は|学校へ, あの|ジュース), except after the
 *     prefixes お and ご (お茶, ご飯), after one hiragana between kanji that is
 *     no particle (振り返る, 落ち着く; の を が に は と で も へ や か are), and
 *     before a suffix (あなた達, おばあ様: 様 達 君 殿 氏 等 共).
 * Nowhere else: never inside a kanji or katakana run, between kanji and its
 * kana (食べ|る), inside a kana run, a Latin word or a number, or between
 * Latin letters or digits and Japanese. So a line breaks between phrases or
 * at a space. It is a rule on the kinds of characters, no dictionary walk: a
 * compound can still break at its seam between kana and kanji (女の|子;
 * docs/touch-lookup.md "The overlay's own line breaks" has the share of
 * allowed places inside a dictionary entry). A text with no such place in a
 * line (one word wider than the line) keeps the break the layout chose. */
#ifndef VJO_LINEBREAK_H
#define VJO_LINEBREAK_H
#include <stddef.h>
#include <stdint.h>

/* Where the walk is: what the characters so far tell the next one. Zeroed:
 * a line's start (no break before its first character). */
typedef struct {
    uint8_t prev;   /* the last character's class, a mark that goes with its letter taking the letter's */
    uint8_t own;    /* its own class */
    uint8_t run;    /* hiragana at the end, up to 3 */
    uint8_t before; /* the class before them */
    uint32_t cp;    /* the last character */
} VjoBreakState;

/* Feeds the next code point; whether a line may break before it. */
int vjo_break_next(VjoBreakState *st, uint32_t cp);

/* The last place a line starting at unit `from` of UTF-16 text u (n units)
 * may break, in (from, to]: the unit the next line would start with, or 0
 * when there is none. A surrogate pair is one character; the rules see the
 * few characters before `from` too, back to a hard break. */
uint32_t vjo_break_last16(const uint16_t *u, uint32_t n, uint32_t from, uint32_t to);

/* The same in UTF-8 text s of len bytes: byte offsets. */
size_t vjo_break_last8(const char *s, size_t len, size_t from, size_t to);
#endif
