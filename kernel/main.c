/* Rubi kernel module: module entry, worker thread and the
 * syscalls used by the SceShell plugin (see include/vjo_api.h). */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "vjo_kernel.h"
#include "game_ocr.h"
#include "../core/boot_guard.h"
#include "../include/vjo_ledger_budget.h"

VjoKernelState g;

static SceUID worker_uid = -1;
static volatile int worker_run = 1;

#define POLL_US         16000   /* input poll period */
#define CHECK_EVERY     4       /* change detection every 4th poll (~64 ms) */

/* Called with the lock held. Signatures already published are skipped. One
 * the hook is still building for the old region can still arrive; it only
 * costs an extra change. */
static void reset_change_detection(void)
{
    scene_reset(&g.tracker);
    g.region_seq = 0; /* for another region */
    g.hook_scene = 0; /* API 49: the tracker's numbers start again */
    g.seen_sig_seq = g.hook_sig_seq;
}

static void change_detection(void)
{
    SceneSig sig; /* 524 B of VjoWorker's 16 KiB stack: the hook's signature, copied under the lock */
    uint32_t seq, id = 0;
    SceneEvent ev = SCENE_NONE;
    int animated = 0;
    if (g.game_pid <= 0 || !g.game_active)
        return;
    VJO_LOCK(); /* against reset_change_detection */
    seq = g.hook_sig_seq;
    if (seq != g.seen_sig_seq) {
        __sync_synchronize(); /* pairs with the hook's barrier */
        sig = g.hook_sig[seq & 1];
        __sync_synchronize();
        if (g.hook_sig_seq == seq) { /* else rewritten meanwhile: next poll */
            g.seen_sig_seq = seq;
            int64_t now = ksceKernelGetSystemTimeWide();
            /* the low 32 bits, widened: within the last 71 min */
            int64_t input_us = now - (uint32_t)((uint32_t)now - g.input_us32);
            int64_t edge_us = now - (uint32_t)((uint32_t)now - g.input_edge_us32);
            ev = scene_update(&g.tracker, &sig, now, input_us, edge_us);
            id = g.tracker.id;
            animated = g.tracker.masked;
        }
    }
    VJO_UNLOCK();
    if (ev == SCENE_QUIET) {
        klog_d("region quiet: scene %u", id);
        ksceKernelSetEventFlag(g.evf, VJO_EV_REGION_QUIET);
    } else if (ev == SCENE_SETTLED) {
        klog_d("region settled: scene %u (%d animated cells masked)", id, animated);
        ksceKernelSetEventFlag(g.evf, VJO_EV_REGION_STABLE);
    }
}

/* The kernel log goes straight to ux0:data/VitaJPOverlay/kernel.txt (flushed
 * every worker tick) so it survives a crash; the previous boot's file is kept
 * as kernel.prev.txt. Capped at 256 KB per boot (64 KB until 0.8.0-test: one long
 * console session with every OCR job could fill that, and the lines after it were lost).
 * The file is first opened once
 * SceShell has registered: earlier in boot ux0: can still be another device
 * (SD2Vita remaps it), and the rotation would happen there. Lines logged
 * before that wait in the ring. In safe mode the Shell never registers; its
 * boot mark serves the same purpose. */
#define KFILE "ux0:data/VitaJPOverlay/kernel.txt"
#define KFILE_PREV "ux0:data/VitaJPOverlay/kernel.prev.txt"
#define KFILE_CAP (256 * 1024)
static int kfile_size = -1;

