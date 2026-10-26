/* vita-vn-ocr backend: bee-san/vita-vn-ocr's polarity-agnostic dialogue line
 * finder and tiny CTC recognizer (plain C99, no malloc), with the int8 weights
 * published in its v0.3.0 (VOCR5, the model tiers), v0.2.5-vocr3.1 (VOCR3),
 * v0.2.5-vocr4.1 (VOCR4-W20) and v0.2.0 releases. Built with
 * -DVJO_WITH_VOCR=ON; see docs/vita-vn-ocr.md.
 *
 * API 19: a job never holds the region. It reads the region in strips of grey
 * rows (VjoGreySource: in the game, the display hook copies each strip from the
 * game's framebuffer), finds the lines with the runtime's row-callback finder
 * (vocr_lines_rows.h) and recognizes each line straight from the strip that
 * holds it. Its memory is a workspace (the finder's arena, then the
 * recognizer's, and the runtime's two structs: vjo_vocr_job_bytes) and a
 * capture block (one strip, the rows' checksums: vjo_vocr_capture_bytes, under
 * 64 KiB page-rounded). Every row's checksum is kept the first time it is
 * read and compared whenever it is read again; a row that changed means the
 * screen changed while it was read, and the job starts over (VJO_VOCR_TRIES in
 * all, then VJO_E_SCREEN_CHANGED). A row that goes back to a state it had a
 * little earlier in the job (a blinking or bobbing "next" icon, a looping
 * background) is not a change: each row keeps its recent states. The weights live in a separate caller-owned
 * block (VjoVocrResident) that is read and checked once and then kept for later
 * jobs while the file's size and stamp stay the same; nothing else is
 * allocated. vjo_vocr_ocr() is the one-shot form: weights and job share one
 * workspace (vjo_vocr_workspace_bytes()) and are read for every job.
 *
 * Two optional checks (VjoVocrPolicy; vjo_vocr_ocr_policy): the stable-text
 * wait recognizes nothing until every row of the region has held still for
 * settle_us (text that types out character by character is read once it is
 * whole, never half-typed), and the blank-line check skips a found line whose
 * crop holds no text-like ink (vjo_vocr_ink) instead of recognizing it.
 *
 * Vertical text (vita-vn-ocr #99, docs/vita-vn-ocr.md "Vertical text"): a job on
 * a model of columns (VJO_VOCR_FINDER_COLUMNS, the vertical recognizer) finds
 * the region's columns instead of its rows and reads each one turned 90 degrees,
 * right to left. Every other model's job is the same as before, bit for bit. */
#ifndef VJO_VOCR_OCR_H
#define VJO_VOCR_OCR_H

#include <stddef.h>
#include <stdint.h>

#include "local_ocr.h" /* VJO_OCR_MAX_WIDTH/HEIGHT, VJO_OCR_TEXT_CAP */
#include "platform.h"  /* VjoPlatform.file_open, now_us */
#include "../include/vjo_vocr_sizes.h" /* VJO_VOCR_GATE_NAME, VJO_VOCR_GATE_BYTES */

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_VOCR_DEFAULT_MODEL "VOCR5_N_w8g2.vocr" /* the Normal tier's, VJO_VOCR_TIER2_NAME (include/vjo_vocr_sizes.h) */

/* The capture block: one strip of at most this many bytes of grey rows (the
 * strip is region width x VJO_VOCR_STRIP_ROWS(width) rows), plus per region
 * row its checksum, its earlier states (16-bit, the latest first) and two
 * bits; as many earlier states as fit within VJO_VOCR_CAPTURE_LIMIT, 2 to
 * VJO_VOCR_STATES_MAX (vjo_vocr_state_slots: 16 for a dialogue box, 6 at
 * 960x544, 5 at 896x544, the widest strip). */
#define VJO_VOCR_STRIP_BYTES (56u * 1024u)
#define VJO_VOCR_STRIP_MAX_ROWS 64u /* the kernel copies at most this many rows per call */
#define VJO_VOCR_TRIES 3u           /* captures of one job before VJO_E_SCREEN_CHANGED */
#define VJO_VOCR_CAPTURE_LIMIT 65536u /* include/vjo_ledger_budget.h VJO_BUDGET_GAME_CAPTURE */
#define VJO_VOCR_STATES_MAX 16u
/* With the stable-text wait on, a row that goes back to one of its recent
 * states (an animated icon) is not a change, in this many rows of a job at
 * most: a page of new text, or a box of several lines clearing back to what
 * the job saw before, changes more. A one- or two-line box that clears back to
 * an empty box the same job saw earlier, while the job reads it, can be taken
 * for an icon (the job's text then misses those lines; it is tagged with the
 * screen it read, which is no longer shown). Not learning such rows once the
 * job reads would close that, at a retry whenever a "next" icon that blinks
 * slower than the wait goes back while the job reads (docs/vita-vn-ocr.md).
 * Without the wait every change is one, as in API 19. */
#define VJO_VOCR_CYCLE_ROWS 64u
/* The wait reads at most this many strips a job, whatever the clock says (a
 * real job reads a few hundred at most: its waits end by settle_max_us). */
#define VJO_VOCR_SETTLE_READS 4096u
/* A line taller than a strip is read through the strip and kept whole, its own
 * columns only, in VjoGreySource.line_buffer: line width x line height bytes, up
 * to this many (include/vjo_ledger_budget.h VJO_BUDGET_GAME_LINE), so the
 * recognizer gets the same input as from a copy of the whole region. Dialogue
 * lines are 20-53 px on the shared eval screenshots, inside one strip; the
 * finder can report a far taller band (lines it could not separate, a picture),
 * up to the region: 41 of 372 linefind dev regions have one, the largest
 * 533 x 148 = 78,884 B. A band of more than this many bytes is left out and
 * counted (tall_skipped). */
#define VJO_VOCR_LINE_MAX_BYTES 516096u

