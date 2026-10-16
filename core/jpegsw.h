/* Baseline JPEG encoder (YCbCr 4:2:0, the standard Huffman tables, an AAN float
 * DCT): the Anki screenshot's (docs/anki.md). Linked into the game worker only,
 * never into SceShell (AGENTS.md rule 1; tools/tests/test_jpegsw_placement.py
 * checks the builds). Restored from the Shell's Google Lens encoder (removed in
 * #130), which encoded the region then; this one takes the picture a band of 16
 * rows (one row of MCUs) at a time, so its caller holds one band of pixels and
 * the JPEG, never the frame. */
#ifndef VJO_JPEGSW_H
#define VJO_JPEGSW_H

#include <stdint.h>

#include "arena.h"

#define VJO_JPEG_BAND 16u        /* rows per vjo_jpeg_rows call (the last band may have fewer) */
#define VJO_JPEG_MAX_SIDE 4096u
#define VJO_JPEG_QUALITY_MAX 95  /* above it a coefficient could need a code the standard tables lack */
#define VJO_JPEG_STATE_BYTES 4096u /* the encoder's state in its arena, at most (core/jpegsw.c checks) */

typedef struct VjoJpeg VjoJpeg;

/* Starts a w x h picture at quality 1-95 (clamped): the encoder's state from a,
 * then out initialized on a and the headers written into it. out grows as a's last
 * allocation, so nothing else may be taken from a until vjo_jpeg_end. NULL when a
 * cannot hold the state or the headers, or for a size of 0 or over 4096. */
VjoJpeg *vjo_jpeg_begin(VjoArena *a, uint32_t w, uint32_t h, int quality, VjoBuf *out);
/* The next n rows (1 to 16; fewer than 16 only for the last band, whose missing
 * rows repeat its last), R, G, B and an ignored byte a pixel (A8B8G8R8 words, R in the
 * low byte), stride bytes apart. 0, or -1 when out ran out of room (out->oom) or the
 * rows go past the picture's height. */
int vjo_jpeg_rows(VjoJpeg *e, const uint8_t *rgba, uint32_t n, uint32_t stride);
/* The end of the picture (its last bits and the EOI marker): 0, or -1 when out ran
 * out of room or rows are missing. */
int vjo_jpeg_end(VjoJpeg *e);

/* Fills rows [row, row + n) as R,G,B,A bytes, `stride` bytes apart, into dst;
 * returns the number of rows written. */
typedef int (*VjoRowsFn)(void *ud, uint32_t row, uint32_t n, uint8_t *dst);
/* A whole picture (host tools and tests): w x h at quality, its rows from rows()
 * into a band of stride x 16 bytes from a, appended to out (initialized here). 0 or -1. */
int vjo_jpeg_encode(VjoArena *a, uint32_t w, uint32_t h, uint32_t stride, VjoRowsFn rows, void *ud,
                    int quality, VjoBuf *out);

#endif
