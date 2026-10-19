/* The Rubi app's side of Anki for Vita (anki_afv.h): the inbox writer and the outbox reader. */
#include "anki_afv.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>

#include "../core/anki_queue.h"
#include "../core/apkg.h"
#include "../core/errors.h"
#include "../core/json.h"
#include "../core/lapis.h"
#include "anki_index_request.h" /* vjo_afv_speaks_1: interface.json lists version 1 */

void vjo_log(const char *fmt, ...); /* the app's log.txt lines (settings/applog.c) */

#define MISSING ((int)0x80010002u) /* ENOENT */
#define PATH_CAP 192
#define ID_BYTES 16
/* The interface's readers: a file neither there nor beside its .old is tried again this often, this
 * far apart, before it counts as absent (a reader can fall between a replace's renames). */
#define RETRIES 3
#define RETRY_US (100 * 1000)
/* created: the interface's range, from 2006-01-01 to 2100-01-01 (seconds UTC). */
#define CREATED_MIN 1136073600
#define CREATED_MAX 4102444800LL

/* ---------------- files ---------------- */

static int io_fail(const char *call, const char *path, int rc)
{
    vjo_log("anki for vita: %s %s failed 0x%08X", call, path, (unsigned)rc);
    return -1;
}

/* 1: the file is there, 0: it is not, -1: not known (a getstat that failed otherwise). */
static int present(const char *path)
{
    SceIoStat st;
    int rc;
    memset(&st, 0, sizeof(st));
    rc = sceIoGetstat(path, &st);
    return rc >= 0 ? 1 : rc == MISSING ? 0 : -1;
}

static int is_dir(const char *path)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    return sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

/* A whole small file of at most max bytes into a, NUL-terminated: 0 (*out, *len), 1 (there is none),
 * 2 (empty or over max: not one of the interface's), or -1 (a read that failed, logged). */
static int read_small(VjoArena *a, const char *path, size_t max, char **out, size_t *len)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    char *buf;
    size_t used = 0;
    int n;
    if (fd < 0) {
        if (fd == MISSING)
            return 1;
        return io_fail("open", path, fd);
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0 || sceIoLseek(fd, 0, SCE_SEEK_SET) != 0) {
        sceIoClose(fd);
        return io_fail("seek", path, (int)size);
    }
    if (!size || (uint64_t)size > max) {
        sceIoClose(fd);
        return 2;
    }
    if (!(buf = (char *)vjo_arena_alloc(a, (size_t)size + 1))) {
        sceIoClose(fd);
        vjo_log("anki for vita: %s: out of memory reading %u bytes", path, (unsigned)size);
        return -1;
    }
    while (used < (size_t)size) {
        if ((n = sceIoRead(fd, buf + used, (SceSize)((size_t)size - used))) <= 0) {
            sceIoClose(fd);
            return io_fail("read", path, n);
        }
        used += (size_t)n;
    }
    sceIoClose(fd);
    buf[used] = '\0';
    *out = buf;
    *len = used;
    return 0;
}

/* A file the app replaces (an outbox result, status.json): <path>, else <path>.old (the previous
 * version: a replace stopped between its renames), and when neither is there, both again up to
 * RETRIES times, RETRY_US apart. As read_small; *old 1 when the .old was read. */
static int read_replaced(VjoArena *a, const char *path, size_t max, char **out, size_t *len, int *old)
{
    char other[PATH_CAP];
    int r;
    *old = 0;
    if (snprintf(other, sizeof(other), "%s.old", path) >= (int)sizeof(other))
        return -1;
    for (int tries = 0;; tries++) {
        if ((r = read_small(a, path, max, out, len)) != 1)
            return r;
        if ((r = read_small(a, other, max, out, len)) != 1) {
            *old = 1;
            return r;
        }
        if (tries == RETRIES)
            return 1;
        sceKernelDelayThread(RETRY_US);
    }
}

/* A new file of the interface: path.tmp written, flushed and closed, then (commit) renamed to path,
 * which the Vita's rename refuses when path is there: a file is never replaced. 0 (written), 1 (path
 * was there already: left alone, nothing written), or -1 (logged; no .tmp left). Without commit only
 * path.tmp is written (the notes' second step renames it). */
