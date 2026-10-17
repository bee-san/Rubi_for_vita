/* The Rubi app's start as a system application with no game running, on the host
 * (settings/sysapp_gfx.c, owner 2026-10-16: "I can't open the Rubi app first. First I have
 * to open a game ..."). libvita2d's own start (host/tests/sysapp_gfx_model.c, a copy of the
 * pinned libvita2d's) runs against a model of the system, through the link's --wrap as on
 * the Vita (CMakeLists.txt). The model:
 *
 * - CPU3 is one core with fixed priorities and no time slicing (SDK Kernel-Overview, "Thread
 *   Scheduling"). The app's main thread runs there at 96 (0x10000100 in a mini app: wiki
 *   SceKernelThreadMgr, "Thread Priority"), at or above most of SceShell's job threads there
 *   (96-120 in the SceShell dumps in test.3's collect). The model takes the thread that hands
 *   the framebuffer over to be one of them (not known), so SceShell gets CPU3 time only while
 *   the app's thread waits: here, only in sceKernelDelayThread; each other call the app makes
 *   takes SYSCALL_US of CPU3 from it.
 * - SceShell hands over the shared framebuffer's second buffer (index 2, fb_base2) once it
 *   has had shell_needs_us of CPU3. Before that, the framebuffer's render info is the world's
 *   first_index: 1 (buffer 1 handed over), or 0 (none yet: fb_base2 NULL, appmgr's
 *   sharedfb.c fills both from SceShell's sceSharedFbUpdateProcessBegin), and the
 *   framebuffer may not open at all (open_fails).
 * - sceGxmVshInitialize with SHAREDPB_OPEN needs a shared parameter buffer another process
 *   made; with neither OPEN nor CREATE it makes the app its own, in CDRAM unless PB_LPDDR (a
 *   system application has none); CREATE and OPEN together are refused (shoui520/psp2
 *   src/os0/us/libgxm_es4/source/init.c and gpu_es4 client_render.c, a reimplementation
 *   from the firmware).
 *
 * "Beside a game" is the model's reading of test.3's console run, where the start worked: the
 * hand-over there at the first read (test.3 logged no read count: an assumption) and a shared
 * parameter buffer (the app's main memory, 38,899,712 B free at its start and 28,286,976 B 60
 * frames later, fits libvita2d's blocks without a 2 MiB buffer of its own: an inference). Which
 * of the no-game cases a console has, if any, is not known: each is a test. A start the model's
 * clock runs past 30 s in ends as a failure (STUCK), never a hang. */
#include "acutest.h"
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sharedfb.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../settings/sysapp_gfx.h"

void vjo_model_vita2d_init(void *display[2]);

#define SYSCALL_US 5u          /* CPU3 time each of the app's calls takes from SceShell */
#define SPIN_LIMIT 2000000u    /* framebuffer reads in a row with no pause: SceShell never runs */
#define STUCK_US (3 * (uint64_t)VJO_GFX_GIVE_UP_US) /* the model's clock past this: the start never ends */
#define NEVER UINT64_MAX
#define FB_OPEN_ERROR ((int)0x80200004) /* appmgr sharedfb.c: no such framebuffer */
#define OUT_OF_MEMORY ((int)0x805B0002) /* SCE_GXM_ERROR_OUT_OF_MEMORY */

typedef struct {
    int system_app;           /* sceAppMgrGetBudgetInfo answers (it does only for a system application) */
    int shared_pb;            /* a shared parameter buffer exists */
    int own_pb_refused;       /* and the app's own one is refused (no memory) */
    int open_fails;           /* the framebuffer does not open before the hand-over */
    int first_index;          /* the render info before the hand-over: 0 none, 1 buffer 1, 2 buffer 2 */
    uint64_t shell_needs_us;  /* CPU3 time SceShell needs to hand over buffer 2 (NEVER: it does not) */
    int stale_base2;          /* index 0 with the second buffer set (an earlier hand-over's render info) */
    int no_base;              /* before the hand-over, the first buffer NULL too (index 2, as if handed over) */
    uint64_t start_at_us;     /* the process's clock when libvita2d starts (the app's start before it) */
} World;

