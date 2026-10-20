/* Which games have a native text hook entry (API 31, docs/native-hooks.md): the Rubi app's Games page
 * tags their rows. Plain C, no allocation; the app calls it for the rows it draws, the host tests link
 * it as is. */
#ifndef VJO_HOOK_TITLES_H
#define VJO_HOOK_TITLES_H

#define VJO_HOOK_TITLE_NONE 0
#define VJO_HOOK_TITLE_LUNA 1 /* in LunaTranslator's ported Vita table (core/hook_luna.h): not checked by Rubi */
#define VJO_HOOK_TITLE_RUBI 2 /* Rubi's own entry: CLANNAD (PCSG00415), the one checked so far */

/* title_id (9 characters, e.g. "PCSG00415"): VJO_HOOK_TITLE_RUBI for Rubi's own entry (first),
 * VJO_HOOK_TITLE_LUNA for Luna's table, VJO_HOOK_TITLE_NONE otherwise (NULL or "" too). */
int vjo_hook_title_listed(const char *title_id);

#endif
