/* The game OCR worker, loaded by the kernel into every game that is ON in
 * games.ini when it starts (#138; docs/worker-loading.md has the prior art, the
 * lifecycle and the memory).
 *
 * taiHEN loads a process's *TITLEID plugins from its hook on
 * ksceKernelStartPreloadingModules, once the process's preloaded modules have
 * started (taiHEN hen.c:360-388, plugin.c:165-175); Adrenaline loads its user
 * module into the PSP emulator from the same hook (kernel/main.c:123-136). This
 * hook does the same for the worker, at the same point: after the original call,
 * for a game whose title is ON by the Shell's own rules (core/games.c
 * vjo_games_enabled_as: its row, a PSP game's shared NPXS10028 row, the
 * default), it loads VJO_GAME_OCR_PATH_UR0 into the process (VJO_GAME_OCR_PATH_UX0
 * only when ur0:tai has none), so the worker needs no taiHEN line. It loads
 * nothing into SceShell, a system app (NPXS*, but the PSP emulator's NPXS10028),
 * VitaShell or the Rubi app (vjo_game_is_candidate), nothing while L is held
 * at the start (taiHEN's own escape, hen.c:368-373) and nothing before the Shell
 * plugin has registered. The original call's result is returned whatever happens.
 *
 * games.ini is read by the kernel worker thread, which reads every ux0 file a
 * game needs (game_ocr.c), never in the starting process's context: the hook
 * asks and waits at most LOADER_WAIT_US. The read takes one transient KERNEL_RW
 * block (VjoPolicy, VJO_BUDGET_KERNEL_POLICY), freed before the answer: the
 * kernel keeps no copy of the policy. The three firmware functions are looked up
 * once by library and function NID (module_get_export_func, as meminfo.c does),
 * never imported: without the title or the module loader the loader stays off
 * and a per-title line still loads the worker.
 *
 * A game whose *TITLEID line still names the worker (older installs, for one
 * release) gets a second copy right after this one: taiHEN's hook heads the
 * chain (patches.c:389-418 hooks_add_hook), so it loads its section's lines
 * after this hook has returned. The worker keeps one copy per process
 * (game_ocr/main.c another_copy: the oldest by name stays) and the kernel one
 * registered worker thread (game_ocr.c vjoOcrRegister). */
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "../core/games.h"
#include "../include/vjo_ledger_budget.h"
#include "../vita/games_storage.h"
#include "game_ocr.h"
#include "vjo_kernel.h"

/* taihenModuleUtils */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

/* The block holds the parsed table and the file's text (vjo_games_parse needs no
 * terminator: vita/games_storage.c passes the bytes read). */
typedef char policy_fits_its_block[sizeof(VjoGames) + VJO_GAMES_TEXT_MAX <= VJO_BUDGET_KERNEL_POLICY ? 1 : -1];

/* The hook's wait for the kernel worker: one open, at most 12 KiB read and a
 * close, two of each when the backup is read (a file append took the console
 * 12-46 ms, #81). */
#define LOADER_WAIT_US 500000
#define ERR_ENOENT ((int)0x80010002u)

/* The kernel worker's verdict on a process start (low 4 bits), with where it
 * came from; the hook's own when the worker did not answer. WL_ON and WL_OFF are
 * the title's verdict as a game; WL_PSP_ON says whether it is ON as a PSP game
 * (its row, then NPXS10028's, then the default), so the question needs no PSP
 * bit: the hook knows which applies. */
enum { WL_ON, WL_OFF, WL_UNREADABLE, WL_NO_MEMORY, WL_NO_FILE, WL_NO_ANSWER };
#define WL_UX0 0x10u      /* ON: VJO_GAME_OCR_PATH_UX0 (none in ur0:tai) */
#define WL_BACKUP 0x20u   /* games.ini.bak was read */
#define WL_DEFAULTS 0x40u /* neither file exists: the defaults */
#define WL_PSP_ON 0x80u   /* WL_ON or WL_OFF: ON as a PSP game */

typedef int (*title_fn)(SceUID pid, char *title, SceSize len);
typedef int (*pspemu_fn)(SceUID pid);
typedef SceUID (*load_fn)(SceUID pid, const char *path, SceSize args, void *argp, int flags, void *option,
                          int *status);

static tai_hook_ref_t hook_ref;
static SceUID hook_uid, ask_lock; /* 0: none */
static int inside;                /* process starts inside the hook (loader_hook_release waits for them) */
static title_fn get_title;        /* ksceKernelSysrootGetProcessTitleId */
static pspemu_fn is_pspemu;       /* ksceSblACMgrIsPspEmu, optional */
static load_fn load_start;        /* ksceKernelLoadStartModuleForPid */