/* What one start did, from its process (each runs in its own: the wrappers keep their state in statics). */
typedef struct {
    int ended;                /* 0 returned, 1 the process exited, 2 it spun (SPIN_LIMIT), 3 stuck (STUCK_US) */
    int exit_status;
    uint64_t now, shell_cpu, delayed_us, exit_at;
    unsigned gxm_calls, flags[4], opens, reads, closes, delays;
    int rcs[4], gxm_up;
    int gpu_used_down;        /* the GPU library was used before it had started */
    char gpu_what[64];
    int display_ok[2];        /* the display buffers libvita2d draws into are the framebuffer's */
    char log[8][320];
    int logs;
    VjoSysappGfx gfx;         /* the wrappers' record */
} Result;

static World world;
static Result *res;
static jmp_buf process_end;
static unsigned spin;
static uint8_t buffers[2][64]; /* the framebuffer's two buffers: only their addresses matter */

static int handed_over(void) { return res->shell_cpu >= world.shell_needs_us; }

/* ---- the system, as the model has it (the wrappers' __real_ calls land here) ---- */
int vjo_model_budget_info(void) { return world.system_app; }

void vjo_model_gpu_used(const char *what)
{
    if (!res->gxm_up && !res->gpu_used_down) {
        res->gpu_used_down = 1;
        snprintf(res->gpu_what, sizeof(res->gpu_what), "%s", what);
    }
}

static int gxm_init(const SceGxmInitializeParams *p, int vsh)
{
    int rc;
    res->now += SYSCALL_US;
    if (res->gxm_up) rc = SCE_GXM_ERROR_ALREADY_INITIALIZED;
    else if (vsh && ((p->flags & ~0x1Fu) || (p->flags & 0xCu) == 0xCu)) rc = SCE_GXM_ERROR_INVALID_VALUE;
    else if (vsh && (p->flags & SCE_GXM_INITIALIZE_FLAG_SHAREDPB_OPEN)) rc = world.shared_pb ? 0 : SCE_GXM_ERROR_DRIVER;
    else if (vsh && (world.own_pb_refused || !(p->flags & SCE_GXM_INITIALIZE_FLAG_PB_LPDDR))) rc = OUT_OF_MEMORY;
    else rc = 0;
    if (!rc) res->gxm_up = 1;
    if (res->gxm_calls < 4) {
        res->flags[res->gxm_calls] = p->flags;
        res->rcs[res->gxm_calls] = rc;
    }
    res->gxm_calls++;
    return rc;
}
int sceGxmVshInitialize(const SceGxmInitializeParams *p) { return gxm_init(p, 1); }
int sceGxmInitialize(const SceGxmInitializeParams *p) { return gxm_init(p, 0); }

SceUID sceSharedFbOpen(int index)
{
    res->now += SYSCALL_US;
    res->opens++;
    return index == 1 && (!world.open_fails || handed_over()) ? 0x40010123 : FB_OPEN_ERROR;
}

int sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info)
{
    res->now += SYSCALL_US;
    res->reads++;
    if (++spin > SPIN_LIMIT) longjmp(process_end, 2);
    if (fb_id < 0) return FB_OPEN_ERROR; /* the caller's info as it was: libvita2d's zeros */
    int index = handed_over() ? 2 : world.first_index;
    info->fb_base = world.no_base && !handed_over() ? NULL : buffers[0];
    info->fb_size = 0x400000;
    info->fb_base2 = index == 2 || (index == 0 && world.stale_base2) ? buffers[1] : index == 1 ? buffers[0] : NULL;
    info->stride = info->width = 960;
    info->height = 544;
    info->index = index;
    return 0;
}