void kernel_file_flush(void)
{
    char buf[512];
    int n;
    SceUID fd;
    if (kfile_size > KFILE_CAP)
        return;
    if (kfile_size < 0) {
        if (g.shell_pid <= 0 && !boot_shell_seen())
            return;
        ksceIoMkdir("ux0:data", 0777);
        ksceIoMkdir("ux0:data/VitaJPOverlay", 0777);
        ksceIoRemove(KFILE_PREV);
        ksceIoRename(KFILE, KFILE_PREV);
        fd = ksceIoOpen(KFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd < 0)
            return; /* ux0 not mounted yet: retry later */
        ksceIoClose(fd);
        kfile_size = 0;
    }
    n = klog_read(buf, sizeof(buf));
    if (n <= 0)
        return;
    fd = ksceIoOpen(KFILE, SCE_O_WRONLY | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    do {
        ksceIoWrite(fd, buf, n);
        kfile_size += n;
    } while ((n = klog_read(buf, sizeof(buf))) > 0);
    ksceIoClose(fd);
}

static int worker(SceSize args, void *argp)
{
    unsigned tick = 0;
    (void)args;
    (void)argp;
    while (worker_run) {
        unsigned int bits = 0;
        SceUInt timeout = POLL_US;
        ksceKernelWaitEventFlag(g.ievf, IEV_GAME_START | IEV_GAME_EXIT | IEV_ACTIVATE | IEV_MODEL | IEV_QUIT | IEV_POLICY,
                                SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
        if (bits & IEV_QUIT)
            break;
        /* A starting game waits for this (loader.c): first. Every round, so a
         * question is never left behind a lost wakeup. */
        loader_tick();
        game_ocr_rows_tick();
        if (bits & IEV_GAME_EXIT) {
            klog("game exit");
            ksceKernelSetEventFlag(g.evf, VJO_EV_GAME_EXIT);
        }
        if ((bits & IEV_GAME_START) && g.game_pid > 0) {
            VJO_LOCK();
            reset_change_detection();
            VJO_UNLOCK();
            ksceKernelSetEventFlag(g.evf, VJO_EV_GAME_START);
        }
        if ((bits & IEV_ACTIVATE) && g.game_pid > 0 && g.game_active) {
            VJO_LOCK();
            reset_change_detection();
            VJO_UNLOCK();
        }

        if (boot_mode() == VJO_BOOT_NORMAL) {
            input_poll();
            if (++tick % CHECK_EVERY == 0)
                change_detection();
        } else {
            /* Recovery keeps its original one worker; no extra file stack. */
            boot_tick();
            kernel_file_flush();
        }
    }
    return 0;
}

/* ---------------- syscalls ---------------- */

/* Only the registered shell process may use the syscalls that read the
 * screen or control input. */
static int caller_ok(void)
{
    return g.shell_pid > 0 && ksceKernelGetProcessId() == g.shell_pid;
}

int vjoGetVersion(void)
{
    return VJO_API_VERSION;
}

/* Module name of the SceShell plugin (shell/exports.yml). */
#define SHELL_MODULE "VitaJPOverlay_Shell"

/* Registers the caller as the shell if it has the shell plugin loaded.
 * SceShell can restart without a reboot (e.g. after a crash), so a new
 * registration that passes the check replaces the old one, and the old
 * shell's game state is reset. */
int vjoRegisterShell(void)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid;
    int ret = 0;
    ENTER_SYSCALL(state);
    pid = ksceKernelGetProcessId();
    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(pid, SHELL_MODULE, &info) < 0) {
        klog("shell registration refused: pid 0x%X has no " SHELL_MODULE, pid);
        ret = VJO_ERR_PERM;
    } else if (g.shell_pid != pid) {
        klog("shell registered: pid 0x%X (was 0x%X)", pid, g.shell_pid);
        VJO_LOCK();
        g.shell_pid = pid;
        game_ocr_shell_changed_locked();
        VJO_UNLOCK();
        g.input_block = 0;
        if (g.game_pid > 0)
            ksceKernelSetEventFlag(g.evf, VJO_EV_GAME_START); /* re-classify the foreground app */
    }
    EXIT_SYSCALL(state);
    return ret;
}

int vjoWaitEvent(uint32_t mask, uint32_t *out, SceUInt32 timeout_us)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    unsigned int bits = 0;
    SceUInt to = timeout_us;
    int ret;
    ENTER_SYSCALL(state);
    ret = ksceKernelWaitEventFlag(g.evf, mask & VJO_EV_ALL, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                  &bits, timeout_us ? &to : NULL);
    if (out)
        ksceKernelMemcpyKernelToUser(out, &bits, sizeof(bits));
    EXIT_SYSCALL(state);
    return ret;
}

int vjoNotifyShell(void)
{
    uint32_t state;
    if (!caller_ok()) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    int ret = ksceKernelSetEventFlag(g.evf, VJO_EV_SHELL);
    EXIT_SYSCALL(state);
    return ret;
}

