/* Actual IPC and capture ownership code with bounded user-copy substitutes. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
typedef struct { size_t size; SceUID modid; } tai_module_info_t;
int taiGetModuleInfoForKernel(SceUID, const char *, tai_module_info_t *);
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.c"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "../../kernel/capture_request.c"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static int module_missing, module_id, copy_in_fail, copy_out_fail;
static unsigned locks, frees, allocations;
static uint8_t raw_storage[VJO_MAX_W * VJO_MAX_H * 4];
static VjoGameOcrResult message;
static unsigned model_opens, model_reads, model_closes;
static unsigned model_events;
static int file_live, open_error, seek_error, read_error, close_error, eof, short_read, bad_size;
static int during_io;
static uint64_t model_clock;
static uint32_t expected_model_size;
static unsigned done_events, waits, stats;
static int expect_fallback;
static int done_pending, wait_runs_worker, stat_error;
static SceIoStat file_stat;

void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return model_clock; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    if (uid == g.mevf && g.mevf > 0) { TEST_CHECK(bits == MEV_DONE); done_events++; done_pending = 1; return 0; }
    TEST_CHECK(uid == g.ievf && bits == IEV_MODEL); model_events++; return 0;
}
static void model_tick(void);
/* A model syscall waiting for the kernel worker: g.lock is not held, so the
 * worker can run (wait_runs_worker) and finish the queued operation. */
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    TEST_ASSERT(uid == g.mevf && bits == MEV_DONE && !locks && syscall_depth == 1);
    TEST_CHECK(wait == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT) && timeout && *timeout <= 50000);
    waits++;
    if (!done_pending && wait_runs_worker) {
        unsigned depth = syscall_depth; syscall_depth = 0; model_tick(); syscall_depth = depth;
    }
    if (done_pending) { done_pending = 0; if (out) *out = MEV_DONE; return 0; }
    model_clock += *timeout; /* nothing finished in time */
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
int ksceIoGetstatByFd(SceUID fd, SceIoStat *out)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    stats++;
    if (stat_error) return stat_error;
    *out = file_stat; out->st_size = expected_model_size; return 0;
}
static void interrupt_io(void)
{
    int action = during_io; during_io = 0;
    TEST_ASSERT(!locks && !syscall_depth);
    if (action == 1) { /* Foreground cancellation during unlocked file I/O. */
        VJO_LOCK(); g.game_active = 0; game_ocr_foreground_changed_locked(); VJO_UNLOCK();
    } else if (action == 2) game_ocr_process_gone(7);
    else if (action == 3) {
        VJO_LOCK(); g.shell_pid = 4; game_ocr_shell_changed_locked(); VJO_UNLOCK();
    } else if (action == 4) {
        SceUID saved = caller; caller = 7;
        TEST_CHECK(vjoOcrModelClose(request.seq) == VJO_ERR_BUSY);
        caller = saved;
    }
}
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && !file_live);
    char expected[320]; snprintf(expected,sizeof(expected),"%s/%s",VJO_VOCR_MODEL_ROOT,
                                 expect_fallback ? request.fallback : request.model);
    TEST_CHECK(!strcmp(path, expected) && flags == SCE_O_RDONLY);
    model_opens++; interrupt_io();
    if (open_error) return open_error;
    file_live = 1; return 31;
}
SceOff ksceIoLseek(SceUID fd, SceOff offset, int whence)
{ TEST_CHECK(fd == 31 && file_live && !locks && !syscall_depth); return seek_error ? (SceOff)seek_error : (SceOff)expected_model_size + bad_size; }
int ksceIoPread(SceUID fd, void *out, SceSize bytes, SceOff offset)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    TEST_CHECK(bytes <= VJO_GAME_OCR_MODEL_CHUNK && offset >= 0 && offset + bytes <= expected_model_size);
    model_reads++; interrupt_io();
    if (read_error) return read_error;
    if (eof) return 0;
    if (short_read && bytes > 37) bytes = 37;
    for (unsigned i=0; i<bytes; i++) ((unsigned char *)out)[i] = (unsigned char)((offset+i)%251);
    return (int)bytes;
}
int ksceIoClose(SceUID fd)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    model_closes++;
    if (close_error) return close_error;
    file_live = 0; return 0;
}
static void model_tick(void)
{ SceUID saved = caller; caller = 0; game_ocr_model_tick(); caller = saved; }
int ksceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    (void)count; (void)timeout; TEST_CHECK(uid == g.lock && locks == 0); locks++; return 0;
}
int ksceKernelUnlockMutex(SceUID uid, int count)
{
    (void)count; TEST_CHECK(uid == g.lock && locks == 1); locks--; return 0;
}
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{
    TEST_CHECK(pid == caller && syscall_depth == 1 && locks == 0);
    TEST_CHECK(!strcmp(name, VJO_GAME_OCR_MODULE) && info->size == sizeof(*info));
    info->modid = module_id;
    return module_missing ? -1 : 0;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize bytes)
{
    TEST_CHECK(syscall_depth <= 1);
    if (copy_in_fail || !src) return -1;
    memcpy(dst, src, bytes); return 0;
}
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize bytes)
{
    TEST_CHECK(syscall_depth == 1 && locks == 1);
    if (copy_out_fail || !dst) return -1;
    memcpy(dst, src, bytes); return 0;
}
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)name; (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW && bytes <= sizeof(raw_storage));
    allocations++; return 27;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_CHECK(uid == 27); *base = raw_storage; return 0;
}
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 27); frees++; return 0; }

