/* Real game worker and real vocr, with Vita scheduling/USER_RW/file boundaries.
 * No emulator claims: the caller PID and SDK responses are failure-injected. */
#include "acutest.h"
#include <sys/mman.h>
#include <psp2/host_stubs.h>
static unsigned test_cascade_conf; /* 0: the default; 1001: every line is doubtful */
#define VJO_GAME_CASCADE_CONF test_cascade_conf
/* #385's audit F5: the tall-line blocks above a page keep the job's reserve here (the worker's bound is a column's,
 * 57,344 B: the 200-px frames' rectangles are smaller). So these tests never take a block under the worker's bound
 * without the check, as the worker takes a column's (#411's audit nit 4: that exemption is untested here). */
#define VJO_GAME_LINE_UNCHECKED 4096u
#ifdef VJO_VOCR_TIERS_TEST
/* Another package's tier map (host/tests/test_shared_tiers.h): this file's own
 * tests for it only, its TEST_LIST at the end; the others are left unused. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../game_ocr/main.c"
#include "test_tier_pins.h"
#include "vocr_models.h" /* vjo_vocr_tier_need: the Rubi app's table (tools/model_tiers.json "need_bytes") */
#include "cache_replay.h" /* #418: every cache operation replayed through the old layout (setup installs it) */

static unsigned char workspace[2*1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_MAX_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char capture_mem[1024*1024] __attribute__((aligned(64)));
/* API 55, the pause: the worker's frame block, and the kernel's frame copies into it (VJO_ROWS_FRAME) */
static unsigned char frame_memory[128 * 1024] __attribute__((aligned(64)));
static int frame_live;
static unsigned frame_block, frame_allocs, frame_copies;
static unsigned char lm_memory[VJO_VOCR_LM_MAX_BYTES + 4096] __attribute__((aligned(64)));
static int lm_live; static unsigned lm_block, lm_allocations, lm_frees, lm_opens, last_which;
#ifndef VJO_VOCR_GATE_TEST_DIR
#define VJO_VOCR_GATE_TEST_DIR ""
#endif
static unsigned gate_opens; /* API 26: opens of the gate's weights */
static int open_gate_missing; /* API 26: the gate's file is not on the card */
/* #418: the gate's block (game_ocr/main.c gate_alloc, "VjoGameVocrGate"), USER_RW only, at most one */
static unsigned char gate_memory[4096] __attribute__((aligned(64)));
static int gate_live, refuse_gate; static unsigned gate_block, gate_allocs, gate_frees;
static unsigned char cascade_memory[VJO_VOCR_MAX_BYTES + 4096] __attribute__((aligned(64)));
static int cascade_live; static unsigned cascade_block, cascade_allocations, cascade_frees, cascade_opens;
/* the block of a line taller than a strip (game_ocr/main.c line_buffer, "VjoGameVocrLine"): a job of columns takes
 * one for most of its columns (API 47); one at most, any memory type */
static unsigned char line_memory[VJO_VOCR_LINE_MAX_BYTES + 4096] __attribute__((aligned(64)));
static int line_live; static unsigned line_block, line_allocs, line_frees;
static unsigned line_block_max; /* the largest tall-line block taken (#385's audit F5: the columns' rectangle) */
/* the tall-line block refused, every memory type (a failed allocation after the job's check passed), and the
 * attempts refused; the job's workspace blocks taken while a tall-line block was live (an earlier try's or job's) */
static int refuse_line; static unsigned line_refused, line_asks, workspace_beside_line;
static int vertical_absent; /* API 47: the vertical model's file is not on the card, whatever VJO_VOCR_MODEL_DIR holds */
static unsigned tier_opens[VJO_VOCR_TIERS + 1]; /* API 21: model opens by tier (VJO_GAME_OCR_OPEN_TIER) */
static unsigned vertical_opens; /* API 47: the vertical model's (VJO_GAME_OCR_OPEN_VERTICAL) */
static int grow_on_vertical_open; /* the game frees this much as the vertical model is opened (its memory moves
                                   * between auto's tries) */
static int drop_on_workspace; /* bytes the game takes when the job's workspace block is allocated */
/* bytes the game takes once, when the drop_at_allocation-th workspace block of the test is allocated (0: never): the
 * memory a try's check saw is gone by the time its weights are asked for (#324's retry) */
static int drop_once; static unsigned drop_at_allocation;
static unsigned char pixels[200*272*4]; /* 200 x frame_h */
static unsigned frame_h = 60; /* rows of the fake frame (over 64: two strips) */
/* The capture block and the memory-type ladder: which types refuse. */
static int capture_live, refuse_user, refuse_cdram, refuse_phycont, rows_error, hook_write_error;
static int lm_bad_size; /* the LM files report one byte more than they hold (a wrong file) */
static int cascade_bad_size; /* the same for the cascade's model */
static int busy_unarmed, busy_armed, allow_capture_held; /* rows calls that answer BUSY, before / after arming */
static unsigned capture_block, capture_allocs, capture_frees, rows_calls, kstrip_calls, last_type;
static unsigned allocations, frees, reads_count, bytes_read, thread_creates, weight_allocations, weight_frees;
static int memory_live, alloc_failure, base_failure, free_failure, close_failure, weights_live;
static unsigned live_bytes, job_block, weights_block;
static int join_failure, start_failure, delete_failure, budget_failure, cancel_reads;
static int available, cancel_job, short_reads, open_failure, bad_read;
/* 1: report free USER memory in whole MiB, rounded down, as a console did (あおかな, 2026-10-26) */
static int free_mib;
static FILE *model;
static char model_path[512];
static unsigned user_open_calls;
static int virtual_file, open_busy, read_busy, close_busy, read_eof;
static char own_title[12];
static uint64_t clock_value;
static unsigned frame_us; /* each rows call waits this long for its frame (0: none) */
static unsigned change_at_rows; /* at this rows call the line moves down 6 rows (the player advanced); 0: never */
static unsigned clear_at_rows;  /* at this rows call the box empties (the player advanced to an empty box); 0: never */
/* until this rows call the screen keeps changing (text typing out, a video): the frame's first pixel takes a new
 * dim grey at each call, never one of its recent states; then it is black again and holds still. 0: never */
static unsigned flicker_rows;

SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
/* Free USER memory as the game sees it: what remains after the worker's blocks. */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    TEST_CHECK(out->size == sizeof(*out));
    out->size_user = available < 0 ? available : available - (int)live_bytes;
    if (free_mib && out->size_user > 0) out->size_user -= out->size_user % (1024 * 1024);
    return budget_failure ? -1 : 0;
}
static unsigned workspace_tries, workspace_types[8];
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(!(size&4095));
    last_type = (unsigned)type;
    if (!strcmp(name, "VjoGameVocrCapture") || !strcmp(name, "VjoGameVocrProbe")) {
        /* SDK units: CDRAM 256 KiB, PHYCONT 1 MiB */
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && refuse_user) return (int)0x80024302u;
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) { TEST_CHECK(!(size % (256*1024))); if (refuse_cdram) return (int)0x80024302u; }
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) { TEST_CHECK(!(size % (1024*1024))); if (refuse_phycont) return (int)0x80024302u; }
        TEST_CHECK(!capture_live && size <= sizeof(capture_mem));
        capture_allocs++; capture_live = 1; capture_block = size; live_bytes += size; return 11;
    }
    if (!strcmp(name, "VjoGameVocrFrame")) { /* API 55: the pause's frame block, USER first on its ladder */
        if (type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || alloc_failure) return (int)0x80024302u;
        TEST_CHECK(!frame_live && size <= sizeof(frame_memory));
        frame_allocs++; frame_live = 1; frame_block = size; live_bytes += size; return 15;
    }
    if (!strcmp(name, "VjoGameVocrLine")) { /* a line taller than a strip, kept whole: at most one block */
        TEST_CHECK(!line_live);
        line_allocs++;
        line_asks += type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW; /* each ask starts at USER (the ladder's first rung) */
        if (alloc_failure || refuse_line) { line_refused++; return (int)0x80024302u; }
        TEST_CHECK(size <= sizeof(line_memory)); /* (a refused rung may ask more: phycont's unit is 1 MiB) */
        if (size > line_block_max) line_block_max = size;
        line_live = 1; line_block = size; live_bytes += size; return 14;
    }
    if (!strcmp(name, "VjoGameVocrCascade")) { /* the cascade's model, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!cascade_live && size <= sizeof(cascade_memory));
        cascade_allocations++;
        if (alloc_failure) return -1;
        cascade_live = 1; cascade_block = size; live_bytes += size; return 13;
    }
    if (!strcmp(name, "VjoGameVocrLM")) { /* the resident LM, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!lm_live && size <= sizeof(lm_memory));
        lm_allocations++;
        if (alloc_failure) return -1;
        lm_live = 1; lm_block = size; live_bytes += size; return 12;
    }
    if (!strcmp(name, "VjoGameVocrGate")) { /* #418: the gate's weights, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!gate_live && size <= sizeof(gate_memory));
        gate_allocs++;
        if (alloc_failure || refuse_gate) return (int)0x80024302u;
        gate_live = 1; gate_block = size; live_bytes += size; return 16;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) { /* resident verified weights, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory));
        weight_allocations++;
        if (alloc_failure) return -1;
        weights_live = 1; weights_block = size; live_bytes += size; return 10;
    }
    /* the job's workspace: the same ladder as the capture, USER first */
    TEST_CHECK(!strcmp(name, "VjoGameVocr") && !memory_live);
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) TEST_CHECK(!(size % (256*1024)));
    else if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) TEST_CHECK(!(size % (1024*1024)));
    else TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && size <= sizeof(workspace));
    if (workspace_tries < 8) workspace_types[workspace_tries] = (unsigned)type;
    workspace_tries++;
    workspace_beside_line += line_live; /* a try's or job's tall-line block outlived it */
    allocations++;
    if (alloc_failure) return (int)0x80024302u;
    TEST_CHECK(size <= sizeof(workspace));
    available -= drop_on_workspace; /* the game allocates right after the job's check */
    if (drop_at_allocation && allocations == drop_at_allocation) available -= drop_once;
    memory_live = 1; job_block = size; live_bytes += size; return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem; return 0; }
    if (uid == 10) { TEST_CHECK(weights_live); *base = weights_memory; return 0; }
    if (uid == 12) { TEST_CHECK(lm_live); *base = lm_memory; return 0; }
    if (uid == 13) { TEST_CHECK(cascade_live); *base = cascade_memory; return 0; }
    if (uid == 14) { TEST_CHECK(line_live); *base = line_memory; return 0; }
    if (uid == 15) { TEST_CHECK(frame_live); *base = frame_memory; return 0; }
    if (uid == 16) { TEST_CHECK(gate_live); *base = gate_memory; return 0; }
    TEST_CHECK(uid == 9 && memory_live); *base = base_failure ? NULL : workspace; return base_failure ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 15) { TEST_CHECK(frame_live); if (free_failure) return -1; frame_live = 0; live_bytes -= frame_block; return 0; }
    if (uid == 11) { TEST_CHECK(capture_live); if (free_failure) return -1; capture_live = 0; capture_frees++; live_bytes -= capture_block; return 0; }
    if (uid == 10) {
        TEST_CHECK(weights_live); if (free_failure) return -1;
        weights_live = 0; weight_frees++; live_bytes -= weights_block; return 0;
    }
    if (uid == 12) {
        TEST_CHECK(lm_live); if (free_failure) return -1;
        lm_live = 0; lm_frees++; live_bytes -= lm_block; return 0;
    }
    if (uid == 13) {
        TEST_CHECK(cascade_live); if (free_failure) return -1;
        cascade_live = 0; cascade_frees++; live_bytes -= cascade_block; return 0;
    }
    if (uid == 14) {
        TEST_CHECK(line_live); if (free_failure) return -1;
        line_live = 0; line_frees++; live_bytes -= line_block; return 0;
    }
    if (uid == 16) {
        TEST_CHECK(gate_live); if (free_failure) return -1;
        gate_live = 0; gate_frees++; live_bytes -= gate_block; return 0;
    }
    TEST_CHECK(uid == 9 && memory_live); if (free_failure) return -1;
    memory_live = 0; frees++; live_bytes -= job_block; return 0;
}
/* API 22: a helper thread (game_ocr/main.c pool_run) runs its line at once, in StartThread. */
static unsigned helper_threads, helper_live, helper_starts;
static int helper_create_failure, helper_join_failure;
/* helper_deferred: a helper's line runs at its join, not at its start, so the worker meets the
 * next lines while it is still out (as a slower core would) */
static int helper_deferred;
static struct { SceUID uid; SceSize size; unsigned char args[16]; int pending; } deferred[4];
static SceUID wait_order[8]; /* the threads WaitThreadEnd was asked for, in order */
static unsigned n_waits;
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{
    if (entry == helper_entry) {
        TEST_CHECK(stack == VJO_STACK_GAME_HELPER && p == 0x10000100 + 31 && cpu == 0x70000 && !strcmp(n, "VjoGameVocrHelper"));
        if (helper_create_failure) return (int)0x80020190u;
        helper_threads++; helper_live++;
        return 40 + (int)helper_threads;
    }
    TEST_CHECK(stack == 32*1024 && entry == worker_main && p == 0x10000100 + 31 && cpu == 0x70000); thread_creates++; return 20;
}
int sceKernelStartThread(SceUID uid, SceSize size, void *args)
{
    if (uid > 40 && helper_deferred) { /* the argument block is copied, as StartThread does */
        unsigned k = (unsigned)(uid - 41) % 4u;
        TEST_CHECK(size <= sizeof(deferred[k].args) && !deferred[k].pending);
        deferred[k].uid = uid;
        deferred[k].size = size;
        memcpy(deferred[k].args, args, size);
        deferred[k].pending = 1;
        helper_starts++;
        return 0;
    }
    if (uid > 40) { helper_starts++; return helper_entry(size, args); }
    return start_failure ? -1 : 0;
}
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout)
{
    if (n_waits < 8) wait_order[n_waits++] = uid;
    if (uid > 40 && helper_join_failure) return (int)0x80028005u; /* the wait timed out */
    if (uid > 40) {
        unsigned k = (unsigned)(uid - 41) % 4u;
        if (deferred[k].pending && deferred[k].uid == uid) {
            deferred[k].pending = 0;
            helper_entry(deferred[k].size, deferred[k].args);
        }
        return 0;
    }
    return join_failure ? -1 : 0;
}
int sceKernelDeleteThread(SceUID uid)
{
    if (uid > 40) { TEST_CHECK(helper_live > 0); helper_live--; return 0; }
    return delete_failure ? -1 : 0;
}
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info)
{ TEST_CHECK(info->size == sizeof(*info)); info->runClocks = clock_value; return 0; }
SceUID sceKernelGetThreadId(void) { return 20; }
/* The ARM clock (ocr_boost_clock): what the game runs at, and the calls that set it. */
static int arm_mhz = 333, arm_sets, arm_set_last;
static unsigned arm_gets, arm_game_at; /* the game sets arm_game_mhz just before the arm_game_at-th read */
static int arm_game_mhz;
int scePowerGetArmClockFrequency(void)
{
    if (arm_game_at && ++arm_gets == arm_game_at) arm_mhz = arm_game_mhz;
    return arm_mhz;
}
int scePowerSetArmClockFrequency(int mhz) { arm_sets++; arm_set_last = mhz; arm_mhz = mhz; return 0; }
static int quit_on_delay; /* the worker loop's next sleep ends it (one pass of worker_main) */
int sceKernelDelayThread(unsigned us) { clock_value += us; if (quit_on_delay) quitting = 1; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ TEST_CHECK(pid == 7); snprintf(out, size, "%s", own_title); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
static int text_source_api = VJO_API_VERSION; /* the kernel's API (module_start_reads_the_text_source) */
int vjoGetVersion(void) { return text_source_api; }
static int safe_boot_mode, boot_info_calls;
int vjoBootInfo(VjoBootInfo *out) { boot_info_calls++; TEST_CHECK(out->size == sizeof(*out)); out->mode = safe_boot_mode; return 0; }
int vjoOcrRegister(void) { return VJO_ERR_NO_GAME; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_job || (cancel_reads && reads_count >= (unsigned)cancel_reads); }
int vjoOcrComplete(const VjoGameOcrResult *out)
{ TEST_CHECK(!memory_live && !model && (!capture_live || allow_capture_held)); return 0; }
/* The kernel's rows call: grey rows of the 200x60 region into the worker's block. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    if (req->flags & VJO_ROWS_FRAME) { /* API 55: the whole region from one frame, into the frame block */
        TEST_CHECK(req->size == sizeof(*req) && req->seq == 12 && !req->y && req->n == frame_h && info);
        TEST_CHECK(frame_live && dst == (void *)frame_memory && req->dst_stride == 200 && capture_live);
        rows_calls++; frame_copies++;
        clock_value += frame_us;
        info->fb_width = 200; info->fb_height = frame_h; info->fb_pitch = 200; info->fb_format = 0; info->wait_us = 16000;
        info->armed = 1; info->copy_us = 1234; /* the game's wait in its display call */
        for (unsigned r = 0; r < req->n; r++)
            for (unsigned x = 0; x < 200; x++) {
                const unsigned char *p = pixels + (r * 200 + x) * 4;
                ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
            }
        return (int)req->n;
    }
    TEST_CHECK(req->size == sizeof(*req) && req->seq == 12 && req->y < frame_h && req->n && req->n <= frame_h - req->y);
    TEST_CHECK(req->dst_stride >= 200 && info && info->size == sizeof(*info));
    TEST_CHECK(capture_live && (unsigned char *)dst >= capture_mem && (unsigned char *)dst < capture_mem + capture_block);
    rows_calls++;
    clock_value += frame_us;
    if (change_at_rows && rows_calls == change_at_rows) { /* the next screen: the line 6 rows lower */
        memset(pixels, 0, sizeof(pixels));
        for (unsigned y = 21; y < 45; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
    }
    if (clear_at_rows && rows_calls == clear_at_rows) memset(pixels, 0, sizeof(pixels));
    if (flicker_rows && rows_calls <= flicker_rows + 1u)
        pixels[0] = pixels[1] = pixels[2] = rows_calls <= flicker_rows ? (unsigned char)(1u + (rows_calls * 5u) % 31u) : 0;
    if (req->mode == VJO_GAME_OCR_SRC_KSTRIP) kstrip_calls++;
    info->fb_width = 200; info->fb_height = frame_h; info->fb_pitch = 200; info->fb_format = 0; info->wait_us = 16000;
    info->armed = 1;
    if (busy_unarmed > 0) { busy_unarmed--; info->armed = 0; return VJO_ERR_BUSY; } /* a previous copy not tidied */
    if (busy_armed) { /* this copy outlived the call; until the kernel worker tidies it, every
                         later call is refused before arming */
        busy_armed = 0; busy_unarmed = 1000;
        return VJO_ERR_BUSY;
    }
    if (rows_error) return rows_error;
    if (hook_write_error && req->mode == VJO_GAME_OCR_SRC_HOOK) return VJO_ERR_COPY;
    for (unsigned r = 0; r < req->n; r++)
        for (unsigned x = 0; x < 200; x++) {
            const unsigned char *p = pixels + ((req->y + r) * 200 + x) * 4;
            ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
        }
    return (int)req->n;
}
/* The probe's user-mode read: the frame sceDisplayGetFrameBuf names (frame_at,
 * none by default) and the block sceKernelGetMemBlockInfoByAddr reports for it. */
static unsigned char *frame_at;
static uintptr_t block_base;
static unsigned block_size, block_access, frame_info_calls, frame_w = 200;
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync)
{
    if (!frame_at) return -1;
    TEST_CHECK(fb->size == sizeof(SceDisplayFrameBuf) && sync == SCE_DISPLAY_SETBUF_NEXTFRAME);
    fb->base = frame_at; fb->pitch = 200; fb->pixelformat = VJO_FB_A8B8G8R8; fb->width = frame_w; fb->height = 60;
    return 0;
}
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info)
{
    if (!frame_at) return -1;
    TEST_CHECK(base == frame_at && info->size == sizeof(*info));
    frame_info_calls++;
    info->mappedBase = (void *)block_base; info->mappedSize = block_size; info->access = block_access;
    return 0;
}
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_open_calls++; return (int)0x80010013u; /* Physical game's ux0 is unavailable. */ }
static uint64_t model_mtime = 0x1234567;
static int model_stat_error;
static int missing_open_rc = -123; /* an absent file's open (API 47's auto: 0x80010002, ENOENT, latches columns_bad) */
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    if (open_busy) return VJO_ERR_BUSY;
    if (open_failure) return -123;
    TEST_ASSERT(!model && seq == 12);
    last_which = which;
    if (which == VJO_GAME_OCR_OPEN_GATE) { /* API 26: the text-presence gate's weights (cmake/VocrGate.cmake) */
        char g_path[600];
        gate_opens++;
        if (!VJO_VOCR_GATE_TEST_DIR[0] || open_gate_missing) return -123;
        snprintf(g_path, sizeof(g_path), "%s/%s", VJO_VOCR_GATE_TEST_DIR, VJO_VOCR_GATE_NAME);
        model = fopen(g_path, "rb");
    } else if (which == VJO_GAME_OCR_OPEN_VERTICAL) { /* API 47: the vertical model, by its own open */
        char v_path[600];
        vertical_opens++;
        available += grow_on_vertical_open;
        snprintf(v_path, sizeof(v_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_VERTICAL_NAME);
        model = vertical_absent ? NULL : fopen(v_path, "rb");
    } else if (which >= VJO_GAME_OCR_OPEN_TIER) { /* API 21: a model size's pinned model, by its tier */
        char t_path[600];
        unsigned t = which - VJO_GAME_OCR_OPEN_TIER + 1u;
        TEST_ASSERT(t >= 1u && t <= VJO_VOCR_TIERS);
        tier_opens[t]++;
        snprintf(t_path, sizeof(t_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", tier_names[t - 1u]);
        model = fopen(t_path, "rb");
    } else if (which == VJO_GAME_OCR_OPEN_CASCADE) { /* the cascade's pinned model, beside the others */
        char c_path[600];
        cascade_opens++;
        snprintf(c_path, sizeof(c_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_CASCADE_NAME);
        model = fopen(c_path, "rb");
    } else if (which >= VJO_GAME_OCR_OPEN_LM) { /* the LM of that tier, beside the model (the kernel's pinned names) */
        char lm_path[600];
        const VjoVocrLm *t = vjo_vocr_lm_tier(which - VJO_GAME_OCR_OPEN_LM + 1u);
        TEST_ASSERT(t != NULL);
        lm_opens++;
        snprintf(lm_path, sizeof(lm_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", t->name);
        model = fopen(lm_path, "rb");
    } else
    model = virtual_file ? tmpfile() : fopen(model_path, "rb");
    if (!model) return missing_open_rc;
    if (virtual_file) {
        const unsigned char bytes[16] = {1,2,3,4};
        TEST_ASSERT(fwrite(bytes, 1, sizeof(bytes), model) == sizeof(bytes));
    }
    TEST_ASSERT(!fseek(model, 0, SEEK_END));
    int size = (int)ftell(model);
    if (lm_bad_size && which >= VJO_GAME_OCR_OPEN_LM && which < VJO_GAME_OCR_OPEN_CASCADE) size++;
    if (cascade_bad_size && which >= VJO_GAME_OCR_OPEN_CASCADE && which != VJO_GAME_OCR_OPEN_GATE) size++;
    if (info) *info = (VjoGameOcrModelInfo){sizeof(*info), (uint32_t)size, model_stat_error, 0,
                                            model_stat_error ? 0 : model_mtime};
    return size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t n, void *out)
{
    TEST_CHECK(seq == 12 && model && n <= VJO_GAME_OCR_MODEL_CHUNK);
    if (read_busy) return VJO_ERR_BUSY;
    TEST_ASSERT(!fseek(model, (long)offset, SEEK_SET));
    reads_count++;
    if (bad_read) return -456;
    if (read_eof) return 0;
    if (short_reads && n > 37) n = 37;
    int got = (int)fread(out, 1, n, model); bytes_read += (unsigned)got; return got;
}
/* API 23: a shot's file; this test's jobs are OCR jobs (test_game_shot.c has the shot's) */
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src)
{ (void)seq; (void)off; (void)bytes; (void)src; TEST_CHECK(0); return -1; }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { (void)seq; (void)bytes; TEST_CHECK(0); return -1; }
/* #382's audit N3: at the close_at-th close of a model file (1: the first), the job is cancelled (close_cancels) or the
 * screen shows columns (close_columns: tall_columns_frame's pixels, the frame's size unchanged); 0: never */
static unsigned model_closes, close_at; static int close_cancels, close_columns;
static void tall_columns_pixels(void);
int vjoOcrModelClose(uint32_t seq)
{
    TEST_CHECK(seq == 12);
    if (close_busy) return VJO_ERR_BUSY;
    if (!model) return 0;
    if (close_failure) return -789;
    int rc = fclose(model);
    model = NULL;
    if (close_at && ++model_closes == close_at) {
        if (close_cancels) cancel_job = 1;
        if (close_columns) tall_columns_pixels();
    }
    return rc;
}

static VjoGameOcrRequest setup(void)
{
    if (model) { fclose(model); model = NULL; }
    worker = workspace_uid = weights_blk.uid = capture_uid = line_uid = -1;
    capture_live = refuse_user = refuse_cdram = refuse_phycont = rows_error = hook_write_error = 0;
    lm_bad_size = cascade_bad_size = 0;
    busy_unarmed = busy_armed = allow_capture_held = 0; hook_owns_capture = 0;
    capture_block = capture_allocs = capture_frees = rows_calls = kstrip_calls = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    lm_res.lm = NULL; lm_res.data = NULL; lm_res.capacity = 0; lm_res.size = lm_res.stamp = 0; lm_blk.uid = -1;
    lm_live = 0; lm_allocations = lm_frees = lm_opens = last_which = 0;
    gate_opens = 0; gate_live = refuse_gate = 0; gate_block = gate_allocs = gate_frees = 0;
    gate_res.verified = NULL; gate_res.data = NULL; gate_res.capacity = 0; gate_res.size = gate_res.stamp = 0;
    gate_blk.uid = -1; gate_blk.held = 0;
    cascade_res.model = NULL; cascade_res.data = NULL; cascade_res.capacity = 0; cascade_res.size = cascade_res.stamp = 0;
    cascade_blk.uid = -1; cascade_live = 0; cascade_allocations = cascade_frees = cascade_opens = 0;
    test_cascade_conf = 0;
    line_live = 0; line_allocs = line_frees = 0; line_block_max = 0;
    refuse_line = vertical_absent = 0; line_refused = line_asks = workspace_beside_line = 0;
    memset(tier_opens, 0, sizeof(tier_opens)); vertical_opens = 0; tiered = 0; job_reserve = GAME_RESERVE; drop_on_workspace = 0;
    drop_once = 0; drop_at_allocation = 0;
    model_closes = close_at = 0; close_cancels = close_columns = 0;
    columns_bad = 0; missing_open_rc = -123; grow_on_vertical_open = 0; /* API 47, auto */
    frame_live = 0; frame_block = frame_allocs = frame_copies = 0; /* API 55, the pause */
    frame_uid = -1; frame_base = NULL; frame_bytes_held = 0; pause_capped = 0; memset(&pz, 0, sizeof(pz));
    psp_title = 0;
    weight_allocations = weight_frees = 0; weights_live = 0; live_bytes = 0;
    model_mtime = 0x1234567; model_stat_error = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    allocations = frees = reads_count = bytes_read = thread_creates = 0;
    user_open_calls = 0;
    virtual_file = open_busy = read_busy = close_busy = read_eof = 0; clock_value = 0; frame_us = 0;
    memory_live = alloc_failure = base_failure = free_failure = close_failure = 0;
    join_failure = start_failure = delete_failure = budget_failure = cancel_reads = 0;
    cancel_job = short_reads = open_failure = bad_read = 0;
    available = 16*1024*1024; free_mib = 0;
    memset(pixels, 0, sizeof(pixels)); strcpy(own_title, "PCSG00415");
    frame_at = NULL; frame_info_calls = 0; frame_w = 200;
    change_at_rows = clear_at_rows = flicker_rows = 0; blocks_held = 0; frame_h = 60;
    /* API 22: no helper, no stuck job, an empty line cache, the clock as the game set it */
    hook_owns_workspace = 0; vjo_vocr_unstick();
    for (unsigned k = 0; k <= VJO_VOCR_HELPERS_MAX; k++) { helper_uid[k] = -1; helper_job[k].fn = NULL; }
    memset(&line_cache, 0, sizeof(line_cache));
    cache_replay_install();
    helper_threads = helper_live = helper_starts = 0; helper_create_failure = helper_join_failure = 0; n_waits = 0;
    helper_deferred = 0; memset(deferred, 0, sizeof(deferred));
    arm_mhz = 333; arm_sets = arm_set_last = 0; arm_gets = arm_game_at = 0; arm_game_mhz = 0;
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=200, .fb_height=60, .width=200, .height=60};
    snprintf(r.model, sizeof(r.model), "FL10_w8.vocr");
    /* Blank images open no weights but the warm-up's (a job that found no line reads the
     * weights it would run, best effort), and requests still need a valid directory.
     * CI intentionally has no private release assets in its source checkout. */
    snprintf(r.model_dir, sizeof(r.model_dir), "%s",
             VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : "unused-model-dir");
    snprintf(model_path, sizeof(model_path), "%s/%s", r.model_dir, r.model);
    return r;
}
/* setup(), keeping the gate's resident weights and their block (as the worker does between jobs) */
static VjoGameOcrRequest setup_keeping_the_gate(void)
{
    VjoVocrGateResident keep = gate_res;
    BlockState keep_blk = gate_blk;
    const int live = gate_live;
    const unsigned block = gate_block;
    VjoGameOcrRequest r = setup();
    gate_res = keep;
    gate_blk = keep_blk;
    gate_live = live;
    gate_block = block;
    live_bytes += live ? block : 0u;
    return r;
}
static void line(void)
{
    for (unsigned y=15; y<39; y++) for (unsigned x=10; x<190; x+=6)
        memset(pixels+(y*200+x)*4, 235, 6);
}
static void repeated_jobs_and_reserve(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    for (unsigned i=0; i<100; i++) {
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        TEST_CHECK(!memory_live && !out.cleanup_status && !out.text[0] && !model);
    }
    TEST_CHECK(allocations == 100 && frees == 100 && capture_allocs == 100 && capture_frees == 100 && !capture_live);
    /* the ledger saw every block in and out, and its peak fits the budget */
    /* ... and the weights the first job warmed (ocr-first: it read no line) stay, read once */
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_WORKSPACE) == 0 &&
               vjo_ledger_current(&g_game_ledger, GL_WEIGHTS) == held_bytes(&resident));
    TEST_CHECK(!VJO_VOCR_MODEL_DIR[0] || (weights_live && weight_allocations == 1));
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) > 0 && vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) <= VJO_BUDGET_GAME_WORKSPACE);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_CAPTURE) == 0 && vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) == 16384); /* 200x60: 12,000 B strip + 8 B and 3 bits per row */
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) <= VJO_BUDGET_GAME_CAPTURE);
    TEST_CHECK(vjo_ledger_pool_peak(&g_game_ledger, VJO_LEDGER_POOL_GAME) <= VJO_WORST_GAME_PERMANENT + VJO_WORST_GAME_JOB);
    available = GAME_RESERVE;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
    available = -1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
    available = 16*1024*1024; budget_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
}
static void memory_failure_and_retry(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    alloc_failure = 1; workspace_tries = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !frees);
    /* every rung was tried, in order, and each attempt is in the step log */
    TEST_CHECK(workspace_tries == 3 && workspace_types[0] == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW &&
               workspace_types[1] == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW &&
               workspace_types[2] == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW);
    unsigned refused = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_WORKSPACE &&
            out.step[i].kind == refused + 1u /* VJO_GAME_OCR_MEM_USER, CDRAM, PHYCONT */ &&
            out.step[i].rc == (int32_t)0x80024302u) refused++;
    TEST_CHECK(refused == 3);
    TEST_MSG("workspace rungs refused in the step log: %u of %u steps", refused, out.n_steps);
    alloc_failure = 0; base_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && frees == 1);
    base_failure = 0; free_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(memory_live && blocked && out.cleanup_status && !out.text[0]);
    unsigned before = allocations;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && allocations == before);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && memory_live);
    free_failure = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !memory_live && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_start());
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}
static void cancel_and_wire(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    cancel_job = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !allocations);
    cancel_job = 0; r.width = 961;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.width = 200; r.x = 1; /* past the frame's edge */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.x = 0; r.flags = VJO_GAME_OCR_F_SHOT; r.speed = 1u << VJO_GAME_OCR_S_VERTICAL_SHIFT; /* API 47: no shot's */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.flags = 0x8000u; r.speed = 0; /* flags bit 15 is unknown (API 27's and draft #223's vertical text) */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.flags = 0; r.speed = VJO_GAME_OCR_S_VERTICAL; /* API 47: the vertical field's 3 is refused */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.flags = 0; r.speed = 0x20; /* VJO_GAME_OCR_S_* bits 5-15 and 17-22, 25-31 unknown (API 26: bit 4 is the gate) */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.speed = VJO_GAME_OCR_S_TRIM; /* API 32: a shot's bit, refused on an OCR job by the worker too */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.speed = 0;
    r.flags = 0; r.model[31] = 'x';
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
}
static void real_model_io_and_cancellation(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; model I/O test skipped"); return; }
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out; line(); short_reads = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(bytes_read == 657664 && reads_count > 1000 && out.lines == 1);
    TEST_CHECK(!memory_live && !model && !out.cleanup_status);
    TEST_CHECK(!user_open_calls); /* Model access never uses the game's unavailable ux0. */
    open_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.io_error == -123);
    open_failure = 0; bad_read = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.io_error == -456);
    bad_read = 0; close_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && model && !out.text[0]);
    close_failure = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model);
    reads_count = 0; cancel_reads = 4;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !model && !memory_live);
}
/* These are always run in CI, including checkouts without release weights. */
static void model_callbacks_timeouts_errors_and_cleanup(void)
{
    setup(); active_seq = 12; virtual_file = 1;
    VjoFile file; unsigned char bytes[16];
    open_failure = 1;
    TEST_CHECK(file_open(NULL, "ignored", &file) == -1 && io_error == -123);
    TEST_CHECK(model_owned && model_seq == 12);
    file_close(NULL); TEST_CHECK(!model_owned && !model_seq);
    open_failure = 0; open_busy = close_busy = 1;
    TEST_CHECK(file_open(NULL, "ignored", &file) == -1 && io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_owned && model_seq == 12 && !model);
    active_seq = 0; file_close(NULL);
    TEST_CHECK(model_owned && model_seq == 12 && io_error == VJO_ERR_TIMEOUT);
    close_busy = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model_owned && !model_seq);
    quitting = 0; active_seq = 12; open_busy = 0;
    TEST_CHECK(!file_open(NULL, "ignored", &file) && file.size == sizeof(bytes));
    TEST_CHECK(!file.read(NULL, 0, bytes, sizeof(bytes)) && bytes[0] == 1 && bytes[3] == 4);
    read_busy = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == VJO_ERR_TIMEOUT);
    read_busy = 0; bad_read = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == -456);
    bad_read = 0; read_eof = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == -1);
    read_eof = 0;
    TEST_CHECK(file.read(NULL, UINT32_MAX, bytes, sizeof(bytes)) == -1);
    TEST_CHECK(file.read(NULL, 0, NULL, sizeof(bytes)) == -1);
    cancel_job = 1; TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1);
    active_seq = 0; close_failure = 1; file.close(NULL);
    TEST_CHECK(model && model_owned && model_seq == 12 && io_error == -789);
    close_failure = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model && !model_owned);
}
/* #138: which copy of the worker stays in a process. The oldest module by name
 * (taiGetModuleInfo) is another copy only when none of its segments holds this
 * copy's code; a report with no segment says nothing, so the only copy can never
 * unload itself on a lookup that went wrong. */
