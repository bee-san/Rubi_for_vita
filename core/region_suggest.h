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

/* A normalized rect as the pixels the kernel reads for it (kernel/capture.c: the origin floored, the far edge
 * ceiled) on the 960 x 544 screen the overlay draws on: box x0, y0, x1, y1 (VjoGameOcrResult.suggest); all 0 for
 * w or h 0. */
void vjo_region_screen(const VjoRect *r, uint16_t box[4]);

/* The kana and kanji of a UTF-8 text, as the Shell's line filter counts Japanese (core/utf.h
 * vjo_is_japanese_char: U+3040-30FF and U+4E00-9FAF), at most 65535. */
uint16_t vjo_suggest_kana(const char *s);

#ifdef __cplusplus
}
#endif

#endif
