/* The Overlay app's Vita3K smoke (tools/vita3k_settings_smoke.sh). settings/main.c and
 * the sources it ships with are linked unchanged with these wrappers (-Wl,--wrap,
 * vita/CMakeLists.txt VJO_SETTINGS_SMOKE): the controller follows a script by frame,
 * and the icon textures, their draws and the GPU waits are counted. A texture freed
 * after being drawn with no GPU wait in between is counted as unsafe (libvita2d #28).
 * The results go to ux0:data/vjo_settings_smoke.txt: a progress line every 60 frames
 * (a crash shows where it was) and the totals when the app exits after the script's
 * Cancel. A crash check of the app's own code in Vita3K, not evidence about the
 * console (docs/offline-testing.md). Never part of the release. */
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>

#include "../vita/buttons_vita.h"

int __real_sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count);
int __real_sceKernelExitProcess(int status);
vita2d_texture *__real_vita2d_load_PNG_buffer(const void *buffer);
vita2d_texture *__real_vita2d_load_PNG_file(const char *filename);
void __real_vita2d_free_texture(vita2d_texture *texture);
void __real_vita2d_wait_rendering_done(void);
void __real_vita2d_draw_texture_scale(const vita2d_texture *texture, float x, float y, float x_scale, float y_scale);

/* The script: frames to hold each set of buttons (0: none). ENTER and CANCEL
 * are the console's enter and cancel buttons (core/buttons.h), read from the
 * same setting as the app (vita/buttons_vita.c): ○ and × trade places with it
 * (#106), and results.txt says which one entered. */
typedef struct { unsigned short frames; unsigned buttons; } Step;
#define TAP(b, gap) {1, (b)}, {(gap), 0}
#define ENTER 0x01000000u  /* no SceCtrl button has this bit: replaced by the enter button */
#define CANCEL 0x02000000u /* nor this one: replaced by the cancel button */
#if VJO_SMOKE_SYSAPP
/* The system-application start (below): each page drawn into the shared framebuffer, then Cancel. */
static const Step script[] = {
    {60, 0}, {60, SCE_CTRL_DOWN}, {10, 0},          /* the Games list, icons loading, moved */
    TAP(SCE_CTRL_SELECT, 40), TAP(SCE_CTRL_DOWN, 10), /* OCR & lookup, a card down */
    TAP(SCE_CTRL_SELECT, 40), TAP(SCE_CTRL_SELECT, 60), /* Anki, Install */
    TAP(SCE_CTRL_SELECT, 40),                         /* Games */
    TAP(CANCEL, 0),
};
#else
static const Step script[] = {
    {90, 0},                                         /* the window's icons load, one a frame */
    {300, SCE_CTRL_DOWN}, {30, 0}, {300, SCE_CTRL_UP}, {30, 0},  /* down about 47 rows, back */
    TAP(SCE_CTRL_RTRIGGER, 14), TAP(SCE_CTRL_RTRIGGER, 14), TAP(SCE_CTRL_RTRIGGER, 14),
    TAP(SCE_CTRL_RTRIGGER, 2), TAP(SCE_CTRL_RTRIGGER, 2),       /* pages, also faster than the prefetch */
    {240, SCE_CTRL_RTRIGGER}, {20, 0},                          /* held: to the last page */
    TAP(SCE_CTRL_LTRIGGER, 14), TAP(SCE_CTRL_LTRIGGER, 14), {240, SCE_CTRL_LTRIGGER}, {20, 0},
    TAP(SCE_CTRL_RIGHT, 8), TAP(SCE_CTRL_RIGHT, 8), TAP(SCE_CTRL_RIGHT, 8), TAP(SCE_CTRL_RIGHT, 1),
    {120, SCE_CTRL_RIGHT}, {20, 0}, {120, SCE_CTRL_LEFT}, {20, 0},      /* letters, held */
    TAP(ENTER, 20), TAP(SCE_CTRL_DOWN, 10), TAP(ENTER, 20),             /* two games off */
    TAP(SCE_CTRL_SQUARE, 30), TAP(SCE_CTRL_SQUARE, 30), TAP(SCE_CTRL_SQUARE, 30), /* the filters */
    TAP(ENTER, 20), TAP(SCE_CTRL_UP, 10), TAP(ENTER, 20),               /* both on again */
    {60, SCE_CTRL_DOWN}, TAP(SCE_CTRL_START, 60),                /* a rescan away from the top */
    TAP(SCE_CTRL_SELECT, 40),                                    /* OCR & lookup */
    TAP(SCE_CTRL_DOWN, 8), TAP(SCE_CTRL_DOWN, 8), TAP(SCE_CTRL_DOWN, 8), TAP(SCE_CTRL_DOWN, 8), /* scrolled, */
    TAP(SCE_CTRL_DOWN, 20), TAP(SCE_CTRL_UP, 8), TAP(SCE_CTRL_UP, 8), TAP(SCE_CTRL_UP, 8),      /* cards cut */
    TAP(SCE_CTRL_UP, 8), TAP(SCE_CTRL_UP, 20),                   /* and back to its top (nothing chosen) */
    TAP(SCE_CTRL_SELECT, 40), TAP(SCE_CTRL_SELECT, 60),          /* Anki, Games */
    {200, SCE_CTRL_DOWN}, TAP(SCE_CTRL_LEFT, 30),
    TAP(CANCEL, 0),
};
#endif

