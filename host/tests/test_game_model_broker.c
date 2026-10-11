/* Real vocr + real game worker + real kernel model transport. File syscalls
 * run only when the harness schedules the persistent kernel worker; the game
 * namespace reproduces physical ENODEV. Host execution is not device proof. */
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
static unsigned locks, user_opens, kernel_opens, kernel_reads, kernel_closes;
static unsigned allocated, released, received;
static unsigned char arena[2*1024*1024] __attribute__((aligned(64)));
static unsigned char pixels[49152]; /* page-rounded 200x60 capture */
static int live, open_error, read_error, close_error, bad_length, early_eof, corrupt, short_read;
static int schedule_kernel, cancel_after;
static uint64_t clock_us;
static FILE *file;
static unsigned boundary_busy, capture_allocations, capture_releases;
static int capture_live;

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
int vjoOcrModelOpen(uint32_t seq)
{ int rc = retail_return(test_kernel_model_open(seq)); if ((uint32_t)rc == 0xBFFFFFFDu) boundary_busy++; return rc; }
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{ int rc = retail_return(test_kernel_model_read(seq, off, bytes, dst)); if ((uint32_t)rc == 0xBFFFFFFDu) boundary_busy++; return rc; }
int vjoOcrModelClose(uint32_t seq)
{ int rc = retail_return(test_kernel_model_close(seq)); if ((uint32_t)rc == 0xBFFFFFFDu) boundary_busy++; return rc; }

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
{ TEST_CHECK(pid == 7 && !strcmp(name, VJO_GAME_OCR_MODULE) && syscall_depth == 1 && !locks); info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n)
{ TEST_CHECK(syscall_depth == 1); if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{ TEST_CHECK(syscall_depth == 1 && locks == 1); if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{ TEST_CHECK(!capture_live && type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW && bytes <= sizeof(pixels)); capture_live = 1; capture_allocations++; return 27; }
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{ TEST_CHECK(uid == 27 && capture_live); *base = pixels; return 0; }
int ksceKernelFreeMemBlock(SceUID uid)
{ TEST_CHECK(uid == 27 && capture_live); capture_live = 0; capture_releases++; return 0; }
void capture_compute_crop(uint32_t width, uint32_t height, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{ *x = *y = 0; *w = 200; *h = 60; }

SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && !file);
    TEST_CHECK(flags == SCE_O_RDONLY && !strncmp(path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT)));
    const char *name = path + sizeof(VJO_VOCR_MODEL_ROOT);
    TEST_ASSERT(!strcmp(name, "FL10_w8.vocr") || !strcmp(name, "H15_w8.vocr"));
    kernel_opens++;
    if (open_error) return open_error;
    char local[1024]; snprintf(local, sizeof(local), "%s/%s", VJO_VOCR_MODEL_DIR, name);
    file = fopen(local, "rb"); TEST_ASSERT(file != NULL); return 31;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence)
{ TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file); TEST_ASSERT(!fseek(file, (long)off, whence)); return (SceOff)ftell(file)+bad_length; }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file);
    TEST_CHECK(n && n <= VJO_GAME_OCR_MODEL_CHUNK && off >= 0);
    kernel_reads++;
    if (read_error && kernel_reads >= 3) return read_error;
    if (early_eof && kernel_reads >= 3) return 0;
    if (short_read && n > 37) n = 37;
    TEST_ASSERT(!fseek(file, (long)off, SEEK_SET));
    int got = (int)fread(out, 1, n, file);
    if (corrupt && off <= 1024 && off+got > 1024) ((unsigned char *)out)[1024-off] ^= 1;
    received += (unsigned)got; return got;
}
int ksceIoClose(SceUID fd)
{ TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file); kernel_closes++; if (close_error) return close_error; int rc = fclose(file); file = NULL; return rc; }

