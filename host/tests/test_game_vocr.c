/* Real game worker and real vocr, with Vita scheduling/USER_RW/file boundaries.
 * No emulator claims: the caller PID and SDK responses are failure-injected. */
#include "acutest.h"
#include <sys/mman.h>
#include <psp2/host_stubs.h>
static unsigned test_cascade_conf; /* 0: the default; 1001: every line is doubtful */
#define VJO_GAME_CASCADE_CONF test_cascade_conf
#include "../../game_ocr/main.c"

static unsigned char workspace[2*1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char capture_mem[1024*1024] __attribute__((aligned(64)));
static unsigned char lm_memory[VJO_VOCR_LM_MAX_BYTES + 4096] __attribute__((aligned(64)));
static int lm_live; static unsigned lm_block, lm_allocations, lm_frees, lm_opens, last_which;
static unsigned char cascade_memory[VJO_VOCR_CASCADE_BYTES + 4096] __attribute__((aligned(64)));
static int cascade_live; static unsigned cascade_block, cascade_allocations, cascade_frees, cascade_opens;
static unsigned char pixels[200*272*4]; /* 200 x frame_h */
static unsigned frame_h = 60; /* rows of the fake frame (over 64: two strips) */
/* The capture block and the memory-type ladder: which types refuse. */
static int capture_live, refuse_user, refuse_cdram, refuse_phycont, rows_error, hook_write_error;
static int busy_unarmed, busy_armed, allow_capture_held; /* rows calls that answer BUSY, before / after arming */
static unsigned capture_block, capture_allocs, capture_frees, rows_calls, kstrip_calls, last_type;
static unsigned allocations, frees, reads_count, bytes_read, thread_creates, weight_allocations, weight_frees;
static int memory_live, alloc_failure, base_failure, free_failure, close_failure, weights_live;
static unsigned live_bytes, job_block, weights_block;
static int join_failure, start_failure, delete_failure, budget_failure, cancel_reads;
static int available, cancel_job, short_reads, open_failure, bad_read;
static FILE *model;
static char model_path[512];
static unsigned user_open_calls;
static int virtual_file, open_busy, read_busy, close_busy, read_eof;
static char own_title[12];
static uint64_t clock_value;
static unsigned frame_us; /* each rows call waits this long for its frame (0: none) */
static unsigned change_at_rows; /* at this rows call the line moves down 6 rows (the player advanced); 0: never */
static unsigned clear_at_rows;  /* at this rows call the box empties (the player advanced to an empty box); 0: never */

SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
/* Free USER memory as the game sees it: what remains after the worker's blocks. */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ TEST_CHECK(out->size == sizeof(*out)); out->size_user = available < 0 ? available : available - (int)live_bytes; return budget_failure ? -1 : 0; }
static unsigned workspace_tries, workspace_types[8];
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(!(size&4095));
    last_type = (unsigned)type;
    if (!strcmp(name, "VjoGameVocrCapture") || !strcmp(name, "VjoGameVocrProbe")) {
        /* SDK units: CDRAM 256 KiB, PHYCONT 1 MiB */
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && refuse_user) return (int)0x80024302u;
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) { TEST_CHECK(!(size % (256*1024))); if (refuse_cdram) return (int)0x80024302u; }
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) { TEST_CHECK(!(size % (1024*1024))); if (refuse_phycont) return (int)0x80024302u; }
        TEST_CHECK(!capture_live && size <= sizeof(capture_mem));
        capture_allocs++; capture_live = 1; capture_block = size; live_bytes += size; return 11;
    }
    if (!strcmp(name, "VjoGameVocrCascade")) { /* the cascade's model, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!cascade_live && size <= sizeof(cascade_memory));
        cascade_allocations++;
        if (alloc_failure) return -1;
        cascade_live = 1; cascade_block = size; live_bytes += size; return 13;
    }
    if (!strcmp(name, "VjoGameVocrLM")) { /* the resident LM, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!lm_live && size <= sizeof(lm_memory));
        lm_allocations++;
        if (alloc_failure) return -1;
        lm_live = 1; lm_block = size; live_bytes += size; return 12;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) { /* resident verified weights, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory));
        weight_allocations++;
        if (alloc_failure) return -1;
        weights_live = 1; weights_block = size; live_bytes += size; return 10;
    }
    /* the job's workspace: the same ladder as the capture, USER first */
    TEST_CHECK(!strcmp(name, "VjoGameVocr") && !memory_live);
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) TEST_CHECK(!(size % (256*1024)));
    else if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) TEST_CHECK(!(size % (1024*1024)));
    else TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && size <= sizeof(workspace));
    if (workspace_tries < 8) workspace_types[workspace_tries] = (unsigned)type;
    workspace_tries++;
    allocations++;
    if (alloc_failure) return (int)0x80024302u;
    TEST_CHECK(size <= sizeof(workspace));
    memory_live = 1; job_block = size; live_bytes += size; return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem; return 0; }
    if (uid == 10) { TEST_CHECK(weights_live); *base = weights_memory; return 0; }
    if (uid == 12) { TEST_CHECK(lm_live); *base = lm_memory; return 0; }
    if (uid == 13) { TEST_CHECK(cascade_live); *base = cascade_memory; return 0; }
    TEST_CHECK(uid == 9 && memory_live); *base = base_failure ? NULL : workspace; return base_failure ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(capture_live); if (free_failure) return -1; capture_live = 0; capture_frees++; live_bytes -= capture_block; return 0; }
    if (uid == 10) {
        TEST_CHECK(weights_live); if (free_failure) return -1;
        weights_live = 0; weight_frees++; live_bytes -= weights_block; return 0;
    }
    if (uid == 12) {
        TEST_CHECK(lm_live); if (free_failure) return -1;
        lm_live = 0; lm_frees++; live_bytes -= lm_block; return 0;
    }
    if (uid == 13) {
        TEST_CHECK(cascade_live); if (free_failure) return -1;
        cascade_live = 0; cascade_frees++; live_bytes -= cascade_block; return 0;
    }
    TEST_CHECK(uid == 9 && memory_live); if (free_failure) return -1;
    memory_live = 0; frees++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ TEST_CHECK(stack == 32*1024 && entry == worker_main && p == 0x10000100 + 31 && cpu == 0x70000); thread_creates++; return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return start_failure ? -1 : 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return join_failure ? -1 : 0; }
int sceKernelDeleteThread(SceUID uid) { return delete_failure ? -1 : 0; }
static int quit_on_delay; /* the worker loop's next sleep ends it (one pass of worker_main) */
int sceKernelDelayThread(unsigned us) { clock_value += us; if (quit_on_delay) quitting = 1; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ TEST_CHECK(pid == 7); snprintf(out, size, "%s", own_title); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
static int safe_boot_mode, boot_info_calls;
int vjoBootInfo(VjoBootInfo *out) { boot_info_calls++; TEST_CHECK(out->size == sizeof(*out)); out->mode = safe_boot_mode; return 0; }
int vjoOcrRegister(void) { return VJO_ERR_NO_GAME; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_job || (cancel_reads && reads_count >= (unsigned)cancel_reads); }
int vjoOcrComplete(const VjoGameOcrResult *out)
{ TEST_CHECK(!memory_live && !model && (!capture_live || allow_capture_held)); return 0; }
/* The kernel's rows call: grey rows of the 200x60 region into the worker's block. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->size == sizeof(*req) && req->seq == 12 && req->y < frame_h && req->n && req->n <= frame_h - req->y);
    TEST_CHECK(req->dst_stride >= 200 && info && info->size == sizeof(*info));
    TEST_CHECK(capture_live && (unsigned char *)dst >= capture_mem && (unsigned char *)dst < capture_mem + capture_block);
    rows_calls++;
    clock_value += frame_us;
    if (change_at_rows && rows_calls == change_at_rows) { /* the next screen: the line 6 rows lower */
        memset(pixels, 0, sizeof(pixels));
        for (unsigned y = 21; y < 45; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
    }
    if (clear_at_rows && rows_calls == clear_at_rows) memset(pixels, 0, sizeof(pixels));
    if (req->mode == VJO_GAME_OCR_SRC_KSTRIP) kstrip_calls++;
    info->fb_width = 200; info->fb_height = frame_h; info->fb_pitch = 200; info->fb_format = 0; info->wait_us = 16000;
    info->armed = 1;
    if (busy_unarmed > 0) { busy_unarmed--; info->armed = 0; return VJO_ERR_BUSY; } /* a previous copy not tidied */
    if (busy_armed) { /* this copy outlived the call; until the kernel worker tidies it, every
                         later call is refused before arming */
        busy_armed = 0; busy_unarmed = 1000;
        return VJO_ERR_BUSY;
    }
    if (rows_error) return rows_error;
    if (hook_write_error && req->mode == VJO_GAME_OCR_SRC_HOOK) return VJO_ERR_COPY;
    for (unsigned r = 0; r < req->n; r++)
        for (unsigned x = 0; x < 200; x++) {
            const unsigned char *p = pixels + ((req->y + r) * 200 + x) * 4;
            ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
        }
    return (int)req->n;
}
/* The probe's user-mode read: the frame sceDisplayGetFrameBuf names (frame_at,
 * none by default) and the block sceKernelGetMemBlockInfoByAddr reports for it. */
