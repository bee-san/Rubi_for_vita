#include "cards.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/errors.h"
#include "../core/json.h"
#include "../core/utf.h"
#include "../core/word_key.h"

void vjo_log(const char *fmt, ...); /* the app's log.txt lines (settings/applog.c) */

#define MISSING ((int)0x80010002u) /* ENOENT */
#define JPEG_MAGIC "\xFF\xD8\xFF"
#define PATH_CAP VJO_EXPORT_PATH_MAX
#define Q VJO_QUEUE_DIR
#define X VJO_QUEUE_EXPORTED_DIR
#define D VJO_CARDS_DELETED_DIR
#define INTENT_MAX 256 /* intents a round of the recovery settles (a stop leaves one) */

typedef char vjo_card_row_is_128_bytes[sizeof(VjoCardRow) == 128 ? 1 : -1];

static int io_fail(const char *call, const char *path, int rc)
{
    vjo_log("cards: %s %s failed 0x%08X", call, path, (unsigned)rc);
    return VJO_E_LOCAL_IO;
}

void vjo_cards_hex(const uint8_t id[16], char hex[VJO_QUEUE_ID_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        hex[2 * i] = digits[id[i] >> 4];
        hex[2 * i + 1] = digits[id[i] & 15];
    }
    hex[32] = '\0';
}

