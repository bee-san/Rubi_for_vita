#include "apkg.h"

#include <string.h>

#include "bearssl.h"
#include "errors.h"
#include "json.h"
#include "lapis.h"
#include "port.h"

#define PAGE VJO_SQLW_PAGE
#define COLLECTION "collection.anki2"
#define MEDIA_MAP "media"
#define TAGS " vita-jp-overlay "
#define DECK_CONF_ID 1
/* Lapis's FreqSort for a word without a frequency, as Yomitan writes
 * {frequency-harmonic-rank} for one (its default-anki-field-templates):
 * sorting by it puts such cards last. */
#define NO_FREQ_SORT "9999999"

/* Anki's schema 11 (rslib/src/storage/schema11.sql), without its indexes:
 * an import opens the file and upgrades it, which adds what it needs. */
static const char *const table_sql[5][2] = {
    {"col", "CREATE TABLE col (id integer PRIMARY KEY, crt integer NOT NULL, mod integer NOT NULL, scm integer NOT "
            "NULL, ver integer NOT NULL, dty integer NOT NULL, usn integer NOT NULL, ls integer NOT NULL, conf text "
            "NOT NULL, models text NOT NULL, decks text NOT NULL, dconf text NOT NULL, tags text NOT NULL)"},
    {"notes", "CREATE TABLE notes (id integer PRIMARY KEY, guid text NOT NULL, mid integer NOT NULL, mod integer NOT "
              "NULL, usn integer NOT NULL, tags text NOT NULL, flds text NOT NULL, sfld integer NOT NULL, csum "
              "integer NOT NULL, flags integer NOT NULL, data text NOT NULL)"},
    {"cards", "CREATE TABLE cards (id integer PRIMARY KEY, nid integer NOT NULL, did integer NOT NULL, ord integer "
              "NOT NULL, mod integer NOT NULL, usn integer NOT NULL, type integer NOT NULL, queue integer NOT NULL, "
              "due integer NOT NULL, ivl integer NOT NULL, factor integer NOT NULL, reps integer NOT NULL, lapses "
              "integer NOT NULL, left integer NOT NULL, odue integer NOT NULL, odid integer NOT NULL, flags integer "
              "NOT NULL, data text NOT NULL)"},
    {"revlog", "CREATE TABLE revlog (id integer PRIMARY KEY, cid integer NOT NULL, usn integer NOT NULL, ease integer "
               "NOT NULL, ivl integer NOT NULL, lastIvl integer NOT NULL, factor integer NOT NULL, time integer NOT "
               "NULL, type integer NOT NULL)"},
    {"graves", "CREATE TABLE graves (usn integer NOT NULL, oid integer NOT NULL, type integer NOT NULL)"},
};
enum { T_COL, T_NOTES, T_CARDS, T_REVLOG, T_GRAVES, TABLES };

/* ---------------- order, IDs, names ---------------- */

static int card_cmp(const VjoApkgCard *x, const VjoApkgCard *y)
{
    if (x->time != y->time)
        return x->time < y->time ? -1 : 1;
    return memcmp(x->id, y->id, sizeof(x->id));
}

static void sift(VjoApkgCard *c, int root, int n)
{
    for (;;) {
        int child = 2 * root + 1;
        VjoApkgCard t;
        if (child >= n)
            return;
        if (child + 1 < n && card_cmp(&c[child], &c[child + 1]) < 0)
            child++;
        if (card_cmp(&c[root], &c[child]) >= 0)
            return;
        t = c[root];
        c[root] = c[child];
        c[child] = t;
        root = child;
    }
}

void vjo_apkg_sort(VjoApkgCard *cards, int n)
{
    /* heapsort: in place, O(n log n) for up to VJO_APKG_MAX_CARDS */
    for (int i = n / 2 - 1; i >= 0; i--)
        sift(cards, i, n);
    for (int end = n - 1; end > 0; end--) {
        VjoApkgCard t = cards[0];
        cards[0] = cards[end];
        cards[end] = t;
        sift(cards, 0, end);
    }
}

int64_t vjo_apkg_note_id(const VjoApkgCard *cards, int i, int64_t previous)
{
    int64_t id = (cards[i].time > 0 ? cards[i].time : VJO_APKG_NO_TIME) * 1000;
    return id > previous ? id : previous + 1;
}