int vjoGetState(VjoState *out)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    VjoState s;
    uint32_t size = 0;
    ENTER_SYSCALL(state);
    memset(&s, 0, sizeof(s));
    ksceKernelMemcpyUserToKernel(&size, out, sizeof(size));
    if (size > sizeof(s))
        size = sizeof(s);
    s.size = sizeof(s);
    s.game_pid = g.game_pid;
    s.fb_width = g.fb_w;
    s.fb_height = g.fb_h;
    s.fb_pitch = g.fb_pitch;
    s.fb_pixelformat = g.fb_fmt;
    VJO_LOCK();
    s.scene = g.tracker.id;
    s.stable = g.tracker.stable;
    s.unsettled_ms = (uint32_t)(scene_unsettled_us(&g.tracker, ksceKernelGetSystemTimeWide()) / 1000);
    /* API 41: a hooked line still drawing is worth reading too (vjoOcrHookLine) */
    s.quiet = (g.tracker.quiet || scene_hook_drawing(&g.tracker)) && !g.tracker.stable;
    /* 0 while a region capture rewrites capture_sig */
    if (g.region_seq) {
        s.region_seq = g.region_seq;
        s.region_scene = scene_match_since(&g.tracker, &g.capture_sig, g.region_us);
    }
    /* API 49: while the region still shows the trusted line's scene (the tracker holds a hooked line's number
     * while it draws and through an icon's changes, kernel/scene.h) */
    if (g.hook_scene == g.tracker.id && g.tracker.hook_us)
        s.hook_scene = g.hook_scene;
    VJO_UNLOCK();
    s.width = g.crop_w;
    s.height = g.crop_h;
    s.capture_scene = g.capture_scene;
    s.done_seq = g.done_seq;
    if (size >= sizeof(uint32_t))
        ksceKernelMemcpyKernelToUser(out, &s, size);
    EXIT_SYSCALL(state);
    return 0;
}

int vjoSetRegion(const VjoRect *r)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    VjoRect k = {0, 0, 0, 0};
    ENTER_SYSCALL(state);
    if (r)
        ksceKernelMemcpyUserToKernel(&k, r, sizeof(k));
    VJO_LOCK();
    g.region = k;
    reset_change_detection();
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return 0;
}

/* The shell enables the kernel for the current foreground process once it
 * has checked that it is a game (not a system app). */
int vjoSetGameActive(int pid, int mode)
{
    uint32_t state;
    int ret;
    if (!caller_ok())
        return VJO_ERR_PERM;
    if (mode < VJO_GAME_NONE || mode > VJO_GAME_STATIC_FB)
        return VJO_ERR_ARG;
    ENTER_SYSCALL(state);
    ret = lifecycle_set_game_active(pid, mode);
    EXIT_SYSCALL(state);
    return ret;
}

int vjoSetTriggers(int toggle, int subtitle)
{
    if (!caller_ok())
        return VJO_ERR_PERM;
    if (toggle < 0 || toggle >= VJO_TRIGGER_COUNT || subtitle < 0 || subtitle >= VJO_TRIGGER_COUNT ||
        toggle == subtitle)
        return VJO_ERR_ARG;
    if (g.trigger[TRIG_TOGGLE] != toggle || g.trigger[TRIG_SUBTITLE] != subtitle) {
        g.trigger[TRIG_TOGGLE] = toggle;
        g.trigger[TRIG_SUBTITLE] = subtitle;
        g.trigger_gen++;
    }
    return 0;
}

int vjoSetInputBlock(int on)
{
    if (!caller_ok()) {
        klog("input block refused: pid=%X shell=%X", ksceKernelGetProcessId(), g.shell_pid);
        return VJO_ERR_PERM;
    }
    if (on) input_trace_reset();
    if (!on && g.input_block) /* e.g. the ○ that closed the overlay */
        __sync_fetch_and_or(&g.suppress_mask, g.raw_buttons);
    __atomic_store_n(&g.input_block, on ? 1 : 0, __ATOMIC_RELEASE);
    klog_d("input block=%d pid=%X game=%X raw=%08X t=%u ms", g.input_block,
           ksceKernelGetProcessId(), g.game_pid, g.raw_buttons, klog_ms());
    return 0;
}

