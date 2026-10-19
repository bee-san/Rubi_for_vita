/* The display hook (kernel/display.c) with only the SDK and taiHEN boundary substituted. The hook reads
 * the game's frame (the region's signature, an armed OCR job's rows or a card's screenshot) before it
 * passes the call on. That order matters (docs/stutter.md, audit-stutter B1): taiHEN runs the hook added
 * first (the chain's head; a one-VPK install puts Rubi's kernel line first) and its TAI_CONTINUE runs the
 * others, and PSVshell's FPS counter and OSD and reVita's menu and popups draw into the same framebuffer
 * before their own TAI_CONTINUE [source: PSVshell 9a47b0e src/main.c, reVita f35adc5 reVita/src/main.c].
 * Read after continuing, Rubi's strips, its scene signature and a card's screenshot would hold their
 * drawings. These tests pin the order: a drawing a later hook makes in the continuation is never read. */
#include "acutest.h"
#include <stdarg.h>
#include <stdio.h>
#include <psp2kern/host_stubs.h>
#include "../../include/vjo_api.h"
#include "../../kernel/scene.h"

/* psp2kern/display.h, the parts display.c uses */
typedef struct {
    SceSize size;
    void *base;
    unsigned int pitch, pixelformat, width, height;
} SceDisplayFrameBuf;
static int primary_head;
int ksceDisplayGetPrimaryHead(void) { return primary_head; }

/* taiHEN: the hooked function's continuation, recorded with the order of every call */
#define VJO_TEST_TAIHEN_H 1
typedef uintptr_t tai_hook_ref_t;
static int continue_rc;
static unsigned order, continue_at, rows_at, sig_at, frame_at, continues, rows_calls, sig_calls, frame_calls;
static int display_continue(tai_hook_ref_t ref, int head, int index, const SceDisplayFrameBuf *p, int sync);
#define TAI_CONTINUE(type, ref, ...) ((type)display_continue((ref), __VA_ARGS__))
SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module, uint32_t library,
                                      uint32_t nid, void *fn);
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref);
#define KERNEL_PID 0x10005

/* The kernel state display.c uses (vjo_kernel.h's VjoKernelState, those fields). */
#define VJO_KERNEL_H
#define VJO_CHECK_EVERY_FRAMES 8
static struct {
    volatile SceUID game_pid;
    volatile int game_active;
    volatile uint32_t fb_pitch, fb_fmt, fb_w, fb_h;
    SceneSig hook_sig[2];
    volatile uint32_t hook_sig_seq;
} g;
void klog(const char *fmt, ...);
int region_sig_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h, SceneSig *out);
#define VJO_KERNEL_GAME_OCR_H
void game_ocr_rows_hook(SceUID pid, uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h);
void game_ocr_frame(int64_t now_us);
#include "../../kernel/display.c"

unsigned syscall_depth;
static SceUID current_pid;
static uint64_t clock_us;
static struct { SceUID pid; uintptr_t base; uint32_t pitch, fmt, w, h; } rows_args;
static unsigned char framebuffer[64];
static unsigned char rows_saw, sig_saw; /* the framebuffer's first byte when the rows were copied, sampled */

SceUID ksceKernelGetProcessId(void) { return current_pid; }
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us += 16667; }
void klog(const char *fmt, ...) { (void)fmt; }
SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module, uint32_t library,
                                      uint32_t nid, void *fn)
{
    *ref = nid;
    return 3;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref) { return 0; }
/* The rest of the chain: a later hook (PSVshell's FPS counter, say) draws into the frame, then the
 * firmware sets it. */
static int display_continue(tai_hook_ref_t ref, int head, int index, const SceDisplayFrameBuf *p, int sync)
{
    continues++;
    continue_at = ++order;
    if (p && p->base) ((unsigned char *)p->base)[0] = 0xEE; /* the later hook's drawing */
    return continue_rc;
}
void game_ocr_rows_hook(SceUID pid, uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    rows_calls++;
    rows_at = ++order;
    rows_saw = ((const unsigned char *)base)[0];
    rows_args.pid = pid;
    rows_args.base = base;
    rows_args.pitch = pitch;
    rows_args.fmt = fmt;
    rows_args.w = w;
    rows_args.h = h;
}
int region_sig_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h, SceneSig *out)
{
    sig_calls++;
    sig_at = ++order;
    sig_saw = ((const unsigned char *)base)[0];
    return 0;
}
void game_ocr_frame(int64_t now_us)
{
    frame_calls++;
    frame_at = ++order;
}