static void the_oldest_copy_stays(void)
{
    const uintptr_t text = 0x81000000u, data = 0x81020000u, other_text = 0x81100000u;
    uintptr_t base[4] = {text, data, 0, 0};
    uint32_t size[4] = {0x12000u, 0x4000u, 0, 0};
    TEST_CHECK(!outside_segments(base, size, 4, text));          /* its first byte */
    TEST_CHECK(!outside_segments(base, size, 4, text + 0x11FFFu)); /* its last */
    TEST_CHECK(!outside_segments(base, size, 4, data + 0x10u));  /* a segment other than the first */
    TEST_CHECK(outside_segments(base, size, 4, text + 0x12000u)); /* one past its end: another module */
    TEST_CHECK(outside_segments(base, size, 4, text - 1u));
    base[0] = other_text; base[1] = other_text + 0x20000u;       /* an older copy elsewhere */
    TEST_CHECK(outside_segments(base, size, 4, text + 0x100u));
    memset(size, 0, sizeof(size));                               /* nothing reported */
    TEST_CHECK(!outside_segments(base, size, 4, text + 0x100u));
    TEST_CHECK(!outside_segments(base, size, 0, text + 0x100u));
}

static void system_app_gate_and_failed_joins(void)
{
    setup();
    /* SceShell ("main") and the system apps: the module unloads itself at
     * once (NO_RESIDENT), so *ALL costs them no pages; so does an unreadable title. */
    const char *excluded[] = {"main", "NPXS10015", "VITASHELL", "VJOSET001", ""};
    for (unsigned i=0; i<sizeof(excluded)/sizeof(excluded[0]); i++) {
        strcpy(own_title, excluded[i]);
        TEST_CHECK_(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !thread_creates && !allocations, "%s", excluded[i]);
    }
    /* A safe boot (the previous one failed): the module unloads itself in
     * every process, a candidate game included, before reading any title. */
    safe_boot_mode = VJO_BOOT_SAFE; boot_info_calls = 0;
    strcpy(own_title, "PCSG00415");
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && boot_info_calls == 1 && !thread_creates && !allocations);
    safe_boot_mode = VJO_BOOT_NORMAL;
    /* #138: the kernel loaded a copy into this game already (a per-title taiHEN
     * line loads another right after): this one gives its pages back at once. */
    vjo_test_another = 1; boot_info_calls = 0; vjo_test_another_calls = 0;
    strcpy(own_title, "PCSG00415");
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !thread_creates && !allocations);
    TEST_CHECK(vjo_test_another_calls == 1);
    strcpy(own_title, "NPXS10015"); /* checked after the title: a system app never asks */
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !thread_creates && vjo_test_another_calls == 1);
    vjo_test_another = 0;
    strcpy(own_title, "PCSG00415"); TEST_CHECK(!module_start(0, NULL) && thread_creates == 1);
    join_failure = 1;
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20 && worker_started);
    join_failure = 0; TEST_CHECK(!module_stop(0, NULL) && worker == -1);
    start_failure = delete_failure = 1;
    TEST_CHECK(!module_start(0, NULL) && worker == 20 && !worker_started);
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20);
    delete_failure = 0; TEST_CHECK(!module_stop(0, NULL));
}
/* The capture's memory ladder: USER_RW, then CDRAM, then PHYCONT, each attempt
 * a step with its native result; nothing left at all: out of memory, logged. */
static void capture_memory_ladder_is_logged(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    line();
    refuse_user = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.capture_memory == VJO_GAME_OCR_MEM_CDRAM && out.capture_bytes == 256u * 1024u);
    unsigned allocs = 0, refused = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_CAPTURE) {
            allocs++;
            if (out.step[i].rc == (int)0x80024302u) { refused++; TEST_CHECK(out.step[i].kind == VJO_GAME_OCR_MEM_USER); }
            else TEST_CHECK(out.step[i].kind == VJO_GAME_OCR_MEM_CDRAM && out.step[i].bytes == 256u * 1024u);
            TEST_CHECK(out.step[i].free_main == 16*1024*1024 - (int)live_bytes || out.step[i].free_main > 0);
        }
    TEST_CHECK(allocs == 2 && refused == 1 && !capture_live);
    refuse_cdram = 1;
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.capture_memory == VJO_GAME_OCR_MEM_PHYCONT && out.capture_bytes == 1024u * 1024u && !capture_live);
    refuse_phycont = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !capture_live && !memory_live);
    allocs = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_CAPTURE) allocs++;
    TEST_CHECK(allocs == 3);
}
/* The hook cannot write into the block: the same rows through the kernel strip,
 * for the rest of the job, and the step log says so. */
static void hook_failure_falls_back_to_the_kernel_strip(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    hook_write_error = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.text[0]);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_KSTRIP && kstrip_calls == rows_calls - 1);
    int saw = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].kind == VJO_GAME_OCR_SRC_HOOK && out.step[i].rc == VJO_ERR_COPY) saw = 1;
    TEST_CHECK(saw);
    /* a timeout (no frame) is not something a kernel strip fixes: the job fails, logged */
    hook_write_error = 0; rows_error = VJO_ERR_TIMEOUT; rows_calls = kstrip_calls = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !kstrip_calls && !capture_live && !memory_live);
    int logged = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].rc == VJO_ERR_TIMEOUT) logged = 1;
    TEST_CHECK(logged);
}
/* A BUSY before arming (a previous copy not tidied yet) is asked again; one
 * after arming (the copy outlived the call) keeps the block for the hook and
 * stops the worker. */
static void busy_rows_calls(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    busy_unarmed = 3;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !blocked && !hook_owns_capture && !capture_live);
    int logged = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].rc == VJO_ERR_BUSY) logged++;
    TEST_CHECK(logged == 1 && busy_unarmed == 0);
    busy_unarmed = 1000; /* never tidied: the job gives up, the worker stays usable */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !blocked && !capture_live && !memory_live);
    busy_unarmed = 0; busy_armed = 1; allow_capture_held = 1; rows_calls = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) != VJO_OK);
    /* the armed BUSY is not retried, so the unarmed ones after it never hide it */
    TEST_CHECK(rows_calls == 1 && blocked && hook_owns_capture && capture_live && !memory_live);
}

#if VJO_DEBUG /* the self-test's probe: the debug build's (docs/release-variants.md) */
/* The probe tries every rung once and recognizes nothing. */
static void probe_tries_every_rung(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.flags = VJO_GAME_OCR_F_PROBE;
    refuse_phycont = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !memory_live && !capture_live && !model);
    unsigned probes = 0, hook_ok = 0, kstrip_ok = 0, user_fail = 0, phycont_refused = 0;
    uint32_t sum = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        const VjoGameOcrStep *s = &out.step[i];
        if (s->id == VJO_GSTEP_ALLOC && s->kind == VJO_GAME_OCR_MEM_PHYCONT && s->rc < 0) phycont_refused++;
        if (s->id != VJO_GSTEP_PROBE) continue;
        probes++;
        if (s->kind == VJO_GAME_OCR_SRC_HOOK && s->rc == 60) { hook_ok++; if (!sum) sum = s->sum; TEST_CHECK(s->sum == sum); }
        if (s->kind == VJO_GAME_OCR_SRC_KSTRIP && s->rc == 60) { kstrip_ok++; TEST_CHECK(s->sum == sum); }
        if (s->kind == VJO_GAME_OCR_SRC_USER && s->rc < 0) user_fail++;
    }
    /* USER and CDRAM blocks, hook and kernel strip each, plus the user-mode read once */
    TEST_CHECK(probes == 5 && hook_ok == 2 && kstrip_ok == 2 && user_fail == 1 && phycont_refused == 1);
    TEST_CHECK(out.strip_rows == 60 && !out.lines && allocations == 0);
}
/* The probe's user-mode read of the frame stays inside the frame's own block:
 * the frame (200x60, 48,000 B) is in 64 KiB of mapped memory, and each refused
 * case makes the bytes outside the block it reports unreadable, so a read past
 * the check faults this test instead of passing it. */
static int probe_user_read(const VjoGameOcrRequest *r, uint32_t *sum)
{
    VjoGameOcrResult out;
    int rc = INT32_MIN;
    TEST_CHECK(!vjo_game_ocr_run_request(r, &out) && !capture_live && !memory_live);
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PROBE && out.step[i].kind == VJO_GAME_OCR_SRC_USER) {
            rc = out.step[i].rc;
            *sum = out.step[i].sum;
        } else if (out.step[i].id == VJO_GSTEP_PROBE && out.step[i].kind == VJO_GAME_OCR_SRC_HOOK && !*sum) {
            *sum = out.step[i].sum; /* the hook's rows of the same frame */
        }
    return rc;
}
static void probe_user_read_stays_in_the_frame_block(void)
{
    VjoGameOcrRequest r = setup();
    uint32_t hook_sum = 0, sum = 0;
    r.flags = VJO_GAME_OCR_F_PROBE;
    refuse_phycont = 1;
    line();
    unsigned char *map = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(map != MAP_FAILED);
    frame_at = map;
    memcpy(frame_at, pixels, 200 * 60 * 4);
    /* the whole frame in one readable block: 60 rows, the same grey as the hook's */
    block_base = (uintptr_t)map; block_size = 0x10000; block_access = SCE_KERNEL_MEMORY_ACCESS_R;
    int rc = probe_user_read(&r, &hook_sum);
    sum = hook_sum;
    TEST_CHECK_(rc == 60, "user-mode read: rc %d", rc);
    TEST_CHECK(frame_info_calls == 1 && hook_sum != 0);
    /* the block ends before the frame's last row: refused, the rest never read */
    block_size = 0xA000;
    TEST_ASSERT(mprotect(map + 0xA000, 0x6000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    TEST_ASSERT(mprotect(map, 0x10000, PROT_READ | PROT_WRITE) == 0);
    /* the exact edge: the block one byte short of the frame's last byte, nothing unreadable.
     * The read's last byte is first + (n-1)*pitch*bpp + width*bpp - 1 = 48,000-1; a block of
     * 47,999 B must refuse it, so a check that drops the last row's width or a row of the end
     * (which still fits 47,999) is caught here, not only by the unreadable bytes. */
    block_size = 48000 - 1;
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    block_size = 48000; /* exactly enough: read */
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == 60);
    block_size = 0x10000;
    /* the block begins after the frame's first row */
    block_base = (uintptr_t)map + 0x1000; block_size = 0xF000;
    TEST_ASSERT(mprotect(map, 0x1000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    /* a block this process cannot read */
    block_base = (uintptr_t)map; block_size = 0x10000; block_access = 0x02; /* write only */
    TEST_ASSERT(mprotect(map, 0x10000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    /* a frame of another size than the job's: refused before its block is asked */
    TEST_ASSERT(mprotect(map, 0x10000, PROT_READ | PROT_WRITE) == 0);
    block_access = SCE_KERNEL_MEMORY_ACCESS_R;
    frame_w = 240; frame_info_calls = 0;
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_FORMAT && !frame_info_calls);
    frame_w = 200;
    munmap(map, 0x10000);
    frame_at = NULL;
}
#else
/* The normal build has no probe (the self-test's): a probe request is refused as a
 * request it cannot run, before anything is allocated or read (docs/release-variants.md). */
static void normal_build_refuses_a_probe(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.flags = VJO_GAME_OCR_F_PROBE;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE);
    TEST_CHECK(!memory_live && !capture_live && !model && !out.lines && !out.strip_rows && allocations == 0);
}
#endif
/* The request's flags become the job's policy (include/vjo_game_ocr.h): the
 * wait in 10 ms steps, its limit in 50 ms steps, settle_first, the blank-line
 * check; a request without them is API 19's job (a zero policy but for the
 * sleep callback, which only a wait uses). */
static void request_flags_become_the_policy(void)
{
    VjoGameOcrRequest r = setup();
    VjoVocrPolicy p;
    request_policy(&r, &p);
    TEST_CHECK(!p.settle_us && !p.settle_max_us && !p.settle_first && !p.blank && p.sleep == policy_sleep);
    r.flags = VJO_GAME_OCR_F_BLANK;
    request_policy(&r, &p);
    TEST_CHECK(p.blank == 1 && !p.settle_us && !p.settle_first);
    r.flags = VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    request_policy(&r, &p);
    TEST_CHECK(p.settle_us == 150000u && p.settle_max_us == 2000000u && p.settle_first == 1 && !p.blank);
    r.flags = 255u << VJO_GAME_OCR_SETTLE_SHIFT | 255u << VJO_GAME_OCR_SETTLE_MAX_SHIFT; /* the largest the bits hold */
    request_policy(&r, &p);
    TEST_CHECK(p.settle_us == 2550000u && p.settle_max_us == 12750000u && !p.settle_first);
    TEST_CHECK(!p.gate && p.gate_res == &gate_res);
    r.flags = 0; r.speed = VJO_GAME_OCR_S_GATE; /* API 26: the text-presence gate */
    request_policy(&r, &p);
    TEST_CHECK(p.gate == 1 && p.gate_res == &gate_res && !p.head_gate && !p.two_level);
    /* API 42: bit 18 the head gate, bit 19 the two-level head, each on its own (ocr_head_gate's values) */
    static const struct { uint32_t speed; int head_gate, two_level; } heads[] = {
        {VJO_GAME_OCR_S_HEAD_GATE, 1, 0}, {VJO_GAME_OCR_S_TWO_LEVEL, 0, 1},
        {VJO_GAME_OCR_S_HEAD_GATE | VJO_GAME_OCR_S_TWO_LEVEL, 1, 1}, {0, 0, 0}};
    for (unsigned i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
        r.speed = heads[i].speed;
        request_policy(&r, &p);
        TEST_CHECK_(p.head_gate == heads[i].head_gate && p.two_level == heads[i].two_level && !p.gate,
                    "speed 0x%x", (unsigned)heads[i].speed);
    }
}

/* API 26: the text-presence gate through the worker. The weights' file opens by its pinned name
 * (VJO_GAME_OCR_OPEN_GATE), is read once and kept; each job whose finder found a line logs a gate step:
 * arg 1 when the gate stopped it (no line returned), 0 when it kept its lines, VJO_GLM_READ in the bytes when
 * the job read the weights. This harness's 200 x 60 region is one strip, so a job that waits first holds it
 * whole before the finder runs (audit-textgate-rubi F1): the gate still decides. */
static const VjoGameOcrStep *gate_step(const VjoGameOcrResult *out)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == VJO_GSTEP_GATE) return &out->step[i];
    return NULL;
}
static void stripes(void)
{
    for (unsigned y = 0; y < 60; y++)
        for (unsigned x = 0; x < 200; x++) {
            unsigned char v = (unsigned char)((x + y) / 10u % 2u ? 40 : 200);
            pixels[(y * 200 + x) * 4] = pixels[(y * 200 + x) * 4 + 1] = pixels[(y * 200 + x) * 4 + 2] = v;
        }
}
static void the_gate_runs_in_the_worker(void)
{
    const uint32_t wait = VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT |
                          40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    VjoGameOcrRequest r;
    VjoGameOcrResult out;
    const VjoGameOcrStep *g;
    if (!VJO_VOCR_GATE_TEST_DIR[0]) { TEST_SKIP("no gate weights (cmake/VocrGate.cmake)"); return; }
    for (unsigned waits = 0; waits <= 1; waits++) {
        /* stripes: the finder finds a line, the gate stops the job */
        r = setup(); stripes(); frame_us = 16667; r.speed = VJO_GAME_OCR_S_GATE; r.flags = waits ? wait : 0u;
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.lines && !out.text[0]);
        g = gate_step(&out);
        TEST_ASSERT(g != NULL);
        TEST_CHECK_(g->arg == 1 && !g->rc && g->kind == VJO_VOCR_GATE_MODE && (g->bytes & VJO_GLM_READ) &&
                    (int)(g->bytes & 0xFFFFu) - 129 < VJO_VOCR_GATE_T && (int)((g->bytes >> 16) & 0x7FFFu) - 129 == VJO_VOCR_GATE_T &&
                    g->sum == 60u && gate_opens == 1,
                    "waits %u: arg %u rc %d score %d rows %u opens %u", waits, g->arg, (int)g->rc,
                    (int)(g->bytes & 0xFFFFu) - 129, (unsigned)g->sum, gate_opens);
        TEST_CHECK(gate_res.verified && gate_res.size == VJO_VOCR_GATE_BYTES && !model);
        /* the same job again: the file is opened for its size and time only, the weights stay */
        r = setup_keeping_the_gate(); stripes(); frame_us = 16667; r.speed = VJO_GAME_OCR_S_GATE; r.flags = waits ? wait : 0u;
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.lines);
        g = gate_step(&out);
        TEST_CHECK_(g && g->arg == 1 && !(g->bytes & VJO_GLM_READ) && gate_opens == 1 && gate_res.verified,
                    "again: arg %u bytes 0x%X opens %u", g ? g->arg : 99u, g ? (unsigned)g->bytes : 0u, gate_opens);
        /* a text line: kept (the job then reads it, or fails on the missing model as without the gate) */
        r = setup(); line(); frame_us = 16667; r.speed = VJO_GAME_OCR_S_GATE; r.flags = waits ? wait : 0u;
        vjo_game_ocr_run_request(&r, &out);
        g = gate_step(&out);
        TEST_CHECK_(g && !g->arg && !g->rc && (int)(g->bytes & 0xFFFFu) - 129 >= VJO_VOCR_GATE_T,
                    "waits %u: a line kept: arg %u rc %d", waits, g ? g->arg : 99u, g ? (int)g->rc : 0);
        /* a blank region: the finder finds no line, the gate does not score, no step */
        r = setup(); frame_us = 16667; r.speed = VJO_GAME_OCR_S_GATE; r.flags = waits ? wait : 0u;
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.lines && !gate_step(&out));
    }
    /* without the bit, no gate: the stripes' line is read (and the model, missing or not, opened) */
    r = setup(); stripes(); frame_us = 16667;
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(!gate_step(&out) && !gate_opens);
    /* without its file, the gate does not decide: the job reads as without it and logs why */
    r = setup(); stripes(); frame_us = 16667; r.speed = VJO_GAME_OCR_S_GATE; open_gate_missing = 1;
    vjo_game_ocr_run_request(&r, &out);
    g = gate_step(&out);
    TEST_CHECK(g && !g->arg && g->rc == VJO_E_OCR_MODEL && !gate_res.verified && gate_opens == 1);
    TEST_CHECK_(last_which != VJO_GAME_OCR_OPEN_GATE && !model_owned && !model,
                "the job's model opens after the gate's failed open (last which %u)", last_which);
    open_gate_missing = 0;
}

/* #418: the gate's weights are a block of their own (1,560 B of the worker's .bss before, in every game, the gate on
 * or off): taken at the first job that asks for the gate, 4,096 B of USER_RW (its ALLOC step's item VJO_GITEM_GATE;
 * the spotter's ledger item counts it, GL_GATE), kept between such jobs (no read, no ALLOC), given back by a job that
 * does not ask for it (its FREE step), with the other resident blocks, when its file goes, and by a game that is no
 * longer the active one. It is taken before the job's weights, so never from the room the job's check counted for
 * them: without 4 KiB more the job reads as without the gate (rc VJO_E_OOM), the game asked for nothing. */
static const VjoGameOcrStep *item_step(const VjoGameOcrResult *out, unsigned id, unsigned item)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == id && out->step[i].arg == item) return &out->step[i];
    return NULL;
}
static void the_gate_s_weights_are_a_block_of_their_own(void)
{
    VjoGameOcrRequest r;
    VjoGameOcrResult out;
    const VjoGameOcrStep *g, *a;
    uint32_t need;
    if (!VJO_VOCR_GATE_TEST_DIR[0]) { TEST_SKIP("no gate weights (cmake/VocrGate.cmake)"); return; }
    /* the first job with the gate: its block taken, the weights read into it */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    a = item_step(&out, VJO_GSTEP_ALLOC, VJO_GITEM_GATE);
    g = gate_step(&out);
    TEST_ASSERT(a != NULL && g != NULL);
    TEST_CHECK(a->kind == VJO_GAME_OCR_MEM_USER && a->rc == 16 && a->bytes == VJO_BUDGET_GAME_GATE);
    TEST_CHECK(g->arg == 1 && (g->bytes & VJO_GLM_READ) && gate_live && gate_allocs == 1 && gate_block == 4096u);
    TEST_CHECK(gate_res.verified && gate_res.data == gate_memory && held_bytes(&gate_res) == VJO_BUDGET_GAME_GATE);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_GATE) == VJO_BUDGET_GAME_GATE);
    TEST_CHECK(blocks_kept() == 1u + !!held_bytes(&resident)); /* verified: its FREE step keeps no slot */
    /* the next job with the gate: kept, not read again, no step for its block */
    r = setup_keeping_the_gate(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    g = gate_step(&out);
    TEST_CHECK(g && g->arg == 1 && !(g->bytes & VJO_GLM_READ) && gate_live && !gate_allocs && !gate_frees);
    TEST_CHECK(!item_step(&out, VJO_GSTEP_ALLOC, VJO_GITEM_GATE) && !item_step(&out, VJO_GSTEP_FREE, VJO_GITEM_GATE));
    /* a job without the gate: the block goes back, its FREE step logged, as the LM's does with the LM off */
    r = setup_keeping_the_gate(); stripes();
    vjo_game_ocr_run_request(&r, &out);
    a = item_step(&out, VJO_GSTEP_FREE, VJO_GITEM_GATE);
    TEST_CHECK(a && !a->rc && a->bytes == VJO_BUDGET_GAME_GATE && !gate_live && gate_frees == 1 && !gate_step(&out));
    TEST_CHECK(!gate_res.verified && !gate_res.data && gate_blk.uid < 0 &&
               !vjo_ledger_current(&g_game_ledger, GL_GATE));
    /* given back with the other resident blocks (a job that does not fit, the worker's stop) */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && gate_live);
    TEST_CHECK(!drop_resident() && !gate_live && gate_frees == 1 && !gate_res.verified && gate_blk.uid < 0);
    /* its file gone: given back (memory is held only for the verified file on the card) */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && gate_live);
    r = setup_keeping_the_gate(); stripes(); r.speed = VJO_GAME_OCR_S_GATE; open_gate_missing = 1;
    vjo_game_ocr_run_request(&r, &out);
    g = gate_step(&out);
    TEST_CHECK(g && !g->arg && g->rc == VJO_E_OCR_MODEL && !gate_live && gate_frees == 1 && !gate_res.verified);
    open_gate_missing = 0;
    /* a game no longer the active one gets it back from the worker's loop (ocr-first) */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && gate_live);
    quit_on_delay = 1;
    inactive_since = now_wide() - INACTIVE_DROP_US;
    worker_main(0, NULL); /* vjoOcrRegister: not the active game */
    quit_on_delay = 0;
    quitting = 0;
    TEST_CHECK(!gate_live && gate_frees == 1 && !gate_res.verified && !blocked);
    /* the room: the job's check passed with 2 KiB to spare beside its weights, still to come: the gate is refused
     * before the game is asked (no ALLOC step), the job reads as without it, its weights as before */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE; refuse_gate = 1;
    vjo_game_ocr_run_request(&r, &out);
    need = out.need_workspace + out.need_weights; /* the job's check (the gate's block is not in it) */
    TEST_ASSERT(need > 0 && out.need_weights > 0 && gate_allocs == 1 && !gate_live);
    g = gate_step(&out);
    TEST_CHECK(g && g->rc == VJO_E_OOM && !g->arg); /* refused by the game: the job read as without the gate */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    available = (int)(GAME_RESERVE + need + 2048u);
    vjo_game_ocr_run_request(&r, &out);
    g = gate_step(&out);
    TEST_CHECK_(g && g->rc == VJO_E_OOM && !gate_allocs && !item_step(&out, VJO_GSTEP_ALLOC, VJO_GITEM_GATE) &&
                    out.need_workspace + out.need_weights == need,
                "2 KiB to spare: gate rc %d, its allocations %u", g ? (int)g->rc : 0, gate_allocs);
    /* 4 KiB more: taken */
    r = setup(); stripes(); r.speed = VJO_GAME_OCR_S_GATE;
    available = (int)(GAME_RESERVE + need + 2048u + VJO_BUDGET_GAME_GATE);
    vjo_game_ocr_run_request(&r, &out);
    g = gate_step(&out);
    TEST_CHECK_(g && !g->rc && g->arg == 1 && gate_allocs == 1 && gate_live,
                "6 KiB to spare: gate rc %d, allocations %u", g ? (int)g->rc : 0, gate_allocs);
    TEST_CHECK(!drop_resident() && !gate_live);
}

/* API 20: the job's policy rides in the request's flags. A wait of 150 ms
 * (15 x 10 ms) first, at 60 frames a second, on a still screen: the job reads
 * the region until it held still, records the wait as a step, and only then
 * finds its line; the blank-line check passes a line of strokes. Unknown flag
 * bits are refused before anything is allocated. */
static void policy_in_the_flags(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    unsigned plain;
    line();
    frame_us = 16667;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    plain = rows_calls;
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_SETTLE);
    rows_calls = 0;
    r.flags = VJO_GAME_OCR_F_BLANK | VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT |
              40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    const VjoGameOcrStep *settle = NULL;
    unsigned blank = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_SETTLE) settle = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_BLANK) blank++;
    }
    TEST_ASSERT(settle != NULL);
    TEST_CHECK(settle->rc == VJO_GSETTLE_HELD && settle->arg >= 2 && settle->bytes == 0 && settle->us >= 150000);
    TEST_MSG("waited %u us in %u passes; %u rows calls against %u without", (unsigned)settle->us, settle->arg,
             rows_calls, plain);
    TEST_CHECK(!blank && out.lines == 1 && rows_calls > plain && !memory_live && !capture_live);
    allocations = 0;
    r.flags = VJO_GAME_OCR_F_SHOT; r.speed = 2u << VJO_GAME_OCR_S_VERTICAL_SHIFT; /* API 47's vertical text: no shot's */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
    r.speed = 0;
    /* a shot carries no job policy: refused before anything is taken */
    r.flags = VJO_GAME_OCR_F_SHOT | VJO_GAME_OCR_F_BLANK;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
    r.flags = VJO_GAME_OCR_F_SHOT;
    r.speed = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
    r.speed = 0;
}

/* API 22 (ocr-speed): helpers and the ARM clock ride in the flags. Helpers are
 * counted in the memory check and dropped one at a time when they do not fit;
 * the clock goes to 444 MHz around the recognition only and back after it. */
static void helpers_and_clock_in_the_flags(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    const size_t block = vjo_vocr_job_bytes(r.model, 200, 60); /* no LM, cascade or refinement here */
    uint32_t plain_need, extra2 = (uint32_t)helpers_need(r.model, block, 2);
    uint32_t extra1 = (uint32_t)helpers_need(r.model, block, 1);
    const VjoGameOcrStep *clock = NULL, *par = NULL;
    line();
    arm_mhz = 333; arm_sets = 0; helper_threads = helper_live = helper_starts = 0;
    vjo_game_ocr_run_request(&r, &out);
    plain_need = out.need_workspace;
    TEST_CHECK(arm_sets == 0 && plain_need > 0);
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_CLOCK && out.step[i].id != VJO_GSTEP_PAR);
    /* two helpers asked for, room for both: counted, stacks included */
    TEST_CHECK(extra1 > 2u * VJO_BUDGET_GAME_HELPER_STACK / 2u && extra2 > extra1);
    r.speed = 2u | VJO_GAME_OCR_S_BOOST;
    memset(&line_cache, 0, sizeof(line_cache)); /* the plain job's line is cached: recognize it again */
    int rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(!rc || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.need_workspace == plain_need + extra2);
    TEST_MSG("need %u, without helpers %u, two helpers add %u", (unsigned)out.need_workspace, plain_need, extra2);
    TEST_CHECK(!memory_live && !capture_live && helper_live == 0);
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_CLOCK) clock = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_PAR) par = &out.step[i];
    }
    if (VJO_VOCR_MODEL_DIR[0] && !rc && out.recognized) {
        /* one line: it runs on the worker's own thread (the last line always does) */
        TEST_ASSERT(clock != NULL && par != NULL);
        TEST_CHECK(clock->arg == 333 && clock->bytes == 444 && clock->sum == 333 && clock->rc == 0);
        TEST_CHECK(arm_sets == 2 && arm_set_last == 333 && arm_mhz == 333);
        TEST_CHECK(par->arg == 2 && par->rc == 0 && par->bytes == extra2 && helper_threads == 0);
        TEST_CHECK(((par->sum >> 16) & 0xFFu) == 0); /* no line from the cache */
        /* already at 444 or more (PSVshell's 500): not raised, never lowered, logged as left alone */
        static const int already[] = {444, 500};
        for (unsigned k = 0; k < 2; k++) {
            arm_mhz = already[k]; arm_sets = 0;
            resident.model = NULL; /* weights again, so the line is recognized, not cached */
            memset(&line_cache, 0, sizeof(line_cache));
            TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
            clock = NULL;
            for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_CLOCK) clock = &out.step[i];
            TEST_CHECK(clock && clock->rc == 1 && clock->arg == (unsigned)already[k]);
            TEST_CHECK(clock && clock->bytes == (uint32_t)already[k] && clock->sum == (uint32_t)already[k]);
            TEST_CHECK(arm_sets == 0 && arm_mhz == already[k]);
            TEST_MSG("at %d MHz: %d set call(s), now %d MHz", already[k], arm_sets, arm_mhz);
        }
        /* the same screen again: the line comes from the cache, no clock change, no recognizer */
        arm_mhz = 333; arm_sets = 0;
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        par = NULL;
        for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_PAR) par = &out.step[i];
        TEST_CHECK(arm_sets == 0 && out.frames == 0 && out.recognized == 1 && par && ((par->sum >> 16) & 0xFFu) == 1);
    }
    /* free memory for one helper only, then for none: fewer helpers, never a refusal
     * (the resident weights, if any, are already part of live_bytes) */
    TEST_ASSERT(resident.model != NULL || !VJO_VOCR_MODEL_DIR[0]); /* weights resident: they need nothing more */
    available = (int)(live_bytes + VJO_GAME_RESERVE + plain_need + extra1 + 4096u);
    if (!resident.model) available += (int)PAGE_ROUND(vjo_vocr_model(r.model)->bytes);
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.need_workspace == plain_need + extra1);
    TEST_MSG("need %u (plain %u, +1 helper %u, +2 %u)", (unsigned)out.need_workspace, plain_need, extra1, extra2);
    available = (int)(live_bytes + VJO_GAME_RESERVE + plain_need + 4096u);
    if (!resident.model) available += (int)PAGE_ROUND(vjo_vocr_model(r.model)->bytes);
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.need_workspace == plain_need && out.rc != VJO_E_OOM);
    TEST_MSG("need %u (plain %u, +1 helper %u)", (unsigned)out.need_workspace, plain_need, extra1);
    arm_mhz = 333;
}

