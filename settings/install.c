/* The Overlay app's install engine (settings/install.h; docs/one-vpk-install.md). */
#include "install.h"
#include "../include/vjo_build.h"
#include "anki_page.h" /* vjo_log: log.txt */
#include "preferences.h"
#include "../core/app_stamp.h"
#include "../core/config.h"
#include <bearssl_hash.h>
#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/shellutil.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MISSING ((int)0x80010002u)   /* SCE_ERROR_ERRNO_ENOENT */
#define SAFE_MODE ((int)0x80010030u) /* sceIoDevctl's answer to a sandboxed app (VitaShell init.c:353-355) */
#define CHUNK (256 * 1024)           /* the most read, written or hashed per call */
#define ROOM (512 * 1024)            /* free space kept beside the copies (settings/psp.c kept as much) */
#define BOOT_RECORD "ur0:data/VitaJPOverlay/boot.txt" /* core/boot_guard.h */
/* The running kernel writes this boot's record at its next change, the Shell's ok 60 s after the start
 * (kernel/boot.c BOOT_OK_MIN_US): a reset before then is written over, so it waits for 70 s of uptime. */
#define BOOT_RECORD_FINAL_US (70ull * 1000000ull)
#define CONFIG_INI_BACKUP VJO_PREFERENCES_PATH ".bak"
#define PATH 200

static char cfg_text[2][VJO_TAI_TEXT_MAX + 2], cfg_new[2][VJO_TAI_TEXT_MAX + 1], cfg_check[VJO_TAI_TEXT_MAX + 2];
static int cfg_len[2], cfg_new_len[2];
static unsigned char chunk[CHUNK];
static char ini_new[VJO_CONFIG_TEXT_MAX + 1], ini_step[VJO_CONFIG_TEXT_MAX + 1], template_text[VJO_CONFIG_TEXT_MAX + 2];
static int ini_new_len;
static unsigned char arena_mem[128 * 1024] __attribute__((aligned(16)));

/* ---------------- files ---------------- */

static int exists(const char *path)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(path, &st) >= 0;
}

static int64_t size_of(const char *path)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(path, &st) < 0 || SCE_S_ISDIR(st.st_mode) ? -1 : (int64_t)st.st_size;
}

static void sync_path(const char *path)
{
    char dev[5];
    memcpy(dev, path, 4);
    dev[4] = '\0';
    sceIoSync(dev, 0);
}

/* path's text into buf (NUL-terminated): its length; MISSING when it is not there; another error, or
 * cap - 1 when it is larger than buf holds (the caller's limit then refuses it). */
static int read_all(const char *path, char *buf, size_t cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    size_t used = 0;
    if (fd < 0) return fd == MISSING ? MISSING : fd;
    while (used < cap - 1) {
        int n = sceIoRead(fd, buf + used, cap - 1 - used);
        if (n < 0) { sceIoClose(fd); return n; }
        if (!n) break;
        used += (size_t)n;
    }
    sceIoClose(fd);
    buf[used] = '\0';
    return (int)used;
}

static int write_all(SceUID fd, const void *data, size_t len)
{
    for (size_t used = 0; used < len;) {
        int n = sceIoWrite(fd, (const char *)data + used, len - used);
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

/* data as path, synced and read back: 0, or -1. */
static int write_verified(const char *path, const void *data, size_t len)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    if (sceIoClose(fd) < 0) rc = -1;
    sync_path(path);
    if (rc < 0 || len > sizeof(cfg_check) - 1) return -1;
    int n = read_all(path, cfg_check, sizeof(cfg_check));
    return n == (int)len && !memcmp(cfg_check, data, len) ? 0 : -1;
}

/* The first line of path (at most cap - 1 bytes, its line end cut), or NULL. */
static const char *first_line(const char *path, char *buf, size_t cap)
{
    int n = read_all(path, buf, cap);
    if (n <= 0) return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf;
}

/* Each folder of dest up to its file, made (errors ignored: the probe finds a folder that is not there). */
static void make_dirs(const char *dest)
{
    char p[PATH];
    snprintf(p, sizeof(p), "%s", dest);
    for (char *s = strchr(p + 5, '/'); s; s = strchr(s + 1, '/')) {
        *s = '\0';
        sceIoMkdir(p, 0777);
        *s = '/';
    }
}

static int64_t free_bytes(const char *dev)
{
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    return sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof(info)) < 0 ? -1 : (int64_t)info.free_size;
}

/* A name beside path that is not there: path + suffix, else path + suffix + "-2", "-3", ... */
static void free_name(char *out, size_t cap, const char *path, const char *suffix)
{
    snprintf(out, cap, "%s%s", path, suffix);
    for (int k = 2; exists(out) && k < 100; k++) snprintf(out, cap, "%s%s-%d", path, suffix, k);
}

/* ---------------- a run's progress ---------------- */

typedef struct {
    const VjoInstallUi *ui;
    VjoInstallResult *r;
    uint64_t last_tick;
    int action;                /* VJO_INSTALL_ACT_*: what finishes the job after a stop */
    int backed_up;             /* dated backups written (new files: nothing else changed by them) */
    char step[120];
} Run;

static void progress(Run *x, uint64_t done, uint64_t total)
{
    uint64_t now = sceKernelGetProcessTimeWide();
    /* held off for the whole run (VitaShell utils.c:128-138 ticks every 10 s) */
    if (now - x->last_tick >= 10000000u) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        x->last_tick = now;
    }
    if (x->ui && x->ui->progress) x->ui->progress(x->ui->ud, x->step, done, total);
}

static void step(Run *x, const char *fmt, const char *arg)
{
    snprintf(x->step, sizeof(x->step), fmt, arg ? arg : "");
    progress(x, 0, 0);
}

static void say(VjoInstallResult *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(VjoInstallResult *r, const char *fmt, ...)
{
    va_list ap;
    if (r->count >= VJO_INSTALL_LINES) return;
    va_start(ap, fmt);
    vsnprintf(r->lines[r->count++], VJO_INSTALL_LINE, fmt, ap);
    va_end(ap);
}

/* path's SHA-256 (CHUNK at a time): its size, or -1. */
static int64_t hash_file(Run *x, const char *path, uint8_t sha[32], uint64_t total)
{
    br_sha256_context h;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int64_t used = 0;
    if (fd < 0) return -1;
    br_sha256_init(&h);
    for (;;) {
        int n = sceIoRead(fd, chunk, CHUNK);
        if (n < 0) { used = -1; break; }
        if (!n) break;
        br_sha256_update(&h, chunk, (size_t)n);
        used += n;
        if (x) progress(x, (uint64_t)used, total);
    }
    sceIoClose(fd);
    br_sha256_out(&h, sha);
    return used;
}

/* src to dst, checked against f while it is read and read back after: 0, or -1. */
static int copy_verified(Run *x, const char *src, const char *dst, const VjoPayloadFile *f)
{
    br_sha256_context h;
    uint8_t sha[32];
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0), out;
    uint64_t used = 0;
    int rc = 0;
    if (in < 0) return -1;
    out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) { sceIoClose(in); return -1; }
    br_sha256_init(&h);
    for (;;) {
        int n = sceIoRead(in, chunk, CHUNK);
        if (n < 0) { rc = -1; break; }
        if (!n) break;
        br_sha256_update(&h, chunk, (size_t)n);
        if (write_all(out, chunk, (size_t)n) < 0) { rc = -1; break; }
        used += (uint64_t)n;
        progress(x, used, f->bytes * 2);
    }
    sceIoClose(in);
    if (sceIoClose(out) < 0) rc = -1;
    br_sha256_out(&h, sha);
    sync_path(dst);
    if (rc < 0 || used != f->bytes || memcmp(sha, f->sha256, 32)) return -1;
    /* read back */
    if (hash_file(NULL, dst, sha, 0) != (int64_t)f->bytes || memcmp(sha, f->sha256, 32)) return -1;
    progress(x, f->bytes * 2, f->bytes * 2);
    return 0;
}

/* dest.vjo-new into dest: the old file aside as dest.vjo-old, then removed. 0, or -1 with dest as it was. */
static int swap_in(const char *dest, const char *fresh)
{
    char old[PATH];
    int had = exists(dest);
    snprintf(old, sizeof(old), "%s.vjo-old", dest);
    if (had) {
        if (exists(old)) sceIoRemove(old); /* an earlier swap's: dest exists */
        if (sceIoRename(dest, old) < 0) return -1;
    }
    if (sceIoRename(fresh, dest) < 0) {
        if (had) sceIoRename(old, dest);
        return -1;
    }
    if (had) sceIoRemove(old);
    sync_path(dest);
    return 0;
}

/* ---------------- what the app is and what the card holds ---------------- */

/* The configuration HENkaku (henkaku 784e483 bootstrap.c:46-69) and h-encore, h-encore2 (c59a248
 * bootstrap/bootstrap.c:25-45, the same bytes) write when there is none: ux0's, and ur0's with the
 * recovery header. h-encore writes both when both are missing, as after a stop in ur0's swap window. */
#define STOCK_RECOVERY "# This file is used as an alternative if ux0:tai/config.txt is not found.\n"
#define STOCK_TEXT                                                                                             \
    "# For users plugins, you must refresh taiHEN from HENkaku Settings for\n# changes to take place.\n"      \
    "# For kernel plugins, you must reboot for changes to take place.\n*KERNEL\n"                              \
    "# henkaku.skprx is hard-coded to load and is not listed here\n*main\n# main is a special titleid for "   \
    "SceShell\nur0:tai/henkaku.suprx\n*NPXS10015\n# this is for modifying the version string\n"               \
    "ur0:tai/henkaku.suprx\n*NPXS10016\n# this is for modifying the version string in settings widget\n"      \
    "ur0:tai/henkaku.suprx\n"
static int stock_text(const char *t, size_t n)
{
    return (n == sizeof(STOCK_TEXT) - 1 && !memcmp(t, STOCK_TEXT, n)) ||
           (n == sizeof(STOCK_RECOVERY STOCK_TEXT) - 1 && !memcmp(t, STOCK_RECOVERY STOCK_TEXT, n));
}

/* A configuration the editor wrote: an install's result (it has our kernel line and an install leaves it). */
static int ours_text(const char *text, int len, int worker)
{
    VjoTaiReport r;
    return len > 0 && vjo_tai_has_kernel_line(text, (size_t)len) &&
           vjo_tai_edit(text, (size_t)len, VJO_TAI_INSTALL, worker, NULL, 0, cfg_check, sizeof(cfg_check), &r) >= 0 && !r.changed;
}

/* text (n bytes) is the editor's result of ref (an install's or an uninstall's), byte for byte, and that
 * result changes ref: the run (VJO_INSTALL_ACT_INSTALL or _UNINSTALL), or 0. A run writes a configuration
 * only when its edit changes it, so a .vjo-new equal to ref is no run's whole file: it is a stopped write
 * of a longer one (an install's that appends its sections, cut at ref's length; audit-overlay-one-vpk-
 * followups), never an uninstall's that changed nothing. */
static int edit_of(const char *ref, int len, const char *text, int n, int worker)
{
    for (int op = 0; op < 2; op++) {
        VjoTaiReport r;
        int m = vjo_tai_edit(ref, (size_t)len, op, worker, NULL, 0, cfg_check, sizeof(cfg_check), &r);
        if (m == n && r.changed && !memcmp(cfg_check, text, (size_t)n))
            return op == VJO_TAI_INSTALL ? VJO_INSTALL_ACT_INSTALL : VJO_INSTALL_ACT_UNINSTALL;
    }
    return 0;
}

/* .vjo-new is the swap's whole new text: the editor's result of what it was made of. With a .vjo-old
 * (the swap had started: config.txt was renamed to it) that is .vjo-old; without one the swap never
 * started, so config.txt (current, len bytes) is what it was made of, and a .vjo-new that is not exactly
 * its edit is a stopped write's (empty, or a prefix). With neither (a .vjo-old deleted by hand), only an
 * install's whole result is used. The run that wrote it (VJO_INSTALL_ACT_*), or 0. */