void vjo_apkg_guid(const uint8_t id[16], char out[VJO_APKG_GUID_MAX])
{
    static const char table[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                                "!#$%&()*+,-./:;<=>?@[]^_`{|}~";
    char rev[VJO_APKG_GUID_MAX];
    uint64_t v = 0;
    int n = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | id[i];
    do {
        rev[n++] = table[v % 91u];
        v /= 91u;
    } while (v);
    for (int i = 0; i < n; i++)
        out[i] = rev[n - 1 - i];
    out[n] = '\0';
}

static void hex(const uint8_t *p, int n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    out[2 * n] = '\0';
}

static void sha256(const void *p, size_t n, uint8_t out[32])
{
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, p, n);
    br_sha256_out(&c, out);
}

static void media_name_of(const uint8_t hash[16], char out[48])
{
    char h[33];
    hex(hash, 16, h);
    vjo_snprintf(out, 48, "vitajp_%s.jpg", h);
}

void vjo_apkg_media_name(const uint8_t *jpeg, size_t len, char out[48])
{
    uint8_t d[32];
    sha256(jpeg, len, d);
    media_name_of(d, out);
}

/* Anki's field checksum: the first 32 bits of the SHA-1 of the first
 * field without HTML (its text here: the Expression is escaped plain text). */
static uint32_t field_checksum(const char *text)
{
    br_sha1_context c;
    uint8_t d[20];
    br_sha1_init(&c);
    br_sha1_update(&c, text, strlen(text));
    br_sha1_out(&c, d);
    return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
}

/* ---------------- the col row ---------------- */

/* A signed 64-bit decimal, without printf: the IDs are above 2^32, and whether
 * the Vita's newlib formats %lld is not something this code should depend on. */
static void buf_i64(VjoBuf *b, int64_t v)
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

static void json_deck(VjoBuf *b, int64_t id, const char *name, int64_t mtime)
{
    vjo_buf_putc(b, '"');
    buf_i64(b, id);
    vjo_buf_puts(b, "\":{\"id\":");
    buf_i64(b, id);
    vjo_buf_puts(b, ",\"mod\":");
    buf_i64(b, mtime);
    vjo_buf_puts(b, ",\"name\":");
    vjo_json_write_string(b, name);
    vjo_buf_puts(b, ",\"usn\":-1,\"lrnToday\":[0,0],\"revToday\":[0,0],\"newToday\":[0,0],\"timeToday\":[0,0],"
                    "\"collapsed\":false,\"browserCollapsed\":false,\"desc\":\"\",\"dyn\":0,\"conf\":1,"
                    "\"extendNew\":0,\"extendRev\":0}");
}

/* Anki's schema-11 defaults (rslib config/schema11.rs; the deck options are
 * genanki's, which every current Anki imports). */
#define DCONF                                                                                                   \
    "{\"1\":{\"id\":1,\"mod\":0,\"name\":\"Default\",\"usn\":0,\"maxTaken\":60,\"autoplay\":true,\"timer\":0,"   \
    "\"replayq\":true,\"dyn\":false,\"new\":{\"bury\":false,\"delays\":[1,10],\"initialFactor\":2500,"           \
    "\"ints\":[1,4,0],\"order\":1,\"perDay\":20,\"separate\":true},\"rev\":{\"bury\":false,\"ease4\":1.3,"      \
    "\"fuzz\":0.05,\"ivlFct\":1,\"maxIvl\":36500,\"minSpace\":1,\"perDay\":200},\"lapse\":{\"delays\":[10],"     \
    "\"leechAction\":1,\"leechFails\":8,\"minInt\":1,\"mult\":0}}}"

