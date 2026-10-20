/* Exercise the shipped controller wrappers and filter. Only SDK boundaries
 * are substituted; the trigger state machine is linked from production. */
#include "acutest.h"
#include <stdarg.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/ctrl.h>
#include "../../include/vjo_api.h"
#include "../../kernel/triggers.h"
#define VJO_TEST_INPUT_HOOKS 1
#include <taihen.h>

/* Keep unrelated framebuffer structures out of this input-only harness. */
#define VJO_KERNEL_H
static struct {
    SceUID game_pid, shell_pid, evf;
    int game_active, input_block;
    int trigger[TRIG_COUNT];
    uint32_t trigger_gen, suppress_mask, raw_buttons;
    uint32_t input_us32, input_edge_us32;
    VjoInput last_input;
} g;
void klog(const char *fmt, ...);
void display_static_fb_sample(void);
SceUID ksceKernelGetThreadId(void);
int ksceKernelSetEventFlag(SceUID, unsigned);
#include "../../kernel/input.c"

/* The debug build reads a blocked call's first sample for its trace line and logs the
 * trace (kernel/input.c input_trace_claim); the normal build reads none and logs nothing
 * (docs/release-variants.md). */
#define TRACE_READS(n) (VJO_DEBUG ? (n) : 0u)
#define TRACE_LOGS(n) (VJO_DEBUG ? (n) : 0u)

unsigned syscall_depth;
static SceUID caller;
static uint64_t now;
static SceCtrlData physical;
static unsigned read_calls, write_calls, continue_calls, frame_samples;
static unsigned fail_read_at, fail_write_at;
static unsigned log_calls;
static int longest_log;
static char last_log[192];
static int original_error;
static tai_hook_ref_t continued_ref;
static uint32_t second_native_buttons;
static uint8_t second_lt, second_rt;
static const int copy_error = -123;

/* The firmware's *2 exports remap Vita shoulders to L1/R1. L2/R2 on an
 * external controller remain separate buttons and pressure values. */
static int second_encoding(tai_hook_ref_t ref)
{
    return ref == 0x81A89660 || ref == 0x15F81E8C || ref == 0x27A0C5FB ||
           ref == 0xC4226A3E || ref == 0x860BF292 || ref == 0xA7178860;
}

void klog(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_calls++;
    snprintf(last_log, sizeof(last_log), "%s", line);
    if (length > longest_log) longest_log = length;
    TEST_CHECK(length >= 0 && length <= 190); /* production logger reserves newline */
}
SceUID ksceKernelGetProcessId(void) { return caller; }
SceUID ksceKernelGetThreadId(void) { return 3; }
uint64_t ksceKernelGetSystemTimeWide(void) { return now; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }
void display_static_fb_sample(void) { frame_samples++; }
int ksceCtrlGetSamplingMode(int *mode) { *mode = SCE_CTRL_MODE_ANALOG; return 0; }
int ksceCtrlSetSamplingMode(int mode) { return 0; }
int ksceCtrlPeekBufferPositive(int port, SceCtrlData *out, int count)
{
    TEST_CHECK(count == 1);
    *out = physical;
    return 1;
}
int ksceTouchPeek(uint32_t port, KTouchData *data, uint32_t count) { return 0; }
int ksceTouchSetSamplingState(uint32_t port, int state) { return 0; }
SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref,
                                    const char *module, uint32_t library, uint32_t nid, void *fn)
{
    TEST_CHECK(pid == KERNEL_PID && fn != NULL);
    *ref = nid;
    return 1;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref) { return 0; }
