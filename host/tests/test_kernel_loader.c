/* The worker loader (kernel/loader.c, #138) with only the SDK boundary
 * substituted: taiHEN's hook and export lookup, the process title, the PSP
 * emulator check, the pad, the module loader, the event flags, the mutex, the
 * kernel worker's memory block, and its files as real files in a temporary
 * directory under the console's own paths. The policy is the Shell's own code
 * (core/games.c), and the kernel's games.ini reader is checked against the
 * Shell's and the Rubi app's (vita/games_storage.c) on the same files. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2kern/host_stubs.h>

/* taiHEN: the original ksceKernelStartPreloadingModules, recorded */
#define VJO_TEST_TAIHEN_H 1
typedef uintptr_t tai_hook_ref_t;
static int preload_continue(tai_hook_ref_t ref, SceUID pid);
#define TAI_CONTINUE(type, ref, ...) ((type)preload_continue((ref), __VA_ARGS__))
SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module, uint32_t library,
                                      uint32_t nid, void *fn);
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref);

#include "../../kernel/vjo_kernel.h"
#include "../../kernel/loader.c"
#include "../../kernel/text_source.c" /* audit-hooks F1: the loader's verdicts decide who reads the text source */
#include "../../vita/games_storage.h"

VjoKernelState g;
unsigned syscall_depth;

/* ---- the boundary ---- */
#define SHELL_PID 0x118FF
#define GAME_PID 0x72ED3
#define MOD_ID 0x40010129
#define CONTINUE_RC 0x1234

static SceUID caller; /* the process a syscall comes from */
SceUID ksceKernelGetProcessId(void) { return caller; }
static int boot = VJO_BOOT_NORMAL;
int boot_mode(void) { return boot; }
static int load_source; /* what the loaded worker's module_start read (vjoGetTextSource), -1: no load */

static int continue_calls, hook_installs, hook_releases, fail_360, fail_363;
static int (*hooked)(SceUID);
static uint32_t hooked_lib;
static int missing_title, missing_load, missing_pspemu;
static int mutexes, mutex_deleted, lock_depth, lock_refused;
static int answer_mode; /* 0: the kernel worker answers when asked, 1: while the hook waits, 2: never */
static int asked, waits, stale_once;
static uint64_t clock_us;
static int alloc_fail, live_blocks, blocks, block_type;
static unsigned block_bytes;
static unsigned char block[VJO_BUDGET_KERNEL_POLICY];
static uint32_t pad_buttons;
static int load_calls, load_pid, load_rc = MOD_ID, load_status;
static char load_path[64];
static int title_calls, title_rc;
static const char *title_of_game = "PCSG00940";
static int pspemu_pid;
static int opens;
static char lines[32][192];
static unsigned n_lines;

static int preload_continue(tai_hook_ref_t ref, SceUID pid)
{
    (void)ref; (void)pid;
    continue_calls++;
    TEST_CHECK(!load_calls); /* the process's own modules start first, then the worker */
    return CONTINUE_RC;
}
SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module, uint32_t library,
                                      uint32_t nid, void *fn)
{
    TEST_CHECK(pid == KERNEL_PID && !strcmp(module, "SceKernelModulemgr"));
    hook_installs++;
    if (library == 0xC445FA63) {
        TEST_CHECK(nid == 0x432DCC7A); /* ksceKernelStartPreloadingModules, 3.60-3.61 */
        if (fail_360) return (int)0x90010002; /* TAI_ERROR_NOT_FOUND */
    } else {
        TEST_CHECK(library == 0x92C9FFC2 && nid == 0x998C7AE9); /* 3.63-3.74 */
        if (fail_363) return (int)0x90010002;
    }
    *ref = 0x77;
    hooked = (int (*)(SceUID))fn;
    hooked_lib = library;
    return 0x55;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref)
{
    TEST_CHECK(uid == 0x55 && ref == 0x77);
    hook_releases++;
    hooked = NULL;
    return 0;
}

