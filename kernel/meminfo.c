/* The self-test's kernel memory sample (vjoMemInfo). API 18 asked one route
 * and the owner's 3.65 answered 0x80020001 at every step (#74), so API 19 asks
 * three, each reported with its own result and the step where it stopped
 * (include/vjo_api.h VJO_MEMSTEP_*), so that one console run says which works
 * on that firmware:
 *
 *  1. a memory block's partition: ksceKernelFindMemBlockByAddr(addr, 0) (size 0
 *     is "the block that contains addr" [wiki SceSysmem]; API 18 passed a size,
 *     which asks for a block that exactly fits the range) ->
 *     ksceKernelMemBlockGetInfoEx -> PhyMemPartInfoCore, for our module's block
 *     (the kernel's partition) and for the caller's out pointer;
 *  2. the caller's address space control block at +328/+332/+316 (main, CDRAM,
 *     phycont) and each partition's InfoCore: PSVshell's route on 3.60 and 3.65
 *     (src/perf.c:115-146), PSVshellPlus's (info.c:75-145);
 *  3. a process's address space info (SceSysmemForKernel_C3EF4055) and its
 *     partitions' InfoCore: faps-coredump's route with 3.65 NIDs
 *     (src/process_address_space.c:198-268, src/main.c:198-227); the kernel
 *     process first, the caller if that is refused.
 *
 * Every firmware-specific function is looked up at run time by library and
 * function NID through taiHEN's module_get_export_func, never imported, so a
 * firmware without it cannot stop this module from loading. Kernel objects
 * live below 0x40000000 (TTBR0; 0x40000000 and up is user memory: VitaSDK
 * psp2kern/kernel/sysmem/data_transfers.h, wiki Virtual_Memory). A partition
 * object pointer is handed to InfoCore when it is in PSVshellPlus's range,
 * 0x1000-0x1000000 (info.c:57, :91, the range it reads on 3.60-3.65), or higher
 * in the kernel half only when it lies in a kernel memory block; anything
 * else is refused, with its own step, before it is read. The firmware's
 * structures are read as 32-bit words, so this file also builds and runs in
 * the 64-bit host tests (host/tests/test_kernel_meminfo.c). Nothing here runs
 * at boot or in a hook: only the self-test asks. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>

#include "../include/vjo_ledger_budget.h"
#include "vjo_kernel.h"

/* taihenModuleUtils */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

typedef struct {
    uint32_t base;
    uint32_t total;
    uint32_t free;
    uint32_t unk;
} PartInfoCore;

typedef int (*part_info_fn)(void *part, PartInfoCore *out);
typedef void *(*cas_fn)(void);
typedef SceUID (*as_of_pid_fn)(SceUID pid);
typedef int (*as_info_fn)(SceUID asid, void *info);

/* faps-coredump src/process_address_space.c: 0x654 bytes on 3.60-3.65, every
 * pointer a 32-bit word. */
typedef struct {
    uint32_t size;
    SceUID asid;
    uint8_t context_id, pad[3];
    uint32_t n_list;
    struct {
        uint32_t size;
        uint32_t part;
        uint32_t vbase;
        uint32_t vsize, unk_10, vsize_remain;
        uint32_t name;
        uint32_t unk_1c, unk_20, unk_24, unk_28, unk_2c;
    } list[0x20];
    uint32_t n_phy_mem_part;
    uint32_t phy_mem_part[0x10];
} AsInfo;
typedef char as_info_is_0x654_bytes[sizeof(AsInfo) == 0x654 ? 1 : -1];
static AsInfo as_info; /* under g.lock: too large for a syscall stack */

/* The address space control block: PSVshell's offsets. */
#define CAS_MAIN 328
#define CAS_CDRAM 332
#define CAS_PHYCONT 316

static part_info_fn part_info;
static int part_info_rc; /* 0 found, else the lookup's last error */

#define KERNEL_HALF 0x40000000u
#define PSVSHELLPLUS_LOW 0x1000u
#define PSVSHELLPLUS_HIGH 0x1000000u

/* Library NID first, then function NID, for each firmware (wiki SceSysmem;
 * PSVshell src/main.c:361-366; faps-coredump src/main.c:198-227). */
