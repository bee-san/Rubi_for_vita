/* The kernel's one pixel buffer: the capture ladder's last rung, one strip of a job's
 * rows (below). OCR and the Anki screenshot read the screen in the game's process
 * (include/vjo_game_ocr.h): the kernel keeps no copy of a frame. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>

#include "../include/vjo_game_ocr.h"
#include "vjo_kernel.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* The last rung of the capture ladder (game_ocr.c vjoOcrCaptureRows,
 * VJO_GAME_OCR_SRC_KSTRIP): one strip of a job's rows (grey, or a shot's colour
 * rows, API 23), at most VJO_GAME_OCR_KSTRIP_MAX, held for one call and freed
 * before it returns; a copy the call abandoned keeps it until the copy ends, when
 * the kernel worker frees it. Capture lock held. */
static SceUID kstrip_uid = -1;
static uint8_t *kstrip_base;
static uint32_t kstrip_bytes;

int buffers_kstrip_alloc(uint32_t bytes)
{
    void *base = NULL;
    SceUID uid;
    if (!bytes || bytes > VJO_GAME_OCR_KSTRIP_MAX || kstrip_uid >= 0)
        return VJO_ERR_ARG;
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
