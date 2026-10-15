/* The letter a game title is listed under (the Games page's Left/Right) and
 * the catalog's order, which sorts by that letter first so that each letter's
 * titles are one run of the list. Pure C, no SDK calls. */
#ifndef VJO_SETTINGS_LETTER_H
#define VJO_SETTINGS_LETTER_H
#include <stdint.h>

/* An ASCII letter in upper case; '#' for any other ASCII start (digits,
 * symbols, a space) and for an empty title; for kana, the first kana of its
 * gojuon row: か for が and ご, あ for ゔ, か for ゕ and ゖ, with katakana apart
 * from hiragana (カ for ガ, ア for ヴ, カ for ヵ and ヶ); else the first
 * character itself, U+FFFD for a byte that does not start UTF-8. */
uint32_t vjo_title_letter(const char *title);

/* The catalog's order, as qsort compares: by letter ('#', then A-Z, then the
 * other letters by code point: hiragana rows, katakana rows, other
 * characters), then the titles with ASCII letters compared without case, then
 * their bytes (a capital before its small letter), then the title IDs. */
int vjo_title_order(const char *title_a, const char *id_a, const char *title_b, const char *id_b);
#endif