static void setup(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    g.lock = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    g.raw = raw_storage; g.mem_uid = 27; g.raw_capacity = 4096;
    g.crop_w = g.crop_h = 32; g.raw_stride = 128;
    g.capture_once = 1;
    g.capture_seq = g.done_seq = 11; g.fb_w = g.fb_h = 32;
    g.capture_game_pid = 7; g.capture_epoch = 1;
    caller = 7; module_id = 99;
    module_missing = copy_in_fail = copy_out_fail = 0;
    locks = allocations = frees = syscall_depth = 0;
    memset(&message, 0, sizeof(message));
    model_clock = 1000000; expected_model_size = VJO_VOCR_FL10_BYTES;
    model_opens = model_reads = model_closes = model_events = 0;
    file_live = open_error = seek_error = read_error = close_error = eof = short_read = bad_size = during_io = 0;
    done_events = waits = stats = 0; done_pending = wait_runs_worker = stat_error = 0; expect_fallback = 0;
    memset(&file_stat, 0, sizeof(file_stat));
    file_stat.st_mtime = (SceDateTime){2026, 10, 11, 9, 43, 58, 123456};
    TEST_ASSERT(vjoOcrRegister() == 0);
    caller = 3;
}

static VjoGameOcrRequest make_request(void)
{
    VjoGameOcrRequest r = {0};
    r.size = sizeof(r); r.done_seq = g.done_seq;
    strcpy(r.model, "FL10_w8.vocr");
    strcpy(r.model_dir, "ux0:data/VitaJPOverlay/ocr");
    return r;
}

static uint32_t submit(void)
{
    VjoGameOcrRequest r = make_request();
    caller = 3;
    int seq = vjoOcrSubmit(&r);
    TEST_ASSERT(seq > 0);
    return (uint32_t)seq;
}

static void take(uint32_t seq)
{
    VjoGameOcrRequest r = {0};
    caller = 7;
    TEST_ASSERT(vjoOcrTake(&r) == 0);
    TEST_CHECK(r.size == sizeof(r) && r.seq == seq && r.done_seq == 11);
    TEST_CHECK(r.width == 32 && r.height == 32 && r.stride == 128);
    memset(&message, 0, sizeof(message));
    message.size = sizeof(message); message.seq = seq;
    strcpy(message.text, "猫");
}

static void registration_and_permissions(void)
{
    setup(); caller = 8;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrSubmit(&request) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrRead(1, &message) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrCancel(1) == VJO_ERR_PERM);
    caller = 7; module_missing = 1;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_PERM);
    module_missing = 0; g.game_active = 0;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
}

