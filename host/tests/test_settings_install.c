/* The Rubi app's one-VPK install (settings/install.c, settings/install_page.c, core/payload.c) on
 * a temporary memory card: real files under ux0:, ur0: (folders of those names), the app's own
 * folder ux0:app/VJOSET001 with a payload (synthetic plugin and model bytes; the repository's config
 * template), and the SDK's file calls over them with the Vita's rename rule (a rename onto an
 * existing file is refused with EEXIST, as VitaShell file.c:518-551 handles it). Faults are injected
 * at every open for writing, write, rename and remove; after each, every file on the card is the old
 * or the new one, and a configuration is missing only between its swap's two renames, which the next
 * scan finds as "interrupted". The page runs its action on a thread the stubs run at once.
 *
 * Host runs only: the console's file system, its free-space answers, app0:'s unmount and the Unsafe
 * Homebrew probe are stubbed; docs/one-vpk-install.md lists what a console run must show. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <psp2/shellutil.h>
#include <bearssl_hash.h>
#include "../../include/vjo_api.h"
#include "../../core/app_stamp.h"
#include "../../core/config.h"
#include "../../core/payload.h"
#include "../../core/taicfg.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../settings/install.h"
#include "../../settings/install_page.h"

#ifndef VJO_SOURCE_DIR
#define VJO_SOURCE_DIR "."
#endif
#define K VJO_TAI_KERNEL
#define S VJO_TAI_SHELL
#define W VJO_TAI_WORKER
#define VJO_STR_(x) #x
#define VJO_STR(x) VJO_STR_(x)
#define STAMP "vjoapp1 api=" VJO_STR(VJO_API_VERSION) " build=0123456789abcdef"

static const VjoInstallBuild build = {VJO_API_VERSION, "0.8.0", VJO_VARIANT, VJO_TAI_WORKER_KERNEL};

/* ---------------- the card ---------------- */
static char card[PATH_MAX];
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }

/* faults: the nth matching call fails (0: none); the counters count matching calls */
static struct {
    const char *pattern;   /* a path containing it */
    int open_write, write, rename, remove; /* fail the nth such call */
    int calls;
} fault;
static int unsafe_off, umount_rc, app_readonly, locked, lock_calls;
/* The app's log.txt lines (settings/applog.c, #149): not this suite's to check. */
void vjo_log_enable(int on) { (void)on; }
void vjo_log(const char *fmt, ...) { (void)fmt; }
static long long free_space[2] = {1LL << 34, 1LL << 34}; /* ux0, ur0 */

static int matches(const char *p) { return fault.pattern && p && strstr(p, fault.pattern); }
static int hit(int nth) { return nth && ++fault.calls == nth; }
static int in_app(const char *p) { return p && !strncmp(p, "ux0:app/", 8); }

int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int writing = flags & (O_WRONLY | O_RDWR);
    if (writing && app_readonly && in_app(p)) return (int)0x80010013u;
    if (writing && matches(p) && hit(fault.open_write)) return (int)(0x80010000u | ENOSPC);
    return native(open(p, flags, mode));
}
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *b, size_t n) { return native((int)read(fd, b, n)); }
static char fd_path[1024][64];
int sceIoWrite(int fd, const void *b, size_t n)
{
    char link[64], path[PATH_MAX]; /* the card is in TMPDIR: its files' paths are as long as it is */
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t m = readlink(link, path, sizeof(path) - 1);
    if (m > 0 && m < (ssize_t)sizeof(path) - 1) {
        path[m] = '\0';
        if (fault.pattern && strstr(path, fault.pattern) && hit(fault.write)) return (int)(0x80010000u | ENOSPC);
    }
    (void)fd_path;
    return native((int)write(fd, b, n));
}
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
    if (app_readonly && (in_app(a) || in_app(b))) return (int)0x80010013u;
    if ((matches(a) || matches(b)) && hit(fault.rename)) return (int)(0x80010000u | EIO);
    if (!stat(b, &st)) return (int)0x80010011u; /* the Vita's rename refuses an existing destination */
    return native(rename(a, b));
}
int sceIoRemove(const char *p)
{
    if (app_readonly && in_app(p)) return (int)0x80010013u;
    if (matches(p) && hit(fault.remove)) return (int)(0x80010000u | EIO);
    return native(unlink(p));
}
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
int sceIoSyncByFd(int fd, int flag) { (void)fd; (void)flag; return 0; }
int sceIoChstat(const char *p, SceIoStat *st, int bits) { (void)bits; return native(truncate(p, st->st_size)); }
static DIR *dirs[8];
static char dir_path[8][256];
int sceIoDopen(const char *p)
{
    for (int i = 0; i < 8; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p))) return native(-1);
            snprintf(dir_path[i], sizeof(dir_path[i]), "%s", p);
            return 100 + i;
        }
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
    (void)in; (void)inlen;
    if (cmd != 0x3001) return (int)0x80010016u;
    if (!out) return unsafe_off ? (int)0x80010030u : (int)0x80010016u;
    if (outlen < (int)sizeof(SceIoDevInfo)) return (int)0x80010016u;
    SceIoDevInfo *info = out;
    memset(info, 0, sizeof(*info));
    info->free_size = free_space[!strncmp(dev, "ur0", 3)];
    return 0;
}
int sceAppMgrUmount(const char *mp) { (void)mp; return umount_rc; }
int sceShellUtilInitEvents(int unk) { (void)unk; return 0; }
int sceShellUtilLock(int type) { (void)type; locked = 1; lock_calls++; return 0; }
int sceShellUtilUnlock(int type) { (void)type; locked = 0; return 0; }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    memset(t, 0, sizeof(*t));
    t->year = 2026; t->month = 10; t->day = 16; t->hour = 7; t->minute = 15; t->second = 3;
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 0; return 0; }
static uint64_t clock_us, uptime_us = 3600ull * 1000000ull;
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us += 1000; }
uint64_t sceKernelGetSystemTimeWide(void) { return uptime_us; }
/* the page's thread: run at once */
static int (*thread_entry)(SceSize, void *);
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { (void)n; (void)a; (void)c; (void)o; return 1; }
int sceKernelLockMutex(SceUID u, int c, void *t) { (void)u; (void)c; (void)t; return 0; }
int sceKernelUnlockMutex(SceUID u, int c) { (void)u; (void)c; return 0; }
SceUID sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned int s, unsigned int a, int af, void *o)
{
    (void)n; (void)p; (void)s; (void)a; (void)af; (void)o;
    thread_entry = e;
    return 2;
}
int sceKernelStartThread(SceUID u, SceSize s, void *a) { (void)u; return thread_entry(s, a); }
int sceKernelWaitThreadEnd(SceUID u, int *s, void *t) { (void)u; (void)s; (void)t; return 0; }
int sceKernelDeleteThread(SceUID u) { (void)u; return 0; }
int sceKernelDeleteMutex(SceUID u) { (void)u; return 0; }

/* ---------------- helpers ---------------- */
static void put(const char *path, const void *data, size_t n)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *s = strchr(dir, '/'); s; s = strchr(s + 1, '/')) { *s = '\0'; mkdir(dir, 0777); *s = '/'; }
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    fwrite(data, 1, n, f);
    fclose(f);
}
static void put_text(const char *path, const char *text) { put(path, text, strlen(text)); }
static char *slurp(const char *path, size_t *n)
{
    static char buf[8][1 << 20];
    static int next;
    char *b = buf[next++ % 8];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t m = fread(b, 1, sizeof(buf[0]) - 1, f);
    fclose(f);
    b[m] = '\0';
    if (n) *n = m;
    return b;
}
static int there(const char *path) { struct stat st; return !stat(path, &st); }
/* (test_tmpdir.h's: no link followed, no truncated path walked) */
static void remove_tree(const char *path) { vjo_test_remove_tree(path); }
static int count_files(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while ((e = readdir(d))) n += !strncmp(e->d_name, prefix, strlen(prefix));
    closedir(d);
    return n;
}

/* A payload file's synthetic bytes: the dictionary is larger than a chunk (256 KiB), so it is read in pieces. */
static size_t payload_bytes(int i, int version, unsigned char *out, size_t cap)
{
    size_t n = vjo_payload_kind(i) == VJO_PAYLOAD_MOVE ? 600 * 1024 + 17 : 3000 + (size_t)i * 101;
    TEST_ASSERT(n <= cap);
    for (size_t k = 0; k < n; k++) out[k] = (unsigned char)(i * 31 + k * 7 + version * 13 + (k >> 9));
    return n;
}

static char template_text[VJO_CONFIG_TEXT_MAX + 1];
static size_t template_len;
static char manifest_text[VJO_PAYLOAD_MANIFEST_MAX];

static void sha_hex(const void *data, size_t n, char hex[65])
{
    br_sha256_context h;
    uint8_t sha[32];
    br_sha256_init(&h);
    br_sha256_update(&h, data, n);
    br_sha256_out(&h, sha);
    vjo_sha256_hex(sha, hex);
}

/* The app's folder with its stamp and a payload of version (its plugins' bytes differ by version). */
static void install_app(int version, const char *pkg_version, const char *sets)
{
    static unsigned char bytes[1 << 20];
    char src[256], hex[65];
    size_t used = 0;
    remove_tree(VJO_PAYLOAD_APP_DIR);
    put_text(VJO_PAYLOAD_APP_DIR "/vjo_app.txt", STAMP "\n");
    put_text(VJO_PAYLOAD_APP_DIR "/eboot.bin", "SCE eboot");
    used += (size_t)snprintf(manifest_text + used, sizeof(manifest_text) - used,
                             "vjopayload 1\nversion %s\napp_version 0.8.0\nvariant %s\napi %d\nworker kernel\nbuild 0123456789abcdef\n",
                             pkg_version, VJO_VARIANT, VJO_API_VERSION);
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) {
        size_t n;
        if (vjo_payload_kind(i) == VJO_PAYLOAD_TEMPLATE) {
            n = template_len;
            memcpy(bytes, template_text, n);
        } else {
            n = payload_bytes(i, version, bytes, sizeof(bytes));
        }
        TEST_ASSERT(vjo_payload_source(i, src, sizeof(src)) == 0);
        put(src, bytes, n);
        sha_hex(bytes, n, hex);
        used += (size_t)snprintf(manifest_text + used, sizeof(manifest_text) - used,
                                 vjo_payload_kind(i) == VJO_PAYLOAD_TEMPLATE ? "template %s %zu %s\n" : "file %s %zu %s %s\n",
                                 vjo_payload_dest(i), n, hex, vjo_payload_kind(i) == VJO_PAYLOAD_MOVE ? "move" : "copy");
    }
    if (sets) used += (size_t)snprintf(manifest_text + used, sizeof(manifest_text) - used, "%s", sets);
    put_text(VJO_PAYLOAD_MANIFEST, manifest_text);
}

static const char *stock =
    "# For users plugins, you must refresh taiHEN from HENkaku Settings for\n# changes to take place.\n"
    "# For kernel plugins, you must reboot for changes to take place.\n*KERNEL\n"
    "# henkaku.skprx is hard-coded to load and is not listed here\n*main\n# main is a special titleid for SceShell\n"
    "ur0:tai/henkaku.suprx\n*NPXS10015\n# this is for modifying the version string\nur0:tai/henkaku.suprx\n"
    "*NPXS10016\n# this is for modifying the version string in settings widget\nur0:tai/henkaku.suprx\n";

static void remove_last_card(void)
{
    if (card[0] && chdir("/") == 0) remove_tree(card);
}

static void fresh_card(int with_ux0)
{
    static int registered;
    if (!registered) registered = atexit(remove_last_card) == 0;
    if (card[0]) { if (chdir("/") == 0) remove_tree(card); }
    TEST_ASSERT(vjo_test_tmpdir_new(card, sizeof(card)) != NULL);
    TEST_ASSERT(chdir(card) == 0);
    memset(&fault, 0, sizeof(fault));
    unsafe_off = umount_rc = app_readonly = locked = lock_calls = 0;
    free_space[0] = free_space[1] = 1LL << 34;
    if (!template_len) {
        size_t n;
        char *t = slurp(VJO_SOURCE_DIR "/docs/config.example.ini", &n);
        TEST_ASSERT(t != NULL && n < sizeof(template_text));
        memcpy(template_text, t, n + 1);
        template_len = n;
    }
    char r0[2048];
    snprintf(r0, sizeof(r0), "# This file is used as an alternative if ux0:tai/config.txt is not found.\n%s", stock);
    put_text("ur0:tai/config.txt", r0);
    if (with_ux0) put_text("ux0:tai/config.txt", stock);
    mkdir("ux0:data", 0777);
    install_app(1, "0.8.0", NULL);
}

static void dump_lines(const char lines[][VJO_INSTALL_LINE], int n)
{
    for (int i = 0; i < n; i++) TEST_MSG("%s", lines[i]);
}
static int has_line(const char lines[][VJO_INSTALL_LINE], int n, const char *needle)
{
    for (int i = 0; i < n; i++)
        if (strstr(lines[i], needle)) return 1;
    return 0;
}

static VjoInstallScan scan_now(void)
{
    static VjoInstallScan s;
    vjo_install_scan(&s, &build);
    return s;
}

static int run(VjoInstallScan *s, int action, VjoInstallResult *r)
{
    int rc = vjo_install_run(s, action, NULL, r);
    dump_lines((const char (*)[VJO_INSTALL_LINE])r->lines, r->count);
    return rc;
}

/* every payload file at its destination with the payload's bytes (version), and nothing half there */
static int installed_files(int version)
{
    static unsigned char bytes[1 << 20];
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) {
        if (vjo_payload_kind(i) == VJO_PAYLOAD_TEMPLATE) continue;
        size_t n = payload_bytes(i, version, bytes, sizeof(bytes)), m;
        char *have = slurp(vjo_payload_dest(i), &m);
        char fresh[300], old[300];
        snprintf(fresh, sizeof(fresh), "%s.vjo-new", vjo_payload_dest(i));
        snprintf(old, sizeof(old), "%s.vjo-old", vjo_payload_dest(i));
        if (!have || m != n || memcmp(have, bytes, n) || there(fresh) || there(old)) {
            TEST_MSG("file %s: %s", vjo_payload_dest(i), !have ? "missing" : m != n || memcmp(have, bytes, n) ? "differs" : "a temp file beside it");
            return 0;
        }
    }
    return 1;
}

/* Each payload file is either missing (never installed), version a's or version b's, and no
 * configuration is half written. */
static int every_file_old_or_new(int a, int b)
{
    static unsigned char x[1 << 20], y[1 << 20];
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) {
        if (vjo_payload_kind(i) == VJO_PAYLOAD_TEMPLATE) continue;
        size_t n = payload_bytes(i, a, x, sizeof(x)), n2 = payload_bytes(i, b, y, sizeof(y)), m;
        char *have = slurp(vjo_payload_dest(i), &m);
        if (have && !(m == n && !memcmp(have, x, n)) && !(m == n2 && !memcmp(have, y, n2))) {
            TEST_MSG("%s is neither", vjo_payload_dest(i));
            return 0;
        }
    }
    return 1;
}

/* ---------------- the tests ---------------- */

