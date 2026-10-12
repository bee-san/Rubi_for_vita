/* Real PP-OCRv6 adapter + real game worker + real kernel model broker. File
 * syscalls run only when the harness schedules the persistent kernel worker; the
 * game namespace reproduces physical ENODEV. The per-job thread runs when the
 * worker joins it. Host execution is not device proof, and no OCR accuracy is
 * claimed here (docs/ppocrv6.md has the measured accuracy). */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../game_ocr/main.c"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static unsigned locks, user_opens, kernel_opens, kernel_reads, kernel_closes, received;
static unsigned allocated, released, threads_created, threads_deleted, joins;
static unsigned char arena[22u*1024u*1024u] __attribute__((aligned(64)));
static unsigned char pixels[49152]; /* page-rounded 200x60 capture */
static int live, open_error, read_error, close_error, bad_length, corrupt, short_read, cancel_after;
static int schedule_kernel, virtual_model, create_failure, join_failure, delete_failure;
static int shell_cancel_after;
static uint32_t shell_cancel_seq;
static unsigned thread_stack;
static int available;
static uint64_t clock_us;
static FILE *file;
static int (*thread_entry)(SceSize, void *);
static int thread_ran, capture_live;
static unsigned capture_allocations, capture_releases;

int test_kernel_register(void);
int test_kernel_submit(const VjoGameOcrRequest *request);
int test_kernel_take(VjoGameOcrRequest *out);
int test_kernel_read_raw(uint32_t seq, uint32_t row, uint32_t count, void *dst);
int test_kernel_cancelled(uint32_t seq);
int test_kernel_complete(const VjoGameOcrResult *result);
int test_kernel_read(uint32_t seq, VjoGameOcrResult *out);
int test_kernel_cancel(uint32_t seq);
int test_kernel_model_open(uint32_t seq);
int test_kernel_model_read(uint32_t seq, uint32_t off, uint32_t bytes, void *dst);
int test_kernel_model_close(uint32_t seq);
static int retail_return(int rc)
{ return rc < 0 ? (int32_t)((uint32_t)rc & ~0x40000000u) : rc; }
int vjoOcrRegister(void) { return retail_return(test_kernel_register()); }
int vjoOcrSubmit(const VjoGameOcrRequest *r) { return retail_return(test_kernel_submit(r)); }
int vjoOcrTake(VjoGameOcrRequest *out) { return retail_return(test_kernel_take(out)); }
int vjoOcrReadRaw(uint32_t seq, uint32_t row, uint32_t count, void *dst)
{ return retail_return(test_kernel_read_raw(seq, row, count, dst)); }
int vjoOcrCancelled(uint32_t seq) { return retail_return(test_kernel_cancelled(seq)); }
int vjoOcrComplete(const VjoGameOcrResult *r) { return retail_return(test_kernel_complete(r)); }
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out) { return retail_return(test_kernel_read(seq, out)); }
int vjoOcrCancel(uint32_t seq) { return retail_return(test_kernel_cancel(seq)); }
int vjoOcrModelOpen(uint32_t seq) { return retail_return(test_kernel_model_open(seq)); }
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{ return retail_return(test_kernel_model_read(seq, off, bytes, dst)); }
int vjoOcrModelClose(uint32_t seq) { return retail_return(test_kernel_model_close(seq)); }

