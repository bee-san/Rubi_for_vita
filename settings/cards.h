/* The Rubi app's Cards page's storage (#315; settings/cards_page.c draws it): the queue's cards
 * (ux0:data/VitaJPOverlay/anki_queue/<id>.json and <id>.jpg, core/anki_queue.h) and the exported
 * ones (anki_queue/exported/, read only), listed newest first; a queued card deleted, its sentence
 * fixed or its picture dropped; anki_queue/deleted/ emptied.
 *
 * The rule behind every order here: a record never names a picture that is not there. A card's
 * picture is <id>.jpg, and its ID is its record's checksum (core/anki_queue.h), so a changed card
 * is a new card, with a new ID:
 *
 * - Delete moves <id>.json into anki_queue/deleted/ first, then <id>.jpg. A JPEG a stop leaves
 *   between the two is only a picture no card names, which the export's sweep removes
 *   (settings/anki_export.c remove_orphans). Nothing reads deleted/: not the export, not the Shell's
 *   find() (so a deleted card's word can be mined again), not the sweep. Empty deleted removes its
 *   files for good.
 * - Fix sentence and Drop picture: (0) an intent file deleted/<old>.fix naming the new ID,
 *   committed; (1) <old>.jpg copied to <new>.jpg.tmp and (2) renamed to <new>.jpg (one left by an
 *   earlier, stopped try is kept when it has the same bytes); (3) the new record committed (a
 *   temporary name, a rename, a sync of the memory card, as the Shell's save); (4) the old record
 *   moved into deleted/; (5) the old JPEG moved there too, then the intent file removed. Drop
 *   picture has no copy; a new record that exists already (queued, exported or an earlier build's
 *   <id>.duplicate.json: the Shell's find() set) skips steps 1-3. vjo_cards_recover, before the
 *   Cards page lists the queue and before an export scans it, finishes steps 4-5 for an intent
 *   whose new record exists (queued or exported; one only in deleted/ is an older copy, unless a
 *   later card of the same word and time is queued) and otherwise removes it and the new card's
 *   files, a card's change before its fixed card's: a stop anywhere leaves the old card or the new
 *   one, never neither, and never both for an export.
 *
 * Nothing here renames over a file. Every storage call that fails is logged (vjo_log, log.txt). The
 * calls are made by the Anki page's worker thread, one at a time (settings/anki_page.c). */
#ifndef VJO_SETTINGS_CARDS_H
#define VJO_SETTINGS_CARDS_H
#include <stddef.h>
#include <stdint.h>
#include "../core/anki_queue.h"
#include "../core/arena.h"
#include "anki_export.h"

#define VJO_CARDS_DELETED_DIR VJO_QUEUE_DIR "/deleted"

/* The list's table: the newest VJO_CARDS_MAX cards of a directory; older ones a window at a time,
 * each window a scan for the newest cards older than the one before's last. */
#define VJO_CARDS_MAX 2000
#define VJO_CARDS_GAMES 64 /* the distinct game names a window keeps (a row has its index) */
#define VJO_CARDS_GAME_BYTES 64

enum { VJO_CARDS_QUEUED = 0, VJO_CARDS_EXPORTED = 1 };

#define VJO_CARD_PICTURE 1u    /* its record says a picture goes with it */
#define VJO_CARD_UNREADABLE 2u /* not exportable: its name is not its checksum, a damaged record, a version this
                                * build does not know, or a read that failed (log.txt) */
#define VJO_CARD_DUPLICATE 4u  /* a word mined before: an earlier card, queued or exported, has it (#314's rule, the
                                * export's Duplicates: settings/anki_export.h) */
#define VJO_CARD_HELD 8u       /* a change of it did not finish (VjoCardsHeld): the export leaves it queued */
#define VJO_CARD_NO_GAME 0xFF  /* a row's game: none, or more games than the window keeps */

/* A row of the list: 128 B. word and reading are cut at a character when longer (the order uses
 * them as kept here; the word key, from the record's whole strings, finds the duplicates). */
