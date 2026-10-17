/* Rubi kernel module: shared state between the hooks, the worker
 * thread and the syscall exports. */
#ifndef VJO_KERNEL_H
#define VJO_KERNEL_H

#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/types.h>
#include <stdint.h>

#include "../core/ledger.h"
#include "../include/vjo_api.h"
#include "foreground.h" /* IEV_* */
#include "scene.h"
#include "triggers.h"   /* TRIG_* */

#define VJO_MAX_W 960
#define VJO_MAX_H 544
#define VJO_CHECK_EVERY_FRAMES 8 /* a region signature every 8th game frame */

typedef struct {
    SceUID evf;    /* public events (VJO_EV_*) */
    SceUID ievf;   /* internal events (IEV_*) */
    SceUID mevf;   /* MEV_DONE: a model operation finished (game_ocr.c); its syscalls wait on it */
    SceUID lock;   /* mutex: the OCR transport (game_ocr.c), the region, its signatures and the kernel strip */
    SceUID game_lock; /* mutex: game_pid, game_active, prev_game_pid changes (lifecycle.c) */

    /* foreground game (VjoForeground in foreground.h) */
    volatile SceUID game_pid;
    volatile uint32_t ocr_epoch; /* invalidates game jobs on foreground/policy changes */
    volatile int game_active;  /* the shell confirmed game_pid is a game: its VJO_GAME* mode */
    SceUID prev_game_pid;      /* confirmed game behind game_pid, still running */

    /* last game frame seen (index 0, primary head; submitted or sampled) */
    volatile uint32_t fb_pitch, fb_fmt, fb_w, fb_h;

    /* OCR region (normalized), set by the shell */
    VjoRect region;

    /* the capture of the last OCR job's region: its first pass (game_ocr.c
     * publish_capture_locked); no pixels are kept */
    uint32_t crop_w, crop_h;   /* captured pixels */
    SceneSig capture_sig;      /* of the captured region */
    uint32_t capture_scene;    /* scene of the captured pixels, 0 = unknown */
    uint32_t region_seq;       /* capture_sig is this capture's (0 = none) */
    int64_t region_us;         /* when its first pass started */
    uint32_t capture_seq;      /* bumped per job */
    uint32_t done_seq;         /* seq of the last finished first pass */

    /* change detection: the display hook writes hook_sig[(seq + 1) & 1],
     * then bumps hook_sig_seq; the worker reads hook_sig[seq & 1] */
    SceneSig hook_sig[2];
    volatile uint32_t hook_sig_seq;
    uint32_t seen_sig_seq;
    SceneTracker tracker; /* under the lock */

    /* input */
    volatile int trigger[TRIG_COUNT]; /* enum VjoTrigger, by TRIG_* */
    volatile uint32_t trigger_gen;    /* bumped when trigger changes */
    volatile int input_block;         /* the Shell's (vjoSetInputBlock); any foreground change releases it (lifecycle.c) */
    volatile SceUID shell_pid;
    volatile uint32_t suppress_mask; /* combo buttons hidden until released */
    volatile uint32_t raw_buttons;
    volatile uint32_t input_us32;      /* last player input (pad, or a touch the game read), incl. held */
    volatile uint32_t input_edge_us32; /* last press/release/touch edge; both: the system time's low
                                          32 bits, one store from any thread */
    VjoInput last_input;
} VjoKernelState;

extern VjoKernelState g;
void input_trace_reset(void);

/* log.c: ring buffer, flushed to kernel.txt by the worker */
void klog(const char *fmt, ...);
/* klog_d: a debug-build line (include/vjo_build.h, docs/release-variants.md). */
unsigned klog_ms(void); /* the system clock in ms: the t= of transition lines */
int klog_read(char *dst, int len);

/* buffers.c */
int buffers_kstrip_alloc(uint32_t bytes); /* the OCR kernel strip (lock held): 0 or the error */
uint8_t *buffers_kstrip(void);
void buffers_kstrip_free(void);

/* capture.c */
void capture_compute_crop(uint32_t fb_w, uint32_t fb_h, uint32_t *x, uint32_t *y, uint32_t *w,
                          uint32_t *h);
/* The same crop for any normalized region (w == 0: the whole frame). */
void capture_crop_of(const VjoRect *region, uint32_t fb_w, uint32_t fb_h, uint32_t *x, uint32_t *y,
                     uint32_t *w, uint32_t *h);
int region_sig_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h, SceneSig *out);
/* An armed vjoOcrCaptureRows (game_ocr.c), as the display hook copies it. */
typedef struct {
    uint32_t x0, y0, w, h, fb_w, fb_h; /* the job's region in a frame of fb_w x fb_h */
    uint32_t y, n, stride;             /* region rows [y, y + n), stride bytes apart at the destination */
    uintptr_t dst_user;                /* the worker's buffer (same process as the hook), or 0 */
    uint8_t *dst_kernel;               /* a kernel strip, or NULL */
    int sig;                           /* the rows continue g.capture_sig (capture_sig_begin) */
    int rgba;                          /* API 23, a shot: A8B8G8R8 words (4 bytes a pixel), not grey */
} OcrRowsJob;
int capture_grey_rows(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h,
                      const OcrRowsJob *job);
void capture_sig_begin(uint32_t w, uint32_t h); /* a new capture signature into g.capture_sig */

/* input.c */
int input_hooks_install(void);
void input_hooks_release(void);
void input_poll(void);
void input_trace_reset(void);

/* lifecycle.c */
int lifecycle_hooks_install(void);
void lifecycle_hooks_release(void);
int lifecycle_set_game_active(SceUID pid, int mode);

/* display.c */
int display_hook_install(void);
void display_hook_release(void);
void display_static_fb_sample(void); /* game context, from its pad calls */

/* ledger.c: the memory ledger (core/ledger.h, include/vjo_ledger_budget.h) */
enum VjoKernelLedgerItem { VJO_KL_MODULE = 0, VJO_KL_STACK, VJO_KL_OCR_STRIP, VJO_KL_POLICY, VJO_KL_COUNT };
extern VjoLedger g_kernel_ledger;
void kledger_init(void);
void kledger_log(const char *when); /* the pool and its items, to kernel.txt */

/* loader.c: the game OCR worker loaded into every game ON in games.ini at its start (#138) */
int loader_hook_install(void);  /* 0, or why the loader is off (the per-title lines still work) */
int loader_hook_release(void);  /* 0, or VJO_ERR_BUSY: a game start is still inside the hook */
void loader_tick(void);         /* the kernel worker thread: answers a starting process's question */

/* meminfo.c: the self-test's memory sample (vjoMemInfo), kernel context */
void meminfo_fill(SceUID caller_pid, const void *caller_addr, VjoMemInfo *out);

/* boot.c: boot-safety guard (core/boot_guard.h has the record and values) */
int boot_init(int api);        /* first in module_start; returns VJO_BOOT_NORMAL or VJO_BOOT_SAFE */
int boot_mode(void);
int boot_shell_seen(void);     /* the Shell plugin reported in this boot */
int boot_mark(int stage);      /* the Shell reached stage (vjoBootMark) */
void boot_mark_inert(void);    /* the Shell unloaded itself on purpose */
void boot_fault(int fault);    /* VJO_BOOT_FAULT_* observed */
void boot_info(VjoBootInfo *out);
void boot_tick(void);          /* worker thread: writes pending changes */

#define VJO_LOCK() ksceKernelLockMutex(g.lock, 1, NULL)
#define VJO_UNLOCK() ksceKernelUnlockMutex(g.lock, 1)

#endif