int sceSharedFbClose(SceUID fb_id)
{
    res->now += SYSCALL_US;
    res->closes++;
    return fb_id < 0 ? FB_OPEN_ERROR : 0;
}

int sceKernelDelayThread(SceUInt us)
{
    res->now += us;
    res->shell_cpu += us; /* the app's thread waits: SceShell has CPU3 */
    res->delays++;
    res->delayed_us += us;
    spin = 0;
    if (res->now > STUCK_US) longjmp(process_end, 3); /* a wait with no end: fail, never hang */
    return 0;
}

uint64_t sceKernelGetProcessTimeWide(void) { return res->now; }
int sceKernelGetThreadCurrentPriority(void) { return 96; }
int sceKernelGetThreadCpuAffinityMask(SceUID thid) { (void)thid; return 0x80000; }

int sceKernelExitProcess(int status)
{
    res->exit_status = status;
    res->exit_at = res->now;
    longjmp(process_end, 1); /* the process ends: no call of it comes back */
}

/* settings/applog.c's vjo_log: the lines, as log.txt would get them */
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    if (res->logs >= 8) return;
    va_start(ap, fmt);
    vsnprintf(res->log[res->logs++], sizeof(res->log[0]), fmt, ap);
    va_end(ap);
}

/* ---- one start of the app ---- */
static Result *shared;

static void start(void)
{
    void *display[2] = {NULL, NULL};
    int ended = setjmp(process_end);
    if (!ended) {
        vjo_model_vita2d_init(display);
        res->display_ok[0] = display[0] == buffers[0];
        res->display_ok[1] = display[1] == buffers[1];
        res->gfx = *vjo_sysapp_gfx();
        return;
    }
    res->ended = ended;
    res->gfx = *vjo_sysapp_gfx();
}

static Result run(World w)
{
    Result out;
    if (!shared) {
        shared = mmap(NULL, sizeof(Result), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        TEST_ASSERT(shared != MAP_FAILED);
    }
    memset(shared, 0, sizeof(*shared));
    fflush(NULL);
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);
    if (!pid) {
        world = w;
        res = shared;
        res->now = w.start_at_us;
        spin = 0;
        start();
        _exit(0);
    }
    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_ASSERT_(WIFEXITED(status) && !WEXITSTATUS(status), "the start's process ended with status 0x%x", status);
    out = *shared;
    return out;
}

static const char *log_with(const Result *r, const char *needle)
{
    for (int i = 0; i < r->logs; i++)
        if (strstr(r->log[i], needle)) return r->log[i];
    return NULL;
}

/* The start finished with the framebuffer's two buffers to draw into and the GPU library up. */
static void started(const Result *r)
{
    TEST_CHECK_(r->ended == 0, "the start %s after %u reads (%llu us)", r->ended == 2 ? "spun on CPU3: SceShell never ran"
                : r->ended == 3 ? "never ended (the model's clock passed 30 s)" : "ended the process", r->reads,
                (unsigned long long)r->now);
    TEST_CHECK_(!r->gpu_used_down, "the GPU library was used before it started: %s", r->gpu_what);
    TEST_CHECK_(r->display_ok[0] && r->display_ok[1], "the display buffers are %s and %s", r->display_ok[0] ? "right" :
                "wrong", r->display_ok[1] ? "right" : "wrong (NULL: drawn into address 0)");
}

/* ---- beside a game: what test.3's console run had ---- */
static const World beside_a_game = {1, 1, 0, 0, 2, 0, 0, 0, 0};

/* The working path is unchanged: libvita2d's flags once, one read, no pause. */
static void beside_a_game_nothing_changes(void)
{
    Result r = run(beside_a_game);
    started(&r);
    TEST_CHECK_(r.gxm_calls == 1 && r.flags[0] == VJO_GFX_FLAGS_LIBVITA2D && r.rcs[0] == 0, "%u calls, flags 0x%02X: 0x%08X",
                r.gxm_calls, r.flags[0], (unsigned)r.rcs[0]);
    TEST_CHECK_(r.opens == 1 && r.reads == 1 && r.closes == 0 && r.delays == 0, "%u opens, %u reads, %u closes, %u pauses",
                r.opens, r.reads, r.closes, r.delays);
    TEST_CHECK_(r.logs <= 1 && !log_with(&r, "waiting"), "%d log lines", r.logs);
}

