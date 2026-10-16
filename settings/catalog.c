#include "catalog.h"
#include "anki_page.h" /* vjo_log: log.txt */
#include "letter.h"
#include "psp.h"
#include "../core/sfo.h"
#include "../core/utf.h"
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* By letter, then alphabetically with capital and small letters together
 * ("echo Garden" with the E titles), then by title ID (settings/letter.c):
 * each letter's games are one run, for Left/Right. */
static int name_order(const void *a, const void *b)
{
    const VjoInstalledGame *ga = a, *gb = b;
    return vjo_title_order(ga->title, ga->id, gb->title, gb->id);
}

/* The TITLE of path's PARAM.SFO in out, 1; out unchanged and 0 when there is none. */
static int read_title(const char *path, char *out, size_t cap)
{
    /* One bounded SFO buffer reused during enumeration, never while drawing. */
    static unsigned char sfo[64 * 1024];
    int got = 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size > 0 && (size_t)size <= sizeof(sfo)) {
        size_t used = 0;
        while (used < (size_t)size) {
            int n = sceIoRead(fd, sfo + used, (size_t)size - used);
            if (n <= 0) break;
            used += n;
        }
        if (used == (size_t)size) got = vjo_sfo_string(sfo, used, "TITLE", out, cap) == 0;
    }
    sceIoClose(fd);
    /* Some game names contain line breaks, which do not belong in list rows. */
    for (char *s = out; *s; s++) if ((unsigned char)*s < 32) *s = ' ';
    return got;
}

/* ---- log.txt: one line per root and one for the rows, each one vjo_log line ---- */

/* A line built in pieces, cut at what one vjo_log line holds (settings/applog.c:
 * 300 bytes with "overlay app: " and the line break). */
typedef struct { char text[286]; size_t used; } Line;
static void add(Line *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(Line *l, const char *fmt, ...)
{
    va_list ap;
    if (l->used >= sizeof(l->text) - 1) return;
    va_start(ap, fmt);
    int n = vsnprintf(l->text + l->used, sizeof(l->text) - l->used, fmt, ap);
    va_end(ap);
    if (n > 0) l->used = l->used + (size_t)n < sizeof(l->text) ? l->used + (size_t)n : sizeof(l->text) - 1;
}
/* An sceIo result: its hex and, for the usual errors, the SDK's errno name
 * (SDK_doc__1st_read__Error-Overview_e.txt: 0x800100xx are the errno values). */
static void add_rc(Line *l, int rc)
{
    static const struct { unsigned rc; const char *name; } names[] = {
        {0x80010001u, "EPERM"}, {0x80010002u, "ENOENT"}, {0x80010005u, "EIO"}, {0x8001000Du, "EACCES"},
        {0x80010010u, "EBUSY"}, {0x80010013u, "ENODEV"}, {0x80010014u, "ENOTDIR"}, {0x80010018u, "EMFILE"},
        {0x80010030u, "EUNSUP"}};
    add(l, "0x%08X", (unsigned)rc);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].rc == (unsigned)rc) add(l, " (%s)", names[i].name);
}
/* A list's names, kept until its line is written: each item and a line break,
 * in what fits ("lost" counts the rest). */
