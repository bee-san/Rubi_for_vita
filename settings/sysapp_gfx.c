/* The Overlay app's graphics start as a system application, with or without a game running.
 *
 * As a system application the app draws into SceShell's shared framebuffer, and libvita2d's
 * start (a8f15ab vita2d.c:233-262, the pinned libvita2d.a) waits on SceShell twice before its
 * first frame:
 *
 * - sceGxmVshInitialize(SHARED_SYNC | SHAREDPB_OPEN, 0x0A): the GPU library opens the parameter
 *   buffer another process made. libvita2d keeps no result: refused, the GPU library stays down
 *   and every later call of it fails (the rings, the render target, the shaders).
 * - its framebuffer loop: open the shared framebuffer, read it, close it and try again while its
 *   buffer index is 1, with no pause. The index is the buffer SceShell last handed over, which
 *   changes only when SceShell's thread that hands it over runs. The app's main thread runs on
 *   CPU3 at 96 (0x10000100 in a mini application, wiki SceKernelThreadMgr), and while the loop
 *   spins SceShell's threads at or below 96 on CPU3 do not run (most of its job threads: 96-120 in
 *   the SceShell dumps in test.3's collect; SceShellMain, at 76, does). A read before any hand-over
 *   (index 0, the second buffer NULL) ends the loop with a NULL buffer to draw into.
 *
 * (The third wait, libvita2d's sceSharedFbBegin at each frame, vita2d.c:859-860, most likely
 * blocks in the kernel, so SceShell's threads run meanwhile: an inference; it is not wrapped.) Beside a game (test.3's console
 * run) the start worked; how many reads its hand-over took was not logged. With no game running
 * the app does not open (owner, 2026-10-16), and where it stops is not known: main.c's start
 * line and this file's line in log.txt will say (docs/vita-memory.md section 5.4, "Starting with
 * no game running", has what each means).
 *
 * The link wraps libvita2d's two calls into the system (vita/CMakeLists.txt --wrap; the build
 * checks that libvita2d calls them, tools/check_settings_self.py):
 *
 * - the GPU library: refused, it is asked again for the app's own parameter buffer in main memory
 *   (the same flags without SHAREDPB_OPEN, with PB_LPDDR: a system application has no CDRAM; the
 *   2 MiB libvita2d asks for, which docs/vita-memory.md section 5.4 already counts). Refused
 *   again, the app closes with a log.txt line instead of drawing with a GPU library it never had.
 * - the framebuffer read: an answer libvita2d would take but cannot draw with (no framebuffer, a
 *   failed read, no second buffer: index 0 before SceShell's first hand-over), or one it retries
 *   (buffer 1 still the last one), is given to it as "buffer 1", on which libvita2d closes the
 *   framebuffer and opens it again, after a 1 ms pause in which SceShell's threads at or below the
 *   app's priority run. A wait over a second is logged; after 10 s the app closes with a log.txt
 *   line (below appmgr's own 20 s for an app that does not end its frame).
 *
 * Every answer libvita2d takes and can draw with goes to it unchanged: beside a game nothing
 * changes but one log.txt append, the hand-over's line, in both builds. */
#include "sysapp_gfx.h"
#include "anki_page.h" /* vjo_log: log.txt */
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sharedfb.h>
#include <stdio.h>
#include <string.h>

static VjoSysappGfx gfx = {.fb_id = -1};

const VjoSysappGfx *vjo_sysapp_gfx(void) { return &gfx; }

/* The process ends here: what libvita2d holds goes with it, as at the app's own exit. */
static void close_app(void)
{
    sceKernelExitProcess(1);
    for (;;) sceKernelDelayThread(100 * 1000); /* an exit that comes back (Vita3K's): never return into libvita2d */
}

static void note_gxm(unsigned flags, int rc)
{
    if (gfx.gxm_calls < 2) {
        gfx.flags[gfx.gxm_calls] = flags;
        gfx.rc[gfx.gxm_calls] = rc;
    }
    gfx.gxm_calls++;
}

