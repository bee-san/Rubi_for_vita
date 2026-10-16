/* Shared, bounded per-title policy for the Shell plugin and settings app. */
#ifndef VJO_GAMES_H
#define VJO_GAMES_H
#include <stddef.h>
#define VJO_GAMES_MAX 512
#define VJO_TITLE_ID_SIZE 12
#define VJO_GAMES_TEXT_MAX (12 * 1024)
#define VJO_SETTINGS_TITLE_ID "VJOSET001"
typedef struct {
    char title_id[VJO_TITLE_ID_SIZE];
    int enabled;
} VjoGameRule;
typedef struct {
    int default_enabled;
    int count;
    VjoGameRule rules[VJO_GAMES_MAX];
} VjoGames;
int vjo_game_id_valid(const char *id);
/* Adrenaline is allowed: its PSP games share NPXS10028. */
int vjo_game_is_candidate(const char *id);
/* The PSP emulator's own title ID: the Shell's name for the emulator process
 * (a console log showed it for every Adrenaline launch) and the Overlay app's
 * shared row for PSP games without a row of their own. */
#define VJO_PSP_EMULATOR "NPXS10028"
/* A PSP game's disc or PSN ID (a NUL-terminated string): U[CL] and two
 * capitals (UMD) or NP[UEJHAK][HGZ] (PSN), and 5 digits (ULJS00378, NPJH50443).
 * Neither shape is a Vita title ID (those are PCS*, V*, NPXS* or homebrew), so
 * it is the fallback PSP rule when a launch's bubble files cannot be read. */
int vjo_psp_disc_id(const char *id);
/* A PSP game by its ID alone: NPXS10028, a PSPEMU* bubble (Adrenaline's own
 * PSPEMUCFW, an Adrenaline Bubble Manager bubble) or a PSP disc ID (a bubble
 * that ABM named after its game). A bubble with any other ID is found by its
 * files (core/pspbubble.h). */
int vjo_game_is_psp(const char *id);
void vjo_games_defaults(VjoGames *games);
int vjo_games_enabled(const VjoGames *games, const char *id);
/* As vjo_games_enabled; for a PSP game (psp != 0) with no rule of its own,
 * the rule of NPXS10028, the shared PSP row, applies before the default. */
int vjo_games_enabled_as(const VjoGames *games, const char *id, int psp);
int vjo_games_set(VjoGames *games, const char *id, int enabled);
/* Strict parsing: callers must not use partially parsed data on error. */
int vjo_games_parse(VjoGames *games, const char *text, size_t len);
int vjo_games_format(const VjoGames *games, char *out, size_t cap);
#endif