static void fresh_install_on_both_configurations(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_NOT_INSTALLED, "state %d: %s %s", s.state, s.title, s.reason);
    TEST_CHECK(s.actions == VJO_INSTALL_ACT_INSTALL);
    TEST_CHECK(!strcmp(s.title, VJO_DEBUG ? "Install the overlay (v0.8.0, debug build)" : "Install the overlay (v0.8.0)"));
    TEST_MSG("%s", s.title);
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_CHECK(has_line(plan, n, "Move the dictionary (1 MB) to ux0:data/VitaJPOverlay/dictionaries from this app: moved, not copied"));
    TEST_CHECK(has_line(plan, n, "Back up ux0:tai/config.txt and ur0:tai/config.txt as config.txt.vjo-20261016-071503.bak in each folder (kept)."));
    TEST_CHECK(has_line(plan, n, "ux0:tai/config.txt (taiHEN reads this one at boot): add the kernel line under *KERNEL and the Shell line under *main. Exactly:"));
    /* rule 7: the exact lines, in the file's order */
    int at = -1;
    for (int i = 0; i < n; i++)
        if (strstr(plan[i], "ux0:tai/config.txt (taiHEN reads this one at boot)")) at = i;
    TEST_ASSERT(at >= 0 && at + 2 < n);
    TEST_CHECK_(!strcmp(plan[at + 1], "   + " K) && !strcmp(plan[at + 2], "   + " S), "[%s] [%s]", plan[at + 1], plan[at + 2]);
    TEST_CHECK(has_line(plan, n, "ur0:tai/config.txt: add the kernel line"));
    TEST_CHECK(has_line(plan, n, "config.ini: written from the package's template."));
    TEST_CHECK(has_line(plan, n, "Then restart the Vita yourself: this app never restarts it."));
    TEST_CHECK(has_line(plan, n, VJO_INSTALL_RECOVERY_BOOT) && has_line(plan, n, VJO_INSTALL_RECOVERY_GAME));
    TEST_CHECK(has_line(plan, n, "(ur0:tai/config.txt.vjo") == 0); /* no backup yet */

    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && r.rc == 0 && r.written);
    TEST_CHECK(has_line(r.lines, r.count, "Installed the overlay v0.8.0") && has_line(r.lines, r.count, "Restart the Vita now"));
    TEST_CHECK(has_line(r.lines, r.count, "The dictionary (1 MB) was moved out of this app, not copied."));
    TEST_CHECK(has_line(r.lines, r.count, "Backup: ux0:tai/config.txt.vjo-20261016-071503.bak (kept)."));
    TEST_CHECK(installed_files(1));
    char src[256];
    vjo_payload_source(s.dict_index, src, sizeof(src));
    TEST_CHECK(!there(src)); /* moved */
    for (int c = 0; c < 2; c++) {
        const char *path = c ? "ur0:tai/config.txt" : "ux0:tai/config.txt";
        char *t = slurp(path, NULL), bak[128];
        TEST_ASSERT(t != NULL);
        TEST_CHECK(strstr(t, "*KERNEL\n" K "\n") && strstr(t, "*main\n" S "\n") && !strstr(t, "VitaJPOverlay_OCR"));
        snprintf(bak, sizeof(bak), "%s.vjo-20261016-071503.bak", path);
        TEST_CHECK(slurp(bak, NULL) && !vjo_tai_has_kernel_line(slurp(bak, NULL), strlen(slurp(bak, NULL))));
        char fresh[128];
        snprintf(fresh, sizeof(fresh), "%s.vjo-new", path);
        TEST_CHECK(!there(fresh));
    }
    TEST_CHECK(!strcmp(slurp(VJO_PREFERENCES_PATH_TEST, NULL), template_text));
    TEST_CHECK(!strcmp(slurp(VJO_APP_STAMP_PACKAGE, NULL), STAMP "\n"));
    VjoInstalled rec;
    char *it = slurp(VJO_INSTALLED_RECORD, NULL);
    TEST_CHECK(it && vjo_installed_parse(it, strlen(it), &rec) == 0 && !strcmp(rec.version, "0.8.0") && rec.api == VJO_API_VERSION);
    size_t bn;
    TEST_CHECK(slurp("ur0:data/VitaJPOverlay/boot.txt", &bn) && bn == 0);
    TEST_CHECK(!locked && lock_calls == 1); /* held for the run, then let go */
    /* in a boot's first minute the kernel writes its record over a reset: it is left, and said */
    put_text("ur0:data/VitaJPOverlay/boot.txt", "safe");
    uptime_us = 20ull * 1000000ull;
    VjoInstallScan early = scan_now();
    VjoInstallResult er;
    TEST_CHECK(run(&early, VJO_INSTALL_ACT_INSTALL, &er) == 0 && has_line(er.lines, er.count, "the Vita has run for under a minute"));
    TEST_CHECK(!strcmp(slurp("ur0:data/VitaJPOverlay/boot.txt", NULL), "safe"));
    uptime_us = 3600ull * 1000000ull;
    /* the card is installed now, and a check writes nothing */
    VjoInstallScan again = scan_now();
    TEST_CHECK_(again.state == VJO_INSTALL_INSTALLED, "state %d: %s", again.state, again.title);
    TEST_CHECK(again.actions == (VJO_INSTALL_ACT_INSTALL | VJO_INSTALL_ACT_UNINSTALL));
    int backups = count_files("ux0:tai", "config.txt.vjo-");
    TEST_CHECK(run(&again, VJO_INSTALL_ACT_INSTALL, &r) == 0 && !r.written);
    TEST_CHECK(has_line(r.lines, r.count, "nothing needed writing"));
    TEST_CHECK(count_files("ux0:tai", "config.txt.vjo-") == backups);
}

/* An older install's worker lines go, its plugins are replaced (#138: the kernel loads the worker). */
static void update_over_a_test3_install(void)
{
    static unsigned char bytes[1 << 20];
    fresh_card(0);
    char cfg[2048];
    snprintf(cfg, sizeof(cfg), "*KERNEL\n%s\n*main\n%s\nur0:tai/henkaku.suprx\n*ALL\nur0:tai/Every.suprx\n%s\n\n*PCSG00415\n%s\n", K, S, W, W);
    put_text("ur0:tai/config.txt", cfg);
    /* version 0's plugins and data, a test.3-style record */
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) {
        if (vjo_payload_kind(i) == VJO_PAYLOAD_TEMPLATE) continue;
        size_t n = payload_bytes(i, 0, bytes, sizeof(bytes));
        put(vjo_payload_dest(i), bytes, n);
    }
    put_text(VJO_APP_STAMP_PACKAGE, "vjoapp1 api=" VJO_STR(VJO_API_VERSION) " build=fedcba9876543210\n");
    put_text(VJO_PREFERENCES_PATH_TEST, "vocr_tier = large\nocr_mode = on_press\nlog_file = off\n");
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_UPDATE, "state %d %s", s.state, s.title);
    TEST_CHECK(!strcmp(s.title, "Update the overlay to v0.8.0 (the installed one has no version record)"));
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_CHECK(has_line(plan, n, "ur0:tai/config.txt (taiHEN reads this one at boot): remove the OCR worker's lines from *ALL, *PCSG00415. Exactly:"));
    {
        /* the worker under *ALL, the blank line and *PCSG00415 with its worker: removed, in that order */
        static const char *const want[] = {"   - " W, "   - ", "   - *PCSG00415", "   - " W};
        int at = -1;
        for (int i = 0; i < n; i++)
            if (strstr(plan[i], "ur0:tai/config.txt (taiHEN reads this one at boot): remove")) at = i;
        TEST_ASSERT(at >= 0 && at + 4 < n);
        for (int i = 0; i < 4; i++) TEST_CHECK_(!strcmp(plan[at + 1 + i], want[i]), "%d: [%s]", i, plan[at + 1 + i]);
        TEST_CHECK(strncmp(plan[at + 5], "   ", 3) != 0);
    }
    TEST_CHECK(has_line(plan, n, "config.ini: these lines change; every other setting stays, and the old file is kept as config.ini.bak:"));
    TEST_CHECK(has_line(plan, n, "   local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries"));
    /* R1 on the page: a dated backup does not undo an update */
    TEST_CHECK(has_line(plan, n, "A dated backup is the configuration from before this install: it does not undo the update"));
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(installed_files(1));
    char *t = slurp("ur0:tai/config.txt", NULL);
    TEST_CHECK(t && !strcmp(t, "*KERNEL\n" K "\n*main\n" S "\nur0:tai/henkaku.suprx\n*ALL\nur0:tai/Every.suprx\n"));
    TEST_MSG("%s", t);
    /* the user's settings stay (the OCR page's and log_file), the install's paths are set */
    char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL);
    TEST_ASSERT(ini != NULL);
    TEST_CHECK(strstr(ini, "vocr_tier = large\n") && strstr(ini, "ocr_mode = on_press\n"));
    TEST_CHECK(strstr(ini, VJO_DEBUG ? "log_file = on\n" : "log_file = off\n") != NULL);
    TEST_CHECK(strstr(ini, "ocr_model_dir = ux0:data/VitaJPOverlay/ocr\n") != NULL);
    VjoConfig parsed;
    vjo_config_defaults(&parsed);
    vjo_config_parse(&parsed, ini, strlen(ini));
    TEST_CHECK(!strcmp(parsed.local_dictionaries, "plaintext.vjdict") && !parsed.n_warnings); /* the default: no line needed */
    TEST_MSG("%s", ini);
    TEST_CHECK(!strcmp(slurp(VJO_APP_STAMP_PACKAGE, NULL), STAMP "\n"));
    TEST_CHECK(scan_now().state == VJO_INSTALL_INSTALLED);
}

/* config.ini of 0.7.12: its model, its dictionary, removed settings and an API key. */
static void config_ini_from_0712(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "dictionary = jpdb\njpdb_api_key = secret-key\nlocal_dictionaries = jmdict.vjdict, mine.vjdict\n"
             "ocr_backend = vocr\nvocr_model = FL10_w8.vocr\nfont_size_ja = 24\nhachidori_host = 192.168.1.5\n"
             "  FREQUENCY_FILTER = 5\nhw_jpeg=1\nfont_size = 3\ncombo_delay_ms = 10\n");
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL), *bak = slurp(VJO_PREFERENCES_PATH_TEST ".bak", NULL);
    TEST_ASSERT(ini != NULL);
    TEST_MSG("%s", ini);
    TEST_CHECK(strstr(ini, "dictionary = local\n") && !strstr(ini, "secret") && !strstr(ini, "api_key"));
    TEST_CHECK(strstr(ini, "local_dictionaries = plaintext.vjdict, mine.vjdict\n") != NULL);
    TEST_CHECK(strstr(ini, "vocr_tier = small\n") && !strstr(ini, "vocr_model") && !strstr(ini, "ocr_backend"));
    TEST_CHECK(strstr(ini, "font_size_ja = 24\n") != NULL);
    /* the removed settings go, as tools/migrate_config.py drops them; a key that only starts alike stays */
    TEST_CHECK(!strstr(ini, "hachidori_host") && !strstr(ini, "FREQUENCY_FILTER") && !strstr(ini, "hw_jpeg") &&
               !strstr(ini, "font_size =") && !strstr(ini, "combo_delay_ms"));
    TEST_CHECK(!bak || !strstr(bak, "secret")); /* no file keeps the key */
    /* a list of 8 names is left as it is, with a note */
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "local_dictionaries = a.vjdict,b.vjdict,c.vjdict,d.vjdict,e.vjdict,f.vjdict,g.vjdict,h.vjdict\n");
    s = scan_now();
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    TEST_CHECK(has_line(plan, n, "local_dictionaries has 8 names: add plaintext.vjdict to it yourself"));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(strstr(slurp(VJO_PREFERENCES_PATH_TEST, NULL), "local_dictionaries = a.vjdict,b.vjdict,c.vjdict,d.vjdict,e.vjdict,f.vjdict,g.vjdict,h.vjdict\n") != NULL);
    /* only config.ini.bak (a save stopped between its renames): patched from it, never the template */
    fresh_card(1);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    put_text(VJO_PREFERENCES_PATH_TEST ".bak", "vocr_tier = large\n");
    s = scan_now();
    TEST_CHECK(s.config_ini == 1);
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(strstr(slurp(VJO_PREFERENCES_PATH_TEST, NULL), "vocr_tier = large\n") != NULL);
}

/* The plan's config.ini part, at the index of its "N. config.ini" line: -1 when it has none. */
static int ini_part(const char plan[][VJO_INSTALL_LINE], int n)
{
    for (int i = 0; i < n; i++)
        if (strstr(plan[i], ". config.ini")) return i;
    return -1;
}

/* test.4's dry run: over the owner's test.3 install (its config.ini as test.3's collect had it, #138), the
 * Update drops ocr_backend and log_host, two removed settings, and the plan listed no line under "these lines
 * change". Each line that only goes is named by its setting now, never its value; the user's Large stays. */
static void the_plan_names_each_config_ini_line_that_goes(void)
{
    static char owner[VJO_CONFIG_TEXT_MAX + 1];
    static unsigned char bytes[1 << 20];
    size_t on = 0;
    char *fixture = slurp(VJO_SOURCE_DIR "/host/tests/fixtures/config-test3-owner.ini", &on);
    TEST_ASSERT(fixture != NULL && on < sizeof(owner));
    memcpy(owner, fixture, on + 1);
    fresh_card(0);
    char cfg[2048];
    snprintf(cfg, sizeof(cfg), "*KERNEL\n%s\n*main\n%s\n*PCSG00415\n%s\n", K, S, W);
    put_text("ur0:tai/config.txt", cfg);
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) { /* test.3's plugins and data, its record: an Update */
        if (vjo_payload_kind(i) == VJO_PAYLOAD_TEMPLATE) continue;
        size_t n = payload_bytes(i, 0, bytes, sizeof(bytes));
        put(vjo_payload_dest(i), bytes, n);
    }
    put_text(VJO_APP_STAMP_PACKAGE, "vjoapp1 api=" VJO_STR(VJO_API_VERSION) " build=fedcba9876543210\n");
    put_text(VJO_PREFERENCES_PATH_TEST, owner);
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_UPDATE && s.config_ini == 1, "state %d config_ini %d", s.state, s.config_ini);
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    int at = ini_part((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_ASSERT(at >= 0 && at + 3 < n);
    TEST_CHECK(strstr(plan[at], "config.ini: these lines change; every other setting stays") != NULL);
    /* exactly the two lines that go, in the file's order, and nothing after them */
    TEST_CHECK_(!strcmp(plan[at + 1], "   ocr_backend: a removed setting, goes"), "[%s]", plan[at + 1]);
    TEST_CHECK_(!strcmp(plan[at + 2], "   log_host: a removed setting, goes"), "[%s]", plan[at + 2]);
    TEST_CHECK_(strncmp(plan[at + 3], "   ", 3) != 0, "[%s]", plan[at + 3]);
    TEST_CHECK(!has_line(plan, n, "vocr_tier")); /* the user's Large is not a change */
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL);
    TEST_ASSERT(ini != NULL);
    TEST_CHECK(strstr(ini, "vocr_tier = large\n") && !strstr(ini, "\nocr_backend") && !strstr(ini, "\nlog_host"));
    TEST_CHECK(strstr(ini, "anki_mining = on\n") && strstr(ini, "log_file = on\n"));
    /* Check and repair over it: config.ini is the user's, as it is */
    VjoInstallScan again = scan_now();
    TEST_CHECK_(again.state == VJO_INSTALL_INSTALLED && again.config_ini == 0, "state %d config_ini %d", again.state,
                again.config_ini);
    n = vjo_install_plan(&again, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    TEST_CHECK(has_line(plan, n, "config.ini: unchanged (your settings stay as they are)."));
}

/* Whether the scan's config.ini list has exactly this line. */
static int ini_listed(const VjoInstallScan *s, const char *line)
{
    for (int i = 0; i < s->config_change_count; i++)
        if (!strcmp(s->config_changes[i], line)) return 1;
    return 0;
}

/* A line that only goes is named by its setting: an API key's by the key's name alone, whatever follows it
 * (removed_secret drops a line that starts with the name), never by a value; a changed setting is listed
 * once, by its new line; a second copy of a setting is named as one. The scan's whole list, each on a card
 * small enough that none is only counted; and no value on the page either (log.txt gets none of these lines: the
 * install logs only config.ini's size, settings/install.c). */
static void a_line_that_goes_is_named_by_its_setting_never_its_value(void)
{
    /* three of the app's own lines, which its save writes when they are missing (the cards stay under 12 changes) */
#define OWNED "japanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n"
    static const struct {
        const char *ini, *const goes[6], *const changed[2];
    } cards[] = {
        {OWNED "dictionary = jpdb\njpdb_api_key = secret-key\njiten_api_keySECRET2\nocr_backend = vocr2\n"
         "vocr_model = FL10_w8.vocr\nanki_audio_url = https://example.invalid/a?key=SECRET3\n",
         {"jpdb_api_key: a removed setting, goes", "jiten_api_key: a removed setting, goes",
          "ocr_backend: a removed setting, goes", "vocr_model: a removed setting, goes",
          "anki_audio_url: a removed setting, goes", NULL},
         {"dictionary = local", "vocr_tier = small"}},
        {OWNED "hachidori_host = 192.168.1.5\n  FREQUENCY_FILTER = 5\nhw_jpeg=1\nvocr_tier = large\nvocr_tier = small\n",
         {"hachidori_host: a removed setting, goes", "FREQUENCY_FILTER: a removed setting, goes",
          "hw_jpeg: a removed setting, goes", "vocr_tier: a repeated line goes", NULL},
         {"text_source = ocr", NULL}},
    };
    for (unsigned c = 0; c < sizeof(cards) / sizeof(cards[0]); c++) {
        fresh_card(1);
        put_text(VJO_PREFERENCES_PATH_TEST, cards[c].ini);
        VjoInstallScan s = scan_now();
        TEST_CHECK_(s.config_ini == 1 && s.config_change_more == 0, "card %u: config_ini %d, %d more", c, s.config_ini,
                    s.config_change_more);
        for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
        for (int i = 0; cards[c].goes[i]; i++) TEST_CHECK_(ini_listed(&s, cards[c].goes[i]), "card %u: %s", c, cards[c].goes[i]);
        for (int i = 0; i < 2 && cards[c].changed[i]; i++)
            TEST_CHECK_(ini_listed(&s, cards[c].changed[i]), "card %u: %s", c, cards[c].changed[i]);
        TEST_CHECK(!ini_listed(&s, "dictionary: a removed setting, goes")); /* changed, so listed by its new line */
        TEST_CHECK(!ini_listed(&s, "vocr_tier: a removed setting, goes"));
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
        for (int i = 0; i < n + s.config_change_count; i++) {
            const char *l = i < n ? plan[i] : s.config_changes[i - n];
            TEST_CHECK_(!strstr(l, "SECRET") && !strstr(l, "secret-key") && !strstr(l, "192.168") &&
                        !strstr(l, "example.invalid") && !strstr(l, "FL10_w8") && !strstr(l, "vocr2"), "[%s]", l);
        }
    }
#undef OWNED
}

/* audit-pr177 N1: the patches keep a UTF-8 BOM at offset 0, whatever line follows it. When the first line goes, the
 * BOM ends up in front of the next one: it is no part of either line, so the line that goes is named and the next
 * one, which stays, is not (before: "<BOM>japanese_only_selection = on" listed, and named as gone). */
static void a_bom_is_no_part_of_a_line(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "\xEF\xBB\xBF" "ocr_backend = vocr\njapanese_only_selection = on\nhighlight_mode = word\n"
             "local_dictionary_results = 1\n");
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.config_ini == 1 && s.config_change_more == 0, "config_ini %d, %d more", s.config_ini, s.config_change_more);
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK(ini_listed(&s, "ocr_backend: a removed setting, goes"));
    TEST_CHECK(!ini_listed(&s, "japanese_only_selection: a removed setting, goes"));
    for (int i = 0; i < s.config_change_count; i++)
        TEST_CHECK_(!strstr(s.config_changes[i], "\xEF\xBB\xBF") && !strstr(s.config_changes[i], "japanese_only_selection"),
                    "[%s]", s.config_changes[i]);
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL);
    TEST_ASSERT(ini != NULL);
    TEST_CHECK(!strncmp(ini, "\xEF\xBB\xBF" "japanese_only_selection = on\n", 32) && !strstr(ini, "ocr_backend"));
}

