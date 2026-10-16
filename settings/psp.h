/* The Overlay app's PSP rows: Adrenaline bubbles and Adrenaline itself among
 * the installed apps, the game each bubble boots, and the description the
 * Shell reads at launch (core/pspbubble.h). The app, not SceShell, reads the
 * targets. Turning a row ON edits no taiHEN configuration: the kernel plugin
 * loads the OCR worker into every game turned ON (#138, Agent A;
 * docs/one-vpk-install.md, "Which games get the worker"). */
#ifndef VJO_SETTINGS_PSP_H
#define VJO_SETTINGS_PSP_H
#include <stddef.h>
#include "catalog.h"

/* g is ux0:app/<g->id> with its own title read: if it is a bubble (its
 * data/boot.bin has the ABB magic, or it has a data/boot.inf) or Adrenaline
 * (PSPEMU*), sets its kind, reads its target's title and disc ID, writes the
 * Shell's description and logs what it found. Others are left as they are. */
void vjo_psp_scan_app(VjoInstalledGame *g);
/* Removes the descriptions of IDs that are no bubble's in games[0, count),
 * the whole catalog (not when it was truncated). */
void vjo_psp_prune(const VjoInstalledGame *games, int count);
#endif
