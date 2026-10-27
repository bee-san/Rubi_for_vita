/* A word's key (#314): the same for every card of the same word, from every line, in every inflection, so that a
 * word mined before can be found again (the Rubi app's Duplicates at export, settings/anki_words.h, and its Cards
 * page).
 *
 * A card's record keeps the word's dictionary form (`spelling`: 食べる for 食べた) and its reading (`reading`). The
 * key is the first 64 bits of the SHA-256 of the two, folded, with a TAB between them:
 *   - katakana as hiragana (ァ-ヶ, ヽ and ヾ: コーヒー is こーひー; ヷ-ヺ have none and stay), in both;
 *   - the long vowel mark as written (コーヒー is not こおひい);
 *   - the middle dot ・ (and its half-width ･) left out, in both (ジョン・スミス is じょんすみす: dictionaries differ
 *     in writing it between a name's parts);
 *   - a missing reading is the spelling's (a dictionary of kana words leaves it empty).
 * Nothing else changes (kanji, other characters, invalid UTF-8 bytes: as they are). */
#ifndef VJO_WORD_KEY_H
#define VJO_WORD_KEY_H

#include <stdint.h>

#define VJO_WORD_KEY_HEX 17 /* 16 lowercase hex digits and a NUL */

/* The key of a word (NULL spelling or reading: ""). */
uint64_t vjo_word_key(const char *spelling, const char *reading);
/* The key of a spelling alone, whatever its reading (#314's known words: a line of known_words.txt without a
 * reading matches every card of its spelling): the first 64 bits of the SHA-256 of the folded spelling, without a
 * TAB, so that it is never a word's key (a spelling has no TAB). */
uint64_t vjo_word_spelling_key(const char *spelling);
/* The key as 16 lowercase hex digits, its most significant digit first (the SHA-256's first byte). */
void vjo_word_key_hex(uint64_t key, char out[VJO_WORD_KEY_HEX]);

#endif