static char leftover_old[VJO_TAI_TEXT_MAX + 2], leftover_new[VJO_TAI_TEXT_MAX + 2];
static int new_is_result_of_old(const char *path, const char *current, int len, int worker)
{
    char a[PATH], b[PATH];
    snprintf(a, sizeof(a), "%s.vjo-old", path);
    snprintf(b, sizeof(b), "%s.vjo-new", path);
    int n = read_all(b, leftover_new, sizeof(leftover_new)), o = read_all(a, leftover_old, sizeof(leftover_old));
    if (n <= 0 || n > VJO_TAI_TEXT_MAX) return 0;
    if (o >= 0) return o <= VJO_TAI_TEXT_MAX ? edit_of(leftover_old, o, leftover_new, n, worker) : 0;
    if (current) return len >= 0 ? edit_of(current, len, leftover_new, n, worker) : 0;
    return ours_text(leftover_new, n, worker) ? VJO_INSTALL_ACT_INSTALL : 0;
}

/* A .vjo-new beside config.txt (current, len bytes) and no .vjo-old: the swap never started, so a run was
 * writing it from config.txt. Each run writes only an edit that changes the file, whole or, stopped, a
 * prefix of it: the run (VJO_INSTALL_ACT_*) is the one whose edit changes config.txt and starts with the
 * .vjo-new's bytes, when only one does; 0 when both could have (an empty file, a start both share). */
static int stopped_write_run(const char *path, const char *current, int len, int worker)
{
    char b[PATH];
    snprintf(b, sizeof(b), "%s.vjo-new", path);
    int n = read_all(b, leftover_new, sizeof(leftover_new)), run = 0, runs = 0;
    if (n < 0 || n > VJO_TAI_TEXT_MAX) return 0;
    for (int op = 0; op < 2; op++) {
        VjoTaiReport r;
        int m = vjo_tai_edit(current, (size_t)len, op, worker, NULL, 0, cfg_check, sizeof(cfg_check), &r);
        if (m >= 0 && r.changed && n <= m && !memcmp(cfg_check, leftover_new, (size_t)n)) {
            run = op == VJO_TAI_INSTALL ? VJO_INSTALL_ACT_INSTALL : VJO_INSTALL_ACT_UNINSTALL;
            runs++;
        }
    }
    return runs == 1 ? run : 0;
}

static const char *tai_path(int c) { return c ? VJO_TAI_UR0 : VJO_TAI_UX0; }

/* One line of text from *pos (its bytes up to its end: CRLF, CR or LF, without it); 0 at the end. */
static int next_line(const char *t, int len, int *pos, const char **start, int *n)
{
    if (*pos >= len) return 0;
    int i = *pos;
    while (i < len && t[i] != '\r' && t[i] != '\n') i++;
    *start = t + *pos;
    *n = i - *pos;
    if (i < len && t[i] == '\r') {
        i++;
        if (i < len && t[i] == '\n') i++; /* CRLF is one end */
    } else if (i < len) {
        i++; /* LF */
    }
    *pos = i;
    return 1;
}

/* The greedy walk: a line of old that new does not have at the same place is "-", one of new that old does
 * not have "+"; an old line found further on in new means lines were added before it, else it was removed.
 * Always the edit (applying its lines to old gives new), but not always the shortest: when a line the editor
 * removes or comments out has a copy further on, the user's lines in between show as added, then removed
 * (code audit note 1). vjo_install_diff's fallback past its bounds. */
static void walk_greedy(const char *old, int old_len, const char *neu, int new_len,
                        void (*emit)(void *ctx, char sign, const char *s, int n), void *ctx)
{
    int a = 0, b = 0;
    const char *ls, *ns;
    int ln, nn;
    int have_l = next_line(old, old_len, &a, &ls, &ln), have_n = next_line(neu, new_len, &b, &ns, &nn);
    while (have_l || have_n) {
        if (have_l && have_n && ln == nn && !memcmp(ls, ns, (size_t)ln)) {
            have_l = next_line(old, old_len, &a, &ls, &ln);
            have_n = next_line(neu, new_len, &b, &ns, &nn);
            continue;
        }
        int found = 0;
        if (have_l && have_n) {
            int p = b;
            const char *qs;
            int qn;
            while (!found && next_line(neu, new_len, &p, &qs, &qn)) found = qn == ln && !memcmp(qs, ls, (size_t)ln);
        }
        if (have_n && (found || !have_l)) {
            emit(ctx, '+', ns, nn);
            have_n = next_line(neu, new_len, &b, &ns, &nn);
        } else {
            emit(ctx, '-', ls, ln);
            have_l = next_line(old, old_len, &a, &ls, &ln);
        }
    }
}

/* The exact diff's room: the changed stretch (the lines between the texts' common first and last ones) of
 * each text, and its band (the fewest changed lines, + 1): every shortest diff keeps within it. */
#define DIFF_LINES 1024
#define DIFF_BAND 256
static struct {
    uint16_t at[2][DIFF_LINES], len[2][DIFF_LINES]; /* each line's start in its text and its length */
    unsigned char user[DIFF_LINES];                  /* an old line that is not the overlay's own */
    int16_t v[2 * DIFF_BAND + 1];                    /* Myers' furthest x per diagonal */
    uint32_t row[2][DIFF_BAND];                       /* the best (lines kept << 16 | user lines kept) of a row */
    unsigned char del[DIFF_LINES * DIFF_BAND / 8];    /* at a mismatch: removing the old line is as good */
} dw;

/* The lines of t[from, to) into dw (side 0 old, 1 new): their count, or -1 when they do not fit. */
static int diff_table(int side, const char *t, int from, int to)
{
    int pos = from, n = 0, len;
    const char *s;
    while (next_line(t, to, &pos, &s, &len)) {
        if (n >= DIFF_LINES || len > 0xFFFF) return -1;
        dw.at[side][n] = (uint16_t)(s - t);
        dw.len[side][n++] = (uint16_t)len;
    }
    return n;
}

static int diff_same(const char *old, const char *neu, int i, int j)
{
    return dw.len[0][i] == dw.len[1][j] && !memcmp(old + dw.at[0][i], neu + dw.at[1][j], dw.len[0][i]);
}

/* A line that names the overlay (its plugins, their commented-out copies, its installers' notes): the
 * editor's own lines, which a tie between two shortest diffs gives up first. */
static int names_overlay(const char *s, int n)
{
    for (int i = 0; i + 13 <= n; i++)
        if (!strncasecmp(s + i, "vitajpoverlay", 13)) return 1;
    return 0;
}

/* p starts a line of t (len bytes), or is its end. */
static int line_boundary(const char *t, int len, int p)
{
    return p == 0 || p == len || t[p - 1] == '\n' || (t[p - 1] == '\r' && t[p] != '\n');
}

/* The fewest changed lines between the tables' n old and m new lines (Myers' greedy O(ND) pass, which
 * keeps only the furthest point of each diagonal k = x - y), or -1 when it is over max (< DIFF_BAND). */
static int diff_distance(const char *old, const char *neu, int n, int m, int max)
{
    int16_t *v = dw.v + DIFF_BAND;
    v[1] = 0;
    for (int d = 0; d <= max; d++)
        for (int k = -d; k <= d; k += 2) {
            int x = k == -d || (k != d && v[k - 1] < v[k + 1]) ? v[k + 1] : v[k - 1] + 1, y = x - k;
            while (x < n && y < m && diff_same(old, neu, x, y)) { x++; y++; }
            v[k] = (int16_t)x;
            if (x >= n && y >= m) return d;
        }
    return -1;
}

void vjo_install_diff(const char *old, int old_len, const char *neu, int new_len,
                      void (*emit)(void *ctx, char sign, const char *s, int n), void *ctx)
{
    /* the common first lines, and the common last ones (the longest equal tail of bytes that starts a line in
     * both): the edit is between them */
    int a = 0, b = 0;
    for (;;) {
        int pa = a, pb = b, ln, nn;
        const char *ls, *ns;
        if (!next_line(old, old_len, &pa, &ls, &ln) || !next_line(neu, new_len, &pb, &ns, &nn) || ln != nn ||
            memcmp(ls, ns, (size_t)ln))
            break;
        a = pa;
        b = pb;
    }
    int ea = old_len, eb = new_len;
    while (ea > a && eb > b && old[ea - 1] == neu[eb - 1]) { ea--; eb--; }
    while (!line_boundary(old, old_len, ea) || !line_boundary(neu, new_len, eb)) { ea++; eb++; }
    int n = old_len <= 0x10000 && new_len <= 0x10000 ? diff_table(0, old, a, ea) : -1;
    int m = n >= 0 ? diff_table(1, neu, b, eb) : -1;
    int D = m >= 0 ? diff_distance(old, neu, n, m, DIFF_BAND - 1) : -1;
    if (D < 0) {
        walk_greedy(old + a, ea - a, neu + b, eb - b, emit, ctx);
        return;
    }
    int I = (D + m - n) / 2, R = (D - (m - n)) / 2, W = D + 1; /* additions and removals of every shortest diff */
    /* The fewest changed lines is the longest common subsequence of the lines. Among the shortest diffs the
     * one kept is the one that keeps the most lines not the overlay's own (a user's line is never shown as
     * removed and added back when the editor's own line can be), then removes before it adds. A shortest diff
     * removes R lines and adds I, so it keeps to the diagonals j - i in [-R, I]: row i of the table holds
     * j = i - R .. i + I, as t = j - i + R. Filled from the end:
     * best(i, j) = best(i + 1, j + 1) + the line when old's line i is new's line j (keeping an equal pair is
     * always as good), else the better of removing old's line i or adding new's line j. */
    for (int i = 0; i < n; i++) dw.user[i] = !names_overlay(old + dw.at[0][i], dw.len[0][i]);
    uint32_t *cur = dw.row[0], *next = dw.row[1];
    for (int i = n; i >= 0; i--) {
        int lo = i - R > 0 ? i - R : 0, hi = i + I < m ? i + I : m;
        for (int j = hi; j >= lo; j--) {
            int t = j - i + R;
            uint32_t v;
            if (i == n || j == m) {
                v = 0;
            } else if (diff_same(old, neu, i, j)) {
                v = next[t] + 0x10000u + dw.user[i];
            } else {
                /* (i + 1, j) is on diagonal t - 1 of the next row, (i, j + 1) on t + 1 of this one */
                uint32_t d = t > 0 ? next[t - 1] : 0, r = t + 1 < W ? cur[t + 1] : 0;
                int del = t > 0 && (t + 1 >= W || d >= r);
                size_t bit = (size_t)i * (size_t)W + (size_t)t;
                if (del) dw.del[bit >> 3] |= (unsigned char)(1u << (bit & 7));
                else dw.del[bit >> 3] &= (unsigned char)~(1u << (bit & 7));
                v = del ? d : r;
            }
            cur[t] = v;
        }
        uint32_t *swap = cur;
        cur = next;
        next = swap;
    }
    for (int i = 0, j = 0; i < n || j < m;) {
        if (i < n && j < m && diff_same(old, neu, i, j)) {
            i++;
            j++;
            continue;
        }
        size_t bit = (size_t)i * (size_t)W + (size_t)(j - i + R);
        if (j == m || (i < n && (dw.del[bit >> 3] >> (bit & 7) & 1))) {
            emit(ctx, '-', old + dw.at[0][i], dw.len[0][i]);
            i++;
        } else {
            emit(ctx, '+', neu + dw.at[1][j], dw.len[1][j]);
            j++;
        }
    }
}

typedef struct { VjoInstallConfig *g; int which; } DiffKeep;
static void keep_line(void *ctx, char sign, const char *s, int n)
{
    DiffKeep *k = ctx;
    VjoInstallConfig *g = k->g;
    if (g->diff_count[k->which] < VJO_INSTALL_DIFF)
        snprintf(g->diff[k->which][g->diff_count[k->which]++], sizeof(g->diff[0][0]), "%c %.*s", sign,
                 n > VJO_TAI_LINE_MAX ? VJO_TAI_LINE_MAX : n, s);
    else
        g->diff_more[k->which]++;
}

static void diff_lines(VjoInstallConfig *g, int which, const char *old, int old_len, const char *neu, int new_len)
{
    DiffKeep k = {g, which};
    g->diff_count[which] = g->diff_more[which] = 0;
    vjo_install_diff(old, old_len, neu, new_len, keep_line, &k);
}

static void sha_text(const char *text, size_t len, uint8_t out[32])
{
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, text, len);
    br_sha256_out(&h, out);
}

