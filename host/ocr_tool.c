/* Diagnostic runner for the bounded local recognizers.
 * Input: tightly packed RGBA8 (no image-decoder dependency).
 *
 *   vjo-ocr [--backend ncnn|vocr|ppocrv6] [--model FILE] MODEL_DIR WIDTH HEIGHT INPUT.rgba
 *   vjo-ocr --backend ppocrv6 --lines MODEL_DIR < crop paths (binary PPM, one per line)
 *
 * stderr gets one line, `rc=... lines=... tensor_heap_peak=...`: the peak bytes
 * used inside the job's workspace (ncnn: its tensor heap in the 64 MiB block;
 * vocr: the exact workspace, all of which the job uses; ppocrv6: ncnn's private
 * heap peak), then backend details.
 *
 * --lines is vita-vn-ocr eval/run.py's cmd: protocol: every crop runs as one
 * complete ppocrv6 job (weights read and checked, one line, no line finder),
 * printing exactly one UTF-8 line of text per crop. Host experiments may
 * substitute another graph: --ppocr-param FILE.bin:INPUT:OUTPUT (from
 * tools/prepare_ppocrv6.py param-bin), --ppocr-dict FILE, --ppocr-weights NAME
 * (in MODEL_DIR, size taken from the file and its SHA-256 unchecked) and
 * --ppocr-heap BYTES and --ppocr-flags N (VJO_PPOCR_SGEMM 2, VJO_PPOCR_WINOGRAD 4). With VJO_STACK_PROBE=1 each PP-OCRv6 job runs on a thread
 * with a painted 1 MiB stack and stderr reports its high-water mark
 * (stack_peak=), the figure the game worker's job thread stack is sized from. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef VJO_WITH_PPOCRV6
#include <pthread.h>
#endif
#include "local_ocr.h"
#include "net_posix.h"
#ifdef VJO_WITH_VOCR
#include "vocr_ocr.h"
#endif
#ifdef VJO_WITH_PPOCRV6
#include "ppocrv6_ocr.h"
#endif

typedef struct { unsigned char *pixels; unsigned width, height; } Image;
static int rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    Image *im = ud;
    if (y > im->height || n > im->height-y) return -1;
    memcpy(dst, im->pixels+(size_t)y*im->width*4, (size_t)n*im->width*4);
    return (int)n;
}

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
    fprintf(stderr, "Usage: vjo-ocr [--backend ncnn|vocr|ppocrv6] [--model FILE] MODEL_DIR WIDTH HEIGHT INPUT.rgba\n"
                    "       vjo-ocr --backend ppocrv6 --lines MODEL_DIR < crop paths\n"
                    "backends built:"
#ifdef VJO_WITH_NCNN
                    " ncnn"
#endif
#ifdef VJO_WITH_VOCR
                    " vocr (--model default " VJO_VOCR_DEFAULT_MODEL ")"
#endif
#ifdef VJO_WITH_PPOCRV6
                    " ppocrv6"
#endif
                    "\n");
    return 2;
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data) { data[n] = 0; *size = (size_t)n; }
    return data;
}

/* Binary PPM (P6, maxval 255) to RGBA. */
static int read_ppm(const char *path, Image *im)
{
    size_t size = 0;
    unsigned char *data = read_file(path, &size);
    unsigned w, h, maxval;
    int at = 0;
    if (!data || sscanf((char *)data, "P6 %u %u %u%n", &w, &h, &maxval, &at) != 3 || maxval != 255 ||
        !w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT ||
        size < (size_t)at + 1 + (size_t)w * h * 3) { free(data); return -1; }
    const unsigned char *p = data + at + 1;
    im->pixels = malloc((size_t)w * h * 4);
    if (!im->pixels) { free(data); return -1; }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        im->pixels[i*4] = p[i*3]; im->pixels[i*4+1] = p[i*3+1]; im->pixels[i*4+2] = p[i*3+2];
        im->pixels[i*4+3] = 255;
    }
    im->width = w; im->height = h;
    free(data);
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#ifdef VJO_WITH_PPOCRV6
/* An experiment graph (see the header comment); the pinned model otherwise. */
static int ppocr_model(VjoPpocrModel *m, const char *param, const char *dict, const char *weights,
                       const char *dir, size_t heap)
{
    *m = *vjo_ppocrv6_pinned();
    if (param) {
        char path[512];
        int in = -1, out = -1;
        const char *colon = strchr(param, ':');
        if (!colon || (size_t)(colon - param) >= sizeof(path) || sscanf(colon + 1, "%d:%d", &in, &out) != 2)
            return -1;
        memcpy(path, param, (size_t)(colon - param)); path[colon - param] = 0;
        size_t n = 0;
        m->param_bin = read_file(path, &n);
        if (!m->param_bin) return -1;
        m->param_bytes = n; m->input_blob = in; m->output_blob = out;
    }
    if (dict) {
        size_t n = 0;
        char *chars = (char *)read_file(dict, &n);
        if (!chars) return -1;
        uint16_t *offsets = malloc(sizeof(uint16_t) * (n + 1));
        unsigned count = 0;
        for (size_t start = 0, i = 0; i < n; i++)
            if (chars[i] == '\n') { chars[i] = 0; offsets[count++] = (uint16_t)start; start = i + 1; }
        m->chars = chars; m->offsets = offsets; m->class_count = count;
    }
    if (weights) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, weights);
        FILE *f = fopen(path, "rb");
        if (!f) return -1;
        fseek(f, 0, SEEK_END);
        m->weights_bytes = (uint32_t)ftell(f);
        fclose(f);
        m->weights_name = weights;
        m->weights_sha256 = NULL;
    }
    if (heap) m->heap_bytes = heap;
    return 0;
}