int __real_sceGxmVshInitialize(const SceGxmInitializeParams *params);
int __wrap_sceGxmVshInitialize(const SceGxmInitializeParams *params)
{
    int rc = __real_sceGxmVshInitialize(params);
    if (!params) return rc;
    note_gxm(params->flags, rc);
    if (rc >= 0) return rc;
    if (params->flags & SCE_GXM_INITIALIZE_FLAG_SHAREDPB_OPEN) {
        SceGxmInitializeParams own = *params;
        own.flags = (params->flags & ~(SCE_GXM_INITIALIZE_FLAG_SHAREDPB_OPEN | SCE_GXM_INITIALIZE_FLAG_SHAREDPB_CREATE)) |
                    SCE_GXM_INITIALIZE_FLAG_PB_LPDDR;
        rc = __real_sceGxmVshInitialize(&own);
        note_gxm(own.flags, rc);
        if (rc >= 0) return rc;
        vjo_log("graphics: the GPU library refused the app: 0x%02X: 0x%08X, then its own parameter buffer 0x%02X: 0x%08X; "
                "closing", gfx.flags[0], (unsigned)gfx.rc[0], gfx.flags[1], (unsigned)gfx.rc[1]);
    } else {
        vjo_log("graphics: the GPU library refused the app: 0x%02X: 0x%08X; closing", params->flags, (unsigned)rc);
    }
    close_app();
    return rc;
}

/* What the last read said, for log.txt. */
static void last_read(char *out, size_t cap)
{
    if (gfx.fb_id < 0) snprintf(out, cap, "the framebuffer did not open: 0x%08X", (unsigned)gfx.fb_id);
    else if (gfx.fb_rc < 0) snprintf(out, cap, "its read failed: 0x%08X", (unsigned)gfx.fb_rc);
    else snprintf(out, cap, "buffer index %d", gfx.index);
}

static unsigned ms_since_first(uint64_t now) { return (unsigned)((now - gfx.first_us) / 1000u); }

/* The hand-over's line: what the GPU library and the framebuffer did, and the main thread's
 * place on the CPU (the console's own figures for docs/vita-memory.md section 5.4). */
static void log_ready(void)
{
    char gpu[96];
    if (gfx.gxm_calls >= 2)
        snprintf(gpu, sizeof(gpu), "0x%02X: 0x%08X, then its own parameter buffer 0x%02X: 0x%08X", gfx.flags[0],
                 (unsigned)gfx.rc[0], gfx.flags[1], (unsigned)gfx.rc[1]);
    else if (gfx.gxm_calls)
        snprintf(gpu, sizeof(gpu), "0x%02X: 0x%08X", gfx.flags[0], (unsigned)gfx.rc[0]);
    else
        snprintf(gpu, sizeof(gpu), "-");
    vjo_log("graphics: system application; GPU library %s; display: buffer %d at read %u after %u ms; main thread "
            "priority %d, CPU mask 0x%08X", gpu, gfx.index, gfx.reads, ms_since_first(gfx.ready_us),
            sceKernelGetThreadCurrentPriority(), (unsigned)sceKernelGetThreadCpuAffinityMask(0));
}

int __real_sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info);
int __wrap_sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info)
{
    int rc = __real_sceSharedFbGetInfo(fb_id, info);
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!info || gfx.ready) return rc; /* libvita2d reads it only to start */
    if (!gfx.reads++) gfx.first_us = now;
    gfx.fb_id = fb_id;
    gfx.fb_rc = rc;
    gfx.index = rc >= 0 ? info->index : -1;
    /* What libvita2d takes (any index but 1: it then draws into fb_base and fb_base2), when both
     * are there: before SceShell's first hand-over the second is NULL (index 0) */
    if (fb_id >= 0 && rc >= 0 && info->index != 1 && info->fb_base && info->fb_base2) {
        gfx.ready = 1;
        gfx.ready_us = now;
        log_ready();
        return rc;
    }
    char why[64];
    if (now - gfx.first_us >= VJO_GFX_GIVE_UP_US) {
        last_read(why, sizeof(why));
        vjo_log("graphics: no display from the system in %u s (%u reads; last: %s); closing",
                VJO_GFX_GIVE_UP_US / 1000000u, gfx.reads, why);
        close_app();
    }
    if (!gfx.waiting_logged && now - gfx.first_us >= VJO_GFX_LOG_AFTER_US) {
        gfx.waiting_logged = 1;
        last_read(why, sizeof(why));
        vjo_log("graphics: waiting for the display: nothing handed over in %u ms (%u reads; last: %s)",
                ms_since_first(now), gfx.reads, why);
    }
    sceKernelDelayThread(VJO_GFX_RETRY_US); /* SceShell's turn on CPU3 */
    info->index = 1;                         /* libvita2d closes the framebuffer and opens it again */
    return rc;
}
