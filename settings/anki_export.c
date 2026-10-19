#include "anki_export.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>

#include "../core/anki_queue.h"
#include "../core/errors.h"

void vjo_log(const char *fmt, ...); /* the app's log.txt lines (settings/applog.c) */

#define MISSING ((int)0x80010002u) /* ENOENT */
#define JPEG_MAGIC "\xFF\xD8\xFF"

static int io_fail(const char *call, const char *path, int rc)
{
    vjo_log("anki export: %s %s failed 0x%08X", call, path, (unsigned)rc);
    return -1;
}

/* The same, as the package's error. */
static int io_error(const char *call, const char *path, int rc)
{
    io_fail(call, path, rc);
    return VJO_E_LOCAL_IO;
}

static void to_hex(const uint8_t id[16], char out[VJO_QUEUE_ID_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = digits[id[i] >> 4];
        out[2 * i + 1] = digits[id[i] & 15];
    }
    out[32] = '\0';
}

static void from_hex(const char *hex, uint8_t id[16])
{
    for (int i = 0; i < 16; i++) {
        int hi = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10;
        int lo = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        id[i] = (uint8_t)(hi << 4 | lo);
    }
}

static void card_path(char path[VJO_EXPORT_PATH_MAX], int archive, const char *hex, const char *ext)
{
    snprintf(path, VJO_EXPORT_PATH_MAX, "%s/%s.%s", archive ? VJO_EXPORT_ARCHIVE_DIR : VJO_EXPORT_QUEUE_DIR, hex,
             ext);
}

static int is_dir(const char *path)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

/* The <32 hex digits>.json cards of a directory: fn for each (it returns
 * < 0 to stop). The count, or -1 (a directory that cannot be read; one that
 * does not exist has none). */
static int scan(const char *dir, int (*fn)(void *ud, const char *hex), void *ud)
{
    SceUID d;
    SceIoDirent ent;
    int n = 0, rc, closed;
    if (!is_dir(dir))
        return 0;
    if ((d = sceIoDopen(dir)) < 0)
        return io_fail("dopen", dir, d);
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if ((rc = sceIoDread(d, &ent)) <= 0)
            break;
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || strlen(ent.d_name) != 37 || strcmp(ent.d_name + 32, ".json"))
            continue;
        ent.d_name[32] = '\0';
        if (!vjo_queue_valid_id(ent.d_name))
            continue;
        n++;
        if (fn && fn(ud, ent.d_name) < 0) {
            rc = 0;
            break;
        }
    }
    if (rc < 0)
        io_fail("dread", dir, rc);
    if ((closed = sceIoDclose(d)) < 0)
        return io_fail("dclose", dir, closed);
    return rc < 0 ? -1 : n;
}

void vjo_export_count(VjoExportCounts *c)
{
    c->queued = scan(VJO_EXPORT_QUEUE_DIR, NULL, NULL);
    c->exported = scan(VJO_EXPORT_ARCHIVE_DIR, NULL, NULL);
}

/* A whole file of at most max bytes into a (NUL-terminated), or NULL. error distinguishes an
 * absent/damaged image, whose card can export without it, from I/O or memory trouble to retry. */
static char *read_file(VjoArena *a, const char *path, size_t max, size_t *len, int quiet_missing, int *error)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    char *data = NULL;
    size_t off = 0;
    int n;
    if (error)
        *error = VJO_E_LOCAL_IO;
    if (fd < 0) {
        if (error && fd == MISSING)
            *error = MISSING;
        if (!(quiet_missing && fd == MISSING))
            io_fail("open", path, fd);
        return NULL;
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) {
        io_fail("seek", path, (int)size);
        sceIoClose(fd);
        return NULL;
    }
    if (!size || (uint64_t)size > max) {
        vjo_log("anki export: %s: size %d (limit %u)", path, (int)size, (unsigned)max);
        if (error)
            *error = VJO_E_PARSE;
        sceIoClose(fd);
        return NULL;
    }
    if (sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) {
        vjo_log("anki export: %s: cannot seek to the start", path);
        sceIoClose(fd);
        return NULL;
    }
    if (!(data = (char *)vjo_arena_alloc(a, (size_t)size + 1))) {
        vjo_log("anki export: %s: out of memory reading %u bytes", path, (unsigned)size);
        if (error)
            *error = VJO_E_OOM;
        sceIoClose(fd);
        return NULL;
    }
    while (off < (size_t)size) {
        n = sceIoRead(fd, data + off, (SceSize)((size_t)size - off));
        if (n <= 0) {
            io_fail("read", path, n);
            sceIoClose(fd);
            return NULL;
        }
        off += (size_t)n;
    }
    if ((n = sceIoClose(fd)) < 0) {
        io_fail("close", path, n);
        return NULL;
    }
    data[off] = '\0';
    *len = off;
    if (error)
        *error = 0;
    return data;
}

