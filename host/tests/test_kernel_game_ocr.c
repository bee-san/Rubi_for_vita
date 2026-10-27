/* Actual IPC and capture ownership code with bounded user-copy substitutes. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
typedef struct { size_t size; SceUID modid; } tai_module_info_t;
int taiGetModuleInfoForKernel(SceUID, const char *, tai_module_info_t *);
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.c"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "test_tier_pins.h"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static int module_missing, module_id, copy_in_fail, copy_out_fail;
static unsigned locks, allocations;
static VjoGameOcrResult message;
static unsigned model_opens, model_reads, model_closes;
static unsigned model_events;
static int file_live, open_error, seek_error, read_error, close_error, eof, short_read, bad_size;
static int during_io;
static uint64_t model_clock;
static uint32_t expected_model_size;
static unsigned done_events, waits, stats;
static int expect_fallback;
static const char *expect_lm; /* API 20: the LM file Open should name */
static int done_pending, wait_runs_worker, stat_error;
static SceIoStat file_stat;
/* The game's frame, as the display hook sees it (32 x 32 A8B8G8R8, pitch 40). */
#define FB_W 32u
#define FB_H 32u
#define FB_PITCH 40u
static uint32_t game_fb[FB_PITCH * FB_H];
static uint32_t fb_fmt;
static unsigned fb_w = FB_W, fb_h = FB_H;
static int frames_come, frame_pid, in_hook, user_block_ok, hook_copy_fail, kstrip_alloc_fail;
static int stall_copy; /* the hook starts the copy and has not finished it (another core) */
static int gone_after_frame; /* the game exits right after its frame, while the rows call is in the kernel */
static unsigned frees_seen_at_exit;
static unsigned rows_events, capture_events, hook_frames, kstrip_allocs, kstrip_frees;
static unsigned quiet_events; /* API 41: vjoOcrHookLine's wakeup of the Shell */
static unsigned char user_rows[64 * 64]; /* the worker's capture block */

static unsigned klogs, delays;
static const char *last_klog = "";
static void (*before_lock)(void); /* another thread, just before a syscall's lock */
void klog(const char *format, ...) { klogs++; last_klog = format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
/* The calling thread (#138: one registered worker thread per game process). */
static int caller_thread = 0x7001;
int ksceKernelGetThreadId(void) { return caller_thread; }
uint64_t ksceKernelGetSystemTimeWide(void) { return model_clock; }
/* MEV_JOB and MEV_RESULT as the event flag holds them, and what runs on another
 * thread while vjoOcrTake or vjoOcrRead waits for them (the Shell's submission, the
 * game's completion, a cancellation). */
static int job_bit, result_bit;
static unsigned job_events, result_events, job_waits, result_waits;
static unsigned take_wait_us = 100000u; /* API 49: the take's wait (vjoOcrTake's out->seq, else 100 ms) */
static void (*during_event_wait)(void);
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    if (uid == g.mevf && g.mevf > 0 && bits == MEV_ROWS) { rows_events++; return 0; }
    if (uid == g.mevf && g.mevf > 0 && (bits & (MEV_JOB | MEV_RESULT)) && !(bits & ~(MEV_JOB | MEV_RESULT))) {
        TEST_CHECK(locks == 1);
        if (bits & MEV_JOB) { job_events++; job_bit = 1; }
        if (bits & MEV_RESULT) { result_events++; result_bit = 1; }
        return 0;
    }
    if (uid == g.mevf && g.mevf > 0) { TEST_CHECK(bits == MEV_DONE); done_events++; done_pending = 1; return 0; }
    if (uid == g.evf && bits == VJO_EV_CAPTURE_DONE) { capture_events++; return 0; }
    if (uid == g.evf && bits == VJO_EV_REGION_QUIET) { TEST_CHECK(locks == 0); quiet_events++; return 0; } /* API 41 */
    TEST_CHECK(uid == g.ioevf && bits == IEV_MODEL); model_events++; return 0;
}
int ksceKernelClearEventFlag(SceUID uid, unsigned bits)
{
    TEST_CHECK(uid == g.mevf && (bits == ~MEV_ROWS || bits == ~MEV_JOB || bits == ~MEV_RESULT));
    if (bits == ~MEV_JOB) { TEST_CHECK(locks == 1); job_bit = 0; } /* cleared under the lock: no lost wakeup */
    if (bits == ~MEV_RESULT) { TEST_CHECK(locks == 1); result_bit = 0; }
    return 0;
}
int ksceKernelDelayThread(SceUInt us) { model_clock += us; delays++; return 0; }
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }
/* The worker's memory: only its capture block counts as a block of process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    TEST_CHECK(size == 0);
    if (pid != 7 || !user_block_ok || p < user_rows || p >= user_rows + sizeof(user_rows)) return -1;
    return 41;
}
/* The game submits a frame: the display hook runs in its thread, no lock held. */
static void game_frame(void)
{
    in_hook = 1; hook_frames++;
    game_ocr_rows_hook(frame_pid, (uintptr_t)game_fb, FB_PITCH, fb_fmt, fb_w, fb_h);
    in_hook = 0;
}
static void model_tick(void);
/* A model syscall waiting for the kernel worker: g.lock is not held, so the
 * worker can run (wait_runs_worker) and finish the queued operation. */
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    if (bits == MEV_ROWS) { /* a rows call waits for the game's next frame, without the lock */
        TEST_ASSERT(uid == g.mevf && !locks && syscall_depth == 1 && timeout && *timeout <= 50000);
        if (stall_copy && __sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_COPYING)) {
            model_clock += *timeout;
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        if (frames_come) {
            frames_come--; game_frame();
            if (gone_after_frame) { gone_after_frame = 0; game_ocr_process_gone(7); frees_seen_at_exit = kstrip_frees; }
            if (out) *out = MEV_ROWS;
            return 0;
        }
        model_clock += *timeout;
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
    if (bits == MEV_JOB || bits == MEV_RESULT) { /* vjoOcrTake / vjoOcrRead, without the lock */
        int *bit = bits == MEV_JOB ? &job_bit : &result_bit;
        TEST_ASSERT(uid == g.mevf && !locks && syscall_depth == 1 && timeout);
        TEST_CHECK(wait == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT));
        TEST_CHECK(*timeout == (bits == MEV_JOB ? take_wait_us : 50000u));
        if (bits == MEV_JOB) job_waits++; else result_waits++;
        if (during_event_wait) { /* another thread's syscall meanwhile */
            void (*fn)(void) = during_event_wait;
            SceUID saved = caller; unsigned depth = syscall_depth;
            during_event_wait = NULL; syscall_depth = 0; fn(); caller = saved; syscall_depth = depth;
        }
        if (*bit) { *bit = 0; if (out) *out = bits; return 0; }
        model_clock += *timeout;
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
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
/* ---- API 23: a shot's file, on an in-memory card: the .tmp the first write
 * creates, the .jpg the save renames it to. Only the kernel worker (caller 0,
 * no lock, no syscall) may touch it. ---- */
#define SHOT_FD 32
#define SHOT_FS_SLOTS 4
static struct {
    char name[128];
    unsigned char data[VJO_GAME_OCR_SHOT_MAX];
    uint32_t len;
    int live;
} shot_fs[SHOT_FS_SLOTS];
static int shot_open, shot_index, shot_open_error, shot_write_error, shot_close_error, shot_rename_error, shot_sync_error;
static int shot_short_writes, shot_trim_error, shot_claim_error, shot_last_open_flags;
static int spare_open_error, spare_stat_error, spare_write_error, spare_zero_write, spare_close_error;
static int spare_last_open_flags, spare_stat_fail_at, spare_bad_verify;
static uint32_t shot_next_offset, shot_trim_size;
static uint32_t shot_cancel_on_rename; /* the Shell cancels this job while its file is renamed */
static unsigned shot_opens, shot_writes, shot_closes, shot_renames, shot_removes, shot_syncs, shot_trims;
static unsigned shot_extensions, spare_opens, spare_stats, spare_writes, spare_closes, spare_trims;
static void (*during_spare_write)(void); /* a submission while storage is blocked */
static int shot_entry(const char *name)
{
    for (int i = 0; i < SHOT_FS_SLOTS; i++)
        if (shot_fs[i].live && !strcmp(shot_fs[i].name, name)) return i;
    return -1;
}
static int shot_seed(const char *name, uint32_t len, unsigned char fill)
{
    int i;
    TEST_ASSERT(len <= VJO_GAME_OCR_SHOT_MAX && shot_entry(name) < 0);
    for (i = 0; i < SHOT_FS_SLOTS && shot_fs[i].live; i++) {}
    TEST_ASSERT(i < SHOT_FS_SLOTS);
    memset(&shot_fs[i], 0, sizeof(shot_fs[i]));
    snprintf(shot_fs[i].name, sizeof(shot_fs[i].name), "%s", name);
    memset(shot_fs[i].data, fill, sizeof(shot_fs[i].data));
    shot_fs[i].len = len;
    shot_fs[i].live = 1;
    return i;
}
static int shot_worker_context(void) { return caller == 0 && !locks && !syscall_depth; }
static SceUID shot_file_open(const char *path, int flags)
{
    int i, spare = !strcmp(path, SHOT_SPARE_PATH);
    int cache = !strncmp(path, HOOK_CACHE_PATH, sizeof(HOOK_CACHE_PATH) - 1u);
    TEST_ASSERT(shot_worker_context() && !shot_open);
    if (spare) {
        TEST_CHECK(flags == (SCE_O_WRONLY | SCE_O_CREAT));
        spare_opens++;
        spare_last_open_flags = flags;
        if (spare_open_error) return spare_open_error;
    } else {
        TEST_CHECK(cache ? flags == SCE_O_RDONLY || flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC)
                        : (flags == SCE_O_WRONLY || flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC)) &&
                          strlen(path) > 8 && !strcmp(path + strlen(path) - 8, ".jpg.tmp"));
        shot_opens++;
        shot_last_open_flags = flags;
        if (shot_open_error) return shot_open_error;
    }
    if ((i = shot_entry(path)) < 0) {
        if (!(flags & SCE_O_CREAT)) return (int)0x80010002u;
        i = shot_seed(path, 0, 0);
    }
    if (flags & SCE_O_TRUNC) shot_fs[i].len = 0;
    shot_open = 1;
    shot_index = i;
    shot_next_offset = 0;
    return SHOT_FD;
}
int ksceIoGetstatByFd(SceUID fd, SceIoStat *out)
{
    if (fd == SHOT_FD) {
        TEST_ASSERT(shot_worker_context() && shot_open);
        spare_stats++;
        if (spare_stat_error && (!spare_stat_fail_at || spare_stats == (unsigned)spare_stat_fail_at))
            return spare_stat_error;
        memset(out, 0, sizeof(*out));
        out->st_size = shot_fs[shot_index].len;
        if (spare_bad_verify && spare_stats == 2) out->st_size--;
        return 0;
    }
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    stats++;
    if (stat_error) return stat_error;
    *out = file_stat; out->st_size = expected_model_size; return 0;
}
int ksceIoChstatByFd(SceUID fd, SceIoStat *st, int bits)
{
    TEST_ASSERT(shot_worker_context() && fd == SHOT_FD && shot_open);
    TEST_CHECK(bits == SCE_CST_SIZE);
    TEST_ASSERT(st->st_size >= 0 && st->st_size <= shot_fs[shot_index].len);
    if (!strcmp(shot_fs[shot_index].name, SHOT_SPARE_PATH)) {
        spare_trims++;
    } else {
        shot_trims++;
        shot_trim_size = (uint32_t)st->st_size;
        if (shot_trim_error) return shot_trim_error;
    }
    shot_fs[shot_index].len = (uint32_t)st->st_size;
    return 0;
}
int ksceIoPwrite(SceUID fd, const void *data, SceSize bytes, SceOff offset)
{
    int i = shot_index;
    TEST_ASSERT(shot_worker_context() && fd == SHOT_FD && shot_open);
    TEST_ASSERT(shot_fs[i].live && offset >= 0 && (uint64_t)offset + bytes <= sizeof(shot_fs[i].data));
    if (!strcmp(shot_fs[i].name, SHOT_SPARE_PATH)) {
        TEST_CHECK(bytes == 1 && offset == VJO_GAME_OCR_SHOT_MAX - 1u && *(const unsigned char *)data == 0);
        spare_writes++;
        if (during_spare_write) {
            void (*fn)(void) = during_spare_write;
            during_spare_write = NULL;
            fn();
        }
        if (spare_write_error) return spare_write_error;
        if (spare_zero_write) return 0;
        /* A file extended past EOF reads as zeroes in the intervening range. */
        if ((uint32_t)offset > shot_fs[i].len)
            memset(shot_fs[i].data + shot_fs[i].len, 0, (uint32_t)offset - shot_fs[i].len);
    } else {
        TEST_CHECK((uint32_t)offset == shot_next_offset); /* in order, including preallocated files */
        shot_writes++;
        if (shot_write_error) return shot_write_error;
        if (shot_short_writes && bytes > 1000) bytes = 1000;
        if ((uint32_t)offset + bytes > shot_fs[i].len) shot_extensions++;
        shot_next_offset = (uint32_t)offset + bytes;
    }
    memcpy(shot_fs[i].data + offset, data, bytes);
    if ((uint32_t)offset + bytes > shot_fs[i].len) shot_fs[i].len = (uint32_t)offset + bytes;
    return (int)bytes;
}
int ksceIoRename(const char *from, const char *to)
{
    int i;
    TEST_ASSERT(shot_worker_context() && !shot_open);
    shot_renames++;
    if (!strcmp(from, SHOT_SPARE_PATH) && shot_claim_error) return shot_claim_error;
    if (shot_rename_error) return shot_rename_error;
    if (shot_cancel_on_rename) { /* the Shell's cancel, on another thread, while the kernel worker saves */
        uint32_t seq = shot_cancel_on_rename;
        shot_cancel_on_rename = 0;
        caller = 3;
        TEST_CHECK(vjoOcrCancel(seq) == 0);
        caller = 0;
    }
    if ((i = shot_entry(from)) < 0) return (int)0x80010002u;
    TEST_CHECK(shot_entry(to) < 0); /* never renamed over a file */
    snprintf(shot_fs[i].name, sizeof(shot_fs[i].name), "%s", to);
    return 0;
}
int ksceIoRemove(const char *path)
{
    int i;
    TEST_ASSERT(shot_worker_context() && !shot_open);
    shot_removes++;
    if ((i = shot_entry(path)) < 0) return (int)0x80010002u;
    shot_fs[i].live = 0;
    return 0;
}
int ksceIoSync(const char *device, unsigned int unk) { (void)device; (void)unk; TEST_CHECK(0); return -1; }
/* #194: a shot's save syncs nothing (test.4.2's ksceIoSyncByFd turned the console off) */
int ksceIoSyncByFd(SceUID fd, int flag) { (void)fd; (void)flag; TEST_CHECK(0); shot_syncs++; return shot_sync_error; }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    if (!strcmp(path, SHOT_SPARE_PATH) ||
        !strncmp(path, HOOK_CACHE_PATH, sizeof(HOOK_CACHE_PATH) - 1u) ||
        !strncmp(path, VJO_GAME_OCR_SHOT_DIR "/", sizeof(VJO_GAME_OCR_SHOT_DIR))) return shot_file_open(path, flags);
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && !file_live);
    char expected[320]; snprintf(expected,sizeof(expected),"%s/%s",VJO_VOCR_MODEL_ROOT,
                                 expect_lm ? expect_lm : expect_fallback ? request.fallback : request.model);
    TEST_CHECK(!strcmp(path, expected) && flags == SCE_O_RDONLY);
    model_opens++; interrupt_io();
    if (open_error) return open_error;
    file_live = 1; return 31;
}
SceOff ksceIoLseek(SceUID fd, SceOff offset, int whence)
{
    if (fd == SHOT_FD) {
        TEST_ASSERT(shot_worker_context() && shot_open && offset == 0 && whence == SCE_SEEK_END);
        return seek_error ? (SceOff)seek_error : shot_fs[shot_index].len;
    }
    TEST_CHECK(fd == 31 && file_live && !locks && !syscall_depth);
    return seek_error ? (SceOff)seek_error : (SceOff)expected_model_size + bad_size;
}
int ksceIoPread(SceUID fd, void *out, SceSize bytes, SceOff offset)
{
    if (fd == SHOT_FD) {
        TEST_ASSERT(shot_worker_context() && shot_open && offset >= 0);
        interrupt_io();
        if (read_error) return read_error;
        if (eof) return 0;
        if (short_read && bytes > 37) bytes = 37;
        if ((uint64_t)offset + bytes > shot_fs[shot_index].len) bytes = shot_fs[shot_index].len - offset;
        memcpy(out, shot_fs[shot_index].data + offset, bytes);
        return (int)bytes;
    }
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
    if (fd == SHOT_FD) {
        TEST_ASSERT(shot_worker_context() && shot_open);
        if (!strcmp(shot_fs[shot_index].name, SHOT_SPARE_PATH)) {
            spare_closes++;
            if (spare_close_error) return spare_close_error;
        } else {
            shot_closes++;
            if (shot_close_error) return shot_close_error;
        }
        shot_open = 0;
        return 0;
    }
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    model_closes++;
    if (close_error) return close_error;
    file_live = 0; return 0;
}
static void model_tick(void)
{ SceUID saved = caller; caller = 0; game_ocr_model_tick(); caller = saved; }
static int record_unlocks, unlock_waiters[8], lock_waiters[8];
static void (*after_unlock)(void); /* another thread, just after a syscall's unlock (once) */
static unsigned unlock_count, lock_count;
int ksceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    (void)count; (void)timeout;
    if (before_lock) before_lock();
    TEST_CHECK(uid == g.lock && locks == 0); locks++;
    if (record_unlocks && syscall_depth == 1 && lock_count < 8) lock_waiters[lock_count++] = waiters;
    return 0;
}
/* The waiter count as another thread (module_stop) would see it right after each unlock
 * inside a syscall: it must already count a caller about to wait, and no longer count
 * one that has left the wait (the count is changed under the lock, never after it). */
