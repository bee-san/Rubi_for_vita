/* Anki cards: what the overlay's Enter records about the word shown
 * (VjoAnkiNote, saved in the offline queue, core/anki_queue.h) and the HTML
 * a card puts in each field of its note, a Lapis note
 * (github.com/donkuri/lapis 1.7.0; core/lapis.h has the note type) in the
 * .apkg the Rubi app exports (core/apkg.h). The values are those Yomitan
 * writes for the fields Lapis's README lists. */
#ifndef VJO_ANKI_H
#define VJO_ANKI_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "entries.h"

/* The Lapis note type's fields, in its order (core/lapis_notetype.inc, which
 * host/tests checks against these). */
enum {
    VJO_LAPIS_EXPRESSION = 0,               /* the dictionary form */
    VJO_LAPIS_EXPRESSION_FURIGANA,          /* 言葉[ことば] ({furigana-plain}) */
    VJO_LAPIS_EXPRESSION_READING,           /* kana */
    VJO_LAPIS_EXPRESSION_AUDIO,             /* (none: no audio on the console) */
    VJO_LAPIS_SELECTION_TEXT,
    VJO_LAPIS_MAIN_DEFINITION,              /* the entry shown, as Yomitan writes one dictionary's */
    VJO_LAPIS_DEFINITION_PICTURE,
    VJO_LAPIS_SENTENCE,                     /* the recognized text, the word in <b> */
    VJO_LAPIS_SENTENCE_FURIGANA,            /* the same with readings, when it has any */
    VJO_LAPIS_SENTENCE_AUDIO,
    VJO_LAPIS_PICTURE,                      /* the card's screenshot, when it has one (core/lapis.c) */
    VJO_LAPIS_GLOSSARY,                     /* every dictionary's entry, with two or more */
    VJO_LAPIS_HINT,
    VJO_LAPIS_IS_WORD_AND_SENTENCE_CARD,
    VJO_LAPIS_IS_CLICK_CARD,
    VJO_LAPIS_IS_SENTENCE_CARD,
    VJO_LAPIS_IS_AUDIO_CARD,
    VJO_LAPIS_PITCH_POSITION,               /* [2], as Yomitan writes it */
    VJO_LAPIS_PITCH_CATEGORIES,             /* heiban,kifuku */
    VJO_LAPIS_FREQUENCY,                    /* <ul><li>SOURCE: 384</li></ul>, SOURCE the rank's (VjoDictResult.source) */
    VJO_LAPIS_FREQ_SORT,                    /* 384; 9999999 without one */
    VJO_LAPIS_MISC_INFO,                    /* the game and the date */
    VJO_LAPIS_FIELDS
};

/* The largest screenshot JPEG a card keeps (cards of earlier builds: this
 * build takes none). */
#define VJO_ANKI_PICTURE_MAX (192u * 1024u)
#define VJO_ANKI_MAX_MORE 7 /* other dictionaries' entries on a card (local_dictionary_results 8) */

/* ---- field values (HTML, as Anki stores fields) ---- */

/* The text, escaped, with bytes [hl_start, hl_end) in <b>...</b> (none if
 * hl_start < 0) and line breaks as <br>. */
void vjo_anki_sentence(VjoBuf *b, const char *text, int hl_start, int hl_end);
/* "言葉[ことば]" (one reading for the whole word), or just the spelling when it
 * has no kanji or equals the reading. */
void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading);
/* Downstep positions as Yomitan's PitchPosition: [2], each in spans; two
 * or more in <ol><li>...</li></ol>. Nothing for n = 0. */
void vjo_anki_pitch_position(VjoBuf *b, const int *pitch, int n);
/* Yomitan's pitch categories (vjo_pitch_category), comma-separated
 * without repeats. */
void vjo_anki_pitch_categories(VjoBuf *b, const char *reading, const int *pitch, int n, const char *const *pos,
                               int n_pos);
/* The dictionary a local definition names on its first line ("[JMdict]\n
 * ..."): its title in name (cap bytes) and the definition after that line;
 * name "" and the meaning itself when it names none. */
const char *vjo_anki_meaning_source(const char *meaning, char *name, size_t cap);

typedef struct {
    int year, month, day, hour, minute;
} VjoAnkiDate;
/* MiscInfo: "game (2026-10-11 14:32)"; the date alone without a game, the
 * game alone without a date (when = NULL). Cut on a character boundary. */
void vjo_anki_misc_info(char *out, size_t cap, const char *game, const VjoAnkiDate *when);

/* Another dictionary's entry for the word (a local page). */
typedef struct {
    const char *dictionary; /* its name, NULL if unknown */
    const char *spelling, *reading;
    const char **meanings;
    int n_meanings;
} VjoAnkiGloss;

/* One card, copied out of the result arena (which is reused while the
 * card is saved), or read back from the queue (anki_queue.h). */
typedef struct {
    const char *spelling, *reading;
    int rank; /* VJO_NO_RANK if unknown */
    const char **meanings;
    int n_meanings;
    const char *sentence; /* VjoEntryList.header: the whole recognized text (the Sentence field joins its lines) */
    int hl_start, hl_end; /* the word in sentence, -1 if none */
    const char *sentence_furigana; /* the sentence with every word's furigana, NULL = none */
    int sf_hl_start, sf_hl_end;    /* the word in sentence_furigana, -1 if none */
    const char *source;            /* the frequency rank's source (its list's name), NULL if unknown */
    int pitch[VJO_MAX_PITCH], n_pitch;
    const char **pos; /* part-of-speech codes */
    int n_pos;
    const char *misc; /* MiscInfo text, set by the caller; NULL = none */
    /* Record version 3 (NULL / 0 for older records): */
    /* the word's furigana as the dictionary gave it (ruby markup, "来[く]る"; only the removed online
     * dictionaries gave one, so only their queued records have it), NULL = from spelling and reading */
    const char *furigana;
    const char *dictionary; /* the shown entry's dictionary, NULL = from its definitions' first lines */
    const VjoAnkiGloss *more; /* the other dictionaries' entries */
    int n_more;
    const char *game, *title_id, *platform; /* "CLANNAD", "PCSG00415", "vita" | "psp" */
    int64_t unix_time;      /* when × was pressed, seconds UTC (misc has the Vita's local time); 0 unknown */
    const char *ocr_engine, *ocr_model; /* "vocr", "VOCR5_N_w8g.vocr" */
} VjoAnkiNote;

/* The note of entry `entry` of the shown list (always with the sentence's
 * furigana when memory allows; without it the note is still made). */
int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out);

/* The HTML of Lapis field f (VJO_LAPIS_*) for the note: "" when the card has
 * nothing for it (and for the Picture, which core/lapis.c fills), NULL on
 * OOM. */
char *vjo_anki_field_value(VjoArena *a, int f, const VjoAnkiNote *n);

#endif