/* A region read in strips of grey rows: strip(ud, y, n, dst, stride, flags)
 * fills region rows [y, y + n) as (77 R + 150 G + 29 B) >> 8, width bytes each,
 * stride apart, and returns n, or a negative VJO_E_* / native error (the job
 * then fails with it; VJO_E_CANCELLED for a cancelled one). flags:
 * VJO_STRIP_FIRST when the rows continue the job's first pass from row 0, in
 * order, and VJO_STRIP_LAST on the one that ends it. line_buffer(ud, bytes),
 * optional, gives memory for one line taller than a strip (valid until the next
 * call or the end of the job), or NULL: that line is then left out. */
#define VJO_STRIP_FIRST 0x1u
#define VJO_STRIP_LAST 0x2u
typedef struct {
    void *ud;
    unsigned width, height;
    int (*strip)(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags);
    int (*cancelled)(void *ud); /* optional; checked between strips and lines, and by helper
                                 * threads (VjoVocrPool) between tiles, which may still call it
                                 * after a failed join: it must not depend on the caller's stack */
    void *(*line_buffer)(void *ud, size_t bytes);
} VjoGreySource;

/* A weights file this build accepts: exact size and SHA-256 of the release
 * asset, and the recognizer's arena (W8I mode, default tiling). */
typedef struct {
    const char *name;
    uint32_t bytes;
    unsigned char sha256[32];
    uint32_t arena_bytes;
    unsigned finder_mode;     /* vocr_lines.h: VOCR_LINES_TEXTBOX_RECOVERY for every pinned model of rows;
                               * VJO_VOCR_FINDER_COLUMNS | VOCR_COLUMNS_* flags for the model of columns */
} VjoVocrModel;

/* A model of vertical text (VjoVocrModel.finder_mode; include/vjo_vocr_sizes.h VJO_VOCR_VERTICAL_NAME):
 * its jobs find the region's columns with vita-vn-ocr's column finder (vocr_vertical.h
 * vocr_find_columns_rows, the low byte its VOCR_COLUMNS_* flags), right to left, turn each column 90
 * degrees anticlockwise (vocr_column_rotate) into the job's rotation buffer, and read that strip as the
 * line: the blank-line check, the line cache, the helpers' copies, vocr_run and the LM take it; the
 * column's box on the screen stays the line's geometry (VjoVocrLineStat, VjoVocrLineInfo). The text is
 * the columns' right to left, '\n' between them, as rows are top to bottom. The steps for rows do not
 * run: the refinement, the short-line probe (VjoVocrPolicy.short_lines) and the cascade. */
#define VJO_VOCR_FINDER_COLUMNS 0x100u
/* The rotation buffer, in the workspace of a job of columns: a column whose crop holds more bytes
 * (width x height) is left out, never read, and counted (VjoVocrStats.rotate_skipped): one wider than
 * 105 px at the full 544-px height. */
#define VJO_VOCR_ROTATE_BYTES VJO_VOCR_STRIP_BYTES
/* API 47, config.ini ocr_vertical = auto (VjoVocrPolicy.vertical_check): whether the row finder's n lines
 * (vita-vn-ocr's VocrLine, vocr_lines.h) on a w x h region look like vertical text read as rows, so that the
 * job reads its columns instead. Geometry only (core/vocr_ocr.c: the rule; docs/vita-vn-ocr.md "Vertical
 * text": how it was measured). */
int vjo_vocr_rows_vertical(const void *lines, unsigned n, unsigned w, unsigned h);

/* The model named `name` (a file name in ocr_model_dir), or NULL. */
const VjoVocrModel *vjo_vocr_model(const char *name);
/* The supported models, for messages and tools; *count is set. */
const VjoVocrModel *vjo_vocr_models(unsigned *count);

/* A character language model this build accepts for the beam + LM decoder
 * (bee-san/vita-vn-ocr runtime/vocr_lm.h; pre-release v0.3.7-lm6.2): the asset's
 * exact size and SHA-256, and the decoder settings its tier was selected with
 * (vita-vn-ocr docs/vocr6-lm.md, the release's settings.json): the LM weight, the
 * per-character bonus and the line flags (VOCR_LM_BOS: start each line from the
 * start-of-line context; VOCR_LM_EOS: an end-of-line term). Every tier shares the
 * rest: beam 8, the top 8 classes a frame, gate and bracket cost 8 nats. One LM
 * serves every model. */
typedef struct {
    const char *name;
    uint32_t bytes;
    unsigned char sha256[32];
    int beta;          /* 1/1000 nat */
    unsigned tier;     /* 1 (0.5 MB) .. VJO_VOCR_LM_TIERS (4 MB) */
    int alpha;         /* the LM weight, 1/1000 */
    unsigned flags;    /* VOCR_LM_BOS | VOCR_LM_EOS */
} VjoVocrLm;
const VjoVocrLm *vjo_vocr_lm(const char *name);
const VjoVocrLm *vjo_vocr_lm_tier(unsigned tier); /* NULL for 0 or one out of range */

/* LM bytes kept between jobs, as VjoVocrResident keeps the recognizer's. */
typedef struct {
    void *(*alloc)(void *ud, size_t bytes);
    int (*release)(void *ud);
    void *ud;
    const VjoVocrLm *lm;        /* verified bytes of this LM are in data, else NULL */
    unsigned char *data;
    size_t capacity;
    uint64_t size, stamp;
} VjoVocrLmResident;
int vjo_vocr_lm_drop(VjoVocrLmResident *resident);

/* API 26: the text-presence gate's weights (VjoVocrPolicy.gate_res), kept between jobs. The file
 * VJO_VOCR_GATE_NAME in the job's model directory is opened in every job that uses the gate and read and
 * hashed (its SHA-256 pinned in core/vocr_ocr.c) only when its size or stamp changed or nothing verified is
 * held. The caller owns it (the worker: a static) and zero-initializes it. */
