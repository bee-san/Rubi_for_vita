/* The Rubi app's Anki export (settings/anki_page.c runs it on its worker
 * thread): the queue's cards (ux0:data/VitaJPOverlay/anki_queue/<id>.json,
 * saved by the overlay's Enter, shell/anki_queue.c) as Lapis notes in a new
 * package ux0:data/VitaJPOverlay/anki/<YYYY-MM-DD_HHMMSS>-<N>cards.apkg
 * (core/apkg.h). The package is written to a temporary name, flushed,
 * closed, renamed and the memory card synced, then opened again and verified
 * (core/apkg.c vjo_apkg_verify); only then do its queued cards move into
 * anki_queue/exported/, and the queue is empty of them. A failure at any step
 * before that leaves the queue exactly as it was and no package. "All cards"
 * also takes the exported ones again: their notes have the same GUIDs, so
 * Anki adds only what it does not have. After a verified package, the queue's
 * screenshots no card names (a save stopped between its JPEG and its record,
 * or during the JPEG's write) are removed (#161; anki_export.c remove_orphans).
 *
 * Every storage call that fails is logged (vjo_log, log.txt) with its error
 * code. All calls are made by one thread at a time. */
#ifndef VJO_SETTINGS_ANKI_EXPORT_H
#define VJO_SETTINGS_ANKI_EXPORT_H

#include <stddef.h>
#include <stdint.h>

#include "../core/apkg.h"
#include "app_prefs.h"

#define VJO_EXPORT_DATA_DIR "ux0:data/VitaJPOverlay"
#define VJO_EXPORT_QUEUE_DIR VJO_QUEUE_DIR
#define VJO_EXPORT_ARCHIVE_DIR VJO_QUEUE_EXPORTED_DIR
#define VJO_EXPORT_DIR VJO_EXPORT_DATA_DIR "/anki"
#define VJO_EXPORT_PATH_MAX 128
/* The export's memory: the card list (32 B a card) and one card's work. */
#define VJO_EXPORT_MEM (2u * 1024u * 1024u)
/* A screenshot no card names is removed only when its file time is this much older than the
 * package's: a save still finishing has its record within seconds of its JPEG (anki_export.c). */
#define VJO_EXPORT_ORPHAN_AGE_S (10 * 60)

typedef struct {
    int queued;   /* cards in the queue (-1: unreadable) */
    int exported; /* cards in the archive, exported before (-1: unreadable) */
} VjoExportCounts;

/* One directory scan each, no card read. */
void vjo_export_count(VjoExportCounts *c);

typedef struct {
    void *ud;
    void (*status)(void *ud, const char *msg); /* progress, for the screen */
    int (*stopped)(void *ud);                  /* the user cancelled */
} VjoExportUi;

typedef struct {
    int rc;          /* VJO_OK, or why there is no package (VJO_E_*) */
    int cards;       /* notes in the package */
    int moved;       /* queued cards it took out of the queue */
    int kept;        /* exported, but still queued: a move failed (the next export has them again, harmlessly) */
    int stopped;     /* exported, but still queued: the user stopped the clearing (the same) */
    int unreadable;  /* left in the queue: damaged/edited record, or an I/O failure (log.txt) */
    int too_large;   /* left in the queue: no room in memory to make their note */
    int missing_picture; /* exported without an absent/damaged screenshot; original record preserved */
    int left;        /* over VJO_APKG_MAX_CARDS: the newest, left for the next export */
    int swept;       /* screenshots no card names, removed from the queue (#161) */
    int media;       /* screenshot files in it */
    int duplicates;  /* #314: cards whose word an earlier card has: tagged duplicate (Tag) or left out (Skip) */
    int unchecked;   /* #314: 1 when the duplicates could not be checked (memory): no card was tagged */
    int known;       /* #314 stage C: cards tagged known (known_words.txt, imported on the Anki page) */
    int known_unchecked; /* 1 when the known words could not be read (memory, a damaged index): none tagged */
    uint32_t bytes;  /* its size */
    char path[VJO_EXPORT_PATH_MAX]; /* where it is */
    char name[64];   /* its file name */
} VjoExportResult;

/* Exports the queue (all: and the archive) into a new package, following the app's own settings (prefs, app.ini:
 * settings/app_prefs.h; NULL: the defaults); a one- or two-sentence result for the screen in msg. Returns r->rc.
 *
 * Duplicates (#314, prefs->anki_duplicates): a card is a duplicate when an earlier card, queued or exported, has its
 * word (core/word_key.h): earlier in the package's order, by the record's time, then its ID. Tag gives it Anki's
 * `duplicate` tag; Skip leaves it out of the package (a queued one stays queued, and is checked again by the next
 * export); Keep checks nothing, as before #314. */