static int fake_title(SceUID pid, char *title, SceSize len)
{
    title_calls++;
    TEST_CHECK(len >= 12);
    if (title_rc) return title_rc;
    snprintf(title, len, "%s", pid == SHELL_PID ? "main" : title_of_game);
    return 0;
}
static int fake_pspemu(SceUID pid) { return pid == pspemu_pid; }
static SceUID fake_load(SceUID pid, const char *path, SceSize args, void *argp, int flags, void *option, int *status)
{
    SceUID keep = caller;
    TEST_CHECK(!args && !argp && !flags && !option && status);
    load_calls++;
    load_pid = pid;
    snprintf(load_path, sizeof(load_path), "%s", path);
    caller = pid; /* the worker's module_start, in the game's process, inside the load */
    load_source = vjoGetTextSource();
    caller = keep;
    *status = load_status;
    return load_rc;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t lib, uint32_t nid, uintptr_t *fn)
{
    TEST_CHECK(pid == KERNEL_PID);
    if (!strcmp(module, "SceSysmem") && lib == 0x3691DA45 && nid == 0xEC3124A3 && !missing_title)
        *fn = (uintptr_t)fake_title;
    else if (!strcmp(module, "SceKernelModulemgr") && lib == 0xD4A60A52 && nid == 0x9D953C22 && !missing_load)
        *fn = (uintptr_t)fake_load;
    else if (!strcmp(module, "SceSblACMgr") && lib == 0x9AD8E213 && nid == 0xFD00C72A && !missing_pspemu)
        *fn = (uintptr_t)fake_pspemu;
    else
        return (int)0x90010002;
    return 0;
}

int ksceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    TEST_CHECK(port == 0 && count == 1);
    pad->buttons = pad_buttons;
    return 1;
}

SceUID ksceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt)
{
    TEST_CHECK(!strcmp(name, "VjoLoader"));
    mutexes++;
    return 0x99;
}
int ksceKernelDeleteMutex(SceUID uid) { TEST_CHECK(uid == 0x99); mutex_deleted++; return 0; }
int ksceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    TEST_CHECK(uid == 0x99 && count == 1 && timeout && *(SceUInt *)timeout == LOADER_WAIT_US);
    if (lock_refused) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    lock_depth++;
    return 0;
}
int ksceKernelUnlockMutex(SceUID uid, int count) { TEST_CHECK(uid == 0x99 && count == 1 && lock_depth == 1); lock_depth--; return 0; }

uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
/* module_stop's drain: a start inside the hook leaves after leave_after delays (0: never). */
static int delays, leave_after;
int ksceKernelDelayThread(SceUInt us)
{
    clock_us += us;
    if (++delays == leave_after) __atomic_sub_fetch(&inside, 1, __ATOMIC_ACQ_REL);
    return 0;
}
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    if (uid == g.ievf) {
        TEST_CHECK(bits == IEV_POLICY && lock_depth == 1);
        asked++;
        if (answer_mode == 0) loader_tick(); /* the kernel worker ran before the hook waits */
        return 0;
    }
    TEST_CHECK(uid == g.mevf && bits == MEV_POLICY);
    return 0;
}
int ksceKernelClearEventFlag(SceUID uid, unsigned int bits)
{
    TEST_CHECK(uid == g.mevf && bits == ~MEV_POLICY && lock_depth == 1);
    return 0;
}
int ksceKernelWaitEventFlag(SceUID uid, unsigned int bits, unsigned int mode, unsigned int *out, SceUInt *timeout)
{
    TEST_CHECK(uid == g.mevf && bits == MEV_POLICY && mode == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT));
    TEST_CHECK(timeout && *timeout > 0 && *timeout <= LOADER_WAIT_US);
    waits++;
    if (stale_once) { /* an answer to an earlier question that timed out */
        stale_once = 0;
        ask.answer = ((ask.seq - 2u) & 0xFFFFFFu) << 8 | WL_ON;
        clock_us += 1000;
        return 0;
    }
    if (answer_mode == 1) { clock_us += 20000; loader_tick(); return 0; }
    clock_us += *timeout;
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}

SceUID ksceKernelAllocMemBlock(const char *name, unsigned int type, SceSize size, void *opt)
{
    TEST_CHECK(!strcmp(name, "VjoPolicy") && !opt);
    blocks++;
    block_type = (int)type;
    block_bytes = size;
    if (alloc_fail) return (int)0x80024302; /* SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE */
    live_blocks++;
    memset(block, 0xA5, sizeof(block));
    return 0x5000;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base) { TEST_CHECK(uid == 0x5000); *base = block; return 0; }
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 0x5000 && live_blocks == 1); live_blocks--; return 0; }

static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    TEST_CHECK(flags == SCE_O_RDONLY);
    opens++;
    return native(open(path, O_RDONLY));
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int ksceIoRead(SceUID fd, void *dst, SceSize len) { return native((int)read(fd, dst, len > 13 ? 13 : len)); }
int ksceIoClose(SceUID fd) { return native(close(fd)); }
int ksceIoGetstat(const char *path, SceIoStat *st)
{
    struct stat s;
    memset(st, 0, sizeof(*st));
    return native(stat(path, &s));
}

