/* The offline Anki queue on the memory card: ux0:data/VitaJPOverlay/anki_queue/
 * holds <id>.json per card (a record, core/anki_queue.h) and, for a card saved
 * with its screenshot (API 23), <id>.jpg beside it, which the game worker wrote
 * through the kernel (include/vjo_game_ocr.h VJO_GAME_OCR_F_SHOT); SceShell never
 * holds its bytes. The Rubi app exports the queue as an .apkg and moves the
 * cards it holds, screenshots first, into anki_queue/exported/
 * (settings/anki_export.c); cards of earlier builds may also have
 * <id>.duplicate.json (a card an earlier build found in Anki). All calls are made by
 * the Anki thread, one at a time. Nothing reads the whole queue into memory: a
 * count scans the directory, a save writes one card. */
#ifndef VJO_SHELL_ANKI_QUEUE_H
#define VJO_SHELL_ANKI_QUEUE_H
#include "../core/anki_queue.h"

/* vjo_queue_save results */
enum {
    VJO_QUEUE_SAVED = 0,
    VJO_QUEUE_EXISTS = 1,    /* the same card is queued already */
    VJO_QUEUE_ARCHIVED = 2,  /* the same card was exported before (or an earlier build found it in Anki) */
    VJO_QUEUE_FAILED = -1,   /* storage error (logged with its SCE code) */
    VJO_QUEUE_TOO_LARGE = -2 /* over VJO_QUEUE_TEXT_MAX of text */
};

/* The card's screenshot (API 23): take(id) has <id>.jpg written in the queue
 * (anki.c: the game worker's shot) and returns 0, or why not (VJO_E_*); rc is
 * then what it returned, or VJO_QUEUE_FAILED when it returned 0 but left no
 * file (0 also when none was asked): the card was saved with it when rc is 0
 * and the save returned VJO_QUEUE_SAVED. */
typedef struct {
    int (*take)(const char id[VJO_QUEUE_ID_SIZE]);
    int rc;
} VjoQueueShot;

/* The number of queued cards, or -1 on an I/O error. */
int vjo_queue_count(void);
/* Saves the card. With shot (and shot->take), its screenshot first: the
 * record says so (picture: true) and is committed once the JPEG is on the card;
 * a screenshot that fails leaves no file and the card is saved without one.
 * The same card saved before with or without a screenshot is that card
 * (VJO_QUEUE_EXISTS or ARCHIVED: no screenshot is taken). A commit is the
 * record written to a temp file that is flushed and renamed, then the mount is
 * synced. Returns VJO_QUEUE_*. */
int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note, VjoQueueShot *shot);
#endif