static void reset(void)
{
    memset(&g, 0, sizeof(g));
    g.game_pid = 0x40010003;
    g.game_active = VJO_GAME;
    current_pid = g.game_pid;
    primary_head = 0;
    continue_rc = 0;
    order = continue_at = rows_at = sig_at = frame_at = continues = rows_calls = sig_calls = frame_calls = 0;
    memset(&rows_args, 0, sizeof(rows_args));
    memset(framebuffer, 0x11, sizeof(framebuffer)); /* the game's pixels */
    rows_saw = sig_saw = 0;
    frame_n = 0;
    frame_busy = 0;
}

static int submit(const SceDisplayFrameBuf *fb)
{
    return display_hook(0, 0, fb, 1);
}

static const SceDisplayFrameBuf game_fb = {sizeof(SceDisplayFrameBuf), framebuffer, 960, 0, 960, 544};

static void rows_copied_before_the_chain_continues(void)
{
    reset();
    TEST_CHECK(submit(&game_fb) == 0);
    TEST_CHECK(continues == 1 && rows_calls == 1);
    TEST_CHECK(rows_at < continue_at); /* read, then passed on */
    TEST_MSG("rows at %u, continue at %u", rows_at, continue_at);
    TEST_CHECK(rows_saw == 0x11); /* the game's pixels, not the later hook's drawing */
    TEST_CHECK(rows_args.pid == g.game_pid && rows_args.base == (uintptr_t)framebuffer && rows_args.pitch == 960 &&
               rows_args.fmt == 0 && rows_args.w == 960 && rows_args.h == 544);
    TEST_CHECK(g.fb_pitch == 960 && g.fb_w == 960 && g.fb_h == 544);
#if VJO_DEBUG
    TEST_CHECK(frame_calls == 1 && frame_at < rows_at); /* the frame's time first */
#else
    TEST_CHECK(frame_calls == 0);
#endif
}

static void signature_sampled_before_the_chain_continues(void)
{
    reset();
    for (unsigned i = 0; i < VJO_CHECK_EVERY_FRAMES; i++) {
        memset(framebuffer, 0x11, sizeof(framebuffer)); /* the game draws its next frame */
        TEST_CHECK(submit(&game_fb) == 0);
    }
    TEST_CHECK(sig_calls == 1 && continues == VJO_CHECK_EVERY_FRAMES && rows_calls == VJO_CHECK_EVERY_FRAMES);
    TEST_CHECK(sig_at < rows_at && rows_at < continue_at); /* the 8th frame: sampled, copied, then passed on */
    TEST_CHECK(sig_saw == 0x11 && rows_saw == 0x11);
    TEST_CHECK(g.hook_sig_seq == 1);
}

static void the_calls_result_is_the_games(void)
{
    reset();
    continue_rc = (int)0x80290002u; /* the firmware refused the framebuffer */
    TEST_CHECK(submit(&game_fb) == continue_rc);
    continue_rc = 1;
    TEST_CHECK(submit(&game_fb) == 1);
    TEST_CHECK(continues == 2 && rows_calls == 2);
}

static void other_calls_pass_through(void)
{
    SceDisplayFrameBuf none = game_fb;
    none.base = NULL;
    reset();
    TEST_CHECK(display_hook(0, 1, &game_fb, 1) == 0); /* slot 1: the LiveArea overlay, not the game's */
    TEST_CHECK(display_hook(0, 0, &none, 1) == 0);
    TEST_CHECK(display_hook(0, 0, NULL, 1) == 0);
    primary_head = 1;
    TEST_CHECK(submit(&game_fb) == 0); /* another head */
    primary_head = 0;
    current_pid = 0x40010005; /* another process (SceShell) */
    TEST_CHECK(submit(&game_fb) == 0);
    current_pid = g.game_pid;
    g.game_pid = 0;
    TEST_CHECK(submit(&game_fb) == 0); /* no game */
    TEST_CHECK(continues == 6 && rows_calls == 0 && sig_calls == 0 && frame_calls == 0);
}

TEST_LIST = {
    {"a job's rows are copied before the chain continues: a later hook's drawing is not read",
     rows_copied_before_the_chain_continues},
    {"the region's signature is sampled before the chain continues too", signature_sampled_before_the_chain_continues},
    {"the call's result is the game's", the_calls_result_is_the_games},
    {"other frames and other processes only continue", other_calls_pass_through},
    {NULL, NULL}};