/* ---- no game: each way the start can depend on SceShell ---- */

/* Whatever libvita2d takes (any index but 1) and can draw with (both buffers there) goes to it
 * at once, as on main: index 0 with an earlier hand-over's second buffer. */
static void what_libvita2d_takes_and_can_draw_with_is_unchanged(void)
{
    World w = beside_a_game;
    w.first_index = 0;
    w.stale_base2 = 1;
    w.shell_needs_us = NEVER;
    Result r = run(w);
    started(&r);
    TEST_CHECK_(r.reads == 1 && r.delays == 0 && r.gfx.index == 0, "%u reads, %u pauses, index %d", r.reads, r.delays,
                r.gfx.index);
}

/* Buffer 1 handed over; buffer 2 needs 5 ms of SceShell's CPU3 time. */
static void no_game_the_hand_over_needs_cpu3(void)
{
    World w = beside_a_game;
    w.first_index = 1;
    w.shell_needs_us = 5000;
    Result r = run(w);
    started(&r);
    TEST_CHECK_(r.delays >= 5 && r.delays <= 6 && r.delayed_us == r.delays * VJO_GFX_RETRY_US, "%u pauses, %llu us",
                r.delays, (unsigned long long)r.delayed_us);
    TEST_CHECK(r.gfx.ready && r.gfx.reads == r.reads);
    TEST_CHECK_(!log_with(&r, "waiting"), "a wait under a second is not logged: %s", r.logs ? r.log[0] : "-");
}

/* No hand-over yet: the render info is empty (index 0, no second buffer). libvita2d leaves
 * its loop on any index but 1, with a NULL second buffer. */
static void no_game_before_the_first_hand_over(void)
{
    World w = beside_a_game;
    w.first_index = 0;
    w.shell_needs_us = 5000;
    Result r = run(w);
    started(&r);
}

/* The framebuffer does not open before SceShell's hand-over: the read fails, the caller's zeros stay. */
static void no_game_the_framebuffer_does_not_open_yet(void)
{
    World w = beside_a_game;
    w.open_fails = 1;
    w.first_index = 0;
    w.shell_needs_us = 5000;
    Result r = run(w);
    started(&r);
    TEST_CHECK_(r.gfx.fb_rc == 0 && r.gfx.fb_id >= 0, "the last read: id 0x%08X, rc 0x%08X", (unsigned)r.gfx.fb_id,
                (unsigned)r.gfx.fb_rc);
}

/* No shared parameter buffer to open: the app makes its own in main memory (PB_LPDDR). */
static void no_game_no_shared_parameter_buffer(void)
{
    World w = beside_a_game;
    w.shared_pb = 0;
    Result r = run(w);
    started(&r);
    TEST_CHECK_(r.gxm_calls == 2 && r.flags[0] == VJO_GFX_FLAGS_LIBVITA2D && r.rcs[0] == SCE_GXM_ERROR_DRIVER &&
                r.flags[1] == VJO_GFX_FLAGS_OWN_PB && r.rcs[1] == 0, "%u calls: 0x%02X 0x%08X, 0x%02X 0x%08X", r.gxm_calls,
                r.flags[0], (unsigned)r.rcs[0], r.flags[1], (unsigned)r.rcs[1]);
}

/* All of it at once. */
static void no_game_everything_late(void)
{
    World w = {1, 0, 0, 1, 0, 20000, 0, 0, 0};
    Result r = run(w);
    started(&r);
}