int ksceKernelUnlockMutex(SceUID uid, int count)
{
    (void)count; TEST_CHECK(uid == g.lock && locks == 1); locks--;
    if (record_unlocks && syscall_depth == 1 && unlock_count < 8) unlock_waiters[unlock_count++] = waiters;
    if (after_unlock && syscall_depth == 1) {
        void (*fn)(void) = after_unlock;
        SceUID saved = caller; unsigned depth = syscall_depth;
        after_unlock = NULL; syscall_depth = 0; fn(); caller = saved; syscall_depth = depth;
    }
    return 0;
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
    /* The hook writes the worker's rows in the game's thread with no lock; every
     * other copy out is a syscall's, under the lock. */
    if (in_hook) { TEST_CHECK(!locks); if (hook_copy_fail || !dst) return -1; memcpy(dst, src, bytes); return 0; }
    TEST_CHECK(syscall_depth == 1 && locks == 1);
    if (copy_out_fail || !dst) return -1;
    memcpy(dst, src, bytes); return 0;
}
static unsigned char kstrip_storage[VJO_GAME_OCR_KSTRIP_MAX];
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    if (!strcmp(name, "VjoOcrStrip")) {
        TEST_CHECK(bytes <= sizeof(kstrip_storage) && !(bytes & 4095) && locks == 1);
        kstrip_allocs++;
        return kstrip_alloc_fail ? (int)0x80024302u : 28;
    }
    /* API 24: the strip is the kernel's only block for a job (no VjoCapture) */
    TEST_CHECK_(0, "kernel block %s of %u B", name, (unsigned)bytes);
    allocations++; return (int)0x80024302u;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_CHECK(uid == 28); *base = kstrip_storage; return 0;
}
int ksceKernelFreeMemBlock(SceUID uid)
{
    TEST_CHECK(uid == 28);
    kstrip_frees++;
    return 0;
}

static void shot_fs_reset(void);
static void setup(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    buffers_kstrip_free();
    g.lock = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.evf = 2; g.ievf = 6; g.mevf = 5;
    g.capture_seq = g.done_seq = 11; g.fb_w = FB_W; g.fb_h = FB_H;
    for (unsigned y = 0; y < FB_H; y++)
        for (unsigned x = 0; x < FB_PITCH; x++) /* R, G, B, A bytes: R = x * 7, G = y * 5, B = x ^ y */
            game_fb[y * FB_PITCH + x] = 0xFF000000u | ((x ^ y) & 0xFFu) << 16 | ((y * 5u) & 0xFFu) << 8 | ((x * 7u) & 0xFFu);
    fb_fmt = VJO_FB_A8B8G8R8; fb_w = FB_W; fb_h = FB_H;
    frames_come = 0; frame_pid = 7; in_hook = 0; user_block_ok = 1; hook_copy_fail = kstrip_alloc_fail = 0;
    stall_copy = gone_after_frame = 0;
    kledger_init();
    rows_events = capture_events = hook_frames = kstrip_allocs = kstrip_frees = 0;
    quiet_events = 0;
    memset(user_rows, 0xCC, sizeof(user_rows));
    caller = 7; module_id = 99;
    module_missing = copy_in_fail = copy_out_fail = 0;
    locks = allocations = syscall_depth = 0;
    memset(&message, 0, sizeof(message));
    model_clock = 1000000; expected_model_size = VJO_VOCR_FL10_BYTES;
    model_opens = model_reads = model_closes = model_events = 0;
    file_live = open_error = seek_error = read_error = close_error = eof = short_read = bad_size = during_io = 0;
    done_events = waits = stats = 0; done_pending = wait_runs_worker = stat_error = 0; expect_fallback = 0; expect_lm = NULL;
    job_bit = result_bit = 0; job_events = result_events = job_waits = result_waits = 0; during_event_wait = NULL;
    take_wait_us = 100000u;
    record_unlocks = 0; unlock_count = lock_count = 0; klogs = delays = 0; last_klog = ""; before_lock = NULL;
    memset(&file_stat, 0, sizeof(file_stat));
    file_stat.st_mtime = (SceDateTime){2026, 10, 11, 9, 43, 58, 123456};
    shot_fs_reset();
    TEST_ASSERT(vjoOcrRegister() == 0);
    caller = 3;
}

static VjoGameOcrRequest make_request(void)
{
    VjoGameOcrRequest r = {0};
    r.size = sizeof(r);
    strcpy(r.model, "FL10_w8.vocr");
    strcpy(r.model_dir, "ux0:data/VitaJPOverlay/ocr");
    return r;
}

/* #138: a game holds one worker. Should a second copy still run in it (the
 * kernel's and a per-title taiHEN line's), its thread is refused, logged once,
 * and takes no job; the registered thread keeps working; the game's exit lets
 * the next process's thread register. */
static void second_worker_thread_is_refused(void)
{
    VjoGameOcrRequest r = make_request(), got = {0};
    setup(); /* thread 0x7001 of process 7 registered */
    caller = 7; caller_thread = 0x7002; klogs = 0;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY && klogs == 1);
    TEST_CHECK(strstr(last_klog, "second worker thread") != NULL);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY && klogs == 1); /* it asks every 100 ms: one line */
    caller_thread = 0x7001;
    TEST_CHECK(vjoOcrRegister() == 0 && klogs == 1); /* the registered one, as before */
    caller = 3;
    TEST_CHECK(vjoOcrSubmit(&r) > 0);
    caller = 7; caller_thread = 0x7002;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    caller_thread = 0x7001;
    got.size = sizeof(got);
    TEST_CHECK(vjoOcrTake(&got) == 0 && got.seq == request.seq); /* the job goes to the registered thread */
    message.size = sizeof(message); message.seq = got.seq;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* A Shell restart re-registers the same thread; the other stays out. */
    game_ocr_shell_changed_locked();
    TEST_CHECK(vjoOcrRegister() == 0);
    caller_thread = 0x7002;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    /* The game exits; the next game's thread registers (it is a new process). */
    game_ocr_process_gone(7);
    g.game_pid = 8;
    caller = 8; caller_thread = 0x8001;
    TEST_CHECK(vjoOcrRegister() == 0 && game_ocr_worker_pid_locked() == 8);
    caller_thread = 0x8002; /* and that process holds one too */
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    caller_thread = 0x7001;
}

static uint32_t submit(void)
{
    VjoGameOcrRequest r = make_request();
    caller = 3;
    int seq = vjoOcrSubmit(&r);
    TEST_ASSERT(seq > 0);
    return (uint32_t)seq;
}

/* The Shell's next job: VJO_ERR_BUSY while the slot is held (queued, claimed, a model or rows
 * call in flight), its sequence once it is free. API 18-23 probed the slot with the kernel's
 * full-frame capture request, which the same states refused; API 24 removed it. */
static int next_submit(void)
{
    VjoGameOcrRequest r = make_request();
    caller = 3;
    return vjoOcrSubmit(&r);
}

static void take(uint32_t seq)
{
    VjoGameOcrRequest r = {0};
    caller = 7;
    TEST_ASSERT(vjoOcrTake(&r) == 0);
    /* Submit filled in the capture this job takes and the region in the last frame. */
    TEST_CHECK(r.size == sizeof(r) && r.seq == seq && r.capture_seq > 11);
    TEST_CHECK(r.x == 0 && r.y == 0 && r.width == 32 && r.height == 32 && r.fb_width == 32 && r.fb_height == 32);
    memset(&message, 0, sizeof(message));
    message.size = sizeof(message); message.seq = seq;
    strcpy(message.text, "猫");
}

/* The hand-off without polls: an idle vjoOcrTake waits (once, at most 100 ms,
 * without the lock) for vjoOcrSubmit's MEV_JOB, a pending vjoOcrRead (once, at
 * most 50 ms) for vjoOcrComplete's MEV_RESULT. A job or result already there
 * is answered without waiting; the bit is cleared under the lock first, so one
 * set while the caller is between its check and its wait is not lost. */
static void shell_submits(void) { VjoGameOcrRequest r = make_request(); caller = 3; TEST_CHECK(vjoOcrSubmit(&r) > 0); }
static void game_completes(void) { caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0); }
static void shell_cancels(void) { caller = 3; TEST_CHECK(vjoOcrCancel(request.seq) == 0); }
static void game_exits(void) { game_ocr_process_gone(7); }
static int waiters_seen;
static void module_stops(void) /* module_stop's shutdown, on another thread */
{
    waiters_seen = waiters;
    VJO_LOCK(); TEST_CHECK(game_ocr_shutdown_locked() == 0); VJO_UNLOCK();
}

static void take_wakes_on_submit(void)
{
    VjoGameOcrRequest r = {0};
    setup(); caller = 7;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1); /* idle: one bounded wait, then 1 */
    TEST_CHECK(model_clock - t0 == 100000);
    /* a submission while the game worker waits: claimed at once, no timeout passes */
    t0 = model_clock; during_event_wait = shell_submits; caller = 7;
    TEST_CHECK(vjoOcrTake(&r) == 0 && job_waits == 2 && job_events == 1);
    TEST_CHECK(model_clock == t0 && r.seq == request.seq && claimed && !pending);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* claimed: BUSY at once, no wait */
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_BUSY && job_waits == 2);
    /* a job already queued: no wait */
    setup(); uint32_t seq = submit(); caller = 7;
    TEST_CHECK(vjoOcrTake(&r) == 0 && r.seq == seq && job_waits == 0);
    /* an older job's bit left set never makes an idle take skip its wait */
    setup(); seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(job_bit == 1); /* nobody waited for that submission */
    t0 = model_clock;
    r.seq = 0; /* as the worker asks before every take (API 49: else out->seq is the wait) */
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1 && model_clock - t0 == 100000);
    /* the module stopping while the worker waits: the shutdown wakes it, it leaves at once, and
     * module_stop sees no thread inside the wait any more */
    setup(); caller = 7; during_event_wait = module_stops; t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && waiters_seen == 1 && model_clock == t0);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* not the registered, current worker: refused without waiting */
    setup(); caller = 8;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && !job_waits);
    caller = 7; g.game_active = 0;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && !job_waits);
}

static void read_wakes_on_complete(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    caller = 3;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == 1 && result_waits == 1 && model_clock - t0 == 50000);
    /* the game completes while the Shell waits: the result at once */
    memset(&message, 0, sizeof(message)); message.size = sizeof(message); message.seq = seq; strcpy(message.text, "猫");
    t0 = model_clock; during_event_wait = game_completes; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    TEST_CHECK(result_waits == 2 && result_events == 1 && model_clock == t0);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* completed already: no wait */
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && result_waits == 2);
    /* cancelled while the Shell waits (its own cancel, the game's exit): CANCELLED after the wait */
    setup(); seq = submit(); take(seq);
    during_event_wait = shell_cancels; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && result_waits == 1);
    setup(); seq = submit(); take(seq);
    during_event_wait = game_exits; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    /* an earlier job's result bit, left set (no read was waiting), never cuts a later read's wait short */
    setup(); seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0); TEST_CHECK(result_bit == 1);
    uint32_t next = submit(); take(next); caller = 3; t0 = model_clock;
    TEST_CHECK(vjoOcrRead(next, &message) == 1 && result_waits == 1 && model_clock - t0 == 50000);
    /* a queued or claimed job refuses the shutdown (module_stop fails then, and nothing changed) */
    setup(); seq = submit(); caller = 3;
    VJO_LOCK(); TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY && !game_ocr_waiters_locked()); VJO_UNLOCK();
    /* a stale or foreign read: refused without waiting */
    setup(); seq = submit(); caller = 3;
    TEST_CHECK(vjoOcrRead(seq + 1, &message) == VJO_ERR_ARG && !result_waits);
    caller = 4; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_PERM && !result_waits);
}

/* The read side of the waiter count (audit-overlay-perf-4, B1; its cases): a read can be
 * waiting when the shutdown passes, since the game's exit during the read drops the claim
 * that would refuse it. The shutdown counts that reader and wakes it; every waited read
 * gives its count back; a read of a cancelled job never waits. */
static int stop_rc = 99;
static void game_exits_then_module_stops(void)
{
    game_ocr_process_gone(7); /* its claim goes: the shutdown is not refused */
    VJO_LOCK(); waiters_seen = waiters; stop_rc = game_ocr_shutdown_locked(); VJO_UNLOCK();
}

static void read_waiters_are_counted_and_woken(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    caller = 3; during_event_wait = game_exits_then_module_stops; stop_rc = 99;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(stop_rc == 0 && waiters_seen == 1 && model_clock == t0); /* counted, woken at once */
    TEST_MSG("shutdown rc %d, waiters %d, %llu us", stop_rc, waiters_seen, (unsigned long long)(model_clock - t0));
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    /* every waited read gives its count back, or module_stop would fail for good */
    setup(); seq = submit(); take(seq); caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 1 && result_waits == 1);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    TEST_CHECK(vjoOcrRead(seq, &message) == 1 && result_waits == 2);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    /* a read of a job already cancelled answers at once, as main did */
    setup(); seq = submit(); caller = 3; TEST_CHECK(vjoOcrCancel(seq) == 0);
    t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && !result_waits && model_clock == t0);
}

static void game_suspends(void) { g.game_active = 0; }

/* The count changes under the lock: right after the unlock before a wait it counts the
 * caller, right after the unlock that follows the wait it no longer does. And the re-check
 * after a wait sees what changed meanwhile: a suspend during a read's wait ends it CANCELLED
 * at once, not one read later. */
static void waiter_count_changes_under_the_lock(void)
{
    VjoGameOcrRequest r = {0};
    /* and never between an unlock and the next lock (outside it): the count at the re-check's
     * lock is the one at the wait's unlock (audit-overlay-perf-4b's case) */
    setup(); caller = 7; record_unlocks = 1; unlock_count = lock_count = 0;
    TEST_CHECK(vjoOcrTake(&r) == 1);
    record_unlocks = 0;
    TEST_CHECK(unlock_count == 2 && unlock_waiters[0] == 1 && unlock_waiters[1] == 0);
    TEST_CHECK(lock_count == 2 && lock_waiters[0] == 0 && lock_waiters[1] == unlock_waiters[0]);
    TEST_MSG("take: locks see %d %d, unlocks %d %d", lock_waiters[0], lock_waiters[1], unlock_waiters[0],
             unlock_waiters[1]);
    setup(); uint32_t seq = submit(); take(seq); caller = 3; record_unlocks = 1; unlock_count = lock_count = 0;
    TEST_CHECK(vjoOcrRead(seq, &message) == 1);
    record_unlocks = 0;
    TEST_CHECK(unlock_count == 2 && unlock_waiters[0] == 1 && unlock_waiters[1] == 0);
    TEST_CHECK(lock_count == 2 && lock_waiters[0] == 0 && lock_waiters[1] == unlock_waiters[0]);
    TEST_MSG("read: locks see %d %d, unlocks %d %d", lock_waiters[0], lock_waiters[1], unlock_waiters[0],
             unlock_waiters[1]);
    setup(); seq = submit(); take(seq); caller = 3; during_event_wait = game_suspends;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && result_waits == 1 && model_clock - t0 == 50000);
    setup(); caller = 7; during_event_wait = game_suspends;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && job_waits == 1);
}

