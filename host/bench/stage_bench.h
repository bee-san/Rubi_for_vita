/* The stage benchmark (docs/performance.md): every stage of an OCR job and of
 * the Shell's lookups, run with the real overlay and vita-vn-ocr sources.
 *
 * stage_bench.c holds the stages and kernel_glue.c the kernel's half; both are
 * built natively for the host timing and, for the Cortex-A9 counts, by the
 * Vita compiler (the plugins' own static libraries and kernel objects from a
 * Vita build directory are linked, so the instructions counted are the ones the
 * console runs). bench_main.c (argument parsing, files, the clock, printing)
 * is built by the host or the ARM Linux toolchain. Only plain types cross
 * between the two halves: no enums, no libc structures. */
#ifndef VJO_STAGE_BENCH_H
#define VJO_STAGE_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include "net.h" /* VjoFile */

typedef struct {
    const uint8_t *rgba;      /* the frame: R, G, B, A bytes, fb_w x fb_h */
    uint32_t fb_w, fb_h;
    uint32_t x, y, w, h;      /* the region in pixels */
    uint32_t fmt;             /* SceDisplay pixel format (core/fbgrey.h) for the capture stages */
    uint32_t strip_rows;      /* rows per kernel rows call; 0: vjo_vocr_strip_rows */
    const uint8_t *model;     /* weights (a pinned release asset), or NULL */
    uint32_t model_bytes;
    const char *model_name;   /* its name in core/vocr_ocr.c's table */
    const char *text;         /* lines of text, '\n'-separated (lookup, postproc, anki, furigana) */
    const char *dict_dir;     /* directory holding plaintext.vjdict */
    const char *config_text;  /* config.ini text (config stage); NULL: the built-in default */
    uint32_t mode;            /* stage-specific, see stage_bench.c */
    /* Files for the lookup stages (bench_main.c counts every read). */
    int (*file_open)(void *ud, const char *path, VjoFile *out);
    /* Host clock in ns, around the timed loop only (NULL on the A9 lane). */
    uint64_t (*clock_ns)(void);
} BenchIn;

#define BENCH_COUNTERS 12
typedef struct {
    uint64_t ops;                   /* units of work per iteration (strips, lines, lookups...) */
    uint64_t check;                 /* FNV-1a of the stage's output: equal before and after a change */
    uint64_t ns;                    /* host: time of the timed loop (all iterations) */
    uint64_t c[BENCH_COUNTERS];     /* per stage, named in stage_bench.c (bench_counter_names) */
    char text[4096];                /* the recognized text of a recognition stage */
} BenchOut;

/* Runs stage `name` once untimed (setup and warm-up) and then `iters` times
 * timed. 0, or a negative error (unknown stage, bad input, a stage failure). */
int bench_stage(const char *name, const BenchIn *in, unsigned iters, BenchOut *out);
/* The names of out->c[0..] for a stage, NULL-terminated. */
const char *const *bench_counter_names(const char *name);
/* The stage names, NULL-terminated. */
const char *const *bench_stages(void);

/* bench_main.c: the lookup stages' file reads (since bench_io_mark), and how
 * many of them read a 4 KiB page already read since bench_io_text (one text). */
void bench_io_mark(void);
void bench_io_text(void);
void bench_io_counts(uint64_t *opens, uint64_t *reads, uint64_t *bytes, uint64_t *repeat_reads,
                     uint64_t *pages);

/* kernel_glue.c: the real kernel/capture.c and kernel/scene.c entry points. */
int bench_grey_rows(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h, uint32_t x0,
                    uint32_t y0, uint32_t w, uint32_t h, uint32_t y, uint32_t n, unsigned char *dst,
                    uint32_t stride, int sig);
void bench_capture_sig_begin(uint32_t w, uint32_t h);
/* The display hook's region signature (region in 1/65536 of the frame). */
int bench_region_sig(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h, uint16_t rx,
                     uint16_t ry, uint16_t rw, uint16_t rh, void *sig);
uint32_t bench_capture_sig_hash(void);

#endif
