/* API 30, a scan's result (scan screen: VJO_GAME_OCR_S_SPOT, include/vjo_game_ocr.h): vita-vn-ocr #104's text
 * spotter's set as the game worker returns it, at the start of VjoGameOcrResult.text, the rest of the text zero
 * (so it keeps its NUL), and nothing there after a failed scan. The layout is the runtime's own
 * (runtime/vocr_textspot.h VocrTextspotSet, which the spotter writes there in place; core/vocr_ocr.c checks every
 * field's offset), in frame pixels (the result's fb_width x fb_height): boxes and blocks in the spotter's reading
 * order, a block's boxes box[first .. first + count). Plain types only: core/vocr_ocr.h and the host tools
 * include it too. */
#ifndef VJO_SPOT_H
#define VJO_SPOT_H

#include <stdint.h>

#define VJO_SPOT_MAX 64u /* boxes and blocks: VOCR_TEXTSPOT_MAX_BOXES */

typedef struct {
    uint16_t x, y, w, h;
    uint8_t vertical; /* 1: a vertical column (read turned 90 degrees, by vertical text: not in this build) */
    uint8_t conf;     /* 1..127 */
    uint8_t dark;     /* 0: a scan leaves it to the read (VOCR_TEXTSPOT_NO_DARK) */
    uint8_t block;    /* its block */
} VjoSpotBox;

typedef struct {
    uint16_t x, y, w, h; /* the union of its boxes */
    uint8_t vertical, first, count, reserved;
} VjoSpotBlock;

typedef struct {
    VjoSpotBox box[VJO_SPOT_MAX];
    VjoSpotBlock block[VJO_SPOT_MAX];
    uint32_t count;       /* boxes; 0: no text on the frame */
    uint32_t block_count;
    int32_t pick;         /* the block picked as the likely dialogue, -1 for none */
    uint32_t truncated;   /* 1: more than VJO_SPOT_MAX candidates (the most confident are kept) */
} VjoSpotResult;

typedef char vjo_spot_layout[sizeof(VjoSpotBox) == 12 && sizeof(VjoSpotBlock) == 12 &&
                             sizeof(VjoSpotResult) == 2 * 12 * VJO_SPOT_MAX + 16 ? 1 : -1];

/* The overlay's map of a scan (core/spot_pick.c vjo_spot_map), which the game worker makes from the set so that
 * SceShell's code for it is a table lookup (AGENTS.md rule 1): the first VJO_SPOT_SHOWN blocks in reading order,
 * the last place the pick's when it is not among them (vita-vn-ocr #104's h2 family B spotters gave more than 16
 * blocks on 2 of their 2,632 results on its 188 development frames, at most 19: a design statistic, not a
 * score), each as the region it is read as: the block on the 960 x 544 screen, grown by VJO_SPOT_MARGIN pixels on
 * every side and clipped to the screen. The Shell uses a map only when every index in it is one of its blocks
 * (core/spot_pick.h vjo_spot_map_ok). */
#define VJO_SPOT_SHOWN 16
#define VJO_SPOT_MARGIN 8
typedef struct {
    uint8_t count;      /* blocks shown (0..VJO_SPOT_SHOWN; 0: none, or the scan failed) */
    int8_t start;       /* the block the cursor starts on: the spotter's pick, or when it cannot be picked (vertical,
                           or not shown) the first block that can; -1 for none */
    uint16_t vertical;  /* bit i: block i is vertical (drawn, not picked: no vertical text in this build) */
    int16_t rect[VJO_SPOT_SHOWN][4]; /* x, y, w, h in screen pixels */
    uint8_t next[VJO_SPOT_SHOWN][4]; /* from block i, the nearest that can be picked up, right, down and left (the
                                        d-pad's VJO_RSEL_UP << d; core/spot_pick.h says which lie that way);
                                        i itself where there is none */
} VjoSpotMap;

/* A scan's result text: the spotter's set, then the overlay's map. */
typedef struct {
    VjoSpotResult set;
    VjoSpotMap map;
} VjoSpotScan;

typedef char vjo_spot_map_layout[sizeof(VjoSpotMap) == 4 + 8 * VJO_SPOT_SHOWN + 4 * VJO_SPOT_SHOWN &&
                                 sizeof(VjoSpotScan) == sizeof(VjoSpotResult) + sizeof(VjoSpotMap) ? 1 : -1];

#endif