int vjo_cards_unhex(const char *hex, uint8_t id[16])
{
    char own[VJO_QUEUE_ID_SIZE];
    if (strlen(hex) < 32)
        return -1;
    memcpy(own, hex, 32);
    own[32] = '\0';
    if (!vjo_queue_valid_id(own))
        return -1;
    for (int i = 0; i < 16; i++) {
        int hi = own[2 * i] <= '9' ? own[2 * i] - '0' : own[2 * i] - 'a' + 10;
        int lo = own[2 * i + 1] <= '9' ? own[2 * i + 1] - '0' : own[2 * i + 1] - 'a' + 10;
        id[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static void at(char out[PATH_CAP], const char *dir, const char *hex, const char *ext)
{
    snprintf(out, PATH_CAP, "%s/%s.%s", dir, hex, ext);
}

/* 1: the file is there, 0: it is not, -1: not known (a getstat that failed other than "missing"). */
static int present(const char *path)
{
    SceIoStat st;
    int rc;
    memset(&st, 0, sizeof(st));
    rc = sceIoGetstat(path, &st);
    if (rc < 0 && rc != MISSING)
        io_fail("getstat", path, rc);
    return rc >= 0 ? 1 : rc == MISSING ? 0 : -1;
}

static int is_dir(const char *path)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

/* The memory card's buffers, its directories' renames and removals too. */
static int mount_sync(void)
{
    int rc = sceIoSync("ux0:", 0);
    return rc < 0 ? io_fail("sync", "ux0:", rc) : 0;
}

/* Removes path if it is there. 0, or VJO_E_LOCAL_IO. */
static int remove_if(const char *path)
{
    int rc = sceIoRemove(path);
    return rc < 0 && rc != MISSING ? io_fail("remove", path, rc) : 0;
}

/* A whole file of at most max bytes (NUL-terminated) into *out. 0; MISSING; VJO_E_PARSE (empty or
 * over max); VJO_E_OOM; VJO_E_LOCAL_IO (logged). */
static int read_whole(VjoArena *a, const char *path, size_t max, char **out, size_t *len)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    size_t off = 0;
    char *data;
    int n;
    *out = NULL;
    *len = 0;
    if (fd < 0)
        return fd == MISSING ? MISSING : io_fail("open", path, fd);
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0 || sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) {
        sceIoClose(fd);
        return io_fail("seek", path, (int)size);
    }
    if (!size || (uint64_t)size > max) {
        vjo_log("cards: %s: size %d (limit %u)", path, (int)size, (unsigned)max);
        sceIoClose(fd);
        return VJO_E_PARSE;
    }
    if (!(data = (char *)vjo_arena_alloc(a, (size_t)size + 1))) {
        vjo_log("cards: %s: out of memory reading %u bytes", path, (unsigned)size);
        sceIoClose(fd);
        return VJO_E_OOM;
    }
    while (off < (size_t)size) {
        n = sceIoRead(fd, data + off, (SceSize)((size_t)size - off));
        if (n <= 0) {
            sceIoClose(fd);
            return io_fail("read", path, n);
        }
        off += (size_t)n;
    }
    if ((n = sceIoClose(fd)) < 0)
        return io_fail("close", path, n);
    data[off] = '\0';
    *out = data;
    *len = off;
    return 0;
}

/* data into path, as the Shell's save commits a card (shell/anki_queue.c): a temporary name,
 * written, flushed, closed and renamed, then the memory card synced. path is not there (nothing
 * renames over a file); a failure leaves no temporary file. 0, or VJO_E_LOCAL_IO. */
static int commit(const char *path, const void *data, size_t len)
{
    char tmp[PATH_CAP + 4];
    SceUID fd;
    size_t off = 0;
    int rc = 0, n;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if ((fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0)
        return io_fail("open", tmp, fd);
    while (!rc && off < len) {
        n = sceIoWrite(fd, (const char *)data + off, (SceSize)(len - off));
        if (n <= 0)
            rc = io_fail("write", tmp, n);
        else
            off += (size_t)n;
    }
    if (!rc && (n = sceIoSyncByFd(fd, 0)) < 0)
        rc = io_fail("flush", tmp, n);
    if ((n = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", tmp, n);
    if (!rc && (n = sceIoRename(tmp, path)) < 0)
        rc = io_fail("rename", tmp, n);
    if (rc) {
        sceIoRemove(tmp);
        return rc;
    }
    return mount_sync();
}

static int prepare_deleted(void)
{
    sceIoMkdir(D, 0777); /* fails when it exists */
    if (!is_dir(D)) {
        vjo_log("cards: %s is not a directory", D);
        return VJO_E_LOCAL_IO;
    }
    return 0;
}

/* dir/<hex>.<ext>, when it is there, into deleted/ (a copy there already goes first: it is the
 * same card's, deleted before). 0 (moved, or not there), or VJO_E_LOCAL_IO. */
static int retire(const char *dir, const char *hex, const char *ext)
{
    char from[PATH_CAP], to[PATH_CAP];
    int there, rc;
    at(from, dir, hex, ext);
    if ((there = present(from)) <= 0)
        return there < 0 ? VJO_E_LOCAL_IO : 0;
    at(to, D, hex, ext);
    if (remove_if(to))
        return VJO_E_LOCAL_IO;
    if ((rc = sceIoRename(from, to)) < 0)
        return io_fail("rename", from, rc);
    return 0;
}

/* Some record names this ID: queued, exported, or an earlier build's <id>.duplicate.json (the
 * Shell's find() set), or one of them cannot be checked (-1). */
static int named(const char *hex)
{
    static const struct {
        const char *dir, *ext;
    } where[] = {{Q, "json"}, {X, "json"}, {Q, "duplicate.json"}};
    char path[PATH_CAP];
    int unknown = 0;
    for (unsigned i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        int p;
        at(path, where[i].dir, hex, where[i].ext);
        if ((p = present(path)) > 0)
            return 1;
        unknown |= p < 0;
    }
    return unknown ? -1 : 0;
}

/* A retired queued card's picture into deleted/: the queue's, and one left in exported/ by an
 * export stopped between the card's two moves when no exported record names it. */
static int retire_picture(const char *hex)
{
    char path[PATH_CAP];
    int rc = retire(Q, hex, "jpg");
    at(path, X, hex, "json");
    if (!rc && present(path) == 0) {
        char trash[PATH_CAP];
        at(path, X, hex, "jpg");
        at(trash, D, hex, "jpg");
        if (present(path) > 0 && present(trash) == 0)
            rc = retire(X, hex, "jpg");
    }
    return rc;
}

/* An intent file's name: deleted/<old>.fix. It holds the new card's ID and a line break. */
static void intent_path(char out[PATH_CAP], const char *old)
{
    at(out, D, old, "fix");
}

/* An intent's outcome, after a stop or a failure: the new card there, the old one is retired
 * (steps 4-5); else the new card's files go. Then the intent goes. 0, or VJO_E_LOCAL_IO (it stays,
 * for the next try). */
static int settle(const char *old, const char *new_id)
{
    char path[PATH_CAP], tmp[PATH_CAP + 4];
    int there = named(new_id), rc = 0;
    if (there < 0)
        return VJO_E_LOCAL_IO;
    if (there) {
        if ((rc = retire(Q, old, "json")) == 0)
            rc = retire_picture(old);
    } else {
        at(path, Q, new_id, "jpg");
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        rc |= remove_if(tmp);
        rc |= remove_if(path); /* no record names it: none is there */
        at(path, Q, new_id, "json");
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        rc |= remove_if(tmp);
    }
    if (rc)
        return VJO_E_LOCAL_IO;
    intent_path(path, old);
    if (remove_if(path))
        return VJO_E_LOCAL_IO;
    return mount_sync();
}

/* ---------------- recovery ---------------- */

typedef struct {
    char old[VJO_QUEUE_ID_SIZE], new_id[VJO_QUEUE_ID_SIZE];
    int tmp; /* an intent's own temporary file (a stop while it was written): removed */
} Intent;

/* One round: up to INTENT_MAX intents settled. The ones that could not be (failed), and whether there were
 * more (more). */
static void recover_round(VjoArena *a, Intent *intents, int *failed, int *more)
{
    SceIoDirent ent;
    SceUID d;
    int n = 0, rc;
    *failed = *more = 0;
    if ((d = sceIoDopen(D)) < 0) {
        io_fail("dopen", D, d);
        *failed = 1;
        return;
    }
    /* collected first: nothing is removed from a directory while it is read */
    for (;;) {
        size_t len;
        memset(&ent, 0, sizeof(ent));
        if ((rc = sceIoDread(d, &ent)) <= 0)
            break;
        len = strlen(ent.d_name);
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || len < 36 || strncmp(ent.d_name + 32, ".fix", 4) ||
            (len != 36 && (len != 40 || strcmp(ent.d_name + 36, ".tmp"))))
            continue;
        if (n == INTENT_MAX) {
            *more = 1;
            continue;
        }
        memcpy(intents[n].old, ent.d_name, 32);
        intents[n].old[32] = '\0';
        intents[n].tmp = len == 40;
        if (vjo_queue_valid_id(intents[n].old))
            n++;
    }
    if (rc < 0) {
        io_fail("dread", D, rc);
        *failed = 1;
    }
    sceIoDclose(d);
    for (int i = 0; i < n; i++) {
        char path[PATH_CAP], tmp[PATH_CAP + 4], *text;
        size_t len, mark = vjo_arena_mark(a);
        uint8_t id[16];
        intent_path(path, intents[i].old);
        if (intents[i].tmp) { /* never a complete intent: its change had not started */
            snprintf(tmp, sizeof(tmp), "%s.tmp", path);
            *failed += remove_if(tmp) != 0;
            continue;
        }
        rc = read_whole(a, path, 64, &text, &len);
        if (rc == 0 && len >= 32)
            memcpy(intents[i].new_id, text, 32);
        vjo_arena_release(a, mark);
        if (rc == MISSING)
            continue;
        if (rc == VJO_E_LOCAL_IO || rc == VJO_E_OOM) { /* not read now: kept for the next try */
            (*failed)++;
            continue;
        }
        intents[i].new_id[32] = '\0';
        if (rc || len < 32 || vjo_cards_unhex(intents[i].new_id, id) < 0) {
            /* a damaged intent (a bad card: it is committed whole): the two cards stay as they are */
            vjo_log("cards: %s: no new card's ID in it (%d); removed, the cards left as they are", path, rc);
            *failed += remove_if(path) != 0;
            continue;
        }
        if (settle(intents[i].old, intents[i].new_id) != 0) {
            vjo_log("cards: %s: the change to %s could not be settled; tried again later", path, intents[i].new_id);
            (*failed)++;
        } else {
            vjo_log("cards: %s settled: %s %s", path, intents[i].new_id,
                    named(intents[i].new_id) > 0 ? "kept, the old card retired" : "undone, the old card kept");
        }
    }
}

int vjo_cards_recover(VjoArena *a)
{
    size_t mark = vjo_arena_mark(a);
    Intent *intents;
    int failed = 0, more = 0;
    if (!is_dir(D))
        return 0;
    if (!(intents = (Intent *)vjo_arena_alloc(a, sizeof(Intent) * INTENT_MAX)))
        return 1;
    /* rounds while there were more than a round takes (a stop leaves one; more are a copy's) */
    for (int round = 0; round < 16; round++) {
        recover_round(a, intents, &failed, &more);
        if (!more)
            break;
    }
    vjo_arena_release(a, mark);
    return failed + more;
}

/* ---------------- the list ---------------- */

int vjo_cards_order(const VjoCardRow *a, const VjoCardRow *b)
{
    int c;
    if (a->time != b->time)
        return a->time > b->time ? -1 : 1;
    if ((c = strcmp(a->word, b->word)) != 0)
        return c < 0 ? -1 : 1;
    if ((c = strcmp(a->reading, b->reading)) != 0)
        return c < 0 ? -1 : 1;
    c = memcmp(a->id, b->id, 16);
    return c < 0 ? -1 : c > 0;
}

static int newest_first(const void *x, const void *y)
{
    return vjo_cards_order((const VjoCardRow *)x, (const VjoCardRow *)y);
}

/* s into out (cap bytes), cut at a character. */
static void copy_cut(char *out, size_t cap, const char *s)
{
    snprintf(out, cap, "%s", s ? s : "");
    vjo_utf8_trim_partial(out);
}

/* days since 1970-01-01 to a civil date (H. Hinnant's civil_from_days) */
static void utc_day(uint32_t t, char out[17])
{
    long z = (long)(t / 86400u) + 719468, era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, 17, "%04u-%02u-%02u", (unsigned)((y + (m <= 2)) % 10000), (unsigned)(m % 100), (unsigned)(d % 100));
}

/* The local date and time MiscInfo ends with ("CLANNAD (2026-10-14 17:12)"), else the UTC day of
 * time, else "". */
static void date_of(const char *misc, uint32_t time, char out[17])
{
    size_t n = misc ? strlen(misc) : 0;
    out[0] = '\0';
    if (n >= 18 && misc[n - 18] == '(' && misc[n - 1] == ')') {
        static const char form[] = "0000-00-00 00:00";
        const char *s = misc + n - 17;
        int ok = 1;
        for (int i = 0; i < 16 && ok; i++)
            ok = form[i] == '0' ? s[i] >= '0' && s[i] <= '9' : s[i] == form[i];
        if (ok) {
            memcpy(out, s, 16);
            out[16] = '\0';
            return;
        }
    }
    if (time)
        utc_day(time, out);
}

/* What a record shows, whether or not it can be exported: its strings if they are there. */
typedef struct {
    const char *spelling, *reading, *game, *misc, *sentence;
    uint32_t time;
    int picture, hl_start, hl_end;
} Loose;

static const char *loose_string(VjoArena *a, const VjoJson *j, const char *key)
{
    int t = vjo_json_obj_get(j, 0, key);
    return t >= 0 && vjo_json_is_type(j, t, JSMN_STRING) ? vjo_json_str(a, j, t) : NULL;
}

static int loose_int(const VjoJson *j, const char *key, int fallback)
{
    long v;
    int t = vjo_json_obj_get(j, 0, key);
    return t >= 0 && vjo_json_is_type(j, t, JSMN_PRIMITIVE) && vjo_json_int(j, t, &v) == 0 ? (int)v : fallback;
}

static void loose(VjoArena *a, const char *json, size_t len, Loose *out)
{
    VjoJson j;
    int t;
    memset(out, 0, sizeof(*out));
    out->hl_start = out->hl_end = -1;
    if (vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT))
        return;
    out->spelling = loose_string(a, &j, "spelling");
    out->reading = loose_string(a, &j, "reading");
    out->game = loose_string(a, &j, "game");
    out->misc = loose_string(a, &j, "misc");
    out->sentence = loose_string(a, &j, "sentence");
    out->hl_start = loose_int(&j, "hl_start", -1);
    out->hl_end = loose_int(&j, "hl_end", -1);
    if ((t = vjo_json_obj_get(&j, 0, "picture")) >= 0)
        vjo_json_bool(&j, t, &out->picture);
    /* time: digits written with %u (core/anki_queue.c), read without a 32-bit long */
    if ((t = vjo_json_obj_get(&j, 0, "time")) >= 0 && vjo_json_is_type(&j, t, JSMN_PRIMITIVE) &&
        j.t[t].end - j.t[t].start <= 10) {
        uint64_t v = 0;
        int ok = 1;
        for (int i = j.t[t].start; i < j.t[t].end && ok; i++) {
            ok = json[i] >= '0' && json[i] <= '9';
            v = v * 10 + (uint64_t)(json[i] - '0');
        }
        if (ok && v <= 0xFFFFFFFFu)
            out->time = (uint32_t)v;
    }
}

/* A record read and checked: 0 exportable; else VJO_CARD_WHY_*. */
static int check(VjoArena *a, const char *hex, const char *json, size_t len, VjoAnkiNote *note, int *picture)
{
    char actual[VJO_QUEUE_ID_SIZE];
    vjo_queue_id(json, len, actual);
    if (strcmp(actual, hex))
        return VJO_CARD_WHY_CHECKSUM;
    return vjo_queue_decode(a, json, len, note, picture) < 0 ? VJO_CARD_WHY_FORMAT : VJO_CARD_WHY_NONE;
}

typedef struct {
    VjoArena *a;
    VjoCardList *l;
    const char *dir;
    const VjoCardRow *bound;
    const VjoExportUi *ui;
    int seen, total;
} Scan;

/* The heap of the cap newest rows, the oldest on top once it is full. */
static void sift_down(VjoCardRow *r, int n, int i)
{
    for (;;) {
        int k = 2 * i + 1;
        VjoCardRow t;
        if (k >= n)
            return;
        if (k + 1 < n && vjo_cards_order(&r[k + 1], &r[k]) > 0)
            k++;
        if (vjo_cards_order(&r[k], &r[i]) <= 0)
            return;
        t = r[i];
        r[i] = r[k];
        r[k] = t;
        i = k;
    }
}

static int game_index(VjoCardList *l, const char *game)
{
    char cut[VJO_CARDS_GAME_BYTES];
    if (!game || !*game)
        return VJO_CARD_NO_GAME;
    copy_cut(cut, sizeof(cut), game);
    for (int i = 0; i < l->n_games; i++)
        if (!strcmp(l->games[i], cut))
            return i;
    if (l->n_games == VJO_CARDS_GAMES)
        return VJO_CARD_NO_GAME;
    memcpy(l->games[l->n_games], cut, sizeof(cut));
    return l->n_games++;
}

static int status_every(const VjoExportUi *ui, int done, int total)
{
    char msg[96];
    if (ui && ui->stopped && ui->stopped(ui->ud))
        return -1;
    if (ui && ui->status && (done % 8 == 0 || done == total)) {
        snprintf(msg, sizeof(msg), "Reading the cards… %d of %d", done, total);
        ui->status(ui->ud, msg);
    }
    return 0;
}

static void row_of(VjoArena *a, const char *dir, const char *hex, VjoCardRow *row, VjoCardList *l)
{
    char path[PATH_CAP], *json;
    size_t len, mark = vjo_arena_mark(a);
    VjoAnkiNote note;
    Loose f;
    int picture = 0, rc;
    memset(row, 0, sizeof(*row));
    vjo_cards_unhex(hex, row->id);
    row->game = VJO_CARD_NO_GAME;
    at(path, dir, hex, "json");
    if ((rc = read_whole(a, path, VJO_QUEUE_TEXT_MAX, &json, &len)) != 0) {
        row->flags = VJO_CARD_UNREADABLE;
        vjo_arena_release(a, mark);
        return;
    }
    if (check(a, hex, json, len, &note, &picture) != VJO_CARD_WHY_NONE)
        row->flags |= VJO_CARD_UNREADABLE;
    loose(a, json, len, &f);
    if (!(row->flags & VJO_CARD_UNREADABLE)) {
        f.spelling = note.spelling;
        f.reading = note.reading;
        f.picture = picture;
        f.time = note.unix_time > 0 && note.unix_time <= 0xFFFFFFFFll ? (uint32_t)note.unix_time : 0;
    }
    copy_cut(row->word, sizeof(row->word), f.spelling);
    copy_cut(row->reading, sizeof(row->reading), f.reading);
    row->key = f.spelling && *f.spelling ? vjo_word_key(f.spelling, f.reading) : 0;
    row->time = f.time;
    row->flags |= f.picture ? VJO_CARD_PICTURE : 0;
    row->game = (uint8_t)game_index(l, f.game);
    date_of(f.misc, f.time, row->date);
    vjo_arena_release(a, mark);
}

static int list_card(Scan *s, const char *hex)
{
    VjoCardList *l = s->l;
    VjoCardRow row;
    if (status_every(s->ui, ++s->seen, s->total) < 0)
        return -1;
    row_of(s->a, s->dir, hex, &row, l);
    if (s->bound && vjo_cards_order(&row, s->bound) <= 0) {
        l->newer++;
        return 0;
    }
    if (l->n < l->cap) {
        l->rows[l->n++] = row;
        if (l->n == l->cap) /* full: the oldest on top from now on */
            for (int i = l->n / 2 - 1; i >= 0; i--)
                sift_down(l->rows, l->n, i);
        return 0;
    }
    l->older++;
    if (l->n && vjo_cards_order(&row, &l->rows[0]) < 0) {
        l->rows[0] = row;
        sift_down(l->rows, l->n, 0);
    }
    return 0;
}

/* dir's <32 hex digits>.json files: fn for each (it returns < 0 to stop, then so does this), or
 * only their count. The count, -1 (it cannot be read) or -2 (stopped). */
static int scan(const char *dir, Scan *s)
{
    SceUID d;
    SceIoDirent ent;
    int n = 0, rc, stopped = 0;
    if (!is_dir(dir))
        return 0;
    if ((d = sceIoDopen(dir)) < 0) {
        io_fail("dopen", dir, d);
        return -1;
    }
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
        if (s && list_card(s, ent.d_name) < 0) {
            stopped = 1;
            rc = 0;
            break;
        }
    }
    if (rc < 0)
        io_fail("dread", dir, rc);
    sceIoDclose(d);
    return rc < 0 ? -1 : stopped ? -2 : n;
}