/* Two lines of strokes, 30 rows apart, in a 200 x 120 region (two strips). */
static void two_lines(VjoGameOcrRequest *r)
{
    frame_h = 120;
    r->height = r->fb_height = 120;
    for (unsigned y = 15; y < 39; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
    for (unsigned y = 70; y < 94; y++) for (unsigned x = 14; x < 150; x += 5) memset(pixels + (y * 200 + x) * 4, 235, 5);
}

/* Five lines of strokes, 50 rows apart, in a 200 x 272 region (five strips). */
static void five_lines(VjoGameOcrRequest *r)
{
    frame_h = 272;
    r->height = r->fb_height = 272;
    for (unsigned l = 0; l < 5; l++)
        for (unsigned y = 15 + 50 * l; y < 39 + 50 * l; y++)
            for (unsigned x = 10 + 2 * l; x < 190 - 15 * l; x += 5 + l) memset(pixels + (y * 200 + x) * 4, 235, 4);
}

/* API 22: a helper that cannot be joined (its WaitThreadEnd times out) may still run in the
 * job's workspace and read the weights. The job fails with VJO_E_OCR_UNAVAILABLE, both stay
 * held, the worker takes no more jobs; worker_stop waits for the worker first, then for the
 * helper, and only then gives everything back. */
static void a_stuck_helper_keeps_its_memory_until_stop(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("no release weights"); return; }
    two_lines(&r);
    r.speed = 1; /* one helper: the first line runs on it, the last on the worker */
    helper_join_failure = 1;
    int rc = vjo_game_ocr_run_request(&r, &out);
    if (!helper_threads) { TEST_SKIP("the finder did not find two lines (rc %d, %u lines)", rc, out.lines); return; }
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE && out.rc == VJO_E_OCR_UNAVAILABLE && !out.text[0]);
    TEST_CHECK(blocked && hook_owns_workspace && workspace_uid >= 0 && memory_live && weights_live);
    TEST_CHECK(!capture_live && helper_uid[1] >= 0 && helper_live == 1);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_HELPERS) == VJO_BUDGET_GAME_HELPER_STACK);
    TEST_MSG("rc %d, %u lines, helper threads %u", rc, out.lines, helper_threads);
    /* no other job while it may run */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && memory_live && weights_live);
    /* stop: the worker thread first (its own job joins its helpers), then the helper left over */
    vjo_ledger_add(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK);
    worker = 20; worker_started = 1; n_waits = 0;
    helper_join_failure = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop());
    TEST_CHECK(n_waits == 2 && wait_order[0] == 20 && wait_order[1] > 40);
    TEST_CHECK(!memory_live && !weights_live && workspace_uid < 0 && !hook_owns_workspace && !blocked);
    TEST_CHECK(helper_uid[1] < 0 && helper_live == 0 && vjo_ledger_current(&g_game_ledger, GL_HELPERS) == 0);
    /* and after a start again (worker_start clears quitting) the next job runs */
    __atomic_store_n(&quitting, 0, __ATOMIC_RELEASE);
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(!memory_live && helper_live == 0);
}

/* API 22 with the release weights, the 1 MB LM and the cascade's VOCR4-W20 reading every line again:
 * one and two helpers give the serial job's text and its recognizer text (kept after the NUL), and the
 * helpers' line slots, at the end of the workspace, overlap neither the LM's extras nor that text. */
static void helpers_with_the_lm_and_cascade_give_the_serial_text(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    static char serial_text[sizeof(out.text)], serial_raw[sizeof(out.text)];
    char lm2[600], w20[600];
    FILE *f;
    snprintf(lm2, sizeof(lm2), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_LM2_NAME);
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(lm2, "rb"))) { TEST_SKIP("no LM beside the release weights"); return; }
    fclose(f);
    if (!(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    five_lines(&r); /* with one helper, its line is still out when the worker's own slot takes the next */
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001; /* every line with text is read again */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    if (out.lines < 5) { TEST_SKIP("the finder did not find five lines (%u)", out.lines); return; }
    strcpy(serial_text, out.text);
    strcpy(serial_raw, out.text + strlen(out.text) + 1);
    TEST_CHECK(serial_text[0] && serial_raw[0]);
    for (unsigned helpers = 1; helpers <= VJO_VOCR_HELPERS_MAX; helpers++) {
        vjo_game_ocr_cache_clear(); /* recognized again, not cached */
        helper_threads = 0;
        helper_deferred = 1; /* the first line still out when the last runs: the worker's own slot */
        r.speed = helpers;
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
        TEST_CHECK_(!strcmp(out.text, serial_text) && !strcmp(out.text + strlen(out.text) + 1, serial_raw),
                    "%u helper(s): \"%s\" / \"%s\", serially \"%s\" / \"%s\"", helpers, out.text,
                    out.text + strlen(out.text) + 1, serial_text, serial_raw);
        TEST_CHECK(helper_threads >= 1 && helper_live == 0 && !memory_live);
        for (unsigned i = 0; i < out.recognized; i++) TEST_CHECK(out.line[i].flags & VJO_VOCR_LINE_CASCADE);
        {   /* the first line on a helper (the PAR step's rc: lines on helpers) */
            const VjoGameOcrStep *par = NULL;
            for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_PAR) par = &out.step[i];
            TEST_CHECK(out.recognized >= 2 && par && par->arg == helpers && par->rc >= 1);
        }
    }
    helper_deferred = 0;
    r.speed = 0;
}

/* API 22: VJO_GAME_OCR_S_NOCACHE (the self-test's stages) empties the line cache and leaves it
 * unused: the job recognizes its line, and nothing is cached from it. */
static void a_nocache_job_recognizes_and_caches_nothing(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("no release weights"); return; }
    line();
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && out.recognized == 1 && out.frames > 0);
    unsigned keys = 0;
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) keys += line_cache.line[i].key != 0;
    TEST_CHECK(keys == 1);
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && out.frames == 0); /* the same screen: from the cache */
    r.speed = VJO_GAME_OCR_S_NOCACHE;
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && out.recognized == 1 && out.frames > 0);
    keys = 0;
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) keys += line_cache.line[i].key != 0;
    TEST_CHECK(keys == 0);
    r.speed = 0;
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && out.frames > 0); /* recognized again, then cached */
}

/* #418: the worker's line cache keeps a line's greedy text after its shown text, only where it is another: every
 * job of this file replays its cache operations through the old layout (cache_replay.h, installed by setup) and
 * checks that both hold the same after each. Here the worker's sequence with the release weights (those of
 * a_nocache_job_recognizes_and_caches_nothing, then the LM and the cascade, the self-test's NOCACHE and the weights
 * read again), whose hits and misses main's cache gives too. */
static void the_worker_s_cache_replayed_through_the_old_layout(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    static const unsigned want[][2] = {{0, 1}, {1, 1}, {0, 1}, {1, 1}, {1, 2}, {2, 2}, {0, 1}};
    unsigned seq[7][2], n = 0;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("no release weights"); return; }
    line();
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && out.recognized == 1); /* cold */
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && !out.frames); /* the same screen: from the cache */
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    r.speed = VJO_GAME_OCR_S_NOCACHE; /* the self-test's: emptied, unused */
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out));
    r.speed = 0;
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && out.frames > 0); /* recognized again, then kept */
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out) && !out.frames);
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT; /* the LM: another cache variant */
    vjo_game_ocr_run_request(&r, &out);
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    vjo_game_ocr_run_request(&r, &out);
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    TEST_CHECK(!drop_resident()); /* the weights read again: the cache emptied */
    vjo_game_ocr_run_request(&r, &out);
    seq[n][0] = line_cache.hits; seq[n++][1] = line_cache.misses;
    for (unsigned i = 0; i < n; i++)
        TEST_CHECK_(seq[i][0] == want[i][0] && seq[i][1] == want[i][1], "job %u: %u hits, %u misses (main's: %u, %u)",
                    i + 1u, seq[i][0], seq[i][1], want[i][0], want[i][1]);
    TEST_CHECK_(!replay.mismatches && !replay.left_out && replay.lookups > 0,
                "replayed: %u lookups, %u hits, %u entries offered, %u synced, %u left out, %u differed",
                replay.lookups, replay.hits, replay.puts, replay.synced, replay.left_out, replay.mismatches);
    r.flags = 0;
}

/* ocr_boost_clock: when the game sets another clock while a job recognizes, the job leaves
 * the game's clock as it is (no restore over it), and the step logs before, during, after. */
static void the_clock_stays_where_the_game_set_it(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    const VjoGameOcrStep *clock = NULL;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("no release weights"); return; }
    line();
    r.speed = VJO_GAME_OCR_S_BOOST;
    arm_mhz = 333;
    arm_game_mhz = 222;
    arm_game_at = 3; /* before, during, then the game's 222 MHz at the check after the last line */
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out));
    if (!out.recognized) { TEST_SKIP("no line recognized"); return; }
    for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_CLOCK) clock = &out.step[i];
    TEST_ASSERT(clock != NULL);
    TEST_CHECK(arm_sets == 1 && arm_set_last == 444 && arm_mhz == 222); /* raised once, never put back */
    TEST_CHECK(clock->arg == 333 && clock->bytes == 444 && clock->sum == 222 && clock->rc == 0);
    /* the game leaves it alone: the job puts its own value back */
    arm_mhz = 333; arm_sets = 0; arm_gets = 0; arm_game_at = 0;
    vjo_game_ocr_cache_clear();
    TEST_ASSERT(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(arm_sets == 2 && arm_set_last == 333 && arm_mhz == 333);
}

/* The step log: a retried job logs its first pass and its signature once (the
 * TEAR step counts the tries), and when the log fills up the other steps make
 * room for the release of every block the worker holds, so the FREE steps that
 * show the job gave its memory back are never the ones lost. */
static void step_log_keeps_the_releases(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    unsigned pass = 0, sig = 0, tear = 0, frees = 0;
    line();
    change_at_rows = 2; /* during the finder's second pass of the first try */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    for (unsigned i = 0; i < out.n_steps; i++) {
        pass += out.step[i].id == VJO_GSTEP_PASS && !out.step[i].rc;
        sig += out.step[i].id == VJO_GSTEP_SIG;
        if (out.step[i].id == VJO_GSTEP_TEAR) { tear++; TEST_CHECK(out.step[i].rc >= 2); }
        frees += out.step[i].id == VJO_GSTEP_FREE;
    }
    TEST_CHECK(tear == 1 && pass == 1 && sig == 1 && frees == 2 && !memory_live && !capture_live);
    TEST_MSG("%u steps: %u PASS, %u SIG, %u TEAR, %u FREE", out.n_steps, pass, sig, tear, frees);
    /* every block the job took is back, and counted back */
    TEST_CHECK(blocks_held == (unsigned)(weights_live ? 1 : 0));
    /* a full log: with two blocks held that the job frees, the 19th and 20th steps are the two
     * FREEs; verified resident weights (kept for the next job) keep no slot */
    VjoGameOcrResult log;
    memset(&log, 0, sizeof(log));
    log_to = &log;
    TEST_CHECK(blocks_kept() == (unsigned)(weights_live ? 1 : 0));
    blocks_held = 2 + blocks_kept();
    for (unsigned i = 0; i < VJO_GAME_OCR_STEPS; i++) step_add(VJO_GSTEP_ALLOC, 0, 0, 0, 0, 0, 1, 0);
    TEST_CHECK(log.n_steps == VJO_GAME_OCR_STEPS - 2);
    TEST_CHECK(step_add(VJO_GSTEP_FREE, 0, 1, 0, 4096, 0, 1, 0) && step_add(VJO_GSTEP_FREE, 0, 2, 0, 4096, 0, 1, 0));
    TEST_CHECK(log.n_steps == VJO_GAME_OCR_STEPS && !step_add(VJO_GSTEP_FREE, 0, 3, 0, 4096, 0, 1, 0));
    TEST_CHECK(log.step[VJO_GAME_OCR_STEPS - 1].id == VJO_GSTEP_FREE && log.step[VJO_GAME_OCR_STEPS - 1].arg == 2);
    log_to = NULL;
    blocks_held = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
}

/* API 20: a recognized job returns its text normalised, the recognizer's own
 * text after its NUL, and a NORM step; the last byte stays 0 (the kernel's check). */
static void returned_text_is_normalised_and_kept(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("VJO_VOCR_MODEL_DIR not set"); return; }
    line();
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.text[0] && !out.text[sizeof(out.text) - 1]);
    const char *raw = vjo_text_raw(out.text, sizeof(out.text));
    char again[sizeof(out.text)];
    snprintf(again, sizeof(again), "%s", raw);
    vjo_text_normalize(again, NULL);
    TEST_CHECK(raw[0] && !strcmp(again, out.text));
    unsigned norm = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_NORM) { norm++; TEST_CHECK(out.step[i].us > 0); }
    TEST_CHECK(norm == 1);
    TEST_MSG("text '%s', raw '%s'", out.text, raw);
}

/* A try that read a line, then saw the screen change, and a retry that found
 * the box empty: the job returns no text, and nothing of the first try after
 * the NUL either, which the Shell would log as the recognizer's text
 * (audit-overlay-dialogue-3 B2). A 200x100 region is two strips, with a line in
 * each: the first line is read before the second strip is read again. The box
 * empties at each rows call in turn, so one of them lands between the two. */
static void a_retry_to_an_empty_box_returns_nothing(void)
{
    unsigned torn_empty = 0;
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_SKIP("VJO_VOCR_MODEL_DIR not set"); return; }
    for (unsigned at = 2; at <= 12; at++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        size_t stale = 0;
        int torn = 0;
        frame_h = r.fb_height = r.height = 100;
        for (unsigned y = 15; y < 39; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
        for (unsigned y = 70; y < 94; y++) for (unsigned x = 13; x < 190; x += 7) memset(pixels + (y * 200 + x) * 4, 235, 5);
        clear_at_rows = at;
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
        if (out.rc || out.text[0]) continue;
        for (unsigned i = 0; i < out.n_steps; i++) torn |= out.step[i].id == VJO_GSTEP_TEAR;
        for (size_t i = 0; i < sizeof(out.text); i++) stale += out.text[i] != 0;
        TEST_CHECK(!stale && !vjo_text_raw(out.text, sizeof(out.text))[0]);
        TEST_MSG("box emptied at rows call %u: %zu bytes left after the NUL", at, stale);
        torn_empty += torn;
    }
    TEST_CHECK(torn_empty > 0); /* some job did retry into the empty box */
}

/* Whether the job freed block `gitem` before it allocated its workspace: the
 * memory check counted that block as released, so it must go first. */
static int freed_before_the_workspace(const VjoGameOcrResult *out, unsigned gitem)
{
    int freed = -1, alloc = -1;
    for (unsigned i = 0; i < out->n_steps; i++) {
        if (freed < 0 && out->step[i].id == VJO_GSTEP_FREE && out->step[i].arg == gitem) freed = (int)i;
        if (alloc < 0 && out->step[i].id == VJO_GSTEP_ALLOC && out->step[i].arg == VJO_GITEM_WORKSPACE) alloc = (int)i;
    }
    return freed >= 0 && alloc >= 0 && freed < alloc;
}

/* The memory check's choices (check_with_lm), from the tables' sizes alone (no
 * files): room for everything, the cascade with the largest LM tier; for the LM
 * alone or the cascade alone but not both, the LM, which brings more on the
 * dialogue lines measured; for neither, greedy. A resident LM the job gives back
 * because it chose none counts as free (audit-overlay-dialogue-4 N1). */
static void memory_check_choices(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult o;
    const VjoVocrModel *m = vjo_vocr_model(VJO_VOCR3_FL10_NAME), *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME), *casc;
    const VjoVocrLm *lm, *lm2 = vjo_vocr_lm_tier(2);
    size_t jb = vjo_vocr_job_bytes(VJO_VOCR3_FL10_NAME, r.width, r.height), cb = vjo_vocr_capture_bytes(r.width, r.height);
    uint32_t need_none, need_lm, need_casc, need_both;
    TEST_ASSERT(m && w20 && lm2 && jb && cb);
    memset(&o, 0, sizeof(o));
    o.free_user = INT32_MAX;
    check_memory(&o, m, jb, cb, NULL, NULL, 0);
    need_none = o.need_workspace + o.need_weights;
    check_memory(&o, m, jb, cb, lm2, NULL, 0);
    need_lm = o.need_workspace + o.need_weights;
    check_memory(&o, m, jb, cb, NULL, w20, 0);
    need_casc = o.need_workspace + o.need_weights;
    check_memory(&o, m, jb, cb, lm2, w20, 0);
    need_both = o.need_workspace + o.need_weights;
    TEST_ASSERT(need_none < need_lm && need_lm < need_casc && need_casc < need_both);
    /* room for everything */
    casc = w20;
    o.free_user = (int32_t)(GAME_RESERVE + need_both);
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 2, 0, &casc, &lm) && casc == w20 && lm == lm2);
    /* the LM alone or the cascade alone, not both: the LM */
    casc = w20;
    o.free_user = (int32_t)(GAME_RESERVE + need_casc);
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 2, 0, &casc, &lm) && !casc && lm == lm2);
    /* no LM allowed: the cascade alone */
    casc = w20;
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 0, 0, &casc, &lm) && casc == w20 && !lm);
    /* room for neither: greedy, and the job still fits */
    casc = w20;
    o.free_user = (int32_t)(GAME_RESERVE + need_none);
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 2, 0, &casc, &lm) && !casc && !lm);
    /* the 1 MB LM resident and no LM chosen: the job gives it back, so its block counts as free */
    lm_res.lm = lm2;
    lm_blk.uid = 12;
    lm_blk.held = PAGE_ROUND(lm2->bytes); /* its block, as the worker holds it (resident_alloc) */
    o.free_user = (int32_t)(GAME_RESERVE + need_none - PAGE_ROUND(lm2->bytes) + 4096u);
    casc = NULL;
    TEST_CHECK(check_with_lm(&o, m, jb, cb, 0, 0, &casc, &lm) && !lm && o.release_bytes == PAGE_ROUND(lm2->bytes));
    lm_res.lm = NULL;
    lm_blk.uid = -1;
    lm_blk.held = 0;
}

/* The cascade step's fields, from the job's figures (include/vjo_game_ocr.h):
 * kind the lines it read as empty, arg the lines read again, the lines it
 * changed << 24, those left for the ceiling << 16, its frames; each clamped to
 * its field. */
static void the_cascade_step_carries_its_figures(void)
{
    VjoGameOcrResult log;
    VjoVocrStats st;
    memset(&log, 0, sizeof(log));
    memset(&st, 0, sizeof(st));
    log_to = &log;
    st.cascade_lines = 4; st.cascade_changed = 3; st.cascade_skipped = 2; st.cascade_blanked = 1;
    st.cascade_frames = 300; st.cascade_loaded = 1; st.cascade_load_us = 290000; st.cascade_us = 128000;
    st.cascade_rc = VJO_E_OOM;
    cascade_step(&st);
    st.cascade_changed = 100; st.cascade_skipped = 300; st.cascade_frames = 70000; st.cascade_loaded = 0;
    st.cascade_cached = 1; /* past each field: clamped, never spilling into the next */
    cascade_step(&st);
    log_to = NULL;
    TEST_ASSERT(log.n_steps == 2);
    const VjoGameOcrStep *a = &log.step[0], *b = &log.step[1];
    TEST_CHECK(a->id == VJO_GSTEP_CASCADE && a->kind == 1 && a->arg == 4 && a->rc == VJO_E_OOM);
    TEST_CHECK(a->bytes == (VJO_GLM_READ | 3u << 24 | 2u << 16 | 300u) && a->sum == 290000u && a->us == 128001u);
    TEST_CHECK(b->bytes == (VJO_GLM_RESIDENT | 0x3Fu << 24 | 0xFFu << 16 | 0xFFFFu));
}

/* Between jobs the worker gives back an LM or cascade block whose contents were
 * never verified (a read refused half-way, the job cancelled), as it does the
 * weights' (cleanup). */
static void cleanup_gives_back_unverified_blocks(void)
{
    VjoGameOcrRequest r = setup();
    (void)r;
    available = 64 * 1024 * 1024;
    TEST_ASSERT(resident_alloc((void *)&cascade_desc, VJO_VOCR_CASCADE_BYTES) != NULL && cascade_live &&
                cascade_blk.uid >= 0);
    TEST_ASSERT(resident_alloc((void *)&lm_desc, vjo_vocr_lm_tier(2)->bytes) != NULL && lm_live && lm_blk.uid >= 0);
    TEST_CHECK(blocks_held == 2 && !blocks_kept()); /* neither verified: both keep a slot of the log */
    TEST_CHECK(cleanup() == 0 && !cascade_live && cascade_blk.uid < 0 && !lm_live && lm_blk.uid < 0);
    TEST_CHECK(cascade_frees == 1 && lm_frees == 1 && !blocks_held);
}

/* The weights block and the cascade's block hold the largest pinned model, the Large
 * size's (test_tier_pins.h: VOCR6-L, 2,475,680 B, 2,478,080 B page-rounded, the ledger's
 * VJO_BUDGET_GAME_WEIGHTS and VJO_BUDGET_GAME_CASCADE), and refuse a byte more before
 * asking the game for anything; each block keeps the job's reserve. Before VOCR5
 * both stopped at F20's 1,558,016 B. */
static void the_weights_blocks_hold_the_large_size_and_no_more(void)
{
    VjoGameOcrRequest r = setup();
    const uint32_t block = PAGE_ROUND(test_tier_pins[2].bytes);
    (void)r;
    TEST_CHECK(VJO_VOCR_MAX_BYTES == test_tier_pins[2].bytes && VJO_VOCR_MAX_BYTES == VJO_VOCR_TIER3_BYTES);
    TEST_CHECK(vjo_vocr_model(VJO_VOCR_TIER3_NAME)->bytes == VJO_VOCR_MAX_BYTES);
    available = 64 * 1024 * 1024;
    job_reserve = VJO_GAME_RESERVE_LARGE;
    /* one byte more is refused, the game never asked */
    TEST_CHECK(!resident_alloc((void *)&weights_desc, VJO_VOCR_MAX_BYTES + 1u) && !weights_live && weights_blk.uid < 0);
    TEST_CHECK(!resident_alloc((void *)&cascade_desc, VJO_VOCR_MAX_BYTES + 1u) && !cascade_live && cascade_blk.uid < 0);
    TEST_CHECK(!weight_allocations && !cascade_allocations);
    /* the Large size's blob: one page-rounded USER block each */
    TEST_ASSERT(resident_alloc((void *)&weights_desc, VJO_VOCR_MAX_BYTES) != NULL && weights_live &&
                weights_block == block);
    TEST_CHECK(weights_block == VJO_BUDGET_GAME_WEIGHTS && weights_blk.held == block);
    /* never two blocks */
    TEST_CHECK(!resident_alloc((void *)&weights_desc, VJO_VOCR_TIER1_BYTES) && weight_allocations == 1);
    TEST_ASSERT(resident_alloc((void *)&cascade_desc, VJO_VOCR_MAX_BYTES) != NULL && cascade_live &&
                cascade_block == block);
    TEST_CHECK(cascade_block == VJO_BUDGET_GAME_CASCADE);
    TEST_CHECK(!resident_release((void *)&weights_desc) && !resident_release((void *)&cascade_desc) && !weights_live &&
               !cascade_live);
    /* the reserve is kept: Large's block with exactly 1 MiB left over fits, a page less does not */
    available = (int)(VJO_GAME_RESERVE_LARGE + block);
    TEST_ASSERT(resident_alloc((void *)&weights_desc, VJO_VOCR_MAX_BYTES) != NULL && weights_live);
    TEST_CHECK(!resident_release((void *)&weights_desc));
    available = (int)(VJO_GAME_RESERVE_LARGE + block - 4096u);
    /* not asked */
    TEST_CHECK(!resident_alloc((void *)&weights_desc, VJO_VOCR_MAX_BYTES) && !weights_live && weight_allocations == 2);
    job_reserve = GAME_RESERVE;
}

/* A game that is no longer the active one gets every resident block back from
 * the worker's loop, also when only the cascade's is left (the LM off, the
 * weights already gone: audit-overlay-dialogue-4 N2). */
static void an_inactive_game_gets_the_cascade_block_back(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char w20[600];
    FILE *f;
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001;
    r.flags = 8u << VJO_GAME_OCR_CASCADE_SHIFT; /* ocr_lm = off, the cascade on */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(weights_live && cascade_live && !lm_live);
    TEST_CHECK(!drop_one(&resident) && !weights_live && cascade_live); /* say a later job's model failed to open */
    quit_on_delay = 1;
    inactive_since = now_wide() - INACTIVE_DROP_US; /* not the active game for that long (ocr-first) */
    worker_main(0, NULL); /* vjoOcrRegister: not the active game */
    quit_on_delay = 0;
    quitting = 0;
    TEST_CHECK(!cascade_live && cascade_blk.uid < 0 && !cascade_res.model && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* The step log of the job that first reads everything: the self-test's first
 * stage (the fallback forced, the 1 MB LM, the cascade re-reading the line, the
 * stable-text wait). Verified resident blocks keep no slot, so its lm, cascade and
 * norm steps (the LM's and the larger model's first read times) are all there,
 * and the job's own two releases are its last steps (audit-overlay-dialogue-4 B1). */
static void the_first_job_logs_the_lm_cascade_and_norm(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char lm2[600], w20[600];
    FILE *f;
    snprintf(lm2, sizeof(lm2), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_LM2_NAME);
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(lm2, "rb"))) { TEST_SKIP("no LM beside the release weights"); return; }
    fclose(f);
    if (!(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    frame_us = 16667;
    test_cascade_conf = 1001;
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK | 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT |
              15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && out.fallback == 1);
    unsigned lm = 0, casc = 0, norm = 0, settle = 0, frees = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        lm += out.step[i].id == VJO_GSTEP_LM && (out.step[i].bytes & VJO_GLM_READ) && out.step[i].kind == 2;
        casc += out.step[i].id == VJO_GSTEP_CASCADE && (out.step[i].bytes & VJO_GLM_READ) && out.step[i].arg == 1 &&
                (out.step[i].bytes & 0xFFFFu) > 0 && out.step[i].kind <= out.step[i].arg; /* W20's frames */
        norm += out.step[i].id == VJO_GSTEP_NORM;
        settle += out.step[i].id == VJO_GSTEP_SETTLE;
        frees += out.step[i].id == VJO_GSTEP_FREE;
    }
    TEST_CHECK(lm == 1 && casc == 1 && settle == 1 && frees == 2 && out.n_steps <= VJO_GAME_OCR_STEPS);
    TEST_CHECK(norm == !!out.text[0]);
    /* the fallback's check names what it chose: the 1 MB LM's tier and the cascade (8) */
    unsigned fb_check = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_CHECK && out.step[i].arg == 1) fb_check = out.step[i].kind;
    TEST_CHECK(fb_check == (2u | 8u));
    TEST_CHECK(out.n_steps >= 2 && out.step[out.n_steps - 1].id == VJO_GSTEP_FREE &&
               out.step[out.n_steps - 2].id == VJO_GSTEP_FREE);
    TEST_CHECK(weights_live && lm_live && cascade_live && blocks_held == 3 && blocks_kept() == 3);
    TEST_MSG("%u steps: lm %u cascade %u norm %u settle %u free %u", out.n_steps, lm, casc, norm, settle, frees);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live && !cascade_live);
    /* the same first job with the refinement (a PSP game's): its step too, all within the log */
    {
        VjoGameOcrRequest p = setup(); VjoGameOcrResult o;
        unsigned seen = 0;
        line();
        available = 64 * 1024 * 1024;
        frame_us = 16667;
        test_cascade_conf = 1001;
        snprintf(p.model, sizeof(p.model), "H15_w8.vocr");
        snprintf(p.fallback, sizeof(p.fallback), "FL10_w8.vocr");
        p.flags = r.flags | VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
        TEST_ASSERT(vjo_game_ocr_run_request(&p, &o) == 0 && o.fallback == 1);
        for (unsigned i = 0; i < o.n_steps; i++) {
            unsigned id = o.step[i].id;
            seen |= (id == VJO_GSTEP_REFINE) << 0 | (id == VJO_GSTEP_LM) << 1 | (id == VJO_GSTEP_CASCADE) << 2 |
                    (id == VJO_GSTEP_NORM && o.text[0]) << 3;
        }
        TEST_CHECK(seen == (o.text[0] ? 15u : 7u) && o.n_steps <= VJO_GAME_OCR_STEPS &&
                   o.step[o.n_steps - 1].id == VJO_GSTEP_FREE && o.step[o.n_steps - 2].id == VJO_GSTEP_FREE);
        TEST_MSG("with the refinement: %u steps, seen 0x%x", o.n_steps, seen);
        TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live && !cascade_live);
    }
}

/* API 20, the beam + LM decoder: with the LMs beside the models, a request
 * allowing tier 4 reads the 4 MB LM once (a resident USER block, like the
 * weights), decodes every line with it, records an LM step and the lines'
 * confidence, and keeps the greedy text after the shown one; the next job finds
 * it resident. Too little memory for it: a smaller tier; for none: greedy. A
 * request allowing no LM gives a resident one back. */
static void lm_jobs(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char lm4[600];
    FILE *f;
    snprintf(lm4, sizeof(lm4), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_LM4_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(lm4, "rb"))) { TEST_SKIP("no LM beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    const VjoGameOcrStep *lm = NULL;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_LM) lm = &out.step[i];
    TEST_ASSERT(lm != NULL);
    TEST_CHECK(lm->kind == 4 && lm->rc == 0 && (lm->bytes & VJO_GLM_READ) && (lm->bytes & 0xFFFF) == out.recognized);
    TEST_CHECK(lm_live && lm_res.lm == vjo_vocr_lm_tier(4) && lm_allocations == 1 && lm_opens == 1);
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_LM) &&
               (out.line[0].flags & VJO_VOCR_LINE_CONF) <= 1000);
    TEST_CHECK(vjo_text_raw(out.text, sizeof(out.text))[0] && !out.text[sizeof(out.text) - 1]);
    TEST_MSG("text '%s' greedy '%s' conf %u", out.text, vjo_text_raw(out.text, sizeof(out.text)),
             out.line[0].flags & VJO_VOCR_LINE_CONF);
    /* the next job: resident, not read again */
    unsigned reads = reads_count;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_LM) TEST_CHECK((out.step[i].bytes & VJO_GLM_RESIDENT) && out.step[i].kind == 4);
    TEST_CHECK(lm_allocations == 1 && reads_count - reads < 4); /* a stat, no 32 KiB reads of the LM */
    /* at most tier 2 allowed now: the resident 4 MB LM goes back first, the 1 MB one is read */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(lm_live && lm_res.lm == vjo_vocr_lm_tier(2) && lm_frees == 1 && lm_allocations == 2);
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_LM));
    TEST_CHECK(freed_before_the_workspace(&out, VJO_GITEM_LM));
    /* the LM switched off: the resident one goes, the job reads greedily (no LM step) */
    r.flags = 0;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!lm_live && !lm_res.lm && lm_frees == 2);
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_LM);
    TEST_CHECK(!(out.line[0].flags & VJO_VOCR_LINE_LM) && (out.line[0].flags & VJO_VOCR_LINE_CONF) == VJO_VOCR_LINE_CONF_NONE);
    /* room beside the resident weights for the 2 MB LM, not the 4 MB one: the 2 MB one */
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    available = (int)(live_bytes + GAME_RESERVE + 400000u + 2900000u);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(lm_res.lm && lm_res.lm->tier == 3 && lm_live);
    TEST_MSG("tier %u with %d B free", lm_res.lm ? lm_res.lm->tier : 0, available - (int)live_bytes);
    /* a fresh game with room for no LM at all: greedy, and the job still reads */
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !lm_live && !weights_live);
    r = setup();
    line();
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    /* FL10 greedy needs 806,912 B beside the reserve, with the 0.5 MB LM 1,368,064 B */
    available = (int)(GAME_RESERVE + 900000u);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!lm_live && !lm_res.lm && out.recognized >= 1 && !(out.line[0].flags & VJO_VOCR_LINE_LM));
    TEST_MSG("lm_live %d lm %p recognized %u flags 0x%x free %d need %u+%u", lm_live, (void *)lm_res.lm, out.recognized,
             out.line[0].flags, out.free_user, out.need_workspace, out.need_weights);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !lm_live);
}

