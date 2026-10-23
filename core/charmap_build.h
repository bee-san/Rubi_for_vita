/* The game worker's part of the char map (core/charmap.h): the places of the sentence's characters from the
 * recognized lines' characters (core/vocr_ocr.h VjoVocrChars), written after the job's text and its
 * recognizer's copy (API 37, docs/overlay-on-text.md). Not in the Shell. */
#ifndef VJO_CHARMAP_BUILD_H
#define VJO_CHARMAP_BUILD_H

#include <stddef.h>
#include <stdint.h>

#include "charmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One recognized line: its greedy characters (BMP code points) and their cells along it, in frame px, and its rows. */
typedef struct {
    const uint16_t *cp;
    const uint16_t *a, *b;
    unsigned n;
    unsigned x;               /* added to every a and b (the region's left edge in the frame) */
    unsigned y0, y1;
} VjoCmapSource;

#define VJO_CMAP_LINE_CPS 128u /* a text line's characters the builder places (longer lines: their first ones) */
#define VJO_CMAP_SRC_CPS 64u   /* a recognized line's characters it aligns to (core/vocr_ocr.h VJO_VOCR_CHARS_LINE) */

/* The builder's working memory for one text line, the caller's (the game worker lends its idle per-line box
 * buffer, core/vocr_ocr.h VjoVocrChars.scratch): no static of its own. */
typedef struct {
    uint32_t cp[VJO_CMAP_LINE_CPS];  /* the text line's code points */
    uint32_t src[VJO_CMAP_SRC_CPS];  /* a recognized line's, widened for vocr_charbox_align */
    uint16_t off[VJO_CMAP_LINE_CPS];
    int16_t map[VJO_CMAP_LINE_CPS];
} VjoCmapScratch;

/* The sentence the Shell shows of text (shell/worker.c run_vocr: core/textfilter.c vjo_filter_lines with
 * non_japanese_filter, then vjo_java_trim), as ranges of text: *lead and *len, the trimmed sentence's first byte
 * in the filtered text and its length; kept[k] = 1 for each of text's lines (at most VJO_CMAP_LINES) the filter
 * keeps. Returns text's line count (lines beyond VJO_CMAP_LINES count but are not marked). */
unsigned vjo_cmap_sentence(const char *text, int filter_lines, unsigned char kept[VJO_CMAP_LINES], size_t *lead,
                           size_t *len, uint32_t *fnv);

/* Writes the char map of the text in buf (cap bytes: the text, its NUL, the recognizer's copy and its NUL, zeros
 * after) right after the copy: each line the filter keeps is matched to a recognized line (the next of at most
 * four, the first whose characters align with at least half of its own), and its characters get the cells of
 * the recognized characters they align to (a character the recognizer did not read, a zero-width cell at its
 * neighbour). Returns the block's bytes, 0 when no character is placed (nothing is written then). */
size_t vjo_cmap_build(char *buf, size_t cap, int filter_lines, const VjoCmapSource *src, unsigned n_src,
                      unsigned fb_w, unsigned fb_h, VjoCmapScratch *scratch);

#ifdef __cplusplus
}
#endif
#endif