/* audit-pr180 B1: a BOM the new text starts with but the old one does not was a later line's (every line before it
 * went). The comparisons skip it on both sides (ini_skip_bom), so the unchanged line is not listed, and never its
 * value. */
static void a_later_lines_bom_stays_part_of_it(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "ocr_backend = vocr\n" "\xEF\xBB\xBF" "hachidori_host = 192.168.1.5\n"
             "japanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n");
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.config_ini == 1, "config_ini %d", s.config_ini);
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK(ini_listed(&s, "ocr_backend: a removed setting, goes"));
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    for (int i = 0; i < n + s.config_change_count; i++) {
        const char *l = i < n ? plan[i] : s.config_changes[i - n];
        TEST_CHECK_(!strstr(l, "192.168") && !strstr(l, "hachidori_host ="), "[%s]", l);
    }
}

/* The engine's own eight lines as it writes them: a card with them all adds no line of its own. */
#define COMPLETE "japanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\nvocr_tier = normal\n" \
    "local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\nocr_model_dir = ux0:data/VitaJPOverlay/ocr\n"      \
    "text_source = ocr\nlog_file = on\n"

/* audit-pr180 N1, A4: the bits of one scan never reach the next. The first scan marks its new line 0 as a line the
 * old text has; the second's new line 0 is a changed one, which a stale bit would call kept (a false "repeated"). */
static void each_scan_marks_its_own_lines(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "anki_mining = on\nocr_backend = vocr\n" COMPLETE);
    VjoInstallScan first = scan_now();
    TEST_CHECK_(first.config_ini == 1 && ini_listed(&first, "ocr_backend: a removed setting, goes"), "first: %d",
                first.config_ini);
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "dictionary = jpdb\n" COMPLETE);
    VjoInstallScan s = scan_now();
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK_(s.config_ini == 1 && s.config_change_count == 1 && ini_listed(&s, "dictionary = local"),
                "config_ini %d, %d lines", s.config_ini, s.config_change_count);
    TEST_CHECK(!ini_listed(&s, "dictionary: a repeated line goes"));
}

/* audit-pr180 N1, A5: a line that goes is "repeated" only when no same-named new line changed, whichever comes
 * last (an OR over them, as #177's): dictionary = jpdb becomes dictionary = local, a changed line, though
 * dictionary = mine stays. */
static void any_changed_same_named_line_makes_it_a_change(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "dictionary = jpdb\ndictionary = mine\n" COMPLETE);
    VjoInstallScan s = scan_now();
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK_(s.config_ini == 1 && s.config_change_count == 1 && ini_listed(&s, "dictionary = local"),
                "config_ini %d, %d lines", s.config_ini, s.config_change_count);
    TEST_CHECK(!ini_listed(&s, "dictionary: a repeated line goes"));
}
/* audit-pr180-b N1: a BOM in the middle of a hand-joined file, on a line that goes after every line before it went,
 * moves onto the next line (the patches take a BOM at offset 0 as the file's own). That line is unchanged, so it is
 * not listed, and never with its value; the line that went is named by its own setting. Cards K1, K3 and K4 of the
 * audit: the next line an unknown setting with a value, a comment holding a key, and a key behind a BOM. */
static void a_mid_file_bom_never_shows_a_value(void)
{
    static const struct { const char *third, *never; int lines; } cards[] = {
        {"unknown_setting = VALUEKEEP\n", "VALUEKEEP", 2},                    /* K1 */
        {"; my jpdb_api_key = SECRETk3\n", "SECRETk3", 2},                   /* K3 */
        /* K4: the save drops a BOM-led API key too now (vjo_config_line_start), and the plan names it */
        {"\xEF\xBB\xBF" "jpdb_api_key = SECRETk4\n", "SECRETk4", 3},
        /* audit-pr185 N1 (B2e): a space, then a BOM, then a removed setting: named as it goes, in either order */
        {" \xEF\xBB\xBF" "hachidori_host = SECRETk5\n", "SECRETk5", 3},
    };
    char text[1024];
    for (unsigned c = 0; c < sizeof(cards) / sizeof(cards[0]); c++) {
        snprintf(text, sizeof(text), "ocr_backend = vocr\n" "\xEF\xBB\xBF" "log_host = 10.9.8.1\n%s" COMPLETE, cards[c].third);
        fresh_card(1);
        put_text(VJO_PREFERENCES_PATH_TEST, text);
        VjoInstallScan s = scan_now();
        for (int i = 0; i < s.config_change_count; i++) TEST_MSG("K%u: %s", c, s.config_changes[i]);
        TEST_CHECK_(s.config_ini == 1, "card %u: config_ini %d", c, s.config_ini);
        TEST_CHECK_(ini_listed(&s, "ocr_backend: a removed setting, goes"), "card %u", c);
        TEST_CHECK_(ini_listed(&s, "log_host: a removed setting, goes"), "card %u", c);
        TEST_CHECK_(s.config_change_count == cards[c].lines && s.config_change_more == 0, "card %u: %d lines", c,
                    s.config_change_count);
        if (cards[c].lines == 3)
            TEST_CHECK_(ini_listed(&s, c == 2 ? "jpdb_api_key: a removed setting, goes" :
                                               "hachidori_host: a removed setting, goes"), "card %u", c);
        TEST_CHECK_(!ini_listed(&s, "unknown_setting: a removed setting, goes"), "card %u", c);
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
        for (int i = 0; i < n + s.config_change_count; i++) {
            const char *l = i < n ? plan[i] : s.config_changes[i - n];
            TEST_CHECK_(!strstr(l, cards[c].never) && !strstr(l, "10.9.8.1") && !strstr(l, "\xEF\xBB\xBF"), "card %u: [%s]", c, l);
        }
    }
}
/* audit-pr183 N1: a BOM-led copy of a line the install writes is the same setting to the parser and to the engine
 * now (core/config.c vjo_config_line_start), so "a repeated line goes" is true: what the plugins read does not
 * change. Cards X1 and X6 of that audit; X6's log_file is the debug build's own install key. */
static void a_bom_led_copy_is_the_same_setting_to_the_plugins(void)
{
    static const struct { const char *ini, *goes; int debug_only; } cards[] = {
        {"local_dictionary_dir = ux0:Other\nanki_mining = on\n"
         "\xEF\xBB\xBF" "local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n", "local_dictionary_dir: a repeated line goes", 0},
        {"log_file = off\nanki_mining = on\n" "\xEF\xBB\xBF" "log_file = on\n", "log_file: a repeated line goes", 1},
    };
    for (unsigned c = 0; c < sizeof(cards) / sizeof(cards[0]); c++) {
        if (cards[c].debug_only && !VJO_DEBUG) continue; /* the normal build sets no log_file */
        char text[512];
        snprintf(text, sizeof(text), "%sjapanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n"
                 "vocr_tier = normal\n", cards[c].ini);
        VjoConfig before, after;
        vjo_config_defaults(&before);
        vjo_config_parse(&before, text, strlen(text));
        fresh_card(1);
        put_text(VJO_PREFERENCES_PATH_TEST, text);
        VjoInstallScan s = scan_now();
        for (int i = 0; i < s.config_change_count; i++) TEST_MSG("X%u: %s", c, s.config_changes[i]);
        TEST_CHECK_(ini_listed(&s, cards[c].goes), "card %u: %s", c, cards[c].goes);
        VjoInstallResult r;
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
        char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL);
        TEST_ASSERT(ini != NULL);
        vjo_config_defaults(&after);
        vjo_config_parse(&after, ini, strlen(ini));
        /* what the plugins read of that setting is the same before and after */
        TEST_CHECK_(!strcmp(before.local_dictionary_dir, after.local_dictionary_dir) && before.log_file == after.log_file,
                    "card %u: %s -> %s, log_file %d -> %d", c, before.local_dictionary_dir, after.local_dictionary_dir,
                    before.log_file, after.log_file);
        TEST_CHECK_(before.n_warnings == 0 && after.n_warnings == 0, "card %u: warnings %d, %d", c, before.n_warnings,
                    after.n_warnings);
    }
}

/* audit-pr185-b N1 (R7) and N2: the Install page's removed-settings filter reads a line's name as the parser does
 * (a CR before it is skipped too), and a removed setting on the file's first line goes without the file's own BOM,
 * which the patches keep at offset 0. */
static void removed_settings_go_however_their_line_starts(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "\xEF\xBB\xBF" "frequency_filter = 3\nanki_mining = on\n\rhw_jpeg = 1\n"
             " \xEF\xBB\xBF" "combo_delay_ms = 10\n" "japanese_only_selection = on\nhighlight_mode = word\n"
             "local_dictionary_results = 1\n");
    VjoInstallScan s = scan_now();
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK(ini_listed(&s, "frequency_filter: a removed setting, goes") && ini_listed(&s, "hw_jpeg: a removed setting, goes") &&
               ini_listed(&s, "combo_delay_ms: a removed setting, goes"));
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL);
    TEST_ASSERT(ini != NULL);
    TEST_MSG("%s", ini);
    TEST_CHECK(!strstr(ini, "frequency_filter") && !strstr(ini, "hw_jpeg") && !strstr(ini, "combo_delay_ms"));
    TEST_CHECK(!strncmp(ini, "\xEF\xBB\xBF" "anki_mining = on\n", 20)); /* the file's own BOM stays, at offset 0 */
    TEST_CHECK(strstr(ini + 3, "\xEF\xBB\xBF") == NULL);
}

/* audit-pr183 N2: a BOM file whose first line changes lists that line without its BOM (the file's own BOM, which
 * the patches keep at offset 0, is no part of a line: the ob/nb strip). */
static void a_bom_files_changed_first_line_is_listed_without_its_bom(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "\xEF\xBB\xBF" "vocr_tier=large\n"
             "japanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n");
    VjoInstallScan s = scan_now();
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK(ini_listed(&s, "vocr_tier = large"));
    for (int i = 0; i < s.config_change_count; i++)
        TEST_CHECK_(!strstr(s.config_changes[i], "\xEF\xBB\xBF"), "[%s]", s.config_changes[i]);
}
#undef COMPLETE

/* audit-pr177 N3 and N6: an API key's name in capitals glued to its value is named by the key alone; a setting in
 * capitals that the app's save writes in lower case is listed once, by its new line, never also as gone; a CRLF
 * line that stays makes its same-named copy "repeated"; a key line after a CR is named too (removed_secret skips
 * CR, core/config.c is_space). */
static void a_line_that_goes_is_named_whatever_its_case_or_line_ends(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "JITEN_API_KEYSECRET4\nVOCR_TIER = large\ndictionary = local\r\ndictionary = jpdb\r\n"
             "\rjpdb_api_key = SECRET5\njapanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n");
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.config_ini == 1 && s.config_change_more == 0, "config_ini %d, %d more", s.config_ini, s.config_change_more);
    for (int i = 0; i < s.config_change_count; i++) TEST_MSG("%s", s.config_changes[i]);
    TEST_CHECK(ini_listed(&s, "jiten_api_key: a removed setting, goes"));
    TEST_CHECK(ini_listed(&s, "jpdb_api_key: a removed setting, goes"));
    TEST_CHECK(ini_listed(&s, "vocr_tier = large") && !ini_listed(&s, "VOCR_TIER: a removed setting, goes"));
    TEST_CHECK(ini_listed(&s, "dictionary: a repeated line goes"));
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    for (int i = 0; i < n + s.config_change_count; i++) {
        const char *l = i < n ? plan[i] : s.config_changes[i - n];
        TEST_CHECK_(!strstr(l, "SECRET") && !strstr(l, "VOCR_TIER") && !strchr(l, '\r'), "[%s]", l);
    }
}

/* audit-pr177 N2: many lines that go, sharing a name with many lines that stay, made the second pass cubic (a
 * scan of the old text for each same-named new line): audit-pr177 measured 14.8 s on its host for a crafted
 * 23,291 B file, which would be minutes on the console (est.). Each case below is a hand-made file of the kind;
 * its scan must take well under half a second here (host times: main took 6.6 s and 2.5 s for these two, this
 * engine milliseconds). */
static double scan_seconds(VjoInstallScan *out)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    *out = scan_now();
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static void a_crafted_config_ini_is_scanned_in_bounded_time(void)
{
    static char text[VJO_CONFIG_TEXT_MAX + 1];
    static const struct { const char *goes, *stays; int n_goes, n_stays; const char *listed; } cases[] = {
        {"dictionary = jpdb\n", NULL, 1000, 0, "dictionary = local"},             /* each replaced by the same line */
        {"ocr_backend = vocr\n", "ocr_backend:x\n", 645, 863, "ocr_backend: a repeated line goes"}, /* names shared */
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t used = 0;
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "japanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n");
        for (int i = 0; i < cases[c].n_goes; i++) used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", cases[c].goes);
        for (int i = 0; i < cases[c].n_stays; i++) used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", cases[c].stays);
        TEST_ASSERT_(used < VJO_CONFIG_TEXT_MAX, "case %u: %zu B", c, used);
        fresh_card(1);
        put_text(VJO_PREFERENCES_PATH_TEST, text);
        VjoInstallScan s;
        double took = scan_seconds(&s);
        TEST_CHECK_(took < 0.5, "case %u (%zu B): the scan took %.3f s", c, used, took);
        TEST_MSG("case %u (%zu B): %.3f s, %d listed + %d counted", c, used, took, s.config_change_count, s.config_change_more);
        TEST_CHECK_(s.config_ini == 1 && ini_listed(&s, cases[c].listed), "case %u: config_ini %d", c, s.config_ini);
    }
}

/* A config.ini whose new text differs but sets every setting as before (CRLF line ends: the app's own save
 * writes its four lines with LF) gets no empty list: the plan says it is written again as it was. */
static void a_rewrite_that_changes_no_setting_says_so(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "japanese_only_selection = on\r\nhighlight_mode = word\r\nlocal_dictionary_results = 1\r\n"
             "vocr_tier = normal\r\nlocal_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\r\n"
             "ocr_model_dir = ux0:data/VitaJPOverlay/ocr\r\ntext_source = ocr\r\nlog_file = on\r\n");
    VjoInstallScan s = scan_now();
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    int at = ini_part((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_ASSERT(at >= 0);
    TEST_CHECK_(s.config_ini == 1 && s.config_change_count == 0 && s.config_change_more == 0, "config_ini %d, %d lines",
                s.config_ini, s.config_change_count);
    TEST_CHECK_(strstr(plan[at], "config.ini: written again with every setting as it was; the old file is kept as "
                       "config.ini.bak.") != NULL, "[%s]", plan[at]);
    TEST_CHECK(at + 1 < n && strncmp(plan[at + 1], "   ", 3) != 0);
}

static void the_dictionary_already_installed_is_not_moved_again(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    /* a newer VPK installed again in VitaShell: its own copy of the (frozen) dictionary is back */
    install_app(2, "0.8.1", NULL);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_UPDATE && !strcmp(s.title, "Update the overlay from v0.8.0 to v0.8.1"), "%d %s", s.state, s.title);
    /* the dictionary's bytes are version-independent? no: version 2's differ; make them equal */
    static unsigned char bytes[1 << 20];
    char src[256];
    size_t n = payload_bytes(s.dict_index, 1, bytes, sizeof(bytes));
    vjo_payload_source(s.dict_index, src, sizeof(src));
    put(src, bytes, n);
    char hex[65], line[400];
    sha_hex(bytes, n, hex);
    /* rewrite the manifest's dictionary line */
    char *m = strstr(manifest_text, "file " VJO_PAYLOAD_DATA "dictionaries/plaintext.vjdict ");
    TEST_ASSERT(m != NULL);
    char *e = strchr(m, '\n');
    snprintf(line, sizeof(line), "file %s %zu %s move", VJO_PAYLOAD_DATA "dictionaries/plaintext.vjdict", n, hex);
    char rest[VJO_PAYLOAD_MANIFEST_MAX];
    snprintf(rest, sizeof(rest), "%s", e);
    snprintf(m, sizeof(manifest_text) - (size_t)(m - manifest_text), "%s%s", line, rest);
    put_text(VJO_PAYLOAD_MANIFEST, manifest_text);
    s = scan_now();
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int lines = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, lines);
    TEST_CHECK(has_line(plan, lines, "The dictionary is already installed: once it is checked, this app's copy is deleted"));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(has_line(r.lines, r.count, "The dictionary was already installed: this app's copy was deleted"));
    TEST_CHECK(!there(src));
    TEST_CHECK(scan_now().state == VJO_INSTALL_INSTALLED);
}