/* The newest config.txt.vjo-*.bak in path's folder that has no kernel line of ours ("" when none). */
static void before_overlay(const char *path, char *out, size_t cap)
{
    char dir[64], best[96] = "", file[PATH];
    SceIoDirent e;
    out[0] = '\0';
    snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(path, '/') - path), path);
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        size_t n = strlen(e.d_name);
        if (n < 20 || strncmp(e.d_name, "config.txt.vjo-", 15) || strcmp(e.d_name + n - 4, ".bak") ||
            !strcmp(e.d_name, "config.txt.vjo-bak") || n >= sizeof(best) || strcmp(e.d_name, best) <= 0)
            continue;
        snprintf(file, sizeof(file), "%s/%.*s", dir, (int)(sizeof(best) - 1), e.d_name);
        int m = read_all(file, cfg_check, sizeof(cfg_check));
        if (m >= 0 && m <= VJO_TAI_TEXT_MAX && !vjo_tai_has_kernel_line(cfg_check, (size_t)m)) {
            memcpy(best, e.d_name, n);
            best[n] = '\0';
        }
    }
    sceIoDclose(d);
    if (best[0]) snprintf(out, cap, "%.20s/%.*s", dir, (int)(cap > 22 ? cap - 22 : 0), best);
}

/* The local dictionaries list with plaintext.vjdict in it: 0.7.12's jmdict.vjdict replaced, else first;
 * 1 when it changed, 0 unchanged, -1 when it would hold more than 8 names (left as it is). */
static int dictionaries(const char *names, char *out, size_t cap)
{
    char list[512], *parts[16];
    int n = 0, has = 0, jm = -1;
    snprintf(list, sizeof(list), "%s", names);
    for (char *s = strtok(list, ","); s && n < 16; s = strtok(NULL, ",")) {
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        if (!*s) continue;
        if (!strcmp(s, "plaintext.vjdict")) has = 1;
        if (!strcmp(s, "jmdict.vjdict") && jm < 0) jm = n;
        parts[n++] = s;
    }
    if (has) return 0;
    size_t used = 0;
    out[0] = '\0';
    if (jm < 0) {
        if (n >= 8) return -1;
        used = (size_t)snprintf(out, cap, "plaintext.vjdict");
    }
    for (int i = 0; i < n && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%s%s", used ? ", " : "", i == jm ? "plaintext.vjdict" : parts[i]);
    return used < cap ? 1 : -1;
}

/* The next line of the plan's config.ini list (NULL past VJO_INSTALL_INI_CHANGES: it is counted). */
static char *ini_change(VjoInstallScan *s)
{
    if (s->config_change_count < VJO_INSTALL_INI_CHANGES) return s->config_changes[s->config_change_count++];
    s->config_change_more++;
    return NULL;
}
#define INI_CHANGE(s, ...)                                                                \
    do {                                                                                  \
        char *line_ = ini_change(s);                                                      \
        if (line_) snprintf(line_, sizeof((s)->config_changes[0]), __VA_ARGS__);          \
    } while (0)

/* The bytes of the UTF-8 BOMs a line starts with (none, one, or several). A BOM is no part of a line here: the
 * patches keep the file's one at offset 0, and a BOM in the middle of a hand-joined file moves onto the next line
 * when its own line goes (audit-pr180-b N1: such a line was listed with its value, and its setting named wrongly). */
static size_t ini_skip_bom(const char *s, size_t n)
{
    size_t b = 0;
    while (n - b >= 3 && !memcmp(s + b, "\xEF\xBB\xBF", 3)) b += 3;
    return b;
}

/* The setting a config.ini line sets, safe to show on the page and in log.txt: its name, never its value.
 * The name is the line's leading run of letters, digits and '_' (no setting's name has another character).
 * An API key's line is named by the key alone, whatever follows it: core/config.c removed_secret drops a
 * line that starts with one of these names, so the rest of such a line may be the key itself. 0 for a
 * line that names nothing (blank, a comment). */
static size_t ini_line_name(const char *line, size_t n, char *out, size_t cap)
{
    static const char *const secrets[] = {"jpdb_api_key", "jiten_api_key", "api_key"}; /* removed_secret's */
    size_t b = vjo_config_line_start(line, n), e; /* where the parser reads a name: after spaces, CRs and BOMs */
    for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); i++) {
        size_t len = strlen(secrets[i]);
        if (n - b >= len && !strncasecmp(line + b, secrets[i], len)) {
            snprintf(out, cap, "%s", secrets[i]);
            return len;
        }
    }
    for (e = b; e < n; e++) {
        char c = line[e];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) break;
    }
    if (e == b || e - b >= cap) return 0;
    memcpy(out, line + b, e - b);
    out[e - b] = '\0';
    return e - b;
}

/* Whether text (len bytes, LF lines) has a line equal to the n bytes at line (a CR before its LF ignored, and
 * either line's leading BOMs: ini_skip_bom). */
static int ini_has_line(const char *text, size_t len, const char *line, size_t n)
{
    size_t lb = ini_skip_bom(line, n);
    line += lb;
    n -= lb;
    for (size_t q = 0; q < len;) {
        size_t qe = q;
        while (qe < len && text[qe] != '\n') qe++;
        size_t ql = qe - q;
        if (ql && text[qe - 1] == '\r') ql--;
        size_t tb = ini_skip_bom(text + q, ql);
        if (ql - tb == n && !memcmp(text + q + tb, line, n)) return 1;
        q = qe + 1;
    }
    return 0;
}

/* One bit per line of ini_new (counted after a BOM): set when the old text has that line. The first pass sets
 * them, the second reads them, so a line that goes costs one walk of the new text, not a scan of the old text
 * for each same-named new line (audit-pr177 N2: that was cubic, 14.8 s on the host for a crafted 23 KB file). */
static unsigned char ini_new_kept[(VJO_CONFIG_TEXT_MAX + 1) / 8 + 1];
typedef char vjo_ini_new_kept_covers_ini_new[sizeof(ini_new_kept) * 8 >= sizeof(ini_new) ? 1 : -1]; /* a bit a line */

/* config.ini's new text for an install (into ini_new): 2 written from the package's template (there is
 * neither config.ini nor its backup, which the app's load reads when config.ini is missing), 1 patched,
 * 3 written from its backup, 0 unchanged, -1 unreadable (s->reason). The lines it changes go into
 * s->config_changes.
 * patch_existing: a first install, or an install of another build than installed.txt's, patches an
 * existing config.ini (the package's set lines, the paths, the dictionary list, the app's own save).
 * Over the build installed.txt names (Check and repair, the same VPK again) an existing config.ini is
 * the user's: a later choice (card mining OFF, log_file = off, another dictionary list) is no damage,
 * so it is left as it is; a missing one is written from its backup config.ini.bak (3: a save that
 * stopped between its two renames left only the backup, which the app reads and the plugins do not,
 * code audit note 6), else from the template. */
static int plan_config_ini(VjoInstallScan *s, int patch_existing)
{
    VjoArena a;
    VjoConfig base, cfg;
    char *old = NULL, value[512];
    size_t bytes = 0;
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
    int rc = vjo_preferences_text(&a, &old, &bytes);
    s->config_change_count = s->config_change_more = 0;
    if (!patch_existing && rc == 1) {
        /* as the app's next save writes it: the backup's settings, a removed feature's lines and the API keys
         * gone (vjo_preferences_publish_text keeps no key in either file) */
        vjo_config_defaults(&base);
        vjo_config_parse(&base, old, bytes);
        int n = vjo_config_patch_preferences(old, bytes, base.vocr_tier, base.japanese_only_selection, base.highlight_mode,
                                             base.local_dictionary_results, ini_new, sizeof(ini_new));
        if (n < 0) return 0; /* too large for the app's save as well: the app's load still reads the backup */
        ini_new_len = n;
        INI_CHANGE(s, "written again from config.ini.bak");
        return 3;
    }
    if (!patch_existing && rc != 2) return 0;
    if (rc < 0) {
        snprintf(s->reason, sizeof(s->reason), "config.ini cannot be read: repair or delete it in VitaShell first.");
        return -1;
    }
    if (rc == 2) {
        char src[PATH];
        int t = vjo_payload_source(VJO_PAYLOAD_COUNT - 1, src, sizeof(src)) < 0 ? -1
              : read_all(src, template_text, sizeof(template_text));
        uint8_t sha[32];
        if (t >= 0) sha_text(template_text, (size_t)t, sha);
        if (t < 0 || (uint64_t)t != s->payload.file[VJO_PAYLOAD_COUNT - 1].bytes || t > VJO_CONFIG_TEXT_MAX ||
            memcmp(sha, s->payload.file[VJO_PAYLOAD_COUNT - 1].sha256, 32)) {
            snprintf(s->reason, sizeof(s->reason), "the package's config.ini template is missing or damaged: install the "
                     "VPK again.");
            return -1;
        }
        memcpy(ini_new, template_text, (size_t)t + 1);
        ini_new_len = t;
        INI_CHANGE(s, "written from the package's template");
        return 2;
    }
    /* the app's own save first (the four owned lines; an older build's vocr_model becomes its size; a
     * removed feature's lines and the API keys go), then the install's own keys */
    vjo_config_defaults(&base);
    vjo_config_parse(&base, old, bytes);
    int n = vjo_config_patch_preferences(old, bytes, base.vocr_tier, base.japanese_only_selection, base.highlight_mode,
                                         base.local_dictionary_results, ini_new, sizeof(ini_new));
    if (n < 0) {
        snprintf(s->reason, sizeof(s->reason), "config.ini is too large to update: shorten it in VitaShell first.");
        return -1;
    }
    ini_new_len = n;
    /* the removed settings the plugins and the app ignore without a warning go, as tools/migrate_config.py
     * REMOVED drops them for install-vita.sh (the others, API keys, Anki's and the engine choice's, the
     * app's own save above drops) */
    {
        static const char *const gone[] = {"frequency_filter", "font_size", "hw_jpeg", "combo_delay_ms", "hachidori_host"};
        int w = 0;
        for (int pos = 0; pos < ini_new_len;) {
            int e = pos;
            while (e < ini_new_len && ini_new[e] != '\n') e++;
            if (e < ini_new_len) e++; /* the line with its end */
            int b = pos;
            while (b < e && (ini_new[b] == ' ' || ini_new[b] == '\t')) b++;
            int drop = 0;
            for (unsigned g = 0; g < sizeof(gone) / sizeof(gone[0]) && !drop; g++) {
                size_t len = strlen(gone[g]);
                int v = b + (int)len;
                if (v > e || strncasecmp(ini_new + b, gone[g], len)) continue;
                while (v < e && (ini_new[v] == ' ' || ini_new[v] == '\t')) v++;
                drop = v < e && ini_new[v] == '=';
            }
            if (!drop) { memmove(ini_new + w, ini_new + pos, (size_t)(e - pos)); w += e - pos; }
            pos = e;
        }
        ini_new_len = w;
        ini_new[w] = '\0';
        /* each line dropped here is named below, with every other line that only goes */
    }
    const char *keys[6], *values[6];
    int k = 0;
    keys[k] = "local_dictionary_dir"; values[k++] = "ux0:data/VitaJPOverlay/dictionaries";
    keys[k] = "ocr_model_dir"; values[k++] = "ux0:data/VitaJPOverlay/ocr";
    keys[k] = "text_source"; values[k++] = "ocr";
    int d = dictionaries(base.local_dictionaries, value, sizeof(value));
    if (d > 0) { keys[k] = "local_dictionaries"; values[k++] = value; }
    else if (d < 0)
        INI_CHANGE(s, "local_dictionaries has 8 names: add plaintext.vjdict to it yourself");
    int log_set = 0;
    for (int i = 0; i < s->payload.sets && k < 6; i++) {
        keys[k] = s->payload.set[i].key; values[k++] = s->payload.set[i].value;
        log_set |= !strcmp(s->payload.set[i].key, "log_file");
    }
    /* the debug build is the one asked for when a full log is needed (docs/release-variants.md) */
    if (VJO_DEBUG && !log_set && k < 6) { keys[k] = "log_file"; values[k++] = "on"; }
    /* each key only when it adds no config warning (a value the overlay would turn off) */
    vjo_config_defaults(&cfg);
    vjo_config_parse(&cfg, ini_new, (size_t)ini_new_len);
    int warnings = cfg.n_warnings;
    for (int i = 0; i < k; i++) {
        const char *one_key[1] = {keys[i]}, *one_value[1] = {values[i]};
        int m = vjo_config_patch_keys(ini_new, (size_t)ini_new_len, one_key, one_value, 1, ini_step, sizeof(ini_step));
        if (m < 0) continue;
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, ini_step, (size_t)m);
        if (cfg.n_warnings > warnings) {
            INI_CHANGE(s, "%s left as it is (the new value would be refused)", keys[i]);
            continue;
        }
        memcpy(ini_new, ini_step, (size_t)m + 1);
        ini_new_len = m;
    }
    if ((size_t)ini_new_len == bytes && !memcmp(ini_new, old, bytes)) return 0;
    /* both texts' lines after the old text's UTF-8 BOM: the patches keep it at offset 0, in front of whatever line
     * comes first (core/config.c, settings/preferences.c), so it is no part of a line here (audit-pr177 N1). A BOM
     * the new text starts with and the old one does not was a later line's (every line before it went): it is not
     * stripped here on one side only (audit-pr180 B1: the unchanged line was then listed with its value); the line
     * comparisons and names skip a line's leading BOMs on both sides (ini_skip_bom), whichever line it is on now. */
    size_t ob = bytes >= 3 && !memcmp(old, "\xEF\xBB\xBF", 3) ? 3 : 0;
    int nb = ob && ini_new_len >= 3 && !memcmp(ini_new, "\xEF\xBB\xBF", 3) ? 3 : 0;
    const char *ot = old + ob;
    size_t on = bytes - ob;
    /* the changed lines: those of the new text that the old one does not have (no API key: they only go); the
     * others are marked in ini_new_kept */
    memset(ini_new_kept, 0, sizeof(ini_new_kept));
    for (int pos = nb, line = 0; pos < ini_new_len; line++) {
        int e = pos;
        while (e < ini_new_len && ini_new[e] != '\n') e++;
        int len = e - pos;
        if (len && ini_new[e - 1] == '\r') len--; /* a CRLF line the patch kept: one line, as in the old text */
        if (len > 0 && ini_new[pos] != ';' && ini_new[pos] != '#') {
            if (ini_has_line(ot, on, ini_new + pos, (size_t)len))
                ini_new_kept[line >> 3] |= (unsigned char)(1u << (line & 7));
            else
                INI_CHANGE(s, "%.*s", len > 110 ? 110 : len, ini_new + pos);
        }
        pos = e + 1;
    }
    /* the lines that only go (a removed setting, an API key, a second copy of a setting): each named by its
     * setting, never its value (test.4's dry run: the plan listed nothing while ocr_backend and log_host went).
     * A setting the new text sets on a line of its own is changed, and that line is listed above. A named line
     * of the new text is always one the first pass checked (it starts with a name, not ';' or '#'). */
    for (size_t q = 0; q < on;) {
        size_t qe = q;
        while (qe < on && ot[qe] != '\n') qe++;
        size_t ql = qe - q;
        if (ql && ot[qe - 1] == '\r') ql--;
        char name[48], other[48];
        if (ini_line_name(ot + q, ql, name, sizeof(name)) &&
            !ini_has_line(ini_new + nb, (size_t)(ini_new_len - nb), ot + q, ql)) {
            int kept = 0, changed = 0;
            /* any same-named new line the old text lacks decides it: the setting changed, listed above (an OR, as
             * #177's; the stop at the first is a speed-up only) */
            for (int pos = nb, line = 0; pos < ini_new_len && !changed; line++) {
                int e = pos;
                while (e < ini_new_len && ini_new[e] != '\n') e++;
                if (ini_line_name(ini_new + pos, (size_t)(e - pos), other, sizeof(other)) && !strcasecmp(name, other)) {
                    kept = 1;
                    changed |= !(ini_new_kept[line >> 3] & (1u << (line & 7)));
                }
                pos = e + 1;
            }
            if (!changed) INI_CHANGE(s, kept ? "%s: a repeated line goes" : "%s: a removed setting, goes", name);
        }
        q = qe + 1;
    }
    return 1;
}