int test_tai_continue(tai_hook_ref_t ref, int port, void *out, unsigned count, ...)
{
    continued_ref = ref;
    continue_calls++;
    if (original_error) return original_error;
    TEST_ASSERT(count <= 64);
    SceCtrlData *pads = out;
    int negative = ref == 0x104ED1A7 || ref == 0x81A89660 ||
                   ref == 0x15F96FB0 || ref == 0x27A0C5FB;
    for (unsigned i = 0; i < count; i++) {
        pads[i] = physical;
        pads[i].timeStamp += i;
        if (second_encoding(ref)) {
            pads[i].buttons = (physical.buttons & ~0x300u) |
                              ((physical.buttons & 0x300u) << 2) | second_native_buttons;
            pads[i].l1 = physical.lt; pads[i].r1 = physical.rt;
            pads[i].lt = second_lt; pads[i].rt = second_rt;
        }
        if (negative) pads[i].buttons = ~pads[i].buttons;
    }
    return (int)count;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize bytes)
{
    if (++read_calls == fail_read_at) return copy_error;
    memcpy(dst, src, bytes);
    return 0;
}
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize bytes)
{
    if (++write_calls == fail_write_at) return copy_error;
    memcpy(dst, src, bytes);
    return 0;
}

static void setup(void)
{
    memset(&g, 0, sizeof(g));
    memset(&physical, 0xA5, sizeof(physical));
    memset(hold, 0, sizeof(hold));
    memset(edges, 0, sizeof(edges));
    memset(pad_activity, 0, sizeof(pad_activity));
    memset(&touch_activity, 0, sizeof(touch_activity));
    hold_busy = rear_busy = rear_peek_thid = hold_gen = analog_on = 0;
    input_trace_reset();
    rear_down = rear_taps = 0;
    rear_sampled_us = rear_down_t = rear_up_t = 0;
    g.game_pid = caller = 7;
    g.shell_pid = 3;
    g.game_active = VJO_GAME;
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_L_R;
    g.trigger[TRIG_SUBTITLE] = VJO_TRIGGER_SELECT_L;
    g.trigger_gen = 1;
    physical.timeStamp = now = 1000000;
    physical.buttons = SCE_CTRL_CROSS;
    g.raw_buttons = physical.buttons;
    physical.lx = 12; physical.ly = 34; physical.rx = 210; physical.ry = 240;
    read_calls = write_calls = continue_calls = frame_samples = 0;
    fail_read_at = fail_write_at = 0;
    log_calls = 0; longest_log = 0;
    last_log[0] = 0;
    original_error = 0;
    second_native_buttons = second_lt = second_rt = 0;
    continued_ref = 0;
    syscall_depth = 0;
    TEST_ASSERT(sizeof(SceCtrlData) == 32);
    TEST_ASSERT(offsetof(SceCtrlData, up) == 16 && offsetof(SceCtrlData, reserved) == 28);
    TEST_ASSERT(input_hooks_install() == 0);
}

static void check_neutral(const SceCtrlData *pad, unsigned index, int negative)
{
    TEST_CHECK(pad->buttons == (negative ? UINT32_MAX : 0));
    TEST_CHECK(pad->lx == 128 && pad->ly == 128 && pad->rx == 128 && pad->ry == 128);
    const uint8_t *pressure = (const uint8_t *)pad + offsetof(SceCtrlData, up);
    for (unsigned i = 0; i < 12; i++) TEST_CHECK(pressure[i] == 0);
    TEST_CHECK(pad->timeStamp == physical.timeStamp + index);
    TEST_CHECK(memcmp(pad->reserved, physical.reserved, sizeof(pad->reserved)) == 0);
}

static void test_positive2_block_and_raw_poll(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    fail_read_at = 1; /* A failed diagnostic read cannot disable blocking. */
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(continued_ref == 0xC4226A3E && continue_calls == 1);
    check_neutral(&pad, 0, 0);
    TEST_CHECK(read_calls == TRACE_READS(1) && write_calls == 1);
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    check_neutral(&pad, 0, 0);
    TEST_CHECK(read_calls == TRACE_READS(1) && write_calls == 2); /* no per-frame diagnostic reads */
    caller = KERNEL_PID;
    input_poll();
    TEST_CHECK(g.raw_buttons == SCE_CTRL_CROSS && g.last_input.buttons == SCE_CTRL_CROSS);
    TEST_CHECK(g.last_input.lx == physical.lx && g.last_input.ry == physical.ry);
    TEST_CHECK(physical.buttons == SCE_CTRL_CROSS);
}

