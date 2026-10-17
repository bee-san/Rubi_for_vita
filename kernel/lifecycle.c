/* Foreground game tracking via the process event handler (as PSVshell and
 * reVita do); the state machine is in foreground.c. Buffer allocation is
 * deferred to the worker thread. A change of the foreground also releases the
 * game's input block, in the same handler (#104; fg_store). */
#include <psp2kern/kernel/sysclib.h>
#include <taihen.h>

#include "vjo_kernel.h"
#include "game_ocr.h"
#include "../core/boot_guard.h"

static tai_hook_ref_t hook_ref;
static SceUID hook_uid = -1;

/* The transitions are in foreground.c; these copy g's foreground state in
 * and out around them. Called with g.game_lock held. game_active is stored
 * before game_pid, so the lock-free readers (hooks) never see a new
 * foreground process still marked active. */
static VjoForeground fg_load(void)
{
    VjoForeground f;
    f.game_pid = g.game_pid;
    f.game_active = g.game_active;
    f.prev = g.prev_game_pid;
    f.shell_pid = g.shell_pid;
    return f;
}

/* ev: the process event (PROCEV_*), or 0 for the Shell's verdict. Out of
 * line: one copy of the call, not one per path of fg_store. */
static void __attribute__((noinline)) fg_log(SceUID was, SceUID now, int ev, int released)
{
    static const char *const why[] = {"shell", "startup", "event 2", "exit", "suspend", "resume"};
    klog("foreground 0x%X -> 0x%X (%s) t=%u ms%s", was, now, ev >= 0 && ev <= PROCEV_RESUME ? why[ev] : "event",
         klog_ms(), released ? ": input released" : "");
}

static void fg_store(const VjoForeground *f, uint32_t iev, int ev)
{
    SceUID was;
    int changed, log, released = 0;
    VJO_LOCK();
    was = g.game_pid;
    changed = was != f->game_pid || g.game_active != f->game_active;
    /* a confirmed game left, or an app was suspended or resumed */
    log = changed && (g.game_active || ev == PROCEV_SUSPEND || ev == PROCEV_RESUME);
    if (changed)
        __atomic_add_fetch(&g.ocr_epoch, 1, __ATOMIC_RELEASE);
    g.game_active = f->game_active;
    g.game_pid = f->game_pid;
    g.prev_game_pid = f->prev;
    /* #104: the input block belongs to the foreground it was set for. Any
     * change releases it here, in the change's own handler (the game
     * suspended by PS, its exit, a process in front of it, the Shell's
     * verdict), not when the Shell gets round to it: nothing blocked reaches
     * whatever comes to the front, and a resumed game starts with its input
     * free (the Shell blocks it again only when it opens the overlay). Held
     * buttons stay hidden from the game until released, as at
     * vjoSetInputBlock(0). */
    if (changed && __atomic_load_n(&g.input_block, __ATOMIC_ACQUIRE)) {
        __sync_fetch_and_or(&g.suppress_mask, g.raw_buttons); /* first, as vjoSetInputBlock(0) */
        released = __atomic_exchange_n(&g.input_block, 0, __ATOMIC_ACQ_REL) != 0;
        log |= released;
    }
    game_ocr_foreground_changed_locked();
    VJO_UNLOCK();
    if (iev)
        ksceKernelSetEventFlag(g.ievf, iev);
    if (log)
        fg_log(was, f->game_pid, ev, released);
}

static int procevent_patched(int pid, int ev, int a3, int a4, int *a5, int a6)
{
    VjoForeground f;
    uint32_t iev;
    /* SceShell dying with our plugin inside it is the failure the boot guard
     * exists for: the next boot runs in safe mode even if this one had
     * already been marked ok. */
    if (ev == PROCEV_EXIT && pid > 0 && pid == g.shell_pid)
        boot_fault(VJO_BOOT_FAULT_SHELL_EXIT);
    if (ksceKernelLockMutex(g.game_lock, 1, NULL) < 0)
        goto out;
    f = fg_load();
    iev = fg_process_event(&f, pid, ev);
    if ((iev & IEV_GAME_EXIT) && (iev & IEV_GAME_START))
        klog("pid 0x%X gone: back to game pid 0x%X", pid, f.game_pid);
    fg_store(&f, iev, ev);
    if (ev == PROCEV_EXIT) game_ocr_process_gone(pid);
    ksceKernelUnlockMutex(g.game_lock, 1);
out:
    return TAI_CONTINUE(int, hook_ref, pid, ev, a3, a4, a5, a6);
}

/* vjoSetGameActive: the shell's verdict on the foreground process. As in the
 * hook, nothing is changed without g.game_lock. It is locked with no timeout,
 * never recursively (neither holder calls this) and never cancelled, so only
 * module_stop's delete_sync_objects makes the lock fail: a thread waiting in
 * it wakes with SCE_KERNEL_ERROR_WAIT_DELETE, and a later lock names a deleted
 * id (then 0) [SDK: Kernel-Overview, Deleting Synchronization Objects]. That
 * does not come back, so the verdict is dropped, never retried: the
 * foreground stays as it was (an app the Shell has not confirmed stays
 * inactive: its input passes, no region of it is read; a confirmed game stays
 * confirmed), the Shell gets the lock's error, and the first refusal is
 * logged, once a boot. */
static int lock_refusal_logged;

int lifecycle_set_game_active(SceUID pid, int mode)
{
    VjoForeground f;
    uint32_t iev;
    int ret = 0, rc = ksceKernelLockMutex(g.game_lock, 1, NULL);
    if (rc < 0) {
        if (!__atomic_exchange_n(&lock_refusal_logged, 1, __ATOMIC_RELAXED))
            klog("vjoSetGameActive(pid 0x%X, mode %d) not applied: the game lock refused 0x%08X, the foreground is "
                 "unchanged (logged once a boot)", pid, mode, rc);
        return rc;
    }
    f = fg_load();
    if (fg_set_game_active(&f, pid, mode, &iev) < 0) {
        ret = VJO_ERR_NO_GAME;
    } else {
        if (mode)
            klog("game pid 0x%X active%s t=%u ms", pid, mode == VJO_GAME_STATIC_FB ? " (static framebuffer)" : "",
                 klog_ms());
        else if (iev & IEV_GAME_START)
            klog("pid 0x%X is not a game: back to game pid 0x%X", pid, f.game_pid);
        fg_store(&f, iev, 0);
    }
    ksceKernelUnlockMutex(g.game_lock, 1);
    return ret;
}

int lifecycle_hooks_install(void)
{
    hook_uid = taiHookFunctionImportForKernel(KERNEL_PID, &hook_ref, "SceProcessmgr", 0x887F19D0,
                                              0x414CC813, procevent_patched);
    if (hook_uid < 0)
        klog("procevent hook failed 0x%08X", hook_uid);
    return hook_uid < 0 ? hook_uid : 0;
}

void lifecycle_hooks_release(void)
{
    if (hook_uid >= 0)
        taiHookReleaseForKernel(hook_uid, hook_ref);
    hook_uid = -1;
}