/* A card's screenshot, trying both directories even when the first copy is damaged. Failed
 * reads release their scratch before the next try. unavailable is true only when neither copy
 * exists or is a bounded, complete JPEG; I/O failures and OOM keep the card queued for retry. */
static const uint8_t *read_jpeg(VjoArena *a, int archive, const char *hex, size_t *len, int *unavailable)
{
    char path[VJO_EXPORT_PATH_MAX];
    const uint8_t *p;
    size_t mark = vjo_arena_mark(a);
    int retry = 0;
    if (unavailable)
        *unavailable = 0;
    for (int k = 0; k < 2; k++) {
        int error;
        card_path(path, k ? !archive : archive, hex, "jpg");
        p = (const uint8_t *)read_file(a, path, VJO_QUEUE_PICTURE_MAX, len, 1, &error);
        if (p && *len >= 6 && !memcmp(p, JPEG_MAGIC, 3) && p[*len - 2] == 0xFF && p[*len - 1] == 0xD9)
            return p;
        if (p)
            vjo_log("anki export: %s: missing JPEG start or end marker", path);
        else if (error != MISSING && error != VJO_E_PARSE)
            retry = 1;
        vjo_arena_release(a, mark);
    }
    *len = 0;
    if (unavailable)
        *unavailable = !retry;
    vjo_log("anki export: %s: screenshot %s", hex,
            retry ? "could not be read; retry later" : "missing or damaged; exporting text without it");
    return NULL;
}

/* A card's record, checked against its name (the ID is its checksum) and
 * decoded into note. 0, or -1 (logged). */
static int read_card(VjoArena *a, int archive, const char *hex, VjoAnkiNote *note, int *picture)
{
    char path[VJO_EXPORT_PATH_MAX], actual[VJO_QUEUE_ID_SIZE], *json;
    size_t len;
    card_path(path, archive, hex, "json");
    if (!(json = read_file(a, path, VJO_QUEUE_TEXT_MAX, &len, 0, NULL)))
        return -1;
    vjo_queue_id(json, len, actual);
    if (strcmp(actual, hex)) {
        vjo_log("anki export: %s: checksum mismatch (edited or damaged)", path);
        return -1;
    }
    if (vjo_queue_decode(a, json, len, note, picture) < 0) {
        vjo_log("anki export: %s: invalid or unsupported card format", path);
        return -1;
    }
    return 0;
}

/* ---------------- the list of cards ---------------- */

typedef struct {
    VjoArena *a;
    VjoApkgCard *cards;
    int n, cap, archive, seen, total;
    int unreadable, left;
    const VjoExportUi *ui;
} List;

static int newer(const VjoApkgCard *x, const VjoApkgCard *y)
{
    return x->time != y->time ? x->time > y->time : memcmp(x->id, y->id, 16) > 0;
}

/* The list keeps the cap oldest cards: a heap with the newest on top once it is full. */
static void heap_down(VjoApkgCard *c, int n, int i)
{
    for (;;) {
        int k = 2 * i + 1;
        VjoApkgCard t;
        if (k >= n)
            return;
        if (k + 1 < n && newer(&c[k + 1], &c[k]))
            k++;
        if (!newer(&c[k], &c[i]))
            return;
        t = c[i];
        c[i] = c[k];
        c[k] = t;
        i = k;
    }
}

