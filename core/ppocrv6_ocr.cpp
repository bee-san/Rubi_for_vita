/* PP-OCRv6 small backend (ppocrv6_ocr.h): region -> grey -> vocr_find_lines ->
 * ncnn per line, in one caller workspace. ncnn is the pinned commit of
 * cmake/Ncnn.cmake with only this graph's layers; every allocation it makes
 * (tensors via malloc/posix_memalign, objects via operator new) is served by
 * core/ocr_heap.c from this job's workspace. */
#include "ppocrv6_ocr.h"
#include "ocr_heap.h"
#include "../include/vjo_vocr_sizes.h"
#include <net.h>
#include <datareader.h>
#include <mat.h>
#include <bearssl_hash.h>
#include <string.h>
#include "vocr_lines.h"
#include "vjo_ppocrv6_data.h"

static const VjoPpocrModel pinned = {
    vjo_ppocrv6_param, sizeof(vjo_ppocrv6_param), vjo_ppocrv6_input, vjo_ppocrv6_output,
    (const char *)vjo_ppocrv6_chars, vjo_ppocrv6_offsets, VJO_PPOCRV6_CLASSES,
    VJO_PPOCRV6_MODEL_NAME, VJO_PPOCRV6_MODEL_BYTES, vjo_ppocrv6_weights_sha256,
    VJO_PPOCR_SGEMM, VJO_PPOCRV6_HEAP_BYTES,
};
typedef char pinned_size_matches[VJO_PPOCRV6_MODEL_BYTES == VJO_PPOCRV6_WEIGHTS_BYTES ? 1 : -1];

extern "C" const VjoPpocrModel *vjo_ppocrv6_pinned(void) { return &pinned; }

#define ALIGN64(x) (((x) + (size_t)63) & ~(size_t)63)

/* Workspace: [grey region | heap]. The line finder's arena is the start of
 * the heap's bytes, before the heap begins. */
extern "C" size_t vjo_ppocrv6_workspace_bytes_for(const VjoPpocrModel *m, unsigned w, unsigned h)
{
    if (!m || !w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT) return 0;
    size_t heap = m->heap_bytes;
    if ((m->flags & VJO_PPOCR_SINGLE_LINE) == 0 && heap < vocr_lines_arena_bytes(w, h)) return 0;
    return ALIGN64((size_t)w * h) + ALIGN64(heap);
}

extern "C" size_t vjo_ppocrv6_workspace_bytes(unsigned w, unsigned h)
{
    return vjo_ppocrv6_workspace_bytes_for(&pinned, w, h);
}

static int cancelled(const VjoOcrImage *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static uint32_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? (uint32_t)p->now_us(p->ud) : 0;
}

/* All rows, once each and in order, as grey = (77 R + 150 G + 29 B) >> 8 (the
 * console's luma, also what VOCR reads). The heap's bytes hold the RGBA rows. */
static int read_grey(const VjoOcrImage *im, unsigned char *rgba, size_t cap, unsigned char *grey)
{
    const unsigned w = im->width, batch = (unsigned)(cap / ((size_t)w * 4));
    if (!batch) return VJO_E_OOM;
    for (unsigned y = 0; y < im->height;) {
        unsigned n = im->height - y < batch ? im->height - y : batch;
        if (cancelled(im)) return VJO_E_CANCELLED;
        int got = im->rows(im->ud, y, n, rgba);
        if (got <= 0 || (unsigned)got > n) return VJO_E_SOURCE;
        for (unsigned r = 0; r < (unsigned)got; r++) {
            const unsigned char *s = rgba + (size_t)r * w * 4;
            unsigned char *d = grey + (size_t)(y + r) * w;
            for (unsigned x = 0; x < w; x++, s += 4)
                d[x] = (unsigned char)((77u * s[0] + 150u * s[1] + 29u * s[2]) >> 8);
        }
        y += (unsigned)got;
    }
    return VJO_OK;
}

/* Streams the weights into ncnn straight from the file: no copy of the file
 * is held. Every byte is hashed; the caller rejects the model unless the
 * whole pinned file was consumed with the pinned SHA-256. */
class StreamReader : public ncnn::DataReader {
public:
    StreamReader(VjoFile *f, const VjoOcrImage *im) : file(f), image(im), offset(0), error(VJO_OK)
    {
        br_sha256_init(&hash);
    }
    virtual size_t read(void *buf, size_t size) const
    {
        if (error || !size) return 0;
        if (cancelled(image)) { error = VJO_E_CANCELLED; return 0; }
        if (size > file->size - offset || file->read(file->ctx, offset, buf, size) < 0) {
            error = VJO_E_OCR_MODEL;
            return 0;
        }
        br_sha256_update(&hash, buf, size);
        offset += size;
        return size;
    }
    virtual size_t reference(size_t, const void **) const { return 0; }
    VjoFile *file;
    const VjoOcrImage *image;
    mutable uint64_t offset;
    mutable int error;
    mutable br_sha256_context hash;
};

