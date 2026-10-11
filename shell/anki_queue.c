#include "anki_queue.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#include "bearssl.h"

#define MISSING ((int)0x80010002u)
#define PREFIX_SIZE 48
/* Keep scan scratch on the existing worker stack (well below 1 KiB). */
static int scan(int fd, const char *wanted, SceOff *good, int *found, int *partial)
{
    unsigned char block[256], hashed[256], digest[32];
    char prefix[PREFIX_SIZE + 1], actual[VJO_QUEUE_ID_SIZE];
    static const char hex[] = "0123456789abcdef";
    br_sha256_context hash;
    size_t line = 0;
    SceOff offset = 0;
    int count = 0, last = 0;
    *good = 0; *found = 0; *partial = 0;
    for (;;) {
        int n = sceIoRead(fd, block, sizeof(block));
        if (n < 0 || n > (int)sizeof(block)) return -1;
        if (!n) break;
        size_t hash_bytes = 0;
        for (int i = 0; i < n; i++) {
            unsigned char ch = block[i];
            if (++offset > VJO_QUEUE_FILE_MAX) return -2;
            if (ch == '\n') {
                if (line <= PREFIX_SIZE + 2 || last != '}' ||
                    memcmp(prefix, "{\"id\":\"", 7) || memcmp(prefix + 39, "\",\"note\":", 9)) return -1;
                prefix[39] = 0;
                if (!vjo_queue_valid_id(prefix + 7)) return -1;
                br_sha256_update(&hash, hashed, hash_bytes); hash_bytes = 0;
                br_sha256_out(&hash, digest);
                for (int k = 0; k < 16; k++) {
                    actual[k * 2] = hex[digest[k] >> 4];
                    actual[k * 2 + 1] = hex[digest[k] & 15];
                }
                actual[32] = 0;
                if (strcmp(actual, prefix + 7)) return -1;
                if (wanted && !strcmp(actual, wanted)) *found = 1;
                count++; *good = offset; line = 0;
            } else {
                if (line < PREFIX_SIZE) prefix[line] = (char)ch;
                else {
                    if (line == PREFIX_SIZE) {
                        if (ch != '{') return -1;
                        br_sha256_init(&hash);
                    } else {
                        hashed[hash_bytes++] = (unsigned char)last;
                    }
                    last = ch;
                }
                if (++line > VJO_QUEUE_TEXT_MAX + PREFIX_SIZE + 1) return -1;
            }
        }
        if (hash_bytes) br_sha256_update(&hash, hashed, hash_bytes);
    }
    *partial = line != 0;
    return count;
}

int vjo_queue_count(void)
{
    int fd = sceIoOpen(VJO_QUEUE_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return fd == MISSING ? 0 : -1;
    SceOff good;
    int found, partial;
    int count = scan(fd, NULL, &good, &found, &partial);
    if (sceIoClose(fd) < 0) return -1;
    return count;
}

static int write_all(int fd, const char *data, size_t n)
{
    while (n) {
        int written = sceIoWrite(fd, data, n);
        if (written <= 0 || (size_t)written > n) return -1;
        data += written; n -= (size_t)written;
    }
    return 0;
}

int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note,
                   const VjoConfig *cfg, const char *source_id)
{
    size_t mark = vjo_arena_mark(a);
    char *json = vjo_queue_encode(a, note, cfg, source_id);
    char id[VJO_QUEUE_ID_SIZE], prefix[PREFIX_SIZE + 1];
    int fd = -1, rc = -1, found, partial;
    SceOff good;
    if (!json) { rc = -3; goto done; }
    size_t len = strlen(json);
    vjo_queue_id(json, len, id);
    memcpy(prefix, "{\"id\":\"", 7); memcpy(prefix + 7, id, 32);
    memcpy(prefix + 39, "\",\"note\":", 9);
    sceIoMkdir("ux0:data", 0777); sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    fd = sceIoOpen(VJO_QUEUE_PATH, SCE_O_RDWR | SCE_O_CREAT, 0666);
    if (fd < 0) goto done;
    int count = scan(fd, id, &good, &found, &partial);
    if (count < 0) { rc = count; goto done; }
    if (partial) {
        SceIoStat st;
        memset(&st, 0, sizeof(st)); st.st_size = good;
        if (sceIoChstatByFd(fd, &st, SCE_CST_SIZE) < 0) goto done;
    }
    if (!found) {
        if ((uint64_t)good + PREFIX_SIZE + len + 2 > VJO_QUEUE_FILE_MAX) { rc = -2; goto done; }
        if (sceIoLseek(fd, good, SCE_SEEK_SET) != good ||
            write_all(fd, prefix, PREFIX_SIZE) || write_all(fd, json, len) || write_all(fd, "}\n", 2)) goto done;
    }
    /* A repeated save after an uncertain sync must flush again before it
     * can be acknowledged as already saved. */
    if (sceIoSyncByFd(fd, 0) < 0) goto done;
    rc = found ? 1 : 0;
done:
    if (fd >= 0 && sceIoClose(fd) < 0) rc = -1;
    if (rc >= 0 && sceIoSync("ux0:", 0) < 0) rc = -1;
    vjo_arena_release(a, mark);
    return rc;
}