void klog(const char *fmt, ...)
{
    va_list ap;
    if (n_lines >= sizeof(lines) / sizeof(lines[0])) return;
    va_start(ap, fmt);
    vsnprintf(lines[n_lines++], sizeof(lines[0]), fmt, ap);
    va_end(ap);
}
static const char *last_line(void) { return n_lines ? lines[n_lines - 1] : ""; }

/* ---- the files ---- */
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    if (text) fputs(text, f);
    fclose(f);
}

static void reset(void)
{
    static int initialized;
    if (!initialized) {
        char dir[PATH_MAX];
        TEST_ASSERT(vjo_test_tmpdir_new(dir, sizeof(dir)) != NULL);
        TEST_ASSERT(chdir(dir) == 0);
        mkdir("ux0:data", 0777);
        mkdir(VJO_GAMES_DIR, 0777);
        mkdir("ur0:tai", 0777);
        mkdir("ux0:tai", 0777);
        initialized = 1;
    }
    unlink(VJO_GAMES_PATH);
    unlink(VJO_GAMES_BACKUP);
    unlink(VJO_GAME_OCR_PATH_UX0);
    put(VJO_GAME_OCR_PATH_UR0, "worker");
    memset(&g, 0, sizeof(g));
    g.ievf = 11;
    g.mevf = 13;
    g.shell_pid = SHELL_PID;
    memset(&ask, 0, sizeof(ask));
    hook_uid = ask_lock = 0;
    inside = delays = leave_after = 0;
    get_title = NULL;
    is_pspemu = NULL;
    load_start = NULL;
    continue_calls = hook_installs = hook_releases = fail_360 = fail_363 = 0;
    hooked = NULL;
    missing_title = missing_load = missing_pspemu = 0;
    mutexes = mutex_deleted = lock_depth = lock_refused = 0;
    answer_mode = asked = waits = stale_once = 0;
    clock_us = 1000000;
    alloc_fail = live_blocks = blocks = block_type = 0;
    block_bytes = 0;
    pad_buttons = 0;
    load_calls = load_pid = load_status = 0;
    load_rc = MOD_ID;
    load_path[0] = '\0';
    title_calls = title_rc = 0;
    title_of_game = "PCSG00940";
    pspemu_pid = 0;
    opens = 0;
    n_lines = 0;
    text_source = 0; /* text_source.c's .bss as the module loads */
    memset(on_pid, 0, sizeof(on_pid));
    on_next = 0;
    caller = 0;
    boot = VJO_BOOT_NORMAL;
    load_source = -1;
    kledger_init();
    TEST_ASSERT(loader_hook_install() == 0 && hooked);
}

/* One process start: the hook as firmware calls it. */
static int start(SceUID pid, const char *title)
{
    title_of_game = title;
    load_calls = 0;
    return hooked(pid);
}

/* ---- the tests ---- */

static void test_hook_install_and_release(void)
{
    reset();
    TEST_CHECK(hook_installs == 1 && hooked_lib == 0xC445FA63 && mutexes == 1);
    TEST_CHECK(get_title == fake_title && load_start == fake_load && is_pspemu == fake_pspemu);
    loader_hook_release();
    TEST_CHECK(hook_releases == 1 && mutex_deleted == 1 && !hook_uid && !ask_lock);
    loader_hook_release(); /* nothing twice */
    TEST_CHECK(hook_releases == 1 && mutex_deleted == 1);

    /* 3.63-3.74: the 3.60 library is not there, the second pair is hooked. */
    fail_360 = 1; hook_installs = 0;
    TEST_CHECK(loader_hook_install() == 0 && hook_installs == 2 && hooked_lib == 0x92C9FFC2);
    loader_hook_release();
    /* Neither: the loader is off and gives its mutex back (a per-title line still works). */
    fail_363 = 1; mutex_deleted = 0; hooked = NULL;
    TEST_CHECK(loader_hook_install() < 0 && !hooked && mutex_deleted == 1 && !hook_uid && !ask_lock);
    TEST_CHECK(strstr(last_line(), "worker loader hook failed") != NULL);
    fail_360 = fail_363 = 0;
    /* Without the title or the module loader it is off before any hook or mutex. */
    missing_title = 1; hook_installs = mutexes = 0;
    TEST_CHECK(loader_hook_install() < 0 && !hook_installs && !mutexes);
    TEST_CHECK(strstr(last_line(), "worker loader off") != NULL);
    missing_title = 0; missing_load = 1;
    TEST_CHECK(loader_hook_install() < 0 && !hook_installs && !mutexes);
    missing_load = 0;
    /* Without the PSP emulator check it runs, PSP games by their IDs. */
    missing_pspemu = 1;
    TEST_CHECK(loader_hook_install() == 0 && hooked && !is_pspemu);
    loader_hook_release();
}

