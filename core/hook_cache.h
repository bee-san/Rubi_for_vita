/* Bounded, versioned hook learning. No dialogue, runtime addresses or stream slots are saved.
 * The game worker owns the byte buffer; storage replaces it atomically through the file broker. */
#ifndef VJO_HOOK_CACHE_H
#define VJO_HOOK_CACHE_H
#include "hook_select.h"

#define VJO_HOOK_CACHE_ENTRIES 32u
#define VJO_HOOK_CACHE_BYTES (16u + VJO_HOOK_CACHE_ENTRIES * 96u)

typedef struct {
    char title[12]; /* exactly nine title-ID characters, followed by zero padding */
    uint8_t build[32]; /* SHA-256 of the complete executable, never a display version or module NID */
    uint8_t definition[32]; /* build-generated revision of hook definitions and decoders */
    uint32_t mode; /* the applied text-source word, flags included */
} VjoHookCacheKey;

/* A missing, truncated, corrupt, stale or otherwise invalid cache yields 0. */
int vjo_hook_cache_get(const uint8_t *data, size_t n, const VjoHookCacheKey *key, VjoHookLearned *out);

/* Update in place, newest changed entry first, evicting the oldest of 32 games. A changed build,
 * definition or mode replaces that title's previous learning. Invalid input data starts a fresh cache.
 * NULL learned removes the title (two disagreements or a changed source). 1 changed, 0 identical,
 * -1 invalid key/learning. The caller writes only a changed buffer, always VJO_HOOK_CACHE_BYTES. */
int vjo_hook_cache_put(uint8_t *data, size_t n, const VjoHookCacheKey *key, const VjoHookLearned *learned);
#endif