static const VjoCardRow *dup_rows;
static int by_key(const void *x, const void *y)
{
    uint64_t a = dup_rows[*(const int *)x].key, b = dup_rows[*(const int *)y].key;
    return a < b ? -1 : a > b;
}

/* Rows of the same word as another readable one: #314's word key (core/word_key.h), as the export's Duplicates
 * finds them (katakana as hiragana, a missing reading the spelling's), among the rows of the window. */
static void mark_duplicates(VjoArena *a, VjoCardList *l)
{
    size_t mark = vjo_arena_mark(a);
    int *ix = (int *)vjo_arena_alloc(a, sizeof(int) * (size_t)(l->n ? l->n : 1)), k = 0;
    if (!ix)
        return;
    for (int i = 0; i < l->n; i++)
        if (!(l->rows[i].flags & VJO_CARD_UNREADABLE) && l->rows[i].key)
            ix[k++] = i;
    dup_rows = l->rows;
    qsort(ix, (size_t)k, sizeof(int), by_key);
    for (int i = 1; i < k; i++)
        if (!by_key(&ix[i - 1], &ix[i])) {
            l->rows[ix[i - 1]].flags |= VJO_CARD_DUPLICATE;
            l->rows[ix[i]].flags |= VJO_CARD_DUPLICATE;
        }
    vjo_arena_release(a, mark);
}

