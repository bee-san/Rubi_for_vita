/* Local OCR runs in the enabled game's process. No Paf, newlib startup, hooks,
 * network, or borrowed game allocations. Capture rows and bounded model chunks
 * cross the kernel transport; weights and inference use private memblocks:
 * the verified weights stay resident between jobs, the job workspace does not. */
#include "main.h"
#include "../core/vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"
#define WORKER_STACK_BYTES (32u*1024u)
#include "../core/games.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#define GAME_RESERVE (4u * 1024u * 1024u)
#define JOIN_US 2000000u
#define MODEL_WAIT_US 2000000u
/* Only when a model syscall still answers BUSY after its own bounded wait
 * for the kernel worker (a slow card): yield, then repeat it. */
#define MODEL_POLL_US 1000u
#define PAGE_ROUND(x) (((uint32_t)(x) + 4095u) & ~4095u)
#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static SceUID worker = -1, workspace_uid = -1, weights_uid = -1;
static uint32_t model_seq;
static int model_owned;
static uint32_t quitting, active_seq;
static int worker_started, claim_outstanding, blocked, io_error, stat_error;
static VjoGameOcrResult pending_result; /* never put 4 KiB results on thread stacks */
static void *weights_alloc(void *ud, size_t bytes);
static int weights_release(void *ud);
/* Verified weights kept between jobs while this game is the active one. */
static VjoVocrResident resident = {weights_alloc, weights_release, NULL, NULL, NULL, 0, 0, 0};
int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len);

static int cancelled(void *ud)
{
    (void)ud;
    return __atomic_load_n(&quitting, __ATOMIC_ACQUIRE) ||
           !active_seq || vjoOcrCancelled(active_seq) != 0;
}

static int rows(void *ud, unsigned row, unsigned count, unsigned char *dst)
{
    const VjoGameOcrRequest *r = ud;
    if (cancelled(NULL) || row >= r->height || !count || count > r->height-row) return -1;
    return vjoOcrReadRaw(r->seq, row, count, dst);
}

static int file_read(void *ud, uint64_t off, void *dst, size_t bytes)
{
    (void)ud;
    if (!model_owned || !dst || off > UINT32_MAX || bytes > UINT32_MAX-off || cancelled(NULL)) return -1;
    for (size_t used = 0; used < bytes;) {
        if (cancelled(NULL)) return -1;
        size_t n = bytes-used < VJO_GAME_OCR_MODEL_CHUNK ? bytes-used : VJO_GAME_OCR_MODEL_CHUNK;
        uint64_t started = sceKernelGetProcessTimeWide();
        int got;
        do {
            if (cancelled(NULL)) return -1;
            got = vjoOcrModelRead(model_seq, (uint32_t)(off+used), (uint32_t)n, (unsigned char *)dst+used);
            if (got != VJO_ERR_BUSY) break;
            if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { got = VJO_ERR_TIMEOUT; break; }
            sceKernelDelayThread(MODEL_POLL_US);
        } while (1);
        if (got <= 0 || (size_t)got > n) { io_error = got < 0 ? got : -1; return -1; }
        used += (size_t)got;
    }
    return 0;
}

static void file_close(void *ud)
{
    (void)ud;
    if (model_owned) {
        uint64_t started = sceKernelGetProcessTimeWide();
        int rc;
        do {
            /* Cleanup must drain even after cancellation or active_seq reset. */
            rc = vjoOcrModelClose(model_seq);
            if (rc != VJO_ERR_BUSY) break;
            if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { rc = VJO_ERR_TIMEOUT; break; }
            sceKernelDelayThread(MODEL_POLL_US);
        } while (1);
        if (rc >= 0) { model_owned = 0; model_seq = 0; }
        else io_error = rc; /* cleanup retries; never acknowledge a leaked descriptor */
    }
}

static int file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    if (model_owned || !path || !out || cancelled(NULL)) return -1;
    /* Reserve ownership before enqueue: a timed-out Open may still finish.
     * The kernel resolves only this claimed job's pinned model path. */
    model_seq = active_seq;
    model_owned = 1;
    uint64_t started = sceKernelGetProcessTimeWide();
    VjoGameOcrModelInfo info;
    int size;
    do {
        if (cancelled(NULL)) return -1;
        sceClibMemset(&info, 0, sizeof(info));
        size = vjoOcrModelOpen(model_seq, &info);
        if (size != VJO_ERR_BUSY) break;
        if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { size = VJO_ERR_TIMEOUT; break; }
        sceKernelDelayThread(MODEL_POLL_US);
    } while (1);
    if (size <= 0) { io_error = size; return -1; }
    /* The kernel's view of the file version: resident weights are reused only
     * while it is known and unchanged (an unknown time means: read again). */
    stat_error = info.stat_rc;
    uint64_t stamp = info.size == sizeof(info) && info.bytes == (uint32_t)size && !info.stat_rc ? info.mtime : 0;
    *out = (VjoFile){NULL, (uint64_t)size, file_read, file_close, stamp};
    return 0;
}

static uint64_t now_us(void *ud)
{
    (void)ud;
    return sceKernelGetProcessTimeWide();
}

