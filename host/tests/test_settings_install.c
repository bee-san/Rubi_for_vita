/* The Overlay app's one-VPK install (settings/install.c, settings/install_page.c, core/payload.c) on
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
#include "acutest.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#include "../../settings/install.h"
#include "../../settings/install_page.h"

#ifndef VJO_SOURCE_DIR
#define VJO_SOURCE_DIR "."
#endif
#define K VJO_TAI_KERNEL
#define S VJO_TAI_SHELL
#define W VJO_TAI_WORKER
#define STAMP "vjoapp1 api=22 build=0123456789abcdef"

static const VjoInstallBuild build = {VJO_API_VERSION, "0.8.0", VJO_VARIANT, VJO_TAI_WORKER_KERNEL};

/* ---------------- the card ---------------- */
static char card[256];
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }

/* faults: the nth matching call fails (0: none); the counters count matching calls */
static struct {
    const char *pattern;   /* a path containing it */
    int open_write, write, rename, remove; /* fail the nth such call */
    int calls;
} fault;
static int unsafe_off, umount_rc, app_readonly, locked, lock_calls;
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
    char link[64], path[512];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t m = readlink(link, path, sizeof(path) - 1);
    if (m > 0) {
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
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    if (!d) { unlink(path); return; }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        remove_tree(sub);
    }
    closedir(d);
    rmdir(path);
}
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

static void fresh_card(int with_ux0)
{
    if (card[0]) { if (chdir("/") == 0) remove_tree(card); }
    snprintf(card, sizeof(card), "/tmp/vjo-install-XXXXXX");
    TEST_ASSERT(mkdtemp(card) != NULL);
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
    TEST_CHECK(has_line(plan, n, "ux0:tai/config.txt (taiHEN reads this one at boot): add the kernel line under *KERNEL and the Shell line under *main."));
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
    put_text(VJO_APP_STAMP_PACKAGE, "vjoapp1 api=22 build=fedcba9876543210\n");
    put_text(VJO_PREFERENCES_PATH_TEST, "vocr_tier = large\nocr_mode = on_press\nlog_file = off\n");
    VjoInstallScan s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_UPDATE, "state %d %s", s.state, s.title);
    TEST_CHECK(!strcmp(s.title, "Update the overlay to v0.8.0 (the installed one has no version record)"));
    char plan[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int n = vjo_install_plan(&s, VJO_INSTALL_ACT_INSTALL, plan, VJO_INSTALL_LINES);
    dump_lines((const char (*)[VJO_INSTALL_LINE])plan, n);
    TEST_CHECK(has_line(plan, n, "ur0:tai/config.txt (taiHEN reads this one at boot): remove the OCR worker's lines from *ALL, *PCSG00415."));
    TEST_CHECK(has_line(plan, n, "config.ini: "));
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
             "ocr_backend = vocr\nvocr_model = FL10_w8.vocr\nfont_size_ja = 24\n");
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
    vjo_payload_source(8, src, sizeof(src));
    unlink(src);
    s = scan_now();
    TEST_CHECK_(s.state == VJO_INSTALL_CANNOT && strstr(s.reason, "The dictionary is missing"), "%s", s.reason);
    (void)plan;
}

/* A fault at each step of an install: the card is then as the result says, and Install again finishes. */
static void faults_leave_old_or_new_files(void)
{
    static const struct { const char *pattern; int open_write, write, rename, remove; } cases[] = {
        {"VOCR3_H15", 1, 0, 0, 0}, {"VOCR3_H15", 0, 1, 0, 0}, {"VOCR3_H15", 0, 0, 1, 0}, {"VOCR3_H15", 0, 0, 2, 0},
        {"plaintext.vjdict", 0, 0, 1, 0}, {"plaintext.vjdict", 0, 0, 2, 0}, {"VitaJPOverlay_Kernel", 0, 1, 0, 0},
        {"VitaJPOverlay_Shell", 0, 0, 2, 0}, {"config.ini", 1, 0, 0, 0}, {"ux0:tai/config.txt", 0, 1, 0, 0},
        {"ux0:tai/config.txt", 0, 0, 1, 0}, {"ur0:tai/config.txt", 0, 0, 1, 0}, {".bak", 1, 0, 0, 0},
        {"vjo_app.txt", 1, 0, 0, 0}, {"installed.txt", 0, 0, 1, 0}, {".vjo-probe", 1, 0, 0, 0}};
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
        memset(&fault, 0, sizeof(fault));
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
        {"api 22", "api 21", "not this app's build"},
        {"worker kernel", "worker lines", "not this app's build"},
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
}

/* ---------------- the page ---------------- */
static char drawn[64][300];
static int drawn_count;
static void d_text(int x, int y, unsigned c, float px, const char *s) { (void)x; (void)y; (void)c; (void)px; if (drawn_count < 64) snprintf(drawn[drawn_count++], 300, "%s", s); }
static int d_measure(float px, const char *s) { return (int)(strlen(s) * (size_t)(px * 0.5f)); }
static void d_rect(int x, int y, int w, int h, unsigned c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
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

TEST_LIST = {
    {"fresh_install_on_both_configurations", fresh_install_on_both_configurations},
    {"update_over_a_test3_install", update_over_a_test3_install},
    {"config_ini_from_0712", config_ini_from_0712},
    {"the_dictionary_already_installed_is_not_moved_again", the_dictionary_already_installed_is_not_moved_again},
    {"a_move_that_cannot_happen_copies", a_move_that_cannot_happen_copies},
    {"cannot_states", cannot_states},
    {"faults_leave_old_or_new_files", faults_leave_old_or_new_files},
    {"an_interrupted_swap_is_finished", an_interrupted_swap_is_finished},
    {"a_swap_healed_by_hencore_is_still_interrupted", a_swap_healed_by_hencore_is_still_interrupted},
    {"one_or_two_different_configurations", one_or_two_different_configurations},
    {"uninstall_keeps_files_and_backups", uninstall_keeps_files_and_backups},
    {"the_plan_must_still_be_the_cards", the_plan_must_still_be_the_cards},
    {"titles_name_the_versions", titles_name_the_versions},
    {"the_manifest_is_exactly_the_builds", the_manifest_is_exactly_the_builds},
    {"package_settings_are_applied", package_settings_are_applied},
    {"the_page_shows_the_plan_and_runs_it", the_page_shows_the_plan_and_runs_it},
    {NULL, NULL}};
