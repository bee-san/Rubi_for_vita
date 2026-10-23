/* Bounded OCR-only transport between the Shell, the kernel and the game worker.
 *
 * API 19: the capture happens in the game's process. The worker asks the kernel
 * for rows of the job's region (vjoOcrCaptureRows); the kernel's display hook,
 * which runs in the game's own thread on the frame the game submits, converts
 * those rows to 8-bit grey and copies them straight into the worker's buffer in
 * the game's memory. The kernel holds no pixel buffer for OCR (a small kernel
 * strip only on the last rung of the fallback ladder), and the worker holds one
 * strip of the region, never the whole frame. The only user pointer the kernel
 * sees is the destination of one vjoOcrCaptureRows call, checked to lie in a
 * memory block of the calling process and used only until that call returns. */
#ifndef VJO_GAME_OCR_H
#define VJO_GAME_OCR_H

#include <stdint.h>
#include "vjo_api.h" /* VjoRect */

#define VJO_GAME_OCR_MODULE "VitaJPOverlay_OCR"
/* The model directory (API 23: 128 B, from 256): the kernel accepts only VJO_VOCR_MODEL_ROOT
 * (27 characters), and its file paths, the root's and a shot's (75 characters), fit. */
#define VJO_GAME_OCR_MODEL_DIR_BYTES 128u
#define VJO_GAME_OCR_TEXT_BYTES 4096u
#define VJO_GAME_OCR_MODEL_BYTES 32u
/* The kernel's one staging buffer, and the most one model Read copies: the
 * game reads and hashes the weights in pieces of this size. */
#define VJO_GAME_OCR_MODEL_CHUNK 32768u

/* VjoGameOcrRequest.flags (the Shell sets them; normal jobs use 0). */
#define VJO_GAME_OCR_F_FULL 0x1u           /* the whole frame, whatever the region */
#define VJO_GAME_OCR_F_FORCE_FALLBACK 0x2u /* self-test: the memory check refuses the model, so the fallback runs
                                              (a request by tiers: the smallest of its tiers runs) */
#define VJO_GAME_OCR_F_PROBE 0x4u          /* self-test: try every capture path and memory type, recognize nothing */
/* API 20: the job's policy (core/vocr_ocr.h VjoVocrPolicy). It rides in the
 * flags so the request, which the kernel keeps a copy of, keeps its size. */
#define VJO_GAME_OCR_F_BLANK 0x8u          /* skip found lines whose crop holds no text-like ink */
#define VJO_GAME_OCR_F_SETTLE_FIRST 0x10u  /* the screen was still changing when the job was asked: wait first */
/* Bits 5-7: the largest LM tier (include/vjo_vocr_sizes.h, 1-4) the beam + LM
 * decoder may use; 0: greedy decoding, as API 19. The worker uses the largest that fits. */
#define VJO_GAME_OCR_LM_SHIFT 5u
#define VJO_GAME_OCR_LM_MASK 0x7u
/* Bits 8-11: the confidence cascade's time ceiling for the job's recognition,
 * in 250 ms (0: no cascade). A doubtful line is read again by the larger pinned
 * model (include/vjo_vocr_sizes.h VJO_VOCR_CASCADE_NAME) while the job stays
 * within it. */
#define VJO_GAME_OCR_CASCADE_SHIFT 8u
#define VJO_GAME_OCR_CASCADE_MASK 0xFu
#define VJO_GAME_OCR_CASCADE_UNIT_US 250000u
/* Bits 12-13: the finder's boxes refined (vita-vn-ocr's vocr_lines_refine.h): 0
 * off, 1 on, 2 in PSP games only (the worker's own title: Adrenaline's
 * NPXS10028, or a PSPEMU* bubble); 3 is refused. */
#define VJO_GAME_OCR_REFINE_SHIFT 12u
#define VJO_GAME_OCR_REFINE_MASK 0x3u
#define VJO_GAME_OCR_REFINE_ON 1u
#define VJO_GAME_OCR_REFINE_PSP 2u
/* Bits 16-23: the stable-text wait, in 10 ms (0: off); bits 24-31: the most the
 * job spends waiting, in 50 ms (0: VJO_VOCR_SETTLE_MAX_US, 2 s). */
#define VJO_GAME_OCR_SETTLE_SHIFT 16u
#define VJO_GAME_OCR_SETTLE_UNIT_US 10000u
#define VJO_GAME_OCR_SETTLE_MAX_SHIFT 24u
#define VJO_GAME_OCR_SETTLE_MAX_UNIT_US 50000u
/* API 23, bit 14: the Anki screenshot (docs/anki.md). The worker reads the game's whole
 * frame in colour (VJO_ROWS_RGBA strips), encodes it as a baseline JPEG in its own memory
 * (core/jpegsw.h) and has the kernel's worker thread write it, a VJO_GAME_OCR_SHOT_CHUNK
 * at a time, to VJO_GAME_OCR_SHOT_DIR/<shot>.jpg (vjoOcrShotWrite, vjoOcrShotSave): the
 * card's screenshot, beside its record. A shot recognizes nothing, opens no model and
 * publishes no capture; it carries no other flag but F_FULL, no tiers and no speed but
 * (API 32) VJO_GAME_OCR_S_TRIM. */