/* SceShell, the system apps, VitaShell, the Rubi app, an unreadable title, and
 * every process before the Shell plugin registers: nothing asked, read, loaded
 * or logged, and the original call's result comes back. */
static void test_never_into_shell_or_system_apps(void)
{
    static const char *const not_games[] = {"main", "NPXS10015", "NPXS10003", "VITASHELL", "VJOSET001", "", "BAD ID"};
    reset();
    TEST_CHECK(start(SHELL_PID, "main") == CONTINUE_RC && !title_calls); /* the registered SceShell */
    for (unsigned i = 0; i < sizeof(not_games) / sizeof(not_games[0]); i++) {
        TEST_CHECK_(start(GAME_PID, not_games[i]) == CONTINUE_RC, "%s", not_games[i]);
        TEST_CHECK_(!asked && !opens && !blocks && !load_calls && !n_lines, "%s", not_games[i]);
    }
    title_rc = (int)0x80020003;
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !asked && !load_calls && !n_lines);
    title_rc = 0;
    g.shell_pid = 0; /* boot: the Shell plugin has not registered yet */
    title_calls = 0;
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !title_calls && !asked && !load_calls);
    TEST_CHECK(continue_calls == 10);
}

/* test.3's AIR (PCSG00940): no taiHEN line, ON by the default. */
static void test_on_by_default_loads_the_worker(void)
{
    reset();
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC);
    TEST_CHECK(load_calls == 1 && load_pid == GAME_PID && !strcmp(load_path, "ur0:tai/VitaJPOverlay_OCR.suprx"));
    TEST_CHECK_(!strcmp(last_line(), "worker for PCSG00940 (pid 0x72ED3): ON (no games.ini): "
                                     "ur0:tai/VitaJPOverlay_OCR.suprx 0x40010129, start 0"), "%s", last_line());
    put(VJO_GAMES_PATH, "# Rubi per-game settings\ndefault = enabled\nPCSG00415 = enabled\nPCSF00001 = disabled\n");
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK(strstr(last_line(), ": ON in games.ini: ur0:tai/") != NULL);
    /* The block is taken for the read and given back before the answer. */
    TEST_CHECK(blocks == 2 && !live_blocks && block_type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW &&
               block_bytes == VJO_BUDGET_KERNEL_POLICY);
    TEST_CHECK(vjo_ledger_current(&g_kernel_ledger, VJO_KL_POLICY) == 0 &&
               vjo_ledger_peak(&g_kernel_ledger, VJO_KL_POLICY) == VJO_BUDGET_KERNEL_POLICY);
}

/* The Shell's rules, as core/games.c has them: the title's own row, then the default. */
static void test_rows_and_default(void)
{
    reset();
    put(VJO_GAMES_PATH, "default = disabled\nPCSG00415 = enabled\nPCSF00001 = disabled\n");
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls); /* no row: the default, OFF */
    TEST_CHECK_(!strcmp(last_line(), "worker for PCSG00940 (pid 0x72ED3): OFF in games.ini: not loaded"), "%s",
                last_line());
    put(VJO_GAMES_PATH, "default = enabled\nPCSF00001 = disabled\n");
    TEST_CHECK(start(GAME_PID, "PCSF00001") == CONTINUE_RC && !load_calls);
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1);
    /* Homebrew is a game to the Shell too: ON by the default. */
    TEST_CHECK(start(GAME_PID, "VITADBDLD") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK(!live_blocks && vjo_ledger_current(&g_kernel_ledger, VJO_KL_POLICY) == 0);
}

/* Adrenaline (#138: the case test.3 saw): the emulator's process is PSPEMUCFW,
 * a bubble's own ID, or NPXS10028; a PSP game without a row of its own follows
 * the shared NPXS10028 row, then the default. */
