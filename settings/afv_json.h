/* The rules of Anki for Vita's file interface for every JSON file (its docs/interface.md, "Files"),
 * which hold for every version, so a reader checks them before it reads "v". The Rubi app reads
 * interface.json, status.json and its outbox results through this (settings/anki_afv.c,
 * settings/anki_index_request.c). */
#ifndef VJO_SETTINGS_AFV_JSON_H
#define VJO_SETTINGS_AFV_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "../core/arena.h"
#include "../core/json.h"

#define VJO_AFV_JSON_DEPTH 64 /* the most nested objects and arrays */

/* json (len bytes) as the interface requires a JSON file: at most max bytes, valid UTF-8, no NUL
 * written or escaped, no escaped lone surrogate, no key given twice in any object (compared
 * unescaped), nested at most VJO_AFV_JSON_DEPTH levels, one object and nothing after it. 0 with its
 * tokens in j (in a), or -1 (malformed). */
int vjo_afv_json_strict(VjoArena *a, const char *json, size_t len, size_t max, VjoJson *j);
/* Object 0's "v": 0 with *v when it is an integer written in at most 18 digits, else -1 (none, or no
 * such integer). The interface's version is 1; another integer is another version's file. */
int vjo_afv_json_version(const VjoJson *j, int64_t *v);

#endif
