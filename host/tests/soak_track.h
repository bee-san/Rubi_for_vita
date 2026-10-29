/* What a soak test holds, by pool and name: every allocator the plugins use on the console, as the test's own
 * stubs of the SDK see it (memory blocks of each type, Paf blocks by ledger item, threads, file descriptors), with
 * the live count and bytes, the peak bytes, and how many were taken and given back. A soak runs a scenario's cycle
 * many times and compares two snapshots taken at the same point of the cycle: any item whose live count or bytes
 * differ grew (or shrank) over those cycles, and soak_growth names it with its bytes per cycle.
 * Host tests only (host/tests/test_game_soak.c, test_shell_worker.c). */
#ifndef VJO_SOAK_TRACK_H
#define VJO_SOAK_TRACK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A suite may use only some of these (test_shell_anki.c: soak_cycles). */
#define SOAK_UNUSED __attribute__((unused))
#define SOAK_ITEMS 64
typedef struct {
    const char *pool;
    char name[40];
    long long live, bytes, peak;
    unsigned long long allocs, frees;
} SoakItem;
static SoakItem soak_items[SOAK_ITEMS] SOAK_UNUSED;
static unsigned soak_n SOAK_UNUSED;

SOAK_UNUSED static SoakItem *soak_item(const char *pool, const char *name)
{
    for (unsigned i = 0; i < soak_n; i++)
        if (!strcmp(soak_items[i].pool, pool) && !strcmp(soak_items[i].name, name)) return &soak_items[i];
    if (soak_n == SOAK_ITEMS) {
        fprintf(stderr, "soak: more than %d items\n", SOAK_ITEMS);
        abort();
    }
    SoakItem *it = &soak_items[soak_n++];
    memset(it, 0, sizeof(*it));
    it->pool = pool;
    snprintf(it->name, sizeof(it->name), "%s", name);
    return it;
}

SOAK_UNUSED static void soak_take(const char *pool, const char *name, long long bytes)
{
    SoakItem *it = soak_item(pool, name);
    it->live++;
    it->bytes += bytes;
    it->allocs++;
    if (it->bytes > it->peak) it->peak = it->bytes;
}

/* 0, or -1 when nothing of that item was held (a double free or a free of something never taken) */
SOAK_UNUSED static int soak_give(const char *pool, const char *name, long long bytes)
{
    SoakItem *it = soak_item(pool, name);
    if (it->live <= 0 || it->bytes < bytes) return -1;
    it->live--;
    it->bytes -= bytes;
    it->frees++;
    return 0;
}

typedef struct {
    unsigned n;
    long long live[SOAK_ITEMS], bytes[SOAK_ITEMS];
    unsigned long long allocs[SOAK_ITEMS];
} SoakSnap;

SOAK_UNUSED static void soak_snap(SoakSnap *s)
{
    memset(s, 0, sizeof(*s));
    s->n = soak_n;
    for (unsigned i = 0; i < soak_n; i++) {
        s->live[i] = soak_items[i].live;
        s->bytes[i] = soak_items[i].bytes;
        s->allocs[i] = soak_items[i].allocs;
    }
}

/* Every pool's bytes held in b: the pools' names in order of first use. */
SOAK_UNUSED static long long soak_pool_bytes(const SoakSnap *s, const char *pool)
{
    long long sum = 0;
    for (unsigned i = 0; i < s->n; i++)
        if (!strcmp(soak_items[i].pool, pool)) sum += s->bytes[i];
    return sum;
}

/* a, then `cycles` cycles later b: each item that holds more (or less) at b, with its net bytes per cycle, on
 * stderr; the number of such items. The report line names the scenario, its cycles and, per pool, the bytes held
 * at b, the net bytes per cycle and the blocks taken per cycle. */
SOAK_UNUSED static unsigned soak_growth(const char *who, const char *scenario, const SoakSnap *a, const SoakSnap *b, unsigned cycles)
{
    unsigned grew = 0;
    const char *pools[16];
    unsigned np = 0;
    char line[1024];
    size_t len;
    for (unsigned i = 0; i < b->n; i++) {
        long long live0 = i < a->n ? a->live[i] : 0, bytes0 = i < a->n ? a->bytes[i] : 0;
        unsigned k;
        if (b->live[i] != live0 || b->bytes[i] != bytes0) {
            grew++;
            fprintf(stderr, "soak %s: %s: GROWTH %s %s: %lld -> %lld held, %lld -> %lld B over %u cycles (%+.1f B/cycle)\n",
                    who, scenario, soak_items[i].pool, soak_items[i].name, live0, b->live[i], bytes0, b->bytes[i], cycles,
                    cycles ? (double)(b->bytes[i] - bytes0) / cycles : 0.0);
        }
        for (k = 0; k < np && strcmp(pools[k], soak_items[i].pool); k++) {}
        if (k == np && np < 16) pools[np++] = soak_items[i].pool;
    }
    len = (size_t)snprintf(line, sizeof(line), "soak %s: %-44s %5u cycles:", who, scenario, cycles);
    for (unsigned k = 0; k < np && len < sizeof(line); k++) {
        long long held = soak_pool_bytes(b, pools[k]), before = soak_pool_bytes(a, pools[k]);
        unsigned long long taken = 0;
        for (unsigned i = 0; i < b->n; i++)
            if (!strcmp(soak_items[i].pool, pools[k])) taken += b->allocs[i] - (i < a->n ? a->allocs[i] : 0);
        len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %lld B held, %+.1f B/cycle, %.2f taken/cycle;",
                                pools[k], held, cycles ? (double)(held - before) / cycles : 0.0,
                                cycles ? (double)taken / cycles : 0.0);
    }
    fprintf(stderr, "%s\n", line);
    return grew;
}

/* The per-item table: name, live, bytes, peak, taken, given back (stderr). */
SOAK_UNUSED static void soak_table(const char *who)
{
    fprintf(stderr, "soak %s: %-14s %-34s %6s %10s %10s %10s %10s\n", who, "pool", "item", "live", "bytes", "peak",
            "taken", "freed");
    for (unsigned i = 0; i < soak_n; i++)
        fprintf(stderr, "soak %s: %-14s %-34s %6lld %10lld %10lld %10llu %10llu\n", who, soak_items[i].pool,
                soak_items[i].name, soak_items[i].live, soak_items[i].bytes, soak_items[i].peak, soak_items[i].allocs,
                soak_items[i].frees);
}

/* VJO_SOAK_CYCLES (the environment), else the default: the cycles a scenario runs. */
SOAK_UNUSED static unsigned soak_cycles(unsigned def)
{
    const char *s = getenv("VJO_SOAK_CYCLES");
    long v = s ? strtol(s, NULL, 10) : 0;
    return v > 0 ? (unsigned)v : def;
}

#endif
