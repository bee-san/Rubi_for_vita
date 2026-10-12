#include "preferences.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#define BACKUP VJO_PREFERENCES_PATH ".bak"
#define TEMP VJO_PREFERENCES_PATH ".tmp"
#define MISSING ((int)0x80010002u)

static int read_text(const char *path, VjoArena *a, char **out, size_t *bytes)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd == MISSING ? 1 : -1;
    int rc = -1;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size >= 0 && size <= VJO_PREFERENCES_TEXT_MAX && sceIoLseek(fd, 0, SCE_SEEK_SET) == 0) {
        char *text = vjo_arena_alloc(a, (size_t)size + 1);
        size_t used = 0;
        if (text) {
            while (used < (size_t)size) {
                int n = sceIoRead(fd, text + used, (size_t)size - used);
                if (n <= 0 || (size_t)n > (size_t)size - used) break;
                used += (size_t)n;
            }
            if (used == (size_t)size && !memchr(text, 0, used)) {
                text[used] = 0; *out = text; *bytes = used; rc = 0;
            }
        }
    }
    if (sceIoClose(fd) < 0) rc = -1;
    return rc;
}

static int current_text(VjoArena *a, char **out, size_t *bytes, int *primary)
{
    int rc = read_text(VJO_PREFERENCES_PATH, a, out, bytes);
    *primary = rc == 0;
    if (rc != 1) return rc;
    rc = read_text(BACKUP, a, out, bytes);
    if (rc == 0) return 1;
    if (rc != 1) return -1;
    *out = (char *)vjo_config_default_text(); *bytes = strlen(*out);
    return 0;
}

int vjo_preferences_load(VjoConfig *cfg, VjoArena *a)
{
    size_t mark = vjo_arena_mark(a), bytes = 0;
    char *text = NULL;
    int primary;
    vjo_config_defaults(cfg);
    int rc = current_text(a, &text, &bytes, &primary);
    if (rc >= 0) vjo_config_parse(cfg, text, bytes);
    vjo_arena_release(a, mark);
    return rc;
}

int vjo_preferences_save(int backend, int japanese_only, int highlight_mode,
                         int dictionary_results, VjoArena *a)
{
    size_t mark = vjo_arena_mark(a), bytes = 0;
    char *old = NULL;
    int primary, rc = -1;
    if (current_text(a, &old, &bytes, &primary) < 0) goto done;
    size_t cap = bytes + 128;
    char *next = vjo_arena_alloc(a, cap);
    int len = next ? vjo_config_patch_preferences(old, bytes, backend, japanese_only,
                                                 highlight_mode, dictionary_results, next, cap) : -1;
    if (len < 0 || len > VJO_PREFERENCES_TEXT_MAX) goto done;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    SceUID fd = sceIoOpen(TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) goto done;
    int used = 0;
    while (used < len) {
        int n = sceIoWrite(fd, next + used, (size_t)(len - used));
        if (n <= 0 || n > len - used) break;
        used += n;
    }
    int close_rc = sceIoClose(fd);
    if (used != len || close_rc < 0 || sceIoSync("ux0:", 0) < 0) goto done;
    if (primary) {
        SceIoStat st;
        int stat_rc = sceIoGetstat(BACKUP, &st);
        if (stat_rc >= 0) { if (sceIoRemove(BACKUP) < 0) goto done; }
        else if (stat_rc != MISSING) goto done;
        if (sceIoRename(VJO_PREFERENCES_PATH, BACKUP) < 0) goto done;
    }
    if (sceIoRename(TEMP, VJO_PREFERENCES_PATH) < 0) {
        /* Keep the last known primary available to the running overlay. */
        if (primary) sceIoRename(BACKUP, VJO_PREFERENCES_PATH);
        sceIoSync("ux0:", 0);
        goto done;
    }
    rc = sceIoSync("ux0:", 0) < 0 ? -1 : 0;
done:
    vjo_arena_release(a, mark);
    return rc;
}