int vjo_cards_list(VjoArena *a, int which, const VjoCardRow *bound, VjoCardList *l, const VjoExportUi *ui)
{
    const char *dir = which == VJO_CARDS_EXPORTED ? X : Q;
    Scan s;
    int total, rc;
    l->n = l->total = l->unreadable = l->newer = l->older = l->n_games = 0;
    if ((total = scan(dir, NULL)) < 0)
        return VJO_E_LOCAL_IO;
    memset(&s, 0, sizeof(s));
    s.a = a;
    s.l = l;
    s.dir = dir;
    s.bound = bound;
    s.ui = ui;
    s.total = total;
    rc = scan(dir, &s);
    if (rc == -2)
        return VJO_E_CANCELLED;
    if (rc < 0)
        return VJO_E_LOCAL_IO;
    l->total = rc;
    qsort(l->rows, (size_t)l->n, sizeof(VjoCardRow), newest_first);
    for (int i = 0; i < l->n; i++)
        l->unreadable += !!(l->rows[i].flags & VJO_CARD_UNREADABLE);
    mark_duplicates(a, l);
    return 0;
}

int vjo_cards_deleted_files(void)
{
    SceUID d;
    SceIoDirent ent;
    int n = 0, rc;
    if (!is_dir(D))
        return 0;
    if ((d = sceIoDopen(D)) < 0)
        return -1;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if ((rc = sceIoDread(d, &ent)) <= 0)
            break;
        n += SCE_S_ISREG(ent.d_stat.st_mode) && strlen(ent.d_name) == 37 && !strcmp(ent.d_name + 32, ".json");
    }
    sceIoDclose(d);
    return rc < 0 ? -1 : n;
}