static int write_new(const char *path, const void *data, size_t len, int commit)
{
    char tmp[PATH_CAP];
    SceUID fd;
    size_t off = 0;
    int rc = 0, n, there = present(path);
    if (there)
        return there > 0 ? 1 : io_fail("getstat", path, there);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;
    if ((fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0)
        return io_fail("open", tmp, fd);
    while (!rc && off < len) {
        if ((n = sceIoWrite(fd, (const char *)data + off, (SceSize)(len - off))) <= 0)
            rc = io_fail("write", tmp, n);
        else
            off += (size_t)n;
    }
    if (!rc && (n = sceIoSyncByFd(fd, 0)) < 0)
        rc = io_fail("flush", tmp, n);
    if ((n = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", tmp, n);
    if (!rc && commit && (n = sceIoRename(tmp, path)) < 0) {
        /* refused: another writer's file is there by now (it stays), or the card failed */
        rc = present(path) > 0 ? 1 : io_fail("rename", tmp, n);
    }
    if (rc)
        sceIoRemove(tmp);
    return rc;
}

/* The file at path holds exactly data (len bytes): 1, 0 (other bytes or another size), or -1 (it
 * cannot be read; logged). Read in pieces into the arena's free space (given back). */
static int same_bytes(VjoArena *a, const char *path, const void *data, size_t len)
{
    size_t mark = vjo_arena_mark(a), off = 0, piece = 16 * 1024;
    uint8_t *buf = (uint8_t *)vjo_arena_alloc(a, piece);
    SceUID fd;
    SceOff size;
    int same = 1;
    if (!buf)
        return -1;
    if ((fd = sceIoOpen(path, SCE_O_RDONLY, 0)) < 0) {
        vjo_arena_release(a, mark);
        return io_fail("open", path, fd);
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0 || sceIoLseek(fd, 0, SCE_SEEK_SET) != 0) {
        sceIoClose(fd);
        vjo_arena_release(a, mark);
        return io_fail("seek", path, (int)size);
    }
    if ((uint64_t)size != len)
        same = 0;
    while (same && off < len) {
        size_t want = len - off < piece ? len - off : piece;
        int n = sceIoRead(fd, buf, (SceSize)want);
        if (n <= 0) {
            sceIoClose(fd);
            vjo_arena_release(a, mark);
            return io_fail("read", path, n);
        }
        same = !memcmp(buf, (const uint8_t *)data + off, (size_t)n);
        off += (size_t)n;
    }
    sceIoClose(fd);
    vjo_arena_release(a, mark);
    return same;
}

/* A file of Rubi's in the inbox, written once (write_new) or there already with these bytes: 0, or
 * -1 (it could not be written, or the name holds other bytes, which are left as they are: the client
 * never rewrites an inbox file; logged). */
static int put_once(VjoArena *a, const char *path, const void *data, size_t len)
{
    int rc = write_new(path, data, len, 1), same;
    if (rc <= 0)
        return rc;
    if ((same = same_bytes(a, path, data, len)) > 0)
        return 0;
    if (!same)
        vjo_log("anki for vita: %s holds other bytes than Rubi's: left as it is", path);
    return -1;
}

static void mkdirs(void)
{
    /* each fails when it is there */
    sceIoMkdir(VJO_AFV_INBOX_ROOT, 0777);
    sceIoMkdir(VJO_AFV_INBOX_DIR, 0777);
    sceIoMkdir(VJO_AFV_MEDIA_DIR, 0777);
    sceIoMkdir(VJO_AFV_NOTETYPES_DIR, 0777);
}

/* ---------------- JSON ---------------- */

static const char *token_text(const VjoJson *j, int t, int *n)
{
    *n = j->t[t].end - j->t[t].start;
    return j->js + j->t[t].start;
}

/* A primitive token of digits only (at most 18: no overflow) as a number. */
static int digits(const VjoJson *j, int t, int64_t *out)
{
    int n;
    const char *s;
    int64_t v = 0;
    if (t < 0 || !vjo_json_is_type(j, t, JSMN_PRIMITIVE))
        return -1;
    s = token_text(j, t, &n);
    if (n < 1 || n > 18)
        return -1;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

/* Valid UTF-8 (RFC 3629: no overlong form, no surrogate, nothing above U+10FFFF), the interface's
 * rule for every file. */
static int utf8_valid(const char *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i], need, min, cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 1, min = 0x80, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 2, min = 0x800, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 3, min = 0x10000, cp = c & 0x07;
        else
            return 0;
        if (n - i <= need)
            return 0;
        for (unsigned k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += need + 1;
    }
    return 1;
}

/* A key given twice in object obj (the interface: such a file is malformed); compared as written. */
static int duplicate_keys(const VjoJson *j, int obj)
{
    int k = obj + 1;
    for (int a = 0; a < j->t[obj].size; a++, k = vjo_json_skip(j, k + 1)) {
        int m = vjo_json_skip(j, k + 1), na, nb;
        const char *sa = token_text(j, k, &na);
        for (int b = a + 1; b < j->t[obj].size; b++, m = vjo_json_skip(j, m + 1)) {
            const char *sb = token_text(j, m, &nb);
            if (na == nb && !memcmp(sa, sb, (size_t)na))
                return 1;
        }
    }
    return 0;
}

/* A string token (a key or a value) that holds U+0000, escaped (the interface: a file that holds a
 * NUL anywhere is malformed; written as such it is refused before the parse). */
static int escaped_nul(const VjoJson *j)
{
    for (int t = 0; t < j->n; t++) {
        int n;
        const char *s;
        if (j->t[t].type != JSMN_STRING)
            continue;
        s = token_text(j, t, &n);
        for (int i = 0; i + 1 < n; i++)
            if (s[i] == '\\') {
                if (s[i + 1] == 'u' && i + 5 < n && !memcmp(s + i + 2, "0000", 4))
                    return 1;
                i++; /* (the escaped character) */
            }
    }
    return 0;
}

/* The top level an object (and nothing after it) whose "v" is the number 1, written so; valid UTF-8,
 * no NUL, no key given twice. */
static int version_1(VjoArena *a, const char *json, size_t len, size_t max, VjoJson *j)
{
    int t, n;
    const char *s;
    if (!len || len > max || memchr(json, 0, len) || !utf8_valid(json, len) || vjo_json_parse(a, json, len, j) < 0 ||
        j->n < 1 || !vjo_json_is_type(j, 0, JSMN_OBJECT) || vjo_json_skip(j, 0) != j->n || duplicate_keys(j, 0) ||
        escaped_nul(j))
        return 0;
    if ((t = vjo_json_obj_get(j, 0, "v")) < 0 || !vjo_json_is_type(j, t, JSMN_PRIMITIVE))
        return 0;
    s = token_text(j, t, &n);
    return n == 1 && s[0] == '1';
}

/* A string value of object 0, unescaped into a (NULL: absent or not a string). */
static const char *string_of(VjoArena *a, const VjoJson *j, const char *key)
{
    int t = vjo_json_obj_get(j, 0, key);
    return t >= 0 && vjo_json_is_type(j, t, JSMN_STRING) ? vjo_json_str(a, j, t) : NULL;
}

/* s into out (cap bytes) for the screen: control characters and DEL as spaces, cut before a
 * character that does not fit whole (the page draws it; nothing in it is an instruction). */
static void screen_copy(char *out, size_t cap, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!cap)
        return;
    if (n >= cap) /* cut at the start of a character */
        for (n = cap - 1; n && ((unsigned char)s[n] & 0xC0) == 0x80; n--) {}
    for (size_t i = 0; i < n; i++)
        out[i] = (unsigned char)s[i] < 0x20 || s[i] == 0x7F ? ' ' : s[i];
    out[n] = '\0';
}

/* A text snprintf cut at cap - 1 bytes ends in "..." instead, at the start of a character. */
static void ellipsize(char *out, size_t cap)
{
    size_t p;
    if (cap < 4)
        return;
    for (p = cap - 4; p && ((unsigned char)out[p] & 0xC0) == 0x80; p--) {}
    memcpy(out + p, "...", 4);
}

/* ---------------- status.json ---------------- */

int vjo_afv_status_parse(VjoArena *a, const char *json, size_t len, VjoAfvStatus *s)
{
    size_t mark = vjo_arena_mark(a);
    VjoJson j;
    int64_t v;
    int t, rc = -1;
    memset(s, 0, sizeof(*s));
    if (version_1(a, json, len, VJO_AFV_RESULT_MAX, &j)) {
        const char *server = string_of(a, &j, "server"), *result = string_of(a, &j, "last_result");
        t = vjo_json_obj_get(&j, 0, "signed_in");
        if (t >= 0 && vjo_json_bool(&j, t, &s->signed_in) < 0)
            s->signed_in = 0;
        s->signed_in = !!s->signed_in;
        if (server) { /* a host name: printable ASCII */
            size_t n = 0;
            for (const char *p = server; *p && n + 1 < sizeof(s->server); p++)
                s->server[n++] = *p > 0x20 && *p < 0x7F ? *p : '?';
            s->server[n] = '\0';
        }
        if (!digits(&j, vjo_json_obj_get(&j, 0, "last_sync"), &v))
            s->last_sync = v;
        if (result && strlen(result) < sizeof(s->last_result) && strspn(result, "abcdefghijklmnopqrstuvwxyz_") ==
                                                                        strlen(result))
            memcpy(s->last_result, result, strlen(result) + 1);
        if (!digits(&j, vjo_json_obj_get(&j, 0, "pending"), &v))
            s->pending = v > 1000000 ? 1000000 : (int)v;
        s->known = 1;
        rc = 0;
    }
    vjo_arena_release(a, mark);
    return rc;
}

void vjo_afv_status_read(VjoArena *a, VjoAfvStatus *s)
{
    size_t mark = vjo_arena_mark(a), len = 0;
    char *json = NULL;
    int old, r = read_replaced(a, VJO_AFV_STATUS_PATH, VJO_AFV_RESULT_MAX, &json, &len, &old);
    memset(s, 0, sizeof(*s));
    if (r == 0 && vjo_afv_status_parse(a, json, len, s) < 0)
        vjo_log("anki for vita: status.json is not a version-1 status, ignored");
    vjo_arena_release(a, mark);
}

void vjo_afv_status_text(const VjoAfvStatus *s, int64_t now, char *out, size_t cap)
{
    char ago[40] = "", waiting[40] = "";
    const char *result = !strcmp(s->last_result, "ok")              ? ", ok"
                         : !strcmp(s->last_result, "error")          ? ", it failed"
                         : !strcmp(s->last_result, "cancelled")      ? ", cancelled"
                         : !strcmp(s->last_result, "full_sync_needed") ? ": choose a full sync's direction in it"
                                                                       : "";
    if (!s->known) {
        snprintf(out, cap, "It adds the cards to your collection at its next sync.");
        return;
    }
    if (!s->signed_in) {
        snprintf(out, cap, "Not signed in: open Anki for Vita to sign in, then sync.");
        return;
    }
    if (s->pending > 0)
        snprintf(waiting, sizeof(waiting), "; %d waiting", s->pending);
    if (!s->last_sync) {
        snprintf(out, cap, "Signed in%s%s; no sync yet%s.", s->server[0] ? " to " : "", s->server, waiting);
        return;
    }
    if (now >= s->last_sync) {
        int64_t d = now - s->last_sync;
        if (d < 60)
            snprintf(ago, sizeof(ago), " under a minute ago");
        else if (d < 3600)
            snprintf(ago, sizeof(ago), " %d min ago", (int)(d / 60));
        else if (d < 2 * 86400)
            snprintf(ago, sizeof(ago), " %d h ago", (int)(d / 3600));
        else
            snprintf(ago, sizeof(ago), " %d days ago", (int)(d / 86400 > 99999 ? 99999 : d / 86400));
    }
    if (s->server[0])
        snprintf(out, cap, "%s: last sync%s%s%s.", s->server, ago, result, waiting);
    else
        snprintf(out, cap, "Last sync%s%s%s.", ago, result, waiting);
}

/* ---------------- results ---------------- */

/* A file whose "v" is an integer other than 1: another version of the interface's, which a client
 * treats as absent (its docs/interface.md, "Versions"). */
static int other_version(VjoArena *a, const char *json, size_t len)
{
    size_t mark = vjo_arena_mark(a);
    VjoJson j;
    int64_t v = 1;
    int other = len <= VJO_AFV_RESULT_MAX && !memchr(json, 0, len) && utf8_valid(json, len) &&
                vjo_json_parse(a, json, len, &j) == 0 && j.n > 0 && vjo_json_is_type(&j, 0, JSMN_OBJECT) &&
                !digits(&j, vjo_json_obj_get(&j, 0, "v"), &v) && v != 1;
    vjo_arena_release(a, mark);
    return other;
}

int vjo_afv_result_parse(VjoArena *a, const char *json, size_t len, const char *id, VjoAfvResult *r)
{
    static const char *const names[] = {"", "added", "exists", "duplicate", "rejected"};
    size_t mark = vjo_arena_mark(a);
    const char *got, *status, *message;
    VjoJson j;
    int rc = -1, t;
    memset(r, 0, sizeof(*r));
    if (version_1(a, json, len, VJO_AFV_RESULT_MAX, &j) && (got = string_of(a, &j, "id")) && !strcmp(got, id) &&
        (status = string_of(a, &j, "status"))) {
        for (int k = VJO_AFV_ADDED; k <= VJO_AFV_REJECTED; k++)
            if (!strcmp(status, names[k]))
                r->status = k;
        t = vjo_json_obj_get(&j, 0, "synced");
        if (r->status && (t < 0 || !vjo_json_bool(&j, t, &r->synced))) {
            r->synced = !!r->synced;
            if ((message = string_of(a, &j, "message")) != NULL)
                screen_copy(r->message, sizeof(r->message), message);
            rc = 0;
        }
    }
    if (rc)
        memset(r, 0, sizeof(*r));
    vjo_arena_release(a, mark);
    return rc;
}

int vjo_afv_result_final(const VjoAfvResult *r)
{
    return r->status == VJO_AFV_DUPLICATE || r->status == VJO_AFV_REJECTED ||
           ((r->status == VJO_AFV_ADDED || r->status == VJO_AFV_EXISTS) && r->synced);
}

static void to_hex(const uint8_t id[ID_BYTES], char out[VJO_QUEUE_ID_SIZE])
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < ID_BYTES; i++) {
        out[2 * i] = hexd[id[i] >> 4];
        out[2 * i + 1] = hexd[id[i] & 15];
    }
    out[32] = '\0';
}