#define VJO_GAME_OCR_F_SHOT 0x4000u
#define VJO_GAME_OCR_F_ALL 0xFFFF7FFFu
/* The shot's file: the Anki queue (core/anki_queue.h VJO_QUEUE_DIR; a host test checks the
 * two agree), named by the card's queue ID (32 lowercase hex digits), at most
 * VJO_GAME_OCR_SHOT_MAX bytes (core/anki.h VJO_ANKI_PICTURE_MAX, what an export reads). */
#define VJO_GAME_OCR_SHOT_DIR "ux0:data/VitaJPOverlay/anki_queue"
#define VJO_GAME_OCR_SHOT_ID_BYTES 36u
#define VJO_GAME_OCR_SHOT_MAX (192u * 1024u)
#define VJO_GAME_OCR_SHOT_CHUNK 32768u /* the kernel's staging buffer (VJO_GAME_OCR_MODEL_CHUNK) */
/* One deadline for the whole JPEG file, including every chunk and its close.
 * Shell must leave time for capture/encoding before this deadline expires. */
#define VJO_GAME_OCR_SHOT_FILE_US 30000000u
#define VJO_GAME_OCR_SHOT_TIMEOUT_US 35000000u
/* API 22 (ocr-speed), VjoGameOcrRequest.speed: bits 0-1 the helper threads that
 * recognize lines in parallel (0-2, VjoVocrPool: config ocr_threads minus one);
 * bit 2 the ARM clock at 444 MHz while the job recognizes (ocr_boost_clock, on
 * by default; never lowered); bit 3 the line cache neither read nor filled, and emptied (the
 * self-test's stages: each recognizes what it reads); API 26, bit 4 the text-presence gate (below); API 36, bit 16
 * the border rule (below). Other bits are refused, as unknown flags are. */
#define VJO_GAME_OCR_S_HELPERS_MASK 0x3u
#define VJO_GAME_OCR_S_BOOST 0x4u
#define VJO_GAME_OCR_S_NOCACHE 0x8u
/* API 26, bit 4: the text-presence gate (vita-vn-ocr #100, runtime/vocr_textgate.c; core/vocr_ocr.h
 * VjoVocrPolicy.gate, config ocr_text_gate). A region the gate says holds no text returns no line: nothing is
 * recognized. It reads the rows the finder reads, no strip of its own, and its weights from the pinned file
 * VJO_VOCR_GATE_NAME beside the models (VJO_GAME_OCR_OPEN_GATE). It is in `speed` because the flags have one
 * bit left. */
#define VJO_GAME_OCR_S_GATE 0x10u
/* API 36, bit 16: the text the job returns (OCR's or a hook's) drops a line made only of a box's border read as
 * text (core/textnorm.h, the border rule; its norm step counts them); without it such a line stays. The Shell
 * sets it in the user's jobs with config non_japanese_filter = lines, the default (the self-test's stages read as
 * before, as with the gate). Not bit 5 (draft #228's VJO_GAME_OCR_S_SPOT), bit 7 (draft #245's PSP probe) or
 * bits 8-15 (draft #226's frame pacing), so all four can merge. */
#define VJO_GAME_OCR_S_DROP_BORDERS 0x10000u
/* API 35, bit 7: the debug build's read-only PSP probe (config.ini psp_probe; docs/native-hooks.md "PSP probe
 * (debug build)"): after the job, once its result is with the kernel, the debug build's worker in a PSP game
 * reads and logs what PSP hooks will need (game_ocr/psp_probe.c). Both builds' kernels and workers accept it
 * on an OCR job, so the two builds keep one interface; only the debug build's Shell sets it, and the normal
 * build's worker has no probe. Not bit 5 (draft #228's VJO_GAME_OCR_S_SPOT) nor bits 8-15 (draft #226's). */
#define VJO_GAME_OCR_S_PSP_PROBE 0x80u
#define VJO_GAME_OCR_S_ALL 0x1009Fu
/* API 32, bit 6, a shot's only bit (VJO_GAME_OCR_F_SHOT; refused on an OCR job, as every other bit is on a
 * shot): trim the frame's black bars (config.ini anki_screenshot_trim_bars, core/shottrim.h): the JPEG is of
 * the picture between the letterbox or pillarbox bars the game worker finds as it reads the frame's rows.
 * Not bit 5: draft #228's scan screen (API 30) has it (VJO_GAME_OCR_S_SPOT), so the two can merge. */
#define VJO_GAME_OCR_S_TRIM 0x40u
#define VJO_GAME_OCR_S_SHOT_ALL VJO_GAME_OCR_S_TRIM
#define VJO_PACE_SLOW_US 25000 /* a frame gap over 25 ms: a dropped frame at 60 fps */

