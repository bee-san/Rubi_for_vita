/* vita-vn-ocr backend: bee-san/vita-vn-ocr's polarity-agnostic dialogue line
 * finder and tiny CTC recognizer (plain C99, no malloc), with the int8 weights
 * published in its v0.2.0 release. Built with -DVJO_WITH_VOCR=ON; see
 * docs/vita-vn-ocr.md.
 *
 * API 19: a job never holds the region. It reads the region in strips of grey
 * rows (VjoGreySource: in the game, the display hook copies each strip from the
 * game's framebuffer), finds the lines with the runtime's row-callback finder
 * (vocr_lines_rows.h) and recognizes each line straight from the strip that
 * holds it. Its memory is a workspace (the finder's arena, then the
 * recognizer's, and the runtime's two structs: vjo_vocr_job_bytes) and a
 * capture block (one strip, the rows' checksums: vjo_vocr_capture_bytes, at
 * most VJO_VOCR_CAPTURE_MAX). Every row's checksum is kept the first time it is
 * read and compared whenever it is read again; a row that changed means the
 * screen changed while it was read, and the job starts over (VJO_VOCR_TRIES in
 * all, then VJO_E_SCREEN_CHANGED). The weights live in a separate caller-owned
 * block (VjoVocrResident) that is read and checked once and then kept for later
 * jobs while the file's size and stamp stay the same; nothing else is
 * allocated. vjo_vocr_ocr() is the one-shot form: weights and job share one
 * workspace (vjo_vocr_workspace_bytes()) and are read for every job. */
#ifndef VJO_VOCR_OCR_H
#define VJO_VOCR_OCR_H

#include <stddef.h>
#include <stdint.h>

#include "local_ocr.h" /* VJO_OCR_MAX_WIDTH/HEIGHT, VJO_OCR_TEXT_CAP */
#include "net.h"       /* VjoPlatform.file_open, now_us */

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_VOCR_DEFAULT_MODEL "H15_w8.vocr"

/* The capture block: one strip of at most this many bytes of grey rows (the
 * strip is region width x VJO_VOCR_STRIP_ROWS(width) rows), plus 4 bytes of
 * checksum and one bit per region row; page-rounded it stays under 64 KiB. */
#define VJO_VOCR_STRIP_BYTES (56u * 1024u)
#define VJO_VOCR_STRIP_MAX_ROWS 64u /* the kernel copies at most this many rows per call */
#define VJO_VOCR_TRIES 3u           /* captures of one job before VJO_E_SCREEN_CHANGED */
/* A line taller than a strip is read whole into VjoGreySource.line_buffer: region
 * width x line height bytes, up to this many (include/vjo_ledger_budget.h
 * VJO_BUDGET_GAME_LINE), so the recognizer gets the same input as from a copy of
 * the whole region. Dialogue lines are 20-53 px on the shared eval screenshots,
 * inside one strip; the finder can report a far taller band (several lines it
 * could not separate, a picture), up to the region's height. A larger one (only
 * a full-width band of more than 537 rows) is left out and counted. */
#define VJO_VOCR_LINE_MAX_BYTES 516096u

/* A region read in strips of grey rows: strip(ud, y, n, dst, stride, flags)
 * fills region rows [y, y + n) as (77 R + 150 G + 29 B) >> 8, width bytes each,
 * stride apart, and returns n, or a negative VJO_E_* / native error (the job
 * then fails with it; VJO_E_CANCELLED for a cancelled one). flags:
 * VJO_STRIP_FIRST when the rows continue the job's first pass from row 0, in
 * order, and VJO_STRIP_LAST on the one that ends it. line_buffer(ud, bytes),
 * optional, gives memory for one line taller than a strip (valid until the next
 * call or the end of the job), or NULL: that line is then left out. */
#define VJO_STRIP_FIRST 0x1u
#define VJO_STRIP_LAST 0x2u
typedef struct {
    void *ud;
    unsigned width, height;
    int (*strip)(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags);
    int (*cancelled)(void *ud); /* optional; checked between strips and lines */
    void *(*line_buffer)(void *ud, size_t bytes);
} VjoGreySource;

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

