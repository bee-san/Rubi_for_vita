/* OCR requests, copied capture/model bytes and results. Inference stays in-game.
 * g.lock serializes this transport, the region, its signatures and the kernel strip. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "vjo_kernel.h"
#include "game_ocr.h"
#include "../core/games.h"
#include "../include/vjo_vocr_sizes.h"

static VjoGameOcrRequest request;
static VjoGameOcrResult result, incoming; /* Never put a 4 KiB result on syscall stacks. */
#if !VJO_DEBUG
/* #528: the game's memory over one activation (from the game's start or resume in front to its leaving it), for the
 * normal build's kernel.txt; the debug build's log.txt has every job's memory check (shell/worker.c
 * log_vocr_measurements). vjoOcrComplete adds each job's check once, as the job's claim is released; the kernel worker
 * writes the line as the game leaves the front (game_ocr_activation_memory, kernel/main.c) and starts afresh. Under
 * g.lock. low and last are free USER at the checks, valid when known > 0. */
static struct {
    uint32_t checks, refused, known, weights, workspace;
    int32_t low, last;
} act_mem;

static void act_mem_note_locked(const VjoGameOcrResult *r)
{
    int64_t room;
    if (!r->reserve_bytes) return; /* the job ended before its memory check */
    act_mem.checks++;
    /* refused as the Shell's own line says it (shell/worker.c vocr_headroom): free USER, with the worker's resident
     * weights of another model it gives back first, against the reserve and what the job had to take */
    room = (int64_t)r->free_user + r->release_bytes - r->reserve_bytes - r->need_workspace - r->need_weights;
    if (r->free_user < 0 || room < 0) act_mem.refused++;
    if (r->free_user >= 0) {
        if (!act_mem.known++ || r->free_user < act_mem.low) act_mem.low = r->free_user;
        act_mem.last = r->free_user;
    }
    if (r->resident_bytes > act_mem.weights) act_mem.weights = r->resident_bytes;
    if (r->workspace_bytes > act_mem.workspace) act_mem.workspace = r->workspace_bytes;
}

void game_ocr_activation_memory(void)
{
    uint32_t checks, refused, weights, workspace;
    int32_t low, last;
    VJO_LOCK();
    checks = act_mem.checks;
    refused = act_mem.refused;
    weights = act_mem.weights;
    workspace = act_mem.workspace;
    low = act_mem.known ? act_mem.low : -1;
    last = act_mem.known ? act_mem.last : -1;
    memset(&act_mem, 0, sizeof(act_mem));
    VJO_UNLOCK();
    if (checks)
        klog("game memory: %u checks, %u refused; free USER lowest %d B, last %d B; weights kept %u B, workspace up "
             "to %u B", checks, refused, low, last, weights, workspace);
}
#endif
static SceUID worker_pid, worker_module, request_pid, request_shell;
/* #138: the registered worker's thread. A game holds one worker (the kernel loads
 * it, game_ocr/main.c keeps one copy per process); should a second copy still run
 * (a per-title taiHEN line too), its thread is refused here, so it never takes a
 * job or holds weights; refused_thread: the last one logged. */
static SceUID worker_thread, refused_thread;
static uint32_t worker_epoch, request_epoch, sequence;
static int valid, pending, claimed, cancelled, completed, stopping;
/* Threads inside vjoOcrTake's or vjoOcrRead's wait (under g.lock): module_stop
 * wakes them and waits until they have left before the module goes. */
static int waiters;

enum { MODEL_IDLE, MODEL_QUEUED, MODEL_RUNNING, MODEL_DONE };
/* SHOT_WRITE, SHOT_SAVE (API 23): a shot's JPEG, the other way through the same broker. */
enum { MODEL_OPEN = 1, MODEL_READ, MODEL_CLOSE, SHOT_WRITE, SHOT_SAVE, HOOK_CACHE_LOAD, HOOK_CACHE_SAVE, PROFILE_LOAD };
#include "../core/hook_profile_file.h"
int vjoGetTextSource(void);
#define PROFILE_WHICH 0xFFFFFFFEu
#define HOOK_CACHE_WHICH 0xFFFFFFFFu
#define HOOK_CACHE_PATH "ux0:data/VitaJPOverlay/hooks.cache"
#define HOOK_CACHE_TEMP HOOK_CACHE_PATH ".tmp"
#define HOOK_CACHE_BACKUP HOOK_CACHE_PATH ".bak"
static struct {
    uint32_t seq, offset, bytes, size;
    uint32_t which;    /* VJO_GAME_OCR_OPEN_*: the request's model or its fallback */
    SceUID pid, fd;
    int phase, operation, rc, closing, orphan;
    int stat_rc;       /* Open: the modification-time query's native error, or 0 */
    uint64_t retry_us;
    uint64_t mtime;    /* Open: packed modification time, 0 if unknown */
    /* API 23: a shot's file (shot): the bytes written so far and the last two of them;
     * saved once it has its .jpg name (a close then removes nothing) */
    int shot, saved;
    uint32_t written;
    uint8_t tail[2];
    char path[VJO_GAME_OCR_MODEL_DIR_BYTES];
} model_io;
/* One kernel staging buffer; weights and inference remain in game USER_RW. A
 * shot's JPEG crosses it too, a chunk at a time (API 23). */
static unsigned char model_stage[VJO_GAME_OCR_MODEL_CHUNK];
typedef char vjo_profile_stage_check[VJO_PROFILE_BYTES <= sizeof(model_stage) ? 1 : -1];
typedef char vjo_hook_cache_stage_check[VJO_GAME_OCR_HOOK_CACHE_BYTES <= sizeof(model_stage) ? 1 : -1];
typedef char vjo_sc3_stage_check[VJO_GAME_OCR_SC3_BYTES <= sizeof(model_stage) ? 1 : -1]; /* a charset: one read */
typedef char vjo_shot_chunk_check[VJO_GAME_OCR_SHOT_CHUNK == sizeof(model_stage) ? 1 : -1];
/* API 54: the spotter's pinned name, as every name the broker opens, fits a request's model field (the copy below) */
typedef char vjo_spot_name_check[sizeof(VJO_VOCR_SPOT_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                 sizeof(VJO_VOCR_GATE_NAME) <= VJO_GAME_OCR_MODEL_BYTES ? 1 : -1];
/* ... and so do the model sizes', the vertical model's, the cascade's and the LMs' (the copy below; a size's name is
 * also the Shell's request.model, shell/worker.c, whose snprintf would cut a longer one, which this broker would then
 * refuse): a pin set's longer names (v0.4.1-vocr6.2's VOCR6.2_* take 18-21 B with the NUL) fail here first. */
typedef char vjo_model_names_check[sizeof(VJO_VOCR_TIER1_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_TIER2_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_TIER3_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_VERTICAL_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_CASCADE_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_LM1_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_LM2_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_LM3_NAME) <= VJO_GAME_OCR_MODEL_BYTES &&
                                   sizeof(VJO_VOCR_LM4_NAME) <= VJO_GAME_OCR_MODEL_BYTES ? 1 : -1];
/* exFAT's first file extension while a game ran took 20.9 s on the console;
 * overwriting reserved space took 6 ms. Keep one maximum-sized spare on disk,
 * prepared while the broker is idle. No image or extra buffer is kept here. */
#define SHOT_SPARE_PATH "ux0:data/VitaJPOverlay/screenshot.reserve"
static int shot_spare_ready, shot_spare_usable; /* file worker only */
static SceUID shot_spare_fd = -1; /* retained across a failed close */
static int shot_spare_busy; /* g.lock: do not unload while its file is owned */
static int shot_spare_preparing; /* g.lock: no job may start behind reserve I/O */
static uint64_t shot_spare_retry_us;


/* The worker's rows request (vjoOcrCaptureRows), armed by its syscall and
 * copied by the display hook in the game's own thread. state moves IDLE ->
 * PENDING (the syscall, under g.lock) -> COPYING (the hook, CAS) -> DONE (the
 * hook) -> IDLE (the syscall, under g.lock), or PENDING -> IDLE when the
 * syscall gives up (CAS). The syscall returns only once the state is IDLE
 * again, so the kernel never holds the worker's buffer beyond that call. */
enum { ROWS_IDLE, ROWS_PENDING, ROWS_COPYING, ROWS_DONE };
static struct {
    volatile int state;
    SceUID pid;
    OcrRowsJob job;
    int rc;                         /* the hook's: rows copied or the error */
    uint32_t fb_pitch, fb_fmt, fb_w, fb_h;
    int64_t armed_us, copy_start_us, copy_end_us;
    uint32_t sig_next;              /* the first pass's next row while it runs */
    uint32_t sig_seq;               /* the job it belongs to */
    int orphan;                     /* its process exited during a copy: the kernel worker tidies up */
    int abandoned;                  /* the copy outlived its rows call (BUSY): the kernel worker returns
                                       the state to IDLE once the copy ends; no new request meanwhile */
    int in_call;                    /* a rows call is between arming and its last use of the state and
                                       the kernel strip: nobody else resets or frees them meanwhile */
    uint64_t gone_us;               /* when its process exited during the call */
    volatile uint32_t frame_start;  /* API 55: a frame copy's start (the system time's low 32 bits, | 1) while
                                       the hook copies it, else 0: the watchdog's (game_ocr_rows_tick) */
} rows;
/* API 55 (VJO_ROWS_FRAME, docs/game-pause.md): set by the watchdog when a frame copy held the game's display call
 * for longer than PAUSE_WATCHDOG_US, which its own cap (VJO_GAME_OCR_PAUSE_MAX_US, checked between chunks) should
 * never allow; from then on, until the kernel module restarts (the next boot), every frame copy is refused
 * (VJO_ERR_UNSUPPORTED) and jobs read strip by strip, as without ocr_pause. */
static int pause_off;
#define PAUSE_WATCHDOG_US (2u * VJO_GAME_OCR_PAUSE_MAX_US)
/* The watchdog's count: the frame copy it is timing (its frame_start), the time it has seen that copy running with
 * its game in front, and its last look. Only that time counts (#355's review M1): a game suspended (the PS button, a
 * crash dump) while its display thread is in a copy may hold that thread, and the copy, for as long as the suspension
 * lasts (inferred, unmeasured), and a look after a longer gap than PAUSE_WATCHDOG_GAP_US (the kernel worker did not
 * run: standby) counts nothing; neither is a stalled row read. */
static uint32_t wd_copy, wd_held, wd_look;
#define PAUSE_WATCHDOG_GAP_US 100000u
/* A frame has to come within this; cancellation is checked every slice. */
#define ROWS_WAIT_US 1500000u
#define ROWS_SLICE_US 50000u
/* A copy the hook started finishes within this (one strip, a few ms). */
#define ROWS_COPY_WAIT_US 2000000u
/* Longer than any call can stay inside the kernel (ROWS_WAIT_US + ROWS_COPY_WAIT_US). */
#define ROWS_GONE_US 10000000u
/* A model syscall waits this long for the kernel worker before answering
 * BUSY; the game then yields and repeats it, as before. */
#define MODEL_SYSCALL_WAIT_US 50000u
/* With no job queued, the game worker's vjoOcrTake waits this long for one
 * (MEV_JOB) before answering 1, so a job is claimed when it is submitted, not
 * at the worker's next 100 ms poll; with no result yet, the Shell's vjoOcrRead
 * waits this long for it (MEV_RESULT). One wait each, without the lock; the
 * periods match the polls they replace, so a caller's loop keeps its pace. */
#define TAKE_WAIT_US 100000u
#define READ_WAIT_US 50000u

/* Waits for bits of g.mevf for at most us, without g.lock (the caller cleared
 * them under the lock when it saw nothing to take). */
static void event_wait(unsigned int bits, SceUInt us)
{
    unsigned int out = 0;
    SceUInt timeout = us;
    ksceKernelWaitEventFlag(g.mevf, bits, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &out, &timeout);
}

static void model_reset_locked(void)
{
    memset(&model_io, 0, sizeof(model_io));
    model_io.fd = -1;
}

static void model_wake_locked(void)
{
    ksceKernelSetEventFlag(g.ioevf, IEV_MODEL);
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
    if (model_io.pid == request_pid && model_io.seq == request.seq && model_io.which != PROFILE_WHICH)
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
    worker_thread = refused_thread = 0;
    worker_epoch = request_epoch = sequence = 0;
    valid = pending = claimed = cancelled = completed = stopping = 0;
    shot_spare_ready = shot_spare_usable = shot_spare_busy = shot_spare_preparing = 0;
    shot_spare_fd = -1;
    shot_spare_retry_us = 0;
    waiters = 0;
    model_reset_locked();
    memset(&rows, 0, sizeof(rows));
    pause_off = 0;
    wd_copy = wd_held = wd_look = 0;
#if !VJO_DEBUG
    memset(&act_mem, 0, sizeof(act_mem));
#endif
}

#ifdef VJO_HOST
/* A job is queued or claimed, or a model or rows call is in flight: the state
 * vjoOcrSubmit refuses a new job on (VJO_ERR_BUSY), which it tests inline; only
 * the host tests call this (not in the module). */
int game_ocr_capture_busy_locked(void)
{
    sync_locked();
    return pending || claimed || model_io.seq || rows.state != ROWS_IDLE || shot_spare_preparing;
}
#endif

SceUID game_ocr_worker_pid_locked(void)
{
    return worker_pid > 0 && registration_current(worker_pid) ? worker_pid : 0;
}

/* No job is queued, claimed or in a model or rows call: the worker is refused
 * from now on. A job slot left (cancelled, read, or completed and not read yet)
 * is kept as it is, so an undone stop (game_ocr_stop) leaves the transport as
 * it found it; a take or read waiting in the kernel wakes and re-checks. */
int game_ocr_shutdown_locked(void)
{
    sync_locked();
    if (pending || claimed || model_io.seq || rows.state != ROWS_IDLE || shot_spare_busy) return VJO_ERR_BUSY;
    stopping = 1;
    if (waiters && g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_JOB | MEV_RESULT);
    return 0;
}

#ifdef VJO_HOST
int game_ocr_waiters_locked(void) /* the host tests' only (not in the module) */
{
    return waiters;
}
#endif

/* module_stop's part: the shutdown, then until the takes and reads it woke have
 * left their wait (they give their count back under the lock), at most
 * STOP_DRAIN_POLLS ms, and 1 ms more for the few instructions such a thread
 * still runs after its unlock. One that does not leave in time fails the stop,
 * which is undone (the transport works as before; a worker refused meanwhile
 * reloads its weights, a job submitted meanwhile fails once). 0, or VJO_ERR_BUSY. */
#define STOP_DRAIN_POLLS 200
int game_ocr_stop(void)
{
    int busy, woke;
    VJO_LOCK();
    busy = game_ocr_shutdown_locked();
    woke = waiters;
    VJO_UNLOCK();
    if (busy) return VJO_ERR_BUSY;
    for (int i = 0;; i++) {
        VJO_LOCK();
        busy = waiters;
        if (busy && i >= STOP_DRAIN_POLLS) stopping = 0;
        VJO_UNLOCK();
        if (!busy) break;
        if (i >= STOP_DRAIN_POLLS) {
            klog("module_stop: %d OCR waiter(s) still in the kernel after %d ms; stop undone", busy, STOP_DRAIN_POLLS);
            return VJO_ERR_BUSY;
        }
        ksceKernelDelayThread(1000);
    }
    if (woke) ksceKernelDelayThread(1000);
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
    /* Its rows call will never return to it: nothing may be copied into the
     * process any more, and an idle kernel strip goes. A copy in progress runs
     * in that process's own display call and ends by itself. */
    if (pid == rows.pid) {
        if (rows.in_call) {
            /* its rows call is still inside the kernel: it sees the process gone,
             * stops, and tidies up itself (nothing is freed under it) */
            rows.orphan = 1;
            rows.gone_us = ksceKernelGetSystemTimeWide();
        } else if (__sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_IDLE) ||
                   rows.state == ROWS_DONE) {
            rows.state = ROWS_IDLE;
            rows.orphan = rows.abandoned = 0;
            buffers_kstrip_free();
        } else if (rows.state == ROWS_COPYING) {
            rows.orphan = 1;
        }
        rows.sig_seq = 0;
    }
    if (pid == worker_pid) {
        worker_pid = worker_module = 0;
        worker_thread = refused_thread = 0;
        worker_epoch = 0;
    }
    VJO_UNLOCK();
}

