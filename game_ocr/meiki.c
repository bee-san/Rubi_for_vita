/* Optional single-line Meiki runner. All caller buffers are private, identity-
 * checked game USER_RW blocks. MNN/newlib live only in the isolated module. */
#include "meiki.h"
#include "../include/vjo_import.h"
#include "meiki_bridge.h"
#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdarg.h>
#define USER_MAX_BYTES (512u * 1024u * 1024u)
#ifdef VJO_HOST
#define MEIKI_IMPORT_READY(fn) 1
#else
#define MEIKI_IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif
typedef struct {
    SceUID uid;
    void *base;
    uint32_t bytes;
    int free_accepted;
} OwnedBlock;
static OwnedBlock blocks[3] = {{.uid = -1}, {.uid = -1}, {.uid = -1}};
static VjoMeikiBridge bridge = {.module = -1};
/* No game namespace file I/O and no allocation. TTY availability is a native
 * observation boundary; free USER is reported, total/maxchunk unavailable. */
void vjo_log(const char *format, ...)
{
    char line[192];
    va_list args;
    va_start(args, format);
    int rc = sceClibVsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (rc >= 0)
        sceClibPrintf("%s\n", line);
}
static int uid_invalid(int rc)
{
    return (uint32_t)rc == (uint32_t)SCE_KERNEL_ERROR_INVALID_UID ||
           (uint32_t)rc == (uint32_t)SCE_KERNEL_ERROR_UNKNOWN_UID;
}

static int memory_imports_ready(void)
{
    return MEIKI_IMPORT_READY(sceKernelAllocMemBlock) &&
           MEIKI_IMPORT_READY(sceKernelGetMemBlockBase) &&
           MEIKI_IMPORT_READY(sceKernelGetMemBlockInfoByRange) &&
           MEIKI_IMPORT_READY(sceKernelFreeMemBlock) &&
           MEIKI_IMPORT_READY(sceKernelGetFreeMemorySize);
}

static int release_owned(OwnedBlock *block)
{
    if (block->uid < 0)
        return VJO_OK;
    if (!memory_imports_ready())
        return VJO_E_OCR_UNAVAILABLE;
    /* Once Free succeeded, never free that UID again: a still-valid lookup
     * could be a reused object. Only definitive invalidation clears ownership. */
    if (!block->free_accepted) {
        void *current = NULL;
        int rc = sceKernelGetMemBlockBase(block->uid, &current);
        if (uid_invalid(rc)) {
            block->uid = -1;
            block->base = NULL;
            block->bytes = 0;
            block->free_accepted = 0;
            return VJO_OK;
        }
        if (rc < 0 || (block->base && current != block->base)) {
            vjo_log("USER release identity failed uid=0x%08X rc=0x%08X", block->uid, rc);
            return VJO_E_OCR_UNAVAILABLE;
        }
        rc = sceKernelFreeMemBlock(block->uid);
        if (rc < 0) {
            vjo_log("USER free failed uid=0x%08X bytes=%u rc=0x%08X", block->uid, block->bytes, rc);
            return VJO_E_OCR_UNAVAILABLE;
        }
        block->free_accepted = 1;
    }
    void *current = NULL;
    int rc = sceKernelGetMemBlockBase(block->uid, &current);
    if (!uid_invalid(rc)) {
        vjo_log("USER free invalidation unproved uid=0x%08X rc=0x%08X", block->uid, rc);
        return VJO_E_OCR_UNAVAILABLE;
    }
    vjo_log("USER released uid=0x%08X bytes=%u lookup=0x%08X", block->uid, block->bytes, rc);
    block->uid = -1;
    block->base = NULL;
    block->bytes = 0;
    block->free_accepted = 0;
    return VJO_OK;
}

static void *owned_alloc(void *ud, size_t bytes)
{
    (void)ud;
    OwnedBlock *block = NULL;
    if (!bytes || bytes > USER_MAX_BYTES - 4095u || !memory_imports_ready())
        return NULL;
    for (unsigned i = 0; i < 3; i++)
        if (blocks[i].uid < 0) {
            block = &blocks[i];
            break;
        }
    if (!block)
        return NULL;
    block->bytes = ((uint32_t)bytes + 4095u) & ~4095u;
    block->free_accepted = 0;
    block->uid = sceKernelAllocMemBlock("VjoGameOcrData", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                        block->bytes, NULL);
    vjo_log("USER alloc requested=%u rounded=%u uid=0x%08X", (unsigned)bytes, block->bytes,
            block->uid);
    if (block->uid < 0) {
        block->bytes = 0;
        return NULL;
    }
    int rc = sceKernelGetMemBlockBase(block->uid, &block->base);
    SceKernelMemBlockInfo info = {.size = sizeof(info)};
    if (rc >= 0 && block->base && !((uintptr_t)block->base & 63u))
        rc = sceKernelGetMemBlockInfoByRange(block->base, block->bytes, &info);
    else
        rc = -1;
    if (rc < 0 || info.mappedBase != block->base || info.mappedSize < block->bytes ||
        info.memoryType != SCE_KERNEL_MEMORY_TYPE_NORMAL ||
        (info.access & (SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W)) !=
            (SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W) ||
        info.type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) {
        vjo_log("USER mapping refused uid=0x%08X rc=0x%08X size=%u type=0x%08X", block->uid, rc,
                info.mappedSize, info.type);
        (void)release_owned(block);
        return NULL;
    }
    return block->base;
}

