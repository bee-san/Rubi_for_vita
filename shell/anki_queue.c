/* Vita queue storage: one committed JSON record per card. A commit is a
 * temp file, written, flushed, closed and renamed, then a sync of the mount:
 * a card is visible only once it is whole. Temp files are never cards. A
 * failed write never erases a card. Every failed storage call is logged
 * with its SCE error code. */
#include <psp2/kernel/clib.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#include "anki_queue.h"
#include "shell.h"

#define QUEUE_DIR VJO_ANKI_QUEUE_DIR
#define PATH_CAP 128

static void path_for(char path[PATH_CAP], const char *dir, const char *id, const char *ext)
{
    sceClibSnprintf(path, PATH_CAP, "%s/%s.%s", dir, id, ext);
}

static int io_fail(const char *call, const char *path, int rc)
{
    vjo_log("anki queue: %s %s failed 0x%08X", call, path, (unsigned)rc);
    return -1;
}

/* The mount's buffer cache, including renames and removals. */
static int mount_sync(void)
{
    int rc = sceIoSync("ux0:", 0);
    return rc < 0 ? io_fail("sync", "ux0:", rc) : 0;
}

static int prepare(void)
{
    SceIoStat st;
    int rc;
    sceIoMkdir(VJO_DATA_DIR, 0777); /* fails when it exists */
    sceIoMkdir(QUEUE_DIR, 0777);
    if ((rc = sceIoGetstat(QUEUE_DIR, &st)) < 0)
        return io_fail("getstat", QUEUE_DIR, rc);
    if (!SCE_S_ISDIR(st.st_mode)) {
        vjo_log("anki queue: %s is not a directory", QUEUE_DIR);
        return -1;
    }
    return 0;
}

int vjo_queue_count(void)
{
    SceUID dir;
    SceIoDirent ent;
    int n = 0, rc, closed;
    if (prepare() < 0)
        return -1;
    if ((dir = sceIoDopen(QUEUE_DIR)) < 0)
        return io_fail("dopen", QUEUE_DIR, dir);
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        rc = sceIoDread(dir, &ent);
        if (rc <= 0)
            break;
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || strlen(ent.d_name) != 37 || strcmp(ent.d_name + 32, ".json"))
            continue;
        ent.d_name[32] = '\0';
        n += vjo_queue_valid_id(ent.d_name);
    }
    if (rc < 0)
        io_fail("dread", QUEUE_DIR, rc);
    if ((closed = sceIoDclose(dir)) < 0)
        return io_fail("dclose", QUEUE_DIR, closed);
    return rc < 0 ? -1 : n;
}

/* Never remove the destination to rename over it: a commit is a new name. */
static int commit(const char *path, const void *data, size_t len)
{
    char tmp[PATH_CAP];
    SceUID fd;
    size_t off = 0;
    int rc = 0, n;
    sceClibSnprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return io_fail("open", tmp, fd);
    while (!rc && off < len) {
        n = sceIoWrite(fd, (const char *)data + off, len - off);
        if (n <= 0)
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

int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note)
{
    char id[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    SceIoStat st;
    char *json;
    if (!(json = vjo_queue_encode(a, note, 0))) {
        vjo_log("anki queue: card over %u KiB of text, or out of memory", VJO_QUEUE_TEXT_MAX >> 10);
        return VJO_QUEUE_TOO_LARGE;
    }
    if (prepare() < 0)
        return VJO_QUEUE_FAILED;
    vjo_queue_id(json, strlen(json), id);
    path_for(path, QUEUE_DIR, id, "json");
    if (sceIoGetstat(path, &st) >= 0)
        /* A previous rename may be visible even though its final mount sync
         * failed. Never acknowledge a retry until the directory is durable. */
        return mount_sync() < 0 ? VJO_QUEUE_FAILED : VJO_QUEUE_EXISTS;
    /* the same card in an exported package already (or one AnkiConnect had, an earlier build's);
     * the move that put it there is made durable first, as a retry's rename above */
    path_for(path, VJO_ANKI_EXPORTED_DIR, id, "json");
    if (sceIoGetstat(path, &st) >= 0)
        return mount_sync() < 0 ? VJO_QUEUE_FAILED : VJO_QUEUE_ARCHIVED;
    path_for(path, QUEUE_DIR, id, "duplicate.json");
    if (sceIoGetstat(path, &st) >= 0)
        return mount_sync() < 0 ? VJO_QUEUE_FAILED : VJO_QUEUE_ARCHIVED;
    path_for(path, QUEUE_DIR, id, "json");
    if (commit(path, json, strlen(json)) < 0)
        return VJO_QUEUE_FAILED;
    vjo_log("anki queue: %s saved, %u B record", id, (unsigned)strlen(json));
    return VJO_QUEUE_SAVED;
}
