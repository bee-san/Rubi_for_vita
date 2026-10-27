/* The Rubi app's own games.ini helpers (core/games.h). Not in games.c,
 * which the plugins link: one linked without --gc-sections (the kernel's)
 * would take every function of games.c, and the plugins keep their bytes. */
#include "games.h"
#include <string.h>

const char *vjo_game_reject_reason(const char *id)
{
    /* Derived from vjo_game_is_candidate, so the two cannot disagree; the
     * checks below only name its reasons (host/tests/test_games.c). */
    if (vjo_game_is_candidate(id)) return NULL;
    if (!vjo_game_id_valid(id)) return "not a title ID";
    if (!strcmp(id, VJO_SETTINGS_TITLE_ID)) return "this app";
    if (!strncmp(id, "NPXS", 4)) return "a system app";
    if (!strncmp(id, "VITASHELL", 9)) return "VitaShell";
    return "SceShell's name"; /* main*: taiHEN's *main is SceShell */
}

int vjo_games_disable_all(VjoGames *g)
{
    int kept = 0, removed = 0;
    if (g->count < 0 || g->count > VJO_GAMES_MAX) return -1;
    for (int i = 0; i < g->count; i++) {
        if (g->rules[i].enabled) {
            removed++;
            continue;
        }
        if (kept != i) g->rules[kept] = g->rules[i];
        kept++;
    }
    memset(g->rules + kept, 0, sizeof(g->rules[0]) * (size_t)(g->count - kept));
    g->count = kept;
    g->default_enabled = 0;
    return removed;
}

int vjo_line_fix_set(VjoGames *g, const char *id, int value)
{
    if (!vjo_game_id_valid(id) || value < -1 || value > 3 || g->count < 0 || g->count > VJO_GAMES_MAX) return -1;
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->rules[i].title_id, id)) continue;
        if (value >= 0) {
            g->rules[i].enabled = value;
            return 0;
        }
        /* Default: the row goes, the others keep their order */
        memmove(g->rules + i, g->rules + i + 1, sizeof(g->rules[0]) * (size_t)(g->count - i - 1));
        memset(&g->rules[--g->count], 0, sizeof(g->rules[0]));
        return 0;
    }
    if (value < 0) return 0; /* no row to remove */
    if (g->count >= VJO_GAMES_MAX) return -1;
    VjoGameRule *r = &g->rules[g->count++];
    memcpy(r->title_id, id, strlen(id) + 1);
    r->enabled = value;
    return 0;
}
