#include "games_storage.h"
#include "../core/port.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#define BACKUP VJO_GAMES_BACKUP
#define TEMP VJO_GAMES_PATH ".tmp"

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int missing(const char *path)
{
    SceIoStat st;
    /* sceIo returns encoded Sce errno values, not newlib's errno. */
    return sceIoGetstat(path, &st) == (int)0x80010002u;
}

/* path's rows: games.ini's (line_fix 0) or, #333, ocr_line_fix.ini's (1). */
static int read_policy(const char *path, VjoGames *g, VjoArena *scratch, int line_fix)
{
    size_t mark = vjo_arena_mark(scratch);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int rc = -1;
    if (fd < 0) return -1;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size >= 0 && size <= VJO_GAMES_TEXT_MAX && sceIoLseek(fd, 0, SCE_SEEK_SET) >= 0) {
        char *text = vjo_arena_alloc(scratch, (size_t)size + 1);
        size_t used = 0;
        if (text) {
            while (used < (size_t)size) {
                int n = sceIoRead(fd, text + used, (SceSize)size - used);
                if (n <= 0) break;
                used += (size_t)n;
            }
            if (used == (size_t)size && size > 0) rc = vjo_games_parse_kind(g, text, used, line_fix);
        }
    }
    sceIoClose(fd);
    vjo_arena_release(scratch, mark);
    return rc;
}

static int load(VjoGames *g, VjoArena *scratch, const char *path, const char *backup, int line_fix)
{
    if (read_policy(path, g, scratch, line_fix) == 0) return 0;
    if (read_policy(backup, g, scratch, line_fix) == 0) return 1;
    vjo_games_defaults(g);
    if (missing(path) && missing(backup)) return 0;
    g->default_enabled = 0;
    return -1;
}

int vjo_games_file_load(VjoGames *g, VjoArena *scratch) { return load(g, scratch, VJO_GAMES_PATH, BACKUP, 0); }

/* (in the data segment: the Shell's code pages are full, core/port.h) */
static const char line_fix_path[] VJO_DATA_TABLE(line_fix_path) = VJO_LINE_FIX_PATH;

int vjo_line_fix_file_read(VjoGames *g, VjoArena *scratch) { return read_policy(line_fix_path, g, scratch, 1); }

int vjo_line_fix_file_load(VjoGames *g, VjoArena *scratch)
{
    return load(g, scratch, line_fix_path, VJO_LINE_FIX_BACKUP, 1);
}

static int save(const VjoGames *g, VjoArena *scratch, const char *path, const char *temp, const char *backup,
                int line_fix)
{
    size_t mark = vjo_arena_mark(scratch);
    char *text = vjo_arena_alloc(scratch, VJO_GAMES_TEXT_MAX + 1);
    int len = !text ? -1 : line_fix ? vjo_line_fix_format(g, text, VJO_GAMES_TEXT_MAX + 1)
                                    : vjo_games_format(g, text, VJO_GAMES_TEXT_MAX + 1);
    int rc = -1;
    if (len < 0) goto done;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(VJO_GAMES_DIR, 0777);
    SceUID fd = sceIoOpen(temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) goto done;
    int used = 0;
    while (used < len) {
        int n = sceIoWrite(fd, text + used, (SceSize)(len - used));
        if (n <= 0) break;
        used += n;
    }
    int close_rc = sceIoClose(fd);
    if (used != len || close_rc < 0 || sceIoSync("ux0:", 0) < 0) goto done;
    VjoGames *old = vjo_arena_alloc(scratch, sizeof(*old));
    if (!old) goto done;
    if (read_policy(path, old, scratch, line_fix) == 0) {
        if (exists(backup) && sceIoRemove(backup) < 0) goto done;
        if (sceIoRename(path, backup) < 0) goto done;
    } else if (exists(path) && sceIoRemove(path) < 0) goto done;
    if (sceIoSync("ux0:", 0) < 0) goto done;
    if (sceIoRename(temp, path) < 0) goto done;
    rc = sceIoSync("ux0:", 0) < 0 ? -1 : 0;
done:
    vjo_arena_release(scratch, mark);
    return rc;
}

int vjo_games_file_save(const VjoGames *g, VjoArena *scratch) { return save(g, scratch, VJO_GAMES_PATH, TEMP, BACKUP, 0); }

int vjo_line_fix_file_save(const VjoGames *g, VjoArena *scratch)
{
    return save(g, scratch, VJO_LINE_FIX_PATH, VJO_LINE_FIX_PATH ".tmp", VJO_LINE_FIX_BACKUP, 1);
}
