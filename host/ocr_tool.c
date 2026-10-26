/* Diagnostic runner for the bounded local recognizers SceShell uses.
 * Input: tightly packed RGBA8 (no image-decoder dependency).
 *
 *   vjo-ocr [--backend ncnn|vocr] [--model FILE] MODEL_DIR WIDTH HEIGHT INPUT.rgba
 *
 * stderr gets one line, `rc=... lines=... tensor_heap_peak=...`: the peak bytes
 * used inside the job's workspace (ncnn: its tensor heap in the 64 MiB block;
 * vocr: the exact workspace, all of which the job uses), then backend details. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local_ocr.h"
#include "platform_posix.h"
#ifdef VJO_WITH_VOCR
#include "vocr_ocr.h"
#include "vocr_lines_fix.h" /* VOCR_FIX_DEFAULT, _JUNK, _VALLEY (--line-fix) */
#endif

typedef struct { unsigned char *pixels; unsigned width, height; } Image;
static int rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    Image *im = ud;
    if (y > im->height || n > im->height-y) return -1;
    memcpy(dst, im->pixels+(size_t)y*im->width*4, (size_t)n*im->width*4);
    return (int)n;
}

#ifdef VJO_WITH_VOCR
/* --lines: one JSON object per found line (the policy's line callback). */
static void json_text(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void print_line(void *ud, const VjoVocrLineInfo *l)
{
    FILE *f = ud;
    const VjoVocrInk *k = &l->ink;
    fprintf(f, "{\"index\": %u, \"box\": [%u, %u, %u, %u], \"dark\": %u, \"ink\": {\"lo\": %u, \"hi\": %u, "
            "\"edge\": %u, \"edges\": %u}, \"blank\": %d, \"us\": %u, \"flags\": %u, \"text\": ",
            l->index, l->x, l->y, l->w, l->h, l->dark, k->lo, k->hi, k->edge, k->edges, l->blank, (unsigned)l->us,
            l->flags);
    json_text(f, l->text);
    fputs(", \"greedy\": ", f);
    json_text(f, l->greedy);
    fputs("}\n", f);
}

/* A line taller than a strip, kept whole, as the game worker's line block. */
static void *line_mem;
static void *line_buffer(void *ud, size_t bytes)
{
    (void)ud;
    if (!line_mem) line_mem = malloc(VJO_VOCR_LINE_MAX_BYTES);
    return bytes <= VJO_VOCR_LINE_MAX_BYTES ? line_mem : NULL;
}

static void *lm_mem;
static void *lm_alloc(void *ud, size_t bytes)
{
    (void)ud;
    if (lm_mem || posix_memalign(&lm_mem, 64, bytes)) return NULL;
    return lm_mem;
}
static int lm_release(void *ud)
{
    (void)ud;
    free(lm_mem);
    lm_mem = NULL;
    return 0;
}

static void *resident_mem;
static void *resident_alloc(void *ud, size_t bytes)
{
    (void)ud;
    if (resident_mem || posix_memalign(&resident_mem, 64, bytes)) return NULL;
    return resident_mem;
}
static int resident_release(void *ud)
{
    (void)ud;
    free(resident_mem);
    resident_mem = NULL;
    return 0;
}
/* The cascade's larger model: its own resident block. */
static void *cascade_mem;
static void *cascade_alloc(void *ud, size_t bytes)
{
    (void)ud;
    if (cascade_mem || posix_memalign(&cascade_mem, 64, bytes)) return NULL;
    return cascade_mem;
}
static int cascade_release(void *ud)
{
    (void)ud;
    free(cascade_mem);
    cascade_mem = NULL;
    return 0;
}
#endif

/* This process's peak resident set since exec (Linux VmHWM), or 0. A parent's
 * wait4 ru_maxrss would also count the pages it forked from. */
static unsigned long peak_rss(void)
{
    char line[128];
    unsigned long kib = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "VmHWM: %lu kB", &kib) == 1) break;
    fclose(f);
    return kib * 1024;
}

