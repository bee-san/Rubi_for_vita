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
static unsigned char pixels[200*60*4];
static int live, open_error, read_error, close_error, bad_length, early_eof, corrupt, short_read;
static int schedule_kernel, cancel_after;
static uint64_t clock_us;
static FILE *file;

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
void buffers_read_done(uint32_t row, uint32_t count)
{ TEST_CHECK(locks == 1); if (row+count == g.crop_h) { g.raw_valid = 0; g.raw = NULL; } }

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
    open_error = read_error = close_error = bad_length = early_eof = corrupt = short_read = cancel_after = 0;
    schedule_kernel = 1; clock_us = 1000000;
    g.lock = g.ievf = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.capture_state = CAPTURE_IDLE; g.capture_once = g.raw_valid = 1;
    g.raw = pixels; g.raw_capacity = sizeof(pixels); g.raw_stride = 800;
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
        TEST_CHECK(!vjo_game_ocr_run_request(&request, &out));
        TEST_CHECK(out.lines == 1 && out.workspace_bytes <= sizeof(arena));
        TEST_CHECK(received == (i ? VJO_VOCR_H15_BYTES : VJO_VOCR_FL10_BYTES));
        TEST_CHECK(kernel_opens == 1 && kernel_closes == 1 && kernel_reads > 1);
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
}
TEST_LIST = {
    {"actual game worker and kernel broker load pinned FL10 and H15 despite game ENODEV", real_models_cross_namespace},
    {"actual broker model I/O corruption cancellation close failure and recovery", real_model_failure_recovery},
    {"actual queued Open timeout and later Close drain without weights", queued_open_timeout_and_drain},
    {NULL, NULL}
};
