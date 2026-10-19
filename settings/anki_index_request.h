/* The Rubi app's request to the Anki for Vita app for Rubi's duplicate index (core/anki_index.h).
 *
 * The separate Anki for Vita app syncs the user's Anki collection and, for each app that asks,
 * writes an index of its key fields after each sync (its docs/interface.md, "The duplicate index").
 * Rubi asks with ux0:data/Anki_for_vita/index/rubi.request.json:
 *     {"v": 1, "notetypes": [], "key": "Expression", "reading": "ExpressionReading"}
 * every note type that has the key field, the fields config.ini names (anki_duplicate_field,
 * anki_duplicate_reading_field: Lapis's by default), and Anki for Vita answers with
 * index/rubi.idx, which the overlay reads on Enter (shell/anki.c).
 *
 * At the app's start (settings/main.c): with anki_duplicate_check on (the default) and Anki for
 * Vita installed (its interface.json lists interface version 1), the request is written when it is
 * missing or says something else, as the interface has a file that replaces one written (the Vita's
 * rename refuses an existing name): a .tmp written, flushed and closed, the old file renamed to
 * .old, the .tmp to its name, the card synced, the .old removed. A leftover of an earlier write is
 * finished or undone first. Off, or without Anki for Vita, nothing is written. These are files on
 * the memory card: Rubi sends nothing anywhere. */
#ifndef VJO_SETTINGS_ANKI_INDEX_REQUEST_H
#define VJO_SETTINGS_ANKI_INDEX_REQUEST_H
#include <stddef.h>

#include "../core/anki_index.h"
#include "../core/arena.h"

#define VJO_AFV_INTERFACE_PATH VJO_AFV_DIR "/interface.json"
#define VJO_AFV_INDEX_DIR VJO_AFV_DIR "/index"
#define VJO_ANKI_REQUEST_PATH VJO_AFV_INDEX_DIR "/rubi.request.json"
#define VJO_ANKI_FIELD_MAX 63 /* bytes of a field's name */

/* config.ini's settings for it, as core/config.c reads its lines (anki_duplicate_check on by
 * default; a name that is empty, too long or holds a control character keeps the default and
 * counts a warning). */
typedef struct {
    int on;
    char field[VJO_ANKI_FIELD_MAX + 1], reading[VJO_ANKI_FIELD_MAX + 1];
    int warnings;
} VjoAnkiRequestConfig;
void vjo_anki_request_config(VjoAnkiRequestConfig *c, const char *text, size_t len);

/* Anki for Vita is installed and speaks interface version 1: its interface.json (at most 64 KiB) is
 * an object whose "interfaces" array holds the number 1, written so. 1 or 0; scratch is given back. */
int vjo_afv_speaks_1(VjoArena *scratch);

enum {
    VJO_ANKI_REQUEST_FAILED = -1, /* a file could not be read or written (msg says which) */
    VJO_ANKI_REQUEST_OFF = 0,     /* anki_duplicate_check = off: nothing done */
    VJO_ANKI_REQUEST_NO_APP,      /* Anki for Vita is not installed, or speaks no interface version 1 */
    VJO_ANKI_REQUEST_SAME,        /* the request was there already */
    VJO_ANKI_REQUEST_WRITTEN      /* written (or rewritten with other fields) */
};
/* The request, as above: VJO_ANKI_REQUEST_*, with a line for log.txt in msg (cap bytes). scratch
 * holds the files read; it is given back. */
int vjo_anki_index_request(VjoArena *scratch, char *msg, size_t cap);

#endif