/* module_stop's part (game_ocr_stop): the shutdown, the drain until the woken takes and reads
 * have left, 1 ms of grace after any it woke, and an undone stop when one does not leave in
 * time (cases after audit-overlay-perf-4b's harness of the drain). */
static int stop_result = 99;
static void module_stop_runs(void) { stop_result = game_ocr_stop(); }
static void game_exits_then_module_stop_runs(void) { game_ocr_process_gone(7); stop_result = game_ocr_stop(); }

/* A take inside the wait for the whole drain: 200 polls, the stop undone once and logged,
 * BUSY; the take, woken by the shutdown, re-checks after the undo, gives its count back and
 * answers 1 without a second wait; the next job goes through. */
static void a_failed_stop_keeps_the_transport(void)
{
    VjoGameOcrRequest r = {0};
    setup(); caller = 7; during_event_wait = module_stop_runs; stop_result = 99;
    uint64_t t0 = model_clock;
    unsigned k0 = klogs;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1);
    TEST_CHECK(stop_result == VJO_ERR_BUSY && delays == 200 && model_clock - t0 == 200000);
    TEST_MSG("drain: %u delays, %llu us", delays, (unsigned long long)(model_clock - t0));
    TEST_CHECK(klogs - k0 == 1 && strstr(last_klog, "stop undone"));
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0 && !stopping); VJO_UNLOCK();
    caller = 7; TEST_CHECK(vjoOcrRegister() == 0);
    uint32_t seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* the same with a reader: the game exits during the read, the stop gives up; CANCELLED, the
     * count back, a new job works */
    setup(); seq = submit(); take(seq); caller = 3; during_event_wait = game_exits_then_module_stop_runs;
    stop_result = 99; k0 = klogs;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && result_waits == 1);
    TEST_CHECK(stop_result == VJO_ERR_BUSY && delays == 200 && klogs - k0 == 1);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0 && !stopping); VJO_UNLOCK();
    caller = 7; TEST_CHECK(vjoOcrRegister() == 0);
    seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    /* an undone stop keeps a result the Shell has not read yet: no invalidation */
    setup(); seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 7; during_event_wait = module_stop_runs; stop_result = 99;
    TEST_CHECK(vjoOcrTake(&r) == 1 && stop_result == VJO_ERR_BUSY);
    caller = 3; memset(&message, 0, sizeof(message)); message.size = sizeof(message);
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
}

/* The drain's paths: a job queued or claimed refuses the stop (nothing changes); no waiter:
 * no delay; a waiter leaving after 3 polls: 3 polls and 1 ms; a woken waiter that leaves before
 * the first poll: still the 1 ms; a successful stop refuses the worker from then on. */
static unsigned locks_seen, leave_at;
static uint64_t left_at;
static void waiter_leaves_on_lock(void)
{
    if (++locks_seen == leave_at) { waiters = 0; left_at = model_clock; }
}
static void stop_drains_its_waiters(void)
{
    setup(); uint32_t seq = submit(); (void)seq;
    TEST_CHECK(game_ocr_stop() == VJO_ERR_BUSY && !stopping && !delays);
    setup();
    TEST_CHECK(game_ocr_stop() == 0 && stopping && !delays);
    caller = 7; TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    setup(); waiters = 1; locks_seen = 0; leave_at = 5; before_lock = waiter_leaves_on_lock;
    uint64_t t0 = model_clock;
    TEST_CHECK(game_ocr_stop() == 0);
    before_lock = NULL;
    TEST_CHECK(delays == 4 && model_clock - t0 == 4000 && model_clock - left_at == 1000);
    TEST_MSG("%u delays, %llu us, %llu us after the count went to 0", delays, (unsigned long long)(model_clock - t0),
             (unsigned long long)(model_clock - left_at));
    TEST_CHECK(job_bit && result_bit); /* the shutdown woke both kinds */
    setup(); waiters = 1; locks_seen = 0; leave_at = 2; before_lock = waiter_leaves_on_lock;
    TEST_CHECK(game_ocr_stop() == 0);
    before_lock = NULL;
    TEST_CHECK(delays == 1 && model_clock - left_at == 1000);
    TEST_MSG("%u delays, %llu us after the count went to 0", delays, (unsigned long long)(model_clock - left_at));
    /* a drain that waited and then succeeded keeps the stop: the undo belongs to the give-up only,
     * whether the waiter leaves after 4 polls or after 150 (audit-overlay-perf-4c's case) */
    static const unsigned at[] = {5, 151};
    for (unsigned k = 0; k < 2; k++) {
        setup(); waiters = 1; locks_seen = 0; leave_at = at[k]; before_lock = waiter_leaves_on_lock;
        int rc = game_ocr_stop();
        before_lock = NULL;
        TEST_CHECK(rc == 0 && stopping && delays == at[k] - 1);
        TEST_MSG("leave at lock %u: rc %d, stopping %d, %u delays", at[k], rc, stopping, delays);
        caller = 7; TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    }
}

#if VJO_DEBUG /* the frame pacing: the debug build's (docs/release-variants.md) */
/* API 22 (ocr-speed): the game's frame pacing per job, as one step the kernel adds
 * to the job's result: the frames during the job (from its claim to its
 * completion) and those since the job before, their longest gap, their sum, the
 * gaps over VJO_PACE_SLOW_US. */
static void frame_pacing_step(void)
{
    setup();
    uint32_t seq = submit();
    int64_t t = 1000000;
    for (unsigned i = 0; i < 10; i++) game_ocr_frame(t += 16667); /* before the job: 60 fps */
    take(seq);
    for (unsigned i = 0; i < 6; i++) game_ocr_frame(t += 16667);
    game_ocr_frame(t += 50001); /* one dropped frame (two at 60 fps) during the job */
    game_ocr_frame(t += 16667);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0);
    TEST_ASSERT(message.n_steps == 1);
    const VjoGameOcrStep *p = &message.step[0];
    TEST_CHECK(p->id == VJO_GSTEP_PACE && p->arg == 8 && p->rc == 50001 && p->sum == 1);
    TEST_CHECK(p->bytes == 7u * 16667u + 50001u);
    TEST_CHECK(p->us == 9 && p->free_main == 9 * 16667 && p->free_cdram == 16667 && p->free_phycont == 0);
    TEST_MSG("pace: job %u frames, max %d, sum %u, slow %u; before %u frames, sum %d, max %d", p->arg, (int)p->rc,
             (unsigned)p->bytes, (unsigned)p->sum, (unsigned)p->us, (int)p->free_main, (int)p->free_cdram);
    /* the next job's window starts again; a full step log gets no pacing step */
    seq = submit();
    game_ocr_frame(t += 16667);
    take(seq);
    message.n_steps = VJO_GAME_OCR_STEPS;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && message.n_steps == VJO_GAME_OCR_STEPS);
    /* with #102's hand-off: a read waiting for the result (MEV_RESULT) wakes to the result with its
     * pacing step already in it (the step is written before the bit is set, under the lock) */
    setup(); seq = submit();
    game_ocr_frame(t += 16667);
    take(seq);
    game_ocr_frame(t += 16667);
    game_ocr_frame(t += 16667);
    memset(&message, 0, sizeof(message)); message.size = sizeof(message); message.seq = seq;
    during_event_wait = game_completes; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && result_waits == 1 && result_events == 1);
    TEST_CHECK(message.n_steps == 1 && message.step[0].id == VJO_GSTEP_PACE && message.step[0].arg == 2);
    TEST_MSG("%u steps, the first id %u arg %u", (unsigned)message.n_steps, (unsigned)message.step[0].id,
             (unsigned)message.step[0].arg);
}

/* API 22: the pacing windows follow the claim, not every take (#102's takes run about every
 * 100 ms while the worker is idle); a cancelled job ends its window as a finished one does;
 * and the step is in the result before any other thread can read it (it is written under
 * g.lock, before vjoOcrComplete unlocks). (audit-ocr-speed-2-c K-S1) */
static uint32_t read_steps_at_unlock, read_rc_at_unlock;
static void shell_reads_now(void)
{
    VjoGameOcrResult got;
    caller = 3;
    memset(&got, 0, sizeof(got));
    read_rc_at_unlock = (uint32_t)vjoOcrRead(request.seq, &got);
    read_steps_at_unlock = got.n_steps;
}
static void pacing_windows_follow_the_claim(void)
{
    VjoGameOcrRequest r = {0};
    int64_t t = 1000000;
    /* frames, then an idle take (nothing queued: the bounded wait, rc 1), more frames, then the job */
    setup();
    for (unsigned i = 0; i < 5; i++) game_ocr_frame(t += 16667);
    caller = 7;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1);
    for (unsigned i = 0; i < 4; i++) game_ocr_frame(t += 16667);
    uint32_t seq = submit();
    take(seq);
    game_ocr_frame(t += 16667);
    game_ocr_frame(t += 16667);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0);
    TEST_ASSERT(message.n_steps == 1 && message.step[0].id == VJO_GSTEP_PACE);
    /* before the job: 9 frames, 8 gaps, the idle take's included; during it: the 2 after the claim */
    TEST_CHECK(message.step[0].us == 8 && message.step[0].free_main == 8 * 16667);
    TEST_CHECK(message.step[0].arg == 2 && message.step[0].bytes == 2u * 16667u);
    TEST_MSG("before %u frames (sum %d), during %u (sum %u)", (unsigned)message.step[0].us,
             (int)message.step[0].free_main, (unsigned)message.step[0].arg, (unsigned)message.step[0].bytes);
    /* a cancelled job: its window ends at its completion as a finished job's does, so the next
     * job's "before" holds only the frames after it */
    seq = submit();
    take(seq);
    for (unsigned i = 0; i < 3; i++) game_ocr_frame(t += 16667);
    caller = 3; TEST_CHECK(vjoOcrCancel(seq) == 0);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    for (unsigned i = 0; i < 6; i++) game_ocr_frame(t += 50000);
    seq = submit();
    take(seq);
    game_ocr_frame(t += 16667);
    memset(&message, 0, sizeof(message)); message.size = sizeof(message); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0);
    TEST_ASSERT(message.n_steps == 1 && message.step[0].id == VJO_GSTEP_PACE);
    TEST_CHECK(message.step[0].us == 6 && message.step[0].free_main == 6 * 50000 && message.step[0].free_phycont == 6);
    TEST_CHECK(message.step[0].arg == 1);
    TEST_MSG("after the cancelled job: before %u frames, sum %d, slow %d; during %u", (unsigned)message.step[0].us,
             (int)message.step[0].free_main, (int)message.step[0].free_phycont, (unsigned)message.step[0].arg);
    /* a read that runs at vjoOcrComplete's unlock already gets the step */
    seq = submit();
    take(seq);
    game_ocr_frame(t += 16667);
    memset(&message, 0, sizeof(message)); message.size = sizeof(message); message.seq = seq;
    read_steps_at_unlock = 99; read_rc_at_unlock = 99;
    caller = 7; after_unlock = shell_reads_now;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    after_unlock = NULL;
    TEST_CHECK(read_rc_at_unlock == 0 && read_steps_at_unlock == 1);
    TEST_MSG("read at the unlock: rc %u, %u step(s)", read_rc_at_unlock, read_steps_at_unlock);
}
#else
/* The normal build measures no frame pacing (docs/release-variants.md): a job's
 * result gets no step from the kernel, whatever the game's frames did. */
static void normal_build_adds_no_pacing_step(void)
{
    setup();
    uint32_t seq = submit();
    int64_t t = 1000000;
    for (unsigned i = 0; i < 10; i++) game_ocr_frame(t += 16667);
    take(seq);
    game_ocr_frame(t += 50001);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0);
    TEST_CHECK(message.n_steps == 0);
}
#endif

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
    TEST_CHECK(vjoOcrRead(seq, &message) == 1);
    take(seq);
    TEST_CHECK(vjoOcrRegister() == 0); /* Repeated registration keeps the claim. */
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrCancelled(seq) == 0);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked());
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    uint32_t second = submit();
    TEST_CHECK(second > seq && vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    take(second); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG && game_ocr_capture_busy_locked());
}

static void job_slot_pins_queued_and_claimed(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(next_submit() == VJO_ERR_BUSY && game_ocr_capture_busy_locked());
    take(seq);
    TEST_CHECK(next_submit() == VJO_ERR_BUSY && game_ocr_capture_busy_locked());
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked() && next_submit() == (int)seq + 1);
}

static void cancellation_drains_reader(void)
{
    setup(); uint32_t seq = submit(); take(seq); caller = 3;
    TEST_CHECK(vjoOcrCancel(seq) == 0);
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(next_submit() == VJO_ERR_BUSY);
    caller = 7;
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked());
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(next_submit() == (int)seq + 1);
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
    TEST_CHECK(game_ocr_capture_busy_locked());
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    g.game_active = VJO_GAME; g.ocr_epoch++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0);
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
    /* No kernel crop to go stale (API 19): the new Shell's first job is accepted. */
    TEST_CHECK(vjoOcrSubmit(&r) > 0);
}

static void cleanup_failure_and_module_replacement(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    message.cleanup_status = -1; message.rc = -12; message.text[0] = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && game_ocr_capture_busy_locked());
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && message.cleanup_status == -1);
    TEST_CHECK(next_submit() == VJO_ERR_BUSY);
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
    r.model_dir[sizeof(r.model_dir) - 1] = 'x'; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r.model_dir[sizeof(r.model_dir) - 1] = 0;
    r.flags = 0x4000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* API 23: a shot names no model */
    /* flags bit 15 is unknown: API 27's and draft #223's vertical text, which API 47 has in the speed word */
    TEST_CHECK(VJO_GAME_OCR_F_ALL == 0xFFFF7FFFu);
    r.flags = 0x8000u; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r.flags = 0;
    r.speed = 0x20; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* VJO_GAME_OCR_S_* bits 5-31 unknown (API 26: 4) */
    r.speed = VJO_GAME_OCR_S_TRIM; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* API 32: bit 6, a shot's only */
    r.speed = 0x80; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r.speed = 0x8000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r.speed = 0x20000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* (API 36: but bit 16) */
    r.speed = 0x100000; TEST_CHECK(vjoOcrSubmit(&r) > 0); /* API 43: bit 20, a trusted hook may answer (S_TRUST) */
    r.speed = 0x1000000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* (API 42: but bits 18 and 19; API 43: 20; API 50: 21-23) */
    r.speed = 0x20000u | VJO_GAME_OCR_S_HEAD_GATE; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* bit 17 still */
    r.speed = 0x2000000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* bits 21-27 (in flight elsewhere), 30-31 */
    r.speed = 0x40000000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    TEST_CHECK(VJO_GAME_OCR_S_VERTICAL == 0x30000000u && (VJO_GAME_OCR_S_ALL & VJO_GAME_OCR_S_VERTICAL) ==
                                                            VJO_GAME_OCR_S_VERTICAL);
    setup(); r = make_request();
    TEST_CHECK(VJO_GAME_OCR_S_TRIM == 0x40u); /* bit 5 is draft #228's scan screen (VJO_GAME_OCR_S_SPOT) */
    /* API 36: bit 16; bit 7 is draft #245's PSP probe, bits 8-15 draft #226's frame pacing */
    TEST_CHECK(VJO_GAME_OCR_S_DROP_BORDERS == 0x10000u);
    TEST_CHECK(VJO_GAME_OCR_S_ALL & VJO_GAME_OCR_S_DROP_BORDERS); /* an OCR job's: the submit below carries it */
    /* API 42: bit 18 the head gate, bit 19 the two-level head, an OCR job's, never a shot's */
    TEST_CHECK(VJO_GAME_OCR_S_HEAD_GATE == 0x40000u && (VJO_GAME_OCR_S_ALL & VJO_GAME_OCR_S_HEAD_GATE));
    TEST_CHECK(VJO_GAME_OCR_S_TWO_LEVEL == 0x80000u && (VJO_GAME_OCR_S_ALL & VJO_GAME_OCR_S_TWO_LEVEL));
    TEST_CHECK(!((VJO_GAME_OCR_S_HEAD_GATE | VJO_GAME_OCR_S_TWO_LEVEL) & VJO_GAME_OCR_S_SHOT_ALL));
    /* API 43: bit 20, trusted hooks, an OCR job's, never a shot's */
    TEST_CHECK(VJO_GAME_OCR_S_TRUST == 0x100000u && (VJO_GAME_OCR_S_ALL & VJO_GAME_OCR_S_TRUST) &&
               !(VJO_GAME_OCR_S_TRUST & VJO_GAME_OCR_S_SHOT_ALL));
    /* API 50: bits 21-23, the line finder's fixes; bit 17 is closed draft #271's */
    TEST_CHECK(VJO_GAME_OCR_S_LINE_FIX == 0x200000u && VJO_GAME_OCR_S_LINE_JUNK == 0x400000u &&
               VJO_GAME_OCR_S_LINE_VALLEY == 0x800000u);
    TEST_CHECK((VJO_GAME_OCR_S_ALL & 0xE00000u) == 0xE00000u && !(0xE00000u & VJO_GAME_OCR_S_SHOT_ALL));
    r.speed = VJO_GAME_OCR_S_ALL; /* helpers, the clock, no line cache, the gate, (API 36) the border rule, (API 42) the
                                   * head gate and the two-level head, (API 43) trust, (API 50) the line finder's
                                   * fixes: known */
    uint32_t seq = submit(); take(seq);
    message.size--; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.size++;
    message.text[4095] = 'x'; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.text[4095] = 0;
    message.cleanup_status = -1; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    TEST_CHECK(game_ocr_capture_busy_locked());
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