static void complete_and_replacement(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(game_ocr_capture_busy_locked());
    TEST_CHECK(!game_ocr_raw_caller_locked(7));
    TEST_CHECK(vjoOcrRead(seq, &message) == 1);
    take(seq);
    TEST_CHECK(vjoOcrRegister() == 0); /* Repeated registration keeps the claim. */
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_BUSY);
    TEST_CHECK(game_ocr_raw_caller_locked(7) && !game_ocr_raw_caller_locked(8));
    TEST_CHECK(vjoOcrCancelled(seq) == 0);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked() && !game_ocr_raw_caller_locked(7));
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    uint32_t second = submit();
    TEST_CHECK(second > seq && vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    take(second); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG && game_ocr_capture_busy_locked());
}

static void capture_pins_queued_and_claimed(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_BUSY);
    take(seq);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(capture_request(0) > 0);
}

static void cancellation_drains_reader(void)
{
    setup(); uint32_t seq = submit(); take(seq); caller = 3;
    TEST_CHECK(vjoOcrCancel(seq) == 0);
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(!game_ocr_raw_caller_locked(7));
    caller = 7;
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked());
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(capture_request(0) > 0);
}

static void cancel_before_take(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(vjoOcrCancel(seq) == 0 && !game_ocr_capture_busy_locked());
    caller = 7; VjoGameOcrRequest r;
    TEST_CHECK(vjoOcrTake(&r) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
}

static void suspended_and_same_pid_epoch(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.game_active = 0; g.ocr_epoch++;
    game_ocr_foreground_changed_locked();
    TEST_CHECK(game_ocr_capture_busy_locked() && !game_ocr_raw_caller_locked(7));
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    g.game_active = VJO_GAME; g.ocr_epoch++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0);
    g.capture_epoch = g.ocr_epoch; /* A fresh capture in the resumed session. */
    caller = 3; uint32_t next = submit(); TEST_CHECK(next > seq);
    caller = 7;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); /* Old reply cannot claim a new job. */
}

static void foreground_replacement_and_exit(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.game_pid = 8; g.game_active = VJO_GAME; g.ocr_epoch++;
    game_ocr_foreground_changed_locked();
    caller = 8; TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_PERM);
    game_ocr_process_gone(7);
    TEST_CHECK(!game_ocr_capture_busy_locked() && vjoOcrRegister() == 0);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
}

static void shell_restart(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.shell_pid = 4; game_ocr_shell_changed_locked();
    TEST_CHECK(game_ocr_capture_busy_locked() && vjoOcrCancelled(seq) == 1);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_PERM);
    caller = 4; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0 && !game_ocr_capture_busy_locked());
    caller = 4; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* Old Shell's finished crop is stale too. */
}

static void cleanup_failure_and_module_replacement(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    message.cleanup_status = -1; message.rc = -12; message.text[0] = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && game_ocr_capture_busy_locked());
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && message.cleanup_status == -1);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    caller = 7; module_id++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    module_id--; message.cleanup_status = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}

static void invalid_capture_and_wire(void)
{
    setup(); VjoGameOcrRequest r = make_request();
    r.size--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.size++;
    r.model[31] = 'x'; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.model[31] = 0;
    r.model_dir[255] = 'x'; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.model_dir[255] = 0;
    r.done_seq--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.done_seq++;
    g.capture_once = 0; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.capture_once = 1;
    g.raw_capacity--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.raw_capacity++;
    g.raw_valid = 0; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.raw_valid = 1;
    g.capture_epoch++; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.capture_epoch--;
    g.capture_game_pid++; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.capture_game_pid--;
    g.capture_state = CAPTURE_PENDING; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    g.capture_state = CAPTURE_IDLE; uint32_t seq = submit(); take(seq);
    message.size--; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.size++;
    message.text[4095] = 'x'; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.text[4095] = 0;
    message.cleanup_status = -1; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    TEST_CHECK(game_ocr_capture_busy_locked());
    g.done_seq++; TEST_CHECK(!game_ocr_raw_caller_locked(7));
}

