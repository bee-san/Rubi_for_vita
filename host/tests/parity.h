/* The capture parity test (host/tests/test_capture_parity.c): the API 18
 * pipeline (kernel copies the region as A8B8G8R8, the game worker builds the
 * grey plane, core 8228c3c) against API 19 (the display hook writes grey rows
 * into the worker's strip, the current core), each driven to vocr_run with
 * only the runtime's inference replaced by a recorder. */
#ifndef VJO_PARITY_H
#define VJO_PARITY_H

#include <stddef.h>
#include <stdint.h>

#define PARITY_MAX_LINES 64u

/* A game frame: pixels in the SceDisplay format fmt, pitch pixels per row;
 * region in 1/65536 of the frame (VjoRect), w == 0 for the whole frame. */
typedef struct {
    const void *pixels;
    unsigned fb_w, fb_h, pitch;
    uint32_t fmt;
    uint16_t rx, ry, rw, rh;
} ParityFrame;

typedef struct {
    unsigned w, h;
    int format;                 /* vocr_image.format */
} ParityLine;

/* What one pipeline handed the recognizer for one region. */
typedef struct {
    int rc;
    unsigned crop_x, crop_y, crop_w, crop_h;
    unsigned lines;
    ParityLine line[PARITY_MAX_LINES];
    unsigned char *bytes;       /* every line's w x h grey bytes, in order */
    size_t used, cap;
    unsigned char *tensor;      /* with a real model: each line as vocr_preprocess makes it */
    size_t tensor_used, tensor_cap;
    unsigned tall;              /* lines taller than a strip (API 19 reads them whole) */
} ParityRun;

/* The real kernel (kernel/capture.c, through parity_kernel.c): the crop of a
 * normalized region (w == 0: the whole frame) and the display hook's grey rows
 * of an armed vjoOcrCaptureRows (OcrRowsJob, same layout). */
typedef struct {
    uint32_t x0, y0, w, h, fb_w, fb_h;
    uint32_t y, n, stride;
    uintptr_t dst_user;
    uint8_t *dst_kernel;
    int sig;
    int rgba; /* API 23: 0 here, the grey rows (a shot's colour rows are test_game_shot.c's) */
    int frame; /* API 51: 0 here (the hook's chunks are game_ocr_rows_hook's: test_kernel_game_ocr.c) */
} ParityRowsJob;
void parity_crop_of(uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh, uint32_t fb_w, uint32_t fb_h, uint32_t *x,
                    uint32_t *y, uint32_t *w, uint32_t *h);
int parity_grey_rows(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h,
                     const ParityRowsJob *job);

/* A vocr_model parsed from the real weights, or NULL (then no tensors). */
extern const void *parity_model;

/* Records one vocr_run input (a const vocr_image *) into run. */
void parity_record(ParityRun *run, const void *image);
void parity_reset(ParityRun *run);

/* The two pipelines; model picks the finder mode of its pinned table entry. */
int parity_api18(const ParityFrame *f, const char *model, ParityRun *out);
int parity_api19(const ParityFrame *f, const char *model, ParityRun *out);

#endif
