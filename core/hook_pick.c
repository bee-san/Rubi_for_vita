#include "hook_pick.h"

#include "port.h"
#include "../include/vjo_game_ocr.h"

#define ALL_STREAMS ((1u << VJO_HOOK_STREAMS) - 1u)

static unsigned bits(uint32_t m) /* no libgcc in the Shell: __builtin_popcount would call it */
{
    unsigned n = 0;
    for (; m; m &= m - 1u)
        n++;
    return n;
}

uint32_t vjo_hook_pick_next(uint32_t pick, uint32_t streams)
{
    uint32_t from;
    if (pick == VJO_HOOK_PICK_OCR || pick > VJO_HOOK_STREAMS)
        return VJO_HOOK_PICK_AUTO;
    /* the streams with text after pick (all of them after automatic, whose ID is 0) */
    from = pick ? streams & ALL_STREAMS & ~((VJO_HOOK_STREAM_BIT(pick) << 1) - 1u) : streams & ALL_STREAMS;
    return from ? (uint32_t)__builtin_ctz(from) + 1u : VJO_HOOK_PICK_OCR;
}

size_t vjo_hook_label(char *out, size_t cap, uint32_t pick, int installed, int have_result, uint32_t text_from,
                      uint32_t flags, uint32_t streams)
{
    uint32_t id, mask;
    const char *pre = "", *how = "";
    int n;
    if (!cap)
        return 0;
    out[0] = '\0';
    if (pick == VJO_HOOK_PICK_OCR)
        n = vjo_snprintf(out, cap, "OCR only");
    else if (pick > VJO_HOOK_STREAMS || (!installed && pick == VJO_HOOK_PICK_AUTO))
        return 0;
    else if (!(id = have_result && text_from >= 1 && text_from <= VJO_HOOK_STREAMS ? text_from : pick))
        n = vjo_snprintf(out, cap, have_result ? "OCR" : "Automatic");
    else {
        /* id: the stream shown, or the pinned one; its place among the streams with text */
        mask = (streams & ALL_STREAMS) | VJO_HOOK_STREAM_BIT(id);
        if (!have_result || pick == id || (flags & VJO_HOOK_R_PINNED))
            how = " pinned";
        else if (flags & VJO_HOOK_R_LOCKED)
            how = " locked";
        if (have_result && id != text_from) { /* pinned, but its stream had no text: OCR answered */
            pre = "OCR, ";
            how = " empty";
        }
        n = vjo_snprintf(out, cap, "%sHook %u/%u%s", pre,
                         bits(mask & ((VJO_HOOK_STREAM_BIT(id) << 1) - 1u)), bits(mask), how);
    }
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}