static int write_col(VjoArena *a, VjoSqlw *w, VjoSqlwTable *t, int n, const VjoApkgClock *clock)
{
    int64_t now = clock->now > 0 ? clock->now : VJO_APKG_NO_TIME;
    size_t mark = vjo_arena_mark(a), len;
    VjoBuf conf, models, decks;
    uint8_t *rec;
    VjoSqlwVal v[13];
    int rc;
    /* (vjo_buf_printf formats at most 255 bytes: the constant parts go in whole) */
    vjo_buf_init(&conf, a);
    vjo_buf_puts(&conf, "{\"activeDecks\":[1],\"curDeck\":1,\"newSpread\":0,\"collapseTime\":1200,\"timeLim\":0,"
                        "\"estTimes\":true,\"dueCounts\":true,\"sortType\":\"noteFld\",\"sortBackwards\":false,"
                        "\"addToCur\":true,\"dayLearnFirst\":false,\"schedVer\":2,\"sched2021\":true,");
    vjo_buf_puts(&conf, "\"curModel\":\"");
    buf_i64(&conf, vjo_lapis_notetype_id);
    vjo_buf_printf(&conf, "\",\"nextPos\":%d}", n + 1);
    vjo_buf_init(&models, a);
    vjo_buf_puts(&models, "{\"");
    buf_i64(&models, vjo_lapis_notetype_id);
    vjo_buf_puts(&models, "\":");
    vjo_buf_puts(&models, vjo_lapis_notetype);
    vjo_buf_putc(&models, '}');
    vjo_buf_init(&decks, a);
    vjo_buf_putc(&decks, '{');
    json_deck(&decks, 1, "Default", 0);
    vjo_buf_putc(&decks, ',');
    json_deck(&decks, vjo_lapis_deck_id, vjo_lapis_deck_name, now);
    vjo_buf_putc(&decks, '}');
    if (!vjo_buf_cstr(&conf) || !vjo_buf_cstr(&models) || !vjo_buf_cstr(&decks))
        return VJO_E_OOM;
    {
        VjoSqlwVal row[13] = {
            {VJO_SQLW_NULL, 0, NULL, 0},
            {VJO_SQLW_INT, now - now % 86400, NULL, 0}, /* crt: the day it was made */
            {VJO_SQLW_INT, now * 1000, NULL, 0},
            {VJO_SQLW_INT, now * 1000, NULL, 0},
            {VJO_SQLW_INT, 11, NULL, 0},
            {VJO_SQLW_INT, 0, NULL, 0},
            {VJO_SQLW_INT, 0, NULL, 0},
            {VJO_SQLW_INT, 0, NULL, 0},
            {VJO_SQLW_TEXT, 0, (const char *)conf.data, conf.len},
            {VJO_SQLW_TEXT, 0, (const char *)models.data, models.len},
            {VJO_SQLW_TEXT, 0, (const char *)decks.data, decks.len},
            {VJO_SQLW_TEXT, 0, DCONF, sizeof(DCONF) - 1},
            {VJO_SQLW_TEXT, 0, "{}", 2},
        };
        memcpy(v, row, sizeof(v));
    }
    if ((rc = vjo_sqlw_record(a, v, 13, &rec, &len)) < 0)
        return rc;
    rc = vjo_sqlw_table_begin(w, t, table_sql[T_COL][0], table_sql[T_COL][1]);
    if (!rc)
        rc = vjo_sqlw_row(w, 1, rec, len);
    if (!rc)
        rc = vjo_sqlw_table_end(w);
    vjo_arena_release(a, mark);
    return rc;
}

/* ---------------- the screenshots ---------------- */

/* The distinct screenshots by hash: an open-addressing set of indices into
 * the hashes, in the order first seen (their media numbers). */
typedef struct {
    uint8_t (*hash)[16];
    uint32_t *slot; /* index + 1, 0 = empty */
    uint32_t cap, n;
} MediaSet;

static int media_init(VjoArena *a, MediaSet *m, int pictures)
{
    m->cap = 16;
    while (m->cap < 2u * (uint32_t)pictures)
        m->cap <<= 1;
    m->n = 0;
    m->hash = (uint8_t(*)[16])vjo_arena_alloc(a, 16u * (size_t)(pictures ? pictures : 1));
    m->slot = (uint32_t *)vjo_arena_zalloc(a, sizeof(uint32_t) * m->cap);
    return m->hash && m->slot ? 0 : VJO_E_OOM;
}

/* The media number of hash h (added when new): its index. */
static uint32_t media_add(MediaSet *m, const uint8_t h[16], int *added)
{
    uint32_t i = ((uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24) & (m->cap - 1);
    *added = 0;
    for (;; i = (i + 1) & (m->cap - 1)) {
        if (!m->slot[i]) {
            memcpy(m->hash[m->n], h, 16);
            m->slot[i] = ++m->n;
            *added = 1;
            return m->n - 1;
        }
        if (!memcmp(m->hash[m->slot[i] - 1], h, 16))
            return m->slot[i] - 1;
    }
}

/* ---------------- the package ---------------- */

/* The collection's pages, into the package at base, and the CRC-32 of what
 * was written: the SQLite writer writes every page once, page 1 last and
 * the others in order, so the entry's CRC is page 1's combined with theirs. */
typedef struct {
    const VjoApkgOps *ops;
    uint32_t base;            /* the collection's data offset in the package */
    uint32_t first_crc, rest_crc;
    uint64_t next;            /* where the next page after page 1 must start */
    int have_first, out_of_order;
} DbOut;

static int db_write(void *ctx, uint64_t off, const void *src, size_t n)
{
    DbOut *d = (DbOut *)ctx;
    int rc = d->ops->write(d->ops->ud, d->base + off, src, n);
    if (rc < 0)
        return rc;
    if (off == 0 && n == PAGE && !d->have_first) {
        d->first_crc = vjo_crc32(0, src, n);
        d->have_first = 1;
    } else if (off == d->next && off >= PAGE) {
        d->rest_crc = vjo_crc32(d->rest_crc, src, n);
        d->next += n;
    } else {
        d->out_of_order = 1; /* not the writer's order: no CRC (the package fails) */
    }
    return 0;
}

static int pkg_write(void *ctx, uint64_t off, const void *src, size_t n)
{
    const VjoApkgOps *o = (const VjoApkgOps *)ctx;
    return o->write(o->ud, off, src, n);
}

static int pkg_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    const VjoApkgOps *o = (const VjoApkgOps *)ctx;
    return o->read(o->ud, off, dst, n);
}

