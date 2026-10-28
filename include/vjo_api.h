/* Kernel <-> SceShell interface of Rubi. Shared by kernel/,
 * shell/ and core/ (for the plain data types). */
#ifndef VJO_API_H
#define VJO_API_H

#include <stdint.h>

#include "vjo_build.h" /* VJO_DEBUG: the debug or the normal build (docs/release-variants.md) */

/* The interface's number. The kernel, the Shell and the game worker are one tree's: the Shell and the worker
 * refuse a kernel whose vjoGetVersion() is another (shell/worker.c, game_ocr/main.c), so there is no compatibility
 * path, and a change the three must agree on (a struct's layout, a syscall, a flag bit, a step kind, a value's
 * meaning) takes the next number. A number names one interface only, so numbers only grow, past those that drafts
 * or reverted changes used.
 * 56: current-game hook profiles, loaded by vjoOcrHookProfile before hook installation.
 * 55: the pause for the frame (config.ini ocr_pause, off by default; #348, docs/game-pause.md):
 * VjoGameOcrRequest.speed bit 30 (VJO_GAME_OCR_S_PAUSE), and vjoOcrCaptureRows' VJO_ROWS_FRAME, the whole region
 * copied from one frame in one display call (at most VJO_GAME_OCR_PAUSE_MAX_US), into the worker's frame block
 * (VJO_GITEM_FRAME), which the job reads from; the step log's VJO_GSTEP_PAUSE. Sizes and layouts are unchanged.
 * What each earlier number changed: 14 to 55 one by one in this comment as main f5d66795 has it (git show
 * f5d66795:include/vjo_api.h), and CHANGELOG.md (the releases that changed it name it). */
#define VJO_API_VERSION 56

/* API 31: the text source (config.ini text_source; docs/native-hooks.md). The kernel holds the Shell's
 * choice; a game worker reads it once, as its game starts, so a change applies at a game's next start. */
#define VJO_TEXT_SOURCE_OCR 0u   /* no hooks anywhere: the kernel's value until the Shell sets one */
#define VJO_TEXT_SOURCE_AUTO 1u  /* CALL hooks in every enabled native Vita game, plus available table sites */
#define VJO_TEXT_SOURCE_HOOKS 2u /* legacy choice: the same hook attempts as AUTO */
#define VJO_TEXT_SOURCE_COUNT 3u
/* The word vjoSetTextSource takes and vjoGetTextSource returns: a VJO_TEXT_SOURCE_* in its low byte, plus
 * flags. VJO_TEXT_SOURCE_NO_SITES (config.ini hook_sites = off): install no table site, only the CALL
 * source, so a console run can tell the two apart (audit-hooks F3). Every other bit is 0. */
#define VJO_TEXT_SOURCE_MODE_MASK 0xFFu
#define VJO_TEXT_SOURCE_NO_SITES 0x100u

/* #138: the kernel plugin loads the game OCR worker into every game that is ON
 * in games.ini when it starts (kernel/loader.c, docs/worker-loading.md), from
 * VJO_GAME_OCR_PATH_UR0, or VJO_GAME_OCR_PATH_UX0 when ur0:tai has none, so
 * installers write no *TITLEID or *ALL line for it. 0 would mean they must again.
 * A per-title line older installs left is harmless: the worker keeps one copy
 * per process. */
#define VJO_KERNEL_LOADS_WORKER 1
#define VJO_GAME_OCR_PATH_UR0 "ur0:tai/VitaJPOverlay_OCR.suprx"
#define VJO_GAME_OCR_PATH_UX0 "ux0:tai/VitaJPOverlay_OCR.suprx"

/* Normalized rectangle, 0..65535 on both axes. w == 0 means full screen. */
typedef struct {
    uint16_t x, y, w, h;
} VjoRect;

enum VjoTrigger {
    VJO_TRIGGER_SELECT = 0,
    VJO_TRIGGER_START = 1,
    VJO_TRIGGER_L_R = 2,
    VJO_TRIGGER_SELECT_L = 3,
    VJO_TRIGGER_SELECT_R = 4,
    VJO_TRIGGER_REAR_DOUBLE_TAP = 5,
    VJO_TRIGGER_COUNT
};

/* vjoWaitEvent bits */
#define VJO_EV_TRIGGER       0x01u /* toggle trigger pressed */
#define VJO_EV_REGION_STABLE 0x02u /* region changed, then unchanged (animation aside) for ~300 ms */
#define VJO_EV_GAME_START    0x04u
#define VJO_EV_GAME_EXIT     0x08u
#define VJO_EV_CAPTURE_DONE  0x10u /* an OCR job's first pass is in (VjoState.done_seq, capture_scene, region_seq) */
#define VJO_EV_SUBTITLE      0x20u /* subtitle trigger pressed */
#define VJO_EV_REGION_QUIET  0x40u /* the region became quiet (VjoState.quiet) */
#define VJO_EV_SHELL         0x80u /* local command/text/result ready: inspect Shell state */
#define VJO_EV_ALL           0xFFu

