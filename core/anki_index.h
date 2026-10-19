/* Anki for Vita's duplicate index, as Rubi reads it (issue #220). The separate Anki for Vita app
 * keeps a copy of the user's Anki collection on the Vita and syncs it; after each sync it writes,
 * for each app that asked, an index of the collection's key fields. Rubi's (its client name "rubi")
 * is VJO_ANKI_INDEX_PATH; the Rubi app asks for it (settings/anki_index_request.c). The overlay looks
 * the shown word up there on Enter (shell/anki.c do_save) and says "Already in Anki" instead of
 * saving a word the user has. Rubi only reads the file: it has no network code, and nothing here
 * writes it.
 *
 * The format is Anki for Vita's AFVINDEX version 1 (its docs/duplicate-index.md, which this reader
 * was written from; host/tests/test_anki_index.c checks it against that document's worked example):
 *     header   128 B: "AFVINDEX", version 1, its size 128, N keys, B bucket bits (0-16), the largest
 *              bucket's keys (<= 64), ..., at 124 BE32(SHA-256(0x00 || bytes 0-123))
 *     dir      (2^B + 1) x u32: dir[b] = the keys in the buckets before b; dir[2^B] = N
 *     keys     N x u64, ascending; a key's bucket is its top B bits
 * all little-endian, exactly 128 + 4 (2^B + 1) + 8 N bytes. A word's form key is
 * BE64(SHA-256(0x01 || F)), its pair key (K_F & 0xFFFFFFFF00000000) | BE32(SHA-256(0x02 || F ||
 * 0x1F || R)), F and R the form and the reading as a note's fields hold them, normalised (below). A
 * word is in the collection when its pair key is in the index (a note with that reading) or its form
 * key is (a note with that form and no reading).
 *
 * A missing, damaged or other-version file, or one made for another request, means "not known to be
 * there": the card is saved as before. Rubi trusts an index only when its header's request hash
 * (bytes 64-71) is that of Rubi's own request, the fields config.ini names (VjoConfig
 * anki_duplicate_request, core/config.c: the hash the request the Rubi app writes has). */
#ifndef VJO_ANKI_INDEX_H
#define VJO_ANKI_INDEX_H

#include <stdint.h>

#include "platform.h"

#define VJO_AFV_DIR "ux0:data/Anki_for_vita"
#define VJO_ANKI_INDEX_PATH VJO_AFV_DIR "/index/rubi.idx"
/* The previous index, which an Anki for Vita publish stopped between its two renames leaves (its
 * docs/interface.md): read when there is no rubi.idx. */
#define VJO_ANKI_INDEX_OLD_PATH VJO_ANKI_INDEX_PATH ".old"
#define VJO_ANKI_INDEX_HEADER 128u
#define VJO_ANKI_INDEX_MAX_BITS 16u
#define VJO_ANKI_INDEX_MAX_KEYS 1048576u
#define VJO_ANKI_INDEX_MAX_BUCKET 64u /* keys a lookup reads at most: 512 B */

enum {
    VJO_ANKI_INDEX_PAIR = 2,     /* a note has the form and this reading */
    VJO_ANKI_INDEX_FORM = 1,     /* a note has the form, and no reading */
    VJO_ANKI_INDEX_NONE = 0,     /* not in the index */
    VJO_ANKI_INDEX_MISSING = -1, /* no index (neither file can be opened) */
    VJO_ANKI_INDEX_BAD = -2,     /* not an index of this version, damaged, or a read failed */
    VJO_ANKI_INDEX_NOKEY = -3,   /* the word gives no key (empty, or an ASCII '[': not plain text) */
    VJO_ANKI_INDEX_OTHER = -4    /* an index made for another request (other fields) */
};

typedef struct {
    uint64_t form, pair; /* pair only with has_pair */
    int has_pair;
} VjoAnkiIndexKeys;

/* The keys of a word shown in the overlay: its dictionary form and reading (NULL or "" for none),
 * plain UTF-8, normalised as Anki for Vita normalises a field that holds the word HTML-escaped:
 * every whitespace character (ASCII 9-13 and 32, U+00A0, U+3000) a space, runs of them one and none
 * at either end, U+FF01-U+FF5E as ASCII, and in the reading katakana (U+30A1-U+30F6, U+30FD-U+30FE)
 * as hiragana. 0, or VJO_ANKI_INDEX_NOKEY when the form is empty or holds an ASCII '[' (a field
 * holding one is read as furigana or a sound tag); a reading that is empty or holds one gives no pair
 * key. No allocation; two SHA-256 runs. */
int vjo_anki_index_keys(const char *form, const char *reading, VjoAnkiIndexKeys *k);
/* Looks the keys up in the index at path, else at old_path, opened with p->file_open: the header
 * (128 B), two directory words and the word's bucket (at most 512 B) read, then closed. The header's
 * request hash must be request (else VJO_ANKI_INDEX_OTHER, after the header only). A bucket is
 * checked whole (every key in it, ascending) before it answers. VJO_ANKI_INDEX_PAIR, _FORM or _NONE,
 * or _MISSING, _BAD or _OTHER. No allocation; one 512-byte buffer on the stack. */
int vjo_anki_index_lookup(const VjoPlatform *p, const char *path, const char *old_path, uint64_t request,
                          const VjoAnkiIndexKeys *k);

#endif
