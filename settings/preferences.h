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
int vjo_preferences_save(int backend, int japanese_only, VjoArena *scratch);
#endif