typedef struct {
    uint32_t size;
    uint32_t seq;         /* Assigned by Submit; never reused within this kernel module's lifetime. */
    uint32_t capture_seq; /* Assigned by Submit: VjoState.done_seq reaches it once the job's first
                             pass over the region is in (vjoOcrCaptureRows with VJO_ROWS_LAST) */
    uint32_t flags;       /* VJO_GAME_OCR_F_* */
    uint32_t speed;       /* API 22: VJO_GAME_OCR_S_* */
    /* Filled by Submit: the kernel's region (normalized; w == 0 is the whole frame), the size of
     * the game's last frame and the region's pixels in it, which every row request reads. */
    VjoRect region;
    uint32_t fb_width, fb_height;
    uint32_t x, y, width, height;
    char model[VJO_GAME_OCR_MODEL_BYTES];
    char model_dir[VJO_GAME_OCR_MODEL_DIR_BYTES];
    /* A smaller pinned model the worker runs instead when the game's free
     * memory has room for it but not for `model`; "" for none. */
    char fallback[VJO_GAME_OCR_MODEL_BYTES];
    /* API 21: the model tiers this job may run (bit t - 1 for tier t of
     * include/vjo_vocr_sizes.h: 1 Small, 2 Normal, 4 Large), or 0 for a request by
     * model name (model, then fallback, as before). With tiers the worker runs the
     * largest of them whose job fits beside that tier's reserve, else the next one
     * down, and opens each by its tier (VJO_GAME_OCR_OPEN_TIER), never by `model`
     * (which names the largest, for the logs) or `fallback` (""). */
    uint32_t tiers;
    /* API 23: a shot's card (VJO_GAME_OCR_F_SHOT): its queue ID, 32 lowercase hex digits and a
     * NUL; the kernel writes VJO_GAME_OCR_SHOT_DIR/<shot>.jpg, a name no caller supplies. Empty for
     * an OCR job. */
    char shot[VJO_GAME_OCR_SHOT_ID_BYTES];
    /* API 31, native text hooks (docs/native-hooks.md): what this job may answer from.
     * VJO_HOOK_PICK_AUTO: a nonempty Luna site without recognition, else OCR with a regular hook's
     * agreeing text (the worker may learn that source and answer without recognition between checks); a stream ID
     * 1..VJO_HOOK_STREAMS: that stream's latest text, pinned, without recognition (OCR when it has none);
     * VJO_HOOK_PICK_OCR: OCR only. Normal Shell jobs use AUTO; explicit picks are for diagnostics. */
    uint32_t hook_pick;
} VjoGameOcrRequest;

/* API 31: hook streams (game_ocr/hooks.h). IDs 1..VJO_HOOK_SITE_STREAMS are table sites (Luna's or
 * Rubi's own), the rest CALL callers (the game's string copies, keyed by caller); an ID names the same
 * stream for the whole game session. */
#define VJO_HOOK_SITE_STREAMS 8u
#define VJO_HOOK_CALL_STREAMS 12u
#define VJO_HOOK_STREAMS (VJO_HOOK_SITE_STREAMS + VJO_HOOK_CALL_STREAMS)
#define VJO_HOOK_PICK_AUTO 0u
#define VJO_HOOK_PICK_OCR 0xFFu
/* A pick the kernel's vjoOcrSubmit accepts in an OCR job (a shot's is VJO_HOOK_PICK_AUTO). */
#define VJO_HOOK_PICK_OK(p) ((p) <= VJO_HOOK_STREAMS || (p) == VJO_HOOK_PICK_OCR)
/* VjoGameOcrResult.hook_streams: bit (id - 1) for each stream holding text, for diagnostics.
 * Sites 1..8 and CALL callers 9..20 need not be contiguous. */
#define VJO_HOOK_STREAM_BIT(id) (1u << ((id) - 1u))
/* VjoGameOcrResult.text_from: 0 = OCR (or no text), else the stream ID whose text text[] holds. */
#define VJO_TEXT_FROM_OCR 0u
/* VjoGameOcrResult.hook_flags. */
#define VJO_HOOK_R_AGREED 0x01u /* text_from's stream agreed with this job's OCR text (hook_score) */
#define VJO_HOOK_R_LOCKED 0x02u /* text_from's stream is locked: two agreements in a row */
#define VJO_HOOK_R_PINNED 0x04u /* the request pinned text_from's stream (hook_pick) */
#define VJO_HOOK_R_NO_OCR 0x08u /* answered without recognition (the region's first capture pass only) */
#define VJO_HOOK_R_SITE 0x10u   /* text_from is a table site (else a CALL caller) */
#define VJO_HOOK_R_UNLOCK 0x20u /* this job unlocked the stream that was locked (two disagreements) */
#define VJO_HOOK_R_CALLS_FULL 0x40u /* every CALL slot is taken: a new caller was dropped or replaced one */
#define VJO_HOOK_R_UNCONFIRMED 0x80u /* a Rubi verified table site answered with no OCR agreement ever: OCR had no memory */