static int usage(void)
{
    fprintf(stderr, "Usage: vjo-ocr [--backend ncnn|vocr] [--model FILE] MODEL_DIR WIDTH HEIGHT INPUT.rgba\n"
#ifdef VJO_WITH_VOCR
                    "       vocr: [--lines OUT.jsonl] each found line as JSON; [--blank on] skip blank lines;\n"
                    "             [--lm vocr_lm6_o3_4000k.vlm] decode with that LM (in MODEL_DIR)\n"
                    "             [--cascade VOCR4_W20_w8.vocr [--cascade-conf 700] [--cascade-ms 2000]] read\n"
                    "             lines under that confidence again with the larger model, within that time;\n"
                    "             [--refine on] refine the finder's boxes (vita-vn-ocr's vocr_lines_refine)\n"
                    "             [--counters on] the counter re-decode (vita-vn-ocr's vocr_post_redecode) as the\n"
                    "             game worker runs it; needs --lm or --cascade (their top-K)\n"
                    "             [--head-gate on] skip the output head on blank frames (vita-vn-ocr's head gate,\n"
                    "             a model whose blob has one) as the game worker runs it by default\n"
                    "             [--two-level on] compute the rare output rows only where needed (vita-vn-ocr's\n"
                    "             two-level head, a model whose blob has one) as the game worker runs it by default\n"
                    "             [--line-fix on|junk|all] the line finder's fixes (vita-vn-ocr #129): on, Rubi's\n"
                    "             default, is VOCR_FIX_DEFAULT; junk adds JUNK, all also VALLEY\n"
#endif
                    "backends built:"
#ifdef VJO_WITH_NCNN
                    " ncnn"
#endif
#ifdef VJO_WITH_VOCR
                    " vocr (--model default " VJO_VOCR_DEFAULT_MODEL ")"
#endif
                    "\n");
    return 2;
}

int main(int argc, char **argv)
{
#ifdef VJO_WITH_NCNN
    const char *backend = "ncnn";
#else
    const char *backend = "vocr";
#endif
    const char *model = NULL;
#ifdef VJO_WITH_VOCR
    const char *lines_path = NULL, *lm_name = NULL, *cascade_name = NULL;
    int blank = 0, refine = 0, counters = 0, head_gate = 0, two_level = 0;
    unsigned line_fix = 0;
    unsigned cascade_conf = 0, cascade_ms = 2000;
#endif
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (i + 1 >= argc) return usage();
        if (!strcmp(argv[i], "--backend")) backend = argv[i+1];
        else if (!strcmp(argv[i], "--model")) model = argv[i+1];
#ifdef VJO_WITH_VOCR
        else if (!strcmp(argv[i], "--lines")) lines_path = argv[i+1];
        else if (!strcmp(argv[i], "--blank")) blank = !strcmp(argv[i+1], "on");
        else if (!strcmp(argv[i], "--lm")) lm_name = argv[i+1];
        else if (!strcmp(argv[i], "--cascade")) cascade_name = argv[i+1];
        else if (!strcmp(argv[i], "--cascade-conf")) cascade_conf = (unsigned)strtoul(argv[i+1], NULL, 10);
        else if (!strcmp(argv[i], "--cascade-ms")) cascade_ms = (unsigned)strtoul(argv[i+1], NULL, 10);
        else if (!strcmp(argv[i], "--refine")) refine = !strcmp(argv[i+1], "on");
        else if (!strcmp(argv[i], "--counters")) counters = !strcmp(argv[i+1], "on");
        else if (!strcmp(argv[i], "--head-gate")) head_gate = !strcmp(argv[i+1], "on");
        else if (!strcmp(argv[i], "--two-level")) two_level = !strcmp(argv[i+1], "on");
        else if (!strcmp(argv[i], "--line-fix")) /* API 48: off | on (VOCR_FIX_DEFAULT) | junk (+ JUNK) | all (+ VALLEY) */
            line_fix = !strcmp(argv[i+1], "all") ? (unsigned)(VOCR_FIX_DEFAULT | VOCR_FIX_JUNK | VOCR_FIX_VALLEY) :
                       !strcmp(argv[i+1], "junk") ? (unsigned)(VOCR_FIX_DEFAULT | VOCR_FIX_JUNK) :
                       !strcmp(argv[i+1], "on") ? (unsigned)VOCR_FIX_DEFAULT : 0u;
#endif
        else return usage();
    }
    if (argc - i != 4) return usage();
    Image im = {0};
    im.width = (unsigned)atoi(argv[i+1]); im.height = (unsigned)atoi(argv[i+2]);
    if (!im.width || im.width > VJO_OCR_MAX_WIDTH || !im.height || im.height > VJO_OCR_MAX_HEIGHT)
        return 2;
    size_t bytes = (size_t)im.width*im.height*4;
    im.pixels = malloc(bytes);
    FILE *f = fopen(argv[i+3], "rb");
    if (!im.pixels || !f || fread(im.pixels, 1, bytes, f) != bytes || fgetc(f) != EOF) {
        fprintf(stderr, "Invalid RGBA input\n"); return 2;
    }
    fclose(f);
    VjoPlatform platform;
    PosixPlatform pp = {0};
    posix_platform_init(&pp, &platform);
    VjoOcrImage input = {&im, im.width, im.height, rows, NULL};
    char text[VJO_OCR_TEXT_CAP];
    int rc;
    void *workspace = NULL;
    if (!strcmp(backend, "ncnn")) {
#ifdef VJO_WITH_NCNN
        VjoOcrStats stats;
        if (posix_memalign(&workspace, 64, VJO_OCR_HEAP_BYTES)) return 2;
        rc = vjo_local_ocr(&platform, argv[i], &input, workspace, VJO_OCR_HEAP_BYTES,
                           text, sizeof(text), &stats);
        fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu backend=ncnn workspace=%u\n", rc, stats.lines,
                stats.heap_peak, (unsigned)VJO_OCR_HEAP_BYTES);
