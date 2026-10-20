/* The black bars around a game's picture, for the Anki screenshot (docs/anki.md, "Black bars";
 * API 32, VJO_GAME_OCR_S_TRIM): letterbox bars above and below the picture and pillarbox bars
 * left and right of it, found as the game worker reads the frame's rows from the top, and the
 * picture between them, the frame's sub-rectangle that the JPEG then encodes (core/jpegsw.h,
 * from the rows as they come: no copy of the frame). Linked into the game worker only, as
 * core/jpegsw.c is (vita/CMakeLists.txt).
 *
 * A bar is black in every pixel: its rows (or columns) hold no pixel whose R, G or B is 16 or
 * more. An axis is trimmed when both of its bars are at least VJO_TRIM_MIN_BAR wide, neither is
 * over 3/10 of the frame's side (the picture keeps at least 2/5 of it) and the narrower is at
 * least 3/4 of the wider: by the narrower one less VJO_TRIM_MARGIN, rounded down to an even
 * number, on both sides. An axis whose bars are both narrower than VJO_TRIM_MIN_BAR has none;
 * any other (one bar alone, one too wide, two too unequal) frames no centred picture, and the
 * frame is then kept whole, as an all-black frame is. */
#ifndef VJO_SHOTTRIM_H
#define VJO_SHOTTRIM_H

#include <stdint.h>

#define VJO_TRIM_BLACK_MASK 0x00F0F0F0u /* of an A8B8G8R8 word (R in the low byte): 0 when R, G, B < 16 */
#define VJO_TRIM_MIN_BAR 8u             /* a bar is at least 8 rows or columns */
#define VJO_TRIM_MARGIN 2u              /* of each bar, kept beside the picture */

/* What a frame's bars came to (the VJO_GSTEP_TRIM step's kind). */
enum { VJO_TRIM_NONE = 0, VJO_TRIM_TRIMMED = 1, VJO_TRIM_KEPT = 2 };

typedef struct {
    uint32_t x, y, w, h;
} VjoTrimRect;

typedef struct {
    uint32_t w, h, rows;  /* the frame, and its rows scanned so far: row 0 first, in order */
    uint32_t top, last;   /* the black rows above the first content row; the last content row */
    uint32_t left, right; /* the fewest black pixels a content row starts and ends with (w: no content row) */
    uint32_t color;       /* row 0's first pixel (R, G, B) */
    uint8_t content;      /* a row with a pixel that is not black was scanned */
    uint8_t above;        /* every pixel above the first content row is `color` */
    uint8_t uniform;      /* every black pixel read is `color` (so `above` too) */
} VjoTrimScan;

void vjo_trim_begin(VjoTrimScan *s, uint32_t w, uint32_t h);
/* The next n rows of the frame (rows s->rows to s->rows + n - 1; no more than are left), as
 * A8B8G8R8 words, stride bytes apart. A black row is read whole, once; a content row from its left
 * end to its first content pixel and from its right end to its last content pixel, but no further
 * than the fewest black pixels a content row has ended with yet (a row's only content pixel is read
 * from both ends). */
void vjo_trim_rows(VjoTrimScan *s, const uint8_t *rgba, uint32_t n, uint32_t stride);
/* The picture once every row is scanned: VJO_TRIM_*, and *r the frame's sub-rectangle to encode
 * (the whole frame unless VJO_TRIM_TRIMMED). */
int vjo_trim_decide(const VjoTrimScan *s, VjoTrimRect *r);
/* The guess from the rows scanned so far, once a content row is among them or more than 3/10 of
 * the frame's rows are all black: the bars as decided above with the rows below holding nothing
 * outside it and, under a top bar, a bottom bar as wide; but side bars under no top bar whose black
 * pixels are not all one colour (a dark picture's edges, where an emulator's bars are one colour) are
 * guessed as the whole frame (VJO_TRIM_KEPT). *exact is 1 when no row below can change the picture:
 * every row is scanned already (the guess is vjo_trim_decide's), or no centred picture can have so tall
 * a black bar at the top (8 rows or more, over 3/10 of the frame: kept whole), or the first rows reach the
 * top edge and a side's (none is trimmed; the kind is the first rows'). */
int vjo_trim_guess(const VjoTrimScan *s, VjoTrimRect *r, int *exact);

#endif