void vjo_install_scan(VjoInstallScan *s, const VjoInstallBuild *build)
{
    static char manifest[VJO_PAYLOAD_MANIFEST_MAX + 2];
    char why[320], record[VJO_APP_STAMP_MAX], path[PATH];
    SceDateTime t;
    memset(s, 0, sizeof(*s));
    s->build = *build;
    s->dict_index = -1;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(s->stamp, sizeof(s->stamp), "%04u%02u%02u-%02u%02u%02u", t.year % 10000u, t.month % 100u, t.day % 100u,
             t.hour % 100u, t.minute % 100u, t.second % 100u);
    s->unsafe_off = sceIoDevctl("ux0:", 0x3001, NULL, 0, NULL, 0) == SAFE_MODE;

    /* the payload, and the app's own stamp (the payload's build must be this app's eboot) */
    const char *own = first_line(VJO_APP_STAMP_OWN, s->own_stamp, sizeof(s->own_stamp));
    if (!own) own = first_line(VJO_PAYLOAD_APP_DIR "/vjo_app.txt", s->own_stamp, sizeof(s->own_stamp));
    int damaged = 0;
    int m = read_all(VJO_PAYLOAD_MANIFEST, manifest, sizeof(manifest));
    if (m >= 0) {
        if (vjo_payload_parse(manifest, (size_t)m, build, &s->payload, why, sizeof(why)) == 0) {
            const char *b = own ? strstr(own, "build=") : NULL;
            if (!b || strncmp(b + 6, s->payload.build, 16)) {
                damaged = 1;
                snprintf(s->reason, sizeof(s->reason), "the package is damaged (its plugins are not this app's build): "
                         "install the VPK again in VitaShell.");
            } else {
                s->has_payload = 1;
            }
        } else {
            damaged = 1;
            snprintf(s->reason, sizeof(s->reason), "%.300s.", why);
        }
    } else if (m != MISSING) {
        damaged = 1;
        snprintf(s->reason, sizeof(s->reason), "this app's files cannot be read (0x%08X).", (unsigned)m);
    }

    /* the configurations */
    int configs = 0, refused = 0, loaded = 0, kernel_lines = 0, need_lines = 0, interrupted = 0;
    for (int c = 0; c < 2; c++) {
        VjoInstallConfig *g = &s->cfg[c];
        g->path = tai_path(c);
        int n = read_all(g->path, cfg_text[c], sizeof(cfg_text[c]));
        if (n < 0) {
            snprintf(path, sizeof(path), "%s.vjo-new", g->path);
            g->interrupted = exists(path) ? 2 : 0;
            snprintf(path, sizeof(path), "%s.vjo-old", g->path);
            if (!g->interrupted && exists(path)) g->interrupted = 1;
            interrupted |= g->interrupted != 0;
            cfg_len[c] = -1;
            continue;
        }
        g->exists = 1;
        configs++;
        cfg_len[c] = n;
        /* a stopped swap's file beside it: never removed (h-encore rewrites the stock text when both are
         * missing, so a stop in ur0's window can come back as a config.txt beside its leftovers, C4) */
        snprintf(path, sizeof(path), "%s.vjo-old", g->path);
        g->leftover = exists(path) ? 1 : 0;
        snprintf(path, sizeof(path), "%s.vjo-new", g->path);
        g->leftover |= exists(path) ? 2 : 0;
        sha_text(cfg_text[c], (size_t)n, g->sha256);
        g->has_kernel = vjo_tai_has_kernel_line(cfg_text[c], (size_t)n);
        VjoTaiLoads k, sh;
        vjo_tai_loads(cfg_text[c], "KERNEL", &k);
        vjo_tai_loads(cfg_text[c], "main", &sh);
        g->loads_ours = k.kernel > 0 && sh.shell > 0;
        loaded |= g->loads_ours;
        kernel_lines |= g->has_kernel;
        int rc = vjo_tai_edit(cfg_text[c], (size_t)n, VJO_TAI_INSTALL, build->worker, NULL, 0, cfg_new[c],
                              sizeof(cfg_new[c]), &g->install);
        if (rc < 0) {
            g->refused = 1;
            refused = 1;
            snprintf(g->why, sizeof(g->why), "%s", g->install.why);
            cfg_new_len[c] = -1;
        } else {
            cfg_new_len[c] = rc;
            g->install_changes = g->install.changed;
            need_lines |= g->install_changes;
        }
        if (cfg_new_len[c] >= 0 && g->install_changes) diff_lines(g, 0, cfg_text[c], n, cfg_new[c], cfg_new_len[c]);
        VjoTaiReport u;
        rc = vjo_tai_edit(cfg_text[c], (size_t)n, VJO_TAI_UNINSTALL, build->worker, NULL, 0, cfg_check, sizeof(cfg_check), &u);
        g->uninstall_changes = rc >= 0 && u.changed;
        if (g->uninstall_changes) diff_lines(g, 1, cfg_text[c], n, cfg_check, rc);
        before_overlay(g->path, s->before_overlay[c], sizeof(s->before_overlay[c]));
        if (g->leftover) {
            interrupted = 1;
            g->now_kind = stock_text(cfg_text[c], (size_t)n) ? VJO_TAI_NOW_STOCK
                        : ours_text(cfg_text[c], n, build->worker) ? VJO_TAI_NOW_OURS : VJO_TAI_NOW_OTHER;
        }
    }
    for (int c = 0; c < 2; c++) {
        VjoInstallConfig *g = &s->cfg[c];
        if (!g->interrupted && !g->leftover) continue;
        snprintf(path, sizeof(path), "%s.vjo-old", g->path);
        g->swap_files = exists(path) ? 1 : 0;
        snprintf(path, sizeof(path), "%s.vjo-new", g->path);
        g->swap_files |= exists(path) ? 2 : 0;
        int run = (g->swap_files & 2) ? new_is_result_of_old(g->path, g->exists ? cfg_text[c] : NULL, cfg_len[c],
                                                              build->worker) : 0;
        g->use_new = run != 0;
        g->stopped_run = run ? run : (g->swap_files == 2 && g->exists)
                       ? stopped_write_run(g->path, cfg_text[c], cfg_len[c], build->worker) : 0;
        g->finish_ok = g->use_new || (g->swap_files & 1);
    }
    if (s->cfg[1].exists) vjo_tai_storage_plugin(cfg_text[1], (size_t)cfg_len[1], s->storage, sizeof(s->storage));

    /* the files (plugins hashed, data by size) */
    s->files_ok = s->plugins_ok = 1;
    for (int i = 0; s->has_payload && i < VJO_PAYLOAD_COUNT; i++) {
        const VjoPayloadFile *f = &s->payload.file[i];
        if (f->kind == VJO_PAYLOAD_TEMPLATE) { s->file_ok[i] = 1; continue; }
        int64_t size = size_of(f->dest);
        int ok = size >= 0 && (uint64_t)size == f->bytes;
        if (ok && i >= VJO_PAYLOAD_KERNEL_INDEX && i < VJO_PAYLOAD_KERNEL_INDEX + VJO_PAYLOAD_PLUGINS) {
            uint8_t sha[32];
            ok = hash_file(NULL, f->dest, sha, 0) == size && !memcmp(sha, f->sha256, 32);
            if (!ok) s->plugins_ok = 0;
        }
        s->file_ok[i] = (unsigned char)ok;
        s->files_ok &= ok;
        if (f->kind == VJO_PAYLOAD_MOVE) {
            s->dict_index = i;
            char src[PATH];
            s->dict_in_app = vjo_payload_source(i, src, sizeof(src)) == 0 && exists(src);
        }
    }

    /* the records */
    s->record_ok = own && first_line(VJO_APP_STAMP_PACKAGE, record, sizeof(record)) && !strcmp(record, own);
    {
        char text[256];
        int n = read_all(VJO_INSTALLED_RECORD, text, sizeof(text));
        s->has_installed = n > 0 && vjo_installed_parse(text, (size_t)n, &s->installed) == 0;
    }
    int same_build = s->has_payload && s->has_installed &&
                     !memcmp(s->installed.manifest_sha256, s->payload.manifest_sha256, 32);

    /* config.ini */
    if (s->has_payload && !interrupted && !damaged && !s->unsafe_off) s->config_ini = plan_config_ini(s, !same_build);

    /* the state */
    const char *v = s->payload.version;
    if (interrupted) {
        s->state = VJO_INSTALL_INTERRUPTED;
        /* Put it back: every configuration with a stopped swap's files has a whole file to put back.
         * Keep the current one: every one of them has a config.txt (a missing one is never kept missing). */
        int finish = 1, keep = 1;
        for (int k = 0; k < 2; k++)
            if (s->cfg[k].interrupted || s->cfg[k].leftover) {
                finish &= s->cfg[k].finish_ok;
                keep &= s->cfg[k].exists;
            }
        s->actions = (finish ? VJO_INSTALL_ACT_FINISH : 0) | (keep ? VJO_INSTALL_ACT_KEEP : 0);
        snprintf(s->title, sizeof(s->title), "An install was interrupted");
        int c = s->cfg[0].interrupted || s->cfg[0].leftover ? 0 : 1;
        const VjoInstallConfig *g = &s->cfg[c];
        if (g->interrupted && g->finish_ok)
            snprintf(s->reason, sizeof(s->reason), "%s is missing: an install or uninstall stopped between two renames. "
                     "Put it back here, then restart the Vita.", g->path);
        else if (g->interrupted)
            snprintf(s->reason, sizeof(s->reason), "%s is missing, and its config.txt.vjo-new is not a configuration "
                     "this app wrote whole: in VitaShell copy its newest dated backup (config.txt.vjo-*.bak) to "
                     "config.txt before you restart the Vita.", g->path);
        else if (!(g->swap_files & 1)) {
            /* the run that was writing it, when the file tells (code audit note 4: an uninstall's too) */
            const char *whose = g->stopped_run == VJO_INSTALL_ACT_INSTALL ? "install's"
                              : g->stopped_run == VJO_INSTALL_ACT_UNINSTALL ? "uninstall's" : "install's or uninstall's";
            const char *again = g->stopped_run == VJO_INSTALL_ACT_INSTALL ? "Install"
                              : g->stopped_run == VJO_INSTALL_ACT_UNINSTALL ? "Uninstall" : "Install or Uninstall";
            snprintf(s->reason, sizeof(s->reason), "%s has a stopped %s config.txt.vjo-new beside it: it stopped "
                     "before it changed config.txt, which is as it was. Keep it (the .vjo-new is kept as a dated "
                     "backup), then choose %s again.%s", g->path, whose, again,
                     g->use_new ? " Or put the new one in place: it is this app's whole edit of the current one." : "");
        } else
            snprintf(s->reason, sizeof(s->reason), "%s has a stopped swap's %s beside it, and is %s now. Put it back "
                     "(the current one is kept as a dated backup), or keep the current one (the others are kept as "
                     "dated backups).", g->path, g->leftover == 3 ? "config.txt.vjo-old and .vjo-new"
                     : g->leftover == 2 ? "config.txt.vjo-new" : "config.txt.vjo-old",
                     g->now_kind == VJO_TAI_NOW_STOCK ? "HENkaku's default (h-encore writes it when there is none)"
                     : g->now_kind == VJO_TAI_NOW_OURS ? "the overlay's" : "another configuration");
        if (s->unsafe_off) {
            size_t used = strlen(s->reason);
            if (g->interrupted && g->finish_ok)
                snprintf(s->reason + used, sizeof(s->reason) - used, " This app cannot write ur0:tai now (Enable Unsafe "
                         "Homebrew is off): turn it on and open Overlay again, or in VitaShell rename %s.%s to config.txt.",
                         g->path, g->use_new ? "vjo-new" : "vjo-old");
            else
                snprintf(s->reason + used, sizeof(s->reason) - used, " This app cannot write ur0:tai now (Enable Unsafe "
                         "Homebrew is off): turn it on and open Overlay again.");
            s->actions = 0;
        }
        return;
    }
    if (s->unsafe_off) {
        s->state = VJO_INSTALL_CANNOT;
        snprintf(s->title, sizeof(s->title), "Cannot install the overlay yet");
        snprintf(s->reason, sizeof(s->reason), "Turn on Settings > HENkaku Settings > Enable Unsafe Homebrew, then open "
                 "Overlay again: without it this app cannot write ur0:tai.");
        return;
    }
    int can_uninstall = (s->cfg[0].uninstall_changes || s->cfg[1].uninstall_changes) ? VJO_INSTALL_ACT_UNINSTALL : 0;
    if (!configs) {
        s->state = VJO_INSTALL_CANNOT;
        snprintf(s->title, sizeof(s->title), "Cannot install the overlay");
        snprintf(s->reason, sizeof(s->reason), "taiHEN's config.txt was not found in ur0:tai or ux0:tai: is HENkaku "
                 "installed? This app never creates one.");
        return;
    }
    if (!s->has_payload && !damaged) {
        s->state = VJO_INSTALL_NO_PAYLOAD;
        s->actions = can_uninstall;
        snprintf(s->title, sizeof(s->title), "This app carries no overlay");
        snprintf(s->reason, sizeof(s->reason), "This is the release zip's Overlay app: install-vita.sh installs its "
                 "plugins. To install from the Vita alone, install VitaJPOverlay-<version>.vpk from the release.");
        return;
    }
    if (damaged || refused || s->config_ini < 0) {
        s->state = VJO_INSTALL_CANNOT;
        s->actions = can_uninstall;
        snprintf(s->title, sizeof(s->title), "Cannot install the overlay");
        if (!damaged && refused) {
            int c = s->cfg[0].refused ? 0 : 1;
            snprintf(s->reason, sizeof(s->reason), "%s: %s", s->cfg[c].path, s->cfg[c].why);
        }
        return;
    }
    int dict_leftover = 0;
    if (s->dict_index >= 0) {
        char left[PATH];
        uint64_t want = s->payload.file[s->dict_index].bytes;
        snprintf(left, sizeof(left), "%s.vjo-new", s->payload.file[s->dict_index].dest);
        dict_leftover = size_of(left) == (int64_t)want;
        snprintf(left, sizeof(left), "%s.vjo-old", s->payload.file[s->dict_index].dest);
        dict_leftover |= size_of(left) == (int64_t)want;
    }
    if (s->dict_index >= 0 && !s->file_ok[s->dict_index] && !s->dict_in_app && !dict_leftover) {
        s->state = VJO_INSTALL_CANNOT;
        s->actions = can_uninstall;
        snprintf(s->title, sizeof(s->title), "Cannot install the overlay");
        snprintf(s->reason, sizeof(s->reason), "The dictionary is missing, and this app no longer has its copy (an "
                 "earlier install moved it): install the VPK again in VitaShell, then open Overlay.");
        return;
    }
    s->actions = VJO_INSTALL_ACT_INSTALL | can_uninstall;
    if (!loaded && !kernel_lines) {
        s->state = VJO_INSTALL_NOT_INSTALLED;
        snprintf(s->title, sizeof(s->title), "Install the overlay (v%s%s)", v, strcmp(s->payload.variant, "debug") ? "" : ", debug build");
    } else if (!s->has_installed || !same_build) {
        s->state = VJO_INSTALL_UPDATE;
        int cmp = s->has_installed ? vjo_version_compare(v, s->installed.version) : 1;
        if (!s->has_installed)
            snprintf(s->title, sizeof(s->title), "Update the overlay to v%s (the installed one has no version record)", v);
        else if (cmp > 0)
            snprintf(s->title, sizeof(s->title), "Update the overlay from v%s to v%s", s->installed.version, v);
        else if (cmp < 0)
            snprintf(s->title, sizeof(s->title), "Go back from v%s to this app's v%s", s->installed.version, v);
        else if (strcmp(s->installed.variant, s->payload.variant))
            snprintf(s->title, sizeof(s->title), "Install the %s build of v%s (the %s build is installed)", s->payload.variant,
                     v, s->installed.variant);
        else
            snprintf(s->title, sizeof(s->title), "Reinstall v%s (another build of it is installed)", v);
    } else if (!s->record_ok || !s->plugins_ok || !s->files_ok || need_lines) {
        /* this build (installed.txt), with a plugin that is not its own, the plugins' record of another app
         * build, a data file missing or a line (code audit note 4: no longer "another build");
         * config.ini is the user's here (plan_config_ini) */
        s->state = VJO_INSTALL_REPAIR;
        snprintf(s->title, sizeof(s->title), "Repair the overlay (v%s)", v);
    } else {
        s->state = VJO_INSTALL_INSTALLED;
        snprintf(s->title, sizeof(s->title), "The overlay is installed (v%s%s)", v,
                 strcmp(s->payload.variant, "debug") ? "" : ", debug build");
    }
}