static unsigned char *frame_at;
static uintptr_t block_base;
static unsigned block_size, block_access, frame_info_calls, frame_w = 200;
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync)
{
    if (!frame_at) return -1;
    TEST_CHECK(fb->size == sizeof(SceDisplayFrameBuf) && sync == SCE_DISPLAY_SETBUF_NEXTFRAME);
    fb->base = frame_at; fb->pitch = 200; fb->pixelformat = VJO_FB_A8B8G8R8; fb->width = frame_w; fb->height = 60;
    return 0;
}
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info)
{
    if (!frame_at) return -1;
    TEST_CHECK(base == frame_at && info->size == sizeof(*info));
    frame_info_calls++;
    info->mappedBase = (void *)block_base; info->mappedSize = block_size; info->access = block_access;
    return 0;
}
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_open_calls++; return (int)0x80010013u; /* Physical game's ux0 is unavailable. */ }
static uint64_t model_mtime = 0x1234567;
static int model_stat_error;
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    if (open_busy) return VJO_ERR_BUSY;
    if (open_failure) return -123;
    TEST_ASSERT(!model && seq == 12);
    last_which = which;
    if (which == VJO_GAME_OCR_OPEN_CASCADE) { /* the cascade's pinned model, beside the others */
        char c_path[600];
        cascade_opens++;
        snprintf(c_path, sizeof(c_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_CASCADE_NAME);
        model = fopen(c_path, "rb");
    } else if (which >= VJO_GAME_OCR_OPEN_LM) { /* the LM of that tier, beside the model (the kernel's pinned names) */
        char lm_path[600];
        const VjoVocrLm *t = vjo_vocr_lm_tier(which - VJO_GAME_OCR_OPEN_LM + 1u);
        TEST_ASSERT(t != NULL);
        lm_opens++;
        snprintf(lm_path, sizeof(lm_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", t->name);
        model = fopen(lm_path, "rb");
    } else
    model = virtual_file ? tmpfile() : fopen(model_path, "rb");
    if (!model) return -123;
    if (virtual_file) {
        const unsigned char bytes[16] = {1,2,3,4};
        TEST_ASSERT(fwrite(bytes, 1, sizeof(bytes), model) == sizeof(bytes));
    }
    TEST_ASSERT(!fseek(model, 0, SEEK_END));
    int size = (int)ftell(model);
    if (info) *info = (VjoGameOcrModelInfo){sizeof(*info), (uint32_t)size, model_stat_error, 0,
                                            model_stat_error ? 0 : model_mtime};
    return size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t n, void *out)
{
    TEST_CHECK(seq == 12 && model && n <= VJO_GAME_OCR_MODEL_CHUNK);
    if (read_busy) return VJO_ERR_BUSY;
    TEST_ASSERT(!fseek(model, (long)offset, SEEK_SET));
    reads_count++;
    if (bad_read) return -456;
    if (read_eof) return 0;
    if (short_reads && n > 37) n = 37;
    int got = (int)fread(out, 1, n, model); bytes_read += (unsigned)got; return got;
}
int vjoOcrModelClose(uint32_t seq)
{ TEST_CHECK(seq == 12); if (close_busy) return VJO_ERR_BUSY; if (!model) return 0; if (close_failure) return -789; int rc = fclose(model); model = NULL; return rc; }

static VjoGameOcrRequest setup(void)
{
    if (model) { fclose(model); model = NULL; }
    worker = workspace_uid = weights_uid = capture_uid = line_uid = -1;
    capture_live = refuse_user = refuse_cdram = refuse_phycont = rows_error = hook_write_error = 0;
    busy_unarmed = busy_armed = allow_capture_held = 0; hook_owns_capture = 0;
    capture_block = capture_allocs = capture_frees = rows_calls = kstrip_calls = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    lm_res.lm = NULL; lm_res.data = NULL; lm_res.capacity = 0; lm_res.size = lm_res.stamp = 0; lm_uid = -1;
    lm_live = 0; lm_allocations = lm_frees = lm_opens = last_which = 0;
    cascade_res.model = NULL; cascade_res.data = NULL; cascade_res.capacity = 0; cascade_res.size = cascade_res.stamp = 0;
    cascade_uid = -1; cascade_live = 0; cascade_allocations = cascade_frees = cascade_opens = 0; test_cascade_conf = 0;
    psp_title = 0;
    weight_allocations = weight_frees = 0; weights_live = 0; live_bytes = 0;
    model_mtime = 0x1234567; model_stat_error = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    allocations = frees = reads_count = bytes_read = thread_creates = 0;
    user_open_calls = 0;
    virtual_file = open_busy = read_busy = close_busy = read_eof = 0; clock_value = 0; frame_us = 0;
    memory_live = alloc_failure = base_failure = free_failure = close_failure = 0;
    join_failure = start_failure = delete_failure = budget_failure = cancel_reads = 0;
    cancel_job = short_reads = open_failure = bad_read = 0;
    available = 16*1024*1024;
    memset(pixels, 0, sizeof(pixels)); strcpy(own_title, "PCSG00415");
    frame_at = NULL; frame_info_calls = 0; frame_w = 200;
    change_at_rows = clear_at_rows = 0; blocks_held = 0; frame_h = 60;
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=200, .fb_height=60, .width=200, .height=60};
    snprintf(r.model, sizeof(r.model), "FL10_w8.vocr");
    /* Blank images never open weights, but requests still need a valid directory.
     * CI intentionally has no private release assets in its source checkout. */
    snprintf(r.model_dir, sizeof(r.model_dir), "%s",
             VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : "unused-model-dir");
    snprintf(model_path, sizeof(model_path), "%s/%s", r.model_dir, r.model);
    return r;
}
static void line(void)
{
    for (unsigned y=15; y<39; y++) for (unsigned x=10; x<190; x+=6)
        memset(pixels+(y*200+x)*4, 235, 6);
}
static void repeated_jobs_and_reserve(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    for (unsigned i=0; i<100; i++) {
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        TEST_CHECK(!memory_live && !out.cleanup_status && !out.text[0] && !model);
    }
    TEST_CHECK(allocations == 100 && frees == 100 && capture_allocs == 100 && capture_frees == 100 && !capture_live);
    /* the ledger saw every block in and out, and its peak fits the budget */
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_WORKSPACE) == 0 && vjo_ledger_current(&g_game_ledger, GL_WEIGHTS) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) > 0 && vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) <= VJO_BUDGET_GAME_WORKSPACE);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_CAPTURE) == 0 && vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) == 16384); /* 200x60: 12,000 B strip + 8 B and 3 bits per row */
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) <= VJO_BUDGET_GAME_CAPTURE);
    TEST_CHECK(vjo_ledger_pool_peak(&g_game_ledger, VJO_LEDGER_POOL_GAME) <= VJO_WORST_GAME_PERMANENT + VJO_WORST_GAME_JOB);
    available = 4*1024*1024;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
    available = -1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
    available = 16*1024*1024; budget_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
}
static void memory_failure_and_retry(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    alloc_failure = 1; workspace_tries = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !frees);
    /* every rung was tried, in order, and each attempt is in the step log */
    TEST_CHECK(workspace_tries == 3 && workspace_types[0] == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW &&
               workspace_types[1] == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW &&
               workspace_types[2] == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW);
    unsigned refused = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_WORKSPACE &&
            out.step[i].kind == refused + 1u /* VJO_GAME_OCR_MEM_USER, CDRAM, PHYCONT */ &&
            out.step[i].rc == (int32_t)0x80024302u) refused++;
    TEST_CHECK(refused == 3);
    TEST_MSG("workspace rungs refused in the step log: %u of %u steps", refused, out.n_steps);
    alloc_failure = 0; base_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && frees == 1);
    base_failure = 0; free_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(memory_live && blocked && out.cleanup_status && !out.text[0]);
    unsigned before = allocations;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && allocations == before);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && memory_live);
    free_failure = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !memory_live && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_start());
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}
static void cancel_and_wire(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    cancel_job = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !allocations);
    cancel_job = 0; r.width = 961;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.width = 200; r.x = 1; /* past the frame's edge */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.x = 0; r.flags = 0x4000; /* API 20: bits 14-15 mean nothing yet */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.flags = 0; r.model[31] = 'x';
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
}
static void real_model_io_and_cancellation(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; model I/O test skipped"); return; }
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out; line(); short_reads = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(bytes_read == 657664 && reads_count > 1000 && out.lines == 1);
    TEST_CHECK(!memory_live && !model && !out.cleanup_status);
    TEST_CHECK(!user_open_calls); /* Model access never uses the game's unavailable ux0. */
    open_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.io_error == -123);
    open_failure = 0; bad_read = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.io_error == -456);
    bad_read = 0; close_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && model && !out.text[0]);
    close_failure = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model);
    reads_count = 0; cancel_reads = 4;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !model && !memory_live);
}
/* These are always run in CI, including checkouts without release weights. */
static void model_callbacks_timeouts_errors_and_cleanup(void)
{
    setup(); active_seq = 12; virtual_file = 1;
    VjoFile file; unsigned char bytes[16];
    open_failure = 1;
    TEST_CHECK(file_open(NULL, "ignored", &file) == -1 && io_error == -123);
    TEST_CHECK(model_owned && model_seq == 12);
    file_close(NULL); TEST_CHECK(!model_owned && !model_seq);
    open_failure = 0; open_busy = close_busy = 1;
    TEST_CHECK(file_open(NULL, "ignored", &file) == -1 && io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_owned && model_seq == 12 && !model);
    active_seq = 0; file_close(NULL);
    TEST_CHECK(model_owned && model_seq == 12 && io_error == VJO_ERR_TIMEOUT);
    close_busy = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model_owned && !model_seq);
    quitting = 0; active_seq = 12; open_busy = 0;
    TEST_CHECK(!file_open(NULL, "ignored", &file) && file.size == sizeof(bytes));
    TEST_CHECK(!file.read(NULL, 0, bytes, sizeof(bytes)) && bytes[0] == 1 && bytes[3] == 4);
    read_busy = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == VJO_ERR_TIMEOUT);
    read_busy = 0; bad_read = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == -456);
    bad_read = 0; read_eof = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == -1);
    read_eof = 0;
    TEST_CHECK(file.read(NULL, UINT32_MAX, bytes, sizeof(bytes)) == -1);
    TEST_CHECK(file.read(NULL, 0, NULL, sizeof(bytes)) == -1);
    cancel_job = 1; TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1);
    active_seq = 0; close_failure = 1; file.close(NULL);
    TEST_CHECK(model && model_owned && model_seq == 12 && io_error == -789);
    close_failure = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model && !model_owned);
}
static void system_app_gate_and_failed_joins(void)
{
    setup();
    /* SceShell ("main") and the system apps: the module unloads itself at
     * once (NO_RESIDENT), so *ALL costs them no pages; so does an unreadable title. */
    const char *excluded[] = {"main", "NPXS10015", "VITASHELL", "VJOSET001", ""};
    for (unsigned i=0; i<sizeof(excluded)/sizeof(excluded[0]); i++) {
        strcpy(own_title, excluded[i]);
        TEST_CHECK_(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !thread_creates && !allocations, "%s", excluded[i]);
    }
    /* A safe boot (the previous one failed): the module unloads itself in
     * every process, a candidate game included, before reading any title. */
    safe_boot_mode = VJO_BOOT_SAFE; boot_info_calls = 0;
    strcpy(own_title, "PCSG00415");
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && boot_info_calls == 1 && !thread_creates && !allocations);
    safe_boot_mode = VJO_BOOT_NORMAL;
    strcpy(own_title, "PCSG00415"); TEST_CHECK(!module_start(0, NULL) && thread_creates == 1);
    join_failure = 1;
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20 && worker_started);
    join_failure = 0; TEST_CHECK(!module_stop(0, NULL) && worker == -1);
    start_failure = delete_failure = 1;
    TEST_CHECK(!module_start(0, NULL) && worker == 20 && !worker_started);
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20);
    delete_failure = 0; TEST_CHECK(!module_stop(0, NULL));
}
/* The capture's memory ladder: USER_RW, then CDRAM, then PHYCONT, each attempt
 * a step with its native result; nothing left at all: out of memory, logged. */
