#ifndef VJO_CATALOG_H
#define VJO_CATALOG_H
#include "../core/games.h"
/* What a row of the Games page is (settings/psp.c finds the PSP ones). */
enum {
    VJO_ROW_GAME = 0,       /* a Vita game or app */
    VJO_ROW_BUBBLE,         /* an Adrenaline bubble (AdrBubbleBooter, made by Adrenaline Bubble Manager) */
    VJO_ROW_ADRENALINE,     /* Adrenaline itself, PSPEMUCFW */
    VJO_ROW_PSP_SHARED      /* NPXS10028: PSP games without a row of their own */
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
/* Installed ux0/ur0 titles and inserted gro0 cartridge, deduplicated by ID. */
int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated);
/* The NPXS10028 row's title: its rule applies to every PSP game (a bubble,
 * Adrenaline) that has no rule of its own (core/games.h vjo_games_enabled_as). */
#define VJO_CATALOG_PSP_TITLE "Adrenaline / PSP games without a setting of their own"
/* A PSP row whose game the OCR worker can be registered for (bubbles, Adrenaline). */
static inline int vjo_catalog_registrable(const VjoInstalledGame *g)
{ return g->kind == VJO_ROW_BUBBLE || g->kind == VJO_ROW_ADRENALINE; }
#endif