/* The dated backup name a run would use for path now: config.txt.vjo-<stamp>.bak, else -2, -3... */
static void backup_name(const char *path, const char *stamp, char *out, size_t cap)
{
    char base[PATH - 16];
    snprintf(base, sizeof(base), "%s.vjo-%s", path, stamp);
    snprintf(out, cap, "%s.bak", base);
    for (int k = 2; exists(out) && k < 100; k++) snprintf(out, cap, "%s-%d.bak", base, k);
}

/* ---------------- the plans ---------------- */

/* A plan's line, written when it fits (n < max); the count goes on either way, so that a plan knows how
 * many lines it has. */
static int add(char lines[][VJO_INSTALL_LINE], int n, int max, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int add(char lines[][VJO_INSTALL_LINE], int n, int max, const char *fmt, ...)
{
    va_list ap;
    if (n < max) {
        va_start(ap, fmt);
        vsnprintf(lines[n], VJO_INSTALL_LINE, fmt, ap);
        va_end(ap);
    }
    return n + 1;
}

/* What an edit of one configuration does, in a few words. */
/* Every line the swap changed (the plan shows VJO_INSTALL_DIFF of them per file), in log.txt. */
static void log_line(void *ctx, char sign, const char *s, int n)
{
    vjo_log("install: %s: %c %.*s", (const char *)ctx, sign, n > VJO_TAI_LINE_MAX ? VJO_TAI_LINE_MAX : n, s);
}
static void log_diff(const char *path, const char *old, int old_len, const char *neu, int new_len)
{
    vjo_install_diff(old, old_len, neu, new_len, log_line, (void *)path);
}

/* The configuration's exact changed lines, one plan line each, as they stand in the file ("+" added,
 * "-" removed; a commented-out line shows as its old text removed and the commented one added): the
 * first cap of them, the rest counted. */
static int add_diff(char lines[][VJO_INSTALL_LINE], int n, int max, const VjoInstallConfig *g, int which, int cap)
{
    int shown = g->diff_count[which] < cap ? g->diff_count[which] : cap;
    int more = g->diff_count[which] - shown + g->diff_more[which];
    for (int i = 0; i < shown; i++) n = add(lines, n, max, "   %s", g->diff[which][i]);
    if (more) n = add(lines, n, max, "   and %d more line%s (log.txt has every one).", more, more == 1 ? "" : "s");
    return n;
}

static void edit_words(const VjoTaiReport *r, int install, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    if (install && r->added)
        used += (size_t)snprintf(out + used, cap - used, "add the kernel line under *KERNEL and the Shell line under *main");
    if (r->removed_sections[0] && used < cap)
        used += (size_t)snprintf(out + used, cap - used, "%sremove the OCR worker's lines from %s", used ? "; " : "",
                                 r->removed_sections);
    else if (!install && r->removed && used < cap)
        used += (size_t)snprintf(out + used, cap - used, "%sremove the overlay's %d line%s", used ? "; " : "", r->removed,
                                 r->removed == 1 ? "" : "s");
    if (r->commented && used < cap)
        used += (size_t)snprintf(out + used, cap - used, "%scomment out %d old line%s", used ? "; " : "", r->commented,
                                 r->commented == 1 ? "" : "s");
    if (r->bom_hides_section && used < cap)
        snprintf(out + used, cap - used, " (note: a byte-order mark hides its first section from taiHEN)");
}

/* taiHEN reads ux0's when it is there at its start, else ur0's (plugin.c:73-83); with a storage plugin
 * in ur0's *KERNEL (an SD2Vita), ux0 becomes the SD card only once it ran, so ur0's is read. */
static const char *reads_note(const VjoInstallScan *s, int c)
{
    int ur0_read = s->storage[0] || !s->cfg[0].exists;
    return (c == 1) == ur0_read ? " (taiHEN reads this one at boot)" : "";
}

static double mb(uint64_t bytes) { return (double)bytes / 1000000.0; }

static int install_plan(const VjoInstallScan *s, char lines[][VJO_INSTALL_LINE], int max, int dcap, int icap, int *need);

int vjo_install_plan(const VjoInstallScan *s, int action, char lines[][VJO_INSTALL_LINE], int max)
{
    int n = 0;
    if (action == VJO_INSTALL_ACT_FINISH || action == VJO_INSTALL_ACT_KEEP) {
        for (int c = 0; c < 2; c++) {
            const VjoInstallConfig *g = &s->cfg[c];
            if (!g->interrupted && !g->leftover) continue;
            if (action == VJO_INSTALL_ACT_KEEP) {
                if (g->leftover)
                    n = add(lines, n, max, "%s stays as it is; %s kept as config.txt.vjo-%s-%s.bak.", g->path,
                            g->swap_files == 3 ? "its config.txt.vjo-old and .vjo-new are" : g->swap_files == 2
                            ? "its config.txt.vjo-new is" : "its config.txt.vjo-old is", s->stamp,
                            g->swap_files == 3 ? "old.bak and -new" : g->swap_files == 2 ? "new" : "old");
                continue;
            }
            if (g->leftover)
                n = add(lines, n, max, "%s: the current one is kept as config.txt.vjo-%s.bak, then %s becomes config.txt.",
                        g->path, s->stamp, !g->use_new ? "config.txt.vjo-old (the configuration from before the stop)"
                        : g->swap_files & 1 ? g->stopped_run == VJO_INSTALL_ACT_UNINSTALL
                        ? "config.txt.vjo-new (the stopped uninstall's new configuration, checked against config.txt.vjo-old)"
                        : "config.txt.vjo-new (the stopped install's new configuration, checked against config.txt.vjo-old)"
                        : "config.txt.vjo-new (this app's whole edit of the current one, checked byte for byte)");
            else
                n = add(lines, n, max, g->use_new
                        ? "%s: config.txt.vjo-new, the new configuration (written and checked before the stop), becomes config.txt."
                        : "%s: config.txt.vjo-old, the configuration from before the stop, becomes config.txt again.", g->path);
        }
        n = add(lines, n, max, "Nothing is deleted: every other copy stays as a dated backup.");
        n = add(lines, n, max, "Then restart the Vita yourself.");
        n = add(lines, n, max, VJO_INSTALL_RECOVERY_BOOT);
        return n < max ? n : max;
    }
    if (action == VJO_INSTALL_ACT_UNINSTALL) {
        int k = 1;
        for (int c = 0; c < 2; c++)
            if (s->cfg[c].uninstall_changes) {
                char name[PATH];
                backup_name(s->cfg[c].path, s->stamp, name, sizeof(name));
                n = add(lines, n, max, "%d. Back up %s as %s (kept), then remove the overlay's lines from it. Exactly:", k++,
                        s->cfg[c].path, strrchr(name, '/') + 1);
                n = add_diff(lines, n, max, &s->cfg[c], 1, VJO_INSTALL_DIFF);
            }
        n = add(lines, n, max, "%d. Keep the plugin files, the models, the dictionary, your settings and your cards: delete "
                "ur0:tai/VitaJPOverlay_*, ux0:data/VitaJPOverlay and ur0:data/VitaJPOverlay in VitaShell for the room.", k++);
        n = add(lines, n, max, "Then restart the Vita: the overlay no longer loads. This app stays until you delete it.");
        return n < max ? n : max;
    }
    /* install, update, repair, check: the whole plan when it fits; else fewer of config.ini's changed lines,
     * then of config.txt's exact lines (never fewer than two of each), the rest counted (code audit note 2) */
    int dcap = 0, icap = s->config_change_count, need;
    for (int c = 0; c < 2; c++)
        if (s->cfg[c].diff_count[0] > dcap) dcap = s->cfg[c].diff_count[0];
    n = install_plan(s, lines, max, dcap, icap, &need);
    while (need > max && (dcap > 2 || icap > 2)) {
        if (icap > 2) icap--;
        else dcap--;
        n = install_plan(s, lines, max, dcap, icap, &need);
    }
    return n;
}

/* An install's (update's, repair's, check's) plan with at most dcap exact lines per configuration and icap
 * config.ini lines: the lines written (at most max), and in *need how many it has. Its closing lines (the
 * restart, the recovery, the backups from before the overlay) always fit: the steps get what they leave. */
static int install_plan(const VjoInstallScan *s, char lines[][VJO_INSTALL_LINE], int max, int dcap, int icap, int *need)
{
    char words[200];
    int n = 0, backups = 0;
    for (int c = 0; c < 2; c++) backups += s->cfg[c].install_changes;
    int update_note = s->state == VJO_INSTALL_UPDATE && backups, tail = 3 + update_note;
    for (int c = 0; c < 2; c++) tail += s->before_overlay[c][0] != 0;
    int steps = max > tail ? max - tail : 0;
    uint64_t models = 0;
    int plugins_to_write = 0, models_to_write = 0;
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) {
        const VjoPayloadFile *f = &s->payload.file[i];
        if (f->kind != VJO_PAYLOAD_COPY) continue;
        int plugin = i >= VJO_PAYLOAD_KERNEL_INDEX;
        if (!plugin) models += f->bytes;
        if (!s->file_ok[i]) { if (plugin) plugins_to_write++; else models_to_write++; }
    }
    int k = 1;
    if (s->state == VJO_INSTALL_INSTALLED)
        n = add(lines, n, steps, "%d. Check every file against the package (SHA-256): one that differs is written again.", k++);
    else if (s->state == VJO_INSTALL_NOT_INSTALLED || (plugins_to_write && models_to_write))
        n = add(lines, n, steps, "%d. Copy the 3 plugins and their RCO to ur0:tai and ur0:data/VitaJPOverlay, and the OCR models "
                "(%.1f MB) to ux0:data/VitaJPOverlay/ocr; a file that already matches is not written again.", k++, mb(models));
    else if (plugins_to_write)
        n = add(lines, n, steps, "%d. Replace %d of the plugins and their RCO in ur0:tai and ur0:data/VitaJPOverlay (the models "
                "already match the package's sizes; every file is checked by SHA-256 first).", k++, plugins_to_write);
    else if (models_to_write)
        n = add(lines, n, steps, "%d. Copy %d OCR model file%s to ux0:data/VitaJPOverlay/ocr (the plugins already match).", k++,
                models_to_write, models_to_write == 1 ? "" : "s");
    else
        n = add(lines, n, steps, "%d. Check every file against the package (SHA-256): one that differs is written again.", k++);
    if (s->state == VJO_INSTALL_REPAIR && !s->record_ok)
        n = add(lines, n, steps, "   " VJO_APP_STAMP_PACKAGE " (the plugins' record of their app) is written again: it is "
                "missing or names another build of this app.");
    if (s->dict_index >= 0) {
        uint64_t d = s->payload.file[s->dict_index].bytes;
        if (s->dict_in_app && !s->file_ok[s->dict_index])
            n = add(lines, n, steps, "   Move the dictionary (%.0f MB) to ux0:data/VitaJPOverlay/dictionaries from this app: "
                    "moved, not copied, so this app no longer has it afterwards.", mb(d));
        else if (s->dict_in_app)
            n = add(lines, n, steps, "   The dictionary is already installed: once it is checked, this app's copy is deleted "
                    "(%.0f MB freed).", mb(d));
    }
    if (backups) {
        char names[2][PATH];
        for (int c = 0; c < 2; c++) {
            names[c][0] = '\0';
            if (s->cfg[c].install_changes) backup_name(s->cfg[c].path, s->stamp, names[c], sizeof(names[c]));
        }
        if (backups > 1 && !strcmp(strrchr(names[0], '/'), strrchr(names[1], '/')))
            n = add(lines, n, steps, "%d. Back up %s and %s as %s in each folder (kept).", k++, VJO_TAI_UX0, VJO_TAI_UR0,
                    strrchr(names[0], '/') + 1);
        else
            for (int c = 0; c < 2; c++)
                if (names[c][0])
                    n = add(lines, n, steps, "%d. Back up %s as %s (kept).", k++, s->cfg[c].path, strrchr(names[c], '/') + 1);
    }
    for (int c = 0; c < 2; c++) {
        const VjoInstallConfig *g = &s->cfg[c];
        if (!g->exists) continue;
        if (g->install_changes) {
            edit_words(&g->install, 1, words, sizeof(words));
            n = add(lines, n, steps, "%d. %s%s: %s. Exactly:", k++, g->path, reads_note(s, c), words);
            n = add_diff(lines, n, steps, g, 0, dcap);
        } else {
            n = add(lines, n, steps, "%d. %s%s: unchanged (it already loads the overlay).", k++, g->path, reads_note(s, c));
        }
    }
    if (s->config_ini == 0)
        n = add(lines, n, steps, "%d. config.ini: unchanged (your settings stay as they are).", k++);
    else if (s->config_ini == 2)
        n = add(lines, n, steps, "%d. config.ini: %s.", k++, s->config_change_count ? s->config_changes[0] : "written");
    else if (s->config_ini == 3)
        n = add(lines, n, steps, "%d. config.ini is missing beside its backup config.ini.bak (a save that stopped): it is "
                "written again from the backup, your settings as they were.", k++);
    else if (!s->config_change_count && !s->config_change_more)
        /* a text that differs in line ends or comments only: written, but no setting changes (never an empty list) */
        n = add(lines, n, steps, "%d. config.ini: written again with every setting as it was; the old file is kept as "
                "config.ini.bak.", k++);
    else {
        int shown = s->config_change_count < icap ? s->config_change_count : icap;
        int more = s->config_change_count - shown + s->config_change_more;
        n = add(lines, n, steps, "%d. config.ini: these lines change; every other setting stays, and the old file is kept "
                "as config.ini.bak:", k++);
        for (int i = 0; i < shown; i++) n = add(lines, n, steps, "   %s", s->config_changes[i]);
        if (more) n = add(lines, n, steps, "   and %d more line%s.", more, more == 1 ? "" : "s");
    }
    n = add(lines, n, steps, "%d. Read everything back to check it.", k++);
    *need = n + tail;
    n = n < steps ? n : steps;
    if (update_note)
        n = add(lines, n, max, "A dated backup is the configuration from before this install: it does not undo the update "
                "(the plugin files are replaced). To go back, install the older VPK and choose Install.");
    n = add(lines, n, max, "Then restart the Vita yourself: this app never restarts it.");
    n = add(lines, n, max, VJO_INSTALL_RECOVERY_BOOT);
    n = add(lines, n, max, VJO_INSTALL_RECOVERY_GAME);
    for (int c = 0; c < 2; c++)
        if (s->before_overlay[c][0])
            n = add(lines, n, max, "(%s is the configuration from before the overlay.)", s->before_overlay[c]);
    return n < max ? n : max;
}

