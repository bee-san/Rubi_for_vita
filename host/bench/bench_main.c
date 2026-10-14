/* The stage benchmark's driver (stage_bench.h): arguments, input files, the
 * dictionary file with every read counted, the host clock and one JSON line of
 * results. Built by the host compiler, or for the Cortex-A9 lane by the ARM
 * Linux toolchain (VJO_A9_LANE), where it also stands in for the SceShell libc
 * the Vita-built core imports (sceClibSnprintf, sceClibVsnprintf).
 *
 *   vjo-stage-bench STAGE [--iters N] [--frame RAW --fb WxH --region X,Y,W,H]
 *       [--fmt abgr|a2b10|a1b5] [--strip-rows N] [--model FILE --model-name NAME]
 *       [--texts TSV | --text TEXT] [--dict DIR] [--config FILE] [--mode N]
 *
 * RAW: R, G, B, A bytes, W x H. TSV: host/fixtures/word_lookup/vocr_lines.tsv
 * (the text is the sixth column, "\n" escaped). */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stage_bench.h"

#ifdef VJO_A9_LANE
/* The Vita-built core formats through SceShell's sceClib; glibc stands in. */
int sceClibVsnprintf(char *dst, size_t cap, const char *fmt, va_list ap) { return vsnprintf(dst, cap, fmt, ap); }
int sceClibSnprintf(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}
#endif

/* ---- the dictionary file, every read counted ---- */

#define SEEN_SLOTS 4096u
static uint64_t io_opens, io_reads, io_bytes, io_repeats, io_pages;
static uint64_t seen[SEEN_SLOTS]; /* 4 KiB pages read since bench_io_text, +1 (0 = empty) */
/* VJO_BENCH_TRACE=FILE: every read as "TEXT OFFSET BYTES" (TEXT counts bench_io_text calls),
 * every open as "TEXT open", for cache studies (tools/bench_stages.py dict-trace). */
static FILE *trace;
static unsigned trace_text;

void bench_io_mark(void)
{
    io_opens = io_reads = io_bytes = io_repeats = io_pages = 0;
#ifndef VJO_A9_LANE
    if (!trace && getenv("VJO_BENCH_TRACE")) trace = fopen(getenv("VJO_BENCH_TRACE"), "w");
#endif
}

void bench_io_text(void)
{
    memset(seen, 0, sizeof(seen));
    trace_text++;
}

void bench_io_counts(uint64_t *opens, uint64_t *reads, uint64_t *bytes, uint64_t *repeat_reads, uint64_t *pages)
{
    *opens = io_opens;
    *reads = io_reads;
    *bytes = io_bytes;
    *repeat_reads = io_repeats;
    *pages = io_pages;
}

static void note_page(uint64_t page)
{
    uint64_t key = page + 1, h = (page * UINT64_C(0x9E3779B97F4A7C15)) >> 52;
    for (unsigned i = 0; i < SEEN_SLOTS; i++) {
        uint64_t *s = &seen[(h + i) & (SEEN_SLOTS - 1)];
        if (*s == key) { io_repeats++; return; }
        if (!*s) { *s = key; io_pages++; return; }
    }
}

static int file_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    int fd = (int)(intptr_t)ctx;
    unsigned char *p = dst;
    io_reads++;
    io_bytes += n;
    note_page(off >> 12);
    if (trace) fprintf(trace, "%u %" PRIu64 " %zu\n", trace_text, off, n);
    while (n) {
        ssize_t got = pread(fd, p, n, (off_t)off);
        if (got <= 0) return -1;
        p += got;
        off += (uint64_t)got;
        n -= (size_t)got;
    }
    return 0;
}

static void file_close(void *ctx) { close((int)(intptr_t)ctx); }

static int file_open(void *ud, const char *path, VjoFile *out)
{
    int fd = open(path, O_RDONLY);
    off_t size;
    (void)ud;
    if (fd < 0) return -1;
    size = lseek(fd, 0, SEEK_END);
    if (size < 0) { close(fd); return -1; }
    io_opens++;
    if (trace) fprintf(trace, "%u open\n", trace_text);
    *out = (VjoFile){(void *)(intptr_t)fd, (uint64_t)size, file_read, file_close, 0};
    return 0;
}

static uint64_t clock_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* ---- inputs ---- */

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        if (f) fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