void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{ TEST_CHECK(uid == g.ievf && bits == IEV_MODEL); return 0; }
int ksceKernelLockMutex(SceUID uid, int n, void *timeout)
{ TEST_CHECK(uid == g.lock && !locks); locks++; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n)
{ TEST_CHECK(uid == g.lock && locks == 1); locks--; return 0; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ TEST_CHECK(pid == 7 && !strcmp(name, VJO_GAME_OCR_MODULE)); info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n)
{ if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{ TEST_CHECK(locks == 1); if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{ TEST_CHECK(!capture_live && bytes <= sizeof(pixels)); capture_live = 1; capture_allocations++; return 27; }
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{ TEST_CHECK(uid == 27 && capture_live); *base = pixels; return 0; }
int ksceKernelFreeMemBlock(SceUID uid)
{ TEST_CHECK(uid == 27 && capture_live); capture_live = 0; capture_releases++; return 0; }
void capture_compute_crop(uint32_t width, uint32_t height, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{ *x = *y = 0; *w = 200; *h = 60; }

SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    TEST_ASSERT(caller == 0 && !locks && !file);
    /* The kernel resolves only the canonical pinned path; never a caller path. */
    TEST_CHECK(flags == SCE_O_RDONLY && !strcmp(path, VJO_VOCR_MODEL_ROOT "/" VJO_PPOCRV6_MODEL_NAME));
    kernel_opens++;
    if (open_error) return open_error;
    if (virtual_model) {
        file = tmpfile(); TEST_ASSERT(file != NULL);
        static unsigned char zeros[65536];
        for (unsigned n = 0; n < VJO_PPOCRV6_MODEL_BYTES;) {
            unsigned k = VJO_PPOCRV6_MODEL_BYTES - n < sizeof(zeros) ? VJO_PPOCRV6_MODEL_BYTES - n : sizeof(zeros);
            TEST_ASSERT(fwrite(zeros, 1, k, file) == k); n += k;
        }
    } else {
        char local[1024];
        snprintf(local, sizeof(local), "%s/%s", VJO_PPOCRV6_MODEL_DIR, VJO_PPOCRV6_MODEL_NAME);
        file = fopen(local, "rb"); TEST_ASSERT(file != NULL);
    }
    return 31;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence)
{ TEST_ASSERT(caller == 0 && fd == 31 && file); TEST_ASSERT(!fseek(file, (long)off, whence)); return (SceOff)ftell(file) + bad_length; }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{
    TEST_ASSERT(caller == 0 && !locks && fd == 31 && file);
    TEST_CHECK(n && n <= VJO_GAME_OCR_MODEL_CHUNK && off >= 0);
    kernel_reads++;
    if (read_error && kernel_reads >= 3) return read_error;
    if (short_read && n > 37) n = 37;
    TEST_ASSERT(!fseek(file, (long)off, SEEK_SET));
    int got = (int)fread(out, 1, n, file);
    if (corrupt && off <= 100000 && off + got > 100000) ((unsigned char *)out)[100000 - off] ^= 1;
    received += (unsigned)got; return got;
}
int ksceIoClose(SceUID fd)
{ TEST_ASSERT(caller == 0 && fd == 31 && file); kernel_closes++; if (close_error) return close_error; int rc = fclose(file); file = NULL; return rc; }

SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
int sceKernelDelayThread(unsigned us)
{
    clock_us += us;
    if (schedule_kernel) {
        SceUID saved = caller; caller = 0; game_ocr_model_tick(); caller = saved;
        if (cancel_after && kernel_reads >= (unsigned)cancel_after) {
            VJO_LOCK(); g.game_active = 0; game_ocr_foreground_changed_locked(); VJO_UNLOCK();
        }
        if (shell_cancel_after && kernel_reads >= (unsigned)shell_cancel_after) {
            caller = 3; TEST_CHECK(!vjoOcrCancel(shell_cancel_seq)); caller = saved; shell_cancel_after = 0;
        }
    }
    return 0;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out) { out->size_user = available; return 0; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    TEST_CHECK(!strcmp(name, "VjoGamePpocr") && type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(n & 4095));
    TEST_ASSERT(!live && n <= sizeof(arena)); live = 1; allocated++; return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base) { TEST_CHECK(uid == 9 && live); *base = arena; return 0; }
int sceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 9 && live); live = 0; released++; return 0; }
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{
    if (strcmp(name, "VjoGamePpocr")) return 20; /* the idle worker keeps its own stack */
    TEST_CHECK(entry == ppocr_main && stack == PPOCR_STACK_BYTES && live && !thread_entry);
    if (create_failure) return -1;
    thread_stack = stack; thread_entry = entry; thread_ran = 0; threads_created++;
    return 40;
}
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout)
{
    if (uid != 40) return 0;
    joins++;
    if (thread_entry && !thread_ran) { thread_ran = 1; thread_entry(0, NULL); }
    return join_failure ? -1 : 0;
}
int sceKernelDeleteThread(SceUID uid)
{
    if (uid != 40) return 0;
    if (delete_failure) return -1;
    TEST_CHECK(thread_ran); thread_entry = NULL; threads_deleted++; return 0;
}
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { user_opens++; return (int)0x80010013u; }