int vjo_export_run(VjoArena *a, int all, const VjoAppPrefs *prefs, const VjoExportUi *ui, VjoExportResult *r,
                   char *msg, size_t cap);

/* ---- the words of every card (#314) ---- */

/* A card's word: its key (core/word_key.h), the first 64 bits of its ID (big-endian: its name's first 16 hex
 * digits), and its record's time (VJO_APKG_NO_TIME for records without one), the package's order. */
typedef struct {
    uint64_t key, id;
    uint32_t time;
    uint32_t flags; /* VJO_WORD_QUEUED: in the queue (else only in the archive) */
} VjoWord;
#define VJO_WORD_QUEUED 1u

typedef struct {
    VjoWord *w;      /* every card's word, in the arena, sorted by key, then time and ID */
    int n;
    int queued;      /* the queue's cards with a word */
    int duplicates;  /* queued cards whose word an earlier card has (the Anki page's "already mined") */
    int read;        /* records read (the others' words came from the cache) */
    int unreadable;  /* records that could not be read: no word (and not cached) */
} VjoWords;

/* The words of every card in the queue and the archive (into a), from the records, and from the cache of them that
 * this keeps in ux0:data/VitaJPOverlay/anki_words.bin (written again when a word was read; a missing or damaged
 * cache is rebuilt from the records). ui may be NULL (no progress, no stop). max_reads: at most this many records
 * are read (0: every one), so that the Anki page can count on its own thread without waiting long; the words read
 * are kept in the cache all the same. 0; 1 when more records than max_reads are not in the cache (out has no words);
 * VJO_E_LOCAL_IO when a directory cannot be read; VJO_E_OOM (no room in a for every card's word); VJO_E_CANCELLED
 * (ui->stopped). */
int vjo_export_words(VjoArena *a, const VjoExportUi *ui, int max_reads, VjoWords *out);
/* 1 when an earlier card (by time, then ID) than this one (its word, time and ID) has its word. */
int vjo_words_earlier(const VjoWords *w, uint64_t key, int64_t time, const uint8_t id[16]);

/* ---- known words (#314 stage C) ---- */

/* ux0:data/VitaJPOverlay/known_words.txt, which tools/known_words.py writes from an Anki collection: one word a line,
 * then (optionally) a TAB and its reading; UTF-8, a BOM, CRLF and blank lines allowed, a line whose first character
 * (after spaces) is '#' a comment, ASCII spaces, tabs and U+3000 around each field ignored, anything after a second TAB
 * ignored. The Anki page's Import reads it into the app's own index, anki_known.bin (the keys, sorted), which the
 * export reads: a card of a word in it gets the tag `known`. A line with a reading matches the card of that word
 * (vjo_word_key); a line without one, every card of that spelling (vjo_word_spelling_key). */
#define VJO_KNOWN_PATH VJO_EXPORT_DATA_DIR "/known_words.txt"
#define VJO_KNOWN_INDEX VJO_EXPORT_DATA_DIR "/anki_known.bin"
#define VJO_KNOWN_MAX 65536 /* words an import keeps (512 KiB of keys at an export) */
#define VJO_KNOWN_LINE_MAX 512 /* a longer line is no word (skipped, counted) */

/* One line (n bytes, without its newline; a CR at its end is dropped): 1 a word (its key in *key), 0 a blank line
 * or a comment, -1 not a word (an empty word, a control character or NUL, a field over VJO_KNOWN_LINE_MAX). */
int vjo_known_line(const char *line, size_t n, uint64_t *key);

typedef struct {
    int rc;        /* VJO_OK, or why the index is as it was (VJO_E_*) */
    int missing;   /* no known_words.txt: the index was removed (no known words) */
    int words;     /* lines that are words */
    int keys;      /* distinct keys in the index */
    int skipped;   /* lines that are not words (log.txt names the first ones) */
    int over;      /* words past VJO_KNOWN_MAX, left out */
} VjoKnownResult;

/* The Anki page's Import: known_words.txt into anki_known.bin (written under a temporary name, then in place of the
 * old one; nothing changes when the file cannot be read or the index cannot be written). A one-sentence result in
 * msg. Returns r->rc. */
int vjo_known_import(VjoArena *a, const VjoExportUi *ui, VjoKnownResult *r, char *msg, size_t cap);
/* The keys in anki_known.bin (its head only), or -1 when there is none or it is damaged. */
int vjo_known_count(void);

#endif
