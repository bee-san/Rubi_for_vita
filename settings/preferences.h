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
/* The Anki page's settings, saved the same way: anki_mining, anki_screenshot and (API 32)
 * anki_screenshot_trim_bars on/off. */
int vjo_preferences_save_anki(int mining, int screenshot, int trim_bars, VjoArena *scratch);
/* The OCR page's reading settings, saved the same way: ocr_mode (VJO_OCR_AUTO
 * auto, VJO_OCR_ON_PRESS on_press: the Rubi app's Read text) and
 * ocr_boost_clock (1 on, 0 off: its Speed boost while reading). Any other
 * value is refused (-1) before anything is read or written. */
int vjo_preferences_save_reading(int ocr_mode, int boost_clock, VjoArena *scratch);
/* API 31: Text source (config.ini text_source: VJO_SOURCE_AUTO auto, _HOOKS hooks, _OCR ocr), saved as the
 * others are; a game reads it as it starts (docs/native-hooks.md). 0, or -1 (an unknown source: nothing saved). */
int vjo_preferences_save_text_source(int source, VjoArena *a);
/* API 43, the OCR page's two OCR-skipping switches, saved together as the others are: ocr_text_gate (1 on: no OCR
 * on a screen whose text box holds no text, docs/vita-vn-ocr.md "Text-presence gate") and hook_trust (1 on: a
 * trusted hook's line with no OCR at all, docs/native-hooks.md "Trusted hooks"), each 0 or 1 (else -1, nothing
 * saved); both apply when the overlay next opens. */
int vjo_preferences_save_skips(int text_gate, int hook_trust, VjoArena *a);
/* API 47: config.ini ocr_vertical (core/config.h VJO_VERTICAL_*), published as the other saves are. */
int vjo_preferences_save_vertical(int mode, VjoArena *a);
/* #358, Suggest a text box (docs/first-box.md), kept in region.ini, the Shell's file of every game's box
 * (core/regions.h): OFF is its line for every game without one of its own (VJO_REGION_FALLBACK), "region = full",
 * so such a game reads its whole screen and is offered no box; ON (the default: no such line) offers the box the
 * game worker finds there. The Shell reads region.ini as a game starts or resumes. vjo_preferences_suggest: 1 on, 0
 * off (any such line: an old install's box for every game too), -1 unreadable. The save: on drops that line, off
 * writes it, every game's own line kept; a temp file, synced, then renamed in its place, the old file kept as
 * region.ini.bak until then (and put back when a save was cut short). 0 or -1. */
#define VJO_PREFERENCES_REGION_PATH "ux0:data/VitaJPOverlay/region.ini"
int vjo_preferences_suggest(VjoArena *a);
int vjo_preferences_save_suggest(int on, VjoArena *a);
/* API 46: the overlay on the game's text (config.ini overlay_on_text: 1 on, 0 off; docs/overlay-on-text.md),
 * saved as the others are; the Shell reads it when the overlay opens. 0, or -1 (another value: nothing saved). */
int vjo_preferences_save_overlay_on_text(int on, VjoArena *a);
/* config.ini's text as the app's load reads it (into the arena): 0 config.ini; 1 its backup
 * (config.ini.bak, when config.ini is missing: a save that stopped between its two renames); 2 there
 * is neither (*out NULL); -1 unreadable. The installer's update patches this text and writes the
 * template only for 2 (docs/one-vpk-install.md, "config.ini on install and update"). */
int vjo_preferences_text(VjoArena *scratch, char **out, size_t *bytes);
/* Publishes next (len bytes, a whole config.ini) as the saves do: a file holding a removed feature's
 * API key is first published without it, then next, the old file kept as config.ini.bak. 0 or -1. */
int vjo_preferences_publish_text(const char *next, size_t len, VjoArena *scratch);
/* text with a `key = value` line for each of the n (0-8, lowercase) keys:
 * a key's first line is replaced, its later duplicates dropped, a missing
 * key appended; every other line kept verbatim but a removed feature's
 * (vjo_config_removed_line). Returns the bytes written to out
 * (NUL-terminated), or -1 (no room, embedded NUL, a value with a line
 * break, over VJO_CONFIG_TEXT_MAX). */
int vjo_config_patch_keys(const char *text, size_t len, const char *const *keys, const char *const *values,
                          int n, char *out, size_t cap);
/* The same for another of the app's INI files (app.ini, settings/app_prefs.h): no config.ini rule
 * (vjo_config_removed_line) applies to its other lines, which are all kept verbatim. */
int vjo_ini_patch_keys(const char *text, size_t len, const char *const *keys, const char *const *values, int n,
                       char *out, size_t cap);
/* The file layer of the saves above, for another file of ux0:data/VitaJPOverlay/ (app.ini): a whole text file of at
 * most VJO_PREFERENCES_TEXT_MAX bytes and no NUL into the arena (NUL-terminated): 0, 1 when it does not exist, -1
 * unreadable; and its publication as config.ini's is (path.tmp written and synced, the old file as path.bak when
 * primary says there is one, then renamed; a failed rename puts the old one back): 0 or -1. */
int vjo_prefs_read_file(const char *path, VjoArena *a, char **out, size_t *bytes);
int vjo_prefs_publish_file(const char *path, const char *next, int len, int primary);
#endif
