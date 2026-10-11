/* OCR requests, copied capture/model bytes and results. Inference stays in-game.
 * g.lock serializes this transport with capture allocation/read/free. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>
#include "vjo_kernel.h"
#include "game_ocr.h"
#include "../include/vjo_vocr_sizes.h"

static VjoGameOcrRequest request;
static VjoGameOcrResult result, incoming; /* Never put a 4 KiB result on syscall stacks. */
static SceUID worker_pid, worker_module, request_pid, request_shell;
static uint32_t worker_epoch, request_epoch, sequence;
static int valid, pending, claimed, cancelled, completed, stopping;

enum { MODEL_IDLE, MODEL_QUEUED, MODEL_RUNNING, MODEL_DONE };
enum { MODEL_OPEN = 1, MODEL_READ, MODEL_CLOSE };
static struct {
    uint32_t seq, offset, bytes, size;
    SceUID pid, fd;
    int phase, operation, rc, closing, orphan;
    uint64_t retry_us;
    char path[VJO_GAME_OCR_MODEL_DIR_BYTES];
} model_io;
/* One kernel staging buffer; weights and inference remain in game USER_RW. */
static unsigned char model_stage[VJO_GAME_OCR_MODEL_CHUNK];

static void model_reset_locked(void)
{
    memset(&model_io, 0, sizeof(model_io));
    model_io.fd = -1;
}

static void model_wake_locked(void)
{
    ksceKernelSetEventFlag(g.ievf, IEV_MODEL);
}

static void model_invalidate_locked(void)
{
    if (!model_io.seq || model_io.closing) return;
    model_io.closing = 1;
    model_wake_locked();
}

static int shell_caller(SceUID pid)
{
    return pid > 0 && pid == g.shell_pid;
}

static int registration_current(SceUID pid)
{
    return pid > 0 && pid == worker_pid && pid == g.game_pid && g.game_active &&
           !stopping &&
           worker_epoch == __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
}

static void invalidate_locked(void)
{
    if (!valid) return;
    cancelled = 1;
    completed = 0;
    pending = 0;
    model_invalidate_locked();
    /* A taken job can still be executing after suspend, timeout or Shell
     * restart. Only its drained completion or actual process exit unpins it. */
}

static void sync_locked(void)
{
    if (valid && (request_epoch != __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE) ||
                  request_pid != g.game_pid || !g.game_active ||
                  request_shell != g.shell_pid))
        invalidate_locked();
}

void game_ocr_init(void)
{
    memset(&request, 0, sizeof(request));
    memset(&result, 0, sizeof(result));
    memset(&incoming, 0, sizeof(incoming));
    worker_pid = worker_module = request_pid = request_shell = 0;
    worker_epoch = request_epoch = sequence = 0;
    valid = pending = claimed = cancelled = completed = stopping = 0;
    model_reset_locked();
}

int game_ocr_capture_busy_locked(void)
{
    sync_locked();
    return pending || claimed || model_io.seq;
}

int game_ocr_raw_caller_locked(SceUID pid)
{
    sync_locked();
    return valid && claimed && !cancelled && pid == request_pid &&
           registration_current(pid) && request.done_seq == g.done_seq &&
           request.done_seq == g.capture_seq && request.width == g.crop_w &&
           g.capture_game_pid == request_pid && g.capture_epoch == request_epoch &&
           request.height == g.crop_h && request.stride == g.raw_stride &&
           g.capture_once;
}

int game_ocr_shutdown_locked(void)
{
    sync_locked();
    if (pending || claimed || model_io.seq) return VJO_ERR_BUSY;
    stopping = 1;
    invalidate_locked();
    return 0;
}

void game_ocr_shell_changed_locked(void)
{
    __atomic_add_fetch(&g.ocr_epoch, 1, __ATOMIC_RELEASE);
    invalidate_locked();
    worker_epoch = 0;
}

void game_ocr_foreground_changed_locked(void)
{
    sync_locked();
}

