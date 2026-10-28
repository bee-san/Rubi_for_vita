/* The Rubi app's small file helpers, each once for the files that had their own copy: the SDK's "no such file", the
 * clock's Unix epoch, a whole write (settings/anki_export.c, install.c, psp.c) and an exact read (anki_export.c,
 * psp.c). Static inline, as the copies were static: each file keeps its own code. The app's own: the Shell keeps its
 * writer (shell/anki_queue.c). */
#ifndef VJO_SETTINGS_APP_IO_H
#define VJO_SETTINGS_APP_IO_H
#include <psp2/io/fcntl.h>
#include <stddef.h>

/* sceIo*'s result for a file or directory that is not there (SCE_ERROR_ERRNO_ENOENT) */
#define VJO_APP_MISSING ((int)0x80010002u)
/* SceRtcTick's microseconds from 0001-01-01 to 1970-01-01 */
#define VJO_APP_RTC_UNIX_EPOCH 62135596800000000ull

/* len bytes of data to fd, written again after a short write: 0, or -1 (then *failed, unless failed is NULL, is the
 * write's result that stopped it). */
static inline int vjo_app_write_all(SceUID fd, const void *data, size_t len, int *failed)
{
    for (size_t used = 0; used < len;) {
        int n = sceIoWrite(fd, (const char *)data + used, (SceSize)(len - used));
        if (n <= 0) {
            if (failed)
                *failed = n;
            return -1;
        }
        used += (size_t)n;
    }
    return 0;
}

/* Exactly n bytes of fd into buf, read again after a short read: 0, or -1. */
static inline int vjo_app_read_exact(SceUID fd, void *buf, size_t n)
{
    for (size_t done = 0; done < n;) {
        int k = sceIoRead(fd, (char *)buf + done, (SceSize)(n - done));
        if (k <= 0)
            return -1;
        done += (size_t)k;
    }
    return 0;
}

#endif