/* The app folder cannot be written (app0: stays mounted): the dictionary is copied, and stays in the app. */
static void a_move_that_cannot_happen_copies(void)
{
    fresh_card(1);
    app_readonly = 1;
    umount_rc = (int)0x80800001u;
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(has_line(r.lines, r.count, "The dictionary was copied (it could not be moved): this app keeps its copy too"));
    TEST_CHECK(installed_files(1));
    char src[256];
    vjo_payload_source(s.dict_index, src, sizeof(src));
    TEST_CHECK(there(src));
    /* without room for the copy, nothing is written at all */
    fresh_card(1);
    app_readonly = 1;
    free_space[0] = 64 * 1024;
    s = scan_now();
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) < 0 && !r.written);
    TEST_CHECK(has_line(r.lines, r.count, "Stopped: ux0 has 0.1 MB free and the install needs"));
    TEST_CHECK(has_line(r.lines, r.count, "the .vpk this app was installed from can go), then choose Install again."));
    TEST_CHECK(has_line(r.lines, r.count, "Nothing on the card was changed."));
    TEST_CHECK(!strcmp(slurp("ux0:tai/config.txt", NULL), stock) && count_files("ux0:tai", "config.txt.vjo-") == 0);
    TEST_CHECK(!there(VJO_PAYLOAD_DATA "dictionaries/plaintext.vjdict"));
}

static void cannot_states(void)
{
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    fresh_card(1);
    unsafe_off = 1;
    VjoInstallScan s = scan_now();
    TEST_CHECK(s.state == VJO_INSTALL_CANNOT && !s.actions && strstr(s.reason, "Enable Unsafe Homebrew"));
    /* a configuration taiHEN loads nothing from */
    fresh_card(1);
    put_text("ux0:tai/config.txt", "# only comments\n");
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_CANNOT && strstr(s.reason, "ux0:tai/config.txt: It has no section"), "%s", s.reason);
    /* no configuration at all */
    fresh_card(0);
    unlink("ur0:tai/config.txt");
    s = scan_now();
    TEST_CHECK(s.state == VJO_INSTALL_CANNOT && strstr(s.reason, "taiHEN's config.txt was not found"));
    /* the zip's app: no payload */
    fresh_card(1);
    remove_tree(VJO_PAYLOAD_DIR);
    s = scan_now();
    TEST_CHECK(s.state == VJO_INSTALL_NO_PAYLOAD && !s.actions && strstr(s.reason, "install-vita.sh"));
    /* a damaged manifest: a file this app does not install */
    fresh_card(1);
    char bad[VJO_PAYLOAD_MANIFEST_MAX + 400];
    snprintf(bad, sizeof(bad), "%sfile ux0:data/VitaJPOverlay/games.ini 1 %s copy\n", manifest_text,
             "0000000000000000000000000000000000000000000000000000000000000000");
    put_text(VJO_PAYLOAD_MANIFEST, bad);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_CANNOT && strstr(s.reason, "a file this app does not install"), "%s", s.reason);
    /* another build's payload (a debug app with a normal payload, another worker mode) */
    fresh_card(1);
    VjoInstallBuild other = build;
    other.worker = VJO_TAI_WORKER_LINES;
    vjo_install_scan(&s, &other);
    TEST_CHECK_(s.state == VJO_INSTALL_CANNOT && strstr(s.reason, "is not this app's build"), "%s", s.reason);
    /* the dictionary gone from both places */
    fresh_card(1);
    char src[256];
    vjo_payload_source(VJO_PAYLOAD_KERNEL_INDEX - 1, src, sizeof(src)); /* the dictionary, the last data file */
    unlink(src);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_CANNOT && strstr(s.reason, "The dictionary is missing"), "%s", s.reason);
    /* #194: right after VitaShell's install the console showed the app's folder unreadable while its
     * app0: read: restart first, no action (each reads that folder), and nothing written */
    fresh_card(1);
    put_text("app0:payload/manifest.txt", manifest_text);
    unlink(VJO_PAYLOAD_MANIFEST);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_CANNOT && !s.actions && !strcmp(s.title, "Restart the Vita first") &&
                strstr(s.reason, "Restart the Vita, then open Rubi again"), "%s: %s", s.title, s.reason);
    /* the zip's app has no app0:payload: still "no overlay", never a restart */
    fresh_card(1);
    remove_tree(VJO_PAYLOAD_DIR);
    s = scan_now();
    TEST_CHECK(s.state == VJO_INSTALL_NO_PAYLOAD);
    (void)plan;
}

/* A fault at each step of an install: the card is then as the result says, and Install again finishes.
 * A model's cases fault the Normal size's file, whatever the pins. `fires`: the update meets the case's
 * fault, checked, so a pattern no file of the install has (a model the package no longer ships) fails
 * instead of testing nothing. The six marked 0 do not meet theirs in this update, on main as here. */
static void faults_leave_old_or_new_files(void)
{
    static const struct { const char *pattern; int open_write, write, rename, remove, fires; } cases[] = {
        {VJO_VOCR_TIER2_NAME, 1, 0, 0, 0, 1}, {VJO_VOCR_TIER2_NAME, 0, 1, 0, 0, 1},
        {VJO_VOCR_TIER2_NAME, 0, 0, 1, 0, 1}, {VJO_VOCR_TIER2_NAME, 0, 0, 2, 0, 1},
        {"plaintext.vjdict", 0, 0, 1, 0, 1}, {"plaintext.vjdict", 0, 0, 2, 0, 1},
        {"VitaJPOverlay_Kernel", 0, 1, 0, 0, 1}, {"VitaJPOverlay_Shell", 0, 0, 2, 0, 1},
        {"config.ini", 1, 0, 0, 0, 0}, {"ux0:tai/config.txt", 0, 1, 0, 0, 0}, {"ux0:tai/config.txt", 0, 0, 1, 0, 0},
        {"ur0:tai/config.txt", 0, 0, 1, 0, 0}, {".bak", 1, 0, 0, 0, 0}, {"vjo_app.txt", 1, 0, 0, 0, 0},
        {"installed.txt", 0, 0, 1, 0, 1}, {".vjo-probe", 1, 0, 0, 0, 1}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        fresh_card(1);
        /* an older install of version 0 first, so that a stop leaves old and new files side by side */
        install_app(0, "0.7.99", NULL);
        VjoInstallScan s = scan_now();
        VjoInstallResult r;
        TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
        install_app(1, "0.8.0", NULL);
        fault.pattern = cases[c].pattern;
        fault.open_write = cases[c].open_write;
        fault.write = cases[c].write;
        fault.rename = cases[c].rename;
        fault.remove = cases[c].remove;
        fault.calls = 0;
        s = scan_now();
        int rc = run(&s, VJO_INSTALL_ACT_INSTALL, &r);
        int calls = fault.calls, nth = cases[c].open_write + cases[c].write + cases[c].rename + cases[c].remove;
        memset(&fault, 0, sizeof(fault));
        if (cases[c].fires)
            TEST_CHECK_(calls >= nth, "case %u (%s): %d matching calls, the fault is the %dth", c, cases[c].pattern,
                        calls, nth);
        TEST_CHECK_(every_file_old_or_new(0, 1), "case %u (%s)", c, cases[c].pattern);
        VjoInstallScan after = scan_now();
        if (rc < 0) {
            TEST_CHECK_(has_line(r.lines, r.count, "Stopped: "), "case %u", c);
            if (after.state == VJO_INSTALL_INTERRUPTED) {
                /* a stop between the swap's renames: finished from the Install page */
                TEST_CHECK_(run(&after, VJO_INSTALL_ACT_FINISH, &r) == 0, "case %u: finish", c);
                after = scan_now();
            }
            TEST_CHECK_(after.actions & VJO_INSTALL_ACT_INSTALL, "case %u: state %d", c, after.state);
            TEST_CHECK_(run(&after, VJO_INSTALL_ACT_INSTALL, &r) == 0, "case %u: again", c);
        }
        TEST_CHECK_(installed_files(1), "case %u", c);
        TEST_CHECK_(scan_now().state == VJO_INSTALL_INSTALLED, "case %u: state %d", c, scan_now().state);
        TEST_CHECK_(there("ux0:tai/config.txt") && there("ur0:tai/config.txt"), "case %u", c);
    }
}

static void an_interrupted_swap_is_finished(void)
{
    for (int with_new = 0; with_new < 2; with_new++) {
        fresh_card(0);
        char *before = strdup(slurp("ur0:tai/config.txt", NULL));
        VjoInstallScan s = scan_now();
        VjoTaiReport rep;
        static char out[VJO_TAI_TEXT_MAX + 1];
        int n = vjo_tai_edit(before, strlen(before), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
        TEST_ASSERT(n > 0);
        /* the power went between the two renames */
        TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-old") == 0);
        if (with_new) put("ur0:tai/config.txt.vjo-new", out, (size_t)n);
        s = scan_now();
        TEST_CHECK(s.state == VJO_INSTALL_INTERRUPTED && s.actions == VJO_INSTALL_ACT_FINISH);
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int lines = vjo_install_plan(&s, VJO_INSTALL_ACT_FINISH, plan, VJO_INSTALL_LINES);
        TEST_CHECK(has_line(plan, lines, with_new ? "config.txt.vjo-new, the new configuration" : "config.txt.vjo-old, the configuration from before the stop"));
        VjoInstallResult r;
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_FINISH, &r) == 0);
        TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt", NULL), with_new ? out : before));
        TEST_CHECK(!there("ur0:tai/config.txt.vjo-old"));
        free(before);
    }
    /* a .vjo-new that is no editor's output (an install stopped while writing it, then config.txt deleted by hand) is not used */
    fresh_card(0);
    char *before = strdup(slurp("ur0:tai/config.txt", NULL));
    TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-old") == 0);
    put_text("ur0:tai/config.txt.vjo-new", "*KERNEL\nur0:tai/Vita");
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(!s.cfg[1].use_new);
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_FINISH, &r) == 0 && !strcmp(slurp("ur0:tai/config.txt", NULL), before));
    TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt.vjo-20261016-071503-new.bak", NULL), "*KERNEL\nur0:tai/Vita"));
    free(before);
}

/* C4: h-encore writes its stock text into both configurations when both are missing, as after a stop in
 * ur0's swap window on a console with ur0's alone: config.txt comes back beside the stopped swap's files,
 * which are never removed: the page offers to put them back or to keep the current one. */
static void a_swap_healed_by_hencore_is_still_interrupted(void)
{
    static char out[VJO_TAI_TEXT_MAX + 1];
    for (int put_back = 0; put_back < 2; put_back++) {
        fresh_card(0);
        char mine[1024];
        snprintf(mine, sizeof(mine), "*KERNEL\nur0:tai/StorageMgr.skprx\n*main\nur0:tai/henkaku.suprx\nux0:tai/mine.suprx\n");
        put_text("ur0:tai/config.txt", mine);
        VjoTaiReport rep;
        int n = vjo_tai_edit(mine, strlen(mine), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
        TEST_ASSERT(n > 0);
        /* the stop between the renames, then h-encore's rewrite */
        TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-old") == 0);
        put("ur0:tai/config.txt.vjo-new", out, (size_t)n);
        char stock_ur0[2048];
        snprintf(stock_ur0, sizeof(stock_ur0), "# This file is used as an alternative if ux0:tai/config.txt is not found.\n%s", stock);
        put_text("ur0:tai/config.txt", stock_ur0);
        put_text("ux0:tai/config.txt", stock);
        VjoInstallScan s = scan_now();
        TEST_CHECK_(s.state == VJO_INSTALL_INTERRUPTED && (s.actions & VJO_INSTALL_ACT_FINISH) && (s.actions & VJO_INSTALL_ACT_KEEP),
                    "state %d actions %d", s.state, s.actions);
        TEST_CHECK(s.cfg[1].leftover == 3 && s.cfg[1].now_kind == VJO_TAI_NOW_STOCK && s.cfg[1].use_new);
        TEST_CHECK_(strstr(s.reason, "HENkaku's default (h-encore writes it when there is none)") != NULL, "%s", s.reason);
        /* an install over it stops rather than lose the leftovers */
        VjoInstallResult r;
        VjoInstallScan copy = s;
        TEST_CHECK(vjo_install_run(&copy, VJO_INSTALL_ACT_INSTALL, NULL, &r) < 0);
        TEST_CHECK(there("ur0:tai/config.txt.vjo-old") && there("ur0:tai/config.txt.vjo-new"));
        s = scan_now();
        TEST_CHECK(run(&s, put_back ? VJO_INSTALL_ACT_FINISH : VJO_INSTALL_ACT_KEEP, &r) == 0);
        TEST_CHECK(!there("ur0:tai/config.txt.vjo-old") && !there("ur0:tai/config.txt.vjo-new"));
        if (put_back) {
            TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt", NULL), out)); /* the user's plugins and ours */
            TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt.vjo-20261016-071503.bak", NULL), stock_ur0));
            TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt.vjo-20261016-071503-old.bak", NULL), mine));
        } else {
            TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt", NULL), stock_ur0));
            TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt.vjo-20261016-071503-old.bak", NULL), mine));
            TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt.vjo-20261016-071503-new.bak", NULL), out));
        }
        s = scan_now();
        TEST_CHECK_(s.state != VJO_INSTALL_INTERRUPTED && (s.actions & VJO_INSTALL_ACT_INSTALL), "state %d", s.state);
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && scan_now().state == VJO_INSTALL_INSTALLED);
    }
}

/* Only ux0's configuration, and two configurations that differ. */
static void one_or_two_different_configurations(void)
{
    fresh_card(1);
    unlink("ur0:tai/config.txt");
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(s.state == VJO_INSTALL_NOT_INSTALLED && !s.cfg[1].exists);
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && !there("ur0:tai/config.txt"));
    TEST_CHECK(strstr(slurp("ux0:tai/config.txt", NULL), "*main\n" S "\n") != NULL);
    fresh_card(1);
    put_text("ux0:tai/config.txt", "*KERNEL\nux0:tai/other.skprx\n*main\nur0:tai/henkaku.suprx\n");
    s = scan_now();
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(strstr(slurp("ux0:tai/config.txt", NULL), "ux0:tai/other.skprx") && !strstr(slurp("ur0:tai/config.txt", NULL), "other.skprx"));
    TEST_CHECK(scan_now().state == VJO_INSTALL_INSTALLED);
    /* a stale copy of ours in ux0:tai goes aside under its own name, never .vjo-old */
    fresh_card(1);
    put_text("ux0:tai/VitaJPOverlay_OCR.suprx", "an old worker");
    s = scan_now();
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(!there("ux0:tai/VitaJPOverlay_OCR.suprx") && !there("ux0:tai/VitaJPOverlay_OCR.suprx.vjo-old"));
    TEST_CHECK(!strcmp(slurp("ux0:tai/VitaJPOverlay_OCR.suprx.vjo-stale-20261016-071503", NULL), "an old worker"));
}

static void uninstall_keeps_files_and_backups(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    s = scan_now();
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_UNINSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    /* the install made config.txt.vjo-20261016-071503.bak: the plan names the file it will write */
    TEST_CHECK(has_line(plan, n, "Back up ux0:tai/config.txt as config.txt.vjo-20261016-071503-2.bak (kept), then remove"));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_UNINSTALL, &r) == 0);
    TEST_CHECK(has_line(r.lines, r.count, "Uninstalled: the overlay's lines are gone. Restart the Vita"));
    TEST_CHECK(!strcmp(slurp("ux0:tai/config.txt", NULL), stock));
    TEST_CHECK(installed_files(1)); /* the files stay */
    TEST_CHECK(count_files("ux0:tai", "config.txt.vjo-20261016-071503") == 2); /* the install's and the uninstall's backups */
    TEST_CHECK(scan_now().state == VJO_INSTALL_NOT_INSTALLED);
}

static void the_plan_must_still_be_the_cards(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    put_text("ux0:tai/config.txt", "*KERNEL\nur0:tai/other.skprx\n");
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) < 0 && !r.written);
    TEST_CHECK(has_line(r.lines, r.count, "The card changed since the plan was shown"));
    TEST_CHECK(!strcmp(slurp("ux0:tai/config.txt", NULL), "*KERNEL\nur0:tai/other.skprx\n"));
}

