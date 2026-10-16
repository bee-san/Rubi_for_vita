/* vjo-install-e2e: the Overlay app's install engine (settings/install.c) on a memory card folder, for
 * the one-VPK e2e (a real VPK extracted as VitaShell installs it; docs/one-vpk-install.md). Host only:
 * the SDK's file calls over real files with the Vita's rename rule (an existing destination is
 * refused), app0:'s unmount and the locks answered, the clock past the boot record's first minute.
 *
 *   vjo-install-e2e CARD install|uninstall|finish|keep     prints the scan, the plan and the result */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <psp2/shellutil.h>
#include "../include/vjo_api.h"
#include "../settings/install.h"

static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode) { return native(open(p, flags, mode)); }
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *b, size_t n) { return native((int)read(fd, b, n)); }
int sceIoWrite(int fd, const void *b, size_t n) { return native((int)write(fd, b, n)); }
int sceIoPread(int fd, void *d, size_t n, SceOff off) { return native((int)pread(fd, d, n, off)); }
int sceIoPwrite(int fd, const void *d, size_t n, SceOff off) { return native((int)pwrite(fd, d, n, off)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{
    struct stat st;
    int rc = stat(p, &st);
    if (!rc) { out->st_mode = st.st_mode; out->st_size = st.st_size; }
    return native(rc);
}
int sceIoRename(const char *a, const char *b)
{
    struct stat st;
    if (!stat(b, &st)) return (int)0x80010011u; /* EEXIST, as the Vita's rename */
    return native(rename(a, b));
}
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
int sceIoSyncByFd(int fd, int flag) { (void)fd; (void)flag; return 0; }
int sceIoChstat(const char *p, SceIoStat *st, int bits) { (void)bits; return native(truncate(p, st->st_size)); }
static DIR *dirs[8];
int sceIoDopen(const char *p)
{
    for (int i = 0; i < 8; i++)
        if (!dirs[i]) return (dirs[i] = opendir(p)) ? 100 + i : native(-1);
    return (int)0x80010018u;
}
int sceIoDread(int d, SceIoDirent *e)
{
    DIR *dir = d >= 100 && d < 108 ? dirs[d - 100] : NULL;
    struct dirent *de;
    if (!dir) return (int)0x80010009u;
    while ((de = readdir(dir)) && de->d_name[0] == '.') {}
    if (!de) return 0;
    memset(e, 0, sizeof(*e));
    snprintf(e->d_name, sizeof(e->d_name), "%s", de->d_name);
    return 1;
}
int sceIoDclose(int d)
{
    if (d < 100 || d >= 108 || !dirs[d - 100]) return (int)0x80010009u;
    closedir(dirs[d - 100]);
    dirs[d - 100] = NULL;
    return 0;
}
int sceIoDevctl(const char *dev, unsigned int cmd, void *in, int inlen, void *out, int outlen)
{
    (void)dev; (void)in; (void)inlen;
    if (cmd != 0x3001 || !out || outlen < (int)sizeof(SceIoDevInfo)) return (int)0x80010016u;
    SceIoDevInfo *info = out;
    memset(info, 0, sizeof(*info));
    info->free_size = 1LL << 34;
    return 0;
}
int sceAppMgrUmount(const char *mp) { (void)mp; return 0; }
int sceShellUtilInitEvents(int unk) { (void)unk; return 0; }
int sceShellUtilLock(int type) { (void)type; return 0; }
int sceShellUtilUnlock(int type) { (void)type; return 0; }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    memset(t, 0, sizeof(*t));
    t->year = 2026; t->month = 10; t->day = 16; t->hour = 12; t->minute = 0; t->second = 0;
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 0; return 0; }
static uint64_t clock_us;
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us += 1000; }
uint64_t sceKernelGetSystemTimeWide(void) { return 3600ull * 1000000ull; }
void vjo_log_enable(int on) { (void)on; }
void vjo_log(const char *fmt, ...) { (void)fmt; }

int main(int argc, char **argv)
{
    static VjoInstallScan s;
    static VjoInstallResult r;
    static char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    const VjoInstallBuild build = {VJO_API_VERSION, VJO_VERSION_STRING, VJO_VARIANT,
                                   VJO_KERNEL_LOADS_WORKER ? VJO_TAI_WORKER_KERNEL : VJO_TAI_WORKER_LINES};
    if (argc != 3 || chdir(argv[1]) != 0) {
        fprintf(stderr, "usage: vjo-install-e2e CARD install|uninstall|finish|keep\n");
        return 2;
    }
    int action = !strcmp(argv[2], "uninstall") ? VJO_INSTALL_ACT_UNINSTALL : !strcmp(argv[2], "finish") ? VJO_INSTALL_ACT_FINISH
               : !strcmp(argv[2], "keep") ? VJO_INSTALL_ACT_KEEP : VJO_INSTALL_ACT_INSTALL;
    vjo_install_scan(&s, &build);
    printf("state %d: %s\n%s%s", s.state, s.title, s.reason, s.reason[0] ? "\n" : "");
    int n = vjo_install_plan(&s, action, plan, VJO_INSTALL_LINES);
    for (int i = 0; i < n; i++) printf("plan: %s\n", plan[i]);
    if (!(s.actions & action)) {
        printf("not offered (actions 0x%x)\n", (unsigned)s.actions);
        return 3;
    }
    int rc = vjo_install_run(&s, action, NULL, &r);
    for (int i = 0; i < r.count; i++) printf("result: %s\n", r.lines[i]);
    vjo_install_scan(&s, &build);
    printf("then: state %d: %s\n", s.state, s.title);
    return rc < 0 ? 1 : 0;
}
