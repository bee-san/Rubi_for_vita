/* Real game worker and real vocr, with Vita scheduling/USER_RW/file boundaries.
 * No emulator claims: the caller PID and SDK responses are failure-injected. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "../../game_ocr/main.c"

static unsigned char workspace[2*1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char capture_mem[1024*1024] __attribute__((aligned(64)));
static unsigned char pixels[200*60*4];
/* The capture block and the memory-type ladder: which types refuse. */
static int capture_live, refuse_user, refuse_cdram, refuse_phycont, rows_error, hook_write_error;
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
    TEST_CHECK(uid == 9 && memory_live); *base = base_failure ? NULL : workspace; return base_failure ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(capture_live); if (free_failure) return -1; capture_live = 0; capture_frees++; live_bytes -= capture_block; return 0; }
    if (uid == 10) {
        TEST_CHECK(weights_live); if (free_failure) return -1;
        weights_live = 0; weight_frees++; live_bytes -= weights_block; return 0;
    }
    TEST_CHECK(uid == 9 && memory_live); if (free_failure) return -1;
    memory_live = 0; frees++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ TEST_CHECK(stack == 32*1024 && entry == worker_main && p == 0x10000100 + 31 && cpu == 0x70000); thread_creates++; return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return start_failure ? -1 : 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return join_failure ? -1 : 0; }
int sceKernelDeleteThread(SceUID uid) { return delete_failure ? -1 : 0; }
int sceKernelDelayThread(unsigned us) { clock_value += us; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ TEST_CHECK(pid == 7); snprintf(out, size, "%s", own_title); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
static int safe_boot_mode, boot_info_calls;
int vjoBootInfo(VjoBootInfo *out) { boot_info_calls++; TEST_CHECK(out->size == sizeof(*out)); out->mode = safe_boot_mode; return 0; }
int vjoOcrRegister(void) { return VJO_ERR_NO_GAME; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_job || (cancel_reads && reads_count >= (unsigned)cancel_reads); }
int vjoOcrComplete(const VjoGameOcrResult *out) { TEST_CHECK(!memory_live && !model && !capture_live); return 0; }
/* The kernel's rows call: grey rows of the 200x60 region into the worker's block. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->size == sizeof(*req) && req->seq == 12 && req->y < 60 && req->n && req->n <= 60 - req->y);
    TEST_CHECK(req->dst_stride >= 200 && info && info->size == sizeof(*info));
    TEST_CHECK(capture_live && (unsigned char *)dst >= capture_mem && (unsigned char *)dst < capture_mem + capture_block);
    rows_calls++;
    if (req->mode == VJO_GAME_OCR_SRC_KSTRIP) kstrip_calls++;
    info->fb_width = 200; info->fb_height = 60; info->fb_pitch = 200; info->fb_format = 0; info->wait_us = 16000;
    if (rows_error) return rows_error;
    if (hook_write_error && req->mode == VJO_GAME_OCR_SRC_HOOK) return VJO_ERR_COPY;
    for (unsigned r = 0; r < req->n; r++)
        for (unsigned x = 0; x < 200; x++) {
            const unsigned char *p = pixels + ((req->y + r) * 200 + x) * 4;
            ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
        }
    return (int)req->n;
}
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { return -1; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_open_calls++; return (int)0x80010013u; /* Physical game's ux0 is unavailable. */ }
static uint64_t model_mtime = 0x1234567;
static int model_stat_error;
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    if (open_busy) return VJO_ERR_BUSY;
    if (open_failure) return -123;
    TEST_ASSERT(!model && seq == 12);
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
    capture_block = capture_allocs = capture_frees = rows_calls = kstrip_calls = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    weight_allocations = weight_frees = 0; weights_live = 0; live_bytes = 0;
    model_mtime = 0x1234567; model_stat_error = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    allocations = frees = reads_count = bytes_read = thread_creates = 0;
    user_open_calls = 0;
    virtual_file = open_busy = read_busy = close_busy = read_eof = 0; clock_value = 0;
    memory_live = alloc_failure = base_failure = free_failure = close_failure = 0;
    join_failure = start_failure = delete_failure = budget_failure = cancel_reads = 0;
    cancel_job = short_reads = open_failure = bad_read = 0;
    available = 16*1024*1024;
    memset(pixels, 0, sizeof(pixels)); strcpy(own_title, "PCSG00415");
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
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_CAPTURE) == 0 && vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) == 12288);
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
    r.x = 0; r.flags = 0x80;
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
        if (out.step[i].id == VJO_GSTEP_FALLBACK && out.step[i].arg == 1) forced = 1;
    TEST_CHECK(forced);
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
    {"the self-test probe tries every rung", probe_tries_every_rung},
    {"the self-test fallback stage forces the fallback", forced_fallback_runs_the_fallback},
    {NULL, NULL}
};