/* vjoOcrModelOpen's `which`: the request's model or its fallback, or (API 20)
 * the LM of tier which - VJO_GAME_OCR_OPEN_LM + 1, up to the request's LM tier,
 * or the cascade's model when the request gives a cascade ceiling; or (API 21,
 * a request by tiers) the model of tier which - VJO_GAME_OCR_OPEN_TIER + 1: one of
 * the request's tiers, or with a cascade ceiling the tier above its largest
 * (the cascade's model). A request by tiers opens no MODEL, FALLBACK or CASCADE;
 * one by name opens no TIER. */
#define VJO_GAME_OCR_OPEN_MODEL 0u
#define VJO_GAME_OCR_OPEN_FALLBACK 1u
#define VJO_GAME_OCR_OPEN_LM 2u
#define VJO_GAME_OCR_OPEN_CASCADE 8u
#define VJO_GAME_OCR_OPEN_TIER 9u
/* API 26: the text-presence gate's weights (include/vjo_vocr_sizes.h VJO_VOCR_GATE_NAME), for a request with
 * VJO_GAME_OCR_S_GATE only. */
#define VJO_GAME_OCR_OPEN_GATE 17u

/* VjoGameOcrResult.weights: how this job had verified weights. */
#define VJO_GAME_OCR_WEIGHTS_NONE 0u     /* no line found, or the job failed first */
#define VJO_GAME_OCR_WEIGHTS_READ 1u     /* read from the card and SHA-256 checked */
#define VJO_GAME_OCR_WEIGHTS_RESIDENT 2u /* kept from an earlier job; the file is unchanged */

#define VJO_GAME_OCR_LINES 16u /* the line finder's maximum (vocr_lines.h) */

/* How rows reached the worker (VjoGameOcrRows.mode, VjoGameOcrResult.source, probe steps). */
#define VJO_GAME_OCR_SRC_NONE 0u
#define VJO_GAME_OCR_SRC_HOOK 1u    /* the display hook copies grey rows into the worker's buffer */
#define VJO_GAME_OCR_SRC_KSTRIP 2u  /* the hook fills a small kernel strip; the worker's call copies it out */
#define VJO_GAME_OCR_SRC_USER 3u    /* the worker reads sceDisplayGetFrameBuf's frame itself (probe only) */

/* Where a block of the worker lives (steps, VjoGameOcrResult.capture_memory). */
#define VJO_GAME_OCR_MEM_NONE 0u
#define VJO_GAME_OCR_MEM_USER 1u    /* USER_RW, 4 KiB pages, cached */
#define VJO_GAME_OCR_MEM_CDRAM 2u   /* USER_CDRAM_RW, 256 KiB units, uncached */
#define VJO_GAME_OCR_MEM_PHYCONT 3u /* USER_MAIN_PHYCONT_NC_RW, 1 MiB units, uncached */
#define VJO_GAME_OCR_MEM_KERNEL 4u  /* a kernel strip (VJO_GAME_OCR_SRC_KSTRIP) */

/* One step of a job, for the logs and the self-test: what was tried, its native
 * result, the bytes involved and the game's free memory right after it
 * (sceKernelGetFreeMemorySize: main, CDRAM, phycont; main holds the API's error,
 * the others 0, when the query failed). */
