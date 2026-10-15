/* The Shell's word lookups over real recognized lines, every file read counted
 * and attributed (tools/dict_lookup_bench.py, docs/dict-lookup-speed.md).
 *
 *   vjo-dict-bench DIR FILES LINES MODE ROUNDS
 *
 * LINES: host/fixtures/word_lookup/vocr_lines.tsv's layout (id, model, two
 * checksums, region, text with "\n" escaped; '#' comments). Each line becomes
 * the overlay's sentence (vjo_overlay_ocr_text, as the Shell keeps it) and is
 * looked up as the Shell does with the release defaults (Whole word,
 * Japanese-only, one dictionary page): MODE first = the first word only (what
 * Auto OCR prepares), words = then Right after each match to the end of the
 * text (core/text_rows.c vjo_word_step), letters = every selectable letter
 * (Character mode, or a tap on each). Every lookup gets a fresh 64 KiB arena,
 * the Shell's result arena, and starts with empty application caches, as in
 * the Shell; the host OS file cache stays warm.
 *
 * Round 0 counts: reads by what they fetch (core/local_dict.c, built here with
 * VJO_LOCAL_STATS), bytes, distinct 4 KiB pages, nonsequential reads, the
 * deinflection candidates generated, searched and found, and the arena peak.
 * Rounds 1..ROUNDS-1 time each lookup again in the same order; the printed
 * time is its median. One JSON line per lookup, then one summary line. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "client.h"
#include "local_dict.h"
#include "net_posix.h"
#include "text_rows.h"
#include "utf.h"

#define ARENA_BYTES (64u * 1024u) /* the Shell's local result arena */
#define MAX_LINES 4096
#define MAX_PAGES 512

typedef struct {
    VjoFile inner;
    uint64_t last_end;
    int active;
} TrackedFile;

static TrackedFile files[VJO_LOCAL_MAX_DICTS];
static uint64_t n_reads, n_bytes, n_seeks, n_opens, n_closes, n_excess;
static uint64_t pages[MAX_PAGES];
static unsigned n_pages, pages_overflow;
static int (*inner_open)(void *, const char *, VjoFile *);

static void note_page(int file, uint64_t page)
{
    uint64_t key = ((uint64_t)file << 56) | page;
    for (unsigned i = 0; i < n_pages; i++)
        if (pages[i] == key) return;
    if (n_pages < MAX_PAGES) pages[n_pages++] = key;
    else pages_overflow++;
}

static int tracked_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    TrackedFile *f = ctx;
    n_reads++;
    n_bytes += n;
    if (n > 4096) n_excess += n - 4096; /* beyond one 4 KiB page: the console model's bound */
    if (off != f->last_end) n_seeks++;
    f->last_end = off + n;
    for (uint64_t p = off >> 12; n && p <= (off + n - 1) >> 12; p++) note_page((int)(f - files), p);
    return f->inner.read(f->inner.ctx, off, dst, n);
}

static void tracked_close(void *ctx)
{
    TrackedFile *f = ctx;
    f->inner.close(f->inner.ctx);
    f->active = 0;
    n_closes++;
}

static int tracked_open(void *ud, const char *path, VjoFile *out)
{
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        TrackedFile *f = &files[i];
        if (f->active) continue;
        if (inner_open(ud, path, &f->inner)) return -1;
        f->active = 1;
        f->last_end = 0;
        n_opens++;
        *out = (VjoFile){f, f->inner.size, tracked_read, tracked_close, f->inner.stamp};
        return 0;
    }
    return -1;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *s = p;
    while (n--) h = (h ^ *s++) * UINT64_C(1099511628211);
    return h;
}

/* The shown result: rc, entries, each entry's text (headword, reading and
 * definition as displayed) and its highlight. */
static uint64_t digest(int rc, const VjoOverlayData *d)
{
    uint64_t h = UINT64_C(14695981039346656037);
    h = fnv(h, &rc, sizeof(rc));
    h = fnv(h, &d->list.n_entries, sizeof(d->list.n_entries));
    for (int i = 0; i < d->list.n_entries; i++) {
        const VjoEntry *e = &d->list.entries[i];
        int pos[3] = {e->pos16, e->hl_start, e->hl_end};
        h = fnv(h, e->text, strlen(e->text) + 1);
        h = fnv(h, pos, sizeof(pos));
    }
    return h;
}

typedef struct {
    char id[96], model[16];
    char *text;
} Line;

static int load_lines(const char *path, Line *lines, int max)
{
    FILE *f = fopen(path, "r");
    static char buf[65536];
    int n = 0;
    if (!f) return -1;
    while (n < max && fgets(buf, sizeof(buf), f)) {
        char *field[6], *s = buf, *out;
        int k = 0;
        size_t len = strlen(buf);
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
        if (!len || buf[0] == '#') continue;
        for (; k < 6; k++) {
            field[k] = s;
            if (k == 5) break;
            if (!(s = strchr(s, '\t'))) break;
            *s++ = 0;
        }
        if (k != 5) { fclose(f); return -1; }
        snprintf(lines[n].id, sizeof(lines[n].id), "%s", field[0]);
        snprintf(lines[n].model, sizeof(lines[n].model), "%s", field[1]);
        out = lines[n].text = malloc(strlen(field[5]) + 1);
        if (!out) { fclose(f); return -1; }
        for (s = field[5]; *s; s++) {
            if (s[0] == '\\' && s[1] == 'n') { *out++ = '\n'; s++; }
            else *out++ = *s;
        }
        *out = 0;
        n++;
    }
    fclose(f);
    return n;
}