static int status_every(const VjoExportUi *ui, const char *what, int done, int total)
{
    char msg[96];
    if (ui->stopped && ui->stopped(ui->ud))
        return -1;
    if (ui->status && (done % 8 == 0 || done == total)) {
        snprintf(msg, sizeof(msg), "%s… %d of %d", what, done, total);
        ui->status(ui->ud, msg);
    }
    return 0;
}

static int list_card(void *ud, const char *hex)
{
    List *l = (List *)ud;
    size_t mark = vjo_arena_mark(l->a);
    VjoAnkiNote note;
    VjoApkgCard c;
    int picture = 0;
    if (status_every(l->ui, "Reading the cards", ++l->seen, l->total) < 0)
        return -1;
    memset(&c, 0, sizeof(c));
    if (read_card(l->a, l->archive, hex, &note, &picture) < 0) {
        l->unreadable++;
        vjo_arena_release(l->a, mark);
        return 0;
    }
    if (picture) {
        size_t len;
        int unavailable;
        if (!read_jpeg(l->a, l->archive, hex, &len, &unavailable)) {
            if (!unavailable) {
                l->unreadable++;
                vjo_arena_release(l->a, mark);
                return 0;
            }
            c.missing_picture = 1; /* original record and GUID kept; the result reports the omission */
            picture = 0;
        }
    }
    vjo_arena_release(l->a, mark);
    from_hex(hex, c.id);
    c.time = note.unix_time > 0 ? note.unix_time : VJO_APKG_NO_TIME;
    c.exported = (uint8_t)l->archive;
    c.picture = (uint8_t)!!picture;
    if (l->n < l->cap) {
        l->cards[l->n++] = c;
        if (l->n == l->cap) /* full: from now on the newest goes */
            for (int i = l->n / 2 - 1; i >= 0; i--)
                heap_down(l->cards, l->n, i);
    } else {
        l->left++;
        if (newer(&l->cards[0], &c)) {
            l->cards[0] = c;
            heap_down(l->cards, l->n, 0);
        }
    }
    return 0;
}

/* One entry per card ID (sorted): the queued copy when both exist. */
static int dedupe(VjoApkgCard *c, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && !memcmp(c[k - 1].id, c[i].id, 16)) {
            if (!c[i].exported)
                c[k - 1] = c[i];
            continue;
        }
        c[k++] = c[i];
    }
    return k;
}

/* ---------------- the package file ---------------- */

typedef struct {
    SceUID fd;
    const VjoExportUi *ui;
    int total;
} Out;

static int out_write(void *ud, uint64_t off, const void *src, size_t n)
{
    const Out *o = (const Out *)ud;
    size_t done = 0;
    while (done < n) {
        int k = sceIoPwrite(o->fd, (const char *)src + done, (SceSize)(n - done), (SceOff)(off + done));
        if (k <= 0) {
            io_fail("write", "the package", k);
            return VJO_E_LOCAL_IO;
        }
        done += (size_t)k;
    }
    return 0;
}

static int out_read(void *ud, uint64_t off, void *dst, size_t n)
{
    const Out *o = (const Out *)ud;
    size_t done = 0;
    while (done < n) {
        int k = sceIoPread(o->fd, (char *)dst + done, (SceSize)(n - done), (SceOff)(off + done));
        if (k <= 0) {
            io_fail("read", "the package", k);
            return VJO_E_LOCAL_IO;
        }
        done += (size_t)k;
    }
    return 0;
}

static int load(void *ud, VjoArena *a, const VjoApkgCard *c, VjoAnkiNote *note, const uint8_t **jpeg, size_t *len)
{
    char hex[VJO_QUEUE_ID_SIZE];
    int picture = 0;
    (void)ud;
    to_hex(c->id, hex);
    *jpeg = NULL;
    *len = 0;
    if (note && (read_card(a, c->exported, hex, note, &picture) < 0 ||
                 !!picture != !!(c->picture || c->missing_picture)))
        return -1;
    if (c->picture && !(*jpeg = read_jpeg(a, c->exported, hex, len, NULL)))
        return -1;
    return 0;
}