static void titles_name_the_versions(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    static const struct { const char *version; const char *title; } cases[] = {
        {"0.8.1", "Update the overlay from v0.8.0 to v0.8.1"},
        {"0.7.12", "Go back from v0.8.0 to this app's v0.7.12"},
        {"0.8.0-test.4", "Go back from v0.8.0 to this app's v0.8.0-test.4"},
        {"0.8.0", "Reinstall v0.8.0 (another build of it is installed)"}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        install_app(3 + (int)i, cases[i].version, NULL);
        s = scan_now();
        TEST_CHECK_(!strcmp(s.title, cases[i].title), "%s: %s", cases[i].version, s.title);
    }
    TEST_CHECK(vjo_version_compare("0.8.0-test.3", "0.8.0-test.4") < 0 && vjo_version_compare("0.8.0-test.10", "0.8.0-test.9") > 0);
    TEST_CHECK(vjo_version_compare("0.8.0", "0.8.0-test.9") > 0 && vjo_version_compare("0.8", "0.8.0") == 0);
    TEST_CHECK(vjo_version_compare("0.10.0", "0.9.9") > 0 && vjo_version_compare("1.0.0+build", "1.0.0") == 0);
}

/* A respin of a test build (0.8.0-test.4.1: test.4 with fixes, #194; tools/package_variants.py release_of) is a
 * version the manifest takes, after its test build and before the next and the release: over test.4 it is an
 * update, and test.4 over it goes back. */
static void a_respin_is_after_its_test_build(void)
{
    TEST_CHECK(vjo_version_compare("0.8.0-test.4.1", "0.8.0-test.4") > 0 && vjo_version_compare("0.8.0-test.4.1", "0.8.0-test.5") < 0);
    TEST_CHECK(vjo_version_compare("0.8.0-test.4.2", "0.8.0-test.4.1") > 0 && vjo_version_compare("0.8.0-test.4.10", "0.8.0-test.4.9") > 0);
    TEST_CHECK(vjo_version_compare("0.8.0", "0.8.0-test.4.1") > 0 && vjo_version_compare("0.8.0-test.4.1", "0.8.0-test.4.1") == 0);
    fresh_card(1);
    install_app(2, "0.8.0-test.4", NULL);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    install_app(3, "0.8.0-test.4.1", NULL);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_UPDATE && !strcmp(s.title, "Update the overlay from v0.8.0-test.4 to v0.8.0-test.4.1"),
                "state %d: %s", s.state, s.title);
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(scan_now().state == VJO_INSTALL_INSTALLED);
    install_app(4, "0.8.0-test.4", NULL);
    s = scan_now();
    TEST_CHECK_(!strcmp(s.title, "Go back from v0.8.0-test.4.1 to this app's v0.8.0-test.4"), "%s", s.title);
    install_app(5, "0.8.0-test.5", NULL);
    s = scan_now();
    TEST_CHECK_(!strcmp(s.title, "Update the overlay from v0.8.0-test.4.1 to v0.8.0-test.5"), "%s", s.title);
}

/* The manifest parser's rules (core/payload.c), one damaged line at a time. */
static void the_manifest_is_exactly_the_builds(void)
{
    fresh_card(1);
    VjoPayload p;
    char why[320];
    TEST_ASSERT(vjo_payload_parse(manifest_text, strlen(manifest_text), &build, &p, why, sizeof(why)) == 0);
    TEST_CHECK(!strcmp(p.version, "0.8.0") && p.worker == VJO_TAI_WORKER_KERNEL && !p.sets);
    static const struct { const char *from, *to, *why; } cases[] = {
        {"vjopayload 1\n", "vjopayload 2\n", "not a payload manifest"},
        {"api " VJO_STR(VJO_API_VERSION) "\n", "api 21\n", "not this app's build"},
        {"worker kernel", "worker lines", "not this app's build"},
        {"variant " VJO_VARIANT "\n", VJO_DEBUG ? "variant normal\n" : "variant debug\n", "not this app's build"},
        {"app_version 0.8.0", "app_version 0.8.1", "not this app's build"},
        {"worker kernel", "worker nothing", "the worker mode"},
        {"build 0123456789abcdef", "build 0123456789ABCDEF", "the app's build"},
        {" copy\n", " move\n", "a file's kind"},
        {"version 0.8.0\n", "version 0.8.0\nversion 0.8.0\n", "a line named twice"},
        {"version 0.8.0\n", "", "is missing"},
        {"vjopayload 1\n", "vjopayload 1\nset log_file maybe\n", "a setting's value"},
        {"vjopayload 1\n", "vjopayload 1\nset games default\n", "a setting this app does not set"},
        {"vjopayload 1\n", "vjopayload 1\ncolour red\n", "an unknown line"},
        {"vjopayload 1\n", "vjopayload 1\r\n", "not printable ASCII"},
        {"ocr/LM-ATTRIBUTION.md ", "ocr/../../../tai/config.txt ", "a file this app does not install"}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[VJO_PAYLOAD_MANIFEST_MAX + 200];
        const char *at = strstr(manifest_text, cases[i].from);
        TEST_ASSERT(at != NULL);
        snprintf(text, sizeof(text), "%.*s%s%s", (int)(at - manifest_text), manifest_text, cases[i].to, at + strlen(cases[i].from));
        TEST_CHECK_(vjo_payload_parse(text, strlen(text), &build, &p, why, sizeof(why)) < 0 && strstr(why, cases[i].why),
                    "case %u: %s", i, why);
    }
    /* a missing file line, a duplicated one, and the bounds */
    char text[VJO_PAYLOAD_MANIFEST_MAX + 200];
    const char *dict = strstr(manifest_text, "file " VJO_PAYLOAD_DATA "dictionaries");
    const char *after = strchr(dict, '\n') + 1;
    snprintf(text, sizeof(text), "%.*s%s", (int)(dict - manifest_text), manifest_text, after);
    TEST_CHECK(vjo_payload_parse(text, strlen(text), &build, &p, why, sizeof(why)) < 0 && strstr(why, "a file is missing"));
    snprintf(text, sizeof(text), "%s%.*s", manifest_text, (int)(after - dict), dict);
    TEST_CHECK(vjo_payload_parse(text, strlen(text), &build, &p, why, sizeof(why)) < 0 && strstr(why, "a file named twice"));
    TEST_CHECK(vjo_payload_parse(manifest_text, strlen(manifest_text) - 1, &build, &p, why, sizeof(why)) < 0);
    /* the set lines: on/off, at most 4 of the compiled keys */
    snprintf(text, sizeof(text), "%sset anki_mining on\nset log_file off\n", manifest_text);
    TEST_CHECK(vjo_payload_parse(text, strlen(text), &build, &p, why, sizeof(why)) == 0 && p.sets == 2 &&
               !strcmp(p.set[0].key, "anki_mining") && !strcmp(p.set[1].value, "off"));
    /* installed.txt round trip */
    char line[200];
    VjoInstalled rec;
    TEST_ASSERT(vjo_payload_parse(manifest_text, strlen(manifest_text), &build, &p, why, sizeof(why)) == 0);
    TEST_CHECK(vjo_installed_format(line, sizeof(line), &p) > 0 && vjo_installed_parse(line, strlen(line), &rec) == 0);
    TEST_CHECK(!strcmp(rec.version, "0.8.0") && !strcmp(rec.variant, VJO_VARIANT) && !memcmp(rec.manifest_sha256, p.manifest_sha256, 32));
    TEST_CHECK(vjo_installed_parse("", 0, &rec) < 0 && vjo_installed_parse("vjoinst1 version=0.8.0\n", 23, &rec) < 0);
}

/* A package's own settings (a test build's anki_mining = on) are applied on an update. */
static void package_settings_are_applied(void)
{
    fresh_card(1);
    put_text(VJO_PREFERENCES_PATH_TEST, "anki_mining = off\n");
    install_app(1, "0.8.0-test.4", "set anki_mining on\n");
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(strstr(slurp(VJO_PREFERENCES_PATH_TEST, NULL), "anki_mining = on\n") != NULL);
    /* the user's later choice over the same build is theirs: card mining OFF, the log off, another
     * dictionary list are no damage, and Check and repair leaves them as they are */
    char *mine = strdup(slurp(VJO_PREFERENCES_PATH_TEST, NULL));
    char *at = strstr(mine, "anki_mining = on\n");
    TEST_ASSERT(at != NULL);
    static char next[16384];
    snprintf(next, sizeof(next), "%.*sanki_mining = off\n%slog_file = off\nlocal_dictionaries = mine.vjdict\n",
             (int)(at - mine), mine, at + strlen("anki_mining = on\n"));
    put_text(VJO_PREFERENCES_PATH_TEST, next);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_INSTALLED && s.config_ini == 0, "state %d config_ini %d", s.state, s.config_ini);
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int lines = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    TEST_CHECK(has_line(plan, lines, "config.ini: unchanged (your settings stay as they are)"));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(!strcmp(slurp(VJO_PREFERENCES_PATH_TEST, NULL), next));
    TEST_CHECK(scan_now().state == VJO_INSTALL_INSTALLED);
    /* a missing config.ini (and no .bak) over the same build is written from the template, not a repair */
    remove(VJO_PREFERENCES_PATH_TEST);
    remove(VJO_PREFERENCES_PATH_TEST ".bak");
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_INSTALLED && s.config_ini == 2, "state %d config_ini %d", s.state, s.config_ini);
    /* another build (an update) patches it again: the package's set lines apply */
    put_text(VJO_PREFERENCES_PATH_TEST, next);
    install_app(1, "0.8.0-test.5", "set anki_mining on\n");
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_UPDATE && s.config_ini == 1, "state %d config_ini %d", s.state, s.config_ini);
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(strstr(slurp(VJO_PREFERENCES_PATH_TEST, NULL), "anki_mining = on\n") != NULL);
    free(mine);
}

/* ---------------- the page ---------------- */
static char drawn[64][300];
static int drawn_count;
static void d_text(int x, int y, unsigned c, float px, const char *s) { (void)x; (void)y; (void)c; (void)px; if (drawn_count < 64) snprintf(drawn[drawn_count++], 300, "%s", s); }
/* rows laid out in the last frame: fit_line measures each row's first character alone first (plans are ASCII) */
static int rows_laid_out;
static int d_measure(float px, const char *s)
{
    rows_laid_out += strlen(s) == 1;
    return (int)(strlen(s) * (size_t)(px * 0.5f));
}
static int bar_rects; /* rectangles right of the plan's text (its scroll bar) in the last frame */
static int bar_y[2], bar_h[2]; /* the bar's first two: its track, then its thumb */
static void d_rect(int x, int y, int w, int h, unsigned c)
{
    (void)w; (void)c;
    if (x < 900) return;
    if (bar_rects < 2) { bar_y[bar_rects] = y; bar_h[bar_rects] = h; }
    bar_rects++;
}
static void d_card(int y, int h, int f) { (void)y; (void)h; (void)f; }
static void d_badge(int x, int y, int w, int h, const char *l, int a)
{
    (void)x; (void)y; (void)w; (void)h;
    if (drawn_count < 64) snprintf(drawn[drawn_count++], 300, "[%s%s]", l, a ? "*" : "");
}
static void d_hints(const char *c) { if (drawn_count < 64) snprintf(drawn[drawn_count++], 300, "hints: %s", c); }
static void d_header(void) {}
static const VjoButtons glyphs = {SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, "×", "○"};
static const VjoInstallDraw draw = {d_text, d_measure, d_rect, d_card, d_badge, d_hints, d_header, 1, 2, 3, 4, 5, &glyphs};
static int shown(const char *needle)
{
    for (int i = 0; i < drawn_count; i++)
        if (strstr(drawn[i], needle)) return 1;
    return 0;
}
static void frame(uint32_t pressed)
{
    drawn_count = 0;
    bar_rects = rows_laid_out = 0;
    vjo_install_page_frame(&draw, pressed, -1, -1);
}

static void the_page_shows_the_plan_and_runs_it(void)
{
    fresh_card(1);
    TEST_CHECK(vjo_install_page_start() == 1 && vjo_install_page_has_payload());
    frame(0);
    for (int i = 0; i < drawn_count; i++) TEST_MSG("%s", drawn[i]);
    TEST_CHECK(shown(VJO_DEBUG ? "Install the overlay (v0.8.0, debug build)" : "Install the overlay (v0.8.0)"));
    TEST_CHECK(shown("[Install*]") && shown("hints: ×: Install   Left/Right: action   Up/Down: scroll   ○: exit"));
    TEST_CHECK(shown("1. Copy the 3 plugins"));
    frame(SCE_CTRL_CROSS); /* the stubs run the thread at once */
    frame(0);
    for (int i = 0; i < drawn_count; i++) TEST_MSG("%s", drawn[i]);
    TEST_CHECK(!vjo_install_page_busy());
    TEST_CHECK(shown("Installed the overlay v0.8.0") && shown("hints: ×: back"));
    TEST_CHECK(shown(VJO_DEBUG ? "The overlay is installed (v0.8.0, debug build)" : "The overlay is installed (v0.8.0)"));
    frame(SCE_CTRL_CROSS); /* back to the plan: the installed card's */
    frame(0);
    TEST_CHECK(shown("[Check and repair*]") && shown("[Uninstall]"));
    frame(SCE_CTRL_RIGHT);
    frame(0);
    TEST_CHECK(shown("[Uninstall*]") && shown("Back up ux0:tai/config.txt as config.txt.vjo-"));
    TEST_CHECK(installed_files(1));
}

/* A stop while swap_config wrote config.txt.vjo-new (after its open, before or during the write; a power
 * cut, so nothing removed the partial file): config.txt is as it was and there is no .vjo-old, so the swap
 * never started. Only Keep is offered, first; its .vjo-new (empty, or a prefix) is never put in place.
 * A whole .vjo-new (this app's edit of the current config.txt, byte for byte) can be put in place too. */
static void a_stopped_write_of_vjo_new_keeps_config_txt(void)
{
    static char out[VJO_TAI_TEXT_MAX + 1];
    for (int both = 0; both < 2; both++)
        for (int cut = 0; cut < 4; cut++) { /* 0 B, half, the whole edit, a prefix ending after our Shell line */
            fresh_card(both);
            install_app(1, "0.8.0", NULL);
            const char *target = both ? "ux0:tai/config.txt" : "ur0:tai/config.txt";
            char *before = strdup(slurp(target, NULL));
            VjoTaiReport rep;
            int n = vjo_tai_edit(before, strlen(before), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
            TEST_ASSERT(n > 200);
            char fresh[64];
            snprintf(fresh, sizeof(fresh), "%s.vjo-new", target);
            /* the last cut is a whole configuration of its own: our two lines, the user's later sections lost */
            const char *shell = strstr(out, VJO_TAI_SHELL "\n");
            TEST_ASSERT(shell != NULL);
            size_t len = cut == 0 ? 0 : cut == 1 ? (size_t)n / 2 : cut == 2 ? (size_t)n
                       : (size_t)(shell - out) + strlen(VJO_TAI_SHELL "\n");
            put(fresh, out, len);
            VjoInstallScan s = scan_now();
            const VjoInstallConfig *g = &s.cfg[both ? 0 : 1];
            TEST_CHECK_(s.state == VJO_INSTALL_INTERRUPTED, "both %d cut %d: state %d", both, cut, s.state);
            TEST_CHECK_(g->leftover == 2 && g->swap_files == 2 && g->use_new == (cut == 2), "both %d cut %d", both, cut);
            TEST_CHECK_(s.actions == (cut == 2 ? (VJO_INSTALL_ACT_KEEP | VJO_INSTALL_ACT_FINISH) : VJO_INSTALL_ACT_KEEP),
                        "both %d cut %d: actions 0x%x", both, cut, (unsigned)s.actions);
            TEST_CHECK_(strstr(s.reason, "stopped before it changed config.txt, which is as it was") != NULL, "%s", s.reason);
            /* the page offers Keep first */
            vjo_install_page_start();
            frame(0);
            TEST_CHECK_(shown("[Keep the current one*]"), "both %d cut %d", both, cut);
            TEST_CHECK_(shown("[Put it back]") == (cut == 2), "both %d cut %d", both, cut);
            if (cut == 2) {
                char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
                int lines = vjo_install_plan(&s, VJO_INSTALL_ACT_FINISH, plan, VJO_INSTALL_LINES);
                TEST_CHECK(has_line(plan, lines, "this app's whole edit of the current one, checked byte for byte"));
                VjoInstallResult r;
                TEST_CHECK(run(&s, VJO_INSTALL_ACT_FINISH, &r) == 0);
                size_t m;
                char *now = slurp(target, &m);
                TEST_CHECK(now && m == (size_t)n && !memcmp(now, out, (size_t)n));
                TEST_CHECK(!strcmp(slurp(both ? "ux0:tai/config.txt.vjo-20261016-071503.bak" : "ur0:tai/config.txt.vjo-20261016-071503.bak", NULL), before));
                TEST_CHECK(!there(fresh));
            } else {
                char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
                int lines = vjo_install_plan(&s, VJO_INSTALL_ACT_KEEP, plan, VJO_INSTALL_LINES);
                TEST_CHECK(has_line(plan, lines, "its config.txt.vjo-new is kept as config.txt.vjo-20261016-071503-new.bak"));
                /* Put it back is refused even when asked for */
                VjoInstallResult r;
                VjoInstallScan copy = s;
                TEST_CHECK(vjo_install_run(&copy, VJO_INSTALL_ACT_FINISH, NULL, &r) < 0);
                TEST_CHECK(!strcmp(slurp(target, NULL), before) && there(fresh));
                s = scan_now();
                TEST_CHECK(run(&s, VJO_INSTALL_ACT_KEEP, &r) == 0);
                TEST_CHECK_(!strcmp(slurp(target, NULL), before), "both %d cut %d: config.txt changed", both, cut);
                size_t m;
                char *kept = slurp(both ? "ux0:tai/config.txt.vjo-20261016-071503-new.bak" : "ur0:tai/config.txt.vjo-20261016-071503-new.bak", &m);
                TEST_CHECK(kept && m == len && !memcmp(kept, out, len) && !there(fresh));
                /* then Install works as on any card */
                s = scan_now();
                TEST_CHECK_(s.actions & VJO_INSTALL_ACT_INSTALL, "state %d", s.state);
                TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && installed_files(1));
                TEST_CHECK(scan_now().state == VJO_INSTALL_INSTALLED);
            }
            free(before);
        }
    /* config.txt missing beside a .vjo-new that is no whole configuration this app wrote, and no .vjo-old:
     * nothing is offered (the reason says what to do in VitaShell); it is never put in place */
    fresh_card(0);
    install_app(1, "0.8.0", NULL);
    char *before = strdup(slurp("ur0:tai/config.txt", NULL));
    TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-20261016-071500.bak") == 0);
    put_text("ur0:tai/config.txt.vjo-new", "*KERNEL\nur0:tai/VitaJPOverlay_Kernel.skprx\n*main\nur0:tai/hen");
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_INTERRUPTED && s.actions == 0, "state %d actions 0x%x", s.state, (unsigned)s.actions);
    TEST_CHECK_(strstr(s.reason, "copy its newest dated backup") != NULL, "%s", s.reason);
    VjoInstallResult r;
    TEST_CHECK(vjo_install_run(&s, VJO_INSTALL_ACT_FINISH, NULL, &r) < 0 && !there("ur0:tai/config.txt"));
    free(before);
}