/* ocr-first (docs/first-ocr.md): a job that finds no line reads the weights, the
 * LM and the cascade's model (as its memory check allowed) with their steps, so
 * the session's first job with text reads none of them while the user waits:
 * test.2's first text job read all three, 223 + 201 + 299 ms. That job finds them
 * resident; a later job without a line opens no file. */
static void a_job_without_lines_reads_the_weights_lm_and_cascade(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char lm2[600], w20[600];
    FILE *f;
    snprintf(lm2, sizeof(lm2), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_LM2_NAME);
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(lm2, "rb"))) { TEST_SKIP("no LM beside the release weights"); return; }
    fclose(f);
    if (!(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001; /* every line read again by the cascade */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT; /* an empty region */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!out.lines && !out.text[0] && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(weights_live && lm_live && cascade_live && lm_res.lm == vjo_vocr_lm_tier(2) && cascade_res.model);
    unsigned load = 0, lm = 0, casc = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        load += out.step[i].id == VJO_GSTEP_LOAD && out.step[i].arg == 1 && out.step[i].kind == VJO_GAME_OCR_WEIGHTS_READ;
        lm += out.step[i].id == VJO_GSTEP_LM && (out.step[i].bytes & VJO_GLM_READ) && !(out.step[i].bytes & 0xFFFFu);
        casc += out.step[i].id == VJO_GSTEP_CASCADE && (out.step[i].bytes & VJO_GLM_READ) && !out.step[i].arg;
    }
    TEST_CHECK(load == 1 && lm == 1 && casc == 1 && out.n_steps <= VJO_GAME_OCR_STEPS);
    TEST_CHECK(out.step[out.n_steps - 1].id == VJO_GSTEP_FREE); /* the job's own releases are still logged */
    unsigned weights = weight_allocations, lms = lm_allocations, cascades = cascade_allocations;
    line(); /* text: everything resident, nothing read again */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && out.recognized >= 1 && out.text[0]);
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT && weight_allocations == weights);
    TEST_CHECK(lm_allocations == lms && cascade_allocations == cascades);
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_LM) TEST_CHECK(out.step[i].bytes & VJO_GLM_RESIDENT);
        if (out.step[i].id == VJO_GSTEP_CASCADE) TEST_CHECK(out.step[i].bytes & VJO_GLM_RESIDENT);
    }
    memset(pixels, 0, sizeof(pixels)); /* no line again: no file opened */
    unsigned opened = lm_opens + cascade_opens, reads = reads_count;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && !out.lines);
    TEST_CHECK(lm_opens + cascade_opens == opened && reads_count == reads && out.weights == VJO_GAME_OCR_WEIGHTS_NONE);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live && !cascade_live);
    /* The model file missing: the job without a line still succeeds and says why; the LM and
     * the cascade's model are not read for weights that cannot run; a text job then fails as before. */
    r = setup();
    available = 64 * 1024 * 1024;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    snprintf(model_path, sizeof(model_path), "%s/no-such-model.vocr", r.model_dir);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && !out.lines && !out.text[0]);
    TEST_CHECK(out.io_error == -123 && !weights_live && !lm_live && !cascade_live && !lm_opens && !cascade_opens);
    line();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && !weights_live && !lm_live);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
    /* Weights of the wrong size: nothing else is read either (the warm-up stops at its first failure). */
    r = setup();
    available = 64 * 1024 * 1024;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    virtual_file = 1;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && !out.lines);
    TEST_CHECK(!weights_live && !lm_live && !cascade_live && !lm_opens && !cascade_opens);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
    /* An LM of the wrong size: the weights stay read and verified, the cascade's model is not read. */
    r = setup();
    available = 64 * 1024 * 1024;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    lm_bad_size = 1;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && !out.lines);
    TEST_CHECK(weights_live && resident.model && !lm_live && !lm_res.lm && lm_opens == 1 && !cascade_opens);
    { /* its LM step says why, in the job that tried (audit-overlay-first-ocr nit) */
        const VjoGameOcrStep *lms = NULL;
        for (unsigned i = 0; i < out.n_steps; i++)
            if (out.step[i].id == VJO_GSTEP_LM) lms = &out.step[i];
        TEST_CHECK(lms && lms->rc == VJO_E_OCR_MODEL && !(lms->bytes & (VJO_GLM_READ | VJO_GLM_RESIDENT)));
    }
    TEST_CHECK(!vjo_game_ocr_worker_stop());
    /* A cascade model of the wrong size: the weights and the LM stay read, its CASCADE step says why. */
    r = setup();
    available = 64 * 1024 * 1024;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    cascade_bad_size = 1;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && !out.lines);
    TEST_CHECK(weights_live && lm_live && !cascade_live && cascade_opens == 1);
    {
        const VjoGameOcrStep *cs = NULL;
        for (unsigned i = 0; i < out.n_steps; i++)
            if (out.step[i].id == VJO_GSTEP_CASCADE) cs = &out.step[i];
        TEST_CHECK(cs && cs->rc == VJO_E_OCR_MODEL && !(cs->bytes & (VJO_GLM_READ | VJO_GLM_RESIDENT)));
        TEST_MSG("cascade step %s, rc %d", cs ? "present" : "missing", cs ? (int)cs->rc : 0);
    }
    TEST_CHECK(!vjo_game_ocr_worker_stop());
    /* A job that failed (its screen could not be read) reads nothing for later jobs. */
    r = setup();
    available = 64 * 1024 * 1024;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    rows_error = VJO_ERR_TIMEOUT;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !out.lines);
    TEST_CHECK(!weights_live && !lm_live && !cascade_live && !lm_opens && !cascade_opens);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
    /* Jobs that read text read only what their lines need: no doubtful line, no cascade model,
     * whether the weights were read for the job or resident. */
    r = setup();
    available = 64 * 1024 * 1024;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    test_cascade_conf = 1; /* no line under 0.001 */
    line();
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && out.recognized >= 1);
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && lm_live && !cascade_live && !cascade_opens);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && out.recognized >= 1);
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT && !cascade_live && !cascade_opens);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* The TEAR step says when the job's last try began (bytes: us since the job's
 * start): its tears happened before that, the retry ran from there to the end
 * (test.2's "tear re-read 2.3 s" was its us, the job's end, misread). */
static void the_tear_step_says_when_the_retry_began(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    line();
    frame_us = 16667;
    change_at_rows = 2; /* during the finder's second pass of the first try */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    const VjoGameOcrStep *tear = NULL;
    uint32_t end = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_TEAR) tear = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_FREE && out.step[i].us > end) end = out.step[i].us;
    }
    TEST_ASSERT(tear != NULL);
    TEST_CHECK(tear->rc >= 2 && tear->bytes > 0 && tear->bytes < tear->us && tear->bytes <= end);
    TEST_CHECK(tear->bytes >= 2u * 16667u); /* after the first try's strips */
    TEST_MSG("retry began at %u us, the tear step at %u us, the job's end %u us", tear->bytes, tear->us, end);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* API 20, the confidence cascade: a request with a time ceiling and room for
 * it reads its doubtful lines again with the larger pinned model (forced here:
 * every line), whose weights become a resident USER block; the next job finds
 * them resident; a request without the ceiling gives the block back; a game
 * with room for the LM but not the cascade runs without it. */
static void cascade_jobs(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char w20[600];
    FILE *f;
    snprintf(w20, sizeof(w20), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_CASCADE_NAME);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(w20, "rb"))) { TEST_SKIP("no VOCR4-W20 beside the release weights"); return; }
    fclose(f);
    line();
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001;
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    const VjoGameOcrStep *c = NULL, *check = NULL;
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_CASCADE) c = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_CHECK && !check) check = &out.step[i];
    }
    TEST_ASSERT(c != NULL && check != NULL);
    TEST_CHECK((check->kind & 8u) && (check->kind & 7u) == 2u); /* the check passed with the cascade and the 1 MB LM */
    TEST_CHECK(c->rc == 0 && c->arg == out.recognized && (c->bytes & VJO_GLM_READ) && !((c->bytes >> 16) & 0xFFu));
    TEST_CHECK(cascade_live && cascade_res.model == vjo_vocr_model(VJO_VOCR_CASCADE_NAME) && cascade_allocations == 1 &&
               cascade_opens == 1 && cascade_block == PAGE_ROUND(VJO_VOCR_CASCADE_BYTES));
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    TEST_CHECK(!memory_live && !capture_live && blocks_held == 3u); /* weights, LM, cascade */
    TEST_MSG("text '%s', %u steps, cascade step arg %u bytes 0x%08X", out.text, out.n_steps, c->arg, (unsigned)c->bytes);
    /* the next job: resident */
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    c = NULL;
    for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_CASCADE) c = &out.step[i];
    TEST_CHECK(c && (c->bytes & VJO_GLM_RESIDENT) && cascade_allocations == 1 && cascade_opens == 2);
    /* no ceiling in the request: the block goes back, no cascade step */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT;
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && !cascade_res.model && cascade_frees == 1);
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_CASCADE);
    TEST_CHECK(!(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    /* room for the model and the LM, not for the cascade too: the LM alone */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    available = (int)(live_bytes + GAME_RESERVE + 500000u);
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && lm_live && !(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    for (unsigned i = 0; i < out.n_steps; i++) {
        TEST_CHECK(out.step[i].id != VJO_GSTEP_CASCADE);
        if (out.step[i].id == VJO_GSTEP_CHECK) TEST_CHECK(!(out.step[i].kind & 8u));
    }
    /* resident again, then a game with no room for the job at all: refused, and every
     * resident block (weights, LM, the cascade's) goes back to the game */
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    available = 64 * 1024 * 1024;
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(cascade_live && lm_live && weights_live);
    /* everything resident, and too little room for the cascade with any LM tier (counting what
     * each would release) though plenty for the LM alone: the LM, and the cascade's block goes
     * back to the game */
    {
        VjoGameOcrResult o;
        int64_t least = INT64_MAX;
        memset(&o, 0, sizeof(o));
        o.free_user = INT32_MAX;
        for (unsigned t = 1; t <= 2; t++) {
            check_memory(&o, vjo_vocr_model(r.model), vjo_vocr_job_bytes(r.model, r.width, r.height),
                         vjo_vocr_capture_bytes(r.width, r.height), vjo_vocr_lm_tier(t), cascade_res.model, 0);
            int64_t need = (int64_t)o.need_workspace + o.need_weights - o.release_bytes;
            if (need < least) least = need;
        }
        available = (int)(live_bytes + GAME_RESERVE + least - 4096);
    }
    unsigned frees_before = cascade_frees;
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && cascade_frees == frees_before + 1 && !cascade_res.model && lm_live && weights_live);
    TEST_CHECK(lm_res.lm == vjo_vocr_lm_tier(2) && freed_before_the_workspace(&out, VJO_GITEM_CASCADE));
    TEST_CHECK(out.recognized >= 1 && !(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    /* resident again */
    available = 64 * 1024 * 1024;
    vjo_game_ocr_cache_clear(); /* the same screen is recognized again, not cached */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(cascade_live && lm_live && weights_live);
    /* free memory 48,576 B under the reserve (1,000,000 B beside #280's 1 MiB, 475,712 B beside the 512 KiB test
     * draft's): the job still reads, in room the LM's and the cascade's blocks give back (under the 4 MiB reserve
     * before #280, it was refused), and leaves the game at least the reserve free beside what stays resident */
    available = (int)(live_bytes + GAME_RESERVE - 48576u);
    vjo_game_ocr_cache_clear();
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.recognized >= 1 && weights_live && resident.model && (!cascade_live || !lm_live));
    TEST_CHECK(available - (int)live_bytes >= (int)GAME_RESERVE);
    TEST_MSG("free %d B: cascade %d, LM %d (tier %u), %u B resident", available - (int)live_bytes, cascade_live, lm_live,
             lm_res.lm ? lm_res.lm->tier : 0u, live_bytes);
    /* no LM or cascade asked for (they go), then no room for the job beside the reserve with only its weights
     * resident: refused, and the weights go back to the game too */
    r.flags = 0;
    available = 64 * 1024 * 1024;
    vjo_game_ocr_cache_clear();
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(!cascade_live && !lm_live && weights_live);
    {
        VjoGameOcrResult o;
        memset(&o, 0, sizeof(o));
        o.free_user = INT32_MAX;
        check_memory(&o, vjo_vocr_model(r.model), vjo_vocr_job_bytes(r.model, r.width, r.height),
                     vjo_vocr_capture_bytes(r.width, r.height), NULL, NULL, 0);
        TEST_ASSERT(!o.need_weights && !o.release_bytes); /* the weights resident, nothing else held */
        available = (int)(live_bytes + GAME_RESERVE + o.need_workspace - 4096u);
    }
    vjo_game_ocr_cache_clear();
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
    TEST_CHECK(!cascade_live && !lm_live && !weights_live && !cascade_res.model && !lm_res.lm && !resident.model);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !cascade_live && !lm_live && !weights_live);
}

/* Glyph-like strokes lh rows tall across the frame (as test_vocr_policy.c's). */
static void strokes_at(unsigned y0, unsigned lh)
{
    unsigned pitch = 6u + (lh > 12u ? lh / 2u : 6u);
    for (unsigned y = y0; y < y0 + lh; y++)
        for (unsigned x = 10; x < 190; x++) {
            unsigned cx = (x - 10) % pitch, cy = y - y0;
            if (cx < pitch * 3u / 4u && (cx % 4u < 2u || cy % 7u < 2u || (cx + cy) % 9u == 0u))
                memset(pixels + ((size_t)y * 200 + x) * 4, 235, 3);
        }
}

/* The REFINE step of a job's log, or NULL. */
static const VjoGameOcrStep *refine_step(const VjoGameOcrResult *out)
{
    for (unsigned i = 0; i < out->n_steps; i++)
        if (out->step[i].id == VJO_GSTEP_REFINE) return &out->step[i];
    return NULL;
}

/* API 20, the finder's boxes refined: on, or in PSP games only (the worker's own
 * title: NPXS10028 or a PSPEMU* bubble). The job's workspace holds the arena,
 * and a REFINE step reports it; mode 3 is refused before anything is allocated. */
static void refine_modes(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    static const struct { const char *title; int psp; } titles[] = {
        {"NPXS10028", 1}, {"PSPEMU005", 1}, {"PSPEMUCFW", 1}, {"PSPEMU", 1}, {"PCSG00415", 0}, {"PSPEM", 0},
        {"XPSPEMU01", 0}, {"pspemu001", 0}, {"NPXS10028X", 0}, {"NPXS1002", 0}, {"NPXS10027", 0}, {"", 0}};
    for (unsigned i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        TEST_CHECK_(vjo_game_is_psp(titles[i].title) == titles[i].psp, "%s", titles[i].title);
    line();
    struct { unsigned mode; int psp; int refined; } cases[] = {
        {0, 1, 0}, {VJO_GAME_OCR_REFINE_ON, 0, 1}, {VJO_GAME_OCR_REFINE_PSP, 0, 0}, {VJO_GAME_OCR_REFINE_PSP, 1, 1}};
    unsigned plain_block = 0;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const VjoGameOcrStep *rs;
        r = setup();
        line();
        psp_title = cases[c].psp;
        r.flags = cases[c].mode << VJO_GAME_OCR_REFINE_SHIFT;
        int rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK(rc == 0 || rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
        rs = refine_step(&out);
        TEST_CHECK_((rs != NULL) == cases[c].refined, "mode %u psp %d", cases[c].mode, cases[c].psp);
        if (rs) TEST_CHECK(rs->kind == cases[c].mode && rs->rc == 0);
        /* the boxes are refined as they are read, after the weights: without the release
         * files (CI) the job ends at the model, so only a job that read its lines counts them */
        if (rs && rc == 0)
            TEST_CHECK((rs->bytes >> 16) == out.lines && rs->arg >= out.lines && rs->sum > 0);
        else if (rs)
            TEST_CHECK(!(rs->bytes >> 16) && !rs->arg && !rs->sum);
        if (!c) plain_block = job_block;
        else if (cases[c].refined)
            TEST_CHECK(job_block == PAGE_ROUND(vjo_vocr_job_bytes(r.model, r.width, r.height) +
                                               vjo_vocr_refine_bytes(r.width, r.height)) && job_block >= plain_block);
        TEST_MSG("mode %u psp %d: workspace %u B (plain %u B)", cases[c].mode, cases[c].psp, job_block, plain_block);
    }
    /* a job that finds no line logs no refine step */
    r = setup();
    r.flags = VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.lines && !refine_step(&out));
    /* mode 3, also in a probe: refused before anything, as unknown bits are */
    r = setup();
    allocations = 0;
    r.flags = 3u << VJO_GAME_OCR_REFINE_SHIFT;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
    r.flags |= VJO_GAME_OCR_F_PROBE;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps && !capture_allocs);
    psp_title = 0;
}

/* The refine step's fields, from a job that reads its lines (the release weights):
 * pairs of text rows set one row apart, each pair one finder box that the refinement
 * splits, nine of them in a 200 x 272 region: seven split, the last two not (their
 * split would pass 16 lines), 16 lines, and the rows the refinement read: each box's
 * 31 rows once (its grey pixels fit beside its mask), the two capped boxes' again
 * for their second call, 9 x 31 + 2 x 31. */
static void the_refine_step_carries_its_counts(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    char path[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, r.model);
    if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(path, "rb"))) { TEST_SKIP("no release weights"); return; }
    fclose(f);
    frame_h = r.fb_height = r.height = 272;
    for (unsigned k = 0; k < 9; k++) {
        strokes_at(4 + k * 29, 11);
        strokes_at(4 + k * 29 + 12, 11);
    }
    r.flags = VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    const VjoGameOcrStep *rs = refine_step(&out);
    TEST_ASSERT(rs != NULL);
    TEST_CHECK(out.lines == 9 && rs->rc == 0 && rs->arg == 16 && (rs->bytes >> 16) == 9 &&
               ((rs->bytes >> 8) & 0xFFu) == 2 && (rs->bytes & 0xFFu) == 7 && rs->sum == 11u * 31u && rs->us > 0);
    TEST_MSG("%u boxes: refine arg %u bytes 0x%08X sum %u", out.lines, (unsigned)rs->arg, (unsigned)rs->bytes,
             (unsigned)rs->sum);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}

/* API 31: the text source as module_start reads it for the hooks (text_source): the kernel's word, a
 * VJO_TEXT_SOURCE_* in its low byte with or without VJO_TEXT_SOURCE_NO_SITES (hook_sites = off, audit-hooks
 * F3), passed whole to vjo_hooks_start; a low byte this build does not know, or any other bit, as OCR (no
 * hooks); OCR without asking from another API's kernel; and only in a game that keeps the worker: none of
 * the processes module_start unloads from asks. */
static void module_start_reads_the_text_source(void)
{
    const unsigned no_sites = VJO_TEXT_SOURCE_NO_SITES;
    static const struct { int kernel; unsigned seen; } cases[] = {
        {(int)VJO_TEXT_SOURCE_OCR, VJO_TEXT_SOURCE_OCR}, {(int)VJO_TEXT_SOURCE_AUTO, VJO_TEXT_SOURCE_AUTO},
        {(int)VJO_TEXT_SOURCE_HOOKS, VJO_TEXT_SOURCE_HOOKS}, {(int)VJO_TEXT_SOURCE_COUNT, VJO_TEXT_SOURCE_OCR},
        {0x7FFFFFFF, VJO_TEXT_SOURCE_OCR}, {VJO_ERR_PERM, VJO_TEXT_SOURCE_OCR},
        /* the word with hook_sites = off: kept whole */
        {(int)(VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES), VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES},
        {(int)(VJO_TEXT_SOURCE_HOOKS | VJO_TEXT_SOURCE_NO_SITES), VJO_TEXT_SOURCE_HOOKS | VJO_TEXT_SOURCE_NO_SITES},
        {(int)(VJO_TEXT_SOURCE_OCR | VJO_TEXT_SOURCE_NO_SITES), VJO_TEXT_SOURCE_OCR | VJO_TEXT_SOURCE_NO_SITES},
        /* a low byte out of range, with the flag or above it; any other bit: OCR */
        {(int)(VJO_TEXT_SOURCE_COUNT | VJO_TEXT_SOURCE_NO_SITES), VJO_TEXT_SOURCE_OCR},
        {(int)(0xFFu | VJO_TEXT_SOURCE_NO_SITES), VJO_TEXT_SOURCE_OCR},
        {(int)(VJO_TEXT_SOURCE_AUTO | 0x200u), VJO_TEXT_SOURCE_OCR},
        {(int)(VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES | 0x200u), VJO_TEXT_SOURCE_OCR},
        {(int)(VJO_TEXT_SOURCE_HOOKS | 0x10000u), VJO_TEXT_SOURCE_OCR},
        {(int)(VJO_TEXT_SOURCE_AUTO | 0x40000000u), VJO_TEXT_SOURCE_OCR},
        {(int)(VJO_TEXT_SOURCE_AUTO | 0x80000000u), VJO_TEXT_SOURCE_OCR}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vjo_test_text_source = cases[i].kernel;
        vjo_test_text_source_calls = 0;
        unsigned m = text_source();
        TEST_CHECK_(m == cases[i].seen && vjo_test_text_source_calls == 1, "kernel %d: %u", cases[i].kernel, m);
    }
    text_source_api = VJO_API_VERSION - 1; /* another API's kernel: not asked */
    vjo_test_text_source = (int)VJO_TEXT_SOURCE_HOOKS;
    vjo_test_text_source_calls = 0;
    TEST_CHECK(text_source() == VJO_TEXT_SOURCE_OCR && !vjo_test_text_source_calls);
    text_source_api = VJO_API_VERSION;
    /* module_start: a candidate game asks, and passes the whole word to vjo_hooks_start */
    setup();
    strcpy(own_title, "PCSG00415");
    vjo_test_text_source = (int)(VJO_TEXT_SOURCE_AUTO | no_sites);
    vjo_test_hooks_starts = 0;
    vjo_test_hooks_word = 0;
    TEST_CHECK(!module_start(0, NULL) && thread_creates == 1 && vjo_test_text_source_calls >= 1);
    TEST_CHECK(vjo_test_hooks_starts == 1 && vjo_test_hooks_word == (VJO_TEXT_SOURCE_AUTO | no_sites));
    TEST_CHECK(!module_stop(0, NULL));
    setup();
    vjo_test_text_source = (int)(VJO_TEXT_SOURCE_HOOKS | 0x200u); /* an unknown flag: OCR */
    vjo_test_hooks_starts = 0;
    TEST_CHECK(!module_start(0, NULL) && vjo_test_hooks_starts == 1 && vjo_test_hooks_word == VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(!module_stop(0, NULL));
    vjo_test_text_source = (int)VJO_TEXT_SOURCE_AUTO;
    setup();
    vjo_test_text_source_calls = 0;
    vjo_test_hooks_starts = 0;
    strcpy(own_title, "NPXS10015"); /* a system app */
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !vjo_test_text_source_calls);
    safe_boot_mode = VJO_BOOT_SAFE; /* a safe boot: before any title is read */
    strcpy(own_title, "PCSG00415");
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !vjo_test_text_source_calls);
    safe_boot_mode = VJO_BOOT_NORMAL;
    vjo_test_another = 1; /* another copy runs in this game */
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !vjo_test_text_source_calls);
    TEST_CHECK(!vjo_test_hooks_starts); /* none of them installs anything */
    vjo_test_another = 0;
    vjo_test_text_source = 0;
}

/* module_start records whether the game is a PSP game: in psp mode its jobs
 * refine. The PSP emulator's process (its ScePspemu module, whatever its
 * title: an Adrenaline bubble named ABCD12345), Adrenaline's NPXS10028, a
 * PSPEMU* bubble or one named after its disc ID; CLANNAD's do not. */
static void module_start_sets_the_psp_title(void)
{
    static const struct { const char *title; int pspemu, psp; } games[] = {
        {"NPXS10028", 0, 1}, {"PSPEMU001", 0, 1}, {"ULJS00378", 0, 1}, {"ABCD12345", 1, 1}, {"ABCD12345", 0, 0},
        {"PCSG00415", 0, 0}};
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        psp_title = !games[i].psp; /* only module_start can make it right */
        vjo_test_pspemu = games[i].pspemu;
        strcpy(own_title, games[i].title);
        TEST_CHECK_(!module_start(0, NULL) && thread_creates == 1 && psp_title == games[i].psp, "%s", games[i].title);
        line();
        r.flags = VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT;
        int rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK(rc == 0 || rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
        TEST_CHECK_((refine_step(&out) != NULL) == games[i].psp, "%s", games[i].title);
        TEST_CHECK(!module_stop(0, NULL));
    }
    psp_title = vjo_test_pspemu = 0;
}

/* The self-test's fallback stage: the memory check refuses the model on
 * purpose and the fallback runs. */
static void forced_fallback_runs_the_fallback(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.fallback == 1);
    int forced = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_CHECK && out.step[i].arg == 1 && out.step[i].sum == 1) forced = 1;
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_FALLBACK);
    TEST_CHECK(forced);
    /* with the refinement: the fallback's workspace holds its arena too, and its boxes are refined */
    r = setup();
    line();
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK | VJO_GAME_OCR_REFINE_ON << VJO_GAME_OCR_REFINE_SHIFT;
    int rc = vjo_game_ocr_run_request(&r, &out);
    const VjoGameOcrStep *rs = refine_step(&out);
    TEST_CHECK(rc == 0 || rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.fallback == 1 && rs && rs->rc == 0 &&
               job_block == PAGE_ROUND(vjo_vocr_job_bytes(r.fallback, r.width, r.height) +
                                       vjo_vocr_refine_bytes(r.width, r.height)));
    if (rs && !rc) TEST_CHECK(out.lines == 1 && (rs->bytes >> 16) == 1 && rs->arg == 1);
    TEST_MSG("rc %d, workspace %u B, refine rc %d", rc, job_block, rs ? (int)rs->rc : 1);
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}
/* ---- API 21: the model tiers ---- */

/* What a bare job of tier t needs free in the game (no LM, no cascade, as the
 * check's last try): its page-rounded workspace, capture block and weights, and
 * its tier's reserve (include/vjo_ledger_budget.h). */
static uint32_t tier_need(const VjoGameOcrRequest *r, unsigned t, size_t extra)
{
    VjoGameOcrResult o;
    const VjoVocrModel *m = vjo_vocr_model(tier_names[t - 1u]);
    memset(&o, 0, sizeof(o));
    o.free_user = INT32_MAX;
    check_memory(&o, m, vjo_vocr_job_bytes(m->name, r->width, r->height) + extra,
                 vjo_vocr_capture_bytes(r->width, r->height), NULL, NULL, t);
    return o.need_workspace + o.need_weights + o.reserve_bytes;
}
/* The same with LM lm beside the model (the check's with_lm choice when that LM fits). */
static uint32_t tier_lm_need(const VjoGameOcrRequest *r, unsigned t, const VjoVocrLm *lm)
{
    VjoGameOcrResult o;
    const VjoVocrModel *m = vjo_vocr_model(tier_names[t - 1u]);
    memset(&o, 0, sizeof(o));
    o.free_user = INT32_MAX;
    check_memory(&o, m, vjo_vocr_job_bytes(m->name, r->width, r->height), vjo_vocr_capture_bytes(r->width, r->height),
                 lm, NULL, t);
    return o.need_workspace + o.need_weights + o.reserve_bytes;
}
/* The same with LM lm and extra job bytes (the PSP refinement's) beside the model. */
static uint32_t tier_lm_extra_need(const VjoGameOcrRequest *r, unsigned t, const VjoVocrLm *lm, size_t extra)
{
    VjoGameOcrResult o;
    const VjoVocrModel *m = vjo_vocr_model(tier_names[t - 1u]);
    memset(&o, 0, sizeof(o));
    o.free_user = INT32_MAX;
    check_memory(&o, m, vjo_vocr_job_bytes(m->name, r->width, r->height) + extra,
                 vjo_vocr_capture_bytes(r->width, r->height), lm, NULL, t);
    return o.need_workspace + o.need_weights + o.reserve_bytes;
}
static unsigned count_steps(const VjoGameOcrResult *out, unsigned id)
{
    unsigned n = 0;
    for (unsigned i = 0; i < out->n_steps && i < VJO_GAME_OCR_STEPS; i++) n += out->step[i].id == id;
    return n;
}
/* Regression for あおかな's 3 MiB report: a real worker request reaches the
 * largest fitting tier with the fixed margin. Blank captures make this
 * independent of private model files; this checks budgeting, not inference.
 * The 512 KiB test draft: with 3 MiB free Large chosen runs Normal, as beside
 * #280's 1 MiB; with 2 MiB (アマツツミ's) Normal too, where beside 1 MiB only
 * Small fitted; with 1.5 MiB (a console that reports exact bytes) Small, where
 * beside 1 MiB none did. */
static void three_mib_free_can_start_ocr(void)
{
    static const unsigned masks[] = {7u, 3u, 1u};
    static const uint32_t frees[] = {3u * 1024u * 1024u, 2u * 1024u * 1024u, 1536u * 1024u};
    static const unsigned ran_at[3][3] = {{2u, 2u, 1u}, {2u, 2u, 1u}, {1u, 1u, 1u}}; /* [free][mask]: the size run */
    TEST_ASSERT(VJO_GAME_RESERVE == 512u * 1024u);
    for (unsigned f = 0; f < sizeof(frees) / sizeof(frees[0]); f++)
        for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
            VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
            unsigned top = i == 0 ? 3u : i == 1 ? 2u : 1u, ran = ran_at[f][i];
            r.tiers = masks[i];
            snprintf(r.model, sizeof(r.model), "%s", tier_names[top - 1u]);
            /* the size run is the largest the request allows whose need fits */
            for (unsigned t = top; t > ran; t--) TEST_ASSERT(tier_need(&r, t, 0) > frees[f]);
            TEST_ASSERT(tier_need(&r, ran, 0) <= frees[f]);
            available = (int)frees[f];
            TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == VJO_OK);
            TEST_CHECK_(out.reserve_bytes == VJO_GAME_RESERVE && out.fallback == top - ran,
                        "%u B free, sizes up to %u: reserve %u, fallback %u", frees[f], top, (unsigned)out.reserve_bytes,
                        (unsigned)out.fallback);
            TEST_CHECK(out.need_workspace + out.need_weights + out.reserve_bytes <= frees[f]);
            TEST_CHECK(allocations == 1 && !memory_live && !capture_live);
            TEST_CHECK(tier_opens[ran] == 1);
            TEST_CHECK(!drop_resident());
        }
    /* The margin is still enforced at the boundary; an allocation that fails
     * after a successful preflight still returns OOM and releases the job. */
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.tiers = 1u;
    available = (int)tier_need(&r, 1, 0) - 4096;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !allocations && !capture_allocs);
    r = setup(); r.tiers = 1u;
    available = 3 * 1024 * 1024; alloc_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !out.text[0]);
    TEST_CHECK(!memory_live && !capture_live && !weights_live);
}

/* VOCR6's three sizes beside the 512 KiB test draft's fixed reserve (PR #280's 1 MiB on main), through the game
 * worker's own memory check at the full screen (960x544, the case tools/model_tiers.json prices): every size keeps
 * 512 KiB, with its cascade (the next size up) and with each LM tier too; a first job of a size needs its table's
 * figure (core/vocr_models.c vjo_vocr_tier_need, model_tiers.json "need_bytes": 1,544,192, 1,970,176 and 3,338,240 B,
 * 2,068,480, 2,494,464 and 3,862,528 B beside 1 MiB; a 64-bit host's runtime structs may add a page), fits with exactly
 * that free and not with a page less. In whole MiB, as the console reports the game's free memory (#324): with 3 MiB
 * (あおかな's 3,072 KiB) Large does not fit and Normal does; with 2 MiB (アマツツミ's) Normal passes the check too
 * (beside 1 MiB only Small did); with 1 MiB none does. In exact bytes, 1.5 MiB passes Small's check and not Normal's
 * (beside 1 MiB, not even Small's). The hook-only pass keeps its own 1 MiB (#273's), unchanged. No model files: the
 * check reads none. */