static void progress(void *ud, int done, int total, const char *step)
{
    const Out *o = (const Out *)ud;
    char msg[96];
    if (!o->ui->status)
        return;
    if (!strcmp(step, "notes")) {
        if (done % 8 && done != total)
            return;
        snprintf(msg, sizeof(msg), "Writing the cards… %d of %d", done, total);
    } else {
        snprintf(msg, sizeof(msg), "%s", !strcmp(step, "media") ? "Adding the screenshots…" : "Checking the file…");
    }
    o->ui->status(o->ui->ud, msg);
}

static int out_stopped(void *ud)
{
    const Out *o = (const Out *)ud;
    return o->ui->stopped && o->ui->stopped(o->ui->ud);
}

/* ux0:data/VitaJPOverlay/anki/<YYYY-MM-DD_HHMMSS>[-all]-<N>cards[-k].apkg: a
 * name no file has yet. N is the notes the package holds. */
static int package_name(const SceDateTime *t, int all, int cards, char *name, size_t cap, char *path)
{
    for (int k = 1; k < 100; k++) {
        SceIoStat st;
        char suffix[8] = "";
        if (k > 1)
            snprintf(suffix, sizeof(suffix), "-%d", k);
        snprintf(name, cap, "%04d-%02d-%02d_%02d%02d%02d%s-%dcard%s%s.apkg", t->year, t->month, t->day, t->hour,
                 t->minute, t->second, all ? "-all" : "", cards, cards == 1 ? "" : "s", suffix);
        snprintf(path, VJO_EXPORT_PATH_MAX, "%s/%s", VJO_EXPORT_DIR, name);
        if (sceIoGetstat(path, &st) < 0)
            return 0;
    }
    return -1;
}

/* An archived record that is this card's, byte for byte: its checksum is its
 * name (a damaged or hand-copied file is not). */
static int archived_copy_good(VjoArena *a, const char *hex)
{
    char path[VJO_EXPORT_PATH_MAX], actual[VJO_QUEUE_ID_SIZE];
    size_t mark = vjo_arena_mark(a), len;
    char *json;
    int good;
    card_path(path, 1, hex, "json");
    json = read_file(a, path, VJO_QUEUE_TEXT_MAX, &len, 0, NULL);
    if (json)
        vjo_queue_id(json, len, actual);
    good = json && !strcmp(actual, hex);
    vjo_arena_release(a, mark);
    return good;
}

/* Moves a card that is now in a verified package out of the queue: its
 * JPEG first, then its record (a card is in the queue while its record
 * is). 0, or -1 (it stays queued, harmlessly: its note's GUID is the same). */
static int archive_card(VjoArena *a, const VjoApkgCard *c)
{
    char hex[VJO_QUEUE_ID_SIZE], from[VJO_EXPORT_PATH_MAX], to[VJO_EXPORT_PATH_MAX];
    SceIoStat st;
    int rc;
    to_hex(c->id, hex);
    if (c->picture) {
        card_path(from, 0, hex, "jpg");
        card_path(to, 1, hex, "jpg");
        if (sceIoGetstat(from, &st) >= 0 && sceIoGetstat(to, &st) < 0 && (rc = sceIoRename(from, to)) < 0)
            return io_fail("rename", from, rc);
    }
    card_path(from, 0, hex, "json");
    card_path(to, 1, hex, "json");
    if (sceIoGetstat(to, &st) >= 0) {
        /* the same record archived already: the queued copy goes; a damaged one there is replaced */
        if (archived_copy_good(a, hex)) {
            if ((rc = sceIoRemove(from)) < 0)
                return io_fail("remove", from, rc);
            return 0;
        }
        vjo_log("anki export: %s: the archived copy is damaged: replaced by the queued one", to);
        if ((rc = sceIoRemove(to)) < 0)
            return io_fail("remove", to, rc);
    }
    if ((rc = sceIoRename(from, to)) < 0)
        return io_fail("rename", from, rc);
    /* When both JPEGs exist, keep the queued copy too: the archived one may be damaged.
     * read_jpeg tries both and named() protects this copy from the orphan sweep. */
    return 0;
}

