/* The overlay page's memory (shell/overlay.cpp vjo_overlay_boot, open_page, close_page): its text arena,
 * VJO_OVERLAY_MEM_BYTES of SceShell's Paf heap (#59: it was a 96 KiB static buffer). #528: it is taken once as the
 * Shell starts, through the same reserve guard, while the heap is at its fullest (3,725,976-3,744,264 B free at all
 * 90 Shell starts in #528's logs [measured]), and kept, so an open asks for no memory of ours. Taken at every open
 * before, beside another plugin's 2,187,264 B buffer (202,296-426,872 B free) the guard refused it 34 times and the
 * overlay did not open, with nothing on screen. The host tests open and close the overlay through these same
 * functions under the free sizes the owner's console logged (the owner scenario, docs/owner-scenario.md,
 * tools/owner_scenario.py), so what the overlay needs in order to open is measured there.
 *
 * Include after shell.h (vjo_shell_heap_alloc_for, vjo_shell_heap_free_size, vjo_log, VJO_SL_PAF_UI). Inline, so the
 * Vita build's code is the same as when these steps were written out in overlay.cpp. */
#ifndef VJO_OVERLAY_MEM_H
#define VJO_OVERLAY_MEM_H

#include <stdint.h>
#include "../core/port.h"
#include "../include/vjo_ledger_budget.h"
#include "paf_guard.h" /* vjo_paf_page_fits: the room the page itself needs */

/* The arena's size: two styled texts of HEADER_UNITS/BODY_UNITS UTF-16 units plus their spans, 18,016 B, and the
 * entry ranges (VJO_HEADER_RANGE_BYTES), within the ledger's 20 KiB (host test: test_budget_table_is_consistent). */
#define VJO_OVERLAY_MEM_BYTES VJO_BUDGET_PAF_UI

/* The Shell's start (shell/worker.c vjo_worker_start, before its boot ledger line): the arena, kept in *mem from now
 * on; a refusal is the guard's (logged), and the first open asks again. */
static inline void vjo_overlay_mem_boot(uint8_t **mem)
{
    *mem = (uint8_t *)vjo_shell_heap_alloc_for(VJO_SL_PAF_UI, VJO_OVERLAY_MEM_BYTES);
}

/* The arena for a page that is opening, kept in *mem: the one already held, or a new one (the start's take was
 * refused); NULL (logged) when none can be had, or when the heap has no room for the page itself beside SceShell's
 * margin (shell/paf_guard.h vjo_paf_page_fits), and the page does not open this time (fail closed). */
static inline uint8_t *vjo_overlay_mem_open(uint8_t **mem)
{
    if (!*mem) {
        *mem = (uint8_t *)vjo_shell_heap_alloc_for(VJO_SL_PAF_UI, VJO_OVERLAY_MEM_BYTES);
        if (!*mem) {
            static const char no_memory[] VJO_DATA_TABLE(overlay_no_memory) =
                "overlay not opened: no Paf memory for its text (%u KiB)";
            vjo_log(no_memory, VJO_OVERLAY_MEM_BYTES >> 10);
            return NULL;
        }
    }
    return vjo_paf_page_fits(vjo_shell_heap_free_size()) ? *mem : NULL;
}

/* The page closed, or did not open after all: the arena in *mem stays for the next open (#528). Nothing gives it
 * back while the plugin runs; SceShell's heap is SceShell's until it stops. */
static inline void vjo_overlay_mem_close(uint8_t **mem)
{
    (void)mem;
}

#endif
