#include "anki_export.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>

#include "../core/anki_queue.h"
#include "../core/errors.h"
#include "../core/word_key.h"
#include "cards.h"

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

/* ---------------- the words of every card (#314) ---------------- */

static int status_every(const VjoExportUi *ui, const char *what, int done, int total);
static int present(const char *path);

/* The cache: a 16-byte head (WORDS_MAGIC, the entries' count and their CRC-32, both big-endian), then each card's
 * word, sorted by ID: its ID's first 8 bytes, its key (8 bytes) and its time (4), big-endian. A card's ID is its
 * record's checksum, so an entry never goes stale: it is only dropped once its card is gone from both directories.
 * The file is the app's alone (neither the Shell nor an export reads it otherwise), outside anki_queue/, and only
 * ever a copy of what the records say: a damaged, short or unknown one is rebuilt from them. */
#define WORDS_PATH VJO_EXPORT_DATA_DIR "/anki_words.bin"
#define WORDS_TEMP WORDS_PATH ".tmp"
#define WORDS_MAGIC "RubiWd1\n" /* 1: core/word_key.h's key; a new key would be a new magic */
#define WORDS_HEAD 16
#define WORDS_ENTRY 20
#define WORDS_CHUNK 128 /* entries a read or write takes, on the stack (2,560 B) */
#define WORD_SEEN 0x100u /* (the build's own: the card is in a directory now) */

static uint64_t be_get(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = v << 8 | p[i];
    return v;
}

static void be_put(uint8_t *p, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)v;
}

/* A record's time as a word's (and as the package orders it): VJO_APKG_NO_TIME without one; 32 bits, as the Shell
 * writes it (a hand-made record's larger time is the largest). */
static uint32_t word_time(int64_t t)
{
    return t <= 0 ? (uint32_t)VJO_APKG_NO_TIME : t > (int64_t)0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)t;
}

/* (key, time, ID) order; by_id: ID order alone */
static int word_less(const VjoWord *x, const VjoWord *y, int by_id)
{
    if (by_id)
        return x->id < y->id;
    if (x->key != y->key)
        return x->key < y->key;
    return x->time != y->time ? x->time < y->time : x->id < y->id;
}

static void word_down(VjoWord *w, int n, int i, int by_id)
{
    for (;;) {
        int k = 2 * i + 1;
        VjoWord t;
        if (k >= n)
            return;
        if (k + 1 < n && word_less(&w[k], &w[k + 1], by_id))
            k++;
        if (!word_less(&w[i], &w[k], by_id))
            return;
        t = w[i];
        w[i] = w[k];
        w[k] = t;
        i = k;
    }
}

/* heapsort: in place, for any number of cards */
static void word_sort(VjoWord *w, int n, int by_id)
{
    for (int i = n / 2 - 1; i >= 0; i--)
        word_down(w, n, i, by_id);
    for (int end = n - 1; end > 0; end--) {
        VjoWord t = w[0];
        w[0] = w[end];
        w[end] = t;
        word_down(w, end, 0, by_id);
    }
}

/* The next n bytes of fd into p, read again after a short read: 0, or -1. */
static int read_all(SceUID fd, void *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        int k = sceIoRead(fd, (char *)p + done, (SceSize)(n - done));
        if (k <= 0)
            return -1;
        done += (size_t)k;
    }
    return 0;
}

/* n bytes of p to fd, written again after a short write: 0, or -1 (logged). */
static int write_all(SceUID fd, const void *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        int k = sceIoWrite(fd, (const char *)p + done, (SceSize)(n - done));
        if (k <= 0)
            return io_fail("write", WORDS_TEMP, k);
        done += (size_t)k;
    }
    return 0;
}

/* Entries [at, at + m) of w in the file's form, into buf. */
static void words_encode(const VjoWord *w, int at, int m, uint8_t *buf)
{
    for (int i = 0; i < m; i++) {
        uint8_t *e = buf + i * WORDS_ENTRY;
        be_put(e, w[at + i].id, 8);
        be_put(e + 8, w[at + i].key, 8);
        be_put(e + 16, w[at + i].time, 4);
    }
}

/* The cache's entries into a, in ID order (each one's flags 0): their count; 0 for no cache (none, or one that is
 * damaged or unknown: logged), with nothing left in a; -1 when its words do not fit in a (the records' would not
 * either). */