static int stopped(const VjoApkgOps *ops)
{
    return ops->stopped && ops->stopped(ops->ud);
}

static void progress(const VjoApkgOps *ops, int done, int total, const char *step)
{
    if (ops->progress)
        ops->progress(ops->ud, done, total, step);
}

/* One card's note row. 1: the card is left out (it cannot be read again, or
 * its note does not fit in the arena); 0, or < 0 for the package's error. */
static int write_note(VjoArena *a, const VjoApkgOps *ops, VjoSqlw *w, const VjoApkgCard *c, int64_t note_id,
                      MediaSet *media, int *pictures)
{
    VjoAnkiNote note;
    const uint8_t *jpeg = NULL;
    size_t jpeg_len = 0, flds_len, len;
    char guid[VJO_APKG_GUID_MAX], name[48], *flds;
    const char *picture = NULL;
    uint8_t *rec, d[32];
    int added = 0;
    if (ops->load(ops->ud, a, c, &note, &jpeg, &jpeg_len) < 0 || !jpeg_len != !c->picture)
        return 1;
    if (jpeg_len) {
        sha256(jpeg, jpeg_len, d);
        media_name_of(d, name);
        picture = name;
    }
    vjo_apkg_guid(c->id, guid);
    if (!(flds = vjo_lapis_fields(a, &note, picture, &flds_len)))
        return 1;
    {
        VjoSqlwVal v[11] = {
            {VJO_SQLW_NULL, 0, NULL, 0},
            {VJO_SQLW_TEXT, 0, guid, strlen(guid)},
            {VJO_SQLW_INT, vjo_lapis_notetype_id, NULL, 0},
            {VJO_SQLW_INT, c->time > 0 ? c->time : VJO_APKG_NO_TIME, NULL, 0},
            {VJO_SQLW_INT, -1, NULL, 0},
            {VJO_SQLW_TEXT, 0, TAGS, sizeof(TAGS) - 1},
            {VJO_SQLW_TEXT, 0, flds, flds_len},
            {VJO_SQLW_TEXT, 0, note.spelling, strlen(note.spelling)},
            {VJO_SQLW_INT, (int64_t)field_checksum(note.spelling), NULL, 0},
            {VJO_SQLW_INT, 0, NULL, 0},
            {VJO_SQLW_TEXT, 0, "", 0},
        };
        if (vjo_sqlw_record(a, v, 11, &rec, &len) < 0)
            return 1;
    }
    if (vjo_sqlw_row(w, note_id, rec, len) < 0)
        return w->err;
    if (jpeg_len) { /* counted once its note is in */
        media_add(media, d, &added);
        (*pictures)++;
    }
    return 0;
}

static int write_card(VjoArena *a, VjoSqlw *w, const VjoApkgCard *c, int64_t note_id, int due)
{
    int64_t mod = c->time > 0 ? c->time : VJO_APKG_NO_TIME;
    VjoSqlwVal v[18] = {
        {VJO_SQLW_NULL, 0, NULL, 0},
        {VJO_SQLW_INT, note_id, NULL, 0},
        {VJO_SQLW_INT, vjo_lapis_deck_id, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0}, /* the template: Lapis has one, Mining */
        {VJO_SQLW_INT, mod, NULL, 0},
        {VJO_SQLW_INT, -1, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0}, /* new */
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, due, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_INT, 0, NULL, 0},
        {VJO_SQLW_TEXT, 0, "", 0},
    };
    size_t mark = vjo_arena_mark(a), len;
    uint8_t *rec;
    int rc = vjo_sqlw_record(a, v, 18, &rec, &len);
    if (!rc)
        rc = vjo_sqlw_row(w, note_id, rec, len); /* a card's ID: its note's (one card per note) */
    vjo_arena_release(a, mark);
    return rc;
}

