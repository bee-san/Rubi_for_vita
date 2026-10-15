/* The Overlay app's Anki export (settings/anki_page.c runs it on its worker
 * thread): the queue's cards (ux0:data/VitaJPOverlay/anki_queue/<id>.json,
 * saved by the overlay's Enter, shell/anki_queue.c) as Lapis notes in a new
 * package ux0:data/VitaJPOverlay/anki/<YYYY-MM-DD_HHMMSS>-<N>cards.apkg
 * (core/apkg.h). The package is written to a temporary name, flushed,
 * closed, renamed and the memory card synced, then opened again and verified
 * (core/apkg.c vjo_apkg_verify); only then do its queued cards move into
 * anki_queue/exported/, and the queue is empty of them. A failure at any step
 * before that leaves the queue exactly as it was and no package. "All cards"
 * also takes the exported ones again: their notes have the same GUIDs, so
 * Anki adds only what it does not have.
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
    int unreadable;  /* left in the queue: damaged, edited, or a screenshot missing (log.txt) */
    int too_large;   /* left in the queue: no room in memory to make their note */
    int left;        /* over VJO_APKG_MAX_CARDS: the newest, left for the next export */
    int media;       /* screenshot files in it */
    uint32_t bytes;  /* its size */
    char path[VJO_EXPORT_PATH_MAX]; /* where it is */
    char name[64];   /* its file name */
} VjoExportResult;

/* Exports the queue (all: and the archive) into a new package; a one- or
 * two-sentence result for the screen in msg. Returns r->rc. */
int vjo_export_run(VjoArena *a, int all, const VjoExportUi *ui, VjoExportResult *r, char *msg, size_t cap);

#endif