static int words_load(VjoArena *a, VjoWord **out)
{
    uint8_t head[WORDS_HEAD], buf[WORDS_CHUNK * WORDS_ENTRY];
    size_t mark = vjo_arena_mark(a);
    SceUID fd = sceIoOpen(WORDS_PATH, SCE_O_RDONLY, 0);
    SceOff size;
    uint32_t n = 0, crc = 0;
    VjoWord *w = NULL;
    const char *why = NULL;
    *out = NULL;
    if (fd < 0) {
        if (fd != MISSING)
            io_fail("open", WORDS_PATH, fd);
        return 0;
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < WORDS_HEAD || sceIoLseek(fd, 0, SCE_SEEK_SET) != 0 || read_all(fd, head, WORDS_HEAD) < 0 ||
        memcmp(head, WORDS_MAGIC, 8))
        why = "not a cache of this build's";
    else if ((n = (uint32_t)be_get(head + 8, 4), (uint64_t)size != WORDS_HEAD + (uint64_t)n * WORDS_ENTRY))
        why = "its size is not its count's";
    else if (n && !(w = (VjoWord *)vjo_arena_alloc(a, sizeof(*w) * n))) {
        vjo_log("anki export: %s: %u words, more than memory holds", WORDS_PATH, (unsigned)n);
        sceIoClose(fd);
        return -1;
    }
    for (uint32_t done = 0; !why && done < n;) {
        uint32_t k = n - done < WORDS_CHUNK ? n - done : WORDS_CHUNK;
        if (read_all(fd, buf, (size_t)k * WORDS_ENTRY) < 0) {
            why = "a read failed";
            break;
        }
        crc = vjo_crc32(crc, buf, (size_t)k * WORDS_ENTRY);
        for (uint32_t i = 0; i < k; i++, done++) {
            const uint8_t *e = buf + i * WORDS_ENTRY;
            w[done].id = be_get(e, 8);
            w[done].key = be_get(e + 8, 8);
            w[done].time = (uint32_t)be_get(e + 16, 4);
            w[done].flags = 0;
            if (done && w[done].id <= w[done - 1].id) {
                why = "not in order";
                break;
            }
        }
    }
    sceIoClose(fd);
    if (!why && crc != (uint32_t)be_get(head + 12, 4))
        why = "its checksum does not match";
    if (why) {
        vjo_log("anki export: %s ignored (%s): the words are read from the cards", WORDS_PATH, why);
        vjo_arena_release(a, mark);
        return 0;
    }
    *out = w;
    return (int)n;
}

/* Writes the cache (w: n words in ID order) under its temporary name, then in its place. A failure is logged and
 * harmless: the next build reads the records again. */