/* ---------------- the runs ---------------- */

/* The card is still what the plan was made from: the state, the configurations and the payload. */
static int unchanged(const VjoInstallScan *a, const VjoInstallScan *b)
{
    if (a->state != b->state || a->actions != b->actions || a->config_ini != b->config_ini) return 0;
    if (memcmp(a->payload.manifest_sha256, b->payload.manifest_sha256, 32)) return 0;
    for (int c = 0; c < 2; c++)
        if (a->cfg[c].exists != b->cfg[c].exists || a->cfg[c].interrupted != b->cfg[c].interrupted ||
            a->cfg[c].swap_files != b->cfg[c].swap_files || a->cfg[c].use_new != b->cfg[c].use_new ||
            memcmp(a->cfg[c].sha256, b->cfg[c].sha256, 32))
            return 0;
    return 1;
}

static void lock(int on)
{
    static int initialised;
    if (on && !initialised) {
        sceShellUtilInitEvents(0);
        initialised = 1;
    }
    int rc = on ? sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU)
                : sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU);
    if (rc < 0) vjo_log("install: the PS button and power-off menu %s refused (0x%08X)", on ? "lock" : "unlock", (unsigned)rc);
    if (on) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

/* A folder written, renamed in and removed from: 0, or -1. */
static int probe_dir(const char *dir)
{
    char a[PATH], b[PATH];
    snprintf(a, sizeof(a), "%s/.vjo-probe", dir);
    snprintf(b, sizeof(b), "%s/.vjo-probe2", dir);
    sceIoRemove(a);
    sceIoRemove(b);
    int ok = write_verified(a, "probe", 5) == 0 && sceIoRename(a, b) >= 0 && sceIoRemove(b) >= 0;
    sceIoRemove(a);
    sceIoRemove(b);
    return ok ? 0 : -1;
}

/* A file moved from the app folder to dir (the dictionary's way): 0, or -1. */
static int probe_move(const char *dir)
{
    char a[PATH], b[PATH];
    snprintf(a, sizeof(a), VJO_PAYLOAD_APP_DIR "/.vjo-probe");
    snprintf(b, sizeof(b), "%s/.vjo-probe-move", dir);
    sceIoRemove(a);
    sceIoRemove(b);
    int ok = write_verified(a, "probe", 5) == 0 && sceIoRename(a, b) >= 0 && sceIoRemove(b) >= 0;
    sceIoRemove(a);
    sceIoRemove(b);
    return ok ? 0 : -1;
}

