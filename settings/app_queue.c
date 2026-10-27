#include "app_queue.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

void vjo_log(const char *fmt, ...); /* the app's log.txt lines (settings/applog.c) */

#define DATA_DIR "ux0:data/VitaJPOverlay"
#define QUEUE_DIR VJO_QUEUE_DIR
#define PATH_CAP 128

static int io_fail(const char *call, const char *path, int rc)
{
    vjo_log("anki queue: %s %s failed 0x%08X", call, path, (unsigned)rc);
    return -1;
}

/* The mount's buffer cache, the renames and removals included (shell/anki_queue.c mount_sync). */
static int mount_sync(void)
{
    int rc = sceIoSync("ux0:", 0);
    return rc < 0 ? io_fail("sync", "ux0:", rc) : 0;
}

static int prepare(void)
{
    SceIoStat st;
    int rc;
    sceIoMkdir(DATA_DIR, 0777); /* fails when it exists */
    sceIoMkdir(QUEUE_DIR, 0777);
    memset(&st, 0, sizeof(st));
    if ((rc = sceIoGetstat(QUEUE_DIR, &st)) < 0)
        return io_fail("getstat", QUEUE_DIR, rc);
    if (!SCE_S_ISDIR(st.st_mode)) {
        vjo_log("anki queue: %s is not a directory", QUEUE_DIR);
        return -1;
    }
    return 0;
}

/* The card under this ID already: queued, exported, or found in Anki by an earlier build's AnkiConnect send. A
 * previous rename may be visible even though its final sync failed: never acknowledge a retry until the directory
 * is durable (shell/anki_queue.c find). 0 when there is none. */
static int find(const char *id)
{
    static const struct {
        const char *dir, *ext;
        int rc;
    } where[] = {{QUEUE_DIR, "json", VJO_APP_QUEUE_EXISTS},
                 {VJO_QUEUE_EXPORTED_DIR, "json", VJO_APP_QUEUE_ARCHIVED},
                 {QUEUE_DIR, "duplicate.json", VJO_APP_QUEUE_ARCHIVED}};
    char path[PATH_CAP];
    SceIoStat st;
    for (unsigned i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s.%s", where[i].dir, id, where[i].ext);
        if (sceIoGetstat(path, &st) >= 0)
            return mount_sync() < 0 ? VJO_APP_QUEUE_FAILED : where[i].rc;
    }
    return 0;
}

/* The record under its new name: a temporary file, written, flushed, closed, renamed (never over a card: the name
 * is new), then the mount synced. 0, or -1 (logged), with no temporary file left. */
static int commit(const char *path, const char *data, size_t len)
{
    char tmp[PATH_CAP + 8];
    SceUID fd;
    size_t off = 0;
    int rc = 0, n;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return io_fail("open", tmp, fd);
    while (!rc && off < len) {
        n = sceIoWrite(fd, data + off, (SceSize)(len - off));
        if (n <= 0 || (size_t)n > len - off)
            rc = io_fail("write", tmp, n);
        else
            off += (size_t)n;
    }
    if (!rc && (n = sceIoSyncByFd(fd, 0)) < 0)
        rc = io_fail("flush", tmp, n);
    if ((n = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", tmp, n);
    if (!rc && (n = sceIoRename(tmp, path)) < 0)
        rc = io_fail("rename", tmp, n);
    if (rc) {
        sceIoRemove(tmp); /* never a card */
        return -1;
    }
    return mount_sync();
}

int vjo_app_queue_save(VjoArena *a, const VjoAnkiNote *note, char out_id[VJO_QUEUE_ID_SIZE])
{
    char id[2][VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    size_t mark = vjo_arena_mark(a), len;
    char *json;
    int rc;
    if (out_id)
        out_id[0] = '\0';
    /* Its records without and with a picture: either one, queued or exported, is this card. */
    for (int k = 0; k <= 1; k++) {
        if (!(json = vjo_queue_encode(a, note, k))) {
            vjo_log("anki queue: card over %u KiB of text, or out of memory", VJO_QUEUE_TEXT_MAX >> 10);
            vjo_arena_release(a, mark);
            return VJO_APP_QUEUE_TOO_LARGE;
        }
        vjo_queue_id(json, strlen(json), id[k]);
        vjo_arena_release(a, mark);
        if (k == 0 && prepare() < 0)
            return VJO_APP_QUEUE_FAILED;
        if ((rc = find(id[k])) != 0) {
            if (out_id)
                memcpy(out_id, id[k], VJO_QUEUE_ID_SIZE);
            return rc;
        }
    }
    if (!(json = vjo_queue_encode(a, note, 0))) {
        vjo_arena_release(a, mark);
        return VJO_APP_QUEUE_TOO_LARGE; /* (cannot happen: the same card fit above) */
    }
    snprintf(path, sizeof(path), "%s/%s.json", QUEUE_DIR, id[0]);
    len = strlen(json);
    rc = commit(path, json, len);
    vjo_arena_release(a, mark);
    if (rc < 0)
        return VJO_APP_QUEUE_FAILED;
    if (out_id)
        memcpy(out_id, id[0], VJO_QUEUE_ID_SIZE);
    vjo_log("anki queue: %s saved from the Look up page, %u B record", id[0], (unsigned)len);
    return VJO_APP_QUEUE_SAVED;
}
