#include "psp.h"
#include "anki_page.h" /* vjo_log: log.txt */
#include "../core/pspbubble.h"
#include "../core/psptarget.h"
#include "../core/sfo.h"
#include "../core/taicfg.h"
#include "../core/utf.h"
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#if !defined(VJO_HOST) || defined(VJO_TEST_ZLIB)
#include <zlib.h>
#define VJO_PSP_DEFLATE 1
#endif

/* The configuration's largest size the app reads (an installed one is a few KiB). */
#define TAI_MAX (64 * 1024)
/* Free space a registration leaves on the card at least, beside the copy and
 * the block: a few clusters (32 KiB on a Sony card, up to 128 KiB on exFAT). */
#define TAI_ROOM (512 * 1024)

/* At most cap bytes of path: the count read, or -1 when it cannot be opened. */
static int read_small(const char *path, void *buf, size_t cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    size_t used = 0;
    if (fd < 0) return -1;
    while (used < cap) {
        int n = sceIoRead(fd, (char *)buf + used, cap - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    sceIoClose(fd);
    return (int)used;
}

static int file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int write_all(SceUID fd, const void *data, size_t len)
{
    size_t used = 0;
    while (used < len) {
        int n = sceIoWrite(fd, (const char *)data + used, len - used);
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

static int write_file(const char *path, const void *data, size_t len)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    return sceIoClose(fd) < 0 ? -1 : rc;
}

/* ---- the target: its PARAM.SFO through core/psptarget.c ---- */

static int target_read(void *ctx, uint64_t off, void *buf, size_t n)
{
    SceUID fd = *(SceUID *)ctx;
    if (sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) != (SceOff)off) return -1;
    for (size_t used = 0; used < n;) {
        int got = sceIoRead(fd, (char *)buf + used, n - used);
        if (got <= 0) return -1;
        used += (size_t)got;
    }
    return 0;
}

#ifdef VJO_PSP_DEFLATE
static long target_inflate(void *ctx, const void *in, size_t n, void *out, size_t cap)
{
    z_stream z;
    (void)ctx;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    int rc = inflate(&z, Z_FINISH);
    long done = (long)(cap - z.avail_out);
    inflateEnd(&z);
    /* A block ends with its stream, or fills the output first. */
    return rc == Z_STREAM_END || (!z.avail_out && (rc == Z_OK || rc == Z_BUF_ERROR)) ? done : -1;
}
#endif

static VjoPspWork work;
static unsigned char sfo[VJO_PSP_SFO_MAX];

/* The target's TITLE and DISC_ID: its format, or VJO_PSP_E_*. */
static int read_target(const char *path, char *title, size_t title_cap, char *disc, size_t disc_cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    size_t len = 0;
    if (fd < 0) return VJO_PSP_E_READ;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    VjoPspFile f = {&fd, size > 0 ? (uint64_t)size : 0, target_read, NULL};
#ifdef VJO_PSP_DEFLATE
    f.inflate = target_inflate;
#endif
    int format = size > 0 ? vjo_psp_param_sfo(&f, &work, sfo, sizeof(sfo), &len) : VJO_PSP_E_READ;
    sceIoClose(fd);
    if (format < 0) return format;
    if (vjo_sfo_string(sfo, len, "TITLE", title, title_cap) < 0) title[0] = '\0';
    if (vjo_sfo_string(sfo, len, "DISC_ID", disc, disc_cap) < 0 || !vjo_game_id_valid(disc)) disc[0] = '\0';
    return format;
}

/* The Shell's description of the bubble's game (core/pspbubble.h), rewritten
 * only when it changed. */
/* 0 when the description is unchanged, 1 written, -1 not written. */
static int describe(const VjoInstalledGame *g, const char *path)
{
    char text[VJO_PSP_INFO_MAX], old[VJO_PSP_INFO_MAX], file[96], title[sizeof(g->title)];
    int n;
    snprintf(title, sizeof(title), "%s", g->title);
    /* A title too long for the description (only a hand-made SFO has one) is
     * cut at a whole character until it fits: the description is always new. */
    while ((n = vjo_psp_info_format(text, sizeof(text), g->disc, title, path)) < 0 && title[0]) {
        size_t len = strlen(title);
        title[len > 16 ? len - 16 : 0] = '\0';
        vjo_utf8_trim_partial(title);
    }
    if (n < 0) return -1;
    snprintf(file, sizeof(file), VJO_PSP_INFO_DIR "/%s.txt", g->id);
    if (read_small(file, old, sizeof(old)) == n && !memcmp(old, text, (size_t)n)) return 0;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    sceIoMkdir(VJO_PSP_INFO_DIR, 0777);
    return write_file(file, text, (size_t)n) < 0 ? -1 : 1;
}

void vjo_psp_scan_app(VjoInstalledGame *g)
{
    static unsigned char buf[VJO_ABB_INF_MAX];
    char file[96], path[VJO_ABB_PATH_MAX] = "", title[256], disc[VJO_TITLE_ID_SIZE] = "";
    const char *how = NULL;
    int n = -1;
    /* A bubble is an app with a title ID: a folder named otherwise (ALL,
     * KERNEL, any hand-made name) is listed as it is, never as a bubble, so
     * no taiHEN section is ever offered for it. */
    if (!vjo_tai_title_ok(g->id)) return;
    /* Most apps have no data folder: one lookup for them. */
    snprintf(file, sizeof(file), "ux0:app/%s/data", g->id);
    int data = file_exists(file);
    snprintf(file, sizeof(file), "ux0:app/%s/data/boot.bin", g->id);
    /* One byte more than the layout: a longer file is the older layout too. */
    if (data) n = read_small(file, buf, VJO_ABB_BOOT_SIZE + 1);
    if (n >= 0 && vjo_abb_boot_bin(buf, (size_t)n, path, sizeof(path)) != -1) how = "boot.bin";
    if (!how && data) {
        snprintf(file, sizeof(file), "ux0:app/%s/data/boot.inf", g->id);
        n = read_small(file, buf, sizeof(buf));
        if (n >= 0) {
            how = "boot.inf";
            vjo_abb_boot_inf((const char *)buf, (size_t)n, path, sizeof(path));
        }
    }
    if (!how && strncmp(g->id, "PSPEMU", 6)) return;
    g->kind = !strcmp(g->id, "PSPEMUCFW") && !how ? VJO_ROW_ADRENALINE : VJO_ROW_BUBBLE;
    if (g->kind == VJO_ROW_ADRENALINE) {
        vjo_log("games: %s: Adrenaline", g->id);
        return;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int format = path[0] ? read_target(path, title, sizeof(title), disc, sizeof(disc)) : VJO_PSP_E_READ;
    unsigned ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000u);
    if (format > 0 && title[0]) {
        for (char *s = title; *s; s++) if ((unsigned char)*s < 32) *s = ' ';
        snprintf(g->title, sizeof(g->title), "%s", title);
    }
    snprintf(g->disc, sizeof(g->disc), "%s", disc[0] ? disc : vjo_psp_disc_id(g->id) ? g->id : "");
    /* Logged when its description is new or changed (or not written): an
     * append costs the console 12-46 ms (#81), so an unchanged bubble costs none. */
    int described = describe(g, path);
    if (described)
        vjo_log("games: bubble %s (%s): %s: %s, disc %s, \"%s\" in %u ms%s", g->id, how ? how : "PSPEMU ID",
                path[0] ? path : "no target", format > 0 ? vjo_psp_format_name(format) :
                format == VJO_PSP_E_READ ? "unreadable" : format == VJO_PSP_E_FORMAT ? "format not read" :
                format == VJO_PSP_E_NO_SFO ? "no PARAM.SFO" : "damaged image", g->disc[0] ? g->disc : "?", g->title, ms,
                described < 0 ? "; its description NOT written" : "");
}

/* A description is the Shell's sign that an ID is a bubble's: one whose ID
 * is no bubble now (deleted, or the ID used by another app) goes. */
void vjo_psp_prune(const VjoInstalledGame *games, int count)
{
    SceIoDirent e;
    char file[sizeof(VJO_PSP_INFO_DIR) + sizeof(e.d_name)];
    SceUID dir = sceIoDopen(VJO_PSP_INFO_DIR);
    if (dir < 0) return;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(dir, &e) > 0) {
        size_t n = strlen(e.d_name);
        int keep = n < 5 || strcmp(e.d_name + n - 4, ".txt"); /* not ours */
        for (int i = 0; i < count && !keep; i++)
            keep = games[i].kind == VJO_ROW_BUBBLE && strlen(games[i].id) == n - 4 && !strncmp(games[i].id, e.d_name, n - 4);
        if (keep) continue;
        snprintf(file, sizeof(file), VJO_PSP_INFO_DIR "/%s", e.d_name);
        if (sceIoRemove(file) >= 0) vjo_log("games: %s removed: no bubble has that ID now", file);
    }
    sceIoDclose(dir);
}

/* ---- taiHEN's configuration ---- */

static char tai_text[TAI_MAX + 1], tai_check[TAI_MAX + 1];
static char tai_block[VJO_TAI_LINE_MAX * 4];

/* path's text, at most TAI_MAX bytes: its length, or -1. */
static int read_config(const char *path, char *text)
{
    int n = read_small(path, text, TAI_MAX + 1);
    if (n < 0 || n > TAI_MAX) return -1;
    text[n] = '\0';
    return n;
}

/* A state's rank: blocked, absent, then loaded (VJO_TAI_PRESENT, VJO_TAI_UNDER_ALL). */
static int rank(int state) { return state == VJO_TAI_BLOCKED ? 0 : state == VJO_TAI_ABSENT ? 1 : 2; }

/* Registered means in every configuration there is, as the PC installer
 * patches them: taiHEN reads ux0:tai/config.txt when ux0 is there at boot
 * (taiHEN 309b380 plugin.c:73-79), and ur0's otherwise, as with Enso and an
 * SD2Vita card (tools/install_ftp.sh). The lowest state counts. */
void vjo_psp_scan_tai(VjoInstalledGame *games, int count)
{
    const char *const paths[2] = {VJO_TAI_UR0, VJO_TAI_UX0};
    for (int p = 0; p < 2; p++) {
        if (p && !file_exists(VJO_TAI_UX0)) break;
        int n = read_config(paths[p], tai_text);
        for (int i = 0; i < count; i++) {
            if (!vjo_catalog_registrable(&games[i])) continue;
            int state = n < 0 ? VJO_TAI_BLOCKED : vjo_tai_worker_state(tai_text, (size_t)n, games[i].id);
            if (!p || rank(state) < rank(games[i].tai)) games[i].tai = (signed char)state;
        }
    }
}

/* Free bytes on path's device ("ur0:", "ux0:"), or -1 when it does not say. */
static long long free_bytes(const char *path)
{
    SceIoDevInfo info;
    char dev[5];
    memcpy(dev, path, 4);
    dev[4] = '\0';
    memset(&info, 0, sizeof(info));
    return sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof(info)) < 0 ? -1 : (long long)info.free_size;
}

