/* Local OCR runs in the enabled game's process. No Paf, newlib startup, hooks,
 * network, or borrowed game allocations. Capture rows and bounded model chunks
 * cross the kernel transport; weights and inference use a private memblock. */
#include "main.h"
#include "../core/vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"
#ifdef VJO_WITH_PPOCRV6
#include "../core/ppocrv6_ocr.h"
#endif
#define WORKER_STACK_BYTES (32u*1024u)
/* PP-OCRv6 runs on its own thread, created for one job only, so games that
 * never select it keep the 32 KiB worker stack. The Vita build's code used
 * 14,260 bytes of stack (docs/ppocrv6.md). */
#define PPOCR_STACK_BYTES (64u*1024u)
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
#define MODEL_POLL_US 1000u
#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static SceUID worker = -1, workspace_uid = -1, ppocr_thread = -1;
static uint32_t model_seq;
static int model_owned;
static uint32_t quitting, active_seq;
static int worker_started, claim_outstanding, blocked, io_error;
static VjoGameOcrResult pending_result; /* never put 4 KiB results on thread stacks */
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
    int size;
    do {
        if (cancelled(NULL)) return -1;
        size = vjoOcrModelOpen(model_seq);
        if (size != VJO_ERR_BUSY) break;
        if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { size = VJO_ERR_TIMEOUT; break; }
        sceKernelDelayThread(MODEL_POLL_US);
    } while (1);
    if (size <= 0) { io_error = size; return -1; }
    *out = (VjoFile){NULL, (uint64_t)size, file_read, file_close};
    return 0;
}

static uint64_t now_us(void *ud)
{
    (void)ud;
    return sceKernelGetProcessTimeWide();
}

/* A PP-OCRv6 thread whose join failed may still use the workspace: wait for
 * it again (bounded) before anything it uses is released. */
static int ppocr_reap(void)
{
    if (ppocr_thread < 0) return 0;
    SceUInt timeout = JOIN_US;
    if (sceKernelWaitThreadEnd(ppocr_thread, NULL, &timeout) < 0) return -1;
    if (sceKernelDeleteThread(ppocr_thread) < 0) return -1;
    ppocr_thread = -1;
    return 0;
}

static int cleanup(void)
{
    int rc = 0;
    if (ppocr_reap()) return VJO_E_OCR_UNAVAILABLE;
    if (workspace_uid >= 0) {
        if (sceKernelFreeMemBlock(workspace_uid) < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else workspace_uid = -1;
    }
    file_close(NULL);
    if (model_owned) rc = VJO_E_OCR_UNAVAILABLE;
    return rc;
}

#ifdef VJO_WITH_PPOCRV6
/* Everything the job thread touches lives in its own workspace block, after
 * the engine's bytes: a copy of the request and its text. A thread that could
 * not be joined therefore never reaches the worker's stack or the result that
 * is later sent to the kernel; the block is freed only after a join. */
typedef struct {
    VjoGameOcrRequest request;
    char text[VJO_GAME_OCR_TEXT_BYTES];
} PpocrJobTail;
static struct {
    PpocrJobTail *tail;
    void *workspace;
    size_t bytes;
    VjoPpocrStats stats;
    int rc;
} ppocr_job;

static int ppocr_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    PpocrJobTail *t = ppocr_job.tail;
    VjoPlatform platform = {.file_open=file_open, .now_us=now_us};
    VjoOcrImage image = {&t->request, t->request.width, t->request.height, rows, cancelled};
    ppocr_job.rc = vjo_ppocrv6_ocr(&platform, t->request.model_dir, &image, ppocr_job.workspace, ppocr_job.bytes,
                                   t->text, sizeof(t->text), &ppocr_job.stats);
    return 0;
}
#endif

/* Same ownership as VOCR: one private USER_RW workspace, the broker's model
 * reads and complete cleanup before a result returns. The job thread's stack
 * exists only while it runs. */
