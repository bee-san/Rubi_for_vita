#include "sqlw.h"

#include <string.h>

#include "conn.h"

#define U VJO_SQLW_PAGE
#define LEAF_HDR 8u
#define INTERIOR_HDR 12u
#define DB_HDR 100u
/* A table leaf's payload limits (fileformat2.html, "Cell Payload Overflow
 * Pages"; SQLite's btreeParseCellPtr): X, and M below. */
#define MAX_LOCAL (U - 35u)
#define MIN_LOCAL ((U - 12u) * 32u / 255u - 23u)
#define FANOUT VJO_SQLW_FANOUT
/* The version of SQLite the file says wrote it (header offset 96): any
 * reader takes it as information only. */
#define WRITER_VERSION 3046000u

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int vjo_sqlw_varint(uint8_t *p, uint64_t v)
{
    uint8_t buf[10];
    int n = 0;
    if (v & ((uint64_t)0xFF << 56)) { /* nine bytes: the last holds eight bits */
        p[8] = (uint8_t)v;
        v >>= 8;
        for (int i = 7; i >= 0; i--) {
            p[i] = (uint8_t)((v & 0x7F) | 0x80);
            v >>= 7;
        }
        return 9;
    }
    do {
        buf[n++] = (uint8_t)((v & 0x7F) | 0x80);
        v >>= 7;
    } while (v);
    buf[0] &= 0x7F;
    for (int i = 0; i < n; i++)
        p[i] = buf[n - 1 - i];
    return n;
}

static int varint_len(uint64_t v)
{
    uint8_t tmp[9];
    return vjo_sqlw_varint(tmp, v);
}

int vjo_sqlw_get_varint(const uint8_t *p, size_t n, uint64_t *v)
{
    uint64_t r = 0;
    for (int i = 0; i < 9; i++) {
        if ((size_t)i >= n)
            return 0;
        if (i == 8) {
            *v = (r << 8) | p[8];
            return 9;
        }
        r = (r << 7) | (p[i] & 0x7F);
        if (!(p[i] & 0x80)) {
            *v = r;
            return i + 1;
        }
    }
    return 0;
}

size_t vjo_sqlw_local_payload(size_t payload)
{
    size_t k;
    if (payload <= MAX_LOCAL)
        return payload;
    k = MIN_LOCAL + (payload - MIN_LOCAL) % (U - 4u);
    return k <= MAX_LOCAL ? k : MIN_LOCAL;
}

/* ---------------- records ---------------- */

/* The serial type of v and its body's length (fileformat2.html, "Record
 * Format"; the smallest integer width, as SQLite chooses it). */
static uint64_t serial_type(const VjoSqlwVal *v, size_t *len)
{
    if (v->kind == VJO_SQLW_INT) {
        uint64_t u = v->i < 0 ? ~(uint64_t)v->i : (uint64_t)v->i;
        *len = 0;
        if (v->i == 0 || v->i == 1)
            return 8u + (uint64_t)v->i; /* schema format 4: no body */
        if (u <= 127u)
            return *len = 1, 1;
        if (u <= 32767u)
            return *len = 2, 2;
        if (u <= 8388607u)
            return *len = 3, 3;
        if (u <= 2147483647u)
            return *len = 4, 4;
        if (u <= 0x7FFFFFFFFFFFull)
            return *len = 6, 5;
        return *len = 8, 6;
    }
    if (v->kind == VJO_SQLW_TEXT) {
        *len = v->n;
        return 13u + 2u * (uint64_t)v->n;
    }
    *len = 0;
    return 0;
}

/* The record's length, and its header's in *header. */
static size_t record_len(const VjoSqlwVal *v, int n, size_t *header)
{
    size_t types = 0, body = 0, h, len;
    for (int i = 0; i < n; i++) {
        types += (size_t)varint_len(serial_type(&v[i], &len));
        body += len;
    }
    h = types + 1;
    while ((size_t)varint_len(h) + types != h) /* the header's size counts itself */
        h = types + (size_t)varint_len(h);
    *header = h;
    return h + body;
}