static int stop(Run *x, const char *fmt, const char *arg)
{
    char text[VJO_INSTALL_LINE];
    snprintf(text, sizeof(text), fmt, arg ? arg : "");
    x->r->rc = -1;
    say(x->r, "Stopped: %s", text);
    vjo_log("install: stopped: %s", text);
    /* what finishes the job: the same action again; the Install page's next plan when a stopped swap's file
     * is left beside a configuration (it offers to put it back or keep the current one first, and an Install
     * or Uninstall stops beside one) or after Put it back and Keep */
    int left = 0;
    for (int c = 0; c < 2; c++) {
        char path[PATH];
        snprintf(path, sizeof(path), "%s.vjo-new", tai_path(c));
        left |= exists(path);
        snprintf(path, sizeof(path), "%s.vjo-old", tai_path(c));
        left |= exists(path);
    }
    say(x->r, !x->r->written ? x->backed_up ? "Nothing on the card was changed but the dated backups this run wrote (new files, kept)."
                                            : "Nothing on the card was changed."
              : left || (x->action != VJO_INSTALL_ACT_INSTALL && x->action != VJO_INSTALL_ACT_UNINSTALL)
              ? "Every file on the card is the old one or the new one; the Install page shows what is left to do."
              : x->action == VJO_INSTALL_ACT_UNINSTALL ? "Every file on the card is the old one or the new one; Uninstall "
                                                         "again finishes the job."
              : "Every file on the card is the old one or the new one; Install again finishes the job.");
    return -1;
}

/* A configuration's new text into place (ux0's, then ur0's): 0, or -1 with r's lines. */
static int swap_config(Run *x, int c, const char *text, int len)
{
    char fresh[PATH], old[PATH];
    const char *path = tai_path(c);
    snprintf(fresh, sizeof(fresh), "%s.vjo-new", path);
    snprintf(old, sizeof(old), "%s.vjo-old", path);
    step(x, "Writing %s", path);
    if (exists(fresh) || exists(old)) /* the scan's "interrupted": never removed, never written over (C4) */
        return stop(x, "%s has a stopped swap's file beside it: open the Install page again.", path);
    if (write_verified(fresh, text, (size_t)len) < 0) {
        sceIoRemove(fresh);
        return stop(x, "%s.vjo-new could not be written and read back.", path);
    }
    if (sceIoRename(path, old) < 0) {
        sceIoRemove(fresh);
        return stop(x, "%s could not be renamed (it is as it was).", path);
    }
    /* no config.txt here until the next call: the window docs/one-vpk-install.md step 5 describes */
    if (sceIoRename(fresh, path) < 0) {
        int back = sceIoRename(old, path) >= 0;
        x->r->written = 1;
        return stop(x, back ? "%s.vjo-new could not be renamed: config.txt is back as it was."
                            : "%s is missing: open Overlay again to finish (its Install page has the old and the new copy).",
                    path);
    }
    sync_path(path);
    x->r->written = 1;
    int n = read_all(path, cfg_check, sizeof(cfg_check));
    if (n != len || memcmp(cfg_check, text, (size_t)len)) {
        char bad[PATH];
        free_name(bad, sizeof(bad), path, ".vjo-bad");
        int back = sceIoRename(path, bad) >= 0 && sceIoRename(old, path) >= 0;
        return stop(x, back ? "%s did not read back as written: the old one is back (the bad copy is config.txt.vjo-bad)."
                            : "%s did not read back as written: compare it with its backup in VitaShell.", path);
    }
    sceIoRemove(old);
    vjo_log("install: %s written (%d B)", path, len);
    log_diff(path, cfg_text[c], cfg_len[c], text, len);
    return 0;
}

/* 1 reset, 0 not yet (the Vita has run under BOOT_RECORD_FINAL_US). */
static int reset_boot_record(void)
{
    if ((uint64_t)sceKernelGetSystemTimeWide() < BOOT_RECORD_FINAL_US) return 0;
    sceIoMkdir("ur0:data", 0777);
    sceIoMkdir("ur0:data/VitaJPOverlay", 0777);
    SceUID fd = sceIoOpen(BOOT_RECORD, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    sync_path(BOOT_RECORD);
    return 1;
}

/* A one-line record (vjo_app.txt, installed.txt) by temp file and rename: 0, or -1. */
static int write_record(const char *path, const char *text)
{
    char fresh[PATH];
    snprintf(fresh, sizeof(fresh), "%s.vjo-new", path);
    sceIoRemove(fresh);
    if (write_verified(fresh, text, strlen(text)) < 0 || swap_in(path, fresh) < 0) {
        sceIoRemove(fresh);
        return -1;
    }
    return 0;
}

static int backups(Run *x, VjoInstallScan *s, int install, char names[2][PATH])
{
    for (int c = 0; c < 2; c++) {
        names[c][0] = '\0';
        if (!(install ? s->cfg[c].install_changes : s->cfg[c].uninstall_changes)) continue;
        char base[PATH - 16];
        snprintf(base, sizeof(base), "%s.vjo-%s", tai_path(c), s->stamp);
        snprintf(names[c], PATH, "%s.bak", base);
        for (int k = 2; exists(names[c]) && k < 100; k++) snprintf(names[c], PATH, "%s-%d.bak", base, k);
        step(x, "Backing up %s", tai_path(c));
        if (write_verified(names[c], cfg_text[c], (size_t)cfg_len[c]) < 0) {
            sceIoRemove(names[c]);
            return stop(x, "the backup of %s could not be written and read back.", tai_path(c));
        }
        x->backed_up = 1;
        vjo_log("install: %s backed up as %s", tai_path(c), names[c]);
    }
    return 0;
}

static int install_files(Run *x, VjoInstallScan *s, int move_ok, char *dict_note, size_t note_cap)
{
    char src[PATH], fresh[PATH];
    uint8_t sha[32];
    dict_note[0] = '\0';
    /* the dictionary first: when it cannot be satisfied, no other file is written */
    int order[VJO_PAYLOAD_COUNT], n = 0;
    if (s->dict_index >= 0) order[n++] = s->dict_index;
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++)
        if (i != s->dict_index && s->payload.file[i].kind == VJO_PAYLOAD_COPY) order[n++] = i;
    for (int j = 0; j < n; j++) {
        int i = order[j];
        const VjoPayloadFile *f = &s->payload.file[i];
        const char *name = strrchr(f->dest, '/') + 1;
        vjo_payload_source(i, src, sizeof(src));
        snprintf(fresh, sizeof(fresh), "%s.vjo-new", f->dest);
        /* a file that already has the payload's SHA-256 is not written again */
        step(x, "Checking %s", name);
        int64_t have = size_of(f->dest);
        char old[PATH];
        snprintf(old, sizeof(old), "%s.vjo-old", f->dest);
        if (have >= 0 && (uint64_t)have == f->bytes && hash_file(x, f->dest, sha, f->bytes) == have &&
            !memcmp(sha, f->sha256, 32)) {
            /* the overlay's own temporary files beside a file that is right: an earlier run's */
            if (exists(fresh) && sceIoRemove(fresh) >= 0) x->r->written = 1;
            if (exists(old) && sceIoRemove(old) >= 0) x->r->written = 1;
            if (f->kind == VJO_PAYLOAD_MOVE && exists(src)) {
                int gone = move_ok && sceIoRemove(src) >= 0;
                snprintf(dict_note, note_cap, gone ? "The dictionary was already installed: this app's copy was deleted (%.0f MB freed)."
                         : "The dictionary was already installed; this app's copy could not be deleted (%.0f MB).", mb(f->bytes));
            }
            continue;
        }
        /* an earlier run's file with the payload's SHA-256 is used first: a stopped swap's new file, or the
         * old one put aside when the payload's is the same (a frozen dictionary) */
        const char *ready = NULL;
        step(x, "Checking an earlier run's %s", name);
        if (size_of(fresh) == (int64_t)f->bytes && hash_file(x, fresh, sha, f->bytes) == (int64_t)f->bytes &&
            !memcmp(sha, f->sha256, 32))
            ready = fresh;
        else if (have < 0 && size_of(old) == (int64_t)f->bytes && hash_file(x, old, sha, f->bytes) == (int64_t)f->bytes &&
                 !memcmp(sha, f->sha256, 32))
            ready = old;
        if (ready) {
            int ok = ready == fresh ? swap_in(f->dest, fresh) == 0 : sceIoRename(old, f->dest) >= 0;
            if (!ok) return stop(x, "%s could not be put in place (the old one is as it was).", f->dest);
            x->r->written = 1;
            if (exists(old)) sceIoRemove(old);
            if (f->kind == VJO_PAYLOAD_MOVE && exists(src) && move_ok) sceIoRemove(src);
            vjo_log("install: %s put in place from an earlier run's %s", f->dest, ready == fresh ? ".vjo-new" : ".vjo-old");
            if (i == VJO_PAYLOAD_KERNEL_INDEX) reset_boot_record();
            continue;
        }
        if (!exists(src))
            return stop(x, f->kind == VJO_PAYLOAD_MOVE ? "the installed dictionary differs from the package's and this app no "
                        "longer has its copy: install the VPK again in VitaShell, then Repair." : "%s is missing from this app: "
                        "install the VPK again in VitaShell.", name);
        make_dirs(f->dest);
        sceIoRemove(fresh); /* an earlier run's copy without the payload's SHA-256 (an interrupted copy) */
        if (f->kind == VJO_PAYLOAD_MOVE) {
            step(x, "Checking this app's %s", name);
            if (hash_file(x, src, sha, f->bytes) != (int64_t)f->bytes || memcmp(sha, f->sha256, 32))
                return stop(x, "this app's %s is damaged: install the VPK again in VitaShell.", name);
            step(x, "Moving %s", name);
            if (move_ok && sceIoRename(src, fresh) >= 0) {
                snprintf(dict_note, note_cap, "The dictionary (%.0f MB) was moved out of this app, not copied.", mb(f->bytes));
            } else {
                int64_t room = free_bytes("ux0:");
                if (room >= 0 && (uint64_t)room < f->bytes + ROOM)
                    return stop(x, "%s cannot be moved out of this app, and ux0 has no room for a copy.", name);
                step(x, "Copying %s", name);
                if (copy_verified(x, src, fresh, f) < 0) {
                    sceIoRemove(fresh);
                    return stop(x, "%s could not be copied and read back.", name);
                }
                snprintf(dict_note, note_cap, "The dictionary was copied (it could not be moved): this app keeps its copy too "
                         "(%.0f MB).", mb(f->bytes));
            }
        } else {
            step(x, "Copying %s", name);
            if (copy_verified(x, src, fresh, f) < 0) {
                sceIoRemove(fresh);
                return stop(x, "%s could not be copied and read back (a full card?).", name);
            }
        }
        x->r->written = 1;
        if (swap_in(f->dest, fresh) < 0) {
            /* the moved dictionary goes back into the app, so a later Install has it again */
            if (f->kind == VJO_PAYLOAD_MOVE && !exists(src)) sceIoRename(fresh, src);
            return stop(x, "%s could not be put in place (the old one is as it was).", f->dest);
        }
        vjo_log("install: %s written (%llu B)", f->dest, (unsigned long long)f->bytes);
        if (i == VJO_PAYLOAD_KERNEL_INDEX) reset_boot_record(); /* a new build starts with a clean record */
    }
    return 0;
}