static uintptr_t lookup(const char *module, uint32_t lib_a, uint32_t fn_a, uint32_t lib_b, uint32_t fn_b, int *rc)
{
    uintptr_t fn = 0;
    *rc = module_get_export_func(KERNEL_PID, module, lib_a, fn_a, &fn);
    if (*rc < 0 || !fn)
        *rc = module_get_export_func(KERNEL_PID, module, lib_b, fn_b, &fn);
    return *rc >= 0 ? fn : 0;
}

static part_info_fn resolve(void)
{
    if (!part_info)
        /* sceKernelGetPhyMemPartInfoCoreForKernel: 3.60-3.61, then 3.63-3.74 */
        part_info = (part_info_fn)lookup("SceSysmem", 0x63A519E5, 0x3650963F, 0x02451F0F, 0xB9B69700, &part_info_rc);
    return part_info;
}

/* Whether a kernel object pointer may be read: 0, or the VJO_MEMSTEP_* that
 * refuses it. */
static int kernel_object_ok(uintptr_t p)
{
    if (p <= PSVSHELLPLUS_LOW || p >= KERNEL_HALF || (p & 3u))
        return VJO_MEMSTEP_RANGE;
    if (p < PSVSHELLPLUS_HIGH)
        return 0;
    return ksceKernelFindMemBlockByAddr((const void *)p, 0) >= 0 ? 0 : VJO_MEMSTEP_NO_BLOCK;
}

/* InfoCore of the partition object at p: 0 and its figures, or the error with
 * *step set to where it stopped. */
static int part_read(uintptr_t p, uint32_t *total, uint32_t *free, int32_t *step)
{
    PartInfoCore core;
    part_info_fn fn = resolve();
    int rc;
    *total = *free = 0;
    if (!fn) { /* InfoCore not found on this firmware: its lookup's error */
        *step = VJO_MEMSTEP_CORE;
        return part_info_rc < 0 ? part_info_rc : VJO_ERR_FORMAT;
    }
    if ((rc = kernel_object_ok(p)) != 0) {
        *step = rc;
        return VJO_ERR_ARG;
    }
    memset(&core, 0, sizeof(core));
    rc = fn((void *)p, &core);
    if (rc < 0) {
        *step = VJO_MEMSTEP_CORE;
        return rc;
    }
    *total = core.total;
    *free = core.free;
    *step = 0;
    return 0;
}

/* Route 1 for the block holding addr (of process pid, or the kernel's for pid 0):
 * 0 and the partition's figures, or the error, with the failing step. */
static int route_block(SceUID pid, const void *addr, uint32_t *total, uint32_t *free, int32_t *step)
{
    SceKernelMemBlockInfoEx info;
    SceUID uid;
    int rc;
    *total = *free = 0;
    *step = VJO_MEMSTEP_FIND;
    uid = pid ? ksceKernelFindProcMemBlockByAddr(pid, addr, 0) : ksceKernelFindMemBlockByAddr(addr, 0);
    if (uid < 0)
        return uid;
    *step = VJO_MEMSTEP_INFO;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = ksceKernelMemBlockGetInfoEx(uid, &info);
    if (rc < 0)
        return rc;
    return part_read((uintptr_t)info.core_info.phy_mem_part, total, free, step);
}

static uint32_t word_at(uintptr_t base, unsigned offset)
{
    return *(const volatile uint32_t *)(base + offset);
}

/* Route 2: the current (calling) process's address space. */
static int route_cas(VjoMemInfo *out)
{
    static cas_fn cas;
    static int cas_rc;
    uintptr_t block;
    int32_t step;
    int rc;
    if (!cas)
        /* SceSysrootForKernel sceKernelSysrootGetCurrentAddressSpaceCBForKernel: the
         * library is the same on 3.60 and 3.63+ (vita-headers db), NID 0x63EBB05B on
         * 0.940-3.740 (wiki SceSysmem). */
        cas = (cas_fn)lookup("SceSysmem", 0x3691DA45, 0x63EBB05B, 0x3691DA45, 0x63EBB05B, &cas_rc);
    if (!cas) {
        out->cas_step = VJO_MEMSTEP_FIND;
        return cas_rc < 0 ? cas_rc : VJO_ERR_FORMAT;
    }
    block = (uintptr_t)cas();
    if ((rc = kernel_object_ok(block)) != 0) {
        out->cas_step = rc;
        return VJO_ERR_ARG;
    }
    rc = part_read(word_at(block, CAS_MAIN), &out->cas_main_total, &out->cas_main_free, &out->cas_step);
    if (rc < 0)
        return rc;
    /* none (0) is normal for some processes; a refused one reads as 0 */
    if (word_at(block, CAS_CDRAM))
        part_read(word_at(block, CAS_CDRAM), &out->cas_cdram_total, &out->cas_cdram_free, &step);
    if (word_at(block, CAS_PHYCONT))
        part_read(word_at(block, CAS_PHYCONT), &out->cas_phycont_total, &out->cas_phycont_free, &step);
    return 0;
}

