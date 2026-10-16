/* The Overlay app's install engine (settings/install.h; docs/one-vpk-install.md). */
#include "install.h"
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

/* .vjo-new is the editor's result of .vjo-old (an install's or an uninstall's): the swap's new text. */
static char leftover_old[VJO_TAI_TEXT_MAX + 2], leftover_new[VJO_TAI_TEXT_MAX + 2];
static int new_is_result_of_old(const char *path, int worker)
{
    char a[PATH], b[PATH];
    snprintf(a, sizeof(a), "%s.vjo-old", path);
    snprintf(b, sizeof(b), "%s.vjo-new", path);
    int n = read_all(b, leftover_new, sizeof(leftover_new)), o = read_all(a, leftover_old, sizeof(leftover_old));
    if (n <= 0 || n > VJO_TAI_TEXT_MAX) return 0;
    if (o < 0) return ours_text(leftover_new, n, worker); /* no .vjo-old: only an install's result is used */
    if (o > VJO_TAI_TEXT_MAX) return 0;
    for (int op = 0; op < 2; op++) {
        VjoTaiReport r;
        int m = vjo_tai_edit(leftover_old, (size_t)o, op, worker, NULL, 0, cfg_check, sizeof(cfg_check), &r);
        if (m == n && !memcmp(cfg_check, leftover_new, (size_t)n)) return 1;
    }
    return 0;
}

static const char *tai_path(int c) { return c ? VJO_TAI_UR0 : VJO_TAI_UX0; }

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

/* config.ini's new text for an install (into ini_new): 2 written from the package's template (there is
 * neither config.ini nor its backup, which the app's load reads when config.ini is missing), 1 patched,
 * 0 unchanged, -1 unreadable (s->reason). The lines it changes go into s->config_changes. */