static void vocr6_sizes_beside_the_fixed_reserve(void)
{
    const uint32_t mib = 1024u * 1024u, res = 512u * 1024u;
    static const uint32_t table_need[VJO_VOCR_TIERS + 1u] = {0, 1544192u, 1970176u, 3338240u};
    VjoGameOcrRequest r = setup();
    r.width = r.fb_width = 960;
    r.height = r.fb_height = 544;
    const size_t capture = vjo_vocr_capture_bytes(r.width, r.height);
    uint32_t need[VJO_VOCR_TIERS + 1u] = {0};
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
        const VjoVocrModel *m = vjo_vocr_model(tier_names[t - 1u]);
        TEST_ASSERT(m != NULL);
        TEST_CHECK_(!strcmp(m->name, test_tier_pins[t - 1u].name) && m->bytes == test_tier_pins[t - 1u].bytes,
                    "size %u: %s, %u B", t, m->name, m->bytes);
        const size_t job = vjo_vocr_job_bytes(m->name, r.width, r.height);
        const uint32_t table = vjo_vocr_tier_need(t);
        TEST_CHECK_(table == table_need[t] && vjo_vocr_tier_info(t)->reserve == res &&
                        table == res + vjo_vocr_model_info(m->name)->job_bytes,
                    "size %u: the table's need %u B", t, table);
        need[t] = tier_need(&r, t, 0);
        TEST_CHECK_(sizeof(void *) == 4 ? need[t] == table : need[t] >= table && need[t] <= table + 4096u,
                    "size %u: the worker's check needs %u B, the table %u B", t, need[t], table);
        VjoGameOcrResult o;
        memset(&o, 0, sizeof(o));
        o.free_user = (int32_t)need[t];
        TEST_CHECK(check_memory(&o, m, job, capture, NULL, NULL, t) && o.reserve_bytes == res);
        TEST_CHECK(o.need_workspace + o.need_weights + o.reserve_bytes == need[t] && !o.release_bytes);
        o.free_user = (int32_t)(need[t] - 4096u);
        TEST_CHECK(!check_memory(&o, m, job, capture, NULL, NULL, t));
        /* with its cascade, the next size's model (Large has none), and each LM tier: the same 512 KiB */
        const VjoVocrModel *casc = t < VJO_VOCR_TIERS ? vjo_vocr_model(tier_names[t]) : NULL;
        for (unsigned l = 0; l <= VJO_VOCR_LM_TIERS; l++)
            for (unsigned c = 0; c < 2u; c++) {
                o.free_user = INT32_MAX;
                (void)check_memory(&o, m, job, capture, l ? vjo_vocr_lm_tier(l) : NULL, c ? casc : NULL, t);
                TEST_CHECK_(o.reserve_bytes == res, "size %u, LM tier %u, cascade %u: reserve %u B", t, l, c,
                            (unsigned)o.reserve_bytes);
            }
        TEST_CHECK(check_reserve(t, NULL) == res && check_reserve(t, casc) == res);
    }
    TEST_CHECK(check_reserve(0, NULL) == res && GAME_RESERVE == res && VJO_GAME_RESERVE_HOOK_ONLY == mib);
    TEST_CHECK(need[1] < need[2] && need[2] < need[3]);
    TEST_CHECK_(need[3] > 3u * mib && need[2] <= 3u * mib, "3 MiB: Large needs %u B, Normal %u B", need[3], need[2]);
    TEST_CHECK_(need[2] <= 2u * mib, "2 MiB: Normal needs %u B", need[2]);
    TEST_CHECK_(need[2] > 3u * mib / 2u && need[1] <= 3u * mib / 2u, "1.5 MiB: Normal needs %u B, Small %u B", need[2],
                need[1]);
    TEST_CHECK_(need[1] > mib, "1 MiB: Small needs %u B", need[1]);
    /* beside #280's 1 MiB each need is 512 KiB more: Normal over 2 MiB, Small over 1.5 MiB */
    TEST_CHECK(need[2] + (mib - res) > 2u * mib && need[1] + (mib - res) > 3u * mib / 2u);
}

static unsigned check_steps(const VjoGameOcrResult *out, const VjoGameOcrStep **steps)
{
    unsigned n = 0;
    for (unsigned i = 0; i < out->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out->step[i].id == VJO_GSTEP_CHECK) steps[n++] = &out->step[i];
    return n;
}

/* #324's construction in whole MiB (below): the free memory at which Normal passes its check at F, the least whole
 * MiB its need fits, its weights are refused once its blocks are taken (the report then reads F - 1 MiB), and Small,
 * checked again, reads with its weights: F plus Small's own blocks, which are smaller than Normal's, so that once
 * they are taken the report still reads F. Beside #280's 1 MiB, F plus a page was enough (F - 1 MiB less 1 MiB had
 * room for Small's weights); beside the 512 KiB test draft's reserve F is 2 MiB, and F - 1 MiB less 512 KiB has no
 * room for Small's 823,296 B either. */
static uint32_t normal_refused_small_reads(const VjoGameOcrRequest *r)
{
    const uint32_t mib = 1024u * 1024u;
    const uint32_t small = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER1_NAME)->bytes);
    const uint32_t normal = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER2_NAME)->bytes);
    const uint32_t need_n = tier_need(r, 2, 0), need_s = tier_need(r, 1, 0);
    const uint32_t blocks_n = need_n - normal - VJO_GAME_RESERVE_NORMAL, blocks_s = need_s - small - VJO_GAME_RESERVE_SMALL;
    const uint32_t f = (need_n + mib - 1u) / mib * mib, free_bytes = f + blocks_s;
    TEST_ASSERT(blocks_s < blocks_n && blocks_n < mib);
    TEST_ASSERT(free_bytes / mib * mib == f && need_s <= f);                                 /* both checks read F */
    TEST_ASSERT((free_bytes - blocks_n) / mib * mib < VJO_GAME_RESERVE_NORMAL + normal);     /* Normal's weights */
    TEST_ASSERT((free_bytes - blocks_s) / mib * mib >= VJO_GAME_RESERVE_SMALL + small);      /* Small's weights */
    return free_bytes;
}

/* The console reports the game's free memory in whole MiB (あおかな, PCSG00700, 2026-10-26: every sample 1, 2,
 * 3 or 4 MiB). A job whose check passed with less than 1 MiB to spare took its own blocks, the next sample read
 * a whole MiB less, and its model's weights, checked again against that less the reserve, were refused before
 * any was read: recognition ended out of memory, and no smaller model was tried. Now the job gives its blocks
 * back and checks the next model, which fits, and reads the line. With that model the last one, its refusal
 * is the job's: no loop. Whatever the tiers' reserves: the free memory is set from each tier's own need. */
static void weights_refused_after_the_check_retry_the_next_smaller_model(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    const uint32_t mib = 1024u * 1024u;
    const uint32_t small = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER1_NAME)->bytes);
    const uint32_t normal = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER2_NAME)->bytes);
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    /* Normal passes the check at F, the least whole MiB its need fits; once its blocks are taken the sample
     * reads F - 1 MiB, which less its reserve is short of its weights. Small fits both (normal_refused_small_reads). */
    const uint32_t f = normal_refused_small_reads(&r);
    (void)normal;
    line();
    free_mib = 1;
    available = (int)f;
    int rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK_(rc == VJO_OK, "free %u: rc %d", f, rc);
    TEST_CHECK_(out.lines >= 1 && out.fallback == 1u, "%u lines, fallback %u", out.lines, out.fallback);
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && weights_live && weights_block == small);
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_SMALL && !memory_live && !capture_live && !out.cleanup_status);
    {
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        unsigned n = check_steps(&out, s);
        TEST_CHECK_(n == 2 && VJO_GCHECK_TIER(s[0]->kind) == 2 && !s[0]->rc && VJO_GCHECK_TIER(s[1]->kind) == 1 &&
                    s[1]->arg == 1u && !s[1]->rc, "%u checks", n); /* Normal passed, then Small, the retry's */
    }
    TEST_CHECK(!drop_resident());

    /* Small is the last one left: beside the 512 KiB test draft's reserve, at 2 MiB (アマツツミ's, PCSG01198) and a
     * page, in whole MiB, Normal passes its check and is refused its weights (the report reads 1 MiB once its blocks
     * are taken), and so is Small, checked again: the job's OOM, its blocks given back, each size tried once, no loop.
     * (Beside #280's 1 MiB, Normal's check refused it there and only Small was tried.) */
    r = setup();
    r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    const uint32_t f2 = 2u * mib;
    {
        const uint32_t blocks_s = tier_need(&r, 1, 0) - small - VJO_GAME_RESERVE_SMALL;
        TEST_ASSERT(tier_need(&r, 2, 0) <= f2 && blocks_s > 4096u);
        TEST_ASSERT((f2 + 4096u - blocks_s) / mib * mib < VJO_GAME_RESERVE_SMALL + small);
    }
    line();
    free_mib = 1;
    available = (int)(f2 + 4096u);
    rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK_(rc == VJO_E_OOM && !out.text[0], "free %u: rc %d", f2, rc);
    TEST_CHECK(!memory_live && !capture_live && !weights_live && !out.cleanup_status);
    {
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        unsigned n = check_steps(&out, s);
        TEST_CHECK_(n == 2 && VJO_GCHECK_TIER(s[0]->kind) == 2 && !s[0]->rc && VJO_GCHECK_TIER(s[1]->kind) == 1 &&
                        s[1]->arg == 1u && !s[1]->rc,
                    "%u checks", n);
    }
    free_mib = 0;
}

/* #324's audit F1: the retry after a model's refused weights is the same job. It shares the job's stable-text wait
 * (at most ocr_settle_max_ms over all its tries, docs/text-stability.md), as auto's tries do (API 47), and its
 * captures count every try's. The text types out for about 1.9 s at 60 Hz, then holds, and the job was asked while it
 * changed (wait first; 150 ms of stillness, within 2 s): Normal, at its whole-MiB edge, waits out the whole limit,
 * then its weights are refused; Small reads at once, within what is left of the wait (nothing). The job takes no
 * longer than Small alone at the same memory plus the refused try's own passes (before: Small waited again, about
 * 217 ms more, and the wait step said it held), and its wait step counts both tries' wait. */
static void weights_refused_retry_shares_the_wait(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    const uint32_t mib = 1024u * 1024u;
    const uint32_t wait = VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT | /* 150 ms, at most 2 s */
                          40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    static VjoGameOcrResult out[2];
    const VjoGameOcrStep *settle[2] = {NULL, NULL};
    uint64_t took[2] = {0, 0};
    for (int retry = 0; retry <= 1; retry++) { /* Small alone, then Normal (its weights refused) and Small */
        VjoGameOcrRequest r = setup();
        r.tiers = 1u << (VJO_VOCR_TIER_SMALL - 1u) | (retry ? 1u << (VJO_VOCR_TIER_NORMAL - 1u) : 0u);
        snprintf(r.model, sizeof(r.model), "%s", retry ? VJO_VOCR_TIER2_NAME : VJO_VOCR_TIER1_NAME);
        r.flags = wait;
        const uint32_t f = normal_refused_small_reads(&r); /* Normal's whole-MiB edge and Small's blocks, as above */
        (void)mib;
        line();
        free_mib = 1;
        available = (int)f;
        frame_us = 16667;   /* each rows call waits for the game's next frame */
        flicker_rows = 115; /* about 1.9 s of a changing screen, then still */
        const uint64_t t0 = clock_value;
        int rc = vjo_game_ocr_run_request(&r, &out[retry]);
        took[retry] = clock_value - t0;
        TEST_CHECK_(rc == VJO_OK && out[retry].lines >= 1 && out[retry].fallback == (unsigned)retry,
                    "%s: rc %d, %u lines, fallback %u", retry ? "retry" : "Small alone", rc, out[retry].lines,
                    (unsigned)out[retry].fallback);
        /* every rows call is one of the job's captures, whichever try made it */
        TEST_CHECK_(out[retry].captures == rows_calls, "%s: %u captures, %u rows calls",
                    retry ? "retry" : "Small alone", (unsigned)out[retry].captures, rows_calls);
        for (unsigned i = 0; i < out[retry].n_steps && i < VJO_GAME_OCR_STEPS; i++)
            if (out[retry].step[i].id == VJO_GSTEP_SETTLE) settle[retry] = &out[retry].step[i];
        TEST_CHECK(!memory_live && !capture_live && !line_live);
        TEST_CHECK(!drop_resident());
    }
    TEST_ASSERT(settle[0] != NULL && settle[1] != NULL);
    /* the whole wait was the refused try's: the job waited 2 s, timed out, and the retry read at once */
    TEST_CHECK_(settle[1]->us >= 2000000u && settle[1]->rc == VJO_GSETTLE_TIMEOUT && settle[0]->rc == VJO_GSETTLE_TIMEOUT,
                "the wait step: %u us, rc %d (Small alone: %u us, rc %d)", (unsigned)settle[1]->us,
                (int)settle[1]->rc, (unsigned)settle[0]->us, (int)settle[0]->rc);
    /* the refused try costs its own passes (the finder's and the wait's last, a few frames), never another wait */
    TEST_CHECK_(took[1] < took[0] + 120000u, "the retry took %llu us, Small alone %llu us: %lld us more",
                (unsigned long long)took[1], (unsigned long long)took[0], (long long)(took[1] - took[0]));
    free_mib = 0;
}

/* #324's audit F2: the retry's step log is the job's that read the text, as API 47's tries' is. The refused try
 * gives back its blocks, so its ALLOC and FREE steps leave the log, and the capture is kept: its PASS and SIG steps
 * are logged once. On a still box at 60 Hz with the stable-text wait (150 ms, within 2 s), the log is Small alone's
 * at the same memory with one CHECK step more, Normal's (passed): the last try's LOAD, RECOGNIZE and NORM steps are
 * there (before, 20 steps filled the log and NORM was left out). A passed check followed by the next model's
 * (include/vjo_game_ocr.h VJO_GSTEP_CHECK) is what says the weights were refused. */
static void weights_refused_retry_keeps_the_last_try_s_steps(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    const uint32_t mib = 1024u * 1024u;
    static VjoGameOcrResult out[2];
    for (int retry = 0; retry <= 1; retry++) { /* Small alone, then Normal (its weights refused) and Small */
        VjoGameOcrRequest r = setup();
        r.tiers = 1u << (VJO_VOCR_TIER_SMALL - 1u) | (retry ? 1u << (VJO_VOCR_TIER_NORMAL - 1u) : 0u);
        snprintf(r.model, sizeof(r.model), "%s", retry ? VJO_VOCR_TIER2_NAME : VJO_VOCR_TIER1_NAME);
        r.flags = 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
        const uint32_t f = normal_refused_small_reads(&r);
        (void)mib;
        line();
        free_mib = 1;
        available = (int)f;
        frame_us = 16667;
        int rc = vjo_game_ocr_run_request(&r, &out[retry]);
        TEST_CHECK_(rc == VJO_OK && out[retry].fallback == (unsigned)retry, "%s: rc %d, fallback %u",
                    retry ? "retry" : "Small alone", rc, (unsigned)out[retry].fallback);
        TEST_CHECK(!memory_live && !capture_live);
        TEST_CHECK(!drop_resident());
    }
    TEST_CHECK_(count_steps(&out[1], VJO_GSTEP_PASS) == 1 && count_steps(&out[1], VJO_GSTEP_SIG) == 1,
                "%u PASS and %u SIG steps", count_steps(&out[1], VJO_GSTEP_PASS), count_steps(&out[1], VJO_GSTEP_SIG));
    TEST_CHECK_(count_steps(&out[1], VJO_GSTEP_LOAD) == 1 && count_steps(&out[1], VJO_GSTEP_RECOGNIZE) == 1 &&
                    count_steps(&out[1], VJO_GSTEP_NORM) == 1,
                "%u steps: LOAD %u, RECOGNIZE %u, NORM %u", (unsigned)out[1].n_steps,
                count_steps(&out[1], VJO_GSTEP_LOAD), count_steps(&out[1], VJO_GSTEP_RECOGNIZE),
                count_steps(&out[1], VJO_GSTEP_NORM));
    for (unsigned id = VJO_GSTEP_MEM; id < VJO_GSTEP_COUNT; id++)
        TEST_CHECK_(count_steps(&out[1], id) == count_steps(&out[0], id) + (id == VJO_GSTEP_CHECK),
                    "step %u: %u, Small alone %u", id, count_steps(&out[1], id), count_steps(&out[0], id));
    free_mib = 0;
}
static const uint32_t tier_reserve[] = {0, VJO_GAME_RESERVE_SMALL, VJO_GAME_RESERVE_NORMAL, VJO_GAME_RESERVE_LARGE};

/* A request by tiers runs the largest of its tiers whose job fits the game's free
 * USER memory beside the fixed 1 MiB reserve, else the next one
 * down, and refuses only when none fits. Each tier tried is a CHECK step (its tier
 * in the kind, refused or passed, what it needed, the own blocks it counted as
 * free); the result says how many tiers down the job went, with the largest's need.
 * A blank region: only the size that ran is opened, by the job's warm-up (ocr-first:
 * a job that found no line reads the weights it would run, within its check), and a
 * refused job opens none. */
static void tier_ladder_takes_the_largest_size_that_fits(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.tiers = VJO_VOCR_TIERS_ALL;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    uint32_t need[4] = {0, tier_need(&r, 1, 0), tier_need(&r, 2, 0), tier_need(&r, 3, 0)};
    for (unsigned t = 1; t <= 3; t++) {
        const VjoVocrModel *m = vjo_vocr_model(tier_names[t - 1u]);
        TEST_CHECK(need[t] == PAGE_ROUND(m->bytes) + PAGE_ROUND(vjo_vocr_job_bytes(m->name, 200, 60)) +
                              PAGE_ROUND(vjo_vocr_capture_bytes(200, 60)) + tier_reserve[t]);
        if (t > 1) TEST_CHECK(need[t] > need[t - 1u]);
    }
    for (unsigned want = 3; want >= 1; want--)
        for (int short_by = 0; short_by <= 4096; short_by += 4096) {
            r = setup(); r.tiers = VJO_VOCR_TIERS_ALL;
            snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
            available = (int)need[want] - short_by;
            unsigned ran = short_by ? want - 1u : want; /* 0: none fits */
            int rc = vjo_game_ocr_run_request(&r, &out);
            const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
            unsigned n = check_steps(&out, s), last = ran ? ran : 1u;
            TEST_CHECK_(rc == (ran ? 0 : VJO_E_OOM), "free %d: rc %d", available, rc);
            TEST_CHECK_(n == 4u - last, "free %d (tier %u%s): %u checks", available, want, short_by ? " short" : "", n);
            for (unsigned i = 0; i < n; i++) {
                unsigned t = 3u - i;
                TEST_CHECK_(VJO_GCHECK_TIER(s[i]->kind) == t && s[i]->arg == (i ? 1u : 0u) &&
                            s[i]->rc == (t == ran ? 0 : 1) && s[i]->bytes + tier_reserve[t] == need[t] && !s[i]->sum,
                            "free %d: check %u kind 0x%X arg %u rc %d bytes %u", available, i, s[i]->kind, s[i]->arg,
                            (int)s[i]->rc, (unsigned)s[i]->bytes);
            }
            TEST_CHECK(out.fallback == 3u - last && out.reserve_bytes == tier_reserve[last]);
            TEST_CHECK(out.need_workspace + out.need_weights + out.reserve_bytes == need[last]);
            if (last < 3u) TEST_CHECK(out.model_need_workspace + out.model_need_weights + tier_reserve[3] == need[3]);
            else TEST_CHECK(!out.model_need_workspace && !out.model_need_weights);
            for (unsigned t = 1; t <= 3; t++)
                TEST_CHECK_(tier_opens[t] == (t == ran ? 1u : 0u), "free %d: tier %u opened %u times", available, t,
                            tier_opens[t]); /* the warm-up's read of the size that ran */
            TEST_CHECK(!memory_live && !capture_live);
            TEST_CHECK(allocations == (ran ? 1u : 0u)); /* a job that fits takes its workspace */
            if (!ran) break;
        }
    /* the largest only: one check */
    r = setup(); r.tiers = 1u << (VJO_VOCR_TIER_LARGE - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    available = (int)need[3] - 4096;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !out.fallback && out.reserve_bytes == tier_reserve[3]);
    /* a tier left out of the request (a model the Shell found unusable here) is not tried */
    r = setup(); r.tiers = 1u << (VJO_VOCR_TIER_LARGE - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    available = (int)need[2] + 4096; /* Normal would fit; it is not asked for */
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.fallback == 2u && out.reserve_bytes == tier_reserve[1]);
    {
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        TEST_CHECK(check_steps(&out, s) == 2 && VJO_GCHECK_TIER(s[0]->kind) == 3 && VJO_GCHECK_TIER(s[1]->kind) == 1);
    }
    /* unknown tier bits are refused, as unknown flags are; a request by name keeps its flat reserve */
    r = setup(); r.tiers = 8u;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r = setup(); /* FL10_w8.vocr by name */
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.reserve_bytes == GAME_RESERVE);
    {
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        TEST_CHECK(check_steps(&out, s) == 1 && !VJO_GCHECK_TIER(s[0]->kind) && !s[0]->sum);
    }
}

static void tall_columns_frame(VjoGameOcrRequest *r);
static void wide_columns_frame(VjoGameOcrRequest *r);
static void box_of_two_lines(void);
static unsigned find_steps(const VjoGameOcrResult *out, unsigned kind);
static int rows_rc_here(void);
/* API 53 (#379), on (VJO_GAME_OCR_S_VERTICAL 1) prefers columns. Its job reads the region's rows first, as auto's,
 * with the size's models (its tiers), and checks them the other way (core/vocr_ocr.h VJO_VOCR_CHECK_ON). Rows of
 * horizontal text are read there: off's job exactly, but for the find step's kind (4, the log's line that on read
 * rows), and no vertical model opened. Other rows read nothing (find kind 2) and the region's columns are read by a
 * job of the vertical model alone, opened by its own open, never a size's model: one memory check of its own beside a
 * request by name's reserve (its rotation buffer in the workspace, the LM its flags allow), never the cascade or the
 * refinement the flags ask for (a PSP game's included); without the vertical model the rows are read (find kind 3).
 * Too little memory for the vertical model's job: the rows are read (find kind 5), with the size that fits. Before,
 * on read the columns of every region, and failed without the vertical model or its memory. #385's audit F6: a
 * region at least as tall as wide is read as columns from the job's first try (no rows' try, as before API 53). */
