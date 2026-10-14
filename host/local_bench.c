/* Benchmark the real overlay lookup with counted file callbacks, without OCR,
 * stdout rendering or process startup in the timer. All buffers here are host
 * instrumentation; reported arena_peak measures the production arena only.
 * With RESULTS (local_dictionary_results), every other dictionary page at the
 * cursor is then read and timed separately, as an L/R tap does in the Shell. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "client.h"
#include "local_dict.h"
#include "net_posix.h"

typedef struct {
    VjoFile inner;
    uint64_t last_end;
    int active;
} TrackedFile;
static TrackedFile files[VJO_LOCAL_MAX_DICTS];
static uint64_t calls, bytes, seeks;
static int opened, closed;
static int (*inner_open)(void *, const char *, VjoFile *);

static int tracked_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    TrackedFile *f = ctx;
    calls++; bytes += n;
    if (off != f->last_end) seeks++;
    f->last_end = off + n;
    return f->inner.read(f->inner.ctx, off, dst, n);
}
static void tracked_close(void *ctx)
{
    TrackedFile *f = ctx;
    f->inner.close(f->inner.ctx); f->active = 0; closed++;
}
static int tracked_open(void *ud, const char *path, VjoFile *out)
{
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        if (files[i].active) continue;
        TrackedFile *f = &files[i];
        if (inner_open(ud, path, &f->inner)) return -1;
        f->active = 1; f->last_end = 0; opened++;
        *out = (VjoFile){f, f->inner.size, tracked_read, tracked_close, f->inner.stamp};
        return 0;
    }
    return -1;
}
static uint64_t nanoseconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static uint64_t hash_string(uint64_t hash, const char *s)
{
    do { hash = (hash ^ (uint8_t)*s) * UINT64_C(1099511628211); } while (*s++);
    return hash;
}
static uint64_t hash_entries(uint64_t digest, const VjoEntryList *l)
{
    char range[64];
    for (int i = 0; i < l->n_entries; i++) {
        const VjoEntry *e = &l->entries[i];
        digest = hash_string(digest, e->text);
        snprintf(range, sizeof(range), "%d,%d,%d", e->pos16, e->hl_start, e->hl_end);
        digest = hash_string(digest, range);
    }
    return digest;
}
static int parse_count(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long value = strtoul(s, &end, 10);
    if (!s[0] || s[0] == '-' || *end || value > max) return -1;
    *out = value;
    return 0;
}
int main(int argc, char **argv)
{
    static uint8_t mem[384 * 1024];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    int runs, results = 0;
    size_t cursor = 0;
    unsigned long value;
    if ((argc < 5 || argc > 7) || (runs = atoi(argv[4])) < 1 || runs > 10000) {
        fprintf(stderr, "usage: vjo-local-bench DIR FILENAMES TEXT RUNS [CURSOR_BYTE [RESULTS]]\n"); return 2;
    }
    if (argc >= 6) {
        if (parse_count(argv[5], strlen(argv[3]), &value)) return 2;
        cursor = (size_t)value;
    }
    if (argc == 7) {
        if (parse_count(argv[6], VJO_LOCAL_MAX_DICTS, &value) || !value) return 2;
        results = (int)value;
    }
    vjo_config_defaults(&cfg); cfg.dictionary = VJO_DICT_LOCAL;
    if (results) cfg.local_dictionary_results = results;
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries)) return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]); strcpy(cfg.local_dictionaries, argv[2]);
    posix_platform_init(&pp, &p); inner_open = p.file_open; p.file_open = tracked_open;
    for (int run = 0; run < runs; run++) {
        VjoArena a;
        VjoOverlayData d = {0};
        uint64_t start, elapsed, digest = UINT64_C(14695981039346656037);
        calls = bytes = seeks = 0; opened = closed = 0;
        vjo_arena_init(&a, mem, sizeof(mem));
        start = nanoseconds();
        int rc;
        if (argc >= 6) {
            rc = vjo_overlay_ocr_text(&a, &cfg, argv[3], &d);
            if (!rc) rc = vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d);
        } else {
            rc = vjo_overlay_from_text(&a, &p, &cfg, argv[3], &d);
        }
        elapsed = nanoseconds() - start;
        if (rc || opened != closed) {
            fprintf(stderr, "%s (handles %d/%d)\n", vjo_err_text(&a, d.failed_stage, &d.err), opened, closed); return 1;
        }
        digest = hash_entries(digest, &d.list);
        printf("{\"run\":%d,\"lookup_us\":%.3f,\"read_calls\":%" PRIu64 ",\"read_bytes\":%" PRIu64
               ",\"nonsequential_reads\":%" PRIu64 ",\"arena_peak\":%zu,\"entries\":%d,\"digest\":\"%016" PRIx64 "\"",
               run, elapsed / 1000.0, calls, bytes, seeks, a.peak, d.list.n_entries, digest);
        if (results) {
            /* What stays allocated while this word is shown, then each page in
             * turn: its own time, reads and arena peak above that residency. */
            int count = vjo_overlay_page_count(&d);
            printf(",\"resident\":%zu,\"page_count\":%d,\"pages\":[", a.used, count);
            for (int page = 1; page < count; page++) {
                uint64_t page_digest = UINT64_C(14695981039346656037);
                calls = bytes = seeks = 0; opened = closed = 0;
                a.peak = a.used;
                start = nanoseconds();
                rc = vjo_overlay_lookup_page(&a, &p, &cfg, page, &d);
                elapsed = nanoseconds() - start;
                if (rc || opened != closed || d.list.n_entries != 1) {
                    fprintf(stderr, "page %d: %s (handles %d/%d)\n", page,
                            vjo_err_text(&a, d.failed_stage, &d.err), opened, closed);
                    return 1;
                }
                page_digest = hash_entries(page_digest, &d.list);
                printf("%s{\"page\":%d,\"lookup_us\":%.3f,\"read_calls\":%" PRIu64 ",\"read_bytes\":%" PRIu64
                       ",\"files_opened\":%d,\"arena_peak\":%zu,\"resident\":%zu,\"digest\":\"%016" PRIx64 "\"}",
                       page > 1 ? "," : "", page, elapsed / 1000.0, calls, bytes, opened, a.peak, a.used,
                       page_digest);
            }
            printf("]");
        }
        printf("}\n");
    }
    return 0;
}