static void words_save(const VjoWord *w, int n)
{
    uint8_t head[WORDS_HEAD], buf[WORDS_CHUNK * WORDS_ENTRY];
    uint32_t crc = 0;
    int rc = 0, k;
    SceUID fd;
    for (int done = 0; done < n; done += WORDS_CHUNK) { /* the entries' CRC first: the head is written ahead of them */
        int m = n - done < WORDS_CHUNK ? n - done : WORDS_CHUNK;
        words_encode(w, done, m, buf);
        crc = vjo_crc32(crc, buf, (size_t)m * WORDS_ENTRY);
    }
    memcpy(head, WORDS_MAGIC, 8);
    be_put(head + 8, (uint32_t)n, 4);
    be_put(head + 12, crc, 4);
    sceIoMkdir(VJO_EXPORT_DATA_DIR, 0777);
    if ((fd = sceIoOpen(WORDS_TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0) {
        io_fail("open", WORDS_TEMP, fd);
        return;
    }
    rc = write_all(fd, head, WORDS_HEAD);
    for (int done = 0; !rc && done < n; done += WORDS_CHUNK) {
        int m = n - done < WORDS_CHUNK ? n - done : WORDS_CHUNK;
        words_encode(w, done, m, buf);
        rc = write_all(fd, buf, (size_t)m * WORDS_ENTRY);
    }
    if ((k = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", WORDS_TEMP, k);
    /* never renamed over a file: the old cache goes first (a stop between the two leaves none: rebuilt) */
    if (!rc && present(WORDS_PATH) == 1 && (k = sceIoRemove(WORDS_PATH)) < 0)
        rc = io_fail("remove", WORDS_PATH, k);
    if (!rc && (k = sceIoRename(WORDS_TEMP, WORDS_PATH)) < 0)
        rc = io_fail("rename", WORDS_TEMP, k);
    if (rc)
        sceIoRemove(WORDS_TEMP);
}

typedef struct {
    VjoArena *a;
    VjoWord *c, *w; /* the cache's (in ID order); every word (the cache's, then the new ones after them) */
    int nc, n;
    int queue;      /* the directory scanned is the queue */
    int seen, total, read, unreadable, oom;
    int max_reads, more; /* more: a record to read past max_reads (the scan stopped there) */
    const VjoExportUi *ui;
} Words;

/* The cache's entry of this ID, or -1. */
static int cached(const Words *b, uint64_t id)
{
    int lo = 0, hi = b->nc - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (b->c[mid].id == id)
            return mid;
        if (b->c[mid].id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int word_of(void *ud, const char *hex)
{
    Words *b = (Words *)ud;
    uint32_t flags = WORD_SEEN | (b->queue ? VJO_WORD_QUEUED : 0u);
    uint8_t id[16];
    VjoAnkiNote note;
    VjoWord *slot;
    size_t before = vjo_arena_mark(b->a), mark;
    int i, picture;
    if (b->ui && status_every(b->ui, "Checking for words mined before", ++b->seen, b->total) < 0)
        return -1;
    from_hex(hex, id);
    if ((i = cached(b, be_get(id, 8))) >= 0) {
        b->c[i].flags |= flags;
        return 0;
    }
    if (b->max_reads && b->read + b->unreadable >= b->max_reads) {
        b->more = 1;
        return -1;
    }
    /* a word to read: its place after the others' (the record's work above it is released after it) */
    if (!(slot = (VjoWord *)vjo_arena_alloc(b->a, sizeof(*slot))) || (b->n && slot != b->w + b->n)) {
        b->oom = 1;
        return -1;
    }
    if (!b->n)
        b->w = slot;
    mark = vjo_arena_mark(b->a);
    if (read_card(b->a, !b->queue, hex, &note, &picture) < 0) {
        b->unreadable++;
        vjo_arena_release(b->a, before); /* its place too */
        return 0;
    }
    slot->key = vjo_word_key(note.spelling, note.reading);
    slot->id = be_get(id, 8);
    slot->time = word_time(note.unix_time);
    slot->flags = flags;
    vjo_arena_release(b->a, mark);
    b->n++;
    b->read++;
    return 0;
}

int vjo_export_words(VjoArena *a, const VjoExportUi *ui, int max_reads, VjoWords *out)
{
    Words b;
    int queued, archived, rc = 0, n, dropped = 0;
    size_t base = vjo_arena_mark(a);
    memset(out, 0, sizeof(*out));
    memset(&b, 0, sizeof(b));
    queued = scan(VJO_EXPORT_QUEUE_DIR, NULL, NULL);
    archived = scan(VJO_EXPORT_ARCHIVE_DIR, NULL, NULL);
    if (queued < 0 || archived < 0)
        return VJO_E_LOCAL_IO;
    b.a = a;
    b.ui = ui;
    b.max_reads = max_reads;
    b.total = queued + archived;
    if ((b.nc = words_load(a, &b.c)) < 0)
        return VJO_E_OOM;
    b.w = b.c;
    b.n = b.nc;
    b.queue = 1;
    /* (a scan that word_of stops returns its count: b says why) */
    if (scan(VJO_EXPORT_QUEUE_DIR, word_of, &b) < 0)
        rc = VJO_E_LOCAL_IO;
    b.queue = 0;
    if (!rc && !b.oom && !b.more && scan(VJO_EXPORT_ARCHIVE_DIR, word_of, &b) < 0)
        rc = VJO_E_LOCAL_IO;
    if (b.oom)
        rc = VJO_E_OOM;
    else if (ui && ui->stopped && ui->stopped(ui->ud))
        rc = VJO_E_CANCELLED;
    else if (!rc && b.more)
        rc = 1;
    if (!b.w)
        b.n = 0;
    /* one word per card (a card in both directories, an export stopped between its moves, was read twice) and, once
     * both directories were read whole, none of a card that is gone; the cache then has what was read */
    word_sort(b.w, b.n, 1);
    n = 0;
    for (int i = 0; i < b.n; i++) {
        if (n && b.w[n - 1].id == b.w[i].id) {
            b.w[n - 1].flags |= b.w[i].flags;
            dropped++;
            continue;
        }
        if (!rc && !(b.w[i].flags & WORD_SEEN)) {
            dropped++;
            continue;
        }
        b.w[n++] = b.w[i];
    }
    if ((b.read || dropped) && rc != VJO_E_OOM && rc != VJO_E_LOCAL_IO)
        words_save(b.w, n); /* (a stop keeps every word it had and every word it read) */
    if (rc) {
        vjo_arena_release(a, base);
        return rc;
    }
    /* the words in (key, time, ID) order: the first of a key's run is its earliest card */
    word_sort(b.w, n, 0);
    for (int i = 0; i < n; i++) {
        b.w[i].flags &= VJO_WORD_QUEUED;
        out->queued += !!(b.w[i].flags & VJO_WORD_QUEUED);
        out->duplicates += i && b.w[i].key == b.w[i - 1].key && (b.w[i].flags & VJO_WORD_QUEUED);
    }
    out->w = b.w;
    out->n = n;
    out->read = b.read;
    out->unreadable = b.unreadable;
    /* (no "unreadable": a record that is has its own line, read_card's) */
    vjo_log("anki export: words of %d cards (%d records read, %d without a word); %d queued, %d already mined", n,
            b.read, b.unreadable, out->queued, out->duplicates);
    return 0;
}

int vjo_words_earlier(const VjoWords *w, uint64_t key, int64_t time, const uint8_t id[16])
{
    int lo = 0, hi = w->n;
    uint64_t me = be_get(id, 8);
    uint32_t t = word_time(time);
    while (lo < hi) { /* the first word of this key */
        int mid = lo + (hi - lo) / 2;
        if (w->w[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= w->n || w->w[lo].key != key)
        return 0;
    return w->w[lo].time != t ? w->w[lo].time < t : w->w[lo].id < me;
}

/* ---------------- known words (#314 stage C) ---------------- */

/* anki_known.bin: a 16-byte head (KNOWN_MAGIC, the keys' count and their CRC-32, big-endian), then the keys, sorted
 * and distinct, 8 bytes each, big-endian. The app's alone: made by vjo_known_import from known_words.txt, read by the
 * export. */
#define KNOWN_MAGIC "RubiKn1\n"
#define KNOWN_TEMP VJO_KNOWN_INDEX ".tmp"
#define KNOWN_BACKUP VJO_KNOWN_INDEX ".bak" /* the old index while the new one is renamed in (never over a file) */
#define KNOWN_CHUNK (16 * 1024) /* known_words.txt is read this much at a time, from the arena */

/* The bytes of an ASCII space or U+3000 (an ideographic space) at s (n bytes left), or 0. */
static size_t space_at(const char *s, size_t n)
{
    if (n && (s[0] == ' ' || s[0] == '\t'))
        return 1;
    return n >= 3 && (uint8_t)s[0] == 0xE3 && (uint8_t)s[1] == 0x80 && (uint8_t)s[2] == 0x80 ? 3 : 0;
}

/* [*b, *e) without the spaces (space_at) at either end. */
static void trim(const char *s, size_t *b, size_t *e)
{
    size_t k;
    while (*b < *e && (k = space_at(s + *b, *e - *b)))
        *b += k;
    for (;;) {
        if (*e > *b && (s[*e - 1] == ' ' || s[*e - 1] == '\t'))
            (*e)--;
        else if (*e >= *b + 3 && (uint8_t)s[*e - 3] == 0xE3 && (uint8_t)s[*e - 2] == 0x80 && (uint8_t)s[*e - 1] == 0x80)
            *e -= 3;
        else
            return;
    }
}

int vjo_known_line(const char *line, size_t n, uint64_t *key)
{
    char word[VJO_KNOWN_LINE_MAX + 1], reading[VJO_KNOWN_LINE_MAX + 1];
    size_t b = 0, e, rb, re, k;
    if (n && line[n - 1] == '\r')
        n--;
    if (n >= 3 && !memcmp(line, "\xEF\xBB\xBF", 3)) /* the file's BOM (Notepad) */
        b = 3;
    /* blank, or a comment: its first character after spaces is '#' */
    for (size_t i = b;;) {
        if (i >= n)
            return 0;
        if ((k = space_at(line + i, n - i))) {
            i += k;
            continue;
        }
        if (line[i] == '#')
            return 0;
        break;
    }
    for (size_t i = b; i < n; i++)
        if (((uint8_t)line[i] < 0x20 && line[i] != '\t') || line[i] == 0x7F)
            return -1; /* a control character, a NUL */
    /* the word, up to the first TAB; then the reading, up to the next */
    for (e = b; e < n && line[e] != '\t'; e++)
        ;
    rb = re = e < n ? e + 1 : n;
    while (re < n && line[re] != '\t')
        re++;
    trim(line, &b, &e);
    trim(line, &rb, &re);
    if (b == e || e - b > VJO_KNOWN_LINE_MAX || re - rb > VJO_KNOWN_LINE_MAX)
        return -1;
    memcpy(word, line + b, e - b);
    word[e - b] = '\0';
    memcpy(reading, line + rb, re - rb);
    reading[re - rb] = '\0';
    *key = rb == re ? vjo_word_spelling_key(word) : vjo_word_key(word, reading);
    return 1;
}

static void key_down(uint64_t *k, int n, int i)
{
    for (;;) {
        int c = 2 * i + 1;
        uint64_t t;
        if (c >= n)
            return;
        if (c + 1 < n && k[c] < k[c + 1])
            c++;
        if (k[i] >= k[c])
            return;
        t = k[i];
        k[i] = k[c];
        k[c] = t;
        i = c;
    }
}

/* Sorts n keys and drops the repeats: the distinct ones' count. */
static int key_set(uint64_t *k, int n)
{
    int m = 0;
    for (int i = n / 2 - 1; i >= 0; i--)
        key_down(k, n, i);
    for (int end = n - 1; end > 0; end--) {
        uint64_t t = k[0];
        k[0] = k[end];
        k[end] = t;
        key_down(k, end, 0);
    }
    for (int i = 0; i < n; i++)
        if (!m || k[m - 1] != k[i])
            k[m++] = k[i];
    return m;
}

/* The index of n sorted keys, under its temporary name, then in place of the old one, which is kept as the backup
 * until then and put back when the rename fails: 0, or -1 (logged; the old index stays). */
static int known_save(const uint64_t *k, int n)
{
    uint8_t head[16], buf[2048];
    uint32_t crc = 0;
    int rc, c;
    SceUID fd;
    for (int i = 0; i < n; i += (int)(sizeof(buf) / 8)) {
        int m = n - i < (int)(sizeof(buf) / 8) ? n - i : (int)(sizeof(buf) / 8);
        for (int j = 0; j < m; j++)
            be_put(buf + 8 * j, k[i + j], 8);
        crc = vjo_crc32(crc, buf, (size_t)m * 8);
    }
    memcpy(head, KNOWN_MAGIC, 8);
    be_put(head + 8, (uint32_t)n, 4);
    be_put(head + 12, crc, 4);
    if ((fd = sceIoOpen(KNOWN_TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0)
        return io_fail("open", KNOWN_TEMP, fd);
    rc = write_all(fd, head, sizeof(head));
    for (int i = 0; !rc && i < n; i += (int)(sizeof(buf) / 8)) {
        int m = n - i < (int)(sizeof(buf) / 8) ? n - i : (int)(sizeof(buf) / 8);
        for (int j = 0; j < m; j++)
            be_put(buf + 8 * j, k[i + j], 8);
        rc = write_all(fd, buf, (size_t)m * 8);
    }
    if ((c = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", KNOWN_TEMP, c);
    if (!rc && (c = sceIoSync("ux0:", 0)) < 0)
        rc = io_fail("sync", "ux0:", c);
    if (rc) {
        sceIoRemove(KNOWN_TEMP);
        return rc;
    }
    /* the old index as the backup (an older backup goes first: the index is the newer), then the new one in place */
    if (present(VJO_KNOWN_INDEX) == 1) {
        if (present(KNOWN_BACKUP) == 1 && (c = sceIoRemove(KNOWN_BACKUP)) < 0)
            rc = io_fail("remove", KNOWN_BACKUP, c);
        else if ((c = sceIoRename(VJO_KNOWN_INDEX, KNOWN_BACKUP)) < 0)
            rc = io_fail("rename", VJO_KNOWN_INDEX, c);
    }
    if (!rc && (c = sceIoRename(KNOWN_TEMP, VJO_KNOWN_INDEX)) < 0) {
        rc = io_fail("rename", KNOWN_TEMP, c);
        if (present(KNOWN_BACKUP) == 1 && (c = sceIoRename(KNOWN_BACKUP, VJO_KNOWN_INDEX)) < 0) /* the old one back */
            io_fail("rename", KNOWN_BACKUP, c);
    }
    if (rc)
        sceIoRemove(KNOWN_TEMP);
    else if (present(KNOWN_BACKUP) == 1 && (c = sceIoRemove(KNOWN_BACKUP)) < 0)
        io_fail("remove", KNOWN_BACKUP, c); /* (harmless: the index is read first) */
    if ((c = sceIoSync("ux0:", 0)) < 0)
        io_fail("sync", "ux0:", c);
    return rc;
}

/* The index for reading: anki_known.bin, else its backup (an import stopped between its two renames), else < 0. */
static SceUID known_open(void)
{
    SceUID fd = sceIoOpen(VJO_KNOWN_INDEX, SCE_O_RDONLY, 0);
    return fd == MISSING ? sceIoOpen(KNOWN_BACKUP, SCE_O_RDONLY, 0) : fd;
}

typedef struct {
    uint64_t *keys;
    int n, line_no;
    VjoKnownResult *r;
} KnownRead;

/* A whole line of known_words.txt (too_long: more than the line buffer held). */
static void known_add(KnownRead *k, const char *line, size_t n, int too_long)
{
    uint64_t key;
    int rc = too_long ? -1 : vjo_known_line(line, n, &key);
    k->line_no++;
    if (rc < 0) {
        if (k->r->skipped++ < 3)
            vjo_log("anki known: known_words.txt line %d is not a word (skipped)", k->line_no);
    } else if (rc > 0) {
        if (k->n < VJO_KNOWN_MAX) {
            k->keys[k->n++] = key;
            k->r->words++;
        } else {
            k->r->over++;
        }
    }
}

int vjo_known_import(VjoArena *a, const VjoExportUi *ui, VjoKnownResult *r, char *msg, size_t cap)
{
    size_t base = vjo_arena_mark(a), len = 0;
    char *chunk, *line;
    KnownRead k;
    SceUID fd;
    int got, too_long = 0, tabs = 0, rc = 0;
    memset(r, 0, sizeof(*r));
    msg[0] = '\0';
    if ((fd = sceIoOpen(VJO_KNOWN_PATH, SCE_O_RDONLY, 0)) < 0) {
        if (fd != MISSING) {
            io_fail("open", VJO_KNOWN_PATH, fd);
            snprintf(msg, cap, "Cannot read known_words.txt (see log.txt). The known words are as they were.");
            return r->rc = VJO_E_LOCAL_IO;
        }
        r->missing = 1;
        for (int i = 0; i < 2; i++) {
            const char *path = i ? KNOWN_BACKUP : VJO_KNOWN_INDEX;
            if (present(path) == 1 && (got = sceIoRemove(path)) < 0) {
                io_fail("remove", path, got);
                snprintf(msg, cap, "No known_words.txt, but the known words could not be cleared (see log.txt).");
                return r->rc = VJO_E_LOCAL_IO;
            }
        }
        snprintf(msg, cap, "No known_words.txt in %s/: no known words. tools/known_words.py makes it from Anki.",
                 VJO_EXPORT_DATA_DIR);
        return r->rc = VJO_OK;
    }
    memset(&k, 0, sizeof(k));
    k.r = r;
    k.keys = (uint64_t *)vjo_arena_alloc(a, sizeof(uint64_t) * VJO_KNOWN_MAX);
    chunk = (char *)vjo_arena_alloc(a, KNOWN_CHUNK);
    line = (char *)vjo_arena_alloc(a, VJO_KNOWN_LINE_MAX * 2 + 8); /* a line's bytes: a word, a TAB, a reading */
    if (!k.keys || !chunk || !line) {
        sceIoClose(fd);
        vjo_arena_release(a, base);
        snprintf(msg, cap, "Not enough memory to read known_words.txt. The known words are as they were.");
        return r->rc = VJO_E_OOM;
    }
    for (;;) {
        if (ui && ui->stopped && ui->stopped(ui->ud)) {
            rc = VJO_E_CANCELLED;
            break;
        }
        if ((got = sceIoRead(fd, chunk, KNOWN_CHUNK)) < 0) {
            rc = io_error("read", VJO_KNOWN_PATH, got);
            break;
        }
        if (!got)
            break;
        for (int i = 0; i < got; i++) {
            if (chunk[i] == '\n') {
                known_add(&k, line, len, too_long);
                len = 0;
                too_long = tabs = 0;
            } else if (tabs == 2) {
                continue; /* past the second TAB: the columns vjo_known_line ignores, however long */
            } else if (chunk[i] == '\t' && ++tabs == 2) {
                continue;
            } else if (len < VJO_KNOWN_LINE_MAX * 2 + 8) {
                line[len++] = chunk[i];
            } else {
                too_long = 1;
            }
        }
        if (ui && ui->status) {
            char status[64];
            snprintf(status, sizeof(status), "Reading known_words.txt… %d words", r->words);
            ui->status(ui->ud, status);
        }
    }
    if (!rc && (len || too_long)) /* the last line, without a newline */
        known_add(&k, line, len, too_long);
    if ((got = sceIoClose(fd)) < 0 && !rc)
        rc = io_error("close", VJO_KNOWN_PATH, got);
    if (!rc) {
        r->keys = key_set(k.keys, k.n);
        if (known_save(k.keys, r->keys) < 0)
            rc = VJO_E_LOCAL_IO;
    }
    vjo_arena_release(a, base);
    if (rc == VJO_E_CANCELLED)
        snprintf(msg, cap, "Stopped. The known words are as they were.");
    else if (rc)
        snprintf(msg, cap, "Could not read known_words.txt or write its words (see log.txt). The known words are as "
                           "they were.");
    else {
        size_t m = (size_t)snprintf(msg, cap, "Imported %d known word%s from known_words.txt: their cards get the tag "
                                    "known.", r->words, r->words == 1 ? "" : "s");
        if (r->skipped && m < cap)
            m += (size_t)snprintf(msg + m, cap - m, " %d line%s not a word (see log.txt).", r->skipped,
                                  r->skipped == 1 ? " is" : "s are");
        if (r->over && m < cap)
            snprintf(msg + m, cap - m, " %d more left out: %d at most.", r->over, VJO_KNOWN_MAX);
    }
    vjo_log("anki known: import rc=%d: %d words, %d keys, %d skipped, %d over", rc, r->words, r->keys, r->skipped,
            r->over);
    return r->rc = rc;
}

/* The index's head: its count (0 for a file of none), or -1 (none, damaged: logged unless missing). */
static int known_head(SceUID fd, uint8_t head[16])
{
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    uint32_t n;
    if (size < 16 || sceIoLseek(fd, 0, SCE_SEEK_SET) != 0 || read_all(fd, head, 16) < 0 || memcmp(head, KNOWN_MAGIC, 8))
        return -1;
    n = (uint32_t)be_get(head + 8, 4);
    return n <= VJO_KNOWN_MAX && (uint64_t)size == 16 + (uint64_t)n * 8 ? (int)n : -1;
}

int vjo_known_count(void)
{
    uint8_t head[16], buf[2048];
    SceUID fd = known_open();
    uint64_t last = 0;
    uint32_t crc = 0;
    int n;
    if (fd < 0)
        return -1;
    /* the whole index, as the export reads it (audit-rubi-rm4 N9: the page shows no count the export would not use) */
    n = known_head(fd, head);
    for (int i = 0; n > 0 && i < n;) {
        int m = n - i < (int)(sizeof(buf) / 8) ? n - i : (int)(sizeof(buf) / 8);
        if (read_all(fd, buf, (size_t)m * 8) < 0) {
            n = -1;
            break;
        }
        crc = vjo_crc32(crc, buf, (size_t)m * 8);
        for (int j = 0; j < m && n > 0; j++, i++) {
            uint64_t key = be_get(buf + 8 * j, 8);
            if (i && key <= last)
                n = -1;
            last = key;
        }
    }
    if (n > 0 && crc != (uint32_t)be_get(head + 12, 4))
        n = -1;
    sceIoClose(fd);
    return n < 0 ? -2 : n;
}

/* The index's keys into a (*out; NULL and 0 without an index): their count, or -1 (damaged: re-import), -2 (no room). */
static int known_load(VjoArena *a, uint64_t **out)
{
    uint8_t head[16], buf[2048];
    size_t mark = vjo_arena_mark(a);
    SceUID fd = known_open();
    uint32_t crc = 0;
    int n, rc = 0;
    *out = NULL;
    if (fd < 0) {
        if (fd != MISSING)
            io_fail("open", VJO_KNOWN_INDEX, fd);
        return fd == MISSING ? 0 : -1;
    }
    if ((n = known_head(fd, head)) < 0)
        rc = -1;
    else if (n && !(*out = (uint64_t *)vjo_arena_alloc(a, sizeof(uint64_t) * (size_t)n)))
        rc = -2;
    for (int i = 0; !rc && i < n;) {
        int m = n - i < (int)(sizeof(buf) / 8) ? n - i : (int)(sizeof(buf) / 8);
        if (read_all(fd, buf, (size_t)m * 8) < 0) {
            rc = -1;
            break;
        }
        crc = vjo_crc32(crc, buf, (size_t)m * 8);
        for (int j = 0; j < m && !rc; j++, i++) {
            (*out)[i] = be_get(buf + 8 * j, 8);
            if (i && (*out)[i] <= (*out)[i - 1])
                rc = -1; /* not sorted and distinct */
        }
    }
    sceIoClose(fd);
    if (!rc && crc != (uint32_t)be_get(head + 12, 4))
        rc = -1;
    if (rc) {
        vjo_log("anki export: %s %s: no card is tagged known", VJO_KNOWN_INDEX,
                rc == -2 ? "does not fit in memory" : "is damaged (import known_words.txt again)");
        vjo_arena_release(a, mark);
        *out = NULL;
        return rc;
    }
    return n;
}

static int known_has(const uint64_t *k, int n, uint64_t key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (k[mid] == key)
            return 1;
        if (k[mid] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
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
    const VjoWords *words; /* #314: every card's word (NULL: Duplicates Keep, or not checked) */
    int skip;              /* Duplicates Skip: a duplicate is left out */
    int skipped_queued, skipped_archived; /* duplicates left out: in the queue (they stay), in the archive */
    const uint64_t *known; /* #314 stage C: the known words' keys (anki_known.bin), sorted */
    int n_known;
} List;

/* VjoApkgCard.flags: its note's tags (export_tags) */
#define CARD_DUPLICATE 1u
#define CARD_KNOWN 2u

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
    from_hex(hex, c.id);
    c.time = note.unix_time > 0 ? note.unix_time : VJO_APKG_NO_TIME;
    /* #314: a word an earlier card has: Anki's duplicate tag, or (Skip) not in this package; a known word's (stage
     * C, by its key or, for a known word without a reading, by its spelling alone): the tag known */
    if (l->n_known && (known_has(l->known, l->n_known, vjo_word_key(note.spelling, note.reading)) ||
                       known_has(l->known, l->n_known, vjo_word_spelling_key(note.spelling))))
        c.flags |= CARD_KNOWN;
    if (l->words && vjo_words_earlier(l->words, vjo_word_key(note.spelling, note.reading), c.time, c.id)) {
        if (l->skip) {
            char path[VJO_EXPORT_PATH_MAX];
            card_path(path, 0, hex, "json");
            /* (an archived card still queued, an export stopped between its moves, was counted in the queue's pass) */
            if (!l->archive)
                l->skipped_queued++;
            else if (present(path) != 1)
                l->skipped_archived++;
            vjo_arena_release(l->a, mark);
            return 0;
        }
        c.flags |= CARD_DUPLICATE;
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

/* A note's tags besides vita-jp-overlay (VjoApkgOps.tags): `duplicate` (#314: the tag an English Anki's Find
 * Duplicates adds) for a card whose word an earlier card has, and `known` for a word of known_words.txt. */
static const char *export_tags(void *ud, VjoArena *a, const VjoApkgCard *c, const VjoAnkiNote *note)
{
    static const char *const tags[4] = {NULL, "duplicate", "known", "duplicate known"};
    (void)ud;
    (void)a;
    (void)note;
    return tags[c->flags & (CARD_DUPLICATE | CARD_KNOWN)];
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

int vjo_export_run(VjoArena *a, int all, const VjoAppPrefs *prefs, const VjoExportUi *ui, VjoExportResult *r,
                   char *msg, size_t cap)
{
    List l;
    Out o;
    VjoApkgOps ops;
    VjoApkgResult pr;
    VjoApkgClock clock;
    VjoAppPrefs defaults;
    VjoWords words;
    SceDateTime t;
    SceRtcTick tick;
    char tmp[VJO_EXPORT_PATH_MAX + 4];
    int rc, queued, archived = 0, made = 0;
    size_t base = vjo_arena_mark(a), listed;
    if (!prefs) {
        vjo_app_prefs_defaults(&defaults);
        prefs = &defaults;
    }
    memset(r, 0, sizeof(*r));
    memset(&l, 0, sizeof(l));
    msg[0] = '\0';
    remove_stale_temps();
    /* 0. a Cards page change a stop left (#315, settings/cards.h): finished or undone first, so the queue holds
     * the old card or the new one, never both; one that cannot be settled now stops the export (nothing changed) */
    if (vjo_cards_recover(a)) {
        snprintf(msg, cap, "A change of a card that stopped could not be finished (see log.txt). Nothing was changed.");
        return r->rc = VJO_E_LOCAL_IO;
    }
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
    listed = vjo_arena_mark(a);
    /* #314: every card's word, above the list (released once the list is made: its cards keep their tags) */
    if (prefs->anki_duplicates != VJO_DUPLICATES_KEEP) {
        rc = vjo_export_words(a, ui, 0, &words);
        if (rc == VJO_E_CANCELLED) {
            snprintf(msg, cap, "Stopped. Nothing was changed.");
            vjo_arena_release(a, base);
            return r->rc = rc;
        }
        if (rc == VJO_E_OOM && prefs->anki_duplicates == VJO_DUPLICATES_SKIP) {
            snprintf(msg, cap, "Not enough memory to check every card for duplicates. Nothing was changed; with "
                               "Duplicates: Keep the cards export unchecked.");
            vjo_arena_release(a, base);
            return r->rc = rc;
        }
        if (rc && rc != VJO_E_OOM) {
            snprintf(msg, cap, "Cannot read the queue (see log.txt). Nothing was changed.");
            vjo_arena_release(a, base);
            return r->rc = rc;
        }
        if (rc) { /* Tag: the cards export without it, and the result says so */
            vjo_log("anki export: not enough memory for every card's word: no duplicate tags");
            vjo_arena_release(a, listed);
            r->unchecked = 1;
        } else {
            l.words = &words;
        }
    }
    l.skip = prefs->anki_duplicates == VJO_DUPLICATES_SKIP;
    /* #314 stage C: the known words, whatever Duplicates says (above the words, released with them) */
    {
        uint64_t *known;
        int n = known_load(a, &known);
        if (n < 0)
            r->known_unchecked = 1;
        else {
            l.known = known;
            l.n_known = n;
        }
    }
    rc = scan(VJO_EXPORT_QUEUE_DIR, list_card, &l);
    if (rc >= 0 && all) {
        l.archive = 1;
        rc = scan(VJO_EXPORT_ARCHIVE_DIR, list_card, &l);
    }
    vjo_arena_release(a, listed); /* the words and the known words */
    l.words = NULL;
    l.known = NULL;
    l.n_known = 0;
    if (ui->stopped && ui->stopped(ui->ud)) {
        snprintf(msg, cap, "Stopped. Nothing was changed.");
        vjo_arena_release(a, base);
        return r->rc = VJO_E_CANCELLED;
    }
    r->unreadable = l.unreadable;
    r->left = l.left;
    r->duplicates = l.skipped_queued + l.skipped_archived;
    if (rc < 0) {
        snprintf(msg, cap, "Cannot read the queue (see log.txt). Nothing was changed.");
        vjo_arena_release(a, base);
        return r->rc = VJO_E_LOCAL_IO;
    }
    vjo_apkg_sort(l.cards, l.n);
    l.n = dedupe(l.cards, l.n);
    if (!l.n && !l.unreadable && l.skipped_queued) { /* (Skip) every queued card's word was mined before */
        snprintf(msg, cap, "No new cards: the %d queued %s already mined, and stay%s queued (Duplicates: Skip).",
                 l.skipped_queued, l.skipped_queued == 1 ? "is a word" : "are words",
                 l.skipped_queued == 1 ? "s" : "");
        vjo_arena_release(a, base);
        return r->rc = VJO_OK;
    }
    if (!l.n) {
        size_t m = (size_t)snprintf(msg, cap, "%d card%s could not be read (see log.txt); they stay queued.",
                                    l.unreadable, l.unreadable == 1 ? "" : "s");
        if (l.skipped_queued && m < cap) /* (a word an unreadable card has, from the cache: docs/anki.md) */
            snprintf(msg + m, cap - m, " %d already mined stay%s queued (Duplicates: Skip).", l.skipped_queued,
                     l.skipped_queued == 1 ? "s" : "");
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
    ops.tags = export_tags;
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
                r->duplicates += !!(l.cards[i].flags & CARD_DUPLICATE); /* tagged */
                r->known += !!(l.cards[i].flags & CARD_KNOWN);
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
        char size[24], extra[400] = "";
        size_text(r->bytes, size, sizeof(size));
        /* #314 first: what Duplicates did */
        if (l.skipped_queued)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                     " %d already mined stay%s queued (Duplicates: Skip).", l.skipped_queued,
                     l.skipped_queued == 1 ? "s" : "");
        if (l.skipped_archived)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                     " %d already mined %s not in it (Duplicates: Skip).", l.skipped_archived,
                     l.skipped_archived == 1 ? "is" : "are");
        if (!l.skip && r->duplicates)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " %d tagged duplicate (mined before).",
                     r->duplicates);
        if (r->unchecked)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                     " Not checked for duplicates: too many cards for memory.");
        if (r->known)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " %d tagged known (known_words.txt).",
                     r->known);
        if (r->known_unchecked)
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                     " Not checked for known words (see log.txt).");
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