static void on_prefers_columns(void)
{
    const VjoVocrModel *v = vjo_vocr_model(VJO_VOCR_VERTICAL_NAME);
    VjoGameOcrRequest r;
    VjoGameOcrResult out, off;
    char v_path[600];
    TEST_ASSERT(v && (v->finder_mode & VJO_VOCR_FINDER_COLUMNS));
    snprintf(v_path, sizeof(v_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_VERTICAL_NAME);
    FILE *vf = fopen(v_path, "rb");
    int have_v = vf != NULL; /* the release file (a model directory with it) */
    if (vf) fclose(vf);
    const int rows_rc = rows_rc_here();
    /* horizontal text (the tests' 200 x 60 frame of two lines): on is off's job, its find step kind 4 */
    for (int mode = 0; mode <= 1; mode++) {
        r = setup();
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.flags = 1u << VJO_GAME_OCR_LM_SHIFT;
        r.speed = mode ? VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT : 0u;
        for (unsigned y = 10; y < 30; y++)
            for (unsigned x = 10; x < 190; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
        for (unsigned y = 36; y < 54; y++)
            for (unsigned x = 10; x < 120; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
        (void)vjo_game_ocr_run_request(&r, mode ? &out : &off);
        TEST_CHECK_(!vertical_opens && !find_steps(mode ? &out : &off, 2) && find_steps(mode ? &out : &off, 4) == (unsigned)mode,
                    "mode %d: %u vertical opens, %u and %u find steps of kinds 2 and 4", mode, vertical_opens,
                    find_steps(mode ? &out : &off, 2), find_steps(mode ? &out : &off, 4));
    }
    TEST_CHECK_(out.rc == off.rc && out.lines == off.lines && out.lines == 2 && out.recognized == off.recognized &&
                    !strcmp(out.text, off.text) && out.n_steps == off.n_steps && out.captures == off.captures,
                "on: rc %d %u lines, off: rc %d %u lines", out.rc, out.lines, off.rc, off.lines);
    for (unsigned i = 0; i < out.recognized; i++) TEST_CHECK(!(out.line[i].flags & VJO_GAME_OCR_LINE_COLUMN));
    /* vertical text: in a region wider than tall the rows' try reads nothing (find kind 2) and leaves the log with its
     * blocks and memory checks (#385's audit F7), the columns' the vertical model alone; in a region at least as tall
     * as wide the columns are read from the first try, with no rows' try (#385's audit F6) */
    const size_t extra = vjo_vocr_lm_job_bytes(v->name) + VJO_GAME_OCR_TEXT_BYTES;
    int rc = VJO_OK;
    for (int wide = 0; wide <= 1; wide++) {
        r = setup();
        r.tiers = VJO_VOCR_TIERS_ALL;
        snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME); /* the Shell names the largest size, for the log */
        r.flags = 1u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT |
                  VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT;
        r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
        psp_title = 1; /* the refinement's "psp" mode would apply to rows here */
        if (wide) wide_columns_frame(&r);
        else tall_columns_frame(&r);
        const uint32_t need_ws = PAGE_ROUND(vjo_vocr_job_bytes(v->name, 200, r.height) + extra) +
                                 PAGE_ROUND(vjo_vocr_capture_bytes(200, r.height)),
                       need_w = PAGE_ROUND(v->bytes) + PAGE_ROUND(VJO_VOCR_LM1_BYTES);
        rc = vjo_game_ocr_run_request(&r, &out);
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        unsigned n = check_steps(&out, s), refine_steps = 0, first_check = VJO_GAME_OCR_STEPS, find2 = VJO_GAME_OCR_STEPS;
        for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
            refine_steps += out.step[i].id == VJO_GSTEP_REFINE;
            if (out.step[i].id == VJO_GSTEP_CHECK && first_check == VJO_GAME_OCR_STEPS) first_check = i;
            if (out.step[i].id == VJO_GSTEP_FIND && out.step[i].kind == 2) find2 = i;
        }
        /* the columns' try's check: the vertical model's job, by name (no tier), beside a request by name's reserve,
         * the job's first check (a wide region's rows' try left none before its find step) */
        const VjoGameOcrStep *vcheck = n ? s[0] : NULL;
        TEST_CHECK_(find_steps(&out, 2) == (unsigned)wide && vertical_opens >= 1 && vcheck &&
                        !VJO_GCHECK_TIER(vcheck->kind) && !vcheck->rc && vcheck->bytes == need_ws + need_w &&
                        (!wide || first_check > find2),
                    "%s columns: %u steps of kind 2, vertical %u, the first check's bytes %u (want %u), tier %u, at "
                    "step %u (find kind 2 at %u)", wide ? "wide" : "tall", find_steps(&out, 2), vertical_opens,
                    vcheck ? (unsigned)vcheck->bytes : 0u, (unsigned)(need_ws + need_w),
                    vcheck ? VJO_GCHECK_TIER(vcheck->kind) : 0u, first_check, find2);
        if (have_v) { /* the columns read: no size's model, cascade or refinement, the only check the vertical model's */
            TEST_CHECK_(rc == VJO_OK && find_steps(&out, 1) == 1 && out.lines == 3 && out.recognized == 3 &&
                            !tier_opens[1] && !tier_opens[2] && !tier_opens[3] && !cascade_opens &&
                            !cascade_allocations && !refine_steps && n == 1 && out.reserve_bytes == GAME_RESERVE &&
                            !out.fallback && out.need_workspace == need_ws && out.need_weights == need_w,
                        "%s columns: rc %d, %u lines, tiers %u/%u/%u, cascade %u, refine steps %u, %u checks",
                        wide ? "wide" : "tall", rc, out.lines, tier_opens[1], tier_opens[2], tier_opens[3],
                        cascade_opens, refine_steps, n);
            for (unsigned i = 0; i < out.recognized; i++) TEST_CHECK(out.line[i].flags & VJO_GAME_OCR_LINE_COLUMN);
        } else { /* no vertical model (CI): its try fails to read it (find kind 3), and the rows are read, a size's model */
            TEST_CHECK_(rc == rows_rc && find_steps(&out, 3) == 1 && !find_steps(&out, 1) &&
                            out.lines == (wide ? 6u : 7u) && tier_opens[1] + tier_opens[2] + tier_opens[3] >= 1,
                        "%s columns, no model file: rc %d (the rows' %d), %u lines", wide ? "wide" : "tall", rc, rows_rc,
                        out.lines);
            for (unsigned i = 0; i < out.recognized; i++) TEST_CHECK(!(out.line[i].flags & VJO_GAME_OCR_LINE_COLUMN));
        }
        TEST_CHECK(!memory_live && !capture_live && !line_live);
    }
    /* too little memory for the vertical model's job (the columns' check refused): the rows, with Small, which needs
     * the rotation buffer's bytes less (rc: Small's file here or not); a wide region's rows' try first */
    for (int wide = 0; wide <= 1; wide++) {
        r = setup();
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
        if (wide) wide_columns_frame(&r);
        else tall_columns_frame(&r);
        available = (int)(PAGE_ROUND(vjo_vocr_job_bytes(v->name, 200, r.height)) +
                          PAGE_ROUND(vjo_vocr_capture_bytes(200, r.height)) + PAGE_ROUND(v->bytes) + GAME_RESERVE) -
                    4096;
        rc = vjo_game_ocr_run_request(&r, &out);
        const VjoVocrModel *small = vjo_vocr_model(VJO_VOCR_TIER1_NAME);
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", small->name);
        FILE *f = fopen(p, "rb");
        if (f) fclose(f);
        TEST_CHECK_(find_steps(&out, 2) == (unsigned)wide && find_steps(&out, 5) == 1 && !vertical_opens &&
                        (f ? rc == VJO_OK : rc == VJO_E_OCR_MODEL),
                    "%s, no room: rc %d, %u and %u steps of kinds 2 and 5, %u vertical opens", wide ? "wide" : "tall", rc,
                    find_steps(&out, 2), find_steps(&out, 5), vertical_opens);
        TEST_CHECK(!memory_live && !capture_live && !line_live);
    }
    /* a shot carries no vertical field (the kernel refuses it too) */
    r = setup();
    r.flags = VJO_GAME_OCR_F_SHOT;
    r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    r.model[0] = r.model_dir[0] = '\0';
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
}

/* API 47: a frame of three columns of seven glyph-like cells (light on dark), 200 x 232, the region the whole
 * frame (the fake capture's rows are 200 wide): taller than wide; the row finder finds a row for each cell
 * across the columns. */
static void columns_pixels(unsigned cells)
{
    memset(pixels, 0, sizeof(pixels));
    for (unsigned c = 0; c < 3; c++) {
        unsigned x0 = 200u - 30u - (c + 1u) * 32u;
        for (unsigned g = 0; g < cells; g++) {
            unsigned s = (c + 1u) * 131u + g * 29u, cy = 4u + 8u + g * 29u;
            for (unsigned b = 0; b < 24u; b += 3u + (s + b) % 3u) {
                unsigned from = (s * 7u + b) % 6u, to = 24u - (s * 3u + b * 5u) % 6u;
                memset(pixels + ((size_t)(cy + b) * 200u + x0 + from) * 4u, 235, (to - from) * 4u);
            }
            for (unsigned v = 0; v < 1u + s % 2u; v++)
                for (unsigned y = (s + v) % 9u; y < 24u; y++)
                    memset(pixels + ((size_t)(cy + y) * 200u + x0 + (s * 5u + v * 9u) % 22u) * 4u, 235, 8);
        }
    }
}
static void tall_columns_pixels(void)
{
    columns_pixels(7);
}
static void tall_columns_frame(VjoGameOcrRequest *r)
{
    frame_h = r->fb_height = r->height = 232;
    tall_columns_pixels();
}
/* #385's audit: the same columns, six cells each, in a region wider than tall (200 x 190): on's job checks its rows
 * first (six of them, one a cell across the columns: not lines), then reads the columns. */
static void wide_columns_frame(VjoGameOcrRequest *r)
{
    frame_h = r->fb_height = r->height = 190;
    columns_pixels(6);
}
/* Two lines of horizontal text, the tests' 200 x 60 frame (on_prefers_columns'). */
static void box_of_two_lines(void)
{
    memset(pixels, 0, sizeof(pixels));
    for (unsigned y = 10; y < 30; y++)
        for (unsigned x = 10; x < 190; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
    for (unsigned y = 36; y < 54; y++)
        for (unsigned x = 10; x < 120; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
}

static unsigned find_steps(const VjoGameOcrResult *out, unsigned kind)
{
    unsigned n = 0;
    for (unsigned i = 0; i < out->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        n += out->step[i].id == VJO_GSTEP_FIND && out->step[i].kind == kind;
    return n;
}

/* The audit's F3b: auto with the pause. Each try copies its own frame into its own block: the rows' (which look
 * vertical here), the columns' and, without the vertical model, the rows' again. The step's us is their copies' sum,
 * the last try's region is frozen, and the job's captures are its copies. */
static void auto_with_the_pause_copies_each_tries_frame(void)
{
    VjoGameOcrRequest r;
    VjoGameOcrResult out;
    r = setup();
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.flags = 1u << VJO_GAME_OCR_LM_SHIFT;
    r.speed = VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_PAUSE;
    tall_columns_frame(&r);
    (void)vjo_game_ocr_run_request(&r, &out);
    const VjoGameOcrStep *ps = NULL;
    unsigned finds = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        if (out.step[i].id == VJO_GSTEP_PAUSE) ps = &out.step[i];
        finds += out.step[i].id == VJO_GSTEP_FIND;
    }
    TEST_CHECK_(find_steps(&out, 2) == 1 && frame_copies >= 2 && frame_copies == finds && frame_allocs == frame_copies,
                "%u copies, %u blocks, %u find steps", frame_copies, frame_allocs, finds);
    TEST_CHECK(!frame_live && !memory_live && !capture_live);
    TEST_CHECK_(ps && ps->kind == 1 && ps->arg == frame_h && ps->us == 1234u * frame_copies && out.captures == frame_copies,
                "pause step: kind %u, rows %u, us %u; captures %u", ps ? ps->kind : 9u, ps ? ps->arg : 0u,
                ps ? ps->us : 0u, out.captures);
}

/* API 47, auto (VJO_GAME_OCR_VERTICAL_AUTO): a job of rows whose finder's rows look like vertical text reads no
 * weights for them, logs a find step of kind 2 and reads the region's columns with the vertical model, opened by
 * its own open, as with on (find kind 1, lines marked VJO_GAME_OCR_LINE_COLUMN); with no vertical model to read
 * (here, without the release file) it reads the rows instead, and the game's later jobs read rows unchecked. On
 * horizontal text, auto is off's job: no vertical open, no step of kind 2, the same lines. */
static void auto_reads_columns_where_the_rows_are_vertical(void)
{
    VjoGameOcrRequest r;
    VjoGameOcrResult out, off;
    char v_path[600];
    snprintf(v_path, sizeof(v_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_VERTICAL_NAME);
    FILE *vf = fopen(v_path, "rb");
    int have_v = vf != NULL;
    if (vf) fclose(vf);
    /* the Large size's file (a request by every tier tries it first): a models' directory with it reads the rows */
    snprintf(v_path, sizeof(v_path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_TIER3_NAME);
    vf = fopen(v_path, "rb");
    const int have_t = vf != NULL, rows_rc = have_t ? VJO_OK : VJO_E_OCR_MODEL;
    if (vf) fclose(vf);
    r = setup();
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.flags = 1u << VJO_GAME_OCR_LM_SHIFT;
    r.speed = VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    tall_columns_frame(&r);
    int rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK_(find_steps(&out, 2) == 1 && vertical_opens >= 1, "rows checked: %u steps of kind 2, %u vertical opens",
                find_steps(&out, 2), vertical_opens);
    if (have_v) {
        TEST_CHECK_(rc == VJO_OK && find_steps(&out, 1) == 1 && out.recognized == 3 && !columns_bad,
                    "columns: rc %d, %u read", rc, out.recognized);
        for (unsigned i = 0; i < out.recognized; i++) TEST_CHECK(out.line[i].flags & VJO_GAME_OCR_LINE_COLUMN);
        /* the next job of the same screen: the rows' try keeps the vertical model's weights resident (it reads none
         * before its check), and the columns' try reads with them, from memory */
        const unsigned reads = weight_allocations;
        rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK_(rc == VJO_OK && find_steps(&out, 2) == 1 && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT &&
                        weight_allocations == reads,
                    "the next job: rc %d, weights %u (resident %u), %u weight blocks more", rc, (unsigned)out.weights,
                    (unsigned)VJO_GAME_OCR_WEIGHTS_RESIDENT, weight_allocations - reads);
    } else {
        /* the vertical model cannot be read (a storage error, not a file one): find kind 3, the rows read (without
         * the size's file either, the rows' own I/O error, not the vertical model's); the game's next job checks
         * again */
        TEST_CHECK_(rc == rows_rc && !columns_bad && find_steps(&out, 3) == 1 &&
                    tier_opens[3] + tier_opens[2] + tier_opens[1] >= 1, "no vertical model: rc %d, columns_bad %d",
                    rc, columns_bad);
        unsigned v = vertical_opens;
        rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK_(find_steps(&out, 2) == 1 && vertical_opens == v + 1, "next job: %u steps of kind 2, %u opens",
                    find_steps(&out, 2), vertical_opens - v);
        /* the file absent (ENOENT): the same, and the game's later jobs read rows without checking them */
        missing_open_rc = (int)0x80010002u;
        rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK_(rc == rows_rc && columns_bad && find_steps(&out, 3) == 1, "ENOENT: rc %d, columns_bad %d",
                    rc, columns_bad);
        v = vertical_opens;
        rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK_(!find_steps(&out, 2) && !find_steps(&out, 3) && vertical_opens == v,
                    "later job: %u steps of kind 2, %u opens", find_steps(&out, 2), vertical_opens - v);
    }
    TEST_CHECK(!memory_live && !capture_live && !line_live);
    /* horizontal text (the tests' 200 x 60 frame of two lines): auto is off's job */
    for (int mode = 0; mode <= 1; mode++) {
        r = setup();
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.flags = 1u << VJO_GAME_OCR_LM_SHIFT;
        r.speed = mode ? VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT : 0u;
        for (unsigned y = 10; y < 30; y++)
            for (unsigned x = 10; x < 190; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
        for (unsigned y = 36; y < 54; y++)
            for (unsigned x = 10; x < 120; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
        rc = vjo_game_ocr_run_request(&r, mode ? &out : &off);
        TEST_CHECK_(!vertical_opens && !find_steps(mode ? &out : &off, 2), "mode %d: %u vertical opens", mode,
                    vertical_opens);
    }
    TEST_CHECK_(out.rc == off.rc && out.lines == off.lines && out.recognized == off.recognized &&
                !strcmp(out.text, off.text) && out.n_steps == off.n_steps, "auto: rc %d %u lines, off: rc %d %u lines",
                out.rc, out.lines, off.rc, off.lines);
}

/* API 47, auto's tries report the last one's model: a try's step down to a smaller size (VjoGameOcrResult.fallback,
 * from which the Shell names the size that ran on cards, in history.csv and the log, and the first model's needs) is
 * its own, never an earlier try's. Here the rows' first try steps down to Normal (Large does not fit), its rows look
 * like vertical text, the columns' try cannot read the vertical model (no file) as the game frees memory, and the
 * rows' last try reads with Large, its largest size: no fallback. (With the vertical model's file the columns' try
 * reads, a request by name with nothing to step down to: no fallback either.) */
static void auto_tries_report_the_last_ones_model(void)
{
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.flags = 1u << VJO_GAME_OCR_LM_SHIFT;
    r.speed = VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    tall_columns_frame(&r);
    /* room for Normal and its LM beside the 1 MiB reserve, not for Large even without its LM (with the 4, 8 and
     * 16 MiB reserves before PR #280: 12 MiB) */
    const uint32_t large = tier_need(&r, 3, 0);
    TEST_ASSERT(tier_lm_need(&r, 2, vjo_vocr_lm_tier(1)) <= large - 4096u);
    available = (int)(large - 4096u);
    {   /* that memory steps the rows' try down: the request with ocr_vertical = off, one try, runs Normal */
        VjoGameOcrRequest off = r; VjoGameOcrResult o;
        off.speed = 0;
        (void)vjo_game_ocr_run_request(&off, &o);
        TEST_CHECK_(o.fallback == 1u && o.reserve_bytes == VJO_GAME_RESERVE_NORMAL, "off: fallback %u, reserve %u",
                    (unsigned)o.fallback, (unsigned)o.reserve_bytes);
        TEST_CHECK(!drop_resident() && !memory_live && !capture_live && !line_live && !vertical_opens);
    }
    grow_on_vertical_open = (int)(16u * 1024u * 1024u); /* then room for Large */
    (void)vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK_(find_steps(&out, 2) == 1 && vertical_opens == 1, "%u steps of kind 2, %u vertical opens",
                find_steps(&out, 2), vertical_opens);
    TEST_CHECK_(!out.fallback && !out.model_need_workspace && !out.model_need_weights && !out.model_release_bytes,
                "fallback %u, the first model's needs %u + %u B (release %u B): an earlier try's", (unsigned)out.fallback,
                (unsigned)out.model_need_workspace, (unsigned)out.model_need_weights, (unsigned)out.model_release_bytes);
    TEST_CHECK(!memory_live && !capture_live && !line_live);
}

/* API 47, auto's retry and the stable-text wait: a job with the wait waits at most ocr_settle_max_ms over all its
 * tries (docs/text-stability.md), and auto's tries are the job's too. The text
 * types out for longer than the limit allows the rows to hold still, then holds: the rows' try spends the whole
 * limit waiting (it never held still for the wait), and its rows look like vertical text. The columns' try, and
 * here (no vertical model to read) the rows' try after it, read the screen at once: the job waits no longer than
 * the same request with ocr_vertical = off, which makes one try (and on, since API 53, auto's tries), plus the extra
 * tries' own passes; its wait step counts every try's wait. */
static void auto_retry_shares_the_wait(void)
{
    const uint32_t wait = VJO_GAME_OCR_F_SETTLE_FIRST | 30u << VJO_GAME_OCR_SETTLE_SHIFT | /* 300 ms, at most 800 ms */
                          16u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    uint64_t took[2] = {0, 0};
    uint32_t waited = 0;
    VjoGameOcrRequest r;
    VjoGameOcrResult out;
    for (int mode = 0; mode <= 1; mode++) { /* off (one try), then auto (the rows' try, then the columns') */
        r = setup();
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.flags = wait;
        r.speed = mode ? VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT : 0u;
        tall_columns_frame(&r);
        frame_us = 8000;     /* each rows call waits for the game's next frame */
        flicker_rows = 75;   /* about 600 ms of a changing screen, then still */
        const uint64_t t0 = clock_value;
        (void)vjo_game_ocr_run_request(&r, &out);
        took[mode] = clock_value - t0;
        if (mode) { /* each try's find step: the rows' (kind 2), the columns' (1, or 3 without the vertical model) */
            TEST_CHECK_(find_steps(&out, 2) == 1 && find_steps(&out, 1) + find_steps(&out, 3) == 1,
                        "auto: %u, %u and %u steps of kinds 2, 1 and 3", find_steps(&out, 2), find_steps(&out, 1),
                        find_steps(&out, 3));
            for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
                if (out.step[i].id == VJO_GSTEP_SETTLE) waited = out.step[i].us;
        }
        TEST_CHECK(!memory_live && !capture_live && !line_live);
    }
    /* the extra tries cost their own passes (a finder's two each, 4 rows calls a pass here), never another wait */
    TEST_CHECK_(took[1] < took[0] + 300000u, "auto took %llu us, off %llu us: %lld us more (a wait is 300,000)",
                (unsigned long long)took[1], (unsigned long long)took[0], (long long)(took[1] - took[0]));
    TEST_CHECK_(waited >= 800000u && waited < 800000u + 4u * 4u * 8000u + 1000u,
                "the job's wait step: %u us (the limit 800,000)", (unsigned)waited);
}

/* #324's audit F2 on the longest path, and N2 (the free memory checked again for the next model): auto's rows look
 * like vertical text, the columns cannot be read (no vertical model: ENOENT), the rows' try checks Normal with the
 * LM, and the game takes memory meanwhile, so Normal's weights are refused; Small reads with the LM, after a 150 ms
 * wait. Each try's blocks leave the log with it, so the last try's ALLOC steps, its wait, LOAD, RECOGNIZE and LM
 * steps are within the 20 steps (before: none of them; the refused try's blocks and the second PASS and SIG took
 * their places). Small's check is made on the memory as it is then: its result's free memory is the game's less what
 * it took (the first sample saw all of it). The game reports its free memory in whole MiB, as the console does
 * (#324): with every size's reserve 1 MiB (PR #280), Normal's need is only 430,080 B over Small's here, less than
 * the LM's slack at Normal's weights, so only the console's rounding refuses Normal's weights and leaves Small and its
 * LM room (with the 4 MiB difference between the reserves before, a game that took more than the LM's bytes did). */
static void weights_refused_retry_after_auto_s_tries(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    r.flags = 1u << VJO_GAME_OCR_LM_SHIFT | 15u << VJO_GAME_OCR_SETTLE_SHIFT; /* the LM's first tier, 150 ms still */
    r.speed = VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    tall_columns_frame(&r);
    vertical_absent = 1;
    missing_open_rc = (int)0x80010002u; /* ENOENT */
    const VjoVocrLm *lm = vjo_vocr_lm_tier(1);
    TEST_ASSERT(lm != NULL);
    const uint32_t mib = 1024u * 1024u, lm_bytes = (uint32_t)PAGE_ROUND(lm->bytes);
    const uint32_t need_n = tier_lm_need(&r, 2, lm), need_s = tier_lm_need(&r, 1, lm);
    const uint32_t weights_n = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER2_NAME)->bytes);
    const uint32_t weights_s = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER1_NAME)->bytes);
    /* each model's job blocks (workspace and capture block): its need less its weights, the LM and the reserve */
    const uint32_t blocks_n = need_n - weights_n - lm_bytes - VJO_GAME_RESERVE_NORMAL;
    const uint32_t blocks_s = need_s - weights_s - lm_bytes - VJO_GAME_RESERVE_SMALL;
    TEST_ASSERT(need_n - need_s < lm_bytes); /* in bytes, no game's take refuses Normal's weights and fits Small's LM */
    /* K: the least whole MiB where Small's check and its weights both pass (3 MiB beside #280's 1 MiB, 2 MiB beside
     * the 512 KiB test draft's reserve). The check sees K + 1 MiB; the game then takes 1 MiB and a page, so once
     * Normal's blocks are taken it reads K - 1 MiB (short of Normal's weights beside the reserve); Small's check
     * reads K, and its weights and LM fit */
    const uint32_t k_check = (need_s + mib - 1u) / mib * mib;
    const uint32_t k_weights = (VJO_GAME_RESERVE_SMALL + weights_s + mib - 1u) / mib * mib;
    const uint32_t k = k_check > k_weights ? k_check : k_weights;
    const uint32_t first = k + mib, after = k + blocks_n - 4096u, take = first + blocks_n - after;
    TEST_ASSERT(blocks_n >= 8192u && blocks_n < mib && blocks_s <= blocks_n - 4096u && need_n <= first);
    TEST_ASSERT((after - blocks_n) / mib * mib < VJO_GAME_RESERVE_NORMAL + weights_n);           /* Normal's refused */
    TEST_ASSERT(after / mib * mib == k && k >= need_s);                                            /* Small's check */
    TEST_ASSERT((after - blocks_s) / mib * mib >= VJO_GAME_RESERVE_SMALL + weights_s);             /* its weights */
    TEST_ASSERT((after - blocks_s - weights_s) / mib * mib >= VJO_GAME_RESERVE_SMALL + lm_bytes);  /* its LM */
    free_mib = 1;
    available = (int)(first + blocks_n);
    drop_once = (int)take;
    drop_at_allocation = 3; /* at the rows' last try's workspace: the rows' first try's is 1, the columns' 2 */
    int rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK_(rc == VJO_OK && out.lines >= 1 && out.fallback == 1u && find_steps(&out, 2) == 1 &&
                    find_steps(&out, 3) == 1 && vertical_opens == 1 && allocations == 4u,
                "rc %d, %u lines, fallback %u, find steps of kinds 2 and 3: %u, %u; %u vertical opens, %u workspaces", rc,
                out.lines, (unsigned)out.fallback, find_steps(&out, 2), find_steps(&out, 3), vertical_opens, allocations);
    TEST_CHECK_(out.free_user == (int32_t)k && out.reserve_bytes == VJO_GAME_RESERVE_SMALL,
                "Small checked at %d B free (the game's now: %u B; the first check's %u B), reserve %u",
                (int)out.free_user, k, first, (unsigned)out.reserve_bytes);
    unsigned weights = 0, lm_block = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        weights += out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_WEIGHTS;
        lm_block += out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_LM;
    }
    TEST_CHECK_(weights == 1 && lm_block == 1 && count_steps(&out, VJO_GSTEP_SETTLE) == 1 &&
                    count_steps(&out, VJO_GSTEP_LOAD) == 1 && count_steps(&out, VJO_GSTEP_RECOGNIZE) == 1 &&
                    count_steps(&out, VJO_GSTEP_LM) == 1 && count_steps(&out, VJO_GSTEP_PASS) == 1 &&
                    count_steps(&out, VJO_GSTEP_SIG) == 1,
                "%u steps: ALLOC of the weights %u and the LM %u, SETTLE %u, LOAD %u, RECOGNIZE %u, LM %u, PASS %u, "
                "SIG %u", (unsigned)out.n_steps, weights, lm_block, count_steps(&out, VJO_GSTEP_SETTLE),
                count_steps(&out, VJO_GSTEP_LOAD), count_steps(&out, VJO_GSTEP_RECOGNIZE),
                count_steps(&out, VJO_GSTEP_LM), count_steps(&out, VJO_GSTEP_PASS), count_steps(&out, VJO_GSTEP_SIG));
    /* #382's audit N2: this path needed 21 steps (MEM, 4 CHECK, PASS, SIG, 3 FIND, 4 ALLOC, SETTLE, LOAD, RECOGNIZE,
     * LM, NORM and the 2 FREE steps of the job's blocks) for the log's 20, and NORM, the last one in, was left out.
     * #385's audit F7: the rows' first try's check (Normal's, passed) leaves the log with its blocks as it hands over
     * to the columns, so the 20 steps hold every one of them, NORM too. */
    TEST_CHECK_(out.n_steps == VJO_GAME_OCR_STEPS && count_steps(&out, VJO_GSTEP_FREE) == 2 &&
                    count_steps(&out, VJO_GSTEP_NORM) == 1 && count_steps(&out, VJO_GSTEP_CHECK) == 3,
                "%u steps, %u FREE, NORM %u, CHECK %u", (unsigned)out.n_steps, count_steps(&out, VJO_GSTEP_FREE),
                count_steps(&out, VJO_GSTEP_NORM), count_steps(&out, VJO_GSTEP_CHECK));
    TEST_CHECK(!memory_live && !capture_live && !line_live && weights_live && lm_live);
    TEST_CHECK(!drop_resident());
}

/* #382's audit N1: a block the refused try gave back that an earlier job had left resident keeps its FREE step. A
 * Small job leaves its weights resident; the next job of Normal or Small passes Normal's check by counting them as
 * free, releases them, and is refused Normal's weights (4 KiB taken at its workspace); Small reads its weights again.
 * The released weights are gone from the game for good, so their FREE step stays in the log (steps_drop_blocks drops
 * only the steps of the blocks the try took itself; before, it dropped this one too). */
static void weights_refused_retry_keeps_a_resident_block_s_free_step(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    r.tiers = 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER1_NAME);
    line();
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == VJO_OK && weights_live && weight_allocations == 1u);
    const uint32_t small = weights_block;
    r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    available = (int)tier_need(&r, 2, 0); /* Normal's need, Small's resident weights counted as free */
    allocations = 0;
    drop_once = 4096;
    drop_at_allocation = 1;
    int rc = vjo_game_ocr_run_request(&r, &out);
    unsigned freed = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        freed += out.step[i].id == VJO_GSTEP_FREE && out.step[i].arg == VJO_GITEM_WEIGHTS && !out.step[i].rc &&
                 out.step[i].bytes == small;
    TEST_CHECK_(rc == VJO_OK && out.fallback == 1u && weight_allocations == 2u && weight_frees == 1u &&
                    out.weights == VJO_GAME_OCR_WEIGHTS_READ,
                "rc %d, fallback %u, %u weight blocks taken, %u freed", rc, (unsigned)out.fallback, weight_allocations,
                weight_frees);
    TEST_CHECK_(freed == 1u, "the given-back weights' FREE steps in the log: %u (of %u steps)", freed,
                (unsigned)out.n_steps);
    TEST_CHECK(!memory_live && !capture_live);
    TEST_CHECK(!drop_resident());
}

/* #382's audit N3: a job cancelled as its refused weights' file closes ends cancelled: it checks and takes nothing
 * for the next model (the retry's !cancelled), and Small's file is never opened. */
static void weights_refused_then_cancelled_retries_nothing(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
    r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    line();
    available = (int)tier_need(&r, 2, 0);
    drop_once = 4096;
    drop_at_allocation = 1;
    close_at = 1;
    close_cancels = 1;
    int rc = vjo_game_ocr_run_request(&r, &out);
    const unsigned checks = check_steps(&out, s);
    TEST_CHECK_(rc == VJO_E_CANCELLED && checks == 1u && allocations == 1u && !tier_opens[1],
                "rc %d, %u checks, %u workspaces, Small opened %u times", rc, checks, allocations, tier_opens[1]);
    TEST_CHECK(!memory_live && !capture_live && !weights_live);
}

/* #382's audit N3, the reverse order of the tries: the rows' try's Normal weights are refused, and as their file
 * closes the screen turns to columns; Small's try (still a checked one: the retry keeps vertical and check) finds
 * that its rows look like vertical text, the columns cannot be read (no vertical model: ENOENT), and the rows' last
 * try reads them unchecked. The job is one: one PASS and SIG, every rows call a capture, at most 20 steps; the first
 * try's two checks (Normal's passed, Small's) leave the log with its blocks as it hands over to the columns (#385's
 * audit F7): the columns' check and the last try's, Normal's refused and Small's, say what was read. */
static void weights_refused_then_auto_s_tries(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
    r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    r.speed = VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    tall_columns_frame(&r);
    memset(pixels, 0, sizeof(pixels)); /* first, two lines of horizontal text in the tall region */
    for (unsigned y = 10; y < 30; y++)
        for (unsigned x = 10; x < 190; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
    for (unsigned y = 36; y < 54; y++)
        for (unsigned x = 10; x < 120; x += 6) memset(pixels + ((size_t)y * 200u + x) * 4u, 235, 8);
    vertical_absent = 1;
    missing_open_rc = (int)0x80010002u; /* ENOENT */
    TEST_ASSERT(tier_need(&r, 2, 0) - 4096u >= tier_need(&r, 1, 0));
    available = (int)tier_need(&r, 2, 0);
    drop_once = 4096;
    drop_at_allocation = 1;
    close_at = 1;
    close_columns = 1;
    int rc = vjo_game_ocr_run_request(&r, &out);
    const unsigned checks = check_steps(&out, s);
    TEST_CHECK_(rc == VJO_OK && out.fallback == 1u && find_steps(&out, 2) == 1 && find_steps(&out, 3) == 1 &&
                    vertical_opens == 1 && columns_bad,
                "rc %d, fallback %u, find steps of kinds 2 and 3: %u, %u; %u vertical opens, columns_bad %d", rc,
                (unsigned)out.fallback, find_steps(&out, 2), find_steps(&out, 3), vertical_opens, columns_bad);
    TEST_CHECK_(checks == 3u && tier_opens[2] == 1 && tier_opens[1] == 1, "%u checks (V | N, S), opens N %u S %u",
                checks, tier_opens[2], tier_opens[1]); /* #385's audit F7: the hand-over try's N and S leave the log */
    TEST_CHECK_(count_steps(&out, VJO_GSTEP_PASS) == 1 && count_steps(&out, VJO_GSTEP_SIG) == 1 &&
                    out.n_steps <= VJO_GAME_OCR_STEPS && out.captures == rows_calls,
                "PASS %u, SIG %u, %u steps, %u captures of %u rows calls", count_steps(&out, VJO_GSTEP_PASS),
                count_steps(&out, VJO_GSTEP_SIG), (unsigned)out.n_steps, (unsigned)out.captures, rows_calls);
    TEST_CHECK(!memory_live && !capture_live && !line_live);
    TEST_CHECK(!drop_resident());
}

/* Whether the Large size's file is here (a request by every tier reads its rows with it first): the rows' result. */
static int rows_rc_here(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_TIER3_NAME);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f ? VJO_OK : VJO_E_OCR_MODEL;
}

static const VjoGameOcrStep *recognize_step(const VjoGameOcrResult *out)
{
    for (unsigned i = 0; i < out->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out->step[i].id == VJO_GSTEP_RECOGNIZE) return &out->step[i];
    return NULL;
}

/* API 47, auto, the tall-line block (game_ocr/main.c line_buffer, "VjoGameVocrLine") across the retry: with the
 * blank-line check (ocr_skip_blank_lines) the columns' try fetches its first column, taller than a strip, through
 * the block before it reads the vertical model's weights; that file absent, the job reads the rows instead, and the
 * columns' try gives the block back first (cleanup): the rows' try takes its workspace with no tall-line block live,
 * and the job ends with none, the ledger's VjoGameVocrLine as before. No release file needed: CI runs it. */
static void auto_retry_frees_the_line_block(void)
{
    const int rows_rc = rows_rc_here();
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.flags = VJO_GAME_OCR_F_BLANK;
    r.speed = VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    tall_columns_frame(&r);
    vertical_absent = 1;
    missing_open_rc = (int)0x80010002u; /* ENOENT */
    const uint32_t ledger_line = vjo_ledger_current(&g_game_ledger, GL_LINE);
    int rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK_(rc == rows_rc && find_steps(&out, 2) == 1 && find_steps(&out, 3) == 1 && columns_bad,
                "rc %d (the rows' %d), %u and %u steps of kinds 2 and 3, columns_bad %d", rc, rows_rc,
                find_steps(&out, 2), find_steps(&out, 3), columns_bad);
    TEST_CHECK_(line_allocs - line_refused == 1 && line_frees == 1 && !line_live,
                "tall-line blocks: %u taken, %u freed, live %d", line_allocs - line_refused, line_frees, line_live);
    TEST_CHECK_(!workspace_beside_line, "%u workspace blocks taken beside the columns' tall-line block",
                workspace_beside_line);
    TEST_CHECK_(vjo_ledger_current(&g_game_ledger, GL_LINE) == ledger_line, "GL_LINE %u B, %u before the job",
                (unsigned)vjo_ledger_current(&g_game_ledger, GL_LINE), (unsigned)ledger_line);
    TEST_CHECK(!memory_live && !capture_live && !out.cleanup_status);
}

/* API 47, a job of columns (ocr_vertical = on; a region at least as tall as wide is read as columns from the job's
 * first try, #385's audit F6): its columns,
 * taller than a strip, go through the tall-line block, which the job gives back at its end, whatever its result (with
 * the blank-line check the columns are fetched before the vertical model's weights are read: without that file the
 * rows are read, with it the columns), so the next job's workspace is never taken beside it. #379: the columns are
 * fetched together, one block for the batch. Refused on every memory type, after the job's memory check passed, the
 * batch's block is asked for once and each column's once more (each refused too), and each column is left out (the
 * core's tall_skipped; the recognize and find steps report it while the step log has room: each refused rung is a
 * step of its own), and the job ends normally with no text: a failed
 * allocation degrades (AGENTS.md rule 3). No release file needed: CI runs it. */
static void columns_job_line_block_freed_or_refused(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : ".", VJO_VOCR_VERTICAL_NAME);
    FILE *f = fopen(path, "rb");
    const int have_v = f != NULL;
    if (f) fclose(f);
    for (int refuse = 0; refuse <= 1; refuse++) {
        VjoGameOcrRequest r = setup();
        VjoGameOcrResult out;
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.flags = VJO_GAME_OCR_F_BLANK;
        r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
        tall_columns_frame(&r);
        refuse_line = refuse;
        const uint32_t ledger_line = vjo_ledger_current(&g_game_ledger, GL_LINE);
        for (unsigned job = 0; job < 2; job++) {
            int rc = vjo_game_ocr_run_request(&r, &out);
            const VjoGameOcrStep *s = recognize_step(&out);
            /* the columns' try is the job's last unless it reads the vertical model and that file is absent (CI):
             * then the rows' try after it is (find kind 3, the frame's 7 rows) */
            const int columns_last = have_v || refuse;
            TEST_CHECK_(columns_last ? (find_steps(&out, 1) == 1 || (refuse && out.n_steps == VJO_GAME_OCR_STEPS)) &&
                                           out.lines == 3
                                     : find_steps(&out, 3) == 1 && out.lines == 7,
                        "refuse %d, job %u: %u column finds, %u lines", refuse, job, find_steps(&out, 1),
                        (unsigned)out.lines);
            TEST_CHECK(!find_steps(&out, 2)); /* #385's audit F6: a tall region's columns from the first try */
            if (refuse) { /* every column left out (the batch, then each column, asked for once), nothing recognized,
                           * no error; the recognize step counts them as tall_skipped when the log kept it */
                TEST_CHECK_(rc == VJO_OK && !out.text[0] && !out.recognized && (!s || (s->sum == 3 && !s->arg)),
                            "refused, job %u: rc %d, %u read, tall_skipped %d", job, rc, (unsigned)out.recognized,
                            s ? (int)s->sum : -1);
                TEST_CHECK_(line_asks == 4u * (job + 1u) && line_refused == line_allocs && !line_frees,
                            "refused, job %u: %u asks, %u of %u refused, %u freed", job, line_asks, line_refused,
                            line_allocs, line_frees);
            } else {
                TEST_CHECK_(have_v ? rc == VJO_OK && out.recognized == 3 : rc == rows_rc_here() && find_steps(&out, 3) == 1,
                            "job %u: rc %d, %u read (vertical model %s)", job, rc, (unsigned)out.recognized,
                            have_v ? "here" : "absent");
                TEST_CHECK_(line_allocs - line_refused >= job + 1u && line_frees == line_allocs - line_refused,
                            "job %u: tall-line blocks %u taken, %u freed", job, line_allocs - line_refused, line_frees);
            }
            TEST_CHECK_(!line_live && !workspace_beside_line, "job %u: line block live %d, %u workspace blocks beside one",
                        job, line_live, workspace_beside_line);
            TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_LINE) == ledger_line);
            TEST_CHECK(!memory_live && !capture_live && !out.cleanup_status);
        }
    }
}

/* #385's audit F3: on, a drawn horizontal box that empties between lines (Read text: Automatically's jobs: text, an
 * empty box, text, ...). A region wider than tall where the finder finds no row is off's job: nothing to read and no
 * columns' try, so the vertical model is never opened and the size's model, the LM and the cascade's model stay
 * resident: each job's result is off's, and so are its weight, LM and cascade reads (one each in the six jobs, with
 * the release files; the first text's). Before, each empty box went to the columns' try, which gave back the size's
 * model and the cascade's, read the vertical model's weights (its warm-up), and the next text read the size's model
 * and the cascade's again: 6 weight reads and 3 cascade reads in 6 jobs, against off's 1 and 1. No release file
 * needed for the steps and opens: CI runs it. */
static void on_wide_empty_box_is_off_s_job(void)
{
    static VjoGameOcrResult res[2][6];
    unsigned reads[2] = {0, 0}, lm_reads[2] = {0, 0}, cascade_reads[2] = {0, 0};
    for (int mode = 0; mode <= 1; mode++) { /* off, then on */
        VjoGameOcrRequest r = setup();
        r.tiers = 1u << (VJO_VOCR_TIER_NORMAL - 1u) | 1u << (VJO_VOCR_TIER_SMALL - 1u);
        snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
        r.flags = 1u << VJO_GAME_OCR_LM_SHIFT | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
        r.speed = (mode ? VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT : 0u) | VJO_GAME_OCR_S_NOCACHE;
        available = 40 * 1024 * 1024; /* Normal, the LM and Large's model as the cascade, beside Large's reserve */
        test_cascade_conf = 1001;     /* every line doubtful: the cascade reads its model at the first text */
        for (unsigned j = 0; j < 6; j++) {
            if (j % 2) memset(pixels, 0, sizeof(pixels));
            else box_of_two_lines();
            const unsigned w0 = weight_allocations, l0 = lm_allocations, c0 = cascade_allocations;
            VjoGameOcrResult *out = &res[mode][j];
            (void)vjo_game_ocr_run_request(&r, out);
            reads[mode] += weight_allocations - w0;
            lm_reads[mode] += lm_allocations - l0;
            cascade_reads[mode] += cascade_allocations - c0;
            TEST_CHECK_(!find_steps(out, 2) && find_steps(out, 4) == (unsigned)mode && !vertical_opens,
                        "%s, job %u (%s): %u and %u find steps of kinds 2 and 4, %u vertical opens", mode ? "on" : "off",
                        j, j % 2 ? "empty" : "text", find_steps(out, 2), find_steps(out, 4), vertical_opens);
            TEST_CHECK(!memory_live && !capture_live && !line_live);
        }
        TEST_CHECK(!drop_resident());
    }
    for (unsigned j = 0; j < 6; j++)
        TEST_CHECK_(res[1][j].rc == res[0][j].rc && res[1][j].lines == res[0][j].lines &&
                        res[1][j].recognized == res[0][j].recognized && !strcmp(res[1][j].text, res[0][j].text) &&
                        res[1][j].weights == res[0][j].weights && res[1][j].n_steps == res[0][j].n_steps,
                    "job %u: on rc %d, %u lines, weights %u, %u steps; off rc %d, %u lines, weights %u, %u steps", j,
                    res[1][j].rc, res[1][j].lines, (unsigned)res[1][j].weights, (unsigned)res[1][j].n_steps,
                    res[0][j].rc, res[0][j].lines, (unsigned)res[0][j].weights, (unsigned)res[0][j].n_steps);
    TEST_CHECK_(reads[1] == reads[0] && lm_reads[1] == lm_reads[0] && cascade_reads[1] == cascade_reads[0],
                "weight, LM and cascade blocks read in 6 jobs: on %u, %u, %u; off %u, %u, %u", reads[1], lm_reads[1],
                cascade_reads[1], reads[0], lm_reads[0], cascade_reads[0]);
    if (VJO_VOCR_MODEL_DIR[0])
        TEST_CHECK_(reads[0] == 1 && lm_reads[0] == 1 && cascade_reads[0] == 1, "off read %u, %u, %u", reads[0],
                    lm_reads[0], cascade_reads[0]);
    test_cascade_conf = 0;
}

