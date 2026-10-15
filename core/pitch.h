/* Pitch accent: Yomitan's categories for a downstep, as an Anki card shows
 * them. */
#ifndef VJO_PITCH_H
#define VJO_PITCH_H

#include <stddef.h>

/* Morae of a kana reading: small ゃゅょぁぃぅぇぉゎ (and katakana) join
 * the kana before them. */
int vjo_morae(const char *reading);

/* Yomitan's category for a downstep: 0 heiban; a verb or i-adjective
 * (part-of-speech codes, as Yomitan's isNonNounVerbOrAdjective) otherwise
 * kifuku; 1 atamadaka; the reading's mora count odaka; else nakadaka. */
const char *vjo_pitch_category(const char *reading, int downstep, const char *const *pos, int n_pos);

#endif
