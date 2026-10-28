/* The frozen API 18 copy beside this file (vocr_ocr.h, 8228c3c, verbatim:
 * tools/tests/test_capture_parity.py checks its hash) includes "local_ocr.h",
 * the ncnn engine's header, for its image type and three limits. A quoted
 * include finds this file first: what the copy reads of that header, as 8228c3c
 * had it, now that the ncnn engine is gone (#418). */
#ifndef VJO_API18_LOCAL_OCR_H
#define VJO_API18_LOCAL_OCR_H
#include "platform.h"

#define VJO_OCR_MAX_WIDTH 960
#define VJO_OCR_MAX_HEIGHT 544
#define VJO_OCR_TEXT_CAP 4096

typedef struct {
    void *ud;
    unsigned width, height;
    /* Packed RGBA rows; return the number of rows copied, or <0. */
    int (*rows)(void *ud, unsigned y, unsigned n, unsigned char *rgba);
    int (*cancelled)(void *ud); /* optional; checked between reads/lines */
} VjoOcrImage;
#endif