SceUID sceKernelGetProcessId(void) { TEST_CHECK(caller == 7); return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
int sceKernelDelayThread(unsigned us)
{
    clock_us += us;
    if (schedule_kernel) {
        SceUID saved = caller; caller = 0; game_ocr_model_tick(); caller = saved;
        if (cancel_after && kernel_reads >= (unsigned)cancel_after) {
            VJO_LOCK(); g.game_active = 0; game_ocr_foreground_changed_locked(); VJO_UNLOCK();
        }
    }
    return 0;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ out->size_user = 16*1024*1024; return 0; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{ TEST_CHECK(!live && type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && n <= sizeof(arena)); live = 1; allocated++; return 9; }
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{ TEST_CHECK(uid == 9 && live); *base = arena; return 0; }
int sceKernelFreeMemBlock(SceUID uid)
{ TEST_CHECK(uid == 9 && live); live = 0; released++; return 0; }
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_opens++; return (int)0x80010013u; }

static VjoGameOcrRequest setup(const char *name)
{
    TEST_ASSERT(!file && !live);
    memset(&g, 0, sizeof(g)); game_ocr_init();
    worker = workspace_uid = -1; model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    locks = syscall_depth = user_opens = kernel_opens = kernel_reads = kernel_closes = 0;
    allocated = released = received = 0;
    boundary_busy = capture_allocations = capture_releases = 0; capture_live = 1;
    open_error = read_error = close_error = bad_length = early_eof = corrupt = short_read = cancel_after = 0;
    schedule_kernel = 1; clock_us = 1000000;
    g.lock = g.ievf = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.capture_state = CAPTURE_IDLE; g.capture_once = g.raw_valid = 1;
    g.raw = pixels; g.raw_capacity = sizeof(pixels); g.raw_stride = 800;
    g.mem_uid = 27; g.fb_w = 200; g.fb_h = 60;
    g.crop_w = 200; g.crop_h = 60; g.done_seq = g.capture_seq = 11;
    g.capture_game_pid = 7; g.capture_epoch = 1;
    memset(pixels, 0, sizeof(pixels));
    for (unsigned y=15; y<39; y++) for (unsigned x=10; x<190; x+=6) memset(pixels+(y*200+x)*4, 235, 6);
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    VjoGameOcrRequest request = {.size=sizeof(request), .done_seq=11};
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT); strcpy(request.model, name);
    int seq = vjoOcrSubmit(&request); TEST_ASSERT(seq > 0);
    caller = 7; TEST_ASSERT(!vjoOcrTake(&request)); TEST_CHECK(request.seq == (unsigned)seq);
    return request;
}
static void finish(const VjoGameOcrResult *out)
{
    TEST_CHECK(!file && !live && !model_owned && allocated == released && !locks && !syscall_depth);
    TEST_CHECK(!user_opens && !vjoOcrComplete(out));
    VjoGameOcrResult received_result; caller = 3;
    int rc = vjoOcrRead(out->seq, &received_result);
    if (out->rc == VJO_E_CANCELLED) TEST_CHECK(rc == VJO_ERR_CANCELLED);
    else TEST_CHECK(!rc && !memcmp(out, &received_result, sizeof(*out)));
    TEST_CHECK(!capture_live && !game_ocr_capture_busy_locked());
    if (g.game_active) {
        /* The first hardware failure pinned ownership and blocked this next
         * real capture request even though all previous pixel copies worked. */
        TEST_CHECK(retail_return(capture_request(VJO_CAPTURE_ONCE)) > 0);
        TEST_CHECK(capture_live && g.capture_state == CAPTURE_PENDING);
        g.capture_state = CAPTURE_IDLE; buffers_free();
        TEST_CHECK(!capture_live && capture_allocations == 1 && capture_releases == 2);
    }
}
static void real_models_cross_namespace(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; real-model integration skipped"); return; }
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned i=0; i<2; i++) {
        VjoGameOcrRequest request = setup(names[i]); VjoGameOcrResult out;
        /* Demonstrate the physical failing boundary before using the broker. */
        TEST_CHECK(sceIoOpen("ux0:data/VitaJPOverlay/ocr/FL10_w8.vocr", 0, 0) == (int)0x80010013u); user_opens = 0;
        short_read = i == 1;
        int run_rc = vjo_game_ocr_run_request(&request, &out);
        TEST_CHECK(!run_rc);
        TEST_MSG("%s rc=%d io=0x%08X cleanup=%d owned=%d boundary_busy=%u",
                 names[i], run_rc, (unsigned)out.io_error, out.cleanup_status, model_owned, boundary_busy);
        TEST_CHECK(out.lines == 1 && out.workspace_bytes <= sizeof(arena));
        TEST_CHECK(received == (i ? VJO_VOCR_H15_BYTES : VJO_VOCR_FL10_BYTES));
        TEST_CHECK(kernel_opens == 1 && kernel_closes == 1 && kernel_reads > 1);
        TEST_CHECK(boundary_busy > 2); /* Open, Read and Close traverse real-return wrappers. */
        finish(&out);
    }
}
static void real_model_failure_recovery(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; real-model failure integration skipped"); return; }
    for (int failure=0; failure<7; failure++) {
        VjoGameOcrRequest request = setup("FL10_w8.vocr"); VjoGameOcrResult out;
        if (failure == 0) open_error = (int)0x80010013u;
        if (failure == 1) bad_length = 1;
        if (failure == 2) read_error = (int)0x80010005u;
        if (failure == 3) early_eof = 1;
        if (failure == 4) corrupt = 1;
        if (failure == 5) cancel_after = 3;
        if (failure == 6) close_error = (int)0x80010005u;
        int rc = vjo_game_ocr_run_request(&request, &out);
        TEST_CHECK(rc == (failure == 5 ? VJO_E_CANCELLED : failure == 6 ? VJO_E_OCR_UNAVAILABLE : VJO_E_OCR_MODEL));
        TEST_CHECK(!out.text[0] && !live && !user_opens);
        if (failure == 0) TEST_CHECK(out.io_error == open_error && !kernel_closes);
        if (failure == 1) TEST_CHECK(out.io_error == VJO_ERR_FORMAT);
        if (failure == 2) TEST_CHECK(out.io_error == read_error);
        if (failure == 6) {
            TEST_CHECK(file && model_owned && out.cleanup_status && model_seq == request.seq);
            TEST_CHECK(!vjoOcrComplete(&out)); /* Failure retains the claimed job. */
            pending_result = out; claim_outstanding = 1; close_error = 0;
            TEST_CHECK(!vjo_game_ocr_worker_stop() && !file && !model_owned);
            caller = 3;
            TEST_CHECK(retail_return(capture_request(VJO_CAPTURE_ONCE)) > 0);
            g.capture_state = CAPTURE_IDLE; buffers_free();
            continue;
        }
        finish(&out);
    }
}
static void queued_open_timeout_and_drain(void)
{
    /* No weights needed: Open never reaches the filesystem before Close. */
    VjoGameOcrRequest request = setup("FL10_w8.vocr");
    active_seq = request.seq; schedule_kernel = 0; VjoFile out;
    TEST_CHECK(file_open(NULL, "ignored", &out) == -1 && io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_owned && model_seq == request.seq && !kernel_opens);
    active_seq = 0; file_close(NULL);
    TEST_CHECK(model_owned && io_error == VJO_ERR_TIMEOUT);
    schedule_kernel = 1; TEST_CHECK(!vjo_game_ocr_worker_stop());
    TEST_CHECK(!model_owned && !kernel_opens && !file && !locks);
    VjoGameOcrResult result = {.size=sizeof(result), .seq=request.seq, .rc=VJO_E_CANCELLED};
    TEST_CHECK(!vjoOcrComplete(&result));
    caller = 3; TEST_CHECK(retail_return(capture_request(VJO_CAPTURE_ONCE)) > 0);
    g.capture_state = CAPTURE_IDLE; buffers_free();
}
static void native_status_contract(void)
{
    const int errors[] = {VJO_ERR_NO_GAME, VJO_ERR_NO_MEMORY, VJO_ERR_BUSY, VJO_ERR_TIMEOUT,
        VJO_ERR_FORMAT, VJO_ERR_ARG, VJO_ERR_COPY, VJO_ERR_PERM, VJO_ERR_CANCELLED};
    for (unsigned i=0; i<sizeof(errors)/sizeof(errors[0]); i++) {
        TEST_CHECK(errors[i] < 0 && ((uint32_t)errors[i] & 0xC0000000u) == 0x80000000u);
        TEST_CHECK(retail_return(errors[i]) == errors[i]);
        TEST_CHECK((uint32_t)errors[i] == 0xBFFFFFFFu-i);
        for (unsigned j=0; j<i; j++) TEST_CHECK(errors[i] != errors[j]);
    }
    TEST_CHECK(retail_return(-3) == (int32_t)0xBFFFFFFDu && retail_return(-3) != -3);
    const int unchanged[] = {0,1,13,657664, (int32_t)0x80010013u, (int32_t)0x8001000Du, (int32_t)0x80010005u};
    for (unsigned i=0; i<sizeof(unchanged)/sizeof(unchanged[0]); i++) TEST_CHECK(retail_return(unchanged[i]) == unchanged[i]);
}
TEST_LIST = {
    {"API13 stable statuses survive retail return encoding; native errno unchanged", native_status_contract},
    {"actual game worker and kernel broker load pinned FL10 and H15 despite game ENODEV", real_models_cross_namespace},
    {"actual broker model I/O corruption cancellation close failure and recovery", real_model_failure_recovery},
    {"actual queued Open timeout and later Close drain without weights", queued_open_timeout_and_drain},
    {NULL, NULL}
};