/* The line finder returns at most this many lines (vocr_lines.h VOCR_LINES_MAX). */
#define VJO_VOCR_MAX_LINES 16u

/* One recognized line: its crop, the CTC frames the recognizer ran, and the
 * time it took, so a console log gives ms per frame for each line. */
typedef struct {
    uint16_t width, height;  /* crop in region pixels */
    uint16_t frames;         /* CTC frames (the resized width / 8) */
    uint16_t reserved;
    uint32_t us;             /* vocr_run time with VjoPlatform.now_us, else 0 */
} VjoVocrLineStat;

typedef struct {
    size_t workspace;        /* bytes of this job's workspace (see vjo_vocr_ocr*) */
    size_t capture;          /* bytes of its capture block (vjo_vocr_capture_bytes) */
    size_t model_bytes;      /* weights, used in place */
    size_t scratch_bytes;    /* the finder's arena, then the recognizer's */
    unsigned width, height;  /* region */
    unsigned strip_rows;     /* rows per strip */
    unsigned strips;         /* strips read (in the game: one frame each) */
    unsigned tears;          /* rows that read differently on a later read */
    unsigned tries;          /* captures of the region (1 unless the screen changed) */
    unsigned tall;           /* lines taller than a strip read into line_buffer (or left out: tall_skipped) */
    unsigned tall_skipped;
    uint32_t region_sum;     /* FNV-1a of the region's grey rows, first pass of the last try */
    uint32_t capture_us;     /* time inside strip() */
    unsigned lines;          /* lines found */
    unsigned dark;           /* of those, dark text (inverted before recognition) */
    unsigned truncated;      /* more lines than the finder returns (16) */
    unsigned frames;         /* CTC frames recognized, all lines */
    unsigned model_loaded;   /* 1: the weights were read and verified in this job */
    unsigned model_cached;   /* 1: resident verified weights were reused (file unchanged) */
    uint32_t find_us, load_us, recognize_us; /* with VjoPlatform.now_us, else 0 */
    unsigned recognized;     /* entries of line[] filled: lines vocr_run finished */
    VjoVocrLineStat line[VJO_VOCR_MAX_LINES];
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
 * model or region. Neither includes the capture block. */
size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height);
size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height);
/* Rows per strip for a region of this size, and the capture block's exact
 * bytes (0 for an unsupported region). */
unsigned vjo_vocr_strip_rows(unsigned width, unsigned height);
size_t vjo_vocr_capture_bytes(unsigned width, unsigned height);

/* Recognizes the region: lines top to bottom, separated by '\n', in text (at
 * most text_cap bytes with the NUL). workspace: 16-byte aligned, private to
 * the call, at least vjo_vocr_job_bytes(). capture: at least
 * vjo_vocr_capture_bytes(), any alignment.
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
 * runtime), VJO_E_OOM (workspace or capture too small, or no memory for the
 * weights), VJO_E_OCR_REGION, the source's error (VJO_E_SOURCE when it gave no
 * code), VJO_E_SCREEN_CHANGED, VJO_E_CANCELLED, VJO_E_TOO_LARGE (text),
 * VJO_E_OCR_INFERENCE, or VJO_E_OCR_UNAVAILABLE (resident memory could not be
 * released). text is "" on error. */
int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoGreySource *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                          char *text, size_t text_cap, VjoVocrStats *stats);

/* The one-shot form: the weights are read into the front of workspace (at
 * least vjo_vocr_workspace_bytes()) for this job only, as the host tools do. */
int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoGreySource *image, void *workspace, size_t workspace_size,
                 void *capture, size_t capture_size, char *text, size_t text_cap, VjoVocrStats *stats);

/* An RGBA image in memory (R, G, B, A bytes) as a VjoGreySource, for the host
 * tools and tests. */
typedef struct {
    const unsigned char *pixels;
    unsigned width, height;
    size_t stride;
} VjoRgbaImage;
void vjo_grey_source_rgba(const VjoRgbaImage *img, VjoGreySource *out);

#ifdef __cplusplus
}
#endif
#endif