/* API 23: a shot's card, 32 lowercase hex digits and a NUL (core/anki_queue.h
 * vjo_queue_valid_id): the file name the kernel makes, nothing that could name
 * another path. */
static int shot_id_ok(const char *id)
{
    for (unsigned i = 0; i < 32u; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return id[32] == 0;
}

int vjoOcrRegister(void)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid = ksceKernelGetProcessId(), thread = ksceKernelGetThreadId();
    int rc;
    ENTER_SYSCALL(state);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = taiGetModuleInfoForKernel(pid, VJO_GAME_OCR_MODULE, &info);
    if (rc < 0) { rc = VJO_ERR_PERM; goto out; }
    VJO_LOCK();
    sync_locked();
    if (stopping || pid <= 0 || pid != g.game_pid || !g.game_active) rc = VJO_ERR_NO_GAME;
    else if (pid == worker_pid && worker_thread != 0 && thread != worker_thread) {
        /* Another copy's thread in the registered game (#138): it stays idle. */
        if (thread != refused_thread)
            klog("ocr worker: second worker thread 0x%X in game pid 0x%X refused (registered: 0x%X)", thread, pid,
                 worker_thread);
        refused_thread = thread;
        rc = VJO_ERR_BUSY;
    }
    else if (claimed && (pid != worker_pid || worker_module != info.modid ||
                        worker_epoch != __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE)))
        rc = VJO_ERR_BUSY;
    else {
        /* The worker asks every 100 ms: only a new registration is logged, so kernel.txt
         * says which game process the worker loaded into. */
        if (pid != worker_pid || worker_module != info.modid)
            klog("ocr worker registered: game pid 0x%X module 0x%X", pid, info.modid);
        worker_pid = pid;
        worker_module = info.modid;
        worker_thread = thread;
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
    int rc, shot;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    if (!user_request || ksceKernelMemcpyUserToKernel(&next, user_request, sizeof(next)) < 0) {
        rc = VJO_ERR_COPY; goto out;
    }
    /* API 23: a shot names its card (32 lowercase hex digits) and nothing else; an OCR job its
     * models and no card. API 32: a shot's only speed bit is VJO_GAME_OCR_S_TRIM, an OCR job's never. */
    shot = (next.flags & VJO_GAME_OCR_F_SHOT) != 0;
    if (next.size != sizeof(next) || next.model[sizeof(next.model)-1] != 0 ||
        next.model_dir[sizeof(next.model_dir)-1] != 0 ||
        next.fallback[sizeof(next.fallback)-1] != 0 || (next.flags & ~VJO_GAME_OCR_F_ALL) ||
        (next.tiers & ~VJO_VOCR_TIERS_ALL) || (next.speed & ~(shot ? VJO_GAME_OCR_S_SHOT_ALL : VJO_GAME_OCR_S_ALL)) ||
        /* API 47: the vertical field's 3 names no mode, as the game worker refuses it */
        ((next.speed >> VJO_GAME_OCR_S_VERTICAL_SHIFT) & VJO_GAME_OCR_S_VERTICAL_MASK) > VJO_GAME_OCR_VERTICAL_AUTO ||
        /* API 31: an OCR job's hook pick is automatic, a stream or OCR only; a shot answers from no text */
        (shot ? !shot_id_ok(next.shot) || (next.flags & ~(VJO_GAME_OCR_F_SHOT | VJO_GAME_OCR_F_FULL)) ||
                    next.tiers || next.hook_pick
              : !next.model[0] || !next.model_dir[0] || next.shot[0] || !VJO_HOOK_PICK_OK(next.hook_pick))) {
        rc = VJO_ERR_ARG; goto out;
    }
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (pending || claimed || model_io.seq || rows.state != ROWS_IDLE || shot_spare_preparing) rc = VJO_ERR_BUSY;
    else if (!registration_current(worker_pid)) rc = VJO_ERR_NO_GAME;
    else if (sequence == 0x7FFFFFFFu) rc = VJO_ERR_BUSY; /* Never recycle a sequence. */
    else {
        /* The region in the game's last frame. Every row request reads that
         * geometry; a frame of another size fails it (VJO_ERR_FORMAT). */
        uint32_t fw = g.fb_w ? g.fb_w : VJO_MAX_W, fh = g.fb_h ? g.fb_h : VJO_MAX_H;
        if (fw > VJO_MAX_W) fw = VJO_MAX_W;
        if (fh > VJO_MAX_H) fh = VJO_MAX_H;
        if (next.flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_SHOT)) memset(&next.region, 0, sizeof(next.region));
        else next.region = g.region;
        capture_crop_of(&next.region, fw, fh, &next.x, &next.y, &next.width, &next.height);
        next.fb_width = fw;
        next.fb_height = fh;
        next.seq = ++sequence;
        /* The capture this job takes: VjoState.done_seq reaches it with the first pass. */
        g.capture_seq = (g.capture_seq + 1) & 0x7FFFFFFF;
        if (!g.capture_seq) g.capture_seq = 1;
        next.capture_seq = g.capture_seq;
        request = next;
        request_pid = worker_pid;
        request_shell = pid;
        request_epoch = __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
        memset(&result, 0, sizeof(result));
        valid = pending = 1;
        claimed = cancelled = completed = 0;
        rc = (int)request.seq;
        if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_JOB); /* a waiting vjoOcrTake claims it now */
        klog_d("ocr job %u: %ux%u at %u,%u of %ux%u, capture %u, flags 0x%X, tiers 0x%X, %s / %s", request.seq,
             request.width, request.height, request.x, request.y, fw, fh, request.capture_seq,
             request.flags, request.tiers, request.model, request.fallback[0] ? request.fallback : "-");
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

/* ocr-speed: the game's frame pacing while an OCR job runs (from its claim,
 * vjoOcrTake, to its completion) and over the frames since the job before, as
 * one more step of the job's result (VJO_GSTEP_PACE) when it has room. The
 * display hook updates it in the game's display call and the worker's syscalls
 * switch the window, without a lock: a diagnostic (a frame on the switch counts
 * in either window). */
