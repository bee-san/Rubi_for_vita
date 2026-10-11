#ifndef VJO_WORD_NAVIGATION_H
#define VJO_WORD_NAVIGATION_H

extern "C" {
#include "../core/utf.h"
}

/* Store positions, not definitions or dictionary pointers. Right skips the
 * matched source span; Left retraces up to 16 recent forward selections.
 * Without a match/history, keep the existing character navigation. */
struct VjoWordNavigation {
    uint32_t previous[16];
    unsigned count;
};

inline size_t vjo_word_cursor_step(VjoWordNavigation &history, const char *text,
                                  size_t len, size_t cursor, size_t matched_end,
                                  int direction, int japanese_only)
{
    if (!text || !len || cursor >= len || !direction)
        return cursor;
    if (direction < 0 && history.count && history.previous[history.count - 1] < cursor)
        return history.previous[--history.count];

    size_t next;
    if (direction > 0 && matched_end > cursor && matched_end <= len &&
        (matched_end == len || vjo_utf8_cursor_step(text, len, matched_end, 0) == matched_end)) {
        if (matched_end == len)
            return cursor;
        next = japanese_only ? vjo_utf8_japanese_cursor_step(text, len, matched_end, 0) : matched_end;
        if (next < matched_end) /* only punctuation/non-Japanese text remains */
            return cursor;
    } else {
        next = japanese_only ? vjo_utf8_japanese_cursor_step(text, len, cursor, direction)
                             : vjo_utf8_cursor_step(text, len, cursor, direction);
    }
    if (next != cursor) {
        if (direction > 0) {
            if (history.count == 16) {
                for (unsigned i = 1; i < history.count; ++i)
                    history.previous[i - 1] = history.previous[i];
                --history.count;
            }
            history.previous[history.count++] = (uint32_t)cursor;
        } else {
            history.count = 0;
        }
    }
    return next;
}

#endif