static void capture_memory_ladder_is_logged(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    line();
    refuse_user = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.capture_memory == VJO_GAME_OCR_MEM_CDRAM && out.capture_bytes == 256u * 1024u);
    unsigned allocs = 0, refused = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_CAPTURE) {
            allocs++;
            if (out.step[i].rc == (int)0x80024302u) { refused++; TEST_CHECK(out.step[i].kind == VJO_GAME_OCR_MEM_USER); }
            else TEST_CHECK(out.step[i].kind == VJO_GAME_OCR_MEM_CDRAM && out.step[i].bytes == 256u * 1024u);
            TEST_CHECK(out.step[i].free_main == 16*1024*1024 - (int)live_bytes || out.step[i].free_main > 0);
        }
    TEST_CHECK(allocs == 2 && refused == 1 && !capture_live);
    refuse_cdram = 1;
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.capture_memory == VJO_GAME_OCR_MEM_PHYCONT && out.capture_bytes == 1024u * 1024u && !capture_live);
    refuse_phycont = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !capture_live && !memory_live);
    allocs = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_CAPTURE) allocs++;
    TEST_CHECK(allocs == 3);
}
/* The hook cannot write into the block: the same rows through the kernel strip,
 * for the rest of the job, and the step log says so. */
static void hook_failure_falls_back_to_the_kernel_strip(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    hook_write_error = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.text[0]);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_KSTRIP && kstrip_calls == rows_calls - 1);
    int saw = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].kind == VJO_GAME_OCR_SRC_HOOK && out.step[i].rc == VJO_ERR_COPY) saw = 1;
    TEST_CHECK(saw);
    /* a timeout (no frame) is not something a kernel strip fixes: the job fails, logged */
    hook_write_error = 0; rows_error = VJO_ERR_TIMEOUT; rows_calls = kstrip_calls = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !kstrip_calls && !capture_live && !memory_live);
    int logged = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].rc == VJO_ERR_TIMEOUT) logged = 1;
    TEST_CHECK(logged);
}
/* A BUSY before arming (a previous copy not tidied yet) is asked again; one
 * after arming (the copy outlived the call) keeps the block for the hook and
 * stops the worker. */
static void busy_rows_calls(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    busy_unarmed = 3;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !blocked && !hook_owns_capture && !capture_live);
    int logged = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].rc == VJO_ERR_BUSY) logged++;
    TEST_CHECK(logged == 1 && busy_unarmed == 0);
    busy_unarmed = 1000; /* never tidied: the job gives up, the worker stays usable */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !blocked && !capture_live && !memory_live);
    busy_unarmed = 0; busy_armed = 1; allow_capture_held = 1; rows_calls = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) != VJO_OK);
    /* the armed BUSY is not retried, so the unarmed ones after it never hide it */
    TEST_CHECK(rows_calls == 1 && blocked && hook_owns_capture && capture_live && !memory_live);
}

