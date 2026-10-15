/* The Overlay app's PSP rows: Adrenaline bubbles and Adrenaline itself among
 * the installed apps, the game each bubble boots, the description the Shell
 * reads at launch (core/pspbubble.h), and the OCR worker's registration in
 * taiHEN's configuration when the user turns one ON. The app, not SceShell,
 * reads the targets and writes the configuration. */
#ifndef VJO_SETTINGS_PSP_H
#define VJO_SETTINGS_PSP_H
#include <stddef.h>
#include "catalog.h"

#define VJO_TAI_UX0 "ux0:tai/config.txt"
#define VJO_TAI_UR0 "ur0:tai/config.txt"
/* The copy kept before the app appends to a configuration (the PC installer's is .vjo-bak). */
#define VJO_TAI_APP_BACKUP ".vjo-app-bak"

/* g is ux0:app/<g->id> with its own title read: if it is a bubble (its
 * data/boot.bin has the ABB magic, or it has a data/boot.inf) or Adrenaline
 * (PSPEMU*), sets its kind, reads its target's title and disc ID, writes the
 * Shell's description and logs what it found. Others are left as they are. */
void vjo_psp_scan_app(VjoInstalledGame *g);
/* Removes the descriptions of IDs that are no bubble's in games[0, count),
 * the whole catalog (not when it was truncated). */
void vjo_psp_prune(const VjoInstalledGame *games, int count);
/* Each bubble's and Adrenaline's worker state: the lowest of ur0:tai/config.txt's
 * and, if it exists, ux0:tai/config.txt's (registered means in both). */
void vjo_psp_scan_tai(VjoInstalledGame *games, int count);
/* Registers the worker for g in every configuration there is: a backup copy,
 * then the block of core/taicfg.h appended and checked. 0 (registered, or
 * already) or -1, with a message for the status line either way. */
int vjo_psp_register(VjoInstalledGame *g, char *msg, size_t cap);
#endif
