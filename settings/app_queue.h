/* The Rubi app's writer of Anki cards into the queue (#313: a card mined on the Look up page), with the overlay's
 * rules (shell/anki_queue.c): one record per card, ux0:data/VitaJPOverlay/anki_queue/<id>.json (core/anki_queue.h,
 * version 4), its ID the SHA-256 of its bytes, so the export (settings/anki_export.c) takes it as any other card.
 *
 * A save is the overlay's without a screenshot: a card queued already, exported already (anki_queue/exported/) or
 * found in Anki by an earlier build (<id>.duplicate.json), with or without a picture, is that card and is not saved
 * again; else the record is written to <id>.json.tmp, flushed, closed and renamed to <id>.json, and the memory card
 * synced: a card is there only once it is whole, a temporary file is never a card, and a failed save never erases
 * one. Every failed storage call is logged (vjo_log, log.txt) with its error code. One thread at a time. */
#ifndef VJO_SETTINGS_APP_QUEUE_H
#define VJO_SETTINGS_APP_QUEUE_H
#include "../core/anki_queue.h"

/* vjo_app_queue_save's results: the overlay's (shell/anki_queue.h VJO_QUEUE_*). */
enum {
    VJO_APP_QUEUE_SAVED = 0,
    VJO_APP_QUEUE_EXISTS = 1,    /* the same card is queued already */
    VJO_APP_QUEUE_ARCHIVED = 2,  /* the same card was exported before (or an earlier build's AnkiConnect had it) */
    VJO_APP_QUEUE_FAILED = -1,   /* a storage error (logged) */
    VJO_APP_QUEUE_TOO_LARGE = -2 /* over VJO_QUEUE_TEXT_MAX of text, or no memory for it in a */
};

/* Saves the card, without a picture, its record encoded in a (two records' room: VJO_QUEUE_TEXT_MAX each); the
 * arena is as it was after. Returns VJO_APP_QUEUE_*; id (when not NULL) gets the card's ID. */
int vjo_app_queue_save(VjoArena *a, const VjoAnkiNote *note, char id[VJO_QUEUE_ID_SIZE]);

#endif