/* A read that says buffer 2 with no first buffer: libvita2d would draw into NULL; it waits. */
static void a_read_with_no_first_buffer_waits(void)
{
    World w = beside_a_game;
    w.no_base = 1;
    w.shell_needs_us = 5000;
    Result r = run(w);
    started(&r);
    TEST_CHECK_(r.delays >= 5, "%u pauses", r.delays);
}

/* The wait's line comes after a second, once: a hand-over at 0.5 s logs none, one at 1.5 s
 * logs it at its first read past the second. */
static void a_wait_over_a_second_is_logged_once(void)
{
    World w = beside_a_game;
    w.first_index = 1;
    w.shell_needs_us = 500000;
    Result r = run(w);
    started(&r);
    TEST_CHECK_(!log_with(&r, "waiting"), "half a second: %s", log_with(&r, "waiting"));
    w.shell_needs_us = 1500000;
    r = run(w);
    started(&r);
    const char *waiting = log_with(&r, "graphics: waiting for the display: nothing handed over in ");
    unsigned ms = 0;
    TEST_CHECK_(waiting && sscanf(waiting, "graphics: waiting for the display: nothing handed over in %u ms", &ms) == 1 &&
                ms >= 1000 && ms < 1020 && strstr(waiting, "last: buffer index 1)"), "%s", waiting ? waiting : "-");
    TEST_CHECK_(r.logs == 2, "%d lines: the wait's and the hand-over's", r.logs);
}

/* The 10 s count from libvita2d's first read, not the process's start: an app whose own start
 * took 5 s still waits 10 s, and a hand-over 7 s after the first read is taken. */
static void the_ten_seconds_count_from_the_first_read(void)
{
    World w = beside_a_game;
    w.first_index = 1;
    w.start_at_us = 5000000;
    w.shell_needs_us = 7000000;
    Result r = run(w);
    started(&r);
    w.shell_needs_us = NEVER;
    r = run(w);
    TEST_CHECK_(r.ended == 1 && r.exit_at >= w.start_at_us + VJO_GFX_GIVE_UP_US &&
                r.exit_at <= w.start_at_us + VJO_GFX_GIVE_UP_US + 20000, "ended %d at %llu us", r.ended,
                (unsigned long long)r.exit_at);
}

/* SceShell never hands over: the app closes within 10 s, says so in log.txt (once while it
 * waits, after a second, then why it closes), and pauses between reads all the while. */
static void a_display_never_handed_over_closes_the_app(void)
{
    World w = beside_a_game;
    w.first_index = 1;
    w.shell_needs_us = NEVER;
    Result r = run(w);
    TEST_CHECK_(r.ended == 1, "the start %s", r.ended == 2 ? "spun on CPU3" : r.ended == 3 ? "never closed (30 s)" :
                r.ended ? "?" : "returned");
    TEST_CHECK_(r.exit_at >= VJO_GFX_GIVE_UP_US && r.exit_at <= VJO_GFX_GIVE_UP_US + 20000, "closed at %llu us",
                (unsigned long long)r.exit_at);
    TEST_CHECK_(r.delays > 9000 && r.reads <= r.delays + 1, "%u reads, %u pauses", r.reads, r.delays);
    const char *waiting = log_with(&r, "graphics: waiting for the display");
    const char *closing = log_with(&r, "graphics: no display from the system");
    TEST_CHECK_(waiting && closing && r.logs == 2, "%d lines: %s | %s", r.logs, r.logs > 0 ? r.log[0] : "-",
                r.logs > 1 ? r.log[1] : "-");
    TEST_CHECK_(closing && strstr(closing, "buffer index 1") && strstr(closing, "closing"), "%s", closing ? closing : "-");
    TEST_CHECK(!r.gpu_used_down);
}

/* The GPU library refuses the shared buffer and the app's own: the app closes before
 * libvita2d uses a library it never started, and log.txt says why. */
