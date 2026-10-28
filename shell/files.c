/* config.ini / region.ini access. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <string.h>

#include "../core/regions.h"
#include "../vita/games_storage.h" /* vjo_io_read_fd, vjo_io_exists, vjo_io_missing (#418: one copy in the Shell) */
#include "shell.h"

#define MAX_FILE (64 * 1024)

static char *file_read_limit(VjoArena *a, const char *path, size_t *len, size_t max)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    size_t used = 0;
    char *buf;
    if (fd < 0)
        return NULL;
    buf = vjo_io_read_fd(fd, a, max, &used);
    if (sceIoClose(fd) < 0 || !buf)
        return NULL;
    if (len)
        *len = used;
    return buf;
}

char *vjo_file_read(VjoArena *a, const char *path, size_t *len)
{
    return file_read_limit(a, path, len, MAX_FILE);
}

int vjo_file_write(const char *path, const void *data, size_t len)
{
    /* Write to a temp file and rename, so a crash never leaves a torn file. */
    char tmp[128];
    SceUID fd;
    int n;
    sceClibSnprintf(tmp, sizeof(tmp), "%s.tmp", path);
    sceIoMkdir(VJO_DATA_DIR, 0777);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    n = sceIoWrite(fd, data, len);
    sceIoClose(fd);
    if (n != (int)len)
        return -1;
    sceIoRemove(path);
    return sceIoRename(tmp, path);
}

int vjo_file_exists(const char *path)
{
    return vjo_io_exists(path);
}

int vjo_file_line(const char *path, char *buf, int cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int used = 0, n = 0;
    buf[0] = '\0';
    if (fd < 0)
        return fd;
    while (used < cap - 1 && (n = sceIoRead(fd, buf + used, (SceSize)(cap - 1 - used))) > 0)
        used += n;
    sceIoClose(fd);
    buf[used] = '\0';
    for (char *p = buf; *p; p++)
        if ((unsigned char)*p < ' ') { /* the first line only */
            *p = '\0';
            break;
        }
    return n < 0 ? n : used;
}

void vjo_config_load(VjoConfig *cfg, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), len = 0;
    char *text = file_read_limit(scratch, VJO_CONFIG_PATH, &len, VJO_CONFIG_TEXT_MAX);
    vjo_config_defaults(cfg);
    /* An interrupted settings publication may leave only the backup: it is
     * read, never altered. With neither file the defaults apply. SceShell
     * never writes config.ini: the installer, the package (its copy in
     * ux0/data/VitaJPOverlay) and the Rubi app do, so the commented
     * template (docs/memory-ledger.md) is not in the module. */
    if (!text && vjo_io_missing(VJO_CONFIG_PATH)) {
        vjo_arena_release(scratch, mark);
        text = file_read_limit(scratch, VJO_CONFIG_PATH ".bak", &len, VJO_CONFIG_TEXT_MAX);
    }
    if (text && !memchr(text, 0, len))
        vjo_config_parse(cfg, text, len);
    vjo_arena_release(scratch, mark);
}

/* region.ini: see core/regions.h. */
int vjo_region_load(const char *title_id, VjoRect *out, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), len = 0;
    char *text = vjo_file_read(scratch, VJO_REGION_PATH, &len);
    int found = text ? vjo_region_parse(text, len, title_id, out) : VJO_REGION_NONE;
    vjo_arena_release(scratch, mark);
    return found;
}

int vjo_region_save(const char *title_id, const VjoRect *r, VjoArena *scratch)
{
    static const VjoRect full = {0, 0, 0, 0};
    size_t mark = vjo_arena_mark(scratch), len = 0;
    char *text = vjo_file_read(scratch, VJO_REGION_PATH, &len), *next;
    int rc = -1;
    /* never rewrite a file that exists but could not be read: it holds the
     * other games' regions */
    if (text || !vjo_file_exists(VJO_REGION_PATH)) {
        next = vjo_region_update(scratch, text ? text : "", text ? len : 0, title_id, r ? r : &full);
        if (next)
            rc = vjo_file_write(VJO_REGION_PATH, next, strlen(next));
    }
    vjo_arena_release(scratch, mark);
    return rc;
}