typedef struct {
    uint32_t size;          /* sizeof(VjoState), set by caller */
    int32_t game_pid;
    uint32_t fb_width, fb_height, fb_pitch, fb_pixelformat;
    uint32_t scene;         /* the screen in the region: a new number per change (animation aside), 0 = none yet */
    uint32_t stable;        /* 1 if the scene has settled */
    /* The capture of the last region job (not a full-frame, probe or shot job's), published
     * when its first pass is in (kernel/game_ocr.c publish_capture_locked): no pixels are
     * kept, only its size and its signature's scene. */
    uint32_t width, height; /* captured region size in pixels */
    uint32_t capture_scene; /* scene of the captured pixels; 0 = unknown */
    uint32_t done_seq;      /* that job's capture sequence number */
    uint32_t unsettled_ms;  /* the region has been changing this long without settling (0 = settled) */
    /* not settled, but only a small spot keeps changing (an icon not
     * masked yet), or (API 41) a hooked line still drawing: worth capturing */
    uint32_t quiet;
    uint32_t region_seq;    /* the same capture's; 0 after a region change, and while a job's first pass rewrites it */
    /* the scene if that capture shows it now (animated cells aside, and
     * the quiet spot for one taken since it started), else 0: a capture
     * taken before an icon was masked shows the screen once it is */
    uint32_t region_scene;
    /* API 49: the scene of the newest line a trusted hook reported (vjoOcrHookLine with VJO_HOOK_LINE_TRUSTED)
     * while the region still shows it, else 0: the Shell reads it at once, past the background read's pacing */
    uint32_t hook_scene;
} VjoState;

typedef struct {
    uint32_t buttons;       /* SCE_CTRL_* of the real pad, unfiltered */
    uint8_t lx, ly, rx, ry; /* analog sticks, 128 = centre */
} VjoInput;

/* Shared syscall and copied-payload statuses. Physical retail hardware returned
 * queued -3 as 0xBFFFFFFD. Keep every custom status negative with bit 30 clear,
 * so that observed return transformation leaves the API 13+ contract unchanged.
 * Native SDK/file errors are returned untouched in their own namespaces. */
#define VJO_ERR_NO_GAME      ((int32_t)0xBFFFFFFFu)
#define VJO_ERR_NO_MEMORY    ((int32_t)0xBFFFFFFEu)
#define VJO_ERR_BUSY         ((int32_t)0xBFFFFFFDu)
#define VJO_ERR_TIMEOUT      ((int32_t)0xBFFFFFFCu)
#define VJO_ERR_FORMAT       ((int32_t)0xBFFFFFFBu)
#define VJO_ERR_ARG          ((int32_t)0xBFFFFFFAu) /* bad argument */
#define VJO_ERR_COPY         ((int32_t)0xBFFFFFF9u)
#define VJO_ERR_CANCELLED    ((int32_t)0xBFFFFFF7u)
#define VJO_ERR_PERM         ((int32_t)0xBFFFFFF8u) /* caller is not the registered role */
#define VJO_ERR_UNSUPPORTED  ((int32_t)0xBFFFFFF6u) /* not in this build: the normal build's self-test calls */

/* vjoSetGameActive's mode for the foreground process. */
enum VjoGameMode {
    VJO_GAME_NONE = 0,      /* a system app */
    VJO_GAME = 1,
    VJO_GAME_STATIC_FB = 2, /* draws into a framebuffer it set once and submits no
                             * frames (the PSP emulator): sampled from its pad calls */
};

/* The boot-safety guard's view of this boot (core/boot_guard.h has the
 * values: VJO_BOOT_NORMAL/SAFE, VJO_BOOT_STAGE_*, VJO_BOOT_FAULT_*). */
typedef struct {
    uint32_t size;        /* sizeof(VjoBootInfo), set by the caller */
    uint32_t seq;         /* this boot's number */
    int32_t mode;         /* VJO_BOOT_NORMAL or VJO_BOOT_SAFE */
    int32_t stage;        /* furthest VJO_BOOT_STAGE_* reached so far */
    int32_t inert;        /* the Shell plugin unloaded itself this boot */
    int32_t fault;        /* VJO_BOOT_FAULT_* seen this boot */
    int32_t failures;     /* normal boots in a row that did not complete */
    int32_t prev_mode, prev_stage, prev_inert, prev_fault; /* the previous boot */
    int32_t write_status; /* the last boot.txt write: 0 ok, else the error */
} VjoBootInfo;

