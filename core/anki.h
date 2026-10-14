/* Anki notes over AnkiConnect (API version 6, plain HTTP on the LAN), for
 * one note type: Kiku (github.com/youyoumu/kiku), with the field names of
 * its v2.1.0 release (docs/anki.md pins it), in one deck. There is no field
 * mapping, deck or note type to choose. Where Anki is (anki_host) is parsed
 * by vjo_anki_endpoint in config.h. */
#ifndef VJO_ANKI_H
#define VJO_ANKI_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "config.h"
#include "entries.h"
#include "json.h"
#include "net.h"

#define VJO_ANKI_NOTE_TYPE "Kiku"
#define VJO_ANKI_DECK "Kiku"        /* created with the first card if missing */
#define VJO_ANKI_TAG "vita-jp-overlay"

/* The Kiku v2.1.0 fields a card fills (the others stay empty: Kiku's
 * card-type switches, SelectionText, translations, sentence audio). */
enum {
    VJO_KIKU_EXPRESSION = 0,          /* the dictionary form */
    VJO_KIKU_EXPRESSION_FURIGANA,     /* 言葉[ことば] */
    VJO_KIKU_EXPRESSION_READING,      /* kana */
    VJO_KIKU_EXPRESSION_AUDIO,        /* the word's recording (anki_audio_url), downloaded by Anki */
    VJO_KIKU_MAIN_DEFINITION,         /* the entry shown, as Yomitan writes one dictionary's */
    VJO_KIKU_SENTENCE,                /* the recognized text, the word in <b> */
    VJO_KIKU_SENTENCE_FURIGANA,       /* the same in 漢字[かんじ] syntax */
    VJO_KIKU_PICTURE,                 /* the card's screenshot, when it has one */
    VJO_KIKU_GLOSSARY,                /* every dictionary's entry, with two or more */
    VJO_KIKU_PITCH_POSITION,          /* [2], as Yomitan writes it */
    VJO_KIKU_PITCH_CATEGORIES,        /* heiban,kifuku */
    VJO_KIKU_FREQUENCY,               /* <ul><li>Jiten: 384</li></ul> */
    VJO_KIKU_FREQ_SORT,               /* 384 */
    VJO_KIKU_MISC_INFO,               /* the game and the date */
    VJO_KIKU_FIELDS
};
/* Their names in the note type (index VJO_KIKU_*). */
extern const char *const vjo_kiku_field[VJO_KIKU_FIELDS];

#define VJO_ANKI_MAX_RESPONSE (64u * 1024u)
/* The largest screenshot JPEG sent with a note (it is streamed as base64,
 * never encoded whole); a larger one is left out. */
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
 * note is being sent), or read back from the queue (anki_queue.h). */
typedef struct {
    const char *spelling, *reading;
    int rank; /* VJO_NO_RANK if unknown */
    const char **meanings;
    int n_meanings;
    const char *sentence; /* VjoEntryList.header: the whole recognized text */
    int hl_start, hl_end; /* the word in sentence, -1 if none */
    const char *sentence_furigana; /* the sentence with every word's furigana, NULL = none */
    int sf_hl_start, sf_hl_end;    /* the word in sentence_furigana, -1 if none */
    const char *source;            /* the frequency's source ("Jiten"), NULL if unknown */
    int pitch[VJO_MAX_PITCH], n_pitch;
    const char **pos; /* part-of-speech codes */
    int n_pos;
    const char *misc; /* MiscInfo text, set by the caller; NULL = none */
    const char *queue_tag; /* optional stable tag for retry-safe queued sends */
    /* Record version 3 (NULL / 0 for older records): */
    const char *furigana;   /* the word's furigana as the dictionary gave it, NULL = from spelling and reading */
    const char *dictionary; /* the shown entry's dictionary, NULL = from its definitions' first lines */
    const VjoAnkiGloss *more; /* the other dictionaries' entries */
    int n_more;
    const char *game, *title_id, *platform; /* "CLANNAD", "PCSG00415", "vita" | "psp" */
    int64_t unix_time;      /* when × was pressed, seconds UTC (misc has the Vita's local time); 0 unknown */
    const char *ocr_engine, *ocr_model; /* "vocr", "VOCR3_H15_w8.vocr" */
} VjoAnkiNote;

/* The note of entry `entry` of the shown list (always with the sentence's
 * furigana when memory allows; without it the note is still made). */
int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out);

/* What a card adds besides the entry's text (NULL / 0 = none): the
 * screenshot, a JPEG sent in the request, and the word's audio, a URL Anki
 * downloads. */
typedef struct {
    const uint8_t *picture;
    size_t picture_len;
    const char *picture_name;
    const char *audio_url, *audio_name;
} VjoAnkiMedia;

/* "vitajp_<unix ms>.<ext>" */
void vjo_anki_media_name(char *out, size_t cap, uint64_t unix_ms, const char *ext);

/* ---- requests ---- */

/* A request body: prefix, then `picture` base64-encoded (when picture_len is
 * not 0), then suffix. */