static int rows_call(uint32_t seq, unsigned y, unsigned n, uint32_t mode, uint32_t flags, unsigned stride, void *dst,
                     VjoGameOcrRowsInfo *info)
{
    VjoGameOcrRows r = {sizeof(r), seq, y, n, mode, flags, stride, 0};
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    caller = 7;
    return vjoOcrCaptureRows(&r, dst, info);
}
static unsigned grey_of(unsigned x, unsigned y)
{
    uint32_t v = game_fb[y * FB_PITCH + x];
    return (77u * (v & 0xFF) + 150u * ((v >> 8) & 0xFF) + 29u * ((v >> 16) & 0xFF)) >> 8;
}
static int rows_match(const unsigned char *dst, unsigned y0, unsigned n, unsigned stride, unsigned x0)
{
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < 32; x++)
            if (dst[r * stride + x] != grey_of(x0 + x, y0 + r)) return 0;
    return 1;
}
/* The display hook copies grey rows of the region straight into the worker's
 * block on the game's next frame; the first pass publishes the capture. No
 * kernel block is taken. */
static void rows_hook_copies_grey_and_publishes_the_capture(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    uint32_t done_before = g.done_seq;
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 16, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG, 40, user_rows, &info) == 16);
    TEST_CHECK(rows_match(user_rows, 0, 16, 40, 0) && user_rows[32] == 0xCC); /* only width bytes per row */
    TEST_CHECK(info.rc == 16 && info.fb_width == 32 && info.fb_pitch == FB_PITCH && info.fb_format == 0);
    TEST_CHECK(hook_frames == 1 && rows_events == 1 && !allocations && !kstrip_allocs && !locks && !syscall_depth);
    TEST_CHECK(g.done_seq == done_before && !capture_events); /* the pass is not over */
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 16, 16, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG | VJO_ROWS_LAST, 40, user_rows, &info) == 16);
    TEST_CHECK(rows_match(user_rows, 16, 16, 40, 0));
    TEST_CHECK(g.done_seq == request.capture_seq && g.region_seq == request.capture_seq && capture_events == 1);
    TEST_CHECK(g.crop_w == 32 && g.crop_h == 32);
    /* Later reads (the second pass, a line) need no signature. */
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 3, 5, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == 5 && rows_match(user_rows, 3, 5, 32, 0));
    TEST_CHECK(game_ocr_capture_busy_locked()); /* the job still owns the capture */
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}
/* API 41: the registered worker's thread reports a hooked line between jobs (vjoOcrHookLine): the region
 * gets a new scene, held while the line draws, the last capture stops standing for the screen, the Shell is
 * woken as for a quiet screen (outside the lock), and the next job's capture is that scene's whatever its
 * pixels; a line reported while the last still draws (a skip) wakes nobody. The Shell, another thread of
 * the game, a claimed job or a game not in front is refused. */
static void hook_line_starts_a_held_scene(void)
{
    VjoGameOcrRowsInfo info;
    uint32_t seq, before;
    setup();
    seq = submit(); take(seq);
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 32, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG | VJO_ROWS_LAST, 40, user_rows, &info) == 32);
    TEST_CHECK(g.region_seq == request.capture_seq && capture_events == 1);
    before = g.tracker.id;
    caller = 3;
    TEST_CHECK(vjoOcrHookLine(5, 24) == VJO_ERR_PERM);
    caller = 7; caller_thread = 0x7002;
    TEST_CHECK(vjoOcrHookLine(5, 24) == VJO_ERR_PERM);
    caller_thread = 0x7001;
    TEST_CHECK(vjoOcrHookLine(5, 24) == VJO_ERR_BUSY); /* its job is claimed: never during one */
    TEST_CHECK(g.tracker.id == before && !g.tracker.hook_us && g.region_seq && !quiet_events);
    message.size = sizeof(message); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* between jobs */
    caller = 7;
    TEST_CHECK(vjoOcrHookLine(5, 24) == 0 && !locks && !syscall_depth);
    TEST_CHECK(g.tracker.id == before + 1 && g.tracker.hook_us == (int64_t)model_clock && scene_hook_drawing(&g.tracker));
    TEST_CHECK(!g.region_seq && quiet_events == 1);
    /* the job the Shell starts for it: its capture is the hooked line's scene */
    seq = submit(); take(seq);
    model_clock += 1000;
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 32, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG | VJO_ROWS_LAST, 40, user_rows, &info) == 32);
    TEST_CHECK(g.capture_scene == before + 1 && g.region_seq == request.capture_seq && capture_events == 2);
    caller = 7; message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* the next line while that one is still drawing (a skip): a new scene, but nobody woken */
    before = g.tracker.id;
    model_clock += 1000;
    TEST_CHECK(vjoOcrHookLine(6, 24) == 1);
    TEST_CHECK(g.tracker.id == before + 1 && g.tracker.hook_us == (int64_t)model_clock && quiet_events == 1);
    /* once the region has settled, the next line wakes the Shell again */
    g.tracker.stable = 1; g.tracker.hook_drawn = 1;
    TEST_CHECK(vjoOcrHookLine(7, 24) == 0 && quiet_events == 2 && g.tracker.id == before + 2);
    /* another app in front: refused */
    g.game_active = 0;
    TEST_CHECK(vjoOcrHookLine(8, 24) == VJO_ERR_PERM && quiet_events == 2 && g.tracker.id == before + 2);
    g.game_active = VJO_GAME;
    caller = 3;
}
/* API 49: the take's wait is what the worker asks in out->seq when it is shorter than 100 ms (the trusted hook
 * watch's pace), at least VJO_GAME_OCR_TAKE_WAIT_MIN_US; 0, a longer one, or a copy that fails is 100 ms. A job
 * queued meanwhile is claimed at once, as before. */
static void take_waits_the_asked_time(void)
{
    VjoGameOcrRequest r = {0};
    uint64_t t0;
    setup(); caller = 7;
    r.seq = 16000; take_wait_us = 16000; t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1 && model_clock - t0 == 16000);
    r.seq = 1; take_wait_us = VJO_GAME_OCR_TAKE_WAIT_MIN_US; t0 = model_clock; /* an old job's number: floored */
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 2 && model_clock - t0 == VJO_GAME_OCR_TAKE_WAIT_MIN_US);
    r.seq = 250000; take_wait_us = 100000; t0 = model_clock; /* never longer than before */
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 3 && model_clock - t0 == 100000);
    r.seq = 16000; copy_in_fail = 1; take_wait_us = 100000; t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 4 && model_clock - t0 == 100000);
    copy_in_fail = 0;
    /* a submission during a short wait is claimed at once */
    r.seq = 16000; take_wait_us = 16000; t0 = model_clock; during_event_wait = shell_submits; caller = 7;
    TEST_CHECK(vjoOcrTake(&r) == 0 && job_waits == 5 && model_clock == t0 && claimed);
    take_wait_us = 100000;
    caller = 3;
}

/* API 49: a trusted source's line (VJO_HOOK_LINE_TRUSTED) wakes the Shell even while the last line still draws, and
 * its scene is g.hook_scene; an untrusted line after it is not (0 again). VJO_ROWS_HOOK then publishes a trusted
 * answer's capture as that scene, with no rows armed, no frame waited for and no signature: refused (FORMAT) once
 * the region shows another scene, and (ARG) with rows, a destination, other flags or for a full-frame job. */
static void trusted_hook_line_reads_at_once(void)
{
    VjoGameOcrRows q;
    VjoGameOcrRowsInfo info;
    uint32_t seq, before;
    memset(&q, 0, sizeof(q)); q.size = sizeof(q);
    memset(&info, 0, sizeof(info)); info.size = sizeof(info);
    setup();
    caller = 7;
    TEST_CHECK(vjoOcrHookLine(5, 24 | VJO_HOOK_LINE_TRUSTED) == 0 && quiet_events == 1);
    before = g.tracker.id;
    TEST_CHECK(g.hook_scene == before && scene_hook_drawing(&g.tracker));
    /* the next trusted line while that one still draws: woken all the same */
    model_clock += 1000;
    TEST_CHECK(vjoOcrHookLine(6, 30 | VJO_HOOK_LINE_TRUSTED) == 0 && quiet_events == 2);
    TEST_CHECK(g.tracker.id == before + 1 && g.hook_scene == before + 1);
    /* the Shell's job for it: the capture without pixels */
    seq = submit(); take(seq);
    caller = 7;
    q.seq = seq; q.mode = VJO_GAME_OCR_SRC_HOOK; q.flags = VJO_ROWS_HOOK;
    q.n = 1; TEST_CHECK(vjoOcrCaptureRows(&q, NULL, &info) == VJO_ERR_ARG); q.n = 0;
    TEST_CHECK(vjoOcrCaptureRows(&q, user_rows, &info) == VJO_ERR_ARG);
    q.flags = VJO_ROWS_HOOK | VJO_ROWS_SIG; TEST_CHECK(vjoOcrCaptureRows(&q, NULL, &info) == VJO_ERR_ARG);
    q.flags = VJO_ROWS_HOOK;
    TEST_CHECK(capture_events == 0 && !hook_frames && rows.state == ROWS_IDLE);
    TEST_CHECK(vjoOcrCaptureRows(&q, NULL, &info) == 0 && info.rc == 0 && !info.armed);
    TEST_CHECK(g.capture_scene == before + 1 && g.region_seq == request.capture_seq && g.done_seq == request.capture_seq);
    TEST_CHECK(capture_events == 1 && !hook_frames && !g.capture_sig.w && !locks && !syscall_depth);
    message.size = sizeof(message); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* an untrusted line since: no hook scene; a trusted answer's capture then reads the pass */
    model_clock += 1000;
    TEST_CHECK(vjoOcrHookLine(7, 24) == 1 && !g.hook_scene); /* the last one still draws: nobody woken */
    seq = submit(); take(seq); caller = 7; q.seq = seq;
    TEST_CHECK(vjoOcrCaptureRows(&q, NULL, &info) == VJO_ERR_FORMAT && capture_events == 1);
    message.seq = seq; TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* a trusted line, then the region shows another screen (a menu over it): refused */
    g.tracker.stable = 1; g.tracker.hook_drawn = 1;
    TEST_CHECK(vjoOcrHookLine(8, 24 | VJO_HOOK_LINE_TRUSTED) == 0 && g.hook_scene == g.tracker.id);
    g.tracker.hook_us = 0; g.tracker.id++; /* what scene_update does on a larger change once the line is drawn */
    seq = submit(); take(seq); caller = 7; q.seq = seq;
    TEST_CHECK(vjoOcrCaptureRows(&q, NULL, &info) == VJO_ERR_FORMAT && capture_events == 1);
    message.seq = seq; TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* a full-frame job never stands for the region */
    TEST_CHECK(vjoOcrHookLine(9, 24 | VJO_HOOK_LINE_TRUSTED) == 0);
    {
        VjoGameOcrRequest r = make_request();
        r.flags = VJO_GAME_OCR_F_FULL; caller = 3;
        seq = (uint32_t)vjoOcrSubmit(&r); TEST_CHECK((int)seq > 0); take(seq);
    }
    caller = 7; q.seq = seq;
    TEST_CHECK(vjoOcrCaptureRows(&q, NULL, &info) == VJO_ERR_ARG && capture_events == 1);
    message.seq = seq; TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3;
}

static void rows_refuse_bad_requests_without_arming(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    frames_come = 100;
    caller = 8; { VjoGameOcrRows r = {sizeof(r), seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, 0};
                  TEST_CHECK(vjoOcrCaptureRows(&r, user_rows, &info) == VJO_ERR_PERM); }
    TEST_CHECK(rows_call(seq + 1, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 0, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, VJO_GAME_OCR_ROWS_MAX + 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 30, 3, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 31, user_rows, &info) == VJO_ERR_ARG); /* stride < width */
    TEST_CHECK(rows_call(seq, 0, 1, 7, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_LAST, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 4, 1, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG, 32, user_rows, &info) == VJO_ERR_ARG);
    /* The destination must lie in one block of the caller: here, the end runs past it. */
    TEST_CHECK(rows_call(seq, 0, 32, VJO_GAME_OCR_SRC_HOOK, 0, 200, user_rows, &info) == VJO_ERR_ARG);
    user_block_ok = 0;
    TEST_CHECK(rows_call(seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(!hook_frames && rows.state == ROWS_IDLE && user_rows[0] == 0xCC && !locks && !syscall_depth);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}
/* No frame within the wait: a timeout, and nothing is armed afterwards; a
 * frame of another process or another size does not satisfy the request. */
static void rows_timeout_other_process_and_size(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    uint64_t before = model_clock;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_clock - before >= ROWS_WAIT_US && rows.state == ROWS_IDLE && user_rows[0] == 0xCC);
    frame_pid = 8; frames_come = 1000;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_TIMEOUT);
    TEST_CHECK(user_rows[0] == 0xCC && rows.state == ROWS_IDLE);
    frame_pid = 7; fb_w = 24; frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_FORMAT);
    fb_w = FB_W; fb_fmt = 0x12345678u; frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_FORMAT);
    TEST_CHECK(info.fb_format == 0x12345678u); /* the log names the format */
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}
/* Cancellation during the wait ends it; the hook copies nothing afterwards. */
static void rows_cancelled_while_waiting(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    caller = 3; TEST_CHECK(vjoOcrCancel(seq) == 0);
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_CANCELLED);
    frames_come = 1; game_frame();
    TEST_CHECK(user_rows[0] == 0xCC && rows.state == ROWS_IDLE);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}
/* The ladder's last rung: the hook fills a small kernel strip and the call
 * copies it out; the strip is freed before the call returns. */
static void rows_kernel_strip_rung(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    hook_copy_fail = 1; frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_COPY);
    TEST_CHECK(user_rows[0] == 0xCC && !kstrip_allocs);
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info) == 8);
    TEST_CHECK(rows_match(user_rows, 0, 8, 48, 0));
    TEST_CHECK(kstrip_allocs == 1 && kstrip_frees == 1 && info.kstrip_bytes == 8 * 32 && !info.kstrip_rc);
    TEST_CHECK(vjo_ledger_current(&g_kernel_ledger, VJO_KL_OCR_STRIP) == 0 &&
               vjo_ledger_peak(&g_kernel_ledger, VJO_KL_OCR_STRIP) == 4096);
    TEST_MSG("ledger current %u peak %u", (unsigned)vjo_ledger_current(&g_kernel_ledger, VJO_KL_OCR_STRIP),
             (unsigned)vjo_ledger_peak(&g_kernel_ledger, VJO_KL_OCR_STRIP));
    kstrip_alloc_fail = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info) == (int)0x80024302u);
    TEST_CHECK(info.kstrip_rc == (int)0x80024302u && rows.state == ROWS_IDLE && kstrip_frees == 1);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}
/* A pending rows request holds off the next job's Submit; the game exiting
 * clears it without any copy. */
static void rows_pending_pins_and_process_exit(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    (void)info;
    rows.state = ROWS_PENDING; rows.pid = 7; /* as a waiting syscall leaves it */
    VJO_LOCK(); TEST_CHECK(game_ocr_capture_busy_locked()); VJO_UNLOCK();
    game_ocr_process_gone(7);
    TEST_CHECK(rows.state == ROWS_IDLE);
    frames_come = 1; game_frame();
    TEST_CHECK(user_rows[0] == 0xCC);
    /* A copy in progress when the process exits: the kernel worker tidies up. */
    setup(); seq = submit(); take(seq);
    rows.state = ROWS_COPYING; rows.pid = 7;
    game_ocr_process_gone(7);
    TEST_CHECK(rows.state == ROWS_COPYING && rows.orphan);
    rows.state = ROWS_DONE;
    model_tick();
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.orphan);
}