typedef struct {
    uint32_t frames, sum_us, max_us, slow; /* slow: gaps over VJO_PACE_SLOW_US */
} Pace;
static Pace pace[2]; /* 0 between jobs, 1 during one */
#if VJO_DEBUG
static uint32_t pace_last_us; /* the low 32 bits: gaps are differences, wrap included */
#endif
static volatile int pace_job;

void game_ocr_frame(int64_t now_us)
{
#if !VJO_DEBUG
    (void)now_us; /* the normal build measures no pacing (docs/release-variants.md) */
}
#else
    Pace *pc = &pace[pace_job & 1];
    uint32_t now = (uint32_t)now_us, gap = now - pace_last_us;
    if (pace_last_us) {
        pc->frames++;
        pc->sum_us += gap;
        if (gap > pc->max_us) pc->max_us = gap;
        if (gap > VJO_PACE_SLOW_US) pc->slow++;
    }
    pace_last_us = now ? now : 1u;
}
#endif

int vjoOcrTake(VjoGameOcrRequest *out)
{
    uint32_t state, wait_us = 0;
    SceUID pid = ksceKernelGetProcessId();
    int rc, waited = 0;
    ENTER_SYSCALL(state);
    /* API 49: a shorter wait the caller asks for in out->seq (the trusted hook watch's pace) */
    if (!out || ksceKernelMemcpyUserToKernel(&wait_us, &out->seq, sizeof(wait_us)) < 0 || !wait_us ||
        wait_us > TAKE_WAIT_US)
        wait_us = TAKE_WAIT_US;
    else if (wait_us < VJO_GAME_OCR_TAKE_WAIT_MIN_US)
        wait_us = VJO_GAME_OCR_TAKE_WAIT_MIN_US;
    for (;;) {
        int wait;
        VJO_LOCK();
        if (waited) waiters--;
        sync_locked();
        if (!registration_current(pid)) rc = VJO_ERR_PERM;
        else if (claimed) rc = VJO_ERR_BUSY;
        else if (!pending || !valid || cancelled) rc = 1;
        else if (!out || ksceKernelMemcpyKernelToUser(out, &request, sizeof(request)) < 0)
            rc = VJO_ERR_COPY;
        else {
            pending = 0; claimed = 1; rc = 0; pace[1] = (Pace){0}; pace_job = 1;
            /* #194: a shot's stages, with times, so a stalled one says where (its steps never reach the
             * Shell once it gives up) */
            if (request.flags & VJO_GAME_OCR_F_SHOT) klog_d("ocr job %u: shot taken t=%u ms", request.seq, klog_ms());
        }
        /* Nothing queued: wait once. The bit is cleared under the lock, so a
         * vjoOcrSubmit after this point wakes the wait. */
        wait = rc == 1 && !waited && g.mevf > 0;
        if (wait) {
            ksceKernelClearEventFlag(g.mevf, ~MEV_JOB);
            waiters++;
        }
        VJO_UNLOCK();
        if (!wait) break;
        waited = 1;
        event_wait(MEV_JOB, wait_us);
    }
    EXIT_SYSCALL(state);
    return rc;
}

/* Display hook, game context, no lock: the armed rows of this process from
 * this frame (see the rows state machine above). API 55, a frame copy
 * (VJO_ROWS_FRAME, docs/game-pause.md): every row of the region from this one
 * frame, VJO_GAME_OCR_PAUSE_ROWS at a time, the clock looked at between them, so
 * the game's display call waits at most VJO_GAME_OCR_PAUSE_MAX_US and a chunk:
 * that wait is the game's pause, and it ends when this function returns, on
 * every path (the copy done, its cap, a copy error, another frame size). */
void game_ocr_rows_hook(SceUID pid, uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    int64_t start;
    if (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) != ROWS_PENDING || pid != rows.pid)
        return;
    start = ksceKernelGetSystemTimeWide();
    /* the watchdog's: written before the claim, so a COPYING it sees has it (one hook caller at a time:
     * display.c frame_busy) */
    if (rows.job.frame) __atomic_store_n(&rows.frame_start, (uint32_t)start | 1u, __ATOMIC_RELAXED);
    if (!__sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_COPYING)) {
        __atomic_store_n(&rows.frame_start, 0u, __ATOMIC_RELAXED);
        return;
    }
    rows.copy_start_us = start;
    rows.fb_pitch = pitch;
    rows.fb_fmt = fmt;
    rows.fb_w = w;
    rows.fb_h = h;
    if (!rows.job.frame) {
        rows.rc = capture_grey_rows(base, pitch, fmt, w, h, &rows.job);
    } else {
        OcrRowsJob part = rows.job;
        int rc = 0;
        for (uint32_t done = 0; done < rows.job.n; done += part.n) {
            if (done && (uint64_t)(ksceKernelGetSystemTimeWide() - start) >= VJO_GAME_OCR_PAUSE_MAX_US) {
                rc = VJO_ERR_TIMEOUT; /* the cap: the game goes on; the job reads as before */
                break;
            }
            part.y = rows.job.y + done;
            part.n = rows.job.n - done < VJO_GAME_OCR_PAUSE_ROWS ? rows.job.n - done : VJO_GAME_OCR_PAUSE_ROWS;
            part.dst_user = rows.job.dst_user + (uintptr_t)done * rows.job.stride;
            if ((rc = capture_grey_rows(base, pitch, fmt, w, h, &part)) < 0)
                break;
        }
        rows.rc = rc < 0 ? rc : (int)rows.job.n;
    }
    rows.copy_end_us = ksceKernelGetSystemTimeWide();
    __atomic_store_n(&rows.frame_start, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&rows.state, ROWS_DONE, __ATOMIC_RELEASE);
    if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_ROWS);
}

/* The first pass of the job's capture is in (VJO_ROWS_LAST): its signature
 * against the current screen, published in VjoState (capture_scene, region_seq,
 * done_seq), so the Shell can tell whether the job's text is for the screen it
 * shows now. */
static void publish_capture_locked(void)
{
    /* API 41: a capture begun since the hook's newest line (vjoOcrHookLine) is that line's scene */
    g.capture_scene = scene_capture(&g.tracker, &g.capture_sig, g.capture_begin_us);
    g.crop_w = request.width;
    g.crop_h = request.height;
    g.region_seq = request.capture_seq;
    g.region_us = rows.copy_start_us;
    g.done_seq = request.capture_seq;
}

/* The caller's memory holds [dst, dst + bytes): one block of process pid. */
static int user_range_ok(SceUID pid, uintptr_t dst, uint32_t bytes)
{
    SceUID first, last;
    if (!dst || !bytes || dst + bytes < dst) return 0;
    first = ksceKernelFindProcMemBlockByAddr(pid, (const void *)dst, 0);
    last = ksceKernelFindProcMemBlockByAddr(pid, (const void *)(dst + bytes - 1u), 0);
    return first >= 0 && first == last;
}