static void size_text(uint32_t bytes, char *out, size_t cap)
{
    if (bytes >= 1024u * 1024u)
        snprintf(out, cap, "%u.%u MB", (unsigned)(bytes / (1024u * 1024u)),
                 (unsigned)(bytes % (1024u * 1024u) * 10u / (1024u * 1024u)));
    else
        snprintf(out, cap, "%u KB", (unsigned)((bytes + 1023u) / 1024u));
}

/* A package an interrupted export left under its temporary name is never a
 * complete one: removed before the next. */
static void remove_stale_temps(void)
{
    SceUID d;
    SceIoDirent ent;
    char path[VJO_EXPORT_PATH_MAX + 64];
    if (!is_dir(VJO_EXPORT_DIR) || (d = sceIoDopen(VJO_EXPORT_DIR)) < 0)
        return;
    for (;;) {
        size_t n;
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0)
            break;
        n = strlen(ent.d_name);
        if (SCE_S_ISREG(ent.d_stat.st_mode) && n > 9 && !strcmp(ent.d_name + n - 9, ".apkg.tmp")) {
            /* (a name too long for the path is not ours: never remove a truncated path) */
            if (snprintf(path, sizeof(path), "%s/%s", VJO_EXPORT_DIR, ent.d_name) >= (int)sizeof(path))
                continue;
            vjo_log("anki export: %s: an interrupted export's, removed", path);
            sceIoRemove(path);
        }
    }
    sceIoDclose(d);
}

/* ---------------- screenshots no card names (#161) ---------------- */

/* A file time in seconds from a fixed day (only differences are used), or -1 for none (a time
 * before 1980, FAT's first, or out of range). */
static int64_t file_seconds(const SceDateTime *t)
{
    int64_t y = t->year, m = t->month, era, yoe, doy, doe;
    if (y < 1980 || m < 1 || m > 12 || t->day < 1 || t->day > 31 || t->hour > 23 || t->minute > 59 || t->second > 60)
        return -1;
    /* days from the civil date (H. Hinnant's days_from_civil, for years from 1980) */
    y -= m <= 2;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + t->day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return ((era * 146097 + doe) * 24 + t->hour) * 3600 + t->minute * 60 + t->second;
}

/* 1: the file is there, 0: it is not, -1: not known (a failed getstat that is not "missing"). */
static int present(const char *path)
{
    SceIoStat st;
    int rc;
    memset(&st, 0, sizeof(st));
    rc = sceIoGetstat(path, &st);
    return rc >= 0 ? 1 : rc == MISSING ? 0 : -1;
}

/* Some record may name this ID: queued, exported (a card's JPEG copy can stay beside its archived
 * record; read_jpeg reads either directory), or an earlier build's <id>.duplicate.json, which is kept;
 * or one of them cannot be checked. */
static int named(const char *hex)
{
    char path[VJO_EXPORT_PATH_MAX];
    card_path(path, 0, hex, "json");
    if (present(path))
        return 1;
    card_path(path, 1, hex, "json");
    if (present(path))
        return 1;
    card_path(path, 0, hex, "duplicate.json");
    return present(path) != 0;
}

/* After a verified package (made: its file time), anki_queue/'s pictures no card names go: every
 * <id>.jpg no record names, and every <id>.jpg.tmp (a record follows its JPEG's rename, so a .tmp is
 * never a card's picture), each only when its file time is VJO_EXPORT_ORPHAN_AGE_S older than the
 * package's. A save still finishing beside the app (the Shell's Anki thread runs on) commits its
 * record within seconds of its JPEG: its screenshot job ends within 15 s (shell/anki.c
 * SHOT_TIMEOUT_US), and the kernel removes a stopped job's .tmp itself. A file of unknown time, or
 * dated after the package (a clock set back), stays. exported/ is never swept: an export stopped
 * between a card's two moves leaves the JPEG there ahead of its record. Returns how many went; a
 * removal that fails is logged and left for the next export. */