void game_ocr_process_gone(SceUID pid)
{
    VJO_LOCK();
    if (pid == request_pid) {
        invalidate_locked();
        claimed = pending = 0;
    }
    if (pid == model_io.pid && model_io.seq) {
        model_io.orphan = 1;
        model_invalidate_locked();
    }
    if (pid == worker_pid) {
        worker_pid = worker_module = 0;
        worker_epoch = 0;
    }
    VJO_UNLOCK();
}

int vjoOcrRegister(void)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = taiGetModuleInfoForKernel(pid, VJO_GAME_OCR_MODULE, &info);
    if (rc < 0) { rc = VJO_ERR_PERM; goto out; }
    VJO_LOCK();
    sync_locked();
    if (stopping || pid <= 0 || pid != g.game_pid || !g.game_active) rc = VJO_ERR_NO_GAME;
    else if (claimed && (pid != worker_pid || worker_module != info.modid ||
                        worker_epoch != __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE)))
        rc = VJO_ERR_BUSY;
    else {
        worker_pid = pid;
        worker_module = info.modid;
        worker_epoch = __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
        rc = 0;
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrSubmit(const VjoGameOcrRequest *user_request)
{
    uint32_t state;
    VjoGameOcrRequest next;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    if (!user_request || ksceKernelMemcpyUserToKernel(&next, user_request, sizeof(next)) < 0) {
        rc = VJO_ERR_COPY; goto out;
    }
    if (next.size != sizeof(next) || !next.model[0] || next.model[sizeof(next.model)-1] != 0 ||
        !next.model_dir[0] || next.model_dir[sizeof(next.model_dir)-1] != 0) {
        rc = VJO_ERR_ARG; goto out;
    }
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (pending || claimed || model_io.seq) rc = VJO_ERR_BUSY;
    else if (!registration_current(worker_pid)) rc = VJO_ERR_NO_GAME;
    else if (g.capture_state != CAPTURE_IDLE || !g.raw_valid || !g.raw ||
             g.capture_result || !g.capture_once || !next.done_seq ||
             next.done_seq != g.done_seq || next.done_seq != g.capture_seq ||
             g.capture_game_pid != worker_pid ||
             g.capture_epoch != __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE) ||
             !g.crop_w || !g.crop_h || g.crop_w > VJO_MAX_W || g.crop_h > VJO_MAX_H ||
             g.raw_stride != g.crop_w * 4 || g.raw_capacity < g.raw_stride * g.crop_h)
        rc = VJO_ERR_ARG;
    else if (sequence == 0x7FFFFFFFu) rc = VJO_ERR_BUSY; /* Never recycle a sequence. */
    else {
        next.seq = ++sequence;
        next.width = g.crop_w;
        next.height = g.crop_h;
        next.stride = g.raw_stride;
        request = next;
        request_pid = worker_pid;
        request_shell = pid;
        request_epoch = __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
        memset(&result, 0, sizeof(result));
        valid = pending = 1;
        claimed = cancelled = completed = 0;
        rc = (int)request.seq;
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrTake(VjoGameOcrRequest *out)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!registration_current(pid)) rc = VJO_ERR_PERM;
    else if (claimed) rc = VJO_ERR_BUSY;
    else if (!pending || !valid || cancelled) rc = 1;
    else if (!out || ksceKernelMemcpyKernelToUser(out, &request, sizeof(request)) < 0)
        rc = VJO_ERR_COPY;
    else { pending = 0; claimed = 1; rc = 0; }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrReadRaw(uint32_t seq, uint32_t row, uint32_t n, void *dst)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    if (!game_ocr_raw_caller_locked(pid)) rc = VJO_ERR_PERM;
    else if (seq != request.seq || !dst || !n || row >= g.crop_h ||
             n > g.crop_h - row || g.capture_state != CAPTURE_IDLE || !g.raw_valid || !g.raw)
        rc = VJO_ERR_ARG;
    else if (ksceKernelMemcpyKernelToUser(dst, g.raw + row * g.raw_stride, n * g.raw_stride) < 0)
        rc = VJO_ERR_COPY;
    else { rc = (int)n; buffers_read_done(row, n); }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrCancelled(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed || seq != request.seq) rc = VJO_ERR_ARG;
    else rc = cancelled ? 1 : 0;
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrComplete(const VjoGameOcrResult *user_result)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed) rc = VJO_ERR_ARG;
    else if (!user_result || ksceKernelMemcpyUserToKernel(&incoming, user_result, sizeof(incoming)) < 0)
        rc = VJO_ERR_COPY;
    else if (!incoming.cleanup_status && model_io.seq) rc = VJO_ERR_BUSY;
    else if (incoming.size != sizeof(incoming) || incoming.seq != request.seq ||
             incoming.text[sizeof(incoming.text)-1] != 0 ||
             (incoming.rc > 0 || (!incoming.rc && incoming.cleanup_status) ||
              (incoming.rc && incoming.text[0]))) rc = VJO_ERR_ARG;
    else {
        if (!cancelled) {
            result = incoming;
            completed = 1;
        }
        if (!incoming.cleanup_status) claimed = 0;
        /* Failed cleanup retains the claim and may be retried. Completed
         * cancellation drains the reader but never republishes stale text. */
        rc = 0;
    }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
    else if (cancelled) rc = VJO_ERR_CANCELLED;
    else if (!completed) rc = 1;
    else if (!out || ksceKernelMemcpyKernelToUser(out, &result, sizeof(result)) < 0)
        rc = VJO_ERR_COPY;
    else rc = 0;
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrCancel(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
    else { invalidate_locked(); rc = 0; }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

static uint32_t model_size(const char *name)
{
    if (!strcmp(name, "FL10_w8.vocr")) return VJO_VOCR_FL10_BYTES;
    if (!strcmp(name, "H15_w8.vocr")) return VJO_VOCR_H15_BYTES;
    if (!strcmp(name, "F20_w8.vocr")) return VJO_VOCR_F20_BYTES;
    return 0;
}

static int model_caller_locked(SceUID pid, uint32_t seq, int cleanup)
{
    sync_locked();
    if (pid <= 0 || pid != request_pid) return VJO_ERR_PERM;
    if (!valid || !claimed || seq != request.seq) return VJO_ERR_ARG;
    if (!cleanup && (cancelled || !registration_current(pid))) return VJO_ERR_CANCELLED;
    return 0;
}

int vjoOcrModelOpen(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    rc = model_caller_locked(pid, seq, 0);
    if (rc) goto out;
    if (model_io.seq) {
        if (model_io.seq != seq || model_io.pid != pid) rc = VJO_ERR_BUSY;
        else if (model_io.closing) rc = VJO_ERR_CANCELLED;
        else if (model_io.phase == MODEL_DONE && model_io.operation == MODEL_OPEN) {
            rc = model_io.rc;
            model_io.phase = MODEL_IDLE;
        } else if (model_io.phase == MODEL_IDLE && model_io.fd >= 0) rc = (int)model_io.size;
        else rc = VJO_ERR_BUSY;
    } else {
        uint32_t size = model_size(request.model);
        if (!size || strcmp(request.model_dir, VJO_VOCR_MODEL_ROOT)) { rc = VJO_ERR_ARG; goto out; }
        model_io.seq = seq;
        model_io.pid = pid;
        model_io.size = size;
        memcpy(model_io.path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT));
        /* Both strings are bounded and already NUL-validated by Submit. */
        memcpy(model_io.path + sizeof(VJO_VOCR_MODEL_ROOT), request.model, sizeof(request.model));
        model_io.operation = MODEL_OPEN;
        model_io.phase = MODEL_QUEUED;
        model_wake_locked();
        rc = VJO_ERR_BUSY;
    }
out:
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t bytes, void *dst)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    rc = model_caller_locked(pid, seq, 0);
    if (rc) goto out;
    if (model_io.seq != seq || model_io.pid != pid || model_io.fd < 0 ||
        !dst || !bytes || bytes > sizeof(model_stage) || offset > model_io.size ||
        bytes > model_io.size - offset) { rc = VJO_ERR_ARG; goto out; }
    if (model_io.closing) { rc = VJO_ERR_CANCELLED; goto out; }
    if (model_io.phase != MODEL_IDLE) {
        if (model_io.operation != MODEL_READ || offset != model_io.offset || bytes != model_io.bytes)
            rc = VJO_ERR_ARG;
        else if (model_io.phase != MODEL_DONE) rc = VJO_ERR_BUSY;
        else if (model_io.rc > 0 &&
                 ksceKernelMemcpyKernelToUser(dst, model_stage, (SceSize)model_io.rc) < 0)
            rc = VJO_ERR_COPY; /* The exact completed chunk remains retryable. */
        else { rc = model_io.rc; model_io.phase = MODEL_IDLE; }
    } else {
        model_io.offset = offset;
        model_io.bytes = bytes;
        model_io.operation = MODEL_READ;
        model_io.phase = MODEL_QUEUED;
        model_wake_locked();
        rc = VJO_ERR_BUSY;
    }
