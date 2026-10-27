/* shell/platform_vita.c on the host: the Shell's VjoPlatform, all that is left of it since the
 * network went (the local dictionaries' files through sceIo, and the process clock). The SDK's
 * sceIo calls are stubbed over real files: reads (sceIoPread, the offset with each read) come back
 * short to exercise the loop, an open or the size's seek can be made to fail, and every descriptor
 * and seek is counted (audit-overlay-shell-slim-3 N6). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

uint32_t vjo_dict_reads, vjo_dict_read_bytes; /* shell/worker.c's: the lookup log's counts */

static uint64_t clock_now;
static int fail_open, fail_seek, open_fds, max_read, reads, eof_reads, seeks;
static SceOff read_at[8]; /* the offsets of the first sceIoPread calls since reset */
uint64_t sceKernelGetProcessTimeWide(void) { return clock_now; }
int sceIoOpen(const char *path, int flags, unsigned mode)
{
    if (fail_open) return (int)0x80010002; /* SCE_ERROR_ERRNO_ENOENT */
    int fd = open(path, flags, mode);
    if (fd < 0) return (int)0x80010002;
    open_fds++;
    return fd;
}
int sceIoClose(int fd) { open_fds--; return close(fd); }
int sceIoPread(int fd, void *data, size_t len, SceOff off)
{
    if (reads < 8) read_at[reads] = off;
    reads++;
    if (off < 0) return (int)0x80010016; /* EINVAL */
    if (max_read && len > (size_t)max_read) len = (size_t)max_read; /* the SDK may return less */
    int got = (int)pread(fd, data, len, off);
    /* at the end of the file: 0 once, then an error, so a reader that loops on 0 stops (and is seen) */
    if (got == 0 && eof_reads++) return (int)0x80010005; /* EIO */
    return got;
}
SceOff sceIoLseek(int fd, SceOff off, int whence)
{
    seeks++;
    if (fail_seek && whence == SCE_SEEK_END) return (SceOff)(int)0x80010016; /* EINVAL */
    return lseek(fd, off, whence);
}

#include "../../shell/platform_vita.c"

static char path[PATH_MAX];
static const char contents[] = "0123456789abcdefghijklmnopqrstuvwxyz";

static void reset(void)
{
    clock_now = 0;
    fail_open = fail_seek = open_fds = max_read = reads = eof_reads = seeks = 0;
    memset(read_at, 0, sizeof(read_at));
    vjo_dict_reads = vjo_dict_read_bytes = 0;
    TEST_ASSERT(snprintf(path, sizeof(path), "%s/%d.bin", vjo_test_tmpdir_path, (int)getpid()) < (int)sizeof(path));
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(contents, 1, sizeof(contents) - 1, f) == sizeof(contents) - 1);
    fclose(f);
}

static void test_the_platform_is_files_and_a_clock(void)
{
    VjoPlatform p;
    reset();
    memset(&p, 0x5A, sizeof(p));
    vjo_platform_vita(&p);
    TEST_CHECK(p.ud == NULL && p.file_open == local_open && p.now_us == vita_now_us);
    clock_now = 987654321;
    TEST_CHECK(p.now_us(p.ud) == 987654321); /* the process clock, in microseconds */
    unlink(path);
}

static void test_a_file_reads_exactly_what_it_holds(void)
{
    VjoPlatform p;
    VjoFile f;
    char buf[64];
    reset();
    vjo_platform_vita(&p);
    max_read = 5; /* every sceIoPread returns at most 5 bytes: the loop must ask again, further on */
    memset(&f, 0, sizeof(f));
    TEST_ASSERT(p.file_open(p.ud, path, &f) == 0);
    TEST_CHECK(f.size == sizeof(contents) - 1 && f.read && f.close && open_fds == 1);
    TEST_CHECK(seeks == 1); /* the size, at the open */
    memset(buf, 0, sizeof(buf));
    TEST_CHECK(f.read(f.ctx, 10, buf, 17) == 0 && !memcmp(buf, contents + 10, 17));
    TEST_CHECK_(reads == 4 && read_at[0] == 10 && read_at[1] == 15 && read_at[2] == 20 && read_at[3] == 25,
                "%d calls at %lld, %lld, %lld, %lld", reads, (long long)read_at[0], (long long)read_at[1],
                (long long)read_at[2], (long long)read_at[3]);
    TEST_CHECK(vjo_dict_reads == 1 && vjo_dict_read_bytes == 17);
    /* the descriptor's own position is not the read's: none of it is a seek */
    TEST_CHECK(f.read(f.ctx, 0, buf, 3) == 0 && !memcmp(buf, "012", 3));
    TEST_CHECK(vjo_dict_reads == 2 && vjo_dict_read_bytes == 20 && seeks == 1);
    /* past the end: a short read is an error, not a partial success, and the end is not read again */
    reads = 0;
    TEST_CHECK(f.read(f.ctx, sizeof(contents) - 4, buf, 8) == -1);
    TEST_CHECK_(reads == 2, "%d sceIoPread calls for a read that meets the end", reads); /* 3 bytes, then 0 */
    /* an error at the first call: nothing more is read */
    reads = 0;
    TEST_CHECK(f.read(f.ctx, (uint64_t)sizeof(contents) + 100, buf, 3) == -1 && reads == 1);
    reads = 0;
    TEST_CHECK(f.read(f.ctx, (uint64_t)INT64_MAX + 1, buf, 1) == -1 && reads == 0); /* an offset SceOff cannot hold */
    f.close(f.ctx);
    TEST_CHECK(open_fds == 0);
    unlink(path);
}

static void test_a_failed_open_or_seek_opens_nothing(void)
{
    VjoPlatform p;
    VjoFile f;
    reset();
    vjo_platform_vita(&p);
    memset(&f, 0, sizeof(f));
    char missing[PATH_MAX + 32];
    snprintf(missing, sizeof(missing), "%s/missing/none.vjdict", vjo_test_tmpdir_path);
    TEST_CHECK(p.file_open(p.ud, missing, &f) == -1);
    fail_open = 1;
    TEST_CHECK(p.file_open(p.ud, path, &f) == -1 && open_fds == 0);
    fail_open = 0;
    fail_seek = 1; /* the size cannot be read: the descriptor is closed again */
    TEST_CHECK(p.file_open(p.ud, path, &f) == -1 && open_fds == 0);
    TEST_CHECK(!f.read && !f.close);
    unlink(path);
}

TEST_LIST = {
    {"the_platform_is_files_and_a_clock", test_the_platform_is_files_and_a_clock},
    {"a_file_reads_exactly_what_it_holds", test_a_file_reads_exactly_what_it_holds},
    {"a_failed_open_or_seek_opens_nothing", test_a_failed_open_or_seek_opens_nothing},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("platform-vita")) return 2; /* the test files in it, removed at the end */
    return acutest_main(argc, argv);
}