typedef struct {
    uint8_t id[16];
    uint64_t key;   /* the word's key (core/word_key.h vjo_word_key), 0 for a record without a word */
    uint32_t time;  /* the Enter press, seconds since 1970 (UTC); 0: none */
    uint8_t flags;  /* VJO_CARD_* */
    uint8_t game;   /* its name's index in VjoCardList.games, or VJO_CARD_NO_GAME */
    char date[17];  /* "2026-10-14 17:12", the local time MiscInfo has; else the UTC day; "" */
    char word[44];
    char reading[37];
} VjoCardRow;

typedef struct {
    VjoCardRow *rows; /* the caller's table of cap rows */
    int cap, n;       /* rows kept, newest first */
    int total;        /* cards in the directory */
    int unreadable;   /* of the rows kept */
    int newer;        /* cards newer than the window (with a bound) */
    int older;        /* cards older than its last row: the next window's */
    int n_games;
    char games[VJO_CARDS_GAMES][VJO_CARDS_GAME_BYTES];
} VjoCardList;

/* -1, 0, 1: row a is newer than b, the same card, older (time, then word, reading and ID). */
int vjo_cards_order(const VjoCardRow *a, const VjoCardRow *b);

/* The old cards of the changes a recovery could not settle (an intent that cannot be read, a damaged one beside a
 * card that may be its new one, a move that keeps failing): an export leaves them queued, as the Cards page marks
 * them, so that a package never has an old card and its new one. */
#define VJO_CARDS_HELD_MAX 256
typedef struct {
    int n;
    int incomplete; /* more were to hold than fit, or deleted/ could not be read: an export then holds every card it
                     * cannot tell (it exports nothing queued: anki_export.c) */
    uint8_t id[VJO_CARDS_HELD_MAX][16];
} VjoCardsHeld;
/* Finishes or undoes what a stop left of a Fix sentence or Drop picture (deleted/<old>.fix): before every list and
 * export. An intent naming no usable new card (damaged, or not readable) is removed when its old card is not queued,
 * or, damaged, when no other queued card has the old card's word, reading and time (the change made no card); else
 * it is kept and its old card held. Returns the intents it could not settle (logged; 0 normally); held (may be NULL)
 * receives their old cards. */
int vjo_cards_recover(VjoArena *a, VjoCardsHeld *held);
/* id is one of held's. */
int vjo_cards_is_held(const VjoCardsHeld *held, const uint8_t id[16]);

/* The newest list->cap cards of the queue or exported/ (which), older than *bound when bound is
 * given (the window after the one that ended there), into list (newest first, duplicates marked);
 * each record read once, checked and decoded. ui reports progress and can stop it. 0, or VJO_E_*
 * (VJO_E_LOCAL_IO: the directory cannot be read; VJO_E_CANCELLED). Uses a only while it runs. */
int vjo_cards_list(VjoArena *a, int which, const VjoCardRow *bound, VjoCardList *list, const VjoExportUi *ui);
/* The files in deleted/ (each card is its record and its picture), or -1. */
int vjo_cards_deleted_files(void);

/* A card as the Cards page shows it. */
#define VJO_CARD_SENTENCE_MAX 6144 /* bytes kept of its sentence (the IME takes 2,048 UTF-16 units) */
#define VJO_CARD_DEFINITION_MAX 1024
typedef struct {
    uint8_t id[16];
    int which;               /* VJO_CARDS_QUEUED or _EXPORTED */
    unsigned flags;          /* VJO_CARD_* */
    int why;                 /* unreadable: VJO_CARD_WHY_* */
    char word[256], reading[256], game[VJO_CARDS_GAME_BYTES], date[17], dictionary[96];
    char sentence[VJO_CARD_SENTENCE_MAX];
    int hl_start, hl_end;    /* the word in sentence (byte offsets), -1 when none */
    int sentence_cut;        /* sentence is longer than VJO_CARD_SENTENCE_MAX: shown cut, not editable */
    char definition[VJO_CARD_DEFINITION_MAX]; /* its meanings, one a line, cut */
} VjoCardView;
enum { VJO_CARD_WHY_NONE, VJO_CARD_WHY_CHECKSUM, VJO_CARD_WHY_FORMAT, VJO_CARD_WHY_READ, VJO_CARD_WHY_GONE };