static int remove_orphans(const SceDateTime *made)
{
    SceUID d;
    SceIoDirent ent;
    char hex[VJO_QUEUE_ID_SIZE], path[VJO_EXPORT_PATH_MAX];
    int64_t now = file_seconds(made), then;
    int n = 0, rc;
    if (now < 0 || !is_dir(VJO_EXPORT_QUEUE_DIR) || (d = sceIoDopen(VJO_EXPORT_QUEUE_DIR)) < 0)
        return 0;
    for (;;) {
        size_t len;
        int tmp;
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0)
            break;
        len = strlen(ent.d_name);
        tmp = len == 40 && !strcmp(ent.d_name + 32, ".jpg.tmp");
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || (!tmp && (len != 36 || strcmp(ent.d_name + 32, ".jpg"))))
            continue;
        memcpy(hex, ent.d_name, 32);
        hex[32] = '\0';
        if (!vjo_queue_valid_id(hex) || (!tmp && named(hex)))
            continue;
        then = file_seconds(&ent.d_stat.st_mtime);
        if (then < 0 || now - then < VJO_EXPORT_ORPHAN_AGE_S)
            continue;
        snprintf(path, sizeof(path), "%s/%s", VJO_EXPORT_QUEUE_DIR, ent.d_name);
        if ((rc = sceIoRemove(path)) < 0) {
            io_fail("remove", path, rc);
            continue;
        }
        vjo_log("anki export: %s: no card names it, removed", path);
        n++;
    }
    sceIoDclose(d);
    return n;
}

