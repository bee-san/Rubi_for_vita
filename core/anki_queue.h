/* The offline Anki queue's records: one readable UTF-8 JSON object per card
 * (its optional screenshot is a separate JPEG, see shell/anki_queue.c).
 * A record keeps the card's content; the Kiku note (core/anki.h) and the
 * word audio are made from it when the card is sent.
 *
 * Version 3 records everything × knows: the word, its reading and furigana,
 * the shown entry and its dictionary's name, the other dictionaries'
 * entries, the whole recognized text with the word's offsets, the game, its
 * title ID and platform, the time, and the OCR engine and model (docs/anki.md
 * has the format). Version 2 (#49) added upstream's note values to #5's
 * version 1; both still load and send. */
#ifndef VJO_ANKI_QUEUE_H
#define VJO_ANKI_QUEUE_H
#include "anki.h"

#define VJO_QUEUE_VERSION 3
#define VJO_QUEUE_ID_SIZE 33 /* 128 bits of SHA-256, lowercase hex + NUL */
#define VJO_QUEUE_TEXT_MAX (32u * 1024u)               /* one record's JSON */
#define VJO_QUEUE_PICTURE_MAX VJO_ANKI_PICTURE_MAX      /* its JPEG */
#define VJO_QUEUE_MAX_MEANINGS 1024
#define VJO_QUEUE_MAX_POS 64

/* The record of a note (picture: a JPEG goes with it): NULL when it would be
 * over VJO_QUEUE_TEXT_MAX, on OOM, or for a note without a spelling. The
 * same note gives the same bytes, so the same ID (vjo_queue_id): saving a
 * card twice keeps the first one and its screenshot. */
char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *note, int picture);
/* A record (version 1, 2 or 3) into note, its strings in a; queue_tag
 * stays NULL. What sending uses is read back (version 3's game, time and
 * OCR facts stay in the file). Returns the version, or -1 for anything but
 * a complete, exact record. */
int vjo_queue_decode(VjoArena *a, const char *json, size_t len, VjoAnkiNote *note, int *picture);
/* The content-derived ID of a record's bytes. */
void vjo_queue_id(const char *json, size_t len, char id[VJO_QUEUE_ID_SIZE]);
/* 32 lowercase hex digits (nothing that could name another path). */
int vjo_queue_valid_id(const char *id);
#endif