/* The question of the process starting now: its hook writes it under ask_lock,
 * the kernel worker reads it. seq is odd while it is written; the answer
 * carries, in its top 24 bits, the even seq it answers. */
static struct {
    volatile uint32_t seq, answer;
    char title[VJO_TITLE_ID_SIZE];
} ask;

/* ---- the kernel worker's side ---- */

/* As vita/games_storage.c read_policy, which the Shell and the Rubi app use,
 * with the kernel's file calls. */
static int read_policy(const char *path, VjoGames *g, char *text)
{
    SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    int rc = -1;
    if (fd < 0) return -1;
    size = ksceIoLseek(fd, 0, SCE_SEEK_END);
    if (size >= 0 && size <= VJO_GAMES_TEXT_MAX && ksceIoLseek(fd, 0, SCE_SEEK_SET) >= 0) {
        int used = 0;
        while (used < (int)size) {
            int n = ksceIoRead(fd, text + used, (SceSize)((int)size - used));
            if (n <= 0) break;
            used += n;
        }
        if (used == (int)size && size > 0) rc = vjo_games_parse(g, text, (size_t)used);
    }
    ksceIoClose(fd);
    return rc;
}

static int missing(const char *path)
{
    SceIoStat st;
    return ksceIoGetstat(path, &st) == ERR_ENOENT;
}

/* As vjo_games_file_load: 0 the file, 1 its backup, 2 neither exists (the
 * defaults), -1 unreadable or corrupt (the Shell disables the game then). */
static int load_policy(VjoGames *g, char *text)
{
    if (read_policy(VJO_GAMES_PATH, g, text) == 0) return 0;
    if (read_policy(VJO_GAMES_BACKUP, g, text) == 0) return 1;
    vjo_games_defaults(g);
    return missing(VJO_GAMES_PATH) && missing(VJO_GAMES_BACKUP) ? 2 : -1;
}

/* The one allocation site of the loader (docs/memory-ledger.md): the read's
 * block, freed by policy_free before the answer. */
static SceUID policy_alloc(void **base)
{
    SceUID uid = ksceKernelAllocMemBlock("VjoPolicy", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, VJO_BUDGET_KERNEL_POLICY,
                                         NULL);
    *base = NULL;
    if (uid < 0) return uid;
    if (ksceKernelGetMemBlockBase(uid, base) < 0 || !*base) {
        ksceKernelFreeMemBlock(uid);
        return VJO_ERR_NO_MEMORY;
    }
    vjo_ledger_add(&g_kernel_ledger, VJO_KL_POLICY, VJO_BUDGET_KERNEL_POLICY);
    return uid;
}

static void policy_free(SceUID uid)
{
    if (ksceKernelFreeMemBlock(uid) >= 0) vjo_ledger_sub(&g_kernel_ledger, VJO_KL_POLICY, VJO_BUDGET_KERNEL_POLICY);
    else klog("VjoPolicy not freed");
}

static uint32_t verdict_of(const char *title)
{
    void *base;
    SceIoStat st;
    SceUID uid = policy_alloc(&base);
    uint32_t v;
    int rc, ur0;
    if (uid < 0) return WL_NO_MEMORY;
    rc = load_policy((VjoGames *)base, (char *)base + sizeof(VjoGames));
    if (rc < 0) v = WL_UNREADABLE;
    else
        v = (vjo_games_enabled_as((const VjoGames *)base, title, 0) ? WL_ON : WL_OFF) |
            (vjo_games_enabled_as((const VjoGames *)base, title, 1) ? WL_PSP_ON : 0u);
    policy_free(uid);
    if (rc == 1) v |= WL_BACKUP;
    if (rc == 2) v |= WL_DEFAULTS;
    if ((v & 0xFu) != WL_ON && !(v & WL_PSP_ON)) return v;
    /* ur0:tai first; ux0:tai only when ur0:tai has no worker (another error
     * leaves it to the load, which logs it). */
    ur0 = ksceIoGetstat(VJO_GAME_OCR_PATH_UR0, &st);
    if (ur0 != ERR_ENOENT) return v;
    return ksceIoGetstat(VJO_GAME_OCR_PATH_UX0, &st) >= 0 ? v | WL_UX0 : (v & ~0x8Fu) | WL_NO_FILE;
}