static void user_copy_failures_and_sequence_exhaustion(void)
{
    setup(); VjoGameOcrRequest r = make_request(); copy_in_fail = 1;
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_COPY && !game_ocr_capture_busy_locked());
    copy_in_fail = 0; uint32_t seq = submit(); caller = 7; copy_out_fail = 1;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_COPY && !claimed && pending);
    copy_out_fail = 0; take(seq); copy_in_fail = 1;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_COPY && claimed);
    copy_in_fail = 0; TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; copy_out_fail = 1; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_COPY);
    copy_out_fail = 0; sequence = 0x7FFFFFFFu;
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
}

static void shutdown_refuses_claim_and_closes_registration(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(game_ocr_shutdown_locked() == 0);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    caller = 3; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_NO_GAME);
}

static void single_pass_raw_and_stale_sequences(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    unsigned char out[4096];
    memset(raw_storage, 0x52, 4096);
    TEST_CHECK(vjoOcrReadRaw(seq+1, 0, 1, out) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrReadRaw(seq, 0, 0, out) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrReadRaw(seq, 30, 3, out) == VJO_ERR_ARG);
    caller = 8; TEST_CHECK(vjoOcrReadRaw(seq, 0, 1, out) == VJO_ERR_PERM);
    caller = 7; copy_out_fail = 1;
    TEST_CHECK(vjoOcrReadRaw(seq, 0, 32, out) == VJO_ERR_COPY && !frees);
    copy_out_fail = 0;
    TEST_CHECK(vjoOcrReadRaw(seq, 0, 16, out) == 16 && !frees);
    TEST_CHECK(out[0] == 0x52 && out[2047] == 0x52);
    TEST_CHECK(vjoOcrReadRaw(seq, 16, 16, out) == 16 && frees == 1 && !g.raw);
    TEST_CHECK(vjoOcrReadRaw(seq, 0, 1, out) < 0);
    TEST_CHECK(game_ocr_capture_busy_locked()); /* inference still owns the job */
    TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}

static void open_model(uint32_t seq)
{
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY && !file_live);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY && !model_opens);
    model_tick();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES && file_live);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES && model_opens == 1);
}
static void close_model(uint32_t seq)
{
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live);
}
static void model_permissions_paths_and_bounds(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_PERM);
    take(seq); caller = 8;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_PERM);
    caller = 7;
    TEST_CHECK(vjoOcrModelOpen(seq+1, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG);
    open_model(seq);
    static unsigned char bytes[VJO_GAME_OCR_MODEL_CHUNK + 1];
    TEST_CHECK(vjoOcrModelRead(seq, 0, VJO_GAME_OCR_MODEL_CHUNK + 1, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, UINT32_MAX, 1, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES, 1, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, 0, 0, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, NULL) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_BUSY); /* No premature cleanup ack. */
    close_model(seq);
    TEST_CHECK(!vjoOcrComplete(&message));
    setup(); seq = submit(); take(seq);
    strcpy(request.model_dir, "ux0:data/../../tai");
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && !model_opens);
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT); strcpy(request.model, "../secret");
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && !model_opens);
    TEST_CHECK(!vjoOcrModelClose(seq));
}
static void removed_meiki_model_is_refused(void)
{
    TEST_CHECK(sizeof(model_stage)==32768);
    /* The broker opens only the VOCR/VOCR2 pins; the removed recognizer is unlisted. */
    TEST_CHECK(!model_size("meiki-stream-int8.mnn") && !model_size("meiki-detect-int8.mnn"));
    setup(); uint32_t seq=submit(); take(seq);
    strcpy(request.model,"meiki-stream-int8.mnn");
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL)==VJO_ERR_ARG && !model_opens && !file_live);
    TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
}

static void vocr2_fixed_pin_preserves_paths_and_bounds(void)
{
    TEST_CHECK(!model_size("../" VJO_VOCR2_ARTIFACT_NAME));
    setup(); uint32_t seq = submit(); take(seq);
    strcpy(request.model, VJO_VOCR2_ARTIFACT_NAME);
#if VJO_VOCR2_ENABLED
    TEST_CHECK(model_size(VJO_VOCR2_ARTIFACT_NAME) == VJO_VOCR_FL10_BYTES);
    expected_model_size = VJO_VOCR2_BYTES;
    open_model(seq);
    unsigned char tail[2] = {0xCC, 0xCC};
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR2_BYTES - 1, 2, tail) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR2_BYTES - 1, 1, tail) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR2_BYTES - 1, 1, tail) == 1 && tail[1] == 0xCC);
    close_model(seq);