/* #385's audit F2: on, vertical text, the columns' memory check passes and then their weights are refused (the game
 * took memory once the columns' blocks were taken: #324's case, on a job of columns, which has no smaller model to try):
 * the job reads the rows instead (find kind 5, rc VJO_E_OOM, arg the columns it found), with the size that fits, and
 * its log keeps the rows' LOAD, RECOGNIZE and NORM steps. Both ways in: a region at least as tall as wide (its columns
 * first, #385's audit F6) and a wider one (its rows' try first, then the columns'), with and without the LM. Without
 * that arm (game_ocr/main.c: rc == VJO_E_OOM && vertical) the job failed for memory: rc -103, nothing read. */
static void on_columns_weights_refused_read_the_rows(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; the weights' own check is not reached"); return; }
    const VjoVocrModel *v = vjo_vocr_model(VJO_VOCR_VERTICAL_NAME);
    TEST_ASSERT(v != NULL);
    for (int wide = 0; wide <= 1; wide++)
        for (int lm = 0; lm <= 1; lm++) {
            VjoGameOcrRequest r = setup();
            VjoGameOcrResult out;
            r.tiers = VJO_VOCR_TIERS_ALL;
            snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
            r.flags = lm ? 1u << VJO_GAME_OCR_LM_SHIFT : 0u;
            r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
            if (wide) wide_columns_frame(&r);
            else tall_columns_frame(&r);
            const size_t extra = lm ? vjo_vocr_lm_job_bytes(v->name) + VJO_GAME_OCR_TEXT_BYTES : 0;
            const uint32_t need_v = PAGE_ROUND(vjo_vocr_job_bytes(v->name, 200, r.height) + extra) +
                                    PAGE_ROUND(vjo_vocr_capture_bytes(200, r.height)) + PAGE_ROUND(v->bytes) +
                                    (lm ? PAGE_ROUND(VJO_VOCR_LM1_BYTES) : 0u) + GAME_RESERVE;
            const uint32_t need_s = tier_need(&r, 1, 0); /* Small's rows without the LM: the last try's */
            TEST_ASSERT(need_s + 4096u <= need_v);
            available = (int)(need_v + 4096u); /* the columns' check passes */
            drop_once = (int)(need_v - need_s); /* then, at their workspace, the game takes this: Small's rows still
                                                 * fit, the vertical model's weights not beside the columns' blocks */
            drop_at_allocation = wide ? 2u : 1u; /* (a wide region's rows' try took the first workspace) */
            int rc = vjo_game_ocr_run_request(&r, &out);
            const VjoGameOcrStep *five = NULL;
            for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
                if (out.step[i].id == VJO_GSTEP_FIND && out.step[i].kind == 5) five = &out.step[i];
            TEST_CHECK_(rc == VJO_OK && find_steps(&out, 2) == (unsigned)wide && five && five->rc == VJO_E_OOM &&
                            five->arg == 3u && find_steps(&out, 0) == 1 && out.lines == (wide ? 6u : 7u) &&
                            out.recognized == out.lines && vertical_opens == 1 && out.fallback == 2u,
                        "%s, lm %d: rc %d, find kinds 2/5/0 %u/%u/%u (kind 5: rc %d, %u columns), %u lines, %u read, "
                        "%u vertical opens, fallback %u", wide ? "wide" : "tall", lm, rc, find_steps(&out, 2),
                        find_steps(&out, 5), find_steps(&out, 0), five ? (int)five->rc : 0, five ? five->arg : 0u,
                        (unsigned)out.lines, (unsigned)out.recognized, vertical_opens, (unsigned)out.fallback);
            for (unsigned i = 0; i < out.recognized; i++) TEST_CHECK(!(out.line[i].flags & VJO_GAME_OCR_LINE_COLUMN));
            TEST_CHECK_(count_steps(&out, VJO_GSTEP_LOAD) == 1 && count_steps(&out, VJO_GSTEP_RECOGNIZE) == 1 &&
                            count_steps(&out, VJO_GSTEP_NORM) == (out.text[0] ? 1u : 0u) &&
                            count_steps(&out, VJO_GSTEP_PASS) == 1 && count_steps(&out, VJO_GSTEP_SIG) == 1 &&
                            out.n_steps <= VJO_GAME_OCR_STEPS,
                        "%s, lm %d: %u steps: LOAD %u, RECOGNIZE %u, NORM %u, PASS %u, SIG %u", wide ? "wide" : "tall",
                        lm, (unsigned)out.n_steps, count_steps(&out, VJO_GSTEP_LOAD),
                        count_steps(&out, VJO_GSTEP_RECOGNIZE), count_steps(&out, VJO_GSTEP_NORM),
                        count_steps(&out, VJO_GSTEP_PASS), count_steps(&out, VJO_GSTEP_SIG));
            TEST_CHECK(!memory_live && !capture_live && !line_live && !out.cleanup_status);
            TEST_CHECK(!drop_resident());
        }
}

/* #385's audit F7: the step log of on's two tries. Vertical text in a region wider than tall, with a waiting job's flags
 * (a background read's, or since #413's audit a press's retry after a torn read: the LM's first tier, the cascade, a
 * 150 ms wait, two helpers, the blank-line check): the rows' try hands the job to the
 * columns' try (auto's on the tall region the same). Its memory checks leave the log with its blocks (it read
 * nothing, and a later try of rows checks the sizes again), so no CHECK step comes before its find step of kind 2,
 * and the columns' try keeps every step a job of the vertical model alone logs on the same frame (its check, blocks,
 * wait, LOAD, RECOGNIZE, LM, NORM and PAR); the rows' try adds its find step only. Before, the rows' try's two checks
 * (Large refused, Normal passed) took NORM's and PAR's places in the 20 steps. Without the release files the columns'
 * try cannot read the vertical model and the rows' try after it reads: the same ordering, one PASS and SIG. */
static void on_tries_keep_the_last_try_s_steps(void)
{
    static VjoGameOcrResult out, alone;
    const uint32_t press = 1u << VJO_GAME_OCR_LM_SHIFT | 15u << VJO_GAME_OCR_SETTLE_SHIFT | VJO_GAME_OCR_F_BLANK;
    for (int mode = 0; mode <= 1; mode++) { /* on in a region wider than tall, auto in the tall one */
        const char *name = mode ? "auto" : "on";
        VjoGameOcrRequest r = setup();
        r.tiers = VJO_VOCR_TIERS_ALL;
        snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
        r.flags = press | 8u << VJO_GAME_OCR_CASCADE_SHIFT;
        r.speed = 2u | (mode ? VJO_GAME_OCR_VERTICAL_AUTO : VJO_GAME_OCR_VERTICAL_ON) << VJO_GAME_OCR_S_VERTICAL_SHIFT;
        if (mode) tall_columns_frame(&r);
        else wide_columns_frame(&r);
        (void)vjo_game_ocr_run_request(&r, &out);
        unsigned first_check = VJO_GAME_OCR_STEPS, find2 = VJO_GAME_OCR_STEPS;
        for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
            if (out.step[i].id == VJO_GSTEP_CHECK && first_check == VJO_GAME_OCR_STEPS) first_check = i;
            if (out.step[i].id == VJO_GSTEP_FIND && out.step[i].kind == 2) find2 = i;
        }
        TEST_CHECK_(find2 < VJO_GAME_OCR_STEPS && first_check > find2 && count_steps(&out, VJO_GSTEP_PASS) == 1 &&
                        count_steps(&out, VJO_GSTEP_SIG) == 1 && out.n_steps <= VJO_GAME_OCR_STEPS,
                    "%s: %u steps: the first CHECK at %u, find kind 2 at %u; PASS %u, SIG %u", name,
                    (unsigned)out.n_steps, first_check, find2, count_steps(&out, VJO_GSTEP_PASS),
                    count_steps(&out, VJO_GSTEP_SIG));
        TEST_CHECK(!memory_live && !capture_live && !line_live);
        if (!VJO_VOCR_MODEL_DIR[0]) continue;
        TEST_CHECK_(out.rc == VJO_OK && find_steps(&out, 1) == 1 && out.recognized == 3 && out.text[0],
                    "%s: rc %d, %u columns read, text %d", name, out.rc, (unsigned)out.recognized, out.text[0] != 0);
        TEST_CHECK(!drop_resident());
        /* the columns' try's own steps: a job of the vertical model alone, by name, the same flags without the
         * cascade (which a job of columns never takes) */
        const unsigned h = r.height;
        r = setup();
        r.tiers = 0;
        snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_VERTICAL_NAME);
        snprintf(model_path, sizeof(model_path), "%s/%s", r.model_dir, r.model); /* its open by name */
        r.flags = press;
        r.speed = 2u;
        if (h == 232u) tall_columns_frame(&r);
        else wide_columns_frame(&r);
        (void)vjo_game_ocr_run_request(&r, &alone);
        TEST_CHECK_(alone.rc == VJO_OK && !strcmp(alone.text, out.text) && alone.n_steps < VJO_GAME_OCR_STEPS,
                    "%s, alone: rc %d, %u steps, the same text %d", name, alone.rc, (unsigned)alone.n_steps,
                    !strcmp(alone.text, out.text));
        for (unsigned id = VJO_GSTEP_MEM; id < VJO_GSTEP_COUNT; id++)
            TEST_CHECK_(count_steps(&out, id) == count_steps(&alone, id) + (id == VJO_GSTEP_FIND),
                        "%s, step %u: %u, the columns alone %u", name, id, count_steps(&out, id),
                        count_steps(&alone, id));
        TEST_CHECK(count_steps(&out, VJO_GSTEP_NORM) == 1 && count_steps(&out, VJO_GSTEP_PAR) == 1 &&
                   count_steps(&out, VJO_GSTEP_LM) == 1 && count_steps(&out, VJO_GSTEP_SETTLE) == 1);
        TEST_CHECK(!drop_resident());
    }
}

/* #385's audit F5 (AGENTS.md rule 3): a job of columns' rectangle (#379: the tall-line block for its columns together)
 * is taken only from what the game has free beyond the job's reserve and its blocks still to come. At a free memory
 * where the columns' check passes with a byte less than the rectangle's pages to spare, the rectangle is refused and the
 * columns are read one by one, each in a smaller block of its own: the same text as with room. With the blank-line
 * check the rectangle is fetched before the vertical model's weights are read, and before it took their room: they
 * were refused and the job read the columns' text as rows (find kind 5). Without the check the weights come first,
 * and the rectangle cut into the reserve. No release file needed for the blocks: CI runs it (with them, the text). */
static void columns_rectangle_keeps_the_reserve(void)
{
    const VjoVocrModel *v = vjo_vocr_model(VJO_VOCR_VERTICAL_NAME);
    static VjoGameOcrResult roomy, out;
    TEST_ASSERT(v != NULL);
    /* the rectangle, with room: the first block the job asks for (with the check, fetched before any weights) */
    VjoGameOcrRequest r = setup();
    r.flags = VJO_GAME_OCR_F_BLANK;
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_NOCACHE;
    tall_columns_frame(&r);
    (void)vjo_game_ocr_run_request(&r, &roomy);
    const uint32_t rect = line_block_max;
    TEST_ASSERT_(rect > VJO_GAME_LINE_UNCHECKED + 4096u, "the rectangle: %u B", rect);
    TEST_CHECK(!drop_resident());
    const uint32_t need = PAGE_ROUND(vjo_vocr_job_bytes(v->name, 200, 232)) + PAGE_ROUND(vjo_vocr_capture_bytes(200, 232)) +
                          PAGE_ROUND(v->bytes) + GAME_RESERVE;
    for (int blank = 0; blank <= 1; blank++) {
        static VjoGameOcrResult with_room;
        r = setup();
        r.flags = blank ? VJO_GAME_OCR_F_BLANK : 0u;
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_NOCACHE;
        tall_columns_frame(&r);
        (void)vjo_game_ocr_run_request(&r, &with_room); /* the same flags with room: the text to read */
        TEST_CHECK(!drop_resident());
        r = setup();
        r.flags = blank ? VJO_GAME_OCR_F_BLANK : 0u;
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_NOCACHE;
        tall_columns_frame(&r);
        available = (int)(need + rect - 1u); /* a byte short: the rectangle's block is page-rounded (#411's audit) */
        const uint32_t ledger_line = vjo_ledger_current(&g_game_ledger, GL_LINE);
        int rc = vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK_(line_block_max < rect && !find_steps(&out, 5),
                    "blank check %d: the largest tall-line block %u B (the rectangle %u B), %u find steps of kind 5",
                    blank, line_block_max, rect, find_steps(&out, 5));
        TEST_CHECK(!line_live && vjo_ledger_current(&g_game_ledger, GL_LINE) == ledger_line);
        TEST_CHECK(!memory_live && !capture_live && !out.cleanup_status);
        if (VJO_VOCR_MODEL_DIR[0]) { /* a column's block (each next one reuses it), the text the same as with room */
            TEST_CHECK_(rc == VJO_OK && find_steps(&out, 1) == 1 && out.recognized == 3 && line_allocs >= 1u &&
                            line_block_max > 0 && !strcmp(out.text, with_room.text) && with_room.recognized == 3,
                        "blank check %d: rc %d, %u columns read, %u tall-line blocks (the largest %u B), the same text %d",
                        blank, rc, (unsigned)out.recognized, line_allocs, line_block_max, !strcmp(out.text, with_room.text));
        }
        TEST_CHECK(!drop_resident());
    }
}

/* #411's audit F2 (B2): "at least as tall as wide" includes a square region: with on, a 200 x 200 box of columns is read
 * as columns from the job's first try (its first memory check is the vertical model's, by name), with no rows' try (no
 * find step of kind 2 or 4). Without the release file the columns' try cannot read the vertical model and the rows are
 * read after it, the same ordering. No release file needed. */
static void on_square_region_reads_columns_at_once(void)
{
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    frame_h = r.fb_height = r.height = 200;
    columns_pixels(6);
    (void)vjo_game_ocr_run_request(&r, &out);
    const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS] = {NULL};
    const unsigned n = check_steps(&out, s);
    TEST_CHECK_(!find_steps(&out, 2) && !find_steps(&out, 4) && vertical_opens == 1 && n >= 1 &&
                    !VJO_GCHECK_TIER(s[0]->kind),
                "square: find steps of kinds 2 and 4: %u, %u; %u vertical opens; the first check's tier %u", find_steps(&out, 2),
                find_steps(&out, 4), vertical_opens, n ? VJO_GCHECK_TIER(s[0]->kind) : 99u);
    TEST_CHECK(!memory_live && !capture_live && !line_live);
    TEST_CHECK(!drop_resident());
}

/* #411's audit F2 (B3): a job of columns' rectangle keeps the room of its LM too (game_ocr/main.c line_buffer). With the
 * blank-line check the weights and the LM are read after the rectangle is fetched; at a free memory a page short of the
 * rectangle beside the vertical model's weights and the LM (the columns' check passes), the rectangle is refused and the
 * columns read one by one, and the LM is read: the same text as with room. With room for all three the rectangle is
 * taken. Without the LM's term the rectangle was taken and the LM refused (rc VJO_E_OOM, a greedy reading). */
static void columns_rectangle_keeps_the_lm_s_room(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent: the LM is not read"); return; }
    const VjoVocrModel *v = vjo_vocr_model(VJO_VOCR_VERTICAL_NAME);
    static VjoGameOcrResult roomy, out;
    TEST_ASSERT(v != NULL);
    VjoGameOcrRequest r = setup();
    r.flags = VJO_GAME_OCR_F_BLANK | 1u << VJO_GAME_OCR_LM_SHIFT;
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_NOCACHE;
    tall_columns_frame(&r);
    (void)vjo_game_ocr_run_request(&r, &roomy);
    const uint32_t rect = line_block_max;
    TEST_ASSERT_(rect > VJO_GAME_LINE_UNCHECKED + 4096u && roomy.recognized == 3, "the rectangle %u B, %u read", rect,
                 (unsigned)roomy.recognized);
    TEST_CHECK(!drop_resident());
    const size_t extra = vjo_vocr_lm_job_bytes(v->name) + VJO_GAME_OCR_TEXT_BYTES;
    const uint32_t need = PAGE_ROUND(vjo_vocr_job_bytes(v->name, 200, 232) + extra) +
                          PAGE_ROUND(vjo_vocr_capture_bytes(200, 232)) + PAGE_ROUND(v->bytes) +
                          PAGE_ROUND(VJO_VOCR_LM1_BYTES) + GAME_RESERVE;
    for (int shortfall = 0; shortfall <= 1; shortfall++) {
        r = setup();
        r.flags = VJO_GAME_OCR_F_BLANK | 1u << VJO_GAME_OCR_LM_SHIFT;
        r.tiers = VJO_VOCR_TIERS_ALL;
        r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_NOCACHE;
        tall_columns_frame(&r);
        available = (int)(need + rect - (shortfall ? 4096u : 0u));
        const int rc = vjo_game_ocr_run_request(&r, &out);
        int lm_rc = 1;
        for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
            if (out.step[i].id == VJO_GSTEP_LM) lm_rc = out.step[i].rc;
        TEST_CHECK_(rc == VJO_OK && out.recognized == 3 && lm_allocations == 1u && !lm_rc &&
                        (shortfall ? line_block_max < rect : line_block_max == rect) && !strcmp(out.text, roomy.text),
                    "%s: rc %d, %u read, LM blocks %u (its step's rc %d), the largest tall-line block %u B (the rectangle "
                    "%u B), the same text %d", shortfall ? "a page short" : "with room", rc, (unsigned)out.recognized,
                    lm_allocations, lm_rc, line_block_max, rect, !strcmp(out.text, roomy.text));
        TEST_CHECK(!memory_live && !capture_live && !line_live);
        TEST_CHECK(!drop_resident());
    }
}

/* #411's audit F2 (B14): #385's audit F6 keys on the job's check, so a game whose vertical model's file failed
 * (columns_bad: the file absent, ENOENT) reads its later tall regions' rows with on, unchecked, and never opens the
 * vertical model again (keyed on the setting it would open it, and log find kind 3, at every job). No release file
 * needed. */
static void on_tall_region_after_a_bad_vertical_file_reads_rows(void)
{
    VjoGameOcrRequest r = setup();
    VjoGameOcrResult out;
    r.tiers = VJO_VOCR_TIERS_ALL;
    r.speed = VJO_GAME_OCR_VERTICAL_ON << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    tall_columns_frame(&r);
    vertical_absent = 1;
    missing_open_rc = (int)0x80010002u; /* ENOENT: an absent file (latches columns_bad) */
    for (unsigned job = 0; job < 3; job++) { /* no setup() between them: the game's worker keeps columns_bad */
        const unsigned opens = vertical_opens;
        (void)vjo_game_ocr_run_request(&r, &out);
        TEST_CHECK_(vertical_opens - opens == (job ? 0u : 1u) && find_steps(&out, 3) == (job ? 0u : 1u) &&
                        !find_steps(&out, 1) && !find_steps(&out, 2) && columns_bad,
                    "job %u: %u vertical opens, find steps of kinds 3, 1 and 2: %u, %u, %u; columns_bad %d", job,
                    vertical_opens - opens, find_steps(&out, 3), find_steps(&out, 1), find_steps(&out, 2), columns_bad);
        TEST_CHECK(!memory_live && !capture_live && !line_live);
    }
    TEST_CHECK(!drop_resident());
}

/* The cascade (#89) reads doubtful lines again with the next size up, when that
 * fits too: Small's is Normal's model, Normal's Large's, Large has none; a size the
 * request leaves out below its largest is no cascade. The kernel opens the tier
 * above the largest asked for (kernel/game_ocr.c tier_allowed). A job holding the
 * cascade's larger block keeps the same 1 MiB reserve. A request by name has
 * VOCR4-W20 beside a smaller model, as before. No files needed. */
static void tier_cascade_is_the_next_size_up(void)
{
    VjoGameOcrRequest r = setup();
    const VjoVocrModel *small = vjo_vocr_model(VJO_VOCR_TIER1_NAME), *normal = vjo_vocr_model(VJO_VOCR_TIER2_NAME),
                       *large = vjo_vocr_model(VJO_VOCR_TIER3_NAME), *w20 = vjo_vocr_model(VJO_VOCR_CASCADE_NAME);
    Candidate s = {small, 1, VJO_GAME_OCR_OPEN_TIER, 0}, n = {normal, 2, VJO_GAME_OCR_OPEN_TIER + 1u, 0},
              l = {large, 3, VJO_GAME_OCR_OPEN_TIER + 2u, 0}, byname = {small, 0, VJO_GAME_OCR_OPEN_MODEL, 0};
    TEST_ASSERT(small && normal && large && w20);
    /* Normal's cascade: Large's model, unless Normal kept it (one model for two sizes: no cascade,
     * game_vocr_shared_tiers) */
    const VjoVocrModel *above_n = normal != large ? large : NULL;
    TEST_CHECK(small->bytes < normal->bytes && normal->bytes <= large->bytes && large->bytes == VJO_VOCR_MAX_BYTES);
    r.tiers = 7u;
    TEST_CHECK(cascade_of(&s, &r, 3, 1) == normal && cascade_of(&n, &r, 3, 1) == above_n && !cascade_of(&l, &r, 3, 1));
    TEST_CHECK(!cascade_of(&s, &r, 3, 0) && !cascade_of(&n, &r, 3, 0)); /* no ceiling: no cascade */
    r.tiers = 5u; /* Normal left out */
    TEST_CHECK(!cascade_of(&s, &r, 3, 1) && !cascade_of(&l, &r, 3, 1));
    r.tiers = 1u; /* Small chosen: Normal's model above it */
    TEST_CHECK(cascade_of(&s, &r, 1, 1) == normal);
    r.tiers = 3u; /* Normal chosen: Large's model above it */
    TEST_CHECK(cascade_of(&n, &r, 2, 1) == above_n && cascade_of(&s, &r, 2, 1) == normal);
    r.tiers = 4u;
    TEST_CHECK(!cascade_of(&l, &r, 3, 1));
    r.tiers = 0u; /* by name: VOCR4-W20 beside a smaller model, none beside the same or a larger one */
    TEST_CHECK(cascade_of(&byname, &r, 0, 1) == w20);
    byname.m = w20;
    TEST_CHECK(!cascade_of(&byname, &r, 0, 1)); /* not larger than the job's own */
    byname.m = large;
    TEST_CHECK(!cascade_of(&byname, &r, 0, 1)); /* VOCR4-W20 is smaller than the Large size's */
    /* the same fixed reserve, with or without a cascade */
    TEST_CHECK(check_reserve(1, NULL) == VJO_GAME_RESERVE_SMALL && check_reserve(1, normal) == VJO_GAME_RESERVE_NORMAL &&
               check_reserve(2, NULL) == VJO_GAME_RESERVE_NORMAL && check_reserve(2, large) == VJO_GAME_RESERVE_LARGE &&
               check_reserve(3, NULL) == VJO_GAME_RESERVE_LARGE && check_reserve(0, w20) == GAME_RESERVE &&
               check_reserve(0, NULL) == GAME_RESERVE);
    /* which file a path names: a request by tiers opens each model by its tier only, the job's own */
    tiered = 1;
    open_which = n.which;
    TEST_CHECK(which_file("ux0:data/VitaJPOverlay/ocr/" VJO_VOCR_TIER2_NAME) == VJO_GAME_OCR_OPEN_TIER + 1u);
    open_which = l.which;
    TEST_CHECK(which_file(VJO_VOCR_TIER3_NAME) == VJO_GAME_OCR_OPEN_TIER + 2u && which_file(VJO_VOCR_TIER1_NAME) ==
               VJO_GAME_OCR_OPEN_TIER);
    open_which = VJO_GAME_OCR_OPEN_MODEL;
    TEST_CHECK(which_file("dir/" VJO_VOCR_LM2_NAME) == VJO_GAME_OCR_OPEN_LM + 1u);
    TEST_CHECK(which_file("dir/H15_w8.vocr") == VJO_GAME_OCR_OPEN_TIER + VJO_VOCR_TIERS); /* refused by the kernel */
    tiered = 0; open_cascade = 1; open_which = VJO_GAME_OCR_OPEN_FALLBACK;
    TEST_CHECK(which_file("d/" VJO_VOCR_CASCADE_NAME) == VJO_GAME_OCR_OPEN_CASCADE && which_file("d/x.vocr") ==
               VJO_GAME_OCR_OPEN_FALLBACK);
    open_cascade = 0; open_which = VJO_GAME_OCR_OPEN_MODEL;
}

/* Two sizes may have one model (a size that kept the model of the size below
 * it): a job opens its model as its own tier, the one its request asks for; the
 * cascade's (the next size up, a larger model) and a request's other files as
 * the first tier with that name; no tier's name is no tier. Synthetic tables. */
static void two_sizes_with_one_model_open_as_the_job_s_own_tier(void)
{
    static const char *const shared[VJO_VOCR_TIERS] = {"S.vocr", "W.vocr", "W.vocr"};
    static const char *const apart[VJO_VOCR_TIERS] = {"S.vocr", "N.vocr", "L.vocr"};
    TEST_CHECK(tier_of_file(shared, 3, "W.vocr") == 3 && tier_of_file(shared, 2, "W.vocr") == 2);
    TEST_CHECK(tier_of_file(shared, 1, "W.vocr") == 2 && tier_of_file(shared, 0, "W.vocr") == 2); /* Small's cascade */
    TEST_CHECK(tier_of_file(shared, 1, "S.vocr") == 1 && tier_of_file(shared, 3, "S.vocr") == 1);
    TEST_CHECK(!tier_of_file(shared, 2, "N.vocr") && !tier_of_file(shared, 4, "X.vocr"));
    for (unsigned own = 0; own <= VJO_VOCR_TIERS; own++)
        for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++)
            TEST_CHECK_(tier_of_file(apart, own, apart[t - 1u]) == t, "own %u, tier %u", own, t);
}

/* The self-test's fallback stage, by tiers: the largest tier's check for the
 * record, then the smallest tier runs (its check forced); with Small alone there
 * is none smaller and Small runs as the stage's own. */
static void forced_fallback_by_tiers_runs_the_smallest(void)
{
    static const struct { uint32_t tiers; unsigned checks, first, fallback; } cases[] = {
        {7u, 2, 3, 2}, {3u, 2, 2, 1}, {6u, 2, 3, 1}, {1u, 1, 1, 0}, {4u, 1, 3, 0}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
        available = 64 * 1024 * 1024; /* room for every size: the stage still runs the smallest */
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        r.tiers = cases[c].tiers;
        r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK;
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        unsigned n = check_steps(&out, s);
        TEST_CHECK_(n == cases[c].checks && out.fallback == cases[c].fallback, "tiers 0x%X: %u checks, fallback %u",
                    cases[c].tiers, n, out.fallback);
        if (!n) continue;
        TEST_CHECK(VJO_GCHECK_TIER(s[0]->kind) == cases[c].first && !(s[0]->kind & VJO_GCHECK_FORCED) && !s[0]->rc);
        if (n == 2) {
            unsigned smallest = 0;
            while (!(cases[c].tiers & (1u << smallest))) smallest++;
            TEST_CHECK(VJO_GCHECK_TIER(s[1]->kind) == smallest + 1u && (s[1]->kind & VJO_GCHECK_FORCED) &&
                       s[1]->arg == 1 && !s[1]->rc && out.reserve_bytes == tier_reserve[smallest + 1u]);
        }
        for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_FALLBACK);
    }
}

/* Helpers share the fixed 1 MiB guard and never make a job step down a tier:
 * with room for Large alone it runs serially; with room for both helpers beside
 * the reserve it takes them without changing the model. */
static void helpers_keep_the_tier_reserve(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    const VjoGameOcrStep *par;
    line();
    r.tiers = 7u; /* Large chosen */
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    r.speed = 2u | VJO_GAME_OCR_S_NOCACHE;
    const VjoVocrModel *w20 = vjo_vocr_model(VJO_VOCR_TIER3_NAME);
    const size_t block = vjo_vocr_job_bytes(w20->name, r.width, r.height);
    const uint32_t large = tier_need(&r, 3, 0); /* Large's need with the 1 MiB reserve */
    const size_t two = helpers_need(w20->name, block, 2);
    TEST_ASSERT(two > 4096u);
    /* room for Large beside the 1 MiB reserve, not for its helpers too */
    TEST_CHECK(!drop_resident());
    available = (int)(live_bytes + large + 4096u);
    int rc = vjo_game_ocr_run_request(&r, &out);
    /* the job runs, serially: past the memory check it reads the model and recognizes the line, whatever
     * it reads the test's dashes as (without the release weights it stops there, VJO_E_OCR_MODEL; never
     * VJO_E_OOM, a refusal) */
    TEST_CHECK(VJO_VOCR_MODEL_DIR[0] ? rc == 0 && out.rc == VJO_OK && out.recognized >= 1 && out.frames > 0
                                     : out.rc == VJO_E_OCR_MODEL);
    TEST_MSG("rc %d, out.rc %d", rc, (int)out.rc);
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_LARGE && !out.fallback);
    TEST_CHECK(out.need_workspace + out.need_weights + out.reserve_bytes <= (uint32_t)large + 4096u);
    TEST_MSG("rc %d, reserve %u, fallback %u, need %u (Large alone %u)", rc, (unsigned)out.reserve_bytes,
             (unsigned)out.fallback, (unsigned)(out.need_workspace + out.need_weights + out.reserve_bytes), large);
    par = NULL;
    for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_PAR) par = &out.step[i];
    TEST_CHECK(!par || par->arg == 0); /* no helper taken */
    /* room for both helpers beside the 1 MiB reserve too: both taken, the same tier */
    TEST_CHECK(!drop_resident());
    available = (int)(live_bytes + large + two + 4096u);
    rc = vjo_game_ocr_run_request(&r, &out);
    /* the job runs, with both helpers: past the memory check it reads the model and recognizes the line
     * (without the release weights it stops there, VJO_E_OCR_MODEL; never VJO_E_OOM, a refusal) */
    TEST_CHECK(VJO_VOCR_MODEL_DIR[0] ? rc == 0 && out.rc == VJO_OK && out.recognized >= 1 && out.frames > 0
                                     : out.rc == VJO_E_OCR_MODEL);
    TEST_MSG("rc %d, out.rc %d", rc, (int)out.rc);
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_LARGE && !out.fallback);
    TEST_CHECK(out.need_workspace + out.need_weights + out.reserve_bytes == (uint32_t)(large + two));
    TEST_MSG("rc %d, need %u, Large alone %u + two helpers %u", rc,
             (unsigned)(out.need_workspace + out.need_weights + out.reserve_bytes), large, (unsigned)two);
    r.speed = 0;
}

/* With the release weights: a job opens its tier's model and the cascade's (the
 * next size up) by their tiers; each block is resident until a job needs another;
 * a model file that cannot be opened ends the job with VJO_E_OCR_MODEL and says
 * which tier failed (out.fallback), for the Shell to leave it out. And a job that
 * went down three checks keeps its refinement and normalisation steps, and (#280's
 * audit F5, through #324's retry) its LM step too. */