#else
        return usage();
#endif
    } else if (!strcmp(backend, "vocr")) {
#ifdef VJO_WITH_VOCR
        VjoVocrStats st;
        if (!model) model = VJO_VOCR_DEFAULT_MODEL;
        size_t need = vjo_vocr_workspace_bytes(model, im.width, im.height);
        if (!need) {
            fprintf(stderr, "rc=%d lines=0 tensor_heap_peak=0 backend=vocr: unsupported model %s\n",
                    VJO_E_OCR_MODEL, model);
            return 1;
        }
        /* Exactly what the game worker allocates for this region, nothing more:
         * the workspace (here with the weights in front) and one capture strip. */
        size_t cap = vjo_vocr_capture_bytes(im.width, im.height);
        void *capture = malloc(cap);
        if (!capture || posix_memalign(&workspace, 64, need)) return 2;
        VjoRgbaImage rgba = {im.pixels, im.width, im.height, (size_t)im.width * 4};
        VjoGreySource grey;
        vjo_grey_source_rgba(&rgba, &grey);
        if (lines_path || blank || lm_name || cascade_name || refine || counters || head_gate || two_level || line_fix) {
            /* The game worker's form: resident weights, the job policy, the LM. */
            VjoVocrPolicy policy = {.blank = blank, .line = lines_path ? print_line : NULL,
                                    .refine = refine ? VJO_VOCR_REFINE_ALL : 0u, .short_lines = 1,
                                    .counter_tau = counters ? VJO_VOCR_COUNTER_TAU : 0,
                                    .counter_open = counters ? VJO_VOCR_COUNTER_OPEN : 0, .head_gate = head_gate,
                                    .two_level = two_level, .line_fix = line_fix};
            VjoVocrResident res = {resident_alloc, resident_release, NULL, NULL, NULL, 0, 0, 0};
            VjoVocrLmResident lres = {lm_alloc, lm_release, NULL, NULL, NULL, 0, 0, 0};
            FILE *lf = lines_path ? fopen(lines_path, "w") : NULL;
            size_t job = vjo_vocr_job_bytes(model, im.width, im.height) +
                         (refine ? vjo_vocr_refine_bytes(im.width, im.height) : 0);
            void *lws = NULL;
            if (lines_path && !lf) return 2;
            if (lm_name && !(policy.lm = vjo_vocr_lm(lm_name))) {
                fprintf(stderr, "unsupported LM %s\n", lm_name);
                return 2;
            }
            policy.lm_res = &lres;
            VjoVocrResident cres = {cascade_alloc, cascade_release, NULL, NULL, NULL, 0, 0, 0};
            if (cascade_name) {
                if (!(policy.cascade = vjo_vocr_model(cascade_name))) {
                    fprintf(stderr, "unsupported cascade model %s\n", cascade_name);
                    return 2;
                }
                policy.cascade_res = &cres;
                policy.cascade_us = cascade_ms * 1000u;
                policy.cascade_conf = cascade_conf;
            }
            if (policy.lm || policy.cascade || refine) {
                job += vjo_vocr_lm_job_bytes(model) + (policy.cascade ? vjo_vocr_cascade_job_bytes(cascade_name) : 0);
                if (posix_memalign(&lws, 64, job)) return 2;
            }
            policy.ud = lf;
            grey.line_buffer = line_buffer;
            rc = vjo_vocr_ocr_policy(&platform, argv[i], model, &grey, &res, lws ? lws : workspace, job, capture, cap,
                                     &policy, text, sizeof(text), &st);
            if (counters) fprintf(stderr, "counters=%d,%d counter_lines=%u\n", policy.counter_tau, policy.counter_open,
                                  st.counter_lines);
            if (head_gate) fprintf(stderr, "head_gate=on gated_frames=%u of %u rereads=%u\n", st.head_gated,
                                   st.frames + st.cascade_frames, st.head_rereads);
            if (two_level) fprintf(stderr, "two_level=on escaped_frames=%u of %u rereads=%u\n", st.head_escaped,
                                   st.frames + st.cascade_frames, st.head_rereads);
            if (policy.lm)
                fprintf(stderr, "lm=%s lm_rc=%d lm_lines=%u lm_changed=%u lm_us=%u lm_load_us=%u\n",
                        st.lm ? st.lm->name : "-", st.lm_rc, st.lm_lines, st.lm_changed, (unsigned)st.lm_us,
                        (unsigned)st.lm_load_us);
            if (refine)
                fprintf(stderr, "refine_boxes=%u refine_lines=%u refine_split=%u refine_capped=%u refine_rc=%d "
                        "refine_rows=%lu refine_us=%u\n", st.refine_boxes, st.refine_lines, st.refine_split,
                        st.refine_capped, st.refine_rc, st.refine_rows, (unsigned)st.refine_us);
            if (policy.cascade)
                fprintf(stderr, "cascade=%s cascade_rc=%d cascade_lines=%u cascade_changed=%u cascade_skipped=%u "
                        "cascade_us=%u cascade_load_us=%u\n", st.cascade ? st.cascade->name : "-", st.cascade_rc,
                        st.cascade_lines, st.cascade_changed, st.cascade_skipped, (unsigned)st.cascade_us,
                        (unsigned)st.cascade_load_us);
            vjo_vocr_resident_drop(&res);
            vjo_vocr_resident_drop(&cres);
            vjo_vocr_lm_drop(&lres);
            free(lws);
            if (lf) fclose(lf);
        } else {
            rc = vjo_vocr_ocr(&platform, argv[i], model, &grey, workspace, need, capture, cap, text, sizeof(text),
                              &st);
        }
        fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu backend=vocr model=%s workspace=%zu "
                "weights=%zu scratch=%zu capture=%zu strip_rows=%u strips=%u dark=%u truncated=%u frames=%u "
                "loaded=%u find_us=%u load_us=%u recognize_us=%u blank=%u blank_us=%u\n",
                rc, st.lines, st.workspace + st.capture, model, st.workspace, st.model_bytes, st.scratch_bytes,
                st.capture, st.strip_rows, st.strips, st.dark, st.truncated, st.frames, st.model_loaded,
                (unsigned)st.find_us, (unsigned)st.load_us, (unsigned)st.recognize_us, st.blank,
                (unsigned)st.blank_us);
        free(capture);
#else
        return usage();
#endif
    } else {
        return usage();
    }
    fprintf(stderr, "peak_rss=%lu\n", peak_rss());
    if (!rc) puts(text);
    free(workspace); free(im.pixels);
    return rc ? 1 : 0;
}