/* A copy the hook started and has not finished when the call gives up: BUSY,
 * the destination stays the hook's (and so does a kernel strip), no new
 * request is armed meanwhile, and once the copy ends the kernel worker returns
 * the state to IDLE so the next job is not refused for the rest of the game. */
static void rows_copy_outliving_its_call(void)
{
    for (int mode = 0; mode < 2; mode++) {
        setup(); uint32_t seq = submit(); take(seq);
        VjoGameOcrRowsInfo info;
        uint32_t m = mode ? VJO_GAME_OCR_SRC_KSTRIP : VJO_GAME_OCR_SRC_HOOK;
        stall_copy = 1;
        TEST_CHECK(rows_call(seq, 0, 8, m, 0, 48, user_rows, &info) == VJO_ERR_BUSY);
        TEST_CHECK(rows.state == ROWS_COPYING && rows.abandoned && !rows.orphan);
        TEST_CHECK(kstrip_frees == 0 && kstrip_allocs == (unsigned)mode); /* the hook may still write it */
        VJO_LOCK(); TEST_CHECK(game_ocr_capture_busy_locked()); VJO_UNLOCK();
        stall_copy = 0; frames_come = 1;
        TEST_CHECK(rows_call(seq, 8, 8, m, 0, 48, user_rows, &info) == VJO_ERR_BUSY); /* not re-armed */
        TEST_CHECK(rows.state == ROWS_COPYING && frames_come == 1);
        model_tick();
        TEST_CHECK(rows.state == ROWS_COPYING); /* still copying: left alone */
        rows.state = ROWS_DONE;                 /* the hook ends its copy */
        model_tick();
        TEST_CHECK(rows.state == ROWS_IDLE && !rows.abandoned && kstrip_frees == (unsigned)mode);
        TEST_CHECK(rows_call(seq, 0, 8, m, 0, 48, user_rows, &info) == 8); /* usable again */
        caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
        VJO_LOCK(); TEST_CHECK(!game_ocr_capture_busy_locked()); VJO_UNLOCK(); /* the next job may start */
    }
}

/* N1: the destination stride is bounded, so the span the kernel checks is the
 * span the hook writes. */
static void rows_stride_is_bounded(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, VJO_MAX_W + 1, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 0x04104105u, user_rows, &info) == VJO_ERR_ARG);
    /* a stride whose span wraps 32 bits: 31 x 0x08421085 + 32 is 59 B mod 2^32, inside the block */
    TEST_CHECK(rows_call(seq, 0, 32, VJO_GAME_OCR_SRC_HOOK, 0, 0x08421085u, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(!info.armed && rows.state == ROWS_IDLE && frames_come == 1 && user_rows[0] == 0xCC);
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 48, user_rows, &info) == 8 && info.armed);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}

/* N2: a copy whose process exited is never overtaken by a new request (BUSY,
 * not armed: the worker asks again), and a process exit while its rows call is
 * still in the kernel frees nothing the call still uses. */
static void rows_process_exit_cannot_pull_state_from_a_call(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    rows.state = ROWS_COPYING; rows.pid = 9; rows.orphan = 1; /* the last game's copy, its process gone */
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 48, user_rows, &info) == VJO_ERR_BUSY);
    TEST_CHECK(!info.armed && rows.pid == 9 && frames_come == 1); /* not armed, the old copy untouched */
    rows.state = ROWS_DONE; model_tick();                      /* it ends; the kernel worker tidies */
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.orphan);
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 48, user_rows, &info) == 8 && info.armed);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* The game exits after its frame, while the kernel-strip call is still inside:
     * the strip stays until the call has copied it out. */
    setup(); seq = submit(); take(seq);
    gone_after_frame = 1; frames_come = 1;
    int rc = rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info);
    TEST_CHECK(frees_seen_at_exit == 0);                       /* process_gone freed nothing */
    TEST_CHECK(rc == 8 && kstrip_allocs == 1 && kstrip_frees == 1); /* the call freed it, after its copy */
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.in_call && !rows.orphan);
    VJO_LOCK(); TEST_CHECK(!game_ocr_capture_busy_locked()); VJO_UNLOCK();
    /* A call that never returns after its process exited: tidied after ROWS_GONE_US. */
    rows.state = ROWS_DONE; rows.in_call = 1; rows.orphan = 1; rows.gone_us = model_clock;
    model_tick();
    TEST_CHECK(rows.state == ROWS_DONE && rows.in_call);         /* not yet */
    model_clock += ROWS_GONE_US + 1; model_tick();
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.in_call && !rows.orphan);
}

/* ---- API 23: a shot (VJO_GAME_OCR_F_SHOT): the whole frame in colour, its JPEG
 * written beside the card by the kernel's worker thread ---- */
static const char SHOT_ID[] = "0123456789abcdef0123456789abcdef";
static char shot_tmp[160], shot_jpg[160];
static VjoGameOcrRequest shot_request(void)
{
    VjoGameOcrRequest r = {0};
    r.size = sizeof(r);
    r.flags = VJO_GAME_OCR_F_SHOT;
    memcpy(r.shot, SHOT_ID, sizeof(SHOT_ID));
    return r;
}
static void shot_fs_reset(void)
{
    memset(shot_fs, 0, sizeof(shot_fs));
    shot_open = shot_index = 0;
    shot_open_error = shot_write_error = shot_close_error = shot_rename_error = shot_sync_error = 0;
    shot_short_writes = 0;
    shot_trim_error = shot_claim_error = shot_last_open_flags = 0;
    spare_open_error = spare_stat_error = spare_write_error = spare_zero_write = spare_close_error = 0;
    spare_last_open_flags = spare_stat_fail_at = spare_bad_verify = 0;
    shot_next_offset = shot_trim_size = 0;
    shot_cancel_on_rename = 0;
    shot_opens = shot_writes = shot_closes = shot_renames = shot_removes = shot_syncs = 0;
    shot_trims = shot_extensions = spare_opens = spare_stats = spare_writes = spare_closes = spare_trims = 0;
    during_spare_write = NULL;
    snprintf(shot_tmp, sizeof(shot_tmp), VJO_GAME_OCR_SHOT_DIR "/%s.jpg.tmp", SHOT_ID);
    snprintf(shot_jpg, sizeof(shot_jpg), VJO_GAME_OCR_SHOT_DIR "/%s.jpg", SHOT_ID);
}
/* A shot submitted by the Shell and claimed by the worker. */
static uint32_t shot_job_keep_files(const char *id)
{
    VjoGameOcrRequest r = shot_request(), got = {0};
    int seq;
    memcpy(r.shot, id, sizeof(r.shot));
    snprintf(shot_tmp, sizeof(shot_tmp), VJO_GAME_OCR_SHOT_DIR "/%s.jpg.tmp", id);
    snprintf(shot_jpg, sizeof(shot_jpg), VJO_GAME_OCR_SHOT_DIR "/%s.jpg", id);
    caller = 3;
    TEST_ASSERT((seq = vjoOcrSubmit(&r)) > 0);
    caller = 7;
    TEST_ASSERT(vjoOcrTake(&got) == 0);
    /* the whole frame, whatever the region; the card it names, nothing else */
    TEST_CHECK(got.seq == (uint32_t)seq && got.flags == VJO_GAME_OCR_F_SHOT && !strcmp(got.shot, id));
    TEST_CHECK(got.x == 0 && got.y == 0 && got.width == FB_W && got.height == FB_H && !got.region.w);
    memset(&message, 0, sizeof(message));
    message.size = sizeof(message);
    message.seq = (uint32_t)seq;
    return (uint32_t)seq;
}
static uint32_t shot_job(void)
{
    shot_fs_reset();
    return shot_job_keep_files(SHOT_ID);
}
/* A JPEG-shaped payload: SOI and a marker first, EOI last. */
static unsigned char shot_bytes[VJO_GAME_OCR_SHOT_MAX];
static uint32_t make_jpeg(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) shot_bytes[i] = (unsigned char)(i * 31u + 7u);
    shot_bytes[0] = 0xFF; shot_bytes[1] = 0xD8; shot_bytes[2] = 0xFF;
    shot_bytes[len - 2] = 0xFF; shot_bytes[len - 1] = 0xD9;
    return len;
}
static int shot_write(uint32_t seq, uint32_t off, uint32_t n) { caller = 7; return vjoOcrShotWrite(seq, off, n, shot_bytes + off); }
static int shot_save(uint32_t seq, uint32_t n) { caller = 7; return vjoOcrShotSave(seq, n); }

static void shot_requests_name_a_card_and_nothing_else(void)
{
    VjoGameOcrRequest r;
    setup();
    g.region = (VjoRect){1000, 2000, 30000, 20000}; /* a shot ignores it */
    static const char *const bad[] = {"0123456789ABCDEF0123456789abcdef", "0123456789abcdeg0123456789abcdef",
                                      "0123456789abcdef0123456789abcde", ""};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r = shot_request();
        memset(r.shot, 0, sizeof(r.shot));
        memcpy(r.shot, bad[i], strlen(bad[i]));
        TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    }
    r = shot_request(); r.shot[32] = '0'; /* not ended where the 32 digits end */
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    /* a shot carries no job policy, no tiers and no speed but (API 32) VJO_GAME_OCR_S_TRIM */
    r = shot_request(); r.flags |= VJO_GAME_OCR_F_BLANK; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.flags |= VJO_GAME_OCR_F_PROBE; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.tiers = 1; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.speed = 1; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.speed = VJO_GAME_OCR_S_GATE; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* API 26 */
    r = shot_request(); r.speed = VJO_GAME_OCR_S_DROP_BORDERS; /* API 36: an OCR job's text, never a shot's */
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.speed = VJO_GAME_OCR_S_TRIM | VJO_GAME_OCR_S_BOOST; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.speed = VJO_GAME_OCR_S_TRIM << 1; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r = shot_request(); r.speed = 0x20; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* bit 5 */
    r = shot_request(); r.speed = VJO_GAME_OCR_S_TRUST; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* API 43: a job's */
    TEST_CHECK(VJO_GAME_OCR_S_SHOT_ALL == VJO_GAME_OCR_S_TRIM && !(VJO_GAME_OCR_S_TRIM & VJO_GAME_OCR_S_ALL));
    /* an OCR job names no card; without models it is refused as before */
    r = make_request(); memcpy(r.shot, SHOT_ID, sizeof(SHOT_ID)); TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    TEST_CHECK(!valid && !pending);
    /* API 32: a shot that trims its black bars is accepted, the bit given to the worker as it was asked */
    r = shot_request(); r.speed = VJO_GAME_OCR_S_TRIM;
    TEST_CHECK(vjoOcrSubmit(&r) > 0 && request.speed == VJO_GAME_OCR_S_TRIM);
    caller = 7; TEST_CHECK(vjoOcrTake(&r) == 0 && r.speed == VJO_GAME_OCR_S_TRIM && r.flags == VJO_GAME_OCR_F_SHOT);
    message.size = sizeof(message); message.seq = r.seq; message.text[0] = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3;
    /* F_FULL is a shot's anyway; without a model, which a shot never opens */
    r = shot_request(); r.flags |= VJO_GAME_OCR_F_FULL;
    TEST_CHECK(vjoOcrSubmit(&r) > 0 && request.width == FB_W && request.height == FB_H && !request.x && !request.y);
    caller = 7; TEST_CHECK(vjoOcrTake(&r) == 0);
    TEST_CHECK(vjoOcrModelOpen(r.seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && !model_io.seq);
    message.size = sizeof(message); message.seq = r.seq; message.text[0] = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
}

/* A shot's rows are the frame's colours, A8B8G8R8 words, and only a shot reads
 * colour; through the hook or the kernel strip. */
static int colour_rows_match(const unsigned char *dst, unsigned y0, unsigned n, unsigned stride)
{
    for (unsigned r = 0; r < n; r++)
        if (memcmp(dst + r * stride, &game_fb[(y0 + r) * FB_PITCH], FB_W * 4u)) return 0;
    return 1;
}
static void shot_rows_are_colour_and_only_a_shots(void)
{
    VjoGameOcrRowsInfo info;
    setup();
    uint32_t seq = shot_job();
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 16, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_RGBA, FB_W * 4u, user_rows, &info) == 16);
    TEST_CHECK(colour_rows_match(user_rows, 0, 16, FB_W * 4u) && hook_frames == 1 && !kstrip_allocs);
    /* refused before anything is armed: grey rows, a signature, a stride under 4 bytes a pixel */
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, FB_W, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_RGBA | VJO_ROWS_SIG, FB_W * 4u, user_rows, &info) ==
               VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_RGBA, FB_W * 4u - 1u, user_rows, &info) ==
               VJO_ERR_ARG);
    TEST_CHECK(hook_frames == 1 && rows.state == ROWS_IDLE);
    /* the ladder's last rung in colour: the kernel strip holds 4 bytes a pixel, freed by the call */
    memset(user_rows, 0xCC, sizeof(user_rows));
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 8, 16, VJO_GAME_OCR_SRC_KSTRIP, VJO_ROWS_RGBA, FB_W * 4u, user_rows, &info) == 16);
    TEST_CHECK(colour_rows_match(user_rows, 8, 16, FB_W * 4u) && info.kstrip_bytes == 16u * FB_W * 4u);
    TEST_CHECK(kstrip_allocs == 1 && kstrip_frees == 1 && g.done_seq == 11 && !capture_events); /* nothing published */
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* an OCR job reads grey only */
    seq = submit(); take(seq);
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_RGBA, FB_W * 4u, user_rows, &info) == VJO_ERR_ARG);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}

/* The JPEG in staging-buffer pieces, in order, written by the kernel worker
 * (each call waits for it) into the .tmp, then saved: closed, the card synced,
 * renamed to <card>.jpg, synced again. The job then holds no file. */
static void shot_jpeg_is_written_in_order_and_saved_under_its_name(void)
{
    setup();
    uint32_t seq = shot_job(), len = make_jpeg(70001), off = 0;
    wait_runs_worker = 1;
    while (off < len) {
        uint32_t n = len - off < VJO_GAME_OCR_SHOT_CHUNK ? len - off : VJO_GAME_OCR_SHOT_CHUNK;
        TEST_CHECK(shot_write(seq, off, n) == (int)n);
        VjoGameOcrResult progress = {0};
        caller = 3;
        TEST_CHECK(vjoOcrRead(seq, &progress) == 1 && progress.size == sizeof(progress) &&
                   progress.seq == seq && progress.shot_bytes && !progress.text[0]);
        off += n;
    }
    TEST_CHECK(shot_opens == 1 && shot_writes == 3 && shot_open && shot_entry(shot_tmp) >= 0 && shot_entry(shot_jpg) < 0);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_BUSY); /* its file is still open */
    TEST_CHECK(shot_save(seq, len) == 0);
    int i = shot_entry(shot_jpg);
    TEST_ASSERT(i >= 0);
    TEST_CHECK(shot_fs[i].len == len && !memcmp(shot_fs[i].data, shot_bytes, len) && shot_entry(shot_tmp) < 0);
    TEST_CHECK(shot_closes == 1 && shot_renames == 1 && shot_syncs == 0 && !shot_open && !model_io.seq);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
    /* and the Shell reads the shot's result as any job's */
    VjoGameOcrResult got; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &got) == 0 && got.seq == seq && !got.rc);
    /* the kernel's short writes are its own to finish: one call still writes a whole piece */
    setup(); seq = shot_job(); len = make_jpeg(5000); wait_runs_worker = 1; shot_short_writes = 1;
    TEST_CHECK(shot_write(seq, 0, len) == (int)len && shot_writes == 5 && shot_save(seq, len) == 0);
    TEST_CHECK((i = shot_entry(shot_jpg)) >= 0 && shot_fs[i].len == len && !memcmp(shot_fs[i].data, shot_bytes, len));
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}

/* A persistent reservation is claimed without discarding its allocation. The
 * final file ends at the complete JPEG's EOI, even across several chunks. */