enum {
    VJO_GSTEP_NONE = 0,
    VJO_GSTEP_MEM,       /* a memory sample (rc: the query's) */
    VJO_GSTEP_CHECK,     /* the memory check: rc 0 passed, 1 refused; bytes needed; arg 1 = the fallback's, which
                            then runs (sum 1: forced by the self-test). API 21, a request by tiers: one check
                            per tier tried, largest first; kind = LM tier | 8 (with the cascade) | the tier
                            checked << 4 (VJO_GCHECK_TIER) | 0x40 (forced by the self-test); arg 0 for the
                            largest requested tier, 1 for a smaller one; sum = the worker's own resident
                            bytes the check counted as free (released first); the reserve is the tier's
                            (VJO_GAME_RESERVE_*: of the cascade's tier with the cascade) */
    VJO_GSTEP_ALLOC,     /* kind VJO_GAME_OCR_MEM_*, arg VJO_GITEM_*; rc the block uid or the error */
    VJO_GSTEP_FREE,      /* arg VJO_GITEM_* */
    VJO_GSTEP_PASS,      /* a pass over the region: kind VJO_GAME_OCR_SRC_*, arg captures, bytes copied */
    VJO_GSTEP_TEAR,      /* rows read differently than before: arg how many, rc the tries, bytes when the last
                            try began (us since the job started; 0 with one try), us when it was logged,
                            after the tries */
    VJO_GSTEP_SIG,       /* the first pass is in: rc of vjoOcrCaptureRows' publication */
    VJO_GSTEP_FIND,      /* lines found: arg the count, rc the finder's */
    VJO_GSTEP_LOAD,      /* weights: kind VJO_GAME_OCR_WEIGHTS_*, bytes; arg 1: read by a job that found no
                            line (core/vocr_ocr.h vjo_vocr_warm) */
    VJO_GSTEP_RECOGNIZE, /* all lines: arg recognized, rc */
    VJO_GSTEP_FALLBACK,  /* API 19 only: the fallback model runs, arg 1 = forced (now the fallback's CHECK) */
    VJO_GSTEP_FB,        /* the frame: kind 1 = the kernel's, 3 = sceDisplayGetFrameBuf's; bytes = pitch,
                            arg = width, sum = pixel format, rc the query's */
    VJO_GSTEP_PROBE,     /* a probe capture: kind VJO_GAME_OCR_SRC_*, arg VJO_GAME_OCR_MEM_*, sum the
                            rows' checksum, bytes copied */
    VJO_GSTEP_SETTLE,    /* API 20, the stable-text wait: rc VJO_GSETTLE_*, arg passes read waiting, bytes row
                            changes to a new state seen (text typing out), sum = distinct rows that went back
                            to their previous state (an icon) << 16 | such returns (at most 0xFFFF), us the wait */
    VJO_GSTEP_BLANK,     /* API 20, a found line skipped as blank: arg its index, rc the job's skipped lines,
                            bytes = width << 16 | height, sum = contrast (98th - 2nd percentile grey) << 16 |
                            sharp edges per 10,000 pixels (core/vocr_ocr.h VjoVocrInk), us the job's time
                            checking lines */
    VJO_GSTEP_NORM,      /* API 20, the text normalised (core/textnorm.h) before it is returned, the
                            recognizer's own text kept after its NUL: arg the changes, bytes the full-width
                            forms folded, sum = tildes << 24 | dashes << 16 | ellipses << 8 | 一 made ー << 4 |
                            ー made 一 (each at most 255, the last two 15), us the time; API 36: kind the lines
                            of a border read as text it dropped (VJO_GAME_OCR_S_DROP_BORDERS; at most 255).
                            Logged when the text is not empty, or when dropping lines emptied it */
    VJO_GSTEP_LM,        /* API 20, the beam + LM decoder: kind the LM tier, arg lines it changed, rc 0 or why the
                            job read greedily (VJO_E_*), bytes = VJO_GLM_* | lines under VJO_GAME_OCR_LOW_CONF << 16 |
                            lines decoded, sum the LM's load time (us), us the decoding */
    VJO_GSTEP_CASCADE,   /* API 20, the confidence cascade: kind the lines it read as empty where the job's model
                            read text, arg lines read again with the larger model, rc 0 or
                            why it could not run (VJO_E_*), bytes = VJO_GLM_READ / _RESIDENT (its weights) | lines
                            whose text it changed << 24 | doubtful lines left for the time ceiling << 16 | its CTC
                            frames, sum its load time (us), us the re-reading (not in RECOGNIZE's us) */
    VJO_GSTEP_REFINE,    /* API 20, the finder's boxes refined: kind the mode (VJO_GAME_OCR_REFINE_*), arg the
                            lines they became, rc 0 or the first refusal (VOCR_LINES_E_*: that box kept as found),
                            bytes = boxes refined << 16 | boxes not split for the 16-line cap << 8 | boxes split,
                            sum the rows read, us the time */
    VJO_GSTEP_PAR,       /* API 22, parallel lines: arg helpers used, rc lines on helpers, bytes the
                            helpers' workspace and stacks, sum helpers that did not start << 24 | lines
                            from the cache << 16 | the recognized lines' lowest share of a core << 8 |
                            their mean share (percent, run time / wall time; 0 unknown), us the
                            recognition */
    VJO_GSTEP_CLOCK,     /* API 22, ocr_boost_clock: arg the ARM MHz before, bytes during, sum after,
                            rc the set call's result (or 1: already at 444 or more, not raised) */
    VJO_GSTEP_PACE,      /* API 22, added by the kernel: the game's frames during the job, arg how many,
                            rc the longest gap (us), bytes their sum (us), sum the gaps over
                            VJO_PACE_SLOW_US; and since the job before: us how many frames, main their
                            sum, cdram the longest gap, phycont the gaps over VJO_PACE_SLOW_US */
    VJO_GSTEP_SHOT,      /* API 23, a shot's encoding, one per try: kind the JPEG quality, arg the try (1-3; API 32:
                            1-4, a frame whose bars came to another picture than its first rows' read again), rc 0
                            or why it stopped (VJO_E_TOO_LARGE: over VJO_GAME_OCR_SHOT_MAX, the next try encodes a
                            lower quality), bytes the JPEG's, sum the strips read, us the try */
    VJO_GSTEP_SAVE,      /* API 23, a shot's file: kind the chunks written, rc 0 or the kernel's error,
                            bytes written, us the writes and the save */
    VJO_GSTEP_GATE,      /* API 26, the text-presence gate (VJO_GAME_OCR_S_GATE), once per job (its last try)
                            whose finder found a line, or whose gate could not run: kind its mode (0 region, 1
                            the finder's boxes), arg the lines it stopped (0: it saw text and kept them), rc 0 or
                            why it could not decide (VJO_E_*, VOCR_LINES_E_*: the lines are kept), bytes its
                            score + 129 (0..256) | its threshold + 129 << 16 | VJO_GLM_READ (its weights were
                            read from the card in this job), sum the rows it read, us its time (its share of the
                            finder's pass and its decision) */
    VJO_GSTEP_TRIM,      /* API 32, a shot's black bars (VJO_GAME_OCR_S_TRIM), once: kind VJO_TRIM_* of
                            core/shottrim.h (0 no bars, 1 trimmed, 2 kept whole: bars that frame no centred
                            picture, or a black band over 3/10 of the frame at its top), arg how (0 decided by the
                            first rows, 1 the first rows' guess, every row checked, 2 every row came to another
                            picture: read again), bytes = the picture's x << 16 | y, sum = its width << 16 | height
                            (the JPEG's), us the scan's time */
    VJO_GSTEP_COUNT
};
#define VJO_GLM_READ 0x80000000u     /* the LM was read from the card and checked in this job */
#define VJO_GLM_RESIDENT 0x40000000u /* kept from an earlier job */
/* VJO_GSTEP_CHECK kind, a request by tiers (API 21): the tier checked, and forced by the self-test. */
#define VJO_GCHECK_TIER(kind) (((unsigned)(kind) >> 4) & 0x3u)
#define VJO_GCHECK_CASCADE 0x8u
#define VJO_GCHECK_FORCED 0x40u
#define VJO_GAME_OCR_LOW_CONF 700u   /* a line's confidence (VjoGameOcrLine.flags) under 0.7: doubtful */
/* VJO_GSTEP_SETTLE rc. */
#define VJO_GSETTLE_NONE 0     /* not asked for, or no line to read */
#define VJO_GSETTLE_HELD 1     /* the rows held still settle long before the lines were read */
#define VJO_GSETTLE_TIMEOUT 2  /* the job's waiting budget ran out first: read without it */
/* VJO_GSTEP_ALLOC/FREE arg: which block. */
#define VJO_GITEM_CAPTURE 1u   /* "VjoGameVocrCapture": the strip and the row checksums */
#define VJO_GITEM_WORKSPACE 2u /* "VjoGameVocr": the finder's then the recognizer's arena */
#define VJO_GITEM_WEIGHTS 3u   /* "VjoGameVocrWeights" */
#define VJO_GITEM_PROBE 4u     /* a probe's strip */
#define VJO_GITEM_LINE 5u      /* "VjoGameVocrLine": a line taller than a strip, read whole */
#define VJO_GITEM_LM 6u        /* "VjoGameVocrLM": the beam + LM decoder's LM (API 20) */
#define VJO_GITEM_CASCADE 7u   /* "VjoGameVocrCascade": the confidence cascade's larger model (API 20) */
#define VJO_GITEM_SHOT 8u      /* "VjoGameShot": a shot's colour strip, encoder and JPEG (API 23) */
#define VJO_GITEM_COUNT 9u

