/* vjo-sentence-readings: the readings the Rubi app writes into a card's SentenceFurigana
 * (core/sentence_reading.h), for lines of text on a computer: the measurement in
 * tools/sentence_readings/ and its console estimates.
 *
 *   vjo-sentence-readings --dir DIR --dicts plaintext.vjdict,vndb-names.vjdict [--ratio N] [--part-ratio N]
 *                         [--rare N] [--no-RULE ...] [--cache KIB] < lines.tsv > readings.tsv
 *
 * RULE: counters, variants, anchor, compounds, parts, phrases, shorter, names or inline (core/sentence_reading.h
 * VjoReadingRules), each on by default. --trace FILE: each matched word with kanji, `id<TAB>what<TAB>text<TAB>
 * spelling<TAB>reading` (what: read, or why it got none).
 *
 * Each input line is `id<TAB>text` (a line break in the text as \n, a backslash as \\). Each output line is
 * `id<TAB>readings<TAB>lookups<TAB>reads`, readings `start:end:reading` in code points, separated by spaces. On
 * stderr: the
 * totals (VjoReadingStats), the file reads and bytes the lookups made, the reads an app cache of KIB KiB
 * (4 KiB blocks, least recently used first) would have left (default 256), and the host time. One session
 * keeps the dictionaries open, as an export does. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "local_dict.h"
#include "platform_posix.h"
#include "sentence_reading.h"
#include "utf.h"

#define BLOCK 4096u

static FILE *trace_out;
static const char *trace_id;
static void trace(void *ud, const char *why, const char *text, size_t n, const char *spelling, const char *reading)
{
    (void)ud;
    fprintf(trace_out, "%s\t%s\t%.*s\t%s\t%s\n", trace_id, why, (int)n, text, spelling, reading);
}
#define MAX_FILES 8

static uint8_t scratch_mem[VJO_READINGS_SCRATCH + 64 * 1024], keep_mem[64 * 1024];
static uint64_t session_mem[1 + 8 * 1024 / 8];

/* ---- the reads: counted, and an LRU of blocks simulated ---- */
typedef struct {
    VjoFile inner;
    int id;
} Counted;
static Counted files[64];
static int n_files;
static VjoPlatform posix;
static unsigned long long reads, bytes, line_reads, misses;
static struct {
    int file;
    uint64_t block;
    unsigned long long used;
} *lru;
static int lru_cap;
static unsigned long long lru_clock;

static void touch(int file, uint64_t block)
{
    int victim = 0;
    for (int i = 0; i < lru_cap; i++) {
        if (lru[i].used && lru[i].file == file && lru[i].block == block) {
            lru[i].used = ++lru_clock;
            return;
        }
        if (lru[i].used < lru[victim].used)
            victim = i;
    }
    misses++;
    lru[victim].file = file;
    lru[victim].block = block;
    lru[victim].used = ++lru_clock;
}

static int counted_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    Counted *c = (Counted *)ctx;
    reads++;
    line_reads++;
    bytes += n;
    if (lru_cap)
        for (uint64_t b = off / BLOCK; n && b <= (off + n - 1) / BLOCK; b++)
            touch(c->id, b);
    return c->inner.read(c->inner.ctx, off, dst, n);
}

static void counted_close(void *ctx)
{
    Counted *c = (Counted *)ctx;
    c->inner.close(c->inner.ctx);
}

static int counted_open(void *ud, const char *path, VjoFile *out)
{
    Counted *c;
    if (n_files == (int)(sizeof(files) / sizeof(files[0])))
        return -1;
    c = &files[n_files];
    if (posix.file_open(posix.ud, path, &c->inner))
        return -1;
    c->id = n_files++;
    *out = c->inner;
    out->ctx = c;
    out->read = counted_read;
    out->close = counted_close;
    (void)ud;
    return 0;
}