typedef struct {
    unsigned char data[VJO_VOCR_GATE_BYTES];
    int verified;            /* 1: data holds the pinned weights */
    uint64_t size, stamp;    /* of the file they were read from */
} VjoVocrGateResident;

/* More job workspace a job on `model` needs to decode with an LM: the top-K
 * buffers, the decoder's structs and its workspace (bounded by
 * VJO_VOCR_LM_WS_BYTES); 0 for an unsupported model. */
#define VJO_VOCR_LM_TOPK 8u
#define VJO_VOCR_LM_FRAMES 129u     /* a line's most CTC frames: the 1,024-px clamp / 8, + 1 */
#define VJO_VOCR_LM_WS_BYTES 49152u /* >= vocr_lm_workspace_bytes for every pinned model and LM (tested) */
size_t vjo_vocr_lm_job_bytes(const char *model);
/* The workspace the confidence cascade needs after the LM extras, to read lines
 * again with the larger model `cascade` (its arena, structs and LM decoder); 0
 * for a model this build does not pin. */
size_t vjo_vocr_cascade_job_bytes(const char *cascade);
/* The cascade's estimated time per line against the job's model's, x 256, before a
 * re-read measures it (the ratio of their weights); 0 for a model not pinned. */
uint32_t vjo_vocr_cascade_ratio(const char *cascade, const char *model);
/* The refinement's arena for a width x height region (VjoVocrPolicy.refine). */
size_t vjo_vocr_refine_bytes(unsigned width, unsigned height);

/* The line finder returns at most this many lines (vocr_lines.h VOCR_LINES_MAX). */
#define VJO_VOCR_MAX_LINES 16u

/* One recognized line: its crop, the CTC frames the recognizer ran, and the
 * time it took, so a console log gives ms per frame for each line. */
typedef struct {
    uint16_t width, height;  /* crop in region pixels */
    uint16_t frames;         /* CTC frames (the resized width / 8) */
    uint16_t flags;          /* VJO_VOCR_LINE_*: its confidence, what the LM did */
    uint32_t us;             /* vocr_run (and the LM decoder's) time with VjoPlatform.now_us, else 0 */
    /* API 22 (ocr-speed) */
    uint8_t slot;            /* who recognized it: 0 the caller's thread, 1.. a helper (VjoVocrPool) */
    uint8_t cached;          /* 1: its pixels were unchanged since a job read them (VjoVocrCache): no vocr_run */
    uint16_t pad;
    uint32_t cpu_us;         /* the recognizing thread's run time for it (VjoVocrPool.thread_us), else 0 */
} VjoVocrLineStat;
/* flags: the line's confidence, the smallest softmax-max probability over the
 * frames that write a character of its greedy text (vocr_extra.conf_min), in
 * 1/1000 (1000 for a line read as empty), when it was read with the top-K
 * (VJO_VOCR_LINE_CONF_NONE otherwise); whether the LM decoded it; whether the
 * cascade's larger model read it again (VJO_VOCR_LINE_CASCADE: that reading and
 * its confidence are the ones shown); and whether the text shown differs from
 * the recognizer's greedy reading (by the LM or the cascade). */
#define VJO_VOCR_LINE_CONF 0x03FFu
#define VJO_VOCR_LINE_CONF_NONE 0x03FFu
#define VJO_VOCR_LINE_CASCADE 0x0400u
#define VJO_VOCR_LINE_LM 0x4000u
#define VJO_VOCR_LINE_CHANGED 0x8000u
/* API 47: the line was a column, read turned with a model of columns (include/vjo_game_ocr.h
 * VJO_GAME_OCR_LINE_COLUMN, which the Shell reads). */
#define VJO_VOCR_LINE_COLUMN 0x0800u

/* A found line's ink (vjo_vocr_ink): its contrast (the 98th minus the 2nd
 * percentile grey) and its edges: the pixels from which the grey level changes
 * by at least `edge` = max(VJO_VOCR_EDGE_MIN, contrast / VJO_VOCR_EDGE_DIV)
 * within VJO_VOCR_INK_SPAN pixels to the right, and below (at most two a pixel),
 * per 10,000 pixels. Soft edges count: text whose strokes rise over two pixels
 * (a PSP game's 480x272 frame shown 2x with bilinear filtering, a game rendering
 * under 960x544, a blurred font) is strokes too. On the linefind development
 * lines no text line has fewer than 1,553, and in seven soft render paths of the
 * same screenshots (docs/vita-vn-ocr.md) none has fewer than
 * VJO_VOCR_BLANK_EDGES. A crop under that holds no text-like ink: an even area,
 * a smooth part of a picture, the box's gradient, strokes under 24 levels of
 * contrast. A lone small mark ("。") is too few pixels to move the
 * percentiles, but its edges count. */
typedef struct {
    uint8_t lo, hi;          /* 2nd and 98th percentile grey */
    uint8_t edge;            /* the step an edge needs */
    uint8_t reserved;
    uint16_t edges;          /* edges per 10,000 pixels */
    uint16_t reserved2;
} VjoVocrInk;
#define VJO_VOCR_EDGE_MIN 24u
#define VJO_VOCR_EDGE_DIV 4u
#define VJO_VOCR_INK_SPAN 2u
#define VJO_VOCR_BLANK_EDGES 50u
/* Crops of more pixels are never called blank (no line is this large: a strip
 * holds at most 64 rows of 960, a tall line VJO_VOCR_LINE_MAX_BYTES). */
#define VJO_VOCR_INK_PIXELS 4194304u

/* A line the blank-line check skipped, for the log. */
typedef struct {
    uint16_t index;          /* its place among the found lines */
    uint16_t width, height;
    uint16_t x, y;
    VjoVocrInk ink;
} VjoVocrBlankStat;
#define VJO_VOCR_BLANK_LOGGED 4u