static void shot_spare_overwrites_trims_and_refills(void)
{
    static const char *const ids[] = {SHOT_ID, "fedcba9876543210fedcba9876543210"};
    setup();
    shot_seed(SHOT_SPARE_PATH, VJO_GAME_OCR_SHOT_MAX, 0xA5);
    model_stage[0] = 0xBC; model_stage[sizeof(model_stage) - 1] = 0xCD;
    model_tick();
    TEST_CHECK(spare_opens == 1 && spare_closes == 1 && !spare_writes && !shot_open);
    TEST_CHECK(!(spare_last_open_flags & SCE_O_TRUNC));
    TEST_CHECK(model_stage[0] == 0xBC && model_stage[sizeof(model_stage) - 1] == 0xCD);
    for (unsigned round = 0; round < 2; round++) {
        uint32_t seq = shot_job_keep_files(ids[round]), len = make_jpeg(70001 + round * 1000), off = 0;
        unsigned prepared = spare_opens;
        wait_runs_worker = 1;
        shot_short_writes = round != 0;
        while (off < len) {
            uint32_t n = len - off < VJO_GAME_OCR_SHOT_CHUNK ? len - off : VJO_GAME_OCR_SHOT_CHUNK;
            TEST_ASSERT(shot_write(seq, off, n) == (int)n);
            off += n;
        }
        int i = shot_entry(shot_tmp);
        TEST_ASSERT(i >= 0);
        TEST_CHECK(shot_last_open_flags == SCE_O_WRONLY && !shot_extensions);
        TEST_CHECK(shot_fs[i].len == VJO_GAME_OCR_SHOT_MAX && !memcmp(shot_fs[i].data, shot_bytes, len));
        TEST_CHECK(shot_fs[i].data[len] == (round ? 0 : 0xA5)); /* unwritten reservation remains */
        model_tick();
        TEST_CHECK(spare_opens == prepared && shot_entry(SHOT_SPARE_PATH) < 0); /* a claimed job pins the slot */
        TEST_ASSERT(shot_save(seq, len) == 0);
        i = shot_entry(shot_jpg);
        TEST_ASSERT(i >= 0);
        TEST_CHECK(shot_trim_size == len && shot_fs[i].len == len && !memcmp(shot_fs[i].data, shot_bytes, len));
        TEST_CHECK(shot_fs[i].data[len - 2] == 0xFF && shot_fs[i].data[len - 1] == 0xD9);
        TEST_CHECK(shot_entry(shot_tmp) < 0 && !shot_open && !model_io.seq && !shot_syncs);
        caller = 7; TEST_ASSERT(vjoOcrComplete(&message) == 0);
        VjoGameOcrResult got = {0}; caller = 3; TEST_ASSERT(vjoOcrRead(seq, &got) == 0);
        model_clock += 1000000u;
        model_tick();
        i = shot_entry(SHOT_SPARE_PATH);
        TEST_ASSERT(i >= 0);
        TEST_CHECK(shot_fs[i].len == VJO_GAME_OCR_SHOT_MAX && spare_opens == prepared + 1 &&
                   spare_writes == round + 1 && !shot_open && !allocations);
        for (unsigned n = 0; n < 3; n++) model_tick();
        TEST_CHECK(spare_opens == prepared + 1 && spare_writes == round + 1); /* ready: no repeated I/O */
    }
    TEST_CHECK(shot_trims == 2 && shot_renames == 4);
}

static void shot_spare_waits_for_shell_and_idle(void)
{
    setup();
    g.shell_pid = 0;
    model_tick();
    TEST_CHECK(!spare_opens && shot_entry(SHOT_SPARE_PATH) < 0);
    g.shell_pid = 3;
    uint32_t seq = shot_job();
    model_tick();
    TEST_CHECK(!spare_opens); /* do not prepare while a game owns the request */
    caller = 7; TEST_ASSERT(vjoOcrComplete(&message) == 0);
    VjoGameOcrResult got = {0}; caller = 3; TEST_ASSERT(vjoOcrRead(seq, &got) == 0);
    model_tick();
    TEST_CHECK(spare_opens == 1 && spare_writes == 1 && !shot_open);
    setup();
    TEST_ASSERT(game_ocr_stop() == 0);
    model_tick();
    TEST_CHECK(!spare_opens && shot_entry(SHOT_SPARE_PATH) < 0);
}

static void submit_during_slow_reserve_write(void)
{
    SceUID saved = caller;
    VjoGameOcrRequest r = make_request();
    TEST_ASSERT(shot_worker_context());
    for (unsigned round = 0; round < 2; round++) {
        caller = 3;
        TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY && !pending && !claimed && !model_io.seq && !sequence);
        VJO_LOCK();
        TEST_CHECK(game_ocr_capture_busy_locked() && game_ocr_shutdown_locked() == VJO_ERR_BUSY && !stopping);
        VJO_UNLOCK();
        model_clock += 1500000u;
    }
    caller = saved;
}

static void shot_spare_slow_prepare_keeps_the_job_unclaimed(void)
{
    setup();
    during_spare_write = submit_during_slow_reserve_write;
    uint64_t before = model_clock;
    model_tick();
    TEST_CHECK(model_clock - before == 3000000u && shot_spare_ready && !shot_spare_preparing && !shot_spare_busy);
    uint32_t seq = submit();
    take(seq);
    wait_runs_worker = 1;
    /* The model operation's own deadline starts after preparation, not while
     * its file worker is still blocked on the spare. */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES);
    TEST_CHECK(vjoOcrModelClose(seq) == 0 && !file_live);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
}

static void shot_spare_missing_or_unclaimable_falls_back(void)
{
    for (unsigned how = 0; how < 2; how++) {
        setup(); model_tick();
        int spare = shot_entry(SHOT_SPARE_PATH);
        TEST_ASSERT(spare >= 0);
        if (!how) shot_fs[spare].live = 0; /* another tool removed the closed reservation */
        else shot_claim_error = (int)0x80010005u;
        uint32_t seq = shot_job_keep_files(SHOT_ID), len = make_jpeg(9000);
        wait_runs_worker = 1;
        TEST_ASSERT(shot_write(seq, 0, len) == (int)len);
        TEST_CHECK(shot_last_open_flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC) && shot_extensions == 1);
        TEST_ASSERT(shot_save(seq, len) == 0);
        int i = shot_entry(shot_jpg);
        TEST_ASSERT(i >= 0);
        TEST_CHECK(shot_fs[i].len == len && !memcmp(shot_fs[i].data, shot_bytes, len));
        TEST_CHECK(!shot_open && !model_io.seq && !shot_spare_ready);
    }
}

static void shot_spare_failed_trim_never_publishes(void)
{
    setup(); model_tick();
    uint32_t seq = shot_job_keep_files(SHOT_ID), len = make_jpeg(9000);
    wait_runs_worker = 1;
    TEST_ASSERT(shot_write(seq, 0, len) == (int)len);
    unsigned renames = shot_renames, closes = shot_closes;
    shot_trim_error = (int)0x80010005u;
    TEST_CHECK(shot_save(seq, len) == shot_trim_error);
    TEST_CHECK(shot_open && model_io.fd == SHOT_FD && shot_closes == closes && shot_renames == renames);
    TEST_CHECK(shot_entry(shot_tmp) >= 0 && shot_entry(shot_jpg) < 0 && !model_io.saved);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_BUSY);
    TEST_ASSERT(vjoOcrModelClose(seq) == 0);
    TEST_CHECK(!shot_open && !model_io.seq && shot_entry(shot_tmp) < 0 && shot_entry(shot_jpg) < 0);
    message.rc = -113;
    TEST_CHECK(vjoOcrComplete(&message) == 0);
}

static void shot_spare_repairs_partial_files_and_retries_failures(void)
{
    for (unsigned how = 0; how < 6; how++) {
        setup();
        int i = shot_seed(SHOT_SPARE_PATH, 4096, 0xA5);
        if (how == 0) spare_open_error = (int)0x80010013u;
        if (how == 1 || how == 4) {
            spare_stat_error = (int)0x80010005u;
            spare_stat_fail_at = how == 4 ? 2 : 1;
        }
        if (how == 2) spare_write_error = (int)0x80010005u;
        if (how == 3) spare_zero_write = 1;
        if (how == 5) spare_bad_verify = 1;
        model_tick();
        TEST_CHECK_(!shot_spare_ready && !shot_open && !shot_spare_busy, "failure %u", how);
        unsigned opens = spare_opens;
        model_tick(); model_clock += 999999u; model_tick();
        TEST_CHECK(spare_opens == opens); /* a persistent error does not spin every tick */
        spare_open_error = spare_stat_error = spare_write_error = spare_zero_write = spare_bad_verify = 0;
        model_clock++;
        model_tick();
        TEST_CHECK_(shot_spare_ready && !shot_open && spare_opens == opens + 1, "retry %u", how);
        TEST_CHECK(shot_fs[i].len == VJO_GAME_OCR_SHOT_MAX && shot_fs[i].data[0] == 0xA5 &&
                   shot_fs[i].data[VJO_GAME_OCR_SHOT_MAX - 1] == 0 && !allocations);
    }
    setup();
    int i = shot_seed(SHOT_SPARE_PATH, VJO_GAME_OCR_SHOT_MAX, 0xA5);
    shot_fs[i].len += 17; /* stale oversized file: repair its length without O_TRUNC */
    model_tick();
    TEST_CHECK(shot_spare_ready && shot_fs[i].len == VJO_GAME_OCR_SHOT_MAX && spare_trims == 1 &&
               !spare_writes && shot_fs[i].data[0] == 0xA5 && !(spare_last_open_flags & SCE_O_TRUNC));
}

static void shot_spare_failed_close_is_owned_until_cleanup(void)
{
    setup();
    spare_close_error = (int)0x80010005u;
    model_tick();
    TEST_CHECK(shot_open && shot_spare_fd == SHOT_FD && shot_spare_busy && !shot_spare_ready);
    TEST_CHECK(spare_opens == 1 && spare_closes == 1 && spare_writes == 1);
    TEST_CHECK(game_ocr_stop() == VJO_ERR_BUSY && !stopping);
    model_tick();
    TEST_CHECK(spare_closes == 1 && spare_opens == 1); /* retained close respects the backoff */
    model_clock += 1000000u;
    model_tick();
    TEST_CHECK(spare_closes == 2 && spare_opens == 1 && shot_spare_busy && !shot_spare_ready);
    spare_close_error = 0;
    model_clock += 1000000u;
    model_tick();
    TEST_CHECK(!shot_open && shot_spare_fd < 0 && !shot_spare_busy && shot_spare_ready);
    TEST_CHECK(spare_opens == 1 && spare_closes == 3 && spare_writes == 1);
    TEST_CHECK(game_ocr_stop() == 0 && stopping);
}

static void shot_spare_close_retry_waits_for_the_active_job(void)
{
    setup();
    spare_close_error = (int)0x80010005u;
    model_tick();
    TEST_ASSERT(shot_spare_busy && !shot_spare_preparing && spare_closes == 1);
    uint32_t seq = submit();
    take(seq);
    model_clock += 1000000u;
    model_tick();
    TEST_CHECK(spare_closes == 1); /* a retained close never starts ahead of a claimed job */
    wait_runs_worker = 1;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES);
    TEST_CHECK(vjoOcrModelClose(seq) == 0 && !file_live && spare_closes == 1);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    spare_close_error = 0;
    model_tick();
    TEST_CHECK(spare_closes == 2 && !shot_open && !shot_spare_busy && shot_spare_ready);
}

static void shot_spare_close_retry_survives_cancel_and_game_exit(void)
{
    for (unsigned how = 0; how < 4; how++) {
        setup();
        spare_close_error = (int)0x80010005u;
        model_tick();
        TEST_ASSERT(shot_spare_busy && spare_closes == 1);
        uint32_t seq = submit();
        if (how & 1u) take(seq);
        if (how < 2) {
            caller = 3;
            TEST_ASSERT(vjoOcrCancel(seq) == 0);
            if (how & 1u) {
                caller = 7;
                TEST_ASSERT(vjoOcrComplete(&message) == 0); /* the cancelled worker drained */
            }
        } else {
            game_ocr_process_gone(7); /* either queued or claimed when the game exited */
        }
        /* A cancelled result can remain valid and unread without owning any
         * transport resource. It must not pin an unrelated reserve handle. */
        TEST_ASSERT(valid && cancelled && !completed && !pending && !claimed && !model_io.seq);
        TEST_CHECK(game_ocr_stop() == VJO_ERR_BUSY && !stopping);
        spare_close_error = 0;
        model_clock += 1000000u;
        model_tick();
        TEST_CHECK_(spare_closes == 2 && !shot_open && !shot_spare_busy && shot_spare_ready,
                    "cancel/exit case %u", how);
        TEST_CHECK(game_ocr_stop() == 0 && stopping);
    }
}

static void shot_writes_are_checked(void)
{
    VjoGameOcrModelInfo mi;
    setup();
    uint32_t seq = shot_job(), len = make_jpeg(40000);
    wait_runs_worker = 1;
    /* not a JPEG's start: refused, nothing opened */
    shot_bytes[1] = 0xD9;
    TEST_CHECK(shot_write(seq, 0, 100) == VJO_ERR_FORMAT && !shot_opens && !model_io.seq);
    shot_bytes[1] = 0xD8;
    /* the first piece from 0, at most the staging buffer, at most VJO_GAME_OCR_SHOT_MAX in all */
    TEST_CHECK(shot_write(seq, 10, 100) == VJO_ERR_ARG && !model_io.seq);
    TEST_CHECK(shot_write(seq, 0, VJO_GAME_OCR_SHOT_CHUNK + 1) == VJO_ERR_ARG);
    caller = 7; TEST_CHECK(vjoOcrShotWrite(seq, 0, 100, NULL) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrShotWrite(seq, VJO_GAME_OCR_SHOT_MAX - 10, 100, shot_bytes) == VJO_ERR_ARG);
    TEST_CHECK(shot_write(seq, 0, 30000) == 30000);
    /* in order: no gap, no rewrite */
    TEST_CHECK(shot_write(seq, 30001, 100) == VJO_ERR_ARG && shot_write(seq, 0, 100) == VJO_ERR_ARG);
    /* another process, another job: refused */
    caller = 8; TEST_CHECK(vjoOcrShotWrite(seq, 30000, 100, shot_bytes) == VJO_ERR_PERM);
    caller = 7; TEST_CHECK(vjoOcrShotWrite(seq + 1, 30000, 100, shot_bytes) == VJO_ERR_ARG);
    /* the shot's file is never read back, and a shot opens no model */
    caller = 7; TEST_CHECK(vjoOcrModelRead(seq, 0, 100, user_rows) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &mi) == VJO_ERR_ARG);
    /* saved only whole: every byte written, the EOI last */
    TEST_CHECK(shot_save(seq, 30000) == VJO_ERR_ARG); /* (the piece's last bytes are no EOI) */
    TEST_CHECK(shot_write(seq, 30000, len - 30000) == (int)(len - 30000));
    TEST_CHECK(shot_save(seq, len - 1) == VJO_ERR_ARG && shot_save(seq, len + 1) == VJO_ERR_ARG && !shot_renames);
    TEST_CHECK(shot_save(seq, len) == 0 && shot_entry(shot_jpg) >= 0);
    TEST_CHECK(shot_save(seq, len) == VJO_ERR_ARG); /* (saved: nothing open any more) */
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* an OCR job writes no JPEG */
    seq = submit(); take(seq);
    TEST_CHECK(shot_write(seq, 0, 100) == VJO_ERR_ARG && !model_io.seq);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}

/* An unsaved shot leaves nothing: its Close, a cancellation or the game's exit
 * close and remove the .tmp; a failed write, close or rename does too. */
