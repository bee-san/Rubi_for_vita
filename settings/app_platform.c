#include "app_platform.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <string.h>

static uint64_t app_now_us(void *ud)
{
    (void)ud;
    return (uint64_t)sceKernelGetProcessTimeWide();
}

/* One sceIoPread a call (the offset with the read: no seek), again after a short read, at the offset it reached
 * (shell/platform_vita.c local_read). */
static int app_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    SceUID fd = (SceUID)(intptr_t)ctx;
    uint8_t *out = dst;
    if (off > INT64_MAX || n > INT64_MAX - off)
        return -1;
    while (n) {
        int got = sceIoPread(fd, out, (SceSize)n, (SceOff)off);
        if (got <= 0 || (size_t)got > n)
            return -1;
        out += got;
        n -= (size_t)got;
        off += (uint64_t)got;
    }
    return 0;
}

static void app_close(void *ctx) { sceIoClose((SceUID)(intptr_t)ctx); }

static int app_open(void *ud, const char *path, VjoFile *out)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    (void)ud;
    if (fd < 0)
        return -1;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) {
        sceIoClose(fd);
        return -1;
    }
    memset(out, 0, sizeof(*out)); /* stamp 0: unknown, as the Shell's */
    out->ctx = (void *)(intptr_t)fd;
    out->size = (uint64_t)size;
    out->read = app_read;
    out->close = app_close;
    return 0;
}

void vjo_app_platform(VjoPlatform *p)
{
    memset(p, 0, sizeof(*p));
    p->file_open = app_open;
    p->now_us = app_now_us;
}