typedef struct {
    size_t workspace;        /* bytes of this job's workspace (see vjo_vocr_ocr*) */
    size_t capture;          /* bytes of its capture block (vjo_vocr_capture_bytes) */
    size_t model_bytes;      /* weights, used in place */
    size_t scratch_bytes;    /* the finder's arena, then the recognizer's */
    unsigned width, height;  /* region */
    unsigned strip_rows;     /* rows per strip */
    unsigned strips;         /* strips read (in the game: one frame each) */
    unsigned tears;          /* rows that read differently on a later read */
    unsigned tries;          /* captures of the region (1 unless the screen changed) */
    unsigned tall;           /* lines taller than a strip read into line_buffer (or left out: tall_skipped) */
    unsigned tall_skipped;
    size_t line_bytes;       /* the largest line_buffer asked for (a tall line's width x height), 0 none */
    uint32_t region_sum;     /* FNV-1a of the region's grey rows, first pass of the last try */
    uint32_t capture_us;     /* time inside strip() */
    unsigned lines;          /* lines found */
    unsigned dark;           /* of those, dark text (inverted before recognition); a model of columns whose
                              * finder leaves the flag to the job (VOCR_COLUMNS_NO_DARK): of the columns read */
    unsigned truncated;      /* more lines than the finder returns (16) */
    unsigned frames;         /* CTC frames recognized, all lines */
    unsigned model_loaded;   /* 1: the weights were read and verified in this job */
    unsigned model_cached;   /* 1: resident verified weights were reused (file unchanged) */
    /* with VjoPlatform.now_us, else 0. recognize_us: the job's model on every line
     * (its runs and LM decodes); the cascade's re-reads are in cascade_us. */
    uint32_t find_us, load_us, recognize_us;
    unsigned recognized;     /* entries of line[] filled: lines vocr_run finished */
    VjoVocrLineStat line[VJO_VOCR_MAX_LINES];
    /* The stable-text wait (VjoVocrPolicy.settle_us). */
    uint32_t settle_us;      /* time spent on passes read only to see the rows hold still */
    unsigned settle_passes;  /* those passes */
    unsigned settle_changes; /* row changes to a new state they saw (text being typed) */
    unsigned cycles;         /* rows that went back to their previous state (an icon), all reads */
    unsigned cycling_rows;   /* distinct such rows */
    unsigned settled;        /* 1: the rows held still for settle_us before the lines were read */
    unsigned settle_timeout; /* 1: settle_max_us ran out first, so the job went on without it */
    /* The blank-line check (VjoVocrPolicy.blank). */
    unsigned blank;          /* lines skipped: no text-like ink in their crop */
    uint32_t blank_us;       /* time spent checking */
    VjoVocrBlankStat blank_line[VJO_VOCR_BLANK_LOGGED]; /* the first ones skipped */
    /* The beam + LM decoder (VjoVocrPolicy.lm). */
    const VjoVocrLm *lm;     /* the LM the lines were decoded with, or NULL (greedy) */
    int lm_rc;               /* 0, or why the job read greedily although an LM was asked for (VJO_E_*) */
    unsigned lm_loaded;      /* 1: the LM was read and verified in this job */
    unsigned lm_cached;      /* 1: resident verified LM bytes were reused */
    uint32_t lm_load_us;     /* reading, checking and planning it */
    uint32_t lm_us;          /* decoding, all lines */
    unsigned lm_lines;       /* lines decoded with it */
    unsigned lm_changed;     /* of those, lines whose text differs from the greedy text */
    /* The confidence cascade (VjoVocrPolicy.cascade). */
    const VjoVocrModel *cascade; /* the larger model lines were read again with, or NULL */
    int cascade_rc;          /* 0, or why it could not be used (VJO_E_*: missing, wrong, no memory) */
    unsigned cascade_loaded; /* 1: its weights were read and verified in this job */
    unsigned cascade_cached; /* 1: its resident verified weights were reused */
    uint32_t cascade_load_us; /* reading or checking them, and planning it */
    uint32_t cascade_us;     /* reading lines again (its runs and decodes), all lines */
    unsigned cascade_frames; /* its CTC frames, all lines read again */
    unsigned cascade_lines;  /* lines read again */
    unsigned cascade_changed; /* of those, lines whose text it changed */
    unsigned cascade_blanked; /* of those, lines it read as empty where the job's model read text */
    unsigned cascade_skipped; /* doubtful lines not read again: the time ceiling */
    /* The finder's boxes refined (VjoVocrPolicy.refine, vita-vn-ocr's vocr_lines_refine.h). */
    unsigned refine_boxes;   /* boxes refined */
    unsigned refine_lines;   /* the lines they became (a box can split into several) */
    unsigned refine_split;   /* boxes split into two or more */
    unsigned refine_capped;  /* boxes refined without their split, which would pass VJO_VOCR_MAX_LINES lines */
    int refine_rc;           /* 0, or the first refusal (VOCR_LINES_E_*, the box then kept as found) */
    unsigned long refine_rows; /* rows the refinement read (from memory: the strip or the line's block) */
    uint32_t refine_us;      /* refining, all boxes */
    /* Parallel lines and the line cache (VjoVocrPool, VjoVocrCache; API 22). */
    unsigned helpers;        /* helper threads the job could use (VjoVocrPool.helpers, 0 serial) */
    unsigned dispatched;     /* lines a helper recognized */
    unsigned run_failures;   /* helpers that could not start: their line ran on the caller's thread */
    unsigned cached;         /* lines whose text came from the cache */
    uint32_t key_us;         /* time spent hashing lines for the cache */
    unsigned stuck;          /* 1: a helper could not be joined: it may still run in the workspace */
    /* ocr-first (docs/first-ocr.md). */
    uint32_t retry_at;       /* VjoPlatform.now_us when the last capture attempt began (tries > 1) */
    unsigned warmed;         /* 1: vjo_vocr_warm read what the job had not (its loaded/load_us fields) */
    /* The text-presence gate (VjoVocrPolicy.gate, API 26), of the last try. */
    unsigned gate_read;      /* 1: its weights were read from the card and verified in this job */
    unsigned gate_ran;       /* 1: it decided on this try's region (its score is in gate_score) */
    unsigned gate_stopped;   /* the finder's lines it stopped (0: the region holds text, or it did not decide) */
    int gate_score;          /* its score (-129..127): text iff >= VJO_VOCR_GATE_T */
    int gate_rc;             /* 0, or why it could not decide (VJO_E_*, VOCR_LINES_E_*: the lines were kept) */
    unsigned gate_rows;      /* rows it took (each the first time the finder asked for it) */
    uint32_t gate_us;        /* its time: its share of the first pass, and its decision */
    /* The counter re-decode (VjoVocrPolicy.counter_tau, counter_open). */
    unsigned counter_lines;  /* lines whose numeral or opening bracket it put back (the job's model) */
    /* The head gate and the two-level head (VjoVocrPolicy.head_gate, two_level; API 42). */
    unsigned head_gated;     /* CTC frames whose output head the gate skipped, every reading that stands (the
                                cascade's too): of frames + cascade_frames */
    unsigned head_rereads;   /* lines read again with the full head (vocr_extra.ungated; the cascade's too) */
    unsigned head_escaped;   /* CTC frames that also ran the two-level head's rare rows, every reading that stands
                                (the cascade's too) */
    /* Vertical text (a model of columns, VJO_VOCR_FINDER_COLUMNS). */
    unsigned columns;        /* 1: the lines were columns, each read turned 90 degrees */
    unsigned rotate_skipped; /* columns left out: more than VJO_VOCR_ROTATE_BYTES */
} VjoVocrStats;

