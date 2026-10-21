/* Overlay content model: header text + dictionary entries, one per
 * occurrence of a word (a word used twice is navigated to twice), with its
 * byte range in the header. */
#ifndef VJO_ENTRIES_H
#define VJO_ENTRIES_H

#include "arena.h"
#include "dict.h"

typedef struct {
    const VjoVocab *vocab;
    int pos16;              /* token position in the lookup text, -1 if none */
    int hl_start, hl_end;   /* byte range in VjoEntryList.header, -1 if none */
    const char *text;       /* "spelling (reading) #rank\nmeaning..." (shared by a word's entries) */
    uint32_t conjugation;   /* this occurrence's VjoToken.conjugation */
} VjoEntry;

typedef struct {
    const char *header;     /* filtered OCR text, trimmed */
    VjoEntry *entries;
    int n_entries;
    const char *source;     /* VjoDictResult.source, NULL without a lookup */
} VjoEntryList;

/* `filtered` is the non-Japanese-filtered OCR text (before newline removal);
 * token positions refer to vjo_strip_newlines(filtered). Entries are in
 * text order: one per token with a word, then words without a token (in
 * the dictionary's order). */
int vjo_entries_build(VjoArena *a, const char *filtered, const VjoDictResult *r, VjoEntryList *out);

/* Is entry i one of those with text in the header, less one overlapping
 * the one before (in text order)? */
int vjo_entry_in_text(const VjoEntryList *l, int i);

/* Entry format: spelling + " (reading)" if different + the frequency badge,
 * then "\n" + each meaning. */
char *vjo_entry_format(VjoArena *a, const VjoVocab *v, const char *source);

/* The frequency badge: " #1234" for a ranked entry, " unranked" for one whose
 * dictionary has a frequency list (source, VjoDictResult.source) that does not
 * rank it, "" otherwise. Written to buf, VJO_RANK_BADGE_MAX bytes. */
#define VJO_RANK_BADGE_MAX 16
const char *vjo_rank_badge(char *buf, int rank, const char *source);

/* Body text: entries joined by a blank line. */
char *vjo_entries_body(VjoArena *a, const VjoEntryList *l);

/* Bee's Ultimate Dictionary ends an entry's text with one line per distinct
 * kanji of its spelling, "漢=meaning, meaning" (bee-san/plaintext-dicts
 * tools/kanji_lines.py; no other line of its texts has that shape). The byte
 * offset in text where that block of trailing lines starts: each is one CJK
 * ideograph (U+3400-9FFF, the compatibility ideographs U+F900-FAFF or
 * U+20000-3FFFF, a whole code point), then '=' and at least one more byte.
 * strlen(text) when the text ends otherwise (any other dictionary, a kana
 * word). */
size_t vjo_kanji_block(const char *text);

#endif