static void record_put(uint8_t *out, const VjoSqlwVal *v, int n, size_t header)
{
    uint8_t *p = out + vjo_sqlw_varint(out, header), *body = out + header;
    size_t len;
    for (int i = 0; i < n; i++)
        p += vjo_sqlw_varint(p, serial_type(&v[i], &len));
    for (int i = 0; i < n; i++) {
        serial_type(&v[i], &len);
        if (v[i].kind == VJO_SQLW_INT) {
            for (size_t k = 0; k < len; k++)
                body[k] = (uint8_t)((uint64_t)v[i].i >> (8u * (len - 1u - k)));
        } else if (v[i].kind == VJO_SQLW_TEXT && len) {
            memcpy(body, v[i].s, len);
        }
        body += len;
    }
}

int vjo_sqlw_record(VjoArena *a, const VjoSqlwVal *v, int n, uint8_t **out, size_t *len)
{
    size_t header;
    *len = record_len(v, n, &header);
    if (!(*out = (uint8_t *)vjo_arena_alloc(a, *len ? *len : 1)))
        return VJO_E_OOM;
    record_put(*out, v, n, header);
    return 0;
}

/* ---------------- pages ---------------- */

static int fail(VjoSqlw *w, int rc)
{
    if (!w->err)
        w->err = rc;
    return w->err;
}

static int write_page(VjoSqlw *w, uint32_t pgno, const uint8_t *page)
{
    int rc;
    if (w->err)
        return w->err;
    rc = w->out.write(w->out.ctx, (uint64_t)(pgno - 1u) * U, page, U);
    return rc < 0 ? fail(w, rc) : 0;
}

/* n new page numbers (the first, or 0 past the limit). */
static uint32_t new_pages(VjoSqlw *w, uint32_t n)
{
    uint32_t first = w->next_page;
    if (w->err)
        return 0;
    if (n > VJO_SQLW_MAX_PAGES || first > VJO_SQLW_MAX_PAGES - n) {
        fail(w, VJO_E_TOO_LARGE);
        return 0;
    }
    w->next_page += n;
    return first;
}

void vjo_sqlw_init(VjoSqlw *w, const VjoSqlwOut *out)
{
    memset(w, 0, sizeof(*w));
    w->out = *out;
    w->next_page = 2; /* page 1 is sqlite_schema's, written last */
}

int vjo_sqlw_table_begin(VjoSqlw *w, VjoSqlwTable *t, const char *name, const char *sql)
{
    if (w->err)
        return w->err;
    if (w->t || w->ntables == VJO_SQLW_MAX_TABLES)
        return fail(w, VJO_E_PARSE);
    memset(t, 0, sizeof(*t));
    t->content = U;
    w->t = t;
    w->table[w->ntables].name = name;
    w->table[w->ntables].sql = sql;
    return 0;
}

/* Writes interior page of children c[0..k-1] (k >= 2) at level: a cell (the
 * child, its largest rowid) for each but the last, which is the right-most
 * pointer. Its page number, or 0. */
static uint32_t emit_interior(VjoSqlw *w, VjoSqlwTable *t, int level, const VjoSqlwChild *c, unsigned k)
{
    uint32_t pgno = new_pages(w, 1);
    unsigned content = U;
    uint8_t *p = w->page;
    if (!pgno)
        return 0;
    memset(p, 0, U);
    p[0] = 0x05; /* table interior */
    put16(p + 3, k - 1u);
    put32(p + 8, c[k - 1u].pgno);
    for (unsigned i = 0; i + 1u < k; i++) {
        uint8_t cell[13];
        unsigned n = 4u + (unsigned)vjo_sqlw_varint(cell + 4, (uint64_t)c[i].key);
        put32(cell, c[i].pgno);
        content -= n;
        memcpy(p + content, cell, n);
        put16(p + INTERIOR_HDR + 2u * i, content);
    }
    put16(p + 5, content);
    if (write_page(w, pgno, p) < 0)
        return 0;
    t->emitted[level]++;
    return pgno;
}

/* A finished page of the level below becomes a child at level. With
 * FANOUT + 2 children waiting, the first FANOUT make a page and two stay:
 * the level's last page then never has fewer than two. */
