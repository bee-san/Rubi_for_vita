/* The SceShell plugin's memory ledger (core/ledger.h): what we hold in
 * SceShell's partition and in its PAF heap, by item, with the printout that
 * goes to status.txt. Every thread the Shell creates goes through
 * vjo_shell_thread_create, every PAF allocation through shell/memory.cpp;
 * tools/check_memory_ledger.py fails the build on any other site. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "../core/ledger.h"
#include "../include/vjo_ledger_budget.h"
#include "shell.h"

static const VjoLedgerItemDef defs[VJO_SL_COUNT] = {
    [VJO_SL_MODULE] = {"Shell pages", VJO_LEDGER_POOL_SHELL_PAGES},
    [VJO_SL_WORKER_MODULE] = {"OCR worker pages", VJO_LEDGER_POOL_SHELL_PAGES},
    [VJO_SL_NET_STACK] = {"VjoNet stack", VJO_LEDGER_POOL_SHELL_PAGES},
    [VJO_SL_CTL_STACK] = {"VjoControl stack", VJO_LEDGER_POOL_SHELL_PAGES},
    [VJO_SL_ANKI_STACK] = {"VjoAnki stack", VJO_LEDGER_POOL_SHELL_PAGES},
    [VJO_SL_USERRW_RESULTS] = {"VjoShellMem", VJO_LEDGER_POOL_SHELL_PAGES},
    [VJO_SL_PAF_PLUGIN] = {"Paf plugin (est.)", VJO_LEDGER_POOL_SHELL_PAF},
    [VJO_SL_PAF_RESULTS] = {"result arenas", VJO_LEDGER_POOL_SHELL_PAF},
    [VJO_SL_PAF_ANKI] = {"Anki arena", VJO_LEDGER_POOL_SHELL_PAF},
    [VJO_SL_PAF_UI] = {"overlay text arena", VJO_LEDGER_POOL_SHELL_PAF},
};

VjoLedger g_shell_ledger;

void vjo_ledger_shell_init(void)
{
    vjo_ledger_init(&g_shell_ledger, defs, VJO_SL_COUNT);
    /* The permanent items, as declared in the budget (CI checks the packaged
     * modules against the declared pages). */
    vjo_ledger_add(&g_shell_ledger, VJO_SL_MODULE, VJO_BUDGET_SHELL_MODULE_PAGES);
    vjo_ledger_add(&g_shell_ledger, VJO_SL_WORKER_MODULE, VJO_BUDGET_WORKER_IN_SHELL_PAGES); /* 0: NO_RESIDENT */
    vjo_ledger_add(&g_shell_ledger, VJO_SL_PAF_PLUGIN, VJO_BUDGET_PAF_PLUGIN);
}

/* SceShell's free USER memory as the user API reports it (the free size minus a
 * system margin, so it can be negative [SDK: Kernel-Reference]) and the query's
 * rc, for a refusal's log line. The host tests have no SceShell partition: they
 * set these two. */
#ifdef VJO_HOST
int vjo_test_shell_free_rc = -1, vjo_test_shell_free_user;
#endif
static int shell_free_user(int *bytes)
{
#ifdef VJO_HOST
    *bytes = vjo_test_shell_free_rc < 0 ? 0 : vjo_test_shell_free_user;
    return vjo_test_shell_free_rc;
#else
    SceKernelFreeMemorySizeInfo f;
    int rc;
    sceClibMemset(&f, 0, sizeof(f));
    f.size = sizeof(f);
    rc = sceKernelGetFreeMemorySize(&f);
    *bytes = rc < 0 ? 0 : f.size_user;
    return rc;
#endif
}

SceUID vjo_shell_thread_create(int item, const char *name, int (*entry)(SceSize, void *), unsigned stack)
{
    SceUID uid = sceKernelCreateThread(name, entry, 0x10000100, stack, 0, 0, NULL);
    if (uid >= 0) {
        vjo_ledger_add(&g_shell_ledger, item, stack + VJO_LEDGER_TLS);
    } else {
        /* SceShell's partition said no: its rc, what we asked for, its free USER
         * and what we hold there, so the collected logs decide it. */
        int free_user, rc = shell_free_user(&free_user);
        vjo_log("thread %s refused 0x%08X: %u B; SceShell free %d (0x%08X), held %u", name, (unsigned)uid,
                stack + VJO_LEDGER_TLS, free_user, (unsigned)rc,
                (unsigned)g_shell_ledger.pool_current[VJO_LEDGER_POOL_SHELL_PAGES]);
    }
    return uid;
}

int vjo_shell_thread_delete(int item, SceUID uid, unsigned stack)
{
    int rc = sceKernelDeleteThread(uid);
    if (rc >= 0)
        vjo_ledger_sub(&g_shell_ledger, item, stack + VJO_LEDGER_TLS);
    return rc;
}

/* The Shell makes no sceKernelAllocMemBlock after boot on the Vita
 * (AGENTS.md rule 1); this path serves the host tests' USER_RW result memory. */
SceUID vjo_shell_memblock_alloc(int item, const char *name, size_t bytes, void **base)
{
    SceUID uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (unsigned)bytes, NULL);
    *base = NULL;
    if (uid < 0)
        return uid;
    if (sceKernelGetMemBlockBase(uid, base) < 0 || !*base) {
        sceKernelFreeMemBlock(uid);
        return -1;
    }
    vjo_ledger_add(&g_shell_ledger, item, (uint32_t)bytes);
    return uid;
}

void vjo_shell_memblock_free(int item, SceUID uid, size_t bytes)
{
    if (uid < 0)
        return;
    sceKernelFreeMemBlock(uid);
    vjo_ledger_sub(&g_shell_ledger, item, (uint32_t)bytes);
}

/* status.txt: one line per pool (the PAF one with the heap's free size, 0
 * when Paf is not ready) and one per item ever used. Stacks include TLS. */
void vjo_ledger_shell_log(const char *when)
{
    const VjoLedger *l = &g_shell_ledger;
    VjoLogBatch batch;
    char batch_buf[1024]; /* the table, about 10 lines of 50 B: one append per file */
    int i;
    vjo_log_batch_begin(&batch, batch_buf, sizeof(batch_buf));
    vjo_log("ledger %s pages: current=%u peak=%u limit=%u", when,
            (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAGES], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAGES],
            (unsigned)VJO_LIMIT_SHELL_PAGES);
    vjo_log("ledger %s paf: current=%u peak=%u limit=%u free=%u", when,
            (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAF], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAF],
            (unsigned)VJO_LIMIT_SHELL_PAF, (unsigned)vjo_shell_heap_free_size());
    for (i = 0; i < VJO_SL_COUNT; i++)
        if (l->peak[i])
            vjo_log("ledger   %s: current=%u peak=%u", defs[i].name, (unsigned)l->current[i], (unsigned)l->peak[i]);
    vjo_log_batch_end(&batch);
}