typedef struct {
    const VjoPpocrModel *model; const VjoPlatform *platform; const char *dir; const VjoOcrImage *image;
    void *workspace; size_t size; char *text; size_t cap; VjoPpocrStats *stats; int rc;
} PpocrJob;
static size_t stack_peak;

static void *ppocr_job(void *p)
{
    PpocrJob *j = p;
    j->rc = vjo_ppocrv6_ocr_with(j->model, j->platform, j->dir, j->image, j->workspace, j->size,
                                 j->text, j->cap, j->stats);
    return NULL;
}

/* One job, optionally on a painted stack (VJO_STACK_PROBE=1). */
static int run_ppocr(const VjoPpocrModel *m, const VjoPlatform *platform, const char *dir,
                     const VjoOcrImage *im, void *workspace, size_t size, char *text, size_t cap,
                     VjoPpocrStats *st)
{
    PpocrJob job = {m, platform, dir, im, workspace, size, text, cap, st, VJO_E_SOURCE};
    const char *probe = getenv("VJO_STACK_PROBE");
    if (!probe || strcmp(probe, "1")) { ppocr_job(&job); return job.rc; }
    enum { STACK = 1u << 20 };
    unsigned char *stack = NULL;
    if (posix_memalign((void **)&stack, 4096, STACK)) return VJO_E_OOM;
    memset(stack, 0xA5, STACK);
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, STACK);
    if (pthread_create(&thread, &attr, ppocr_job, &job) || pthread_join(thread, NULL)) job.rc = VJO_E_SOURCE;
    pthread_attr_destroy(&attr);
    size_t untouched = 0;
    while (untouched < STACK && stack[untouched] == 0xA5) untouched++;
    if (STACK - untouched > stack_peak) stack_peak = STACK - untouched;
    free(stack);
    return job.rc;
}

static int ppocr_lines(VjoPpocrModel *m, VjoPlatform *platform, const char *dir)
{
    char path[4096], text[VJO_OCR_TEXT_CAP];
    m->flags |= VJO_PPOCR_SINGLE_LINE;
    size_t peak = 0, extent = 0, cap = vjo_ppocrv6_workspace_bytes_for(m, VJO_OCR_MAX_WIDTH, VJO_OCR_MAX_HEIGHT);
    void *workspace = NULL;
    unsigned crops = 0, failed = 0, widest = 0;
    double busy = 0;
    if (!cap || posix_memalign(&workspace, 64, cap)) return 2;
    while (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\r\n")] = 0;
        if (!path[0]) continue;
        Image im = {0};
        if (read_ppm(path, &im)) { fprintf(stderr, "%s: not a binary PPM\n", path); return 2; }
        VjoOcrImage input = {&im, im.width, im.height, rows, NULL};
        VjoPpocrStats st;
        double t0 = now_s();
        int rc = run_ppocr(m, platform, dir, &input, workspace,
                           vjo_ppocrv6_workspace_bytes_for(m, im.width, im.height), text, sizeof(text), &st);
        busy += now_s() - t0;
        if (rc) { failed++; text[0] = 0; fprintf(stderr, "%s: rc=%d\n", path, rc); }
        if (st.heap_peak > peak) peak = st.heap_peak;
        if (st.heap_extent > extent) extent = st.heap_extent;
        if (st.max_input_width > widest) widest = st.max_input_width;
        for (char *c = text; *c; c++) if (*c == '\n') *c = ' ';
        puts(text);
        fflush(stdout);
        crops++;
        free(im.pixels);
    }
    fprintf(stderr, "crops=%u failed=%u heap_peak_max=%zu heap_extent_max=%zu widest=%u ms_per_crop=%.1f "
            "peak_rss=%lu stack_peak=%zu\n", crops, failed, peak, extent, widest,
            crops ? busy * 1000 / crops : 0.0, peak_rss(), stack_peak);
    free(workspace);
    return failed ? 1 : 0;
}
#endif

