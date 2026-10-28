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
    int enabled; /* games.ini: 1 ON, 0 OFF; ocr_line_fix.ini (#333): the value's index in vjo_line_finder_names */
} VjoGameRule;
typedef struct {
    int default_enabled;
    int count;
    VjoGameRule rules[VJO_GAMES_MAX];
} VjoGames;
/* #333: a game's own line finder lives in ux0:data/VitaJPOverlay/ocr_line_fix.ini (vita/games_storage.h
 * VJO_LINE_FIX_PATH), in games.ini's format and through its reader: one "TITLEID = value" line per game (lowercase)
 * and no default line (config.ini's ocr_finder and ocr_line_fix apply to every game without its own). spotter
 * (VJO_LINE_FINDER_SPOTTER) is the text spotter (ocr_finder = spotter, API 54), with config.ini's ocr_line_fix for
 * its fallback; off, on, junk and all are Rubi's finder (ocr_finder = grad) with that ocr_line_fix
 * (VJO_LINE_FINDER_GRAD). games.ini itself keeps its two values: every earlier kernel, Shell and Rubi app reads
 * games.ini strictly, so a line of another kind there would make it unreadable to them (its backup's choices, or
 * every game OFF). The names, by value, NULL-terminated: the spotter's, then ocr_line_fix's in config.ini's order,
 * so vjo_line_finder_names + 1 is ocr_line_fix's own table (core/config.c). */
#define VJO_LINE_FINDER_SPOTTER 0
#define VJO_LINE_FINDER_GRAD(fix) ((fix) + 1) /* Rubi's finder with ocr_line_fix = fix (core/config.h: 0-3) */
#define VJO_LINE_FINDERS 5
extern const char *const vjo_line_finder_names[];
int vjo_game_id_valid(const char *id);
/* A Vita title ID as the kernel reads a process's (kernel/game_ocr.c, an SC3 charset's open) and the hook cache keeps
 * one (core/hook_cache.c): 9 capital letters or digits, then a NUL. */
static inline int vjo_title_id_strict(const char *id)
{
    for (unsigned i = 0; i < 9; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return 0;
    return !id[9];
}
/* Adrenaline is allowed: its PSP games share NPXS10028. */
int vjo_game_is_candidate(const char *id);
/* The PSP emulator's own title ID: the Shell's name for the emulator process
 * (a console log showed it for every Adrenaline launch) and the Rubi app's
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
/* Strict parsing: callers must not use partially parsed data on error. line_fix 0: games.ini's text; 1 (#333):
 * ocr_line_fix.ini's, whose values are vjo_line_finder_names and which has no default line (an error). The kernel's
 * build reads games.ini only. */
int vjo_games_parse_kind(VjoGames *games, const char *text, size_t len, int line_fix);
static inline int vjo_games_parse(VjoGames *games, const char *text, size_t len)
{ return vjo_games_parse_kind(games, text, len, 0); }
int vjo_games_format(const VjoGames *games, char *out, size_t cap);
/* #333: id's own row's value, else (psp != 0) the shared PSP row's (NPXS10028), as vjo_games_enabled_as finds
 * a rule; -1 when neither has one. Any ID: the caller's is the Shell's key (shell/game.c). */
int vjo_games_row_as(const VjoGames *games, const char *id, int psp);
#ifndef VJO_KERNEL /* the kernel reads games.ini only */
static inline int vjo_line_fix_parse(VjoGames *games, const char *text, size_t len)
{ return vjo_games_parse_kind(games, text, len, 1); }
/* The file's text: a comment, then "TITLEID = value" for each row, in order. Its length, or -1 when cap is too
 * small or a row is invalid. */
int vjo_line_fix_format(const VjoGames *games, char *out, size_t cap);
#endif

/* ---- the Rubi app's only (core/games_app.c, linked into the app and the
 * host tests, never into a plugin: a plugin linked without --gc-sections, as
 * the kernel's is, would take every function of the file it links) ---- */
/* Why vjo_game_is_candidate rejects id ("not a title ID", "a system app",
 * ...), or NULL when it does not: the app's catalog logs it for each folder
 * it skips (settings/catalog.c). */
const char *vjo_game_reject_reason(const char *id);
/* "Disable all" (the app's Default ON -> OFF, #138): the default OFF and every
 * "= enabled" rule removed, so no game stays ON by a choice of its own; the
 * "= disabled" rules stay, as the games to keep OFF when the default is next
 * ON. The rules keep their order. The number of rules removed, -1 for a
 * count out of range (games unchanged). */
int vjo_games_disable_all(VjoGames *games);
/* #333, ocr_line_fix.ini's table: id's own value set (0 to VJO_LINE_FINDERS - 1, vjo_line_finder_names' index), or
 * its row removed (value -1: config.ini's, or the shared PSP row's, applies again). 0, or -1 for a bad ID or value or
 * a full table (unchanged). */
int vjo_line_fix_set(VjoGames *games, const char *id, int value);
#endif