typedef struct {
    const char *prefix;
    size_t prefix_len;
    const uint8_t *picture;
    size_t picture_len;
    const char *suffix;
    size_t suffix_len;
} VjoAnkiBody;

/* The HTML of Kiku field f (VJO_KIKU_*, not a media one) for the note; ""
 * when it has nothing for it, NULL on OOM. */
char *vjo_anki_field_value(VjoArena *a, int f, const VjoAnkiNote *n);
/* canAddNotesWithErrorDetail for the first n entries, each word in
 * Expression; NULL on OOM. */
char *vjo_anki_can_add_request(VjoArena *a, const VjoEntryList *l, int n);
/* addNote, with the media the note has. With a picture, the body ends in it
 * (out->picture/picture_len, base64-encoded when sent). Returns 0, or -1 on
 * OOM. */
int vjo_anki_add_request(VjoArena *a, const VjoAnkiNote *n, const VjoAnkiMedia *m, VjoAnkiBody *out);

/* ---- word audio ----
 * anki_audio_url is a Yomitan custom audio source: it lists recordings of
 * a word, best first. */
#define VJO_ANKI_AUDIO_MAX_RESPONSE (32u * 1024u)

/* anki_audio_url is set. */
int vjo_anki_audio_enabled(const VjoConfig *cfg);
/* The request path: the URL's path template (see vjo_anki_audio_endpoint)
 * with {term} and {reading} percent-encoded in ({language}: ja). NULL on
 * OOM. */
char *vjo_anki_audio_path(VjoArena *a, const char *path_template, const char *term, const char *reading);
/* The first http(s) URL of an audioSourceList, or NULL in *url when it
 * has none. */
int vjo_anki_audio_parse(VjoArena *a, const char *json, size_t len, const char **url);
/* The audio file extension of a URL ("opus", ...), "mp3" when it has none
 * that is known (text-to-speech). */
const char *vjo_anki_audio_ext(const char *url);

/* ---- calls (one connection each) ----
 * Results: VJO_OK; VJO_E_ANKI_DUPLICATE or VJO_E_ANKI (VjoErr.detail) when
 * AnkiConnect refuses; VJO_E_PARSE when the reply is not AnkiConnect's;
 * or a transport error (VJO_E_NET, VJO_E_STATUS, ...). */

/* VJO_OK if host:port is AnkiConnect (version 6). */
int vjo_anki_probe(VjoArena *a, const VjoPlatform *p, const char *host, int port, VjoErr *err);
/* Sends a vjo_anki_can_add_request body for n entries: marks[i] = 1 for an
 * entry already in the deck, 0 otherwise (addable, or refused for another
 * reason, such as a deck that does not exist yet). */
int vjo_anki_check(VjoArena *a, const VjoPlatform *p, const char *host, int port, const char *request,
                   uint8_t *marks, int n, VjoErr *err);
/* addNote with the media (see vjo_anki_add_request); a missing deck is
 * created, then the note is sent again. */
int vjo_anki_add(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiNote *n,
                 const VjoAnkiMedia *m, VjoErr *err);

/* Check the queue's unique tag before retrying an ambiguous addNote response.
 * found is set only for a valid array of positive note IDs. */
int vjo_anki_find_queued(VjoArena *a, const VjoPlatform *p, const char *host, int port,
                         const char *tag, int *found, VjoErr *err);

/* One action's reply: {"result": ..., "error": ...} with the result token
 * in *result (j holds the parse); the call results above. For the Overlay
 * app's connection test (anki_app.h). */
int vjo_anki_call(VjoArena *a, const VjoPlatform *p, const char *host, int port, const char *json, VjoJson *j,
                  int *result, VjoErr *err);

/* Asks the audio source (anki_audio_url) for the word: its best recording
 * in m->audio_url and m->audio_name (both NULL when there is none, or on
 * an error). Only those two strings stay in the arena. The source's
 * errors are transport ones (VJO_E_NET, VJO_E_TLS, VJO_E_STATUS, ...) or
 * VJO_E_PARSE (not an audioSourceList, or an invalid anki_audio_url). */
int vjo_anki_find_audio(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const VjoAnkiNote *n,
                        uint64_t unix_ms, VjoAnkiMedia *m, VjoErr *err);
/* Why there is no audio, after vjo_anki_find_audio: "no recording" for
 * VJO_OK, else the error ("HTTP 401", ...). */
const char *vjo_anki_audio_err_text(VjoArena *a, const VjoErr *err);

/* Short user-facing description of a call's result ("" for VJO_OK). */
const char *vjo_anki_err_text(VjoArena *a, const VjoErr *err);
/* After this result from a saved (auto-found) host, search the network
 * again: anything but an answer from AnkiConnect itself (the Overlay app). */
int vjo_anki_host_stale(int rc);

/* The candidate hosts of a network search: the /24 around ip (or the
 * smaller subnet of mask), without the network, broadcast and own
 * addresses, in ascending order. Addresses in host byte order. Returns the
 * count (at most cap). */
int vjo_anki_scan_candidates(uint32_t ip, uint32_t mask, uint32_t *out, int cap);

#endif
