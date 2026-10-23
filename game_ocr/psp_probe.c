/* The debug build's read-only PSP probe (docs/native-hooks.md "PSP probe (debug build)";
 * reports/hooks-psp.md section 5): the worker's half. core/psp_probe.c decides what to read and writes every
 * line; this file gives it the reads, in the PSP emulator's process, and the log file. After a job whose
 * request carries VJO_GAME_OCR_S_PSP_PROBE (config.ini psp_probe, the debug build's Shell), once its result is
 * with the kernel, it appends to ux0:data/VitaJPOverlay/psp_probe.log:
 *   1. ScePspemu's module NID, modid and code segment (taiGetModuleInfo, sceKernelGetModuleInfo: the module
 *      manager's description, nothing in ScePspemu is called), and the memory block at each of the three Vita
 *      addresses of Adrenaline's view of PSP RAM (core/psp_probe.h VJO_PSP_VIEW_*):
 *      sceKernelFindMemBlockByAddr's UID and sceKernelGetMemBlockInfoByAddr's base, size, memory type (its
 *      cache policy), access and block type. It stops there unless all three are found and readable;
 *   2. SceAdrenaline's 12-byte titleid, read only when it lies inside a block step 1 found;
 *   3. when Luna's PSP table lists that ID: at each of its sites, the four words from target - 8 to
 *      target + 4, each read only when it lies inside a found block (the main module assumed at PPSSPP's
 *      0x08804000), and the MIPS classifier's class of each;
 *   4. once per process: the time of one 1 MiB read of the view at 0x70800000, after each of its 256 pages'
 *      lookups names the block that holds it;
 * again whenever what step 1's lookups or the title ID show changes (Adrenaline's XMB, then a game), and for
 * every such job a line of its capture and one of its OCR. It only reads: no write to the game's or the PSP's
 * memory, no cache operation, no call into ScePspemu, no allocation, no thread, nothing at module_start. Each
 * line is written before the next read, so a fault would leave the log ending at the step before it. */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <taihen.h>

#include "psp_probe.h"
#include "../core/psp_probe.h"
#include "../include/vjo_import.h"

#if !VJO_DEBUG
#error "the PSP probe belongs to the debug build only (vita/CMakeLists.txt)"
#endif

_Static_assert(SCE_KERNEL_MEMORY_ACCESS_R == VJO_PSP_ACCESS_R, "the readable bit core/psp_probe.c checks");
_Static_assert((int32_t)SCE_KERNEL_ERROR_INVALID_UID == VJO_PSP_INVALID_UID, "the shared-mapping UID error");
_Static_assert(SCE_KERNEL_MEMORY_TYPE_NORMAL == VJO_PSP_MEMTYPE_CACHED, "the cached memory type");
_Static_assert(SCE_KERNEL_MEMORY_TYPE_NORMAL_NC == VJO_PSP_MEMTYPE_UNCACHED, "the uncached memory type");

/* Its own file, appended to: a log longer than PROBE_LOG_MAX at a process's first open starts again. */
#define PROBE_LOG "ux0:data/VitaJPOverlay/psp_probe.log"
#define PROBE_LOG_MAX (1024 * 1024)

static VjoPspProbe probe; /* this process's: zero to begin with */
static SceUID plog = -1;
static int log_opened; /* the process's first open is past */

static void op_put(void *ud, const char *line, size_t n)
{
    (void)ud;
    if (plog >= 0 && n) sceIoWrite(plog, line, (SceSize)n);
}

static void op_module(void *ud, VjoPspModule *m)
{
    tai_module_info_t tai;
    SceKernelModuleInfo mi;
    (void)ud;
    sceClibMemset(&tai, 0, sizeof(tai));
    tai.size = sizeof(tai);
    m->rc = taiGetModuleInfo("ScePspemu", &tai);
    if (m->rc < 0) return;
    m->nid = tai.module_nid;
    m->modid = tai.modid;
    m->info_rc = -1; /* not asked: the module manager's import is not resolved */
    if (!VJO_IMPORT_READY(sceKernelGetModuleInfo)) return;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    m->info_rc = sceKernelGetModuleInfo(tai.modid, &mi);
    if (m->info_rc < 0) return;
    m->seg0 = (uint32_t)(uintptr_t)mi.segments[0].vaddr;
    m->seg0_size = mi.segments[0].memsz;
}