int vjo_export_run(VjoArena *a, int all, const VjoExportUi *ui, VjoExportResult *r, char *msg, size_t cap)
{
    List l;
    Out o;
    VjoApkgOps ops;
    VjoApkgResult pr;
    VjoApkgClock clock;
    SceDateTime t;
    SceRtcTick tick;
    char tmp[VJO_EXPORT_PATH_MAX + 4];
    int rc, queued, archived = 0, made = 0;
    size_t base = vjo_arena_mark(a);
    memset(r, 0, sizeof(*r));
    memset(&l, 0, sizeof(l));
    msg[0] = '\0';
    remove_stale_temps();
    /* 1. the cards: each read once and checked, the oldest VJO_APKG_MAX_CARDS kept */
    queued = scan(VJO_EXPORT_QUEUE_DIR, NULL, NULL);
    if (all)
        archived = scan(VJO_EXPORT_ARCHIVE_DIR, NULL, NULL);
    if (queued < 0 || archived < 0) {
        snprintf(msg, cap, "Cannot read %s (see log.txt). Nothing was changed.",
                 queued < 0 ? VJO_EXPORT_QUEUE_DIR : VJO_EXPORT_ARCHIVE_DIR);
        return r->rc = VJO_E_LOCAL_IO;
    }
    l.a = a;
    l.ui = ui;
    l.total = queued + archived;
    l.cap = l.total < VJO_APKG_MAX_CARDS ? l.total : VJO_APKG_MAX_CARDS;
    if (!l.total) {
        snprintf(msg, cap, all ? "No cards yet: save words in the overlay first."
                               : "No cards queued: save words in the overlay first.");
        return r->rc = VJO_OK;
    }
    if (!(l.cards = (VjoApkgCard *)vjo_arena_alloc(a, sizeof(VjoApkgCard) * (size_t)l.cap))) {
        snprintf(msg, cap, "Not enough memory for %d cards. Nothing was changed.", l.total);
        return r->rc = VJO_E_OOM;
    }
    rc = scan(VJO_EXPORT_QUEUE_DIR, list_card, &l);
    if (rc >= 0 && all) {
        l.archive = 1;
        rc = scan(VJO_EXPORT_ARCHIVE_DIR, list_card, &l);
    }
    if (ui->stopped && ui->stopped(ui->ud)) {
        snprintf(msg, cap, "Stopped. Nothing was changed.");
        vjo_arena_release(a, base);
        return r->rc = VJO_E_CANCELLED;
    }
    r->unreadable = l.unreadable;
    r->left = l.left;
    if (rc < 0) {
        snprintf(msg, cap, "Cannot read the queue (see log.txt). Nothing was changed.");
        vjo_arena_release(a, base);
        return r->rc = VJO_E_LOCAL_IO;
    }
    vjo_apkg_sort(l.cards, l.n);
    l.n = dedupe(l.cards, l.n);
    if (!l.n) {
        snprintf(msg, cap, "%d card%s could not be read (see log.txt); they stay queued.", l.unreadable,
                 l.unreadable == 1 ? "" : "s");
        vjo_arena_release(a, base);
        return r->rc = VJO_E_PARSE;
    }
    /* 2. the package, under a temporary name */
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    memset(&clock, 0, sizeof(clock));
    clock.now = sceRtcGetCurrentTick(&tick) >= 0 && tick.tick > 62135596800000000ull
                    ? (int64_t)((tick.tick - 62135596800000000ull) / 1000000u)
                    : 0;
    clock.year = t.year;
    clock.month = t.month;
    clock.day = t.day;
    clock.hour = t.hour;
    clock.minute = t.minute;
    clock.second = t.second;
    sceIoMkdir(VJO_EXPORT_DATA_DIR, 0777); /* each fails when it exists */
    sceIoMkdir(VJO_EXPORT_DIR, 0777);
    /* (its final name, with the number of notes written, once it is checked) */
    snprintf(tmp, sizeof(tmp), "%s/%04d-%02d-%02d_%02d%02d%02d%s.apkg.tmp", VJO_EXPORT_DIR, t.year, t.month, t.day,
             t.hour, t.minute, t.second, all ? "-all" : "");
    o.ui = ui;
    o.total = l.n;
    if ((o.fd = sceIoOpen(tmp, SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0) {
        io_fail("open", tmp, o.fd);
        snprintf(msg, cap, "Cannot create a file in %s (memory card full?). Nothing was changed.", VJO_EXPORT_DIR);
        vjo_arena_release(a, base);
        return r->rc = VJO_E_LOCAL_IO;
    }
    memset(&ops, 0, sizeof(ops));
    ops.ud = &o;
    ops.load = load;
    ops.write = out_write;
    ops.read = out_read;
    ops.progress = progress;
    ops.stopped = out_stopped;
    rc = vjo_apkg_write(a, &ops, l.cards, l.n, &clock, &pr);
    if (!rc) {
        int k = sceIoSyncByFd(o.fd, 0);
        if (k < 0)
            rc = io_error("flush", tmp, k);
    }
    {
        int k = sceIoClose(o.fd);
        if (k < 0 && !rc)
            rc = io_error("close", tmp, k);
    }
    /* 3. opened again and verified: every entry's CRC, the notes, their GUIDs, the cards, the media */
    if (!rc) {
        if (ui->status)
            ui->status(ui->ud, "Checking the file…");
        if ((o.fd = sceIoOpen(tmp, SCE_O_RDONLY, 0)) < 0) {
            rc = io_error("open", tmp, o.fd);
        } else {
            SceOff size = sceIoLseek(o.fd, 0, SCE_SEEK_END);
            rc = size < 0 ? VJO_E_LOCAL_IO : vjo_apkg_verify(a, &ops, (uint64_t)size, l.cards, l.n, &pr);
            sceIoClose(o.fd);
            if (rc)
                vjo_log("anki export: %s did not verify (%d): removed, nothing cleared", tmp, rc);
        }
    }
    /* 4. only a checked package gets its name; then the memory card's directory is synced */
    if (!rc && package_name(&t, all, pr.cards, r->name, sizeof(r->name), r->path) < 0) {
        vjo_log("anki export: %s: 99 packages are named for this second already", VJO_EXPORT_DIR);
        rc = VJO_E_LOCAL_IO;
    }
    if (!rc) {
        int k = sceIoRename(tmp, r->path);
        if (k < 0)
            rc = io_error("rename", tmp, k);
        else
            made = 1;
    }
    if (!rc) {
        int k = sceIoSync("ux0:", 0);
        if (k < 0)
            rc = io_error("sync", "ux0:", k);
    }
    if (rc) {
        sceIoRemove(made ? r->path : tmp);
        sceIoSync("ux0:", 0);
        if (rc == VJO_E_CANCELLED)
            snprintf(msg, cap, "Stopped. No file was made; the queue is unchanged.");
        else if (rc == VJO_E_OOM)
            snprintf(msg, cap, "Not enough memory to make any card. The queue is unchanged.");
        else
            snprintf(msg, cap, "Could not write a complete file (memory card full? see log.txt). "
                               "The queue is unchanged.");
        r->path[0] = r->name[0] = '\0';
        vjo_arena_release(a, base);
        return r->rc = rc;
    }
    r->cards = pr.cards;
    r->media = pr.media;
    r->bytes = pr.bytes;
    vjo_log("anki export: %s: %d cards, %d screenshots, %u bytes, verified", r->path, pr.cards, pr.media,
            (unsigned)pr.bytes);
    /* 5. its queued cards leave the queue. A stop between two cards leaves the rest queued:
     * each is in this package and goes into the next too (Anki adds no note twice). */
    sceIoMkdir(VJO_EXPORT_ARCHIVE_DIR, 0777);
    {
        int to_move = 0, done = 0;
        for (int i = 0; i < l.n; i++) {
            if (l.cards[i].skipped)
                r->too_large++;
            else {
                r->missing_picture += l.cards[i].missing_picture;
                if (!l.cards[i].exported)
                    to_move++;
            }
        }
        for (int i = 0; i < l.n; i++) {
            if (l.cards[i].skipped || l.cards[i].exported)
                continue;
            if (r->stopped || status_every(ui, "Clearing the queue", ++done, to_move) < 0) {
                r->stopped++;
                continue;
            }
            if (archive_card(a, &l.cards[i]) < 0)
                r->kept++;
            else
                r->moved++;
        }
    }
    /* 6. the queue's pictures no card names (#161), unless the clearing was stopped */
    if (!r->stopped && !(ui->stopped && ui->stopped(ui->ud))) {
        SceIoStat st;
        int k;
        memset(&st, 0, sizeof(st));
        if ((k = sceIoGetstat(r->path, &st)) < 0)
            io_fail("getstat", r->path, k);
        else
            r->swept = remove_orphans(&st.st_mtime);
    }
    if (sceIoSync("ux0:", 0) < 0)
        vjo_log("anki export: the final sync failed; the queue is checked again on the next export");
    vjo_arena_release(a, base);
    {
        char size[24], extra[224] = "";
        size_text(r->bytes, size, sizeof(size));
        if (r->stopped)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                     " Stopped while clearing: %d stay queued, for the next export too.", r->stopped);
        if (r->kept)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " %d stay queued (see log.txt).",
                     r->kept);
        if (r->missing_picture)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " %d exported without %s.",
                     r->missing_picture, r->missing_picture == 1 ? "a screenshot" : "screenshots");
        if (r->unreadable + r->too_large)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " %d stay queued; see log.txt.",
                     r->unreadable + r->too_large);
        if (r->left)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                     all ? " %d newer are not in it: a package holds %d cards at most."
                         : " %d newer stay queued: export again.",
                     r->left, VJO_APKG_MAX_CARDS);
        snprintf(msg, cap, "Exported %d card%s (%s) to %s.%s", r->cards, r->cards == 1 ? "" : "s", size, r->name,
                 extra);
    }
    return r->rc = VJO_OK;
}

/* ---------------- the export's steps, for the hand-over to Anki for Vita (anki_export.h) ---------------- */

int vjo_export_scan(int archive, int (*fn)(void *ud, const char *hex), void *ud)
{
    return scan(archive ? VJO_EXPORT_ARCHIVE_DIR : VJO_EXPORT_QUEUE_DIR, fn, ud);
}

int vjo_export_read_card(VjoArena *a, int archive, const char *hex, VjoAnkiNote *note, int *picture)
{
    return read_card(a, archive, hex, note, picture);
}

const uint8_t *vjo_export_read_jpeg(VjoArena *a, int archive, const char *hex, size_t *len, int *unavailable)
{
    return read_jpeg(a, archive, hex, len, unavailable);
}

int vjo_export_archive_card(VjoArena *a, const char *hex)
{
    VjoApkgCard c;
    memset(&c, 0, sizeof(c));
    from_hex(hex, c.id);
    /* Its JPEG, when the queue has one (only a card with a screenshot has <id>.jpg: the ID is the
     * record's checksum, and the record says whether a picture goes with it). */
    c.picture = 1;
    sceIoMkdir(VJO_EXPORT_ARCHIVE_DIR, 0777); /* fails when it exists */
    return archive_card(a, &c);
}