/* R6 (the code audit's mutants M9, M9b, M13, M14; the first and the last are the auditor's tests) */
static void a_damaged_payload_file_is_not_installed(void)
{
    fresh_card(1);
    char src[256];
    vjo_payload_source(VJO_PAYLOAD_KERNEL_INDEX, src, sizeof(src));
    size_t n;
    char *k = slurp(src, &n);
    TEST_ASSERT(k != NULL && n > 10);
    k[5] ^= 1; /* the same size: only the hash tells */
    put(src, k, n);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) < 0);
    TEST_CHECK(!there("ur0:tai/VitaJPOverlay_Kernel.skprx") && !there("ur0:tai/VitaJPOverlay_Kernel.skprx.vjo-new"));
    TEST_CHECK(!strcmp(slurp("ux0:tai/config.txt", NULL), stock));
}

static void a_damaged_dictionary_in_the_app_stops(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    char src[256];
    TEST_ASSERT(s.dict_index >= 0 && vjo_payload_source(s.dict_index, src, sizeof(src)) == 0);
    size_t n;
    char *d = slurp(src, &n);
    TEST_ASSERT(d != NULL && n > 10);
    d[n / 2] ^= 1;
    put(src, d, n);
    s = scan_now();
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) < 0);
    TEST_CHECK(has_line(r.lines, r.count, "this app's plaintext.vjdict is damaged: install the VPK again in VitaShell."));
    /* the dated backups were written first (new files), and nothing else */
    TEST_CHECK(has_line(r.lines, r.count, "Nothing on the card was changed but the dated backups this run wrote (new files, kept)."));
    TEST_CHECK(!there(VJO_PAYLOAD_DATA "dictionaries/plaintext.vjdict") && there(src));
    TEST_CHECK(!there("ur0:tai/VitaJPOverlay_Kernel.skprx"));
    TEST_CHECK(!strcmp(slurp("ux0:tai/config.txt", NULL), stock));
}

static void interrupted_comes_before_unsafe_homebrew(void)
{
    fresh_card(0);
    TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-old") == 0);
    unsafe_off = 1;
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_INTERRUPTED && s.actions == 0, "state %d actions 0x%x", s.state, (unsigned)s.actions);
    TEST_CHECK_(strstr(s.reason, "in VitaShell rename ur0:tai/config.txt.vjo-old to config.txt.") != NULL, "%s", s.reason);
    unsafe_off = 0;
}

static void a_write_fault_in_the_swap_keeps_config_txt(void)
{
    for (int c = 0; c < 2; c++) {
        fresh_card(1);
        const char *path = c ? "ur0:tai/config.txt" : "ux0:tai/config.txt";
        char *before = strdup(slurp(path, NULL));
        VjoInstallScan s = scan_now();
        fault.pattern = path;
        fault.write = 2; /* the dated backup is the first write whose name has the path, the swap's .vjo-new the second */
        fault.calls = 0;
        VjoInstallResult r;
        int rc = run(&s, VJO_INSTALL_ACT_INSTALL, &r);
        memset(&fault, 0, sizeof(fault));
        char *now = slurp(path, NULL);
        TEST_CHECK_(rc < 0 && now && !strcmp(now, before), "%s: a failed write leaves config.txt as it was", path);
        free(before);
    }
}

/* ---------------- the code audit's notes (audit-overlay-one-vpk-install-b: 1-6, N5, N17, N24) ---------------- */

/* A plan's "Exactly:" block under the line naming path: its lines (at most cap) and their count, -1 when none. */
static int exact_block(const char plan[][VJO_INSTALL_LINE], int n, const char *path, const char *out[], int cap)
{
    int at = -1, k = 0;
    for (int i = 0; i < n && at < 0; i++)
        if (strstr(plan[i], path) && strstr(plan[i], "Exactly:")) at = i;
    if (at < 0) return -1;
    for (int i = at + 1; i < n && !strncmp(plan[i], "   ", 3); i++)
        if (k < cap) out[k++] = plan[i];
    return k;
}

static int same_lines(const char *const got[], int n, const char *const want[])
{
    int w = 0;
    while (want[w]) w++;
    if (n != w) return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(got[i], want[i])) return 0;
    return 1;
}

#define DIS "# Mainline OCR: "
/* Note 1: when a line the editor comments out has a copy further on, the exact lines are still the edit:
 * the fewest lines, and never a user's kept line shown as removed and added. The first three are the
 * code audit's; in the fourth our line moves up past one user line, where a minimal diff could keep
 * either (GNU diff shows *ALL as removed and added). */
static void the_plan_shows_the_edit_when_lines_repeat(void)
{
    static const struct { const char *name, *text, *want[5]; } cases[] = {
        {"our Shell line under *ALL and *main (fixture shell-under-all)",
         "*KERNEL\n" K "\n*ALL\n" S "\nur0:tai/every.suprx\n*main\n" S "\nur0:tai/henkaku.suprx\n",
         {"   - " S, "   + " DIS S, NULL}},
        {"our kernel line under *ALL before *KERNEL",
         "*ALL\n" K "\n*KERNEL\nur0:tai/user.skprx\n*main\n" S "\nur0:tai/henkaku.suprx\n",
         {"   - " K, "   + " DIS K, "   + " K, NULL}},
        {"our Shell line under *NPXS10015 before *main",
         "*KERNEL\n" K "\n*NPXS10015\n" S "\nur0:tai/henkaku.suprx\n*main\nur0:tai/henkaku.suprx\n",
         {"   - " S, "   + " DIS S, "   + " S, NULL}},
        {"our kernel line under *ALL right after an empty *KERNEL",
         "*KERNEL\n*ALL\n" K "\nur0:tai/every.suprx\n*main\n" S "\nur0:tai/henkaku.suprx\n",
         {"   + " K, "   - " K, "   + " DIS K, NULL}},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        fresh_card(1);
        put_text("ux0:tai/config.txt", cases[c].text);
        VjoInstallScan s = scan_now();
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
        const char *got[16];
        int k = exact_block((const char (*)[VJO_INSTALL_LINE])plan, n, "ux0:tai/config.txt", got, 16);
        TEST_CHECK_(same_lines(got, k, cases[c].want), "%s", cases[c].name);
        for (int i = 0; i < k; i++) TEST_MSG("[%s]", got[i]);
    }
}

/* N17: CRLF, CR-only and mixed line ends: each line end is one, whatever its bytes. */
static void the_plan_reads_every_line_end_as_one(void)
{
    static const char *const ends[] = {"\r\n", "\r", "mixed"};
    for (int e = 0; e < 3; e++) {
        fresh_card(1);
        char text[2048];
        size_t used = 0;
        int line = 0;
        for (const char *p = stock; *p; p++) {
            if (*p == '\n') {
                const char *end = e < 2 ? ends[e] : (line++ % 2 ? "\r\n" : "\n");
                used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", end);
            } else {
                text[used++] = *p;
                text[used] = '\0';
            }
        }
        put_text("ux0:tai/config.txt", text);
        VjoInstallScan s = scan_now();
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
        const char *got[16];
        static const char *const install[] = {"   + " K, "   + " S, NULL}, *const uninstall[] = {"   - " K, "   - " S, NULL};
        int k = exact_block((const char (*)[VJO_INSTALL_LINE])plan, n, "ux0:tai/config.txt", got, 16);
        TEST_CHECK_(same_lines(got, k, install), "line ends %d: %d lines", e, k);
        VjoInstallResult r;
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
        s = scan_now();
        n = vjo_install_plan(&s, VJO_INSTALL_ACT_UNINSTALL, plan, VJO_INSTALL_LINES);
        k = exact_block((const char (*)[VJO_INSTALL_LINE])plan, n, "Back up ux0:tai/config.txt", got, 16);
        TEST_CHECK_(same_lines(got, k, uninstall), "uninstall, line ends %d: %d lines", e, k);
    }
}

/* N5: a Put it back whose rename fails puts the current config.txt back as it was. */
static void a_failed_put_back_puts_the_current_one_back(void)
{
    static char out[VJO_TAI_TEXT_MAX + 1];
    fresh_card(0);
    char *before = strdup(slurp("ur0:tai/config.txt", NULL));
    VjoTaiReport rep;
    int n = vjo_tai_edit(before, strlen(before), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
    TEST_ASSERT(n > 0);
    put("ur0:tai/config.txt.vjo-new", out, (size_t)n); /* the whole edit: Put it back is offered */
    VjoInstallScan s = scan_now();
    TEST_ASSERT(s.actions & VJO_INSTALL_ACT_FINISH);
    fault.pattern = "ur0:tai/config.txt";
    fault.rename = 2; /* 1: the current one to its dated backup; 2: .vjo-new to config.txt */
    fault.calls = 0;
    VjoInstallResult r;
    int rc = run(&s, VJO_INSTALL_ACT_FINISH, &r);
    memset(&fault, 0, sizeof(fault));
    char *now = slurp("ur0:tai/config.txt", NULL);
    TEST_CHECK_(rc < 0 && now && !strcmp(now, before) && there("ur0:tai/config.txt.vjo-new"),
                "a failed put-back leaves config.txt as it was");
    TEST_CHECK(has_line(r.lines, r.count, "the current one is back as it was"));
    /* the Install page says what is left; Install alone would stop beside the leftover */
    TEST_CHECK(has_line(r.lines, r.count, "the Install page shows what is left to do"));
    TEST_CHECK(!has_line(r.lines, r.count, "Install again finishes the job"));
    free(before);
}

/* The card of the code audit's note 2: an update over a card whose two configurations still have four games'
 * worker sections each (test.3's --title), an older config.ini with many changes, dated backups from before the
 * overlay in both folders, and two backup names (ux0's folder already has this second's). */
static void long_plan_card(void)
{
    fresh_card(1);
    static const char *const games[] = {"PCSG00415", "PCSE00001", "ULJS00378", "PSPEMUCFW"};
    static char text[VJO_TAI_TEXT_MAX + 1];
    for (int c = 0; c < 2; c++) {
        char base[2048];
        snprintf(base, sizeof(base), "%s%s", c ? "# This file is used as an alternative if ux0:tai/config.txt is not found.\n" : "", stock);
        VjoTaiReport rep;
        int m = vjo_tai_edit(base, strlen(base), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, text, sizeof(text), &rep);
        TEST_ASSERT(m > 0);
        size_t used = (size_t)m;
        for (int g = 0; g < 4; g++) used += (size_t)snprintf(text + used, sizeof(text) - used, "\n*%s\n" W "\n", games[g]);
        put_text(c ? "ur0:tai/config.txt" : "ux0:tai/config.txt", text);
        put_text(c ? "ur0:tai/config.txt.vjo-20261001-120000.bak" : "ux0:tai/config.txt.vjo-20261001-120000.bak", stock);
    }
    put_text("ux0:tai/config.txt.vjo-20261016-071503.bak", stock);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    put_text(VJO_PREFERENCES_PATH_TEST, "dictionary = jpdb\nlocal_dictionaries = mine.vjdict\nanki_mining = off\nlog_file = off\n"
             "frequency_filter = 5\n");
    install_app(1, "0.8.0-test.5", "set anki_mining on\nset log_file on\n");
}

/* Note 2: a plan longer than the page's lines still shows its restart and recovery lines (AGENTS.md rule 7)
 * and each configuration's exact lines, fewer of them, the rest counted. */
static void a_long_plan_keeps_its_restart_and_recovery_lines(void)
{
    long_plan_card();
    VjoInstallScan s = scan_now();
    TEST_ASSERT(s.state == VJO_INSTALL_UPDATE);
    static char all[80][VJO_INSTALL_LINE];
    int whole = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, all, 80);
    TEST_CHECK_(whole > VJO_INSTALL_LINES, "the case is longer than a page's plan: %d lines", whole);
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_CHECK(n <= VJO_INSTALL_LINES);
    TEST_CHECK(has_line(plan, n, "Then restart the Vita yourself: this app never restarts it."));
    TEST_CHECK(has_line(plan, n, VJO_INSTALL_RECOVERY_BOOT) && has_line(plan, n, VJO_INSTALL_RECOVERY_GAME));
    TEST_CHECK(has_line(plan, n, "(ux0:tai/config.txt.vjo-20261016-071503.bak is the configuration from before the overlay.)"));
    TEST_CHECK(has_line(plan, n, "(ur0:tai/config.txt.vjo-20261001-120000.bak is the configuration from before the overlay.)"));
    TEST_CHECK(has_line(plan, n, "A dated backup is the configuration from before this install"));
    TEST_CHECK(has_line(plan, n, "Back up ux0:tai/config.txt as config.txt.vjo-20261016-071503-2.bak (kept)."));
    TEST_CHECK(has_line(plan, n, "Read everything back to check it."));
    /* both configurations' exact lines, cut and counted: every changed line is shown or counted */
    for (int c = 0; c < 2; c++) {
        const char *got[16];
        int k = exact_block((const char (*)[VJO_INSTALL_LINE])plan, n, c ? "ur0:tai/config.txt" : "ux0:tai/config.txt", got, 16);
        const VjoInstallConfig *g = &s.cfg[c];
        int more = 0;
        TEST_ASSERT(k >= 2);
        TEST_CHECK_(sscanf(got[k - 1], "   and %d more line", &more) == 1, "[%s]", got[k - 1]);
        TEST_CHECK_(k - 1 + more == g->diff_count[0] + g->diff_more[0], "%d shown + %d counted, of %d", k - 1, more,
                    g->diff_count[0] + g->diff_more[0]);
        TEST_CHECK(k - 1 >= 2); /* never fewer than two of them */
    }
    /* config.ini's: the first changes, then the rest counted */
    int at = -1;
    for (int i = 0; i < n; i++)
        if (strstr(plan[i], "config.ini: these lines change")) at = i;
    TEST_ASSERT(at >= 0);
    int listed = 0, more = -1;
    for (int i = at + 1; i < n && !strncmp(plan[i], "   ", 3); i++)
        if (sscanf(plan[i], "   and %d more", &more) != 1) listed++;
    TEST_CHECK_(more < 0 ? listed == s.config_change_count : listed + more == s.config_change_count + s.config_change_more,
                "config.ini: %d listed, %d counted, of %d", listed, more, s.config_change_count);
    /* a plan that fits is the whole plan */
    fresh_card(1);
    s = scan_now();
    TEST_CHECK(vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES) == vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, all, 80));
}

static int first_shown(const char *needle)
{
    for (int i = 0; i < drawn_count; i++)
        if (!strncmp(drawn[i], needle, strlen(needle))) return 1;
    return 0;
}