/* ---------------- one card ---------------- */

int vjo_cards_load(VjoArena *a, int which, const uint8_t id[16], VjoCardView *v)
{
    char hex[VJO_QUEUE_ID_SIZE], path[PATH_CAP], *json;
    size_t len, mark = vjo_arena_mark(a), used = 0;
    VjoAnkiNote note;
    Loose f;
    int picture = 0, rc;
    memset(v, 0, sizeof(*v));
    memcpy(v->id, id, 16);
    v->which = which;
    v->hl_start = v->hl_end = -1;
    vjo_cards_hex(id, hex);
    at(path, which == VJO_CARDS_EXPORTED ? X : Q, hex, "json");
    if ((rc = read_whole(a, path, VJO_QUEUE_TEXT_MAX, &json, &len)) != 0) {
        v->flags = VJO_CARD_UNREADABLE;
        v->why = rc == MISSING ? VJO_CARD_WHY_GONE : rc == VJO_E_PARSE ? VJO_CARD_WHY_FORMAT : VJO_CARD_WHY_READ;
        vjo_arena_release(a, mark);
        return rc == MISSING || rc == VJO_E_LOCAL_IO ? VJO_E_LOCAL_IO : rc == VJO_E_OOM ? VJO_E_OOM : 0;
    }
    if ((v->why = check(a, hex, json, len, &note, &picture)) != VJO_CARD_WHY_NONE) {
        v->flags = VJO_CARD_UNREADABLE;
        loose(a, json, len, &f);
        memset(&note, 0, sizeof(note));
        note.spelling = f.spelling;
        note.reading = f.reading;
        note.sentence = f.sentence;
        note.hl_start = f.hl_start;
        note.hl_end = f.hl_end;
        note.misc = f.misc;
        note.unix_time = f.time;
        picture = f.picture;
    } else {
        loose(a, json, len, &f); /* the game: the decoded note has none */
    }
    v->flags |= picture ? VJO_CARD_PICTURE : 0;
    copy_cut(v->word, sizeof(v->word), note.spelling);
    copy_cut(v->reading, sizeof(v->reading), note.reading);
    copy_cut(v->game, sizeof(v->game), f.game);
    date_of(note.misc, note.unix_time > 0 && note.unix_time <= 0xFFFFFFFFll ? (uint32_t)note.unix_time : 0, v->date);
    if (note.sentence) {
        size_t n = strlen(note.sentence);
        copy_cut(v->sentence, sizeof(v->sentence), note.sentence);
        v->sentence_cut = n >= sizeof(v->sentence);
        if (note.hl_start >= 0 && note.hl_end > note.hl_start && (size_t)note.hl_end <= strlen(v->sentence)) {
            v->hl_start = note.hl_start;
            v->hl_end = note.hl_end;
        }
    }
    {
        char name[96];
        const char *d = note.dictionary;
        if (!d && note.n_meanings)
            d = (vjo_anki_meaning_source(note.meanings[0], name, sizeof(name)), name);
        copy_cut(v->dictionary, sizeof(v->dictionary), d);
    }
    for (int i = 0; i < note.n_meanings && used + 1 < sizeof(v->definition); i++) {
        char name[96];
        const char *m = vjo_anki_meaning_source(note.meanings[i], name, sizeof(name));
        int k = snprintf(v->definition + used, sizeof(v->definition) - used, "%s%s", used ? "\n" : "", m);
        used = k < 0 ? used : used + (size_t)k < sizeof(v->definition) ? used + (size_t)k : sizeof(v->definition) - 1;
    }
    vjo_utf8_trim_partial(v->definition);
    vjo_arena_release(a, mark);
    return 0;
}

/* A complete JPEG in a: 0; 1 missing, empty, too large or not a whole JPEG (the export would send
 * the card without it); -1 a read that failed or no memory (try again). */