/* The probe tries every rung once and recognizes nothing. */
static void probe_tries_every_rung(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.flags = VJO_GAME_OCR_F_PROBE;
    refuse_phycont = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !memory_live && !capture_live && !model);
    unsigned probes = 0, hook_ok = 0, kstrip_ok = 0, user_fail = 0, phycont_refused = 0;
    uint32_t sum = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        const VjoGameOcrStep *s = &out.step[i];
        if (s->id == VJO_GSTEP_ALLOC && s->kind == VJO_GAME_OCR_MEM_PHYCONT && s->rc < 0) phycont_refused++;
        if (s->id != VJO_GSTEP_PROBE) continue;
        probes++;
        if (s->kind == VJO_GAME_OCR_SRC_HOOK && s->rc == 60) { hook_ok++; if (!sum) sum = s->sum; TEST_CHECK(s->sum == sum); }
        if (s->kind == VJO_GAME_OCR_SRC_KSTRIP && s->rc == 60) { kstrip_ok++; TEST_CHECK(s->sum == sum); }
        if (s->kind == VJO_GAME_OCR_SRC_USER && s->rc < 0) user_fail++;
    }
    /* USER and CDRAM blocks, hook and kernel strip each, plus the user-mode read once */
    TEST_CHECK(probes == 5 && hook_ok == 2 && kstrip_ok == 2 && user_fail == 1 && phycont_refused == 1);
    TEST_CHECK(out.strip_rows == 60 && !out.lines && allocations == 0);
}
/* The probe's user-mode read of the frame stays inside the frame's own block:
 * the frame (200x60, 48,000 B) is in 64 KiB of mapped memory, and each refused
 * case makes the bytes outside the block it reports unreadable, so a read past
 * the check faults this test instead of passing it. */
static int probe_user_read(const VjoGameOcrRequest *r, uint32_t *sum)
{
    VjoGameOcrResult out;
    int rc = INT32_MIN;
    TEST_CHECK(!vjo_game_ocr_run_request(r, &out) && !capture_live && !memory_live);
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PROBE && out.step[i].kind == VJO_GAME_OCR_SRC_USER) {
            rc = out.step[i].rc;
            *sum = out.step[i].sum;
        } else if (out.step[i].id == VJO_GSTEP_PROBE && out.step[i].kind == VJO_GAME_OCR_SRC_HOOK && !*sum) {
            *sum = out.step[i].sum; /* the hook's rows of the same frame */
        }
    return rc;
}
static void probe_user_read_stays_in_the_frame_block(void)
{
    VjoGameOcrRequest r = setup();
    uint32_t hook_sum = 0, sum = 0;
    r.flags = VJO_GAME_OCR_F_PROBE;
    refuse_phycont = 1;
    line();
    unsigned char *map = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(map != MAP_FAILED);
    frame_at = map;
    memcpy(frame_at, pixels, 200 * 60 * 4);
    /* the whole frame in one readable block: 60 rows, the same grey as the hook's */
    block_base = (uintptr_t)map; block_size = 0x10000; block_access = SCE_KERNEL_MEMORY_ACCESS_R;
    int rc = probe_user_read(&r, &hook_sum);
    sum = hook_sum;
    TEST_CHECK_(rc == 60, "user-mode read: rc %d", rc);
    TEST_CHECK(frame_info_calls == 1 && hook_sum != 0);
    /* the block ends before the frame's last row: refused, the rest never read */
    block_size = 0xA000;
    TEST_ASSERT(mprotect(map + 0xA000, 0x6000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    TEST_ASSERT(mprotect(map, 0x10000, PROT_READ | PROT_WRITE) == 0);
    /* the exact edge: the block one byte short of the frame's last byte, nothing unreadable.
     * The read's last byte is first + (n-1)*pitch*bpp + width*bpp - 1 = 48,000-1; a block of
     * 47,999 B must refuse it, so a check that drops the last row's width or a row of the end
     * (which still fits 47,999) is caught here, not only by the unreadable bytes. */
    block_size = 48000 - 1;
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    block_size = 48000; /* exactly enough: read */
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == 60);
    block_size = 0x10000;
    /* the block begins after the frame's first row */
    block_base = (uintptr_t)map + 0x1000; block_size = 0xF000;
    TEST_ASSERT(mprotect(map, 0x1000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    /* a block this process cannot read */
    block_base = (uintptr_t)map; block_size = 0x10000; block_access = 0x02; /* write only */
    TEST_ASSERT(mprotect(map, 0x10000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    /* a frame of another size than the job's: refused before its block is asked */
    TEST_ASSERT(mprotect(map, 0x10000, PROT_READ | PROT_WRITE) == 0);
    block_access = SCE_KERNEL_MEMORY_ACCESS_R;
    frame_w = 240; frame_info_calls = 0;
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_FORMAT && !frame_info_calls);
    frame_w = 200;
    munmap(map, 0x10000);
    frame_at = NULL;
}
/* The request's flags become the job's policy (include/vjo_game_ocr.h): the
 * wait in 10 ms steps, its limit in 50 ms steps, settle_first, the blank-line
 * check; a request without them is API 19's job (a zero policy but for the
 * sleep callback, which only a wait uses). */
static void request_flags_become_the_policy(void)
{
    VjoGameOcrRequest r = setup();
    VjoVocrPolicy p;
    request_policy(&r, &p);
    TEST_CHECK(!p.settle_us && !p.settle_max_us && !p.settle_first && !p.blank && p.sleep == policy_sleep);
    r.flags = VJO_GAME_OCR_F_BLANK;
    request_policy(&r, &p);
    TEST_CHECK(p.blank == 1 && !p.settle_us && !p.settle_first);
    r.flags = VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    request_policy(&r, &p);
    TEST_CHECK(p.settle_us == 150000u && p.settle_max_us == 2000000u && p.settle_first == 1 && !p.blank);
    r.flags = 255u << VJO_GAME_OCR_SETTLE_SHIFT | 255u << VJO_GAME_OCR_SETTLE_MAX_SHIFT; /* the largest the bits hold */
    request_policy(&r, &p);
    TEST_CHECK(p.settle_us == 2550000u && p.settle_max_us == 12750000u && !p.settle_first);
}

/* API 20: the job's policy rides in the request's flags. A wait of 150 ms
 * (15 x 10 ms) first, at 60 frames a second, on a still screen: the job reads
 * the region until it held still, records the wait as a step, and only then
 * finds its line; the blank-line check passes a line of strokes. Unknown flag
 * bits are refused before anything is allocated. */
static void policy_in_the_flags(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    unsigned plain;
    line();
    frame_us = 16667;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    plain = rows_calls;
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_SETTLE);
    rows_calls = 0;
    r.flags = VJO_GAME_OCR_F_BLANK | VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT |
              40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    const VjoGameOcrStep *settle = NULL;
    unsigned blank = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_SETTLE) settle = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_BLANK) blank++;
    }
    TEST_ASSERT(settle != NULL);
    TEST_CHECK(settle->rc == VJO_GSETTLE_HELD && settle->arg >= 2 && settle->bytes == 0 && settle->us >= 150000);
    TEST_MSG("waited %u us in %u passes; %u rows calls against %u without", (unsigned)settle->us, settle->arg,
             rows_calls, plain);
    TEST_CHECK(!blank && out.lines == 1 && rows_calls > plain && !memory_live && !capture_live);
    allocations = 0;
    r.flags = 0x4000u; /* bits 14-15 mean nothing yet */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
}

/* The step log: a retried job logs its first pass and its signature once (the
 * TEAR step counts the tries), and when the log fills up the other steps make
 * room for the release of every block the worker holds, so the FREE steps that
 * show the job gave its memory back are never the ones lost. */