static void a_gpu_library_refused_twice_closes_the_app(void)
{
    World w = beside_a_game;
    w.shared_pb = 0;
    w.own_pb_refused = 1;
    Result r = run(w);
    TEST_CHECK_(r.ended == 1 && r.exit_status == 1, "ended %d, status %d", r.ended, r.exit_status);
    TEST_CHECK_(!r.gpu_used_down, "used before it started: %s", r.gpu_what);
    TEST_CHECK_(r.opens == 0, "%u framebuffer opens after the refusal", r.opens);
    const char *line = log_with(&r, "graphics: the GPU library refused the app");
    TEST_CHECK_(line && strstr(line, "0x0A: 0x805B0017") && strstr(line, "0x03: 0x805B0002"), "%s", line ? line : "-");
}

/* ---- log.txt ---- */

/* The hand-over's line names what happened: the flags and results, the reads and the wait,
 * and the main thread's priority and CPU mask (the console's own figures; 96 and CPU3 are
 * the model's). It is written as the display is handed over, so a start that stops later
 * still has it. */
static void the_hand_over_line_says_what_the_start_did(void)
{
    Result r = run(beside_a_game);
    const char *line = log_with(&r, "graphics: system application");
    TEST_CHECK_(line && !strcmp(line, "graphics: system application; GPU library 0x0A: 0x00000000; display: buffer 2 "
                "at read 1 after 0 ms; main thread priority 96, CPU mask 0x00080000"), "%s", line ? line : "-");
    TEST_CHECK(r.logs == 1);
    World w = beside_a_game;
    w.shared_pb = 0;
    w.first_index = 1;
    w.shell_needs_us = 37000;
    r = run(w);
    line = log_with(&r, "graphics: system application");
    TEST_CHECK_(line && !strcmp(line, "graphics: system application; GPU library 0x0A: 0x805B0017, then its own "
                "parameter buffer 0x03: 0x00000000; display: buffer 2 at read 38 after 37 ms; main thread priority 96, "
                "CPU mask 0x00080000"), "%s", line ? line : "-");
    TEST_CHECK(r.logs == 1);
}

/* An ordinary application (Vita3K's stub, or no system budget): libvita2d starts its own
 * way, neither wrapper is called, nothing is logged. */
static void an_ordinary_application_starts_as_before(void)
{
    World w = beside_a_game;
    w.system_app = 0;
    Result r = run(w);
    /* the ordinary path draws into buffers of its own, which the model does not make */
    TEST_CHECK_(r.ended == 0 && !r.gpu_used_down, "ended %d; used before it started: %s", r.ended, r.gpu_what);
    TEST_CHECK(r.gxm_calls == 1 && r.flags[0] == 0 && r.opens == 0 && r.gfx.gxm_calls == 0 && r.logs == 0);
}

TEST_LIST = {
    {"beside a game: nothing changes", beside_a_game_nothing_changes},
    {"what libvita2d takes and can draw with is unchanged", what_libvita2d_takes_and_can_draw_with_is_unchanged},
    {"no game: the hand-over needs CPU3", no_game_the_hand_over_needs_cpu3},
    {"no game: before the first hand-over", no_game_before_the_first_hand_over},
    {"no game: the framebuffer does not open yet", no_game_the_framebuffer_does_not_open_yet},
    {"no game: no shared parameter buffer", no_game_no_shared_parameter_buffer},
    {"no game: everything late", no_game_everything_late},
    {"a read with no first buffer waits", a_read_with_no_first_buffer_waits},
    {"a wait over a second is logged once", a_wait_over_a_second_is_logged_once},
    {"the ten seconds count from the first read", the_ten_seconds_count_from_the_first_read},
    {"a display never handed over closes the app", a_display_never_handed_over_closes_the_app},
    {"a GPU library refused twice closes the app", a_gpu_library_refused_twice_closes_the_app},
    {"the hand-over line says what the start did", the_hand_over_line_says_what_the_start_did},
    {"an ordinary application starts as before", an_ordinary_application_starts_as_before},
    {NULL, NULL}
};
