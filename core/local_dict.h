/* Disk-backed Yomitan term dictionaries; see docs/local-dictionaries.md. */
#ifndef VJO_LOCAL_DICT_H
#define VJO_LOCAL_DICT_H
#include "client.h"

#define VJO_LOCAL_MAX_DICTS 8
#define VJO_LOCAL_MAX_KEY 192
#define VJO_LOCAL_MAX_ENTRY 8192
#define VJO_LOCAL_MAX_TEXT 4096
#define VJO_LOCAL_MAX_RESULTS 128
#define VJO_LOCAL_MAX_MATCHES 8
#define VJO_LOCAL_RESULT_BUDGET (64u * 1024u)
#define VJO_LOCAL_SCRATCH_BUDGET (48u * 1024u)
/* One maximum-size payload plus its vocabulary, token and alignment. */
#define VJO_LOCAL_SELECTED_BUDGET (VJO_LOCAL_MAX_ENTRY + 256u)

/* Validate comma-separated, relative .vjdict filenames (no directory traversal). */
int vjo_local_names_valid(const char *names);
int vjo_local_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                     const char *text, VjoDictResult *res, VjoErr *err);

/* Zero or one best match starting at cursor_byte in raw text. The cursor must
 * be a decoder codepoint boundary; invalid offsets return VJO_E_PARSE.
 * Malformed bytes keep the existing one-byte replacement behavior. Searches
 * at most 32 characters, stopping at punctuation/whitespace, without scanning
 * later words. Longest source span wins, then exact match, then dictionary
 * order. Token positions include raw newlines and use UTF-16 units. */
int vjo_local_lookup_at(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                        const char *text, size_t cursor_byte,
                        VjoDictResult *res, VjoErr *err);
#endif