static int owned_free(void *ud, void *pointer)
{
    (void)ud;
    if (!pointer)
        return VJO_OK;
    for (unsigned i = 0; i < 3; i++)
        if (blocks[i].uid >= 0 && blocks[i].base == pointer)
            return release_owned(&blocks[i]);
    vjo_log("USER free refused unknown pointer");
    return VJO_E_OCR_UNAVAILABLE;
}

static int has_owned_blocks(void)
{
    for (unsigned i = 0; i < 3; i++)
        if (blocks[i].uid >= 0)
            return 1;
    return 0;
}

int vjo_meiki_game_owned(void)
{
    return bridge.module >= 0 || bridge.metadata || bridge.workspace || bridge.preprocessing ||
           has_owned_blocks();
}
int vjo_meiki_game_cleanup(void)
{
    int rc = vjo_meiki_bridge_finish(&bridge);
    if (rc)
        return rc; /* engine must quiesce before any caller buffer release */
    for (unsigned i = 0; i < 3; i++)
        if (release_owned(&blocks[i]))
            rc = VJO_E_OCR_UNAVAILABLE;
    return rc;
}
int vjo_meiki_game_run(const VjoPlatform *platform, const char *model_dir, const VjoOcrImage *image,
                       VjoGameOcrResult *result)
{
    if (vjo_meiki_game_owned())
        return VJO_E_OCR_UNAVAILABLE;
    if (!image || image->height > VJO_OCR_MAX_LINE_HEIGHT)
        return VJO_E_OCR_REGION;
    if (!memory_imports_ready() || !MEIKI_IMPORT_READY(sceKernelLoadStartModule) ||
        !MEIKI_IMPORT_READY(sceKernelStopUnloadModule))
        return VJO_E_OCR_UNAVAILABLE;
    SceKernelFreeMemorySizeInfo info = {.size = sizeof(info)};
    int rc = sceKernelGetFreeMemorySize(&info);
    vjo_log("Meiki budget rc=0x%08X USER_free=%d USER_total=unavailable maxchunk=unavailable", rc,
            info.size_user);
    vjo_log("Meiki owned=%u neural=%u metadata=%u preprocess=%u module=%u reserve=%u required=%u",
            VJO_MEIKI_OWNED_BYTES, MEIKI_WORKSPACE_BYTES, MEIKI_MODULE_METADATA_BYTES,
            VJO_MEIKI_PREPROCESS_BYTES, VJO_MEIKI_MODULE_ALLOWANCE, VJO_MEIKI_GAME_RESERVE,
            VJO_MEIKI_REQUIRED_BYTES);
    result->workspace_bytes = VJO_MEIKI_OWNED_BYTES;
    if (rc < 0 || info.size_user <= 0 || (uint32_t)info.size_user > USER_MAX_BYTES ||
        (uint32_t)info.size_user < VJO_MEIKI_REQUIRED_BYTES)
        return VJO_E_OOM;
    VjoMeikiAllocator allocator = {NULL, owned_alloc, owned_free, NULL};
    rc = vjo_meiki_bridge_set_allocator(&bridge, &allocator);
    if (rc)
        return rc;
    rc = vjo_meiki_bridge_start(&bridge, platform, model_dir, image->cancelled, image->ud);
    if (rc)
        return rc;
    VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
    VjoMeikiStats stats = {0};
    rc = vjo_meiki_ocr_single_line(model_dir, image, &engine, bridge.input, bridge.input_elements,
                                   bridge.scratch, MEIKI_PREPROCESS_SCRATCH_BYTES, result->text,
                                   sizeof(result->text), &stats);
    result->lines = stats.lines;
    result->load_us = stats.load_us;
    result->recognize_us = stats.inference_us;
    vjo_log("Meiki run rc=%d neural_peak=%u metadata_brk_peak=%u reader_bytes=%u reader_error=%d",
            rc, (unsigned)stats.heap_peak, bridge.module_stats.metadata_brk_peak,
            (unsigned)bridge.module_stats.runtime.model_read_bytes,
            bridge.module_stats.runtime.model_read_error);
    return rc;
}