typedef struct {
    uint8_t id;      /* VJO_GSTEP_* */
    uint8_t kind;
    uint16_t arg;
    int32_t rc;
    uint32_t bytes;
    uint32_t sum;
    int32_t free_main, free_cdram, free_phycont;
    uint32_t us;     /* duration, or the time since the job started for instant steps */
} VjoGameOcrStep;

#define VJO_GAME_OCR_STEPS 20u

/* One recognized line: crop size, CTC frames, recognition time and (API 20)
 * flags: its confidence in 1/1000, 1023 when not read with the top K, and
 * whether the LM decoded (0x4000) and changed (0x8000) it (core/vocr_ocr.h
 * VJO_VOCR_LINE_*). */
typedef struct {
    uint16_t width, height, frames, flags;
    uint32_t us;
} VjoGameOcrLine;

typedef struct {
    uint32_t size, seq;
    int32_t rc;
    uint32_t workspace_bytes, lines, find_us, load_us, recognize_us;
    int32_t io_error; /* model I/O error, separate from recognized text */
    int32_t cleanup_status; /* Zero only after workspace and file cleanup completed. */
    uint32_t weights;        /* VJO_GAME_OCR_WEIGHTS_* */
    uint32_t resident_bytes; /* weights block kept in the game for the next job (page-rounded), 0 none */
    /* The model file's modification time could not be read (native error):
     * every job then reads and checks the weights again. */
    int32_t stat_error;
    /* The memory check, also when it refused the job: the game's free USER
     * bytes (or a negative native error, or -1 when unavailable), what the
     * job had to allocate (page-rounded workspace, plus weights unless they
     * were resident) and the reserve always left to the game. 0/0/0 when the
     * job ended before the check. */
    int32_t free_user;
    uint32_t need_workspace, need_weights, reserve_bytes;
    /* 1: the request's model did not fit the memory check, so the check (and
     * the job, if it passed) used the fallback; need_* above are then the
     * fallback's and these the model's. API 21, a request by tiers: how many
     * tiers below the largest requested one the tier the check chose is (0: the
     * largest; 1 or 2: a smaller one ran), with need_* that tier's and these the
     * largest's. */
    uint32_t fallback, model_need_workspace, model_need_weights, model_release_bytes;
    /* The worker's own resident weights of another model, which the check
     * counted as free because the job releases them first (else 0). */
    uint32_t release_bytes;
    /* Recognition: CTC frames over all lines, and each recognized line. */
    uint32_t frames, recognized;
    VjoGameOcrLine line[VJO_GAME_OCR_LINES];
    /* API 19, the capture: the region read and the frame it came from. */
    uint32_t region_x, region_y, width, height;
    uint32_t fb_width, fb_height, fb_pitch, fb_format;
    uint32_t source;         /* VJO_GAME_OCR_SRC_* the rows came through */
    uint32_t capture_memory; /* VJO_GAME_OCR_MEM_* of the capture block */
    uint32_t capture_bytes;  /* the capture block: one strip and the row checksums, page-rounded */
    uint32_t strip_rows;     /* rows per strip */
    uint32_t captures;       /* strips captured (one game frame each) */
    uint32_t tears;          /* rows that read differently on a later pass: the screen changed */
    uint32_t capture_us;     /* time spent waiting for and copying strips */
    uint32_t region_sum;     /* FNV-1a over the region's grey rows on the first pass */
    uint32_t n_steps;        /* entries of step[] (the first VJO_GAME_OCR_STEPS of the job) */
    VjoGameOcrStep step[VJO_GAME_OCR_STEPS];
    /* API 23, a shot: the JPEG written (bytes, 0 for none), the quality and the tries it was
     * encoded in, its block (page-rounded) and where it was (VJO_GAME_OCR_MEM_*), the encoding's
     * time (strips included) and the file's (the kernel's writes and its save). The rows' source,
     * strips, strip rows and waits are in source, captures, strip_rows and capture_us. */
    /* On a pending Read (return 1), nonzero shot_bytes means the encoded
     * screenshot is frozen: closing the overlay can no longer change it.
     * It is a progress marker then, not a committed file size. */
    uint32_t shot_bytes, shot_quality, shot_tries, shot_block, shot_memory, shot_encode_us, shot_write_us;
    /* API 31, native text hooks: where text[] came from (VJO_TEXT_FROM_OCR or a stream ID), how
     * (VJO_HOOK_R_*), the best stream's similarity to this job's OCR text (0..1000; 0 when not compared;
     * text_from's own when that stream replaced OCR's text), the streams holding text as the job began
     * (bit ID - 1 of each), and what the worker installed as its game started: the text source word it
     * applied (a VJO_TEXT_SOURCE_* in the low byte, with VJO_TEXT_SOURCE_NO_SITES when no table site was
     * tried because of it), table sites hooked << 16 | CALL imports hooked, and sites refused. A
     * failed result carries them too, with text_from and hook_flags 0 (it has no text). */
    uint32_t text_from, hook_flags, hook_score, hook_streams;
    uint32_t hook_mode, hook_installed, hook_refused;
    /* UTF-8 OCR text on success; every failed result carries empty text. API 20:
     * the text is normalised (core/textnorm.h) and the recognizer's own text
     * follows its NUL (vjo_text_raw), for the log. */
    char text[VJO_GAME_OCR_TEXT_BYTES];
} VjoGameOcrResult;

