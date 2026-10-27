#include "games.h"
#include "port.h"
#include <string.h>

int vjo_game_id_valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (!n || n >= VJO_TITLE_ID_SIZE) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= 'a' && id[i] <= 'z') ||
              (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-')) return 0;
    return 1;
}

int vjo_game_is_candidate(const char *id)
{
    if (!vjo_game_id_valid(id)) return 0;
    if (!strcmp(id, VJO_PSP_EMULATOR)) return 1;
    return strcmp(id, VJO_SETTINGS_TITLE_ID) && strncmp(id, "NPXS", 4) &&
           strncmp(id, "main", 4) && strncmp(id, "VITASHELL", 9);
}

int vjo_psp_disc_id(const char *id)
{
    /* Four capital letters and five digits; a shorter ID fails at its NUL. */
    for (int i = 0; i < 9; i++)
        if ((unsigned)((unsigned char)id[i] - (i < 4 ? 'A' : '0')) > (i < 4 ? 25u : 9u)) return 0;
    /* UMD: UC or UL; PSN: NP, a region U E J H A K (bits 20, 4, 9, 7, 0, 10) and
     * H, G or Z (bits 7, 6, 25), which no Vita, PS Mobile (NPNA, NPOA) or
     * system (NPXS) ID has. */
    return !id[9] && (id[0] == 'U' ? id[1] == 'C' || id[1] == 'L'
                                   : id[0] == 'N' && id[1] == 'P' && (0x100691u >> (id[2] - 'A') & 0x20000C0u >> (id[3] - 'A') & 1u));
}

int vjo_game_is_psp(const char *id)
{
    return !strcmp(id, VJO_PSP_EMULATOR) || !strncmp(id, "PSPEMU", 6) || vjo_psp_disc_id(id);
}

void vjo_games_defaults(VjoGames *g)
{
    memset(g, 0, sizeof(*g));
    g->default_enabled = 1;
}

int vjo_games_row_as(const VjoGames *g, const char *id, int psp)
{
    for (;;) {
        for (int i = 0; i < g->count; i++)
            if (!strcmp(g->rules[i].title_id, id)) return g->rules[i].enabled;
        if (!psp) return -1;
        psp = 0; /* then the shared PSP row's */
        id = VJO_PSP_EMULATOR;
    }
}

int vjo_games_enabled_as(const VjoGames *g, const char *id, int psp)
{
    if (!vjo_game_is_candidate(id)) return 0;
    /* The Shell's timeout fallback has no title ID to match exclusions. */
    if (!strcmp(id, "GAME")) return g->default_enabled && g->count == 0;
    int row = vjo_games_row_as(g, id, psp);
    return row < 0 ? g->default_enabled : row;
}

int vjo_games_enabled(const VjoGames *g, const char *id) { return vjo_games_enabled_as(g, id, 0); }

/* id's row set to value (any: the caller checks it), added at the end if it has none. */
static int set_row(VjoGames *g, const char *id, int value)
{
    if (!vjo_game_id_valid(id)) return -1;
    for (int i = 0; i < g->count; i++) {
        if (!strcmp(g->rules[i].title_id, id)) {
            g->rules[i].enabled = value;
            return 0;
        }
    }
    if (g->count >= VJO_GAMES_MAX) return -1;
    VjoGameRule *r = &g->rules[g->count++];
    memcpy(r->title_id, id, strlen(id) + 1);
    r->enabled = value;
    return 0;
}

int vjo_games_set(VjoGames *g, const char *id, int enabled)
{
    return enabled != 0 && enabled != 1 ? -1 : set_row(g, id, enabled);
}