/* vjoMemInfo: what the kernel can read about memory for the self-test
 * (docs/self-test.md). total/free are exact partition figures from the
 * kernel; a negative rc means that figure is unavailable and why. Three routes
 * are tried, each reported on its own, because only a console run tells which
 * works on its firmware (3.65's answer to the first route in API 18 was
 * 0x80020001, #74).
 *
 * Every partition object the firmware hands back is checked before
 * PhyMemPartInfoCore reads it: kernel objects live below 0x40000000 (TTBR0;
 * VitaSDK psp2kern/kernel/sysmem/data_transfers.h). A pointer in
 * PSVshellPlus's range 0x1000-0x1000000 (info.c:57, :91) is read; one higher in
 * the kernel half only inside a kernel memory block. The *_step fields name
 * where a route stopped (0: it read its figures):
 *   VJO_MEMSTEP_FIND 1       FindMemBlockByAddr / the function lookup
 *   VJO_MEMSTEP_INFO 2       MemBlockGetInfoEx / the address-space query
 *   VJO_MEMSTEP_RANGE 3      the object pointer is outside the kernel half (or 0, or misaligned)
 *   VJO_MEMSTEP_NO_BLOCK 4   in the kernel half above 0x1000000, but in no kernel memory block
 *   VJO_MEMSTEP_CORE 5       PhyMemPartInfoCore itself (its lookup, or the call: rc tells) */
#define VJO_MEMINFO_PARTS 8u
#define VJO_MEMSTEP_FIND 1
#define VJO_MEMSTEP_INFO 2
#define VJO_MEMSTEP_RANGE 3
#define VJO_MEMSTEP_NO_BLOCK 4
#define VJO_MEMSTEP_CORE 5
typedef struct {
    uint32_t size;           /* sizeof(VjoMemInfo), set by the caller */
    /* Route 1, a memory block's partition: FindMemBlockByAddr (size 0) ->
     * MemBlockGetInfoEx -> PhyMemPartInfoCore, for our module's block (the
     * kernel's partition, ScePhyMemPartKD when total is 36 MiB) and for the
     * caller's out pointer (SceShell's or the game's partition). */
    int32_t kernel_rc;
    uint32_t kernel_total, kernel_free;
    int32_t caller_rc;
    uint32_t caller_total, caller_free;
    uint32_t ledger_current, ledger_peak, ledger_limit; /* the kernel ledger's pool */
    int32_t kernel_step, caller_step;
    /* Route 2 (PSVshell, PSVshellPlus): the caller's address space control
     * block at +328 (main), +332 (CDRAM), +316 (phycont), each partition's
     * PhyMemPartInfoCore. cas_step: where the main partition stopped. */
    int32_t cas_rc;
    uint32_t cas_main_total, cas_main_free, cas_cdram_total, cas_cdram_free;
    uint32_t cas_phycont_total, cas_phycont_free;
    /* Route 3 (faps-coredump): a process's address space info
     * (SceSysmemForKernel_C3EF4055) and each of its partitions' InfoCore; the
     * one of 36 MiB is ScePhyMemPartKD. The kernel process's is asked first;
     * as_caller = 1 when that was refused and the caller's was read instead.
     * as_step: where it stopped, or for a partition that could not be read
     * the last such reason (as_skipped of them). */
    int32_t as_rc;
    uint32_t as_parts;
    uint32_t as_total[VJO_MEMINFO_PARTS], as_free[VJO_MEMINFO_PARTS];
    int32_t cas_step, as_step;
    uint32_t as_caller, as_skipped;
} VjoMemInfo;

/* Syscalls exported by VitaJPOverlay_Kernel (library VitaJPOverlayForUser).
 * All return < 0 on error. */
#if !defined(VJO_HOST)
#include <psp2common/types.h>
int vjoGetVersion(void);                 /* VJO_API_VERSION, no side effects */
int vjoRegisterShell(void);              /* caller becomes the shell if it has VitaJPOverlay_Shell loaded */
int vjoWaitEvent(uint32_t mask, uint32_t *out, SceUInt32 timeout_us); /* 0 timeout = forever */
int vjoNotifyShell(void);               /* registered Shell only; coalesced wakeup */
int vjoGetState(VjoState *out);          /* set out->size first */
int vjoSetRegion(const VjoRect *r);      /* NULL = full screen */
int vjoSetTriggers(int toggle, int subtitle); /* enum VjoTrigger each, not equal */
int vjoSetGameActive(int pid, int mode);  /* shell: enum VjoGameMode of the foreground process */
int vjoSetInputBlock(int on);            /* block all game input (not the shell) */
int vjoPollInput(VjoInput *out);         /* raw pad, unfiltered */
/* API 31, the text source's word (a VJO_TEXT_SOURCE_* mode, | VJO_TEXT_SOURCE_NO_SITES): set by the registered
 * Shell only (0 or < 0); read by any caller: OCR until the Shell set one, and in a process the kernel's loader
 * did not start the worker in as ON (audit-hooks F1). */
int vjoSetTextSource(uint32_t word);
int vjoGetTextSource(void);
/* Startup only: current title's profile, copied after bounded kernel-worker I/O. */
int vjoOcrHookProfile(void *dst, uint32_t cap);
#endif
/* Boot-safety guard (core/boot_guard.h); declared for the host tests too. */
#ifdef __cplusplus
extern "C" {
#endif
int vjoBootInfo(VjoBootInfo *out);       /* any process; set out->size first */
int vjoBootMark(int stage);              /* shell: VJO_BOOT_STAGE_SHELL..OK reached; 0 = the shell is inert */
int vjoMemInfo(VjoMemInfo *out);         /* shell or the registered game worker: set out->size first */
#ifdef __cplusplus
}
#endif

#endif
