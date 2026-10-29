/* The Shell's Paf reserve guard: whether a request may be taken from SceShell's Paf global heap, and the log line
 * of a refusal. shell/memory.cpp vjo_shell_heap_alloc_for asks it before every Paf allocation of the plugin; the
 * host tests ask the same function with the free sizes the owner's console logged (the owner scenario,
 * docs/owner-scenario.md, tools/owner_scenario.py), so a change of policy here is measured there.
 *
 * Include after shell.h (vjo_log). Inline, so the Vita build's code is the same as when the check was written out
 * in memory.cpp. */
#ifndef VJO_PAF_GUARD_H
#define VJO_PAF_GUARD_H

#include <stddef.h>
#include "../include/vjo_ledger_budget.h"

/* The free bytes a request for ledger item `item` (VJO_SL_PAF_*) must leave: the same reserve for every item. */
static inline size_t vjo_paf_guard_reserve(int item)
{
    (void)item;
    return VJO_PAF_RESERVE;
}

/* 1: `bytes` for `item` (named `what` in the log) may be taken with `available` bytes free; 0: refused, logged. */
static inline int vjo_paf_guard(int item, const char *what, size_t available, size_t bytes)
{
    const size_t reserve = vjo_paf_guard_reserve(item);
    if (available < reserve || bytes > available - reserve) {
        vjo_log("%s Paf reserve guard: free=%u requested=%u reserve=%u",
                what, (unsigned)available, (unsigned)bytes, (unsigned)reserve);
        return 0;
    }
    return 1;
}

#endif
