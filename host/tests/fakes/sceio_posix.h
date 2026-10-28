/* The Vita's file calls over the host's files: a memory card of real files and folders (a suite's
 * temporary directory, test_tmpdir.h) for the suites that run the plugins' and the Rubi app's own
 * storage code against the SDK's declarations in host/tests/vita_stubs/psp2/io/. Each sceIo* call is
 * its POSIX twin, and a failure comes back as the SDK returns one: SCEIO_ERROR(errno), by default
 * 0x80010000 | errno (SCE_ERROR_ERRNO_*; a suite may define its own mapping before the include).
 *
 * A program includes this once. The calls a suite fakes otherwise (a fault, a counter, a short read)
 * it defines itself and names before the include, as SCEIO_OWN (SCEIO_OPEN | SCEIO_GETSTAT | ...);
 * its own can end in the call here, sceio_<call>(). sceIoGetstat fills the whole SceIoStat: mode,
 * size and the three times (UTC, as the Vita gives them). Directory listings: SCEIO_DIRS at once,
 * handles 100 + slot (sceio_dirs and sceio_dir_paths, which a suite's own Dread reads), every entry
 * readdir gives ("." and ".." too: a Dread loop skips what is not its own) with the entry's
 * sceio_getstat as its d_stat (included after acutest.h, an entry it cannot stat fails the test). */
#ifndef VJO_TEST_SCEIO_POSIX_H
#define VJO_TEST_SCEIO_POSIX_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

/* SCEIO_OWN's bits: the calls a suite defines itself */
#define SCEIO_OPEN 0x1
#define SCEIO_CLOSE 0x2
#define SCEIO_READ 0x4
#define SCEIO_WRITE 0x8
#define SCEIO_PREAD 0x10
#define SCEIO_PWRITE 0x20
#define SCEIO_LSEEK 0x40
#define SCEIO_MKDIR 0x80
#define SCEIO_GETSTAT 0x100
#define SCEIO_RENAME 0x200
#define SCEIO_REMOVE 0x400
#define SCEIO_SYNC 0x800
#define SCEIO_SYNCBYFD 0x1000
#define SCEIO_DOPEN 0x2000
#define SCEIO_DREAD 0x4000
#define SCEIO_DCLOSE 0x8000
#ifndef SCEIO_OWN
#define SCEIO_OWN 0
#endif

#ifndef SCEIO_ERROR
#define SCEIO_ERROR(e) ((int)(0x80010000u | (unsigned)(e)))
#endif
/* A POSIX call's result as the SDK's: n, or SCEIO_ERROR(errno) when it failed. */
static inline int sceio_native(long n) { return n < 0 ? SCEIO_ERROR(errno) : (int)n; }

static inline int sceio_open(const char *p, int flags, unsigned mode) { return sceio_native(open(p, flags, mode)); }
static inline int sceio_close(int fd) { return sceio_native(close(fd)); }
static inline int sceio_read(int fd, void *p, size_t n) { return sceio_native(read(fd, p, n)); }
static inline int sceio_write(int fd, const void *p, size_t n) { return sceio_native(write(fd, p, n)); }
static inline int sceio_pread(int fd, void *p, size_t n, SceOff off) { return sceio_native(pread(fd, p, n, off)); }
static inline int sceio_pwrite(int fd, const void *p, size_t n, SceOff off)
{
    return sceio_native(pwrite(fd, p, n, off));
}
static inline SceOff sceio_lseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
static inline int sceio_mkdir(const char *p, unsigned mode) { return sceio_native(mkdir(p, mode)); }
static inline int sceio_rename(const char *from, const char *to) { return sceio_native(rename(from, to)); }
static inline int sceio_remove(const char *p) { return sceio_native(unlink(p)); }
static inline int sceio_sync(const char *device, unsigned flags) { (void)device, (void)flags; return 0; }
static inline int sceio_syncbyfd(int fd, int flag) { (void)flag; return sceio_native(fsync(fd)); }

/* A file's time as the Vita gives it: UTC (VitaShell converts st_mtime to local time to show it). */
static inline void sceio_date(time_t t, SceDateTime *d)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    *d = (SceDateTime){(unsigned short)(tm.tm_year + 1900), (unsigned short)(tm.tm_mon + 1), (unsigned short)tm.tm_mday,
                       (unsigned short)tm.tm_hour, (unsigned short)tm.tm_min, (unsigned short)tm.tm_sec, 0};
}
static inline int sceio_getstat(const char *p, SceIoStat *st)
{
    struct stat s;
    if (stat(p, &s))
        return sceio_native(-1);
    memset(st, 0, sizeof(*st));
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    sceio_date(s.st_ctim.tv_sec, &st->st_ctime);
    sceio_date(s.st_atim.tv_sec, &st->st_atime);
    sceio_date(s.st_mtim.tv_sec, &st->st_mtime);
    return 0;
}

