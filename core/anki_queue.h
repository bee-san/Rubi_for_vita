/* The offline Anki queue's records: one readable UTF-8 JSON object per card
 * (a card of an earlier build may have a screenshot, a separate JPEG; see
 * shell/anki_queue.c). A record keeps the card's content; its Lapis note
 * (core/anki.h, core/lapis.h) is made from it when the Rubi app exports
 * the queue (settings/anki_export.c), and the record's ID is the note's GUID's
 * source, so a card is the same note in every package that has it.
 *
 * Version 3 records everything × knows: the word, its reading and furigana,
 * the shown entry and its dictionary's name, the other dictionaries'
 * entries, the whole recognized text with the word's offsets, the game, its
 * title ID and platform, the time, and the OCR engine and model (docs/anki.md
 * has the format). Version 4 is version 3 with the frequency rank's new
 * "none", 0 (VJO_NO_RANK), as the bundled dictionary's Jiten ranks include
 * 99999, versions 1-3's "none". Version 2 (#49) added upstream's note values
 * to #5's version 1; all of them still load and export. */
#ifndef VJO_ANKI_QUEUE_H
#define VJO_ANKI_QUEUE_H
#include "anki.h"

/* The queue, and the cards an export took out of it (settings/anki_export.c). */
#define VJO_QUEUE_DIR "ux0:data/VitaJPOverlay/anki_queue"
#define VJO_QUEUE_EXPORTED_DIR VJO_QUEUE_DIR "/exported"

#define VJO_QUEUE_VERSION 4
#define VJO_QUEUE_OLD_NO_RANK 99999 /* versions 1-3: no frequency rank */
#define VJO_QUEUE_ID_SIZE 33 /* 128 bits of SHA-256, lowercase hex + NUL */
#define VJO_QUEUE_TEXT_MAX (32u * 1024u)               /* one record's JSON */
#define VJO_QUEUE_PICTURE_MAX VJO_ANKI_PICTURE_MAX      /* its JPEG */
#define VJO_QUEUE_MAX_MEANINGS 1024
#define VJO_QUEUE_MAX_POS 64

/* The record of a note (picture: a JPEG goes with it): NULL when it would be
 * over VJO_QUEUE_TEXT_MAX, on OOM, or for a note without a spelling. The
 * same note gives the same bytes, so the same ID (vjo_queue_id): saving a
 * card twice keeps the first one. */
char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *note, int picture);
/* A record (version 1 to 4) into note, its strings in a. What a note
 * uses is read back, and the time (version 3's game and OCR facts stay in the
 * file); versions 1-3's rank 99999 is read as VJO_NO_RANK. Returns the version,
 * or -1 for anything but a complete, exact record. */
int vjo_queue_decode(VjoArena *a, const char *json, size_t len, VjoAnkiNote *note, int *picture);
/* The content-derived ID of a record's bytes. */
void vjo_queue_id(const char *json, size_t len, char id[VJO_QUEUE_ID_SIZE]);
/* 32 lowercase hex digits (nothing that could name another path). */
int vjo_queue_valid_id(const char *id);
#endif