void loader_tick(void)
{
    char title[VJO_TITLE_ID_SIZE];
    uint32_t seq = __atomic_load_n(&ask.seq, __ATOMIC_ACQUIRE), v;
    if ((seq & 1u) || (ask.answer >> 8) == (seq & 0xFFFFFFu)) return; /* being written, or answered */
    memcpy(title, ask.title, sizeof(title));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&ask.seq, __ATOMIC_ACQUIRE) != seq) return; /* rewritten meanwhile: the next tick */
    title[sizeof(title) - 1] = '\0';
    v = verdict_of(title);
    __atomic_store_n(&ask.answer, (seq & 0xFFFFFFu) << 8 | v, __ATOMIC_RELEASE);
    ksceKernelSetEventFlag(g.mevf, MEV_POLICY);
}

/* ---- the starting process's side ---- */

/* Asks the kernel worker for this start's verdict and waits for it, at most
 * LOADER_WAIT_US in all. */
static uint32_t ask_worker(const char *title)
{
    SceUInt timeout = LOADER_WAIT_US;
    int64_t deadline = (int64_t)ksceKernelGetSystemTimeWide() + LOADER_WAIT_US;
    uint32_t seq, v = WL_NO_ANSWER;
    if (ksceKernelLockMutex(ask_lock, 1, &timeout) < 0) return WL_NO_ANSWER;
    seq = ask.seq + 1u;
    __atomic_store_n(&ask.seq, seq, __ATOMIC_RELEASE); /* odd: being written */
    memcpy(ask.title, title, sizeof(ask.title));
    __atomic_store_n(&ask.seq, ++seq, __ATOMIC_RELEASE);
    ksceKernelClearEventFlag(g.mevf, ~MEV_POLICY);
    ksceKernelSetEventFlag(g.ievf, IEV_POLICY);
    for (;;) {
        uint32_t a = __atomic_load_n(&ask.answer, __ATOMIC_ACQUIRE);
        int64_t left = deadline - (int64_t)ksceKernelGetSystemTimeWide();
        unsigned int bits = 0;
        SceUInt us;
        if ((a >> 8) == (seq & 0xFFFFFFu)) {
            v = a & 0xFFu;
            break;
        }
        if (left <= 0) break;
        us = (SceUInt)left;
        ksceKernelWaitEventFlag(g.mevf, MEV_POLICY, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &us);
    }
    ksceKernelUnlockMutex(ask_lock, 1);
    return v;
}

static void worker_for(SceUID pid)
{
    static const char *const why[] = {
        [WL_OFF] = "OFF in games.ini",
        [WL_UNREADABLE] = "games.ini unreadable",
        [WL_NO_MEMORY] = "no kernel memory to read games.ini",
        [WL_NO_FILE] = "no VitaJPOverlay_OCR.suprx in ur0:tai or ux0:tai",
        [WL_NO_ANSWER] = "no answer from the kernel worker in 500 ms",
    };
    char title[32];
    SceCtrlData pad;
    const char *path;
    SceUID mod;
    int psp, status = 0;
    uint32_t v;
    if (pid <= 0 || g.shell_pid <= 0 || pid == g.shell_pid) return;
    memset(title, 0, sizeof(title));
    if (get_title(pid, title, sizeof(title)) < 0) return;
    title[sizeof(title) - 1] = '\0';
    /* SceShell, the system apps, VitaShell, the Rubi app: before any file or log line. */
    if (!vjo_game_is_candidate(title)) return;
    psp = vjo_game_is_psp(title) || (is_pspemu && is_pspemu(pid) > 0);
    memset(&pad, 0, sizeof(pad));
    ksceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) {
        klog("worker for %s%s (pid 0x%X): L held at its start: not loaded", title, psp ? " (PSP)" : "", pid);
        return;
    }
    v = ask_worker(title);
    /* ON or OFF: as a game, or as a PSP game (the shared row before the default). */
    if ((v & 0xFu) == WL_ON || (v & 0xFu) == WL_OFF)
        v = (v & ~0xFu) | ((psp ? v & WL_PSP_ON : (v & 0xFu) == WL_ON) ? WL_ON : WL_OFF);
    if ((v & 0xFu) != WL_ON) {
        klog("worker for %s%s (pid 0x%X): %s: not loaded", title, psp ? " (PSP)" : "", pid,
             (v & 0xFu) <= WL_NO_ANSWER ? why[v & 0xFu] : "?");
        return;
    }
    path = v & WL_UX0 ? VJO_GAME_OCR_PATH_UX0 : VJO_GAME_OCR_PATH_UR0;
    mod = load_start(pid, path, 0, NULL, 0, NULL, &status);
    klog("worker for %s%s (pid 0x%X): ON%s: %s 0x%08X, start %d", title, psp ? " (PSP)" : "", pid,
         v & WL_BACKUP ? " in games.ini.bak" : v & WL_DEFAULTS ? " (no games.ini)" : " in games.ini", path,
         (unsigned)mod, status);
}