/* Model Open's report of the file: its pinned length and the version the
 * game keys its resident weights on. */
typedef struct {
    uint32_t size;      /* sizeof(VjoGameOcrModelInfo) */
    uint32_t bytes;     /* file length; equals the pinned model size */
    int32_t stat_rc;    /* 0, or the native error of the modification-time query */
    uint32_t reserved;
    uint64_t mtime;     /* last modification, packed (kernel/game_ocr.c); 0 if unknown */
} VjoGameOcrModelInfo;

/* vjoOcrCaptureRows: rows [y, y + n) of the claimed job's region, as 8-bit grey
 * (77 R + 150 G + 29 B) >> 8, width bytes each, dst_stride apart in dst. */
#define VJO_ROWS_SIG 0x1u  /* these rows continue the job's first pass from row 0 (scene signature) */
#define VJO_ROWS_LAST 0x2u /* with SIG: the first pass ends here; the capture is published */
/* API 23: the rows in colour, as A8B8G8R8 words (R in the low byte, the frame's format
 * expanded as core/fbgrey.h does), 4 bytes a pixel: a shot's rows, and only a shot's; never
 * with SIG or LAST (a shot publishes no capture). dst_stride >= 4 x the width. */
#define VJO_ROWS_RGBA 0x4u
#define VJO_GAME_OCR_ROWS_MAX 64u  /* rows per call */
#define VJO_GAME_OCR_KSTRIP_MAX (64u * 1024u) /* the kernel strip's largest size */

typedef struct {
    uint32_t size;       /* sizeof(VjoGameOcrRows) */
    uint32_t seq;        /* the claimed job */
    uint32_t y, n;       /* region rows; 1 <= n <= VJO_GAME_OCR_ROWS_MAX */
    uint32_t mode;       /* VJO_GAME_OCR_SRC_HOOK or VJO_GAME_OCR_SRC_KSTRIP */
    uint32_t flags;      /* VJO_ROWS_* */
    uint32_t dst_stride; /* >= the region's width */
    uint32_t reserved;
} VjoGameOcrRows;

