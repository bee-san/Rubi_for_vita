/* The Lapis note type (github.com/donkuri/lapis 1.7.0, GPL-3.0;
 * third_party/lapis, generated into core/lapis_notetype.inc by
 * tools/gen_lapis.py) and a card's note of it: the values Yomitan writes, as
 * Lapis's README asks for each field (core/anki.h VJO_LAPIS_*), and the
 * screenshot when the card has one. */
#ifndef VJO_LAPIS_H
#define VJO_LAPIS_H

#include "anki.h"

/* The fields' names (VJO_LAPIS_* order), from core/lapis_notetype.inc. */
extern const char *const vjo_lapis_field[VJO_LAPIS_FIELDS];

/* The note type, its deck and the tag on every note. */
extern const long long vjo_lapis_notetype_id;
extern const long long vjo_lapis_deck_id;
extern const char vjo_lapis_deck_name[];
extern const char vjo_lapis_tag[];
/* The note type as schema-11 JSON (Anki's NotetypeSchema11). */
extern const char *const vjo_lapis_notetype;

/* The HTML of field f for the note: "" when it has nothing for it, NULL on
 * OOM. picture: the card's screenshot's media file name, or NULL. */
char *vjo_lapis_field_value(VjoArena *a, int f, const VjoAnkiNote *n, const char *picture);
/* All the note's fields joined by 0x1F, as Anki stores them (notes.flds). */
char *vjo_lapis_fields(VjoArena *a, const VjoAnkiNote *n, const char *picture, size_t *len);

#endif