static void from_hex(const char *hex, uint8_t id[ID_BYTES])
{
    for (int i = 0; i < ID_BYTES; i++) {
        int hi = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10;
        int lo = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        id[i] = (uint8_t)(hi << 4 | lo);
    }
}

/* A result file the outbox lists: its card's ID, and whether the file is <id>.json.old (the
 * previous version, when a replace was stopped between its renames: read only without <id>.json). */
typedef struct {
    uint8_t id[ID_BYTES];
    uint8_t old;
    uint8_t status;   /* VJO_AFV_*, once read (0: none or malformed) */
    uint8_t final;
} Seen;

static int id_cmp(const uint8_t *x, const uint8_t *y)
{
    return memcmp(x, y, ID_BYTES);
}

/* Sorts by ID, <id>.json before <id>.json.old (heapsort: in place, bounded). */
static int seen_less(const Seen *x, const Seen *y)
{
    int c = id_cmp(x->id, y->id);
    return c ? c < 0 : x->old < y->old;
}

static void seen_sift(Seen *s, int root, int n)
{
    for (;;) {
        int child = 2 * root + 1;
        Seen t;
        if (child >= n)
            return;
        if (child + 1 < n && seen_less(&s[child], &s[child + 1]))
            child++;
        if (!seen_less(&s[root], &s[child]))
            return;
        t = s[root];
        s[root] = s[child];
        s[child] = t;
        root = child;
    }
}

static void seen_sort(Seen *s, int n)
{
    for (int i = n / 2 - 1; i >= 0; i--)
        seen_sift(s, i, n);
    for (int end = n - 1; end > 0; end--) {
        Seen t = s[0];
        s[0] = s[end];
        s[end] = t;
        seen_sift(s, 0, end);
    }
}

