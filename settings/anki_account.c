#include "anki_account.h"
#include "../core/json.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#define BACKUP VJO_ANKI_ACCOUNT_PATH ".bak"
#define TEMP VJO_ANKI_ACCOUNT_PATH ".tmp"
#define MISSING ((int)0x80010002u)
#define MAX_BYTES 4096

static int read_account(const char *path, VjoAnkiAccount *out, VjoArena *a)
{
    int fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd == MISSING ? 1 : -1;
    int rc = -1;
    SceOff bytes = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (bytes > 0 && bytes <= MAX_BYTES && sceIoLseek(fd, 0, SCE_SEEK_SET) == 0) {
        char *data = vjo_arena_alloc(a, (size_t)bytes + 1);
        size_t used = 0;
        while (data && used < (size_t)bytes) {
            int n = sceIoRead(fd, data + used, (size_t)bytes - used);
            if (n <= 0 || (size_t)n > (size_t)bytes - used) break;
            used += (size_t)n;
        }
        VjoJson j;
        if (used == (size_t)bytes && !memchr(data, 0, used) &&
            vjo_json_parse(a, data, used, &j) == 0 && vjo_json_is_type(&j, 0, JSMN_OBJECT) &&
            vjo_json_skip(&j, 0) == j.n) {
            int version = vjo_json_obj_get(&j, 0, "version");
            if (!vjo_json_is_type(&j, version, JSMN_PRIMITIVE) ||
                j.t[version].end - j.t[version].start != 1 || j.js[j.t[version].start] != '1') goto close;
            const char *keys[] = {"username", "password"};
            char *targets[] = {out->username, out->password};
            rc = 0;
            for (int i = 0; i < 2; i++) {
                int t = vjo_json_obj_get(&j, 0, keys[i]);
                char *s = vjo_json_is_type(&j, t, JSMN_STRING) ? vjo_json_str(a, &j, t) : NULL;
                if (!s || strlen(s) >= 256) { rc = -1; break; }
                /* Embedded NUL is never a valid account field. */
                for (int k = j.t[t].start; k < j.t[t].end; k++) {
                    if (j.js[k] != '\\') continue;
                    if (k + 5 < j.t[t].end && !memcmp(j.js + k, "\\u0000", 6)) { rc = -1; break; }
                    k++; /* An escaped backslash cannot start a NUL escape. */
                }
                if (rc < 0) break;
                memcpy(targets[i], s, strlen(s) + 1);
            }
        }
    }
close:
    if (sceIoClose(fd) < 0) rc = -1;
    return rc;
}

int vjo_anki_account_load(VjoAnkiAccount *account, VjoArena *a)
{
    size_t mark = vjo_arena_mark(a);
    VjoAnkiAccount next = {{0}, {0}};
    int rc = read_account(VJO_ANKI_ACCOUNT_PATH, &next, a);
    if (rc == 1) {
        vjo_arena_release(a, mark);
        rc = read_account(BACKUP, &next, a);
        if (!rc) rc = 1;
        else if (rc == 1) rc = 0;
    }
    memset(account, 0, sizeof(*account));
    if (rc >= 0) *account = next;
    /* Caller owns account; discard transient parsed credentials immediately. */
    memset(a->base + mark, 0, a->used - mark);
    vjo_arena_release(a, mark);
    return rc;
}

int vjo_anki_account_save(const VjoAnkiAccount *account, VjoArena *a)
{
    size_t mark = vjo_arena_mark(a);
    int rc = -1;
    if (!memchr(account->username, 0, sizeof(account->username)) ||
        !memchr(account->password, 0, sizeof(account->password))) return -1;
    VjoBuf b;
    vjo_buf_init(&b, a); vjo_buf_puts(&b, "{\"version\":1,\"username\":");
    vjo_json_write_string(&b, account->username); vjo_buf_puts(&b, ",\"password\":");
    vjo_json_write_string(&b, account->password); vjo_buf_puts(&b, "}\n");
    if (b.oom || b.len > MAX_BYTES) goto done;
    sceIoMkdir("ux0:data", 0777); sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    int fd = sceIoOpen(TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0600);
    if (fd < 0) goto done;
    size_t used = 0;
    while (used < b.len) {
        int n = sceIoWrite(fd, b.data + used, b.len - used);
        if (n <= 0 || (size_t)n > b.len - used) break;
        used += (size_t)n;
    }
    int closed = sceIoClose(fd);
    if (used != b.len || closed < 0 || sceIoSync("ux0:", 0) < 0) goto done;
    SceIoStat st;
    int primary = sceIoGetstat(VJO_ANKI_ACCOUNT_PATH, &st);
    if (primary < 0 && primary != MISSING) goto done;
    if (primary >= 0) {
        int backup = sceIoGetstat(BACKUP, &st);
        if (backup >= 0 && sceIoRemove(BACKUP) < 0) goto done;
        if (backup < 0 && backup != MISSING) goto done;
        if (sceIoRename(VJO_ANKI_ACCOUNT_PATH, BACKUP) < 0) goto done;
    }
    if (sceIoRename(TEMP, VJO_ANKI_ACCOUNT_PATH) < 0) {
        if (primary >= 0) sceIoRename(BACKUP, VJO_ANKI_ACCOUNT_PATH);
        sceIoSync("ux0:", 0); goto done;
    }
    rc = sceIoSync("ux0:", 0) < 0 ? -1 : 0;
done:
    memset(a->base + mark, 0, a->used - mark);
    vjo_arena_release(a, mark);
    return rc;
}

int vjo_anki_account_forget(void)
{
    /* Explicit sign-out removes every recoverable credential copy. Attempt
     * all paths even if one fails; report failure until a retry confirms it. */
    const char *paths[] = {BACKUP, TEMP, VJO_ANKI_ACCOUNT_PATH};
    int rc = 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int removed = sceIoRemove(paths[i]);
        if (removed < 0 && removed != MISSING) rc = -1;
    }
    if (sceIoSync("ux0:", 0) < 0) rc = -1;
    return rc;
}
