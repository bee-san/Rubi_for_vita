#ifndef VJO_HEADER_HIGHLIGHT_H
#define VJO_HEADER_HIGHLIGHT_H

#include <stdint.h>

/* SetStyleAttribute changes the text's styles; invalidate its cached drawing
 * explicitly when a lookup replaces the character highlight with a word.
 * Keep the applied range until all calls succeed so a later frame can retry.
 * The template also lets host tests exercise this exact update sequence. */
template <class Text, class Attribute, class Color>
bool vjo_header_recolor(Text *text, Attribute attribute,
                        uint32_t &applied_start, uint32_t &applied_len, bool &dirty,
                        uint32_t next_start, uint32_t next_len,
                        const Color &normal, const Color &highlight)
{
    if (!text)
        return false;
    if (!next_len)
        next_start = 0;
    if (!dirty && applied_start == next_start && applied_len == next_len)
        return true;

    uint32_t units = 0;
    if (text->GetLength(units) < 0 || next_start > units || next_len > units - next_start)
        return false;
    uint32_t clear_start = dirty ? 0 : applied_start;
    uint32_t clear_len = dirty ? units : applied_len;
    dirty = true; /* a failed update may already have changed some styles */
    if (clear_len && text->SetStyleAttribute(attribute, clear_start, clear_len, normal) < 0)
        return false;
    if (next_len && text->SetStyleAttribute(attribute, next_start, next_len, highlight) < 0)
        return false;
    if (text->Invalidate() < 0)
        return false;

    applied_start = next_start;
    applied_len = next_len;
    dirty = false;
    return true;
}

#endif