typedef struct { char text[640]; size_t used; int count, lost; } Names;
static void name(Names *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void name(Names *n, const char *fmt, ...)
{
    char item[96];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(item, sizeof(item), fmt, ap);
    va_end(ap);
    n->count++;
    if (len < 0 || (size_t)len >= sizeof(item) || n->used + (size_t)len + 1 >= sizeof(n->text)) {
        n->lost++;
        return;
    }
    memcpy(n->text + n->used, item, (size_t)len);
    n->used += (size_t)len;
    n->text[n->used++] = '\n';
    n->text[n->used] = '\0';
}
/* The names, ", " between them: all when they fit in the line, else as many
 * as fit with room kept for " and N more", which counts the rest. */
static void add_names(Line *l, const Names *n);
/* "; label" and the names, only when the label and " and 999 more" fit after
 * it: a full line drops a section whole (its count is in the line's head),
 * never cuts a label or a name. */
static void add_section(Line *l, const char *label, const Names *n)
{
    if (!n->count || l->used + strlen(label) + 16 > sizeof(l->text) - 1) return;
    add(l, "%s", label);
    add_names(l, n);
}
static void add_names(Line *l, const Names *n)
{
    size_t cap = sizeof(l->text) - 1, stored = (size_t)(n->count - n->lost);
    size_t all = stored ? n->used + stored - 2 : 0; /* the items, a line break each, joined by ", " */
    int reserve = n->lost || l->used + all > cap, more = 0, first = 1;
    for (const char *s = n->text; *s;) {
        const char *end = strchr(s, '\n');
        size_t len = (size_t)(end - s), need = len + (first ? 0 : 2);
        if (more || l->used + need + (reserve ? 16 : 0) > cap) more++; /* 16: " and 999 more" */
        else {
            add(l, "%s%.*s", first ? "" : ", ", (int)len, s);
            first = 0;
        }
        s = end + 1;
    }
    more += n->lost;
    if (more) add(l, "%sand %d more", first ? "" : " ", more);
}

/* A folder's name for the log: at most 24 bytes, cut at a whole UTF-8
 * character, "..." when cut. */
static const char *short_name(const char *name, char out[32])
{
    if (strlen(name) <= 24) return name;
    memcpy(out, name, 24);
    out[24] = '\0';
    vjo_utf8_trim_partial(out);
    strcat(out, "...");
    return out;
}

/* A title with a choice of its own that no folder had: its row says so. Its
 * name is its update's TITLE when it has one, as a game card's game can (the
 * update is installed to ux0:patch/ID, whose sce_sys/param.sfo VitaShell reads
 * as it is, refresh.c refreshNeeded), else its ID; its icon, the update's. */
static void missing_row(VjoInstalledGame *g, const char *id)
{
    char path[64], label[sizeof(g->title) - sizeof(VJO_CATALOG_MISSING_NOTE)];
    memset(g, 0, sizeof(*g));
    memcpy(g->id, id, strlen(id) + 1);
    g->kind = VJO_ROW_MISSING;
    snprintf(label, sizeof(label), "%s", id);
    snprintf(path, sizeof(path), "ux0:patch/%s/sce_sys/param.sfo", id);
    if (read_title(path, label, sizeof(label)))
        snprintf(g->icon, sizeof(g->icon), "ux0:patch/%s/sce_sys/icon0.png", id);
    snprintf(g->title, sizeof(g->title), "%s" VJO_CATALOG_MISSING_NOTE, label);
}

int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated, const VjoGames *rules)
{
    static const char *const roots[] = {"ux0:app", "ur0:app", "gro0:app"};
    int count = 0, ux0_listed = 0, from_folders = 0, shared = 0, left_out = 0;
    *truncated = 0;
    /* The scan's lines go out in one append, after it (settings/applog.c);
     * a bubble's line (settings/psp.c) is one of them. */
    vjo_app_log_batch_begin();
    for (unsigned r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        Line line = {"", 0};
        Names skipped = {"", 0, 0, 0}, untitled = {"", 0, 0, 0};
        int entries = 0, listed = 0, duplicates = 0, dropped = 0, end;
        add(&line, "games: %s: ", roots[r]);
        SceUID dir = sceIoDopen(roots[r]);
        if (dir < 0) {
            /* The device itself (gro0:, ur0:): not there at all, or there and
             * the folder refused. VitaShell lists a device when sceIoGetstat on
             * it succeeds (81af709 file.c:963). */
            char device[8];
            SceIoStat st;
            snprintf(device, sizeof(device), "%.*s", (int)(strchr(roots[r], ':') - roots[r] + 1), roots[r]);
            add(&line, "not opened: ");
            add_rc(&line, dir);
            int dev = sceIoGetstat(device, &st);
            add(&line, "; %s ", device);
            if (dev < 0) add_rc(&line, dev);
            else add(&line, "is there");
            vjo_log("%s", line.text);
            continue;
        }
        SceIoDirent e;
        memset(&e, 0, sizeof(e));
        while ((end = sceIoDread(dir, &e)) > 0) {
            entries++;
            const char *why = !SCE_S_ISDIR(e.d_stat.st_mode) ? "not a folder" : vjo_game_reject_reason(e.d_name);
            if (why) {
                char cut[32];
                name(&skipped, "%s (%s)", short_name(e.d_name, cut), why);
                continue;
            }
            int found = 0;
            for (int i = 0; i < count; i++) if (!strcmp(out[i].id, e.d_name)) found = 1;
            if (found) { duplicates++; continue; }
            if (count >= capacity) { *truncated = 1; dropped++; continue; }
            VjoInstalledGame *g = &out[count++];
            /* Candidate validation has already bounded the complete title ID. */
            memset(g, 0, sizeof(*g));
            memcpy(g->id, e.d_name, strlen(e.d_name) + 1);
            snprintf(g->title, sizeof(g->title), "%s", g->id);
            snprintf(g->icon, sizeof(g->icon), "%s/%s/sce_sys/icon0.png", roots[r], g->id);
            char path[160];
            snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", roots[r], g->id);
            int titled = read_title(path, g->title, sizeof(g->title));
            /* AdrBubbleBooter looks for bubbles in ux0:app only. */
            if (r == 0) vjo_psp_scan_app(g);
            /* No title read (a bubble's may come from its game): listed under its
             * ID, a letter its player may not look under. */
            if (!titled && !strcmp(g->title, g->id)) name(&untitled, "%s", g->id);
            listed++;
        }
        sceIoDclose(dir);
        /* Only a whole listing says which bubbles are gone (vjo_psp_prune below). */
        ux0_listed |= r == 0 && end == 0;
        from_folders += listed;
        left_out += dropped;
        /* Every count first, then the names, the skipped ones (with their
         * reasons) before those listed under their IDs: a line too long loses
         * names only, the later section first. */
        add(&line, "%d entries: %d listed, %d already listed, %d skipped", entries, listed, duplicates, skipped.count);
        if (untitled.count) add(&line, ", %d listed under its ID (no title read)", untitled.count);
        if (dropped) add(&line, ", %d left out: the list holds %d", dropped, capacity);
        if (end < 0) {
            add(&line, "; the listing stopped at ");
            add_rc(&line, end);
        }
        add_section(&line, "; skipped: ", &skipped);
        add_section(&line, "; under its ID: ", &untitled);
        vjo_log("%s", line.text);
    }
    /* Adrenaline can be installed under a homebrew ID but runs NPXS10028: the
     * row for PSP games without a row of their own. */
    int psp = 0;
    for (int i = 0; i < count; i++) if (!strcmp(out[i].id, VJO_PSP_EMULATOR)) psp = 1;
    if (!psp && count < capacity) {
        VjoInstalledGame *g = &out[count++];
        memset(g, 0, sizeof(*g));
        strcpy(g->id, VJO_PSP_EMULATOR);
        strcpy(g->title, VJO_CATALOG_PSP_TITLE);
        g->kind = VJO_ROW_PSP_SHARED;
        shared = 1;
    }
    /* Only a complete list of ux0:app's bubbles says which descriptions are stale. */
    if (ux0_listed && !*truncated) vjo_psp_prune(out, count);
    /* A choice of its own for a title no folder had (CLANNAD in #138's run):
     * listed, so it can be seen and changed. A rule the Shell never applies
     * (not a game's ID) is not. */
    Names missing = {"", 0, 0, 0};
    int missing_titles = 0;
    for (int i = 0; rules && i < rules->count && i < VJO_GAMES_MAX; i++) {
        const char *id = rules->rules[i].title_id;
        if (!vjo_game_is_candidate(id)) continue;
        int found = 0;
        for (int k = 0; k < count && !found; k++) found = !strcmp(out[k].id, id);
        if (found) continue;
        missing_titles++;
        name(&missing, "%s = %s", id, rules->rules[i].enabled ? "enabled" : "disabled");
        if (count >= capacity) { *truncated = 1; left_out++; continue; }
        missing_row(&out[count++], id);
    }
    qsort(out, count, sizeof(*out), name_order);
    /* The rows, and the app's permissions as VitaShell checks for its safe mode
     * (HENkaku's Enable Unsafe Homebrew off: VitaShell init.c:353-355), in which
     * VitaShell offers ux0: alone (file.c:958). */
    Line line = {"", 0};
    int devctl = sceIoDevctl("ux0:", 0x3001, NULL, 0, NULL, 0);
    add(&line, "games: %d rows (the list holds %d): %d from the folders%s, %d only in games.ini", count, capacity,
        from_folders, shared ? ", the shared PSP row" : "", missing_titles);
    if (left_out) add(&line, ", %d left out", left_out);
    add(&line, "; safe mode %s (ux0: devctl 0x%08X)", devctl == (int)0x80010030 ? "yes" : "no", (unsigned)devctl);
    add_section(&line, "; only in games.ini: ", &missing);
    vjo_log("%s", line.text);
    vjo_app_log_batch_end();
    return count;
}