static int read_picture(VjoArena *a, const char *path, const uint8_t **out, size_t *len)
{
    char *p;
    size_t mark = vjo_arena_mark(a);
    int rc = read_whole(a, path, VJO_QUEUE_PICTURE_MAX, &p, len);
    *out = NULL;
    if (rc == MISSING || rc == VJO_E_PARSE)
        return 1;
    if (rc)
        return -1;
    if (*len >= 6 && !memcmp(p, JPEG_MAGIC, 3) && (uint8_t)p[*len - 2] == 0xFF && (uint8_t)p[*len - 1] == 0xD9) {
        *out = (const uint8_t *)p;
        return 0;
    }
    vjo_log("cards: %s: missing JPEG start or end marker", path);
    vjo_arena_release(a, mark);
    return 1;
}

/* Its picture from its own directory, else the other one: 0, 1 (neither is a whole JPEG) or -1. */
static int picture_of(VjoArena *a, int which, const char *hex, const uint8_t **out, size_t *len)
{
    char path[PATH_CAP];
    int retry = 0;
    for (int k = 0; k < 2; k++) {
        int rc;
        at(path, (k ? which != VJO_CARDS_EXPORTED : which == VJO_CARDS_EXPORTED) ? X : Q, hex, "jpg");
        if ((rc = read_picture(a, path, out, len)) == 0)
            return 0;
        retry |= rc < 0;
    }
    *len = 0;
    return retry ? -1 : 1;
}

const uint8_t *vjo_cards_picture(VjoArena *a, int which, const uint8_t id[16], size_t *len)
{
    char hex[VJO_QUEUE_ID_SIZE];
    const uint8_t *p;
    vjo_cards_hex(id, hex);
    return picture_of(a, which, hex, &p, len) == 0 ? p : NULL;
}

/* ---------------- delete, empty deleted ---------------- */

int vjo_cards_delete(VjoArena *a, const uint8_t id[16])
{
    char hex[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    int there, rc;
    (void)a;
    vjo_cards_hex(id, hex);
    at(path, Q, hex, "json");
    if ((there = present(path)) <= 0) {
        if (!there)
            vjo_log("cards: %s: not in the queue (exported or deleted meanwhile?)", hex);
        return VJO_E_LOCAL_IO;
    }
    if ((rc = prepare_deleted()) != 0 || (rc = retire(Q, hex, "json")) != 0)
        return rc;
    /* its picture after it: one a stop leaves is a JPEG no card names (the export's sweep) */
    if (retire_picture(hex))
        vjo_log("cards: %s: its picture stays in the queue; the export removes it", hex);
    mount_sync();
    vjo_log("cards: %s deleted", hex);
    return 0;
}

int vjo_cards_empty_deleted(VjoArena *a, int *removed)
{
    size_t mark = vjo_arena_mark(a);
    SceUID d;
    SceIoDirent ent;
    char (*names)[40];
    int n = 0, cap = 4096, rc, failed = 0;
    *removed = 0;
    if (!is_dir(D))
        return 0;
    if (!(names = (char(*)[40])vjo_arena_alloc(a, sizeof(*names) * (size_t)cap)))
        return VJO_E_OOM;
    if ((d = sceIoDopen(D)) < 0)
        return io_fail("dopen", D, d);
    for (;;) {
        size_t len;
        char hex[VJO_QUEUE_ID_SIZE];
        memset(&ent, 0, sizeof(ent));
        if ((rc = sceIoDread(d, &ent)) <= 0 || n == cap)
            break;
        len = strlen(ent.d_name);
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || len < 36 || len > 37)
            continue;
        memcpy(hex, ent.d_name, 32);
        hex[32] = '\0';
        /* the cards' records and pictures only: an intent stays until its change is settled */
        if (vjo_queue_valid_id(hex) && (!strcmp(ent.d_name + 32, ".json") || !strcmp(ent.d_name + 32, ".jpg")))
            memcpy(names[n++], ent.d_name, len + 1);
    }
    if (rc < 0)
        failed = io_fail("dread", D, rc) != 0;
    sceIoDclose(d);
    for (int i = 0; i < n; i++) {
        char path[PATH_CAP];
        snprintf(path, sizeof(path), "%s/%s", D, names[i]);
        if (remove_if(path))
            failed = 1;
        else
            ++*removed;
    }
    vjo_arena_release(a, mark);
    mount_sync();
    vjo_log("cards: deleted/ emptied: %d files removed%s", *removed, failed ? ", some could not be" : "");
    return failed ? VJO_E_LOCAL_IO : 0;
}

/* ---------------- fix sentence, drop picture ---------------- */

/* Occurrences of w (n bytes) in s: at most cap starts in pos; the count. */
static int occurrences(const char *s, const char *w, size_t n, int *pos, int cap)
{
    int k = 0;
    if (!n)
        return 0;
    for (const char *p = s; (p = strstr(p, w)) != NULL; p++) {
        if (k < cap)
            pos[k] = (int)(p - s);
        k++;
    }
    return k;
}

static int is_break(char c) { return c == '\n' || c == '\r'; }

/* A match of w's bytes but its line breaks at s + p, line breaks in s skipped between them: its
 * end, or 0. */
static size_t loose_match(const char *s, size_t p, const char *w)
{
    size_t i = p, k = 0;
    while (w[k] && is_break(w[k]))
        k++;
    if (!w[k] || s[i] != w[k])
        return 0;
    while (w[k]) {
        if (is_break(w[k])) {
            k++;
            continue;
        }
        while (i > p && is_break(s[i]))
            i++;
        if (s[i] != w[k])
            return 0;
        i++;
        k++;
    }
    return i;
}