/* The engine's bytes, then the job's own request copy and text, page-rounded. */
#define NEED_BYTES ((uint32_t)(((vjo_ppocrv6_workspace_bytes(200, 60) + 63u) & ~(size_t)63u) + \
                               sizeof(PpocrJobTail) + 4095u) & ~4095u)
#define GAME_FREE_NEEDED (NEED_BYTES + PPOCR_STACK_BYTES + GAME_RESERVE)

static void bright_line(void)
{
    memset(pixels, 0, sizeof(pixels));
    for (unsigned y = 15; y < 39; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
}
static VjoGameOcrRequest submit_to(int fresh, const char *dir)
{
    if (fresh) {
        memset(&g, 0, sizeof(g)); game_ocr_init();
        g.lock = g.ievf = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    }
    g.capture_state = CAPTURE_IDLE; g.capture_once = g.raw_valid = 1; capture_live = 1;
    g.raw = pixels; g.raw_capacity = sizeof(pixels); g.raw_stride = 800;
    g.mem_uid = 27; g.fb_w = 200; g.fb_h = 60; g.crop_w = 200; g.crop_h = 60;
    g.done_seq = ++g.capture_seq; g.capture_game_pid = 7; g.capture_epoch = g.ocr_epoch;
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    VjoGameOcrRequest request = {.size=sizeof(request), .done_seq=g.done_seq};
    strcpy(request.model_dir, dir); strcpy(request.model, VJO_PPOCRV6_MODEL_NAME);
    int seq = vjoOcrSubmit(&request); TEST_ASSERT(seq > 0);
    caller = 7; TEST_ASSERT(!vjoOcrTake(&request)); TEST_CHECK(request.seq == (unsigned)seq);
    return request;
}
static VjoGameOcrRequest submit(int fresh) { return submit_to(fresh, VJO_VOCR_MODEL_ROOT); }
static VjoGameOcrRequest setup(void)
{
    TEST_ASSERT(!file && !live);
    worker = workspace_uid = ppocr_thread = -1; model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    locks = syscall_depth = user_opens = kernel_opens = kernel_reads = kernel_closes = received = 0;
    allocated = released = threads_created = threads_deleted = joins = 0; thread_entry = NULL;
    open_error = read_error = close_error = bad_length = corrupt = short_read = cancel_after = 0;
    create_failure = join_failure = delete_failure = shell_cancel_after = 0; virtual_model = 1;
    schedule_kernel = 1; clock_us = 1000000; available = 64 * 1024 * 1024;
    capture_allocations = capture_releases = 0;
    bright_line();
    return submit(1);
}
/* Cleanup proof: workspace, thread, descriptor and claim are all released, and
 * the Shell reads this job's own result (or its cancellation) and nothing else. */
static void finish(const VjoGameOcrResult *out)
{
    TEST_CHECK(!file && !live && !model_owned && ppocr_thread < 0 && allocated == released);
    TEST_CHECK(threads_created == threads_deleted && !user_opens && !locks);
    TEST_CHECK(!vjoOcrComplete(out));
    VjoGameOcrResult shell_view; caller = 3;
    int rc = vjoOcrRead(out->seq, &shell_view);
    if (out->rc == VJO_E_CANCELLED) TEST_CHECK(rc == VJO_ERR_CANCELLED);
    else TEST_CHECK(!rc && !memcmp(out, &shell_view, sizeof(*out)));
    if (out->rc) TEST_CHECK(!out->text[0]);
    TEST_CHECK(!game_ocr_capture_busy_locked());
}

static void pin_size_and_canonical_path(void)
{
    /* The kernel allowlist itself is tested in test_kernel_game_ocr.c. */
    TEST_CHECK(vjo_ppocrv6_pinned()->weights_bytes == VJO_PPOCRV6_MODEL_BYTES);
    TEST_CHECK(!strcmp(vjo_ppocrv6_pinned()->weights_name, VJO_PPOCRV6_MODEL_NAME));
    /* A request for another directory never reaches the filesystem. */
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    VjoGameOcrResult drained = {.size=sizeof(drained), .seq=r.seq, .rc=VJO_E_CANCELLED};
    TEST_CHECK(!vjoOcrComplete(&drained));
    g.capture_state = CAPTURE_IDLE;
    r = submit_to(0, "ux0:data/other");
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL);
    TEST_CHECK(!kernel_opens && !out.text[0] && out.lines == 1 && out.io_error == VJO_ERR_ARG);
    finish(&out);
}