#else
    TEST_CHECK(!model_size(VJO_VOCR2_ARTIFACT_NAME));
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && !model_opens);
#endif
    TEST_CHECK(!vjoOcrComplete(&message));
}

static void model_short_read_copy_retry_and_errors(void)
{
    setup(); uint32_t seq = submit(); take(seq); open_model(seq);
    unsigned char bytes[8192]; memset(bytes, 0xCC, sizeof(bytes));
    short_read = 1;
    TEST_CHECK(vjoOcrModelRead(seq, 9, sizeof(bytes), bytes) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrModelRead(seq, 10, sizeof(bytes), bytes) == VJO_ERR_ARG);
    model_tick(); copy_out_fail = 1;
    TEST_CHECK(vjoOcrModelRead(seq, 9, sizeof(bytes), bytes) == VJO_ERR_COPY && bytes[0] == 0xCC);
    copy_out_fail = 0;
    TEST_CHECK(vjoOcrModelRead(seq, 9, sizeof(bytes), bytes) == 37 && model_reads == 1);
    TEST_CHECK(bytes[0] == 9 && bytes[36] == 45 && bytes[37] == 0xCC);
    short_read = 0; eof = 1;
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == 0);
    eof = 0; read_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == read_error);
    close_model(seq);
    for (unsigned failure=0; failure<3; failure++) {
        setup(); seq = submit(); take(seq);
        if (failure == 0) open_error = (int)0x80010013u;
        if (failure == 1) seek_error = (int)0x80010005u;
        if (failure == 2) bad_size = 1;
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY); model_tick();
        int rc = vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL);
        TEST_CHECK(rc == (failure == 0 ? open_error : failure == 1 ? seek_error : VJO_ERR_FORMAT));
        TEST_CHECK(file_live == (failure != 0));
        if (failure != 0) {
            close_error = (int)0x80010005u;
            TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY); model_tick();
            TEST_CHECK(vjoOcrModelClose(seq) == close_error && file_live);
            close_error = 0;
        }
        close_model(seq);
        TEST_CHECK(!vjoOcrComplete(&message));
    }
}
static void model_cancel_exit_restart_and_late_io(void)
{
    for (int action=1; action<=4; action++) for (int at_read=0; at_read<2; at_read++) {
        setup(); uint32_t seq = submit(); take(seq);
        unsigned char bytes[64]; memset(bytes, 0xCC, sizeof(bytes));
        if (at_read) { open_model(seq); TEST_CHECK(vjoOcrModelRead(seq, 0, sizeof(bytes), bytes) == VJO_ERR_BUSY); }
        else TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY);
        during_io = action; model_tick();
        /* The operation completed after cancellation/exit/restart/Close;
         * ownership still pins the live descriptor until the kernel drains it. */
        TEST_CHECK(file_live && game_ocr_capture_busy_locked());
        TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY);
        if (action != 4) TEST_CHECK(vjoOcrModelRead(seq, 0, sizeof(bytes), bytes) < 0);
        TEST_CHECK(bytes[0] == 0xCC);
        model_tick(); TEST_CHECK(!file_live);
        if (action != 2) TEST_CHECK(!vjoOcrModelClose(seq));
        if (action == 2) TEST_CHECK(!game_ocr_capture_busy_locked() && !game_ocr_shutdown_locked());
    }
    setup(); uint32_t seq = submit(); take(seq);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(!file_live && !model_opens && !vjoOcrModelClose(seq)); /* Cancel queued Open. */
}
static void model_failed_close_and_orphan_recovery(void)
{
    setup(); uint32_t seq = submit(); take(seq); open_model(seq);
    close_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(vjoOcrModelClose(seq) == close_error && file_live);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrModelClose(seq+1) == VJO_ERR_ARG && file_live);
    close_error = 0; close_model(seq); TEST_CHECK(!vjoOcrComplete(&message));
    setup(); seq = submit(); take(seq); open_model(seq);
    close_error = (int)0x80010005u; g.game_active = 0; game_ocr_process_gone(7); model_tick();
    TEST_CHECK(file_live && model_closes == 1);
    unsigned events = model_events;
    for (int i=0; i<20; i++) model_tick();
    TEST_CHECK(model_closes == 1 && model_events == events); /* No event-driven busy loop. */
    caller = 3; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY);
    close_error = 0; model_clock += 100000; model_tick();
    TEST_CHECK(!file_live && model_closes == 2 && !game_ocr_capture_busy_locked());
}
static void model_syscalls_wait_for_the_worker(void)
{
    setup(); g.mevf = 5; wait_runs_worker = 1;
    uint32_t seq = submit(); take(seq);
    VjoGameOcrModelInfo info; memset(&info, 0xCC, sizeof(info));
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == (int)VJO_VOCR_FL10_BYTES && file_live && model_opens == 1);
    TEST_CHECK(info.size == sizeof(info) && info.bytes == VJO_VOCR_FL10_BYTES && !info.stat_rc && stats == 1);
    TEST_CHECK(info.mtime && info.mtime == pack_time(&file_stat.st_mtime));
    static unsigned char chunk[VJO_GAME_OCR_MODEL_CHUNK];
    TEST_CHECK(vjoOcrModelRead(seq, 4096, sizeof(chunk), chunk) == (int)sizeof(chunk) && model_reads == 1);
    TEST_CHECK(chunk[0] == 4096 % 251 && chunk[sizeof(chunk)-1] == (4096 + sizeof(chunk) - 1) % 251);
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && model_closes == 1);
    /* One wait per operation, each ended by the worker's completion. */
    TEST_CHECK(waits == 3 && done_events == 3 && !locks && !syscall_depth);
    TEST_MSG("waits=%u done=%u", waits, done_events);
    TEST_CHECK(!vjoOcrComplete(&message));
}
static void model_wait_is_bounded(void)
{
    setup(); g.mevf = 5; /* the kernel worker does not run during the wait */
    uint32_t seq = submit(); take(seq);
    uint64_t before = model_clock;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY && !model_opens && waits == 1);
    TEST_CHECK(model_clock - before == MODEL_SYSCALL_WAIT_US);
    model_tick(); /* the worker, later: its completion is left pending */
    TEST_CHECK(done_pending);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES && waits == 1);
    /* The next wait consumes that stale completion, re-checks, then times out. */
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY && file_live && waits == 3 && !done_pending);
    model_tick();
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && waits == 3);
    TEST_CHECK(!vjoOcrComplete(&message));
}
static void model_open_reports_modification_time(void)
{
    setup(); g.mevf = 5; wait_runs_worker = 1; stat_error = (int)0x80010016u;
    uint32_t seq = submit(); take(seq);
    VjoGameOcrModelInfo info; memset(&info, 0xCC, sizeof(info));
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == (int)VJO_VOCR_FL10_BYTES && file_live);
    TEST_CHECK(info.stat_rc == stat_error && !info.mtime && info.bytes == VJO_VOCR_FL10_BYTES);
    copy_out_fail = 1; /* a failed copy of the report is retryable while the file stays open */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == VJO_ERR_COPY);
    copy_out_fail = 0;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == (int)VJO_VOCR_FL10_BYTES && model_opens == 1 && stats == 1);
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && !vjoOcrComplete(&message));
}
static void model_cancel_while_waiting(void)
{
    setup(); g.mevf = 5; wait_runs_worker = 1;
    uint32_t seq = submit(); take(seq);
    during_io = 1; /* the foreground changes while the worker opens the file */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_CANCELLED);
    TEST_CHECK(file_live && game_ocr_capture_busy_locked());
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && model_closes == 1);
    TEST_CHECK(!vjoOcrComplete(&message) && !game_ocr_capture_busy_locked());
}
/* The fallback is a second pinned name the Shell submits; the game only
 * picks which of the two to open, never a path or another name. */