int vjoOcrCaptureRows(const VjoGameOcrRows *user_req, void *dst, VjoGameOcrRowsInfo *user_info)
{
    uint32_t state;
    VjoGameOcrRows req;
    VjoGameOcrRowsInfo info;
    SceUID pid = ksceKernelGetProcessId();
    uint64_t deadline;
    uint32_t span = 0, kbytes = 0, bpp;
    int rc = 0, armed = 0, kalloc = 0, rgba, frame;
    ENTER_SYSCALL(state);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!user_req || ksceKernelMemcpyUserToKernel(&req, user_req, sizeof(req)) < 0) {
        rc = VJO_ERR_COPY; goto out;
    }
    /* API 23: a shot's rows are in colour (4 bytes a pixel), an OCR job's grey; a shot
     * publishes no capture. */
    rgba = (req.flags & VJO_ROWS_RGBA) != 0;
    bpp = rgba ? 4u : 1u;
    frame = (req.flags & VJO_ROWS_FRAME) != 0; /* API 55: the whole region from one frame (the pause) */
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed || req.seq != request.seq || req.size != sizeof(req)) rc = VJO_ERR_ARG;
    else if (cancelled || !registration_current(pid)) rc = VJO_ERR_CANCELLED;
    else if (req.flags & VJO_ROWS_HOOK) {
        /* API 49: a trusted answer's capture, without pixels: the trusted line's scene, while the region shows
         * it (the tracker keeps a hooked line's number until the region shows another screen). API 51: once the
         * region shows another (a menu over the line, a larger change after it drew), the region's current
         * scene. The answer is the trusted hook's last line whatever the screen shows, so the first pass API 49
         * read there decided nothing and cost the game a strip beside a 1 MiB reserve, which a game with 1 MiB
         * free refused. No signature: region_scene never matches it, so the result stands for that scene alone.
         * Only a region without a screen yet (scene_reset, no trusted line since) is refused, and the worker
         * reads the pass. */
        const int line = g.hook_scene && g.hook_scene == g.tracker.id && g.tracker.hook_us;
        if (req.flags != VJO_ROWS_HOOK || req.n || req.y || dst || req.dst_stride ||
            (request.flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_PROBE | VJO_GAME_OCR_F_SHOT)))
            rc = VJO_ERR_ARG;
        else if (!line && (!g.tracker.id || !g.tracker.ref.w))
            rc = VJO_ERR_FORMAT;
        else {
            span = g.capture_scene = g.tracker.id; /* span: the scene, for the log line */
            g.crop_w = request.width;
            g.crop_h = request.height;
            g.capture_sig.w = 0;
            g.region_seq = request.capture_seq;
            g.region_us = ksceKernelGetSystemTimeWide();
            g.done_seq = request.capture_seq;
            rc = 1; /* published: the event below */
        }
        VJO_UNLOCK();
        if (rc == 1) {
            ksceKernelSetEventFlag(g.evf, VJO_EV_CAPTURE_DONE);
            klog_d("ocr job %u: trusted hook's capture, scene %u%s", req.seq, span, line ? "" : ", the region's");
            rc = 0;
        }
        goto out; /* no rows: the info says none were armed */
    }
    else if (frame && (req.flags != (VJO_ROWS_SIG | VJO_ROWS_LAST | VJO_ROWS_FRAME) || req.y || req.n != request.height ||
                       req.mode != VJO_GAME_OCR_SRC_HOOK || req.dst_stride < request.width || req.dst_stride > VJO_MAX_W ||
                       !(request.speed & VJO_GAME_OCR_S_PAUSE) ||
                       (request.flags & (VJO_GAME_OCR_F_SHOT | VJO_GAME_OCR_F_PROBE))))
        rc = VJO_ERR_ARG; /* API 55: the whole region at once, the first pass, only for a job that asked for the pause */
    else if (frame && (pause_off || g.game_active == VJO_GAME_STATIC_FB))
        rc = VJO_ERR_UNSUPPORTED; /* its frames come from pad calls (no display call to wait in), or the watchdog's */
    else if (!frame && (!req.n || req.n > VJO_GAME_OCR_ROWS_MAX || req.y >= request.height ||
                        req.n > request.height - req.y ||
                        req.dst_stride < request.width * bpp || req.dst_stride > VJO_MAX_W * bpp ||
                        (req.flags & ~(VJO_ROWS_SIG | VJO_ROWS_LAST | VJO_ROWS_RGBA)) ||
                        ((req.flags & VJO_ROWS_LAST) && !(req.flags & VJO_ROWS_SIG)) ||
                        rgba != !!(request.flags & VJO_GAME_OCR_F_SHOT) || (rgba && (req.flags & VJO_ROWS_SIG)) ||
                        (req.mode != VJO_GAME_OCR_SRC_HOOK && req.mode != VJO_GAME_OCR_SRC_KSTRIP)))
        rc = VJO_ERR_ARG;
    else if ((req.flags & VJO_ROWS_SIG) && !(req.y == 0 || (rows.sig_seq == req.seq && req.y == rows.sig_next)))
        rc = VJO_ERR_ARG; /* the first pass runs from row 0, in order */
    /* A previous copy (also one whose process exited, or that outlived its call)
     * is not tidied yet: the kernel worker returns it to IDLE within a tick. */
    else if (rows.state != ROWS_IDLE) rc = VJO_ERR_BUSY;
    else {
        /* <= 61,440 B grey, 245,760 B in colour (64 rows of 960 px), 522,240 B for a frame (544 rows): no wrap */
        uint64_t bytes = (uint64_t)(req.n - 1u) * req.dst_stride + request.width * bpp;
        span = (uint32_t)bytes;
        if (bytes > 0xFFFFFFFFu || !user_range_ok(pid, (uintptr_t)dst, span)) rc = VJO_ERR_ARG;
    }
    if (!rc && req.mode == VJO_GAME_OCR_SRC_KSTRIP) {
        kbytes = req.n * request.width * bpp; /* over VJO_GAME_OCR_KSTRIP_MAX: refused (ARG) */
        rc = buffers_kstrip_alloc(kbytes);
        info.kstrip_rc = rc;
        if (!rc) kalloc = 1;
    }
    if (!rc) {
        memset(&rows.job, 0, sizeof(rows.job));
        rows.job.x0 = request.x;
        rows.job.y0 = request.y;
        rows.job.w = request.width;
        rows.job.h = request.height;
        rows.job.fb_w = request.fb_width;
        rows.job.fb_h = request.fb_height;
        rows.job.y = req.y;
        rows.job.n = req.n;
        if (kalloc) {
            rows.job.dst_kernel = buffers_kstrip();
            rows.job.stride = request.width * bpp;
        } else {
            rows.job.dst_user = (uintptr_t)dst;
            rows.job.stride = req.dst_stride;
        }
        rows.job.sig = (req.flags & VJO_ROWS_SIG) != 0;
        rows.job.rgba = rgba;
        rows.job.frame = frame;
        if (rows.job.sig && req.y == 0) {
            g.region_seq = 0; /* capture_sig gets rewritten */
            g.capture_begin_us = ksceKernelGetSystemTimeWide();
            capture_sig_begin(request.width, request.height);
            rows.sig_seq = req.seq;
        }
        rows.pid = pid;
        rows.rc = 0;
        rows.orphan = rows.abandoned = 0;
        rows.in_call = 1;
        rows.fb_w = rows.fb_h = rows.fb_pitch = rows.fb_fmt = 0;
        rows.copy_start_us = rows.copy_end_us = 0;
        rows.armed_us = ksceKernelGetSystemTimeWide();
        ksceKernelClearEventFlag(g.mevf, ~MEV_ROWS);
        __atomic_store_n(&rows.state, ROWS_PENDING, __ATOMIC_RELEASE);
        armed = 1;
    }
    VJO_UNLOCK();
    if (!armed) goto out;

    /* The next frame the game shows copies the rows (display.c). Wait for it
     * without the lock, waking every slice to see whether the job was
     * cancelled. */
    deadline = (uint64_t)ksceKernelGetSystemTimeWide() + ROWS_WAIT_US;
    for (;;) {
        unsigned int bits = 0;
        SceUInt timeout = ROWS_SLICE_US;
        int stop;
        if (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) == ROWS_DONE) break;
        if ((uint64_t)ksceKernelGetSystemTimeWide() >= deadline) break;
        ksceKernelWaitEventFlag(g.mevf, MEV_ROWS, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
        VJO_LOCK();
        sync_locked();
        stop = cancelled || !registration_current(pid);
        VJO_UNLOCK();
        if (stop) break;
    }
    /* Give up a copy that never started; let one that did finish. */
    if (__sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_IDLE)) {
        VJO_LOCK();
        sync_locked();
        rc = cancelled || !registration_current(pid) ? VJO_ERR_CANCELLED : VJO_ERR_TIMEOUT;
        VJO_UNLOCK();
    } else {
        uint64_t copy_deadline = (uint64_t)ksceKernelGetSystemTimeWide() + ROWS_COPY_WAIT_US;
        while (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) == ROWS_COPYING &&
               (uint64_t)ksceKernelGetSystemTimeWide() < copy_deadline)
            ksceKernelDelayThread(1000);
        if (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) != ROWS_DONE) {
            /* Never seen: the hook still owns dst (and a kernel strip). The caller must
             * not free dst; the kernel worker frees the strip and returns the state to
             * IDLE once the copy ends, so a later rows call is not refused. */
            klog("ocr rows: copy still running after %u us", ROWS_COPY_WAIT_US);
            VJO_LOCK();
            rows.abandoned = 1;
            rows.in_call = 0;
            model_wake_locked();
            VJO_UNLOCK();
            rc = VJO_ERR_BUSY;
            goto info_out;
        }
        rc = rows.rc;
    }
    VJO_LOCK();
    /* The copy is over (DONE) or never started (IDLE): this call's last use of
     * the state and the strip, then both are free for the next request. */
    if (rows.state == ROWS_DONE) rows.state = ROWS_IDLE;
    rows.orphan = rows.abandoned = 0;
    if (rc > 0 && kalloc && !buffers_kstrip()) rc = VJO_ERR_NO_MEMORY; /* never: freed only by this call */
    if (rc > 0 && kalloc) {
        /* Last rung of the ladder: the hook filled the kernel strip; copy it out here. */
        const uint8_t *k = buffers_kstrip();
        for (uint32_t r = 0; r < req.n && rc > 0; r++)
            if (ksceKernelMemcpyKernelToUser((uint8_t *)dst + r * req.dst_stride, k + r * request.width * bpp,
                                             request.width * bpp) < 0)
                rc = VJO_ERR_COPY;
    }
    if (kalloc) buffers_kstrip_free();
    rows.in_call = 0;
    if (rc > 0 && (req.flags & VJO_ROWS_SIG)) {
        rows.sig_next = req.y + req.n;
        /* Only a region job's capture is the region's: a full-frame or probe
         * pass (the self-test's) must not stand in for the Shell's region. */
        if ((req.flags & VJO_ROWS_LAST) && valid && claimed && !cancelled && req.seq == request.seq &&
            !(request.flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_PROBE))) {
            publish_capture_locked();
            VJO_UNLOCK();
            ksceKernelSetEventFlag(g.evf, VJO_EV_CAPTURE_DONE);
            klog_d("ocr job %u: first pass in (%u rows, frame %ux%u fmt %08X pitch %u), scene %u",
                 req.seq, request.height, rows.fb_w, rows.fb_h, rows.fb_fmt, rows.fb_pitch, g.capture_scene);
            VJO_LOCK();
        }
    }
    VJO_UNLOCK();
    if (rc < 0)
        klog("ocr rows %u..%u (%s) -> 0x%08X", req.y, req.y + req.n,
             req.mode == VJO_GAME_OCR_SRC_KSTRIP ? "kernel strip" : "hook", (unsigned)rc);
info_out:
    info.fb_width = rows.fb_w;
    info.fb_height = rows.fb_h;
    info.fb_pitch = rows.fb_pitch;
    info.fb_format = rows.fb_fmt;
    info.wait_us = (uint32_t)(ksceKernelGetSystemTimeWide() - rows.armed_us);
    info.copy_us = rows.copy_end_us > rows.copy_start_us ? (uint32_t)(rows.copy_end_us - rows.copy_start_us) : 0;
    info.kstrip_bytes = kalloc ? kbytes : 0;
    if (rgba && rc > 0)
        klog_d("ocr job %u: shot rows %u..%u (%s), waited %u us, copy %u us, t=%u ms", req.seq, req.y, req.y + req.n,
               req.mode == VJO_GAME_OCR_SRC_KSTRIP ? "kernel strip" : "hook", info.wait_us, info.copy_us, klog_ms());
    if (frame) /* API 55: how long the game's display call waited (the pause) */
        klog_d("ocr job %u: the frame's %u rows in one display call, %u us (rc 0x%08X)", req.seq, req.n, info.copy_us,
               (unsigned)rc);
out:
    info.rc = rc;
    info.armed = (uint32_t)armed;
    if (user_info) {
        uint32_t size = 0;
        if (ksceKernelMemcpyUserToKernel(&size, user_info, sizeof(size)) >= 0 && size == sizeof(info)) {
            VJO_LOCK(); /* every copy out of a syscall is made under the lock */
            ksceKernelMemcpyKernelToUser(user_info, &info, sizeof(info));
            VJO_UNLOCK();
        }
    }
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
        if (request.flags & VJO_GAME_OCR_F_SHOT)
            klog_d("ocr job %u: shot result rc %d, %u B, %u steps%s t=%u ms", request.seq, incoming.rc,
                   incoming.shot_bytes, incoming.n_steps, cancelled ? " (cancelled)" : "", klog_ms());
        if (!cancelled) {
            result = incoming;
            completed = 1;
            if (VJO_DEBUG && pace_job && result.n_steps < VJO_GAME_OCR_STEPS) {
                VjoGameOcrStep *p = &result.step[result.n_steps++];
                p->id = VJO_GSTEP_PACE;
                p->kind = 0;
                p->arg = (uint16_t)(pace[1].frames > 0xFFFFu ? 0xFFFFu : pace[1].frames);
                p->rc = (int32_t)pace[1].max_us;
                p->bytes = pace[1].sum_us;
                p->sum = pace[1].slow;
                p->us = pace[0].frames;
                p->free_main = (int32_t)(pace[0].sum_us > 0x7FFFFFFFu ? 0x7FFFFFFFu : pace[0].sum_us);
                p->free_cdram = (int32_t)pace[0].max_us;
                p->free_phycont = (int32_t)pace[0].slow;
            }
            if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_RESULT); /* a waiting vjoOcrRead returns it now */
        }
        pace[0] = (Pace){0};
        pace_job = 0;
        if (!incoming.cleanup_status) claimed = 0;
#if !VJO_DEBUG
        if (!incoming.cleanup_status) act_mem_note_locked(&incoming); /* once a job: as its claim is released */
