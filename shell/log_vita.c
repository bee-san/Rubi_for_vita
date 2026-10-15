/* Logging: UDP datagrams to log_host:9999 (tools/udp_log_listener.py) and,
 * if enabled (the default), ux0:data/VitaJPOverlay/log.txt capped at 1 MiB with
 * one rotation (log.old.txt), so at most 2 MiB of the memory card is ever used.
 * Every line is its own open/append/close, so a crash loses no written line,
 * except within a batch (vjo_log_batch_begin): the lines one thread logs in one
 * burst, from data it already holds, are written with one open/write/close per
 * file, byte for byte what line-by-line writes give, in the same order. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <stdarg.h>

#include "shell.h"

#define LOG_PORT 9999
#define LOG_FILE_CAP (1024 * 1024)
#define STATUS_CAP (64 * 1024)

static SceUID log_mutex = -1;
static int udp_fd = -1;
static SceNetSockaddrIn udp_addr;
static int file_on;
static SceOff file_size = -1;
/* The open batch, or NULL; under log_mutex (it lives on its thread's stack). */
static VjoLogBatch *batch;
static void batch_flush_locked(void);

void vjo_log_configure(const VjoConfig *cfg)
{
    if (log_mutex < 0)
        log_mutex = sceKernelCreateMutex("VjoLog", 0, 0, NULL);
    sceKernelLockMutex(log_mutex, 1, NULL);
    batch_flush_locked(); /* lines logged before the change go out as they were */
    if (udp_fd >= 0) {
        sceNetSocketClose(udp_fd);
        udp_fd = -1;
    }
    if (cfg->log_host[0]) {
        sceClibMemset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_len = sizeof(udp_addr);
        udp_addr.sin_family = SCE_NET_AF_INET;
        udp_addr.sin_port = sceNetHtons(LOG_PORT);
        if (sceNetInetPton(SCE_NET_AF_INET, cfg->log_host, &udp_addr.sin_addr) == 1)
            udp_fd = sceNetSocket("VjoLogUdp", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    }
    file_on = cfg->log_file;
    file_size = -1;
    sceKernelUnlockMutex(log_mutex, 1);
}

/* lines[0..n) of len[] bytes each, back to back at p: appended to log.txt as
 * one line at a time would, rotating before the first line that would take
 * the file over its cap; one open/write/close per run between rotations. */
static void file_append_lines(const char *p, const uint16_t *len, int n)
{
    int i = 0;
    if (file_size < 0) {
        SceIoStat st;
        file_size = sceIoGetstat(VJO_LOG_PATH, &st) >= 0 ? st.st_size : 0;
    }
    while (i < n) {
        int k = i, bytes = 0;
        SceUID fd;
        if (file_size + len[i] > LOG_FILE_CAP) {
            sceIoRemove(VJO_LOG_OLD_PATH);
            sceIoRename(VJO_LOG_PATH, VJO_LOG_OLD_PATH);
            file_size = 0;
        }
        while (k < n && file_size + bytes + len[k] <= LOG_FILE_CAP)
            bytes += len[k++];
        if (k == i) /* one line over the whole cap: written alone, as before */
            bytes = len[k++];
        fd = sceIoOpen(VJO_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (fd < 0)
            return;
        if (sceIoWrite(fd, p, bytes) == bytes)
            file_size += bytes;
        sceIoClose(fd);
        p += bytes;
        i = k;
    }
}

/* status.txt: always on, rewritten at every boot and capped at 64 KB, so the
 * plugin's state can be checked even when networking or config is broken. */
static SceUID status_mutex = -1;
static int status_size = -1;

void vjo_status_reset(void)
{
    SceUID fd;
    status_mutex = sceKernelCreateMutex("VjoStatus", 0, 0, NULL);
    sceIoMkdir(VJO_DATA_DIR, 0777);
    sceIoRemove(VJO_DATA_DIR "/status.prev.txt");
    sceIoRename(VJO_STATUS_PATH, VJO_DATA_DIR "/status.prev.txt");
    fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoClose(fd);
        status_size = 0;
    }
}

/* Undoes vjo_status_reset when the plugin does not stay resident. */
void vjo_status_close(void)
{
    if (status_mutex >= 0)
        sceKernelDeleteMutex(status_mutex);
    status_mutex = -1;
    status_size = -1;
}

/* The lines that fit under the cap, each as status_append would decide it
 * line by line, with one open; one write per run of fitting lines. */
static void status_append_lines(const char *p, const uint16_t *len, int n)
{
    SceUID fd = -1;
    int i = 0, room;
    if (status_size < 0)
        return;
    if (status_mutex >= 0)
        sceKernelLockMutex(status_mutex, 1, NULL);
    room = status_size;
    while (i < n) {
        int k = i, bytes = 0;
        while (k < n && room + bytes + len[k] <= STATUS_CAP)
            bytes += len[k++];
        if (bytes) {
            if (fd < 0)
                fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
            if (fd < 0)
                break;
            if (sceIoWrite(fd, p, bytes) == bytes)
                status_size += bytes;
            room += bytes;
            p += bytes;
        }
        while (k < n && room + len[k] > STATUS_CAP) /* too long for what is left: skipped */
            p += len[k++];
        i = k;
    }
    if (fd >= 0)
        sceIoClose(fd);
    if (status_mutex >= 0)
        sceKernelUnlockMutex(status_mutex, 1);
}

/* The open batch's lines out to both files; with log_mutex held. */
static void batch_flush_locked(void)
{
    VjoLogBatch *b = batch;
    if (!b || !b->n)
        return;
    status_append_lines(b->buf, b->len, b->n);
    if (file_on)
        file_append_lines(b->buf, b->len, b->n);
    b->n = 0;
    b->used = 0;
}

int vjo_log_batch_begin(VjoLogBatch *b, char *buf, int cap)
{
    int ok = 0;
    if (log_mutex < 0 || !b || !buf || cap <= 0)
        return 0;
    sceKernelLockMutex(log_mutex, 1, NULL);
    if (!batch) {
        b->buf = buf;
        b->cap = cap;
        b->used = b->n = 0;
        b->owner = sceKernelGetThreadId();
        batch = b;
        ok = 1;
    }
    sceKernelUnlockMutex(log_mutex, 1);
    return ok;
}

void vjo_log_batch_end(VjoLogBatch *b)
{
    if (log_mutex < 0 || !b)
        return;
    sceKernelLockMutex(log_mutex, 1, NULL);
    if (batch == b) {
        batch_flush_locked();
        batch = NULL;
    }
    sceKernelUnlockMutex(log_mutex, 1);
}

void vjo_log_raw(const char *line, int len)
{
    uint16_t n = (uint16_t)len;
    if (len <= 0 || len > 0xFFFF)
        return;
    if (log_mutex < 0) { /* before configure (boot): status.txt only */
        status_append_lines(line, &n, 1);
        return;
    }
    sceKernelLockMutex(log_mutex, 1, NULL);
    if (udp_fd >= 0)
        sceNetSendto(udp_fd, line, len, 0, (SceNetSockaddr *)&udp_addr, sizeof(udp_addr));
    if (batch && batch->owner == sceKernelGetThreadId() && batch->n < VJO_LOG_BATCH_LINES &&
        batch->cap - batch->used >= len) {
        sceClibMemcpy(batch->buf + batch->used, line, len);
        batch->used += len;
        batch->len[batch->n++] = n;
    } else {
        /* Another thread's line, or one the batch cannot hold: the batch's
         * lines go first, so the files keep the order of the calls. */
        batch_flush_locked();
        if (batch && batch->owner == sceKernelGetThreadId() && batch->cap >= len) {
            sceClibMemcpy(batch->buf, line, len);
            batch->used = len;
            batch->len[batch->n++] = n;
        } else {
            status_append_lines(line, &n, 1);
            if (file_on)
                file_append_lines(line, &n, 1);
        }
    }
    sceKernelUnlockMutex(log_mutex, 1);
}

/* Whether a line can go anywhere: with no file, no UDP host and status.txt
 * full (or never opened), vjo_log formats nothing. */
static int log_has_sink(void)
{
    return file_on || udp_fd >= 0 || (status_size >= 0 && status_size <= STATUS_CAP - 2);
}

void vjo_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;
    if (!log_has_sink())
        return;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    vjo_log_raw(line, n);
}
