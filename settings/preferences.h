#ifndef VJO_SETTINGS_PREFERENCES_H
#define VJO_SETTINGS_PREFERENCES_H
#include "../core/config.h"
#include "../core/arena.h"
#define VJO_PREFERENCES_PATH "ux0:data/VitaJPOverlay/config.ini"
#define VJO_PREFERENCES_TEXT_MAX VJO_CONFIG_TEXT_MAX
/* 0 = primary/missing; 1 = recovered backup; -1 = unreadable. */
int vjo_preferences_load(VjoConfig *cfg, VjoArena *scratch);
/* Re-read the file, preserve unrelated keys, checked temp write + sync +
 * backup + rename. A failed final publication restores the old primary.
 * vocr_tier: VJO_VOCR_TIER_* (an older build's vocr_model lines go).
 * Both saves also rewrite a removed feature's lines (vjo_config_removed_line:
 * the removed online dictionaries' API keys, the removed Anki settings and the
 * removed engine choice (ocr_backend) go, dictionary = jpdb, jiten or hachidori
 * becomes local); a file with such a key is first published without them and
 * the keyed backup that leaves is deleted, so that no file keeps a key, even
 * when the save itself then fails. */
int vjo_preferences_save(int vocr_tier, int japanese_only, int highlight_mode,
                         int dictionary_results, VjoArena *scratch);
/* The Anki page's settings, saved the same way: anki_mining and anki_screenshot on/off. */
int vjo_preferences_save_anki(int mining, int screenshot, VjoArena *scratch);
/* The OCR page's reading settings, saved the same way: ocr_mode (VJO_OCR_AUTO
 * auto, VJO_OCR_ON_PRESS on_press: the Overlay app's Read text) and
 * ocr_boost_clock (1 on, 0 off: its Speed boost while reading). Any other
 * value is refused (-1) before anything is read or written. */
int vjo_preferences_save_reading(int ocr_mode, int boost_clock, VjoArena *scratch);
/* text with a `key = value` line for each of the n (0-8, lowercase) keys:
 * a key's first line is replaced, its later duplicates dropped, a missing
 * key appended; every other line kept verbatim but a removed feature's
 * (vjo_config_removed_line). Returns the bytes written to out
 * (NUL-terminated), or -1 (no room, embedded NUL, a value with a line
 * break, over VJO_CONFIG_TEXT_MAX). */
int vjo_config_patch_keys(const char *text, size_t len, const char *const *keys, const char *const *values,
                          int n, char *out, size_t cap);
#endif