static const Seen *seen_find(const Seen *s, int n, const uint8_t id[ID_BYTES])
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2, c = id_cmp(s[mid].id, id);
        if (!c)
            return &s[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* The outbox's result files of Rubi's cards: <32 lowercase hex>.json, and <id>.json.old where there
 * is no <id>.json, regular files only; every other name (a hostile one too) is never read. At most
 * cap of them (the rest: *more); sorted. 0, 1 (no outbox), or -1 (it cannot be read; logged). */
static int list_outbox(VjoArena *a, Seen **out, int *n, int cap, int *more)
{
    SceUID d;
    SceIoDirent ent;
    Seen *s;
    int k = 0, rc, closed;
    *out = NULL;
    *n = *more = 0;
    if (!is_dir(VJO_AFV_OUTBOX_DIR))
        return 1;
    if (!(s = (Seen *)vjo_arena_alloc(a, sizeof(Seen) * (size_t)cap))) {
        vjo_log("anki for vita: out of memory for the outbox's list");
        return -1;
    }
    if ((d = sceIoDopen(VJO_AFV_OUTBOX_DIR)) < 0)
        return io_fail("dopen", VJO_AFV_OUTBOX_DIR, d);
    for (;;) {
        size_t len;
        int old;
        memset(&ent, 0, sizeof(ent));
        if ((rc = sceIoDread(d, &ent)) <= 0)
            break;
        len = strnlen(ent.d_name, sizeof(ent.d_name));
        old = len == 41 && !memcmp(ent.d_name + 32, ".json.old", 10);
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || (!old && (len != 37 || memcmp(ent.d_name + 32, ".json", 6))))
            continue;
        ent.d_name[32] = '\0';
        if (!vjo_queue_valid_id(ent.d_name))
            continue;
        if (k == cap) {
            (*more)++;
            continue;
        }
        memset(&s[k], 0, sizeof(s[k]));
        from_hex(ent.d_name, s[k].id);
        s[k++].old = (uint8_t)old;
    }
    if (rc < 0)
        io_fail("dread", VJO_AFV_OUTBOX_DIR, rc);
    if ((closed = sceIoDclose(d)) < 0)
        io_fail("dclose", VJO_AFV_OUTBOX_DIR, closed);
    if (rc < 0)
        return -1;
    seen_sort(s, k);
    { /* an .old beside its <id>.json is not read: the file itself is there */
        int w = 0;
        for (int i = 0; i < k; i++)
            if (!(s[i].old && w && !id_cmp(s[w - 1].id, s[i].id)))
                s[w++] = s[i];
        k = w;
    }
    *out = s;
    *n = k;
    return 0;
}

static void result_path(char path[PATH_CAP], const char *hex, int old)
{
    snprintf(path, PATH_CAP, "%s/%s.json%s", VJO_AFV_OUTBOX_DIR, hex, old ? ".old" : "");
}

static void queue_path(char path[PATH_CAP], const char *dir, const char *hex, const char *ext)
{
    snprintf(path, PATH_CAP, "%s/%s.%s", dir, hex, ext);
}

/* A final result's file goes once it is applied (the interface lets a client remove a final result),
 * and with it a leftover <id>.json.old, an earlier version of it (a replace stopped before its last
 * step: the app writes no more for a final result, so nothing else would remove it). */
static void remove_result(const char *hex, int old, VjoAfvResults *r)
{
    char path[PATH_CAP];
    int rc;
    result_path(path, hex, old);
    if ((rc = sceIoRemove(path)) < 0 && rc != MISSING) {
        io_fail("remove", path, rc);
        r->failed++;
        return;
    }
    r->removed++;
    if (!old) {
        result_path(path, hex, 1);
        if ((rc = sceIoRemove(path)) < 0 && rc != MISSING)
            io_fail("remove", path, rc);
    }
}

/* The queued card becomes <id>.duplicate.json, the queue's "found in Anki" state (its screenshot
 * stays beside it). 0 or -1. */
static int mark_duplicate(const char *hex)
{
    char from[PATH_CAP], to[PATH_CAP];
    int rc, there;
    queue_path(from, VJO_QUEUE_DIR, hex, "json");
    queue_path(to, VJO_QUEUE_DIR, hex, "duplicate.json");
    there = present(to);
    if (there < 0)
        return io_fail("getstat", to, there);
    if (there) /* the same record, marked before: the queued copy goes */
        return (rc = sceIoRemove(from)) < 0 ? io_fail("remove", from, rc) : 0;
    return (rc = sceIoRename(from, to)) < 0 ? io_fail("rename", from, rc) : 0;
}

/* One result, read and applied to its card (Seen e; its status and finality recorded in e). */
static void apply_one(VjoArena *a, Seen *e, VjoAfvResults *r)
{
    char hex[VJO_QUEUE_ID_SIZE], path[PATH_CAP], *json = NULL;
    size_t mark = vjo_arena_mark(a), len = 0;
    VjoAfvResult res;
    int got, queued;
    int old;
    to_hex(e->id, hex);
    result_path(path, hex, 0);
    got = read_replaced(a, path, VJO_AFV_RESULT_MAX, &json, &len, &old);
    e->old = (uint8_t)old; /* (the file read: the one an applied result's removal takes) */
    result_path(path, hex, old);
    if (got == 1) { /* gone since the listing, and no .old */
        vjo_arena_release(a, mark);
        return;
    }
    if (got < 0) {
        r->failed++;
        vjo_arena_release(a, mark);
        return;
    }
    if (got == 0 && other_version(a, json, len)) { /* (absent, to a version-1 client) */
        vjo_log("anki for vita: %s: another version's result, left alone", path);
        vjo_arena_release(a, mark);
        return;
    }
    if (got == 2 || vjo_afv_result_parse(a, json, len, hex, &res) < 0) {
        vjo_log("anki for vita: %s: not a version-1 result for its card, ignored", path);
        r->malformed++;
        vjo_arena_release(a, mark);
        return;
    }
    vjo_arena_release(a, mark);
    e->status = (uint8_t)res.status;
    e->final = (uint8_t)vjo_afv_result_final(&res);
    queue_path(path, VJO_QUEUE_DIR, hex, "json");
    if ((queued = present(path)) < 0) {
        io_fail("getstat", path, queued);
        r->failed++;
        return;
    }
    if (!queued) {
        /* not queued (any more): the final result of a card that is archived or marked goes too (an
         * .apkg export took it meanwhile); any other is left alone */
        char archived[PATH_CAP], duplicate[PATH_CAP];
        queue_path(archived, VJO_QUEUE_EXPORTED_DIR, hex, "json");
        queue_path(duplicate, VJO_QUEUE_DIR, hex, "duplicate.json");
        if (e->final && (present(archived) > 0 || present(duplicate) > 0))
            remove_result(hex, e->old, r);
        return;
    }
    switch (res.status) {
    case VJO_AFV_ADDED:
    case VJO_AFV_EXISTS:
        if (!res.synced) {
            r->waiting++;
        } else if (vjo_export_archive_card(a, hex) < 0) {
            r->failed++; /* stays queued: the next pass moves it */
        } else {
            vjo_log("anki for vita: %s: in Anki (%s), moved to the archive", hex,
                    res.status == VJO_AFV_ADDED ? "added" : "there already");
            r->in_anki++;
            remove_result(hex, e->old, r);
        }
        break;
    case VJO_AFV_DUPLICATE:
        if (mark_duplicate(hex) < 0) {
            r->failed++;
        } else {
            vjo_log("anki for vita: %s: a duplicate of a note in Anki, marked", hex);
            r->duplicate++;
            remove_result(hex, e->old, r);
        }
        break;
    default: /* rejected: it stays queued, for an .apkg; its result stays for the page and the sends,
              * which never hand it in again (the app keeps its own record: the id is final there) */
        r->rejected++;
        if (!r->message[0])
            snprintf(r->message, sizeof(r->message), "%s", res.message[0] ? res.message : "no reason given");
        vjo_log("anki for vita: %s: refused (%s)", hex, res.message);
        break;
    }
}