static void an_abandoned_shot_leaves_no_file(void)
{
    for (int how = 0; how < 10; how++) {
        setup();
        uint32_t seq = shot_job(), len = make_jpeg(9000);
        wait_runs_worker = 1;
        if (how == 3) shot_open_error = (int)0x80010013u;
        if (how == 4) shot_write_error = (int)0x80010005u;
        int w = shot_write(seq, 0, len);
        TEST_CHECK(w == (how == 3 ? shot_open_error : how == 4 ? shot_write_error : (int)len));
        if (how == 0) { caller = 7; TEST_CHECK(vjoOcrModelClose(seq) == 0); }
        if (how == 1) { caller = 3; TEST_CHECK(vjoOcrCancel(seq) == 0); model_tick(); }
        if (how == 2) { game_ocr_process_gone(7); model_tick(); }
        if (how == 3 || how == 4) { caller = 7; TEST_CHECK(vjoOcrModelClose(seq) == 0); }
        if (how == 9) { /* saved, but the game gave up waiting for it: its Close removes the .jpg */
            wait_runs_worker = 0;
            TEST_CHECK(shot_save(seq, len) == VJO_ERR_BUSY);
            model_tick(); /* the kernel worker saves it after all */
            TEST_CHECK(shot_entry(shot_jpg) >= 0 && shot_renames == 1);
            wait_runs_worker = 1;
            caller = 7; TEST_CHECK(vjoOcrModelClose(seq) == 0);
        }
        if (how == 8) { /* cancelled while it is renamed: the kernel removes the .jpg it made */
            shot_cancel_on_rename = seq;
            TEST_CHECK(shot_save(seq, len) == VJO_ERR_CANCELLED && shot_renames == 1);
            caller = 7; TEST_CHECK(vjoOcrModelClose(seq) == 0);
        }
        if (how >= 5 && how < 8) {
            if (how == 5) shot_close_error = (int)0x80010005u;
            if (how == 6) shot_rename_error = (int)0x80010005u; /* (was a sync's error: a save syncs nothing since #194) */
            if (how == 7) shot_rename_error = (int)0x80010011u;
            TEST_CHECK(shot_save(seq, len) == (how == 5 ? shot_close_error : shot_rename_error));
            shot_close_error = shot_rename_error = 0;
            caller = 7; TEST_CHECK(vjoOcrModelClose(seq) == 0);
        }
        TEST_CHECK_(shot_entry(shot_tmp) < 0 && shot_entry(shot_jpg) < 0 && !shot_open && !model_io.seq, "case %d", how);
        if (how != 2) { caller = 7; message.rc = -113; /* VJO_E_LOCAL_IO */ TEST_CHECK(vjoOcrComplete(&message) == 0); message.rc = 0; }
        TEST_CHECK(!game_ocr_capture_busy_locked() || how == 1 || how == 8);
    }
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
static void removed_models_are_refused(void)
{
    TEST_CHECK(sizeof(model_stage)==32768);
    /* The broker opens only the VOCR pins; the removed engines' files (the Meiki
     * recognizer, the VOCR2 trial's FL10-sized model) are unlisted, though they
     * may still be on the memory card. */
    static const char *const removed[] = {"meiki-stream-int8.mnn", "meiki-detect-int8.mnn", "VOCR2_FL10_w8.vocr"};
    for (unsigned i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        TEST_CHECK(!model_size(removed[i]));
        setup(); uint32_t seq=submit(); take(seq);
        strcpy(request.model, removed[i]);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL)==VJO_ERR_ARG && !model_opens && !file_live);
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        TEST_MSG("%s", removed[i]);
    }
}

/* A pinned name opens only as itself (no path around it) and reads end at its
 * pinned size, the last chunk included. */
static void exact_pin_keeps_canonical_path_and_final_chunk_bounds(void)
{
    TEST_CHECK(!model_size("../" VJO_VOCR3_FL10_NAME) && !model_size(VJO_VOCR3_FL10_NAME "/"));
    setup(); uint32_t seq = submit(); take(seq);
    strcpy(request.model, VJO_VOCR3_FL10_NAME);
    TEST_CHECK(model_size(VJO_VOCR3_FL10_NAME) == VJO_VOCR_FL10_BYTES);
    expected_model_size = VJO_VOCR_FL10_BYTES;
    open_model(seq);
    unsigned char tail[2] = {0xCC, 0xCC};
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES - 1, 2, tail) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES - 1, 1, tail) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES - 1, 1, tail) == 1 && tail[1] == 0xCC);
    close_model(seq);
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
/* API 20: an LM is a pinned name the kernel knows; the game names its tier
 * (which), up to the tier the Shell's request allows, never a path. */
static void lm_tiers_open_up_to_the_requested_tier(void)
{
    static const struct { uint32_t tier; const char *name; uint32_t bytes; } lms[] = {
        {1, VJO_VOCR_LM1_NAME, VJO_VOCR_LM1_BYTES}, {2, VJO_VOCR_LM2_NAME, VJO_VOCR_LM2_BYTES},
        {3, VJO_VOCR_LM3_NAME, VJO_VOCR_LM3_BYTES}, {4, VJO_VOCR_LM4_NAME, VJO_VOCR_LM4_BYTES}};
    for (unsigned allowed = 0; allowed <= 4; allowed++)
        for (unsigned i = 0; i < 4; i++) {
            setup(); VjoGameOcrRequest r = make_request();
            r.flags = allowed << VJO_GAME_OCR_LM_SHIFT;
            caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
            uint32_t which = VJO_GAME_OCR_OPEN_LM + lms[i].tier - 1u;
            if (lms[i].tier > allowed) {
                TEST_CHECK(vjoOcrModelOpen(seq, which, NULL) == VJO_ERR_ARG && !model_opens);
                TEST_MSG("tier %u opened with tier %u allowed", lms[i].tier, allowed);
            } else {
                expect_lm = lms[i].name; expected_model_size = lms[i].bytes;
                TEST_CHECK(vjoOcrModelOpen(seq, which, NULL) == VJO_ERR_BUSY);
                model_tick();
                TEST_CHECK(vjoOcrModelOpen(seq, which, NULL) == (int)lms[i].bytes && model_opens == 1);
                close_model(seq);
                expect_lm = NULL;
            }
            TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        }
    setup(); VjoGameOcrRequest r = make_request(); /* past the last tier */
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_LM + 4u, NULL) == VJO_ERR_ARG && !model_opens);
    TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
}

/* API 20: the cascade's larger model is a pinned name the kernel knows; the
 * game may open it only when the request gives the cascade a time ceiling. */
static void cascade_model_opens_only_with_a_ceiling(void)
{
    for (unsigned ceiling = 0; ceiling <= 1; ceiling++) {
        setup(); VjoGameOcrRequest r = make_request();
        r.flags = ceiling ? 8u << VJO_GAME_OCR_CASCADE_SHIFT : 0u;
        caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
        if (!ceiling) {
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == VJO_ERR_ARG && !model_opens);
        } else {
            expect_lm = VJO_VOCR_CASCADE_NAME; expected_model_size = VJO_VOCR_CASCADE_BYTES;
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == VJO_ERR_BUSY);
            model_tick();
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == (int)VJO_VOCR_CASCADE_BYTES &&
                       model_opens == 1);
            close_model(seq);
            expect_lm = NULL;
        }
        /* the slots between the LM tiers and it name nothing */
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_LM + VJO_VOCR_LM_TIERS, NULL) == VJO_ERR_ARG);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE + 1u, NULL) == VJO_ERR_ARG);
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    }
}

/* API 47: the vertical model is a pinned name the kernel knows (its length the pin's), opened as
 * VJO_GAME_OCR_OPEN_VERTICAL by a job whose request has a vertical mode (VJO_GAME_OCR_S_VERTICAL: on or auto), by
 * tiers (the Shell's) or by name, and by no other job; a shot cannot carry the field. */
static void vertical_model_opens_only_for_a_vertical_job(void)
{
    TEST_CHECK(model_size(VJO_VOCR_VERTICAL_NAME) == VJO_VOCR_VERTICAL_BYTES && VJO_VOCR_VERTICAL_BYTES == 819792u);
    for (unsigned vertical = 0; vertical <= VJO_GAME_OCR_VERTICAL_AUTO; vertical++)
        for (unsigned by_tiers = 0; by_tiers <= 1; by_tiers++) {
            setup(); VjoGameOcrRequest r = make_request();
            r.speed = vertical << VJO_GAME_OCR_S_VERTICAL_SHIFT;
            r.tiers = by_tiers ? 3u : 0u;
            caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
            if (!vertical) {
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_VERTICAL, NULL) == VJO_ERR_ARG && !model_opens);
            } else {
                expect_lm = VJO_VOCR_VERTICAL_NAME; expected_model_size = VJO_VOCR_VERTICAL_BYTES;
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_VERTICAL, NULL) == VJO_ERR_BUSY);
                model_tick();
                TEST_CHECK_(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_VERTICAL, NULL) == (int)VJO_VOCR_VERTICAL_BYTES &&
                            model_opens == 1, "mode %u, tiers 0x%X", vertical, r.tiers);
                close_model(seq);
                expect_lm = NULL;
                model_opens = 0;
            }
            /* the values around it name nothing */
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_VERTICAL - 1u, NULL) == VJO_ERR_ARG);
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_VERTICAL + 1u, NULL) == VJO_ERR_ARG && !model_opens);
            TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        }
    /* never by a request's name: as its model or fallback, with a vertical mode or without (audit-vertical-rubi) */
    for (unsigned vertical = 0; vertical <= VJO_GAME_OCR_VERTICAL_AUTO; vertical++)
        for (uint32_t which = VJO_GAME_OCR_OPEN_MODEL; which <= VJO_GAME_OCR_OPEN_FALLBACK; which++) {
            setup(); VjoGameOcrRequest r = make_request();
            strcpy(which == VJO_GAME_OCR_OPEN_MODEL ? r.model : r.fallback, VJO_VOCR_VERTICAL_NAME);
            r.speed = vertical << VJO_GAME_OCR_S_VERTICAL_SHIFT;
            caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
            TEST_CHECK_(vjoOcrModelOpen(seq, which, NULL) == VJO_ERR_ARG && !model_opens, "flag %u, which %u", vertical,
                        (unsigned)which);
            TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        }
    /* a shot with a vertical mode is refused at Submit; the same shot without it is taken */
    setup(); VjoGameOcrRequest r = shot_request();
    r.speed |= VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    caller = 3; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG && !game_ocr_capture_busy_locked());
    r.speed &= ~VJO_GAME_OCR_S_VERTICAL;
    TEST_CHECK(vjoOcrSubmit(&r) > 0);
}

/* API 26: the text-presence gate's weights are a pinned name the kernel knows, at their pinned size; the game
 * may open them only when the request asks for the gate (VJO_GAME_OCR_S_GATE), by tiers or by name. */
static void gate_weights_open_only_with_the_gate(void)
{
    for (unsigned gate = 0; gate <= 1; gate++)
        for (unsigned tiers = 0; tiers <= 1; tiers++) {
            setup(); VjoGameOcrRequest r = make_request();
            r.speed = gate ? VJO_GAME_OCR_S_GATE : 0u;
            if (tiers) { strcpy(r.model, VJO_VOCR_TIER1_NAME); r.tiers = 1u; }
            caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
            if (!gate) {
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_GATE, NULL) == VJO_ERR_ARG && !model_opens);
            } else {
                expect_lm = VJO_VOCR_GATE_NAME; expected_model_size = VJO_VOCR_GATE_BYTES;
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_GATE, NULL) == VJO_ERR_BUSY);
                model_tick();
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_GATE, NULL) == (int)VJO_VOCR_GATE_BYTES &&
                           model_opens == 1);
                close_model(seq);
                expect_lm = NULL;
            }
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_GATE + 1u, NULL) == VJO_ERR_ARG);
            TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        }
}

/* API 21: a request by tiers names the model sizes its job may run (bit t - 1);
 * the game opens a size's pinned model by its tier, never by a name: the kernel
 * opens exactly the request's tiers, and with a cascade ceiling the tier above
 * its largest (the cascade reads with the next size up), at their pinned sizes.
 * Such a request opens nothing by name (model, fallback, cascade); one by name
 * opens no tier; unknown tier bits are refused at Submit. */
static void model_tiers_open_by_tier_only(void)
{
    static const struct { const char *name; uint32_t bytes; } pins[] = {
        {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER1_BYTES}, {VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER2_BYTES},
        {VJO_VOCR_TIER3_NAME, VJO_VOCR_TIER3_BYTES}};
    for (unsigned i = 0; i < 3; i++) /* each size's release file (test_tier_pins.h) */
        TEST_CHECK_(!strcmp(pins[i].name, test_tier_pins[i].name) && pins[i].bytes == test_tier_pins[i].bytes, "%s",
                    pins[i].name);
    /* the broker's length for each, the one it opens the file at */
    for (unsigned i = 0; i < 3; i++) TEST_CHECK_(model_size(pins[i].name) == pins[i].bytes, "%s", pins[i].name);
    /* the earlier generations' files stay openable by name (the host tools) at their own lengths */
    TEST_CHECK(model_size(VJO_VOCR3_H15_NAME) == VJO_VOCR_H15_BYTES && model_size(VJO_VOCR3_FL10_NAME) ==
               VJO_VOCR_FL10_BYTES && model_size(VJO_VOCR4_W20_NAME) == VJO_VOCR_F20_BYTES);
    TEST_CHECK(!model_size("VOCR5_X_w8.vocr") && !model_size(VJO_VOCR_TIER3_NAME ".bak") &&
               !model_size("../" VJO_VOCR_TIER3_NAME));
    for (uint32_t tiers = 1; tiers <= VJO_VOCR_TIERS_ALL; tiers++)
        for (unsigned ceiling = 0; ceiling <= 1; ceiling++) {
            uint32_t top = 0;
            for (uint32_t t = 1; t <= VJO_VOCR_TIERS; t++) if (tiers & (1u << (t - 1u))) top = t;
            for (uint32_t t = 1; t <= VJO_VOCR_TIERS + 1u; t++) {
                setup(); VjoGameOcrRequest r = make_request();
                strcpy(r.model, pins[top - 1u].name); /* the largest names the job (the kernel's log) */
                r.tiers = tiers;
                r.flags = ceiling ? 8u << VJO_GAME_OCR_CASCADE_SHIFT : 0u;
                caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
                int allowed = t <= VJO_VOCR_TIERS && ((tiers & (1u << (t - 1u))) || (ceiling && t == top + 1u));
                if (!allowed) {
                    TEST_CHECK_(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_TIER + t - 1u, NULL) == VJO_ERR_ARG &&
                                !model_opens, "tiers 0x%X ceiling %u: tier %u opened", tiers, ceiling, t);
                } else {
                    expect_lm = pins[t - 1u].name; expected_model_size = pins[t - 1u].bytes;
                    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_TIER + t - 1u, NULL) == VJO_ERR_BUSY);
                    model_tick();
                    TEST_CHECK_(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_TIER + t - 1u, NULL) == (int)pins[t - 1u].bytes &&
                                model_opens == 1, "tiers 0x%X ceiling %u: tier %u", tiers, ceiling, t);
                    close_model(seq);
                    expect_lm = NULL;
                    model_opens = 0;
                }
                /* nothing by name: not the model it names, no fallback, not the cascade's name */
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG);
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == VJO_ERR_ARG);
                TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == VJO_ERR_ARG && !model_opens);
                TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
            }
        }
    /* the LMs open by their tier as before, beside a request by tiers */
    setup(); VjoGameOcrRequest r = make_request();
    r.tiers = 3u; r.flags = 2u << VJO_GAME_OCR_LM_SHIFT;
    caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
    expect_lm = VJO_VOCR_LM2_NAME; expected_model_size = VJO_VOCR_LM2_BYTES;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_LM + 1u, NULL) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_LM + 1u, NULL) == (int)VJO_VOCR_LM2_BYTES);
    close_model(seq); expect_lm = NULL;
    TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    /* a request by name opens no tier */
    setup(); r = make_request();
    r.flags = 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    caller = 3; seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
    for (uint32_t t = 0; t <= VJO_VOCR_TIERS; t++)
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_TIER + t, NULL) == VJO_ERR_ARG && !model_opens);
    TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    /* unknown tier bits are refused at Submit, as unknown flags are */
    for (uint32_t bad = 8u; bad; bad <<= 1) {
        setup(); r = make_request(); r.tiers = 1u | bad;
        caller = 3; TEST_CHECK_(vjoOcrSubmit(&r) == VJO_ERR_ARG, "tiers 0x%X", r.tiers);
    }
}

/* The Large size's file (test_tier_pins.h: VOCR5-L's 2,473,696 B), the largest pin: the broker opens it at
 * exactly that length and reads it to its last byte in 32 KiB pieces; a file one byte
 * longer (or shorter) is refused at the open, before any byte reaches the game. */
static void the_large_size_opens_at_its_pinned_length_only(void)
{
    for (int extra = -1; extra <= 1; extra++) {
        setup(); VjoGameOcrRequest r = make_request();
        strcpy(r.model, VJO_VOCR_TIER3_NAME);
        r.tiers = VJO_VOCR_TIERS_ALL;
        caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
        expect_lm = VJO_VOCR_TIER3_NAME; expected_model_size = VJO_VOCR_TIER3_BYTES; bad_size = extra;
        TEST_CHECK(VJO_VOCR_TIER3_BYTES == test_tier_pins[2].bytes && model_size(VJO_VOCR_TIER3_NAME) == test_tier_pins[2].bytes);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_TIER + 2u, NULL) == VJO_ERR_BUSY);
        model_tick();
        int rc = vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_TIER + 2u, NULL);
        if (extra) {
            TEST_CHECK_(rc == VJO_ERR_FORMAT, "a file of %d B more: rc %d", extra, rc);
        } else {
            unsigned char last[2] = {0xCC, 0xCC};
            TEST_CHECK_(rc == (int)test_tier_pins[2].bytes, "rc %d", rc);
            /* the last byte reads; one past the end is refused */
            TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_TIER3_BYTES - 1u, 2, last) == VJO_ERR_ARG);
            TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_TIER3_BYTES - 1u, 1, last) == VJO_ERR_BUSY);
            model_tick();
            TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_TIER3_BYTES - 1u, 1, last) == 1 && last[1] == 0xCC);
        }
        bad_size = 0;
        close_model((uint32_t)seq);
        expect_lm = NULL;
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    }
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