static void test_adrenaline_ids(void)
{
    reset();
    TEST_CHECK(start(GAME_PID, "PSPEMUCFW") == CONTINUE_RC && load_calls == 1); /* no games.ini: ON */
    TEST_CHECK(strstr(last_line(), "worker for PSPEMUCFW (PSP) (pid 0x72ED3): ON") == last_line());
    TEST_CHECK(start(GAME_PID, "NPXS10028") == CONTINUE_RC && load_calls == 1); /* the emulator's own ID */
    put(VJO_GAMES_PATH, "default = enabled\nNPXS10028 = disabled\n");
    TEST_CHECK(start(GAME_PID, "PSPEMUCFW") == CONTINUE_RC && !load_calls); /* the PSP row, before the default */
    TEST_CHECK(start(GAME_PID, "PSPEMU005") == CONTINUE_RC && !load_calls);
    TEST_CHECK(start(GAME_PID, "ULJS00378") == CONTINUE_RC && !load_calls); /* a bubble named after its disc */
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && load_calls == 1); /* no PSP game: the default */
    put(VJO_GAMES_PATH, "default = disabled\nNPXS10028 = enabled\nPSPEMU005 = disabled\n");
    TEST_CHECK(start(GAME_PID, "PSPEMUCFW") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK(start(GAME_PID, "PSPEMU005") == CONTINUE_RC && !load_calls); /* its own row wins */
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && !load_calls);
    /* A bubble with any other ID is the PSP emulator by its process (reVita's check). */
    TEST_CHECK(start(GAME_PID, "ABCD12345") == CONTINUE_RC && !load_calls);
    pspemu_pid = GAME_PID;
    TEST_CHECK(start(GAME_PID, "ABCD12345") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK(strstr(last_line(), "worker for ABCD12345 (PSP)") == last_line());
    put(VJO_GAMES_PATH, "default = disabled\nNPXS10028 = enabled\nPSPEMUCFW = disabled\n");
    TEST_CHECK(start(GAME_PID, "PSPEMUCFW") == CONTINUE_RC && !load_calls); /* its own row, OFF, wins */
}

/* A broken games.ini: its backup, else nothing (the Shell disables the game then). */
static void test_corrupt_policy_fails_closed(void)
{
    char big[VJO_GAMES_TEXT_MAX + 64];
    reset();
    put(VJO_GAMES_PATH, "default = garbage\n");
    put(VJO_GAMES_BACKUP, "default = disabled\nPCSG00940 = enabled\n");
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK(strstr(last_line(), ": ON in games.ini.bak: ") != NULL);
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && !load_calls);
    unlink(VJO_GAMES_BACKUP);
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls);
    TEST_CHECK_(!strcmp(last_line(), "worker for PCSG00940 (pid 0x72ED3): games.ini unreadable: not loaded"), "%s",
                last_line());
    put(VJO_GAMES_PATH, ""); /* empty: as the Shell, not the defaults */
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls);
    memset(big, 0, sizeof(big));
    for (size_t i = 0; i + 20 < sizeof(big) - 1; i += 20) memcpy(big + i, "PCSG00940 = enabled\n", 20);
    put(VJO_GAMES_PATH, big); /* over VJO_GAMES_TEXT_MAX */
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls);
    TEST_CHECK(!live_blocks && vjo_ledger_current(&g_kernel_ledger, VJO_KL_POLICY) == 0);
}

/* The kernel reads games.ini as the Shell and the Rubi app do
 * (vjo_games_file_load), on the same files: the same rows, the same fallback to
 * the backup, the same failure. */
static void test_reader_matches_the_shells(void)
{
    static const struct { const char *file, *backup; int shell_rc; } cases[] = {
        {NULL, NULL, 0},
        {"default = enabled\nPCSG00415 = disabled\n", NULL, 0},
        {"default = disabled\r\n  PCSG00940 =  enabled \r\n# comment\n; other\n\n", NULL, 0},
        {"default = garbage\n", "default = disabled\nPSPEMUCFW = enabled\n", 1},
        {"", "default = disabled\n", 1},
        {NULL, "NPXS10028 = disabled\n", 1},
        {"default = garbage\n", NULL, -1},
        {"default = garbage\n", "PCSG00940 = maybe\n", -1},
        {"", NULL, -1},
        {"PCSG00940 = enabled\nPCSG00940 = disabled\n", NULL, 0}, /* the last row wins */
    };
    static const char *const titles[] = {"PCSG00415", "PCSG00940", "PSPEMUCFW", "NPXS10028", "PSPEMU005", "VITADBDLD"};
    static unsigned char arena_mem[24 * 1024];
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        VjoGames shell, kernel;
        VjoArena arena;
        char text[VJO_GAMES_TEXT_MAX];
        int k;
        reset();
        if (cases[c].file) put(VJO_GAMES_PATH, cases[c].file);
        if (cases[c].backup) put(VJO_GAMES_BACKUP, cases[c].backup);
        vjo_arena_init(&arena, arena_mem, sizeof(arena_mem));
        TEST_CHECK_(vjo_games_file_load(&shell, &arena) == cases[c].shell_rc, "case %u", c);
        k = load_policy(&kernel, text);
        TEST_CHECK_((k == 2 ? 0 : k) == cases[c].shell_rc && (k == 2) == (!cases[c].file && !cases[c].backup),
                    "case %u: kernel %d", c, k);
        for (unsigned t = 0; t < sizeof(titles) / sizeof(titles[0]); t++)
            for (int psp = 0; psp <= 1; psp++)
                TEST_CHECK_(k < 0 ? !vjo_games_enabled_as(&shell, titles[t], psp)
                                  : vjo_games_enabled_as(&shell, titles[t], psp) ==
                                        vjo_games_enabled_as(&kernel, titles[t], psp),
                            "case %u %s psp %d", c, titles[t], psp);
    }
}