/* USER bytes free after keeping the game reserve, or -1 if unknown. */
static int64_t free_user_after_reserve(void)
{
    SceKernelFreeMemorySizeInfo free_mem = {.size=sizeof(free_mem)};
    if (!IMPORT_READY(sceKernelGetFreeMemorySize) ||
        sceKernelGetFreeMemorySize(&free_mem) < 0 || free_mem.size_user < 0) return -1;
    return (int64_t)free_mem.size_user - (int64_t)GAME_RESERVE;
}

/* VjoVocrResident.alloc: one page-rounded block for the weights, under the
 * same reserve as the job workspace. */
static void *weights_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes);
    void *base = NULL;
    if (weights_uid >= 0 || !bytes || bytes > VJO_VOCR_F20_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    weights_uid = sceKernelAllocMemBlock("VjoGameVocrWeights", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, NULL);
    if (weights_uid < 0) { weights_uid = -1; return NULL; }
    if (sceKernelGetMemBlockBase(weights_uid, &base) < 0 || !base || ((uintptr_t)base & 15u)) {
        /* A block that cannot be released stays recorded; cleanup retries it. */
        if (sceKernelFreeMemBlock(weights_uid) >= 0) weights_uid = -1;
        return NULL;
    }
    return base;
}

static int weights_release(void *ud)
{
    (void)ud;
    if (weights_uid < 0) return 0;
    if (sceKernelFreeMemBlock(weights_uid) < 0) return -1;
    weights_uid = -1;
    return 0;
}

/* Releases the resident weights, and a block whose base lookup failed. */
static int drop_weights(void)
{
    if (vjo_vocr_resident_drop(&resident)) return -1;
    return weights_release(NULL);
}

static uint32_t resident_bytes(void)
{
    return resident.model && weights_uid >= 0 ? PAGE_ROUND(resident.model->bytes) : 0;
}