static int stopped(const VjoExportUi *ui)
{
    return ui && ui->stopped && ui->stopped(ui->ud);
}

static void say(const VjoExportUi *ui, const char *what, int done, int total)
{
    char msg[96];
    if (!ui || !ui->status || (done % 8 && done != total))
        return;
    snprintf(msg, sizeof(msg), "%s… %d of %d", what, done, total);
    ui->status(ui->ud, msg);
}

static int sweep_inbox(VjoArena *a);

/* The results pass; seen (sorted, with each one's status) is left in a for the send. */
static int results(VjoArena *a, const VjoExportUi *ui, VjoAfvResults *r, Seen **seen, int *n_seen)
{
    int listed = list_outbox(a, seen, n_seen, VJO_AFV_RESULTS_MAX, &r->more);
    if (listed > 0)
        return VJO_OK; /* no outbox yet */
    if (listed < 0)
        return VJO_E_LOCAL_IO;
    for (int i = 0; i < *n_seen; i++) {
        if (stopped(ui))
            return VJO_E_CANCELLED;
        say(ui, "Reading Anki for Vita's results", i + 1, *n_seen);
        apply_one(a, &(*seen)[i], r);
    }
    if (r->in_anki || r->duplicate || r->rejected || r->removed) /* final results: their media may go */
        r->swept = sweep_inbox(a);
    if (r->in_anki || r->duplicate || r->failed || r->swept)
        sceIoSync("ux0:", 0); /* the moves and removals */
    return VJO_OK;
}

int vjo_afv_results(VjoArena *a, const VjoExportUi *ui, VjoAfvResults *r)
{
    size_t mark = vjo_arena_mark(a);
    Seen *seen = NULL;
    int n = 0, rc;
    memset(r, 0, sizeof(*r));
    rc = results(a, ui, r, &seen, &n);
    vjo_arena_release(a, mark);
    return rc;
}

void vjo_afv_results_text(const VjoAfvResults *r, char *out, size_t cap)
{
    size_t n = 0;
#define ADD(...) (n < cap ? (void)(n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__)) : (void)0)
    out[0] = '\0';
    if (r->in_anki)
        ADD("Anki for Vita: %d card%s in Anki now (out of the queue).", r->in_anki, r->in_anki == 1 ? " is" : "s are");
    if (r->duplicate)
        ADD("%s%d card%s in your collection already (another note): not added.", n ? " " : "Anki for Vita: ",
            r->duplicate, r->duplicate == 1 ? " was" : "s were");
    if (r->rejected)
        ADD("%s%d refused, still queued: %s", n ? " " : "Anki for Vita: ", r->rejected, r->message);
    if (r->failed)
        ADD("%s%d could not be updated (see log.txt).", n ? " " : "Anki for Vita: ", r->failed);
    if (r->malformed)
        ADD("%s%d unreadable result%s (see log.txt).", n ? " " : "Anki for Vita: ", r->malformed,
            r->malformed == 1 ? "" : "s");
#undef ADD
    if (n >= cap)
        ellipsize(out, cap);
}

/* ---------------- a card's note ---------------- */

int64_t vjo_afv_created(int64_t card_time)
{
    /* the .apkg's time for the card (VJO_APKG_NO_TIME without one), within the interface's range */
    return card_time >= CREATED_MIN && card_time <= CREATED_MAX ? card_time : VJO_APKG_NO_TIME;
}

/* A signed 64-bit decimal without printf (the Vita's newlib and %lld: core/apkg.c's buf_i64). */
static void put_i64(VjoBuf *b, int64_t v)
{
    char d[24];
    int n = 0;
    uint64_t u = v < 0 ? 0u - (uint64_t)v : (uint64_t)v;
    do {
        d[n++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u);
    if (v < 0)
        vjo_buf_putc(b, '-');
    while (n)
        vjo_buf_putc(b, d[--n]);
}

char *vjo_afv_note_json(VjoArena *a, const char id[VJO_QUEUE_ID_SIZE], const VjoAnkiNote *note, const char *media,
                        int64_t created, size_t *len)
{
    size_t mark = vjo_arena_mark(a);
    char guid[VJO_APKG_GUID_MAX], *value[VJO_LAPIS_FIELDS], *out;
    uint8_t raw[ID_BYTES];
    VjoBuf b;
    int first = 1;
    from_hex(id, raw);
    vjo_apkg_guid(raw, guid);
    for (int f = 0; f < VJO_LAPIS_FIELDS; f++) {
        if (!(value[f] = vjo_lapis_field_value(a, f, note, media))) {
            vjo_arena_release(a, mark);
            return NULL;
        }
        for (char *p = value[f]; *p; p++) /* Anki's field separator: a space, as in the package's flds */
            if (*p == 0x1F)
                *p = ' ';
    }
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"v\":1,\"id\":\"");
    vjo_buf_puts(&b, id);
    vjo_buf_puts(&b, "\",\"guid\":");
    vjo_json_write_string(&b, guid);
    vjo_buf_puts(&b, ",\"notetype\":{\"name\":\"Lapis\",\"id\":");
    put_i64(&b, vjo_lapis_notetype_id);
    vjo_buf_puts(&b, ",\"definition\":\"" VJO_AFV_NOTETYPE_FILE "\"},\"deck\":");
    vjo_json_write_string(&b, vjo_lapis_deck_name);
    vjo_buf_puts(&b, ",\"fields\":{");
    for (int f = 0; f < VJO_LAPIS_FIELDS; f++) {
        if (!value[f][0] && f != VJO_LAPIS_EXPRESSION && f != VJO_LAPIS_EXPRESSION_READING)
            continue; /* empty: not given (the interface: a field not given is empty) */
        if (!first)
            vjo_buf_putc(&b, ',');
        first = 0;
        vjo_json_write_string(&b, vjo_lapis_field[f]);
        vjo_buf_putc(&b, ':');
        vjo_json_write_string(&b, value[f]);
    }
    vjo_buf_puts(&b, "},\"tags\":[");
    vjo_json_write_string(&b, vjo_lapis_tag);
    vjo_buf_putc(&b, ']');
    if (media) {
        vjo_buf_puts(&b, ",\"media\":[");
        vjo_json_write_string(&b, media);
        vjo_buf_putc(&b, ']');
    }
    vjo_buf_puts(&b, ",\"created\":");
    put_i64(&b, created);
    vjo_buf_puts(&b, ",\"duplicate_check\":{\"fields\":[\"Expression\",\"ExpressionReading\"]}}\n");
    if (b.oom || !(out = vjo_buf_cstr(&b))) {
        vjo_arena_release(a, mark);
        return NULL;
    }
    *len = b.len;
    return out;
}

