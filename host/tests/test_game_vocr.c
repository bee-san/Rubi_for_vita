/* Real game worker and real vocr, with Vita scheduling/USER_RW/file boundaries.
 * No emulator claims: the caller PID and SDK responses are failure-injected. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "../../game_ocr/main.c"

static unsigned char workspace[2*1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char pixels[200*60*4];
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
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(size&4095));
    if (!strcmp(name, "VjoGameVocrWeights")) { /* resident verified weights, at most one block */
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory));
        weight_allocations++;
        if (alloc_failure) return -1;
        weights_live = 1; weights_block = size; live_bytes += size; return 10;
    }
    TEST_CHECK(!strcmp(name, "VjoGameVocr") && size <= sizeof(workspace) && !memory_live);
    allocations++;
    if (alloc_failure) return -1;
    memory_live = 1; job_block = size; live_bytes += size; return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 10) { TEST_CHECK(weights_live); *base = weights_memory; return 0; }
    TEST_CHECK(uid == 9 && memory_live); *base = base_failure ? NULL : workspace; return base_failure ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 10) {
        TEST_CHECK(weights_live); if (free_failure) return -1;
        weights_live = 0; weight_frees++; live_bytes -= weights_block; return 0;
    }
    TEST_CHECK(uid == 9 && memory_live); if (free_failure) return -1;
    memory_live = 0; frees++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ TEST_CHECK(stack == 32*1024 && entry == worker_main); thread_creates++; return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return start_failure ? -1 : 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return join_failure ? -1 : 0; }
int sceKernelDeleteThread(SceUID uid) { return delete_failure ? -1 : 0; }
int sceKernelDelayThread(unsigned us) { clock_value += us; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ TEST_CHECK(pid == 7); snprintf(out, size, "%s", own_title); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoOcrRegister(void) { return VJO_ERR_NO_GAME; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_job || (cancel_reads && reads_count >= (unsigned)cancel_reads); }
int vjoOcrComplete(const VjoGameOcrResult *out) { TEST_CHECK(!memory_live && !model); return 0; }
int vjoOcrReadRaw(uint32_t seq, uint32_t row, uint32_t n, void *dst)
{ TEST_CHECK(seq == 12 && row < 60 && n <= 60-row); memcpy(dst, pixels+row*200*4, n*200*4); return (int)n; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_open_calls++; return (int)0x80010013u; /* Physical game's ux0 is unavailable. */ }
static uint64_t model_mtime = 0x1234567;
static int model_stat_error;
int vjoOcrModelOpen(uint32_t seq, VjoGameOcrModelInfo *info)
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
    worker = workspace_uid = weights_uid = -1;
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
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .done_seq=11, .width=200, .height=60, .stride=800};
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
    TEST_CHECK(allocations == 100 && frees == 100);
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
    alloc_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !frees);
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
    r.width = 200; r.model[31] = 'x';
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
    const char *excluded[] = {"main", "NPXS10015", "VITASHELL", "VJOSET001", ""};
    for (unsigned i=0; i<sizeof(excluded)/sizeof(excluded[0]); i++) {
        strcpy(own_title, excluded[i]); TEST_CHECK(!module_start(0, NULL) && !thread_creates && !allocations);
    }
    strcpy(own_title, "PCSG00415"); TEST_CHECK(!module_start(0, NULL) && thread_creates == 1);
    join_failure = 1;
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20 && worker_started);
    join_failure = 0; TEST_CHECK(!module_stop(0, NULL) && worker == -1);
    start_failure = delete_failure = 1;
    TEST_CHECK(!module_start(0, NULL) && worker == 20 && !worker_started);
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20);
    delete_failure = 0; TEST_CHECK(!module_stop(0, NULL));
}
TEST_LIST = {
    {"100 real vocr jobs and game USER reserve", repeated_jobs_and_reserve},
    {"allocation base free failure and retry", memory_failure_and_retry},
    {"cancel before allocation and wire validation", cancel_and_wire},
    {"real model bounded short reads errors and cancellation", real_model_io_and_cancellation},
    {"model callbacks without weights timeouts errors and saved cleanup sequence", model_callbacks_timeouts_errors_and_cleanup},
    {"ALL system gate and retained thread joins", system_app_gate_and_failed_joins},
    {NULL, NULL}
};