static int push(VjoSqlw *w, VjoSqlwTable *t, int level, uint32_t pgno, int64_t key)
{
    VjoSqlwChild *c;
    uint32_t page;
    int64_t last;
    if (w->err)
        return w->err;
    if (level > VJO_SQLW_LEVELS)
        return fail(w, VJO_E_TOO_LARGE);
    c = t->child[level];
    c[t->nchild[level]].pgno = pgno;
    c[t->nchild[level]].key = key;
    if (++t->nchild[level] < FANOUT + 2u)
        return 0;
    last = c[FANOUT - 1u].key;
    if (!(page = emit_interior(w, t, level, c, FANOUT)))
        return w->err;
    c[0] = c[FANOUT];
    c[1] = c[FANOUT + 1u];
    t->nchild[level] = 2;
    return push(w, t, level + 1, page, last);
}

/* Writes the leaf being filled. Its page number, or 0. */
static uint32_t emit_leaf(VjoSqlw *w, VjoSqlwTable *t)
{
    uint32_t pgno = new_pages(w, 1);
    uint8_t *p = t->leaf;
    unsigned end = LEAF_HDR + 2u * t->ncells;
    if (!pgno)
        return 0;
    p[0] = 0x0D; /* table leaf */
    put16(p + 1, 0);
    put16(p + 3, t->ncells);
    put16(p + 5, t->content);
    p[7] = 0;
    memset(p + end, 0, t->content - end); /* the unused middle: zeros, as the file is reproducible */
    if (write_page(w, pgno, p) < 0)
        return 0;
    t->emitted[0]++;
    return pgno;
}

int vjo_sqlw_row(VjoSqlw *w, int64_t rowid, const void *record, size_t len)
{
    VjoSqlwTable *t = w->t;
    const uint8_t *rec = (const uint8_t *)record;
    uint8_t head[18], *cell;
    size_t local, size;
    uint32_t first = 0;
    unsigned hl;
    if (w->err)
        return w->err;
    if (!t || rowid <= 0 || (t->rows && rowid <= t->last_rowid) || (len && !record))
        return fail(w, VJO_E_PARSE);
    hl = (unsigned)vjo_sqlw_varint(head, len);
    hl += (unsigned)vjo_sqlw_varint(head + hl, (uint64_t)rowid);
    local = vjo_sqlw_local_payload(len);
    size = hl + local + (local < len ? 4u : 0u); /* at most 4,083 B: an empty leaf takes it */
    if (size + 2u > t->content - (LEAF_HDR + 2u * t->ncells)) {
        uint32_t pgno = emit_leaf(w, t);
        if (!pgno || push(w, t, 1, pgno, t->last_rowid) < 0)
            return w->err;
        t->ncells = 0;
        t->content = U;
    }
    if (local < len) { /* the rest of the payload: a chain of overflow pages */
        size_t rest = len - local, per = U - 4u;
        uint32_t n = (uint32_t)((rest + per - 1u) / per);
        const uint8_t *src = rec + local;
        if (!(first = new_pages(w, n)))
            return w->err;
        for (uint32_t k = 0; k < n; k++) {
            size_t part = rest < per ? rest : per;
            memset(w->page, 0, U);
            put32(w->page, k + 1u < n ? first + k + 1u : 0);
            memcpy(w->page + 4, src, part);
            if (write_page(w, first + k, w->page) < 0)
                return w->err;
            src += part;
            rest -= part;
        }
    }
    t->content -= (unsigned)size;
    cell = t->leaf + t->content;
    memcpy(cell, head, hl);
    if (local)
        memcpy(cell + hl, rec, local);
    if (first)
        put32(cell + hl + local, first);
    put16(t->leaf + LEAF_HDR + 2u * t->ncells, t->content);
    t->ncells++;
    t->last_rowid = rowid;
    t->rows++;
    return 0;
}