static void test_negative_and_full_batch(void)
{
    struct { uint64_t before; SceCtrlData pads[64]; uint64_t after; } batch;
    setup();
    g.input_block = 1;
    batch.before = batch.after = UINT64_C(0xFEED123456789ABC);
    TEST_CHECK(read_neg2_patched(0, batch.pads, 64) == 64);
    TEST_CHECK(continued_ref == 0x27A0C5FB && read_calls == TRACE_READS(1) && write_calls == 64);
    for (unsigned i = 0; i < 64; i++) check_neutral(&batch.pads[i], i, 1);
    TEST_CHECK(batch.before == UINT64_C(0xFEED123456789ABC));
    TEST_CHECK(batch.after == UINT64_C(0xFEED123456789ABC));
}

static void test_foreign_and_shell_processes(void)
{
    SceCtrlData pad;
    setup();
    SceCtrlData expected = physical;
    expected.l1 = physical.lt; expected.r1 = physical.rt;
    expected.lt = expected.rt = 0;
    g.input_block = 1;
    const SceUID callers[] = { 99, 3, KERNEL_PID };
    for (unsigned i = 0; i < sizeof(callers)/sizeof(callers[0]); i++) {
        caller = callers[i];
        TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
        TEST_CHECK(memcmp(&pad, &expected, sizeof(pad)) == 0);
    }
    TEST_CHECK(read_calls == 0 && write_calls == 0);
}

static void test_blocked_write_failure(void)
{
    SceCtrlData pads[3];
    setup();
    g.input_block = 1;
    fail_write_at = 2;
    TEST_CHECK(read_pos2_patched(0, pads, 3) == copy_error);
    TEST_CHECK(read_calls == TRACE_READS(1) && write_calls == 2);
    check_neutral(&pads[0], 0, 0);
    TEST_CHECK(pads[1].buttons == SCE_CTRL_CROSS); /* failure cannot report these as valid */
    TEST_CHECK(!hold_busy);
    fail_write_at = 0;
    TEST_CHECK(read_pos2_patched(0, pads, 1) == 1);
    check_neutral(&pads[0], 0, 0);
}

static void test_unblocked_copy_failures(void)
{
    SceCtrlData pad;
    setup();
    fail_read_at = 1;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    TEST_CHECK(read_calls == 1 && write_calls == 0 && !hold_busy);
    fail_read_at = 0;
    fail_write_at = 1;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    TEST_CHECK(write_calls == 1 && !hold_busy);
    fail_write_at = 0;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_CROSS && pad.lx == physical.lx);
}

static void test_original_error_and_inactive_game(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    original_error = -456;
    memset(&pad, 0xCD, sizeof(pad));
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == -456);
    TEST_CHECK(read_calls == 0 && write_calls == 0 && pad.buttons == 0xCDCDCDCD);
    original_error = 0;
    g.game_active = VJO_GAME_NONE;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    SceCtrlData expected = physical;
    expected.l1 = physical.lt; expected.r1 = physical.rt;
    expected.lt = expected.rt = 0;
    TEST_CHECK(memcmp(&pad, &expected, sizeof(pad)) == 0);
    TEST_CHECK(read_calls == 0 && write_calls == 0);
}

