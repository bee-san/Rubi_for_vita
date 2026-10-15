/* The Overlay app's .apkg export: queued cards as Lapis notes
 * (core/lapis.h) in an Anki package that every current Anki imports (Anki
 * 2.1 to 26.x on a computer, AnkiDroid and AnkiMobile): the legacy layout,
 * a ZIP of `collection.anki2` (an SQLite database of schema 11), the
 * `media` JSON map and the numbered media files, with no `meta` file (Anki's
 * rslib import_export/package/meta.rs reads that as Version::Legacy1). The
 * SQLite and ZIP writers are core/sqlw.h and core/zipw.h; nothing is
 * compressed.
 *
 * A note's GUID comes from its card's queue ID (the record's checksum), so
 * a later export that holds the same card gives the same note: Anki's import
 * matches notes by GUID and leaves one it has (its modification time, the
 * card's, is never newer). Screenshots are named by their own SHA-256, so the
 * same picture is one file, in a package and across packages.
 *
 * The caller (settings/anki_export.c) lists the cards and reads them; this
 * file orders them, writes the package through the caller's file calls and,
 * once the file is closed and opened again, verifies it: the ZIP's
 * directory and every entry's CRC-32, the database's pages, every note's ID
 * and GUID in order, the cards and the media map. */
#ifndef VJO_APKG_H
#define VJO_APKG_H

#include <stddef.h>
#include <stdint.h>

#include "anki_queue.h"
#include "arena.h"
#include "sqlw.h"
#include "zipw.h"

#define VJO_APKG_MAX_CARDS 20000
#define VJO_APKG_GUID_MAX 12 /* base91 of 64 bits: at most 10 characters */
/* A card's record without a time (versions 1 and 2): its note's creation and
 * modification time, the same on every export (2023-11-14). */
#define VJO_APKG_NO_TIME 1700000000

typedef struct {
    uint8_t id[16];   /* the queue ID (vjo_queue_id), as bytes */
    int64_t time;     /* the record's time, seconds UTC; VJO_APKG_NO_TIME without one */
    uint8_t exported; /* in the exported archive (an earlier package had it) */
    uint8_t picture;  /* the record has a screenshot */
    uint8_t skipped;  /* set by vjo_apkg_write: it could not be read again or its note made (no room) */
} VjoApkgCard;

typedef struct {
    void *ud;
    /* The card's record into note (its strings in a); with a screenshot, its
     * JPEG into a too (*jpeg, *len; NULL and 0 without one). note NULL: the
     * JPEG only. 0, or < 0: the package is not written. */
    int (*load)(void *ud, VjoArena *a, const VjoApkgCard *c, VjoAnkiNote *note, const uint8_t **jpeg,
                size_t *len);
    /* The package file: n bytes at off. 0, or < 0. */
    int (*write)(void *ud, uint64_t off, const void *src, size_t n);
    int (*read)(void *ud, uint64_t off, void *dst, size_t n);
    /* Progress (may be NULL): cards written of total, then the step. */
    void (*progress)(void *ud, int done, int total, const char *step);
    int (*stopped)(void *ud); /* may be NULL; 1: stop, VJO_E_CANCELLED */
} VjoApkgOps;

typedef struct {
    int cards;          /* notes written */
    int pictures;       /* cards with a screenshot */
    int media;          /* distinct screenshot files */
    uint32_t db_pages;  /* the collection's 4096-byte pages */
    uint32_t bytes;     /* the package's size */
    uint16_t dos_time, dos_date; /* its entries' time, for vjo_apkg_verify */
} VjoApkgResult;

/* What the package's col row and entries say about when it was made. */
typedef struct {
    int64_t now;                      /* seconds UTC; 0 when the clock is unknown */
    int year, month, day, hour, minute, second; /* the console's local time (the ZIP's), year 0 = unknown */
} VjoApkgClock;

/* Sorts cards by time, then ID (the notes' order and so their IDs). */
void vjo_apkg_sort(VjoApkgCard *cards, int n);
/* Card i's note ID, after the previous card's (0 for the first): its time in
 * milliseconds, made unique by counting up from the previous one. */
int64_t vjo_apkg_note_id(const VjoApkgCard *cards, int i, int64_t previous);
/* The card's GUID: base91 (Anki's alphabet) of the ID's first 64 bits. */
void vjo_apkg_guid(const uint8_t id[16], char out[VJO_APKG_GUID_MAX]);
/* A screenshot's media file name: vitajp_<the first 128 bits of its SHA-256>.jpg */
void vjo_apkg_media_name(const uint8_t *jpeg, size_t len, char out[48]);

/* Writes the package of the n sorted cards from offset 0. The arena holds
 * the writers' state and each card's work, released after each card. A card
 * that cannot be loaded again or whose note does not fit in the arena is
 * left out and marked skipped (the package has the others); one whose
 * screenshot changed once its note was written stops the package. VJO_E_OOM
 * when no card is left. */
int vjo_apkg_write(VjoArena *a, const VjoApkgOps *ops, VjoApkgCard *cards, int n, const VjoApkgClock *clock,
                   VjoApkgResult *r);
/* Reads the package (size bytes) back through ops->read and checks it
 * against what vjo_apkg_write wrote for these cards (the skipped ones left
 * out): 0, or < 0. */
int vjo_apkg_verify(VjoArena *a, const VjoApkgOps *ops, uint64_t size, const VjoApkgCard *cards, int n,
                    const VjoApkgResult *r);

#endif
