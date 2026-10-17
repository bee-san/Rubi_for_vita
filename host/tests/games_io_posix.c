/* The Shell's and the Rubi app's file calls (vita/games_storage.c) over real
 * files, for host/tests/test_kernel_loader.c's parity check: the kernel's
 * games.ini reader against the Shell's own on the same files. Reads come back
 * 13 bytes at a time, as test_games.c's, so every short-read loop runs. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <errno.h>

static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode) { return native(open(p, flags, mode)); }
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t len) { return native((int)read(fd, p, len > 13 ? 13 : len)); }
int sceIoWrite(int fd, const void *p, size_t len) { return native((int)write(fd, p, len)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) out->st_mode = s.st_mode;
    return native(rc);
}
int sceIoRename(const char *a, const char *b) { return native(rename(a, b)); }
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
