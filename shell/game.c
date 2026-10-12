/* Which game the foreground process is: its title ID, and whether that is
 * a game at all. */
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "shell.h"
#include "../core/games.h"
#include "../vita/games_storage.h"

int vjo_game_enabled(const char *tid, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch);
    VjoGames *games = vjo_arena_alloc(scratch, sizeof(*games));
    int rc = games ? vjo_games_file_load(games, scratch) : -1;
    int enabled = rc >= 0 && vjo_games_enabled(games, tid);
    if (rc < 0) vjo_log("games.ini unreadable: OCR disabled until settings are repaired");
    else if (rc > 0) vjo_log("games.ini recovered from backup");
    vjo_arena_release(scratch, mark);
    return enabled;
}

#include "../core/utf.h"

#define PSP_EMULATOR "NPXS10028" /* where Adrenaline's games run */

int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len); /* SceProcessmgr, not in headers */

int vjo_title_id(SceUID pid, char *tid, int size)
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

/* The game's name for MiscInfo: its title (app parameter 9, else 10);
 * "PSP" for the PSP emulator, which every Adrenaline game shares; "" when
 * neither is readable. Both are logged until a device run shows which
 * one is the full title. */
void vjo_title_name(SceUID pid, const char *tid, char *name, int size)
{
    char p9[128], p10[128];
    int r9, r10;
    sceClibMemset(p9, 0, sizeof(p9));
    sceClibMemset(p10, 0, sizeof(p10));
    r9 = sceAppMgrAppParamGetString(pid, 9, p9, sizeof(p9) - 1);
    r10 = sceAppMgrAppParamGetString(pid, 10, p10, sizeof(p10) - 1);
    vjo_log("title %s: param 9 = \"%s\" (0x%08X), param 10 = \"%s\" (0x%08X)", tid, p9, r9, p10, r10);
    if (!sceClibStrcmp(tid, PSP_EMULATOR))
        sceClibSnprintf(name, size, "PSP");
    else if (r9 >= 0 && p9[0])
        sceClibSnprintf(name, size, "%s", p9);
    else if (r10 >= 0 && p10[0])
        sceClibSnprintf(name, size, "%s", p10);
    else
        name[0] = '\0';
    vjo_utf8_trim_partial(name);
}

/* vjoSetGameActive's mode for a title: system apps, SceShell and VitaShell
 * (Select starts its FTP server) are not games. The PSP emulator, where
 * Adrenaline's games run, is one with a static framebuffer; all its games
 * share its title ID, so one region. */
int vjo_title_game_mode(const char *tid)
{
    if (!sceClibStrcmp(tid, PSP_EMULATOR))
        return VJO_GAME_STATIC_FB;
    if (!vjo_game_is_candidate(tid))
        return VJO_GAME_NONE;
    return VJO_GAME;
}
