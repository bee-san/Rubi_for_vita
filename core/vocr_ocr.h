/* vita-vn-ocr backend: bee-san/vita-vn-ocr's polarity-agnostic dialogue line
 * finder and tiny CTC recognizer (plain C99, no malloc), with the int8 weights
 * published in its v0.2.0 release. Built with -DVJO_WITH_VOCR=ON; see
 * docs/vita-vn-ocr.md.
 *
 * One job reads the selected region once, converts it to grey, finds its lines
 * (any text/background polarity), then makes sure verified weights are in
 * memory and recognizes each line, inverting dark text as the model was
 * trained. The job's memory is one caller-provided workspace whose exact size
 * vjo_vocr_job_bytes() reports. The weights live in a separate caller-owned
 * block (VjoVocrResident) that is read and checked once and then kept for
 * later jobs while the file's size and stamp stay the same; nothing else is
 * allocated. vjo_vocr_ocr() is the one-shot form: weights and job share one
 * workspace (vjo_vocr_workspace_bytes()) and are read for every job. */
#ifndef VJO_VOCR_OCR_H
#define VJO_VOCR_OCR_H

#include <stddef.h>
#include <stdint.h>

#include "local_ocr.h" /* VjoOcrImage, VJO_OCR_MAX_WIDTH/HEIGHT, VJO_OCR_TEXT_CAP */
#include "net.h"       /* VjoPlatform.file_open, now_us */

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_VOCR_DEFAULT_MODEL "H15_w8.vocr"

/* A weights file this build accepts: exact size and SHA-256 of the release
 * asset, and the recognizer's arena (W8I mode, default tiling). */
typedef struct {
    const char *name;
    uint32_t bytes;
    unsigned char sha256[32];
    uint32_t arena_bytes;
    unsigned finder_mode;     /* vocr_lines.h: GRAD for old models, TEXTBOX for VOCR2 */
} VjoVocrModel;

/* The model named `name` (a file name in ocr_model_dir), or NULL. */
const VjoVocrModel *vjo_vocr_model(const char *name);
/* The supported models, for messages and tools; *count is set. */
const VjoVocrModel *vjo_vocr_models(unsigned *count);

typedef struct {
    size_t workspace;        /* bytes of this job's workspace (see vjo_vocr_ocr*) */
    size_t model_bytes;      /* weights, used in place */
    size_t scratch_bytes;    /* the finder's arena, then the recognizer's */
    size_t region_bytes;     /* the region in grey, one byte per pixel */
    unsigned width, height;  /* region */
    unsigned lines;          /* lines found */
    unsigned dark;           /* of those, dark text (inverted before recognition) */
    unsigned truncated;      /* more lines than the finder returns (16) */
    unsigned frames;         /* CTC frames recognized */
    unsigned model_loaded;   /* 1: the weights were read and verified in this job */
    unsigned model_cached;   /* 1: resident verified weights were reused (file unchanged) */
    uint32_t find_us, load_us, recognize_us; /* with VjoPlatform.now_us, else 0 */
} VjoVocrStats;

/* Weights kept between jobs. The caller supplies their memory: alloc returns
 * 16-byte aligned memory for `bytes`, or NULL (the job then fails with
 * VJO_E_OOM); release frees it and returns 0, or nonzero if it could not (the
 * memory then stays held but is never used as weights). Zero-initialize the
 * state fields; vocr_ocr.c maintains them. Between jobs, data is non-NULL
 * exactly when model is: memory is held only for verified weights. */
typedef struct {
    void *(*alloc)(void *ud, size_t bytes);
    int (*release)(void *ud);
    void *ud;
    const VjoVocrModel *model;  /* verified weights of this model are in data, else NULL */
    unsigned char *data;        /* the memory held, or NULL */
    size_t capacity;            /* bytes of data */
    uint64_t size, stamp;       /* VjoFile.size and .stamp of the file they were read from */
} VjoVocrResident;

/* Releases the resident weights. 0, or the release callback's failure (the
 * memory is then still held, and model is NULL). */
int vjo_vocr_resident_drop(VjoVocrResident *resident);

/* Exact workspace bytes for a job on a width x height region with `model`:
 * vjo_vocr_job_bytes() without the weights (resident form), or
 * vjo_vocr_workspace_bytes() with them (one-shot form); 0 for an unsupported
 * model or region. */
size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height);
size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height);

/* Recognizes the region: lines top to bottom, separated by '\n', in text (at
 * most text_cap bytes with the NUL). workspace: 16-byte aligned, private to
 * the call, at least vjo_vocr_job_bytes(). image->rows returns RGBA rows
 * (A8B8G8R8 bytes R, G, B, A) and is read exactly once per row, in order.
 *
 * With at least one line, the model file is opened. Weights already resident
 * for this model whose file still reports the same size and a nonzero, equal
 * stamp are used as they are (stats->model_cached); otherwise the file is read
 * into the resident memory (allocated if needed, the old model's released
 * first) and checked against the pinned size and SHA-256 (stats->model_loaded).
 * A missing, wrong or unreadable file, or any failure while loading, releases
 * the resident weights; cancellation during the check keeps verified ones.
 *
 * Returns VJO_OK (also with no line: text "" and no file opened),
 * VJO_E_OCR_MODEL (file missing, wrong size or hash, or refused by the
 * runtime), VJO_E_OOM (workspace too small, or no memory for the weights),
 * VJO_E_OCR_REGION, VJO_E_SOURCE (row read), VJO_E_CANCELLED, VJO_E_TOO_LARGE
 * (text), VJO_E_OCR_INFERENCE, or VJO_E_OCR_UNAVAILABLE (resident memory could
 * not be released). text is "" on error. */
int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoOcrImage *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size,
                          char *text, size_t text_cap, VjoVocrStats *stats);

/* The one-shot form: the weights are read into the front of workspace (at
 * least vjo_vocr_workspace_bytes()) for this job only, as the host tools do. */
int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoOcrImage *image, void *workspace, size_t workspace_size,
                 char *text, size_t text_cap, VjoVocrStats *stats);

#ifdef __cplusplus
}
#endif
#endif
