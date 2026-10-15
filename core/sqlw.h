/* A minimal SQLite 3 database writer: tables bulk-loaded once, each in
 * increasing rowid order, into a new file, as the Overlay app's .apkg export
 * needs (core/apkg.c). It writes the documented file format
 * (https://www.sqlite.org/fileformat2.html) and nothing else: 4096-byte
 * pages, UTF-8, schema format 4, rollback-journal mode, no free pages, no
 * indexes, no auto-vacuum. Page 1 (the header and sqlite_schema) is written
 * last; every other page is written once, when it is complete.
 *
 * A table's B-tree is built bottom-up: leaves are filled in order and an
 * interior level keeps at most VJO_SQLW_FANOUT + 2 children in memory, so a
 * table of any size takes the same memory (a VjoSqlwTable, about 30 KiB).
 * Every interior page has at least two children and every leaf at least one
 * row (the root of an empty table excepted), as SQLite's own balancing
 * leaves them. A payload too large for its leaf goes on overflow pages as
 * SQLite would put it. host/tests and tools/tests/test_apkg.py check the
 * output with SQLite's PRAGMA integrity_check. */
#ifndef VJO_SQLW_H
#define VJO_SQLW_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

#define VJO_SQLW_PAGE 4096u
/* Children of a table interior page: a 12-byte header, then per cell a
 * 2-byte pointer, a 4-byte child page and a rowid of at most 9 bytes. */
#define VJO_SQLW_FANOUT ((VJO_SQLW_PAGE - 12u) / 15u + 1u)
#define VJO_SQLW_LEVELS 5 /* interior levels: FANOUT^5 leaves, far more than the page limit */
/* The lock-byte page (the one holding the file's byte 1,073,741,824) must
 * never hold data; the writer stays below it. */
#define VJO_SQLW_MAX_PAGES (1073741824u / VJO_SQLW_PAGE)
#define VJO_SQLW_MAX_TABLES 8

/* The database's bytes: off is the offset in the database (its caller adds
 * where the database starts in its file). 0, or < 0 (the error). */
typedef struct {
    void *ctx;
    int (*write)(void *ctx, uint64_t off, const void *src, size_t n);
} VjoSqlwOut;

/* A record's column. */
enum { VJO_SQLW_NULL = 0, VJO_SQLW_INT = 1, VJO_SQLW_TEXT = 2 };
typedef struct {
    int kind;
    int64_t i;     /* VJO_SQLW_INT */
    const char *s; /* VJO_SQLW_TEXT: n bytes of UTF-8 */
    size_t n;
} VjoSqlwVal;

typedef struct {
    uint32_t pgno;
    int64_t key; /* the largest rowid below it */
} VjoSqlwChild;

/* The table being written. */
typedef struct {
    uint8_t leaf[VJO_SQLW_PAGE];
    unsigned ncells, content; /* the leaf's cells and where their content starts */
    int64_t last_rowid;
    uint64_t rows;
    VjoSqlwChild child[VJO_SQLW_LEVELS + 1][VJO_SQLW_FANOUT + 2];
    unsigned nchild[VJO_SQLW_LEVELS + 1];
    uint32_t emitted[VJO_SQLW_LEVELS + 1]; /* pages written at each level (0: the leaves) */
    uint32_t root;
} VjoSqlwTable;

typedef struct {
    VjoSqlwOut out;
    int err;            /* the first error; every later call returns it */
    uint32_t next_page; /* the next page number to give out */
    uint8_t page[VJO_SQLW_PAGE];
    VjoSqlwTable *t;    /* the table being written, NULL between tables */
    struct {
        const char *name, *sql; /* kept by pointer until vjo_sqlw_finish */
        uint32_t root;
        uint64_t rows;
    } table[VJO_SQLW_MAX_TABLES];
    int ntables;
} VjoSqlw;

void vjo_sqlw_init(VjoSqlw *w, const VjoSqlwOut *out);
/* Starts table name (its CREATE TABLE statement in sql) in t; its rows
 * follow, then vjo_sqlw_table_end. */
int vjo_sqlw_table_begin(VjoSqlw *w, VjoSqlwTable *t, const char *name, const char *sql);
/* A row of the current table: rowid (greater than the last one, > 0) and its
 * record (vjo_sqlw_record). */
int vjo_sqlw_row(VjoSqlw *w, int64_t rowid, const void *record, size_t len);
int vjo_sqlw_table_end(VjoSqlw *w);
/* Writes page 1, the database header and sqlite_schema, once every table is
 * written: the database is then complete, *pages pages long. 0, or < 0. */
int vjo_sqlw_finish(VjoSqlw *w, uint32_t *pages);

/* The n values as an SQLite record (its header, then its body), allocated
 * in a: *out and *len, or VJO_E_OOM. An INTEGER PRIMARY KEY column is
 * VJO_SQLW_NULL: the rowid holds its value. */
int vjo_sqlw_record(VjoArena *a, const VjoSqlwVal *v, int n, uint8_t **out, size_t *len);

/* SQLite's variable-length integer: writes 1-9 bytes to p, returns how many. */
int vjo_sqlw_varint(uint8_t *p, uint64_t v);
/* Reads one at p (at most n bytes): its length, or 0 when it does not fit. */
int vjo_sqlw_get_varint(const uint8_t *p, size_t n, uint64_t *v);
/* The bytes of a table leaf cell's payload kept on its page (the rest on
 * overflow pages), as SQLite computes it. */
size_t vjo_sqlw_local_payload(size_t payload);

#endif