static void tier_jobs_open_their_models_by_tier(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
        char path[600];
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, tier_names[t - 1u]);
        if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(path, "rb"))) { TEST_SKIP("no %s beside the release weights", tier_names[t - 1u]); return; }
        fclose(f);
    }
    const VjoVocrModel *h15 = vjo_vocr_model(VJO_VOCR_TIER2_NAME), *w20 = vjo_vocr_model(VJO_VOCR_TIER3_NAME);
    /* Normal's cascade is Large's model, unless Normal kept that model (then none: game_vocr_shared_tiers) */
    const unsigned apart = h15 != w20;
    /* Normal chosen (Normal, then Small), the cascade on: Normal reads, Large's model re-reads */
    line();
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001;
    r.tiers = 3u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    r.flags = 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    /* the line read, whatever the models read the test's dashes as */
    TEST_CHECK(out.recognized >= 1 && out.frames > 0 && !out.fallback &&
               out.reserve_bytes == (apart ? VJO_GAME_RESERVE_LARGE : VJO_GAME_RESERVE_NORMAL));
    TEST_CHECK(tier_opens[2] == 1 && tier_opens[3] == apart && !tier_opens[1] && !cascade_opens);
    TEST_CHECK(resident.model == h15 && weights_live && (apart ? cascade_res.model == w20 && cascade_live : !cascade_live));
    TEST_CHECK(out.recognized >= 1 && !(out.line[0].flags & VJO_VOCR_LINE_CASCADE) == !apart);
    {
        const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
        TEST_CHECK(check_steps(&out, s) == 1 && VJO_GCHECK_TIER(s[0]->kind) == 2 &&
                   !(s[0]->kind & VJO_GCHECK_CASCADE) == !apart);
    }
    TEST_MSG("text '%s'", out.text);
    /* The line is a known text now (the line cache, API 22) unless the cascade read it as nothing: a line
     * the cascade changed is kept only with a text (core/vocr_ocr.c), and the Large size's model may read the
     * test's dashes as nothing where Normal's reads something. */
    unsigned cached = 0;
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) cached += line_cache.line[i].key != 0;
    TEST_CHECK_(cached <= 1u && (cached || (apart && !out.text[0])), "%u cached, text '%s'", cached, out.text);
    /* the next job, the same screen: from the cache nothing is recognized, the job's model is opened for
     * its stamp only and the cascade's is not needed; else both read again, the blocks still resident */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.recognized == 1 && (cached ? out.frames == 0 : out.frames > 0) && tier_opens[2] == 2 &&
               tier_opens[3] == (cached ? 1u : 2u) * apart && weight_allocations == 1 && cascade_allocations == apart);
    TEST_MSG("%s job: %u frames, opens %u/%u", cached ? "cached" : "uncached", (unsigned)out.frames,
             (unsigned)tier_opens[2], (unsigned)tier_opens[3]);
    /* without the cache: both resident, opened again only for their stamps */
    r.speed = VJO_GAME_OCR_S_NOCACHE;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    r.speed = 0;
    TEST_CHECK(out.frames > 0 && tier_opens[2] == 3 && tier_opens[3] == (cached ? 2u : 3u) * apart &&
               weight_allocations == 1 && cascade_allocations == apart);
    /* Small chosen: Normal's model is its cascade */
    TEST_CHECK(!drop_resident() && !weights_live && !cascade_live); /* as when the game goes to the back */
    r.tiers = 1u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER1_NAME);
    memset(tier_opens, 0, sizeof(tier_opens));
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(tier_opens[1] == 1 && tier_opens[2] == 1 && !tier_opens[3] && cascade_res.model == h15 &&
               out.reserve_bytes == VJO_GAME_RESERVE_NORMAL);
    /* Large chosen: no size above it, no cascade */
    TEST_CHECK(!drop_resident());
    r.tiers = 7u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    memset(tier_opens, 0, sizeof(tier_opens));
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(tier_opens[3] == 1 && !tier_opens[1] && !tier_opens[2] && !cascade_live && resident.model == w20 &&
               out.reserve_bytes == VJO_GAME_RESERVE_LARGE);
    /* Normal chosen, room for it but not for its cascade too: Normal alone, its own reserve */
    TEST_CHECK(!drop_resident());
    r.tiers = 3u;
    available = (int)tier_need(&r, 2, 0) + 4096;
    memset(tier_opens, 0, sizeof(tier_opens));
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(tier_opens[2] == 1 && !tier_opens[3] && !cascade_live && out.reserve_bytes == VJO_GAME_RESERVE_NORMAL &&
               !(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    /* a tier's file that cannot be opened: the job says so, and which tier it was */
    TEST_CHECK(!drop_resident());
    available = 64 * 1024 * 1024;
    open_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && !out.fallback && !out.text[0]);
    available = (int)tier_need(&r, 2, 0) - 4096; /* Normal does not fit: Small's file fails */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.fallback == 1);
    open_failure = 0;
    TEST_CHECK(!memory_live && !capture_live && !model);
    /* the blocks a job allocates after its check keep its size's reserve: Large's check passes,
     * then the game takes two pages; the weights, read after the workspace, would leave less than
     * 1 MiB free, so they are refused */
    TEST_CHECK(!drop_resident());
    r.tiers = 4u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    r.flags = 0;
    available = (int)tier_need(&r, 3, 0) + 4096;
    drop_on_workspace = 8192;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !weights_live && !out.text[0]);
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_LARGE && !memory_live && !capture_live);
    drop_on_workspace = 0;
    /* A PSP game with room only for bare Small: three checks, optional LM dropped,
     * refinement and normalisation retained, both job blocks released. */
    TEST_CHECK(!drop_resident());
    r = setup();
    line();
    psp_title = 1;
    r.tiers = 7u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    r.flags = 2u << VJO_GAME_OCR_LM_SHIFT | VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT |
              15u << VJO_GAME_OCR_SETTLE_SHIFT;
    available = (int)tier_need(&r, 1, vjo_vocr_refine_bytes(r.width, r.height));
    TEST_ASSERT((uint32_t)available < tier_need(&r, 2, vjo_vocr_refine_bytes(r.width, r.height)));
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    unsigned checks = 0, lm = 0, refine = 0, norm = 0, frees = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        unsigned id = out.step[i].id;
        checks += id == VJO_GSTEP_CHECK; lm += id == VJO_GSTEP_LM; refine += id == VJO_GSTEP_REFINE;
        norm += id == VJO_GSTEP_NORM; frees += id == VJO_GSTEP_FREE;
    }
    TEST_CHECK(out.fallback == 2u && checks == 3 && lm == 0 && refine == 1 && frees == 2 && (norm == 1 || !out.text[0]));
    TEST_CHECK(!lm_res.lm && resident.model == vjo_vocr_model(VJO_VOCR_TIER1_NAME));
    TEST_MSG("%u steps: %u checks, lm %u, refine %u, norm %u, frees %u; text '%s'", out.n_steps, checks, lm, refine,
             norm, frees, out.text);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live);
    /* #280's audit F5: with one reserve for every size, "Small and its LM fit, Normal alone does not" is no check's
     * outcome (Normal needs less than an LM more than Small), so the three checks with an LM come only through #324's
     * retry. In whole MiB, as a console reports it, beside the 512 KiB test draft's reserve: the check sees 3 MiB,
     * Large does not fit, Normal with the 0.5 MB LM does; the game takes 1 MiB and 8 KiB at Normal's workspace, so with
     * its blocks taken the weights' sample reads 1 MiB (512 KiB beside the reserve, short of Normal's weights); Small's
     * check, made again, reads 2 MiB, and its weights and LM fit. (Beside #280's 1 MiB the game took 8 KiB: the sample
     * read 2 MiB, 1 MiB beside the reserve, and Small's check 3 MiB.) Three checks, and the 20-step log still holds the
     * LM, the refinement, the normalisation and the job's two FREE steps (the refused try's blocks leave it). */
    r = setup();
    line();
    psp_title = 1;
    r.tiers = 7u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    r.flags = 1u << VJO_GAME_OCR_LM_SHIFT | VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT |
              15u << VJO_GAME_OCR_SETTLE_SHIFT;
    {
        const uint32_t mib = 1024u * 1024u, refine_b = (uint32_t)vjo_vocr_refine_bytes(r.width, r.height);
        const VjoVocrLm *lm1 = vjo_vocr_lm_tier(1);
        TEST_ASSERT(lm1 != NULL);
        const uint32_t lm_b = (uint32_t)PAGE_ROUND(lm1->bytes);
        const uint32_t w_n = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER2_NAME)->bytes);
        const uint32_t w_s = (uint32_t)PAGE_ROUND(vjo_vocr_model(VJO_VOCR_TIER1_NAME)->bytes);
        const uint32_t need_l = tier_need(&r, 3, refine_b), need_n = tier_lm_extra_need(&r, 2, lm1, refine_b);
        const uint32_t need_s = tier_lm_extra_need(&r, 1, lm1, refine_b);
        const uint32_t blocks_n = need_n - w_n - lm_b - VJO_GAME_RESERVE_NORMAL;
        const uint32_t blocks_s = need_s - w_s - lm_b - VJO_GAME_RESERVE_SMALL;
        const uint32_t after = 2u * mib + blocks_n - 4096u; /* the game's free memory once it took 1 MiB and 8 KiB */
        TEST_ASSERT(need_l > 3u * mib && need_n <= 3u * mib && blocks_n + 4096u < mib && blocks_s + 4096u <= blocks_n);
        TEST_ASSERT((after - blocks_n) / mib * mib < VJO_GAME_RESERVE_NORMAL + w_n);                /* refused */
        TEST_ASSERT(after / mib * mib >= need_s);                                                   /* Small's check */
        TEST_ASSERT((after - blocks_s) / mib * mib >= VJO_GAME_RESERVE_SMALL + w_s);                /* its weights */
        TEST_ASSERT((after - blocks_s - w_s) / mib * mib >= VJO_GAME_RESERVE_SMALL + lm_b);         /* its LM */
        free_mib = 1;
        available = (int)(3u * mib + blocks_n + 4096u); /* the first sample: 3 MiB */
        drop_once = (int)(mib + 8192u);
        drop_at_allocation = 1; /* at Normal's workspace */
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
        free_mib = 0;
    }
    checks = lm = refine = norm = frees = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        unsigned id = out.step[i].id;
        checks += id == VJO_GSTEP_CHECK; lm += id == VJO_GSTEP_LM; refine += id == VJO_GSTEP_REFINE;
        norm += id == VJO_GSTEP_NORM; frees += id == VJO_GSTEP_FREE;
    }
    TEST_CHECK_(out.fallback == 2u && checks == 3 && lm == 1 && refine == 1 && frees == 2 &&
                    (norm == 1 || !out.text[0]) && out.n_steps <= VJO_GAME_OCR_STEPS,
                "fallback %u, %u steps: %u checks, lm %u, refine %u, norm %u, frees %u; text '%s'",
                (unsigned)out.fallback, (unsigned)out.n_steps, checks, lm, refine, norm, frees, out.text);
    TEST_CHECK(lm_res.lm == vjo_vocr_lm_tier(1) && resident.model == vjo_vocr_model(VJO_VOCR_TIER1_NAME));
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE_SMALL && allocations == 2u && !memory_live && !capture_live);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !lm_live);
    psp_title = 0;
}

/* model-tiers 1.5's release term: a resident block of another size counts as free memory for a
 * size's check (the check step's sum), and is released before the job takes its own blocks;
 * two weights blocks are never held at once (the stub asserts that at every allocation).
 * From audit-overlay-ux-tiers' scratch test (its mutants X2 and X6 survived the others). */
static int step_index(const VjoGameOcrResult *o, unsigned id, unsigned arg)
{
    for (unsigned i = 0; i < o->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (o->step[i].id == id && o->step[i].arg == arg) return (int)i;
    return -1;
}
static void tier_release_counts_other_sizes_as_free(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS] = {NULL};
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
        char path[600]; FILE *f;
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, tier_names[t - 1u]);
        if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(path, "rb"))) { TEST_SKIP("no release weights"); return; }
        fclose(f);
    }
    const VjoVocrModel *fl10 = vjo_vocr_model(VJO_VOCR_TIER1_NAME), *h15 = vjo_vocr_model(VJO_VOCR_TIER2_NAME),
                       *w20 = vjo_vocr_model(VJO_VOCR_TIER3_NAME);
    line();
    r.tiers = 3u; snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME); r.flags = 0;
    uint32_t n2 = tier_need(&r, 2, 0);
    /* 1: Normal does not fit, Small runs: FL10 resident */
    available = (int)n2 - 4096;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.fallback == 1 && resident.model == fl10 && weights_live);
    uint32_t fl10_block = weights_block;
    /* 2: free is FL10's block short of Normal's need: Normal fits only with FL10 counted as free */
    available = (int)n2;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK_(out.fallback == 0 && resident.model == h15, "fallback %u", (unsigned)out.fallback);
    TEST_CHECK(check_steps(&out, s) == 1 && s[0]->rc == 0 && s[0]->sum == fl10_block);
    TEST_MSG("check sum %u, FL10 block %u", s[0] ? (unsigned)s[0]->sum : 0u, fl10_block);
    int fw = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS), ac = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_CAPTURE);
    TEST_CHECK_(fw >= 0 && ac > fw, "FL10 freed at step %d, capture taken at %d", fw, ac);
    uint32_t h15_block = weights_block;
    /* 3: 4 KiB less: Normal (its weights resident, need 0 for them) refused; Small runs with H15 counted */
    available = (int)n2 - 4096;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.fallback == 1 && resident.model == fl10);
    TEST_CHECK(check_steps(&out, s) == 2 && s[0]->rc == 1 && s[0]->sum == 0 && s[1]->rc == 0 && s[1]->sum == h15_block);
    if (h15 == w20) { /* Normal kept Large's model: no cascade, no other size's block (game_vocr_shared_tiers) */
        TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !cascade_live);
        return;
    }
    /* 4: Normal with the Large cascade, both resident; then Large at its exact edge: H15's weights
     * and the W20 cascade block both count as free, and both are released first */
    TEST_CHECK(!drop_resident());
    available = 64 * 1024 * 1024; test_cascade_conf = 1001;
    r.flags = 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(resident.model == h15 && cascade_res.model == w20 && weights_live && cascade_live);
    uint32_t held = live_bytes;
    /* the Shell sets the cascade ceiling on every job (ocr_cascade_ms): Large has no cascade, so the
     * resident W20 cascade block is one the check counts as free */
    r.tiers = 7u; snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    uint32_t n3 = tier_need(&r, 3, 0);
    available = (int)n3; /* free = n3 - (H15 + W20 blocks); with them released, exactly Large's need */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK_(out.fallback == 0 && resident.model == w20 && !cascade_live, "fallback %u", (unsigned)out.fallback);
    TEST_CHECK(check_steps(&out, s) == 1 && s[0]->rc == 0 && s[0]->sum == held);
    TEST_MSG("held %u, sum %u", held, s[0] ? (unsigned)s[0]->sum : 0u);
    int fc = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_CASCADE);
    fw = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS); ac = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_CAPTURE);
    TEST_CHECK_(fw >= 0 && fc >= 0 && ac > fw && ac > fc, "weights freed %d, cascade freed %d, capture %d", fw, fc, ac);
    /* 5: 4 KiB less: Large refused, Normal runs (W20's weights counted as free, then released) */
    available = (int)n3 - 4096;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.fallback == 1 && resident.model == h15 && check_steps(&out, s) == 2 && s[1]->sum == PAGE_ROUND(w20->bytes));
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !cascade_live);
    test_cascade_conf = 0;
}

/* ocr-first with model sizes: another size's resident weights stay while the job
 * fits beside them with the fixed 1 MiB reserve. Small's weights resident, a Normal
 * job on a screen with a line: with
 * exactly Normal's need free beside Small's block, that block goes only when the
 * core reads Normal's weights, after the job's workspace; 4 KiB less, before it. */
static void another_sizes_weights_stay_beside_its_reserve(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    for (unsigned t = 1; t <= 2; t++) {
        char path[600];
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, tier_names[t - 1u]);
        if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(path, "rb"))) { TEST_SKIP("no %s beside the release weights", tier_names[t - 1u]); return; }
        fclose(f);
    }
    const VjoVocrModel *fl10 = vjo_vocr_model(VJO_VOCR_TIER1_NAME), *h15 = vjo_vocr_model(VJO_VOCR_TIER2_NAME);
    line();
    r.flags = 0; /* no LM, no cascade: each job holds its own size only */
    r.speed = VJO_GAME_OCR_S_NOCACHE;
    for (int tight = 0; tight < 2; tight++) {
        TEST_CHECK(!drop_resident());
        available = 64 * 1024 * 1024;
        r.tiers = 1u; /* Small: its weights resident */
        snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER1_NAME);
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 && out.text[0] && resident.model == fl10);
        r.tiers = 3u; /* Normal chosen */
        snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
        available = (int)(live_bytes + tier_need(&r, 2, 0) - (tight ? 4096u : 0u));
        TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
        /* the job read its line with Normal's model: its recognize step, not its text (VOCR6-N reads this synthetic
         * line as nothing, where VOCR5-N read something) */
        const VjoGameOcrStep *rs = recognize_step(&out);
        TEST_CHECK(rs && rs->arg >= 1 && rs->bytes > 0 && !out.fallback && resident.model == h15 &&
                   out.reserve_bytes == VJO_GAME_RESERVE_NORMAL);
        TEST_MSG("recognized %u line(s), %u frames; text '%s'", rs ? (unsigned)rs->arg : 0u, rs ? (unsigned)rs->bytes : 0u,
                 out.text);
        int freed = step_index(&out, VJO_GSTEP_FREE, VJO_GITEM_WEIGHTS),
            workspace = step_index(&out, VJO_GSTEP_ALLOC, VJO_GITEM_WORKSPACE);
        TEST_CHECK(workspace >= 0 && freed >= 0 && (tight ? freed < workspace : freed > workspace));
        TEST_MSG("%s: Small's block freed at step %d, the workspace allocated at step %d", tight ? "tight" : "exact",
                 freed, workspace);
    }
    r.speed = 0;
}

#ifdef VJO_VOCR_TIERS_TEST
/* ---- two sizes, one model (host/tests/test_shared_tiers.h: Small VOCR3-FL10,
 * Normal and Large VOCR4-W20) ---- */

/* Normal's next size up has Normal's own model: no cascade (the same model reads
 * a doubtful line the same again), so a Normal job keeps Normal's reserve. Small's
 * cascade is that model, opened as Normal's tier, the tier above a Small request's
 * that the kernel allows it (kernel/game_ocr.c tier_allowed). Each size's job
 * opens the model as its own tier. No files needed. */
static void shared_model_no_cascade_and_the_job_s_own_tier(void)
{
    VjoGameOcrRequest r = setup();
    const VjoVocrModel *small = vjo_vocr_model(VJO_VOCR_TIER1_NAME), *normal = vjo_vocr_model(VJO_VOCR_TIER2_NAME),
                       *large = vjo_vocr_model(VJO_VOCR_TIER3_NAME);
    Candidate s = {small, 1, VJO_GAME_OCR_OPEN_TIER, 0}, n = {normal, 2, VJO_GAME_OCR_OPEN_TIER + 1u, 0},
              l = {large, 3, VJO_GAME_OCR_OPEN_TIER + 2u, 0};
    TEST_ASSERT(small && normal && large);
    TEST_ASSERT(normal == large && small->bytes < normal->bytes); /* the map under test */
    TEST_CHECK(VJO_VOCR_MAX_BYTES == large->bytes);
    r.tiers = 7u;
    TEST_CHECK(!cascade_of(&n, &r, 3, 1) && !cascade_of(&l, &r, 3, 1) && cascade_of(&s, &r, 3, 1) == normal);
    r.tiers = 3u; /* Normal chosen */
    TEST_CHECK(!cascade_of(&n, &r, 2, 1) && cascade_of(&s, &r, 2, 1) == normal);
    r.tiers = 1u; /* Small chosen */
    TEST_CHECK(cascade_of(&s, &r, 1, 1) == normal);
    TEST_CHECK(check_reserve(2, cascade_of(&n, &r, 2, 1)) == VJO_GAME_RESERVE_NORMAL &&
               check_reserve(1, cascade_of(&s, &r, 1, 1)) == VJO_GAME_RESERVE_NORMAL &&
               check_reserve(3, NULL) == VJO_GAME_RESERVE_LARGE);
    tiered = 1;
    open_which = l.which;
    TEST_CHECK(which_file("ux0:data/VitaJPOverlay/ocr/" VJO_VOCR_TIER3_NAME) == l.which);
    open_which = n.which;
    TEST_CHECK(which_file("ux0:data/VitaJPOverlay/ocr/" VJO_VOCR_TIER2_NAME) == n.which);
    open_which = s.which;
    TEST_CHECK(which_file("d/" VJO_VOCR_TIER2_NAME) == n.which && which_file("d/" VJO_VOCR_TIER1_NAME) == s.which);
    TEST_CHECK(which_file("d/H15_w8.vocr") == VJO_GAME_OCR_OPEN_TIER + VJO_VOCR_TIERS);
    tiered = 0; open_which = VJO_GAME_OCR_OPEN_MODEL;
}

/* With the release weights: Large alone opens the model as Large's (tier 3, the
 * only one a Large-only request allows); Normal opens it as Normal's, with no
 * cascade and the same reserve, the block still resident; Large chosen at Normal's
 * memory edge still runs Large, the same model; Small's cascade reads it as Normal's tier. */
static void shared_model_jobs_run_as_their_own_tier(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    const VjoGameOcrStep *s[VJO_GAME_OCR_STEPS];
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
        char path[600]; FILE *f;
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, tier_names[t - 1u]);
        if (!VJO_VOCR_MODEL_DIR[0] || !(f = fopen(path, "rb"))) { TEST_SKIP("no %s beside the release weights", tier_names[t - 1u]); return; }
        fclose(f);
    }
    const VjoVocrModel *small = vjo_vocr_model(VJO_VOCR_TIER1_NAME), *shared = vjo_vocr_model(VJO_VOCR_TIER3_NAME);
    line();
    available = 64 * 1024 * 1024;
    test_cascade_conf = 1001; /* every line doubtful: a cascade, if there is one, reads each again */
    r.flags = 8u << VJO_GAME_OCR_CASCADE_SHIFT;
    r.tiers = 4u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.rc == VJO_OK && out.recognized >= 1 && out.frames > 0 && !out.fallback &&
               out.reserve_bytes == VJO_GAME_RESERVE_LARGE);
    TEST_CHECK(tier_opens[3] >= 1 && !tier_opens[2] && !tier_opens[1] && !cascade_opens && !cascade_live);
    TEST_CHECK(resident.model == shared && weights_live && weight_allocations == 1);
    TEST_MSG("opens %u/%u/%u, text '%s'", tier_opens[1], tier_opens[2], tier_opens[3], out.text);
    /* Normal chosen: the same block, opened as Normal's for its stamp, no cascade */
    memset(tier_opens, 0, sizeof(tier_opens));
    r.tiers = 3u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER2_NAME);
    r.speed = VJO_GAME_OCR_S_NOCACHE; /* recognized again, not from the line cache */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.rc == VJO_OK && out.recognized >= 1 && out.frames > 0 && !out.fallback &&
               out.reserve_bytes == VJO_GAME_RESERVE_NORMAL);
    TEST_CHECK(tier_opens[2] >= 1 && !tier_opens[3] && !tier_opens[1] && !cascade_opens && !cascade_live);
    TEST_CHECK(resident.model == shared && weight_allocations == 1); /* resident across the two sizes */
    TEST_CHECK(out.recognized >= 1 && !(out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    TEST_CHECK(check_steps(&out, s) == 1 && VJO_GCHECK_TIER(s[0]->kind) == 2 && !(s[0]->kind & VJO_GCHECK_CASCADE));
    /* Shared models have identical needs: Large runs at the same memory edge as Normal. */
    TEST_CHECK(!drop_resident());
    memset(tier_opens, 0, sizeof(tier_opens));
    r.tiers = 7u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER3_NAME);
    available = (int)tier_need(&r, 2, 0) + 4096;
    TEST_ASSERT(tier_need(&r, 2, 0) == tier_need(&r, 3, 0));
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.rc == VJO_OK && !out.fallback && out.reserve_bytes == VJO_GAME_RESERVE_LARGE);
    TEST_CHECK(tier_opens[3] >= 1 && !tier_opens[2] && resident.model == shared);
    TEST_CHECK(check_steps(&out, s) == 1 && VJO_GCHECK_TIER(s[0]->kind) == 3 && !s[0]->rc);
    /* Small chosen: its cascade is the shared model, opened as Normal's; Normal's reserve */
    TEST_CHECK(!drop_resident());
    memset(tier_opens, 0, sizeof(tier_opens));
    available = 64 * 1024 * 1024;
    r.tiers = 1u;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_TIER1_NAME);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0);
    TEST_CHECK(out.rc == VJO_OK && !out.fallback && out.reserve_bytes == VJO_GAME_RESERVE_NORMAL);
    TEST_CHECK(tier_opens[1] >= 1 && tier_opens[2] == 1 && !tier_opens[3] && !cascade_opens);
    TEST_CHECK(resident.model == small && cascade_res.model == shared && cascade_live);
    TEST_CHECK(out.recognized >= 1 && (out.line[0].flags & VJO_VOCR_LINE_CASCADE));
    r.speed = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !cascade_live);
    test_cascade_conf = 0;
}

TEST_LIST = {
    {"two sizes, one model: no cascade for the smaller, each job opens it as its own tier",
     shared_model_no_cascade_and_the_job_s_own_tier},
    {"two sizes, one model: real jobs run it as their own tier, resident across both",
     shared_model_jobs_run_as_their_own_tier},
    {"model tiers: two sizes with one model open it as the job's own tier", two_sizes_with_one_model_open_as_the_job_s_own_tier},
    {NULL, NULL}
};
#else
TEST_LIST = {
    {"512 KiB test draft: 3, 2 and 1.5 MiB free start the largest size that fits; reserve and allocation failures "
     "still enforced", three_mib_free_can_start_ocr},
    {"512 KiB test draft: VOCR6's three sizes keep the fixed 512 KiB and need their table's bytes at the full screen",
     vocr6_sizes_beside_the_fixed_reserve},
    {"a job without lines reads the weights, the LM and the cascade (ocr-first)", a_job_without_lines_reads_the_weights_lm_and_cascade},
    {"the TEAR step says when the retry began", the_tear_step_says_when_the_retry_began},
    {"100 real vocr jobs and game USER reserve", repeated_jobs_and_reserve},
    {"allocation base free failure and retry", memory_failure_and_retry},
    {"cancel before allocation and wire validation", cancel_and_wire},
    {"real model bounded short reads errors and cancellation", real_model_io_and_cancellation},
    {"model callbacks without weights timeouts errors and saved cleanup sequence", model_callbacks_timeouts_errors_and_cleanup},
    {"ALL system gate and retained thread joins", system_app_gate_and_failed_joins},
    {"one worker per process: the oldest copy stays (#138)", the_oldest_copy_stays},
    {"capture memory ladder USER CDRAM PHYCONT each logged", capture_memory_ladder_is_logged},
    {"the kernel strip after a failed hook write; timeouts are not retried", hook_failure_falls_back_to_the_kernel_strip},
    {"BUSY rows calls: asked again before arming, the block kept after", busy_rows_calls},
#if VJO_DEBUG
    {"the self-test probe tries every rung", probe_tries_every_rung},
    {"the probe's user-mode frame read stays inside the frame's block", probe_user_read_stays_in_the_frame_block},
#else
    {"the normal build refuses a probe", normal_build_refuses_a_probe},
#endif
    {"the self-test fallback stage forces the fallback", forced_fallback_runs_the_fallback},
    {"the job policy rides in the flags: the wait, its step, unknown bits refused", policy_in_the_flags},
    {"API 47: auto reads the columns where the rows look like vertical text, rows elsewhere",
     auto_reads_columns_where_the_rows_are_vertical},
    {"API 55: auto with the pause copies each try's frame", auto_with_the_pause_copies_each_tries_frame},
    {"API 47: auto's retry waits no longer than one try: the job's stable-text wait is shared", auto_retry_shares_the_wait},
    {"API 47: auto's tries report the last one's model, never an earlier try's step down", auto_tries_report_the_last_ones_model},
    {"API 47: auto's columns try gives its tall-line block back before the rows' try (no vertical model)",
     auto_retry_frees_the_line_block},
    {"API 47: a job of columns frees its tall-line block at its end; a refused one leaves the columns out",
     columns_job_line_block_freed_or_refused},
    {"API 53: on prefers columns: rows of horizontal text as off reads them, else the vertical model alone; rows "
     "without it or its memory", on_prefers_columns},
    {"#385's audit F3: on, a wide box that empties between lines is off's job: no columns' try, no model read again",
     on_wide_empty_box_is_off_s_job},
    {"#385's audit F2: on, the columns' weights refused after their check: the rows, find kind 5",
     on_columns_weights_refused_read_the_rows},
    {"#385's audit F7: on's two tries keep the last try's steps; the rows' try's checks leave with its blocks",
     on_tries_keep_the_last_try_s_steps},
    {"#385's audit F5: a job of columns' rectangle keeps the reserve and its weights' room; refused, one by one",
     columns_rectangle_keeps_the_reserve},
    {"#411's audit F2: on reads a square region's columns at once", on_square_region_reads_columns_at_once},
    {"#411's audit F2: a job of columns' rectangle keeps its LM's room", columns_rectangle_keeps_the_lm_s_room},
    {"#411's audit F2: after a bad vertical file, on's tall regions read rows without opening it",
     on_tall_region_after_a_bad_vertical_file_reads_rows},
    {"the request's flags become the job's policy", request_flags_become_the_policy},
    {"API 26: the text-presence gate in the worker: its file, its step, a wait first on one strip", the_gate_runs_in_the_worker},
    {"#418: the gate's weights a block of their own: taken, kept, given back; never the weights' room",
     the_gate_s_weights_are_a_block_of_their_own},
    {"the step log logs a retried pass once and keeps the releases", step_log_keeps_the_releases},
    {"the returned text is normalised, the recognizer's own kept after it", returned_text_is_normalised_and_kept},
    {"a retry to an empty box returns nothing of the first try", a_retry_to_an_empty_box_returns_nothing},
    {"the memory check: the cascade and the LM, else the LM, else the cascade; a resident LM given back counts",
     memory_check_choices},
    {"cleanup gives back LM and cascade blocks never verified", cleanup_gives_back_unverified_blocks},
    {"the weights and cascade blocks hold the Large size's model (VOCR6-L: 2,475,680 B), not a byte more", the_weights_blocks_hold_the_large_size_and_no_more},
    {"the cascade step carries its figures, each clamped to its field", the_cascade_step_carries_its_figures},
    {"the beam + LM decoder: read once, resident, a smaller tier when tight, given back when off", lm_jobs},
    {"the confidence cascade: doubtful lines read again, its model resident, given back, left when tight", cascade_jobs},
    {"an inactive game gets the cascade's block back", an_inactive_game_gets_the_cascade_block_back},
    {"the first job's step log keeps its lm, cascade and norm steps", the_first_job_logs_the_lm_cascade_and_norm},
    {"the refinement: on, in PSP games only by title, its step and arena; mode 3 refused", refine_modes},
    {"the refine step's counts: boxes, splits, the 16-line cap, rows", the_refine_step_carries_its_counts},
    {"module_start records whether the game is a PSP game", module_start_sets_the_psp_title},
    {"module_start reads the text source for the hooks (API 31)", module_start_reads_the_text_source},
    {"model tiers: the largest that fits beside its reserve, then the next one down, each check logged",
     tier_ladder_takes_the_largest_size_that_fits},
    {"model tiers: weights refused after the check (free memory in whole MiB): the next smaller model",
     weights_refused_after_the_check_retry_the_next_smaller_model},
    {"#324's audit: the retry after refused weights shares the job's wait and counts every try's captures",
     weights_refused_retry_shares_the_wait},
    {"#324's audit: the retry's step log is the job's that read: one PASS and SIG, no refused try's blocks",
     weights_refused_retry_keeps_the_last_try_s_steps},
    {"#324's audit: after auto's tries the retry keeps its own steps, and checks the memory as it is then",
     weights_refused_retry_after_auto_s_tries},
    {"#382's audit: a resident block the refused try gave back keeps its FREE step",
     weights_refused_retry_keeps_a_resident_block_s_free_step},
    {"#382's audit: a job cancelled as its refused weights close retries nothing", weights_refused_then_cancelled_retries_nothing},
    {"#382's audit: refused weights, then auto's tries on the smaller model: one job, its check kept",
     weights_refused_then_auto_s_tries},
    {"model tiers: the cascade is the next size up, a fixed reserve, files by tier",
     tier_cascade_is_the_next_size_up},
    {"model tiers: two sizes with one model open it as the job's own tier", two_sizes_with_one_model_open_as_the_job_s_own_tier},
    {"model tiers: the self-test's fallback stage runs the smallest", forced_fallback_by_tiers_runs_the_smallest},
    {"model tiers: real jobs open each size and its cascade by tier; a failed file names its tier",
     tier_jobs_open_their_models_by_tier},
    {"ocr-first + model tiers: another size's weights stay while the job fits beside them with its own reserve",
     another_sizes_weights_stay_beside_its_reserve},
    {"API 22 + model tiers: the helpers keep the tier's reserve, never a smaller tier", helpers_keep_the_tier_reserve},
    {"model tiers: another size's resident blocks count as free and are released first",
     tier_release_counts_other_sizes_as_free},
    {"API 22: helpers and the ARM clock ride in the flags, counted and restored", helpers_and_clock_in_the_flags},
    {"API 22: a stuck helper keeps the workspace and weights until stop, which waits for the worker first",
     a_stuck_helper_keeps_its_memory_until_stop},
    {"API 22: the clock stays where the game set it during a job", the_clock_stays_where_the_game_set_it},
    {"API 22: a job with VJO_GAME_OCR_S_NOCACHE recognizes and caches nothing", a_nocache_job_recognizes_and_caches_nothing},
    {"#418: the worker's line cache, replayed through the old layout: the same hits, misses and entries",
     the_worker_s_cache_replayed_through_the_old_layout},
    {"API 22: helpers with the LM and the cascade give the serial job's text and recognizer text",
     helpers_with_the_lm_and_cascade_give_the_serial_text},
    {NULL, NULL}
};
#endif /* VJO_VOCR_TIERS_TEST */