static int plan_config_ini(VjoInstallScan *s)
{
    VjoArena a;
    VjoConfig base, cfg;
    char *old = NULL, value[512];
    size_t bytes = 0;
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
    int rc = vjo_preferences_text(&a, &old, &bytes);
    s->config_change_count = 0;
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
        snprintf(s->config_changes[s->config_change_count++], sizeof(s->config_changes[0]), "written from the package's template");
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
    const char *keys[6], *values[6];
    int k = 0;
    keys[k] = "local_dictionary_dir"; values[k++] = "ux0:data/VitaJPOverlay/dictionaries";
    keys[k] = "ocr_model_dir"; values[k++] = "ux0:data/VitaJPOverlay/ocr";
    keys[k] = "text_source"; values[k++] = "ocr";
    int d = dictionaries(base.local_dictionaries, value, sizeof(value));
    if (d > 0) { keys[k] = "local_dictionaries"; values[k++] = value; }
    else if (d < 0 && s->config_change_count < 6)
        snprintf(s->config_changes[s->config_change_count++], sizeof(s->config_changes[0]),
                 "local_dictionaries has 8 names: add plaintext.vjdict to it yourself");
    int log_set = 0;
    for (int i = 0; i < s->payload.sets && k < 6; i++) {
        keys[k] = s->payload.set[i].key; values[k++] = s->payload.set[i].value;
        log_set |= !strcmp(s->payload.set[i].key, "log_file");
    }
#if VJO_DEBUG
    /* the debug build is the one asked for when a full log is needed (docs/release-variants.md) */
    if (!log_set && k < 6) { keys[k] = "log_file"; values[k++] = "on"; }
#else
    (void)log_set;
#endif
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
            if (s->config_change_count < 6)
                snprintf(s->config_changes[s->config_change_count++], sizeof(s->config_changes[0]), "%s left as it is (the new value would be refused)",
                         keys[i]);
            continue;
        }
        memcpy(ini_new, ini_step, (size_t)m + 1);
        ini_new_len = m;
    }
    if ((size_t)ini_new_len == bytes && !memcmp(ini_new, old, bytes)) return 0;
    /* the changed lines: those of the new text that the old one does not have (no API key: they only go) */
    for (int pos = 0; pos < ini_new_len && s->config_change_count < 6;) {
        int e = pos;
        while (e < ini_new_len && ini_new[e] != '\n') e++;
        int len = e - pos, found = 0;
        for (size_t q = 0; q + (size_t)len <= bytes && !found;) {
            size_t qe = q;
            while (qe < bytes && old[qe] != '\n') qe++;
            size_t ql = qe - q;
            if (ql && old[qe - 1] == '\r') ql--;
            found = ql == (size_t)len && !memcmp(old + q, ini_new + pos, (size_t)len);
            q = qe + 1;
        }
        if (!found && len > 0 && ini_new[pos] != ';' && ini_new[pos] != '#')
        {
            int keep = len > 90 ? 90 : len;
            memcpy(s->config_changes[s->config_change_count], ini_new + pos, (size_t)keep);
            s->config_changes[s->config_change_count++][keep] = '\0';
        }
        pos = e + 1;
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
        VjoTaiReport u;
        rc = vjo_tai_edit(cfg_text[c], (size_t)n, VJO_TAI_UNINSTALL, build->worker, NULL, 0, cfg_check, sizeof(cfg_check), &u);
        g->uninstall_changes = rc >= 0 && u.changed;
        before_overlay(g->path, s->before_overlay[c], sizeof(s->before_overlay[c]));
        if (g->leftover) {
            interrupted = 1;
            g->now_kind = stock_text(cfg_text[c], (size_t)n) ? VJO_TAI_NOW_STOCK
                        : ours_text(cfg_text[c], n, build->worker) ? VJO_TAI_NOW_OURS : VJO_TAI_NOW_OTHER;
        }
    }
    for (int c = 0; c < 2; c++)
        if (s->cfg[c].interrupted || s->cfg[c].leftover)
            s->cfg[c].use_new = (s->cfg[c].interrupted == 2 || (s->cfg[c].leftover & 2)) &&
                                new_is_result_of_old(s->cfg[c].path, build->worker);
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
    if (s->has_payload && !interrupted && !damaged && !s->unsafe_off) s->config_ini = plan_config_ini(s);

    /* the state */
    const char *v = s->payload.version;
    if (interrupted) {
        s->state = VJO_INSTALL_INTERRUPTED;
        s->actions = VJO_INSTALL_ACT_FINISH;
        snprintf(s->title, sizeof(s->title), "An install was interrupted");
        int c = s->cfg[0].interrupted || s->cfg[0].leftover ? 0 : 1;
        const VjoInstallConfig *g = &s->cfg[c];
        if (g->interrupted)
            snprintf(s->reason, sizeof(s->reason), "%s is missing: an install or uninstall stopped between two renames. "
                     "Put it back here, then restart the Vita.", g->path);
        else
            snprintf(s->reason, sizeof(s->reason), "%s has a stopped swap's %s beside it, and is %s now. Put it back "
                     "(the current one is kept as a dated backup), or keep the current one (the others are kept as "
                     "dated backups).", g->path, g->leftover == 3 ? "config.txt.vjo-old and .vjo-new"
                     : g->leftover == 2 ? "config.txt.vjo-new" : "config.txt.vjo-old",
                     g->now_kind == VJO_TAI_NOW_STOCK ? "HENkaku's default (h-encore writes it when there is none)"
                     : g->now_kind == VJO_TAI_NOW_OURS ? "the overlay's" : "another configuration");
        for (int k = 0; k < 2; k++)
            if (s->cfg[k].leftover) s->actions |= VJO_INSTALL_ACT_KEEP;
        if (s->unsafe_off) {
            size_t used = strlen(s->reason);
            snprintf(s->reason + used, sizeof(s->reason) - used, " This app cannot write ur0:tai now (Enable Unsafe "
                     "Homebrew is off): in VitaShell rename config.txt.vjo-new (or .vjo-old) to config.txt.");
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
    } else if (!s->record_ok || !s->plugins_ok || (s->has_installed && !same_build) || !s->has_installed) {
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
    } else if (!s->files_ok || need_lines || s->config_ini > 0) {
        s->state = VJO_INSTALL_REPAIR;
        snprintf(s->title, sizeof(s->title), "Repair the overlay (v%s)", v);
    } else {
        s->state = VJO_INSTALL_INSTALLED;
        snprintf(s->title, sizeof(s->title), "The overlay is installed (v%s%s)", v,
                 strcmp(s->payload.variant, "debug") ? "" : ", debug build");
    }
}