static void test_diagnostics_are_bounded(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    g.raw_buttons = 0;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    g.raw_buttons = SCE_CTRL_CROSS;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(read_calls == TRACE_READS(2) && log_calls == TRACE_LOGS(2));
    for (unsigned i = 0; i < 256; i++) {
        fail_write_at = write_calls + 1;
        TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    }
    TEST_CHECK(read_calls == TRACE_READS(2) && log_calls == TRACE_LOGS(3));
    fail_write_at = 0;
    caller = 99;
    for (unsigned i = 0; i < 256; i++) TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(log_calls == TRACE_LOGS(4));
    caller = g.game_pid;
    original_error = -456;
    for (unsigned i = 0; i < 256; i++) TEST_CHECK(read_pos2_patched(0, &pad, 1) == -456);
    TEST_CHECK(log_calls == TRACE_LOGS(5) && longest_log <= 190);
    input_trace_reset();
    original_error = 0;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(read_calls == TRACE_READS(3) && log_calls == TRACE_LOGS(6));
}

static void test_scene_input_tracks_delivered_samples(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(!g.input_us32 && !g.input_edge_us32);
    g.input_block = 0;
    now += 1000;
    fail_write_at = write_calls + 1;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    TEST_CHECK(!g.input_us32 && !g.input_edge_us32);
    fail_write_at = 0;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(g.input_us32 == now && g.input_edge_us32 == now);
    uint32_t edge = g.input_edge_us32;
    now += 1000;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(g.input_us32 == now && g.input_edge_us32 == edge);
    now += HELD_INPUT_US + 1;
    uint32_t previous = g.input_us32;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(g.input_us32 == previous && g.input_edge_us32 == edge);
    physical.buttons = 0;
    physical.lx = physical.ly = physical.rx = physical.ry = 128;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(g.input_us32 == now && g.input_edge_us32 == now);
}

typedef int (*CtrlHook)(int, SceCtrlData *, int);
static const struct {
    const char *name;
    CtrlHook call;
    int negative, second;
} controller_cases[] = {
    {"peek negative", peek_neg_patched, 1, 0},
    {"peek negative2", peek_neg2_patched, 1, 1},
    {"peek positive", peek_pos_patched, 0, 0},
    {"peek positive2", peek_pos2_patched, 0, 1},
    {"read negative", read_neg_patched, 1, 0},
    {"read negative2", read_neg2_patched, 1, 1},
    {"read positive", read_pos_patched, 0, 0},
    {"read positive2", read_pos2_patched, 0, 1},
    {"peek ext", peek_pos_ext_patched, 0, 0},
    {"read ext", read_pos_ext_patched, 0, 0},
    {"peek ext2", peek_pos_ext2_patched, 0, 1},
    {"read ext2", read_pos_ext2_patched, 0, 1},
};

static void test_chord_before_claim_all_exports(void)
{
    for (unsigned h = 0; h < sizeof(controller_cases)/sizeof(controller_cases[0]); h++) {
        SceCtrlData pad;
        setup();
        physical.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS;
        physical.lt = 191; physical.rt = 223; physical.cross = 77;
        TEST_CASE(controller_cases[h].name);
        /* The game can read before the worker polls or Shell claims input. */
        TEST_ASSERT(controller_cases[h].call(0, &pad, 1) == 1);
        uint32_t positive = controller_cases[h].negative ? ~pad.buttons : pad.buttons;
        TEST_CHECK(positive == SCE_CTRL_CROSS);
        TEST_CHECK((controller_cases[h].second ? pad.l1 : pad.lt) == 0);
        TEST_CHECK((controller_cases[h].second ? pad.r1 : pad.rt) == 0);
        TEST_CHECK(pad.cross == 77 && pad.lx == physical.lx && pad.ry == physical.ry);
        TEST_CHECK(pad.timeStamp == physical.timeStamp && !memcmp(pad.reserved, physical.reserved, 4));
        input_poll();
        TEST_CHECK(g.raw_buttons == physical.buttons && g.last_input.buttons == physical.buttons);
        TEST_CHECK(g.suppress_mask == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER));
        TEST_ASSERT(controller_cases[h].call(0, &pad, 1) == 1);
        positive = controller_cases[h].negative ? ~pad.buttons : pad.buttons;
        TEST_CHECK(positive == SCE_CTRL_CROSS);
    }
}

