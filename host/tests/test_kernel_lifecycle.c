/* The process event hook and vjoSetGameActive's verdict (kernel/lifecycle.c),
 * with the real foreground state machine (kernel/foreground.c) and only the
 * SDK boundary substituted. #104: a change of the foreground releases the
 * game's input block in the hook's own call, before the event reaches the
 * firmware's handler, whatever the Shell does; SceShell's own events and other
 * processes' never touch it. */
#include "acutest.h"
#include <stdarg.h>
#include <stdio.h>
#include <psp2kern/host_stubs.h>
#include "../../include/vjo_api.h"
#include "../../kernel/foreground.h"

/* taiHEN: the hooked handler's continuation, recorded */
#define VJO_TEST_TAIHEN_H 1
typedef uintptr_t tai_hook_ref_t;
static int continue_calls, continue_pid, continue_ev, block_at_continue;
static int procevent_continue(tai_hook_ref_t ref, int pid, int ev, int a3, int a4, int *a5, int a6);
#define TAI_CONTINUE(type, ref, ...) ((type)procevent_continue((ref), __VA_ARGS__))
SceUID taiHookFunctionImportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module, uint32_t library,
                                      uint32_t nid, void *fn);
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref);

/* The kernel state lifecycle.c uses (vjo_kernel.h's VjoKernelState, those fields). */
#define VJO_KERNEL_H
static struct {
    SceUID ievf, lock, game_lock;
    volatile SceUID game_pid, shell_pid;
    volatile uint32_t ocr_epoch;
    volatile int game_active, input_block;
    SceUID prev_game_pid;
    volatile uint32_t suppress_mask, raw_buttons;
} g;
#define VJO_LOCK() ksceKernelLockMutex(g.lock, 1, NULL)
#define VJO_UNLOCK() ksceKernelUnlockMutex(g.lock, 1)
void klog(const char *fmt, ...);
unsigned klog_ms(void);
void boot_fault(int fault);
#include "../../kernel/lifecycle.c"

unsigned syscall_depth;
static uint64_t clock_us;
static unsigned ievf_bits, events_set, foreground_changes, faults;
static int locks_held, lock_held, game_lock_held;
static char lines[16][192];
static unsigned n_lines;

static int procevent_continue(tai_hook_ref_t ref, int pid, int ev, int a3, int a4, int *a5, int a6)
{
    continue_calls++;
    continue_pid = pid;
    continue_ev = ev;
    block_at_continue = g.input_block;
    TEST_CHECK(!locks_held); /* the firmware's handler runs with none of ours held */
    return 0;
}
SceUID taiHookFunctionImportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module, uint32_t library,
                                      uint32_t nid, void *fn)
{
    *ref = nid;
    return 7;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref) { return 0; }
void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(lines[n_lines % 16], sizeof(lines[0]), fmt, ap);
    va_end(ap);
    TEST_CHECK(n >= 0 && n < 190); /* kernel/log.c keeps 190 characters and the newline */
    n_lines++;
}
void boot_fault(int fault) { faults++; }
/* lifecycle.c's order: g.game_lock around the transition, g.lock inside it for the store */
void game_ocr_foreground_changed_locked(void) { TEST_CHECK(lock_held == 1 && game_lock_held == 1); foreground_changes++; }
void game_ocr_process_gone(SceUID pid) {}
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
/* kernel/log.c's */
unsigned klog_ms(void) { return (unsigned)(ksceKernelGetSystemTimeWide() / 1000); }
/* A refused g.game_lock: nonzero, each lock of it returns this and takes nothing, as a deleted mutex does
 * (a thread waiting in it wakes with SCE_KERNEL_ERROR_WAIT_DELETE, and its id cannot be used after:
 * [SDK: Kernel-Overview, Deleting Synchronization Objects]; the codes: [SDK: Error-Overview]). */
#define SCE_KERNEL_ERROR_WAIT_DELETE ((int)0x80028006u)
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID ((int)0x80028141u)
static int game_lock_rc;
int ksceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    TEST_CHECK(uid == g.lock || uid == g.game_lock);
    if (uid == g.game_lock && game_lock_rc)
        return game_lock_rc;
    if (uid == g.lock) { TEST_CHECK(game_lock_held == 1); lock_held++; }
    else game_lock_held++;
    locks_held++;
    return 0;
}
/* Only a mutex this thread holds: a refused lock is never unlocked. */
int ksceKernelUnlockMutex(SceUID uid, int count)
{
    TEST_CHECK(uid == g.lock || uid == g.game_lock);
    TEST_CHECK(uid == g.lock ? lock_held > 0 : game_lock_held > 0);
    if (uid == g.lock) lock_held--;
    else game_lock_held--;
    locks_held--;
    return 0;
}
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    if (uid == g.ievf) {
        ievf_bits |= bits;
        events_set++;
    }
    return 0;
}