static int open_weights(const VjoPlatform *p, const char *dir, const VjoPpocrModel *m, VjoFile *file)
{
    char path[256];
    size_t dn = strlen(dir), fn = strlen(m->weights_name);
    if (!dn || dn + 1 + fn >= sizeof(path)) return VJO_E_OCR_MODEL;
    memcpy(path, dir, dn);
    path[dn] = '/';
    memcpy(path + dn + 1, m->weights_name, fn + 1);
    memset(file, 0, sizeof(*file));
    if (!p->file_open || p->file_open(p->ud, path, file) < 0) return VJO_E_OCR_MODEL;
    if (file->size != m->weights_bytes || !file->read || !file->close) {
        if (file->close) file->close(file->ctx);
        memset(file, 0, sizeof(*file));
        return VJO_E_OCR_MODEL;
    }
    return VJO_OK;
}

static int ncnn_error(void)
{
    return vjo_ocr_heap_failed() ? VJO_E_OOM : VJO_E_OCR_INFERENCE;
}

/* One line crop of the grey region: bilinear to height 48 (ncnn's own
 * OpenCV-compatible resize), the same grey in B, G and R, (p - 127.5) / 127.5. */
static int line_input(const unsigned char *grey, size_t stride, const VocrLine *ln, ncnn::Mat &in,
                      unsigned *width)
{
    unsigned tw = (ln->w * VJO_PPOCRV6_LINE_HEIGHT + ln->h / 2) / ln->h;
    if (tw < 8) tw = 8;
    if (tw > VJO_PPOCRV6_MAX_LINE_WIDTH) tw = VJO_PPOCRV6_MAX_LINE_WIDTH;
    *width = tw;
    ncnn::Mat small((int)tw, (int)VJO_PPOCRV6_LINE_HEIGHT, (size_t)1u, 1);
    in.create((int)tw, (int)VJO_PPOCRV6_LINE_HEIGHT, 3, (size_t)4u, 1);
    if (small.empty() || in.empty()) return VJO_E_OOM;
    const unsigned char *src = grey + (size_t)ln->y * stride + ln->x;
    if (ln->w == tw && ln->h == VJO_PPOCRV6_LINE_HEIGHT) {
        for (unsigned y = 0; y < VJO_PPOCRV6_LINE_HEIGHT; y++)
            memcpy((unsigned char *)small.data + (size_t)y * tw, src + (size_t)y * stride, tw);
    } else {
        ncnn::resize_bilinear_c1(src, (int)ln->w, (int)ln->h, (int)stride,
                                 (unsigned char *)small.data, (int)tw, (int)VJO_PPOCRV6_LINE_HEIGHT, (int)tw);
        if (vjo_ocr_heap_failed()) return VJO_E_OOM;
    }
    const unsigned char *s = (const unsigned char *)small.data;
    const size_t n = (size_t)tw * VJO_PPOCRV6_LINE_HEIGHT;
    float *b = in.channel(0), *g = in.channel(1), *r = in.channel(2);
    for (size_t i = 0; i < n; i++)
        b[i] = g[i] = r[i] = ((float)s[i] - 127.5f) / 127.5f;
    return VJO_OK;
}

/* Greedy CTC over one line's logits [T x (class_count + 1)], class 0 = blank. */
static int decode(const VjoPpocrModel *m, const ncnn::Mat &out, char *text, size_t cap, size_t *used)
{
    const unsigned classes = m->class_count + 1;
    if (out.dims != 2 || out.w != (int)classes || out.h <= 0 ||
        out.h > (int)VJO_PPOCRV6_MAX_LINE_WIDTH || out.elempack != 1 || out.elemsize != sizeof(float))
        return VJO_E_OCR_INFERENCE;
    unsigned last = 0;
    for (int t = 0; t < out.h; t++) {
        const float *scores = out.row(t);
        unsigned token = 0;
        for (unsigned i = 1; i < classes; i++)
            if (scores[i] > scores[token]) token = i;
        if (token && token != last) {
            const char *s = m->chars + m->offsets[token - 1];
            size_t n = strlen(s);
            if (n >= cap - *used) return VJO_E_TOO_LARGE;
            memcpy(text + *used, s, n);
            *used += n;
            text[*used] = '\0';
        }
        last = token;
    }
    return VJO_OK;
}

