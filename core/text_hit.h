/* Allocation-free hit testing of the uniformly styled OCR header. */
#ifndef VJO_TEXT_HIT_H
#define VJO_TEXT_HIT_H
#include <stddef.h>
#include <stdint.h>

typedef int (*VjoCharPosition)(void *ctx, uint32_t index16, float *x, uint32_t *line);
typedef struct {
    float left, top, width, clip_height, content_height, scroll, terminal_width;
    uint32_t lines;
    VjoCharPosition position;
    void *ctx;
} VjoTextHitLayout;

/* Touch coordinates are screen pixels. Character starts come from the real
 * renderer; the last cell on a line uses the configured Japanese point width.
 * CR has no rendered units. Blank space, omitted/control characters and
 * non-Japanese cells (when restricted) have no hit; they never select a
 * neighbouring word. A surrogate pair always returns its source start. */
int vjo_text_hit(const char *text, size_t len, const VjoTextHitLayout *layout,
                 float x, float y, int japanese_only, size_t *byte);
#endif