static const char *logged(const char *needle)
{
    for (unsigned i = n_lines > 16 ? n_lines - 16 : 0; i < n_lines; i++)
        if (strstr(lines[i % 16], needle)) return lines[i % 16];
    return NULL;
}

#define SHELL 0x118FF
#define GAME 0x62C03
#define APP 0x1137D9
#define CIRCLE 0x2000u /* SCE_CTRL_CIRCLE */

static void reset(void)
{
    memset(&g, 0, sizeof(g));
    g.ievf = 3;
    g.lock = 4;
    g.game_lock = 5;
    g.shell_pid = SHELL;
    clock_us = 1791614280ull * 1000; /* 20.7 days of uptime: t= stays a whole number of ms */
    ievf_bits = events_set = foreground_changes = faults = 0;
    continue_calls = continue_pid = continue_ev = 0;
    block_at_continue = -1;
    n_lines = 0;
    locks_held = lock_held = game_lock_held = 0;
    game_lock_rc = 0;
    TEST_ASSERT(lifecycle_hooks_install() == 0);
}

/* The game in front, confirmed by the Shell, its overlay open (input blocked). */
static void game_with_overlay_open(void)
{
    TEST_CHECK(procevent_patched(GAME, PROCEV_STARTUP, 0, 0, NULL, 0) == 0);
    TEST_CHECK(lifecycle_set_game_active(GAME, VJO_GAME) == 0);
    TEST_CHECK(logged("game pid 0x62C03 active t=1791614280 ms") != NULL);
    g.input_block = 1; /* vjoSetInputBlock(1) */
    g.raw_buttons = CIRCLE;
    ievf_bits = 0;
}

/* open -> PS -> released: the hook releases the block before the firmware's
 * handler runs; SceShell is in front (no foreground process) and the worker
 * is told the game exited. */
static void test_ps_suspend_releases_input_in_the_same_event(void)
{
    reset();
    game_with_overlay_open();
    clock_us += 4000;
    TEST_CHECK(procevent_patched(GAME, PROCEV_SUSPEND, 0, 0, NULL, 0) == 0);
    TEST_CHECK(continue_calls == 2 && continue_pid == GAME && continue_ev == PROCEV_SUSPEND);
    TEST_CHECK(block_at_continue == 0 && g.input_block == 0);
    TEST_CHECK(g.game_pid == 0 && !g.game_active && ievf_bits == IEV_GAME_EXIT);
    TEST_CHECK(g.suppress_mask == CIRCLE); /* a held Cancel (here ○) stays hidden from the game until released */
    TEST_CHECK_(logged("foreground 0x62C03 -> 0x0 (suspend) t=1791614284 ms: input released") != NULL,
                "%s", lines[(n_lines - 1) % 16]);
    TEST_CHECK(!locks_held);
}

/* resume -> closed: the game comes back with its input free, unconfirmed
 * until the Shell's verdict, and stays unblocked after it: only the Shell's
 * next open blocks it. */
static void test_resume_starts_with_input_free(void)
{
    reset();
    game_with_overlay_open();
    procevent_patched(GAME, PROCEV_SUSPEND, 0, 0, NULL, 0);
    ievf_bits = 0;
    clock_us += 2000000;
    procevent_patched(GAME, PROCEV_RESUME, 0, 0, NULL, 0);
    TEST_CHECK(g.game_pid == GAME && !g.game_active && !g.input_block && ievf_bits == IEV_GAME_START);
    TEST_CHECK(logged("foreground 0x0 -> 0x62C03 (resume) t=1791616280 ms") != NULL &&
               !strstr(lines[(n_lines - 1) % 16], "released")); /* nothing was blocked */
    TEST_CHECK(lifecycle_set_game_active(GAME, VJO_GAME) == 0);
    TEST_CHECK(g.game_active == VJO_GAME && !g.input_block);
    /* A block the Shell set while nothing was in front (its L + R crossing the
     * PS) is gone once the next game is confirmed. */
    procevent_patched(GAME, PROCEV_SUSPEND, 0, 0, NULL, 0);
    g.input_block = 1;
    procevent_patched(GAME, PROCEV_RESUME, 0, 0, NULL, 0);
    TEST_CHECK(!g.input_block);
    g.input_block = 1;
    TEST_CHECK(lifecycle_set_game_active(GAME, VJO_GAME) == 0);
    TEST_CHECK(!g.input_block && logged("foreground 0x62C03 -> 0x62C03 (shell)") != NULL);
}

/* The game exits with the overlay open, or a process starts over it: released
 * too. The Shell's "not a game" verdict brings the game back, unblocked. */