static int run_install(Run *x, VjoInstallScan *s)
{
    char names[2][PATH], dict_note[VJO_INSTALL_LINE];
    VjoInstallResult *r = x->r;
    /* 1: the PS button and the power-off menu held, app0: unmounted (its folder is writable then), every
     * folder probed: nothing of the overlay is written before all of them pass */
    lock(1);
    int umount = sceAppMgrUmount("app0:");
    vjo_log("install: %s, app0: unmount 0x%08X", s->title, (unsigned)umount);
    static const char *const dirs[] = {"ur0:tai", "ur0:data/VitaJPOverlay", "ux0:data/VitaJPOverlay",
                                       "ux0:data/VitaJPOverlay/ocr", "ux0:data/VitaJPOverlay/dictionaries"};
    step(x, "Checking that every folder can be written", NULL);
    sceIoMkdir("ur0:data", 0777);
    sceIoMkdir("ux0:data", 0777);
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        sceIoMkdir(dirs[i], 0777);
        if (probe_dir(dirs[i]) < 0) { lock(0); return stop(x, "%s cannot be written.", dirs[i]); }
    }
    int stale_copies = exists("ux0:tai/VitaJPOverlay_Kernel.skprx") || exists("ux0:tai/VitaJPOverlay_Shell.suprx") ||
                       exists("ux0:tai/VitaJPOverlay_OCR.suprx");
    if ((s->cfg[0].exists || stale_copies) && probe_dir("ux0:tai") < 0) { lock(0); return stop(x, "%s cannot be written.", "ux0:tai"); }
    int move_ok = probe_move("ux0:data/VitaJPOverlay/dictionaries") == 0;
    vjo_log("install: a file %s be moved out of this app", move_ok ? "can" : "cannot");
    /* room for the copies and their temporary files */
    uint64_t need[2] = {ROOM, ROOM}; /* ux0, ur0 */
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++) {
        const VjoPayloadFile *f = &s->payload.file[i];
        int ur0 = f->dest[1] == 'r';
        if (f->kind == VJO_PAYLOAD_COPY && !s->file_ok[i]) need[ur0] += f->bytes;
        if (f->kind == VJO_PAYLOAD_MOVE && !s->file_ok[i] && !move_ok) need[0] += f->bytes;
    }
    for (int d = 0; d < 2; d++) {
        int64_t room = free_bytes(d ? "ur0:" : "ux0:");
        if (room >= 0 && (uint64_t)room < need[d]) {
            char what[96];
            snprintf(what, sizeof(what), "%s has %.1f MB free and the install needs %.1f MB", d ? "ur0" : "ux0", mb((uint64_t)room),
                     mb(need[d]));
            lock(0);
            /* ux0 also holds the .vpk the user copied: after VitaShell's install it can go (61 MB) */
            return stop(x, d ? "%s." : "%s. Delete what you no longer need on ux0 (the .vpk this app was installed from "
                        "can go), then choose Install again.", what);
        }
    }
    /* 2: the backups */
    if (backups(x, s, 1, names) < 0) { lock(0); return -1; }
    /* 3: the files */
    if (install_files(x, s, move_ok, dict_note, sizeof(dict_note)) < 0) { lock(0); return -1; }
    /* 4: config.ini */
    if (s->config_ini > 0) {
        step(x, "Writing %s", VJO_PREFERENCES_PATH);
        VjoArena a;
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        if (vjo_preferences_publish_text(ini_new, (size_t)ini_new_len, &a) < 0) {
            lock(0);
            return stop(x, "config.ini could not be written (it is as it was, or its backup config.ini.bak holds it).", NULL);
        }
        r->written = 1;
        vjo_log("install: config.ini %s (%d B)", s->config_ini == 2 ? "written from the template"
                : s->config_ini == 3 ? "written again from config.ini.bak" : "updated", ini_new_len);
    }
    /* 5: the configurations, ux0's first: while both exist, every moment is bootable */
    for (int c = 0; c < 2; c++)
        if (s->cfg[c].install_changes && swap_config(x, c, cfg_new[c], cfg_new_len[c]) < 0) { lock(0); return -1; }
    /* 6: a stale copy of ours in ux0:tai goes aside (the kernel would load ux0:tai's worker when ur0's is missing) */
    static const char *const stale[] = {"ux0:tai/VitaJPOverlay_Kernel.skprx", "ux0:tai/VitaJPOverlay_Shell.suprx",
                                        "ux0:tai/VitaJPOverlay_OCR.suprx"};
    for (unsigned i = 0; i < sizeof(stale) / sizeof(stale[0]); i++) {
        if (!exists(stale[i])) continue;
        char aside[PATH], suffix[40];
        snprintf(suffix, sizeof(suffix), ".vjo-stale-%s", s->stamp);
        free_name(aside, sizeof(aside), stale[i], suffix);
        int ok = sceIoRename(stale[i], aside) >= 0;
        say(r, ok ? "A stale copy %s was renamed aside (%s)." : "A stale copy %s could not be renamed%s: delete it in VitaShell.",
            stale[i], ok ? strrchr(aside, '/') + 1 : "");
        if (ok) r->written = 1;
    }
    /* 7: the records */
    char line[VJO_APP_STAMP_MAX + 2], record[VJO_APP_STAMP_MAX], installed[200];
    snprintf(line, sizeof(line), "%s\n", s->own_stamp);
    if ((!first_line(VJO_APP_STAMP_PACKAGE, record, sizeof(record)) || strcmp(record, s->own_stamp))) {
        if (write_record(VJO_APP_STAMP_PACKAGE, line) < 0) { lock(0); return stop(x, "%s could not be written.", VJO_APP_STAMP_PACKAGE); }
        r->written = 1;
    }
    if (vjo_installed_format(installed, sizeof(installed), &s->payload) > 0) {
        char old[256];
        int n = read_all(VJO_INSTALLED_RECORD, old, sizeof(old));
        if (n < 0 || strcmp(old, installed)) {
            if (write_record(VJO_INSTALLED_RECORD, installed) < 0) { lock(0); return stop(x, "%s could not be written.", VJO_INSTALLED_RECORD); }
            r->written = 1;
        }
    }
    int reset = reset_boot_record();
    lock(0);
    /* what was done */
    r->rc = 0;
    if (!r->written && s->state == VJO_INSTALL_INSTALLED) {
        say(r, reset ? "Every file and line is in place: nothing needed writing (the boot guard's record is reset)."
                     : "Every file and line is in place: nothing needed writing.");
        if (!reset)
            say(r, "The boot guard's record was left as it is (the Vita has run for under a minute): if it is in safe "
                "mode, choose Check and repair again in a minute.");
        return 0;
    }
    say(r, "Installed the overlay v%s%s. Restart the Vita now: the new plugins load at its next start.",
        s->payload.version, strcmp(s->payload.variant, "debug") ? "" : " (debug build)");
    if (dict_note[0]) say(r, "%s", dict_note);
    if (s->build.worker == VJO_TAI_WORKER_LINES)
        say(r, "No game has OCR yet: this build's kernel does not load the OCR worker, so name each game with "
            "install-vita.sh --title from a computer.");
    for (int c = 0; c < 2; c++)
        if (names[c][0]) say(r, "Backup: %s (kept).", names[c]);
    if (!reset)
        say(r, "The boot guard's record was left as it is (the Vita has run for under a minute): if it is in safe mode, "
            "choose Check and repair again in a minute.");
    say(r, VJO_INSTALL_RECOVERY_BOOT);
    say(r, VJO_INSTALL_RECOVERY_GAME);
    vjo_log("install: done (v%s %s)", s->payload.version, s->payload.variant);
    return 0;
}

static int run_uninstall(Run *x, VjoInstallScan *s)
{
    char names[2][PATH];
    lock(1);
    if (backups(x, s, 0, names) < 0) { lock(0); return -1; }
    for (int c = 0; c < 2; c++) {
        if (!s->cfg[c].uninstall_changes) continue;
        VjoTaiReport u;
        int n = vjo_tai_edit(cfg_text[c], (size_t)cfg_len[c], VJO_TAI_UNINSTALL, s->build.worker, NULL, 0, cfg_new[c],
                             sizeof(cfg_new[c]), &u);
        if (n < 0) { lock(0); return stop(x, "%s cannot be edited.", tai_path(c)); }
        if (swap_config(x, c, cfg_new[c], n) < 0) { lock(0); return -1; }
    }
    reset_boot_record();
    lock(0);
    x->r->rc = 0;
    say(x->r, "Uninstalled: the overlay's lines are gone. Restart the Vita: the overlay no longer loads.");
    for (int c = 0; c < 2; c++)
        if (names[c][0]) say(x->r, "Backup: %s (kept).", names[c]);
    say(x->r, "The plugin files, the models, the dictionary and your settings and cards stay; delete them in VitaShell "
        "for the room (ur0:tai/VitaJPOverlay_*, ux0:data/VitaJPOverlay, ur0:data/VitaJPOverlay).");
    vjo_log("install: uninstalled");
    return 0;
}

/* src renamed to path's next free dated backup name (config.txt.vjo-<stamp>[-<tag>].bak): 0, or -1. */
static int retire(const char *src, const char *path, const char *stamp, const char *tag, char *out, size_t cap)
{
    char base[PATH - 16];
    snprintf(base, sizeof(base), "%s.vjo-%s%s%s", path, stamp, tag[0] ? "-" : "", tag);
    snprintf(out, cap, "%s.bak", base);
    for (int k = 2; exists(out) && k < 100; k++) snprintf(out, cap, "%s-%d.bak", base, k);
    return sceIoRename(src, out) < 0 ? -1 : 0;
}

/* FINISH: a stopped swap's file back as config.txt (the current one, if any, kept as a dated backup);
 * KEEP: the current config.txt stays, the leftovers become dated backups. Nothing is removed (C4). */
static int run_leftovers(Run *x, VjoInstallScan *s, int put_back)
{
    char name[PATH];
    lock(1);
    for (int c = 0; c < 2; c++) {
        const VjoInstallConfig *g = &s->cfg[c];
        const char *path = tai_path(c);
        char fresh[PATH], old[PATH];
        if (!g->interrupted && !g->leftover) continue;
        snprintf(fresh, sizeof(fresh), "%s.vjo-new", path);
        snprintf(old, sizeof(old), "%s.vjo-old", path);
        step(x, put_back ? "Putting %s back" : "Keeping %s", path);
        if (!put_back && !g->exists) { /* never offered (the scan): a missing config.txt is never kept missing */
            lock(0);
            return stop(x, "%s is missing: it cannot be kept.", path);
        }
        if (put_back) {
            /* the scan's choice only: a .vjo-new that is no whole edit is never put back */
            const char *use = g->use_new ? fresh : old;
            if (!g->finish_ok || !exists(use)) {
                lock(0);
                return stop(x, "%s has no whole configuration to put back.", path);
            }
            if (g->leftover) { /* the current one first: a dated backup, by rename */
                if (retire(path, path, s->stamp, "", name, sizeof(name)) < 0) { lock(0); return stop(x, "%s could not be kept aside.", path); }
                x->r->written = 1;
                say(x->r, "The current %s is kept as %s.", path, name);
            }
            if (sceIoRename(use, path) < 0) {
                int back = g->leftover && sceIoRename(name, path) >= 0; /* the current one, as it was */
                lock(0);
                return stop(x, back ? "%s could not be put back: the current one is back as it was."
                                    : "%s could not be put back: in VitaShell, rename config.txt.vjo-new (or .vjo-old) to "
                                      "config.txt.", path);
            }
            x->r->written = 1;
            sync_path(path);
            say(x->r, "%s is back (%s).", path, use != fresh ? "the configuration from before the stop"
                    : g->stopped_run == VJO_INSTALL_ACT_UNINSTALL ? "the stopped uninstall's new configuration"
                    : "the stopped install's new configuration");
            vjo_log("install: %s put back from %s", path, use);
        }
        /* every leftover still there becomes a dated backup */
        const char *left[2] = {old, fresh}, *tags[2] = {"old", "new"};
        for (int k = 0; k < 2; k++) {
            if (!exists(left[k])) continue;
            if (retire(left[k], path, s->stamp, tags[k], name, sizeof(name)) < 0) {
                lock(0);
                return stop(x, "%s could not be renamed to a dated backup.", left[k]);
            }
            x->r->written = 1;
            say(x->r, "Kept: %s.", name);
        }
    }
    lock(0);
    x->r->rc = 0;
    say(x->r, "Restart the Vita now, or Install again first: the Install page shows what is on the card.");
    say(x->r, VJO_INSTALL_RECOVERY_BOOT);
    return 0;
}

int vjo_install_run(VjoInstallScan *s, int action, const VjoInstallUi *ui, VjoInstallResult *r)
{
    static VjoInstallScan now;
    Run x;
    memset(r, 0, sizeof(*r));
    memset(&x, 0, sizeof(x));
    x.ui = ui;
    x.r = r;
    x.action = action;
    x.last_tick = sceKernelGetProcessTimeWide();
    /* step 0 again: the card must still be what the plan said */
    step(&x, "Checking the card", NULL);
    vjo_install_scan(&now, &s->build);
    snprintf(now.stamp, sizeof(now.stamp), "%s", s->stamp); /* the names the plan showed */
    if (!unchanged(s, &now) || !(now.actions & action)) {
        *s = now;
        r->rc = -1;
        say(r, "The card changed since the plan was shown, so nothing was done: look at the new plan.");
        return -1;
    }
    *s = now;
    int rc = action == VJO_INSTALL_ACT_INSTALL ? run_install(&x, s)
           : action == VJO_INSTALL_ACT_UNINSTALL ? run_uninstall(&x, s)
           : action == VJO_INSTALL_ACT_FINISH ? run_leftovers(&x, s, 1)
           : action == VJO_INSTALL_ACT_KEEP ? run_leftovers(&x, s, 0) : stop(&x, "an unknown action.", NULL);
    return rc;
}