/* API 31: an OCR job's hook pick is automatic, a stream ID (1..VJO_HOOK_STREAMS: a table site or a CALL
 * caller) or OCR only, and reaches the worker as sent; any other value, or a shot that names one (a shot
 * answers from no text), is refused before anything is queued. The grown request and result cross the
 * transport whole (their sizes, the text at its new offset). */
static void hook_pick_is_checked(void)
{
    static const uint32_t ok[] = {VJO_HOOK_PICK_AUTO, 1u, VJO_HOOK_SITE_STREAMS, VJO_HOOK_SITE_STREAMS + 1u,
                                  VJO_HOOK_STREAMS, VJO_HOOK_PICK_OCR};
    static const uint32_t bad[] = {VJO_HOOK_STREAMS + 1u, 0x80u, 0xFEu, 0x100u, 0x1FFu, 0xFFFFFFFFu};
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        setup();
        VjoGameOcrRequest r = make_request(), got = {0};
        r.hook_pick = ok[i];
        caller = 3;
        int seq = vjoOcrSubmit(&r);
        TEST_CHECK_(seq > 0, "pick 0x%X", ok[i]);
        caller = 7;
        TEST_CHECK(vjoOcrTake(&got) == 0 && got.seq == (uint32_t)seq && got.hook_pick == ok[i]);
        /* the result's new fields and its text come back as the worker wrote them */
        memset(&message, 0, sizeof(message));
        message.size = sizeof(message); message.seq = (uint32_t)seq;
        message.text_from = ok[i] <= VJO_HOOK_STREAMS ? ok[i] : 0u;
        message.hook_flags = VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR;
        message.hook_score = 1000u; message.hook_streams = 0x000FFFFFu;
        message.hook_mode = VJO_TEXT_SOURCE_AUTO; message.hook_installed = 1u << 16 | 8u; message.hook_refused = 3u;
        strcpy(message.text, "猫が");
        TEST_CHECK(vjoOcrComplete(&message) == 0);
        VjoGameOcrResult out;
        memset(&out, 0, sizeof(out));
        caller = 3;
        TEST_CHECK(vjoOcrRead((uint32_t)seq, &out) == 0);
        TEST_CHECK(out.size == sizeof(out) && !strcmp(out.text, "猫が") && out.text_from == message.text_from &&
                   out.hook_flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR) && out.hook_score == 1000u &&
                   out.hook_streams == 0x000FFFFFu && out.hook_mode == VJO_TEXT_SOURCE_AUTO &&
                   out.hook_installed == (1u << 16 | 8u) && out.hook_refused == 3u);
    }
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setup();
        VjoGameOcrRequest r = make_request();
        r.hook_pick = bad[i];
        caller = 3;
        TEST_CHECK_(vjoOcrSubmit(&r) == VJO_ERR_ARG && !pending && !valid, "pick 0x%X", bad[i]);
    }
    /* The combined interface keeps a screenshot's source pick empty, with trimming both off and on.
     * Exercise submission and Take together: accepting TRIM must not bypass hook-pick validation. */
    static const uint32_t shot_picks[] = {1u, VJO_HOOK_STREAMS, VJO_HOOK_PICK_OCR,
                                          VJO_HOOK_STREAMS + 1u, 0xFFFFFFFFu};
    for (unsigned trim = 0; trim < 2; trim++) {
        setup();
        VjoGameOcrRequest r = shot_request(), got = {0};
        r.speed = trim ? VJO_GAME_OCR_S_TRIM : 0u;
        caller = 3;
        for (unsigned i = 0; i < sizeof(shot_picks) / sizeof(shot_picks[0]); i++) {
            r.hook_pick = shot_picks[i];
            TEST_CHECK_(vjoOcrSubmit(&r) == VJO_ERR_ARG && !pending && !valid,
                        "shot speed 0x%X, pick 0x%X", r.speed, r.hook_pick);
        }
        r.hook_pick = VJO_HOOK_PICK_AUTO;
        int seq = vjoOcrSubmit(&r);
        TEST_ASSERT(seq > 0);
        caller = 7;
        TEST_CHECK(vjoOcrTake(&got) == 0 && got.size == sizeof(got) && got.seq == (uint32_t)seq &&
                   got.speed == r.speed && got.hook_pick == VJO_HOOK_PICK_AUTO && !strcmp(got.shot, SHOT_ID));
    }
    /* The new fields sit before the text, which stays the struct's last field (its NUL check and the
     * Shell's vjo_text_raw read the whole buffer). */
    TEST_CHECK(offsetof(VjoGameOcrResult, hook_refused) < offsetof(VjoGameOcrResult, text) &&
               offsetof(VjoGameOcrResult, text) + sizeof(((VjoGameOcrResult *)0)->text) == sizeof(VjoGameOcrResult));
    TEST_CHECK(offsetof(VjoGameOcrRequest, hook_pick) + sizeof(uint32_t) == sizeof(VjoGameOcrRequest));
}

static void hook_cache_persists_with_bounded_broker(void)
{
    unsigned char data[VJO_GAME_OCR_HOOK_CACHE_BYTES], readback[sizeof(data)];
    setup();
    caller = 7;
    TEST_CHECK(vjoOcrHookCache(1, 0, data) == VJO_ERR_PERM); /* no claimed job */
    uint32_t seq = submit(); take(seq);
    wait_runs_worker = 1;
    caller = 3;
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == VJO_ERR_PERM);
    caller = 7;
    TEST_CHECK(vjoOcrHookCache(seq, 2, data) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrHookCache(seq, 0, NULL) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == 0 && !model_io.seq); /* absent, including backup */
    memset(data, 0x51, sizeof(data));
    shot_short_writes = 1;
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == 0 && !model_io.seq);
    TEST_CHECK(shot_writes == 4 && !shot_open && shot_entry(HOOK_CACHE_TEMP) < 0);
    int main = shot_entry(HOOK_CACHE_PATH);
    TEST_ASSERT(main >= 0);
    TEST_CHECK(shot_fs[main].len == sizeof(data) && !memcmp(shot_fs[main].data, data, sizeof(data)));
    short_read = 1;
    memset(readback, 0, sizeof(readback));
    TEST_CHECK(vjoOcrHookCache(seq, 0, readback) == sizeof(data) && !memcmp(data, readback, sizeof(data)));
    /* Replacement retains the last committed version, without renaming over any destination. */
    memset(data, 0x62, sizeof(data));
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == 0);
    int backup = shot_entry(HOOK_CACHE_BACKUP);
    TEST_ASSERT(backup >= 0);
    TEST_CHECK(shot_fs[backup].data[0] == 0x51 && shot_entry(HOOK_CACHE_TEMP) < 0);
    main = shot_entry(HOOK_CACHE_PATH);
    TEST_ASSERT(main >= 0);
    shot_fs[main].live = 0; /* interrupted after old->backup, before temp->main */
    TEST_CHECK(vjoOcrHookCache(seq, 0, readback) == sizeof(data) && readback[0] == 0x51);
    shot_rename_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == shot_rename_error);
    shot_rename_error = 0;
    TEST_CHECK(vjoOcrHookCache(seq, 0, readback) == sizeof(data) && readback[0] == 0x51);
    TEST_CHECK(!allocations && !shot_syncs && !model_io.seq);
}

static void hook_cache_failures_preserve_previous_file(void)
{
    unsigned char data[VJO_GAME_OCR_HOOK_CACHE_BYTES];
    setup();
    uint32_t seq = submit(); take(seq); wait_runs_worker = 1;
    memset(data, 0x71, sizeof(data));
    TEST_ASSERT(vjoOcrHookCache(seq, 1, data) == 0);
    int main = shot_entry(HOOK_CACHE_PATH);
    TEST_ASSERT(main >= 0);
    memset(data, 0x88, sizeof(data));
    copy_in_fail = 1;
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == VJO_ERR_COPY && !model_io.seq);
    copy_in_fail = 0; shot_write_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == shot_write_error && !model_io.seq);
    TEST_CHECK(shot_fs[main].data[0] == 0x71 && shot_entry(HOOK_CACHE_TEMP) < 0);
    shot_write_error = 0; shot_rename_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == shot_rename_error && !model_io.seq);
    TEST_CHECK(shot_fs[main].data[0] == 0x71 && shot_entry(HOOK_CACHE_TEMP) < 0);
    shot_rename_error = 0; shot_close_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrHookCache(seq, 1, data) == shot_close_error && model_io.seq && shot_open);
    shot_close_error = 0;
    TEST_CHECK(vjoOcrModelClose(seq) == 0 && !model_io.seq && !shot_open);
    TEST_CHECK(shot_fs[main].data[0] == 0x71 && shot_entry(HOOK_CACHE_TEMP) < 0);
    copy_out_fail = 1;
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == VJO_ERR_COPY && model_io.seq);
    copy_out_fail = 0;
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == sizeof(data) && data[0] == 0x71 && !model_io.seq);
    /* Wrong-size and truncated files cannot leak partial data into a valid cache. */
    shot_fs[main].len--;
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == VJO_ERR_FORMAT && !model_io.seq);
    shot_fs[main].len++; eof = 1;
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == VJO_ERR_FORMAT && !model_io.seq);
    eof = 0; during_io = 1;
    TEST_CHECK(vjoOcrHookCache(seq, 0, data) == VJO_ERR_CANCELLED);
    TEST_CHECK(vjoOcrModelClose(seq) == 0 && !model_io.seq && !shot_open);
    TEST_CHECK(shot_fs[main].data[0] == 0x71 && !shot_syncs);
}

TEST_LIST = {
    {"hook cache: bounded authenticated broker and recoverable replacement", hook_cache_persists_with_bounded_broker},
    {"hook cache: copy IO size cancellation and close failures", hook_cache_failures_preserve_previous_file},
    {"one worker thread per game: a second copy's is refused (#138)", second_worker_thread_is_refused},
    {"API 41: a hooked line starts a held scene; only the worker's thread, between jobs", hook_line_starts_a_held_scene},
    {"API 31: the hook pick is checked; the grown request and result cross whole", hook_pick_is_checked},
    {"hand-off: an idle take waits for a submission, bounded", take_wakes_on_submit},
    {"API 49: the take waits the time the worker asks, floored, never longer", take_waits_the_asked_time},
    {"API 49: a trusted hook line wakes at once; its job's capture is its scene, without pixels", trusted_hook_line_reads_at_once},
    {"hand-off: a pending read waits for the completion, bounded", read_wakes_on_complete},
    {"hand-off: a read waiting at a shutdown is counted and woken; every waited read gives its count back",
     read_waiters_are_counted_and_woken},
    {"hand-off: a failed module stop keeps the transport working", a_failed_stop_keeps_the_transport},
    {"hand-off: module_stop's drain: refused, no waiter, waiters leaving, the grace", stop_drains_its_waiters},
    {"hand-off: the waiter count changes under the lock; the re-check sees a suspend", waiter_count_changes_under_the_lock},
    {"model syscalls wait for the kernel worker: Open Read32K Close each finish in one call", model_syscalls_wait_for_the_worker},
    {"model wait is bounded; BUSY still means repeat; a stale completion is re-checked", model_wait_is_bounded},
    {"Open reports the file modification time; a failed stat leaves it unknown and retryable", model_open_reports_modification_time},
    {"cancellation during a waited Open is reported and Close drains in one call", model_cancel_while_waiting},
    {"packed modification times differ for every field", modification_times_pack_distinctly},
    {"the fallback is one of the submitted pinned names; Open picks which", fallback_is_one_of_the_submitted_pinned_names},
    {"an LM tier opens by its pinned name, up to the request's tier", lm_tiers_open_up_to_the_requested_tier},
    {"the cascade's model opens by its pinned name, only with a ceiling", cascade_model_opens_only_with_a_ceiling},
    {"API 26: the gate's weights open by their pinned name, only with the gate", gate_weights_open_only_with_the_gate},
    {"model tiers open by their tier only: the request's, the cascade's next one up; none by name", model_tiers_open_by_tier_only},
    {"API 47: the vertical model opens only for a job with a vertical mode, by tiers or by name; never a shot's", vertical_model_opens_only_for_a_vertical_job},
    {"the Large size's file opens at its pinned length only (VOCR5-L: 2,473,696 B)", the_large_size_opens_at_its_pinned_length_only},
    {"removed_Meiki_and_VOCR2_models_are_not_in_the_broker_allowlist",removed_models_are_refused},
    {"exact_pin_keeps_canonical_path_and_final_chunk_bounds",exact_pin_keeps_canonical_path_and_final_chunk_bounds},
    {"model broker permissions canonical paths bounds and cleanup acknowledgement", model_permissions_paths_and_bounds},
    {"model broker short reads copy retry EOF and native errors", model_short_read_copy_retry_and_errors},
    {"model broker late I/O cancellation exit Shell restart and pending close", model_cancel_exit_restart_and_late_io},
    {"model broker failed close and automatic orphan recovery", model_failed_close_and_orphan_recovery},
    {"rows: the hook copies grey rows into the worker block; the first pass publishes the capture", rows_hook_copies_grey_and_publishes_the_capture},
    {"rows: bad requests are refused without arming", rows_refuse_bad_requests_without_arming},
    {"rows: no frame, another process's frame or another size", rows_timeout_other_process_and_size},
    {"rows: cancellation during the wait", rows_cancelled_while_waiting},
    {"rows: the kernel strip rung, freed before the call returns", rows_kernel_strip_rung},
    {"rows: a copy outliving its call is abandoned, then tidied by the kernel worker", rows_copy_outliving_its_call},
    {"rows: the destination stride is bounded (no wrapped span)", rows_stride_is_bounded},
    {"rows: a process exit never pulls state or the strip from a call", rows_process_exit_cannot_pull_state_from_a_call},
    {"rows: a pending request pins captures; process exit clears it", rows_pending_pins_and_process_exit},
    {"API 23: a shot names its card and nothing else; it reads the whole frame and opens no model",
     shot_requests_name_a_card_and_nothing_else},
    {"API 23: a shot's rows are the frame's colours, through the hook or the kernel strip; only a shot's",
     shot_rows_are_colour_and_only_a_shots},
    {"API 23: the shot's JPEG is written in order by the kernel worker and saved under the card's name",
     shot_jpeg_is_written_in_order_and_saved_under_its_name},
    {"API 23: the shot's writes are checked: a JPEG's start, in order, bounded, saved only whole",
     shot_writes_are_checked},
    {"API 23: an abandoned or failed shot leaves no file", an_abandoned_shot_leaves_no_file},
    {"screenshot reserve: overwrite without truncation, trim the whole JPEG, refill while idle",
     shot_spare_overwrites_trims_and_refills},
    {"screenshot reserve: wait for Shell registration and an idle transport", shot_spare_waits_for_shell_and_idle},
    {"screenshot reserve: a slow preparation keeps new OCR unclaimed", shot_spare_slow_prepare_keeps_the_job_unclaimed},
    {"screenshot reserve: a missing or unclaimable reserve falls back to a normal file",
     shot_spare_missing_or_unclaimable_falls_back},
    {"screenshot reserve: failed trim retains cleanup ownership and publishes no image",
     shot_spare_failed_trim_never_publishes},
    {"screenshot reserve: partial files, preparation errors and bounded retries",
     shot_spare_repairs_partial_files_and_retries_failures},
    {"screenshot reserve: a failed close retains the descriptor and refuses unload until cleanup",
     shot_spare_failed_close_is_owned_until_cleanup},
    {"screenshot reserve: retained close retries never delay a claimed OCR job",
     shot_spare_close_retry_waits_for_the_active_job},
    {"screenshot reserve: cancelled and exited jobs do not pin a retained close",
     shot_spare_close_retry_survives_cancel_and_game_exit},
#if VJO_DEBUG
    {"API 22: the frame pacing of each job, one step the kernel adds", frame_pacing_step},
    {"API 22: the pacing windows follow the claim, a cancelled job ends one, the step is written under the lock",
     pacing_windows_follow_the_claim},
#else
    {"the normal build adds no pacing step", normal_build_adds_no_pacing_step},
#endif
    {"registration and caller permissions", registration_and_permissions},
    {"completion and stale replacement", complete_and_replacement},
    {"a queued or claimed job pins the slot", job_slot_pins_queued_and_claimed},
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