/* Each found line, for host tools (VjoVocrPolicy.line): valid during the call. A column (a model of
 * columns): x, y, w, h its box on the screen, pixels its turned crop, w rows of h bytes. */
typedef struct {
    unsigned index, x, y, w, h, dark;
    const unsigned char *pixels;  /* the crop's grey rows, stride apart */
    size_t stride;
    VjoVocrInk ink;
    int blank;                    /* skipped by the blank-line check */
    const char *text;             /* what the recognizer read ("" when skipped), after the LM */
    uint32_t us;                  /* its recognition time */
    const char *greedy;           /* its greedy text (the same as text without the LM) */
    unsigned flags;               /* VJO_VOCR_LINE_* */
} VjoVocrLineInfo;

/* Weights kept between jobs. The caller supplies their memory: alloc returns
 * 16-byte aligned memory for `bytes`, or NULL (the job then fails with
 * VJO_E_OOM); release frees it and returns 0, or nonzero if it could not (the
 * memory then stays held but is never used as weights). Zero-initialize the
 * state fields; vocr_ocr.c maintains them. Between jobs, data is non-NULL
 * exactly when model is: memory is held only for verified weights. */
typedef struct {
    void *(*alloc)(void *ud, size_t bytes);
    int (*release)(void *ud);
    void *ud;
    const VjoVocrModel *model;  /* verified weights of this model are in data, else NULL */
    unsigned char *data;        /* the memory held, or NULL */
    size_t capacity;            /* bytes of data */
    uint64_t size, stamp;       /* VjoFile.size and .stamp of the file they were read from */
} VjoVocrResident;

/* What a job does besides finding and recognizing the lines; a zeroed policy
 * (or NULL) is API 19's job exactly.
 *
 * settle_us: before any line is recognized, every row of the region has to
 * read the same (an icon toggling between two states aside) on passes that
 * span at least this long, so a line still typing out is never read half
 * done. A job whose screen was already still when it was asked reads its
 * first pass and finds its lines as before, then re-reads the region until the
 * span is long enough; with settle_first, or once the screen changed under a
 * job, it waits first, re-reading the region without finding lines, and takes
 * its first pass (the capture the Shell is told of) only once the rows hold
 * still. settle_max_us bounds the time spent waiting over the whole job (0:
 * VJO_VOCR_SETTLE_MAX_US): a region that never holds still (a video behind the
 * text) is then read as before. blank: a found line whose crop has no
 * text-like ink is skipped (vjo_vocr_blank; blank_edges, if set, in place of
 * VJO_VOCR_BLANK_EDGES, for tools and tests). sleep, optional: once the region
 * has been re-read without a change, the job sleeps until a re-read can be the
 * last one instead of reading passes that are too early to count.
 *
 * lm: decode every line with this LM (vocr_lm.h's beam search over the top-K)
 * instead of greedily; its bytes are read from ocr_model_dir and checked once,
 * into lm_res, and kept between jobs while the file is unchanged, as the
 * weights are. The workspace must hold vjo_vocr_lm_job_bytes() more. When the LM
 * cannot be used (missing, wrong, no memory) the job reads greedily and says
 * why in stats->lm_rc. raw, optional (raw_cap bytes): the greedy text of every
 * line, laid out as the text is, for the log.
 *
 * cascade: a line whose confidence (the top-K's, VJO_VOCR_LINE_CONF) is under
 * cascade_conf (0: VJO_VOCR_CASCADE_CONF) is read again by this larger model,
 * decoded with the same LM, and that reading is the one shown. Its weights are
 * read at the first such line, checked, and kept in cascade_res between jobs
 * like the job's own. A line is read again only while the job's recognition
 * (every line's run and decode, the re-reads, and an estimate for the lines
 * still to read), not counting the weights' loads, stays within cascade_us: the
 * time ceiling, per capture attempt (a job read again after the screen changed
 * starts it again). The workspace must hold vjo_vocr_lm_job_bytes() and
 * vjo_vocr_cascade_job_bytes() more. When the model cannot be used the job goes
 * on without it and says why in stats->cascade_rc.
 *
 * refine: VOCR_REFINE_* steps (vita-vn-ocr's vocr_lines_refine.h; 0: off, the
 * finder's boxes as found, exactly). Each box is refined on the pixels the job
 * already holds for it (its strip, or a tall line's block: no read more): cut to
 * its text band, frame rows and cursors ignored, a box of several text rows split
 * into one line each, the same lines as vocr_refine_lines() on the whole region:
 * a split that would take the job past VJO_VOCR_MAX_LINES lines is not made (the
 * box keeps its main band). The workspace must hold vjo_vocr_refine_bytes() more,
 * before the LM extras.
 *
 * pool, cache, cache_variant, busy: parallel lines, the line cache and the busy
 * callback (API 22), below. NULL / 0 is API 20's serial job exactly. */