static void step_log_keeps_the_releases(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    unsigned pass = 0, sig = 0, tear = 0, frees = 0;
    line();
    change_at_rows = 2; /* during the finder's second pass of the first try */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    for (unsigned i = 0; i < out.n_steps; i++) {
        pass += out.step[i].id == VJO_GSTEP_PASS && !out.step[i].rc;
        sig += out.step[i].id == VJO_GSTEP_SIG;
        if (out.step[i].id == VJO_GSTEP_TEAR) { tear++; TEST_CHECK(out.step[i].rc >= 2); }
        frees += out.step[i].id == VJO_GSTEP_FREE;
    }
    TEST_CHECK(tear == 1 && pass == 1 && sig == 1 && frees == 2 && !memory_live && !capture_live);
    TEST_MSG("%u steps: %u PASS, %u SIG, %u TEAR, %u FREE", out.n_steps, pass, sig, tear, frees);
    /* every block the job took is back, and counted back */
    TEST_CHECK(blocks_held == (unsigned)(weights_live ? 1 : 0));
    /* a full log: with two blocks held that the job frees, the 19th and 20th steps are the two
     * FREEs; verified resident weights (kept for the next job) keep no slot */
    VjoGameOcrResult log;
    memset(&log, 0, sizeof(log));
    log_to = &log;
    TEST_CHECK(blocks_kept() == (unsigned)(weights_live ? 1 : 0));
    blocks_held = 2 + blocks_kept();
    for (unsigned i = 0; i < VJO_GAME_OCR_STEPS; i++) step_add(VJO_GSTEP_ALLOC, 0, 0, 0, 0, 0, 1, 0);
    TEST_CHECK(log.n_steps == VJO_GAME_OCR_STEPS - 2);
    TEST_CHECK(step_add(VJO_GSTEP_FREE, 0, 1, 0, 4096, 0, 1, 0) && step_add(VJO_GSTEP_FREE, 0, 2, 0, 4096, 0, 1, 0));
    TEST_CHECK(log.n_steps == VJO_GAME_OCR_STEPS && !step_add(VJO_GSTEP_FREE, 0, 3, 0, 4096, 0, 1, 0));
    TEST_CHECK(log.step[VJO_GAME_OCR_STEPS - 1].id == VJO_GSTEP_FREE && log.step[VJO_GAME_OCR_STEPS - 1].arg == 2);
    log_to = NULL;
    blocks_held = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
}

/* API 20: a recognized job returns its text normalised, the recognizer's own
 * text after its NUL, and a NORM step; the last byte stays 0 (the kernel's check). */
static void returned_text_is_normalised_and_kept(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("VJO_VOCR_MODEL_DIR not set"); return; }
    line();
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.text[0] && !out.text[sizeof(out.text) - 1]);
    const char *raw = vjo_text_raw(out.text, sizeof(out.text));
    char again[sizeof(out.text)];
    snprintf(again, sizeof(again), "%s", raw);
    vjo_text_normalize(again, NULL);
    TEST_CHECK(raw[0] && !strcmp(again, out.text));
    unsigned norm = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_NORM) { norm++; TEST_CHECK(out.step[i].us > 0); }
    TEST_CHECK(norm == 1);
    TEST_MSG("text '%s', raw '%s'", out.text, raw);
}

/* A try that read a line, then saw the screen change, and a retry that found
 * the box empty: the job returns no text, and nothing of the first try after
 * the NUL either, which the Shell would log as the recognizer's text
 * (audit-overlay-dialogue-3 B2). A 200x100 region is two strips, with a line in
 * each: the first line is read before the second strip is read again. The box
 * empties at each rows call in turn, so one of them lands between the two. */
static void a_retry_to_an_empty_box_returns_nothing(void)
{
    unsigned torn_empty = 0;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("VJO_VOCR_MODEL_DIR not set"); return; }
    for (unsigned at = 2; at <= 12; at++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        size_t stale = 0;
        int torn = 0;
        frame_h = r.fb_height = r.height = 100;
        for (unsigned y = 15; y < 39; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
        for (unsigned y = 70; y < 94; y++) for (unsigned x = 13; x < 190; x += 7) memset(pixels + (y * 200 + x) * 4, 235, 5);
        clear_at_rows = at;
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
        if (out.rc || out.text[0]) continue;
        for (unsigned i = 0; i < out.n_steps; i++) torn |= out.step[i].id == VJO_GSTEP_TEAR;
        for (size_t i = 0; i < sizeof(out.text); i++) stale += out.text[i] != 0;
        TEST_CHECK(!stale && !vjo_text_raw(out.text, sizeof(out.text))[0]);
        TEST_MSG("box emptied at rows call %u: %zu bytes left after the NUL", at, stale);
        torn_empty += torn;
    }
    TEST_CHECK(torn_empty > 0); /* some job did retry into the empty box */
}

/* Whether the job freed block `gitem` before it allocated its workspace: the
 * memory check counted that block as released, so it must go first. */
static int freed_before_the_workspace(const VjoGameOcrResult *out, unsigned gitem)
{
    int freed = -1, alloc = -1;
    for (unsigned i = 0; i < out->n_steps; i++) {
        if (freed < 0 && out->step[i].id == VJO_GSTEP_FREE && out->step[i].arg == gitem) freed = (int)i;
        if (alloc < 0 && out->step[i].id == VJO_GSTEP_ALLOC && out->step[i].arg == VJO_GITEM_WORKSPACE) alloc = (int)i;
    }
    return freed >= 0 && alloc >= 0 && freed < alloc;
}

/* The memory check's choices (check_with_lm), from the tables' sizes alone (no
 * files): room for everything, the cascade with the largest LM tier; for the LM
 * alone or the cascade alone but not both, the LM, which brings more on the
 * dialogue lines measured; for neither, greedy. A resident LM the job gives back
 * because it chose none counts as free (audit-overlay-dialogue-4 N1). */
static void memory_check_choices(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult o;
    const VjoVocrModel *m = vjo_vocr_model(VJO_VOCR3_FL10_NAME), *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME), *casc;
    const VjoVocrLm *lm, *lm2 = vjo_vocr_lm_tier(2);
    size_t jb = vjo_vocr_job_bytes(VJO_VOCR3_FL10_NAME, r.width, r.height), cb = vjo_vocr_capture_bytes(r.width, r.height);
    uint32_t need_none, need_lm, need_casc, need_both;
    TEST_ASSERT(m && w20 && lm2 && jb && cb);
    memset(&o, 0, sizeof(o));
    o.free_user = INT32_MAX;
    check_memory(&o, m, jb, cb, NULL, NULL);
    need_none = o.need_workspace + o.need_weights;
    check_memory(&o, m, jb, cb, lm2, NULL);
    need_lm = o.need_workspace + o.need_weights;
    check_memory(&o, m, jb, cb, NULL, w20);
    need_casc = o.need_workspace + o.need_weights;
    check_memory(&o, m, jb, cb, lm2, w20);
    need_both = o.need_workspace + o.need_weights;
    TEST_ASSERT(need_none < need_lm && need_lm < need_casc && need_casc < need_both);
    /* room for everything */
    casc = w20;
    o.free_user = (int32_t)(GAME_RESERVE + need_both);
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 2, &casc, &lm) && casc == w20 && lm == lm2);
    /* the LM alone or the cascade alone, not both: the LM */
    casc = w20;
    o.free_user = (int32_t)(GAME_RESERVE + need_casc);
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 2, &casc, &lm) && !casc && lm == lm2);
    /* no LM allowed: the cascade alone */
    casc = w20;
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 0, &casc, &lm) && casc == w20 && !lm);
    /* room for neither: greedy, and the job still fits */
    casc = w20;
    o.free_user = (int32_t)(GAME_RESERVE + need_none);
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 2, &casc, &lm) && !casc && !lm);
    /* the 1 MB LM resident and no LM chosen: the job gives it back, so its block counts as free */
    lm_res.lm = lm2;
    lm_uid = 12;
    o.free_user = (int32_t)(GAME_RESERVE + need_none - PAGE_ROUND(lm2->bytes) + 4096u);
    casc = NULL;
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 0, &casc, &lm) && !lm && o.release_bytes == PAGE_ROUND(lm2->bytes));
    lm_res.lm = NULL;
    lm_uid = -1;
}

/* The cascade step's fields, from the job's figures (include/vjo_game_ocr.h):
 * kind the lines it read as empty, arg the lines read again, the lines it
 * changed << 24, those left for the ceiling << 16, its frames; each clamped to
 * its field. */