/* The card id of the queue or exported/ into v (its strings copied, a only while it runs). 0, or
 * VJO_E_*: VJO_E_LOCAL_IO when its record is not there (v->why VJO_CARD_WHY_GONE) or cannot be read. A
 * record that is there but not exportable gives 0 with VJO_CARD_UNREADABLE and v->why. */
int vjo_cards_load(VjoArena *a, int which, const uint8_t id[16], VjoCardView *v);
/* The card's picture, from its own directory else the other one (a queued card's can be in
 * exported/ after an export stopped between its two moves): a complete JPEG of at most
 * VJO_QUEUE_PICTURE_MAX bytes in a, or NULL. */
const uint8_t *vjo_cards_picture(VjoArena *a, int which, const uint8_t id[16], size_t *len);

/* What a change did. */
typedef struct {
    int done;          /* VJO_CARDS_* below */
    uint8_t id[16];    /* the card now: the new one (the old one's when unchanged) */
    int picture;       /* it has a picture */
    int word_found;    /* Fix sentence: the word is in the new sentence (bold), else no bold */
    int picture_lost;  /* Fix sentence: the old picture was missing or damaged, so the new card has none */
    int old_left;      /* the new card was made, but the old one could not be moved (intent kept: recovery) */
    int unsynced;      /* the new card was made, but the memory card did not confirm a write (its sync failed) */
    int exported;      /* refused (VJO_E_PARSE): the card is in exported/ too, so a change would be a second note */
    int pending;       /* refused (VJO_E_LOCAL_IO): the card is the new card of a change not settled yet */
    int held;          /* refused (VJO_E_LOCAL_IO): an earlier change of this card is not settled (its intent kept) */
} VjoCardChange;
enum {
    VJO_CARDS_CHANGED = 1,   /* a new card replaced the old one */
    VJO_CARDS_UNCHANGED = 2, /* the same record: nothing done */
    VJO_CARDS_EXISTED = 3    /* the new card was there already (queued, exported or duplicate): the old one retired */
};

/* Moves a queued card into deleted/: its record, then its picture (one an exported record of the same card still
 * names stays). 0, VJO_CARDS_E_PENDING (it is the new card of a change whose old card is not retired yet: nothing
 * moved; the recovery tries that first), or VJO_E_* (nothing moved, or only its record: then its picture is a JPEG no
 * card names, which the export sweeps). */
#define VJO_CARDS_E_PENDING 1
int vjo_cards_delete(VjoArena *a, const uint8_t id[16]);
/* Removes deleted/'s files for good (an intent file of a change not yet settled stays). The files
 * removed in *removed; 0, or VJO_E_LOCAL_IO (some could not be: logged). */
int vjo_cards_empty_deleted(VjoArena *a, int *removed);
/* A queued card with sentence in place of its own, its word found again in it (-1: none, no
 * bold). VJO_E_TOO_LARGE: the record would be over VJO_QUEUE_TEXT_MAX; VJO_E_PARSE: the card is not
 * readable. */
int vjo_cards_fix_sentence(VjoArena *a, const uint8_t id[16], const char *sentence, VjoCardChange *out);
/* A queued card without its picture. */
int vjo_cards_drop_picture(VjoArena *a, const uint8_t id[16], VjoCardChange *out);

/* The word's offsets in a new sentence: the same occurrence when the word occurs as often as
 * before, else the one nearest the old start; line breaks inside the word may be gone or new. 0 and
 * *start, *end, or -1 when the word is not in it. */
int vjo_cards_find_word(const char *old_sentence, int hl_start, int hl_end, const char *sentence, int *start,
                        int *end);

void vjo_cards_hex(const uint8_t id[16], char hex[VJO_QUEUE_ID_SIZE]);
int vjo_cards_unhex(const char *hex, uint8_t id[16]);
#endif
