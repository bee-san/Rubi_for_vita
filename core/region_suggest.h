/* The text box suggested on first use (#358): a game with no region of its own is read on its whole
 * screen, and from that job's lines the game worker suggests where the dialogue is; the overlay offers it
 * once ("Use this box?", Enter: yes). First-time users took the whole screen's reading, with no box drawn,
 * for recognition that did not work.
 *
 * The rule (docs/first-box.md: how it was chosen and measured on the hooked pages): the recognized lines
 * whose text holds at least VJO_SUGGEST_LINE_KANA kana or kanji and whose height is a dialogue line's
 * (12-90 px of a 544-px frame) are grouped top to bottom, a line joining the one above it when the gap
 * between them is at most the taller one's height, their heights are within twice each other and they
 * overlap across or start within 40 px (of 960) of each other. The group with the most kana and kanji (the
 * upper one on a tie), with at least VJO_SUGGEST_BOX_KANA, is the dialogue; the box is the frame's whole
 * width from half its lines' median height above its first line down to the frame's bottom, where the
 * dialogue's later lines go. A box drawn tight around one screen's lines leaves out the longer and the
 * extra lines of the screens after it.
 *
 * Pure C, no allocation and no SDK call: the game worker (game_ocr/main.c) and the host tools
 * (host/page_ocr.c) call it, the host tests drive it (host/tests/test_region_suggest.c). */
#ifndef VJO_REGION_SUGGEST_H
#define VJO_REGION_SUGGEST_H

#include <stdint.h>

#include "../include/vjo_api.h" /* VjoRect */
#include "suggest_kana.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A recognized line: its box in frame pixels, and the kana and kanji its text holds. */
typedef struct {
    uint16_t x, y, w, h;
    uint16_t kana;
} VjoSuggestLine;

#define VJO_SUGGEST_LINES 16u     /* the most lines taken (the line finder's maximum, vocr_lines.h) */
#define VJO_SUGGEST_LINE_KANA 2u  /* a line counts with at least this many kana or kanji */
#define VJO_SUGGEST_BOX_KANA 6u   /* the dialogue's lines hold at least this many */

/* Fills *out with the suggested box (normalized, as region.ini keeps it: x 0, w 65535, the whole width)
 * and returns 1, or zeroes it and returns 0 when no group of lines qualifies (or the frame is empty).
 * Lines outside the frame are not taken; at most VJO_SUGGEST_LINES of them, in the order given. */
int vjo_region_suggest(const VjoSuggestLine *line, unsigned n, unsigned fb_w, unsigned fb_h, VjoRect *out);

/* A suggested box (vjo_region_suggest's: the frame's width, down to its bottom) as its rows on the 960 x 544 screen the
 * overlay draws on: 544 less its top row there (the normalized top, floored), as VjoGameOcrResult.suggest_rows
 * carries it; 0 for none (w or h 0). The kernel reads the band's rows of the region region.ini keeps for it
 * (kernel/capture.c; the audit's round trip: exact at 960 x 544 and 480 x 272). */
uint32_t vjo_region_rows(const VjoRect *r);

/* vjo_suggest_kana: core/suggest_kana.h (core/vocr_ocr.c's header, without include/vjo_api.h). */

#ifdef __cplusplus
}
#endif

#endif