static int run_ppocrv6(const VjoGameOcrRequest *r, VjoGameOcrResult *out, VjoVocrStats *stats)
{
#ifdef VJO_WITH_PPOCRV6
    size_t need = vjo_ppocrv6_workspace_bytes(r->width, r->height);
    if (!need) return VJO_E_OCR_REGION;
    size_t tail_at = (need + 63u) & ~(size_t)63u;
    uint32_t rounded = ((uint32_t)(tail_at + sizeof(PpocrJobTail)) + 4095u) & ~4095u;
    SceKernelFreeMemorySizeInfo free_mem = {.size=sizeof(free_mem)};
    stats->workspace = rounded;
    if (!IMPORT_READY(sceKernelGetFreeMemorySize) ||
        sceKernelGetFreeMemorySize(&free_mem) < 0 || free_mem.size_user < 0 ||
        (uint32_t)free_mem.size_user < rounded + PPOCR_STACK_BYTES + GAME_RESERVE) return VJO_E_OOM;
    workspace_uid = sceKernelAllocMemBlock("VjoGamePpocr", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, NULL);
    if (workspace_uid < 0) return VJO_E_OOM;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(workspace_uid, &base) < 0 || !base || ((uintptr_t)base & 63u)) return VJO_E_OOM;
    sceClibMemset(&ppocr_job, 0, sizeof(ppocr_job));
    ppocr_job.tail = (PpocrJobTail *)((unsigned char *)base + tail_at);
    sceClibMemcpy(&ppocr_job.tail->request, r, sizeof(*r));
    ppocr_job.tail->text[0] = '\0';
    ppocr_job.workspace = base; ppocr_job.bytes = need;
    ppocr_job.rc = VJO_E_OCR_UNAVAILABLE;
    ppocr_thread = sceKernelCreateThread("VjoGamePpocr", ppocr_main, 0x10000100, PPOCR_STACK_BYTES, 0, 0, NULL);
    if (ppocr_thread < 0) { ppocr_thread = -1; return VJO_E_OOM; }
    if (sceKernelStartThread(ppocr_thread, 0, NULL) < 0) {
        if (sceKernelDeleteThread(ppocr_thread) >= 0) ppocr_thread = -1;
        return VJO_E_OCR_UNAVAILABLE;
    }
    /* Cancellation is checked between model chunks and lines, never mid-line:
     * the job ends on its own. A failed join keeps the thread and workspace. */
    if (sceKernelWaitThreadEnd(ppocr_thread, NULL, NULL) < 0) return VJO_E_OCR_UNAVAILABLE;
    if (sceKernelDeleteThread(ppocr_thread) < 0) return VJO_E_OCR_UNAVAILABLE;
    ppocr_thread = -1;
    if (!ppocr_job.rc) sceClibMemcpy(out->text, ppocr_job.tail->text, sizeof(out->text));
    stats->lines = ppocr_job.stats.lines;
    stats->find_us = ppocr_job.stats.find_us;
    stats->load_us = ppocr_job.stats.load_us;
    stats->recognize_us = ppocr_job.stats.recognize_us;
    return ppocr_job.rc;
#else
    (void)r; (void)out; (void)stats;
    return VJO_E_OCR_UNAVAILABLE;
#endif
}

int vjo_game_ocr_run_request(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    if (!r || !out) return VJO_E_SOURCE;
    sceClibMemset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->seq = r->seq;
    active_seq = r->seq; io_error = 0;
    int rc = VJO_E_SOURCE;
    VjoVocrStats stats = {0};
    if (blocked || workspace_uid >= 0 || model_owned || ppocr_thread >= 0) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if (r->size != sizeof(*r) || !r->seq || !r->done_seq ||
        !r->width || r->width > VJO_OCR_MAX_WIDTH || !r->height || r->height > VJO_OCR_MAX_HEIGHT ||
        r->stride != r->width*4u || !r->model_dir[0] || !r->model[0] ||
        r->model_dir[sizeof(r->model_dir)-1] || r->model[sizeof(r->model)-1]) goto done;
    if (cancelled(NULL)) { rc = VJO_E_CANCELLED; goto done; }
    if (!sceClibStrcmp(r->model, VJO_PPOCRV6_MODEL_NAME)) {
        rc = run_ppocrv6(r, out, &stats);
        goto done;
    }
    size_t bytes = vjo_vocr_workspace_bytes(r->model, r->width, r->height);
    if (!bytes) { rc = vjo_vocr_model(r->model) ? VJO_E_OCR_REGION : VJO_E_OCR_MODEL; goto done; }
    uint32_t rounded = ((uint32_t)bytes+4095u)&~4095u;
    SceKernelFreeMemorySizeInfo free_mem = {.size=sizeof(free_mem)};
    if (!IMPORT_READY(sceKernelGetFreeMemorySize) ||
        sceKernelGetFreeMemorySize(&free_mem) < 0 || free_mem.size_user < 0 ||
        (uint32_t)free_mem.size_user < rounded + GAME_RESERVE) { rc = VJO_E_OOM; goto done; }
    workspace_uid = sceKernelAllocMemBlock("VjoGameVocr", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, NULL);
    if (workspace_uid < 0) { rc = VJO_E_OOM; goto done; }
    void *base = NULL;
    if (sceKernelGetMemBlockBase(workspace_uid, &base) < 0 || !base || ((uintptr_t)base & 15u)) {
        rc = VJO_E_OOM; goto done;
    }
    VjoPlatform platform = {.file_open=file_open, .now_us=now_us};
    VjoOcrImage image = {(void *)r, r->width, r->height, rows, cancelled};
    rc = vjo_vocr_ocr(&platform, r->model_dir, r->model, &image, base, bytes,
                      out->text, sizeof(out->text), &stats);
done:
    out->workspace_bytes = (uint32_t)stats.workspace;
    out->lines = stats.lines; out->find_us = stats.find_us;
    out->load_us = stats.load_us; out->recognize_us = stats.recognize_us;
    out->cleanup_status = cleanup();
    out->io_error = io_error;
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
        } else if (!vjoOcrRegister()) {
            VjoGameOcrRequest request = {.size=sizeof(request)};
            if (!vjoOcrTake(&request)) {
                claim_outstanding = 1;
                vjo_game_ocr_run_request(&request, &pending_result);
                if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
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