/* ---------------- send ---------------- */

/* The IDs of Rubi's notes in the inbox (<32 hex>.json there), sorted: the cards a send leaves alone.
 * 0, 1 (more than cap: each card's file is checked by name instead), or -1 (the inbox cannot be read:
 * nothing is handed over, so nothing could be replaced). */
static int list_inbox(VjoArena *a, Seen **out, int *n, int cap)
{
    SceUID d;
    SceIoDirent ent;
    Seen *ids;
    int k = 0, rc, closed;
    *out = NULL;
    *n = 0;
    if (!is_dir(VJO_AFV_INBOX_DIR))
        return 0;
    if (!(ids = (Seen *)vjo_arena_alloc(a, sizeof(Seen) * (size_t)cap)))
        return -1;
    if ((d = sceIoDopen(VJO_AFV_INBOX_DIR)) < 0)
        return io_fail("dopen", VJO_AFV_INBOX_DIR, d);
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if ((rc = sceIoDread(d, &ent)) <= 0)
            break;
        if (strnlen(ent.d_name, sizeof(ent.d_name)) != 37 || memcmp(ent.d_name + 32, ".json", 6))
            continue; /* (its directories, our .tmp files, anything else) */
        ent.d_name[32] = '\0';
        if (!vjo_queue_valid_id(ent.d_name))
            continue;
        if (k == cap) {
            k = -1;
            rc = 0;
            break;
        }
        memset(&ids[k], 0, sizeof(ids[k]));
        from_hex(ent.d_name, ids[k++].id);
    }
    if (rc < 0)
        io_fail("dread", VJO_AFV_INBOX_DIR, rc);
    if ((closed = sceIoDclose(d)) < 0)
        io_fail("dclose", VJO_AFV_INBOX_DIR, closed);
    if (rc < 0)
        return -1;
    if (k < 0)
        return 1;
    seen_sort(ids, k);
    *out = ids;
    *n = k;
    return 0;
}

/* A media name of Rubi's, vitajp_<32 lowercase hex>.jpg, as its 16 bytes (0), or -1. */
static int rubi_media(const char *name, size_t n, uint8_t id[ID_BYTES])
{
    char hex[VJO_QUEUE_ID_SIZE];
    if (n != 43 || memcmp(name, "vitajp_", 7) || memcmp(name + 39, ".jpg", 4))
        return -1;
    memcpy(hex, name + 7, 32);
    hex[32] = '\0';
    if (!vjo_queue_valid_id(hex))
        return -1;
    from_hex(hex, id);
    return 0;
}

/* The client's own inbox files that no note needs any more. The interface: the client removes its
 * media files once every note that names them has a final result, and its note type file once no
 * note in its inbox names it; the app removes a note's file once its result is final. So a media file
 * of Rubi's (vitajp_<hash>.jpg) that no note left in Rubi's inbox names goes (one an interrupted send
 * wrote for a note it never put in place too: a send writes it again), and the note type's file when
 * no note is left. Anything that cannot be read keeps everything. Never during a send's own steps.
 * The files removed. */
static int sweep_inbox(VjoArena *a)
{
    size_t mark = vjo_arena_mark(a);
    Seen *notes = NULL, *needed, *gone;
    int n_notes = 0, n_needed = 0, n_gone = 0, cap, removed = 0, listed, notetype;
    SceUID d;
    SceIoDirent ent;
    if (!is_dir(VJO_AFV_INBOX_DIR))
        return 0;
    /* Rubi's media files first: with none (and no note type file) there is nothing to read */
    if (!(gone = (Seen *)vjo_arena_alloc(a, sizeof(Seen) * VJO_AFV_RESULTS_MAX)))
        return 0;
    if (is_dir(VJO_AFV_MEDIA_DIR) && (d = sceIoDopen(VJO_AFV_MEDIA_DIR)) >= 0) {
        for (;;) {
            memset(&ent, 0, sizeof(ent));
            if (sceIoDread(d, &ent) <= 0)
                break;
            if (SCE_S_ISREG(ent.d_stat.st_mode) && n_gone < VJO_AFV_RESULTS_MAX &&
                !rubi_media(ent.d_name, strnlen(ent.d_name, sizeof(ent.d_name)), gone[n_gone].id))
                n_gone++;
        }
        sceIoDclose(d);
    }
    notetype = present(VJO_AFV_NOTETYPES_DIR "/" VJO_AFV_NOTETYPE_FILE) > 0;
    if (!n_gone && !notetype) {
        vjo_arena_release(a, mark);
        return 0;
    }
    /* then the notes still in the inbox (a few, once the app has synced: it removes the final ones) */
    if ((listed = list_inbox(a, &notes, &n_notes, VJO_AFV_SWEEP_NOTES)) != 0) {
        if (listed > 0)
            vjo_log("anki for vita: over %d notes wait in the inbox: its media are removed once they are fewer",
                    VJO_AFV_SWEEP_NOTES);
        vjo_arena_release(a, mark);
        return 0; /* (more notes than a sweep reads, or the inbox unreadable: everything stays) */
    }
    cap = 2 * n_notes + 64;
    if (!(needed = (Seen *)vjo_arena_alloc(a, sizeof(Seen) * (size_t)cap))) {
        vjo_arena_release(a, mark);
        return 0;
    }
    /* the media files the notes still in the inbox name */
    for (int i = 0; i < n_notes; i++) {
        char hex[VJO_QUEUE_ID_SIZE], path[PATH_CAP], *json = NULL;
        size_t note_mark = vjo_arena_mark(a), len = 0;
        VjoJson j;
        int t, ok;
        to_hex(notes[i].id, hex);
        snprintf(path, sizeof(path), "%s/%s.json", VJO_AFV_INBOX_DIR, hex);
        ok = read_small(a, path, VJO_AFV_NOTE_MAX, &json, &len);
        if (ok == 1) /* taken since the listing (its result is final) */
            continue;
        if (ok != 0 || vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT)) {
            vjo_log("anki for vita: %s cannot be read: no media file is removed", path);
            vjo_arena_release(a, mark);
            return 0;
        }
        t = vjo_json_obj_get(&j, 0, "media");
        if (t >= 0 && vjo_json_is_type(&j, t, JSMN_ARRAY))
            for (int k = 0, e = t + 1; k < j.t[t].size; k++, e = vjo_json_skip(&j, e)) {
                int n;
                const char *name = vjo_json_is_type(&j, e, JSMN_STRING) ? token_text(&j, e, &n) : NULL;
                if (!name || rubi_media(name, (size_t)n, needed[n_needed].id) < 0)
                    continue;
                if (++n_needed == cap) {
                    vjo_arena_release(a, mark);
                    return 0;
                }
            }
        vjo_arena_release(a, note_mark);
    }
    for (int i = 0; i < n_needed; i++)
        needed[i].old = 0;
    seen_sort(needed, n_needed);
    /* Rubi's media files none of them names go */
    for (int i = 0; i < n_gone; i++) {
        char hex[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
        int rc;
        if (seen_find(needed, n_needed, gone[i].id))
            continue;
        to_hex(gone[i].id, hex);
        snprintf(path, sizeof(path), "%s/vitajp_%s.jpg", VJO_AFV_MEDIA_DIR, hex);
        if ((rc = sceIoRemove(path)) < 0 && rc != MISSING)
            io_fail("remove", path, rc);
        else
            removed++;
    }
    if (!n_notes && notetype) {
        int rc = sceIoRemove(VJO_AFV_NOTETYPES_DIR "/" VJO_AFV_NOTETYPE_FILE);
        if (rc < 0)
            io_fail("remove", VJO_AFV_NOTETYPES_DIR "/" VJO_AFV_NOTETYPE_FILE, rc);
        else
            removed++;
    }
    if (removed)
        vjo_log("anki for vita: %d of Rubi's inbox files no note needs, removed", removed);
    vjo_arena_release(a, mark);
    return removed;
}

