/* The offline Anki queue on the memory card: ux0:data/VitaJPOverlay/anki_queue/
 * holds <id>.json per card (a record, core/anki_queue.h). The Overlay app
 * exports the queue as an .apkg and moves the cards it holds into
 * anki_queue/exported/ (settings/anki_export.c); cards of earlier builds may
 * also have <id>.jpg (a screenshot) and <id>.duplicate.json (a card
 * AnkiConnect already had). All calls are made by the Anki thread, one at a
 * time. Nothing reads the whole queue into memory: a count scans the
 * directory, a save writes one card. */
#ifndef VJO_SHELL_ANKI_QUEUE_H
#define VJO_SHELL_ANKI_QUEUE_H
#include "../core/anki_queue.h"

/* vjo_queue_save results */
enum {
    VJO_QUEUE_SAVED = 0,
    VJO_QUEUE_EXISTS = 1,    /* the same card is queued already */
    VJO_QUEUE_ARCHIVED = 2,  /* the same card was exported before (or AnkiConnect had it) */
    VJO_QUEUE_FAILED = -1,   /* storage error (logged with its SCE code) */
    VJO_QUEUE_TOO_LARGE = -2 /* over VJO_QUEUE_TEXT_MAX of text */
};

/* The number of queued cards, or -1 on an I/O error. */
int vjo_queue_count(void);
/* Writes the card: the JSON record to a temp file that is flushed and
 * renamed, then the mount is synced. Returns VJO_QUEUE_*. */
int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note);
#endif