int vjo_cards_find_word(const char *old_sentence, int hl_start, int hl_end, const char *sentence, int *start, int *end)
{
    char w[512];
    size_t n = old_sentence ? strlen(old_sentence) : 0, wn;
    int before[64], after[64], n_before, n_after, best = -1, k = -1;
    long dist = 0;
    if (!old_sentence || !sentence || hl_start < 0 || hl_end <= hl_start || (size_t)hl_end > n ||
        (size_t)(hl_end - hl_start) >= sizeof(w))
        return -1;
    wn = (size_t)(hl_end - hl_start);
    memcpy(w, old_sentence + hl_start, wn);
    w[wn] = '\0';
    n_before = occurrences(old_sentence, w, wn, before, 64);
    n_after = occurrences(sentence, w, wn, after, 64);
    if (n_after) {
        for (int i = 0; i < n_before && i < 64; i++)
            if (before[i] == hl_start)
                k = i;
        if (n_before == n_after && k >= 0 && k < 64) {
            *start = after[k];
        } else {
            for (int i = 0; i < n_after && i < 64; i++) {
                long d = labs((long)after[i] - hl_start);
                if (best < 0 || d < dist)
                    best = after[i], dist = d;
            }
            *start = best;
        }
        *end = *start + (int)wn;
        return 0;
    }
    /* the word's line breaks gone, or new ones inside it */
    for (size_t p = 0; sentence[p]; p++) {
        size_t e = loose_match(sentence, p, w);
        long d = labs((long)p - hl_start);
        if (e && (best < 0 || d < dist)) {
            best = (int)p, dist = d;
            *end = (int)e;
        }
    }
    if (best < 0)
        return -1;
    *start = best;
    return 0;
}

/* A replacement of the record's text: js[start, end) becomes text (JSON already). */
typedef struct {
    int start, end;
    const char *text;
} Edit;

static int by_start(const void *x, const void *y)
{
    return ((const Edit *)x)->start - ((const Edit *)y)->start;
}

/* Key's value in the record: its span (a string's quotes in it) into e, or -1 when it has none. */
static int span(const VjoJson *j, const char *key, Edit *e)
{
    int t = vjo_json_obj_get(j, 0, key);
    if (t < 0)
        return -1;
    e->start = j->t[t].start - (j->t[t].type == JSMN_STRING);
    e->end = j->t[t].end + (j->t[t].type == JSMN_STRING);
    return t;
}

static const char *json_string(VjoArena *a, const char *s)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_json_write_string(&b, s);
    return b.oom ? NULL : vjo_buf_cstr(&b);
}

static const char *number(VjoArena *a, int v)
{
    char *s = (char *)vjo_arena_alloc(a, 16);
    if (s)
        snprintf(s, 16, "%d", v);
    return s;
}

/* The record with its edits: every other byte as it was. */
static char *patched(VjoArena *a, const char *js, size_t len, Edit *e, int n, size_t *out_len)
{
    VjoBuf b;
    int pos = 0;
    qsort(e, (size_t)n, sizeof(Edit), by_start);
    vjo_buf_init(&b, a);
    for (int i = 0; i < n; i++) {
        if (!e[i].text || e[i].start < pos)
            return NULL;
        vjo_buf_append(&b, js + pos, (size_t)(e[i].start - pos));
        vjo_buf_puts(&b, e[i].text);
        pos = e[i].end;
    }
    vjo_buf_append(&b, js + pos, len - (size_t)pos);
    if (b.oom)
        return NULL;
    *out_len = b.len;
    return vjo_buf_cstr(&b);
}

/* (1)-(2): the new card's picture, <new>.jpg, the old one's bytes: written to <new>.jpg.tmp, then renamed.
 * One an earlier, stopped try left with the same bytes is kept (and a .tmp of it removed); one of other bytes,
 * which no record names (its record is not there yet), is removed first: nothing renames over a file. */