#endif
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
    int rc, waited = 0;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    for (;;) {
        int wait;
        VJO_LOCK();
        if (waited) waiters--;
        sync_locked();
        if (!shell_caller(pid)) rc = VJO_ERR_PERM;
        else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
        else if (cancelled) rc = VJO_ERR_CANCELLED;
        else if (!completed) {
            /* A screenshot is frozen once its first encoded JPEG chunk is
             * accepted. Pending Read can then release Shell's capture hold.
             * Submit cleared this result, so it exposes no stale text. */
            if ((request.flags & VJO_GAME_OCR_F_SHOT) && result.shot_bytes && out &&
                ksceKernelMemcpyKernelToUser(out, &result, sizeof(result)) < 0) rc = VJO_ERR_COPY;
            else rc = 1;
        }
        else if (!out || ksceKernelMemcpyKernelToUser(out, &result, sizeof(result)) < 0)
            rc = VJO_ERR_COPY;
        else rc = 0;
        wait = rc == 1 && !waited && g.mevf > 0; /* as vjoOcrTake */
        if (wait) {
            ksceKernelClearEventFlag(g.mevf, ~MEV_RESULT);
            waiters++;
        }
        VJO_UNLOCK();
        if (!wait) break;
        waited = 1;
        event_wait(MEV_RESULT, READ_WAIT_US);
    }
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
    else {
        if (request.flags & VJO_GAME_OCR_F_SHOT)
            klog_d("ocr job %u: shot cancelled (%s) t=%u ms", seq, !claimed && pending ? "never taken"
                   : completed ? "completed" : model_io.seq ? "its file in the kernel" : "taken, no result", klog_ms());
        invalidate_locked();
        rc = 0;
    }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

static uint32_t model_size(const char *name)
{
    if (!strcmp(name, VJO_VOCR6_N_NAME)) return VJO_VOCR6_N_BYTES;
    if (!strcmp(name, VJO_VOCR6_S_NAME)) return VJO_VOCR6_S_BYTES;
    if (!strcmp(name, VJO_VOCR6_L_NAME)) return VJO_VOCR6_L_BYTES;
    if (!strcmp(name, VJO_VOCR3_H15_NAME)) return VJO_VOCR_H15_BYTES;
    if (!strcmp(name, VJO_VOCR3_FL10_NAME)) return VJO_VOCR_FL10_BYTES;
    if (!strcmp(name, "FL10_w8.vocr")) return VJO_VOCR_FL10_BYTES;
    if (!strcmp(name, "H15_w8.vocr")) return VJO_VOCR_H15_BYTES;
    if (!strcmp(name, "F20_w8.vocr")) return VJO_VOCR_F20_BYTES;
    if (!strcmp(name, VJO_VOCR4_W20_NAME)) return VJO_VOCR_CASCADE_BYTES;
    if (!strcmp(name, VJO_VOCR6_V_NAME)) return VJO_VOCR6_V_BYTES;
    return 0;
}

/* The pinned LM of a tier (1..VJO_VOCR_LM_TIERS): its name and length. */
static const char *lm_name(uint32_t tier)
{
    static const char *const names[VJO_VOCR_LM_TIERS] = {VJO_VOCR_LM1_NAME, VJO_VOCR_LM2_NAME, VJO_VOCR_LM3_NAME,
                                                          VJO_VOCR_LM4_NAME};
    return names[tier - 1u];
}

static uint32_t lm_size(uint32_t tier)
{
    static const uint32_t sizes[VJO_VOCR_LM_TIERS] = {VJO_VOCR_LM1_BYTES, VJO_VOCR_LM2_BYTES, VJO_VOCR_LM3_BYTES,
                                                      VJO_VOCR_LM4_BYTES};
    return sizes[tier - 1u];
}

/* API 21: the pinned model of a model tier (1..VJO_VOCR_TIERS), by its name. */
static const char *tier_name(uint32_t tier)
{
    static const char *const names[VJO_VOCR_TIERS] = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME};
    return names[tier - 1u];
}

/* Whether a request by tiers may open tier t: one of its tiers, or with a
 * cascade ceiling the tier above its largest (the cascade reads with the next
 * tier up of the job's). A request by name (tiers 0) opens none. */
static int tier_allowed(const VjoGameOcrRequest *r, uint32_t t)
{
    uint32_t top = 0;
    if (!r->tiers || !t || t > VJO_VOCR_TIERS) return 0;
    if (r->tiers & (1u << (t - 1u))) return 1;
    for (uint32_t i = 1; i <= VJO_VOCR_TIERS; i++)
        if (r->tiers & (1u << (i - 1u))) top = i;
    return t == top + 1u && ((r->flags >> VJO_GAME_OCR_CASCADE_SHIFT) & VJO_GAME_OCR_CASCADE_MASK);
}

static int model_caller_locked(SceUID pid, uint32_t seq, int cleanup)
{
    sync_locked();
    if (pid <= 0 || pid != request_pid) return VJO_ERR_PERM;
    if (!valid || !claimed || seq != request.seq) return VJO_ERR_ARG;
    if (!cleanup && (cancelled || !registration_current(pid))) return VJO_ERR_CANCELLED;
    return 0;
}

/* The kernel worker finished a model operation, or the time is up: waits
 * without g.lock until deadline. 0: there is nothing to wait on. */
static int model_wait(uint64_t deadline)
{
    uint64_t now = (uint64_t)ksceKernelGetSystemTimeWide();
    unsigned int bits = 0;
    SceUInt timeout;
    if (g.mevf <= 0 || now >= deadline) return 0;
    timeout = (SceUInt)(deadline - now);
    return ksceKernelWaitEventFlag(g.mevf, MEV_DONE, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                   &bits, &timeout) >= 0;
}

/* One step of a model syscall (the *_locked functions below), under g.lock: a, b and p are the syscall's
 * arguments after its seq, those it has (the others 0). */
typedef int (*ModelStep)(SceUID pid, uint32_t seq, uint32_t a, uint32_t b, void *p);

/* The six model syscalls (vjoOcrModelOpen/Read/Close, vjoOcrHookCache, vjoOcrShotWrite/Save): the caller's step
 * under g.lock, again each time the kernel worker finishes an operation while the step answers BUSY, for at most
 * MODEL_SYSCALL_WAIT_US (model_wait, without the lock); then BUSY, and the game yields and repeats the call. */
static int model_syscall_budget(ModelStep step, uint32_t seq, uint32_t a, uint32_t b, void *p, uint32_t budget)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    uint64_t deadline;
    int rc;
    ENTER_SYSCALL(state);
    deadline = (uint64_t)ksceKernelGetSystemTimeWide() + budget;
    do {
        VJO_LOCK();
        rc = step(pid, seq, a, b, p);
        VJO_UNLOCK();
    } while (rc == VJO_ERR_BUSY && model_wait(deadline));
    EXIT_SYSCALL(state);
    return rc;
}

static int model_syscall(ModelStep step, uint32_t seq, uint32_t a, uint32_t b, void *p)
{
    return model_syscall_budget(step, seq, a, b, p, MODEL_SYSCALL_WAIT_US);
}

/* Startup has no claimed OCR job. Authorize it separately using the loader's
 * ON table and safe-boot gate; ordinary model APIs keep their claimed-job checks. */
static int profile_allowed(void)
{
    unsigned word = (unsigned)vjoGetTextSource();
    return word == VJO_TEXT_SOURCE_AUTO || word == VJO_TEXT_SOURCE_HOOKS;
}
static int profile_locked(SceUID pid, uint32_t seq, uint32_t cap, uint32_t unused, void *dst)
{
    char title[32] = {0};
    int rc;
    (void)seq; (void)unused;
    if (model_io.seq && model_io.which == PROFILE_WHICH && model_io.pid == pid) {
        if (model_io.closing || model_io.orphan) return VJO_ERR_CANCELLED;
        if (!profile_allowed() || stopping) {
            model_io.orphan = 1; model_invalidate_locked();
            return VJO_ERR_CANCELLED;
        }
        if (model_io.phase != MODEL_DONE) return VJO_ERR_BUSY;
        rc = model_io.rc;
        if (model_io.fd >= 0) { /* a failed close: no usable profile until drained */
            model_io.orphan = 1; model_invalidate_locked();
            return rc < 0 ? rc : VJO_ERR_BUSY;
        }
        if (!dst || cap != VJO_PROFILE_BYTES) rc = VJO_ERR_ARG;
        else if (rc > 0 && ksceKernelMemcpyKernelToUser(dst, model_stage, (SceSize)rc) < 0) rc = VJO_ERR_COPY;
        model_reset_locked();
        return rc;
    }
    if (!profile_allowed() || pid <= 0 || stopping) return VJO_ERR_PERM;
    if (!dst || cap != VJO_PROFILE_BYTES) return VJO_ERR_ARG;
    sync_locked();
    if (model_io.seq || pending || claimed || rows.state != ROWS_IDLE || shot_spare_preparing) return VJO_ERR_BUSY;
    if (loader_title(pid, title, sizeof(title)) < 0 || !vjo_title_id_strict(title)) return VJO_ERR_ARG;
    model_io.seq = 1; /* reserves the transport; never a claimed-job operation */
    model_io.pid = pid; model_io.which = PROFILE_WHICH; model_io.operation = PROFILE_LOAD;
    model_io.phase = MODEL_QUEUED; model_io.size = VJO_PROFILE_BYTES;
    memcpy(model_io.path, title, 10); /* title only; the file worker fixes the path */
    model_wake_locked();
    return VJO_ERR_BUSY;
}
int vjoOcrHookProfile(void *dst, uint32_t cap)
{
    int rc = model_syscall_budget(profile_locked, 0, cap, 0, dst, 2000000u);
    if (rc == VJO_ERR_BUSY) {
        /* At the deadline, abandon only this caller's startup operation. A
         * later file completion retains no pointer and cannot copy to dst. */
        uint32_t state;
        ENTER_SYSCALL(state);
        VJO_LOCK();
        if (model_io.seq && model_io.which == PROFILE_WHICH && model_io.pid == ksceKernelGetProcessId()) {
            model_io.orphan = 1; model_invalidate_locked();
        }
        VJO_UNLOCK(); EXIT_SYSCALL(state);
        return VJO_ERR_TIMEOUT;
    }
    return rc;
}

/* SceDateTime as one number, each field masked to its range. */
static uint64_t pack_time(const SceDateTime *t)
{
    return ((uint64_t)(t->year & 0xFFFu) << 46) | ((uint64_t)(t->month & 0xFu) << 42) |
           ((uint64_t)(t->day & 0x1Fu) << 37) | ((uint64_t)(t->hour & 0x1Fu) << 32) |
           ((uint64_t)(t->minute & 0x3Fu) << 26) | ((uint64_t)(t->second & 0x3Fu) << 20) |
           (uint64_t)(t->microsecond & 0xFFFFFu);
}

static int model_info_out_locked(VjoGameOcrModelInfo *info)
{
    VjoGameOcrModelInfo out;
    if (!info) return 0;
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    out.bytes = model_io.size;
    out.stat_rc = model_io.stat_rc;
    out.mtime = model_io.mtime;
    return ksceKernelMemcpyKernelToUser(info, &out, sizeof(out)) < 0 ? VJO_ERR_COPY : 0;
}