typedef struct {
    int line;
    size_t cursor, end;
    int rc, entries;
    uint64_t digest, opens, reads, bytes, excess, pages, seeks, arena_peak;
    VjoLocalStats stats;
    double *us;
} Query;

static Query *queries;
static size_t n_queries, cap_queries;

static Query *new_query(void)
{
    if (n_queries == cap_queries) {
        cap_queries = cap_queries ? cap_queries * 2 : 1024;
        queries = realloc(queries, cap_queries * sizeof(*queries));
        if (!queries) exit(3);
    }
    memset(&queries[n_queries], 0, sizeof(*queries));
    return &queries[n_queries++];
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* VJO_DICT_TRACE=FILE: every candidate search of round 0 as
 * "QUERY DICT CANDIDATE DEPTH OUTCOME KEY" (local_dict.h VJO_PROBE_*). */
static FILE *trace_file;
static size_t trace_query;
static void trace_probe(int dict, int cand, int depth, const char *key, int outcome)
{
    fprintf(trace_file, "%zu\t%d\t%d\t%d\t%d\t%s\n", trace_query, dict, cand, depth, outcome, key);
}

int main(int argc, char **argv)
{
    static uint8_t arena_mem[ARENA_BYTES] __attribute__((aligned(16)));
    static uint8_t text_mem[64 * 1024] __attribute__((aligned(16)));
    static Line lines[MAX_LINES];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    int n_lines, mode, rounds;
    char **sentences;
    if (argc != 6 || (rounds = atoi(argv[5])) < 1 || rounds > 1000) {
        fprintf(stderr, "usage: vjo-dict-bench DIR FILES LINES first|words|letters ROUNDS\n");
        return 2;
    }
    if (!strcmp(argv[4], "first")) mode = 0;
    else if (!strcmp(argv[4], "words")) mode = 1;
    else if (!strcmp(argv[4], "letters")) mode = 2;
    else { fprintf(stderr, "unknown mode %s\n", argv[4]); return 2; }
    if ((n_lines = load_lines(argv[3], lines, MAX_LINES)) <= 0) {
        fprintf(stderr, "cannot read lines from %s\n", argv[3]);
        return 2;
    }
    vjo_config_defaults(&cfg); /* the release defaults: Whole word, Japanese-only, one result */
    cfg.dictionary = VJO_DICT_LOCAL;
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries))
        return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]);
    strcpy(cfg.local_dictionaries, argv[2]);
    posix_platform_init(&pp, &p);
    inner_open = p.file_open;
    p.file_open = tracked_open;

    /* The overlay's sentence for each line, as the Shell keeps it. */
    sentences = calloc((size_t)n_lines, sizeof(*sentences));
    for (int i = 0; i < n_lines; i++) {
        VjoArena a;
        VjoOverlayData d;
        memset(&d, 0, sizeof(d));
        vjo_arena_init(&a, text_mem, sizeof(text_mem));
        if (vjo_overlay_ocr_text(&a, &cfg, lines[i].text, &d) || !d.sentence) {
            fprintf(stderr, "%s: no sentence\n", lines[i].id);
            return 1;
        }
        sentences[i] = strdup(d.sentence);
    }

    /* Round 0: the queries, their results and counts. */
    if (getenv("VJO_DICT_TRACE") && (trace_file = fopen(getenv("VJO_DICT_TRACE"), "w")))
        vjo_local_trace = trace_probe;
    for (int i = 0; i < n_lines; i++) {
        const char *s = sentences[i];
        size_t len = strlen(s), cursor = vjo_utf8_japanese_cursor_step(s, len, 0, 0);
        VjoWordNavigation nav;
        memset(&nav, 0, sizeof(nav));
        for (int guard = 0; cursor < len && guard < 4096; guard++) {
            Query *q = new_query();
            VjoArena a;
            VjoOverlayData d;
            size_t next;
            int row = VJO_ROW_STAY;
            memset(&d, 0, sizeof(d));
            d.ocr_text = d.filtered = d.sentence = s;
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            n_reads = n_bytes = n_seeks = n_opens = n_closes = n_excess = 0;
            n_pages = pages_overflow = 0;
            memset(&vjo_local_stats, 0, sizeof(vjo_local_stats));
            q->line = i;
            q->cursor = cursor;
            trace_query = n_queries - 1;
            q->rc = vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d);
            if (n_opens != n_closes) {
                fprintf(stderr, "%s at %zu: %" PRIu64 " opens, %" PRIu64 " closes\n", lines[i].id, cursor, n_opens,
                        n_closes);
                return 1;
            }
            q->entries = d.list.n_entries;
            q->end = d.list.n_entries ? (size_t)d.list.entries[0].hl_end : 0;
            q->digest = digest(q->rc, &d);
            q->opens = n_opens;
            q->reads = n_reads;
            q->bytes = n_bytes;
            q->excess = n_excess;
            q->pages = n_pages + pages_overflow;
            q->seeks = n_seeks;
            q->arena_peak = a.peak;
            q->stats = vjo_local_stats;
            if (mode == 0) break;
            if (mode == 1) {
                next = vjo_word_step(&nav, s, len, cursor, q->end, 1, cfg.japanese_only_selection, NULL, &row);
            } else {
                next = vjo_utf8_cursor_step(s, len, cursor, 1);
                next = next > cursor ? vjo_utf8_japanese_cursor_step(s, len, next, 0) : len;
            }
            if (next <= cursor) break; /* the last word: Right wraps to the first */
            cursor = next;
        }
    }

    /* Rounds 1..: the same lookups, timed. */
    vjo_local_trace = NULL;
    if (trace_file) fclose(trace_file);
    for (size_t k = 0; k < n_queries; k++)
        queries[k].us = calloc((size_t)rounds, sizeof(double));
    for (int r = 1; r < rounds; r++) {
        for (size_t k = 0; k < n_queries; k++) {
            Query *q = &queries[k];
            VjoArena a;
            VjoOverlayData d;
            uint64_t t0;
            int rc;
            memset(&d, 0, sizeof(d));
            d.ocr_text = d.filtered = d.sentence = sentences[q->line];
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            t0 = now_ns();
            rc = vjo_overlay_lookup_at(&a, &p, &cfg, q->cursor, &d);
            q->us[r - 1] = (double)(now_ns() - t0) / 1000.0;
            if (digest(rc, &d) != q->digest) {
                fprintf(stderr, "%s at %zu: result changed between rounds\n", lines[q->line].id, q->cursor);
                return 1;
            }
        }
    }

    for (size_t k = 0; k < n_queries; k++) {
        Query *q = &queries[k];
        const VjoLocalStats *st = &q->stats;
        double us = 0;
        if (rounds > 1) {
            qsort(q->us, (size_t)(rounds - 1), sizeof(double), cmp_double);
            us = q->us[(rounds - 1) / 2];
        }
        printf("{\"line\":%d,\"id\":\"%s\",\"model\":\"%s\",\"q\":%zu,\"cursor\":%zu,\"rc\":%d,\"entries\":%d,"
               "\"end\":%zu,\"digest\":\"%016" PRIx64 "\",\"us\":%.3f,\"opens\":%" PRIu64 ",\"reads\":%" PRIu64
               ",\"bytes\":%" PRIu64 ",\"excess_bytes\":%" PRIu64 ",\"pages\":%" PRIu64 ",\"nonsequential\":%" PRIu64 ",\"arena_peak\":%" PRIu64
               ",\"spans\":%" PRIu64 ",\"candidates\":%" PRIu64 ",\"probes\":%" PRIu64 ",\"filtered\":%" PRIu64
               ",\"miss_cached\":%" PRIu64 ",\"hash_present\":%" PRIu64 ",\"found\":%" PRIu64 ",\"ranked\":%" PRIu64
               ",\"reads_by\":{\"header\":%" PRIu64 ",\"index\":%" PRIu64 ",\"group\":%" PRIu64 ",\"head\":%" PRIu64
               ",\"payload\":%" PRIu64 ",\"filter\":%" PRIu64 "},\"bytes_by\":{\"header\":%" PRIu64
               ",\"index\":%" PRIu64 ",\"group\":%" PRIu64 ",\"head\":%" PRIu64 ",\"payload\":%" PRIu64
               ",\"filter\":%" PRIu64 "}}\n",
               q->line, lines[q->line].id, lines[q->line].model, k, q->cursor, q->rc, q->entries, q->end, q->digest,
               us, q->opens, q->reads, q->bytes, q->excess, q->pages, q->seeks, q->arena_peak, st->spans, st->candidates,
               st->probes, st->filtered, st->miss_cached, st->hash_present, st->found, st->ranked,
               st->reads[VJO_READ_HEADER], st->reads[VJO_READ_INDEX], st->reads[VJO_READ_GROUP],
               st->reads[VJO_READ_HEAD], st->reads[VJO_READ_PAYLOAD], st->reads[VJO_READ_FILTER],
               st->bytes[VJO_READ_HEADER], st->bytes[VJO_READ_INDEX], st->bytes[VJO_READ_GROUP],
               st->bytes[VJO_READ_HEAD], st->bytes[VJO_READ_PAYLOAD], st->bytes[VJO_READ_FILTER]);
    }
    printf("{\"summary\":{\"lines\":%d,\"lookups\":%zu,\"mode\":\"%s\",\"rounds\":%d,\"arena_bytes\":%u}}\n", n_lines,
           n_queries, argv[4], rounds, ARENA_BYTES);
    return 0;
}