/* The sixth column of each row, unescaped, records joined by '\x1e'. */
static char *read_texts(const char *path)
{
    size_t len, used = 0;
    char *tsv = read_file(path, &len), *out, *line, *save = NULL;
    if (!tsv || !(out = malloc(len + 2))) return NULL;
    for (line = strtok_r(tsv, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = line;
        if (line[0] == '#') continue;
        for (int tabs = 0; tabs < 5 && t; tabs++) t = strchr(t, '\t') ? strchr(t, '\t') + 1 : NULL;
        if (!t || !*t) continue;
        if (used) out[used++] = '\x1e';
        for (; *t && *t != '\r'; t++) {
            if (t[0] == '\\' && t[1] == 'n') { out[used++] = '\n'; t++; }
            else out[used++] = *t;
        }
    }
    out[used] = 0;
    free(tsv);
    return out;
}

static void json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') printf("\\n");
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static int usage(void)
{
    const char *const *s = bench_stages();
    fprintf(stderr, "usage: vjo-stage-bench STAGE [options] (docs/performance.md); stages:");
    for (; *s; s++) fprintf(stderr, " %s", *s);
    fprintf(stderr, "\n");
    return 2;
}

int main(int argc, char **argv)
{
    BenchIn in;
    static BenchOut out;
    const char *stage, *frame = NULL, *model = NULL, *texts = NULL, *config = NULL;
    unsigned iters = 1;
    size_t len = 0;
    int rc;
    if (argc < 2) return usage();
    stage = argv[1];
    memset(&in, 0, sizeof(in));
    in.fmt = 0;
    in.file_open = file_open;
#ifndef VJO_A9_LANE
    in.clock_ns = clock_ns;
#endif
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) return usage();
        i++;
        if (!strcmp(a, "--iters")) iters = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--frame")) frame = v;
        else if (!strcmp(a, "--fb")) { if (sscanf(v, "%ux%u", &in.fb_w, &in.fb_h) != 2) return usage(); }
        else if (!strcmp(a, "--region")) { if (sscanf(v, "%u,%u,%u,%u", &in.x, &in.y, &in.w, &in.h) != 4) return usage(); }
        else if (!strcmp(a, "--fmt")) in.fmt = !strcmp(v, "a2b10") ? 0x60800000u : !strcmp(v, "a1b5") ? 0x50000000u : 0u;
        else if (!strcmp(a, "--strip-rows")) in.strip_rows = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--model")) model = v;
        else if (!strcmp(a, "--model-name")) in.model_name = v;
        else if (!strcmp(a, "--texts")) texts = v;
        else if (!strcmp(a, "--text")) in.text = v;
        else if (!strcmp(a, "--dict")) in.dict_dir = v;
        else if (!strcmp(a, "--config")) config = v;
        else if (!strcmp(a, "--mode")) in.mode = (uint32_t)strtoul(v, NULL, 10);
        else return usage();
    }
    if (frame) {
        if (!(in.rgba = read_file(frame, &len)) || len != (size_t)in.fb_w * in.fb_h * 4) {
            fprintf(stderr, "%s: not a %ux%u RGBA frame\n", frame, in.fb_w, in.fb_h);
            return 2;
        }
        if (!in.w) { in.x = in.y = 0; in.w = in.fb_w; in.h = in.fb_h; }
    }
    if (model) {
        if (!(in.model = read_file(model, &len))) { fprintf(stderr, "%s: unreadable\n", model); return 2; }
        in.model_bytes = (uint32_t)len;
    }
    if (texts && !(in.text = read_texts(texts))) { fprintf(stderr, "%s: unreadable\n", texts); return 2; }
    if (config && !(in.config_text = read_file(config, NULL))) { fprintf(stderr, "%s: unreadable\n", config); return 2; }
    rc = bench_stage(stage, &in, iters, &out);
    if (rc) {
        fprintf(stderr, "%s: failed (%d)\n", stage, rc);
        return rc == -100 ? usage() : 1;
    }
    printf("{\"stage\": \"%s\", \"mode\": %u, \"iters\": %u, \"ops\": %" PRIu64 ", \"check\": \"%016" PRIx64 "\"", stage,
           in.mode, iters, out.ops, out.check);
    printf(", \"ns_per_iter\": %.1f", iters ? (double)out.ns / iters : 0.0);
    printf(", \"counters\": {");
    {
        const char *const *names = bench_counter_names(stage);
        for (int k = 0; names && names[k] && k < BENCH_COUNTERS; k++)
            printf("%s\"%s\": %" PRIu64, k ? ", " : "", names[k], out.c[k]);
    }
    printf("}");
    if (out.text[0]) {
        printf(", \"text\": ");
        json_string(out.text);
    }
    printf("}\n");
    return 0;
}
