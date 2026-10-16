/* The recognized text's lines as the one running text a visual novel's
 * dialogue is (owner, 2026-10-16, item 4: "watashi wo tabe / ru kana?" is one
 * sentence, and the word at "tabe" is taberu). The game worker returns one
 * line per text row, '\n' between them; a break the game's own wrapping made
 * inside a sentence joins its two lines for the dictionary lookup (the scan
 * goes on past it, core/local_dict.c) and for an Anki card's sentence
 * (core/anki.c). The overlay still shows the lines as recognized.
 *
 * A game wraps a line only once it is full, so a break joins when the line
 * before it is as long as the text's longest line, within VJO_JOIN_SLACK
 * characters (a mark pushed to the next row or hanging past the box), and at
 * least VJO_JOIN_MIN_CHARS long, it does not end with a sentence's end or a
 * closing bracket (。．！？!?」』）】〕〉》］ and the like), and the line
 * after it does not start with an opening one (「『（【〔〈《［ and the like).
 * Every other break stays a break: after a speaker's name (a short line, or
 * one in brackets), at a sentence's end, or where the game broke a short line
 * by hand. Characters are code points, spaces at a line's ends not counted. */
#ifndef VJO_TEXTJOIN_H
#define VJO_TEXTJOIN_H
#include <stddef.h>

#define VJO_JOIN_MIN_CHARS 8
#define VJO_JOIN_SLACK 2
#define VJO_JOIN_MAX_BREAKS 32 /* vjo_text_join decides the first 32 breaks; later ones stay */

/* Whether the line break at byte `at` of text ('\n', or the '\r' of "\r\n")
 * joins the lines on either side of it. 0 for any other byte. */
int vjo_text_joins(const char *text, size_t len, size_t at);

/* Removes every joining break from the NUL-terminated text in place, as the
 * rule decides on the text before any is removed. Each of the n byte offsets
 * in offs (-1: none) follows its character; one inside a removed break moves
 * to where the break was. Returns the new length. */
size_t vjo_text_join(char *text, int *offs, int n);
#endif