/* taiHEN's escape: L held as an app starts loads none of its plugins
 * (hen.c:368-373), and none of ours. */
static void test_l_held_loads_nothing(void)
{
    static const uint32_t held[] = {SCE_CTRL_LTRIGGER, SCE_CTRL_L1, SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER};
    reset();
    for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        pad_buttons = held[i];
        TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls && !asked && !opens);
        TEST_CHECK_(!strcmp(last_line(), "worker for PCSG00940 (pid 0x72ED3): L held at its start: not loaded"),
                    "%s", last_line());
    }
    pad_buttons = SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS; /* not L */
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1);
}

/* The kernel worker answers while the hook waits; it never answers (the hook
 * gives up within LOADER_WAIT_US); a late answer for an earlier question is not
 * taken for this one's. */
static void test_the_wait_is_bounded(void)
{
    uint64_t t0;
    reset();
    answer_mode = 1;
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1 && waits == 1);
    answer_mode = 2;
    t0 = clock_us;
    waits = opens = 0;
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls && !lock_depth);
    TEST_CHECK(clock_us - t0 <= LOADER_WAIT_US && waits >= 1 && !opens); /* the hook reads no file itself */
    TEST_CHECK_(!strcmp(last_line(), "worker for PCSG00940 (pid 0x72ED3): no answer from the kernel worker in "
                                     "500 ms: not loaded"), "%s", last_line());
    /* The worker gets to the question that timed out: an answer nobody waits for. */
    loader_tick();
    TEST_CHECK(!live_blocks);
    /* The next start: an old answer arrives first, then its own. */
    answer_mode = 1;
    stale_once = 1;
    waits = 0;
    put(VJO_GAMES_PATH, "default = disabled\n");
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls && waits == 2);
    TEST_CHECK(strstr(last_line(), ": OFF in games.ini: not loaded") != NULL);
    /* Another start holds the question longer than this one may wait. */
    lock_refused = 1;
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && !load_calls);
    TEST_CHECK(strstr(last_line(), "no answer from the kernel worker") != NULL);
}

/* A question being written (seq odd) or already answered is left alone. */
static void test_worker_answers_each_question_once(void)
{
    reset();
    ask.seq = 3; /* a hook is writing it */
    loader_tick();
    TEST_CHECK(!blocks && !ask.answer);
    ask.seq = 4;
    memcpy(ask.title, "PCSG00940", 10);
    loader_tick();
    TEST_CHECK(blocks == 1 && (ask.answer >> 8) == 4 && (ask.answer & 0xFu) == WL_ON);
    loader_tick();
    TEST_CHECK(blocks == 1); /* answered: nothing read again */
}

/* No memory for the read, no worker file, a load that fails: nothing stays and
 * each says why; the game starts as it would have. */