#if VJO_SMOKE_SYSAPP
#include <psp2/appmgr.h>
#include <psp2/gxm.h>
#include <psp2/sharedfb.h>
#include <stdlib.h>
/* The system-application start under Vita3K (vita/CMakeLists.txt VJO_SETTINGS_SMOKE_SYSAPP).
 * Vita3K's sceAppMgrGetBudgetInfo is a stub that refuses, so libvita2d starts there as an
 * ordinary application; here it is answered as a system application, and the system's side of
 * the two calls libvita2d's system start makes (the app wraps them, settings/sysapp_gfx.c,
 * linked unchanged) is stood in for, as ux0:data/vjo_settings_smoke_sysapp.txt says:
 *   "handover N"  the framebuffer's first N reads give buffer 1, the next buffer 2 (SceShell's hand-over);
 *   "nopb N"      the same, and the GPU library refuses the shared parameter buffer (SHAREDPB_OPEN);
 *   "never"       every read gives buffer 1: no hand-over.
 * The rest is Vita3K's: its GPU library, and its shared framebuffer's two buffers, one shown at
 * each sceSharedFbEnd. A crash check of that code under Vita3K, not evidence about the console. */
static int mode_read, mode_never, mode_nopb;
static unsigned mode_after, fb_reads;

static void read_mode(void)
{
    char text[32];
    int n = -1;
    if (mode_read) return;
    mode_read = 1;
    memset(text, 0, sizeof(text));
    SceUID fd = sceIoOpen("ux0:data/vjo_settings_smoke_sysapp.txt", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, text, sizeof(text) - 1);
        sceIoClose(fd);
    }
    if (n < 0) text[0] = '\0';
    mode_never = !strncmp(text, "never", 5);
    mode_nopb = !strncmp(text, "nopb", 4);
    const char *number = strchr(text, ' ');
    mode_after = number ? (unsigned)strtoul(number + 1, NULL, 10) : 0;
}

/* A system application's budget: the app's 50 MiB, as test.3's console logged it beside CLANNAD. */
int __wrap_sceAppMgrGetBudgetInfo(SceAppMgrBudgetInfo *info)
{
    int size = info->size;
    memset(info, 0, sizeof(*info));
    info->size = size;
    info->app_mode = 3;
    info->total_user_rw_mem = 52428800;
    info->free_user_rw = 38899712;
    return 0;
}

/* The GPU library: Vita3K's own clears the flags and starts its sceGxmInitialize (b75d5e6
 * SceGxmInternalForVsh.cpp), as this does; in "nopb" the shared parameter buffer is refused. */
int sceGxmVshInitialize(const SceGxmInitializeParams *params)
{
    read_mode();
    if (mode_nopb && (params->flags & SCE_GXM_INITIALIZE_FLAG_SHAREDPB_OPEN)) return SCE_GXM_ERROR_DRIVER;
    SceGxmInitializeParams own = *params;
    own.flags = 0;
    return sceGxmInitialize(&own);
}

/* The framebuffer's read: Vita3K's two buffers, as its sceSharedFbBegin gives them (its own
 * read says buffer 1 whatever happens), with the buffer index the mode gives. */
int sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info)
{
    read_mode();
    int rc = sceSharedFbBegin(fb_id, info);
    if (rc < 0) return rc;
    if (mode_never || fb_reads++ < mode_after) {
        info->index = 1;
        info->fb_base2 = info->fb_base;
    } else {
        info->index = 2;
    }
    return 0;
}
#endif

#if VJO_SMOKE_PAD > 0
/* Moves the code segment's end (vita/CMakeLists.txt VJO_SETTINGS_SMOKE_PAD). */
static const unsigned char smoke_pad[VJO_SMOKE_PAD] = {1};
#endif

static unsigned frame;
static VjoButtons smoke_btn; /* the console's enter and cancel, read at the first pad call */
static int loads, failed_loads, frees, other_frees, waits, draws, unsafe_frees, live, max_live;
/* Textures alive, and whether each was drawn since the GPU was last waited for. */
#define TRACKED 64
static const vita2d_texture *alive[TRACKED];
static unsigned char in_flight[TRACKED];

