/* Wrapped text for the Overlay app (settings/main.c's wrapped_text), laid
 * out once and drawn many times. A layout is where each drawn row starts in
 * the source string, how many bytes it takes and whether it ends in "...";
 * it is computed by the app's own measurement rule (fit_line: the widest
 * prefix whose PVF width fits, cut back to its last space when the text goes
 * on), so drawing from it gives exactly the rows wrapped_text always drew.
 * Measuring is what costs: every prefix of a row is measured, one glyph run
 * each, so a row of n characters costs about n * n / 2 glyph measurements. */
#ifndef VJO_SETTINGS_TEXT_LAYOUT_H
#define VJO_SETTINGS_TEXT_LAYOUT_H
#include <stdint.h>

/* The text's width in pixels at a size (main.c: vita2d_pvf_text_width). */
typedef int (*VjoTextMeasure)(float pixels, const char *s);

/* The most rows a layout holds; the app asks for at most 2. */
#define VJO_TEXT_ROWS 4
/* A drawn row's bytes are at most this (fit_line's 512-byte row with "..."). */
#define VJO_TEXT_ROW_BYTES 508

typedef struct {
    unsigned short start, bytes; /* the row's text: source[start, start + bytes) */
    unsigned char ellipsis;      /* followed by "..." (the last row, text cut) */
} VjoTextRow;

typedef struct {
    unsigned char rows; /* rows drawn, an empty one included */
    VjoTextRow row[VJO_TEXT_ROWS];
} VjoTextLayout;

/* Lays s out in rows of at most width pixels, at most lines rows (clamped to
 * VJO_TEXT_ROWS), as wrapped_text draws it. */
void vjo_text_layout(const char *s, int width, float pixels, int lines, VjoTextMeasure measure,
                     VjoTextLayout *out);
/* Row r's text, as drawn: out holds VJO_TEXT_ROW_BYTES + 4 bytes. */
void vjo_text_layout_row(const char *s, const VjoTextLayout *layout, int r, char out[512]);

/* A few recent layouts by exact source text, width, size and rows: the
 * status line, the hints and the page header, which are drawn every frame
 * and change only on an event. Text longer than VJO_TEXT_MEMO_TEXT - 1 bytes
 * is laid out every time, as before. */
#define VJO_TEXT_MEMO 16
#define VJO_TEXT_MEMO_TEXT 256
typedef struct {
    uint32_t hash;
    unsigned used;          /* 0: empty; else the memo's clock at the last use */
    unsigned short bytes;   /* strlen of text */
    int width;
    unsigned char lines;
    float pixels;
    VjoTextLayout layout;
    char text[VJO_TEXT_MEMO_TEXT];
} VjoTextMemoEntry;
typedef struct {
    VjoTextMemoEntry entry[VJO_TEXT_MEMO];
    unsigned clock, hits, misses;
} VjoTextMemo;

/* s's layout: the memo's when it has one, else computed (into the least
 * recently used entry, or into *scratch for text too long to keep). */
const VjoTextLayout *vjo_text_memo_layout(VjoTextMemo *memo, const char *s, int width, float pixels, int lines,
                                          VjoTextMeasure measure, VjoTextLayout *scratch);
void vjo_text_memo_clear(VjoTextMemo *memo);

#endif