/* Route 3: a process's address space and its partitions. g.lock held. */
static int route_as(SceUID caller_pid, VjoMemInfo *out)
{
    static as_of_pid_fn as_of_pid;
    static as_info_fn as_get;
    static int as_rc;
    SceUID asid;
    int rc;
    if (!as_of_pid)
        /* SceProcessmgrForKernel sceKernelGetProcessAddressSpace: 3.60, then 3.65 */
        as_of_pid = (as_of_pid_fn)lookup("SceProcessmgr", 0x7A69DE86, 0xC77C2085, 0xEB1F8EF7, 0x9BC44974, &as_rc);
    if (as_of_pid && !as_get)
        /* SceSysmemForKernel_C3EF4055: 3.60, then 3.63+ */
        as_get = (as_info_fn)lookup("SceSysmem", 0x63A519E5, 0x4492421F, 0x02451F0F, 0xC3EF4055, &as_rc);
    if (!as_of_pid || !as_get) {
        out->as_step = VJO_MEMSTEP_FIND;
        return as_rc < 0 ? as_rc : VJO_ERR_FORMAT;
    }
    /* faps-coredump asks for a user process; the kernel's is asked first (its
     * partitions include ScePhyMemPartKD), the caller's if that is refused. */
    asid = as_of_pid(KERNEL_PID);
    if (asid < 0 && caller_pid > 0) {
        asid = as_of_pid(caller_pid);
        out->as_caller = 1;
    }
    out->as_step = VJO_MEMSTEP_INFO;
    if (asid < 0)
        return asid;
    memset(&as_info, 0, sizeof(as_info));
    as_info.size = sizeof(as_info);
    rc = as_get(asid, &as_info);
    if (rc < 0)
        return rc;
    out->as_step = 0;
    /* every slot, the empty ones skipped, as faps-coredump reads them */
    for (uint32_t i = 0; i < 0x10 && out->as_parts < VJO_MEMINFO_PARTS; i++) {
        uint32_t total, free;
        int32_t step;
        if (!as_info.phy_mem_part[i])
            continue;
        if (part_read(as_info.phy_mem_part[i], &total, &free, &step) < 0) {
            out->as_step = step;
            out->as_skipped++;
            continue;
        }
        out->as_total[out->as_parts] = total;
        out->as_free[out->as_parts] = free;
        out->as_parts++;
    }
    return out->as_parts ? 0 : VJO_ERR_FORMAT;
}

void meminfo_fill(SceUID caller_pid, const void *caller_addr, VjoMemInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    /* our own module's pages name the kernel partition */
    out->kernel_rc = route_block(0, &g, &out->kernel_total, &out->kernel_free, &out->kernel_step);
    /* the caller's: the block that holds its out pointer */
    out->caller_rc = caller_addr ? route_block(caller_pid, caller_addr, &out->caller_total, &out->caller_free,
                                               &out->caller_step)
                                 : VJO_ERR_ARG;
    out->cas_rc = route_cas(out);
    VJO_LOCK();
    out->as_rc = route_as(caller_pid, out);
    out->ledger_current = g_kernel_ledger.pool_current[VJO_LEDGER_POOL_KERNEL];
    out->ledger_peak = g_kernel_ledger.pool_peak[VJO_LEDGER_POOL_KERNEL];
    VJO_UNLOCK();
    out->ledger_limit = VJO_LIMIT_KERNEL;
}
