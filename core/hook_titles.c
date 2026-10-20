#include "hook_titles.h"

#include <string.h>

#include "hook_luna.h"

/* Rubi's own entries: the games whose hook site Rubi checked itself (docs/native-hooks.md). The worker's
 * own copy is the entry it installs (game_ocr/hooks.c); this one only tags the Games page's rows. */
static const char rubi_titles[][10] = {"PCSG00415"}; /* CLANNAD */

int vjo_hook_title_listed(const char *title_id)
{
    if (!title_id || !title_id[0])
        return VJO_HOOK_TITLE_NONE;
    for (unsigned i = 0; i < sizeof(rubi_titles) / sizeof(rubi_titles[0]); i++)
        if (!strcmp(title_id, rubi_titles[i]))
            return VJO_HOOK_TITLE_RUBI;
#if VJO_HOOK_LUNA /* core/hook_luna.c is in the build (hooks/luna-port) */
    /* a site the worker may try: a title whose every site is unported has no tag (audit-hooks-text N2) */
    return vjo_luna_title_ported(title_id) ? VJO_HOOK_TITLE_LUNA : VJO_HOOK_TITLE_NONE;
#else
    return VJO_HOOK_TITLE_NONE;
#endif
}