static void test_block_all_exports_and_batches(void)
{
    struct { uint64_t before; SceCtrlData pads[64]; uint64_t after; } batch;
    for (unsigned h = 0; h < sizeof(controller_cases)/sizeof(controller_cases[0]); h++) {
        setup();
        TEST_CASE(controller_cases[h].name);
        physical.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS;
        g.input_block = 1;
        batch.before = batch.after = UINT64_C(0xABCD12345678DCBA);
        TEST_ASSERT(controller_cases[h].call(0, batch.pads, 64) == 64);
        for (unsigned i = 0; i < 64; i++) check_neutral(&batch.pads[i], i, controller_cases[h].negative);
        TEST_CHECK(batch.before == UINT64_C(0xABCD12345678DCBA) && batch.after == batch.before);
    }
}

static void test_mixed_encoding_hold_replay_and_real_l2(void)
{
    SceCtrlData pad;
    setup();
    physical.buttons = SCE_CTRL_LTRIGGER;
    physical.lt = 129;
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0 && pad.lt == 0);
    now += 16000;
    physical.buttons |= SCE_CTRL_RTRIGGER;
    physical.rt = 231;
    second_native_buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    second_lt = 53; second_rt = 67;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == second_native_buttons && pad.lt == 53 && pad.rt == 67);
    TEST_CHECK(pad.l1 == 0 && pad.r1 == 0);
    now += COMBO_DELAY_US;
    physical.buttons = SCE_CTRL_LTRIGGER;
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0 && pad.lt == 0); /* completed mixed chord stays owned */
    physical.buttons = 0; physical.lt = physical.rt = 0;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == second_native_buttons); /* no chord replay, L2/R2 survive */

    setup();
    physical.buttons = SCE_CTRL_LTRIGGER; physical.lt = 137;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0 && pad.l1 == 0);
    physical.buttons = 0; physical.lt = 0; now += 16000;
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_LTRIGGER && pad.lt == 255);
    now += 1000;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_L1 && pad.l1 == 255 && pad.lt == 0);
    now += COMBO_DELAY_US;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0 && pad.l1 == 0);

    setup();
    second_native_buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    second_lt = 53; second_rt = 67;
    physical.buttons = 0;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == second_native_buttons && pad.lt == 53 && pad.rt == 67);

    setup();
    physical.buttons = SCE_CTRL_LTRIGGER; physical.lt = 137;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    now += COMBO_DELAY_US;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_L1 && pad.l1 == 137); /* ordinary hold keeps measured pressure */
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_LTRIGGER && pad.lt == 137);
}

/* API 31: △ is the overlay's (it cycles the text source, shell/overlay.cpp). While the overlay is open
 * every hooked read gives the game a neutral pad, △ included, whatever is held; a △ still held as the
 * overlay closes stays hidden until it is released, as vjoSetInputBlock(0) (kernel/main.c) adds the raw
 * buttons of that moment to suppress_mask (mirrored here); a fresh press then reaches the game. */
static void test_triangle_stays_the_overlays(void)
{
    SceCtrlData pad;
    setup();
    physical.buttons = SCE_CTRL_TRIANGLE;
    input_poll(); /* the Shell's vjoPollInput sees it: the overlay acts on it */
    TEST_CHECK((g.last_input.buttons & SCE_CTRL_TRIANGLE) && g.raw_buttons == SCE_CTRL_TRIANGLE);
    g.input_block = 1;
    for (unsigned h = 0; h < sizeof(controller_cases) / sizeof(controller_cases[0]); h++) {
        TEST_CASE(controller_cases[h].name);
        TEST_ASSERT(controller_cases[h].call(0, &pad, 1) == 1);
        check_neutral(&pad, 0, controller_cases[h].negative);
    }
    __sync_fetch_and_or(&g.suppress_mask, g.raw_buttons); /* vjoSetInputBlock(0) with △ held */
    g.input_block = 0;
    for (int i = 0; i < 3; i++) {
        now += 16000;
        input_poll(); /* still held */
        TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
        TEST_CHECK(!(pad.buttons & SCE_CTRL_TRIANGLE));
    }
    physical.buttons = 0;
    input_poll(); /* released: the latch goes */
    TEST_CHECK(g.suppress_mask == 0);
    physical.buttons = SCE_CTRL_TRIANGLE;
    input_poll();
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons & SCE_CTRL_TRIANGLE); /* the game's own △ again */
}

