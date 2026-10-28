/* The Rubi app's own settings: ux0:data/VitaJPOverlay/app.ini (#310's rule for app features; #311, #314).
 *
 * The overlay never reads this file. config.ini is the overlay's: a key it does not know shows "unknown setting" in
 * the open overlay (core/config.c), and teaching the Shell a key costs Shell bytes, so a choice that only the app
 * uses lives here. The lines are config.ini's: `key = value`, comments after ';' or '#' (also after a value, behind
 * a space or tab), a UTF-8 BOM and CRLF line ends allowed, keys and values in any case. A key the app does not know,
 * or a value it does not know, is ignored and the default stays; a save rewrites only its own keys' lines and keeps
 * every other line as it is. config.ini's removed-key rules (core/config.h vjo_config_removed_line) do not apply.
 *
 * Each setting is a field of VjoAppPrefs (its default in vjo_app_prefs_defaults, below) and a row of the key table
 * in app_prefs.c: its key, its field and its values' names; the field holds the index of its value (an on/off
 * setting's values are "off" and "on").
 *
 * Every call keeps its state in the caller's arena (no statics), so any thread may call it; the app's saves are made
 * by its main thread, one at a time. */
#ifndef VJO_SETTINGS_APP_PREFS_H
#define VJO_SETTINGS_APP_PREFS_H
#include <stddef.h>
#include <string.h>

#include "../core/arena.h"
#include "themes.h"

#define VJO_APP_PREFS_PATH "ux0:data/VitaJPOverlay/app.ini"

/* anki_duplicates (#314): what the export does with a card whose word an earlier card has. */
enum { VJO_DUPLICATES_TAG = 0, VJO_DUPLICATES_SKIP = 1, VJO_DUPLICATES_KEEP = 2 };

/* anki_card_type (#311): the cards' Lapis type, core/lapis.h VJO_CARD_*'s order: word (the default), sentence,
 * word_sentence, click. */
typedef struct {
    int anki_duplicates; /* VJO_DUPLICATES_*: tag (the default), skip or keep */
    int anki_card_type;  /* VJO_CARD_* (core/lapis.h): word, the default */
    int anki_game_tag;   /* 1 (the default): each note has its game's tag, Game::<title> */
    int anki_sentence_readings; /* 1 (the default, reports/sentence-readings.md): readings over the sentence */
    int colour_scheme; /* VJO_THEME_*: Mint (the default), Sakura or Paper, app pages only */
} VjoAppPrefs;

/* Every setting at its default (here, so that code which only follows the settings, as the export does, needs no
 * more of this file: the host suites build settings/anki_export.c without the file layer). */
static inline void vjo_app_prefs_defaults(VjoAppPrefs *p)
{
    memset(p, 0, sizeof(*p));
    p->anki_duplicates = VJO_DUPLICATES_TAG;
    p->anki_card_type = 0; /* VJO_CARD_WORD */
    p->anki_game_tag = 1;
    p->anki_sentence_readings = 1;
}
/* app.ini's text (len bytes) over p: each line of a known key with a known value sets it. */
void vjo_app_prefs_parse(VjoAppPrefs *p, const char *text, size_t len);
/* The defaults, then app.ini (else app.ini.bak: a save stopped between its two renames) over them. 0 (also when
 * there is no file: the defaults), 1 (from the backup), -1 unreadable (p holds the defaults: a page shows its rows'
 * values as unknown and saves none, as it does for config.ini). */
int vjo_app_prefs_load(VjoAppPrefs *p, VjoArena *scratch);
/* Saves n settings (1-8): keys[i] = the name of values[i], its value's index. Each key's first line is replaced and
 * its later ones dropped, a missing key appended, a removed setting's line dropped (stats_daily_goal, the Stats tab's
 * Daily goal, #312: removed 2026-10-28), every other line kept; published as config.ini is (app.ini.tmp
 * written and synced, the old file kept as app.ini.bak, then renamed). 0, or -1: a key or value the table does not
 * have (nothing is read or written), an unreadable app.ini (never rewritten), or a storage failure (the old file
 * stays, or is put back). */
int vjo_app_prefs_save(const char *const *keys, const int *values, int n, VjoArena *scratch);

/* The table, for the pages' rows and the tests: row i's key (NULL past the last row), value v's name of a key (NULL
 * past its last value, or for a key the table does not have), and a setting's value in p (its index; -1 for a key
 * the table does not have). */
const char *vjo_app_prefs_key(int i);
const char *vjo_app_prefs_value(const char *key, int v);
int vjo_app_prefs_get(const VjoAppPrefs *p, const char *key);

#endif
