/* Which game the foreground process is: its title ID, and whether that is
 * a game at all. */
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "shell.h"
#include "../core/games.h"
#include "../core/pspbubble.h"
#include "../vita/games_storage.h"

int vjo_game_enabled(const char *tid, int psp, int *line_fix, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch);
    VjoGames *games = vjo_arena_alloc(scratch, sizeof(*games));
    int rc = games ? vjo_games_file_load(games, scratch) : -1;
    int enabled = rc >= 0 && vjo_games_enabled_as(games, tid, psp);
    if (rc < 0) vjo_log("games.ini unreadable: OCR disabled until settings are repaired");
    else if (rc > 0) vjo_log("games.ini recovered from backup");
    /* #333: an enabled game's own ocr_line_fix (ocr_line_fix.ini, read into the same table), its bubble's own row
     * first, then the shared PSP row's, as for ON/OFF; -1 without one, or a file that cannot be read */
    *line_fix = enabled && vjo_line_fix_file_read(games, scratch) == 0 ? vjo_games_row_as(games, tid, psp) : -1;
    vjo_arena_release(scratch, mark);
    return enabled;
}

#include "../core/utf.h"

int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len); /* SceProcessmgr, not in headers */

static int title_id(SceUID pid, char *tid, int size)
{
    int ret;
    sceClibMemset(tid, 0, size);
    ret = sceKernelGetProcessTitleId(pid, tid, size);
    if (ret >= 0 && tid[0])
        return 0;
    sceClibMemset(tid, 0, size);
    ret = sceAppMgrAppParamGetString(pid, 12, tid, size); /* 12 = title ID */
    if (ret >= 0 && tid[0])
        return 0;
    return ret < 0 ? ret : -1;
}

/* The foreground process as the overlay keys it: its title ID (the
 * process's, else AppMgr parameter 12) or, when that is the PSP emulator's
 * NPXS10028, AppMgr's name for its app: an Adrenaline bubble's own ID or
 * Adrenaline's PSPEMUCFW (AdrBubbleBooter-oss src/vita/adrbubblebooter/
 * main.c:62 reads it the same way). A PSP game is the emulator's process (the
 * Shell saw NPXS10028 for every Adrenaline launch on a console) or a game the
 * Rubi app described (a bubble it found by its files: settings/psp.c); its
 * policy, platform and name are a PSP game's. ID shapes (PSPEMU*, disc IDs)
 * are not used here: under its own title ID a bubble's ID is its booter's, an
 * ordinary app that exits. One description of at most VJO_PSP_INFO_MAX bytes
 * is read; nothing is listed. */
int vjo_game_identify(SceUID pid, VjoGameId *g)
{
    char tid[32], app[32], text[VJO_PSP_INFO_MAX + 1], *title, *path;
    sceClibMemset(g, 0, sizeof(*g));
    int rc = title_id(pid, tid, sizeof(tid));
    if (rc < 0) return rc; /* not readable yet: *g is all zero */
    if (sceAppMgrGetNameById(pid, app) < 0) app[0] = '\0';
    app[sizeof(app) - 1] = '\0';
    int emulator = !sceClibStrcmp(tid, VJO_PSP_EMULATOR);
    const char *key = emulator && vjo_game_is_candidate(app) ? app : tid;
    int game = vjo_game_is_candidate(key), n = 0; /* a longer ID is no game's */
    sceClibMemcpy(g->key, key, sceClibStrnlen(key, sizeof(g->key) - 1));
    if (game) {
        sceClibSnprintf(text, sizeof(text), VJO_PSP_INFO_DIR "/%s.txt", g->key);
        SceUID fd = sceIoOpen(text, SCE_O_RDONLY, 0);
        n = fd >= 0 ? sceIoRead(fd, text, sizeof(text) - 1) : 0;
        if (fd >= 0) sceIoClose(fd);
    }
    text[n > 0 ? n : 0] = '\0';
    g->psp = game && (emulator || n > 0);
    /* The static framebuffer's sampling only for the emulator's own process, as
     * before: in another app it may read a freed framebuffer (kernel/display.c),
     * and a bubble's own ID is also its booter's, an ordinary app. */
    g->mode = !game ? VJO_GAME_NONE : emulator ? VJO_GAME_STATIC_FB : VJO_GAME;
    if (!g->psp) return 0;
    vjo_psp_info_lines(text, &title, &path);
    if (vjo_game_id_valid(text)) sceClibMemcpy(g->disc, text, sizeof(g->disc));
    sceClibSnprintf(g->name, sizeof(g->name), "%s", title);
    vjo_utf8_trim_partial(g->name);
    /* The title last: a line longer than the log's 254 B loses its end, not the path's. */
    vjo_log("psp: %s %s -> %s; disc %s; boots %s; \"%s\"", tid, app, g->key, g->disc, n > 0 ? path : "?", g->name);
    return 0;
}

/* The game's name for MiscInfo: its title (app parameter 9, else 10);
 * "PSP" for the PSP emulator, which every Adrenaline game shares; "" when
 * neither is readable. The debug build logged both until a console run
 * showed which one is the title: in the owner's test.1-test.4 logs (115 game
 * starts, four Vita titles) parameter 9 was refused (0x80801000) and empty
 * every time, and parameter 10 held the title. #333 took the log line's
 * bytes for the debug Shell's code room; the order is unchanged. */
void vjo_title_name(SceUID pid, const char *tid, char *name, int size)
{
    char p9[128], p10[128];
    int r9, r10;
    sceClibMemset(p9, 0, sizeof(p9));
    sceClibMemset(p10, 0, sizeof(p10));
    r9 = sceAppMgrAppParamGetString(pid, 9, p9, sizeof(p9) - 1);
    r10 = sceAppMgrAppParamGetString(pid, 10, p10, sizeof(p10) - 1);
    if (!sceClibStrcmp(tid, VJO_PSP_EMULATOR))
        sceClibSnprintf(name, size, "PSP");
    else if (r9 >= 0 && p9[0])
        sceClibSnprintf(name, size, "%s", p9);
    else if (r10 >= 0 && p10[0])
        sceClibSnprintf(name, size, "%s", p10);
    else
        name[0] = '\0';
    vjo_utf8_trim_partial(name);
}