/* Our own leftovers: the .tmp files an interrupted send left in the inbox (only Rubi writes there;
 * Anki for Vita ignores every *.tmp). */
static void remove_stale_temps(const char *dir)
{
    SceUID d;
    SceIoDirent ent;
    char path[PATH_CAP + 64];
    if (!is_dir(dir) || (d = sceIoDopen(dir)) < 0)
        return;
    for (;;) {
        size_t n;
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0)
            break;
        n = strnlen(ent.d_name, sizeof(ent.d_name));
        if (SCE_S_ISREG(ent.d_stat.st_mode) && n > 4 && !memcmp(ent.d_name + n - 4, ".tmp", 4) &&
            snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name) < (int)sizeof(path))
            sceIoRemove(path);
    }
    sceIoDclose(d);
}

typedef struct {
    VjoArena *a;
    const VjoExportUi *ui;
    VjoAfvSendResult *r;
    const Seen *seen;
    int n_seen;
    Seen *inbox;
    int n_inbox, inbox_by_name; /* the inbox was too large to list: check each card's file */
    uint8_t (*staged)[ID_BYTES]; /* the notes written as <id>.json.tmp, renamed after the sync */
    int n_staged, cap_staged, total, done;
} Send;

static void note_path(char path[PATH_CAP], const char *hex, int tmp)
{
    snprintf(path, PATH_CAP, "%s/%s.json%s", VJO_AFV_INBOX_DIR, hex, tmp ? ".tmp" : "");
}

/* One queued card: its media file, then its note's .tmp (step 1). */
static int stage_card(void *ud, const char *hex)
{
    Send *s = (Send *)ud;
    VjoAfvSendResult *r = s->r;
    size_t mark = vjo_arena_mark(s->a), json_len = 0, jpeg_len = 0;
    char path[PATH_CAP], media[48], *json;
    const uint8_t *jpeg = NULL;
    const Seen *e;
    uint8_t id[ID_BYTES];
    VjoAnkiNote note;
    int picture = 0, rc;
    if (stopped(s->ui)) {
        r->stopped++;
        return 0; /* (counted: every card after the stop stays as it is) */
    }
    say(s->ui, "Handing the cards over", ++s->done, s->total);
    from_hex(hex, id);
    if (s->inbox_by_name ? (note_path(path, hex, 0), present(path) != 0)
                         : seen_find(s->inbox, s->n_inbox, id) != NULL) {
        r->already++;
        return 0;
    }
    if ((e = seen_find(s->seen, s->n_seen, id)) != NULL && e->status == VJO_AFV_REJECTED) {
        r->refused++; /* refused: final under this id (the app would drop it unread), for an .apkg */
        return 0;
    }
    if (e && e->final) {
        r->failed++; /* its result is final, but the pass could not apply it (log.txt): not again */
        return 0;
    }
    if (s->n_staged >= s->cap_staged) { /* (a send's cap, or cards saved since the count) */
        r->left++;
        return 0;
    }
    if (vjo_export_read_card(s->a, 0, hex, &note, &picture) < 0) {
        r->unreadable++;
        vjo_arena_release(s->a, mark);
        return 0;
    }
    if (picture) {
        int unavailable = 0;
        if (!(jpeg = vjo_export_read_jpeg(s->a, 0, hex, &jpeg_len, &unavailable))) {
            if (!unavailable) { /* a read that failed: later */
                r->unreadable++;
                vjo_arena_release(s->a, mark);
                return 0;
            }
            r->missing_picture++; /* without it, as the export does */
        }
    }
    if (jpeg) {
        vjo_apkg_media_name(jpeg, jpeg_len, media);
        snprintf(path, sizeof(path), "%s/%s", VJO_AFV_MEDIA_DIR, media);
        /* there already (another card's, the same picture: its name is its hash): checked to be */
        if (put_once(s->a, path, jpeg, jpeg_len) < 0) {
            r->failed++;
            vjo_arena_release(s->a, mark);
            return 0;
        }
    }
    json = vjo_afv_note_json(s->a, hex, &note, jpeg ? media : NULL, vjo_afv_created(note.unix_time), &json_len);
    if (json && !utf8_valid(json, json_len)) { /* (a record's text that is not UTF-8: the interface refuses it) */
        vjo_log("anki for vita: %s: its text is not valid UTF-8: not handed over", hex);
        r->unreadable++;
        vjo_arena_release(s->a, mark);
        return 0;
    }
    if (!json || json_len > VJO_AFV_NOTE_MAX) {
        vjo_log("anki for vita: %s: its note is over %u KiB (or out of memory): not handed over", hex,
                VJO_AFV_NOTE_MAX >> 10);
        r->too_large++;
        vjo_arena_release(s->a, mark);
        return 0;
    }
    note_path(path, hex, 0);
    rc = write_new(path, json, json_len, 0);
    vjo_arena_release(s->a, mark);
    if (rc == 1) {
        r->already++;
        return 0;
    }
    if (rc < 0) {
        r->failed++;
        return 0;
    }
    memcpy(s->staged[s->n_staged++], id, ID_BYTES);
    return 0;
}

