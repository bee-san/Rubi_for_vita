#include "preferences.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
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

/* Publishes next (len bytes) as config.ini: checked temp write + sync,
 * the old primary as the backup, rename. A failed final publication
 * restores the old primary. */
static int publish(const char *next, int len, int primary)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    SceUID fd = sceIoOpen(TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return -1;
    int used = 0;
    while (used < len) {
        int n = sceIoWrite(fd, next + used, (size_t)(len - used));
        if (n <= 0 || n > len - used) break;
        used += n;
    }
    int close_rc = sceIoClose(fd);
    if (used != len || close_rc < 0 || sceIoSync("ux0:", 0) < 0) return -1;
    if (primary) {
        SceIoStat st;
        int stat_rc = sceIoGetstat(BACKUP, &st);
        if (stat_rc >= 0) { if (sceIoRemove(BACKUP) < 0) return -1; }
        else if (stat_rc != MISSING) return -1;
        if (sceIoRename(VJO_PREFERENCES_PATH, BACKUP) < 0) return -1;
    }
    if (sceIoRename(TEMP, VJO_PREFERENCES_PATH) < 0) {
        /* Keep the last known primary available to the running overlay. */
        if (primary) sceIoRename(BACKUP, VJO_PREFERENCES_PATH);
        sceIoSync("ux0:", 0);
        return -1;
    }
    return sceIoSync("ux0:", 0) < 0 ? -1 : 0;
}

int vjo_preferences_save(int backend, int vocr_tier, int japanese_only, int highlight_mode,
                         int dictionary_results, VjoArena *a)
{
    size_t mark = vjo_arena_mark(a), bytes = 0;
    char *old = NULL;
    int primary, rc = -1;
    if (current_text(a, &old, &bytes, &primary) < 0) goto done;
    size_t cap = bytes + 192; /* the five owned lines, appended or longer than before */
    char *next = vjo_arena_alloc(a, cap);
    int len = next ? vjo_config_patch_preferences(old, bytes, backend, vocr_tier, japanese_only,
                                                 highlight_mode, dictionary_results, next, cap) : -1;
    if (len < 0 || len > VJO_PREFERENCES_TEXT_MAX) goto done;
    rc = publish(next, len, primary);
done:
    vjo_arena_release(a, mark);
    return rc;
}

/* The key of an INI line (lowercase into key), or 0 for none. */
static int line_key(const char *line, size_t n, char key[40])
{
    size_t b = 0, e;
    while (b < n && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) b++;
    e = b;
    while (e < n && line[e] != '=' && line[e] != ' ' && line[e] != '\t') e++;
    if (e == b || e - b >= 40 || line[b] == ';' || line[b] == '#') return 0;
    for (size_t i = b; i < e; i++) key[i - b] = (char)(line[i] >= 'A' && line[i] <= 'Z' ? line[i] + 32 : line[i]);
    key[e - b] = 0;
    while (e < n && (line[e] == ' ' || line[e] == '\t')) e++;
    return e < n && line[e] == '=';
}

int vjo_config_patch_keys(const char *text, size_t len, const char *const *keys, const char *const *values,
                          int n, char *out, size_t cap)
{
    size_t i = 0, used = 0;
    unsigned seen = 0;
    if ((!text && len) || n < 1 || n > 8 || len > VJO_CONFIG_TEXT_MAX || !out || !cap ||
        (len && memchr(text, 0, len)))
        return -1;
    for (int k = 0; k < n; k++)
        if (strchr(values[k], '\n') || strchr(values[k], '\r') || strlen(keys[k]) >= 40) return -1;
    /* A BOM (Notepad) stays, and the first line's key after it is still found. */
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) {
        if (cap <= 3) return -1;
        memcpy(out, text, 3); used = i = 3;
    }
    while (i < len) {
        size_t begin = i;
        char key[40];
        int k = -1;
        while (i < len && text[i] != '\n') i++;
        if (i < len) i++;
        if (line_key(text + begin, i - begin, key))
            for (int j = 0; j < n && k < 0; j++)
                if (!strcmp(key, keys[j])) k = j;
        if (k >= 0 && (seen & (1u << k))) continue; /* a duplicate goes */
        int w = k >= 0 ? snprintf(out + used, cap - used, "%s = %s\n", keys[k], values[k])
                       : snprintf(out + used, cap - used, "%.*s", (int)(i - begin), text + begin);
        if (w < 0 || (size_t)w >= cap - used) return -1;
        used += (size_t)w;
        if (k >= 0) seen |= 1u << k;
    }
    for (int k = 0; k < n; k++) {
        if (seen & (1u << k)) continue;
        int w = snprintf(out + used, cap - used, "%s%s = %s\n", used && out[used - 1] != '\n' ? "\n" : "",
                         keys[k], values[k]);
        if (w < 0 || (size_t)w >= cap - used) return -1;
        used += (size_t)w;
    }
    return used > VJO_CONFIG_TEXT_MAX ? -1 : (int)used;
}

int vjo_preferences_save_anki(int mining, const char *host, VjoArena *a)
{
    static const char *const keys[] = {"anki_mining", "anki_host"};
    const char *values[] = {mining ? "on" : "off", host};
    size_t mark = vjo_arena_mark(a), bytes = 0;
    char *old = NULL;
    int primary, rc = -1;
    if (current_text(a, &old, &bytes, &primary) < 0) goto done;
    size_t cap = bytes + 160;
    char *next = vjo_arena_alloc(a, cap);
    int len = next ? vjo_config_patch_keys(old, bytes, keys, values, 2, next, cap) : -1;
    if (len < 0 || len > VJO_PREFERENCES_TEXT_MAX) goto done;
    rc = publish(next, len, primary);
done:
    vjo_arena_release(a, mark);
    return rc;
}