/* \n a line break, \\ a backslash */
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && (r[1] == 'n' || r[1] == '\\')) {
            *w++ = r[1] == 'n' ? '\n' : '\\';
            r++;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static size_t cps(const char *s, size_t bytes_end)
{
    size_t i = 0, n = 0;
    while (i < bytes_end) {
        vjo_utf8_next(s, bytes_end, &i);
        n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    static char line[16384];
    PosixPlatform pp = {0};
    VjoPlatform p;
    VjoConfig cfg;
    VjoReadingRules rules;
    VjoReadingStats st;
    const char *dir = NULL, *dicts = NULL;
    int cache_kib = 256;
    unsigned long long lines = 0, lookups_before;
    struct timespec t0, t1;
    memset(&st, 0, sizeof(st));
    vjo_reading_rules_default(&rules);
    st.trace = NULL;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--dir") && more)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--dicts") && more)
            dicts = argv[++i];
        else if (!strcmp(argv[i], "--ratio") && more)
            rules.ratio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--part-ratio") && more)
            rules.part_ratio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rare") && more)
            rules.rare = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--no-", 5)) {
            static const struct { const char *name; size_t off; } flags[] = {
                {"counters", offsetof(VjoReadingRules, counters)}, {"variants", offsetof(VjoReadingRules, variants)},
                {"anchor", offsetof(VjoReadingRules, anchor)}, {"compounds", offsetof(VjoReadingRules, compounds)},
                {"parts", offsetof(VjoReadingRules, parts)}, {"phrases", offsetof(VjoReadingRules, phrases)},
                {"shorter", offsetof(VjoReadingRules, shorter)}, {"names", offsetof(VjoReadingRules, names)},
                {"inline", offsetof(VjoReadingRules, inline_readings)}};
            unsigned f = 0;
            while (f < sizeof(flags) / sizeof(flags[0]) && strcmp(argv[i] + 5, flags[f].name))
                f++;
            if (f == sizeof(flags) / sizeof(flags[0]))
                return 2;
            *(int *)((char *)&rules + flags[f].off) = 0;
        }
        else if (!strcmp(argv[i], "--trace") && more) {
            if (!(trace_out = fopen(argv[++i], "w")))
                return 1;
        } else if (!strcmp(argv[i], "--cache") && more)
            cache_kib = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: vjo-sentence-readings --dir DIR --dicts A.vjdict[,B.vjdict] [--ratio N] "
                            "[--part-ratio N] [--rare N] [--no-RULE] [--cache KIB] < lines.tsv\n");
            return 2;
        }
    }
    if (!dir || !dicts || rules.ratio < 1 || cache_kib < 0)
        return 2;
    if (trace_out)
        st.trace = trace;
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", dir);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", dicts);
    posix_platform_init(&pp, &posix);
    p = posix;
    p.file_open = counted_open;
    if (vjo_local_session_size() > sizeof(session_mem))
        return 1;
    memset(session_mem, 0, sizeof(session_mem));
    p.local_session = (VjoLocalSession *)session_mem;
    lru_cap = cache_kib * 1024 / (int)BLOCK;
    if (lru_cap && !(lru = calloc((size_t)lru_cap, sizeof(*lru))))
        return 1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (fgets(line, sizeof(line), stdin)) {
        char *tab = strchr(line, '\t'), *text;
        size_t len = strlen(line);
        VjoArena scratch, keep;
        VjoRuby r[1024];
        int n;
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (!tab)
            continue;
        *tab = 0;
        text = tab + 1;
        unescape(text);
        vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
        vjo_arena_init(&keep, keep_mem, sizeof(keep_mem));
        line_reads = 0;
        lookups_before = st.lookups;
        trace_id = line;
        n = vjo_sentence_rubies(&keep, &scratch, &p, &cfg, &rules, text, 0, strlen(text), r, 1024, &st);
        printf("%s\t", line);
        for (int k = 0; k < n; k++)
            printf("%s%zu:%zu:%.*s", k ? " " : "", cps(text, r[k].start), cps(text, r[k].end),
                   (int)r[k].reading_len, r[k].reading);
        if (n < 0)
            printf("ERROR %d", n);
        printf("\t%llu\t%llu\n", (unsigned long long)(st.lookups - lookups_before), line_reads);
        lines++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    vjo_local_session_close(p.local_session);
    fprintf(stderr,
            "lines %llu lookups %u words %u read %u ambiguous %u compound %u part %u phrase %u shorter %u name %u inline %u counters %u "
            "unplaced %u unchecked %u unmatched %u anchored %u failed %u\n",
            lines, st.lookups, st.words, st.read, st.ambiguous, st.compound, st.part, st.phrase, st.shorter, st.name,
            st.inline_readings, st.counters, st.unplaced, st.unchecked, st.unmatched, st.anchored, st.failed);
    fprintf(stderr, "reads %llu bytes %llu; with a %d KiB block cache: %llu reads; host %.3f s\n", reads, bytes,
            cache_kib, lru_cap ? misses : reads,
            (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    return 0;
}