typedef struct {
    uint32_t size;       /* sizeof(VjoGameOcrRowsInfo), set by the caller */
    int32_t rc;          /* rows copied, or the error */
    uint32_t fb_width, fb_height, fb_pitch, fb_format; /* the frame the rows came from */
    uint32_t wait_us;    /* from the request to the end of the copy */
    uint32_t copy_us;    /* the copy itself, in the game's display call */
    uint32_t kstrip_bytes; /* VJO_GAME_OCR_SRC_KSTRIP: the kernel strip held for this call */
    int32_t kstrip_rc;   /* its allocation's result */
    uint32_t armed;      /* 1: the request reached the display hook (a BUSY then means its copy may
                            still write dst); 0: refused before (a BUSY then is a previous copy not
                            yet tidied: ask again shortly) */
} VjoGameOcrRowsInfo;

#ifdef __cplusplus
extern "C" {
#endif
int vjoOcrRegister(void); /* Current active game, verified module; retry at startup. */
int vjoOcrTake(VjoGameOcrRequest *out); /* 0 claimed, 1 no job, <0 error. */
int vjoOcrCancelled(uint32_t seq); /* Worker: 0 active, 1 cancelled, <0 stale. */
/* Worker: rows of the claimed job's region into dst (memory of the calling
 * process), copied by the display hook from the next frame the game shows, or
 * through a kernel strip (mode). Waits for that frame (bounded) and returns the
 * rows copied; VJO_ERR_TIMEOUT without a frame, VJO_ERR_CANCELLED when the job
 * was cancelled meanwhile, VJO_ERR_FORMAT when the frame's size or pixel format
 * changed. When it returns the kernel holds nothing of dst. */
int vjoOcrCaptureRows(const VjoGameOcrRows *request, void *dst, VjoGameOcrRowsInfo *info);
int vjoOcrComplete(const VjoGameOcrResult *result); /* Worker: publish or drain. */
int vjoOcrSubmit(const VjoGameOcrRequest *request); /* Shell: sequence >0. */
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out); /* Shell: 0 done, 1 pending. */
int vjoOcrCancel(uint32_t seq); /* Signals cancellation; worker must acknowledge cleanup. */
/* Model-only broker. The kernel worker thread does the file I/O; each call
 * waits (bounded, without the kernel lock) for that thread to finish the
 * operation. VJO_ERR_BUSY means it is still queued/in flight: repeat the exact
 * operation after yielding. Open opens the claimed request's model or its
 * fallback (which: VJO_GAME_OCR_OPEN_*; never a caller path), returns the
 * verified file length and, when info is non-NULL, copies a
 * VjoGameOcrModelInfo there. Read returns copied bytes (possibly short or
 * EOF), at most VJO_GAME_OCR_MODEL_CHUNK. Close drains even a cancelled job.
 * The kernel never stores a user pointer, accepts paths, or exports a file FD. */
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info);
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t bytes, void *dst);
int vjoOcrModelClose(uint32_t seq);
/* API 23, a shot's JPEG (a VJO_GAME_OCR_F_SHOT job only), through the same broker and its one
 * staging buffer, the other way: Write copies bytes [offset, offset + bytes) of the JPEG from
 * src (the calling worker's memory), in order from 0, at most VJO_GAME_OCR_SHOT_CHUNK a call
 * and VJO_GAME_OCR_SHOT_MAX in all, the first starting with FF D8 FF (a JPEG's SOI and a
 * marker), and the kernel's worker thread writes them to VJO_GAME_OCR_SHOT_DIR/<shot>.jpg.tmp,
 * which the first creates; it returns the bytes written. Save, once every byte is (bytes: their
 * total, the last two FF D9, the EOI): that thread flushes and closes the file, renames it to
 * <shot>.jpg and syncs the memory card; 0, or the native error (the .tmp is then removed).
 * VJO_ERR_BUSY: repeat the exact call. vjoOcrModelClose abandons an unsaved file (closed and
 * removed), as a cancellation or the worker's exit does. The path is the kernel's own. */
int vjoOcrShotWrite(uint32_t seq, uint32_t offset, uint32_t bytes, const void *src);
int vjoOcrShotSave(uint32_t seq, uint32_t bytes);
/* Optional hook-learning cache, through the same file worker/staging buffer. Only the worker of a
 * claimed OCR job may call it, with no model file open. No caller path or pointer is retained.
 * LOAD copies exactly BYTES and returns BYTES (0 if absent); SAVE copies BYTES in and returns 0 after a
 * recoverable temp/backup rename. Invalid storage is a negative error; BUSY means repeat the same call.
 * ModelClose drains a failed/cancelled cache operation too. */
#define VJO_GAME_OCR_HOOK_CACHE_BYTES 3088u
#define VJO_GAME_OCR_HOOK_CACHE_LOAD 0u
#define VJO_GAME_OCR_HOOK_CACHE_SAVE 1u
int vjoOcrHookCache(uint32_t seq, uint32_t operation, void *data);
#ifdef __cplusplus
}
#endif

#endif