/* Parallel lines (API 22): the lines of a region are independent, so a job may
 * recognize up to 1 + helpers of them at once (vocr_run, with the top-K when the
 * job has the LM extras), each on its own vocr_ctx and arena over the one shared,
 * read-only model. The caller's thread keeps all the rest in line order: each
 * line's fetch from the strip and every row check, the refinement, the blank-line
 * check, the cache, and, as each line's turn comes, its LM decode, the cascade's
 * re-read, the stats and the text. A line run while an earlier one is still out
 * is first copied out of the strip (the next fetch rewrites it), and a line runs
 * on the caller's thread when no helper is free; the last one always does. The
 * text, the frames and every line's result are the serial job's: vocr_run and
 * the decoder are deterministic, nothing but the read-only weights and LM is
 * shared, and everything after vocr_run is done in line order on the caller's
 * thread, with the serial job's capacity rule (VJO_E_TOO_LARGE exactly when the
 * serial job gives it). Only the cascade's time ceiling depends on time: a job
 * that recognizes its lines sooner may read more of them again.
 * run(ud, slot, fn, arg) runs fn(arg) on another thread (slot 1..helpers, at
 * most one at a time per slot) and returns 0, or < 0 when it cannot (that line
 * then runs on the caller's thread). join(ud, slot) returns 0 once that fn has
 * returned, or < 0 when it cannot tell: the job then fails with
 * VJO_E_OCR_UNAVAILABLE (stats->stuck), and the workspace must stay allocated
 * and no other job may run in the process (the helper may still be running in
 * it; this module refuses every later job). thread_us, optional: the calling
 * thread's run time so far in microseconds (each line's cpu_us). A per-line
 * callback (VjoVocrPolicy.line) keeps the job serial. The helpers' memory ends
 * the workspace, after every other part: vjo_vocr_helpers_bytes(). */
typedef struct {
    unsigned helpers;        /* 0..VJO_VOCR_HELPERS_MAX */
    int (*run)(void *ud, unsigned slot, void (*fn)(void *arg), void *arg);
    int (*join)(void *ud, unsigned slot);
    uint64_t (*thread_us)(void *ud);
    void *ud;
} VjoVocrPool;
#define VJO_VOCR_HELPERS_MAX 2u

/* The line cache (API 22): a line whose grey pixels, size, polarity and model
 * are those of a line recognized before, under the same VjoVocrPolicy.cache_variant,
 * gets that line's shown and greedy text and its VjoVocrLineStat.flags without
 * being recognized again. The key is a 64-bit FNV-1a of the crop's rows, its
 * size, its polarity and the variant. cache_variant names everything else that
 * decides a line's text (the LM, the cascade and their settings): jobs that
 * differ in it share no entry. A line is kept only when its text is the one an
 * unhurried serial job gives it anywhere in the text: read with the LM the
 * policy asks for, read again by the cascade when its confidence asks for it
 * (not skipped for time), and placed without a shorter fallback near the
 * capacity; a hit that would not fit as it was read makes the job read the
 * region again without the cache. Caller-owned, zero-initialized, kept between
 * jobs; entries of another model never match, and any job that reads the
 * weights from the file again empties it. A job adds its lines once it has
 * succeeded. */
#define VJO_VOCR_CACHE_LINES 8u
#define VJO_VOCR_CACHE_TEXT 368u /* longer line texts are not kept */
typedef struct {
    uint64_t key;            /* 0: empty */
    const VjoVocrModel *model;
    uint32_t used;           /* the cache's tick when last used (least recently used goes first) */
    uint16_t frames, len;    /* the line's CTC frames; its shown text's bytes */
    uint16_t flags;          /* the line's VjoVocrLineStat.flags */
    uint16_t greedy_len;     /* its greedy text's bytes */
    uint16_t need;           /* the longest text the job placed for it (greedy, LM, cascade) */
    uint16_t pad;
    char text[VJO_VOCR_CACHE_TEXT];
    char greedy[VJO_VOCR_CACHE_TEXT];
} VjoVocrCacheLine;
typedef struct {
    VjoVocrCacheLine line[VJO_VOCR_CACHE_LINES];
    uint32_t tick, hits, misses;
} VjoVocrCache;

