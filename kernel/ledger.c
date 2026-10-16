/* The kernel plugin's memory ledger (core/ledger.h): what we hold in
 * ScePhyMemPartKD. The capture ladder's last rung (a kernel strip, buffers.c)
 * and a starting game's games.ini read (VjoPolicy, loader.c) are the only
 * blocks the kernel ever allocates (the full-frame VjoCapture went in API 24);
 * the worker thread's stack and the module's pages are the permanent part.
 * Printed to kernel.txt once a boot, at the kernel's start (kernel/main.c). */
#include "../core/ledger.h"
#include "../include/vjo_ledger_budget.h"
#include "vjo_kernel.h"

static const VjoLedgerItemDef defs[VJO_KL_COUNT] = {
    [VJO_KL_MODULE] = {"kernel module pages", VJO_LEDGER_POOL_KERNEL},
    [VJO_KL_STACK] = {"VjoWorker stack", VJO_LEDGER_POOL_KERNEL},
    [VJO_KL_OCR_STRIP] = {"VjoOcrStrip", VJO_LEDGER_POOL_KERNEL},
    [VJO_KL_POLICY] = {"VjoPolicy", VJO_LEDGER_POOL_KERNEL},
};

VjoLedger g_kernel_ledger;

void kledger_init(void)
{
    vjo_ledger_init(&g_kernel_ledger, defs, VJO_KL_COUNT);
    vjo_ledger_add(&g_kernel_ledger, VJO_KL_MODULE, VJO_BUDGET_KERNEL_MODULE_PAGES);
}

void kledger_log(const char *when)
{
    const VjoLedger *l = &g_kernel_ledger;
    int i;
    klog("ledger kernel: current=%u peak=%u limit=%u (%s)", l->pool_current[VJO_LEDGER_POOL_KERNEL],
         l->pool_peak[VJO_LEDGER_POOL_KERNEL], (unsigned)VJO_LIMIT_KERNEL, when);
    for (i = 0; i < VJO_KL_COUNT; i++)
        if (l->peak[i])
            klog_d("ledger   %s: current=%u peak=%u", defs[i].name, l->current[i], l->peak[i]);
}
