/* Kernel <-> SceShell interface of Rubi. Shared by kernel/,
 * shell/ and core/ (for the plain data types). */
#ifndef VJO_API_H
#define VJO_API_H

#include <stdint.h>

#include "vjo_build.h" /* VJO_DEBUG: the debug or the normal build (docs/release-variants.md) */

/* 14: 32 KiB model reads that wait for the kernel worker, Open reports the
 * file's modification time, results report resident weights.
 * 15: results carry the memory check (free USER, needed bytes, reserve) and
 * each recognized line's crop, CTC frames and time.
 * 16: requests carry a fallback model, Open chooses the model or the fallback,
 * results report when the fallback ran.
 * 17: boot-safety guard: vjoBootInfo tells every plugin whether this boot is
 * a safe one (no hooks, the Shell and game plugins unload themselves),
 * vjoBootMark lets the Shell record the stages it reaches.
 * 18: vjoMemInfo, the self-test's kernel memory sample (exact free/total of
 * the kernel's and the caller's partitions, the kernel ledger).
 * 19: OCR captures in the game's process: the display hook copies grey rows of
 * the region straight into the game worker's buffer (vjoOcrCaptureRows); no
 * kernel capture buffer for OCR (vjoOcrReadRaw is gone). Requests carry the
 * region and flags, results the capture and a step log. vjoMemInfo reports each
 * memory query route's step and result, and the game worker may call it.
 * 20: the job's policy rides in VjoGameOcrRequest.flags (the stable-text wait,
 * its limit, waiting first, blank lines skipped); two new step kinds (settle,
 * blank). A job may publish its first pass, void it (a VJO_ROWS_SIG read from
 * row 0 it does not finish) and publish its retry's. Sizes are unchanged.
 * 21: model tiers (Small, Normal, Large; include/vjo_vocr_sizes.h). A request
 * may name the tiers its job may run (VjoGameOcrRequest.tiers, a new last
 * field) instead of a model and its fallback: the worker runs the largest that
 * fits beside that tier's reserve, and Open opens a tier's pinned model by its
 * tier (VJO_GAME_OCR_OPEN_TIER).
 * 22: VjoGameOcrRequest.speed (VJO_GAME_OCR_S_*: helper threads for parallel
 * lines, the 444 MHz clock while recognizing, no line cache), a word of its own
 * after flags; three new step kinds (parallel lines, the clock, the game's frame
 * pacing, which the kernel appends). The request grows by that word.
 * 23: the Anki screenshot. A request with VJO_GAME_OCR_F_SHOT (and the card's ID,
 * VjoGameOcrRequest.shot, a new last field) has the game worker read the whole frame
 * in colour (VJO_ROWS_RGBA), encode a JPEG in its own memory and write it through the
 * kernel's worker thread (vjoOcrShotWrite, vjoOcrShotSave) beside the card in the Anki
 * queue; results carry the shot's figures, two new step kinds (shot, save).
 * 24: the kernel's full-frame capture is gone: vjoRequestCapture and vjoReadRaw (its
 * last users, Google Lens's and the Shell's Anki JPEGs, were removed), the VjoCapture
 * buffer, VJO_CAPTURE_*, VJO_ALLOC_*, and VjoState's alloc_status, capture_result and
 * raw_stride (VjoMemInfo's capture_bytes). VjoState's capture fields are an OCR job's.
 * 25: vjoNotifyShell wakes the registered Shell's event wait after local input
 * or job completion; no new thread, event object or buffer.
 * 26: the text-presence gate: VjoGameOcrRequest.speed bit 4 (VJO_GAME_OCR_S_GATE), its weights' file
 * (VJO_GAME_OCR_OPEN_GATE, VJO_VOCR_GATE_NAME) in the kernel's broker for such a request, and a new step kind
 * (gate). Sizes are unchanged.
 * 27: vertical text (config.ini ocr_vertical, #213): VjoGameOcrRequest.flags bit 15 and
 * VJO_GAME_OCR_OPEN_VERTICAL. Reverted; main's builds from 4a7ddf8 to 297530c are API 27.
 * 28: API 26's interface again (vertical text left main, to come back as its own pull request): flags
 * bit 15 is unknown again and no Open is VJO_GAME_OCR_OPEN_VERTICAL. A new number, not 26: builds of
 * 27 exist, and a number names one interface only. Sizes are unchanged.
 * 31: native text hooks (docs/native-hooks.md). The Shell sets the text source from config.ini's
 * text_source (vjoSetTextSource, VJO_TEXT_SOURCE_*); the game worker reads it as it starts
 * (vjoGetTextSource) to decide which hooks it installs in its game. VjoGameOcrRequest.hook_pick (a new
 * last field) lets a job answer from a hook stream or from OCR only, and VjoGameOcrResult says where its
 * text came from (text_from and the hook_* fields, new, before text). Both structs grow. 29 and 30 are
 * taken by open drafts (#223 and #226: 29; #228: 30), so this is 31.
 * 32: an Anki screenshot may trim the frame's black bars (config.ini anki_screenshot_trim_bars,
 * docs/anki.md): VjoGameOcrRequest.speed bit 6 (VJO_GAME_OCR_S_TRIM), a shot's only speed bit, and a new
 * step kind (trim). Sizes are unchanged from API 28; drafts 29-31 have their own interfaces.
 * 33: combined test interface: API 31's enlarged hook request/result structs and text-source syscalls
 * with API 32's shot-trimming speed bit and step. A new number keeps existing API 31 and 32 builds from
 * exchanging requests with an interface that differs from theirs.
 * 34: optional worker-only bounded hook-learning cache through the existing file broker
 * (vjoOcrHookCache). Request/result layouts are unchanged; unavailable storage means fresh learning.
 * 36: VjoGameOcrRequest.speed bit 16 (VJO_GAME_OCR_S_DROP_BORDERS): the game worker drops a line made only
 * of a box's border read as text (core/textnorm.h; config.ini non_japanese_filter = lines, the default), and
 * its norm step's kind counts those lines. Sizes and layouts are unchanged. 35 and speed bit 7 are taken by
 * draft #245 (the debug build's PSP probe), so this is 36.
 * 41: the hook watch (docs/native-hooks.md, "A new line starts the read"): the game worker reports a new line
 * in the dialogue's hook stream between jobs (vjoOcrHookLine); the region gets a new scene at once, held
 * while the line draws, VjoState.quiet is set meanwhile and the Shell is woken with VJO_EV_REGION_QUIET, so
 * its background read starts when the game copies the line, not when the pixels settle. Sizes and layouts
 * are unchanged; the Shell's code is too. 37-40 are taken by drafts #271, #278, #281 and #282.
 * 42: vita-vn-ocr #117's speed work in the game worker's recognizers (docs/vita-vn-ocr.md, "Head gate" and
 * "Two-level head"): VjoGameOcrRequest.speed bit 18 (VJO_GAME_OCR_S_HEAD_GATE) skips the output head on the CTC
 * frames a blob's head gate calls blank, and bit 19 (VJO_GAME_OCR_S_TWO_LEVEL) runs a blob's two-level head; the
 * Shell sets them in the user's jobs from config.ini ocr_head_gate (on, the default: both; gate; two_level; off).
 * A line either skipped something of is read again with the full head where the counter re-decode needs it. The
 * model sizes' files are VOCR5_S_w8g.vocr, VOCR5_N_w8g2.vocr and VOCR5_L_w8g2.vocr; the recognize step's kind is
 * the share of frames the gate skipped, in percent. Sizes and layouts are unchanged. The interfaces of closed
 * drafts #281 (39) and #282 (40) have builds, so this is a new number.
 * 43: trusted hooks (docs/native-hooks.md, "Trusted hooks"): VjoGameOcrRequest.speed bit 20
 * (VJO_GAME_OCR_S_TRUST, config.ini hook_trust) lets a trusted source answer a job without OCR, its capture pass
 * beside VJO_GAME_RESERVE_HOOK_ONLY, and VjoGameOcrResult.hook_flags has VJO_HOOK_R_TRUSTED. Sizes and layouts
 * are unchanged.
 * 47: vertical text (config.ini ocr_vertical, vita-vn-ocr #99; docs/vita-vn-ocr.md "Vertical text"):
 * VjoGameOcrRequest.speed bits 28-29 (VJO_GAME_OCR_S_VERTICAL): 1 has the job read the region's columns, right
 * to left, with the vertical model (VJO_GAME_OCR_OPEN_VERTICAL, VJO_VOCR_VERTICAL_NAME), 2 read its rows unless
 * the line finder's rows look like vertical text, then its columns; a line read as a column carries
 * VJO_GAME_OCR_LINE_COLUMN in its flags. Sizes and layouts are unchanged. API 27 and draft #223's API 29 were the
 * same feature as flags bit 15, which stays undefined. 44-46 are taken by work in flight (VOCR6's line-finder
 * fixes, the spotter finder, draft #326's text overlay in test.17), as are speed bits 21-27, so this is 47.
 * 49: trusted hooks without waiting (docs/native-hooks.md, "No wait for a trusted line"): vjoOcrTake's wait for a
 * job is out->seq microseconds when the worker sets it lower than 100 ms; vjoOcrHookLine's bytes carry
 * VJO_HOOK_LINE_TRUSTED, which always wakes the Shell and makes the line's scene VjoState.hook_scene (a new field
 * at the end: the struct is 4 bytes longer); vjoOcrCaptureRows' VJO_ROWS_HOOK publishes a trusted answer's capture
 * as that scene without reading pixels. API numbers on main only grow: 47 is #335's (vertical text) and 48
 * #330's (the line finder).
 * 50: the line finder's fixes (bee-san/vita-vn-ocr #129, runtime/vocr_lines_fix.c): VjoGameOcrRequest.speed bits 21-23
 * (VJO_GAME_OCR_S_LINE_FIX: VOCR_FIX_DEFAULT, _JUNK, _VALLEY), VjoVocrPolicy.line_fix, config.ini ocr_line_fix.
 * Sizes and layouts are unchanged. This is #330: 47's and 49's notes kept 44 and then 48 for it, and it merged
 * after 49, so it is 50.
 * 51: a trusted answer needs no memory (docs/native-hooks.md, "No wait for a trusted line"): vjoOcrCaptureRows'
 * VJO_ROWS_HOOK also publishes the region's current scene once the region shows another than the trusted line's
 * (49 refused it with VJO_ERR_FORMAT, and the worker read the first pass: a strip beside VJO_GAME_RESERVE_HOOK_ONLY,
 * which a game with 1 MiB free refused, so the job failed although the hook had its text). FORMAT now means only
 * that the region has no screen yet. Sizes and layouts are unchanged.
 * 52: the text box suggested on first use (#358, core/region_suggest.h, docs/first-box.md): VjoGameOcrResult.
 * suggest_rows (a word before text: the result is 4 bytes longer), the band a successful job of rows on the whole
 * frame says the dialogue is in, as its rows at the screen's bottom. The request is unchanged. 51 is #354's (merged
 * first), so this is 52.
 * 53: vertical text ON prefers columns (#379; docs/vita-vn-ocr.md "Vertical text"): a request with
 * VJO_GAME_OCR_S_VERTICAL 1 reads the region's rows first, as 2 does, and its columns unless the rows look like
 * horizontal text (VjoVocrPolicy.vertical_check VJO_VOCR_CHECK_ON); with 1 or 2 a job of columns whose model cannot be
 * read or has no room (its memory check refuses it, or its weights after the check) reads the rows; the find step's
 * kinds 4 and 5 say so (a block refused after the check, the columns' capture or workspace, fails the job for memory,
 * as since API 47). A job of columns reads its columns' rows
 * once, the rectangle around them (VJO_VOCR_COLUMNS_BATCH), no longer once per column. The Shell leaves out a size whose model failed
 * with ON too. Sizes and layouts are unchanged. 52 is #369's (merged first), so this is 53. #385's audit (no new API:
 * the request and result are unchanged): with 1, a region at least as tall as wide is read as columns from the job's
 * first try, and a wider one with no row at all as rows (nothing to read).
 * 54: the text spotter as the game worker's line finder (vita-vn-ocr #149 textspot8, #128 textspot6;
 * docs/vita-vn-ocr.md "The spotter finder"): VjoGameOcrRequest.speed bit 24 (VJO_GAME_OCR_S_SPOT) and bit 26
 * (VJO_GAME_OCR_S_SPOT_REFINE), its weights' file in the kernel's broker for such a request (VJO_GAME_OCR_OPEN_SPOT,
 * VJO_VOCR_SPOT_NAME), a new step kind (spot) and block (VJO_GITEM_SPOT); the Shell sets the bits from config.ini
 * ocr_finder (spotter by default) and ocr_spotter_refine. Sizes and layouts are unchanged. API 45 was this change's
 * number while it waited (draft branch feat/vocr6-spotter-finder), then 52 and 53, which #369 and #385 took (merged
 * first); main's numbers only grow, so it is 54.
 * 46: the overlay on the game's text (docs/overlay-on-text.md, #309; closed draft #271's API 37, renumbered):
 * VjoGameOcrRequest.speed bit 25 (VJO_GAME_OCR_S_CHARS): the game worker writes where each character of the text
 * it returns is on the screen (core/charmap.h), after the text and its recognizer's copy in VjoGameOcrResult.text,
 * whose last bytes were zero; the Shell sets it in the user's jobs with config.ini overlay_on_text = on. Sizes and
 * layouts are unchanged. 43-45 and speed bits 17 and 20-24 are left to work in flight (the line finder's fixes,
 * the text spotter, vertical text, trusted hooks).
 * 52 (draft #355's own number; main's 52 is #369's): the pause for the frame (config.ini ocr_pause, #348;
 * docs/game-pause.md): VjoGameOcrRequest.speed bit 30 (VJO_GAME_OCR_S_PAUSE), and vjoOcrCaptureRows' VJO_ROWS_FRAME,
 * the whole region copied from one frame in one display call (at most VJO_GAME_OCR_PAUSE_MAX_US), into the worker's
 * frame block (VJO_GITEM_FRAME), which the job reads from; the step log's VJO_GSTEP_PAUSE. Sizes and layouts are
 * unchanged. This is #355's: #354 merged first, as 51.
 * 55: test/combined, the combined test build (v0.9.8-test.N; never merged, never main's): main's API 54 with
 * every open draft: #326's API 46 (speed bit 25, VJO_GAME_OCR_S_CHARS), #355's own number (speed bit 30,
 * VJO_GAME_OCR_S_PAUSE, VJO_ROWS_FRAME, VJO_GSTEP_PAUSE after main's VJO_GSTEP_SPOT, VJO_GITEM_FRAME after main's
 * VJO_GITEM_SPOT), #318 and #359 (no interface change). test.1 was 51, test.2 52, test.3 53, test.4 54 (main's 54
 * since is the spotter's); this build takes a number no other build has. With the characters' places a job selects
 * without API 43's trusted answers (game_ocr/main.c): it recognizes to place a hook's text. */
#define VJO_API_VERSION 55

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