typedef struct {
    uint32_t settle_us, settle_max_us;
    int settle_first;
    int blank;
    unsigned blank_edges;
    void (*line)(void *ud, const VjoVocrLineInfo *info);
    void *ud;
    void (*sleep)(uint32_t us);
    const VjoVocrLm *lm;
    VjoVocrLmResident *lm_res;
    char *raw;
    size_t raw_cap;
    const VjoVocrModel *cascade;
    VjoVocrResident *cascade_res;
    uint32_t cascade_us;
    unsigned cascade_conf;
    unsigned refine;
    int short_lines;          /* #194: also read a box's short last line no finder mode keeps (core/vocr_ocr.c
                                 probe_short_lines); the overlay's jobs, not the finder-only tests */
    /* API 22 (ocr-speed) */
    const VjoVocrPool *pool;
    VjoVocrCache *cache;
    uint32_t cache_variant;   /* see VjoVocrCache */
    /* busy(ud, 1) just before the first line is recognized, busy(ud, 0) once the
     * last one is (helpers joined, re-reads done); never for a job whose lines all
     * come from the cache or are skipped. The game worker raises the ARM clock in
     * between, when the job asks for it (ocr_boost_clock). */
    void (*busy)(void *ud, int on);
    void *busy_ud;
    /* API 26: the text-presence gate (vita-vn-ocr #100, runtime/vocr_textgate.c): its weights are read
     * from ocr_model_dir (VJO_VOCR_GATE_NAME) into gate_res and kept between jobs, as the recognizer's
     * are. It reads each row of the region as the finder asks for it the first time (no read of its own)
     * and, once the finder is done, scores the region from the cells under the finder's boxes; under
     * VJO_VOCR_GATE_T the job keeps no line and recognizes nothing (the worker's warm-up may still read
     * the weights ahead, as after any job that finds no line). Its arena takes the scratch the finder
     * leaves free (the recognizer's arena is larger at every region size), so the workspace does not
     * grow. When it cannot decide (no weights, no room, an error) the job keeps the lines and says why
     * in stats->gate_rc. A job whose finder finds no line is not scored. gate 0: off, exactly as before. */
    int gate;
    VjoVocrGateResident *gate_res;
    /* The counter re-decode (vita-vn-ocr #109, runtime/vocr_post.h vocr_post_redecode), on the top-K of a
     * job with the LM extras (VjoVocrPolicy.lm or .cascade), before the LM: where a line reads a letter X
     * that is not a numeral before ッ/ヶ/ヵ and 月/所/年/国/条, X becomes the cheapest numeral the frames
     * allow within counter_tau, and a 「/『 the line's left edge cut (its 」/』 read, no opener) is put
     * back before it within counter_open, both 1/1000 nat (VJO_VOCR_COUNTER_TAU/OPEN: vita-vn-ocr's
     * development choice). 0 and 0: off, exactly as before. The greedy text (VjoVocrPolicy.raw) stays the
     * recognizer's own. */
    int counter_tau, counter_open;
    /* API 42: the head gate (vita-vn-ocr #117, docs/vita-vn-ocr.md "Head gate"): a model whose blob carries one (a
     * VOCG section after the version-1 blob) skips its output head on the CTC frames the gate calls blank, in every
     * reading of the job (its model's, its helpers', the cascade's). A frame it skips reads as the blank; the
     * others' logits, top-K and confidence are as without it. A model without one reads as before. 0: off, the
     * output head on every frame. */
    int head_gate;
    /* API 42: the two-level head (vita-vn-ocr #117 item 2, docs/vita-vn-ocr.md "Two-level head"): a model whose blob
     * carries one (a VOC2 trailer; the head's rows in frequency order) computes its level 1 (the blank and the most
     * frequent classes, the head's first rows, as many as the trailer says) on every frame the gate keeps and the
     * other rows only on the frames its escape picks, in every reading of the job. A frame it picks reads as
     * without it; one it does not, over level 1's classes. A model without one reads as before. 0: off, the full
     * head on every frame.
     * A line either skipped something of is read again with the full head (vocr_extra.ungated) when its counter
     * re-decode finds a counter context (whose blank frames' top-K a skipped frame does not offer) or, with the
     * two-level head, when its reading holds a full-size カ or ケ before 月 所 年 国 条 (the escape can miss the
     * small ヵ or ヶ), and that reading is the line's. */
    int two_level;
    /* API 47 (config.ini ocr_vertical = auto): a job of rows checks the finder's rows first
     * (vjo_vocr_rows_vertical); where they look like vertical text it reads nothing, opens no weights and
     * returns VJO_E_VERTICAL, and the caller reads the region's columns with the model of columns. 0: off,
     * exactly as before; a model of columns ignores it. */
    int vertical_check;
    /* API 44: the line finder's fixes (bee-san/vita-vn-ocr #129, runtime/vocr_lines_fix.h): VOCR_FIX_* flags for
     * vocr_find_lines_fix_rows, the finder with COLGAP (a line's parts across a full-width space stay one line; the
     * default), JUNK (frame strips on the region's top or bottom edge and repeating ornaments are not lines; opt-in,
     * as it can drop a text line in a region drawn tight) and VALLEY (two touching rows split; opt-in). The same
     * arena, rows and dark flags as vocr_find_lines_rows. 0: the frozen finder, exactly as before. */
    unsigned line_fix;
} VjoVocrPolicy;
#define VJO_VOCR_COUNTER_TAU 6000
#define VJO_VOCR_COUNTER_OPEN 4000
#define VJO_VOCR_SETTLE_MAX_US 2000000u
/* VjoVocrPolicy.refine: every step of vita-vn-ocr's refinement
 * (VOCR_REFINE_DEFAULT, vocr_lines_refine.h: tighten, frames, split, cursor,
 * portrait), the set its development measurements used. */
#define VJO_VOCR_REFINE_ALL 0x1Fu
/* The confidence under which the cascade reads a line again (vita-vn-ocr #82:
 * under 0.7 flags about 1 line in 5 and 3 of 4 misread lines). */
#define VJO_VOCR_CASCADE_CONF 700u

/* The text-presence gate's operating point (vita-vn-ocr #100: the winner of eval/textgate_dev_20261018.json,
 * selection record 69fc939: `p8_c8_seed1` in boxes mode, k 4, t -8). Its weights are the pinned file
 * VJO_VOCR_GATE_NAME (include/vjo_vocr_sizes.h), kept in a VjoVocrGateResident. */
#define VJO_VOCR_GATE_MODE 1 /* VOCR_TEXTGATE_BOXES */
#define VJO_VOCR_GATE_K 4u
#define VJO_VOCR_GATE_T (-8)
/* The gate's arena for a width x height region with the weights in res, 0 when they are not loaded or
 * the region is not one the job takes. */
size_t vjo_vocr_gate_bytes(const VjoVocrGateResident *res, unsigned width, unsigned height);

/* The ink of a w x h crop of grey rows, stride apart (either polarity).
 * Returns 1 when it holds no text-like ink (vjo_vocr_blank), else 0. */
int vjo_vocr_ink(const unsigned char *pixels, size_t stride, unsigned w, unsigned h, VjoVocrInk *out);
int vjo_vocr_blank(const VjoVocrInk *ink);