static void test_failures_are_logged_and_harmless(void)
{
    reset();
    alloc_fail = 1;
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls && !opens);
    TEST_CHECK_(!strcmp(last_line(), "worker for PCSG00940 (pid 0x72ED3): no kernel memory to read games.ini: "
                                     "not loaded"), "%s", last_line());
    TEST_CHECK(vjo_ledger_peak(&g_kernel_ledger, VJO_KL_POLICY) == 0);
    alloc_fail = 0;
    /* ur0:tai has none, ux0:tai has one: that one. */
    unlink(VJO_GAME_OCR_PATH_UR0);
    put(VJO_GAME_OCR_PATH_UX0, "worker");
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1 &&
               !strcmp(load_path, "ux0:tai/VitaJPOverlay_OCR.suprx"));
    unlink(VJO_GAME_OCR_PATH_UX0);
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !load_calls);
    TEST_CHECK(strstr(last_line(), ": no VitaJPOverlay_OCR.suprx in ur0:tai or ux0:tai: not loaded") != NULL);
    /* Both: ur0:tai's. */
    put(VJO_GAME_OCR_PATH_UR0, "worker");
    put(VJO_GAME_OCR_PATH_UX0, "worker");
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1 &&
               !strcmp(load_path, "ur0:tai/VitaJPOverlay_OCR.suprx"));
    /* A PSP game ON only by the shared row (OFF as a plain game): the same file choice. */
    put(VJO_GAMES_PATH, "default = disabled\nNPXS10028 = enabled\n");
    unlink(VJO_GAME_OCR_PATH_UR0);
    TEST_CHECK(start(GAME_PID, "PSPEMUCFW") == CONTINUE_RC && load_calls == 1 &&
               !strcmp(load_path, "ux0:tai/VitaJPOverlay_OCR.suprx"));
    unlink(VJO_GAME_OCR_PATH_UX0);
    TEST_CHECK(start(GAME_PID, "PSPEMUCFW") == CONTINUE_RC && !load_calls);
    TEST_CHECK(strstr(last_line(), ": no VitaJPOverlay_OCR.suprx in ur0:tai or ux0:tai: not loaded") != NULL);
    put(VJO_GAME_OCR_PATH_UR0, "worker");
    unlink(VJO_GAMES_PATH);
    /* The load itself fails (the game's memory, a bad file): its error in the line. */
    load_rc = (int)0x80024302;
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && load_calls == 1);
    TEST_CHECK_(strstr(last_line(), "ur0:tai/VitaJPOverlay_OCR.suprx 0x80024302, start 0") != NULL, "%s", last_line());
    /* A copy that leaves at once (a second copy, a safe boot): its start's answer. */
    load_rc = MOD_ID;
    load_status = 1; /* SCE_KERNEL_START_NO_RESIDENT */
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && strstr(last_line(), "0x40010129, start 1") != NULL);
    TEST_CHECK(!live_blocks && vjo_ledger_current(&g_kernel_ledger, VJO_KL_POLICY) == 0);
}

/* module_stop (kernel/main.c) releases the loader first: a start inside the hook
 * (it can wait 500 ms for the kernel worker, then load) leaves before the mutex
 * and the module go; one that stays past the bound fails the stop and keeps the
 * mutex. Every start counts itself in and out. */
static void test_release_drains_starts_inside(void)
{
    reset();
    TEST_CHECK(start(GAME_PID, "PCSG00940") == CONTINUE_RC && !inside); /* in and out */
    inside = 1; leave_after = 40; /* a start still inside: it leaves 40 ms later */
    TEST_CHECK(loader_hook_release() == 0 && !inside && hook_releases == 1);
    /* the first pause after the release, the 39 more it took, 1 ms for its last instructions */
    TEST_CHECK_(delays == 41 && mutex_deleted == 1 && !ask_lock, "delays %d", delays);
    reset();
    inside = 1; leave_after = 0; /* it never leaves */
    TEST_CHECK(loader_hook_release() == VJO_ERR_BUSY && hook_releases == 1 && !hook_uid);
    TEST_CHECK_(delays == LOADER_DRAIN_MS + 1 && !mutex_deleted && ask_lock == 0x99, "delays %d", delays);
    TEST_CHECK(strstr(last_line(), "still in the worker loader") != NULL);
    inside = 0; /* it left after all: the next stop finishes, with no hook left to release */
    delays = 0;
    TEST_CHECK(loader_hook_release() == 0 && mutex_deleted == 1 && hook_releases == 1 && !delays);
    /* No start inside: one pause after the release, nothing more. */
    reset();
    TEST_CHECK(loader_hook_release() == 0 && delays == 1 && mutex_deleted == 1);
}

/* audit-hooks F1: the worker a start ON loads reads the Shell's text source (the start is recorded before
 * the load, whose module_start reads it); a worker that a leftover *TITLEID line loads after this hook
 * (taiHEN's own hook heads the chain) reads OCR in every start this hook did not judge ON, so a game that
 * is OFF gets no native text hook. */
static int read_as(SceUID pid)
{
    SceUID keep = caller;
    int v;
    caller = pid;
    v = vjoGetTextSource();
    caller = keep;
    return v;
}