static int start_preloading_patched(SceUID pid)
{
    int ret;
    __atomic_add_fetch(&inside, 1, __ATOMIC_ACQ_REL);
    ret = TAI_CONTINUE(int, hook_ref, pid);
    worker_for(pid);
    __atomic_sub_fetch(&inside, 1, __ATOMIC_ACQ_REL);
    return ret;
}

/* Library NID, then function NID (wiki SceSysmem, SceSblACMgr,
 * SceKernelModulemgr; vita-headers db/360 and db/363: none of the three
 * libraries changed at 3.63). */
static uintptr_t resolve(const char *module, uint32_t lib, uint32_t fn_nid)
{
    uintptr_t fn = 0;
    return module_get_export_func(KERNEL_PID, module, lib, fn_nid, &fn) < 0 ? 0 : fn;
}

int loader_hook_install(void)
{
    get_title = (title_fn)resolve("SceSysmem", 0x3691DA45, 0xEC3124A3);        /* SceSysrootForKernel */
    load_start = (load_fn)resolve("SceKernelModulemgr", 0xD4A60A52, 0x9D953C22); /* SceModulemgrForDriver */
    is_pspemu = (pspemu_fn)resolve("SceSblACMgr", 0x9AD8E213, 0xFD00C72A);      /* SceSblACMgrForDriver */
    if (!get_title || !load_start) {
        klog("worker loader off: title 0x%X, loader 0x%X not found", (unsigned)(uintptr_t)get_title,
             (unsigned)(uintptr_t)load_start);
        return VJO_ERR_NO_GAME;
    }
    ask_lock = ksceKernelCreateMutex("VjoLoader", 0, 0, NULL);
    if (ask_lock < 0) {
        int rc = ask_lock;
        ask_lock = 0;
        return rc;
    }
    /* ksceKernelStartPreloadingModules: SceModulemgrForKernel 0xC445FA63 to 3.61,
     * 0x92C9FFC2 from 3.63 (wiki SceKernelModulemgr; Adrenaline kernel/main.c:200-202). */
    hook_uid = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref, "SceKernelModulemgr", 0xC445FA63, 0x432DCC7A,
                                              start_preloading_patched);
    if (hook_uid < 0)
        hook_uid = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref, "SceKernelModulemgr", 0x92C9FFC2,
                                                  0x998C7AE9, start_preloading_patched);
    if (hook_uid < 0) {
        int rc = hook_uid;
        klog("worker loader hook failed 0x%08X", (unsigned)rc);
        hook_uid = 0;
        ksceKernelDeleteMutex(ask_lock);
        ask_lock = 0;
        return rc;
    }
    return 0;
}

/* The hook goes, then the starts still inside it leave: one can wait up to
 * LOADER_WAIT_US for the kernel worker, which module_stop therefore stops only
 * after this, and then load the worker. A start that entered the hook just
 * before its release counts itself in within a few instructions: the first
 * pause lets it. 0, or VJO_ERR_BUSY when one is still inside after
 * LOADER_DRAIN_MS: the loader is off and its mutex stays, and module_stop fails
 * (as game_ocr_stop's does), so no code of ours goes from under it. (taiHEN
 * stops no kernel plugin; this is module_stop's own order.) */
#define LOADER_DRAIN_MS 3000
int loader_hook_release(void)
{
    int waited = 0;
    if (hook_uid > 0) {
        taiHookReleaseForKernel(hook_uid, hook_ref);
        ksceKernelDelayThread(1000);
    }
    hook_uid = 0;
    while (__atomic_load_n(&inside, __ATOMIC_ACQUIRE)) {
        if (waited++ >= LOADER_DRAIN_MS) {
            klog("module_stop: a game start still in the worker loader after %d ms", LOADER_DRAIN_MS);
            return VJO_ERR_BUSY;
        }
        ksceKernelDelayThread(1000);
    }
    if (waited) ksceKernelDelayThread(1000); /* its last instructions after the count */
    if (ask_lock > 0) ksceKernelDeleteMutex(ask_lock);
    ask_lock = 0;
    return 0;
}