static void test_exit_and_switch_release_input(void)
{
    reset();
    game_with_overlay_open();
    procevent_patched(GAME, PROCEV_EXIT, 0, 0, NULL, 0);
    TEST_CHECK(!g.input_block && block_at_continue == 0 && g.game_pid == 0 && ievf_bits == IEV_GAME_EXIT);
    TEST_CHECK(logged("foreground 0x62C03 -> 0x0 (exit)") != NULL);

    reset();
    game_with_overlay_open();
    procevent_patched(APP, PROCEV_STARTUP, 0, 0, NULL, 0);
    TEST_CHECK(!g.input_block && block_at_continue == 0 && g.game_pid == APP && ievf_bits == IEV_GAME_START);
    TEST_CHECK(logged("foreground 0x62C03 -> 0x1137D9 (startup) t=1791614280 ms: input released") != NULL);
    ievf_bits = 0;
    TEST_CHECK(lifecycle_set_game_active(APP, VJO_GAME_NONE) == 0);
    TEST_CHECK(g.game_pid == GAME && !g.game_active && !g.input_block && ievf_bits == IEV_GAME_START);

    /* The overlay closed: a confirmed game's exit is still logged, with nothing released. */
    reset();
    game_with_overlay_open();
    g.input_block = 0;
    unsigned before = n_lines;
    procevent_patched(GAME, PROCEV_EXIT, 0, 0, NULL, 0);
    TEST_CHECK(n_lines == before + 1 && g.suppress_mask == 0);
    TEST_CHECK_(strcmp(lines[before % 16], "foreground 0x62C03 -> 0x0 (exit) t=1791614280 ms") == 0, "%s",
                lines[before % 16]);
}

/* SceShell's own events, another process's, a second confirmation of the
 * game: the foreground does not change, so the open overlay keeps the game's
 * input blocked and nothing is logged. */
static void test_unrelated_events_keep_the_block(void)
{
    reset();
    game_with_overlay_open();
    unsigned before = n_lines, epoch = g.ocr_epoch;
    procevent_patched(SHELL, PROCEV_RESUME, 0, 0, NULL, 0);
    procevent_patched(SHELL, PROCEV_SUSPEND, 0, 0, NULL, 0);
    procevent_patched(0x7777, PROCEV_EXIT, 0, 0, NULL, 0);
    procevent_patched(0x7777, PROCEV_SUSPEND, 0, 0, NULL, 0);
    procevent_patched(GAME, PROCEV_RESUME, 0, 0, NULL, 0); /* already the foreground */
    procevent_patched(GAME, 2, 0, 0, NULL, 0);             /* an event the state machine ignores */
    TEST_CHECK(n_lines == before);
    TEST_CHECK(lifecycle_set_game_active(GAME, VJO_GAME) == 0); /* confirmed again (a Shell restart) */
    TEST_CHECK(g.input_block == 1 && g.game_pid == GAME && g.game_active == VJO_GAME);
    TEST_CHECK(g.ocr_epoch == epoch && ievf_bits == IEV_ACTIVATE);
    TEST_CHECK(n_lines == before + 1 && strstr(lines[before % 16], "game pid 0x62C03 active") == lines[before % 16]);
    TEST_CHECK(continue_calls == 7 && !faults);
    /* SceShell exiting with our plugin in it is a boot fault, and the event still passes */
    procevent_patched(SHELL, PROCEV_EXIT, 0, 0, NULL, 0);
    TEST_CHECK(faults == 1 && continue_calls == 8 && g.input_block == 1);
}

/* An app that starts and exits over SceShell, never confirmed as a game,
 * changes the foreground twice and logs nothing: only a confirmed game's
 * departure, a suspend, a resume or a release are worth a line. */
static void test_unconfirmed_app_is_not_logged(void)
{
    reset();
    unsigned before = n_lines;
    procevent_patched(APP, PROCEV_STARTUP, 0, 0, NULL, 0);
    TEST_CHECK(g.game_pid == APP && !g.game_active && ievf_bits == IEV_GAME_START);
    procevent_patched(APP, PROCEV_EXIT, 0, 0, NULL, 0);
    TEST_CHECK(g.game_pid == 0 && (ievf_bits & IEV_GAME_EXIT));
    TEST_CHECK_(n_lines == before, "%u line(s): %s", n_lines - before, n_lines ? lines[(n_lines - 1) % 16] : "");
}