static void test_text_source_only_where_started_on(void)
{
    reset();
    caller = SHELL_PID; /* the Shell's config read */
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    caller = 0;
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && load_calls == 1); /* no games.ini: ON */
    TEST_CHECK_(load_source == (int)VJO_TEXT_SOURCE_AUTO, "%d", load_source);
    TEST_CHECK(read_as(GAME_PID) == (int)VJO_TEXT_SOURCE_AUTO); /* a second copy in the ON game: as the first */
    /* OFF in games.ini: no worker of ours; a leftover line's worker reads OCR. */
    put(VJO_GAMES_PATH, "default = enabled\nPCSG00415 = disabled\n");
    TEST_CHECK(start(0x72ED5, "PCSG00415") == CONTINUE_RC && !load_calls);
    TEST_CHECK(read_as(0x72ED5) == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(read_as(GAME_PID) == (int)VJO_TEXT_SOURCE_AUTO); /* another process's start leaves it */
    /* The same process ID at a later start that is OFF (the game turned OFF, an ID used again): OCR. */
    TEST_CHECK(start(GAME_PID, "PCSG00415") == CONTINUE_RC && !load_calls);
    TEST_CHECK(read_as(GAME_PID) == (int)VJO_TEXT_SOURCE_OCR);
    /* L held at the start, not a game, the Shell itself, an unreadable title, no answer: OCR. */
    put(VJO_GAMES_PATH, "default = enabled\n");
    TEST_CHECK(start(0x72ED7, "PCSG00940") == CONTINUE_RC && load_calls == 1 && load_source == (int)VJO_TEXT_SOURCE_AUTO);
    pad_buttons = SCE_CTRL_LTRIGGER;
    TEST_CHECK(start(0x72ED7, "PCSG00940") == CONTINUE_RC && !load_calls);
    TEST_CHECK(read_as(0x72ED7) == (int)VJO_TEXT_SOURCE_OCR);
    pad_buttons = 0;
    TEST_CHECK(start(0x72ED9, "VITASHELL") == CONTINUE_RC && !load_calls);
    TEST_CHECK(read_as(0x72ED9) == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(start(SHELL_PID, "main") == CONTINUE_RC && read_as(SHELL_PID) == (int)VJO_TEXT_SOURCE_OCR);
    title_rc = (int)0x80020003;
    TEST_CHECK(start(0x72EDB, "PCSG00940") == CONTINUE_RC && !load_calls && read_as(0x72EDB) == (int)VJO_TEXT_SOURCE_OCR);
    title_rc = 0;
    put(VJO_GAMES_PATH, "default = garbage\n");
    TEST_CHECK(start(0x72EDD, "PCSG00940") == CONTINUE_RC && !load_calls && read_as(0x72EDD) == (int)VJO_TEXT_SOURCE_OCR);
    unlink(VJO_GAMES_PATH);
    answer_mode = 2; /* the kernel worker never answers */
    TEST_CHECK(start(0x72EDF, "PCSG00940") == CONTINUE_RC && !load_calls && read_as(0x72EDF) == (int)VJO_TEXT_SOURCE_OCR);
    answer_mode = 0;
    /* ON: what the Shell sets later reaches the next start's worker. */
    caller = SHELL_PID;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    caller = 0;
    TEST_CHECK(start(0x72EE1, "PCSG00940") == CONTINUE_RC && load_calls == 1 && load_source == (int)VJO_TEXT_SOURCE_HOOKS);
    /* A safe boot: OCR for every worker, the ON game's too. */
    boot = VJO_BOOT_SAFE;
    TEST_CHECK(start(0x72EE3, "PCSG00940") == CONTINUE_RC && load_calls == 1 && load_source == (int)VJO_TEXT_SOURCE_OCR);
    boot = VJO_BOOT_NORMAL;
    /* The loader off (its hook not installed): no start is recorded, every worker reads OCR. */
    loader_hook_release();
    fail_360 = fail_363 = 1;
    TEST_CHECK(loader_hook_install() < 0 && !hooked);
    TEST_CHECK(read_as(0x72EE5) == (int)VJO_TEXT_SOURCE_OCR);
}

TEST_LIST = {
    {"module_stop waits for a start inside the hook, bounded", test_release_drains_starts_inside},
    {"hook install: 3.60 then 3.63+ NIDs, off without its functions", test_hook_install_and_release},
    {"never into SceShell, system apps, VitaShell, the Rubi app", test_never_into_shell_or_system_apps},
    {"a game without a taiHEN line, ON by the default, gets the worker", test_on_by_default_loads_the_worker},
    {"a title's own row, then the default", test_rows_and_default},
    {"Adrenaline: PSPEMUCFW, bubbles, NPXS10028 and the shared PSP row", test_adrenaline_ids},
    {"a corrupt games.ini: its backup, else no worker", test_corrupt_policy_fails_closed},
    {"the kernel reads games.ini as the Shell does", test_reader_matches_the_shells},
    {"L held at the start loads nothing", test_l_held_loads_nothing},
    {"the wait for the kernel worker is bounded and ignores old answers", test_the_wait_is_bounded},
    {"the kernel worker answers each question once", test_worker_answers_each_question_once},
    {"failures are logged and leave nothing", test_failures_are_logged_and_harmless},
    {"the text source only for a process started ON (audit-hooks F1)", test_text_source_only_where_started_on},
    {NULL, NULL}};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("kernel-loader")) return 2; /* each test's card in it, all removed at the end */
    return acutest_main(argc, argv);
}
