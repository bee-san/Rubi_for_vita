/* Borrow reserved Shell pages while leaving room for the LiveArea and UI.
 * The only PAF allocation site of the plugin: every request is checked
 * against the reserve, named by its ledger item, and counted. */
#include <paf/memory/heap_allocator.h>
#include <paf/std/stdlib.h>
#include <psp2/kernel/clib.h>
#include "../include/vjo_import.h"
#include "../include/vjo_ledger_budget.h"
extern "C" {
#include "shell.h"
#include "paf_guard.h" /* the reserve guard's decision, which the host tests run too (docs/owner-scenario.md) */
}

template <typename Function> static bool ready(Function fn)
{
    if (!fn) return false;
#ifdef __arm__
    return VJO_IMPORT_READY(fn) != 0;
#else
    return true;
#endif
}

static paf::memory::HeapAllocator *heap_ready(void)
{
    if (!ready(::GetGlobalHeapAllocator) ||
        !ready(sce_paf_memalign) || !ready(sce_paf_free))
        return nullptr;
    auto *heap = static_cast<paf::memory::HeapAllocator *>(::GetGlobalHeapAllocator());
    if (!heap) return nullptr;
    uintptr_t vtable = 0;
    sceClibMemcpy(&vtable, heap, sizeof(vtable));
    if (!vtable || !reinterpret_cast<const volatile uintptr_t *>(vtable)[4])
        return nullptr;
    return heap;
}

extern "C" uint32_t vjo_shell_heap_free_size(void)
{
    auto *heap = heap_ready();
    return heap ? (uint32_t)heap->GetFreeSize() : 0;
}

extern "C" void *vjo_shell_heap_alloc_for(int item, size_t bytes)
{
    auto *heap = heap_ready();
    if (!heap) return nullptr;
    const char *what = item >= 0 && item < VJO_SL_COUNT ? g_shell_ledger.defs[item].name : "?";
    const size_t available = heap->GetFreeSize();
    if (!vjo_paf_guard(item, what, available, bytes))
        return nullptr;
    void *p = sce_paf_memalign(16, bytes);
#if VJO_DEBUG
    vjo_log("%s Paf allocation: free=%u requested=%u reserve=%u success=%d",
            what, (unsigned)available, (unsigned)bytes, (unsigned)vjo_paf_guard_reserve(item), p != nullptr);
#else
    if (!p) /* the normal build logs a refusal only */
        vjo_log("%s Paf allocation refused: free=%u requested=%u", what, (unsigned)available, (unsigned)bytes);
#endif
    if (p) vjo_ledger_add(&g_shell_ledger, item, (uint32_t)bytes);
    return p;
}

extern "C" void *vjo_shell_heap_alloc(size_t bytes)
{
    return vjo_shell_heap_alloc_for(VJO_SL_PAF_RESULTS, bytes);
}

extern "C" void vjo_shell_heap_free(int item, void *p, size_t bytes)
{
    if (!p) return;
    sce_paf_free(p);
    vjo_ledger_sub(&g_shell_ledger, item, (uint32_t)bytes);
}