/* ---------------- the plans ---------------- */

static int add(char lines[][VJO_INSTALL_LINE], int n, int max, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int add(char lines[][VJO_INSTALL_LINE], int n, int max, const char *fmt, ...)
{
    va_list ap;
    if (n >= max) return n;
    va_start(ap, fmt);
    vsnprintf(lines[n], VJO_INSTALL_LINE, fmt, ap);
    va_end(ap);
    return n + 1;
}

/* What an edit of one configuration does, in a few words. */
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

int vjo_install_plan(const VjoInstallScan *s, int action, char lines[][VJO_INSTALL_LINE], int max)
{
    int n = 0;
    char words[200];
    if (action == VJO_INSTALL_ACT_FINISH || action == VJO_INSTALL_ACT_KEEP) {
        for (int c = 0; c < 2; c++) {
            const VjoInstallConfig *g = &s->cfg[c];
            if (!g->interrupted && !g->leftover) continue;
            if (action == VJO_INSTALL_ACT_KEEP) {
                if (g->leftover)
                    n = add(lines, n, max, "%s stays as it is; its config.txt.vjo-old and .vjo-new are kept as "
                            "config.txt.vjo-%s-*.bak.", g->path, s->stamp);
                continue;
            }
            if (g->leftover)
                n = add(lines, n, max, "%s: the current one is kept as config.txt.vjo-%s.bak, then %s becomes config.txt.",
                        g->path, s->stamp, g->use_new ? "config.txt.vjo-new (the stopped install's new configuration, "
                        "checked against the old one)" : "config.txt.vjo-old (the configuration from before the stop)");
            else
                n = add(lines, n, max, g->use_new
                        ? "%s: config.txt.vjo-new, the new configuration (written and checked before the stop), becomes config.txt."
                        : "%s: config.txt.vjo-old, the configuration from before the stop, becomes config.txt again.", g->path);
        }
        n = add(lines, n, max, "Nothing is deleted: every other copy stays as a dated backup.");
        n = add(lines, n, max, "Then restart the Vita yourself.");
        return add(lines, n, max, VJO_INSTALL_RECOVERY_BOOT);
    }
    if (action == VJO_INSTALL_ACT_UNINSTALL) {
        int k = 1;
        for (int c = 0; c < 2; c++)
            if (s->cfg[c].uninstall_changes)
                n = add(lines, n, max, "%d. Back up %s as config.txt.vjo-%s.bak (kept), then remove the overlay's lines from it.",
                        k++, s->cfg[c].path, s->stamp);
        n = add(lines, n, max, "%d. Keep the plugin files, the models, the dictionary, your settings and your cards: delete "
                "ur0:tai/VitaJPOverlay_*, ux0:data/VitaJPOverlay and ur0:data/VitaJPOverlay in VitaShell for the room.", k++);
        n = add(lines, n, max, "Then restart the Vita: the overlay no longer loads. This app stays until you delete it.");
        return n;
    }
    /* install, update, repair, check */
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
        n = add(lines, n, max, "%d. Check every file against the package (SHA-256): one that differs is written again.", k++);
    else
        n = add(lines, n, max, "%d. Copy the 3 plugins and their RCO to ur0:tai and ur0:data/VitaJPOverlay, and the OCR models "
                "(%.1f MB) to ux0:data/VitaJPOverlay/ocr; a file that already matches is not written again.", k++, mb(models));
    (void)plugins_to_write;
    (void)models_to_write;
    if (s->dict_index >= 0) {
        uint64_t d = s->payload.file[s->dict_index].bytes;
        if (s->dict_in_app && !s->file_ok[s->dict_index])
            n = add(lines, n, max, "   Move the dictionary (%.0f MB) to ux0:data/VitaJPOverlay/dictionaries from this app: "
                    "moved, not copied, so this app no longer has it afterwards.", mb(d));
        else if (s->dict_in_app)
            n = add(lines, n, max, "   The dictionary is already installed: once it is checked, this app's copy is deleted "
                    "(%.0f MB freed).", mb(d));
    }
    int backups = 0;
    for (int c = 0; c < 2; c++) backups += s->cfg[c].install_changes;
    if (backups) {
        char which[100];
        snprintf(which, sizeof(which), "%s%s%s", s->cfg[0].install_changes ? VJO_TAI_UX0 : "",
                 s->cfg[0].install_changes && s->cfg[1].install_changes ? " and " : "",
                 s->cfg[1].install_changes ? VJO_TAI_UR0 : "");
        n = add(lines, n, max, "%d. Back up %s as config.txt.vjo-%s.bak in %s folder (kept).", k++, which, s->stamp,
                backups > 1 ? "each" : "its");
    }
    for (int c = 0; c < 2; c++) {
        const VjoInstallConfig *g = &s->cfg[c];
        if (!g->exists) continue;
        if (g->install_changes) {
            edit_words(&g->install, 1, words, sizeof(words));
            n = add(lines, n, max, "%d. %s%s: %s.", k++, g->path, reads_note(s, c), words);
        } else {
            n = add(lines, n, max, "%d. %s%s: unchanged (it already loads the overlay).", k++, g->path, reads_note(s, c));
        }
    }
    if (s->config_ini == 0)
        n = add(lines, n, max, "%d. config.ini: unchanged (your settings stay as they are).", k++);
    else {
        char changes[200];
        size_t used = 0;
        changes[0] = '\0';
        for (int i = 0; i < s->config_change_count && used < sizeof(changes); i++)
            used += (size_t)snprintf(changes + used, sizeof(changes) - used, "%s%s", i ? "; " : "", s->config_changes[i]);
        n = add(lines, n, max, "%d. config.ini: %s%s.", k++, changes,
                s->config_ini == 1 ? " (every other setting stays; the old file is kept as config.ini.bak)" : "");
    }
    n = add(lines, n, max, "%d. Read everything back to check it.", k++);
    n = add(lines, n, max, "Then restart the Vita yourself: this app never restarts it.");
    n = add(lines, n, max, VJO_INSTALL_RECOVERY_BOOT);
    n = add(lines, n, max, VJO_INSTALL_RECOVERY_GAME);
    for (int c = 0; c < 2; c++)
        if (s->before_overlay[c][0])
            n = add(lines, n, max, "(%s is the configuration from before the overlay.)", s->before_overlay[c]);
    return n;
}

/* ---------------- the runs ---------------- */

/* The card is still what the plan was made from: the state, the configurations and the payload. */
static int unchanged(const VjoInstallScan *a, const VjoInstallScan *b)
{
    if (a->state != b->state || a->actions != b->actions || a->config_ini != b->config_ini) return 0;
    if (memcmp(a->payload.manifest_sha256, b->payload.manifest_sha256, 32)) return 0;
    for (int c = 0; c < 2; c++)
        if (a->cfg[c].exists != b->cfg[c].exists || a->cfg[c].interrupted != b->cfg[c].interrupted ||
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
    say(x->r, x->r->written ? "Every file on the card is the old one or the new one; Install again finishes the job."
                            : "Nothing on the card was changed.");
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
            return stop(x, "%s.", what);
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
        vjo_log("install: config.ini %s (%d B)", s->config_ini == 2 ? "written from the template" : "updated", ini_new_len);
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
        if (put_back) {
            const char *use = g->use_new ? fresh : old;
            if (!exists(use)) use = exists(old) ? old : fresh;
            if (g->leftover) { /* the current one first: a dated backup, by rename */
                if (retire(path, path, s->stamp, "", name, sizeof(name)) < 0) { lock(0); return stop(x, "%s could not be kept aside.", path); }
                x->r->written = 1;
                say(x->r, "The current %s is kept as %s.", path, name);
            }
            if (sceIoRename(use, path) < 0) {
                lock(0);
                return stop(x, "%s could not be put back: in VitaShell, rename config.txt.vjo-new (or .vjo-old) to config.txt.", path);
            }
            x->r->written = 1;
            sync_path(path);
            say(x->r, "%s is back (%s).", path, use == fresh ? "the stopped install's new configuration"
                                                             : "the configuration from before the stop");
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