int vjoPollInput(VjoInput *out)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    VjoInput in;
    ENTER_SYSCALL(state);
    in = g.last_input;
    ksceKernelMemcpyKernelToUser(out, &in, sizeof(in));
    EXIT_SYSCALL(state);
    return 0;
}

/* The boot-safety guard's record of this boot, for any process: the Shell
 * and the game worker unload themselves when mode is VJO_BOOT_SAFE. */
int vjoBootInfo(VjoBootInfo *out)
{
    uint32_t state;
    VjoBootInfo info;
    uint32_t size = 0;
    ENTER_SYSCALL(state);
    ksceKernelMemcpyUserToKernel(&size, out, sizeof(size));
    boot_info(&info);
    if (size > sizeof(info))
        size = sizeof(info);
    if (size >= sizeof(uint32_t))
        ksceKernelMemcpyKernelToUser(out, &info, size);
    EXIT_SYSCALL(state);
    return 0;
}

/* The self-test's memory sample: exact partition figures the user API cannot
 * give (kernel/meminfo.c). The Shell, or the registered game worker: the
 * caller's partition is SceShell's or the game's. The normal build has no
 * self-test: the export stays (one NID table for both builds) and refuses. */
#if !VJO_DEBUG
int vjoMemInfo(VjoMemInfo *out)
{
    (void)out;
    return VJO_ERR_UNSUPPORTED;
}
#else
int vjoMemInfo(VjoMemInfo *out)
{
    uint32_t state;
    VjoMemInfo info;
    uint32_t size = 0;
    SceUID pid = ksceKernelGetProcessId(), worker = 0;
    ENTER_SYSCALL(state);
    /* The Shell, or the registered game worker (it names its own partitions).
     * The lock is released before meminfo_fill, which takes it itself. */
    if (!caller_ok()) {
        VJO_LOCK();
        worker = game_ocr_worker_pid_locked();
        VJO_UNLOCK();
        if (worker <= 0 || pid != worker) {
            EXIT_SYSCALL(state);
            return VJO_ERR_PERM;
        }
    }
    ksceKernelMemcpyUserToKernel(&size, out, sizeof(size));
    meminfo_fill(pid, out, &info);
    if (size > sizeof(info))
        size = sizeof(info);
    if (size >= sizeof(uint32_t))
        ksceKernelMemcpyKernelToUser(out, &info, size);
    EXIT_SYSCALL(state);
    return 0;
}
#endif

/* The Shell plugin reports a stage it reached (VJO_BOOT_STAGE_SHELL..OK), or
 * 0 when it unloads itself on purpose. Accepted from a process that has the
 * Shell module loaded, which is how the Shell registers too; the Shell is
 * not registered yet when it marks its first stages. */
int vjoBootMark(int stage)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid;
    int ret;
    ENTER_SYSCALL(state);
    pid = ksceKernelGetProcessId();
    info.size = sizeof(info);
    if (pid != g.shell_pid && taiGetModuleInfoForKernel(pid, SHELL_MODULE, &info) < 0) {
        ret = VJO_ERR_PERM;
    } else if (stage == 0) {
        boot_mark_inert();
        ret = 0;
    } else {
        ret = boot_mark(stage);
    }
    EXIT_SYSCALL(state);
    return ret;
}

/* ---------------- module ---------------- */

static void delete_sync_objects(void)
{
    if (g.game_lock > 0)
        ksceKernelDeleteMutex(g.game_lock);
    if (g.lock > 0)
        ksceKernelDeleteMutex(g.lock);
    if (g.ievf > 0)
        ksceKernelDeleteEventFlag(g.ievf);
    if (g.mevf > 0)
        ksceKernelDeleteEventFlag(g.mevf);
    if (g.evf > 0)
        ksceKernelDeleteEventFlag(g.evf);
    g.game_lock = g.lock = g.ievf = g.mevf = g.evf = 0;
}