/* path back to its verified n bytes (tai_text): cut back to that length,
 * else written again whole. 1 when it reads back as them. */
static int restore(const char *path, int n)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    st.st_size = n;
    if (sceIoChstat(path, &st, SCE_CST_SIZE) < 0 || read_config(path, tai_check) != n ||
        memcmp(tai_check, tai_text, (size_t)n))
        write_file(path, tai_text, (size_t)n);
    sceIoSync(path[1] == 'x' ? "ux0:" : "ur0:", 0);
    return read_config(path, tai_check) == n && !memcmp(tai_check, tai_text, (size_t)n);
}

/* One configuration: 0 changed or nothing to change, -1 with why. */
static int register_in(const char *path, const char *id, const char **why)
{
    char backup[64];
    int n = read_config(path, tai_text);
    if (n < 0) { *why = "it cannot be read"; return -1; }
    int state = vjo_tai_worker_state(tai_text, (size_t)n, id);
    if (state == VJO_TAI_PRESENT || state == VJO_TAI_UNDER_ALL) return 0;
    if (state != VJO_TAI_ABSENT) { *why = "taiHEN stops reading it early (a long line or *!)"; return -1; }
    /* A file of a multiple of 4096 B is one taiHEN reads past (its buffer is the size rounded up to
     * 4 KiB, with no NUL after it): it is not kept as the copy, which a later "copy it back" would
     * reinstate, and not added to. */
    if (n % 4096 == 0) { *why = n ? "taiHEN cannot read it safely at this length (a multiple of 4,096 B)" : "it is empty"; return -1; }
    int add = vjo_tai_worker_block(tai_block, sizeof(tai_block), tai_text, (size_t)n, id);
    if (add < 0 || (size_t)n + (size_t)add > TAI_MAX) { *why = "the entry does not fit"; return -1; }
    /* Room first. A full card cuts an append at a cluster, a multiple of 4096 B,
     * the one size taiHEN reads past: its buffer is the file's size rounded up
     * to 4 KiB, and the parser reads the text as a C string (plugin.c:95-96). */
    long long room = free_bytes(path);
    if (room >= 0 && room < (long long)n + add + TAI_ROOM) { *why = "its memory card is full"; return -1; }
    /* The copy first: the configuration is only appended to once it is kept. */
    snprintf(backup, sizeof(backup), "%s" VJO_TAI_APP_BACKUP, path);
    if (write_file(backup, tai_text, (size_t)n) < 0 || read_config(backup, tai_check) != n ||
        memcmp(tai_check, tai_text, (size_t)n)) { *why = "its backup could not be written"; return -1; }
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_APPEND, 0);
    int rc = fd < 0 ? -1 : write_all(fd, tai_block, (size_t)add);
    if (fd >= 0 && sceIoClose(fd) < 0) rc = -1;
    sceIoSync(path[1] == 'x' ? "ux0:" : "ur0:", 0);
    /* Checked: the old text, then the block, and nothing else. */
    int m = read_config(path, tai_check);
    if (rc == 0 && m == n + add && !memcmp(tai_check, tai_text, (size_t)n) && !memcmp(tai_check + n, tai_block, (size_t)add) &&
        vjo_tai_worker_state(tai_check, (size_t)m, id) == VJO_TAI_PRESENT) {
        vjo_log("taiHEN: *%s added to %s (backup %s)", id, path, backup);
        return 0;
    }
    /* Not confirmed (a full card, an I/O error): back to its old text, whatever
     * part of the block the append left, since that part could end the file
     * at a multiple of 4096 B. Not when the old text is no longer its start:
     * something else changed it. */
    int intact = m >= n && !memcmp(tai_check, tai_text, (size_t)n);
    int restored = (m < 0 || (intact && m > n)) ? restore(path, n) : intact;
    vjo_log("taiHEN: *%s not confirmed in %s: %d B after its %d B (meant %d); %s; backup %s", id, path, m < 0 ? m : m - n,
            n, add, restored ? "its old text is back" : intact || m < 0 ? "NOT put back" : "it changed meanwhile", backup);
    *why = restored ? "the write was not confirmed; it is as it was"
                    : intact || m < 0 ? "the write failed: copy config.txt.vjo-app-bak back over it"
                                      : "it changed meanwhile: compare it with config.txt.vjo-app-bak";
    return -1;
}

int vjo_psp_register(VjoInstalledGame *g, char *msg, size_t cap)
{
    const char *why = "", *where = VJO_TAI_UR0;
    int rc = 0;
    if (!vjo_tai_title_ok(g->id)) { /* never ALL, KERNEL or main (vjo_psp_scan_app) */
        snprintf(msg, cap, "Not registered: %s is not a title ID.", g->id);
        return -1;
    }
    /* Both, as the PC installer does: Enso reads ur0's at boot, taiHEN ux0's when it is there. */
    if (file_exists(VJO_TAI_UX0) && register_in(VJO_TAI_UX0, g->id, &why) < 0) { rc = -1; where = VJO_TAI_UX0; }
    if (!rc && register_in(VJO_TAI_UR0, g->id, &why) < 0) rc = -1;
    vjo_psp_scan_tai(g, 1);
    if (rc < 0)
        snprintf(msg, cap, "Not registered: %s: %s. Use install-vita.sh --title %s.", where, why, g->id);
    else
        snprintf(msg, cap, "%s registered for local OCR. Restart the Vita before you start it.", g->id);
    return rc;
}