static int model_open_locked(SceUID pid, uint32_t seq, uint32_t which, uint32_t b, void *info)
{
    int rc = model_caller_locked(pid, seq, 0);
    const char *name;
    if (rc) return rc;
    if (request.flags & VJO_GAME_OCR_F_SHOT) return VJO_ERR_ARG; /* a shot reads no model (API 23) */
    if (model_io.seq) {
        if (model_io.seq != seq || model_io.pid != pid) return VJO_ERR_BUSY;
        if (model_io.closing) return VJO_ERR_CANCELLED;
        if (model_io.which != which) return VJO_ERR_ARG; /* one file per open */
        if (model_io.phase == MODEL_DONE && model_io.operation == MODEL_OPEN) {
            rc = model_io.rc;
            model_io.phase = MODEL_IDLE;
        } else if (model_io.phase == MODEL_IDLE && model_io.fd >= 0) rc = (int)model_io.size;
        else return VJO_ERR_BUSY;
        /* A failed copy can be repeated: the open file still reports itself. */
        if (rc > 0 && model_info_out_locked(info)) rc = VJO_ERR_COPY;
        return rc;
    }
    /* A MAGES SC3 hook's charset (core/hook_sc3.h): the calling game's own, by the title ID of its process (the
     * caller names no path), at most VJO_GAME_OCR_SC3_BYTES; the file worker takes its size as it opens it. */
    if (which == VJO_GAME_OCR_OPEN_SC3) {
        char title[32];
        memset(title, 0, sizeof(title));
        if (loader_title(pid, title, sizeof(title)) < 0 || !vjo_title_id_strict(title)) return VJO_ERR_ARG;
        model_io.seq = seq;
        model_io.pid = pid;
        model_io.which = which;
        model_io.size = VJO_GAME_OCR_SC3_BYTES;
        memcpy(model_io.path, VJO_GAME_OCR_SC3_DIR, sizeof(VJO_GAME_OCR_SC3_DIR) - 1);
        memcpy(model_io.path + sizeof(VJO_GAME_OCR_SC3_DIR) - 1, title, 9);
        memcpy(model_io.path + sizeof(VJO_GAME_OCR_SC3_DIR) + 8, ".sc3", sizeof(".sc3"));
        model_io.operation = MODEL_OPEN;
        model_io.phase = MODEL_QUEUED;
        model_wake_locked();
        return VJO_ERR_BUSY;
    }
    /* Only the two names the Shell submitted for this job, each a pinned model,
     * or (API 20) a pinned LM up to the tier the request allows, or the
     * cascade's pinned model when the request gives it a time ceiling; or (API 21,
     * a request by tiers) the pinned model of one of its tiers, or of the tier
     * above them for the cascade, by its tier; or (API 47) the vertical model,
     * for a request with a VJO_GAME_OCR_S_VERTICAL mode (on or auto), by tiers or by name;
     * or (API 26, 54) the gate's or the spotter's pinned weights, for a request that asks for it. */
    uint32_t size, lm_tier = which >= VJO_GAME_OCR_OPEN_LM && which < VJO_GAME_OCR_OPEN_CASCADE
                             ? which - VJO_GAME_OCR_OPEN_LM + 1u : 0u;
    uint32_t model_tier = which >= VJO_GAME_OCR_OPEN_TIER && which < VJO_GAME_OCR_OPEN_TIER + VJO_VOCR_TIERS
                          ? which - VJO_GAME_OCR_OPEN_TIER + 1u : 0u;
    int gate = which == VJO_GAME_OCR_OPEN_GATE; /* API 26: the text-presence gate's weights, for its request only */
    int spot = which == VJO_GAME_OCR_OPEN_SPOT; /* API 54: the text spotter's weights, for its request only */
    if (gate) {
        if (!(request.speed & VJO_GAME_OCR_S_GATE)) return VJO_ERR_ARG;
        name = VJO_VOCR_GATE_NAME;
    } else if (spot) {
        if (!(request.speed & VJO_GAME_OCR_S_SPOT)) return VJO_ERR_ARG;
        name = VJO_VOCR_SPOT_NAME;
    } else if (lm_tier && lm_tier <= VJO_VOCR_LM_TIERS &&
        lm_tier <= ((request.flags >> VJO_GAME_OCR_LM_SHIFT) & VJO_GAME_OCR_LM_MASK)) name = lm_name(lm_tier);
    else if (model_tier && tier_allowed(&request, model_tier)) name = tier_name(model_tier);
    else if (which == VJO_GAME_OCR_OPEN_VERTICAL && (request.speed & VJO_GAME_OCR_S_VERTICAL))
        name = VJO_VOCR_VERTICAL_NAME;
    else if (request.tiers) return VJO_ERR_ARG; /* a request by tiers names no model of its own */
    else if (which == VJO_GAME_OCR_OPEN_MODEL) name = request.model;
    else if (which == VJO_GAME_OCR_OPEN_FALLBACK) name = request.fallback;
    else if (which == VJO_GAME_OCR_OPEN_CASCADE &&
             ((request.flags >> VJO_GAME_OCR_CASCADE_SHIFT) & VJO_GAME_OCR_CASCADE_MASK)) name = VJO_VOCR_CASCADE_NAME;
    else return VJO_ERR_ARG;
    /* API 47: the vertical model opens by its own open only (a request with a vertical mode), never by the name
     * a request gives as its model or fallback */
    if (which != VJO_GAME_OCR_OPEN_VERTICAL && !strcmp(name, VJO_VOCR_VERTICAL_NAME)) return VJO_ERR_ARG;
    size = gate ? VJO_VOCR_GATE_BYTES : spot ? VJO_VOCR_SPOT_BYTES : lm_tier ? lm_size(lm_tier) : model_size(name);
    if (!size || strcmp(request.model_dir, VJO_VOCR_MODEL_ROOT)) return VJO_ERR_ARG;
    model_io.seq = seq;
    model_io.pid = pid;
    model_io.which = which;
    model_io.size = size;
    memcpy(model_io.path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT));
    /* Both request strings are bounded and NUL-validated by Submit; a pinned LM
     * name is a shorter literal: copy up to the NUL only. */
    {
        uint32_t n = 0;
        while (n + 1u < VJO_GAME_OCR_MODEL_BYTES && name[n]) n++;
        memcpy(model_io.path + sizeof(VJO_VOCR_MODEL_ROOT), name, n);
        model_io.path[sizeof(VJO_VOCR_MODEL_ROOT) + n] = '\0';
    }
    model_io.operation = MODEL_OPEN;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    return model_syscall(model_open_locked, seq, which, 0, info);
}

static int model_read_locked(SceUID pid, uint32_t seq, uint32_t offset, uint32_t bytes, void *dst)
{
    int rc = model_caller_locked(pid, seq, 0);
    if (rc) return rc;
    if (model_io.seq != seq || model_io.pid != pid || model_io.fd < 0 || model_io.shot ||
        !dst || !bytes || bytes > sizeof(model_stage) || offset > model_io.size ||
        bytes > model_io.size - offset) return VJO_ERR_ARG;
    if (model_io.closing) return VJO_ERR_CANCELLED;
    if (model_io.phase != MODEL_IDLE) {
        if (model_io.operation != MODEL_READ || offset != model_io.offset || bytes != model_io.bytes)
            return VJO_ERR_ARG;
        if (model_io.phase != MODEL_DONE) return VJO_ERR_BUSY;
        if (model_io.rc > 0 &&
            ksceKernelMemcpyKernelToUser(dst, model_stage, (SceSize)model_io.rc) < 0)
            return VJO_ERR_COPY; /* The exact completed chunk remains retryable. */
        model_io.phase = MODEL_IDLE;
        return model_io.rc;
    }
    model_io.offset = offset;
    model_io.bytes = bytes;
    model_io.operation = MODEL_READ;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t bytes, void *dst)
{
    return model_syscall(model_read_locked, seq, offset, bytes, dst);
}

static int model_close_locked(SceUID pid, uint32_t seq, uint32_t a, uint32_t b, void *p)
{
    int rc = model_caller_locked(pid, seq, 1);
    if (rc) return rc;
    if (!model_io.seq) return 0;
    if (model_io.seq != seq || model_io.pid != pid) return VJO_ERR_ARG;
    if (model_io.phase == MODEL_DONE && model_io.operation == MODEL_CLOSE && model_io.rc < 0) {
        model_io.phase = MODEL_IDLE; /* Retain FD; a subsequent Close retries. */
        return model_io.rc;
    }
    if (!model_io.closing || model_io.phase == MODEL_IDLE) {
        model_io.closing = 1;
        model_io.retry_us = 0; /* Explicit cleanup retry need not wait for orphan backoff. */
        model_wake_locked();
    }
    return VJO_ERR_BUSY;
}

int vjoOcrModelClose(uint32_t seq)
{
    return model_syscall(model_close_locked, seq, 0, 0, NULL);
}