static void the_cascade_step_carries_its_figures(void)
{
    VjoGameOcrResult log;
    VjoVocrStats st;
    memset(&log, 0, sizeof(log));
    memset(&st, 0, sizeof(st));
    log_to = &log;
    st.cascade_lines = 4; st.cascade_changed = 3; st.cascade_skipped = 2; st.cascade_blanked = 1;
    st.cascade_frames = 300; st.cascade_loaded = 1; st.cascade_load_us = 290000; st.cascade_us = 128000;
    st.cascade_rc = VJO_E_OOM;
    cascade_step(&st);
    st.cascade_changed = 100; st.cascade_skipped = 300; st.cascade_frames = 70000; st.cascade_loaded = 0;
    st.cascade_cached = 1; /* past each field: clamped, never spilling into the next */
    cascade_step(&st);
    log_to = NULL;
    TEST_ASSERT(log.n_steps == 2);
    const VjoGameOcrStep *a = &log.step[0], *b = &log.step[1];
    TEST_CHECK(a->id == VJO_GSTEP_CASCADE && a->kind == 1 && a->arg == 4 && a->rc == VJO_E_OOM);
    TEST_CHECK(a->bytes == (VJO_GLM_READ | 3u << 24 | 2u << 16 | 300u) && a->sum == 290000u && a->us == 128001u);
    TEST_CHECK(b->bytes == (VJO_GLM_RESIDENT | 0x3Fu << 24 | 0xFFu << 16 | 0xFFFFu));
}

/* Between jobs the worker gives back an LM or cascade block whose contents were
 * never verified (a read refused half-way, the job cancelled), as it does the
 * weights' (cleanup). */
static void cleanup_gives_back_unverified_blocks(void)
{
    VjoGameOcrRequest r = setup();
    (void)r;
    available = 64 * 1024 * 1024;
    TEST_ASSERT(cascade_alloc(NULL, VJO_VOCR_CASCADE_BYTES) != NULL && cascade_live && cascade_uid >= 0);
    TEST_ASSERT(lm_alloc(NULL, vjo_vocr_lm_tier(2)->bytes) != NULL && lm_live && lm_uid >= 0);
    TEST_CHECK(blocks_held == 2 && !blocks_kept()); /* neither verified: both keep a slot of the log */
    TEST_CHECK(cleanup() == 0 && !cascade_live && cascade_uid < 0 && !lm_live && lm_uid < 0);
    TEST_CHECK(cascade_frees == 1 && lm_frees == 1 && !blocks_held);
}

/* A game that is no longer the active one gets every resident block back from
 * the worker's loop, also when only the cascade's is left (the LM off, the
 * weights already gone: audit-overlay-dialogue-4 N2). */
static void an_inactive_game_gets_the_cascade_block_back(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char w20[600];
    FILE *f;
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001;
    r.flags = 8u << VJO_GAME_OCR_CASCADE_SHIFT; /* ocr_lm = off, the cascade on */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(weights_live && cascade_live && !lm_live);
    TEST_CHECK(!drop_weights() && !weights_live && cascade_live); /* say a later job's model failed to open */
    quit_on_delay = 1;
    worker_main(0, NULL); /* vjoOcrRegister: not the active game */
    quit_on_delay = 0;
    quitting = 0;
    TEST_CHECK(!cascade_live && cascade_uid < 0 && !cascade_res.model && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* The step log of the job that first reads everything: the self-test's first
 * stage (the fallback forced, the 1 MB LM, the cascade re-reading the line, the
 * stable-text wait). Verified resident blocks keep no slot, so its lm, cascade and
 * norm steps (the LM's and the larger model's first read times) are all there,
 * and the job's own two releases are its last steps (audit-overlay-dialogue-4 B1). */
static void the_first_job_logs_the_lm_cascade_and_norm(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char lm2[600], w20[600];
    FILE *f;
    snprintf(lm2, sizeof(lm2), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_LM2_NAME);
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(lm2, "rb"))) { TEST_SKIP("no LM beside the release weights"); return; }
    fclose(f);
    if (!(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    frame_us = 16667;
    test_cascade_conf = 1001;
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK | 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT |
              15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && out.fallback == 1);
    unsigned lm = 0, casc = 0, norm = 0, settle = 0, frees = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        lm += out.step[i].id == VJO_GSTEP_LM && (out.step[i].bytes & VJO_GLM_READ) && out.step[i].kind == 2;
        casc += out.step[i].id == VJO_GSTEP_CASCADE && (out.step[i].bytes & VJO_GLM_READ) && out.step[i].arg == 1 &&
                (out.step[i].bytes & 0xFFFFu) > 0 && out.step[i].kind <= out.step[i].arg; /* W20's frames */
        norm += out.step[i].id == VJO_GSTEP_NORM;
        settle += out.step[i].id == VJO_GSTEP_SETTLE;
        frees += out.step[i].id == VJO_GSTEP_FREE;
    }
    TEST_CHECK(lm == 1 && casc == 1 && settle == 1 && frees == 2 && out.n_steps <= VJO_GAME_OCR_STEPS);
    TEST_CHECK(norm == !!out.text[0]);
    /* the fallback's check names what it chose: the 1 MB LM's tier and the cascade (8) */
    unsigned fb_check = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_CHECK && out.step[i].arg == 1) fb_check = out.step[i].kind;
    TEST_CHECK(fb_check == (2u | 8u));
    TEST_CHECK(out.n_steps >= 2 && out.step[out.n_steps - 1].id == VJO_GSTEP_FREE &&
               out.step[out.n_steps - 2].id == VJO_GSTEP_FREE);
    TEST_CHECK(weights_live && lm_live && cascade_live && blocks_held == 3 && blocks_kept() == 3);
    TEST_MSG("%u steps: lm %u cascade %u norm %u settle %u free %u", out.n_steps, lm, casc, norm, settle, frees);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live && !cascade_live);
    /* the same first job with the refinement (a PSP game's): its step too, all within the log */
    {
        VjoGameOcrRequest p = setup(); VjoGameOcrResult o;
        unsigned seen = 0;
        line();
        available = 64 * 1024 * 1024;
        frame_us = 16667;
        test_cascade_conf = 1001;
        snprintf(p.model, sizeof(p.model), "H15_w8.vocr");
        snprintf(p.fallback, sizeof(p.fallback), "FL10_w8.vocr");
        p.flags = r.flags | VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
        TEST_ASSERT(vjo_game_ocr_run_request(&p, &o) == 0 && o.fallback == 1);
        for (unsigned i = 0; i < o.n_steps; i++) {
            unsigned id = o.step[i].id;
            seen |= (id == VJO_GSTEP_REFINE) << 0 | (id == VJO_GSTEP_LM) << 1 | (id == VJO_GSTEP_CASCADE) << 2 |
                    (id == VJO_GSTEP_NORM && o.text[0]) << 3;
        }
        TEST_CHECK(seen == (o.text[0] ? 15u : 7u) && o.n_steps <= VJO_GAME_OCR_STEPS &&
                   o.step[o.n_steps - 1].id == VJO_GSTEP_FREE && o.step[o.n_steps - 2].id == VJO_GSTEP_FREE);
        TEST_MSG("with the refinement: %u steps, seen 0x%x", o.n_steps, seen);
        TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live && !cascade_live);
    }
}

/* API 20, the beam + LM decoder: with the LMs beside the models, a request
 * allowing tier 4 reads the 4 MB LM once (a resident USER block, like the
 * weights), decodes every line with it, records an LM step and the lines'
 * confidence, and keeps the greedy text after the shown one; the next job finds
 * it resident. Too little memory for it: a smaller tier; for none: greedy. A
 * request allowing no LM gives a resident one back. */