/* Runs with ncnn's heap active; every ncnn object is destroyed on return. */
static int recognize_lines(const VjoPpocrModel *m, const VjoPlatform *p, const char *dir,
                           const VjoOcrImage *im, const unsigned char *grey, const VocrLineSet *set,
                           char *text, size_t cap, VjoPpocrStats *st)
{
    VjoFile file;
    uint32_t t0 = clock_us(p);
    int rc = open_weights(p, dir, m, &file);
    if (rc) return rc;
    ncnn::Net net;
    net.opt.use_vulkan_compute = false;
    net.opt.num_threads = 1;
    net.opt.lightmode = true;
    net.opt.use_local_pool_allocator = false;
    net.opt.use_fp16_arithmetic = false;
    net.opt.use_fp16_storage = false;
    net.opt.use_fp16_packed = false;
    net.opt.use_bf16_storage = false;
    net.opt.use_winograd_convolution = (m->flags & VJO_PPOCR_WINOGRAD) != 0;
    net.opt.use_sgemm_convolution = (m->flags & VJO_PPOCR_SGEMM) != 0;
    net.opt.use_int8_inference = true;
    {
        const unsigned char *param_cursor = m->param_bin; /* advanced by the reader */
        ncnn::DataReaderFromMemory param(param_cursor);
        StreamReader weights(&file, im);
        int load = net.load_param_bin(param);
        if (!load) load = net.load_model(weights);
        file.close(file.ctx);
        st->model_loaded = 1;
        if (weights.error) return weights.error;
        if (load) return ncnn_error();
        if (weights.offset != m->weights_bytes) return VJO_E_OCR_MODEL;
        unsigned char digest[32];
        br_sha256_out(&weights.hash, digest);
        if (m->weights_sha256 && memcmp(digest, m->weights_sha256, sizeof(digest))) return VJO_E_OCR_MODEL;
    }
    st->load_us = clock_us(p) - t0;
    t0 = clock_us(p);
    size_t used = 0;
    for (unsigned i = 0; i < set->count; i++) {
        const VocrLine *ln = &set->lines[i];
        size_t start = used ? used + 1 : 0;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (start + 1 >= cap) return VJO_E_TOO_LARGE;
        unsigned width = 0;
        ncnn::Mat out;
        {
            ncnn::Mat in;
            rc = line_input(grey, im->width, ln, in, &width);
            if (rc) return rc;
            if (width > st->max_input_width) st->max_input_width = width;
            ncnn::Extractor ex = net.create_extractor();
            if (ex.input(m->input_blob, in) || ex.extract(m->output_blob, out)) return ncnn_error();
        }
        size_t n = start;
        rc = decode(m, out, text, cap, &n);
        if (rc) return rc;
        if (n == start) { /* nothing read on this line: no separator either */
            text[used] = '\0';
            continue;
        }
        if (start) text[used] = '\n';
        used = n;
    }
    st->recognize_us = clock_us(p) - t0;
    return VJO_OK;
}

extern "C" int vjo_ppocrv6_ocr_with(const VjoPpocrModel *m, const VjoPlatform *p, const char *dir,
                                    const VjoOcrImage *im, void *workspace, size_t size,
                                    char *text, size_t cap, VjoPpocrStats *st)
{
    if (!m || !p || !dir || !im || !text || !cap || !st) return VJO_E_SOURCE;
    text[0] = '\0';
    memset(st, 0, sizeof(*st));
    size_t need = vjo_ppocrv6_workspace_bytes_for(m, im->width, im->height);
    if (!im->rows || !need) return VJO_E_OCR_REGION;
    const size_t region = ALIGN64((size_t)im->width * im->height);
    st->workspace = need;
    st->region_bytes = (size_t)im->width * im->height;
    st->heap_bytes = ALIGN64(m->heap_bytes);
    st->width = im->width;
    st->height = im->height;
    if (cancelled(im)) return VJO_E_CANCELLED;
    if (!workspace || ((uintptr_t)workspace & 63u) || size < need) return VJO_E_OOM;
    unsigned char *grey = (unsigned char *)workspace;
    unsigned char *heap = grey + region;
    uint32_t t0 = clock_us(p);
    int rc = read_grey(im, heap, st->heap_bytes, grey);
    if (rc) return rc;
    VocrLineSet set;
    memset(&set, 0, sizeof(set));
    if (m->flags & VJO_PPOCR_SINGLE_LINE) {
        set.count = 1;
        set.lines[0].w = im->width;
        set.lines[0].h = im->height;
    } else {
        VocrLinesImage region_image = {grey, im->width, im->height, im->width, VOCR_LINES_GREY};
        rc = vocr_find_lines(&region_image, VOCR_LINES_GRAD, heap, st->heap_bytes, &set);
        if (rc) return rc == VOCR_LINES_E_ARENA ? VJO_E_OOM : VJO_E_OCR_REGION;
    }
    st->find_us = clock_us(p) - t0;
    st->lines = set.count;
    st->truncated = set.truncated;
    if (!set.count) return VJO_OK; /* nothing to read: the weights stay on the card */
    if (cancelled(im)) return VJO_E_CANCELLED;
    if (vjo_ocr_heap_begin(heap, st->heap_bytes) < 0) return VJO_E_OCR_UNAVAILABLE;
    rc = recognize_lines(m, p, dir, im, grey, &set, text, cap, st);
    st->heap_peak = vjo_ocr_heap_end();
    st->heap_extent = vjo_ocr_heap_extent();
    if (rc) text[0] = '\0'; /* never publish a partial recognition */
    return rc;
}

extern "C" int vjo_ppocrv6_ocr(const VjoPlatform *p, const char *dir, const VjoOcrImage *im,
                               void *workspace, size_t size, char *text, size_t cap, VjoPpocrStats *st)
{
    return vjo_ppocrv6_ocr_with(&pinned, p, dir, im, workspace, size, text, cap, st);
}
