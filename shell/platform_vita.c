/* VjoPlatform for SceShell: the local dictionaries' files (sceIo) and the
 * process clock. The overlay is offline: SceShell's plugin opens no socket. */
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "shell.h"

static uint64_t vita_now_us(void *ud)
{
    (void)ud;
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static int local_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    SceUID fd = (SceUID)(intptr_t)ctx;
    uint8_t *out = dst;
    vjo_dict_reads++; /* the lookup log's read count (worker.c) */
    vjo_dict_read_bytes += (uint32_t)n;
    if (off > INT64_MAX || sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) != (SceOff)off) return -1;
    while (n) {
        int got = sceIoRead(fd, out, (SceSize)n);
        if (got <= 0) return -1;
        out += got; n -= (size_t)got;
    }
    return 0;
}

static void local_close(void *ctx) { sceIoClose((SceUID)(intptr_t)ctx); }

static int local_open(void *ud, const char *path, VjoFile *out)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    (void)ud;
    if (fd < 0) return -1;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) { sceIoClose(fd); return -1; }
    out->ctx = (void *)(intptr_t)fd; out->size = (uint64_t)size;
    out->read = local_read; out->close = local_close;
    return 0;
}

void vjo_platform_vita(VjoPlatform *p)
{
    sceClibMemset(p, 0, sizeof(*p));
    p->file_open = local_open;
    p->now_us = vita_now_us;
}
