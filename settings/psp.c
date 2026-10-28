#include "psp.h"
#include "anki_page.h" /* vjo_log: log.txt */
#include "../core/pspbubble.h"
#include "../core/psptarget.h"
#include "../core/sfo.h"
#include "../core/taicfg.h" /* vjo_tai_title_ok */
#include "../core/utf.h"
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

/* The target's TITLE and DISC_ID (its PARAM.SFO read into sfo, cap bytes): its format, or VJO_PSP_E_*. */
static int read_target(const char *path, unsigned char *sfo, size_t cap, char *title, size_t title_cap, char *disc,
                       size_t disc_cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    size_t len = 0;
    if (fd < 0) return VJO_PSP_E_READ;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    VjoPspFile f = {&fd, size > 0 ? (uint64_t)size : 0, target_read, NULL};
#ifdef VJO_PSP_DEFLATE
    f.inflate = target_inflate;
#endif
    int format = size > 0 ? vjo_psp_param_sfo(&f, &work, sfo, cap, &len) : VJO_PSP_E_READ;
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

void vjo_psp_scan_app(VjoInstalledGame *g, unsigned char *sfo, size_t cap)
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
    int format = path[0] ? read_target(path, sfo, cap, title, sizeof(title), disc, sizeof(disc)) : VJO_PSP_E_READ;
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