static void fallback_is_one_of_the_submitted_pinned_names(void)
{
    setup(); VjoGameOcrRequest r = make_request();
    memset(r.fallback, 'x', sizeof(r.fallback)); /* unterminated */
    caller = 3; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    setup(); r = make_request(); strcpy(r.model, "H15_w8.vocr"); strcpy(r.fallback, "FL10_w8.vocr");
    caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
    TEST_CHECK(!strcmp(request.fallback, "FL10_w8.vocr"));
    TEST_CHECK(vjoOcrModelOpen(seq, 2, NULL) == VJO_ERR_ARG && !model_opens);
    expect_fallback = 1; expected_model_size = VJO_VOCR_FL10_BYTES;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == (int)VJO_VOCR_FL10_BYTES && model_opens == 1);
    /* One file per open: the other name is refused while this one is open. */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && model_opens == 1);
    close_model(seq);
    TEST_CHECK(!vjoOcrComplete(&message));
    /* No fallback, or a name outside the pinned set: nothing is opened. */
    const char *refused[] = {"", "../secret", "meiki-stream-int8.mnn", "FL10_w8.vocr/../x"};
    for (unsigned i = 0; i < sizeof(refused)/sizeof(refused[0]); i++) {
        setup(); r = make_request(); strcpy(r.fallback, refused[i]);
        caller = 3; seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == VJO_ERR_ARG && !model_opens);
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    }
}