#define SCEIO_DIRS 8
static DIR *sceio_dirs[SCEIO_DIRS];
static char sceio_dir_paths[SCEIO_DIRS][256];
static inline int sceio_dopen(const char *p)
{
    for (int i = 0; i < SCEIO_DIRS; i++)
        if (!sceio_dirs[i]) {
            if (!(sceio_dirs[i] = opendir(p)))
                return sceio_native(-1);
            snprintf(sceio_dir_paths[i], sizeof(sceio_dir_paths[i]), "%s", p);
            return 100 + i;
        }
    return SCEIO_ERROR(EMFILE);
}
/* The listing's DIR, or NULL for a handle that is not an open listing's. */
static inline DIR *sceio_dir(int d) { return d >= 100 && d < 100 + SCEIO_DIRS ? sceio_dirs[d - 100] : NULL; }
static inline int sceio_dread(int d, SceIoDirent *ent)
{
    DIR *dir = sceio_dir(d);
    struct dirent *e;
    char path[600];
    int rc;
    if (!dir)
        return SCEIO_ERROR(EBADF);
    if (!(e = readdir(dir)))
        return 0;
    memset(ent, 0, sizeof(*ent));
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", sceio_dir_paths[d - 100], e->d_name);
    rc = sceio_getstat(path, &ent->d_stat);
#ifdef TEST_ASSERT_ /* (acutest's: an entry the fake lists but cannot stat is the fake's failure, not the code's) */
    TEST_ASSERT_(rc == 0, "sceIoDread: cannot stat %s", path);
#endif
    return rc < 0 ? rc : 1;
}
static inline int sceio_dclose(int d)
{
    DIR *dir = sceio_dir(d);
    if (!dir)
        return SCEIO_ERROR(EBADF);
    sceio_dirs[d - 100] = NULL;
    return sceio_native(closedir(dir));
}

/* The calls themselves, but for the suite's own. */
#if !(SCEIO_OWN & SCEIO_OPEN)
int sceIoOpen(const char *p, int flags, unsigned mode) { return sceio_open(p, flags, mode); }
#endif
#if !(SCEIO_OWN & SCEIO_CLOSE)
int sceIoClose(int fd) { return sceio_close(fd); }
#endif
#if !(SCEIO_OWN & SCEIO_READ)
int sceIoRead(int fd, void *p, size_t n) { return sceio_read(fd, p, n); }
#endif
#if !(SCEIO_OWN & SCEIO_WRITE)
int sceIoWrite(int fd, const void *p, size_t n) { return sceio_write(fd, p, n); }
#endif
#if !(SCEIO_OWN & SCEIO_PREAD)
int sceIoPread(int fd, void *p, size_t n, SceOff off) { return sceio_pread(fd, p, n, off); }
#endif
#if !(SCEIO_OWN & SCEIO_PWRITE)
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off) { return sceio_pwrite(fd, p, n, off); }
#endif
#if !(SCEIO_OWN & SCEIO_LSEEK)
SceOff sceIoLseek(int fd, SceOff off, int whence) { return sceio_lseek(fd, off, whence); }
#endif
#if !(SCEIO_OWN & SCEIO_MKDIR)
int sceIoMkdir(const char *p, unsigned mode) { return sceio_mkdir(p, mode); }
#endif
#if !(SCEIO_OWN & SCEIO_GETSTAT)
int sceIoGetstat(const char *p, SceIoStat *st) { return sceio_getstat(p, st); }
#endif
#if !(SCEIO_OWN & SCEIO_RENAME)
int sceIoRename(const char *from, const char *to) { return sceio_rename(from, to); }
#endif
#if !(SCEIO_OWN & SCEIO_REMOVE)
int sceIoRemove(const char *p) { return sceio_remove(p); }
#endif
#if !(SCEIO_OWN & SCEIO_SYNC)
int sceIoSync(const char *device, unsigned flags) { return sceio_sync(device, flags); }
#endif
#if !(SCEIO_OWN & SCEIO_SYNCBYFD)
int sceIoSyncByFd(int fd, int flag) { return sceio_syncbyfd(fd, flag); }
#endif
#if !(SCEIO_OWN & SCEIO_DOPEN)
int sceIoDopen(const char *p) { return sceio_dopen(p); }
#endif
#if !(SCEIO_OWN & SCEIO_DREAD)
int sceIoDread(int d, SceIoDirent *ent) { return sceio_dread(d, ent); }
#endif
#if !(SCEIO_OWN & SCEIO_DCLOSE)
int sceIoDclose(int d) { return sceio_dclose(d); }
#endif
#endif
