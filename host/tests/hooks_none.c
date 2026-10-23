/* game_ocr/hooks.h's job-time half with nothing installed, for the game worker's host tests that are not
 * about native hooks: their jobs run as in a game where vjo_hooks_start installed nothing (no snapshot, no
 * selection; every result reports mode, installed and refused 0). vjo_hooks_start and vjo_hooks_stop are
 * game_ocr/main.c's host macros; host/tests/test_game_hooks.c has the streams. */
#include "../../game_ocr/hooks.h"

void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    *mode = 0;
    *installed = 0;
    *refused = 0;
}

int vjo_hooks_verified(uint32_t id)
{
    (void)id;
    return 0;
}

int vjo_hooks_snap(uint32_t id, VjoHookSnap *out)
{
    (void)id;
    (void)out;
    return 0;
}

void vjo_hooks_keep(uint32_t id, uint32_t key)
{
    (void)id;
    (void)key;
}

void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced)
{
    *dropped = 0;
    *replaced = 0;
}

size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    (void)s;
    if (cap) out[0] = '\0';
    return 0;
}
