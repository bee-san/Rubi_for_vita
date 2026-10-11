#include "text_hit.h"
#include "utf.h"
#include <float.h>

int vjo_text_hit(const char *text, size_t len, const VjoTextHitLayout *l,
                 float x, float y, int japanese_only, size_t *byte)
{
    if (!text || !len || !l || !byte || !l->position || !l->lines ||
        !(l->width > 0) || !(l->clip_height > 0) || !(l->content_height > 0) ||
        !(l->terminal_width > 0) || !(l->scroll >= 0) ||
        !(l->width < FLT_MAX) || !(l->content_height < FLT_MAX) ||
        !(l->terminal_width <= l->width) || !(l->clip_height < FLT_MAX) ||
        !(x >= l->left && x < l->left + l->width) ||
        !(y >= l->top && y < l->top + l->clip_height))
        return 0;
    float local_x = x - l->left;
    float local_y = y - l->top + l->scroll;
    float pitch = l->content_height / (float)l->lines;
    if (!(local_y >= 0 && local_y < l->content_height) || !(pitch > 0)) return 0;
    uint32_t target_line = (uint32_t)(local_y / pitch), unit = 0;
    size_t candidate = len;
    uint32_t candidate_cp = 0;
    float start = 0, end = 0;
    for (size_t i = 0; i < len;) {
        size_t begin = i;
        uint32_t cp = vjo_utf8_next(text, len, &i);
        if (cp == '\r') continue;
        float cx = 0;
        uint32_t line = 0;
        if (l->position(l->ctx, unit, &cx, &line) < 0) return 0;
        unit += (uint32_t)vjo_utf16_units(cp);
        if (line >= l->lines || !(cx >= 0 && cx <= l->width)) return 0;
        if (candidate < len && line > target_line) break;
        if (candidate < len && line == target_line && cx > local_x) {
            end = cx;
            break;
        }
        if (line == target_line && cx <= local_x) {
            candidate = begin;
            candidate_cp = cp;
            start = cx;
            end = cx + l->terminal_width;
        }
    }
    if (candidate == len || !(local_x >= start && local_x < end) ||
        candidate_cp <= 0x20 || (japanese_only &&
        vjo_utf8_japanese_cursor_step(text, len, candidate, 0) != candidate)) return 0;
    *byte = candidate;
    return 1;
}