static unsigned script_frames(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) n += script[i].frames;
    return n;
}

static void write_results(const char *state)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s frames=%u script=%u loads=%d failed=%d frees=%d other_frees=%d waits=%d "
                     "draws=%d unsafe_frees=%d live=%d max_live=%d enter=%s\n", state, frame, script_frames(), loads,
                     failed_loads, frees, other_frees, waits, draws, unsafe_frees, live, max_live,
                     smoke_btn.enter == VJO_BUTTON_CIRCLE ? "circle" : "cross");
#if VJO_SMOKE_PAD > 0
    if (((const volatile unsigned char *)smoke_pad)[0] != 1) return; /* kept by the reference */
#endif
    /* Removed first: Vita3K's sceIoOpen does not truncate. */
    sceIoRemove("ux0:data/vjo_settings_smoke.txt");
    SceUID fd = sceIoOpen("ux0:data/vjo_settings_smoke.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    if (n > 0) sceIoWrite(fd, line, (unsigned)n);
    sceIoClose(fd);
}

static unsigned scripted(unsigned f)
{
    unsigned at = 0;
    for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
        if (f < at + script[i].frames) return script[i].buttons;
        at += script[i].frames;
    }
    return CANCEL; /* past the end: leave */
}

int __wrap_sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    int rc = __real_sceCtrlPeekBufferPositive(port, pad, count);
    unsigned b = scripted(frame);
    if (!frame)
        smoke_btn = vjo_buttons_read(); /* as the app read it at its start */
    pad->buttons = (b & ~(ENTER | CANCEL)) | (b & ENTER ? smoke_btn.enter : 0) | (b & CANCEL ? smoke_btn.cancel : 0);
    if (frame % 60 == 0) write_results("running");
    frame++;
    return rc;
}

/* The exit does not come back. On a console it should not either: the caller
 * is one of the process's threads, which the kernel ends without letting them
 * run on (SDK Kernel-Overview, "Resource Collection When Terminating a
 * Process"; an inference from that passage, not run on a console). Vita3K's returns
 * (b75d5e6, SceLibKernel.cpp:1293-1297): main then returned into newlib's
 * exit, whose _free_vita_heap frees the heap block holding libvita2d's GXM
 * context (Vita3K builds the context in the app's hostMem, SceGxm.cpp:1985),
 * and Vita3K's shutdown read that context to destroy it (app_init.cpp:476):
 * SIGSEGV, exit 133, after the app's exit 0. The app leaves GXM up at exit
 * since #112, as the release does; this keeps that exit, unchanged (#108's
 * vita2d_fini here avoided the crash with a teardown the release never runs). */
int __wrap_sceKernelExitProcess(int status)
{
    write_results(status ? "exit-error" : "done");
    __real_sceKernelExitProcess(status);
    for (;;) sceKernelDelayThread(100 * 1000); /* until Vita3K ends the process */
}

static vita2d_texture *loaded(vita2d_texture *t)
{
    if (!t) { failed_loads++; return t; }
    loads++;
    if (++live > max_live) max_live = live;
    for (int i = 0; i < TRACKED; i++)
        if (!alive[i]) { alive[i] = t; in_flight[i] = 0; break; }
    return t;
}
vita2d_texture *__wrap_vita2d_load_PNG_buffer(const void *buffer) { return loaded(__real_vita2d_load_PNG_buffer(buffer)); }
/* The app loads from memory; an older main.c, smoked for comparison, loaded files. */
vita2d_texture *__wrap_vita2d_load_PNG_file(const char *filename) { return loaded(__real_vita2d_load_PNG_file(filename)); }

/* libvita2d frees its own textures (the font atlas) through here too: only the icons count. */
void __wrap_vita2d_free_texture(vita2d_texture *t)
{
    int icon = 0;
    for (int i = 0; t && i < TRACKED; i++)
        if (alive[i] == t) { icon = 1; unsafe_frees += in_flight[i]; alive[i] = NULL; }
    if (icon) { frees++; live--; }
    else if (t) other_frees++;
    __real_vita2d_free_texture(t);
}

void __wrap_vita2d_wait_rendering_done(void)
{
    waits++;
    __real_vita2d_wait_rendering_done();
    memset(in_flight, 0, sizeof(in_flight));
}

void __wrap_vita2d_draw_texture_scale(const vita2d_texture *t, float x, float y, float x_scale, float y_scale)
{
    draws++;
    for (int i = 0; i < TRACKED; i++)
        if (alive[i] == t) in_flight[i] = 1;
    __real_vita2d_draw_texture_scale(t, x, y, x_scale, y_scale);
}
