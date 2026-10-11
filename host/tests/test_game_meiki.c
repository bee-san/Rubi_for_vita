/* Real API13 game worker, pinned SHA, C preprocessing and reader bridge.
 * Inference is injected at the isolated module ABI boundary: no accuracy claim. */
#include "acutest.h"
#include <stdarg.h>
#include <stdlib.h>
#define VJO_GAME_OCR_NO_MODULE_ENTRY 1
#include "../../game_ocr/main.c"
#include "../../game_ocr/meiki_bridge.h"
#ifndef VJO_MEIKI_MODEL_DIR
#define VJO_MEIKI_MODEL_DIR ""
#endif
static struct {
    void *base;
    unsigned bytes;
    int live;
} memory[3];
static unsigned allocations, releases, opens, reads_count, read_bytes, module_loads, module_unloads,
    runs, row_reads;
static int available, query_fail, alloc_fail_at, mapping_fail, free_fail, invalidation_fail;
static int stop_fail, unload_fail, load_fail, abi_bad, cancel_job, cancel_read_at,
    identity_mismatch;
static int open_fail, read_fail_at, close_fail, corrupt, short_read, truncated, native_rc;
static int module_live, stopped, user_io, clock_ticks;
static unsigned free_accepted_count;
static FILE *file;
static char path[512];
static unsigned char pixels[200 * 32 * 4];
static char logs[16384];
static size_t log_used;
int sceClibPrintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(logs + log_used, sizeof(logs) - log_used, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(logs) - log_used)
        log_used += (size_t)n;
    return n;
}
SceUID sceKernelGetProcessId(void)
{
    return 7;
}
uint64_t sceKernelGetProcessTimeWide(void)
{
    return ++clock_ticks;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = available;
    return query_fail ? -1 : 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned bytes, void *opts)
{
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(bytes & 4095));
    allocations++;
    if (alloc_fail_at == (int)allocations)
        return -9;
    for (int i = 0; i < 3; i++)
        if (!memory[i].live) {
            TEST_ASSERT(!posix_memalign(&memory[i].base, 64, bytes));
            memory[i].bytes = bytes;
            memory[i].live = 1;
            return 100 + i;
        }
    TEST_CHECK(0);
    return -1;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid < 100 || uid >= 103 || !memory[uid - 100].live) {
        *base = NULL;
        return invalidation_fail ? -9 : SCE_KERNEL_ERROR_INVALID_UID;
    }
    *base = memory[uid - 100].base;
    if (identity_mismatch && runs)
        *base = (unsigned char *)*base + 64;
    return 0;
}
int sceKernelGetMemBlockInfoByRange(void *base, unsigned bytes, SceKernelMemBlockInfo *info)
{
    info->mappedBase = base;
    info->mappedSize = bytes;
    info->memoryType = SCE_KERNEL_MEMORY_TYPE_NORMAL;
    info->access = 6;
    info->type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    if (mapping_fail == 1)
        info->memoryType = 0;
    if (mapping_fail == 2)
        info->access = 4;
    if (mapping_fail == 3)
        info->mappedSize--;
    if (mapping_fail == 4)
        info->type = 0;
    if (mapping_fail == 5)
        info->mappedBase = NULL;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    TEST_ASSERT(uid >= 100 && uid < 103 && memory[uid - 100].live);
    if (free_fail)
        return -9;
    free(memory[uid - 100].base);
    memory[uid - 100].base = NULL;
    memory[uid - 100].live = 0;
    releases++;
    free_accepted_count++;
    return 0;
}
static int engine_run(const MeikiModuleRequest *r, MeikiModuleStats *stats)
{
    runs++;
    TEST_CHECK(module_live && !stopped && r->size == sizeof(*r) && !r->model_path && r->reader);
    TEST_ASSERT(r->reader->byte_count == VJO_MEIKI_MODEL_BYTES &&
                r->reader->size == sizeof(*r->reader));
    unsigned char bytes[8192];
    unsigned total = 0;
    TEST_CHECK(r->reader->read(r->reader->context, VJO_MEIKI_MODEL_BYTES, bytes, 1) < 0);
    TEST_CHECK(r->reader->read(r->reader->context, 0, bytes, 8193) < 0);
    memset(&stats->runtime, 0, sizeof(stats->runtime));
    for (unsigned off = 0; off < VJO_MEIKI_MODEL_BYTES;) {
        unsigned n = VJO_MEIKI_MODEL_BYTES - off;
        if (n > sizeof(bytes))
            n = sizeof(bytes);
        int rc = r->reader->read(r->reader->context, off, bytes, n);
        if (rc) {
            stats->runtime.model_read_error = rc;
            return -14;
        }
        off += n;
        total += n;
    }
    stats->runtime.model_read_bytes = total;
    stats->runtime.heap_peak = 13235648;
    stats->runtime.load_us = 10;
    stats->runtime.inference_us = 20;
    memset(r->output, 0, sizeof(*r->output));
    r->output->codes[0] = 0x732b;
    r->output->boxes[2] = 10;
    r->output->scores[0] = .9f;
    return native_rc;
}
static int engine_stop(MeikiModuleStats *stats)
{
    if (stop_fail)
        return -9;
    stopped = 1;
    return 0;
}
SceUID sceKernelLoadStartModule(const char *p, unsigned size, const void *args, unsigned f, void *o,
                                int *status)
{
    module_loads++;
    TEST_CHECK(!strcmp(p, VJO_MEIKI_APP0_MODULE_PATH));
    const MeikiModuleStart *s = args;
    TEST_CHECK(size == sizeof(*s) && s->abi == 3 && s->metadata_bytes == 4194304);
    if (load_fail)
        return -9;
    module_live = 1;
    stopped = abi_bad;
    *status = abi_bad ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_START_SUCCESS;
    *s->api_out = (MeikiModuleApi){.size = sizeof(MeikiModuleApi),
                                   .abi = abi_bad ? 2 : 3,
                                   .run = engine_run,
                                   .stop = engine_stop};
    return 200;
}
int sceKernelStopUnloadModule(SceUID uid, unsigned size, const void *args, unsigned f, void *o,
                              int *status)
{
    TEST_CHECK(uid == 200 && module_live && stopped);
    if (unload_fail)
        return -9;
    module_live = 0;
    module_unloads++;
    *status = 0;
    return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack,
                             unsigned a, int cpu, void *o)
{
    TEST_CHECK(stack == 65536);
    return 201;
}
int sceKernelStartThread(SceUID id, SceSize size, void *args)
{
    return 0;
}
int sceKernelWaitThreadEnd(SceUID id, int *status, void *timeout)
{
    return 0;
}
int sceKernelDeleteThread(SceUID id)
{
    return 0;
}
int sceKernelDelayThread(unsigned us)
{
    clock_ticks += us;
    return 0;
}
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize len)
{
    return -1;
}
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize len)
{
    return -1;
}
int vjoGetVersion(void)
{
    return VJO_API_VERSION;
}
int vjoOcrRegister(void)
{
    return VJO_ERR_NO_GAME;
}
int vjoOcrTake(VjoGameOcrRequest *out)
{
    return 1;
}
int vjoOcrCancelled(uint32_t seq)
{
    return cancel_job || (cancel_read_at && reads_count >= (unsigned)cancel_read_at);
}
int vjoOcrComplete(const VjoGameOcrResult *out)
{
    TEST_CHECK(!vjo_meiki_game_owned() && !file);
    return 0;
}
int vjoOcrReadRaw(uint32_t seq, uint32_t y, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && y < 32 && n <= 32 - y);
    row_reads++;
    memcpy(dst, pixels + y * 200 * 4, n * 200 * 4);
    return (int)n;
}
SceUID sceIoOpen(const char *p, int flags, unsigned mode)
{
    user_io++;
    return (int)0x80010013u;
}
int vjoOcrModelOpen(uint32_t seq)
{
    opens++;
    if (open_fail)
        return (int)0x80010013u;
    TEST_ASSERT(seq == 12 && !file);
    file = fopen(path, "rb");
    if (!file)
        return -9;
    return truncated ? VJO_MEIKI_MODEL_BYTES - 1 : VJO_MEIKI_MODEL_BYTES;
}
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t count, void *dst)
{
    TEST_ASSERT(seq == 12 && file && count <= 8192);
    reads_count++;
    if (read_fail_at && reads_count >= (unsigned)read_fail_at)
        return (int)0x80010013u;
    TEST_ASSERT(!fseek(file, offset, SEEK_SET));
    if (short_read && count > 37)
        count = 37;
    int got = fread(dst, 1, count, file);
    if (corrupt && offset == 0 && got)
        ((unsigned char *)dst)[0] ^= 1;
    read_bytes += got;
    return got;
}
int vjoOcrModelClose(uint32_t seq)
{
    TEST_CHECK(seq == 12);
    if (close_fail)
        return -9;
    if (file) {
        fclose(file);
        file = NULL;
    }
    return 0;
}
static VjoGameOcrRequest setup(void)
{
    stop_fail = unload_fail = free_fail = invalidation_fail = close_fail = identity_mismatch = 0;
    TEST_ASSERT(!vjo_game_ocr_worker_stop());
    TEST_ASSERT(!vjo_meiki_game_owned() && !module_live && !file);
    allocations = releases = opens = reads_count = read_bytes = module_loads = module_unloads =
        runs = row_reads = 0;
    available = VJO_MEIKI_REQUIRED_BYTES;
    query_fail = alloc_fail_at = mapping_fail = load_fail = abi_bad = cancel_job = cancel_read_at =
        0;
    open_fail = read_fail_at = corrupt = short_read = truncated = native_rc = 0;
    user_io = free_accepted_count = clock_ticks = 0;
    quitting = active_seq = 0;
    blocked = 0;
    log_used = 0;
    logs[0] = 0;
    memset(pixels, 90, sizeof(pixels));
    VjoGameOcrRequest r = {
        .size = sizeof(r), .seq = 12, .done_seq = 11, .width = 200, .height = 32, .stride = 800};
    strcpy(r.model, VJO_MEIKI_MODEL_NAME);
    strcpy(r.model_dir, VJO_VOCR_MODEL_ROOT);
    snprintf(path, sizeof(path), "%s/%s", VJO_MEIKI_MODEL_DIR, VJO_MEIKI_MODEL_NAME);
    return r;
}
static int have_model(void)
{
    if (!VJO_MEIKI_MODEL_DIR[0]) {
        TEST_CHECK(0);
        TEST_MSG("Pinned model required: configure VJO_MEIKI_MODEL_DIR to run integration tests");
        return 0;
    }
    return 1;
}
static void budget_and_invalid(void)
{
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    const int free_values[] = {27647999, -1, 0, 536870913};
    for (unsigned i = 0; i < 4; i++) {
        available = free_values[i];
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
        TEST_CHECK(!allocations && !opens && !out.text[0] && out.workspace_bytes == 21356544);
    }
    available = 27648000;
    query_fail = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !opens);
    TEST_CHECK(strstr(logs, "USER_total=unavailable maxchunk=unavailable"));
    query_fail = 0;
    r.height = 125;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_REGION && !opens && !allocations);
    r.height = 32;
    r.model[31] = 'x';
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !opens);
}
static void exact_reader_success(void)
{
    if (!have_model())
        return;
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    short_read = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(!strcmp(out.text, "猫") && out.lines == 1 && out.workspace_bytes == 21356544);
    TEST_CHECK(opens == 2 && read_bytes == 2 * 5392856u && allocations == 3 && releases == 3 &&
               module_loads == 1 && module_unloads == 1 && !user_io);
    TEST_CHECK(!file && !vjo_meiki_game_owned() && !out.cleanup_status && out.load_us == 10 &&
               out.recognize_us == 20);
    TEST_CHECK(strstr(logs, "required=27648000") && strstr(logs, "reader_bytes=5392856"));
    TEST_CHECK(!vjo_game_ocr_worker_start());
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}
static void models_fail_before_allocation(void)
{
    if (!have_model())
        return;
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    corrupt = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && !allocations &&
               !out.text[0]);
    corrupt = 0;
    truncated = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && !allocations);
    truncated = 0;
    open_fail = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL &&
               out.io_error == (int)0x80010013u && !allocations);
    open_fail = 0;
    read_fail_at = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL &&
               out.io_error == (int)0x80010013u && !allocations);
    read_fail_at = 0;
    cancel_read_at = reads_count + 2;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !allocations && !file);
}
static void allocation_and_mapping_failures(void)
{
    if (!have_model())
        return;
    VjoGameOcrResult out;
    for (int i = 1; i <= 3; i++) {
        VjoGameOcrRequest r = setup();
        alloc_fail_at = i;
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
        TEST_CHECK(!vjo_meiki_game_owned() && releases == (unsigned)i - 1 && !out.text[0]);
    }
    for (int i = 1; i <= 5; i++) {
        VjoGameOcrRequest r = setup();
        mapping_fail = i;
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
        TEST_CHECK(!vjo_meiki_game_owned() && allocations == 1 && releases == 1);
    }
    VjoGameOcrRequest r = setup();
    load_fail = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!vjo_meiki_game_owned() && releases == 3);
    r = setup();
    abi_bad = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!vjo_meiki_game_owned() && !module_live && module_unloads == 1 && releases == 3);
}
static void stop_unload_close_retention(void)
{
    if (!have_model())
        return;
    VjoGameOcrResult out;
    for (int i = 0; i < 3; i++) {
        VjoGameOcrRequest r = setup();
        if (i == 0)
            stop_fail = 1;
        if (i == 1)
            unload_fail = 1;
        if (i == 2)
            free_fail = 1;
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE &&
                   out.cleanup_status && !out.text[0] && blocked);
        TEST_CHECK(vjo_meiki_game_owned());
        unsigned before = allocations;
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE &&
                   before == allocations);
        TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE);
        stop_fail = unload_fail = free_fail = 0;
        TEST_CHECK(!vjo_game_ocr_worker_stop() && !vjo_meiki_game_owned());
    }
    VjoGameOcrRequest r = setup();
    close_fail = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && file && blocked);
    close_fail = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !file);
}
static void reader_failure_recovery(void)
{
    if (!have_model())
        return;
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    read_fail_at = (5392856u + 8191u) / 8192u + 2;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL &&
               out.io_error == (int)0x80010013u && !out.text[0]);
    TEST_CHECK(!vjo_meiki_game_owned() && !file && releases == 3 && runs == 1);
    read_fail_at = 0;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !strcmp(out.text, "猫"));
    r = setup();
    cancel_read_at = (5392856u + 8191u) / 8192u + 3;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !out.text[0] &&
               !vjo_meiki_game_owned() && runs == 1);
    TEST_CHECK(!file && releases == 3);
    r = setup();
    native_rc = -10;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !out.text[0] &&
               !vjo_meiki_game_owned());
}
static void free_invalidation_proof(void)
{
    if (!have_model())
        return;
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    invalidation_fail = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && blocked);
    unsigned accepted = free_accepted_count;
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE &&
               free_accepted_count == accepted);
    invalidation_fail = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !vjo_meiki_game_owned() &&
               free_accepted_count == accepted);
}
static void reused_identity_never_frees_another_block(void)
{
    if (!have_model())
        return;
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    identity_mismatch = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && blocked &&
               out.cleanup_status);
    TEST_CHECK(vjo_meiki_game_owned() && !releases && !out.text[0]);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && !releases);
    identity_mismatch = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !vjo_meiki_game_owned() && releases == 3);
}

TEST_LIST = {
    {"meiki_GAME_USER_exact_budget_and_invalid_request", budget_and_invalid},
    {"meiki_pinned_SHA_and_API3_exact_broker_reader", exact_reader_success},
    {"meiki_bad_weights_native_IO_and_cancel_before_buffers", models_fail_before_allocation},
    {"meiki_three_allocations_and_mapping_failure_cleanup", allocation_and_mapping_failures},
    {"meiki_stop_unload_free_and_close_retention", stop_unload_close_retention},
    {"meiki_engine_reader_failure_and_OOM_recover", reader_failure_recovery},
    {"meiki_free_success_requires_definitive_UID_invalidation", free_invalidation_proof},
    {"meiki_changed_UID_mapping_never_frees_unknown_memory",
     reused_identity_never_frees_another_block},
    {NULL, NULL}};