int vjo_apkg_write(VjoArena *a, const VjoApkgOps *ops, VjoApkgCard *cards, int n, const VjoApkgClock *clock,
                   VjoApkgResult *r)
{
    VjoSqlw *w;
    VjoSqlwTable *t;
    VjoZipw *z;
    VjoZipEntry *entries;
    MediaSet media;
    DbOut db;
    VjoSqlwOut sq;
    VjoZipIo io = {(void *)ops, pkg_write, pkg_read};
    uint32_t data, pages = 0;
    int64_t id = 0;
    int rc = 0, pictures = 0, expected = 0, written_notes = 0;
    size_t base = vjo_arena_mark(a);
    memset(r, 0, sizeof(*r));
    if (n < 1 || n > VJO_APKG_MAX_CARDS)
        return VJO_E_PARSE;
    for (int i = 0; i < n; i++) {
        cards[i].skipped = 0;
        expected += cards[i].picture;
    }
    w = (VjoSqlw *)vjo_arena_alloc(a, sizeof(*w));
    t = (VjoSqlwTable *)vjo_arena_alloc(a, sizeof(*t));
    z = (VjoZipw *)vjo_arena_alloc(a, sizeof(*z));
    entries = (VjoZipEntry *)vjo_arena_alloc(a, sizeof(*entries) * (size_t)(expected + 2));
    if (!w || !t || !z || !entries || media_init(a, &media, expected) < 0) {
        vjo_arena_release(a, base);
        return VJO_E_OOM;
    }
    vjo_zip_dos_time(clock->year, clock->month, clock->day, clock->hour, clock->minute, clock->second,
                     &r->dos_time, &r->dos_date);
    vjo_zipw_init(z, &io, entries, expected + 2, r->dos_time, r->dos_date);
    if ((rc = vjo_zipw_begin(z, COLLECTION, &data)) < 0)
        goto done;
    memset(&db, 0, sizeof(db));
    db.ops = ops;
    db.base = data;
    db.next = PAGE;
    sq.ctx = &db;
    sq.write = db_write;
    vjo_sqlw_init(w, &sq);
    progress(ops, 0, n, "notes");
    if ((rc = write_col(a, w, t, n, clock)) < 0)
        goto done;
    rc = vjo_sqlw_table_begin(w, t, table_sql[T_NOTES][0], table_sql[T_NOTES][1]);
    for (int i = 0; !rc && i < n; i++) {
        size_t mark = vjo_arena_mark(a);
        int64_t next;
        if (stopped(ops)) {
            rc = VJO_E_CANCELLED;
            break;
        }
        next = vjo_apkg_note_id(cards, i, id);
        rc = write_note(a, ops, w, &cards[i], next, &media, &pictures);
        if (rc == 1) { /* left out: the next card takes this ID */
            cards[i].skipped = 1;
            rc = 0;
        } else if (!rc) {
            id = next;
            written_notes++;
        }
        vjo_arena_release(a, mark);
        progress(ops, i + 1, n, "notes");
    }
    if (!rc && !written_notes)
        rc = VJO_E_OOM;
    if (!rc)
        rc = vjo_sqlw_table_end(w);
    if (!rc)
        rc = vjo_sqlw_table_begin(w, t, table_sql[T_CARDS][0], table_sql[T_CARDS][1]);
    id = 0;
    for (int i = 0, due = 1; !rc && i < n; i++) {
        if (cards[i].skipped)
            continue;
        id = vjo_apkg_note_id(cards, i, id);
        rc = write_card(a, w, &cards[i], id, due++);
    }
    if (!rc)
        rc = vjo_sqlw_table_end(w);
    for (int k = T_REVLOG; !rc && k < TABLES; k++)
        if (!(rc = vjo_sqlw_table_begin(w, t, table_sql[k][0], table_sql[k][1])))
            rc = vjo_sqlw_table_end(w);
    if (!rc)
        rc = vjo_sqlw_finish(w, &pages);
    if (!rc && (db.out_of_order || !db.have_first || db.next != (uint64_t)pages * PAGE))
        rc = VJO_E_PARSE;
    if (!rc)
        rc = vjo_zipw_end_external(z, pages * PAGE,
                                   vjo_crc32_combine(db.first_crc, db.rest_crc, (uint64_t)(pages - 1u) * PAGE));
    /* the screenshots, each distinct one once, numbered as first seen */
    {
        uint32_t written = 0;
        for (int i = 0; !rc && i < n; i++) {
            size_t mark = vjo_arena_mark(a), len = 0;
            const uint8_t *jpeg = NULL;
            uint8_t d[32];
            char key[12];
            if (!cards[i].picture || cards[i].skipped)
                continue;
            if (stopped(ops)) {
                rc = VJO_E_CANCELLED;
                break;
            }
            if ((rc = ops->load(ops->ud, a, &cards[i], NULL, &jpeg, &len)) < 0)
                break;
            sha256(jpeg, len, d);
            /* the next new hash is the next media number; any other was written before */
            if (written < media.n && !memcmp(media.hash[written], d, 16)) {
                vjo_snprintf(key, sizeof(key), "%u", (unsigned)written);
                rc = vjo_zipw_begin(z, key, NULL);
                if (!rc)
                    rc = vjo_zipw_append(z, jpeg, len);
                if (!rc)
                    rc = vjo_zipw_end(z);
                written++;
            } else {
                int seen = 0;
                for (uint32_t k = 0; k < written && !seen; k++)
                    seen = !memcmp(media.hash[k], d, 16);
                if (!seen)
                    rc = VJO_E_PARSE; /* the screenshot changed since its note was written */
            }
            vjo_arena_release(a, mark);
            progress(ops, n, n, "media");
        }
        if (!rc && written != media.n)
            rc = VJO_E_PARSE;
    }
    if (!rc) { /* the media map: {"0": "vitajp_….jpg", ...} */
        size_t mark = vjo_arena_mark(a);
        VjoBuf b;
        vjo_buf_init(&b, a);
        vjo_buf_putc(&b, '{');
        for (uint32_t k = 0; k < media.n; k++) {
            char name[48];
            media_name_of(media.hash[k], name);
            vjo_buf_printf(&b, "%s\"%u\":\"%s\"", k ? "," : "", (unsigned)k, name);
        }
        vjo_buf_putc(&b, '}');
        if (b.oom)
            rc = VJO_E_OOM;
        if (!rc)
            rc = vjo_zipw_begin(z, MEDIA_MAP, NULL);
        if (!rc)
            rc = vjo_zipw_append(z, b.data, b.len);
        if (!rc)
            rc = vjo_zipw_end(z);
        vjo_arena_release(a, mark);
    }
    if (!rc)
        rc = vjo_zipw_finish(z, &r->bytes);
    if (!rc) {
        r->cards = written_notes;
        r->pictures = pictures;
        r->media = (int)media.n;
        r->db_pages = pages;
    }
done:
    if (rc >= 0 && (w->err || z->err))
        rc = w->err ? w->err : z->err;
    vjo_arena_release(a, base);
    return rc;
}