int vjo_afv_send(VjoArena *a, const VjoExportUi *ui, VjoAfvSendResult *r, char *msg, size_t cap)
{
    size_t base = vjo_arena_mark(a);
    char path[PATH_CAP], tmp[PATH_CAP], hex[VJO_QUEUE_ID_SIZE];
    Seen *seen = NULL;
    Send s;
    int queued, rc, listed, k;
    memset(r, 0, sizeof(*r));
    memset(&s, 0, sizeof(s));
    msg[0] = '\0';
    if (!(r->installed = vjo_afv_speaks_1(a))) {
        snprintf(msg, cap, "Anki for Vita is not installed (or too old for Rubi): get it from " VJO_AFV_WHERE
                           ". Nothing was changed.");
        return r->rc = VJO_E_LOCAL_IO;
    }
    /* 1. its results first: a card in Anki leaves the queue and is not handed over again */
    if ((rc = results(a, ui, &r->results, &seen, &s.n_seen)) < 0) {
        snprintf(msg, cap, rc == VJO_E_CANCELLED ? "Stopped. Nothing was handed over."
                                                 : "Cannot read Anki for Vita's outbox (see log.txt). Nothing was "
                                                   "handed over.");
        vjo_arena_release(a, base);
        return r->rc = rc;
    }
    s.seen = seen;
    /* 2. the queue, and the inbox's notes, which are left alone */
    queued = vjo_export_scan(0, NULL, NULL);
    if (queued < 0) {
        snprintf(msg, cap, "Cannot read %s (see log.txt). Nothing was handed over.", VJO_QUEUE_DIR);
        vjo_arena_release(a, base);
        return r->rc = VJO_E_LOCAL_IO;
    }
    if (!queued) {
        char done[VJO_AFV_MESSAGE_MAX * 2];
        vjo_afv_results_text(&r->results, done, sizeof(done));
        snprintf(msg, cap, "%s%sNo cards queued: save words in the overlay first.", done, done[0] ? " " : "");
        vjo_arena_release(a, base);
        return r->rc = VJO_OK;
    }
    s.a = a;
    s.ui = ui;
    s.r = r;
    s.total = queued;
    listed = list_inbox(a, &s.inbox, &s.n_inbox, VJO_AFV_SEND_MAX);
    s.inbox_by_name = listed == 1;
    s.cap_staged = queued < VJO_AFV_SEND_MAX ? queued : VJO_AFV_SEND_MAX;
    s.staged = (uint8_t(*)[ID_BYTES])vjo_arena_alloc(a, ID_BYTES * (size_t)s.cap_staged);
    if (listed < 0 || !s.staged) {
        snprintf(msg, cap, listed < 0 ? "Cannot read Anki for Vita's inbox (see log.txt). Nothing was handed over."
                                      : "Not enough memory for %d cards. Nothing was handed over.", queued);
        vjo_arena_release(a, base);
        return r->rc = listed < 0 ? VJO_E_LOCAL_IO : VJO_E_OOM;
    }
    mkdirs();
    remove_stale_temps(VJO_AFV_INBOX_DIR);
    remove_stale_temps(VJO_AFV_MEDIA_DIR);
    remove_stale_temps(VJO_AFV_NOTETYPES_DIR);
    /* 3. the note type, then each card's screenshot and its note's .tmp. A note type file of another
     * content is taken back only when no note of Rubi's is in the inbox (the client owns it, and
     * removes it once no note names it); else nothing is handed over. */
    {
        static const char nt_path[] = VJO_AFV_NOTETYPES_DIR "/" VJO_AFV_NOTETYPE_FILE;
        size_t nt_len = strlen(vjo_lapis_notetype);
        int put = put_once(a, nt_path, vjo_lapis_notetype, nt_len);
        if (put < 0 && !s.n_inbox && !s.inbox_by_name && same_bytes(a, nt_path, vjo_lapis_notetype, nt_len) == 0 &&
            sceIoRemove(nt_path) >= 0) {
            vjo_log("anki for vita: %s was not Rubi's, and no note names it: written again", nt_path);
            put = put_once(a, nt_path, vjo_lapis_notetype, nt_len);
        }
        if (put < 0) {
            snprintf(msg, cap, "Cannot write %s (memory card full? see log.txt). Nothing was handed over.", nt_path);
            vjo_arena_release(a, base);
            return r->rc = VJO_E_LOCAL_IO;
        }
    }
    if (vjo_export_scan(0, stage_card, &s) < 0)
        vjo_log("anki for vita: the queue could not be read to its end; the rest is handed over next time");
    /* 4. synced: every note's files are on the card before its note is; then the notes */
    rc = sceIoSync("ux0:", 0);
    if (rc < 0)
        io_fail("sync", "ux0:", rc);
    for (k = 0; k < s.n_staged; k++) {
        int n;
        to_hex(s.staged[k], hex);
        note_path(path, hex, 0);
        note_path(tmp, hex, 1);
        if (rc < 0 || stopped(ui)) { /* not synced, or stopped: none of the rest */
            sceIoRemove(tmp);
            if (rc < 0)
                r->failed++;
            else
                r->stopped++;
            continue;
        }
        if ((n = sceIoRename(tmp, path)) < 0) {
            sceIoRemove(tmp);
            if (present(path) > 0)
                r->already++; /* (the rename refuses a name that is there: never replaced) */
            else {
                io_fail("rename", tmp, n);
                r->failed++;
            }
            continue;
        }
        r->sent++;
    }
    if (s.n_staged && (k = sceIoSync("ux0:", 0)) < 0)
        io_fail("sync", "ux0:", k);
    vjo_log("anki for vita: send: %d handed over, %d in the inbox already, %d unreadable, %d too large, %d failed, "
            "%d without screenshots, %d left, %d stopped; results: %d in Anki, %d duplicates, %d refused, %d waiting",
            r->sent, r->already, r->unreadable, r->too_large, r->failed, r->missing_picture, r->left, r->stopped,
            r->results.in_anki, r->results.duplicate, r->results.rejected, r->results.waiting);
    vjo_arena_release(a, base);
    {
        size_t n = 0;
        char done[VJO_AFV_MESSAGE_MAX * 2];
#define ADD(...) (n < cap ? (void)(n += (size_t)snprintf(msg + n, cap - n, __VA_ARGS__)) : (void)0)
        if (r->sent)
            ADD("Handed %d card%s to Anki for Vita: it adds them to your collection at its next sync.", r->sent,
                r->sent == 1 ? "" : "s");
        else if (!r->stopped)
            ADD("No new cards to hand over.");
        if (r->refused)
            ADD(" %d refused by Anki for Vita before stay%s queued: export %s as an .apkg.", r->refused,
                r->refused == 1 ? "s" : "", r->refused == 1 ? "it" : "them");
        if (r->stopped)
            ADD("%sStopped: %d stay queued, not handed over.", n ? " " : "", r->stopped);
        if (r->already)
            ADD(" %d %s in its inbox already.", r->already, r->already == 1 ? "was" : "were");
        if (r->missing_picture)
            ADD(" %d handed over without %s.", r->missing_picture,
                r->missing_picture == 1 ? "its screenshot" : "their screenshots");
        if (r->unreadable + r->too_large + r->failed)
            ADD(" %d stay queued; see log.txt.", r->unreadable + r->too_large + r->failed);
        if (r->left)
            ADD(" %d more: send again.", r->left);
        {
            VjoAfvResults shown = r->results; /* (the refused ones are said above) */
            if (r->refused)
                shown.rejected = 0;
            vjo_afv_results_text(&shown, done, sizeof(done));
        }
        if (done[0])
            ADD(" %s", done);
#undef ADD
        if (n >= cap)
            ellipsize(msg, cap);
    }
    return r->rc = VJO_OK;
}