/* audit-t3-crash-dumps-c N-4: the Shell's verdict without g.game_lock. A lock with no timeout on a
 * mutex that is not recursive, which nothing cancels, is refused only once the mutex is deleted
 * (kernel/main.c delete_sync_objects, in module_stop after the hooks are released), and that does not
 * come back: the verdict is dropped. The foreground stays as it was, nothing is unlocked, the lock's
 * error goes back to the Shell, and the first refusal is logged, once (lifecycle.c's flag is a static;
 * this is the only test that refuses the lock, so in any acutest mode it is the process's first). The
 * hook's own refusal (it skips and passes the event on) is pinned too. */
static void test_refused_game_lock_keeps_the_foreground(void)
{
    reset();
    game_with_overlay_open();
    procevent_patched(APP, PROCEV_STARTUP, 0, 0, NULL, 0); /* an app over the game, which is kept in prev */
    TEST_ASSERT(g.game_pid == APP && g.prev_game_pid == GAME && !g.game_active && g.suppress_mask == CIRCLE);
    g.input_block = 1; /* the Shell's while APP is unconfirmed: kernel/input.c passes an unconfirmed process's reads */
    ievf_bits = events_set = foreground_changes = 0;
    unsigned before = n_lines, epoch = g.ocr_epoch;
    int calls = continue_calls;

    /* The Shell's "not a game" for APP, waiting in the lock as module_stop deletes it: GAME stays behind APP. */
    game_lock_rc = SCE_KERNEL_ERROR_WAIT_DELETE;
    TEST_CHECK(lifecycle_set_game_active(APP, VJO_GAME_NONE) == SCE_KERNEL_ERROR_WAIT_DELETE);
    TEST_CHECK(g.game_pid == APP && g.prev_game_pid == GAME && !g.game_active);
    TEST_CHECK(g.input_block == 1 && g.suppress_mask == CIRCLE && g.ocr_epoch == epoch);
    TEST_CHECK(!events_set && !foreground_changes && !locks_held && !lock_held && !game_lock_held);
    TEST_CHECK_(n_lines == before + 1, "%u line(s)", n_lines - before);
    /* logged without the lock, so none of tools/analyse_selftest.py's KERNEL_LOCKED lines, and no t= */
    TEST_CHECK_(strcmp(lines[before % 16], "vjoSetGameActive(pid 0x1137D9, mode 0) not applied: the game lock refused "
                                           "0x80028006, the foreground is unchanged (logged once a boot)") == 0,
                "%s", lines[before % 16]);

    /* A game's verdict on the deleted lock's id: refused too, and not logged again. */
    game_lock_rc = SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    TEST_CHECK(lifecycle_set_game_active(APP, VJO_GAME) == SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
    TEST_CHECK(g.game_pid == APP && g.prev_game_pid == GAME && !g.game_active && g.input_block == 1);
    TEST_CHECK(g.ocr_epoch == epoch && !events_set && !foreground_changes && !locks_held);
    TEST_CHECK_(n_lines == before + 1, "%u line(s): %s", n_lines - before, lines[(n_lines - 1) % 16]);

    /* The hook refused the lock skips its transition and passes the event on, as it did before; SceShell's
     * exit is still a boot fault (recorded before the lock). */
    procevent_patched(APP, PROCEV_EXIT, 0, 0, NULL, 0);
    TEST_CHECK(continue_calls == calls + 1 && continue_pid == APP && continue_ev == PROCEV_EXIT);
    TEST_CHECK(g.game_pid == APP && g.prev_game_pid == GAME && !events_set && !locks_held && n_lines == before + 1);
    procevent_patched(SHELL, PROCEV_EXIT, 0, 0, NULL, 0);
    TEST_CHECK(faults == 1 && continue_calls == calls + 2 && n_lines == before + 1);

    /* Nothing was left half done: a lock that works applies the next verdict as ever, logged as ever. */
    game_lock_rc = 0;
    TEST_CHECK(lifecycle_set_game_active(APP, VJO_GAME_NONE) == 0);
    TEST_CHECK(g.game_pid == GAME && !g.prev_game_pid && !g.game_active && !g.input_block);
    TEST_CHECK(ievf_bits == IEV_GAME_START);
    TEST_CHECK(logged("pid 0x1137D9 is not a game: back to game pid 0x62C03") != NULL && !locks_held);
}

TEST_LIST = {
    {"unconfirmed_app_is_not_logged", test_unconfirmed_app_is_not_logged},
    {"ps_suspend_releases_input_in_the_same_event", test_ps_suspend_releases_input_in_the_same_event},
    {"resume_starts_with_input_free", test_resume_starts_with_input_free},
    {"exit_and_switch_release_input", test_exit_and_switch_release_input},
    {"unrelated_events_keep_the_block", test_unrelated_events_keep_the_block},
    {"refused_game_lock_keeps_the_foreground", test_refused_game_lock_keeps_the_foreground},
    {NULL, NULL}
};