static void lm_jobs(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char lm4[600];
    FILE *f;
    snprintf(lm4, sizeof(lm4), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_LM4_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(lm4, "rb"))) { TEST_SKIP("no LM beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    const VjoGameOcrStep *lm = NULL;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_LM) lm = &out.step[i];
    TEST_ASSERT(lm != NULL);
    TEST_CHECK(lm->kind == 4 && lm->rc == 0 && (lm->bytes & VJO_GLM_READ) && (lm->bytes & 0xFFFF) == out.recognized);
    TEST_CHECK(lm_live && lm_res.lm == vjo_vocr_lm_tier(4) && lm_allocations == 1 && lm_opens == 1);
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_LM) &&
               (out.line[0].flags & VJO_VOCR_LINE_CONF) <= 1000);
    TEST_CHECK(vjo_text_raw(out.text, sizeof(out.text))[0] && !out.text[sizeof(out.text) - 1]);
    TEST_MSG("text '%s' greedy '%s' conf %u", out.text, vjo_text_raw(out.text, sizeof(out.text)),
             out.line[0].flags & VJO_VOCR_LINE_CONF);
    /* the next job: resident, not read again */
    unsigned reads = reads_count;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_LM) TEST_CHECK((out.step[i].bytes & VJO_GLM_RESIDENT) && out.step[i].kind == 4);
    TEST_CHECK(lm_allocations == 1 && reads_count - reads < 4); /* a stat, no 32 KiB reads of the LM */
    /* at most tier 2 allowed now: the resident 4 MB LM goes back first, the 1 MB one is read */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(lm_live && lm_res.lm == vjo_vocr_lm_tier(2) && lm_frees == 1 && lm_allocations == 2);
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_LM));
    TEST_CHECK(freed_before_the_workspace(&out, VJO_GITEM_LM));
    /* the LM switched off: the resident one goes, the job reads greedily (no LM step) */
    r.flags = 0;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!lm_live && !lm_res.lm && lm_frees == 2);
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_LM);
    TEST_CHECK(!(out.line[0].flags & VJO_VOCR_LINE_LM) && (out.line[0].flags & VJO_VOCR_LINE_CONF) == VJO_VOCR_LINE_CONF_NONE);
    /* room beside the resident weights for the 2 MB LM, not the 4 MB one: the 2 MB one */
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    available = (int)(live_bytes + 4u * 1024 * 1024 + 400000u + 2900000u);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(lm_res.lm && lm_res.lm->tier == 3 && lm_live);
    TEST_MSG("tier %u with %d B free", lm_res.lm ? lm_res.lm->tier : 0, available - (int)live_bytes);
    /* a fresh game with room for no LM at all: greedy, and the job still reads */
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !lm_live && !weights_live);
    r = setup();
    line();
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    /* FL10 greedy needs 806,912 B beside the reserve, with the 0.5 MB LM 1,368,064 B */
    available = (int)(4u * 1024 * 1024 + 900000u);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!lm_live && !lm_res.lm && out.recognized >= 1 && !(out.line[0].flags & VJO_VOCR_LINE_LM));
    TEST_MSG("lm_live %d lm %p recognized %u flags 0x%x free %d need %u+%u", lm_live, (void *)lm_res.lm, out.recognized,
             out.line[0].flags, out.free_user, out.need_workspace, out.need_weights);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !lm_live);
}

/* API 20, the confidence cascade: a request with a time ceiling and room for
 * it reads its doubtful lines again with the larger pinned model (forced here:
 * every line), whose weights become a resident USER block; the next job finds
 * them resident; a request without the ceiling gives the block back; a game
 * with room for the LM but not the cascade runs without it. */
static void cascade_jobs(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char w20[600];
    FILE *f;
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    const VjoGameOcrStep *c = NULL, *check = NULL;
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_CASCADE) c = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_CHECK && !check) check = &out.step[i];
    }
    TEST_ASSERT(c != NULL && check != NULL);
    TEST_CHECK((check->kind & 8u) && (check->kind & 7u) == 2u); /* the check passed with the cascade and the 1 MB LM */
    TEST_CHECK(c->rc == 0 && c->arg == out.recognized && (c->bytes & VJO_GLM_READ) && !((c->bytes >> 16) & 0xFFu));
    TEST_CHECK(cascade_live && cascade_res.model == vjo_vocr_model(VJO_VOCR_CASCADE_NAME) && cascade_allocations == 1 &&
               cascade_opens == 1 && cascade_block == PAGE_ROUND(VJO_VOCR_CASCADE_BYTES));
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    TEST_CHECK(!memory_live && !capture_live && blocks_held == 3u); /* weights, LM, cascade */
    TEST_MSG("text '%s', %u steps, cascade step arg %u bytes 0x%08X", out.text, out.n_steps, c->arg, (unsigned)c->bytes);
    /* the next job: resident */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    c = NULL;
    for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_CASCADE) c = &out.step[i];
    TEST_CHECK(c && (c->bytes & VJO_GLM_RESIDENT) && cascade_allocations == 1 && cascade_opens == 2);
    /* no ceiling in the request: the block goes back, no cascade step */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && !cascade_res.model && cascade_frees == 1);
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_CASCADE);
    TEST_CHECK(!(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    /* room for the model and the LM, not for the cascade too: the LM alone */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    available = (int)(live_bytes + 4u * 1024 * 1024 + 500000u);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && lm_live && !(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    for (unsigned i = 0; i < out.n_steps; i++) {
        TEST_CHECK(out.step[i].id != VJO_GSTEP_CASCADE);
        if (out.step[i].id == VJO_GSTEP_CHECK) TEST_CHECK(!(out.step[i].kind & 8u));
    }
    /* resident again, then a game with no room for the job at all: refused, and every
     * resident block (weights, LM, the cascade's) goes back to the game */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    available = 64 * 1024 * 1024;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(cascade_live && lm_live && weights_live);
    /* everything resident, and too little room for the cascade with any LM tier (counting what
     * each would release) though plenty for the LM alone: the LM, and the cascade's block goes
     * back to the game */
    {
        VjoGameOcrResult o;
        int64_t least = INT64_MAX;
        memset(&o, 0, sizeof(o));
        o.free_user = INT32_MAX;
        for (unsigned t = 1; t <= 2; t++) {
            check_memory(&o, vjo_vocr_model(r.model), vjo_vocr_job_bytes(r.model, r.width, r.height),
                         vjo_vocr_capture_bytes(r.width, r.height), vjo_vocr_lm_tier(t), cascade_res.model);
            int64_t need = (int64_t)o.need_workspace + o.need_weights - o.release_bytes;
            if (need < least) least = need;
        }
        available = (int)(live_bytes + GAME_RESERVE + least - 4096);
    }
    unsigned frees_before = cascade_frees;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && cascade_frees == frees_before + 1 && !cascade_res.model && lm_live && weights_live);
    TEST_CHECK(lm_res.lm == vjo_vocr_lm_tier(2) && freed_before_the_workspace(&out, VJO_GITEM_CASCADE));
    TEST_CHECK(out.recognized >= 1 && !(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    /* resident again */
    available = 64 * 1024 * 1024;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(cascade_live && lm_live && weights_live);
    available = (int)(live_bytes + 1000000u); /* under the reserve, even with them given back */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
    TEST_CHECK(!cascade_live && !lm_live && !weights_live && !cascade_res.model && !lm_res.lm && !resident.model);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !cascade_live && !lm_live && !weights_live);
}

/* Glyph-like strokes lh rows tall across the frame (as test_vocr_policy.c's). */
static void strokes_at(unsigned y0, unsigned lh)
{
    unsigned pitch = 6u + (lh > 12u ? lh / 2u : 6u);
    for (unsigned y = y0; y < y0 + lh; y++)
        for (unsigned x = 10; x < 190; x++) {
            unsigned cx = (x - 10) % pitch, cy = y - y0;
            if (cx < pitch * 3u / 4u && (cx % 4u < 2u || cy % 7u < 2u || (cx + cy) % 9u == 0u))
                memset(pixels + ((size_t)y * 200 + x) * 4, 235, 3);
        }
}

/* The REFINE step of a job's log, or NULL. */
static const VjoGameOcrStep *refine_step(const VjoGameOcrResult *out)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == VJO_GSTEP_REFINE) return &out->step[i];
    return NULL;
}

/* API 20, the finder's boxes refined: on, or in PSP games only (the worker's own
 * title: NPXS10028 or a PSPEMU* bubble). The job's workspace holds the arena,
 * and a REFINE step reports it; mode 3 is refused before anything is allocated. */