/* Note 2 on the page: L and R page through a long plan, a box at a time, and a bar shows where the view is. */
static void the_page_pages_through_a_long_plan(void)
{
    long_plan_card();
    TEST_CHECK(vjo_install_page_start() == 1);
    frame(0);
    int first_rows = rows_laid_out;
    TEST_CHECK(first_shown("1. Copy the 3 plugins") && !shown("If a game does not start"));
    TEST_CHECK_(bar_rects == 2, "a scroll bar beside a plan longer than its box: %d rectangles", bar_rects);
    /* its thumb: at the track's top first, shorter than it, inside it at every step, at its bottom at the end */
    TEST_CHECK_(bar_y[1] == bar_y[0] && bar_h[1] >= 12 && bar_h[1] < bar_h[0], "track %d+%d, thumb %d+%d", bar_y[0], bar_h[0],
                bar_y[1], bar_h[1]);
    TEST_CHECK(shown("Left/Right: action   Up/Down: scroll   ○: exit"));
    /* the first frame lays out only what the box shows and the first entry past it, as main's page did (the
     * bar counts entries: counting every entry's rows laid out the whole plan at once) */
    TEST_CHECK_(first_rows <= 16, "%d rows laid out in the first frame", first_rows);
    int pages = 0;
    while (!shown("If a game does not start") && pages < 12) {
        frame(SCE_CTRL_RTRIGGER);
        frame(0);
        pages++;
        TEST_CHECK_(bar_rects == 2 && bar_y[1] >= bar_y[0] && bar_y[1] + bar_h[1] <= bar_y[0] + bar_h[0],
                    "page %d: track %d+%d, thumb %d+%d", pages, bar_y[0], bar_h[0], bar_y[1], bar_h[1]);
    }
    TEST_CHECK_(bar_y[1] + bar_h[1] == bar_y[0] + bar_h[0], "the last box: thumb %d+%d, track %d+%d", bar_y[1], bar_h[1],
                bar_y[0], bar_h[0]);
    TEST_CHECK_(shown("If a game does not start") && pages > 1 && pages < 8, "R reaches the plan's end in %d pages", pages);
    TEST_CHECK(!shown("1. Copy the 3 plugins"));
    frame(SCE_CTRL_RTRIGGER); /* at the end it stays */
    frame(0);
    TEST_CHECK(shown("If a game does not start"));
    for (int i = 0; i < pages + 1 && !shown("1. Copy the 3 plugins"); i++) {
        frame(SCE_CTRL_LTRIGGER);
        frame(0);
    }
    TEST_CHECK(first_shown("1. Copy the 3 plugins"));
    /* a plan that fits its box has no bar */
    fresh_card(0);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    vjo_install_page_start();
    frame(SCE_CTRL_RIGHT); /* Uninstall's plan: one short block */
    frame(0);
    TEST_CHECK_(bar_rects == 0, "%d", bar_rects);
    TEST_CHECK(shown("hints: ×: Uninstall   Left/Right: action   ○: exit"));
}

/* N24: the page shows a reason a sentence a line: each sentence starts a row. */
static void the_reason_is_shown_a_sentence_a_line(void)
{
    static char out[VJO_TAI_TEXT_MAX + 1];
    fresh_card(0);
    char *before = strdup(slurp("ur0:tai/config.txt", NULL));
    VjoTaiReport rep;
    int n = vjo_tai_edit(before, strlen(before), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
    TEST_ASSERT(n > 100);
    put("ur0:tai/config.txt.vjo-new", out, (size_t)n / 2); /* an install's write, stopped */
    VjoInstallScan s = scan_now();
    TEST_ASSERT(s.state == VJO_INSTALL_INTERRUPTED);
    vjo_install_page_start();
    frame(0);
    int sentences = 0;
    for (const char *at = s.reason; *at;) {
        const char *end = strstr(at, ". ");
        char start[24];
        snprintf(start, sizeof(start), "%.20s", at);
        TEST_CHECK_(first_shown(start), "a row starts with [%s]", start);
        sentences++;
        if (!end) break;
        at = end + 2;
    }
    TEST_CHECK_(sentences >= 2, "%d sentences: %s", sentences, s.reason);
    for (int i = 0; i < drawn_count; i++) TEST_MSG("%s", drawn[i]);
    free(before);
}

/* Note 4: a stopped uninstall's file is named an uninstall's, and its next step is Uninstall. */
static void a_stopped_uninstall_is_named_one(void)
{
    static char out[VJO_TAI_TEXT_MAX + 1];
    for (int whole = 0; whole < 2; whole++) {
        fresh_card(0);
        VjoInstallScan s = scan_now();
        VjoInstallResult r;
        TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
        char *installed = strdup(slurp("ur0:tai/config.txt", NULL));
        VjoTaiReport rep;
        int n = vjo_tai_edit(installed, strlen(installed), VJO_TAI_UNINSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
        TEST_ASSERT(n > 100 && rep.changed);
        /* the write stopped part way (or it was whole), before the swap */
        put("ur0:tai/config.txt.vjo-new", out, whole ? (size_t)n : (size_t)n / 2);
        s = scan_now();
        TEST_CHECK(s.state == VJO_INSTALL_INTERRUPTED && s.cfg[1].stopped_run == VJO_INSTALL_ACT_UNINSTALL);
        TEST_CHECK_(strstr(s.reason, "a stopped uninstall's config.txt.vjo-new") && strstr(s.reason, "choose Uninstall again") &&
                    !strstr(s.reason, "stopped install's") && strstr(s.reason, "stopped before it changed config.txt, which is as it was"),
                    "%s", s.reason);
        free(installed);
    }
    /* a swap that had started (.vjo-old) and h-encore's text since: Put it back names the uninstall's file */
    fresh_card(0);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    char *installed = strdup(slurp("ur0:tai/config.txt", NULL));
    VjoTaiReport rep;
    int n = vjo_tai_edit(installed, strlen(installed), VJO_TAI_UNINSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
    TEST_ASSERT(n > 0);
    TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-old") == 0);
    put("ur0:tai/config.txt.vjo-new", out, (size_t)n);
    char stock_ur0[2048];
    snprintf(stock_ur0, sizeof(stock_ur0), "# This file is used as an alternative if ux0:tai/config.txt is not found.\n%s", stock);
    put_text("ur0:tai/config.txt", stock_ur0);
    s = scan_now();
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int lines = vjo_install_plan(&s, VJO_INSTALL_ACT_FINISH, plan, VJO_INSTALL_LINES);
    TEST_CHECK(has_line(plan, lines, "config.txt.vjo-new (the stopped uninstall's new configuration, checked against config.txt.vjo-old)"));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_FINISH, &r) == 0 && has_line(r.lines, r.count, "the stopped uninstall's new configuration"));
    free(installed);
    /* an empty file beside a configuration both would change: either */
    fresh_card(0);
    put_text("ur0:tai/config.txt", "*KERNEL\n" K "\n*main\nur0:tai/henkaku.suprx\n");
    put_text("ur0:tai/config.txt.vjo-new", "");
    s = scan_now();
    TEST_CHECK(s.state == VJO_INSTALL_INTERRUPTED && s.cfg[1].stopped_run == 0);
    TEST_CHECK_(strstr(s.reason, "a stopped install's or uninstall's config.txt.vjo-new") &&
                strstr(s.reason, "choose Install or Uninstall again"), "%s", s.reason);
    /* and an install's is an install's */
    fresh_card(0);
    put_text("ur0:tai/config.txt.vjo-new", "");
    s = scan_now();
    TEST_CHECK(s.cfg[1].stopped_run == VJO_INSTALL_ACT_INSTALL);
    TEST_CHECK_(strstr(s.reason, "a stopped install's config.txt.vjo-new") && strstr(s.reason, "choose Install again"), "%s", s.reason);
}

/* Note 4: a run that stops part way says what finishes the job: the same action, or the Install page when a
 * stopped swap's file is left (it offers to put it back or keep the current one first). */
static void a_stopped_uninstall_says_uninstall_again(void)
{
    for (int left = 0; left < 2; left++) {
        fresh_card(1);
        VjoInstallScan s = scan_now();
        VjoInstallResult r;
        TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
        s = scan_now();
        /* ux0's swap is done; ur0's first rename fails (nothing left), or its second (its .vjo-new is left) */
        fault.pattern = "ur0:tai/config.txt";
        fault.rename = left ? 2 : 1;
        fault.calls = 0;
        int rc = run(&s, VJO_INSTALL_ACT_UNINSTALL, &r);
        memset(&fault, 0, sizeof(fault));
        TEST_CHECK(rc < 0 && r.written);
        TEST_CHECK(!has_line(r.lines, r.count, "Install again finishes the job."));
        TEST_CHECK_(has_line(r.lines, r.count, left ? "the Install page shows what is left to do" : "Uninstall again finishes the job."),
                    "left %d", left);
        s = scan_now();
        if (left) {
            TEST_CHECK(s.state == VJO_INSTALL_INTERRUPTED && (s.actions & VJO_INSTALL_ACT_KEEP));
            TEST_CHECK(run(&s, VJO_INSTALL_ACT_KEEP, &r) == 0);
            s = scan_now();
        }
        TEST_CHECK_(s.actions & VJO_INSTALL_ACT_UNINSTALL, "left %d: state %d", left, s.state);
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_UNINSTALL, &r) == 0 && scan_now().state == VJO_INSTALL_NOT_INSTALLED);
    }
}

/* Note 4: this build with a damaged plugin or another record of the app is a Repair, not "another build". */
static void this_build_damaged_is_a_repair(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    for (int what = 0; what < 2; what++) {
        if (what == 0) {
            size_t n;
            char *sh = slurp("ur0:tai/VitaJPOverlay_Shell.suprx", &n);
            TEST_ASSERT(sh && n > 10);
            sh[3] ^= 1; /* the same size: the hash tells */
            put("ur0:tai/VitaJPOverlay_Shell.suprx", sh, n);
        } else {
            put_text(VJO_APP_STAMP_PACKAGE, "vjoapp1 api=" VJO_STR(VJO_API_VERSION) " build=fedcba9876543210\n");
        }
        s = scan_now();
        TEST_CHECK_(s.state == VJO_INSTALL_REPAIR && !strcmp(s.title, "Repair the overlay (v0.8.0)"), "%d: state %d, %s", what,
                    s.state, s.title);
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
        dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
        TEST_CHECK(what == 0 ? has_line(plan, n, "Replace 1 of the plugins")
                             : has_line(plan, n, "vjo_app.txt (the plugins' record of their app) is written again: it is missing or names another build of this app."));
        TEST_CHECK(!has_line(plan, n, "does not undo the update"));
        vjo_install_page_start();
        frame(0);
        TEST_CHECK(shown("[Repair*]"));
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && scan_now().state == VJO_INSTALL_INSTALLED);
    }
}

/* Note 6: over this build, a config.ini missing beside its config.ini.bak (a save stopped between its two
 * renames) is written again from the backup: the plugins read config.ini, the app the backup. */
static void a_missing_config_ini_is_written_from_its_backup(void)
{
    for (int keyed = 0; keyed < 2; keyed++) {
        fresh_card(1);
        VjoInstallScan s = scan_now();
        VjoInstallResult r;
        TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
        char mine[VJO_CONFIG_TEXT_MAX + 1];
        snprintf(mine, sizeof(mine), "%svocr_tier = large\nanki_mining = off\n%s", slurp(VJO_PREFERENCES_PATH_TEST, NULL),
                 keyed ? "jpdb_api_key = secret-key\n" : "");
        put_text(VJO_PREFERENCES_PATH_TEST ".bak", mine);
        TEST_ASSERT(remove(VJO_PREFERENCES_PATH_TEST) == 0);
        s = scan_now();
        TEST_CHECK_(s.state == VJO_INSTALL_INSTALLED && s.config_ini == 3, "state %d config_ini %d", s.state, s.config_ini);
        char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
        int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
        dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
        TEST_CHECK(has_line(plan, n, "config.ini is missing beside its backup config.ini.bak (a save that stopped): it is "
                                     "written again from the backup, your settings as they were."));
        TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && r.written);
        char *ini = slurp(VJO_PREFERENCES_PATH_TEST, NULL);
        TEST_ASSERT(ini != NULL);
        VjoConfig cfg;
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, ini, strlen(ini));
        TEST_CHECK((unsigned)cfg.vocr_tier == VJO_VOCR_TIER_LARGE && !cfg.anki_mining);
        TEST_CHECK(!strstr(ini, "secret") && !strstr(ini, "api_key")); /* as the app's own save writes it */
        char *bak = slurp(VJO_PREFERENCES_PATH_TEST ".bak", NULL);
        TEST_CHECK(!bak || !strstr(bak, "secret"));
        TEST_CHECK(scan_now().config_ini == 0 && scan_now().state == VJO_INSTALL_INSTALLED);
    }
}

/* Note 1, the diff itself (vjo_install_diff) against a reference on random texts: its lines are an edit of
 * old into new (old's lines it does not remove are new's it does not add, in order); within its bounds they
 * are the fewest (old's and new's lines less twice the longest common subsequence), and of those the ones
 * that keep the most lines not naming the overlay; past them (a long changed stretch, many changes) the
 * greedy walk's, still an edit. Line ends are CR, LF or CRLF, mixed, with or without a last one. */
typedef struct { const char *old, *neu; int old_len, new_len, lines[2], n, bad; int side[4096], index[4096]; char sign[4096]; } DiffSeen;
static int split_lines(const char *t, int len, int starts[], int lens[], int cap)
{
    int n = 0, i = 0;
    while (i < len && n < cap) {
        int b = i;
        while (i < len && t[i] != '\r' && t[i] != '\n') i++;
        starts[n] = b;
        lens[n++] = i - b;
        if (i < len && t[i] == '\r') { i++; if (i < len && t[i] == '\n') i++; }
        else if (i < len) i++;
    }
    return n;
}
static int ref_starts[2][4096], ref_lens[2][4096];
static void seen_line(void *ctx, char sign, const char *s, int n)
{
    DiffSeen *d = ctx;
    const char *t = sign == '-' ? d->old : d->neu;
    int side = sign == '+', len = sign == '-' ? d->old_len : d->new_len, at = (int)(s - t), lo = 0, hi = d->lines[side] - 1;
    if (s < t || at > len || d->n >= 4096) { d->bad = 1; return; }
    while (lo < hi) { /* the line that starts there (the starts ascend) */
        int mid = (lo + hi) / 2;
        if (ref_starts[side][mid] < at) lo = mid + 1;
        else hi = mid;
    }
    int k = hi >= 0 && ref_starts[side][lo] == at && ref_lens[side][lo] == n ? lo : -1;
    if (k < 0) { d->bad = 1; return; }
    d->side[d->n] = side;
    d->index[d->n] = k;
    d->sign[d->n++] = sign;
}
static int ref_user(const char *s, int n)
{
    for (int i = 0; i + 13 <= n; i++)
        if (!strncasecmp(s + i, "vitajpoverlay", 13)) return 0;
    return 1;
}
static int ref_same(const DiffSeen *d, int i, int j)
{
    return ref_lens[0][i] == ref_lens[1][j] && !memcmp(d->old + ref_starts[0][i], d->neu + ref_starts[1][j], (size_t)ref_lens[0][i]);
}
/* the longest common subsequence's length, and for small texts the most user lines it can keep */
static int ref_lcs(const DiffSeen *d, int n, int m, int *user)
{
    static int row[2][4097];
    static long best[64][64];
    memset(row, 0, sizeof(row));
    for (int i = n - 1; i >= 0; i--)
        for (int j = m - 1; j >= 0; j--)
            row[i & 1][j] = ref_same(d, i, j) ? row[(i + 1) & 1][j + 1] + 1
                          : row[(i + 1) & 1][j] > row[i & 1][j + 1] ? row[(i + 1) & 1][j] : row[i & 1][j + 1];
    *user = -1;
    if (n < 64 && m < 64) {
        for (int i = n; i >= 0; i--)
            for (int j = m; j >= 0; j--)
                best[i][j] = i == n || j == m ? 0
                           : ref_same(d, i, j) ? best[i + 1][j + 1] + 1000 + ref_user(d->old + ref_starts[0][i], ref_lens[0][i])
                           : best[i + 1][j] > best[i][j + 1] ? best[i + 1][j] : best[i][j + 1];
        *user = (int)(best[0][0] % 1000);
    }
    return n && m ? row[0][0] : 0;
}

static uint64_t diff_seed = 0x9E3779B97F4A7C15ull;
static int rnd(int k)
{
    diff_seed = diff_seed * 6364136223846793005ull + 1442695040888963407ull;
    return (int)((diff_seed >> 33) % (uint64_t)k);
}

