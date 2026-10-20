/* The debug build's read-only PSP probe (docs/native-hooks.md "PSP probe (debug build)";
 * reports/hooks-psp.md section 5). In the PSP emulator's process, once, at the first OCR job, and only
 * when the text source is not OCR, it appends to hooks.log what a later round's PSP hooks need to know:
 *   1. ScePspemu's module NID, and the memory block at each of the three Vita addresses of Adrenaline's
 *      view of PSP RAM (core/psp_probe.h VJO_PSP_VIEW_*): sceKernelFindMemBlockByAddr's UID and
 *      sceKernelGetMemBlockInfoByAddr's base, size, memory type, access and block type. It stops there
 *      unless all three are found and readable;
 *   2. SceAdrenaline's 12-byte titleid, read only when it lies inside a block step 1 found;
 *   3. when Luna's PSP table lists that ID: at each of its sites (at most 16), the four words from
 *      target - 8 to target + 4, each read only when it lies inside a found block. target is Luna's
 *      PPSSPP address in the view, the main module assumed at PPSSPP's 0x08804000.
 * Raw values only: what the words are (a function's entry, a return point, a delay slot) is for a host
 * script that reads the log. It only reads: no write to the game's or the PSP's memory, no cache
 * operation, no call into ScePspemu, no allocation, no thread, nothing at module_start, and it is built
 * into the debug build's worker only. Report section 5's fourth step, timing a 1 MiB read, is left out
 * (docs/native-hooks.md says why). Each line is written before the next read, so a fault would leave
 * the log ending at the step that caused it. */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <taihen.h>

#include "hooks.h"
#include "psp_probe.h"
#include "../core/psp_probe.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"

#if !VJO_DEBUG
#error "the PSP probe belongs to the debug build only (vita/CMakeLists.txt)"
#endif

_Static_assert(SCE_KERNEL_MEMORY_ACCESS_R == VJO_PSP_ACCESS_R, "the readable bit core/psp_probe.c checks");

#define PROBE_LOG "ux0:data/VitaJPOverlay/hooks.log" /* game_ocr/hooks.c's, rewritten at each game start */

static SceUID plog = -1;

static void put(const char *line, size_t n)
{
    if (plog >= 0 && n) sceIoWrite(plog, line, (SceSize)n);
}

/* What the two lookups say about Vita address addr, raw. */
static void find_block(uint32_t addr, VjoPspBlock *b)
{
    SceKernelMemBlockInfo mb;
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

static void probe(uint32_t mode)
{
    static const uint32_t views[VJO_PSP_BLOCKS] = {VJO_PSP_VIEW_RAM, VJO_PSP_VIEW_USER, VJO_PSP_VIEW_ADRENALINE};
    const uint32_t tid = VJO_PSP_VIEW_ADRENALINE + VJO_PSP_TITLEID_OFFSET;
    VjoPspBlock blocks[VJO_PSP_BLOCKS];
    tai_module_info_t tai;
    char line[VJO_PSP_LINE_MAX], id[10];
    uint8_t raw[VJO_PSP_TITLEID_BYTES];
    uint32_t sites[VJO_PSP_SITES_MAX];
    unsigned total = 0, n, ok = 1;
    int rc;

    put(line, vjo_psp_line_start(line, sizeof(line), mode));
    /* 1. which ScePspemu build this is, and the view's three blocks */
    sceClibMemset(&tai, 0, sizeof(tai));
    tai.size = sizeof(tai);
    rc = taiGetModuleInfo("ScePspemu", &tai);
    put(line, vjo_psp_line_module(line, sizeof(line), rc, rc < 0 ? 0u : tai.module_nid));
    for (unsigned i = 0; i < VJO_PSP_BLOCKS; i++) {
        find_block(views[i], &blocks[i]);
        put(line, vjo_psp_line_block(line, sizeof(line), &blocks[i]));
        if (!vjo_psp_block_ok(&blocks[i])) ok = 0;
    }
    if (!ok) {
        put(line, vjo_psp_line_end(line, sizeof(line), "a block is missing or not readable"));
        return;
    }
    /* 2. the running game's ID, as Adrenaline's PSP side wrote it */
    if (!vjo_psp_inside(blocks, VJO_PSP_BLOCKS, tid, VJO_PSP_TITLEID_BYTES)) {
        put(line, vjo_psp_line_titleid(line, sizeof(line), tid, 0, NULL));
        put(line, vjo_psp_line_end(line, sizeof(line), "the title ID is outside the blocks"));
        return;
    }
    for (unsigned i = 0; i < VJO_PSP_TITLEID_BYTES; i++) raw[i] = ((const volatile uint8_t *)(uintptr_t)tid)[i];
    put(line, vjo_psp_line_titleid(line, sizeof(line), tid, 1, raw));
    /* 3. Luna's sites of that ID: four words around each (an ID that is not one is "not listed") */
    vjo_psp_title_id(raw, id);
    n = vjo_psp_luna_sites(id, sites, VJO_PSP_SITES_MAX, &total);
    put(line, vjo_psp_line_sites(line, sizeof(line), id, total, n));
    for (unsigned s = 0; s < n; s++) {
        uint32_t target = vjo_psp_vita_addr(sites[s]), words[VJO_PSP_SITE_WORDS] = {0};
        unsigned mask = 0;
        for (unsigned k = 0; k < VJO_PSP_SITE_WORDS; k++) {
            uint32_t a = target - 8u + 4u * k;
            if (!vjo_psp_inside(blocks, VJO_PSP_BLOCKS, a, 4)) continue;
            words[k] = *(const volatile uint32_t *)(uintptr_t)a; /* aligned: Luna's addresses are */
            mask |= 1u << k;
        }
        put(line, vjo_psp_line_site(line, sizeof(line), sites[s], target, words, mask));
    }
    put(line, vjo_psp_line_end(line, sizeof(line), NULL));
}

void vjo_psp_probe_job(int psp)
{
    static int done;
    uint32_t mode = VJO_TEXT_SOURCE_OCR;
    if (!psp || done) return;
    done = 1; /* once per process, whatever it finds */
    vjo_hooks_counts(&mode, NULL, NULL); /* the text source the worker started with */
    if (mode == VJO_TEXT_SOURCE_OCR) return;
    if (!VJO_IMPORT_READY(sceIoOpen) || !VJO_IMPORT_READY(sceIoWrite) || !VJO_IMPORT_READY(sceIoClose) ||
        !VJO_IMPORT_READY(taiGetModuleInfo) || !VJO_IMPORT_READY(sceKernelFindMemBlockByAddr) ||
        !VJO_IMPORT_READY(sceKernelGetMemBlockInfoByAddr))
        return;
    plog = sceIoOpen(PROBE_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (plog < 0) return; /* nowhere to say what it finds: no probe */
    probe(mode);
    sceIoClose(plog);
    plog = -1;
}