static void game_memory_refused_before_any_allocation(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    available = (int)GAME_FREE_NEEDED - 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
    TEST_CHECK(!allocated && !threads_created && !kernel_opens && !out.text[0] && !out.cleanup_status);
    TEST_CHECK(out.workspace_bytes == NEED_BYTES);
    finish(&out);
    /* Exactly enough: the job proceeds (blank region: the weights stay unread). */
    memset(pixels, 0, sizeof(pixels));
    r = submit(0); available = (int)GAME_FREE_NEEDED;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.lines && !out.text[0]);
    TEST_CHECK(allocated == 1 && threads_created == 1 && thread_stack == PPOCR_STACK_BYTES && !kernel_opens);
    finish(&out);
}

static void missing_wrong_size_and_corrupt_weights(void)
{
    for (int failure = 0; failure < 4; failure++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        if (failure == 0) open_error = (int)0x80010002u; /* ENOENT: not installed */
        if (failure == 1) bad_length = 1;                /* another file under the pinned name */
        if (failure == 2) read_error = (int)0x80010005u; /* EIO mid-stream */
        /* failure 3: the pinned size, but not the pinned bytes */
        TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL);
        TEST_CHECK(!out.text[0] && out.lines == 1 && !out.cleanup_status && threads_created == 1);
        if (failure == 0) TEST_CHECK(out.io_error == open_error && !kernel_closes);
        if (failure == 1) TEST_CHECK(out.io_error == VJO_ERR_FORMAT && kernel_closes == 1);
        if (failure == 2) TEST_CHECK(out.io_error == read_error && kernel_closes == 1);
        if (failure == 3) TEST_CHECK(!out.io_error && kernel_closes == 1 && received > 0);
        finish(&out);
    }
}

static void cancellation_while_streaming_weights(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    cancel_after = 3; /* suspend/foreground change during the model read */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED);
    TEST_CHECK(!out.text[0] && kernel_closes == 1 && received < VJO_PPOCRV6_MODEL_BYTES);
    finish(&out);
}

static void stale_job_isolation(void)
{
    /* Job 1 is cancelled by the Shell after a few chunks; its completion only
     * drains. Job 2 on a new capture sees nothing of job 1. */
    VjoGameOcrRequest first = setup(); VjoGameOcrResult out1, out2;
    shell_cancel_seq = first.seq; shell_cancel_after = 3;
    TEST_CHECK(vjo_game_ocr_run_request(&first, &out1) == VJO_E_CANCELLED && !out1.text[0]);
    TEST_CHECK(kernel_opens == 1 && kernel_closes == 1 && received < VJO_PPOCRV6_MODEL_BYTES);
    TEST_CHECK(!live && !file && ppocr_thread < 0 && !vjoOcrComplete(&out1));
    VjoGameOcrResult stale; caller = 3;
    TEST_CHECK(vjoOcrRead(first.seq, &stale) == VJO_ERR_CANCELLED);
    g.capture_state = CAPTURE_IDLE; capture_live = 0;
    VjoGameOcrRequest second = submit(0);
    TEST_CHECK(second.seq == first.seq + 1);
    caller = 3; TEST_CHECK(vjoOcrRead(first.seq, &stale) == VJO_ERR_ARG); caller = 7;
    TEST_CHECK(vjo_game_ocr_run_request(&second, &out2) == VJO_E_OCR_MODEL); /* virtual zero weights */
    TEST_CHECK(out2.seq == second.seq && !out2.text[0]);
    finish(&out2);
}