static int space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* games.ini's values: a rule's index is its enabled. (In the data segment, as the line finder's below.) */
static const char *const on_off[] VJO_DATA_TABLE(games_on_off) = {"disabled", "enabled", NULL};

#ifndef VJO_KERNEL
/* In the data segment: the Shell's code pages are full (core/port.h); config.c's ocr_line_fix reads them too. */
const char *const vjo_line_fix_names[] VJO_DATA_TABLE(line_fix_names) = {"off", "on", "junk", "all", NULL};
#endif

/* A file in games.ini's format whose rows take one of names (NULL-terminated; a row's value is its index):
 * games.ini's on_off, or (#333, line_fix != 0) ocr_line_fix.ini's vjo_line_fix_names, which has no default line.
 * One function for both, so the Shell's one reader (vita/games_storage.c) has it once. */
int vjo_games_parse_kind(VjoGames *g, const char *text, size_t len, int line_fix)
{
#ifdef VJO_KERNEL
    const char *const *names = on_off;
    (void)line_fix;
#else
    const char *const *names = line_fix ? vjo_line_fix_names : on_off;
#endif
    size_t pos = 0;
    vjo_games_defaults(g);
    if (!text || len > VJO_GAMES_TEXT_MAX || memchr(text, 0, len)) return -1;
    while (pos < len) {
        size_t start = pos, end, eq;
        while (pos < len && text[pos] != '\n') pos++;
        end = pos;
        if (pos < len) pos++;
        while (start < end && space(text[start])) start++;
        if (start == end || text[start] == '#' || text[start] == ';') continue;
        while (end > start && space(text[end - 1])) end--;
        for (eq = start; eq < end && text[eq] != '='; eq++);
        if (eq == end) return -1;
        size_t key_end = eq, value_start = eq + 1;
        while (key_end > start && space(text[key_end - 1])) key_end--;
        while (value_start < end && space(text[value_start])) value_start++;
        size_t key_len = key_end - start, value_len = end - value_start;
        int value = -1;
        for (int k = 0; names[k]; k++)
            if (strlen(names[k]) == value_len && !memcmp(text + value_start, names[k], value_len)) value = k;
        if (value < 0) return -1;
        if (key_len == 7 && !memcmp(text + start, "default", 7)) {
            if (names != on_off) return -1;
            g->default_enabled = value;
        } else {
            char id[VJO_TITLE_ID_SIZE];
            if (!key_len || key_len >= sizeof(id)) return -1;
            memcpy(id, text + start, key_len);
            id[key_len] = 0;
            if (set_row(g, id, value) < 0) return -1;
        }
    }
    return 0;
}

#ifndef VJO_KERNEL
/* The Rubi app's writer (vita/games_storage.c vjo_line_fix_file_save; the Shell never links it). */
int vjo_line_fix_format(const VjoGames *g, char *out, size_t cap)
{
    if (!out || !cap || g->count < 0 || g->count > VJO_GAMES_MAX) return -1;
    int n = vjo_snprintf(out, cap, "# Rubi: a game's own ocr_line_fix (off, on, junk or all), set on its page in the "
                                   "Rubi app\n# A game without a line here uses config.ini's.\n");
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t used = (size_t)n;
    for (int i = 0; i < g->count; i++) {
        int v = g->rules[i].enabled;
        if (!vjo_game_id_valid(g->rules[i].title_id) || v < 0 || v > 3) return -1;
        n = vjo_snprintf(out + used, cap - used, "%s = %s\n", g->rules[i].title_id, vjo_line_fix_names[v]);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    return (int)used;
}
#endif

int vjo_games_format(const VjoGames *g, char *out, size_t cap)
{
    if (!out || !cap || g->count < 0 || g->count > VJO_GAMES_MAX) return -1;
    int n = vjo_snprintf(out, cap, "# Rubi per-game settings\ndefault = %s\n",
                         g->default_enabled ? "enabled" : "disabled");
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t used = (size_t)n;
    for (int i = 0; i < g->count; i++) {
        if (!vjo_game_id_valid(g->rules[i].title_id)) return -1;
        n = vjo_snprintf(out + used, cap - used, "%s = %s\n", g->rules[i].title_id,
                         g->rules[i].enabled ? "enabled" : "disabled");
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    return (int)used;
}
