#ifndef VJO_CATALOG_H
#define VJO_CATALOG_H
#include "../core/games.h"
/* What a row of the Games page is (settings/psp.c finds the PSP ones). */
enum {
    VJO_ROW_GAME = 0,       /* a Vita game or app */
    VJO_ROW_BUBBLE,         /* an Adrenaline bubble (AdrBubbleBooter, made by Adrenaline Bubble Manager) */
    VJO_ROW_ADRENALINE,     /* Adrenaline itself, PSPEMUCFW */
    VJO_ROW_PSP_SHARED,     /* NPXS10028: PSP games without a row of their own */
    VJO_ROW_MISSING         /* a choice of its own in games.ini, in no folder the scan listed: a game
                             * card it could not read, or a game since deleted (#138) */
};
typedef struct {
    char id[VJO_TITLE_ID_SIZE];
    char title[256];
    char icon[160];
    char disc[VJO_TITLE_ID_SIZE]; /* a bubble's game: its disc ID ("" when unknown) */
    signed char kind;             /* VJO_ROW_* */
    signed char tai;              /* bubbles and Adrenaline: the OCR worker in taiHEN's configuration (VJO_TAI_*) */
    unsigned char register_failed; /* the app's registration failed since the scan (settings/main.c) */
} VjoInstalledGame;
/* Installed ux0/ur0 titles and an inserted gro0 cartridge, deduplicated by ID,
 * then a row for each title of rules (games.ini, may be NULL) that has a
 * choice of its own but is in no folder listed (VJO_ROW_MISSING). Each root's
 * listing, what it skipped and why, and the rows are logged (log.txt). */
int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated, const VjoGames *rules);
/* The NPXS10028 row's title: its rule applies to every PSP game (a bubble,
 * Adrenaline) that has no rule of its own (core/games.h vjo_games_enabled_as). */
#define VJO_CATALOG_PSP_TITLE "Adrenaline / PSP games without a setting of their own"
/* A missing title's row: its name (its update's TITLE, else its ID) and this. */
#define VJO_CATALOG_MISSING_NOTE " (not found: a game card, or not installed)"
/* A PSP row: a bubble, Adrenaline or the shared row (its PSP tag; a PSP game
 * without a rule of its own follows the shared row's). */
static inline int vjo_catalog_psp(const VjoInstalledGame *g)
{ return g->kind == VJO_ROW_BUBBLE || g->kind == VJO_ROW_ADRENALINE || g->kind == VJO_ROW_PSP_SHARED; }
/* A PSP row whose game the OCR worker can be registered for (bubbles, Adrenaline). */
static inline int vjo_catalog_registrable(const VjoInstalledGame *g)
{ return g->kind == VJO_ROW_BUBBLE || g->kind == VJO_ROW_ADRENALINE; }
#endif