int vjo_sqlw_table_end(VjoSqlw *w)
{
    VjoSqlwTable *t = w->t;
    uint32_t pgno;
    if (w->err)
        return w->err;
    if (!t)
        return fail(w, VJO_E_PARSE);
    if (!(pgno = emit_leaf(w, t))) /* the last leaf, or an empty table's root */
        return w->err;
    if (t->emitted[0] > 1) {
        if (push(w, t, 1, pgno, t->last_rowid) < 0)
            return w->err;
        /* Every page of the level below is a child here, some already in
         * pages of this level; two to FANOUT + 1 are left. */
        for (int level = 1;; level++) {
            VjoSqlwChild *c = t->child[level];
            unsigned n = t->nchild[level], a = n <= FANOUT ? n : n / 2u;
            if (level > VJO_SQLW_LEVELS || n < 2u)
                return fail(w, VJO_E_TOO_LARGE);
            t->nchild[level] = 0;
            if (!(pgno = emit_interior(w, t, level, c, a)))
                return w->err;
            if (a < n) { /* FANOUT + 1: two pages of at least two */
                if (push(w, t, level + 1, pgno, c[a - 1u].key) < 0 ||
                    !(pgno = emit_interior(w, t, level, c + a, n - a)))
                    return w->err;
            }
            if (t->emitted[level] == 1)
                break; /* the root */
            if (push(w, t, level + 1, pgno, c[n - 1u].key) < 0)
                return w->err;
        }
    }
    t->root = pgno;
    w->table[w->ntables].root = pgno;
    w->table[w->ntables].rows = t->rows;
    w->ntables++;
    w->t = NULL;
    return 0;
}

int vjo_sqlw_finish(VjoSqlw *w, uint32_t *pages)
{
    uint8_t *p = w->page, rec[1536];
    unsigned content = U;
    if (w->err)
        return w->err;
    if (w->t)
        return fail(w, VJO_E_PARSE);
    memset(p, 0, U);
    /* sqlite_schema: (type, name, tbl_name, rootpage, sql) per table, on this page */
    for (int i = 0; i < w->ntables; i++) {
        VjoSqlwVal v[5] = {
            {VJO_SQLW_TEXT, 0, "table", 5},
            {VJO_SQLW_TEXT, 0, w->table[i].name, strlen(w->table[i].name)},
            {VJO_SQLW_TEXT, 0, w->table[i].name, strlen(w->table[i].name)},
            {VJO_SQLW_INT, (int64_t)w->table[i].root, NULL, 0},
            {VJO_SQLW_TEXT, 0, w->table[i].sql, strlen(w->table[i].sql)},
        };
        size_t header, len = record_len(v, 5, &header);
        unsigned hl;
        uint8_t head[18];
        if (len > sizeof(rec))
            return fail(w, VJO_E_PARSE);
        record_put(rec, v, 5, header);
        hl = (unsigned)vjo_sqlw_varint(head, len);
        hl += (unsigned)vjo_sqlw_varint(head + hl, (uint64_t)i + 1u);
        if (hl + len + 2u > content - (DB_HDR + LEAF_HDR + 2u * (unsigned)i))
            return fail(w, VJO_E_PARSE); /* the schema must fit page 1 */
        content -= hl + (unsigned)len;
        memcpy(p + content, head, hl);
        memcpy(p + content + hl, rec, len);
        put16(p + DB_HDR + LEAF_HDR + 2u * (unsigned)i, content);
    }
    p[DB_HDR] = 0x0D;
    put16(p + DB_HDR + 3, (unsigned)w->ntables);
    put16(p + DB_HDR + 5, content);
    /* the database header (fileformat2.html, "The Database Header") */
    memcpy(p, "SQLite format 3", 16);
    put16(p + 16, U);
    p[18] = 1; /* legacy (rollback journal) write and read versions */
    p[19] = 1;
    p[20] = 0;  /* no reserved bytes */
    p[21] = 64; /* the fixed payload fractions */
    p[22] = 32;
    p[23] = 32;
    put32(p + 24, 1);                  /* file change counter */
    put32(p + 28, w->next_page - 1u);  /* pages in the database (valid: 92 == 24) */
    put32(p + 40, 1);                  /* schema cookie */
    put32(p + 44, 4);                  /* schema format 4 */
    put32(p + 56, 1);                  /* UTF-8 */
    put32(p + 92, 1);                  /* version-valid-for = the change counter */
    put32(p + 96, WRITER_VERSION);
    if (write_page(w, 1, p) < 0)
        return w->err;
    if (pages)
        *pages = w->next_page - 1u;
    return 0;
}