static int copy_picture(VjoArena *a, const char *new_id, const uint8_t *jpeg, size_t len)
{
    char path[PATH_CAP], tmp[PATH_CAP + 4], *old;
    size_t mark = vjo_arena_mark(a), old_len, off = 0;
    SceUID fd;
    int rc, same, n;
    at(path, Q, new_id, "jpg");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    rc = read_whole(a, path, VJO_QUEUE_PICTURE_MAX, &old, &old_len);
    same = rc == 0 && old_len == len && !memcmp(old, jpeg, len);
    vjo_arena_release(a, mark);
    if (same)
        return remove_if(tmp);
    if (rc != MISSING) {
        vjo_log("cards: %s is there with other bytes; replaced", path);
        if (remove_if(path))
            return VJO_E_LOCAL_IO;
    }
    if ((fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0)
        return io_fail("open", tmp, fd);
    rc = 0;
    while (!rc && off < len) {
        n = sceIoWrite(fd, jpeg + off, (SceSize)(len - off));
        if (n <= 0)
            rc = io_fail("write", tmp, n);
        else
            off += (size_t)n;
    }
    if (!rc && (n = sceIoSyncByFd(fd, 0)) < 0)
        rc = io_fail("flush", tmp, n);
    if ((n = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", tmp, n);
    if (!rc && (n = sceIoRename(tmp, path)) < 0)
        rc = io_fail("rename", tmp, n);
    if (rc)
        sceIoRemove(tmp);
    return rc;
}

/* Fix sentence (sentence) or Drop picture (NULL). */
static int change(VjoArena *a, const uint8_t card[16], const char *sentence, VjoCardChange *out)
{
    uint8_t id[16]; /* (card may be out->id: a change of the card a change made) */
    char old[VJO_QUEUE_ID_SIZE], new_id[VJO_QUEUE_ID_SIZE], path[PATH_CAP], *json, *edited;
    size_t len, new_len = 0, mark = vjo_arena_mark(a), jpeg_len = 0;
    const uint8_t *jpeg = NULL;
    VjoAnkiNote note, again;
    VjoJson j;
    Edit e[8];
    int n = 0, picture = 0, again_picture = 0, keep, rc, exists, start = -1, end = -1, sf;
    memcpy(id, card, 16);
    memset(out, 0, sizeof(*out));
    memcpy(out->id, id, 16);
    vjo_cards_hex(id, old);
    at(path, Q, old, "json");
    if ((rc = read_whole(a, path, VJO_QUEUE_TEXT_MAX, &json, &len)) != 0) {
        if (rc == MISSING)
            vjo_log("cards: %s: not in the queue (exported or deleted meanwhile?)", old);
        rc = rc == VJO_E_OOM || rc == VJO_E_PARSE ? rc : VJO_E_LOCAL_IO;
        goto done;
    }
    if (check(a, old, json, len, &note, &picture) != VJO_CARD_WHY_NONE || vjo_json_parse(a, json, len, &j) < 0) {
        vjo_log("cards: %s: not a card this build can change", path);
        rc = VJO_E_PARSE;
        goto done;
    }
    out->picture = picture;
    if (sentence) {
        if (vjo_cards_find_word(note.sentence, note.hl_start, note.hl_end, sentence, &start, &end) == 0)
            out->word_found = 1;
        else
            start = end = -1;
        if (!strcmp(sentence, note.sentence) && start == note.hl_start && end == note.hl_end) {
            out->done = VJO_CARDS_UNCHANGED; /* the same text: the same card, whatever its bytes' escapes */
            rc = 0;
            goto done;
        }
    }
    keep = picture && sentence;
    if (keep) {
        int got = picture_of(a, VJO_CARDS_QUEUED, old, &jpeg, &jpeg_len);
        if (got < 0) {
            vjo_log("cards: %s: its picture could not be read; nothing changed", old);
            rc = VJO_E_LOCAL_IO;
            goto done;
        }
        if (got > 0) {
            keep = 0; /* the export would send it without one too: the new card says so */
            out->picture_lost = 1;
        }
    }
    if (sentence) {
        size_t old_len = strlen(note.sentence);
        if (span(&j, "sentence", &e[n]) >= 0)
            e[n++].text = json_string(a, sentence);
        if (span(&j, "hl_start", &e[n]) >= 0)
            e[n++].text = number(a, start);
        if (span(&j, "hl_end", &e[n]) >= 0)
            e[n++].text = number(a, end);
        /* the sentence with readings: the same text from a local dictionary (core/anki.c), which is
         * the new one now; readings of the removed online dictionaries fit the old text only, so they
         * go (Lapis then shows the Sentence) */
        if ((sf = span(&j, "sentence_furigana", &e[n])) >= 0 && vjo_json_is_type(&j, sf, JSMN_STRING)) {
            int same = note.sentence_furigana && strlen(note.sentence_furigana) == old_len &&
                       !memcmp(note.sentence_furigana, note.sentence, old_len);
            e[n++].text = same ? json_string(a, sentence) : "null";
            if (span(&j, "sf_hl_start", &e[n]) >= 0)
                e[n++].text = number(a, same ? start : -1);
            if (span(&j, "sf_hl_end", &e[n]) >= 0)
                e[n++].text = number(a, same ? end : -1);
        }
    }
    if (picture && !keep && span(&j, "picture", &e[n]) >= 0)
        e[n++].text = "false";
    if (!(edited = patched(a, json, len, e, n, &new_len)) || new_len > VJO_QUEUE_TEXT_MAX) {
        if (edited)
            vjo_log("cards: %s: the changed record would be %u bytes (limit %u)", old, (unsigned)new_len,
                    VJO_QUEUE_TEXT_MAX);
        rc = edited ? VJO_E_TOO_LARGE : VJO_E_OOM;
        goto done;
    }
    vjo_queue_id(edited, new_len, new_id);
    if (!strcmp(new_id, old)) {
        out->done = VJO_CARDS_UNCHANGED;
        rc = 0;
        goto done;
    }
    /* what the export will read: the same card, with the change */
    if (vjo_queue_decode(a, edited, new_len, &again, &again_picture) < 0 || !!again_picture != keep ||
        (sentence && strcmp(again.sentence, sentence))) {
        vjo_log("cards: %s: the changed record does not read back; nothing changed", old);
        rc = VJO_E_PARSE;
        goto done;
    }
    if ((exists = named(new_id)) < 0 || prepare_deleted()) {
        rc = VJO_E_LOCAL_IO;
        goto done;
    }
    /* (0) the intent: from here a stop is settled by vjo_cards_recover */
    intent_path(path, old);
    if (present(path) != 0) {
        vjo_log("cards: %s: an earlier change of this card is not settled", path);
        rc = VJO_E_LOCAL_IO;
        goto done;
    }
    {
        char line[VJO_QUEUE_ID_SIZE + 1];
        snprintf(line, sizeof(line), "%s\n", new_id);
        if ((rc = commit(path, line, 33)) != 0) {
            remove_if(path); /* (named after all, when only its sync failed: nothing else is done yet) */
            goto done;
        }
    }
    if (!exists) {
        /* (1)-(2) the picture, (3) the record that names it */
        rc = keep ? copy_picture(a, new_id, jpeg, jpeg_len) : 0;
        at(path, Q, new_id, "json");
        if (!rc)
            rc = commit(path, edited, new_len);
        /* a failure is undone as a recovery would undo it; a record there after all (a sync that
         * failed after its rename) is the change, finished below */
        if (rc && (settle(old, new_id) != 0 || named(new_id) <= 0)) {
            rc = VJO_E_LOCAL_IO;
            goto done;
        }
    }
    /* (4) the old record, (5) its picture, then the intent */
    if (settle(old, new_id) != 0) {
        intent_path(path, old);
        out->old_left = present(path) != 0;
        vjo_log("cards: %s: the old card could not be moved; retried before the next list and export", old);
    }
    vjo_cards_unhex(new_id, out->id);
    out->done = exists ? VJO_CARDS_EXISTED : VJO_CARDS_CHANGED;
    out->picture = keep;
    vjo_log("cards: %s %s: %s%s", old, sentence ? "sentence fixed" : "picture dropped", new_id,
            exists ? " (there already: the old card retired)" : "");
    rc = 0;
done:
    vjo_arena_release(a, mark);
    return rc;
}

int vjo_cards_fix_sentence(VjoArena *a, const uint8_t id[16], const char *sentence, VjoCardChange *out)
{
    if (!sentence || !*sentence) {
        uint8_t own[16];
        memcpy(own, id, 16);
        memset(out, 0, sizeof(*out));
        memcpy(out->id, own, 16);
        return VJO_E_PARSE;
    }
    return change(a, id, sentence, out);
}

int vjo_cards_drop_picture(VjoArena *a, const uint8_t id[16], VjoCardChange *out)
{
    return change(a, id, NULL, out);
}
