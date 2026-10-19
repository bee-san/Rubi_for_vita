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
    uint32_t bytes;  /* its size */
    char path[VJO_EXPORT_PATH_MAX]; /* where it is */
    char name[64];   /* its file name */
} VjoExportResult;

/* Exports the queue (all: and the archive) into a new package; a one- or
 * two-sentence result for the screen in msg. Returns r->rc. */
int vjo_export_run(VjoArena *a, int all, const VjoExportUi *ui, VjoExportResult *r, char *msg, size_t cap);

/* The export's own steps, which the hand-over to Anki for Vita uses too (settings/anki_afv.c), so a
 * card is read, checked and archived there as it is here. */
/* fn for each <32 hex digits>.json card of the queue (archive 0) or the archive (1), until it
 * returns < 0: the cards' count, or -1 (a directory that cannot be read; one that is not there has
 * none). fn may be NULL (a count). */
int vjo_export_scan(int archive, int (*fn)(void *ud, const char *hex), void *ud);
/* A card's record, checked against its name (the ID is its checksum) and decoded into note (its
 * strings in a): 0, or -1 (logged). */
int vjo_export_read_card(VjoArena *a, int archive, const char *hex, VjoAnkiNote *note, int *picture);
/* Its screenshot, from either directory, a whole bounded JPEG (in a): NULL when neither copy is
 * (*unavailable: none is there or both are damaged, so the card goes without it; else a read that
 * failed or no memory, so it is tried again later). */
const uint8_t *vjo_export_read_jpeg(VjoArena *a, int archive, const char *hex, size_t *len, int *unavailable);
/* Moves a queued card into anki_queue/exported/ as an export does once its package is verified:
 * its JPEG first (when the queue has one), then its record. 0, or -1 (it stays queued; logged). */
int vjo_export_archive_card(VjoArena *a, const char *hex);

#endif
