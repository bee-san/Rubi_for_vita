#ifndef VJO_SETTINGS_PREFERENCES_H
#define VJO_SETTINGS_PREFERENCES_H
#include "../core/config.h"
#include "../core/arena.h"
#define VJO_PREFERENCES_PATH "ux0:data/VitaJPOverlay/config.ini"
#define VJO_PREFERENCES_TEXT_MAX VJO_CONFIG_TEXT_MAX
/* 0 = primary/missing; 1 = recovered backup; -1 = unreadable. */
int vjo_preferences_load(VjoConfig *cfg, VjoArena *scratch);
/* Re-read the file, preserve unrelated keys, checked temp write + sync +
 * backup + rename. A failed final publication restores the old primary. */
int vjo_preferences_save(int backend, int japanese_only, int highlight_mode,
                         int dictionary_results, VjoArena *scratch);
/* The Anki page's two settings, saved the same way: anki_mining on/off
 * and anki_host (IP[:port], auto, or ""; validated by the caller). */
int vjo_preferences_save_anki(int mining, const char *host, VjoArena *scratch);
/* text with a `key = value` line for each of the n (<= 8, lowercase) keys:
 * a key's first line is replaced, its later duplicates dropped, a missing
 * key appended; every other line kept verbatim. Returns the bytes written
 * to out (NUL-terminated), or -1 (no room, embedded NUL, a value with a line
 * break, over VJO_CONFIG_TEXT_MAX). */
int vjo_config_patch_keys(const char *text, size_t len, const char *const *keys, const char *const *values,
                          int n, char *out, size_t cap);
#endif
