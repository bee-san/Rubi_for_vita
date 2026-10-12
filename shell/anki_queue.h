/* The offline Anki queue on the memory card: ux0:data/VitaJPOverlay/anki_queue/
 * holds <id>.json (a record, core/anki_queue.h) and <id>.jpg (its optional
 * screenshot) per card; <id>.duplicate.json is a card Anki already had.
 * All calls are made by the Anki thread, one at a time. Nothing reads the
 * whole queue into memory: each call scans the directory or reads one card. */
#ifndef VJO_SHELL_ANKI_QUEUE_H
#define VJO_SHELL_ANKI_QUEUE_H
#include "../core/anki_queue.h"

/* vjo_queue_save results */
enum {
    VJO_QUEUE_SAVED = 0,
    VJO_QUEUE_EXISTS = 1,    /* the same card is queued already */
    VJO_QUEUE_ARCHIVED = 2,  /* the same card was sent before and Anki had it */
    VJO_QUEUE_FAILED = -1,   /* storage error (logged with its SCE code) */
    VJO_QUEUE_TOO_LARGE = -2 /* over VJO_QUEUE_TEXT_MAX of text, or the JPEG over its cap */
};

/* The number of queued cards, or -1 on an I/O error. */
int vjo_queue_count(void);
/* The same count from the same single directory scan, and in id the
 * smallest queued ID after `after` ("" = the smallest; id = "" when there is
 * none). */
int vjo_queue_next(const char *after, char id[VJO_QUEUE_ID_SIZE]);
/* Writes the card: its JPEG first, then the JSON record, each to a temp
 * file that is flushed and renamed, then the mount is synced. Returns
 * VJO_QUEUE_*. */
int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note, const VjoAnkiMedia *media);
/* Reads card id into note (and, with with_picture, its JPEG into media:
 * a card whose JPEG is missing then fails, so a screenshot is never lost
 * silently). Returns 0, or -1 (logged) for a missing, damaged, edited or
 * oversized card. */
int vjo_queue_load(VjoArena *a, const char *id, int with_picture, VjoAnkiNote *note, VjoAnkiMedia *media);
/* Only after Anki confirmed the card (a note ID, or findNotes found its
 * tag): the JSON goes first (and is synced), then the JPEG. */
int vjo_queue_remove(const char *id);
/* Anki refused the card as a duplicate: <id>.json becomes
 * <id>.duplicate.json, keeping its JPEG, for review. */
int vjo_queue_archive_duplicate(const char *id);
#endif