static int cleanup(void)
{
    int rc = 0;
    if (workspace_uid >= 0) {
        if (sceKernelFreeMemBlock(workspace_uid) < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else workspace_uid = -1;
    }
    file_close(NULL);
    if (model_owned) rc = VJO_E_OCR_UNAVAILABLE;
    /* Between jobs a weights block holds verified weights or nothing. */
    if (!resident.model && (resident.data || weights_uid >= 0) && drop_weights()) rc = VJO_E_OCR_UNAVAILABLE;
    return rc;
}

int vjo_game_ocr_run_request(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    if (!r || !out) return VJO_E_SOURCE;
    sceClibMemset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->seq = r->seq;
    active_seq = r->seq; io_error = 0; stat_error = 0;
    int rc = VJO_E_SOURCE;
    VjoVocrStats stats = {0};
    if (blocked || workspace_uid >= 0 || model_owned) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if (r->size != sizeof(*r) || !r->seq || !r->done_seq ||
        !r->width || r->width > VJO_OCR_MAX_WIDTH || !r->height || r->height > VJO_OCR_MAX_HEIGHT ||
        r->stride != r->width*4u || !r->model_dir[0] || !r->model[0] ||
        r->model_dir[sizeof(r->model_dir)-1] || r->model[sizeof(r->model)-1]) goto done;
    if (cancelled(NULL)) { rc = VJO_E_CANCELLED; goto done; }
    const VjoVocrModel *model = vjo_vocr_model(r->model);
    size_t bytes = vjo_vocr_job_bytes(r->model, r->width, r->height);
    if (!bytes) { rc = model ? VJO_E_OCR_REGION : VJO_E_OCR_MODEL; goto done; }
    /* Another model: its weights are useless now, and two are never held. */
    if (resident.model && resident.model != model && drop_weights()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* The job workspace, plus the weights unless they are already resident. */
    uint32_t rounded = PAGE_ROUND(bytes);
    uint32_t weights = resident.model == model ? 0 : PAGE_ROUND(model->bytes);
    int64_t available = free_user_after_reserve();
    if (available < (int64_t)rounded + (int64_t)weights) {
        /* Too little memory: give the game back the resident weights too. */
        if (drop_weights()) rc = VJO_E_OCR_UNAVAILABLE;
        else rc = VJO_E_OOM;
        goto done;
    }
    workspace_uid = sceKernelAllocMemBlock("VjoGameVocr", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, NULL);
    if (workspace_uid < 0) { workspace_uid = -1; rc = VJO_E_OOM; goto done; }
    void *base = NULL;
    if (sceKernelGetMemBlockBase(workspace_uid, &base) < 0 || !base || ((uintptr_t)base & 15u)) {
        rc = VJO_E_OOM; goto done;
    }
    VjoPlatform platform = {.file_open=file_open, .now_us=now_us};
    VjoOcrImage image = {(void *)r, r->width, r->height, rows, cancelled};
    rc = vjo_vocr_ocr_resident(&platform, r->model_dir, r->model, &image, &resident, base, bytes,
                               out->text, sizeof(out->text), &stats);
done:
    out->workspace_bytes = (uint32_t)stats.workspace;
    out->lines = stats.lines; out->find_us = stats.find_us;
    out->load_us = stats.load_us; out->recognize_us = stats.recognize_us;
    out->weights = stats.model_loaded ? VJO_GAME_OCR_WEIGHTS_READ :
                   stats.model_cached ? VJO_GAME_OCR_WEIGHTS_RESIDENT : VJO_GAME_OCR_WEIGHTS_NONE;
    out->cleanup_status = cleanup();
    out->resident_bytes = resident_bytes();
    out->io_error = io_error;
    out->stat_error = stat_error;
    if (out->cleanup_status) { blocked = 1; rc = VJO_E_OCR_UNAVAILABLE; }
    else if (cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (rc) sceClibMemset(out->text, 0, sizeof(out->text));
    out->rc = rc; active_seq = 0;
    return rc;
}

static int worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!__atomic_load_n(&quitting, __ATOMIC_ACQUIRE) && !blocked) {
        if (claim_outstanding) {
            if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
        } else {
            int registered = vjoOcrRegister();
            if (!registered) {
                VjoGameOcrRequest request = {.size=sizeof(request)};
                if (!vjoOcrTake(&request)) {
                    claim_outstanding = 1;
                    vjo_game_ocr_run_request(&request, &pending_result);
                    if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
                }
            } else if (registered != VJO_ERR_BUSY && (resident.data || weights_uid >= 0) && drop_weights()) {
                /* Not the active game (suspended, behind another app, or its
                 * title disabled): the weights go back to it. */
                blocked = 1;
            }
        }
        sceKernelDelayThread(claim_outstanding ? 50000 : 100000);
    }
    return 0;
}

int vjo_game_ocr_worker_start(void)
{
    if (worker >= 0 || blocked) return VJO_E_OCR_UNAVAILABLE;
    if (!IMPORT_READY(vjoGetVersion) || !IMPORT_READY(vjoOcrRegister) || !IMPORT_READY(vjoOcrTake) ||
        !IMPORT_READY(vjoOcrCancelled) || !IMPORT_READY(vjoOcrReadRaw) || !IMPORT_READY(vjoOcrComplete) ||
        !IMPORT_READY(vjoOcrModelOpen) || !IMPORT_READY(vjoOcrModelRead) || !IMPORT_READY(vjoOcrModelClose) ||
        !IMPORT_READY(sceKernelAllocMemBlock) || !IMPORT_READY(sceKernelGetMemBlockBase) ||
        !IMPORT_READY(sceKernelFreeMemBlock) || vjoGetVersion() != VJO_API_VERSION) return VJO_E_OCR_UNAVAILABLE;
    __atomic_store_n(&quitting, 0, __ATOMIC_RELEASE);
    worker = sceKernelCreateThread("VjoGameVocr", worker_main, 0x10000100, WORKER_STACK_BYTES, 0, 0, NULL);
    if (worker < 0) return VJO_E_OCR_UNAVAILABLE;
    if (sceKernelStartThread(worker, 0, NULL) < 0) {
        if (sceKernelDeleteThread(worker) >= 0) worker = -1;
        return VJO_E_OCR_UNAVAILABLE;
    }
    worker_started = 1;
    return VJO_OK;
}

int vjo_game_ocr_worker_stop(void)
{
    __atomic_store_n(&quitting, 1, __ATOMIC_RELEASE);
    if (worker >= 0) {
        SceUInt timeout = JOIN_US;
        if (worker_started && sceKernelWaitThreadEnd(worker, NULL, &timeout) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker_started = 0;
        if (sceKernelDeleteThread(worker) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker = -1;
    }
    if (cleanup()) return VJO_E_OCR_UNAVAILABLE;
    /* The game is exiting or the plugin stops: no weights stay behind. */
    if (drop_weights()) return VJO_E_OCR_UNAVAILABLE;
    if (claim_outstanding) {
        pending_result.cleanup_status = 0; pending_result.rc = VJO_E_CANCELLED;
        sceClibMemset(pending_result.text, 0, sizeof(pending_result.text));
        if (vjoOcrComplete(&pending_result)) return VJO_E_OCR_UNAVAILABLE;
        claim_outstanding = 0;
    }
    blocked = 0;
    return VJO_OK;
}

#ifndef VJO_GAME_OCR_NO_MODULE_ENTRY
int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    char title[VJO_TITLE_ID_SIZE] = {0};
    SceUID pid = sceKernelGetProcessId();
    int rc = -1;
    if (IMPORT_READY(sceKernelGetProcessTitleId)) rc = sceKernelGetProcessTitleId(pid, title, sizeof(title));
    if ((rc < 0 || !title[0]) && IMPORT_READY(sceAppMgrAppParamGetString))
        rc = sceAppMgrAppParamGetString(pid, 12, title, sizeof(title));
    title[sizeof(title)-1] = 0;
    /* *ALL also reaches system apps. They keep no thread, weights or workspace.
     * Candidate games remain idle until the Shell confirms their games.ini policy. */
    if (rc >= 0 && vjo_game_is_candidate(title)) vjo_game_ocr_worker_start();
    /* Import/start failures leave the host app usable. Retained thread objects
     * keep the module loaded until module_stop can safely delete them. */
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return vjo_game_ocr_worker_stop() ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}
#endif