int main(int argc, char **argv)
{
#ifdef VJO_WITH_NCNN
    const char *backend = "ncnn";
#else
    const char *backend = "vocr";
#endif
    const char *model = NULL, *ppocr_param = NULL, *ppocr_dict = NULL, *ppocr_weights = NULL;
    size_t ppocr_heap = 0;
    int ppocr_flags = -1; /* VJO_PPOCR_SGEMM | VJO_PPOCR_WINOGRAD: kernel experiments */
    int i = 1, lines = 0;
    for (; i < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (!strcmp(argv[i], "--lines")) { lines = 1; i--; continue; }
        if (i + 1 >= argc) return usage();
        if (!strcmp(argv[i], "--backend")) backend = argv[i+1];
        else if (!strcmp(argv[i], "--model")) model = argv[i+1];
        else if (!strcmp(argv[i], "--ppocr-param")) ppocr_param = argv[i+1];
        else if (!strcmp(argv[i], "--ppocr-dict")) ppocr_dict = argv[i+1];
        else if (!strcmp(argv[i], "--ppocr-weights")) ppocr_weights = argv[i+1];
        else if (!strcmp(argv[i], "--ppocr-heap")) ppocr_heap = (size_t)strtoull(argv[i+1], NULL, 0);
        else if (!strcmp(argv[i], "--ppocr-flags")) ppocr_flags = (int)strtol(argv[i+1], NULL, 0);
        else return usage();
    }
    VjoPlatform platform;
    PosixPlatform pp = {0};
    posix_platform_init(&pp, &platform);
#ifdef VJO_WITH_PPOCRV6
    VjoPpocrModel pm;
    if (!strcmp(backend, "ppocrv6") && (argc - i == 1 || argc - i == 4) &&
        ppocr_model(&pm, ppocr_param, ppocr_dict, ppocr_weights, argv[i], ppocr_heap)) {
        fprintf(stderr, "Invalid --ppocr-* option\n");
        return 2;
    }
    if (!strcmp(backend, "ppocrv6") && ppocr_flags >= 0)
        pm.flags = (pm.flags & VJO_PPOCR_SINGLE_LINE) | ((unsigned)ppocr_flags & (VJO_PPOCR_SGEMM | VJO_PPOCR_WINOGRAD));
    if (lines) {
        if (strcmp(backend, "ppocrv6") || argc - i != 1) return usage();
        return ppocr_lines(&pm, &platform, argv[i]);
    }
#endif
    if (lines || argc - i != 4) return usage();
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
        /* Exactly what the Shell allocates for this region, nothing more. */
        if (posix_memalign(&workspace, 64, need)) return 2;
        rc = vjo_vocr_ocr(&platform, argv[i], model, &input, workspace, need, text, sizeof(text), &st);
        fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu backend=vocr model=%s workspace=%zu "
                "weights=%zu scratch=%zu region=%zu dark=%u truncated=%u frames=%u loaded=%u "
                "find_us=%u load_us=%u recognize_us=%u\n",
                rc, st.lines, st.workspace, model, st.workspace, st.model_bytes, st.scratch_bytes,
                st.region_bytes, st.dark, st.truncated, st.frames, st.model_loaded,
                (unsigned)st.find_us, (unsigned)st.load_us, (unsigned)st.recognize_us);
#else
        return usage();
#endif
    } else if (!strcmp(backend, "ppocrv6")) {
#ifdef VJO_WITH_PPOCRV6
        VjoPpocrStats st;
        size_t need = vjo_ppocrv6_workspace_bytes_for(&pm, im.width, im.height);
        /* Exactly what the game worker allocates for this region. */
        if (!need || posix_memalign(&workspace, 64, need)) return 2;
        double t0 = now_s();
        rc = run_ppocr(&pm, &platform, argv[i], &input, workspace, need, text, sizeof(text), &st);
        fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu backend=ppocrv6 workspace=%zu heap=%zu "
                "region=%zu truncated=%u widest=%u loaded=%u find_us=%u load_us=%u recognize_us=%u "
                "wall_ms=%.1f stack_peak=%zu heap_extent=%zu\n",
                rc, st.lines, st.heap_peak, st.workspace, st.heap_bytes, st.region_bytes, st.truncated,
                st.max_input_width, st.model_loaded, (unsigned)st.find_us, (unsigned)st.load_us,
                (unsigned)st.recognize_us, (now_s() - t0) * 1000, stack_peak, st.heap_extent);
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