static void refine_modes(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    static const struct { const char *title; int psp; } titles[] = {
        {"NPXS10028", 1}, {"PSPEMU005", 1}, {"PSPEMUCFW", 1}, {"PSPEMU", 1}, {"PCSG00415", 0}, {"PSPEM", 0},
        {"XPSPEMU01", 0}, {"pspemu001", 0}, {"NPXS10028X", 0}, {"NPXS1002", 0}, {"NPXS10027", 0}, {"", 0}};
    for (unsigned i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        TEST_CHECK_(vjo_game_is_psp(titles[i].title) == titles[i].psp, "%s", titles[i].title);
    line();
    struct { unsigned mode; int psp; int refined; } cases[] = {
        {0, 1, 0}, {VJO_GAME_OCR_REFINE_ON, 0, 1}, {VJO_GAME_OCR_REFINE_PSP, 0, 0}, {VJO_GAME_OCR_REFINE_PSP, 1, 1}};
    unsigned plain_block = 0;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const VjoGameOcrStep *rs;
        r = setup();
        line();
        psp_title = cases[c].psp;
        r.flags = cases[c].mode << VJO_GAME_OCR_REFINE_SHIFT;
        int rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK(rc == 0 || rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
        rs = refine_step(&out);
        TEST_CHECK_((rs != NULL) == cases[c].refined, "mode %u psp %d", cases[c].mode, cases[c].psp);
        if (rs) TEST_CHECK(rs->kind == cases[c].mode && rs->rc == 0);
        /* the boxes are refined as they are read, after the weights: without the release
         * files (CI) the job ends at the model, so only a job that read its lines counts them */
        if (rs && rc == 0)
            TEST_CHECK((rs->bytes >> 16) == out.lines && rs->arg >= out.lines && rs->sum > 0);
        else if (rs)
            TEST_CHECK(!(rs->bytes >> 16) && !rs->arg && !rs->sum);
        if (!c) plain_block = job_block;
        else if (cases[c].refined)
            TEST_CHECK(job_block == PAGE_ROUND(vjo_vocr_job_bytes(r.model, r.width, r.height) +
                                               vjo_vocr_refine_bytes(r.width, r.height)) && job_block >= plain_block);
        TEST_MSG("mode %u psp %d: workspace %u B (plain %u B)", cases[c].mode, cases[c].psp, job_block, plain_block);
    }
    /* a job that finds no line logs no refine step */
    r = setup();
    r.flags = VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.lines && !refine_step(&out));
    /* mode 3, also in a probe: refused before anything, as unknown bits are */
    r = setup();
    allocations = 0;
    r.flags = 3u << VJO_GAME_OCR_REFINE_SHIFT;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
    r.flags |= VJO_GAME_OCR_F_PROBE;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps && !capture_allocs);
    psp_title = 0;
}

/* The refine step's fields, from a job that reads its lines (the release weights):
 * pairs of text rows set one row apart, each pair one finder box that the refinement
 * splits, nine of them in a 200 x 272 region: seven split, the last two not (their
 * split would pass 16 lines), 16 lines, and the rows the refinement read. */
static void the_refine_step_carries_its_counts(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char path[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, r.model);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(path, "rb"))) { TEST_SKIP("no release weights"); return; }
    fclose(f);
    frame_h = r.fb_height = r.height = 272;
    for (unsigned k = 0; k < 9; k++) {
        strokes_at(4 + k * 29, 11);
        strokes_at(4 + k * 29 + 12, 11);
    }
    r.flags = VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    const VjoGameOcrStep *rs = refine_step(&out);
    TEST_ASSERT(rs != NULL);
    TEST_CHECK(out.lines == 9 && rs->rc == 0 && rs->arg == 16 && (rs->bytes >> 16) == 9 &&
               ((rs->bytes >> 8) & 0xFFu) == 2 && (rs->bytes & 0xFFu) == 7 && rs->sum >= 9u * 23u && rs->us > 0);
    TEST_MSG("%u boxes: refine arg %u bytes 0x%08X sum %u", out.lines, (unsigned)rs->arg, (unsigned)rs->bytes,
             (unsigned)rs->sum);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* module_start records the game's title: in psp mode a PSP game's jobs refine
 * (Adrenaline's NPXS10028, a PSPEMU* bubble), CLANNAD's do not. */
static void module_start_sets_the_psp_title(void)
{
    static const struct { const char *title; int psp; } games[] = {{"NPXS10028", 1}, {"PSPEMU001", 1}, {"PCSG00415", 0}};
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        psp_title = !games[i].psp; /* only module_start can make it right */
        strcpy(own_title, games[i].title);
        TEST_CHECK_(!module_start(0, NULL) && thread_creates == 1 && psp_title == games[i].psp, "%s", games[i].title);
        line();
        r.flags = VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT;
        int rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK(rc == 0 || rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
        TEST_CHECK_((refine_step(&out) != NULL) == games[i].psp, "%s", games[i].title);
        TEST_CHECK(!module_stop(0, NULL));
    }
    psp_title = 0;
}

/* The self-test's fallback stage: the memory check refuses the model on
 * purpose and the fallback runs. */
static void forced_fallback_runs_the_fallback(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.fallback == 1);
    int forced = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_CHECK && out.step[i].arg == 1 && out.step[i].sum == 1) forced = 1;
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_FALLBACK);
    TEST_CHECK(forced);
    /* with the refinement: the fallback's workspace holds its arena too, and its boxes are refined */
    r = setup();
    line();
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK | VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
    int rc = vjo_game_ocr_run_request(&r, &out);
    const VjoGameOcrStep *rs = refine_step(&out);
    TEST_CHECK(rc == 0 || rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.fallback == 1 && rs && rs->rc == 0 &&
               job_block == PAGE_ROUND(vjo_vocr_job_bytes(r.fallback, r.width, r.height) +
                                       vjo_vocr_refine_bytes(r.width, r.height)));
    if (rs && !rc) TEST_CHECK(out.lines == 1 && (rs->bytes >> 16) == 1 && rs->arg == 1);
    TEST_MSG("rc %d, workspace %u B, refine rc %d", rc, job_block, rs ? (int)rs->rc : 1);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}
TEST_LIST = {
    {"100 real vocr jobs and game USER reserve", repeated_jobs_and_reserve},
    {"allocation base free failure and retry", memory_failure_and_retry},
    {"cancel before allocation and wire validation", cancel_and_wire},
    {"real model bounded short reads errors and cancellation", real_model_io_and_cancellation},
    {"model callbacks without weights timeouts errors and saved cleanup sequence", model_callbacks_timeouts_errors_and_cleanup},
    {"ALL system gate and retained thread joins", system_app_gate_and_failed_joins},
    {"capture memory ladder USER CDRAM PHYCONT each logged", capture_memory_ladder_is_logged},
    {"the kernel strip after a failed hook write; timeouts are not retried", hook_failure_falls_back_to_the_kernel_strip},
    {"BUSY rows calls: asked again before arming, the block kept after", busy_rows_calls},
    {"the self-test probe tries every rung", probe_tries_every_rung},
    {"the probe's user-mode frame read stays inside the frame's block", probe_user_read_stays_in_the_frame_block},
    {"the self-test fallback stage forces the fallback", forced_fallback_runs_the_fallback},
    {"the job policy rides in the flags: the wait, its step, unknown bits refused", policy_in_the_flags},
    {"the request's flags become the job's policy", request_flags_become_the_policy},
    {"the step log logs a retried pass once and keeps the releases", step_log_keeps_the_releases},
    {"the returned text is normalised, the recognizer's own kept after it", returned_text_is_normalised_and_kept},
    {"a retry to an empty box returns nothing of the first try", a_retry_to_an_empty_box_returns_nothing},
    {"the memory check: the cascade and the LM, else the LM, else the cascade; a resident LM given back counts",
     memory_check_choices},
    {"cleanup gives back LM and cascade blocks never verified", cleanup_gives_back_unverified_blocks},
    {"the cascade step carries its figures, each clamped to its field", the_cascade_step_carries_its_figures},
    {"the beam + LM decoder: read once, resident, a smaller tier when tight, given back when off", lm_jobs},
    {"the confidence cascade: doubtful lines read again, its model resident, given back, left when tight", cascade_jobs},
    {"an inactive game gets the cascade's block back", an_inactive_game_gets_the_cascade_block_back},
    {"the first job's step log keeps its lm, cascade and norm steps", the_first_job_logs_the_lm_cascade_and_norm},
    {"the refinement: on, in PSP games only by title, its step and arena; mode 3 refused", refine_modes},
    {"the refine step's counts: boxes, splits, the 16-line cap, rows", the_refine_step_carries_its_counts},
    {"module_start records whether the game is a PSP game", module_start_sets_the_psp_title},
    {NULL, NULL}
};