static void modification_times_pack_distinctly(void)
{
    const SceDateTime a = {2026, 10, 11, 9, 43, 58, 123456};
    const uint64_t base = pack_time(&a);
    SceDateTime b;
    TEST_CHECK(base != 0);
    b = a; b.year++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.month++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.day++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.hour++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.minute++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.second++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.microsecond++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.second = 59; b.microsecond = 999999;
    SceDateTime c = a; c.minute++; c.second = 0; c.microsecond = 0;
    TEST_CHECK(pack_time(&c) > pack_time(&b));
}

TEST_LIST = {
    {"model syscalls wait for the kernel worker: Open Read32K Close each finish in one call", model_syscalls_wait_for_the_worker},
    {"model wait is bounded; BUSY still means repeat; a stale completion is re-checked", model_wait_is_bounded},
    {"Open reports the file modification time; a failed stat leaves it unknown and retryable", model_open_reports_modification_time},
    {"cancellation during a waited Open is reported and Close drains in one call", model_cancel_while_waiting},
    {"packed modification times differ for every field", modification_times_pack_distinctly},
    {"the fallback is one of the submitted pinned names; Open picks which", fallback_is_one_of_the_submitted_pinned_names},
    {"removed_Meiki_recognizer_is_not_in_the_broker_allowlist",removed_meiki_model_is_refused},
    {"VOCR2_exact_pin_keeps_canonical_path_and_final_chunk_bounds",vocr2_fixed_pin_preserves_paths_and_bounds},
    {"model broker permissions canonical paths bounds and cleanup acknowledgement", model_permissions_paths_and_bounds},
    {"model broker short reads copy retry EOF and native errors", model_short_read_copy_retry_and_errors},
    {"model broker late I/O cancellation exit Shell restart and pending close", model_cancel_exit_restart_and_late_io},
    {"model broker failed close and automatic orphan recovery", model_failed_close_and_orphan_recovery},
    {"single-pass raw access and stale job sequences", single_pass_raw_and_stale_sequences},
    {"registration and caller permissions", registration_and_permissions},
    {"completion and stale replacement", complete_and_replacement},
    {"queued and claimed capture pins", capture_pins_queued_and_claimed},
    {"cancelled reader must drain", cancellation_drains_reader},
    {"cancel before take", cancel_before_take},
    {"suspend and same PID epoch", suspended_and_same_pid_epoch},
    {"foreground replacement and process exit", foreground_replacement_and_exit},
    {"Shell restart", shell_restart},
    {"failed cleanup and module replacement", cleanup_failure_and_module_replacement},
    {"capture and wire validation", invalid_capture_and_wire},
    {"copy failures and sequence exhaustion", user_copy_failures_and_sequence_exhaustion},
    {"shutdown ownership", shutdown_refuses_claim_and_closes_registration},
    {NULL, NULL}
};
