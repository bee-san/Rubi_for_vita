/* The dictionary lookup's result (the local dictionaries, local_dict.h). */
#ifndef VJO_DICT_H
#define VJO_DICT_H

/* No frequency rank: ranks start at 1. (It was 99999, jpdb's missing
 * frequency_rank; Jiten's global list ranks to 398,255, 99999 included.) */
#define VJO_NO_RANK 0
#define VJO_MAX_PITCH 4   /* pitch accents kept per word */

typedef struct {
    const char *spelling;
    const char *reading;  /* kana */
    int rank;             /* frequency rank, 1 = most frequent; VJO_NO_RANK if none */
    const char **meanings;
    int n_meanings;
    const char **pos;     /* part-of-speech codes: "n", "v5r", "adj-i", ... */
    int n_pos;
    int pitch[VJO_MAX_PITCH]; /* downstep mora, 0 = heiban */
    int n_pitch;
    /* Its dictionary's own name: a local file's title (page 0, "Bee's Ultimate
     * Dictionary"), NULL if the file has none (local_dict.c). */
    const char *dictionary;
} VjoVocab;

typedef struct {
    int vocab;          /* index into vocabulary, -1 if none */
    int pos16, len16;   /* UTF-16 units into the request text */
} VjoToken;

typedef struct {
    VjoVocab *vocab;
    int n_vocab;
    VjoToken *tokens;
    int n_tokens;
    /* The frequency rank's source, its name on an Anki card: a local v3
     * dictionary's frequency list ("Jiten"), so its entries are ranked or
     * explicitly unranked (VJO_NO_RANK); NULL if none. */
    const char *source;
} VjoDictResult;

#endif