/* ---------------- verification ---------------- */

typedef struct {
    const VjoApkgOps *ops;
    uint32_t base, pages;
    uint8_t *page; /* one page per level of the walk */
} Db;

static int db_page(const Db *db, uint32_t pgno, uint8_t *out)
{
    if (pgno < 1 || pgno > db->pages)
        return -1;
    return db->ops->read(db->ops->ud, db->base + (uint64_t)(pgno - 1u) * PAGE, out, PAGE) < 0 ? -1 : 0;
}

static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A row: its rowid and the payload on its leaf (local bytes of len). */
typedef int (*RowFn)(void *ud, int64_t rowid, const uint8_t *local, size_t local_len, size_t len);

/* Visits the rows of the table rooted at root in rowid order: 0, or -1 for
 * a page that is not what the writer writes. */
static int walk(const Db *db, uint32_t root, RowFn fn, void *ud)
{
    uint32_t stack_pg[VJO_SQLW_LEVELS + 2];
    unsigned stack_i[VJO_SQLW_LEVELS + 2];
    int depth = 0;
    stack_pg[0] = root;
    stack_i[0] = 0;
    if (db_page(db, root, db->page) < 0)
        return -1;
    for (;;) {
        uint8_t *p = db->page + (size_t)depth * PAGE;
        unsigned hdr = stack_pg[depth] == 1 ? 100u : 0u, ncells = get16(p + hdr + 3);
        if (p[hdr] == 0x0D) { /* a leaf: its rows */
            for (unsigned i = 0; i < ncells; i++) {
                unsigned at = get16(p + hdr + 8 + 2 * i);
                uint64_t len, rowid;
                int a, b;
                size_t local;
                if (at < hdr + 8 + 2 * ncells || at >= PAGE)
                    return -1;
                a = vjo_sqlw_get_varint(p + at, PAGE - at, &len);
                b = a ? vjo_sqlw_get_varint(p + at + a, PAGE - at - a, &rowid) : 0;
                local = vjo_sqlw_local_payload((size_t)len);
                if (!a || !b || at + a + b + local + (local < len ? 4u : 0u) > PAGE ||
                    fn(ud, (int64_t)rowid, p + at + a + b, local, (size_t)len) < 0)
                    return -1;
            }
        } else if (p[hdr] == 0x05) { /* interior: the next child */
            unsigned i = stack_i[depth]++;
            uint32_t child;
            if (i > ncells)
                goto up;
            child = i < ncells ? get32(p + get16(p + hdr + 12 + 2 * i)) : get32(p + hdr + 8);
            if (depth + 1 > VJO_SQLW_LEVELS || db_page(db, child, db->page + (size_t)(depth + 1) * PAGE) < 0)
                return -1;
            depth++;
            stack_pg[depth] = child;
            stack_i[depth] = 0;
            continue;
        } else {
            return -1;
        }
    up:
        if (depth == 0)
            return 0;
        depth--;
    }
}