static void the_exact_lines_are_a_shortest_edit(void)
{
    static const char *const words[] = {"*KERNEL", "*main", "*ALL", "*PCSG00415", "*NPXS10015", K, S, W, DIS K, DIS S,
                                         "ur0:tai/henkaku.suprx", "ux0:tai/a.suprx", "", "# a comment", "# VitaJPOverlay note",
                                         "ur0:tai/every.suprx"};
    static const char *const ends[] = {"\n", "\r\n", "\r"};
    static char old[1 << 16], neu[1 << 16];
    static DiffSeen d;
    int exact = 0, greedy = 0, cases = 0;
    for (int c = 0; c < 4000; c++) {
        /* the size: mostly small; some long with few changes (exact), some past the bounds (greedy) */
        int big = c % 200 == 199 ? 2 : c % 200 == 198 ? 1 : 0;
        int lines = big == 2 ? 1200 + rnd(400) : big == 1 ? 600 + rnd(300) : rnd(40);
        int alphabet = c % 3 == 0 ? 4 : (int)(sizeof(words) / sizeof(words[0]));
        int ol = 0, nl = 0, mixed = c % 5;
        for (int i = 0; i < lines; i++) {
            const char *w = words[rnd(alphabet)], *e = mixed ? ends[rnd(3)] : ends[c % 3];
            ol += snprintf(old + ol, sizeof(old) - (size_t)ol, "%s%s", w, i + 1 < lines || rnd(4) ? e : "");
        }
        /* new: old with lines removed, commented out and added, as the editor does (and more) */
        int starts[4096], lens[4096], n0 = split_lines(old, ol, starts, lens, 4096);
        int edits = big ? 1 + rnd(big == 2 ? 600 : 8) : rnd(8);
        for (int i = 0; i <= n0; i++) {
            if (i < n0) {
                int r = rnd(lines + 1);
                if (r < edits && rnd(3) == 0) continue; /* removed */
                const char *e = mixed ? ends[rnd(3)] : ends[c % 3];
                if (r < edits && rnd(2) == 0) nl += snprintf(neu + nl, sizeof(neu) - (size_t)nl, DIS "%.*s%s", lens[i], old + starts[i], e);
                else nl += snprintf(neu + nl, sizeof(neu) - (size_t)nl, "%.*s%s", lens[i], old + starts[i], e);
            }
            if (rnd(lines + 1) < edits && rnd(3) == 0)
                nl += snprintf(neu + nl, sizeof(neu) - (size_t)nl, "%s%s", words[rnd(alphabet)], mixed ? ends[rnd(3)] : ends[c % 3]);
        }
        TEST_ASSERT(ol < (int)sizeof(old) && nl < (int)sizeof(neu));
        int n = split_lines(old, ol, ref_starts[0], ref_lens[0], 4096), m = split_lines(neu, nl, ref_starts[1], ref_lens[1], 4096);
        memset(&d, 0, sizeof(d));
        d.old = old; d.neu = neu; d.old_len = ol; d.new_len = nl; d.lines[0] = n; d.lines[1] = m;
        vjo_install_diff(old, ol, neu, nl, seen_line, &d);
        cases++;
        TEST_ASSERT_(!d.bad, "case %d: a line that is not one of the texts'", c);
        /* an edit: in file order on each side, and old's kept lines are new's kept lines */
        static unsigned char gone[2][4096];
        memset(gone, 0, sizeof(gone));
        int last[2] = {-1, -1}, ordered = 1;
        for (int k = 0; k < d.n; k++) {
            ordered &= d.index[k] > last[d.side[k]];
            last[d.side[k]] = d.index[k];
            gone[d.side[k]][d.index[k]] = 1;
        }
        int i = 0, j = 0, edit = ordered;
        while (edit && (i < n || j < m)) {
            while (i < n && gone[0][i]) i++;
            while (j < m && gone[1][j]) j++;
            if (i == n || j == m) { edit = i == n && j == m; break; }
            edit = ref_same(&d, i, j);
            i++;
            j++;
        }
        TEST_CHECK_(edit, "case %d (%d and %d lines): not an edit of old into new", c, n, m);
        int user, lcs = ref_lcs(&d, n, m, &user), kept_user = 0;
        for (int k = 0; k < n; k++) kept_user += !gone[0][k] && ref_user(old + ref_starts[0][k], ref_lens[0][k]);
        if (d.n == n + m - 2 * lcs) exact++;
        else greedy++;
        if (!big || big == 1) TEST_CHECK_(d.n == n + m - 2 * lcs, "case %d (%d and %d lines): %d lines, the fewest %d", c, n, m, d.n, n + m - 2 * lcs);
        if (user >= 0) TEST_CHECK_(kept_user == user, "case %d: %d user lines kept, %d could be", c, kept_user, user);
    }
    TEST_MSG("%d cases: %d shortest, %d longer (past the bounds)", cases, exact, greedy);
    TEST_CHECK(exact > 3500);
}

/* The follow-ups' audit: a .vjo-new byte-identical to config.txt (an install that appends its sections,
 * stopped at exactly config.txt's length) is a stopped install's, never an uninstall's whose edit changed
 * nothing: Keep only, as for any part of a write, then Install works. */
static void a_vjo_new_equal_to_config_txt_is_a_stopped_install(void)
{
    static char out[VJO_TAI_TEXT_MAX + 1];
    fresh_card(0);
    const char *mine = "# my config\n*PCSG00415\nux0:tai/game.suprx\n"; /* no *KERNEL or *main: the install appends them */
    VjoTaiReport rep;
    int n = vjo_tai_edit(mine, strlen(mine), VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, sizeof(out), &rep);
    TEST_ASSERT(n > (int)strlen(mine) && !memcmp(out, mine, strlen(mine)) && rep.changed);
    put_text("ur0:tai/config.txt", mine);
    put("ur0:tai/config.txt.vjo-new", out, strlen(mine));
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_INTERRUPTED && s.actions == VJO_INSTALL_ACT_KEEP && !s.cfg[1].use_new &&
                s.cfg[1].stopped_run == VJO_INSTALL_ACT_INSTALL, "state %d actions 0x%x use_new %d run %d", s.state,
                (unsigned)s.actions, s.cfg[1].use_new, s.cfg[1].stopped_run);
    TEST_CHECK_(strstr(s.reason, "a stopped install's config.txt.vjo-new") && strstr(s.reason, "choose Install again") &&
                !strstr(s.reason, "Or put the new one in place"), "%s", s.reason);
    VjoInstallResult r;
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_KEEP, &r) == 0 && !strcmp(slurp("ur0:tai/config.txt", NULL), mine));
    s = scan_now();
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt", NULL), out) && scan_now().state == VJO_INSTALL_INSTALLED);
}

/* The follow-ups' audit, gaps. D7a: a changed stretch of 1,024 lines is diffed exactly, one of 1,025 by the
 * greedy walk: either is an edit made of the texts' own lines. */
static void the_diff_at_its_line_bound(void)
{
    static char old[1 << 16], neu[1 << 16];
    static DiffSeen d;
    for (int lines = 1023; lines <= 1026; lines++) {
        int ol = 0, nl = 0;
        for (int i = 0; i < lines; i++) {
            /* the first and the last line differ: the trim leaves every line in the changed stretch */
            ol += snprintf(old + ol, sizeof(old) - (size_t)ol, i == 0 ? "first old\n" : i == lines - 1 ? "last old\n" : "ux0:tai/p%04d.suprx\n", i);
            nl += snprintf(neu + nl, sizeof(neu) - (size_t)nl, i == 0 ? "first new\n" : i == lines - 1 ? "last new\n" : "ux0:tai/p%04d.suprx\n", i);
        }
        int n = split_lines(old, ol, ref_starts[0], ref_lens[0], 4096), m = split_lines(neu, nl, ref_starts[1], ref_lens[1], 4096);
        memset(&d, 0, sizeof(d));
        d.old = old; d.neu = neu; d.old_len = ol; d.new_len = nl; d.lines[0] = n; d.lines[1] = m;
        vjo_install_diff(old, ol, neu, nl, seen_line, &d);
        TEST_CHECK_(!d.bad && d.n == 4 && d.sign[0] == '-' && d.index[0] == 0 && d.sign[1] == '+' && d.index[1] == 0 &&
                    d.sign[2] == '-' && d.index[2] == n - 1 && d.sign[3] == '+' && d.index[3] == m - 1,
                    "%d lines: bad %d, %d lines shown", lines, d.bad, d.n);
    }
}

/* P1: a plan never writes past max and never returns more, for every action and every max. */
static void a_plan_stays_within_its_max(void)
{
    static char lines[80][VJO_INSTALL_LINE];
    for (int card = 0; card < 2; card++) {
        if (card == 0) long_plan_card();
        else { /* interrupted: Put it back and Keep */
            fresh_card(0);
            TEST_ASSERT(rename("ur0:tai/config.txt", "ur0:tai/config.txt.vjo-old") == 0);
            put_text("ur0:tai/config.txt", "*KERNEL\n*main\nur0:tai/henkaku.suprx\n");
        }
        VjoInstallScan s = scan_now();
        static const int acts[] = {VJO_INSTALL_ACT_INSTALL, VJO_INSTALL_ACT_UNINSTALL, VJO_INSTALL_ACT_FINISH, VJO_INSTALL_ACT_KEEP};
        for (unsigned a = 0; a < 4; a++)
            for (int max = 0; max <= VJO_INSTALL_LINES; max++) {
                for (int i = 0; i < 80; i++) snprintf(lines[i], VJO_INSTALL_LINE, "canary %d", i);
                int n = vjo_install_plan(&s, acts[a], lines, max);
                int intact = 1;
                for (int i = max; i < 80; i++) {
                    char want[32];
                    snprintf(want, sizeof(want), "canary %d", i);
                    intact &= !strcmp(lines[i], want);
                }
                TEST_CHECK_(n >= 0 && n <= max && intact, "card %d action %d max %d: %d lines, past max %s", card, acts[a], max, n,
                            intact ? "intact" : "written");
            }
    }
}

/* I3: over this build, a config.ini.bak too large for the app's save to rewrite is left to the app (it reads
 * it): config.ini is not written, and the plan says it is unchanged. */
static void a_backup_too_large_to_rewrite_is_left(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    static char big[VJO_CONFIG_TEXT_MAX + 1];
    memset(big, '#', VJO_CONFIG_TEXT_MAX);
    for (int i = 79; i < VJO_CONFIG_TEXT_MAX; i += 80) big[i] = '\n';
    big[VJO_CONFIG_TEXT_MAX - 1] = '\n';
    big[VJO_CONFIG_TEXT_MAX] = '\0'; /* comments only: the save would append its four lines */
    put_text(VJO_PREFERENCES_PATH_TEST ".bak", big);
    TEST_ASSERT(remove(VJO_PREFERENCES_PATH_TEST) == 0);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_INSTALLED && s.config_ini == 0, "state %d config_ini %d", s.state, s.config_ini);
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    TEST_CHECK(has_line(plan, n, "config.ini: unchanged (your settings stay as they are)."));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && !there(VJO_PREFERENCES_PATH_TEST));
    size_t m;
    TEST_CHECK(slurp(VJO_PREFERENCES_PATH_TEST ".bak", &m) && m == VJO_CONFIG_TEXT_MAX);
}

/* P15: a Repair that writes a configuration backs it up, but says nothing of an update. */
static void a_repair_with_backups_has_no_update_note(void)
{
    fresh_card(1);
    VjoInstallScan s = scan_now();
    VjoInstallResult r;
    TEST_ASSERT(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0);
    char *ux0 = slurp("ux0:tai/config.txt", NULL), mine[4096], *at = strstr(ux0, S "\n");
    TEST_ASSERT(at != NULL);
    snprintf(mine, sizeof(mine), "%.*s%s", (int)(at - ux0), ux0, at + strlen(S "\n")); /* our Shell line gone */
    put_text("ux0:tai/config.txt", mine);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_REPAIR && s.cfg[0].install_changes, "state %d: %s", s.state, s.title);
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_CHECK(has_line(plan, n, "Back up ux0:tai/config.txt as config.txt.vjo-20261016-071503-2.bak (kept)."));
    TEST_CHECK(!has_line(plan, n, "does not undo the update"));
    TEST_CHECK(run(&s, VJO_INSTALL_ACT_INSTALL, &r) == 0 && scan_now().state == VJO_INSTALL_INSTALLED);
}

TEST_LIST = {
    {"the_diff_at_its_line_bound", the_diff_at_its_line_bound},
    {"a_plan_stays_within_its_max", a_plan_stays_within_its_max},
    {"a_backup_too_large_to_rewrite_is_left", a_backup_too_large_to_rewrite_is_left},
    {"a_repair_with_backups_has_no_update_note", a_repair_with_backups_has_no_update_note},
    {"a_vjo_new_equal_to_config_txt_is_a_stopped_install", a_vjo_new_equal_to_config_txt_is_a_stopped_install},
    {"the_exact_lines_are_a_shortest_edit", the_exact_lines_are_a_shortest_edit},
    {"the_plan_shows_the_edit_when_lines_repeat", the_plan_shows_the_edit_when_lines_repeat},
    {"the_plan_reads_every_line_end_as_one", the_plan_reads_every_line_end_as_one},
    {"a_failed_put_back_puts_the_current_one_back", a_failed_put_back_puts_the_current_one_back},
    {"a_long_plan_keeps_its_restart_and_recovery_lines", a_long_plan_keeps_its_restart_and_recovery_lines},
    {"the_page_pages_through_a_long_plan", the_page_pages_through_a_long_plan},
    {"the_reason_is_shown_a_sentence_a_line", the_reason_is_shown_a_sentence_a_line},
    {"a_stopped_uninstall_is_named_one", a_stopped_uninstall_is_named_one},
    {"a_stopped_uninstall_says_uninstall_again", a_stopped_uninstall_says_uninstall_again},
    {"this_build_damaged_is_a_repair", this_build_damaged_is_a_repair},
    {"a_missing_config_ini_is_written_from_its_backup", a_missing_config_ini_is_written_from_its_backup},
    {"fresh_install_on_both_configurations", fresh_install_on_both_configurations},
    {"update_over_a_test3_install", update_over_a_test3_install},
    {"config_ini_from_0712", config_ini_from_0712},
    {"the_plan_names_each_config_ini_line_that_goes", the_plan_names_each_config_ini_line_that_goes},
    {"a_line_that_goes_is_named_by_its_setting_never_its_value", a_line_that_goes_is_named_by_its_setting_never_its_value},
    {"a_rewrite_that_changes_no_setting_says_so", a_rewrite_that_changes_no_setting_says_so},
    {"a_bom_is_no_part_of_a_line", a_bom_is_no_part_of_a_line},
    {"a_later_lines_bom_stays_part_of_it", a_later_lines_bom_stays_part_of_it},
    {"each_scan_marks_its_own_lines", each_scan_marks_its_own_lines},
    {"any_changed_same_named_line_makes_it_a_change", any_changed_same_named_line_makes_it_a_change},
    {"a_mid_file_bom_never_shows_a_value", a_mid_file_bom_never_shows_a_value},
    {"a_bom_led_copy_is_the_same_setting_to_the_plugins", a_bom_led_copy_is_the_same_setting_to_the_plugins},
    {"a_bom_files_changed_first_line_is_listed_without_its_bom", a_bom_files_changed_first_line_is_listed_without_its_bom},
    {"removed_settings_go_however_their_line_starts", removed_settings_go_however_their_line_starts},
    {"a_line_that_goes_is_named_whatever_its_case_or_line_ends", a_line_that_goes_is_named_whatever_its_case_or_line_ends},
    {"a_crafted_config_ini_is_scanned_in_bounded_time", a_crafted_config_ini_is_scanned_in_bounded_time},
    {"the_dictionary_already_installed_is_not_moved_again", the_dictionary_already_installed_is_not_moved_again},
    {"a_move_that_cannot_happen_copies", a_move_that_cannot_happen_copies},
    {"cannot_states", cannot_states},
    {"faults_leave_old_or_new_files", faults_leave_old_or_new_files},
    {"an_interrupted_swap_is_finished", an_interrupted_swap_is_finished},
    {"a_swap_healed_by_hencore_is_still_interrupted", a_swap_healed_by_hencore_is_still_interrupted},
    {"a_stopped_write_of_vjo_new_keeps_config_txt", a_stopped_write_of_vjo_new_keeps_config_txt},
    {"a_damaged_payload_file_is_not_installed", a_damaged_payload_file_is_not_installed},
    {"a_damaged_dictionary_in_the_app_stops", a_damaged_dictionary_in_the_app_stops},
    {"interrupted_comes_before_unsafe_homebrew", interrupted_comes_before_unsafe_homebrew},
    {"a_write_fault_in_the_swap_keeps_config_txt", a_write_fault_in_the_swap_keeps_config_txt},
    {"one_or_two_different_configurations", one_or_two_different_configurations},
    {"uninstall_keeps_files_and_backups", uninstall_keeps_files_and_backups},
    {"the_plan_must_still_be_the_cards", the_plan_must_still_be_the_cards},
    {"titles_name_the_versions", titles_name_the_versions},
    {"a_respin_is_after_its_test_build", a_respin_is_after_its_test_build},
    {"the_manifest_is_exactly_the_builds", the_manifest_is_exactly_the_builds},
    {"package_settings_are_applied", package_settings_are_applied},
    {"the_page_shows_the_plan_and_runs_it", the_page_shows_the_plan_and_runs_it},
    {NULL, NULL}};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("settings-install")) return 2; /* every card in it, all removed at the end */
    return acutest_main(argc, argv);
}