static void thread_create_join_and_delete_failures(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    create_failure = 1; /* no stack for the job: refused as game memory */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
    TEST_CHECK(!live && allocated == 1 && released == 1 && !kernel_opens && !out.cleanup_status);
    finish(&out);
    create_failure = 0; join_failure = 1;
    r = submit(0);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(live && blocked && out.cleanup_status && ppocr_thread == 40 && !out.text[0]);
    unsigned before = allocated;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && allocated == before);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && live);
    join_failure = 0; delete_failure = 1;
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && live && ppocr_thread == 40);
    delete_failure = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !live && ppocr_thread < 0 && !blocked);
}

/* The job thread's request and text live in its workspace, never on the
 * worker's stack or in the result the kernel later receives. */
static void job_state_lives_in_the_job_workspace(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    memset(pixels, 0, sizeof(pixels)); /* no line: the job ends after the finder */
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && threads_created == 1);
    unsigned char *tail = (unsigned char *)ppocr_job.tail;
    TEST_CHECK(tail >= arena && tail + sizeof(PpocrJobTail) <= arena + NEED_BYTES);
    TEST_CHECK(!memcmp(&ppocr_job.tail->request, &r, sizeof(r)));
    TEST_CHECK((size_t)(tail - arena) >= vjo_ppocrv6_workspace_bytes(200, 60));
    finish(&out);
}

static void real_weights_through_the_broker(void)
{
    if (!VJO_PPOCRV6_MODEL_DIR[0]) { TEST_MSG("Pinned PP-OCRv6 weights absent; real-model integration skipped"); return; }
    for (int pass = 0; pass < 3; pass++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        virtual_model = 0; short_read = pass == 1; corrupt = pass == 2;
        int rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK(rc == (pass == 2 ? VJO_E_OCR_MODEL : VJO_OK));
        TEST_MSG("pass %d rc=%d io=0x%08X cleanup=%d", pass, rc, (unsigned)out.io_error, out.cleanup_status);
        TEST_CHECK(out.lines == 1 && received == VJO_PPOCRV6_MODEL_BYTES && kernel_closes == 1);
        TEST_CHECK(kernel_reads >= VJO_PPOCRV6_MODEL_BYTES / VJO_GAME_OCR_MODEL_CHUNK);
        if (pass == 2) TEST_CHECK(!out.text[0]); /* SHA-256 rejects a one-bit change */
        finish(&out);
    }
}

TEST_LIST = {
    {"PP-OCRv6 pinned length and canonical broker path only", pin_size_and_canonical_path},
    {"PP-OCRv6 game USER shortfall refused before allocation, thread or model I/O", game_memory_refused_before_any_allocation},
    {"PP-OCRv6 missing, wrong-size, unreadable and corrupt weights fail cleanly", missing_wrong_size_and_corrupt_weights},
    {"PP-OCRv6 cancellation while streaming weights through the broker", cancellation_while_streaming_weights},
    {"PP-OCRv6 cancelled job never publishes into the next job", stale_job_isolation},
    {"PP-OCRv6 job thread create, join and delete failures retain ownership", thread_create_join_and_delete_failures},
    {"PP-OCRv6 job request and text live in the job's own workspace", job_state_lives_in_the_job_workspace},
    {"PP-OCRv6 real pinned weights stream through the broker and pass SHA-256", real_weights_through_the_broker},
    {NULL, NULL}
};
