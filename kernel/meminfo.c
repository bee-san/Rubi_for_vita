/* The self-test's kernel memory sample (vjoMemInfo): the exact free and
 * total bytes of the partition our kernel module lives in (ScePhyMemPartKD,
 * if the total reads 36 MiB) and of the caller's partition (SceShell's when
 * the Shell asks), plus the kernel ledger.
 *
 * The user API sceKernelGetFreeMemorySize sees only the caller's budget,
 * minus a system margin; the kernel's own pool has no user view at all. The
 * partition of any memory block we can name is read with
 * sceKernelGetPhyMemPartInfoCoreForKernel (its NID changed at 3.63, so it is
 * looked up at run time by library and function NID, the way PSVshell does;
 * no import). Nothing here runs at boot: only the self-test asks. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>

#include "../include/vjo_ledger_budget.h"
#include "vjo_kernel.h"

/* taihenModuleUtils */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

typedef struct {
    uintptr_t base;
    uint32_t total;
    uint32_t free;
    uint32_t unk;
} PartInfoCore;

typedef int (*part_info_fn)(void *part, PartInfoCore *out);

static part_info_fn part_info;
static int part_info_rc; /* 0 found, else the lookup's last error */

/* SceSysmemForKernel sceKernelGetPhyMemPartInfoCoreForKernel: 0.990-3.61
 * library 0x63A519E5 function 0x3650963F; 3.63-3.74 library 0x02451F0F
 * function 0xB9B69700 (wiki SceSysmem; PSVshell src/main.c). */
static part_info_fn resolve(void)
{
    uintptr_t fn = 0;
    if (part_info)
        return part_info;
    part_info_rc = module_get_export_func(KERNEL_PID, "SceSysmem", 0x63A519E5, 0x3650963F, &fn);
    if (part_info_rc < 0 || !fn)
        part_info_rc = module_get_export_func(KERNEL_PID, "SceSysmem", 0x02451F0F, 0xB9B69700, &fn);
    if (part_info_rc >= 0 && fn)
        part_info = (part_info_fn)fn;
    return part_info;
}

/* The partition of memblock uid: 0 and total/free, or the error. */
static int partition_of(SceUID uid, uint32_t *total, uint32_t *free)
{
    SceKernelMemBlockInfoEx info;
    PartInfoCore core;
    part_info_fn fn = resolve();
    int rc;
    *total = *free = 0;
    if (!fn)
        return part_info_rc < 0 ? part_info_rc : VJO_ERR_FORMAT;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = ksceKernelMemBlockGetInfoEx(uid, &info);
    if (rc < 0)
        return rc;
    if (!info.core_info.phy_mem_part)
        return VJO_ERR_FORMAT;
    memset(&core, 0, sizeof(core));
    rc = fn(info.core_info.phy_mem_part, &core);
    if (rc < 0)
        return rc;
    *total = core.total;
    *free = core.free;
    return 0;
}

void meminfo_fill(SceUID caller_pid, const void *caller_addr, VjoMemInfo *out)
{
    SceUID uid;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    /* our own module's pages name the kernel partition */
    uid = ksceKernelFindMemBlockByAddr(&g, sizeof(g));
    out->kernel_rc = uid < 0 ? uid : partition_of(uid, &out->kernel_total, &out->kernel_free);
    /* the caller's: the block that holds its out pointer */
    uid = caller_addr ? ksceKernelFindProcMemBlockByAddr(caller_pid, caller_addr, sizeof(*out)) : -1;
    out->caller_rc = uid < 0 ? uid : partition_of(uid, &out->caller_total, &out->caller_free);
    out->ledger_current = g_kernel_ledger.pool_current[VJO_LEDGER_POOL_KERNEL];
    out->ledger_peak = g_kernel_ledger.pool_peak[VJO_LEDGER_POOL_KERNEL];
    out->ledger_limit = VJO_LIMIT_KERNEL;
    out->capture_bytes = g.raw_capacity;
}