/* Releases the resident weights. 0, or the release callback's failure (the
 * memory is then still held, and model is NULL). */
int vjo_vocr_resident_drop(VjoVocrResident *resident);

/* Exact workspace bytes for a job on a width x height region with `model`:
 * vjo_vocr_job_bytes() without the weights (resident form), or
 * vjo_vocr_workspace_bytes() with them (one-shot form); 0 for an unsupported
 * model or region. Neither includes the capture block. A model of columns
 * (VJO_VOCR_FINDER_COLUMNS): the larger of the two finders' arenas, and its
 * rotation buffer (VJO_VOCR_ROTATE_BYTES) after the runtime's structs. */
size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height);
size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height);
/* Parallel lines (VjoVocrPool): what one helper takes with `model` (its
 * recognizer arena and context, and a line's slot: its pixels, at most
 * VJO_VOCR_STRIP_BYTES for a line inside a strip, its greedy text,
 * VJO_OCR_TEXT_CAP, and its top-K), and what `helpers` of them take at the end of
 * a job's workspace, with the caller's own line slot (0 for none, or an
 * unsupported model). vjo_vocr_job_bytes_helpers() is vjo_vocr_job_bytes() plus
 * vjo_vocr_helpers_bytes(): a job without the refinement, LM or cascade extras. */
size_t vjo_vocr_helper_bytes(const char *model);
size_t vjo_vocr_helpers_bytes(const char *model, unsigned helpers);
size_t vjo_vocr_job_bytes_helpers(const char *model, unsigned width, unsigned height, unsigned helpers);
/* After a job returned stats->stuck every later job fails with
 * VJO_E_OCR_UNAVAILABLE (the helper may still run in that job's state). A caller
 * that has since seen every helper end (a host test that joined its threads)
 * lifts that; the game worker never does (it stops taking jobs). */
void vjo_vocr_unstick(void);
/* Rows per strip for a region of this size, and the capture block's exact
 * bytes (0 for an unsupported region), never more than VJO_VOCR_CAPTURE_LIMIT;
 * the recent states it keeps per row. */
unsigned vjo_vocr_strip_rows(unsigned width, unsigned height);
size_t vjo_vocr_capture_bytes(unsigned width, unsigned height);
unsigned vjo_vocr_state_slots(unsigned width, unsigned height);

/* Recognizes the region: lines top to bottom, separated by '\n', in text (at
 * most text_cap bytes with the NUL). workspace: 16-byte aligned, private to
 * the call, at least vjo_vocr_job_bytes(). capture: at least
 * vjo_vocr_capture_bytes(), any alignment.
 *
 * With at least one line, the model file is opened. Weights already resident
 * for this model whose file still reports the same size and a nonzero, equal
 * stamp are used as they are (stats->model_cached); otherwise the file is read
 * into the resident memory (allocated if needed, the old model's released
 * first) and checked against the pinned size and SHA-256 (stats->model_loaded).
 * A missing, wrong or unreadable file, or any failure while loading, releases
 * the resident weights; cancellation during the check keeps verified ones.
 *
 * Returns VJO_OK (also with no line: text "" and no file opened),
 * VJO_E_OCR_MODEL (file missing, wrong size or hash, or refused by the
 * runtime), VJO_E_OOM (workspace or capture too small, or no memory for the
 * weights), VJO_E_OCR_REGION, the source's error (VJO_E_SOURCE when it gave no
 * code), VJO_E_SCREEN_CHANGED, VJO_E_CANCELLED, VJO_E_TOO_LARGE (text),
 * VJO_E_OCR_INFERENCE, or VJO_E_OCR_UNAVAILABLE (resident memory could not be
 * released, or a helper could not be joined: stats->stuck). text is "" on error. */
int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoGreySource *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                          char *text, size_t text_cap, VjoVocrStats *stats);
/* The same with a policy (NULL: as vjo_vocr_ocr_resident). */
int vjo_vocr_ocr_policy(const VjoPlatform *platform, const char *model_dir, const char *model,
                        const VjoGreySource *image, VjoVocrResident *resident,
                        void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                        const VjoVocrPolicy *policy, char *text, size_t text_cap, VjoVocrStats *stats);

/* ocr-first (docs/first-ocr.md): after a job that read no line (a title screen,
 * a box between two lines, every line blank), what its recognition would have
 * read and verified, in this order: model's weights into resident, the
 * policy's LM into lm_res and the cascade's model into cascade_res, each only
 * if not held already (no file is opened for one that is), so the session's
 * first job with text reads none of them while the user waits. Nothing is
 * parsed or recognized; the memory is the same blocks a recognition takes,
 * which the job's memory check allowed. Stops at the first that fails, which
 * it drops as a recognition's own read would, and returns its rc (VJO_OK,
 * VJO_E_CANCELLED, VJO_E_OCR_UNAVAILABLE for memory that could not be
 * released, VJO_E_OCR_MODEL, VJO_E_OOM), also in stats->lm_rc or cascade_rc;
 * sets stats->warmed and the loaded and load_us fields of what it read. */
int vjo_vocr_warm(const VjoPlatform *platform, const char *model_dir, const char *model,
                  const VjoGreySource *image, VjoVocrResident *resident, const VjoVocrPolicy *policy,
                  VjoVocrStats *stats);

/* The one-shot form: the weights are read into the front of workspace (at
 * least vjo_vocr_workspace_bytes()) for this job only, as the host tools do. */
int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoGreySource *image, void *workspace, size_t workspace_size,
                 void *capture, size_t capture_size, char *text, size_t text_cap, VjoVocrStats *stats);

/* An RGBA image in memory (R, G, B, A bytes) as a VjoGreySource, for the host
 * tools and tests. */
typedef struct {
    const unsigned char *pixels;
    unsigned width, height;
    size_t stride;
} VjoRgbaImage;
void vjo_grey_source_rgba(const VjoRgbaImage *img, VjoGreySource *out);

#ifdef __cplusplus
}
#endif
#endif
