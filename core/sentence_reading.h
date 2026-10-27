/* Sentence readings (#311): the readings of the words of an Anki card's sentence, which the Rubi app writes
 * into the card's SentenceFurigana at export (docs/anki.md, "Sentence readings"). The Rubi app only: the
 * Shell, the game worker and the kernel link none of this.
 *
 * The sentence is parsed as Yomitan's {sentence-furigana} parses it (its scanning parser, anki-note-builder.js
 * _getTextFurigana): from its start, the longest dictionary match at each position, with deinflection, in
 * local_dictionaries order: the overlay's own lookup (core/local_dict.h vjo_local_lookup_at), whose entry
 * gives the reading. A match's reading is placed on its text as written, as Yomitan's
 * distributeFuriganaInflected places it (japanese.js): its kanji read as in the dictionary form, up to where
 * the text's inflection starts (行って from 行く: 行[い]って), 来る, 良い/好い and 行く(ゆく) as conjugated
 * (core/furigana.h vjo_furigana_conjugated: 来[こ]ない, 来[き]ます). Kana never takes a reading.
 *
 * No guessing: a word gets no reading when
 * - no dictionary has it (its kanji stay as they are);
 * - another entry for the same text, about as frequent as the overlay's (Jiten's ranks: within
 *   VJO_READINGS_RATIO of it, or unranked beside an unranked one), reads its kanji otherwise (今日: きょう,
 *   こんにち; 方: ほう, かた): the "top entries disagree";
 * - it is a whole entry ending in kana (今日は: こんにちは) whose kanji, as a word of their own, are far more
 *   frequent and read otherwise (今日: きょう);
 * - it is a counter after a number (3人, 五本: the number changes its reading), and then the number neither;
 * - its reading does not fit the text (an okurigana the dictionary form spells otherwise).
 * A missing reading is better than a wrong one. */
#ifndef VJO_SENTENCE_READING_H
#define VJO_SENTENCE_READING_H

#include <stddef.h>

#include "anki.h"
#include "client.h"
#include "furigana.h"
#include "local_dict.h"

/* Top entries: as frequent as the overlay's entry within this factor of its Jiten rank. */
#define VJO_READINGS_RATIO 2
/* A whole entry ending in kana loses to its kanji part that is this much more frequent. */
#define VJO_READINGS_PART_RATIO 8
/* The scratch a card's sentence takes at most, besides the dictionary session (vjo_local_session_size): one
 * lookup's result and scratch budgets, and for a sentence of VJO_LOCAL_MAX_TEXT bytes its copies (2 x 4 KiB),
 * its readings (32 B a kanji, and their kana) and the lookups' configuration (host test
 * a_longest_sentence_fits_its_scratch). */
#define VJO_READINGS_SCRATCH (VJO_LOCAL_RESULT_BUDGET + VJO_LOCAL_SCRATCH_BUDGET + 80u * 1024u)

typedef struct {
    int ratio;      /* VJO_READINGS_RATIO */
    int part_ratio; /* VJO_READINGS_PART_RATIO; 0: no check of a whole entry's kanji part */
    int counters;   /* 1: a counter after a number gets no reading */
    int variants;   /* 1: a spoken shortening of the reading (ほんとに of 本当に) is no other reading */
    int anchor;     /* 1: a match from kana ending inside a run of kanji gives way to the run's own */
    int compounds;  /* 1: a word in a compound the lookup split (after kanji or katakana): every entry counts */
} VjoReadingRules;

/* What one sentence's (or an export's) parse did, for log.txt and the host's measurements. */
typedef struct {
    unsigned lookups;   /* dictionary lookups */
    unsigned words;     /* matched words with kanji */
    unsigned read;      /* ... given their reading */
    unsigned ambiguous; /* ... none: another entry as frequent reads them otherwise */
    unsigned part;      /* ... none: a whole entry whose kanji part reads otherwise */
    unsigned counters;  /* ... none: a counter after a number, or that number */
    unsigned unplaced;  /* ... none: the reading does not fit the text */
    unsigned unchecked; /* ... none: the other entries could not all be read (over the lookup's budget) */
    unsigned unmatched; /* kanji in no match */
    unsigned anchored;  /* matches from kana that gave way to their kanji run's (rules.anchor) */
    unsigned failed;    /* lookups that failed (an unreadable dictionary, a result over its budget) */
} VjoReadingStats;

/* The readings of the kanji of text[from, to) (its other bytes are never looked up), into out (at most cap,
 * in text order, their readings in keep): byte offsets into text, each a run of kanji or a whole word. The
 * lookups take scratch (released before it returns) and p's dictionaries (cfg: local_dictionary_dir and
 * local_dictionaries; p->local_session, if set, keeps them open between calls). rules NULL: the defaults
 * above. Returns the number of readings, or < 0 (VJO_E_*: a lookup failed, out has none). */
int vjo_sentence_rubies(VjoArena *keep, VjoArena *scratch, const VjoPlatform *p, const VjoConfig *cfg,
                        const VjoReadingRules *rules, const char *text, size_t from, size_t to, VjoRuby *out,
                        int cap, VjoReadingStats *st);

/* A word's readings on its text as written (surface, n bytes): its dictionary form spelling read reading,
 * placed as above (no lookup). Readings point into reading or are static. Returns how many (0: the word has
 * no kanji, or its reading does not fit), at most cap. */
int vjo_word_rubies(const char *surface, size_t n, const char *spelling, const char *reading, VjoRuby *out,
                    int cap);

/* Appends text[0, len) to b in Anki's furigana syntax (" 漢字[かな]"): each reading's base after a space,
 * none at the start or after a tag; text escaped as HTML, '[' and ']' too (no reading of Anki's furigana
 * filter starts in the text itself), a line break as <br>, bytes [hl_start, hl_end) in <b> (none when
 * hl_start < 0). r: sorted readings, offsets into text. */
void vjo_furigana_html(VjoBuf *b, const char *text, size_t len, const VjoRuby *r, int nr, int hl_start, int hl_end);

/* A card's SentenceFurigana (HTML, into a): its sentence joined as its Sentence field is (core/textjoin.h),
 * the mined word in <b> with the card's own reading, every other word's reading from the dictionaries
 * (vjo_sentence_rubies). "" when the sentence gets no reading; NULL on OOM or when the lookups failed (*rc
 * says which: VJO_E_*). scratch as above, plus the sentence's readings (a few KiB). */
char *vjo_sentence_furigana(VjoArena *a, VjoArena *scratch, const VjoPlatform *p, const VjoConfig *cfg,
                            const VjoReadingRules *rules, const VjoAnkiNote *n, VjoReadingStats *st, int *rc);

#endif
