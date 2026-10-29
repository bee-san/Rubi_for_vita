/* The overlay page's memory (shell/overlay.cpp open_page, close_page): its text arena, VJO_OVERLAY_MEM_BYTES of
 * SceShell's Paf heap, taken as the page opens and given back as it closes (#59: it was a 96 KiB static buffer).
 * The host tests open and close the overlay through these same functions under the free sizes the owner's console
 * logged (the owner scenario, docs/owner-scenario.md, tools/owner_scenario.py), so what the overlay needs in order
 * to open is measured there.
 *
 * Include after shell.h (vjo_shell_heap_alloc_for, vjo_log, VJO_SL_PAF_UI). Inline, so the Vita build's code is the
 * same as when these steps were written out in open_page and close_page. */
#ifndef VJO_OVERLAY_MEM_H
#define VJO_OVERLAY_MEM_H

#include <stdint.h>
#include "../core/port.h"
#include "../include/vjo_ledger_budget.h"

/* The arena's size: two styled texts of HEADER_UNITS/BODY_UNITS UTF-16 units plus their spans, 18,016 B, and the
 * entry ranges (VJO_HEADER_RANGE_BYTES), within the ledger's 20 KiB (host test: test_budget_table_is_consistent). */
#define VJO_OVERLAY_MEM_BYTES VJO_BUDGET_PAF_UI

/* The arena for a page that is opening, kept in *mem: the one already held, or a new one; NULL (logged) when none
 * can be had, and the page does not open this time (fail closed). */
static inline uint8_t *vjo_overlay_mem_open(uint8_t **mem)
{
    if (!*mem) {
        *mem = (uint8_t *)vjo_shell_heap_alloc_for(VJO_SL_PAF_UI, VJO_OVERLAY_MEM_BYTES);
        if (!*mem) {
            static const char no_memory[] VJO_DATA_TABLE(overlay_no_memory) =
                "overlay not opened: no Paf memory for its text (%u KiB)";
            vjo_log(no_memory, VJO_OVERLAY_MEM_BYTES >> 10);
        }
    }
    return *mem;
}

/* Gives the arena in *mem back (the page closed, or did not open after all). */
static inline void vjo_overlay_mem_close(uint8_t **mem)
{
    vjo_shell_heap_free(VJO_SL_PAF_UI, *mem, VJO_OVERLAY_MEM_BYTES);
    *mem = NULL;
}

#endif