/* One column of a record (its local bytes): text into *s, *n, or an integer
 * into *v; the column's serial type, or -1. */
static int column(const uint8_t *rec, size_t local, int col, const uint8_t **s, size_t *n, int64_t *v)
{
    uint64_t header, type = 0;
    size_t at, body;
    int k = vjo_sqlw_get_varint(rec, local, &header);
    if (!k || header > local)
        return -1;
    at = (size_t)k;
    body = (size_t)header;
    for (int i = 0; i <= col; i++) {
        size_t len;
        if (at >= header || !(k = vjo_sqlw_get_varint(rec + at, (size_t)header - at, &type)))
            return -1;
        at += (size_t)k;
        /* fileformat2.html, "Record Format": the body's length of each serial type */
        if (type >= 12)
            len = (size_t)(type - 12 - (type & 1)) / 2; /* a blob (even) or text (odd) */
        else if (type == 5)
            len = 6;
        else if (type == 6 || type == 7)
            len = 8;
        else if (type <= 4)
            len = (size_t)type;
        else
            len = 0; /* 8, 9: the integers 0 and 1; 10, 11 are reserved */
        if (i < col) {
            body += len;
            continue;
        }
        if (body + len > local)
            return -1;
        if (type >= 13 && (type & 1)) {
            *s = rec + body;
            *n = len;
        } else if (type >= 1 && type <= 6) {
            int64_t x = (int8_t)rec[body];
            for (size_t b = 1; b < len; b++)
                x = (int64_t)((uint64_t)x << 8 | rec[body + b]);
            *v = x;
        } else if (type == 8 || type == 9) {
            *v = (int64_t)type - 8;
        }
    }
    return (int)type;
}

typedef struct {
    const VjoApkgCard *cards;
    int n, rows, at;
    int64_t id;
} Expect;

/* The next card in the package (the skipped ones are not): its index, or -1. */
static int next_card(Expect *e)
{
    while (e->at < e->n && e->cards[e->at].skipped)
        e->at++;
    return e->at < e->n ? e->at++ : -1;
}

static int check_note(void *ud, int64_t rowid, const uint8_t *rec, size_t local, size_t len)
{
    Expect *e = (Expect *)ud;
    const uint8_t *s;
    size_t n;
    int64_t mid = 0;
    char guid[VJO_APKG_GUID_MAX];
    int i = next_card(e);
    (void)len;
    if (i < 0)
        return -1;
    e->id = vjo_apkg_note_id(e->cards, i, e->id);
    vjo_apkg_guid(e->cards[i].id, guid);
    if (rowid != e->id || column(rec, local, 1, &s, &n, &mid) < 13 || n != strlen(guid) || memcmp(s, guid, n) ||
        column(rec, local, 2, &s, &n, &mid) < 0 || mid != vjo_lapis_notetype_id)
        return -1;
    e->rows++;
    return 0;
}

static int check_card(void *ud, int64_t rowid, const uint8_t *rec, size_t local, size_t len)
{
    Expect *e = (Expect *)ud;
    const uint8_t *s;
    size_t n;
    int64_t nid = 0;
    int i = next_card(e);
    (void)len;
    if (i < 0)
        return -1;
    e->id = vjo_apkg_note_id(e->cards, i, e->id);
    if (rowid != e->id || column(rec, local, 1, &s, &n, &nid) < 0 || nid != e->id)
        return -1;
    e->rows++;
    return 0;
}

static int count_row(void *ud, int64_t rowid, const uint8_t *rec, size_t local, size_t len)
{
    (void)rowid, (void)rec, (void)local, (void)len;
    ((Expect *)ud)->rows++;
    return 0;
}

