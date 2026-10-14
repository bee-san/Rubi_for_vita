/* Capture buffer, allocated on request for the selected region: the Shell's
 * JPEG (Lens, an Anki screenshot) only. JPEG readers release it before the Lens
 * upload begins. OCR takes no kernel buffer (game_ocr.c), except the small
 * strip below on the last rung of its fallback ladder. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>

#include "../include/vjo_game_ocr.h"
#include "../include/vjo_ledger_budget.h"
#include "vjo_kernel.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* Raw A8B8G8R8 rows of the requested region. Plain kernel main memory: only
 * the CPU touches it (the display hook writes it, vjoReadRaw copies it out),
 * so it needs neither physical contiguity nor cache maintenance. */
#define RAW_SIZE ALIGN(VJO_BUDGET_KERNEL_CAPTURE, 0x1000) /* 960 x 544 x 4 */

int buffers_alloc(uint32_t bytes)
{
    void *base = NULL;
    SceUID uid;

    if (!bytes || bytes > RAW_SIZE || g.capture_state != CAPTURE_IDLE)
        return VJO_ERR_ARG;
    bytes = ALIGN(bytes, 0x1000);
    if (g.mem_uid > 0 && g.raw && g.raw_capacity == bytes)
        return 0;
    buffers_free();
    g.raw_valid = 0;
    uid = ksceKernelAllocMemBlock("VjoCapture", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, bytes, NULL);
    if (uid < 0) {
        klog("alloc capture buffer (%u KiB) failed 0x%08X", bytes >> 10, uid);
        g.alloc_status = VJO_ALLOC_FAIL;
        return uid;
    }
    if (ksceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        ksceKernelFreeMemBlock(uid);
        g.alloc_status = VJO_ALLOC_FAIL;
        klog("capture buffer base unavailable");
        return VJO_ERR_NO_MEMORY;
    }
    g.mem_uid = uid;
    g.raw = (uint8_t *)base;
    g.raw_capacity = bytes;
    g.alloc_status = VJO_ALLOC_OK;
    vjo_ledger_add(&g_kernel_ledger, VJO_KL_CAPTURE, bytes);
    klog("capture buffer: %u KiB at %p", bytes >> 10, base);
    kledger_log("capture");
    return 0;
}

void buffers_free(void)
{
    if (g.mem_uid > 0) {
        ksceKernelFreeMemBlock(g.mem_uid);
        vjo_ledger_sub(&g_kernel_ledger, VJO_KL_CAPTURE, g.raw_capacity);
        klog("capture buffer freed");
    }
    g.mem_uid = 0;
    g.raw = NULL;
    g.raw_capacity = 0;
    g.alloc_status = VJO_ALLOC_NONE;
}

void buffers_read_done(uint32_t row, uint32_t rows)
{
    /* JPEG reads each row once; local ncnn OCR rereads rows in multiple passes.
     * Release under the capture lock after the final successful user copy,
     * before the shell starts Lens upload. */
    if (g.capture_once && g.raw_valid && row < g.crop_h && rows >= g.crop_h-row) {
        g.raw_valid = 0;
        g.capture_full = 0;
        g.capture_once = 0;
        buffers_free();
    }
}

/* The last rung of the OCR capture ladder (game_ocr.c vjoOcrCaptureRows,
 * VJO_GAME_OCR_SRC_KSTRIP): one strip of grey rows, at most
 * VJO_GAME_OCR_KSTRIP_MAX, held only for one call and freed before it returns.
 * Capture lock held. */
static SceUID kstrip_uid = -1;
static uint8_t *kstrip_base;
static uint32_t kstrip_bytes;

int buffers_kstrip_alloc(uint32_t bytes)
{
    void *base = NULL;
    SceUID uid;
    if (!bytes || bytes > VJO_GAME_OCR_KSTRIP_MAX || kstrip_uid >= 0)
        return VJO_ERR_ARG;
    /* Never together with the JPEG capture buffer: the kernel's worst case is
     * the larger of the two (include/vjo_ledger_budget.h). */
    if (g.mem_uid > 0)
        return VJO_ERR_NO_MEMORY;
    bytes = ALIGN(bytes, 0x1000);
    uid = ksceKernelAllocMemBlock("VjoOcrStrip", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, bytes, NULL);
    if (uid < 0) {
        klog("alloc ocr kernel strip (%u B) failed 0x%08X", bytes, uid);
        return uid;
    }
    if (ksceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        ksceKernelFreeMemBlock(uid);
        return VJO_ERR_NO_MEMORY;
    }
    kstrip_uid = uid;
    kstrip_base = (uint8_t *)base;
    kstrip_bytes = bytes;
    vjo_ledger_add(&g_kernel_ledger, VJO_KL_OCR_STRIP, bytes);
    return 0;
}

uint8_t *buffers_kstrip(void)
{
    return kstrip_base;
}

void buffers_kstrip_free(void)
{
    if (kstrip_uid >= 0) {
        ksceKernelFreeMemBlock(kstrip_uid);
        vjo_ledger_sub(&g_kernel_ledger, VJO_KL_OCR_STRIP, kstrip_bytes);
    }
    kstrip_uid = -1;
    kstrip_base = NULL;
    kstrip_bytes = 0;
}
