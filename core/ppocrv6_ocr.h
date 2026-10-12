/* PP-OCRv6 small recognizer (ocr_backend = ppocrv6): PaddleOCR's CTC line
 * recognizer with its head pruned to JIS X 0208 + ASCII + full-width forms,
 * int8 in the pinned ncnn (tools/prepare_ppocrv6.py, third_party/ppocrv6).
 * Slow and manual only; it runs in the enabled game's process like VOCR.
 *
 * One job reads the selected region once, converts it to grey with the luma
 * VOCR uses, finds its lines with vita-vn-ocr's polarity-agnostic finder, then
 * streams the weights from the platform's file reader into ncnn (checking the
 * pinned size and SHA-256 before the first inference) and recognizes each line
 * at height 48, width at most 1024. Nothing outside the caller's workspace is
 * allocated: ncnn's tensors and objects live in its private heap. */
#ifndef VJO_PPOCRV6_OCR_H
#define VJO_PPOCRV6_OCR_H

#include <stddef.h>
#include <stdint.h>

#include "local_ocr.h" /* VjoOcrImage, VJO_OCR_MAX_WIDTH/HEIGHT */
#include "net.h"       /* VjoPlatform.file_open, now_us */

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_PPOCRV6_LINE_HEIGHT 48u
#define VJO_PPOCRV6_MAX_LINE_WIDTH 1024u /* resized width clamp: bounds activations */
/* ncnn's private heap: weights, the largest line's activations and ncnn's
 * objects. The Vita build's code reaches 15,331,520 bytes at the 1024-px
 * clamp, for one line or eleven (docs/ppocrv6.md); this leaves 23% spare. */
#define VJO_PPOCRV6_HEAP_BYTES (18u * 1024u * 1024u)

/* VjoPpocrModel.flags */
#define VJO_PPOCR_SINGLE_LINE 1u /* the image is one line crop: no line finder (eval) */
#define VJO_PPOCR_SGEMM 2u       /* ncnn im2col + GEMM int8 convolutions (else its direct kernels) */
#define VJO_PPOCR_WINOGRAD 4u    /* ncnn Winograd for 3x3 stride-1 convolutions */

/* A recognizer: the binary param, its classes and its weights' pin. The game
 * worker only uses vjo_ppocrv6_pinned(); host tools pass others (fp32 parity). */
typedef struct {
    const unsigned char *param_bin;
    size_t param_bytes;
    int input_blob, output_blob;
    const char *chars;          /* class i (1..count) = chars + offsets[i - 1], NUL-terminated */
    const uint16_t *offsets;
    unsigned class_count;       /* without the CTC blank */
    const char *weights_name;   /* file name in the model directory */
    uint32_t weights_bytes;
    const unsigned char *weights_sha256; /* NULL: unchecked (host experiments only) */
    unsigned flags;
    size_t heap_bytes;
} VjoPpocrModel;

typedef struct {
    size_t workspace;        /* bytes the job needs (vjo_ppocrv6_workspace_bytes) */
    size_t region_bytes;     /* the region in grey */
    size_t heap_bytes, heap_peak; /* ncnn's private heap and its high-water mark */
    size_t heap_extent;      /* highest heap offset reached (first fit: what the heap must hold) */
    unsigned width, height;  /* region */
    unsigned lines;          /* lines found (1 with VJO_PPOCR_SINGLE_LINE) */
    unsigned truncated;      /* more lines than the finder returns (16) */
    unsigned max_input_width;/* widest resized line */
    unsigned model_loaded;   /* 0 when no line was found: the file was not read */
    uint32_t find_us, load_us, recognize_us; /* with VjoPlatform.now_us, else 0 */
} VjoPpocrStats;

const VjoPpocrModel *vjo_ppocrv6_pinned(void);

/* Exact workspace bytes for a width x height region (0 if unsupported). */
size_t vjo_ppocrv6_workspace_bytes(unsigned width, unsigned height);
size_t vjo_ppocrv6_workspace_bytes_for(const VjoPpocrModel *model, unsigned width, unsigned height);

/* Recognizes the region: lines top to bottom, separated by '\n'. workspace:
 * 64-byte aligned, private to the call, at least vjo_ppocrv6_workspace_bytes.
 * image->rows is read exactly once per row, in order. Returns VJO_OK (also
 * with no line: text "" and no file read), VJO_E_OCR_MODEL (file missing,
 * wrong size or hash, or refused by ncnn), VJO_E_OOM (workspace or heap too
 * small), VJO_E_OCR_REGION, VJO_E_SOURCE, VJO_E_CANCELLED, VJO_E_TOO_LARGE or
 * VJO_E_OCR_INFERENCE. text is "" on error. Single caller at a time. */
int vjo_ppocrv6_ocr(const VjoPlatform *platform, const char *model_dir, const VjoOcrImage *image,
                    void *workspace, size_t workspace_size, char *text, size_t text_cap,
                    VjoPpocrStats *stats);
int vjo_ppocrv6_ocr_with(const VjoPpocrModel *model, const VjoPlatform *platform,
                         const char *model_dir, const VjoOcrImage *image,
                         void *workspace, size_t workspace_size, char *text, size_t text_cap,
                         VjoPpocrStats *stats);

#ifdef __cplusplus
}
#endif
#endif