static int hook_cache_locked(SceUID pid, uint32_t seq, uint32_t operation, uint32_t b, void *data)
{
    int rc = model_caller_locked(pid, seq, 0);
    if (rc) return rc;
    if (!data || operation > VJO_GAME_OCR_HOOK_CACHE_SAVE ||
        (request.flags & (VJO_GAME_OCR_F_SHOT | VJO_GAME_OCR_F_PROBE))) return VJO_ERR_ARG;
    if (model_io.seq) {
        if (model_io.seq != seq || model_io.pid != pid || model_io.which != HOOK_CACHE_WHICH ||
            model_io.operation != (operation ? HOOK_CACHE_SAVE : HOOK_CACHE_LOAD)) return VJO_ERR_ARG;
        if (model_io.closing) return VJO_ERR_CANCELLED;
        if (model_io.phase != MODEL_DONE) return VJO_ERR_BUSY;
        rc = model_io.rc;
        if (!operation && rc > 0 && ksceKernelMemcpyKernelToUser(data, model_stage, (SceSize)rc) < 0)
            return VJO_ERR_COPY; /* the completed read stays retryable */
        if (model_io.fd < 0) model_reset_locked();
        else model_io.phase = MODEL_IDLE; /* failed close: ModelClose still owns it */
        return rc;
    }
    if (operation && ksceKernelMemcpyUserToKernel(model_stage, data, VJO_GAME_OCR_HOOK_CACHE_BYTES) < 0)
        return VJO_ERR_COPY;
    model_reset_locked();
    model_io.seq = seq;
    model_io.pid = pid;
    model_io.which = HOOK_CACHE_WHICH;
    model_io.operation = operation ? HOOK_CACHE_SAVE : HOOK_CACHE_LOAD;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

/* API 41: the registered worker's loop, between jobs, saw the dialogue's hook stream take a new line
 * (game_ocr/main.c hooks_watch): the region gets a new scene now, held while the line draws
 * (kernel/scene.h scene_hook), the last capture stops standing for the screen (region_seq 0), and the
 * Shell is woken as for a quiet screen, so its background read starts at once instead of once the
 * pixels settle (0). A line reported while the last one is still drawing (lines in quick succession: a
 * skip, clicks through the typing) wakes no one (1): it is read once the region settles, as without the
 * watch, so a skip reads no line it passes. Never during a job: a capture begun since is the line's
 * own. seq and bytes (the stream's sequence and raw length) are only logged. API 49: a trusted source's
 * line (VJO_HOOK_LINE_TRUSTED in bytes) is read at once, drawing or not: its text needs no pixels, and its
 * scene is g.hook_scene (VjoState.hook_scene, VJO_ROWS_HOOK). */
int vjoOcrHookLine(uint32_t seq, uint32_t bytes)
{
    uint32_t state, scene = 0, trusted = bytes & VJO_HOOK_LINE_TRUSTED;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!registration_current(pid) || ksceKernelGetThreadId() != worker_thread) rc = VJO_ERR_PERM;
    else if (claimed) rc = VJO_ERR_BUSY;
    else {
        rc = scene_hook_drawing(&g.tracker) && !trusted ? 1 : 0;
        scene_hook(&g.tracker, ksceKernelGetSystemTimeWide());
        g.region_seq = 0;
        scene = g.tracker.id;
        g.hook_scene = trusted ? scene : 0;
    }
    VJO_UNLOCK();
    if (!rc) ksceKernelSetEventFlag(g.evf, VJO_EV_REGION_QUIET);
    if (rc >= 0)
        klog_d("hook line: seq %u, %u bytes, scene %u%s%s", seq, bytes & ~VJO_HOOK_LINE_TRUSTED, scene,
               trusted ? ", trusted" : "", rc ? ", no wakeup" : "");
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrHookCache(uint32_t seq, uint32_t operation, void *data)
{
    return model_syscall(hook_cache_locked, seq, operation, 0, data);
}

/* File-worker only. Reuse the model staging buffer, without a kernel heap, thread or user pointer.
 * Vita rename does not replace a destination: retain the previous file under .bak until the next save.
 * There is deliberately no device sync here (#194); interrupted or corrupt data means fresh learning. */
static int hook_cache_file(int operation, SceUID *fd)
{
    const int save = operation == HOOK_CACHE_SAVE;
    int rc, n, moved = 0;
    uint32_t done = 0;
    *fd = ksceIoOpen(save ? HOOK_CACHE_TEMP : HOOK_CACHE_PATH,
                     save ? SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC : SCE_O_RDONLY, 0666);
    if (*fd < 0 && !save && *fd == (int)0x80010002u)
        *fd = ksceIoOpen(HOOK_CACHE_BACKUP, SCE_O_RDONLY, 0);
    if (*fd < 0) return !save && *fd == (int)0x80010002u ? 0 : *fd;
    rc = 0;
    if (!save) {
        SceOff size = ksceIoLseek(*fd, 0, SCE_SEEK_END);
        if (size != VJO_GAME_OCR_HOOK_CACHE_BYTES) rc = size < 0 ? (int)size : VJO_ERR_FORMAT;
    }
    while (!rc && done < VJO_GAME_OCR_HOOK_CACHE_BYTES) {
        uint32_t left = VJO_GAME_OCR_HOOK_CACHE_BYTES - done;
        n = save ? ksceIoPwrite(*fd, model_stage + done, left, done)
                 : ksceIoPread(*fd, model_stage + done, left, done);
        if (n <= 0 || (uint32_t)n > left) rc = n < 0 ? n : VJO_ERR_FORMAT;
        else done += (uint32_t)n;
    }
    n = ksceIoClose(*fd);
    if (n >= 0) *fd = -1;
    else if (!rc) rc = n;
    if (save && *fd < 0) {
        if (!rc) {
            /* A recovered backup may be the only committed copy. Probe the main name before removing
             * that backup; a failed publication must preserve it when the main name was absent. */
            *fd = ksceIoOpen(HOOK_CACHE_PATH, SCE_O_RDONLY, 0);
            if (*fd >= 0) {
                n = ksceIoClose(*fd);
                if (n >= 0) {
                    *fd = -1;
                    ksceIoRemove(HOOK_CACHE_BACKUP);
                    n = ksceIoRename(HOOK_CACHE_PATH, HOOK_CACHE_BACKUP);
                    if (n >= 0) moved = 1;
                    else rc = n;
                } else rc = n;
            } else if (*fd != (int)0x80010002u) rc = *fd;
            if (!rc) rc = ksceIoRename(HOOK_CACHE_TEMP, HOOK_CACHE_PATH);
            if (rc < 0 && moved) ksceIoRename(HOOK_CACHE_BACKUP, HOOK_CACHE_PATH);
        }
        if (rc < 0 && *fd < 0) ksceIoRemove(HOOK_CACHE_TEMP);
    }
    return rc < 0 ? rc : save ? 0 : (int)done;
}

/* ---- API 23: a shot's JPEG, written by the kernel worker thread ----
 * The game worker hands its JPEG over in order, a staging buffer at a time; the
 * kernel worker writes each piece to the queue's <shot>.jpg.tmp (the first
 * creates it) and, on Save, closes it and renames it to <shot>.jpg. The Shell's
 * subsequent record commit syncs the mount, including this JPEG and its name.
 * The path is
 * the kernel's own (the request's checked card ID); the game supplies bytes. */
static int shot_write_locked(SceUID pid, uint32_t seq, uint32_t offset, uint32_t bytes, void *src)
{
    int rc = model_caller_locked(pid, seq, 0);
    if (rc) return rc;
    if (!(request.flags & VJO_GAME_OCR_F_SHOT) || !src || !bytes || bytes > sizeof(model_stage) ||
        offset > VJO_GAME_OCR_SHOT_MAX || bytes > VJO_GAME_OCR_SHOT_MAX - offset) return VJO_ERR_ARG;
    if (model_io.seq) {
        if (model_io.seq != seq || model_io.pid != pid || !model_io.shot) return VJO_ERR_ARG;
        if (model_io.closing) return VJO_ERR_CANCELLED;
        if (model_io.phase != MODEL_IDLE) {
            /* the same piece again: its result once the kernel worker has written it */
            if (model_io.operation != SHOT_WRITE || offset != model_io.offset || bytes != model_io.bytes)
                return VJO_ERR_ARG;
            if (model_io.phase != MODEL_DONE) return VJO_ERR_BUSY;
            model_io.phase = MODEL_IDLE;
            return model_io.rc;
        }
        if (offset != model_io.written) return VJO_ERR_ARG; /* in order, no gap, no rewrite */
    } else {
        if (offset) return VJO_ERR_ARG;
        model_reset_locked();
        model_io.seq = seq;
        model_io.pid = pid;
        model_io.shot = 1;
        /* VJO_GAME_OCR_SHOT_DIR/<32 hex digits>.jpg.tmp, from the request Submit checked */
        memcpy(model_io.path, VJO_GAME_OCR_SHOT_DIR "/", sizeof(VJO_GAME_OCR_SHOT_DIR));
        memcpy(model_io.path + sizeof(VJO_GAME_OCR_SHOT_DIR), request.shot, 32);
        memcpy(model_io.path + sizeof(VJO_GAME_OCR_SHOT_DIR) + 32, ".jpg.tmp", sizeof(".jpg.tmp"));
    }
    if (ksceKernelMemcpyUserToKernel(model_stage, src, bytes) < 0)
        rc = VJO_ERR_COPY;
    else if (!offset && (bytes < 3 || model_stage[0] != 0xFF || model_stage[1] != 0xD8 || model_stage[2] != 0xFF))
        rc = VJO_ERR_FORMAT; /* not a JPEG's start */
    if (rc) {
        if (!offset) model_reset_locked(); /* nothing was opened */
        return rc;
    }
    result.size = sizeof(result);
    result.seq = seq;
    result.shot_bytes = bytes; /* pending Read: the JPEG is frozen in the game */
    model_io.offset = offset;
    model_io.bytes = bytes;
    model_io.operation = SHOT_WRITE;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

int vjoOcrShotWrite(uint32_t seq, uint32_t offset, uint32_t bytes, const void *src)
{
    return model_syscall(shot_write_locked, seq, offset, bytes, (void *)src); /* read only (a user copy in) */
}

static int shot_save_locked(SceUID pid, uint32_t seq, uint32_t bytes, uint32_t b, void *p)
{
    int rc = model_caller_locked(pid, seq, 0);
    if (rc) return rc;
    if (!model_io.seq || model_io.seq != seq || model_io.pid != pid || !model_io.shot) return VJO_ERR_ARG;
    if (model_io.closing) return VJO_ERR_CANCELLED;
    if (model_io.phase != MODEL_IDLE) {
        if (model_io.operation != SHOT_SAVE) return VJO_ERR_ARG;
        if (model_io.phase != MODEL_DONE) return VJO_ERR_BUSY;
        rc = model_io.rc;
        if (!rc) model_reset_locked(); /* saved: the job holds no file any more */
        else model_io.phase = MODEL_IDLE; /* a Close removes what is left */
        return rc;
    }
    /* every byte written, and a JPEG's end (EOI) last */
    if (model_io.fd < 0 || bytes != model_io.written || bytes < 4 || model_io.tail[0] != 0xFF ||
        model_io.tail[1] != 0xD9) return VJO_ERR_ARG;
    model_io.operation = SHOT_SAVE;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

int vjoOcrShotSave(uint32_t seq, uint32_t bytes)
{
    return model_syscall(shot_save_locked, seq, bytes, 0, NULL);
}

void game_ocr_rows_tick(void)
{
    VJO_LOCK();
    /* API 55's watchdog (docs/game-pause.md): a frame copy holds the game's display call for at most
     * VJO_GAME_OCR_PAUSE_MAX_US and one chunk (game_ocr_rows_hook looks at the clock between chunks). One still
     * copying after PAUSE_WATCHDOG_US means that bound did not hold (a row read that itself stalls). Nothing may stop
     * a copy from outside the game's thread, and its rows call gives it up as any other (ROWS_COPY_WAIT_US, BUSY,
     * tidied below once it ends); from now on no frame copy is armed again until the kernel restarts (the next
     * boot), so every later job reads strip by strip, as without the pause. The state first: a COPYING seen here
     * has its frame_start, written before the hook's claim. */
    if (!pause_off && __atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) == ROWS_COPYING && rows.job.frame) {
        uint32_t s = __atomic_load_n(&rows.frame_start, __ATOMIC_RELAXED), now = (uint32_t)ksceKernelGetSystemTimeWide();
        /* the time since the last look (or, at the first look at this copy, since it started), if it is the game's
         * in front and the worker kept its pace (review M1) */
        uint32_t since = now - (s != wd_copy ? s : wd_look);
        if (s != wd_copy) wd_held = 0;
        wd_copy = s;
        wd_look = now;
        if (since <= PAUSE_WATCHDOG_GAP_US && rows.pid == g.game_pid && g.game_active) wd_held += since;
        if (s && wd_held > PAUSE_WATCHDOG_US) {
            pause_off = 1;
            klog("ocr pause: a frame copy has held the game's display call over %u ms; no more pauses until the next "
                 "boot", PAUSE_WATCHDOG_US / 1000u);
        }
    }
    /* A rows copy whose process exited during it, or that outlived its call:
     * nobody waits for it now. */
    if ((rows.orphan || rows.abandoned) && rows.state == ROWS_DONE && !rows.in_call) {
        rows.state = ROWS_IDLE;
        rows.orphan = rows.abandoned = 0;
        buffers_kstrip_free();
    }
    /* A call whose process exited and that never reached its end: a call waits
     * at most ROWS_WAIT_US + ROWS_COPY_WAIT_US, so after ROWS_GONE_US its thread
     * is gone and nothing uses the state or the strip any more. */
    if (rows.in_call && rows.orphan && rows.state != ROWS_COPYING &&
        (uint64_t)ksceKernelGetSystemTimeWide() - rows.gone_us > ROWS_GONE_US) {
        klog("ocr rows: the exited process's call never returned; tidied");
        rows.state = ROWS_IDLE;
        rows.in_call = rows.orphan = rows.abandoned = 0;
        buffers_kstrip_free();
    }
    VJO_UNLOCK();
}

/* File worker only, with no active broker job. Do not use model_stage: a
 * syscall can queue the next chunk while this thread is inside the filesystem.
 * One byte at the end allocates the spare's capacity without an image buffer.
 * Wait for Shell registration: ux0 may still be remapped before then. */
static void shot_spare_prepare(void)
{
    uint64_t started = ksceKernelGetSystemTimeWide();
    shot_spare_retry_us = started + 1000000u;
    int rc = 0;
    if (shot_spare_fd < 0) {
        shot_spare_usable = 0;
        shot_spare_fd = ksceIoOpen(SHOT_SPARE_PATH, SCE_O_WRONLY | SCE_O_CREAT, 0666);
        rc = shot_spare_fd;
        if (shot_spare_fd >= 0) {
            SceIoStat st;
            memset(&st, 0, sizeof(st));
            rc = ksceIoGetstatByFd(shot_spare_fd, &st);
            if (rc >= 0 && st.st_size < VJO_GAME_OCR_SHOT_MAX) {
                const unsigned char zero = 0;
                rc = ksceIoPwrite(shot_spare_fd, &zero, 1, VJO_GAME_OCR_SHOT_MAX - 1u);
                if (rc != 1) rc = rc < 0 ? rc : VJO_ERR_COPY;
            } else if (rc >= 0 && st.st_size > VJO_GAME_OCR_SHOT_MAX) {
                st.st_size = VJO_GAME_OCR_SHOT_MAX;
                rc = ksceIoChstatByFd(shot_spare_fd, &st, SCE_CST_SIZE);
            }
            if (rc >= 0) {
                rc = ksceIoGetstatByFd(shot_spare_fd, &st);
                if (rc >= 0 && st.st_size != VJO_GAME_OCR_SHOT_MAX) rc = VJO_ERR_COPY;
            }
            shot_spare_usable = rc >= 0;
        }
    }
    if (shot_spare_fd >= 0) {
        int closed = ksceIoClose(shot_spare_fd);
        if (closed >= 0) {
            shot_spare_fd = -1;
            shot_spare_ready = shot_spare_usable;
        } else if (rc >= 0) rc = closed;
    }
    VJO_LOCK();
    shot_spare_busy = shot_spare_fd >= 0;
    shot_spare_preparing = 0;
    VJO_UNLOCK();
    if (rc < 0 || ksceKernelGetSystemTimeWide() - started >= 100000u)
        klog("screenshot reserve: %u ms, rc 0x%08X",
             (uint32_t)(ksceKernelGetSystemTimeWide() - started) / 1000, (unsigned)(rc < 0 ? rc : 0));
}

static int profile_pread(void *ud, void *dst, unsigned bytes, uint32_t offset)
{
    SceUID fd = *(SceUID *)ud;
    return ksceIoPread(fd, dst, bytes, (SceOff)offset);
}

void game_ocr_model_tick(void)
{
    int operation, rc, stat_rc = 0, shot, saved, hook_cache, sc3;
    SceUID fd;
    uint32_t offset, bytes, size, written;
    uint64_t mtime = 0;
    char path[sizeof(model_io.path)], final[sizeof(model_io.path)];
    game_ocr_rows_tick();
    VJO_LOCK();
    sync_locked();
    if (!model_io.seq || model_io.phase == MODEL_RUNNING) {
        int prepare = !model_io.seq && !pending && !claimed &&
                      rows.state == ROWS_IDLE && g.shell_pid > 0 && !stopping &&
                      !shot_spare_ready && ksceKernelGetSystemTimeWide() >= shot_spare_retry_us;
        /* Preparation and retained-close retries both wait for an idle
         * transport. A job arriving meanwhile stays unclaimed, so its model
         * operation's two-second deadline cannot expire behind this I/O.
         * A valid cancelled result owns nothing once its claim is drained. */
        if (prepare) shot_spare_busy = shot_spare_preparing = 1;
        VJO_UNLOCK();
        if (prepare) shot_spare_prepare();
        return;
    }
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
    written = model_io.written;
    shot = model_io.shot;
    saved = model_io.saved;
    hook_cache = model_io.which == HOOK_CACHE_WHICH;
    sc3 = model_io.which == VJO_GAME_OCR_OPEN_SC3;
    memcpy(path, model_io.path, sizeof(path));
    model_io.phase = MODEL_RUNNING;
    VJO_UNLOCK();
    if (shot) { /* a shot's final name: its .tmp's without ".tmp" */
        size_t n = strlen(path) - 4;
        memcpy(final, path, n);
        final[n] = '\0';
    }

    /* Only the persistent kernel worker executes these calls. Calling them
     * directly in a game's syscall would retain its unavailable ux0 namespace. */
    if (operation == PROFILE_LOAD) {
        fd = ksceIoOpen(VJO_PROFILE_DB_PATH, SCE_O_RDONLY, 0);
        rc = fd;
        if (fd >= 0) {
            SceOff length = ksceIoLseek(fd, 0, SCE_SEEK_END);
            rc = length > 0 && (uint64_t)length <= UINT32_MAX
                     ? vjo_hook_profile_read(profile_pread, &fd, (uint32_t)length, path, model_stage, VJO_PROFILE_BYTES)
                     : VJO_ERR_FORMAT;
            if (rc == -1) rc = VJO_ERR_FORMAT;
            int closed = ksceIoClose(fd);
            if (closed >= 0) fd = -1;
            else rc = closed;
        }
    } else if (operation == HOOK_CACHE_LOAD || operation == HOOK_CACHE_SAVE) {
        rc = hook_cache_file(operation, &fd);
    } else if (operation == MODEL_OPEN) {
        fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
        rc = fd;
        if (fd >= 0) {
            SceOff length = ksceIoLseek(fd, 0, SCE_SEEK_END);
            /* a pinned model's size exactly; an SC3 charset's up to its limit (its pin is the worker's to check) */
            rc = length < 0 ? (int)length : length == size || (sc3 && length > 0 && length < size) ? (int)length
                                                                                                    : VJO_ERR_FORMAT;
            if (rc > 0) {
                /* The game keys its resident weights on this. Without it they
                 * are read and checked again for every job, as before. */
                SceIoStat st;
                memset(&st, 0, sizeof(st));
                stat_rc = ksceIoGetstatByFd(fd, &st);
                if (stat_rc < 0) mtime = 0;
                else { stat_rc = 0; mtime = pack_time(&st.st_mtime); }
            }
        }
    } else if (operation == MODEL_READ) {
        rc = ksceIoPread(fd, model_stage, bytes, (SceOff)offset);
        if (rc > (int)bytes) rc = VJO_ERR_FORMAT;
    } else if (operation == SHOT_WRITE) {
        /* API 23: the shot's next piece, at its place; the first creates the .tmp */
        uint64_t t0 = ksceKernelGetSystemTimeWide(), opened;
        rc = 0;
        if (fd < 0) {
            int prepared = shot_spare_ready;
            shot_spare_ready = 0;
            int reserved = prepared && ksceIoRename(SHOT_SPARE_PATH, path) >= 0;
            /* O_TRUNC would discard the space that makes the first save fast.
             * If preparation failed or the spare disappeared, retain the
             * original bounded create/write path. */
            fd = ksceIoOpen(path, SCE_O_WRONLY | (reserved ? 0 : SCE_O_CREAT | SCE_O_TRUNC), 0666);
            if (fd < 0) rc = fd;
        }
        opened = ksceKernelGetSystemTimeWide();
        for (uint32_t done = 0; !rc && done < bytes;) {
            int n = ksceIoPwrite(fd, model_stage + done, bytes - done, (SceOff)(offset + done));
            if (n <= 0 || (uint32_t)n > bytes - done) rc = n < 0 ? n : VJO_ERR_COPY;
            else done += (uint32_t)n;
        }
        if (!rc) rc = (int)bytes;
        if (ksceKernelGetSystemTimeWide() - t0 >= 100000u)
            klog("screenshot file: offset %u, open %u ms, write %u ms, rc %d", offset,
                 (uint32_t)(opened - t0) / 1000,
                 (uint32_t)(ksceKernelGetSystemTimeWide() - opened) / 1000, rc);
        klog_d("ocr shot write: %u B at %u -> %d t=%u ms", bytes, offset, rc, klog_ms());
    } else if (operation == SHOT_SAVE) {
        /* closed, then its name. #194: no sync here. 0.8.0-test.4's two ksceIoSync("ux0:") held this thread,
         * which also polls the overlay's pad, for over 15 s, and 0.8.0-test.4.2's ksceIoSyncByFd turned the
         * console off (the last line of its kernel.txt is the shot's write). The Shell's record that
         * names this file is committed with a sceIoSync("ux0:") after it (shell/anki_queue.c commit),
         * which syncs this file and its name too. */
        uint64_t t0 = ksceKernelGetSystemTimeWide();
        klog_d("ocr shot save: closing t=%u ms", klog_ms()); /* a stop in the close or rename shows */
        /* Trim the reservation to the entire JPEG, not the last chunk. A
         * failed trim must never publish a padded or incomplete image. */
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        st.st_size = written;
        rc = ksceIoChstatByFd(fd, &st, SCE_CST_SIZE);
        if (rc >= 0 && (rc = ksceIoClose(fd)) >= 0) {
            fd = -1;
            if ((rc = ksceIoRename(path, final)) < 0) ksceIoRemove(path);
        } /* else still open: the game's Close closes and removes it */
        klog_d("ocr shot save: close and rename %u us -> 0x%08X t=%u ms", (uint32_t)(ksceKernelGetSystemTimeWide() - t0),
               (unsigned)(rc < 0 ? rc : 0), klog_ms());
        if (ksceKernelGetSystemTimeWide() - t0 >= 100000u)
            klog("screenshot file: close/rename %u ms, rc 0x%08X",
                 (uint32_t)(ksceKernelGetSystemTimeWide() - t0) / 1000, (unsigned)(rc < 0 ? rc : 0));
        rc = rc < 0 ? rc : 0;
        saved = !rc;
    } else {
        rc = fd < 0 ? 0 : ksceIoClose(fd);
        if (rc >= 0) fd = -1;
        /* An abandoned shot leaves nothing: its .tmp, or the .jpg a save made that the game never
         * collected (a collected save resets the slot, so no close follows it). Every way a shot is
         * abandoned ends here: the game's Close after a failure or a timeout, a cancellation, the
         * game's exit, the Shell's restart (they set closing); the Shell then saves the card without
         * a screenshot, so no record names this file. */
        if (rc >= 0 && shot) ksceIoRemove(saved ? final : path);
        if (rc >= 0 && hook_cache) ksceIoRemove(HOOK_CACHE_TEMP);
    }

    VJO_LOCK();
    /* No syscall can release/reuse the owner while phase is RUNNING. */
    model_io.fd = fd;
    model_io.rc = rc;
    model_io.phase = MODEL_DONE;
    if (operation == MODEL_OPEN) {
        model_io.stat_rc = stat_rc;
        model_io.mtime = mtime;
        if (rc > 0) model_io.size = (uint32_t)rc; /* the reads' bound: an SC3 charset's own size */
    }
    if (operation == SHOT_WRITE && rc > 0) {
        model_io.written = offset + bytes;
        model_io.tail[0] = bytes > 1 ? model_stage[bytes - 2] : model_io.tail[1];
        model_io.tail[1] = model_stage[bytes - 1];
    }
    if (operation == SHOT_SAVE) model_io.saved = saved;
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
    /* Wake a model syscall waiting for this result (none may be). */
    if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_DONE);
}