static void release_hooks(void)
{
    loader_hook_release();
    lifecycle_hooks_release();
    input_hooks_release();
    display_hook_release();
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args)
{
    int safe;
    (void)argc;
    (void)args;
    memset(&g, 0, sizeof(g));
    /* Before anything else exists: a crash later in this boot leaves the
     * record at "kernel", and the next boot runs in safe mode. */
    safe = boot_init(VJO_API_VERSION) == VJO_BOOT_SAFE;
    kledger_init();
    game_ocr_init();
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_L_R;
    g.trigger[TRIG_SUBTITLE] = VJO_TRIGGER_SELECT_R;
    g.input_us32 = g.input_edge_us32 = (uint32_t)(ksceKernelGetSystemTimeWide() - 3600000000LL); /* none yet */
    g.evf = ksceKernelCreateEventFlag("VjoEvents", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    g.ievf = ksceKernelCreateEventFlag("VjoInternal", 0, 0, NULL);
    /* The game worker's model syscalls wait here for the kernel worker. */
    g.mevf = ksceKernelCreateEventFlag("VjoModelDone", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    g.lock = ksceKernelCreateMutex("VjoLock", 0, 0, NULL);
    g.game_lock = ksceKernelCreateMutex("VjoGameLock", 0, 0, NULL);
    if (g.evf < 0 || g.ievf < 0 || g.mevf < 0 || g.lock < 0 || g.game_lock < 0) {
        delete_sync_objects();
        return SCE_KERNEL_START_FAILED;
    }

    if (safe) {
        /* Safe mode: no hook touches a game or SceShell. The worker still
         * runs, to write the boot record and the kernel log; the Shell and
         * game plugins see the mode through vjoBootInfo and unload. */
        klog("hooks: none (safe mode)");
    } else {
        int d = display_hook_install(), i = input_hooks_install(), l = lifecycle_hooks_install();
        /* The worker loader (#138) is optional: without it a per-title taiHEN
         * line still loads the worker. Not without the others (1: not tried). */
        int w = d < 0 || i < 0 || l < 0 ? 1 : loader_hook_install();
        klog("hooks: display %d, input %d, procevent %d, worker loader %d (0 = ok)", d, i, l, w);
        if (d < 0 || i < 0 || l < 0) {
            /* A half-hooked kernel would block input it cannot see or
             * capture frames nobody asked for: run with no hooks instead. */
            release_hooks();
            klog("hooks: partial install released; running without hooks");
        }
    }

    if (!safe && kernel_io_start() < 0) {
        release_hooks();
        delete_sync_objects();
        return SCE_KERNEL_START_FAILED;
    }
    /* Without the worker nothing would ever be polled or finished. */
    worker_uid = ksceKernelCreateThread("VjoWorker", worker, 0x3C, VJO_STACK_KERNEL, 0, 0x10000, NULL);
    if (worker_uid >= 0)
        vjo_ledger_add(&g_kernel_ledger, VJO_KL_STACK, VJO_BUDGET_KERNEL_STACK);
    if (worker_uid < 0 || ksceKernelStartThread(worker_uid, 0, NULL) < 0) {
        if (worker_uid >= 0)
            ksceKernelDeleteThread(worker_uid);
        worker_uid = -1;
        kernel_io_stop();
        release_hooks();
        delete_sync_objects();
        return SCE_KERNEL_START_FAILED;
    }
#if VJO_DEBUG
    klog("Rubi kernel %d started (%s mode)", VJO_API_VERSION, vjo_boot_mode_name(boot_mode()));
#else
    klog("Rubi kernel %d started (%s mode, the normal build: a short log)", VJO_API_VERSION,
         vjo_boot_mode_name(boot_mode()));
#endif
    kledger_log("start");
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    /* The worker loader first: a game start inside its hook waits for the kernel
     * worker, which still answers now. One that stays fails the stop. */
    if (loader_hook_release()) return SCE_KERNEL_STOP_FAIL;
    /* Refused while a job runs; a take or read waiting in the kernel is woken and
     * leaves the module's code first (kernel/game_ocr.c game_ocr_stop). */
    if (game_ocr_stop()) return SCE_KERNEL_STOP_FAIL;
    worker_run = 0;
    if (worker_uid >= 0) {
        ksceKernelSetEventFlag(g.ievf, IEV_QUIT);
        ksceKernelWaitThreadEnd(worker_uid, NULL, NULL);
        ksceKernelDeleteThread(worker_uid);
    }
    kernel_io_stop();
    release_hooks();
    delete_sync_objects();
    return SCE_KERNEL_STOP_SUCCESS;
}
