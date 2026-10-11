/* Portable note text helpers. Mining and sync are separate operations. */
#ifndef VJO_ANKI_H
#define VJO_ANKI_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "config.h"
#include "entries.h"


/* ---- field values (HTML, as Anki stores fields) ---- */

/* The text, escaped, with bytes [hl_start, hl_end) in <b>...</b> (none if
 * hl_start < 0) and line breaks as <br>. */
void vjo_anki_sentence(VjoBuf *b, const char *text, int hl_start, int hl_end);
/* "言葉[ことば]" (one reading for the whole word), or just the spelling when it
 * has no kanji or equals the reading. */
void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading);
/* <ol><li>meaning</li>...</ol>, escaped */
void vjo_anki_definition(VjoBuf *b, const char *const *meanings, int n);

/* Text borrowed from a pinned lookup result, or copied by note_from_entry. */
typedef struct {
    const char *spelling, *reading;
    const char **meanings;
    int n_meanings;
    const char *sentence; /* VjoEntryList.header */
    int hl_start, hl_end; /* the word in sentence, -1 if none */
} VjoAnkiNote;

int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out);

#endif