out:
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrModelClose(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    rc = model_caller_locked(pid, seq, 1);
    if (rc) goto out;
    if (!model_io.seq) rc = 0;
    else if (model_io.seq != seq || model_io.pid != pid) rc = VJO_ERR_ARG;
    else if (model_io.phase == MODEL_DONE && model_io.operation == MODEL_CLOSE && model_io.rc < 0) {
        rc = model_io.rc;
        model_io.phase = MODEL_IDLE; /* Retain FD; a subsequent Close retries. */
    } else {
        if (!model_io.closing || model_io.phase == MODEL_IDLE) {
            model_io.closing = 1;
            model_io.retry_us = 0; /* Explicit cleanup retry need not wait for orphan backoff. */
            model_wake_locked();
        }
        rc = VJO_ERR_BUSY;
    }
out:
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

void game_ocr_model_tick(void)
{
    int operation, rc;
    SceUID fd;
    uint32_t offset, bytes, size;
    char path[sizeof(model_io.path)];
    VJO_LOCK();
    sync_locked();
    if (!model_io.seq || model_io.phase == MODEL_RUNNING) { VJO_UNLOCK(); return; }
    if (model_io.closing || model_io.orphan) {
        if (model_io.retry_us > (uint64_t)ksceKernelGetSystemTimeWide()) { VJO_UNLOCK(); return; }
        /* Pending I/O has no caller pointer and can be discarded before it
         * starts. In-flight I/O reaches this only after its completion. */
        model_io.operation = MODEL_CLOSE;
        model_io.phase = MODEL_QUEUED;
    }
    if (model_io.phase != MODEL_QUEUED) { VJO_UNLOCK(); return; }
    operation = model_io.operation;
    fd = model_io.fd;
    offset = model_io.offset;
    bytes = model_io.bytes;
    size = model_io.size;
    memcpy(path, model_io.path, sizeof(path));
    model_io.phase = MODEL_RUNNING;
    VJO_UNLOCK();

    /* Only the persistent kernel worker executes these calls. Calling them
     * directly in a game's syscall would retain its unavailable ux0 namespace. */
    if (operation == MODEL_OPEN) {
        fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
        rc = fd;
        if (fd >= 0) {
            SceOff length = ksceIoLseek(fd, 0, SCE_SEEK_END);
            rc = length < 0 ? (int)length : length == size ? (int)size : VJO_ERR_FORMAT;
        }
    } else if (operation == MODEL_READ) {
        rc = ksceIoPread(fd, model_stage, bytes, (SceOff)offset);
        if (rc > (int)bytes) rc = VJO_ERR_FORMAT;
    } else {
        rc = fd < 0 ? 0 : ksceIoClose(fd);
        if (rc >= 0) fd = -1;
    }

    VJO_LOCK();
    /* No syscall can release/reuse the owner while phase is RUNNING. */
    model_io.fd = fd;
    model_io.rc = rc;
    model_io.phase = MODEL_DONE;
    sync_locked();
    if (operation == MODEL_CLOSE && rc >= 0) model_reset_locked();
    else if (operation == MODEL_CLOSE) {
        model_io.closing = 0;
        if (model_io.orphan || cancelled) {
            model_io.closing = 1;
            model_io.retry_us = ksceKernelGetSystemTimeWide() + 100000u;
        }
    } else if (model_io.closing || model_io.orphan) model_wake_locked();
    VJO_UNLOCK();
}
