#include "hook_titles.h"

#include "hook_luna.h"
#include "hook_place.h"

int vjo_hook_title_listed(const char *title_id)
{
    if (!title_id || !title_id[0])
        return VJO_HOOK_TITLE_NONE;
    /* Rubi's own entries: the worker's verified table itself (core/hook_place.c), the entries it installs
     * (game_ocr/hooks.c), not a copy of their titles, so a new entry is tagged as soon as it is added. */
    if (vjo_place_title_verified(title_id))
        return VJO_HOOK_TITLE_RUBI;
#if VJO_HOOK_LUNA /* core/hook_luna.c is in the build (hooks/luna-port) */
    /* a site the worker may try: a title whose every site is unported has no tag (audit-hooks-text N2) */
    return vjo_luna_title_ported(title_id) ? VJO_HOOK_TITLE_LUNA : VJO_HOOK_TITLE_NONE;
#else
    return VJO_HOOK_TITLE_NONE;
#endif
}