/* What the two lookups say about Vita address addr, raw. */
static void op_block(void *ud, uint32_t addr, VjoPspBlock *b)
{
    SceKernelMemBlockInfo mb;
    (void)ud;
    sceClibMemset(b, 0, sizeof(*b));
    b->addr = addr;
    b->uid = sceKernelFindMemBlockByAddr((const void *)(uintptr_t)addr, 0);
    sceClibMemset(&mb, 0, sizeof(mb));
    mb.size = sizeof(mb);
    b->info_rc = sceKernelGetMemBlockInfoByAddr((void *)(uintptr_t)addr, &mb);
    if (b->info_rc < 0) return;
    b->base = (uint32_t)(uintptr_t)mb.mappedBase;
    b->size = mb.mappedSize;
    b->memtype = (uint32_t)mb.memoryType;
    b->access = mb.access;
    b->type = (uint32_t)mb.type;
}

static int32_t op_find(void *ud, uint32_t addr)
{
    (void)ud;
    return sceKernelFindMemBlockByAddr((const void *)(uintptr_t)addr, 0);
}

/* The reads: the runner asks only for ranges inside the blocks it found (core/psp_probe.c). */
static uint32_t op_word(void *ud, uint32_t addr)
{
    (void)ud;
    return *(const volatile uint32_t *)(uintptr_t)addr;
}

static void op_bytes(void *ud, uint32_t addr, uint8_t *out, uint32_t n)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    (void)ud;
    for (uint32_t i = 0; i < n; i++) out[i] = p[i];
}

/* n / 4 aligned 32-bit loads, four at a time (n is a multiple of 16: core/psp_probe.c), summed so that none
 * can be left out */
static uint32_t op_sum(void *ud, uint32_t addr, uint32_t n)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)addr;
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    (void)ud;
    for (uint32_t i = 0; i + 4u <= n / 4u; i += 4u) {
        s0 += p[i];
        s1 += p[i + 1u];
        s2 += p[i + 2u];
        s3 += p[i + 3u];
    }
    return s0 + s1 + s2 + s3;
}

static uint64_t op_now(void *ud)
{
    (void)ud;
    return sceKernelGetProcessTimeWide();
}

/* The log, open for one job's lines; 1 when it is. The process's first open writes its "process" line. */
static int open_log(const char *title)
{
    char line[VJO_PSP_LINE_MAX];
    plog = sceIoOpen(PROBE_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (plog < 0) return 0;
    if (log_opened) return 1;
    log_opened = 1;
    if (VJO_IMPORT_READY(sceIoLseek) && sceIoLseek(plog, 0, SCE_SEEK_END) > PROBE_LOG_MAX) {
        sceIoClose(plog);
        plog = sceIoOpen(PROBE_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (plog < 0) return 0;
    }
    op_put(NULL, line, vjo_psp_line_process(line, sizeof(line), title));
    return 1;
}

void vjo_psp_probe_after(int psp, const char *title, const VjoGameOcrRequest *r, const VjoGameOcrResult *res,
                         uint32_t job_us)
{
    static const VjoPspOps ops = {NULL, op_module, op_block, op_find, op_word, op_bytes, op_sum, op_now, op_put};
    if (!psp || !r || !(r->speed & VJO_GAME_OCR_S_PSP_PROBE) || (r->flags & VJO_GAME_OCR_F_SHOT)) return;
    if (!VJO_IMPORT_READY(sceIoOpen) || !VJO_IMPORT_READY(sceIoWrite) || !VJO_IMPORT_READY(sceIoClose) ||
        !VJO_IMPORT_READY(taiGetModuleInfo) || !VJO_IMPORT_READY(sceKernelFindMemBlockByAddr) ||
        !VJO_IMPORT_READY(sceKernelGetMemBlockInfoByAddr))
        return;
    if (!open_log(title)) return; /* nowhere to say what it finds: no probe, no read */
    vjo_psp_probe_job(&probe, &ops, res, r->flags, r->speed, job_us);
    sceIoClose(plog);
    plog = -1;
}
