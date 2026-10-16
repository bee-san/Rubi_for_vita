/* POSIX implementation of VjoPlatform for the host CLI, tools and tests:
 * read-only files and a monotonic clock (the overlay is offline). */
#include "platform_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t posix_now_us(void *ud)
{
    struct timespec ts;
    (void)ud;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* No stdio buffering: local_dict.c owns its bounded page cache. */
static int local_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    int fd = (int)(intptr_t)ctx;
    uint8_t *out = dst;
    while (n) {
        ssize_t got = pread(fd, out, n, (off_t)off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        out += got; off += (size_t)got; n -= (size_t)got;
    }
    return 0;
}

static void local_close(void *ctx) { close((int)(intptr_t)ctx); }

static int local_open(void *ud, const char *path, VjoFile *out)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    (void)ud;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_size < 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    out->ctx = (void *)(intptr_t)fd; out->size = (uint64_t)st.st_size;
    out->read = local_read; out->close = local_close;
    /* Modification time in ns; never 0 for a real file. */
#ifdef __APPLE__
    out->stamp = (uint64_t)st.st_mtimespec.tv_sec * 1000000000u + (uint64_t)st.st_mtimespec.tv_nsec;
#else
    out->stamp = (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
#endif
    if (!out->stamp) out->stamp = 1;
    return 0;
}

void posix_platform_init(PosixPlatform *pp, VjoPlatform *p)
{
    memset(p, 0, sizeof(*p));
    p->ud = pp;
    p->file_open = local_open;
    p->now_us = posix_now_us;
}