int vjo_apkg_verify(VjoArena *a, const VjoApkgOps *ops, uint64_t size, const VjoApkgCard *cards, int n,
                    const VjoApkgResult *r)
{
    VjoZipIo io = {(void *)ops, pkg_write, pkg_read};
    size_t base = vjo_arena_mark(a);
    int entries_n = r->media + 2, rc = VJO_E_PARSE, got;
    VjoZipEntry *e = (VjoZipEntry *)vjo_arena_alloc(a, sizeof(*e) * (size_t)entries_n);
    uint8_t *buf = (uint8_t *)vjo_arena_alloc(a, PAGE * (VJO_SQLW_LEVELS + 2));
    uint32_t root[TABLES] = {0};
    Db db;
    if (!e || !buf) {
        vjo_arena_release(a, base);
        return VJO_E_OOM;
    }
    if (size != r->bytes || (got = vjo_zip_read(&io, size, e, entries_n)) != entries_n ||
        strcmp(e[0].name, COLLECTION) || strcmp(e[entries_n - 1].name, MEDIA_MAP) ||
        e[0].size != r->db_pages * PAGE)
        goto done;
    for (int k = 0; k < entries_n; k++) { /* every byte, against its CRC-32 */
        char key[12];
        vjo_snprintf(key, sizeof(key), "%d", k - 1);
        if ((k > 0 && k < entries_n - 1 && strcmp(e[k].name, key)) || vjo_zip_check_crc(&io, &e[k], buf, PAGE) < 0)
            goto done;
        if (stopped(ops)) {
            rc = VJO_E_CANCELLED;
            goto done;
        }
    }
    db.ops = ops;
    db.base = vjo_zip_data_offset(&io, &e[0]);
    db.pages = r->db_pages;
    db.page = buf;
    /* the header, then sqlite_schema on page 1: the five tables and their roots */
    if (!db.base || db_page(&db, 1, buf) < 0 || memcmp(buf, "SQLite format 3", 16) || get16(buf + 16) != PAGE ||
        get32(buf + 28) != r->db_pages || get32(buf + 44) != 4 || get32(buf + 56) != 1 || buf[100] != 0x0D ||
        get16(buf + 103) != TABLES)
        goto done;
    for (int k = 0; k < TABLES; k++) {
        unsigned at = get16(buf + 108 + 2 * k);
        uint64_t len, rowid;
        int x = at < PAGE ? vjo_sqlw_get_varint(buf + at, PAGE - at, &len) : 0;
        int y = x ? vjo_sqlw_get_varint(buf + at + x, PAGE - at - x, &rowid) : 0;
        const uint8_t *s;
        size_t sn;
        int64_t v = 0;
        if (!x || !y || len > PAGE - at - x - y ||
            column(buf + at + x + y, (size_t)len, 1, &s, &sn, &v) < 13 || sn != strlen(table_sql[k][0]) ||
            memcmp(s, table_sql[k][0], sn) || column(buf + at + x + y, (size_t)len, 3, &s, &sn, &v) < 0 || v < 2 ||
            v > (int64_t)r->db_pages)
            goto done;
        root[k] = (uint32_t)v;
    }
    {
        Expect ex = {cards, n, 0, 0, 0};
        if (walk(&db, root[T_COL], count_row, &ex) < 0 || ex.rows != 1)
            goto done;
        ex.rows = ex.at = 0;
        if (walk(&db, root[T_NOTES], check_note, &ex) < 0 || ex.rows != r->cards || next_card(&ex) >= 0)
            goto done;
        ex.rows = ex.at = 0;
        ex.id = 0;
        if (walk(&db, root[T_CARDS], check_card, &ex) < 0 || ex.rows != r->cards || next_card(&ex) >= 0)
            goto done;
        for (int k = T_REVLOG; k < TABLES; k++) {
            ex.rows = 0;
            if (walk(&db, root[k], count_row, &ex) < 0 || ex.rows)
                goto done;
        }
    }
    { /* the media map names every screenshot entry */
        uint32_t data = vjo_zip_data_offset(&io, &e[entries_n - 1]);
        char *map = (char *)vjo_arena_alloc(a, e[entries_n - 1].size + 1);
        VjoJson j;
        if (!data || !map || ops->read(ops->ud, data, map, e[entries_n - 1].size) < 0)
            goto done;
        map[e[entries_n - 1].size] = '\0';
        if (vjo_json_parse(a, map, e[entries_n - 1].size, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT) ||
            j.t[0].size != r->media)
            goto done;
        for (int k = 0; k < r->media; k++) {
            char key[12];
            int t;
            const char *name;
            vjo_snprintf(key, sizeof(key), "%d", k);
            t = vjo_json_obj_get(&j, 0, key);
            name = t >= 0 ? vjo_json_str(a, &j, t) : NULL;
            if (!name || strncmp(name, "vitajp_", 7) || e[k + 1].size == 0)
                goto done;
        }
    }
    rc = 0;
done:
    vjo_arena_release(a, base);
    return rc;
}