static void test_mapped_suppression_until_release(void)
{
    SceCtrlData pad;
    setup();
    physical.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    input_poll(); /* opening/closing chord is owned before Shell handles the event */
    physical.buttons = SCE_CTRL_LTRIGGER; physical.lt = 129; physical.rt = 0;
    input_poll();
    TEST_CHECK(g.suppress_mask == SCE_CTRL_LTRIGGER);
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    now += COMBO_DELAY_US;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0 && pad.l1 == 0);
    physical.buttons = 0; physical.lt = 0;
    input_poll();
    TEST_CHECK(g.suppress_mask == 0);
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0); /* expired delay does not invent a replay */
}

static void test_contended_half_chord_is_reserved(void)
{
    SceCtrlData pad;
    setup(); physical.buttons = SCE_CTRL_LTRIGGER; physical.lt = 143;
    hold_busy = 1;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0 && pad.l1 == 0);
    hold_busy = 0;
    TEST_ASSERT(read_pos_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == 0);
    physical.buttons = 0; physical.lt = 0; now += 16000;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_L1 && pad.l1 == 255); /* standalone tap remains usable */
}

static void test_containment_diagnostics_are_bounded(void)
{
    SceCtrlData pad;
    setup();
    physical.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    physical.lt = 197; physical.rt = 251;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
#if VJO_DEBUG
    TEST_CHECK(strstr(last_log, "in=00000C00 out=00000000") != NULL);
    TEST_CHECK(strstr(last_log, "L1R1=0/0 write=00000000") != NULL);
#endif
    for (unsigned i = 0; i < 256; i++) TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(log_calls == TRACE_LOGS(1) && longest_log <= 190);
    input_trace_reset();
    g.input_block = 1;
    TEST_ASSERT(read_pos2_patched(0, &pad, 1) == 1);
#if VJO_DEBUG
    TEST_CHECK(strstr(last_log, "out=00000000 LR=0/0 L1R1=0/0") != NULL);
#endif
    TEST_CHECK(log_calls == TRACE_LOGS(2) && longest_log <= 190);
}

TEST_LIST = {
    { "positive2 blocks cross without changing raw shell input", test_positive2_block_and_raw_poll },
    { "negative logic and all 64 queued samples", test_negative_and_full_batch },
    { "foreign processes and shell keep their input", test_foreign_and_shell_processes },
    { "blocked copy failure never reports a successful batch", test_blocked_write_failure },
    { "unblocked copy failures release serialization", test_unblocked_copy_failures },
    { "native errors and inactive games preserve original behavior", test_original_error_and_inactive_game },
    { "controller diagnostics stay bounded across repeated failures", test_diagnostics_are_bounded },
    { "scene input tracks only successful game samples", test_scene_input_tracks_delivered_samples },
    { "initial chord is hidden before Shell claims all12 controller exports", test_chord_before_claim_all_exports },
    { "all12 controller exports block64 SDK pressure samples safely", test_block_all_exports_and_batches },
    { "mixed controller encodings share chord and replay without consuming genuine L2 R2", test_mixed_encoding_hold_replay_and_real_l2 },
    { "mapped held suppression survives until the physical shoulder releases", test_mapped_suppression_until_release },
    { "API 31: the overlay's triangle never reaches the game, held over a close included", test_triangle_stays_the_overlays },
    { "contended half chord is reserved then standalone tap replayed", test_contended_half_chord_is_reserved },
    { "native trigger and blocked output diagnostics stay bounded", test_containment_diagnostics_are_bounded },
    { NULL, NULL }
};
