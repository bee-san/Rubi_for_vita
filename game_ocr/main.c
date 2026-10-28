/* Local OCR runs in the enabled game's process. No Paf, newlib startup, hooks,
 * network, or borrowed game allocations. Model chunks cross the kernel
 * transport; weights and inference use private memblocks: the verified weights
 * stay resident between jobs, the job workspace does not.
 *
 * API 19: the capture is the game's own too. The worker asks the kernel for one
 * strip of the job's region at a time (vjoOcrCaptureRows); the display hook,
 * running in the game's thread on the frame it submits, converts those rows to
 * 8-bit grey and copies them straight into the worker's capture block. There is
 * no copy of the whole frame anywhere, in the kernel or here (#74: the kernel
 * could not give the 2,088,960 B full-frame block it used to take). Every
 * allocation tries USER_RW first, then USER_CDRAM_RW, then
 * USER_MAIN_PHYCONT_NC_RW, and a strip the hook cannot write into this block is
 * read through a small kernel strip instead: each rung's native result is in
 * the job's step log, so one console run says which works. */
#include "main.h"
#include "hooks.h"
#include "psp_probe.h"
#include "../core/fbgrey.h"
#include "../core/jpegsw.h"
#include "../core/shottrim.h"
#include "../core/textnorm.h"
#include "../core/vocr_ocr.h"
#include "vocr_lines_fix.h" /* API 50: VOCR_FIX_DEFAULT, _JUNK, _VALLEY (vita-vn-ocr #129) */
#include "../core/hook_select.h"
#include "../core/hook_cache.h"
#include "../include/vjo_vocr_sizes.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/ledger.h"
#define WORKER_STACK_BYTES VJO_STACK_GAME
/* The lowest user priority (SDK: default 0x10000100 + 31) on every game core
 * (SCE_KERNEL_CPU_MASK_USER_ALL, which the SDK says the default mask equals;
 * named here so the intent is explicit). */
#define WORKER_PRIORITY (0x10000100 + 31)
#define WORKER_AFFINITY (0x00010000 | 0x00020000 | 0x00040000) /* SCE_KERNEL_CPU_MASK_USER_ALL (psp2/kernel/cpu.h) */
#include "../core/boot_guard.h"
#include "../core/games.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"
#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <string.h> /* memmove: shell/libc_shim.c's in the worker */
#ifndef VJO_HOST
#include <taihen.h>
#endif

#define GAME_RESERVE VJO_GAME_RESERVE
#define JOIN_US 2000000u
#define MODEL_WAIT_US 2000000u
/* Only when a model syscall still answers BUSY after its own bounded wait
 * for the kernel worker (a slow card): yield, then repeat it. */
#define MODEL_POLL_US 1000u
#define PAGE_ROUND(x) (((uint32_t)(x) + 4095u) & ~4095u)
#define ROUND_TO(x, unit) (((uint32_t)(x) + ((unit) - 1u)) & ~((unit) - 1u))
#define CDRAM_UNIT (256u * 1024u)     /* SDK: USER_CDRAM_RW sizes are multiples of 256 KB */
#define PHYCONT_UNIT (1024u * 1024u)  /* SDK: USER_MAIN_PHYCONT_NC_RW sizes are multiples of 1 MB */
#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#ifndef VJO_TEST_HOOK_CACHE
#define vjoOcrHookCache(seq, op, data) (-1)
#define vjo_hooks_build_id(out) 0
#endif
#ifndef VJO_TEST_HOOK_LINE
#define vjoOcrHookLine(seq, bytes) ((void)(seq), (void)(bytes), -1)
#endif
#ifndef VJO_TEST_SC3
#define vjo_hooks_sc3_want() ((const VjoSc3Pin *)NULL)
#define vjo_hooks_sc3_set(file, bytes) ((void)(file), (void)(bytes), 0)
#endif
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static SceUID worker = -1, workspace_uid = -1, capture_uid = -1, line_uid = -1;
/* A MAGES SC3 hook's charset (hooks_sc3_load): its block (sc3_blk, below), held from the first job until the worker
 * stops, and the reads tried this session (SC3_TRIES at most: only a cancelled or timed-out one is tried again). */
#define SC3_TRIES 3u
static unsigned sc3_tries;
_Static_assert(VJO_SC3_MAX_BYTES == VJO_GAME_OCR_SC3_BYTES && VJO_SC3_MAX_BYTES <= VJO_BUDGET_GAME_SC3,
               "an SC3 charset: the kernel's limit, the worker's, and its block's budget");
/* API 23: a shot's block (its colour strip, the encoder and the JPEG), only while a shot runs. */
static SceUID shot_uid = -1;
static uint32_t shot_bytes_held;
/* API 55 (ocr_pause, docs/game-pause.md): a paused job's frame block, the region's grey rows from one frame,
 * only while that job runs (freed as soon as the kernel refuses the copy). */
static SceUID frame_uid = -1;
static uint32_t frame_bytes_held;
static unsigned char *frame_base;
/* A rows call returned BUSY: the display hook may still write into the capture
 * (or tall-line) block, so neither is freed (they go with the process). */
static int hook_owns_capture;
/* A helper thread could not be joined (core/vocr_ocr.h VjoVocrStats.stuck): it may
 * still run in the job's workspace, so that block is never freed either. */
static int hook_owns_workspace;
static uint32_t workspace_bytes_held, capture_bytes_held, line_bytes_held;
static uint32_t capture_memory, workspace_memory; /* VJO_GAME_OCR_MEM_* of the blocks held */
static void *line_base;

/* The game worker's memory ledger (core/ledger.h): what we hold in the game's
 * partition. The game has no file access, so it is read through the result
 * fields (workspace_bytes, resident_bytes, capture_bytes, the step log) rather
 * than printed here. */
enum { GL_MODULE = 0, GL_STACK, GL_WEIGHTS, GL_WORKSPACE, GL_CAPTURE, GL_LINE, GL_LM, GL_CASCADE, GL_HELPERS, GL_SHOT,
       GL_SC3, GL_SPOT, GL_COUNT };
/* API 55: the frame block is the tall line's ledger item: a job holds one of the two, never both (the worst case
 * counts VJO_BUDGET_GAME_LINE_OR_FRAME), so the ledger keeps its twelve items in every module (#355's review H1) */
#define GL_FRAME GL_LINE
/* #418: the text-presence gate's block shares the spotter's item, the finder's weights (one more item would grow every
 * module's VjoLedger, VJO_LEDGER_MAX_ITEMS, and with it the Shell's code past its last page), as a probe's strip
 * shares the capture's and the frame block the tall line's; its step-log item (VJO_GITEM_GATE), its name and its
 * budget (VJO_BUDGET_GAME_GATE) are its own. */
#define GL_GATE GL_SPOT
static const VjoLedgerItemDef ledger_defs[GL_COUNT] = {
    [GL_MODULE] = {"OCR worker pages", VJO_LEDGER_POOL_GAME},
    [GL_STACK] = {"VjoGameVocr stack+TLS", VJO_LEDGER_POOL_GAME},
    [GL_WEIGHTS] = {"VjoGameVocrWeights", VJO_LEDGER_POOL_GAME},
    [GL_WORKSPACE] = {"VjoGameVocr workspace", VJO_LEDGER_POOL_GAME},
    [GL_CAPTURE] = {"VjoGameVocrCapture", VJO_LEDGER_POOL_GAME},
    [GL_LINE] = {"VjoGameVocrLine", VJO_LEDGER_POOL_GAME}, /* and API 55's VjoGameVocrFrame (GL_FRAME) */
    [GL_LM] = {"VjoGameVocrLM", VJO_LEDGER_POOL_GAME},
    [GL_CASCADE] = {"VjoGameVocrCascade", VJO_LEDGER_POOL_GAME},
    [GL_HELPERS] = {"VjoGameVocrHelper stacks+TLS", VJO_LEDGER_POOL_GAME},
    [GL_SHOT] = {"VjoGameShot", VJO_LEDGER_POOL_GAME},
    [GL_SC3] = {"VjoGameSc3", VJO_LEDGER_POOL_GAME},
    [GL_SPOT] = {"VjoGameVocrSpot", VJO_LEDGER_POOL_GAME},
};
typedef char vjo_game_ledger_items_fit[GL_COUNT <= VJO_LEDGER_MAX_ITEMS ? 1 : -1];
VjoLedger g_game_ledger;

static void ledger_init(void)
{
    vjo_ledger_init(&g_game_ledger, ledger_defs, GL_COUNT);
    vjo_ledger_add(&g_game_ledger, GL_MODULE, VJO_BUDGET_GAME_MODULE_PAGES);
}

/* A block kept between jobs (#418: one allocator for them all): the weights', the LM's, the cascade model's, (API
 * 54) the text spotter's and (API 26) the text-presence gate's, each the ud of its core/vocr_ocr.h VjoVocrResident
 * (resident_alloc, resident_release), and an SC3 charset's (hooks_sc3_load). Its uid and bytes held are in the data
 * segment (*_blk, as the four blocks' and the charset's were), its constants beside the code (*_desc: its ledger
 * item and step-log name, its sceKernelAllocMemBlock name, the most bytes it may be asked for). */
typedef struct {
    SceUID uid;
    uint32_t held;
} BlockState;
typedef struct {
    BlockState *b;
    uint8_t item, gitem;
    const char *name;
    uint32_t max;
} ResidentBlock;
static BlockState weights_blk = {-1, 0}, lm_blk = {-1, 0}, cascade_blk = {-1, 0}, spot_blk = {-1, 0}, sc3_blk = {-1, 0};
static BlockState gate_blk = {-1, 0};
static const ResidentBlock weights_desc = {&weights_blk, GL_WEIGHTS, VJO_GITEM_WEIGHTS, "VjoGameVocrWeights",
                                           VJO_VOCR_MAX_BYTES};
static const ResidentBlock lm_desc = {&lm_blk, GL_LM, VJO_GITEM_LM, "VjoGameVocrLM", VJO_VOCR_LM_MAX_BYTES};
static const ResidentBlock cascade_desc = {&cascade_blk, GL_CASCADE, VJO_GITEM_CASCADE, "VjoGameVocrCascade",
                                           VJO_VOCR_MAX_BYTES};
static const ResidentBlock spot_desc = {&spot_blk, GL_SPOT, VJO_GITEM_SPOT, "VjoGameVocrSpot", VJO_BUDGET_GAME_SPOT};
static const ResidentBlock sc3_desc = {&sc3_blk, GL_SC3, VJO_GITEM_SC3, "VjoGameSc3", VJO_BUDGET_GAME_SC3};
static const ResidentBlock gate_desc = {&gate_blk, GL_GATE, VJO_GITEM_GATE, "VjoGameVocrGate", VJO_VOCR_GATE_BYTES};
typedef char vjo_gate_budget_check[PAGE_ROUND(VJO_VOCR_GATE_BYTES) <= VJO_BUDGET_GAME_GATE ? 1 : -1];

/* ---- the job's step log (VjoGameOcrResult.step) ---- */
static VjoGameOcrResult *log_to;   /* the result being filled, or NULL */
static uint64_t job_started_us;

static uint64_t now_wide(void)
{
    return sceKernelGetProcessTimeWide();
}

/* The game's free memory as sceKernelGetFreeMemorySize reports it (each figure
 * is the free size minus a system margin [SDK: Kernel-Reference]); main holds
 * the error, the others 0, when the query failed. */
static int free_sample(int32_t *main_bytes, int32_t *cdram, int32_t *phycont)
{
    SceKernelFreeMemorySizeInfo f = {.size = sizeof(f)};
    int rc = IMPORT_READY(sceKernelGetFreeMemorySize) ? sceKernelGetFreeMemorySize(&f) : -1;
    if (rc < 0) {
        *main_bytes = rc;
        *cdram = *phycont = 0;
        return rc;
    }
    *main_bytes = f.size_user;
    *cdram = f.size_cdram;
    *phycont = f.size_phycont;
    return 0;
}

/* The confidence under which the cascade reads a line again: the core's
 * default (VJO_VOCR_CASCADE_CONF); host tests force it. */
#ifndef VJO_GAME_CASCADE_CONF
#define VJO_GAME_CASCADE_CONF 0u
#endif

/* Blocks the worker holds. Each one this job can still free keeps a slot of
 * the step log for its FREE step: the job's own blocks, and a resident block
 * (weights, LM, the cascade's model) only while it is not verified, since a
 * verified one stays for the next job (blocks_kept). */
static unsigned blocks_held;
static unsigned blocks_kept(void);
/* API 55: the frame block's ALLOC and FREE steps are left out (its PAUSE step has its memory, bytes and refusal), so
 * a paused job's log keeps the room the kernel's PACE step takes after it (VJO_GAME_OCR_STEPS). */
static int steps_quiet;

static VjoGameOcrStep *step_add(unsigned id, unsigned kind, unsigned arg, int32_t rc, uint32_t bytes,
                                uint32_t sum, uint32_t us, int sample)
{
    VjoGameOcrStep *s;
    unsigned kept, reserve;
    if (!log_to || log_to->n_steps >= VJO_GAME_OCR_STEPS) return NULL;
    /* The log is full before a block's release is: another step is dropped. */
    kept = blocks_kept();
    reserve = blocks_held > kept ? blocks_held - kept : 0u;
    if (id != VJO_GSTEP_FREE && log_to->n_steps + reserve >= VJO_GAME_OCR_STEPS) return NULL;
    s = &log_to->step[log_to->n_steps++];
    sceClibMemset(s, 0, sizeof(*s));
    s->id = (uint8_t)id;
    s->kind = (uint8_t)kind;
    s->arg = (uint16_t)(arg > 0xFFFFu ? 0xFFFFu : arg);
    s->rc = rc;
    s->bytes = bytes;
    s->sum = sum;
    s->us = us ? us : (uint32_t)(now_wide() - job_started_us);
    if (sample) free_sample(&s->free_main, &s->free_cdram, &s->free_phycont);
    return s;
}

/* API 42: the share of the job's CTC frames whose output head the head gate skipped, percent (its own model's
 * and the cascade's readings), for the recognize step's kind. */
static unsigned head_gated_percent(const VjoVocrStats *st)
{
    const uint32_t all = st->frames + st->cascade_frames;
    return all ? (unsigned)((uint64_t)st->head_gated * 100u / all) : 0u;
}

/* The confidence cascade's step (include/vjo_game_ocr.h VJO_GSTEP_CASCADE). */
static void cascade_step(const VjoVocrStats *st)
{
    step_add(VJO_GSTEP_CASCADE, st->cascade_blanked, st->cascade_lines, st->cascade_rc,
             (st->cascade_loaded ? VJO_GLM_READ : st->cascade_cached ? VJO_GLM_RESIDENT : 0u) |
                 (st->cascade_changed > 0x3Fu ? 0x3Fu : st->cascade_changed) << 24 |
                 (st->cascade_skipped > 0xFFu ? 0xFFu : st->cascade_skipped) << 16 |
                 (st->cascade_frames > 0xFFFFu ? 0xFFFFu : st->cascade_frames),
             st->cascade_load_us, st->cascade_us + 1u, 0);
}

/* ---- memory: the one USER allocation site of the worker ---- */
static const struct {
    int type;
    uint32_t unit, mem;
} ladder[] = {
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096u, VJO_GAME_OCR_MEM_USER},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, CDRAM_UNIT, VJO_GAME_OCR_MEM_CDRAM},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, PHYCONT_UNIT, VJO_GAME_OCR_MEM_PHYCONT},
};
#define LADDER_ALL 7u  /* USER, CDRAM, PHYCONT */
#define LADDER_USER 1u

/* One block for ledger item `item`, from the first memory type in `which`
 * (bits of ladder[]) that gives it, with its base, or < 0 (the last native
 * error). Each attempt is a step: its type, native result and the free
 * memory after it. A block whose base cannot be read is freed again; if even
 * that fails its uid is returned in *leaked so the caller keeps it recorded
 * for cleanup. *held: the bytes actually taken (rounded to the type's unit). */
static SceUID game_memblock_alloc(int item, unsigned gitem, const char *name, uint32_t bytes, unsigned which,
                                  void **base, SceUID *leaked, uint32_t *held, uint32_t *mem)
{
    SceUID uid = -1;
    *base = NULL;
    *leaked = -1;
    *held = 0;
    *mem = VJO_GAME_OCR_MEM_NONE;
    for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        uint32_t rounded = ROUND_TO(bytes, ladder[i].unit);
        if (!(which & (1u << i))) continue;
        uid = sceKernelAllocMemBlock(name, ladder[i].type, rounded, NULL);
        if (uid >= 0 && !steps_quiet) blocks_held++; /* from here its FREE step has a slot */
        if (!steps_quiet) step_add(VJO_GSTEP_ALLOC, ladder[i].mem, gitem, uid, rounded, 0, 0, 1);
        if (uid < 0) continue;
        if (sceKernelGetMemBlockBase(uid, base) < 0 || !*base || ((uintptr_t)*base & 15u)) {
            *base = NULL;
            if (sceKernelFreeMemBlock(uid) < 0) *leaked = uid;
            else if (!steps_quiet) blocks_held--;
            return -1;
        }
        vjo_ledger_add(&g_game_ledger, item, rounded);
        *held = rounded;
        *mem = ladder[i].mem;
        return uid;
    }
    return uid < 0 ? uid : -1;
}

static int game_memblock_free(int item, unsigned gitem, SceUID uid, uint32_t bytes)
{
    int rc = sceKernelFreeMemBlock(uid);
    if (!steps_quiet) step_add(VJO_GSTEP_FREE, 0, gitem, rc, bytes, 0, 0, 1);
    if (rc < 0) return -1;
    if (blocks_held && !steps_quiet) blocks_held--;
    vjo_ledger_sub(&g_game_ledger, item, bytes);
    return 0;
}

/* API 47: an auto try that hands the job to its next try (vjo_game_ocr_run_request's again:) has given back every
 * block it took (cleanup): its ALLOC and FREE steps, from `from` on, leave the log, so that the try that reads the
 * text keeps room for its own steps (the log holds VJO_GAME_OCR_STEPS: two tries' blocks filled it before the last
 * try's recognize, LM and wait steps); the try's memory check, first pass and FIND step stay. A FREE step of a block
 * the try did not take (an earlier job's resident weights, LM or cascade model its check counted as free) stays too:
 * that block is gone from the game's memory for good (#382's audit N1). checks (#385's audit F7): its CHECK steps
 * leave too, for a try of rows whose rows went to the columns (it read nothing, and a later try of rows checks the
 * sizes again): on's and auto's two or three tries had filled the log before the last try's NORM and PAR steps. */
static void steps_drop_blocks(unsigned from, int checks)
{
    unsigned w = from, n, taken = 0; /* a bit per VJO_GITEM_*: an ALLOC step of it from `from` on */
    if (!log_to || from >= log_to->n_steps) return;
    n = log_to->n_steps < VJO_GAME_OCR_STEPS ? log_to->n_steps : VJO_GAME_OCR_STEPS;
    for (unsigned i = from; i < n; i++) {
        const VjoGameOcrStep *s = &log_to->step[i];
        if (s->id == VJO_GSTEP_ALLOC) taken |= 1u << (s->arg & 31u);
        else if ((s->id != VJO_GSTEP_FREE || !(taken >> (s->arg & 31u) & 1u)) && (!checks || s->id != VJO_GSTEP_CHECK))
            log_to->step[w++] = *s;
    }
    if (w < n) sceClibMemset(&log_to->step[w], 0, (n - w) * sizeof(log_to->step[0]));
    log_to->n_steps = w;
}

static uint32_t model_seq;
static int model_owned;
static uint32_t quitting, active_seq;
static int worker_started, claim_outstanding, blocked, io_error, stat_error;
/* A PSP game: the refinement's "psp" mode applies (set by module_start: the
 * PSP emulator's main module is in this process, or the title is a PSP one,
 * vjo_game_is_psp). */
static int psp_title;

/* VJO_GAME_OCR_OPEN_*: which of the request's two pinned files this job opens;
 * and whether the cascade's model is the job's to open by its name. */
static uint32_t open_which;
static int open_cascade;
/* API 21: the job is a request by tiers (its models open by their tier; not a
 * job of columns', API 47, whose model opens as VJO_GAME_OCR_OPEN_VERTICAL), and the
 * reserve its memory check kept (include/vjo_ledger_budget.h VJO_GAME_RESERVE_*),
 * which its resident blocks' allocations keep too. */
static int tiered;
static uint32_t job_reserve = GAME_RESERVE;
/* API 47, auto (config.ini ocr_vertical): the vertical model could not be read in this game (absent, short or
 * failing its hash), so its jobs read rows without checking them for vertical text. */
static int columns_bad;
/* The model tiers' pinned models (include/vjo_vocr_sizes.h), smallest first. */
static const char *const tier_names[VJO_VOCR_TIERS] = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME,
                                                       VJO_VOCR_TIER3_NAME};
/* A model the job may run: its pins, its tier (0: a request by name), how the
 * kernel opens it, and the job's bytes with it (vjo_vocr_job_bytes + refinement). */
typedef struct {
    const VjoVocrModel *m;
    unsigned tier;
    uint32_t which;
    size_t bytes;
} Candidate;
#define CANDIDATES (VJO_VOCR_TIERS > 2u ? VJO_VOCR_TIERS : 2u)
static VjoGameOcrResult pending_result; /* never put large results on thread stacks */
static void *resident_alloc(void *ud, size_t bytes);
static int resident_release(void *ud);
/* Verified weights kept between jobs while this game is the active one. */
static VjoVocrResident resident = {resident_alloc, resident_release, (void *)&weights_desc, {NULL}, NULL, 0, 0, 0};
/* The same for the beam + LM decoder's LM (API 20). */
static VjoVocrLmResident lm_res = {resident_alloc, resident_release, (void *)&lm_desc, {NULL}, NULL, 0, 0, 0};
/* The confidence cascade's larger model, kept like the weights (API 20). */
static VjoVocrResident cascade_res = {resident_alloc, resident_release, (void *)&cascade_desc, {NULL}, NULL, 0, 0,
                                      0};
/* API 26: the text-presence gate's weights (VJO_VOCR_GATE_BYTES), read from the card once and kept while its
 * file is unchanged (core/vocr_ocr.h): #418, a block of their own (GL_GATE, gate_alloc), held like the LM while
 * the game is the active one and its jobs ask for the gate (in .bss before, a page of every game's whether the gate
 * was on or off). */
static void *gate_alloc(void *ud, size_t bytes);
static VjoVocrGateResident gate_res = {gate_alloc, resident_release, (void *)&gate_desc, {NULL}, NULL, 0, 0, 0};
/* API 54: the text spotter's weights and their parsed form (core/vocr_ocr.h VjoVocrSpotResident), 52-450 KB: a
 * block of their own (GL_SPOT), never a static array, kept like the LM while the game is the active one and its
 * jobs ask for the spotter. */
static VjoVocrSpotResident spot_res = {resident_alloc, resident_release, (void *)&spot_desc, {NULL}, NULL, 0, 0, 0};

/* API 54: the text spotter's step (include/vjo_game_ocr.h VJO_GSTEP_SPOT): rc the refusal before the job (spot_rc:
 * its weights, its arena, its room) or the job's own (stats->spot_rc); its weights read in this job, else kept. */
static void spot_step(const VjoVocrStats *st, int spot_rc, unsigned read, uint32_t load_us)
{
    uint32_t ms = load_us / 1000u;
    step_add(VJO_GSTEP_SPOT, st->spot_ran, st->spot_ran ? st->lines : 0u, spot_rc ? spot_rc : st->spot_rc,
             (uint32_t)st->spot_bytes | (read ? VJO_GLM_READ : spot_res.verified ? VJO_GLM_RESIDENT : 0u),
             (st->spot_boxes & 0x7FFFu) | (st->spot_truncated ? 0x8000u : 0u) | (ms > 0xFFFFu ? 0xFFFFu : ms) << 16,
             st->spot_us + 1u, 0);
}
int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len);

static int cancelled(void *ud)
{
    (void)ud;
    return __atomic_load_n(&quitting, __ATOMIC_ACQUIRE) ||
           !active_seq || vjoOcrCancelled(active_seq) != 0;
}

/* ---- the capture: strips of the region from the display hook ---- */
typedef struct {
    const VjoGameOcrRequest *r;
    uint32_t mode;          /* VJO_GAME_OCR_SRC_HOOK, then _KSTRIP once the hook could not write here */
    VjoGameOcrRowsInfo last;
    uint32_t pass_bytes, pass_strips, pass_us;
    int failed;             /* the rows call that ended the job, for the log */
    int fixed;              /* the probe: test this mode only, no ladder */
    int logged;             /* the first pass's PASS and SIG steps are in (a retry's are not logged) */
    int rgba;               /* API 23, a shot: the rows in colour (VJO_ROWS_RGBA), 4 bytes a pixel */
} Capture;

/* A BUSY from a request that never reached the hook (info.armed 0) is a
 * previous copy the kernel worker has not tidied yet (it does within a 16 ms
 * tick): ask again, a few times. */
#define ROWS_BUSY_TRIES 25u
#define ROWS_BUSY_DELAY_US 20000u

/* Errors after which the kernel strip may still work: the hook could not
 * write into this block, or the kernel refused its range. */
static int kernel_strip_may_help(int rc)
{
    return rc == VJO_ERR_COPY || rc == VJO_ERR_ARG;
}

static int capture_rows(Capture *c, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    VjoGameOcrRows req;
    unsigned busy = 0;
    int rc;
    for (;;) {
        sceClibMemset(&req, 0, sizeof(req));
        req.size = sizeof(req);
        req.seq = c->r->seq;
        req.y = y;
        req.n = n;
        req.mode = c->mode;
        req.flags = (flags & VJO_STRIP_FIRST ? VJO_ROWS_SIG : 0u) | (flags & VJO_STRIP_LAST ? VJO_ROWS_LAST : 0u) |
                    (c->rgba ? VJO_ROWS_RGBA : 0u);
        req.dst_stride = (uint32_t)stride;
        sceClibMemset(&c->last, 0, sizeof(c->last));
        c->last.size = sizeof(c->last);
        rc = vjoOcrCaptureRows(&req, dst, &c->last);
        if (rc == (int)n) {
            c->pass_bytes += n * c->r->width * (c->rgba ? 4u : 1u);
            c->pass_strips++;
            c->pass_us += c->last.wait_us;
            return rc;
        }
        if (rc == VJO_ERR_BUSY && !c->last.armed && busy < ROWS_BUSY_TRIES && !cancelled(NULL)) {
            if (!busy++) step_add(VJO_GSTEP_PASS, c->mode, y, rc, 0, 0, 0, 0);
            sceKernelDelayThread(ROWS_BUSY_DELAY_US);
            continue;
        }
        /* The ladder's last rung, once per job: the same rows through a kernel strip. */
        if (!c->fixed && c->mode == VJO_GAME_OCR_SRC_HOOK && kernel_strip_may_help(rc)) {
            step_add(VJO_GSTEP_PASS, VJO_GAME_OCR_SRC_HOOK, y, rc, 0, 0, c->last.wait_us, 0);
            c->mode = VJO_GAME_OCR_SRC_KSTRIP;
            continue;
        }
        if (c->mode == VJO_GAME_OCR_SRC_KSTRIP && c->last.kstrip_rc < 0)
            step_add(VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_KERNEL, c->rgba ? VJO_GITEM_SHOT : VJO_GITEM_CAPTURE,
                     c->last.kstrip_rc, n * c->r->width * (c->rgba ? 4u : 1u), 0, 0, 0);
        c->failed = rc;
        /* An armed request's copy outlived the call: the hook may still write dst, so the
         * block is never freed and this worker takes no more jobs. */
        if (rc == VJO_ERR_BUSY && c->last.armed) blocked = hook_owns_capture = 1;
        return rc == VJO_ERR_CANCELLED ? VJO_E_CANCELLED : rc < 0 ? rc : VJO_E_SOURCE;
    }
}

/* ---- API 55: the pause for the frame (VJO_GAME_OCR_S_PAUSE, config.ini ocr_pause; docs/game-pause.md) ----
 * A job that asks for it and holds its frame block asks the kernel, at its first pass, for the whole region at once
 * (VJO_ROWS_FRAME): the display hook copies every row from the frame the game submits, inside that one display call,
 * which waits as long (the game's pause, at most VJO_GAME_OCR_PAUSE_MAX_US), into the frame block. Every later read
 * of the job comes from the block (strip_source), and the core sees the region frozen (VjoGreySource.frozen): no
 * stable-text check after the finder, no tear, a tall line read in place. Refused or cut short, the block goes back
 * to the game at once and the job reads strip by strip, as without the pause. Nothing here waits for the game: the
 * pause is the kernel's copy in the game's own thread, and it ends with that copy. */
static struct {
    int asked;         /* the job asked for the pause (VJO_GAME_OCR_S_PAUSE) and reached its memory check */
    int held;          /* the frame block is held: the copy may be asked for at the first pass */
    int tried;         /* the copy was asked for (once a job) */
    int frozen;        /* every read of the job is from the block */
    int32_t rc;        /* why not: the kernel's error (VJO_ERR_*), VJO_E_OOM */
    uint32_t rows, us; /* the rows the display call copied, and its time: how long the game waited */
    uint32_t bytes;    /* the frame block's */
    uint32_t mem;      /* its VJO_GAME_OCR_MEM_* */
} pz;
/* The job's frame copies over all its tries (API 47's auto may read twice): its captures are then its rows calls. */
static unsigned pause_frames;
/* A frame copy reached the cap (VJO_GAME_OCR_PAUSE_MAX_US, VJO_ERR_TIMEOUT): the game waited that long for nothing, and
 * would at every reading. No copy is asked again in this game (this process, the worker's life; the audit's F2). */
static int pause_capped;

/* The frame block back to the game (a refused copy, the job's end); kept while the hook may still write it
 * (a rows call that outlived its copy: hook_owns_capture, as the capture block is). 0, or -1 when its free failed. */
static int frame_release(void)
{
    int rc;
    pz.held = pz.frozen = 0;
    if (frame_uid < 0 || hook_owns_capture) return 0;
    steps_quiet = 1;
    rc = game_memblock_free(GL_FRAME, VJO_GITEM_FRAME, frame_uid, frame_bytes_held);
    steps_quiet = 0;
    if (rc < 0) return -1;
    frame_uid = -1;
    frame_bytes_held = 0;
    frame_base = NULL;
    return 0;
}

/* The copy at the first pass: 0 (frozen, or refused: the job reads on as before), or the error that ends the job (a
 * cancellation; a copy that outlived its call, after which no job runs in this game). */
static int pause_copy(Capture *c)
{
    const VjoGameOcrRequest *r = c->r;
    VjoGameOcrRows req;
    unsigned busy = 0;
    int rc;
    pz.tried = 1;
    for (;;) {
        sceClibMemset(&req, 0, sizeof(req));
        req.size = sizeof(req);
        req.seq = r->seq;
        req.n = r->height;
        req.mode = VJO_GAME_OCR_SRC_HOOK;
        req.flags = VJO_ROWS_SIG | VJO_ROWS_LAST | VJO_ROWS_FRAME;
        req.dst_stride = r->width;
        sceClibMemset(&c->last, 0, sizeof(c->last));
        c->last.size = sizeof(c->last);
        rc = vjoOcrCaptureRows(&req, frame_base, &c->last);
        /* a previous copy not tidied yet (capture_rows' rule) */
        if (rc == VJO_ERR_BUSY && !c->last.armed && busy++ < ROWS_BUSY_TRIES && !cancelled(NULL)) {
            sceKernelDelayThread(ROWS_BUSY_DELAY_US);
            continue;
        }
        break;
    }
    pz.us = c->last.copy_us;
    /* the cap: no more copies in this game (a frame that never came is TIMEOUT too, with no wait). Not in a job
     * cancelled meanwhile (#355's review M2): a game suspended during its copy (the PS button) may hold the copy, and
     * the copy's time with it, for as long as it is suspended (inferred), and the suspension cancels the job */
    if (rc == VJO_ERR_TIMEOUT && pz.us >= VJO_GAME_OCR_PAUSE_MAX_US && !cancelled(NULL)) pause_capped = 1;
    if (rc == (int)r->height) {
        pz.frozen = 1;
        pause_frames++;
        pz.rows = r->height;
        c->pass_bytes += r->height * r->width;
        c->pass_strips++;
        c->pass_us += c->last.wait_us;
        return 0;
    }
    pz.rc = rc < 0 ? rc : VJO_E_SOURCE;
    if (rc == VJO_ERR_BUSY && c->last.armed) {
        /* the hook may still write the block: it is never freed, and this worker takes no more jobs */
        blocked = hook_owns_capture = 1;
        pz.held = 0;
        c->failed = rc;
        return rc;
    }
    if (rc == VJO_ERR_CANCELLED) {
        frame_release();
        return VJO_E_CANCELLED;
    }
    return frame_release() ? VJO_E_OCR_UNAVAILABLE : 0; /* the job reads on, strip by strip */
}

/* The job's pause step (VJO_GSTEP_PAUSE), once, when it asked for the pause: kind 1 frozen; sum the reason | the
 * block's memory << 8; us the game's waits over the job's tries (1: none). Before the blocks go back on a path that
 * ends the job early (#500's verified-site answer: #355's audit N7), while pz still says what the copy did. */
static void pause_step_log(uint32_t paused_us)
{
    if (!pz.asked) return;
    step_add(VJO_GSTEP_PAUSE, pz.frozen ? 1u : 0u, pz.rows, pz.frozen ? 0 : pz.rc, pz.bytes,
             (pz.frozen ? VJO_GPAUSE_FROZEN
              : pz.tried ? VJO_GPAUSE_REFUSED
              : pause_capped ? VJO_GPAUSE_CAPPED
              : pz.held ? VJO_GPAUSE_UNUSED
                        : VJO_GPAUSE_NO_ROOM) | pz.mem << 8,
             paused_us + pz.us ? paused_us + pz.us : 1u, 0);
    pz.asked = 0; /* logged */
}

/* VjoGreySource.frozen: the region from one frame, while every read of the job comes from it. */
static const unsigned char *frozen_region(void *ud)
{
    (void)ud;
    return pz.frozen ? frame_base : NULL;
}

static int strip_source(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    Capture *c = ud;
    int rc;
    if (cancelled(NULL)) return VJO_E_CANCELLED;
    /* API 55: the first pass's first strip is the pause's copy, once a job */
    if (pz.held && !pz.tried && (flags & VJO_STRIP_FIRST) && !y && (rc = pause_copy(c)) < 0) return rc;
    if (pz.frozen) {
        for (unsigned r = 0; r < n; r++)
            sceClibMemcpy(dst + (size_t)r * stride, frame_base + (size_t)(y + r) * c->r->width, c->r->width);
        rc = (int)n;
    } else {
        rc = capture_rows(c, y, n, dst, stride, flags);
    }
    /* The first pass is in: one step for it, and the kernel's publication (the
     * job's first; a retry's is the same source, its count is the TEAR step's). */
    if (rc > 0 && (flags & VJO_STRIP_LAST) && !c->logged) {
        c->logged = 1;
        step_add(VJO_GSTEP_PASS, c->mode, c->pass_strips, 0, c->pass_bytes, c->last.fb_format, c->pass_us, 0);
        step_add(VJO_GSTEP_SIG, c->mode, 0, 0, c->last.fb_pitch, c->last.fb_width << 16 | c->last.fb_height, 0, 0);
    }
    return rc;
}

/* API 47: a column's line flag, as the core sets it and the Shell reads it. */
typedef char vjo_line_column_check[VJO_VOCR_LINE_COLUMN == VJO_GAME_OCR_LINE_COLUMN ? 1 : -1];
/* The core never asks for more than the ledger counts. */
typedef char vjo_line_budget_check[VJO_VOCR_LINE_MAX_BYTES <= VJO_BUDGET_GAME_LINE ? 1 : -1];
/* API 55: a frame block holds the largest region, and the worst case counts the larger of it and a tall line's
 * block (a job holds one or the other: include/vjo_ledger_budget.h VJO_BUDGET_GAME_LINE_OR_FRAME). */
typedef char vjo_frame_budget_check[PAGE_ROUND((uint32_t)VJO_OCR_MAX_WIDTH * VJO_OCR_MAX_HEIGHT) <= VJO_BUDGET_GAME_FRAME &&
                                    VJO_BUDGET_GAME_FRAME <= VJO_BUDGET_GAME_LINE_OR_FRAME &&
                                    VJO_BUDGET_GAME_LINE <= VJO_BUDGET_GAME_LINE_OR_FRAME ? 1 : -1];
typedef char vjo_capture_budget_check[VJO_VOCR_CAPTURE_LIMIT <= VJO_BUDGET_GAME_CAPTURE ? 1 : -1];
typedef char vjo_lm_budget_check[PAGE_ROUND(VJO_VOCR_LM_MAX_BYTES) <= VJO_BUDGET_GAME_LM ? 1 : -1];
/* The weights and the cascade's model: at most the largest pinned model each (since API 21
 * the cascade's is the next tier up, so Normal's is Large's, the largest). */
typedef char vjo_weights_budget_check[PAGE_ROUND(VJO_VOCR_MAX_BYTES) <= VJO_BUDGET_GAME_WEIGHTS ? 1 : -1];
typedef char vjo_cascade_budget_check[PAGE_ROUND(VJO_VOCR_MAX_BYTES) <= VJO_BUDGET_GAME_CASCADE &&
                                      VJO_VOCR_CASCADE_BYTES <= VJO_VOCR_MAX_BYTES ? 1 : -1];

/* #385's audit F5 (AGENTS.md rule 3): the job's model and LM while its core runs (else NULL), whose blocks the core
 * takes after it has fetched lines when they are not resident (with ocr_skip_blank_lines on, at the first line not
 * blank) */
static const VjoVocrModel *job_model;
static const VjoVocrLm *job_lm;
static int64_t free_user_after_reserve(void);
/* A tall-line block of at most this many bytes (a column's bound, the rotation buffer's: every block a job could ask
 * for before #379) is taken from the ladder as before; a larger one keeps the job's reserve (line_buffer). The host
 * tests lower it: their frames' rectangles are smaller. */
#ifndef VJO_GAME_LINE_UNCHECKED
#define VJO_GAME_LINE_UNCHECKED VJO_VOCR_ROTATE_BYTES
#endif
typedef char vjo_line_unchecked_check[VJO_GAME_LINE_UNCHECKED <= VJO_VOCR_ROTATE_BYTES ? 1 : -1];

/* One block for a line taller than a strip (VjoGreySource.line_buffer). A block larger than one column's (the
 * rotation buffer's bound: #379's rectangle of a job's columns, or a rare band of rows) is taken only from what the
 * game has free beyond the job's reserve and the job's weights still to come, which its memory check counted and the
 * line block is not in, and for a job of columns its LM's too (#385's audit F5: the rectangle, up to 504 KiB, cut
 * into a job of columns' reserve, 4 MiB then, or with the blank-line check took its weights' room, so they were
 * refused and it read the columns' text as rows). Refused, the columns are read one by one, each in a block of its
 * own, as before #379; such a band of rows is left out and counted (tall_skipped), so a job of rows keeps no LM's or
 * cascade's room for it: those blocks keep the reserve themselves, and their refusal only costs the LM's or the
 * cascade's reading. */
static void *line_buffer(void *ud, size_t bytes)
{
    void *base = NULL;
    SceUID leaked;
    uint32_t held, mem;
    (void)ud;
    if (line_uid >= 0 && line_bytes_held >= bytes) return line_base;
    if (line_uid >= 0) {
        if (game_memblock_free(GL_LINE, VJO_GITEM_LINE, line_uid, line_bytes_held) < 0) return NULL;
        line_uid = -1;
        line_bytes_held = 0;
        line_base = NULL;
    }
    if (bytes > VJO_VOCR_LINE_MAX_BYTES) return NULL;
    if (bytes > VJO_GAME_LINE_UNCHECKED) {
        int64_t to_come = PAGE_ROUND(bytes);
        if (job_model && resident.model != job_model) to_come += PAGE_ROUND(job_model->bytes);
        if (job_lm && lm_res.lm != job_lm && (job_model->finder_mode & VJO_VOCR_FINDER_COLUMNS))
            to_come += PAGE_ROUND(job_lm->bytes);
        if (free_user_after_reserve() < to_come) return NULL;
    }
    line_uid = game_memblock_alloc(GL_LINE, VJO_GITEM_LINE, "VjoGameVocrLine", (uint32_t)bytes, LADDER_ALL,
                                   &base, &leaked, &held, &mem);
    if (line_uid < 0) {
        line_uid = leaked;
        return NULL;
    }
    line_bytes_held = held;
    line_base = base;
    return base;
}

/* ---- the model broker ---- */
/* The kernel broker's calls for the claimed job (#418: one poll loop for them all), each repeated while the kernel's
 * worker is still at it (VJO_ERR_BUSY), MODEL_POLL_US apart, at most wait_us: a file's read (a, b: its offset and
 * bytes, into p), close, open (a: VJO_GAME_OCR_OPEN_*, p: its VjoGameOcrModelInfo, zeroed before each try), a shot's
 * write (a, b, p: its offset, bytes and data) or, p NULL, save (b: its bytes), and the hook cache's load or save (a:
 * the operation, p: its data). The answer, VJO_ERR_TIMEOUT, or BROKER_CANCELLED once the job is cancelled before a
 * try (a close is tried whatever the job: cleanup drains it even after cancellation or active_seq's reset). */
#define BROKER_CANCELLED (-0x7FFFFFFF)
enum { POLL_READ, POLL_CLOSE, POLL_OPEN, POLL_SHOT, POLL_CACHE };
static int broker_poll(unsigned what, uint32_t a, uint32_t b, void *p, uint32_t wait_us)
{
    uint64_t started = now_wide();
    for (;;) {
        int rc;
        if (what != POLL_CLOSE && cancelled(NULL)) return BROKER_CANCELLED;
        if (what == POLL_READ) {
            rc = vjoOcrModelRead(model_seq, a, b, p);
        } else if (what == POLL_CLOSE) {
            rc = vjoOcrModelClose(model_seq);
        } else if (what == POLL_OPEN) {
            sceClibMemset(p, 0, sizeof(VjoGameOcrModelInfo));
            rc = vjoOcrModelOpen(model_seq, a, p);
        } else if (what == POLL_SHOT) {
            rc = p ? vjoOcrShotWrite(model_seq, a, b, p) : vjoOcrShotSave(model_seq, b);
        } else {
            rc = vjoOcrHookCache(model_seq, a, p);
        }
        if (rc != VJO_ERR_BUSY) return rc;
        if (now_wide() - started >= wait_us) return VJO_ERR_TIMEOUT;
        sceKernelDelayThread(MODEL_POLL_US);
    }
}

static int file_read(void *ud, uint64_t off, void *dst, size_t bytes)
{
    (void)ud;
    if (!model_owned || !dst || off > UINT32_MAX || bytes > UINT32_MAX-off || cancelled(NULL)) return -1;
    for (size_t used = 0; used < bytes;) {
        if (cancelled(NULL)) return -1;
        size_t n = bytes-used < VJO_GAME_OCR_MODEL_CHUNK ? bytes-used : VJO_GAME_OCR_MODEL_CHUNK;
        int got = broker_poll(POLL_READ, (uint32_t)(off+used), (uint32_t)n, (unsigned char *)dst+used, MODEL_WAIT_US);
        if (got == BROKER_CANCELLED) return -1;
        if (got <= 0 || (size_t)got > n) { io_error = got < 0 ? got : -1; return -1; }
        used += (size_t)got;
    }
    return 0;
}

/* API 23: the file the kernel holds is a shot's JPEG, whose save (a close and
 * a rename) may outlast a model operation's wait: its close waits SHOT_FILE_WAIT_US. */
static int shot_file;
#define SHOT_FILE_WAIT_US VJO_GAME_OCR_SHOT_FILE_US

static void file_close(void *ud)
{
    (void)ud;
    if (model_owned) {
        /* Cleanup must drain even after cancellation or active_seq reset. */
        int rc = broker_poll(POLL_CLOSE, 0, 0, NULL, shot_file ? SHOT_FILE_WAIT_US : MODEL_WAIT_US);
        if (rc >= 0) { model_owned = 0; model_seq = 0; shot_file = 0; }
        else io_error = rc; /* cleanup retries; never acknowledge a leaked descriptor */
    }
}

/* API 21: the tier a request by tiers opens a model file as. Two sizes may share
 * one model (a size that kept the model of the size below it), so the job's own
 * model is its own tier's (own: 1..VJO_VOCR_TIERS, 0 none) and any other the
 * first tier that has it: the cascade's, the next size up, whose model is always
 * larger than the job's (cascade_of), so no smaller size has it. 0: no tier's. */
static unsigned tier_of_file(const char *const *names, unsigned own, const char *name)
{
    if (own >= 1u && own <= VJO_VOCR_TIERS && !sceClibStrcmp(name, names[own - 1u])) return own;
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++)
        if (!sceClibStrcmp(name, names[t - 1u])) return t;
    return 0;
}

/* Which of the request's files a path names: an LM by its pinned name (API 20),
 * else the job's model or fallback (open_which). */
static uint32_t which_file(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') name = p + 1;
    if (!sceClibStrcmp(name, VJO_VOCR_GATE_NAME)) return VJO_GAME_OCR_OPEN_GATE; /* API 26: the gate's weights */
    if (!sceClibStrcmp(name, VJO_VOCR_SPOT_NAME)) return VJO_GAME_OCR_OPEN_SPOT; /* API 54: the spotter's */
    for (unsigned t = 1; t <= VJO_VOCR_LM_TIERS; t++)
        if (!sceClibStrcmp(name, vjo_vocr_lm_tier(t)->name)) return VJO_GAME_OCR_OPEN_LM + t - 1u;
    if (tiered) {
        /* API 21: a request by tiers opens each model by its tier (the job's and its
         * cascade's); the kernel checks the request allows it. A tier with no file of
         * that name: one past the last, which the kernel refuses. */
        unsigned own = open_which >= VJO_GAME_OCR_OPEN_TIER && open_which < VJO_GAME_OCR_OPEN_TIER + VJO_VOCR_TIERS
                       ? open_which - VJO_GAME_OCR_OPEN_TIER + 1u : 0u;
        unsigned t = tier_of_file(tier_names, own, name);
        return VJO_GAME_OCR_OPEN_TIER + (t ? t - 1u : VJO_VOCR_TIERS);
    }
    if (open_cascade && !sceClibStrcmp(name, VJO_VOCR_CASCADE_NAME)) return VJO_GAME_OCR_OPEN_CASCADE;
    return open_which;
}

/* The claimed job's file `which` (VJO_GAME_OCR_OPEN_*) through the kernel's broker, its BUSY answers waited out
 * (MODEL_WAIT_US): its size, a native error, or BROKER_CANCELLED. Ownership is taken before the request is queued:
 * a timed-out Open may still finish, and only file_close gives it back. */
static int broker_open(uint32_t which, VjoGameOcrModelInfo *info)
{
    model_seq = active_seq;
    model_owned = 1;
    return broker_poll(POLL_OPEN, which, 0, info, MODEL_WAIT_US);
}

static int file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    if (model_owned || !path || !out || cancelled(NULL)) return -1;
    /* The kernel resolves only this claimed job's pinned model path. */
    VjoGameOcrModelInfo info;
    uint32_t which = which_file(path);
    int size = broker_open(which, &info);
    if (size == BROKER_CANCELLED) return -1;
    if (size <= 0) {
        /* API 26: the gate's weights are optional: a job that cannot read them reads as without the gate (its
         * step says why), so their failed open is drained at once rather than holding the job's own files. API 54:
         * so are the spotter's (the job reads with GRAD). */
        if (which == VJO_GAME_OCR_OPEN_GATE || which == VJO_GAME_OCR_OPEN_SPOT) {
            file_close(NULL);
            if (!model_owned) return -1;
        }
        io_error = size;
        return -1;
    }
    /* The kernel's view of the file version: resident weights are reused only
     * while it is known and unchanged (an unknown time means: read again). */
    stat_error = info.stat_rc;
    uint64_t stamp = info.size == sizeof(info) && info.bytes == (uint32_t)size && !info.stat_rc ? info.mtime : 0;
    *out = (VjoFile){NULL, (uint64_t)size, file_read, file_close, stamp};
    return 0;
}

static uint64_t now_us(void *ud)
{
    (void)ud;
    return sceKernelGetProcessTimeWide();
}

/* The game's free USER bytes. A negative figure from the API is the free size
 * under its system margin, i.e. nothing free: 0 (the research's reading of
 * #74's shell_main=-2097152). -1 only when the query itself is unavailable. */
static int32_t free_user(void)
{
    int32_t main_bytes, cdram, phycont;
    if (free_sample(&main_bytes, &cdram, &phycont) < 0) return main_bytes < 0 ? main_bytes : -1;
    return main_bytes < 0 ? 0 : main_bytes;
}

/* USER bytes free after keeping the job's reserve (its tier's, chosen by the
 * memory check; GAME_RESERVE before it and for a request by name), or -1 if unknown. */
static int64_t free_user_after_reserve(void)
{
    int32_t free_bytes = free_user();
    return free_bytes < 0 ? -1 : (int64_t)free_bytes - (int64_t)job_reserve;
}

/* VjoVocrResident.alloc (ud its ResidentBlock): one page-rounded USER_RW block for the weights, the LM, the
 * cascade's model or (API 54) the spotter's weights and their parsed form (vjo_vocr_spot_block_bytes), under the
 * same reserve as the job workspace. */
static void *resident_alloc(void *ud, size_t bytes)
{
    const ResidentBlock *d = ud;
    BlockState *b = d->b;
    uint32_t rounded = PAGE_ROUND(bytes), mem;
    void *base = NULL;
    SceUID leaked;
    if (b->uid >= 0 || !bytes || bytes > d->max) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    b->uid = game_memblock_alloc(d->item, d->gitem, d->name, rounded, LADDER_USER, &base, &leaked, &b->held, &mem);
    if (b->uid < 0) {
        /* A block that cannot be released stays recorded; cleanup retries it. */
        b->uid = leaked;
        return NULL;
    }
    return base;
}

/* VjoVocrResident.release (ud its ResidentBlock), and an SC3 charset's: the block given back, if one is recorded
 * (0); < 0: its free failed, and it stays recorded. */
static int resident_release(void *ud)
{
    const ResidentBlock *d = ud;
    BlockState *b = d->b;
    if (b->uid < 0) return 0;
    if (game_memblock_free(d->item, d->gitem, b->uid, b->held) < 0) return -1;
    b->uid = -1;
    b->held = 0;
    return 0;
}

/* API 26: the gate's block, which a job takes before its weights, LM, cascade model and helpers' stacks
 * (core/vocr_ocr.c run_job: the gate decides before any line is read), so only in the room beyond what its memory
 * check counted for them (job_to_come, set for each job's core call) and the job's reserve: refused, the job reads
 * as without the gate (stats->gate_rc VJO_E_OOM), never with a smaller model. */
static uint32_t job_to_come;
static void *gate_alloc(void *ud, size_t bytes)
{
    return free_user_after_reserve() < (int64_t)PAGE_ROUND(bytes) + job_to_come ? NULL : resident_alloc(ud, bytes);
}

/* Releases r's verified bytes and its block, and a block whose base lookup failed; a no-op (0) when it holds none. */
static int drop_one(VjoVocrResident *r)
{
    if (vjo_vocr_resident_drop(r)) return -1;
    return resident_release(r->ud);
}

/* Everything kept between jobs: the weights, the LM, the cascade's model, (API 54) the spotter's weights and (API 26)
 * the gate's. */
static int drop_resident(void)
{
    int rc = drop_one(&resident);
    rc |= drop_one(&lm_res);
    rc |= drop_one(&spot_res);
    rc |= drop_one(&cascade_res);
    return drop_one(&gate_res) || rc ? -1 : 0;
}

/* r's block while it holds verified contents, else 0: the weights', the LM's and the cascade model's page-rounded
 * file; API 54, the spotter's block (the memory check counts it as one the job gives back: the spotter never takes
 * a tier's, an LM's or the cascade's room). */
static uint32_t held_bytes(const VjoVocrResident *r)
{
    const BlockState *b = ((const ResidentBlock *)r->ud)->b;
    return r->verified && b->uid >= 0 ? b->held : 0;
}

/* Resident blocks with verified contents: no job frees them but on a path that
 * drops them before its recognition, so their FREE steps need no slot. */
static unsigned blocks_kept(void)
{
    return !!held_bytes(&resident) + !!held_bytes(&lm_res) + !!held_bytes(&cascade_res) + (sc3_blk.uid >= 0) +
           !!held_bytes(&spot_res) + !!held_bytes(&gate_res);
}

/* The job workspace's bytes after the model's own, with the LM (lm) and the
 * cascade's larger model (casc): the top-K extras and the greedy text with
 * either, the cascade's arena and decoder with it. */
static size_t extra_job_bytes(const VjoVocrModel *m, const VjoVocrLm *lm, const VjoVocrModel *casc)
{
    if (!lm && !casc) return 0;
    return vjo_vocr_lm_job_bytes(m->name) + (casc ? vjo_vocr_cascade_job_bytes(casc->name) : 0) +
           VJO_GAME_OCR_TEXT_BYTES;
}

/* The reserve a job of model tier `tier` keeps (include/vjo_ledger_budget.h): since PR #280 the same fixed 1 MiB
 * for every tier, the cascade's included, and for a request by name (tier 0, GAME_RESERVE); from API 21 until then
 * the largest tier the job held, the cascade's (the next tier up) when it had one: 4, 8 or 16 MiB. The tier
 * constants stay for the shared model table; the job's blocks are counted separately beside the reserve. */
static uint32_t check_reserve(unsigned tier, const VjoVocrModel *casc)
{
    unsigned held = casc && tier ? tier + 1u : tier;
    return !held ? GAME_RESERVE : held >= VJO_VOCR_TIER_LARGE ? VJO_GAME_RESERVE_LARGE
         : held == VJO_VOCR_TIER_NORMAL ? VJO_GAME_RESERVE_NORMAL : VJO_GAME_RESERVE_SMALL;
}

/* The memory check for model m of tier `tier` (0: a request by name), with LM
 * lm and the cascade's model casc (each or NULL), and a job of job_bytes plus the
 * capture block: records what the job needs in out (its page-rounded workspace
 * and capture block; the weights, the LM and the cascade's model unless they are
 * resident; resident ones of another model or tier, or not used, it would
 * release first) and the reserve it keeps, and returns whether that fits beside
 * the reserve in out->free_user. */
static int check_memory(VjoGameOcrResult *out, const VjoVocrModel *m, size_t job_bytes, size_t capture_bytes,
                        const VjoVocrLm *lm, const VjoVocrModel *casc, unsigned tier)
{
    out->reserve_bytes = check_reserve(tier, casc);
    out->need_workspace = PAGE_ROUND(job_bytes + extra_job_bytes(m, lm, casc)) + PAGE_ROUND(capture_bytes);
    out->need_weights = (resident.model == m ? 0 : PAGE_ROUND(m->bytes)) +
                        (!lm || lm_res.lm == lm ? 0 : PAGE_ROUND(lm->bytes)) +
                        (!casc || cascade_res.model == casc ? 0 : PAGE_ROUND(casc->bytes));
    out->release_bytes = (resident.model && resident.model != m ? held_bytes(&resident) : 0) +
                         (lm_res.lm && lm_res.lm != lm ? held_bytes(&lm_res) : 0) +
                         (cascade_res.model && cascade_res.model != casc ? held_bytes(&cascade_res) : 0) +
                         held_bytes(&spot_res); /* API 54: given back unless the spotter's part fits after the check */
    return out->free_user >= 0 &&
           (int64_t)out->free_user + out->release_bytes - (int64_t)out->reserve_bytes >=
               (int64_t)out->need_workspace + out->need_weights;
}

/* The check with the largest LM tier up to max that fits beside model m and the
 * cascade's casc (*lm; NULL when none does, or max is 0: then the check
 * without one). */
static int check_lm_tiers(VjoGameOcrResult *out, const VjoVocrModel *m, size_t job_bytes, size_t capture_bytes,
                          unsigned max, const VjoVocrModel *casc, unsigned tier, const VjoVocrLm **lm)
{
    for (unsigned t = max; t >= 1; t--) {
        const VjoVocrLm *c = vjo_vocr_lm_tier(t);
        if (c && check_memory(out, m, job_bytes, capture_bytes, c, casc, tier)) {
            *lm = c;
            return 1;
        }
    }
    *lm = NULL;
    return check_memory(out, m, job_bytes, capture_bytes, NULL, casc, tier);
}

/* What fits beside model m, in the order of what each brings on the dialogue
 * lines measured (docs/vita-vn-ocr.md): the cascade with the largest LM tier
 * that fits, else the largest LM alone, else the cascade alone, else neither.
 * *casc is set to NULL when the cascade does not fit. */
static int check_with_lm(VjoGameOcrResult *out, const VjoVocrModel *m, size_t job_bytes, size_t capture_bytes,
                         unsigned max, unsigned tier, const VjoVocrModel **casc, const VjoVocrLm **lm)
{
    if (*casc && *casc != m && check_lm_tiers(out, m, job_bytes, capture_bytes, max, *casc, tier, lm) &&
        (*lm || !max))
        return 1;
    if (check_lm_tiers(out, m, job_bytes, capture_bytes, max, NULL, tier, lm) && (*lm || !max || !*casc)) {
        *casc = NULL;
        return 1;
    }
    if (*casc && *casc != m && check_memory(out, m, job_bytes, capture_bytes, NULL, *casc, tier)) {
        *lm = NULL;
        return 1;
    }
    *casc = NULL;
    *lm = NULL;
    return check_memory(out, m, job_bytes, capture_bytes, NULL, NULL, tier);
}

/* Whether m is one of the job's models (its resident weights may serve it). */
static int candidate_of(const Candidate *cand, unsigned n, const VjoVocrModel *m)
{
    for (unsigned i = 0; i < n; i++)
        if (cand[i].m == m) return 1;
    return 0;
}

/* The cascade's model for candidate c, when the request gives the cascade a time
 * ceiling: a request by name has VOCR4-W20 read again beside a smaller model; one
 * by tiers the next tier up (API 21), unless that tier is one the request leaves
 * out below its largest (a tier the Shell found unusable). Larger than c's own:
 * a size that kept the model of the size below it (two sizes, one file) gives
 * that size no cascade, since the same model reads a line the same again. */
static const VjoVocrModel *cascade_of(const Candidate *c, const VjoGameOcrRequest *r, unsigned top, uint32_t casc_us)
{
    const VjoVocrModel *m;
    if (!casc_us) return NULL;
    if (!c->tier) m = vjo_vocr_model(VJO_VOCR_CASCADE_NAME);
    else if (c->tier >= VJO_VOCR_TIERS || (!(r->tiers & (1u << c->tier)) && c->tier + 1u <= top)) m = NULL;
    else m = vjo_vocr_model(tier_names[c->tier]);
    return m && m->bytes > c->m->bytes ? m : NULL;
}

static int cleanup(void)
{
    int rc = 0;
    if (workspace_uid >= 0 && !hook_owns_workspace) {
        if (game_memblock_free(GL_WORKSPACE, VJO_GITEM_WORKSPACE, workspace_uid, workspace_bytes_held) < 0)
            rc = VJO_E_OCR_UNAVAILABLE;
        else { workspace_uid = -1; workspace_bytes_held = 0; workspace_memory = 0; }
    }
    /* A capture block the display hook may still write into (a rows call that
     * timed out on a copy in progress) is never freed; one whose free failed is
     * freed again here. */
    if (capture_uid >= 0 && !hook_owns_capture) {
        if (game_memblock_free(GL_CAPTURE, VJO_GITEM_CAPTURE, capture_uid, capture_bytes_held) < 0)
            rc = VJO_E_OCR_UNAVAILABLE;
        else { capture_uid = -1; capture_bytes_held = 0; capture_memory = 0; }
    }
    if (line_uid >= 0 && !hook_owns_capture) { /* rows calls write tall lines here too */
        if (game_memblock_free(GL_LINE, VJO_GITEM_LINE, line_uid, line_bytes_held) < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else { line_uid = -1; line_bytes_held = 0; line_base = NULL; }
    }
    if (shot_uid >= 0 && !hook_owns_capture) { /* a shot's colour rows were written here */
        if (game_memblock_free(GL_SHOT, VJO_GITEM_SHOT, shot_uid, shot_bytes_held) < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else { shot_uid = -1; shot_bytes_held = 0; }
    }
    if (frame_release() < 0) rc = VJO_E_OCR_UNAVAILABLE; /* API 55: a frame copy wrote here (kept as the capture is) */
    file_close(NULL);
    if (model_owned) rc = VJO_E_OCR_UNAVAILABLE;
    /* Between jobs a weights (or LM) block holds verified bytes or nothing. */
    if (!resident.model && drop_one(&resident)) rc = VJO_E_OCR_UNAVAILABLE; /* drop_one: a no-op when it holds none */
    if (!lm_res.lm && drop_one(&lm_res)) rc = VJO_E_OCR_UNAVAILABLE;
    if (!cascade_res.model && drop_one(&cascade_res)) rc = VJO_E_OCR_UNAVAILABLE;
    if (!spot_res.verified && drop_one(&spot_res)) rc = VJO_E_OCR_UNAVAILABLE;
    if (!gate_res.verified && drop_one(&gate_res)) rc = VJO_E_OCR_UNAVAILABLE;
    return rc;
}

/* ---- the self-test's probe (VJO_GAME_OCR_F_PROBE): every rung, one strip each ----
 * The debug build's only: the normal build has no self-test, and refuses a
 * probe request as it does any request it cannot run (docs/release-variants.md). */
#if VJO_DEBUG
static uint32_t grey_sum(const unsigned char *p, unsigned w, unsigned n, size_t stride)
{
    uint32_t h = VJO_FNV_INIT;
    for (unsigned r = 0; r < n; r++) {
        uint32_t row = vjo_fnv1a(VJO_FNV_INIT, p + (size_t)r * stride, w);
        h = vjo_fnv1a(h, (const unsigned char *)&row, 4);
    }
    return h;
}

/* sceDisplayGetFrameBuf's frame read by this thread, as VITA2PC does
 * (psvita/main.c:146-161): rows [0, n) of the region into dst, grey. The frame's
 * memory is checked to be a readable block of this process first, so a frame
 * the game freed is refused instead of read. Probe only. */
static int user_frame_rows(const VjoGameOcrRequest *r, unsigned n, unsigned char *dst, uint32_t *fmt_out)
{
    SceDisplayFrameBufExt fb; /* 3.60 writes 0x1C bytes even for a 0x18-byte size (vita-headers) */
    SceKernelMemBlockInfo mb;
    uint32_t bpp, span;
    uintptr_t first;
    int rc;
    if (!IMPORT_READY(sceDisplayGetFrameBuf) || !IMPORT_READY(sceKernelGetMemBlockInfoByAddr)) return -1;
    sceClibMemset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    rc = sceDisplayGetFrameBuf((SceDisplayFrameBuf *)&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    step_add(VJO_GSTEP_FB, VJO_GAME_OCR_SRC_USER, fb.width, rc, fb.pitch, fb.pixelformat, 0, 0);
    if (rc < 0) return rc;
    *fmt_out = fb.pixelformat;
    bpp = vjo_fb_bpp(fb.pixelformat);
    if (!fb.base || !bpp) return VJO_ERR_FORMAT;
    if (fb.width != r->fb_width || fb.height != r->fb_height || fb.pitch < fb.width) return VJO_ERR_FORMAT;
    first = (uintptr_t)fb.base + ((size_t)r->y * fb.pitch + r->x) * bpp;
    span = ((n - 1u) * fb.pitch + r->width) * bpp;
    sceClibMemset(&mb, 0, sizeof(mb));
    mb.size = sizeof(mb);
    rc = sceKernelGetMemBlockInfoByAddr(fb.base, &mb);
    if (rc < 0) return rc;
    if (!(mb.access & SCE_KERNEL_MEMORY_ACCESS_R) || first < (uintptr_t)mb.mappedBase ||
        first + span > (uintptr_t)mb.mappedBase + mb.mappedSize)
        return VJO_ERR_ARG;
    for (unsigned row = 0; row < n; row++) {
        const unsigned char *src = (const unsigned char *)first + (size_t)row * fb.pitch * bpp;
        uint32_t abgr[64];
        for (unsigned x = 0; x < r->width; x += 64) {
            unsigned k = r->width - x < 64 ? r->width - x : 64;
            vjo_fb_to_abgr(abgr, src + (size_t)x * bpp, k, fb.pixelformat);
            vjo_abgr_to_grey(dst + (size_t)row * r->width + x, abgr, k);
        }
    }
    return (int)n;
}

static int run_probe(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    unsigned rows = vjo_vocr_strip_rows(r->width, r->height);
    uint32_t bytes = rows * r->width;
    Capture c = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 1, 0, 0};
    int rc = VJO_OK, any = 0;
    if (!rows) return VJO_E_OCR_REGION;
    for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        void *base = NULL;
        SceUID uid, leaked;
        uint32_t held, mem;
        if (cancelled(NULL)) return VJO_E_CANCELLED;
        uid = game_memblock_alloc(GL_CAPTURE, VJO_GITEM_PROBE, "VjoGameVocrProbe", bytes, 1u << i, &base, &leaked,
                                  &held, &mem);
        if (uid < 0) {
            if (leaked >= 0) { capture_uid = leaked; capture_bytes_held = held; blocked = 1; return VJO_E_OCR_UNAVAILABLE; }
            continue;
        }
        capture_uid = uid;
        capture_bytes_held = held;
        for (uint32_t mode = VJO_GAME_OCR_SRC_HOOK; mode <= VJO_GAME_OCR_SRC_KSTRIP; mode++) {
            int got;
            sceClibMemset(base, 0, bytes);
            c.mode = mode;
            got = capture_rows(&c, 0, rows, base, r->width, i == 0 && mode == VJO_GAME_OCR_SRC_HOOK
                                                              ? VJO_STRIP_FIRST | (rows == r->height ? VJO_STRIP_LAST : 0u) : 0u);
            step_add(VJO_GSTEP_PROBE, mode, mem, got, got > 0 ? bytes : 0, got > 0 ? grey_sum(base, r->width, rows, r->width) : 0,
                     c.last.wait_us, 0);
            if (got > 0) {
                any = 1;
                out->fb_width = c.last.fb_width; out->fb_height = c.last.fb_height;
                out->fb_pitch = c.last.fb_pitch; out->fb_format = c.last.fb_format;
            }
            if (blocked) return VJO_E_OCR_UNAVAILABLE;
        }
        if (i == 0) {
            /* The user-mode read, into the same USER_RW block. */
            uint32_t fmt = 0;
            uint64_t t0 = now_wide();
            int got;
            sceClibMemset(base, 0, bytes);
            got = user_frame_rows(r, rows, base, &fmt);
            step_add(VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_USER, mem, got, got > 0 ? bytes : 0,
                     got > 0 ? grey_sum(base, r->width, rows, r->width) : 0, (uint32_t)(now_wide() - t0), 0);
        }
        if (game_memblock_free(GL_CAPTURE, VJO_GITEM_PROBE, capture_uid, capture_bytes_held) < 0) {
            blocked = 1;
            return VJO_E_OCR_UNAVAILABLE;
        }
        capture_uid = -1;
        capture_bytes_held = 0;
    }
    out->strip_rows = rows;
    return any ? rc : VJO_E_SOURCE;
}
#endif

/* ---- API 23: the Anki screenshot (VJO_GAME_OCR_F_SHOT, docs/anki.md) ----
 * The game's whole frame in colour, a strip of rows a frame through the display hook
 * (an OCR job's capture path: its waits, its checks and the kernel strip as the last
 * rung), encoded band by band (core/jpegsw.h) into one block of the game's memory, and
 * the JPEG written through the kernel's worker thread beside the card in the Anki queue
 * (vjoOcrShotWrite, vjoOcrShotSave; the game itself may have no ux0:). The block holds a
 * strip, the encoder and the JPEG, only while the shot runs; nothing of it reaches
 * SceShell, which learns the JPEG's size from the result.
 *
 * API 32 (VJO_GAME_OCR_S_TRIM): the frame's black bars (core/shottrim.h) are found as its
 * rows come, and the JPEG is of the picture between them, the same rows' sub-rectangle: no
 * copy of the frame. The first pass reads from row 0 without encoding until the picture's
 * first row, encodes the guess from there and scans every row below; a frame whose bars come
 * to another picture is read again (one more pass), as a JPEG too large is. A frame whose
 * first rows reach its top edge and a side's is read and encoded as without the option, the
 * same bytes (docs/anki.md, "Black bars"). */
#define SHOT_QUALITY 70                       /* the first try: docs/anki.md has the sizes it gives */
#define SHOT_TRIES 3                          /* 70, 50, 35: a JPEG over VJO_GAME_OCR_SHOT_MAX again, lower */
#define SHOT_HOOK_ROWS VJO_GAME_OCR_ROWS_MAX  /* rows a frame through the hook: 9 frames for 544 */
#define SHOT_KSTRIP_ROWS VJO_JPEG_BAND        /* through the kernel strip: 16 x 960 x 4 within its 64 KiB */
/* Every JPEG chunk and its close/rename share one 30 s file deadline. The
 * Shell allows 35 s including capture/encoding, or cancels sooner when the
 * overlay closes. Cleanup then waits up to 30 s for an outstanding file call;
 * a slower one leaves the worker blocked: never acknowledge a leaked descriptor. */
#define SHOT_SAVE_WAIT_US SHOT_FILE_WAIT_US
/* The block: the encoder's state, one strip of colour rows and the JPEG (at most
 * VJO_GAME_OCR_SHOT_MAX), with the arena's alignment and the buffer's spare byte. API 32's
 * trimmed picture is a sub-rectangle of the strip's rows: nothing more. */
#define SHOT_BLOCK_BYTES(width) \
    PAGE_ROUND(VJO_JPEG_STATE_BYTES + 16u + SHOT_HOOK_ROWS * (width) * 4u + VJO_GAME_OCR_SHOT_MAX + 16u)
typedef char vjo_shot_budget_check[SHOT_BLOCK_BYTES(VJO_OCR_MAX_WIDTH) <= VJO_BUDGET_GAME_SHOT ? 1 : -1];
typedef char vjo_shot_rows_check[SHOT_HOOK_ROWS % VJO_JPEG_BAND == 0 &&
                                 SHOT_KSTRIP_ROWS * VJO_OCR_MAX_WIDTH * 4u <= VJO_GAME_OCR_KSTRIP_MAX ? 1 : -1];

/* Rows [y, y + n) of the frame into dst: through the hook, or once the hook could not
 * write the block (the ladder's last rung) through the kernel strip, 16 rows a call. */
static int shot_rows(Capture *c, unsigned y, unsigned n, unsigned char *dst, size_t stride)
{
    for (unsigned done = 0; done < n;) {
        unsigned k = c->mode == VJO_GAME_OCR_SRC_KSTRIP && n - done > SHOT_KSTRIP_ROWS ? SHOT_KSTRIP_ROWS : n - done;
        int got = cancelled(NULL) ? VJO_E_CANCELLED : capture_rows(c, y + done, k, dst + (size_t)done * stride, stride, 0);
        if (got == (int)k) {
            done += k;
            continue;
        }
        if (got != VJO_E_CANCELLED && !blocked && c->mode == VJO_GAME_OCR_SRC_HOOK && kernel_strip_may_help(c->failed)) {
            step_add(VJO_GSTEP_PASS, VJO_GAME_OCR_SRC_HOOK, y + done, c->failed, 0, 0, c->last.wait_us, 0);
            c->mode = VJO_GAME_OCR_SRC_KSTRIP;
            c->failed = 0;
            continue;
        }
        return got < 0 ? got : VJO_E_SOURCE;
    }
    return (int)n;
}

/* One call of the shot's file (vjoOcrShotWrite, or with src NULL vjoOcrShotSave), repeated
 * while the kernel's worker is still at it, at most wait_us. */
static int shot_call(uint32_t offset, uint32_t bytes, const void *src, uint32_t wait_us)
{
    int rc = broker_poll(POLL_SHOT, offset, bytes, (void *)src, wait_us);
    return rc == BROKER_CANCELLED ? VJO_E_CANCELLED : rc;
}

/* API 32: rows of the frame through the bars' scan, its time counted. */
static void trim_scan(VjoTrimScan *s, const uint8_t *rows, uint32_t n, uint32_t stride, uint32_t *us)
{
    uint64_t t = now_wide();
    vjo_trim_rows(s, rows, n, stride);
    *us += (uint32_t)(now_wide() - t);
}

/* One pass over the frame: the picture *pic (rows from pic->y, columns from pic->x) encoded at
 * quality in the block, a band of 16 rows at a time from the strip the rows came in. With s (API
 * 32, the first pass of a shot that trims) the rows are scanned from row 0: none is encoded until
 * the picture's first row, *pic is then the guess (*kind its VJO_TRIM_*) and, unless that is exact
 * (*exact), every row is scanned, a pass too large for the block reading on to the last row for it.
 * Rows of the picture above the strip the guess came in are read again, unless they are black rows
 * all of the strip's first row's colour (a whole frame kept under a tall black band, an all-black
 * frame): the bands of that row then stand for them (stride 0), the same pixels. */
static int shot_pass(Capture *c, void *base, uint32_t held, int quality, VjoTrimScan *s, VjoTrimRect *pic,
                     int *exact, int *kind, VjoBuf *jpg, uint32_t *scan_us)
{
    const uint32_t w = c->r->width, h = c->r->height, stride = w * 4u;
    VjoArena a;
    VjoJpeg *e;
    uint8_t *band;
    uint32_t have = 0, n = 0, next, end; /* the strip holds rows [have, have + n) */
    int rc = VJO_OK, got, scan = 0;
    vjo_arena_init(&a, base, held);
    if (!(band = (uint8_t *)vjo_arena_alloc(&a, (size_t)SHOT_HOOK_ROWS * stride))) return VJO_E_OOM;
    if (s) {
        vjo_trim_begin(s, w, h);
        while (!s->content && s->top * 10u <= h * 3u && have + n < h) {
            have += n;
            n = h - have < SHOT_HOOK_ROWS ? h - have : SHOT_HOOK_ROWS;
            if ((got = shot_rows(c, have, n, band, stride)) != (int)n) return got;
            trim_scan(s, band, n, stride, scan_us);
        }
        *kind = vjo_trim_guess(s, pic, exact);
        scan = !*exact;
    }
    if (!(e = vjo_jpeg_begin(&a, pic->w, pic->h, quality, jpg))) return VJO_E_OOM;
    next = pic->y;
    end = pic->y + pic->h;
    /* black rows of the picture above the strip, all one colour, as the strip's first row is: that row's bands */
    if (s && next < have && s->above && s->top > have && !((have - next) % VJO_JPEG_BAND))
        for (; next < have && !rc; next += VJO_JPEG_BAND)
            if (vjo_jpeg_rows(e, band + (size_t)pic->x * 4u, VJO_JPEG_BAND, 0) < 0 || jpg->len > VJO_GAME_OCR_SHOT_MAX)
                rc = VJO_E_TOO_LARGE;
    for (;;) {
        /* the strip's rows of the picture, a band at a time (the picture's last may be shorter) */
        while (!rc && next < end && next >= have) {
            uint32_t k = end - next < VJO_JPEG_BAND ? end - next : VJO_JPEG_BAND;
            if (next + k > have + n) break;
            if (vjo_jpeg_rows(e, band + (size_t)(next - have) * stride + (size_t)pic->x * 4u, k, stride) < 0 ||
                jpg->len > VJO_GAME_OCR_SHOT_MAX)
                rc = VJO_E_TOO_LARGE;
            next += k;
        }
        /* next: the picture's rows not encoded yet, else the rows not scanned yet */
        have = !rc && next < end ? next : scan && s->rows < h ? s->rows : h;
        if (have >= h) break;
        n = h - have < SHOT_HOOK_ROWS ? h - have : SHOT_HOOK_ROWS;
        if ((got = shot_rows(c, have, n, band, stride)) != (int)n) return got;
        if (scan && s->rows < have + n)
            trim_scan(s, band + (size_t)(s->rows - have) * stride, have + n - s->rows, stride, scan_us);
    }
    if (!rc && (vjo_jpeg_end(e) < 0 || jpg->len > VJO_GAME_OCR_SHOT_MAX)) rc = VJO_E_TOO_LARGE;
    return rc;
}

static int run_shot(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    const uint32_t w = r->width, h = r->height, need = SHOT_BLOCK_BYTES(w);
    Capture c = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 1, 0, 1}; /* fixed: shot_rows picks the rung */
    VjoBuf jpg = {0};
    VjoTrimScan scan;
    VjoTrimRect pic = {0, 0, w, h};
    void *base = NULL;
    SceUID leaked;
    uint32_t held, mem, off = 0, chunks = 0, scan_us = 0;
    int rc, quality = SHOT_QUALITY, exact = 1, kind = VJO_TRIM_NONE, how = 0;
    /* API 32: the bars are guessed in the first pass (none without the option) */
    int guessing = (r->speed & VJO_GAME_OCR_S_TRIM) != 0;
    uint64_t t0;
    if (!IMPORT_READY(vjoOcrShotWrite) || !IMPORT_READY(vjoOcrShotSave)) return VJO_E_OCR_UNAVAILABLE;
    /* The memory check: the block beside the reserve the game always keeps (the
     * resident weights stay; a shot runs no model). */
    out->free_user = free_user();
    out->reserve_bytes = GAME_RESERVE;
    out->need_workspace = need;
    rc = out->free_user < 0 || (int64_t)out->free_user - GAME_RESERVE < (int64_t)need;
    step_add(VJO_GSTEP_CHECK, 0, 0, rc, need, 0, 0, 0);
    if (rc) return VJO_E_OOM;
    shot_uid = game_memblock_alloc(GL_SHOT, VJO_GITEM_SHOT, "VjoGameShot", need, LADDER_ALL, &base, &leaked, &held,
                                   &mem);
    if (shot_uid < 0) {
        shot_uid = leaked;
        return VJO_E_OOM;
    }
    shot_bytes_held = out->shot_block = held;
    out->shot_memory = mem;
    t0 = now_wide();
    for (unsigned t = 1; t <= SHOT_TRIES + 1u; t++) {
        uint64_t t1 = now_wide();
        uint32_t strips = c.pass_strips;
        VjoTrimRect guess;
        rc = shot_pass(&c, base, held, quality, guessing ? &scan : NULL, &pic, &exact, &kind, &jpg, &scan_us);
        step_add(VJO_GSTEP_SHOT, (unsigned)quality, t, rc, (uint32_t)jpg.len, c.pass_strips - strips,
                 (uint32_t)(now_wide() - t1), 0);
        out->shot_quality = (uint32_t)quality;
        out->shot_tries = t;
        if (guessing && (rc == VJO_OK || rc == VJO_E_TOO_LARGE)) {
            /* API 32: the guess, or the picture every row of the pass came to (the next pass's) */
            guessing = 0;
            guess = pic;
            if (!exact) kind = vjo_trim_decide(&scan, &pic);
            how = exact ? 0 : guess.x == pic.x && guess.y == pic.y && guess.w == pic.w && guess.h == pic.h ? 1 : 2;
            if (how == 2) continue; /* read again, at this quality: another picture than the one too large, if it was */
        }
        if (rc != VJO_E_TOO_LARGE || quality <= 35) break;
        quality = quality > 50 ? 50 : 35;
    }
    out->shot_encode_us = (uint32_t)(now_wide() - t0);
    if ((r->speed & VJO_GAME_OCR_S_TRIM) && !guessing)
        step_add(VJO_GSTEP_TRIM, (unsigned)kind, (unsigned)how, 0, pic.x << 16 | pic.y, pic.w << 16 | pic.h,
                 scan_us + 1u, 0);
    if (rc < -199) rc = VJO_E_SOURCE; /* a native capture error: its code is in the PASS step */
    out->source = c.pass_strips ? c.mode : VJO_GAME_OCR_SRC_NONE;
    out->captures = c.pass_strips;
    out->capture_us = c.pass_us;
    out->strip_rows = c.mode == VJO_GAME_OCR_SRC_KSTRIP ? SHOT_KSTRIP_ROWS : SHOT_HOOK_ROWS;
    if (c.last.fb_width) {
        out->fb_width = c.last.fb_width; out->fb_height = c.last.fb_height;
        out->fb_pitch = c.last.fb_pitch; out->fb_format = c.last.fb_format;
    }
    if (rc) return rc;
    /* The file, a staging buffer at a time, then saved: until then cleanup's file_close
     * abandons it (the kernel closes and removes it). */
    t0 = now_wide();
    model_seq = active_seq;
    model_owned = 1;
    shot_file = 1;
    while (!rc && off < jpg.len) {
        uint32_t n = (uint32_t)jpg.len - off < VJO_GAME_OCR_SHOT_CHUNK ? (uint32_t)jpg.len - off : VJO_GAME_OCR_SHOT_CHUNK;
        uint64_t elapsed = now_wide() - t0;
        int got = elapsed >= SHOT_FILE_WAIT_US ? VJO_ERR_TIMEOUT
                    : shot_call(off, n, jpg.data + off, (uint32_t)(SHOT_FILE_WAIT_US - elapsed));
        if (got == (int)n) {
            off += n;
            chunks++;
        } else {
            rc = got < 0 ? got : VJO_E_LOCAL_IO;
        }
    }
    if (!rc) {
        uint64_t elapsed = now_wide() - t0;
        rc = elapsed >= SHOT_SAVE_WAIT_US ? VJO_ERR_TIMEOUT
                 : shot_call(0, (uint32_t)jpg.len, NULL, (uint32_t)(SHOT_SAVE_WAIT_US - elapsed));
    }
    out->shot_write_us = (uint32_t)(now_wide() - t0);
    step_add(VJO_GSTEP_SAVE, chunks, 0, rc, off, 0, out->shot_write_us + 1u, 0);
    if (!rc) {
        model_owned = 0; /* saved: the kernel holds no file of this job any more */
        model_seq = 0;
        shot_file = 0;
        out->shot_bytes = (uint32_t)jpg.len;
        return VJO_OK;
    }
    io_error = rc;
    return rc == VJO_E_CANCELLED || rc == VJO_ERR_CANCELLED ? VJO_E_CANCELLED : VJO_E_LOCAL_IO;
}

/* ---- ocr-speed: helper threads for parallel lines (core/vocr_ocr.h VjoVocrPool) ----
 * A line handed to a helper runs on a thread of its own, created for that line
 * and deleted once its line is placed: no thread outlives a job, and no stack is
 * held between jobs. Same priority and cores as the worker: the lowest user
 * priority on every game core, so each game thread preempts them as it does the
 * worker [SDK: Kernel-Overview, 'Notes on Priority and CPU Affinity Mask
 * Combinations']. Their stacks come from the game's memory and go through the
 * ledger (GL_HELPERS); the job's memory check counts them. */
#define HELPER_STACK_BYTES VJO_STACK_GAME_HELPER
#define HELPER_JOIN_US 10000000u /* a line takes under a second; a cancelled one stops within a tile */
static SceUID helper_uid[1 + VJO_VOCR_HELPERS_MAX] = {-1, -1, -1};
static struct {
    void (*fn)(void *arg);
    void *arg;
} helper_job[1 + VJO_VOCR_HELPERS_MAX];
static unsigned helper_creates, helper_create_failures;

static int helper_entry(SceSize args, void *argp)
{
    unsigned slot = args == sizeof(unsigned) && argp ? *(const unsigned *)argp : 0u;
    if (slot >= 1 && slot <= VJO_VOCR_HELPERS_MAX && helper_job[slot].fn) helper_job[slot].fn(helper_job[slot].arg);
    return 0;
}

static int pool_run(void *ud, unsigned slot, void (*fn)(void *arg), void *arg)
{
    SceUID t;
    (void)ud;
    if (slot < 1 || slot > VJO_VOCR_HELPERS_MAX || helper_uid[slot] >= 0 || blocked) return -1;
    helper_job[slot].fn = fn;
    helper_job[slot].arg = arg;
    t = sceKernelCreateThread("VjoGameVocrHelper", helper_entry, WORKER_PRIORITY, HELPER_STACK_BYTES, 0,
                              WORKER_AFFINITY, NULL);
    if (t < 0) { helper_create_failures++; return -1; }
    vjo_ledger_add(&g_game_ledger, GL_HELPERS, VJO_BUDGET_GAME_HELPER_STACK);
    if (sceKernelStartThread(t, sizeof(slot), &slot) < 0) {
        helper_create_failures++;
        if (sceKernelDeleteThread(t) >= 0) vjo_ledger_sub(&g_game_ledger, GL_HELPERS, VJO_BUDGET_GAME_HELPER_STACK);
        else blocked = 1; /* a thread object we cannot delete: no more jobs */
        return -1;
    }
    helper_uid[slot] = t;
    helper_creates++;
    return 0;
}

static int pool_join(void *ud, unsigned slot)
{
    SceUInt timeout = HELPER_JOIN_US;
    (void)ud;
    if (slot < 1 || slot > VJO_VOCR_HELPERS_MAX || helper_uid[slot] < 0) return -1;
    if (sceKernelWaitThreadEnd(helper_uid[slot], NULL, &timeout) < 0) return -1;
    if (sceKernelDeleteThread(helper_uid[slot]) < 0) return -1;
    helper_uid[slot] = -1;
    helper_job[slot].fn = NULL;
    vjo_ledger_sub(&g_game_ledger, GL_HELPERS, VJO_BUDGET_GAME_HELPER_STACK);
    return 0;
}

/* The calling thread's run time [SDK: Kernel-Reference, SceKernelThreadInfo.runClocks,
 * microseconds since the thread was created], for each line's share of a core. */
static uint64_t pool_thread_us(void *ud)
{
    SceKernelThreadInfo info;
    (void)ud;
    if (!IMPORT_READY(sceKernelGetThreadInfo) || !IMPORT_READY(sceKernelGetThreadId)) return 0;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return (uint64_t)info.runClocks;
}

static const VjoVocrPool helper_pool_base = {0, pool_run, pool_join, pool_thread_us, NULL};
static VjoVocrPool helper_pool;
/* Line texts of earlier jobs (core/vocr_ocr.h VjoVocrCache): a page read again
 * is not recognized again. Emptied whenever the weights are read from the file. */
static VjoVocrCache line_cache;

/* Forget every cached line (tools/vita3k_smoke: the next job recognizes every line). */
void vjo_game_ocr_cache_clear(void);
void vjo_game_ocr_cache_clear(void)
{
    sceClibMemset(&line_cache, 0, sizeof(line_cache));
}

/* ---- ocr-speed: the ARM clock while a job recognizes (ocr_boost_clock) ----
 * On by default (the owner's decision of 2026-10-15, on a Vita developer's
 * advice: games set the clock themselves, and 500/222/166 MHz are common
 * settings). The clock goes to 444 MHz just before the first line is recognized
 * and back to what it was once the last one is, unless the game changed it
 * meanwhile. A clock that is already 444 MHz or more (PSVshell's 500, say) is
 * left as it is (rc 1 in the step): this never lowers a clock. Only the ARM
 * clock: the bus and GPU clocks are not touched. ScePower keeps the clock a
 * process asked for and the system applies it while that process is in front:
 * the worker's call is the game's own (PSVshell reads the same per-process
 * value, Electry/PSVshell 9a47b0e src/oc.c:119-147, and hooks the setter to
 * replace a game's request, src/main.c:106-123; this asks for the value only
 * while it recognizes). 444 is the highest the API accepts without a kernel
 * patch (PSVshell's 500 MHz needs one, main.c:113-117), which this does not
 * make. Logged in the step log: before, during, after. */
#define BOOST_MHZ 444
static int boost_job, boost_raised;
static int32_t boost_before, boost_during, boost_rc;

static void job_busy(void *ud, int on)
{
    (void)ud;
    if (!boost_job || !IMPORT_READY(scePowerGetArmClockFrequency) || !IMPORT_READY(scePowerSetArmClockFrequency))
        return;
    if (on) {
        boost_before = scePowerGetArmClockFrequency();
        if (boost_before > 0 && boost_before < BOOST_MHZ) {
            boost_rc = scePowerSetArmClockFrequency(BOOST_MHZ);
            boost_raised = boost_rc >= 0;
        } else {
            boost_rc = 1;
        }
        boost_during = scePowerGetArmClockFrequency();
        return;
    }
    if (boost_raised) {
        /* Back to the clock before, unless the game set another one meanwhile. A game that sets
         * 444 MHz itself during the job cannot be told from this job's own setting, and gets the
         * clock before back (inherent: ScePower keeps one value per process). */
        if (scePowerGetArmClockFrequency() == boost_during) scePowerSetArmClockFrequency(boost_before);
        boost_raised = 0;
    }
    step_add(VJO_GSTEP_CLOCK, 0, boost_before > 0 ? (unsigned)boost_before : 0u, boost_rc,
             boost_during > 0 ? (uint32_t)boost_during : 0u, (uint32_t)scePowerGetArmClockFrequency(), 0, 0);
}

/* The job's policy, from its request's flags (include/vjo_game_ocr.h, API 20):
 * the stable-text wait and its limit, whether to wait first, blank lines. */
static void policy_sleep(uint32_t us)
{
    sceKernelDelayThread(us);
}

static void request_policy(const VjoGameOcrRequest *r, VjoVocrPolicy *p)
{
    sceClibMemset(p, 0, sizeof(*p));
    p->settle_us = ((r->flags >> VJO_GAME_OCR_SETTLE_SHIFT) & 0xFFu) * VJO_GAME_OCR_SETTLE_UNIT_US;
    p->settle_max_us = (r->flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT) * VJO_GAME_OCR_SETTLE_MAX_UNIT_US;
    p->settle_first = (r->flags & VJO_GAME_OCR_F_SETTLE_FIRST) != 0;
    p->blank = (r->flags & VJO_GAME_OCR_F_BLANK) != 0;
    p->gate = (r->speed & VJO_GAME_OCR_S_GATE) != 0; /* API 26: the text-presence gate */
    p->head_gate = (r->speed & VJO_GAME_OCR_S_HEAD_GATE) != 0; /* API 42: the head gate */
    p->two_level = (r->speed & VJO_GAME_OCR_S_TWO_LEVEL) != 0;  /* API 42: the two-level head */
    /* API 50: the line finder's fixes (bee-san/vita-vn-ocr #129) */
    p->line_fix = (r->speed & VJO_GAME_OCR_S_LINE_FIX ? (unsigned)VOCR_FIX_DEFAULT : 0u) |
                  (r->speed & VJO_GAME_OCR_S_LINE_JUNK ? (unsigned)VOCR_FIX_JUNK : 0u) |
                  (r->speed & VJO_GAME_OCR_S_LINE_VALLEY ? (unsigned)VOCR_FIX_VALLEY : 0u);
    p->gate_res = &gate_res;
    p->sleep = policy_sleep;
    /* API 22: helpers, the line cache, the clock around the recognitions. */
    helper_pool = helper_pool_base;
    helper_pool.helpers = r->speed & VJO_GAME_OCR_S_HELPERS_MASK;
    if (helper_pool.helpers > VJO_VOCR_HELPERS_MAX) helper_pool.helpers = VJO_VOCR_HELPERS_MAX;
    p->pool = &helper_pool;
    p->cache = &line_cache;
    if (r->speed & VJO_GAME_OCR_S_NOCACHE) { /* the self-test: every line recognized, no stale entry left */
        sceClibMemset(&line_cache, 0, sizeof(line_cache));
        p->cache = NULL;
    }
    boost_job = (r->speed & VJO_GAME_OCR_S_BOOST) != 0;
    boost_raised = 0;
    boost_before = boost_during = boost_rc = 0;
    p->busy = job_busy;
}

/* Bytes a job's helpers add to its workspace block of `block` bytes (their line
 * slots end it: core/vocr_ocr.h vjo_vocr_helpers_bytes, page-rounded with the
 * block) and their stacks, for the memory check. */
static size_t helpers_need(const char *model, size_t block, unsigned helpers)
{
    size_t hb = vjo_vocr_helpers_bytes(model, helpers);
    if (!hb) return 0;
    return PAGE_ROUND(block + hb) - PAGE_ROUND(block) + (size_t)helpers * VJO_BUDGET_GAME_HELPER_STACK;
}

/* The most helpers, want at most, whose memory fits in room (one fewer at a time; 0: none, the serial job). */
static unsigned helpers_fit(const char *model, size_t block, unsigned want, int64_t room)
{
    for (; want; want--) {
        size_t extra = helpers_need(model, block, want);
        if (extra && room >= (int64_t)extra) break;
    }
    return want;
}

/* ---- native text hooks: which text a job shows (docs/native-hooks.md, core/hook_select.h) ----
 * With hooks installed in this game (game_ocr/hooks.c), a job begins with a snapshot of the streams'
 * texts. vjo_hook_select_before may name a stream that answers the job without recognition: the job
 * then reads only the region's first capture pass, so the kernel publishes its capture (capture_seq,
 * done_seq, the scene) as for OCR, and returns the stream's text. Otherwise OCR runs as without hooks
 * and vjo_hook_select_after may replace its text with an agreeing stream's. With nothing installed
 * a job does none of this. The snapshot is static, in the worker's data pages (never on its stack):
 * one stream's copy, the texts as the job began (HOOK_POOL_BYTES, each at most HOOK_TEXT_CAP) and the
 * candidates; the comparison's scratch, 2,562 B of arrays in frames of 2,736 B at most (core/hook_select.c), is on
 * the stack only while it compares. */
#define HOOK_POOL_BYTES 2048u
/* A stream's text for the comparison: 512 raw bytes (VJO_HOOK_RAW_BYTES) of double-byte CP932 or 256
 * UTF-16 units whole, as UTF-8 (768 B); a text cut by it or by a full pool compares as what it holds
 * (core/hook_select.c counts nothing it did not see as matched) and is shown whole (hook_show). */
#define HOOK_TEXT_CAP 769u
/* The room a shown stream's text keeps in the result: any stream's 512 raw bytes decoded (3 UTF-8
 * bytes each at most) and the NUL; OCR's text after it (for the log) is cut to the rest. */
#define HOOK_SHOW_BYTES (VJO_HOOK_RAW_BYTES * 3u + 1u)
typedef char vjo_hook_show_check[HOOK_SHOW_BYTES < VJO_GAME_OCR_TEXT_BYTES / 2u ? 1 : -1];
/* API 43: after this many trusted answers in a row (8 lines, a minute or two of reading) the weights, LM and cascade
 * model an OCR job left resident go back to the game. Not at the first: a job between them that runs OCR (the
 * site's stream busy or its text filtered out at that moment) would read them all again each time. */
#define TRUST_DROP_ANSWERS 8u
static struct {
    VjoHookSelect sel;   /* the automatic pick's state, for the game session */
    VjoHookSelect saved; /* sel as the job found it: a job that fails changes nothing */
    VjoHookCacheKey cache_key;
    VjoHookLearned cached; /* last loaded/attempted saved identity, never dialogue or partial evidence */
    int cache_attempted, build_known, cache_ready;
    uint32_t mode, installed, refused; /* vjo_hooks_counts */
    uint32_t from, flags, score, streams; /* this job's result fields */
    /* the hook watch (hooks_watch): the sites that answer without recognition and have held text (Luna's, and
     * with API 43's trust Rubi's verified ones; bit id - 1), the newest sequence a job's snapshot saw or the watch
     * reported, and when it last reported one */
    uint32_t luna_seen, watch_seen;
    uint64_t watch_sent;
    /* API 49: the site streams the watch has classified (bit id - 1; a site's kind and table entry never change), the
     * last user job's VJO_GAME_OCR_S_TRUST, and the content of the last trusted line it reported (its decoded text's
     * hash and length: the same text again, from this stream or another, is no new line) */
    uint32_t sites_known;
    int trust_on;
    uint32_t watch_hash, watch_len;
    /* API 43: a trusted source has answered in this game (VJO_GAME_OCR_S_TRUST): no warm-up reads the weights
     * after a job that found no line, as the hook supplies the lines; and the trusted answers since the last job
     * that ran OCR (at TRUST_DROP_ANSWERS the resident blocks go back to the game) */
    int trusted;
    uint32_t trusted_run;
    unsigned n;
    VjoHookCand cand[VJO_HOOK_STREAMS];
    VjoHookSnap snap;
    char pool[HOOK_POOL_BYTES];
} hk;

static size_t text_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

/* AUTO keeps Luna text ahead of generic streams even when the text pool fills. Otherwise keep the
 * requested/learned stream, then the newer text. Pinned and OCR-only jobs pass no Luna priority. */
static int hook_first(uint32_t a, uint32_t b, const uint32_t *seq, uint32_t want, uint32_t luna)
{
    if (!!(luna & VJO_HOOK_STREAM_BIT(a)) != !!(luna & VJO_HOOK_STREAM_BIT(b)))
        return (luna & VJO_HOOK_STREAM_BIT(a)) != 0;
    if ((a == want) != (b == want)) return a == want;
    return (int32_t)(seq[a] - seq[b]) > 0;
}

/* The streams that hold text as the job begins (hk.streams: bit id - 1 each), and as candidates
 * (hk.cand) those whose text decodes to some: AUTO's Luna sites first (with trust, API 43, Rubi's verified
 * sites with them), the stream the pick names (pinned, or the locked one), then the newest texts, so a full pool
 * cuts the oldest generic text. Return whether any such site's decoded candidate is available; it answers without
 * OCR and needs no learned-source cache. */
static int hooks_snapshot(uint32_t pick, int trust)
{
    uint32_t seq[VJO_HOOK_STREAMS + 1u], want = pick == VJO_HOOK_PICK_AUTO ? hk.sel.locked : pick;
    uint32_t luna = 0, matched = 0;
    uint8_t order[VJO_HOOK_STREAMS];
    unsigned k = 0;
    int has_luna = 0;
    size_t used = 0;
    hk.n = hk.streams = 0;
    for (uint32_t id = 1; id <= VJO_HOOK_STREAMS; id++) {
        unsigned at;
        if (!vjo_hooks_snap(id, &hk.snap) || !hk.snap.n) continue;
        hk.streams |= 1u << (id - 1u);
        if ((int32_t)(hk.snap.seq - hk.watch_seen) > 0) hk.watch_seen = hk.snap.seq; /* this job reads it */
        if (pick == VJO_HOOK_PICK_AUTO && hk.snap.kind == VJO_HOOK_KIND_SITE &&
            (vjo_hooks_verified(id) ? trust : hk.snap.site != NULL))
            luna |= VJO_HOOK_STREAM_BIT(id);
        /* a restored source has no runtime slot until this launch supplies it. Of two slots holding the learned
         * source (a race could give a CALL caller a second one, which keeps the text it was taken with), the newest:
         * the one its caller writes (#366's audit-b, F1; core/hook_select.c takes the first one in this order) */
        if (pick == VJO_HOOK_PICK_AUTO && hk.sel.interval &&
            (hk.snap.kind == VJO_HOOK_KIND_SITE) == !!hk.sel.locked_site &&
            hk.snap.key == hk.sel.locked_key && hk.snap.encoding == hk.sel.locked_encoding &&
            (!matched || (int32_t)(hk.snap.seq - seq[matched]) > 0))
            want = matched = id;
        seq[id] = hk.snap.seq;
        for (at = k; at && hook_first(id, order[at - 1u], seq, want, luna); at--) order[at] = order[at - 1u];
        order[at] = (uint8_t)id;
        k++;
    }
    for (unsigned i = 0; i < k; i++) {
        uint32_t id = order[i];
        uint32_t site, verified;
        size_t room = sizeof(hk.pool) - used, len;
        if (room < 2u || !vjo_hooks_snap(id, &hk.snap)) continue;
        len = vjo_hooks_text(&hk.snap, hk.pool + used, room < HOOK_TEXT_CAP ? room : HOOK_TEXT_CAP);
        if (!len) continue;
        site = hk.snap.kind == VJO_HOOK_KIND_SITE ? VJO_HOOK_CAND_SITE : 0u;
        /* A Rubi verified entry may borrow Luna's filter (#273). Keep its verified identity:
         * in a tight-memory game it must reach the capture-only fallback below. */
        verified = (uint32_t)vjo_hooks_verified(id);
        if (site && hk.snap.site && !verified) { site |= VJO_HOOK_CAND_LUNA; has_luna = 1; }
        else if (site && verified && trust) has_luna = 1; /* API 43: it answers as Luna's do */
        hk.cand[hk.n++] = (VjoHookCand){id, hk.snap.seq, site, hk.pool + used, len, hk.snap.key, hk.snap.encoding, verified};
        used += len + 1u;
    }
    hk.luna_seen |= luna;
    return has_luna;
}

static const VjoHookCand *hook_cand(uint32_t id)
{
    for (unsigned i = 0; i < hk.n; i++)
        if (hk.cand[i].id == id) return &hk.cand[i];
    return NULL;
}

/* The rules the job's text gets (core/textnorm.h): every one, or (API 36) all but the border rule without
 * VJO_GAME_OCR_S_DROP_BORDERS (config.ini non_japanese_filter = none). */
static unsigned norm_flags(const VjoGameOcrRequest *r)
{
    return r->speed & VJO_GAME_OCR_S_DROP_BORDERS ? 0u : VJO_TEXTNORM_KEEP_BORDERS;
}

/* Stream k's text in out->text, normalised as OCR's text is (core/textnorm.h, with the job's flags), with,
 * after its NUL for the log, OCR's text when the stream replaces it (ocr), else the stream's own before
 * normalising. The text is the stream's whole when it still holds what the job compared (the same seq), else
 * the job's copy of it. */
static void hook_show(VjoGameOcrResult *out, const VjoHookCand *k, int ocr, unsigned flags)
{
    char *t = out->text;
    const size_t cap = sizeof(out->text);
    size_t o = 0, room = cap, h = 0;
    VjoTextNorm norm;
    if (!k) return;
    if (ocr) { /* OCR's (normalised) text to the end of the buffer, cut at a character to leave the room */
        o = text_len(t, cap - 1u);
        if (o > cap - 1u - HOOK_SHOW_BYTES) {
            o = cap - 1u - HOOK_SHOW_BYTES;
            while (o && ((unsigned char)t[o] & 0xC0) == 0x80) o--;
        }
        memmove(t + cap - 1u - o, t, o);
        t[cap - 1u] = '\0';
        room = cap - 1u - o;
    }
    if (vjo_hooks_snap(k->id, &hk.snap) && hk.snap.seq == k->seq && hk.snap.key == k->key &&
        (hk.snap.kind == VJO_HOOK_KIND_SITE) == !!k->site && hk.snap.encoding == k->encoding)
        h = vjo_hooks_text(&hk.snap, t, room);
    if (!h) {
        h = k->n < room - 1u ? k->n : room - 1u;
        memmove(t, k->text, h);
        t[h] = '\0';
    }
    if (!ocr) {
        vjo_text_normalize_keep(t, cap, NULL, flags, &norm);
        return;
    }
    vjo_text_normalize_as(t, flags, &norm);
    h = text_len(t, room);
    memmove(t + h + 1u, t + cap - 1u - o, o + 1u);
    sceClibMemset(t + h + 2u + o, 0, cap - (h + 2u + o));
}

/* API 49: a trusted answer's capture without a pixel read (VJO_ROWS_HOOK): the kernel publishes the trusted line's
 * scene while the region shows it, and (API 51) the region's current scene once it shows another (a menu over the
 * line, an untrusted line since), 0; else (a region without a screen yet) the first pass as before, run_hook_pass.
 * No memory: the answer needs no block, so a game with less than VJO_GAME_RESERVE_HOOK_ONLY free still gets it. */
static int hook_publish(const VjoGameOcrRequest *r)
{
    VjoGameOcrRows q;
    VjoGameOcrRowsInfo info;
    sceClibMemset(&q, 0, sizeof(q));
    sceClibMemset(&info, 0, sizeof(info));
    q.size = sizeof(q);
    q.seq = r->seq;
    q.mode = VJO_GAME_OCR_SRC_HOOK;
    q.flags = VJO_ROWS_HOOK;
    info.size = sizeof(info);
    return vjoOcrCaptureRows(&q, NULL, &info);
}

/* A job a stream answers: no model and no memory check for one, no finding, no recognition; the region's
 * first capture pass, a strip at a time into one strip's block (VjoGameVocrCapture, under the job's
 * reserve), so the kernel publishes the capture as for an OCR job's first pass. */
static int run_hook_pass(const VjoGameOcrRequest *r, VjoGameOcrResult *out, Capture *c, VjoVocrStats *st)
{
    unsigned rows = vjo_vocr_strip_rows(r->width, r->height);
    uint32_t bytes = rows * r->width, held, mem;
    void *base = NULL;
    SceUID leaked;
    if (!rows) return VJO_E_OCR_REGION;
    out->free_user = free_user();
    out->reserve_bytes = job_reserve;
    out->need_workspace = PAGE_ROUND(bytes);
    if (out->free_user < 0 || (int64_t)out->free_user - (int64_t)job_reserve < (int64_t)PAGE_ROUND(bytes))
        return VJO_E_OOM;
    capture_uid = game_memblock_alloc(GL_CAPTURE, VJO_GITEM_CAPTURE, "VjoGameVocrCapture", bytes, LADDER_ALL, &base,
                                      &leaked, &held, &mem);
    if (capture_uid < 0) {
        capture_uid = leaked;
        return VJO_E_OOM;
    }
    capture_bytes_held = held;
    capture_memory = mem;
    st->strip_rows = rows;
    for (unsigned y = 0; y < r->height; y += rows) {
        unsigned n = r->height - y < rows ? r->height - y : rows;
        uint64_t t0 = now_wide();
        int got = strip_source(c, y, n, base, r->width, VJO_STRIP_FIRST | (y + n == r->height ? VJO_STRIP_LAST : 0u));
        st->capture_us += (uint32_t)(now_wide() - t0);
        st->strips++;
        if (got != (int)n) return got < 0 ? got : VJO_E_SOURCE;
    }
    return VJO_OK;
}

/* The cache is optional (older kernels do not export its broker). All IO and exact-build hashing
 * happen in this worker thread, never module_start or a hook. A temporary cache blob is on the worker
 * stack only while loading/saving; dialogue and partial comparison history never leave the process. */
static int hooks_cache_io(unsigned op, uint8_t *data)
{
    int rc, previous_error = io_error;
    model_owned = 1;
    model_seq = active_seq;
    rc = broker_poll(POLL_CACHE, op, 0, data, MODEL_WAIT_US); /* negative: no cache (BUSY, TIMEOUT, CANCELLED) */
    /* A timeout, user-copy failure or failed close can leave broker ownership even though this helper
     * is optional. Drain it exactly like a model; cleanup() must retain any unresolved descriptor. */
    file_close(NULL);
    if (model_owned) return VJO_ERR_BUSY;
    io_error = previous_error; /* a cache miss/failure is not a model error */
    return rc;
}

static int hook_revision(uint8_t out[32])
{
#ifdef VJO_HOOK_DEFINITION_SHA256
    const char *hex = VJO_HOOK_DEFINITION_SHA256;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned c = (unsigned char)hex[2 * i + j];
            if (c >= '0' && c <= '9') c -= '0';
            else if (c >= 'a' && c <= 'f') c -= 'a' - 10u;
            else return 0;
            v = v * 16u + c;
        }
        out[i] = (uint8_t)v;
    }
    return !hex[64];
#else
    (void)out;
    return 0; /* a build without a definition fingerprint must use fresh learning */
#endif
}

static void hooks_cache_prepare(uint32_t pick)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES];
    VjoHookLearned learned;
    int n;
    /* a broker still held (a charset whose close failed, hooks_sc3_load) refuses the cache: tried at a later job */
    if (pick != VJO_HOOK_PICK_AUTO || !hk.installed || hk.cache_attempted || model_owned) return;
    hk.cache_key.mode = hk.mode;
    hk.cache_attempted = 1;
    if (!IMPORT_READY(vjoOcrHookCache) || !hk.cache_key.title[0] ||
        !hook_revision(hk.cache_key.definition)) return;
    if (!hk.build_known) {
        if (!vjo_hooks_build_id(hk.cache_key.build)) return;
        hk.build_known = 1;
    }
    n = hooks_cache_io(VJO_GAME_OCR_HOOK_CACHE_LOAD, data);
    if (n == VJO_ERR_FORMAT) n = 0; /* corrupt data is equivalent to fresh learning */
    if (n < 0) return; /* fresh learning; an inaccessible cache never affects display */
    hk.cache_ready = 1;
    if (vjo_hook_cache_get(data, (size_t)n, &hk.cache_key, &learned) &&
        vjo_hook_select_restore(&hk.sel, &learned)) hk.cached = learned;
}

static void hooks_cache_save(void)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES];
    VjoHookLearned learned;
    int n, has;
    if (!hk.cache_ready) return;
    has = vjo_hook_select_learned(&hk.sel, hk.trust_on, &learned); /* trusted only with hook_trust = on */
    if (!memcmp(&learned, &hk.cached, sizeof(learned))) return;
    /* Attempt only when the chosen identity or learned interval changes. A failed write must not
     * become a write per reading; OCR selection and in-session confidence remain usable. */
    hk.cached = learned;
    n = hooks_cache_io(VJO_GAME_OCR_HOOK_CACHE_LOAD, data);
    if (n == VJO_ERR_FORMAT) n = 0;
    if (n < 0 || cancelled(NULL)) return;
    if (vjo_hook_cache_put(data, (size_t)n, &hk.cache_key, has ? &learned : NULL) > 0)
        (void)hooks_cache_io(VJO_GAME_OCR_HOOK_CACHE_SAVE, data);
}

/* MAGES SC3 sites (game_ocr/hooks.c, docs/native-hooks.md "SC3 text"): their game's charset, which their text needs,
 * at the first job (the kernel's broker is a claimed job's): VJO_GAME_OCR_OPEN_SC3 opens
 * ux0:data/VitaJPOverlay/hooks/<title ID>.sc3, the calling game's own, into a USER_RW block of its size
 * (VJO_BUDGET_GAME_SC3 at most), kept until the worker stops, beside VJO_GAME_RESERVE_HOOK_ONLY as a trusted hook's
 * pass is (the charset is what lets those sites answer without OCR). No file, another file than the pinned one or a
 * failed read: the block goes back, the sites give no text, OCR reads the game, and it is not tried again; an open
 * or a read cancelled or timed out is tried at a later job (SC3_TRIES in all), and with no memory free beside the
 * reserve nothing is tried (again at the next job). A block an earlier try could not give back (its base unreadable,
 * its free refused) is freed before another is taken, never left behind; while that free fails nothing is taken. */
static void hooks_sc3_load(void)
{
    const VjoSc3Pin *pin = vjo_hooks_sc3_want();
    VjoGameOcrModelInfo info;
    void *base = NULL;
    SceUID leaked;
    uint32_t mem;
    int previous_error = io_error, size, ok = 0;
    if (!pin || sc3_tries >= SC3_TRIES || model_owned || resident_release((void *)&sc3_desc)) return;
    if ((int64_t)free_user() - (int64_t)VJO_GAME_RESERVE_HOOK_ONLY < (int64_t)PAGE_ROUND(pin->bytes)) return;
    sc3_tries++;
    sc3_blk.uid = game_memblock_alloc(GL_SC3, VJO_GITEM_SC3, sc3_desc.name, pin->bytes, LADDER_USER, &base, &leaked,
                                      &sc3_blk.held, &mem);
    if (sc3_blk.uid < 0) {
        /* a block whose base could not be read nor freed stays recorded: freed at the next try, or at the stop */
        sc3_blk.uid = leaked;
        return;
    }
    size = broker_open(VJO_GAME_OCR_OPEN_SC3, &info);
    if (size == (int)pin->bytes && !file_read(NULL, 0, base, pin->bytes)) ok = vjo_hooks_sc3_set(base, pin->bytes);
    /* #368's audit F1: a read that timed out (file_read's io_error; the job's own is 0 here, its first use of the
     * broker) is tried again, as an open that timed out is */
    if (ok || !(size == BROKER_CANCELLED || size == VJO_ERR_TIMEOUT || io_error == VJO_ERR_TIMEOUT || cancelled(NULL)))
        sc3_tries = SC3_TRIES;
    file_close(NULL);
    /* A charset that cannot be read is not the job's model error. A close that failed is (#368's audit F2, as
     * hooks_cache_io): the broker is still the charset's, so the job's own files cannot open, and the job says why. */
    if (!model_owned) io_error = previous_error;
    if (!ok) (void)resident_release((void *)&sc3_desc);
}

/* Every result's hook fields; a job that failed shows no text, from any stream, and leaves the
 * selection as it found it. A stream that agreed with this job's OCR keeps its CALL slot for the session,
 * and CALLS_FULL says a new caller found every CALL slot taken (dropped, or it replaced one): audit-hooks F2. */
static void hooks_result(VjoGameOcrResult *out, int rc)
{
    uint32_t dropped = 0, replaced = 0;
    if (rc) {
        hk.sel = hk.saved;
        hk.from = hk.flags = 0;
    } else if (hk.installed) {
        const VjoHookCand *agreed = hk.from && (hk.flags & VJO_HOOK_R_AGREED) ? hook_cand(hk.from) : NULL;
        if (agreed) vjo_hooks_keep(hk.from, agreed->key); /* only while its slot holds the caller the job saw */
        vjo_hooks_call_stats(&dropped, &replaced);
        if (dropped || replaced) hk.flags |= VJO_HOOK_R_CALLS_FULL;
    }
    out->text_from = hk.from;
    out->hook_flags = hk.flags;
    out->hook_score = hk.score;
    out->hook_streams = hk.streams;
    out->hook_mode = hk.mode;
    out->hook_installed = hk.installed;
    out->hook_refused = hk.refused;
}

/* No OCR can run in this game now: its memory check failed, or (#280's audit F1) the check passed and the last model's
 * weights were refused once the job's blocks were taken. A Rubi verified site's newest text answers, unconfirmed
 * (core/hook_select.h vjo_hook_select_no_memory: id and its flags), with its capture pass beside the smaller
 * VJO_GAME_RESERVE_HOOK_ONLY. 1 when it answered; a pass that fails keeps the job's refusal. */
static __attribute__((noinline)) int no_memory_answer(const VjoGameOcrRequest *r, VjoGameOcrResult *out, Capture *c,
                                                      VjoVocrStats *st, uint32_t id, uint32_t flags)
{
    job_reserve = VJO_GAME_RESERVE_HOOK_ONLY;
    int pass = run_hook_pass(r, out, c, st);
    if (c->failed)
        step_add(VJO_GSTEP_PASS, c->mode, c->pass_strips, c->failed, c->pass_bytes, c->last.fb_format, c->pass_us, 0);
    if (pass) return 0;
    hook_show(out, hook_cand(id), 0, norm_flags(r));
    hk.from = id;
    hk.flags = flags;
    return 1;
}

int vjo_game_ocr_run_request(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    if (!r || !out) return VJO_E_SOURCE;
    if (!g_game_ledger.defs) ledger_init();
    sceClibMemset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->seq = r->seq;
    /* Native hooks: what the worker installed as its game started; this job's hook fields begin empty. */
    vjo_hooks_counts(&hk.mode, &hk.installed, &hk.refused);
    if (hk.cache_attempted && hk.cache_key.mode != hk.mode) {
        sceClibMemset(&hk.sel, 0, sizeof(hk.sel));
        sceClibMemset(&hk.cached, 0, sizeof(hk.cached));
        hk.cache_attempted = hk.cache_ready = 0;
    }
    hk.saved = hk.sel;
    hk.from = hk.flags = hk.score = hk.streams = 0;
    hk.n = 0;
    active_seq = r->seq; io_error = 0; stat_error = 0; open_which = VJO_GAME_OCR_OPEN_MODEL; open_cascade = 0;
    tiered = 0; job_reserve = GAME_RESERVE;
    log_to = out;
    job_started_us = now_wide();
    int rc = VJO_E_SOURCE;
    VjoVocrStats stats = {0};
    uint32_t tried_strips = 0, tried_us = 0, tried_find_us = 0; /* API 47, auto: the tries before the last */
    uint32_t tried_settle_us = 0, tried_settle_passes = 0;      /* and the stable-text wait they spent */
    int retried = 0;
    unsigned try_steps = 0; /* the step log's length as the try began */
    /* API 54, the spotter across the job's tries (audit-vocr6-spotter2-rubi F3): its weights read in the job and
     * their load time, and why the rows' try left its lines to GRAD when auto's columns try follows (spot_handed:
     * VJO_E_VERTICAL for the hand-over, or the try's refusal), so the job's spot step says it */
    unsigned spot_read = 0;
    uint32_t spot_load_us = 0;
    int spot_handed = 0;
    Capture capture = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 0, 0, 0};
    uint32_t paused_us = 0; /* API 55: the game's waits for this job's frame copies (auto's earlier tries') */
    sceClibMemset(&pz, 0, sizeof(pz));
    pause_frames = 0;
    if (blocked || workspace_uid >= 0 || capture_uid >= 0 || shot_uid >= 0 || frame_uid >= 0 || model_owned) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    /* The refinement's mode (bits 12-13): 3 is refused here, as unknown bits are. */
    unsigned refine_mode = (r->flags >> VJO_GAME_OCR_REFINE_SHIFT) & VJO_GAME_OCR_REFINE_MASK;
    /* API 23: a shot carries no model, tiers or other flag but F_FULL, and (API 32) no speed but S_TRIM */
    int shot = (r->flags & VJO_GAME_OCR_F_SHOT) != 0;
    /* API 47: vertical text (config.ini ocr_vertical): on, the vertical model on the region's columns; auto, its
     * rows unless the finder's rows look like vertical text (check), then its columns (vertical, below). API 53
     * (#379): on prefers columns: its rows first, as auto's, and its columns unless they look like horizontal text
     * (core/vocr_ocr.h VJO_VOCR_CHECK_ON); both read rows where the vertical model cannot be read (after a file it
     * could not read, columns_bad, the game's later jobs read rows unchecked) or has no room. */
    unsigned vmode = (r->speed >> VJO_GAME_OCR_S_VERTICAL_SHIFT) & VJO_GAME_OCR_S_VERTICAL_MASK;
    int vertical = 0;
    int check = columns_bad ? 0 : vmode == VJO_GAME_OCR_VERTICAL_AUTO ? VJO_VOCR_CHECK_AUTO
                                : vmode == VJO_GAME_OCR_VERTICAL_ON ? VJO_VOCR_CHECK_ON : 0;
    if (r->size != sizeof(*r) || !r->seq || !r->capture_seq || (r->flags & ~VJO_GAME_OCR_F_ALL) ||
        vmode > VJO_GAME_OCR_VERTICAL_AUTO ||
        refine_mode > VJO_GAME_OCR_REFINE_PSP || (r->tiers & ~VJO_VOCR_TIERS_ALL) ||
        (r->speed & ~(shot ? VJO_GAME_OCR_S_SHOT_ALL : VJO_GAME_OCR_S_ALL)) ||
        !r->width || r->width > VJO_OCR_MAX_WIDTH || !r->height || r->height > VJO_OCR_MAX_HEIGHT ||
        !r->fb_width || r->fb_width > VJO_OCR_MAX_WIDTH || !r->fb_height || r->fb_height > VJO_OCR_MAX_HEIGHT ||
        r->x > r->fb_width - r->width || r->y > r->fb_height - r->height ||
        (shot ? r->tiers || r->hook_pick || (r->flags & ~(VJO_GAME_OCR_F_SHOT | VJO_GAME_OCR_F_FULL))
              : !r->model_dir[0] || !r->model[0]) ||
        r->model_dir[sizeof(r->model_dir)-1] || r->model[sizeof(r->model)-1] ||
        r->fallback[sizeof(r->fallback)-1] ||
        (r->hook_pick > VJO_HOOK_STREAMS && r->hook_pick != VJO_HOOK_PICK_OCR)) goto done;
    out->region_x = r->x; out->region_y = r->y; out->width = r->width; out->height = r->height;
    out->fb_width = r->fb_width; out->fb_height = r->fb_height;
    if (cancelled(NULL)) { rc = VJO_E_CANCELLED; goto done; }
    step_add(VJO_GSTEP_MEM, 0, 0, 0, 0, 0, 0, 1);
    if (shot) { rc = run_shot(r, out); goto done; } /* both builds: an Anki card's screenshot (API 23) */
#if VJO_DEBUG
    if (r->flags & VJO_GAME_OCR_F_PROBE) { rc = run_probe(r, out); goto done; }
    vjo_psp_probe_job(psp_title); /* the read-only PSP probe: once, at the first OCR job (game_ocr/psp_probe.h) */
#else
    if (r->flags & VJO_GAME_OCR_F_PROBE) goto done; /* no probe in the normal build: VJO_E_SOURCE */
#endif
    /* Native hooks (docs/native-hooks.md): the streams as the job begins, and a stream that may answer
     * it without recognition. The self-test's jobs (every line recognized, or the fallback forced) are
     * OCR's work whatever their pick. With nothing installed, none of this runs. */
    uint32_t hook_pick = (r->speed & VJO_GAME_OCR_S_NOCACHE) || (r->flags & VJO_GAME_OCR_F_FORCE_FALLBACK)
                             ? VJO_HOOK_PICK_OCR : r->hook_pick;
    /* API 43: trusted hooks answer without OCR (the automatic pick only) */
    int trust = (r->speed & VJO_GAME_OCR_S_TRUST) && hook_pick == VJO_HOOK_PICK_AUTO;
    if (hook_pick == VJO_HOOK_PICK_AUTO) hk.trust_on = trust; /* API 49: the watch's (a diagnostic job's is not) */
    if (hk.installed) {
        if (hook_pick && hook_pick <= VJO_HOOK_STREAMS) vjo_hooks_keep(hook_pick, 0); /* pinned: keeps its CALL slot */
        hooks_sc3_load(); /* an SC3 site's charset, before its text is read */
        if (!hooks_snapshot(hook_pick, trust)) {
            int attempted = hk.cache_attempted;
            hooks_cache_prepare(hook_pick);
            /* A newly restored identity may need a different stream kept in the bounded pool. */
            if (!attempted && hk.sel.interval) (void)hooks_snapshot(hook_pick, trust);
        }
        hk.saved = hk.sel; /* a failed request preserves the restored provisional identity */
        uint32_t hook_id = trust ? vjo_hook_select_trusted(&hk.sel, hook_pick, hk.cand, hk.n, &hk.flags) : 0;
        if (!hook_id) hook_id = vjo_hook_select_before(&hk.sel, hook_pick, hk.cand, hk.n, &hk.flags);
        if (hook_id) {
            if (hk.flags & VJO_HOOK_R_TRUSTED) {
                /* A trusted hook supplies the text: no OCR reserve for its one pass, and once it has answered
                 * TRUST_DROP_ANSWERS jobs in a row the game gets back the weights, LM and cascade model an earlier
                 * job left resident (the next OCR job reads them again). */
                hk.trusted = 1;
                job_reserve = VJO_GAME_RESERVE_HOOK_ONLY;
                if (hk.trusted_run < TRUST_DROP_ANSWERS) hk.trusted_run++;
                /* A give-back that fails (a block this worker cannot free: nearly unreachable) costs no line, and is
                 * not tried again until a job runs OCR (audit-rubi-ocr-skip N1) */
                if (hk.trusted_run == TRUST_DROP_ANSWERS && drop_resident()) hk.trusted_run = TRUST_DROP_ANSWERS + 1u;
            }
            rc = (hk.flags & VJO_HOOK_R_TRUSTED) && !hook_publish(r) ? VJO_OK : run_hook_pass(r, out, &capture, &stats);
            if (rc < -199) rc = VJO_E_SOURCE; /* a native capture error, as for OCR */
            if (capture.failed)
                step_add(VJO_GSTEP_PASS, capture.mode, capture.pass_strips, capture.failed, capture.pass_bytes,
                         capture.last.fb_format, capture.pass_us, 0);
            if (!rc) {
                hook_show(out, hook_cand(hook_id), 0, norm_flags(r));
                hk.from = hook_id;
            }
            goto done;
        }
    }
    hk.trusted_run = 0; /* API 43: this job runs OCR: the trusted answers' run starts again */
    /* #385's audit F6: on reads a region at least as tall as wide as columns from its first try, as API 47's on did:
     * its rule takes none of the 2,659 such regions of vertical text measured for rows (nor the 2 such hooked
     * horizontal boxes), and the rows' try first made a drawn box of columns slower to read than before API 53 */
    if (check == VJO_VOCR_CHECK_ON && r->height >= r->width) { vertical = 1; check = 0; }
    /* API 47, auto: the job again from here, as columns (its rows looked like vertical text), or as rows (the
     * columns' model could not be read or fit): each try its own check and blocks, the last one's result. */
again:
    try_steps = out->n_steps;
    paused_us += pz.us; /* an earlier try's copy (API 47's auto: rows, then columns) */
    sceClibMemset(&pz, 0, sizeof(pz));
    /* each try's own step down (API 47: auto's last try's, which the Shell names the model by) */
    out->fallback = 0;
    out->model_need_workspace = out->model_need_weights = out->model_release_bytes = 0;
    /* The finder's boxes refined (API 20): the mode, here or in a PSP game; never columns (API 47). */
    unsigned refine = !vertical && (refine_mode == VJO_GAME_OCR_REFINE_ON ||
                                    (refine_mode == VJO_GAME_OCR_REFINE_PSP && psp_title))
                      ? VJO_VOCR_REFINE_ALL : 0u;
    /* API 54: the text spotter as the finder (ocr_finder = spotter, VJO_GAME_OCR_S_SPOT): its weights resident and
     * checked before the memory check, as their parsed form sizes its part of the job's scratch (vjo_vocr_spot_bytes).
     * A job that cannot have them (missing, wrong, refused, no memory) reads with GRAD and gives the block back; so
     * does a job without the spotter. Its boxes are refined by their own setting (VJO_GAME_OCR_S_SPOT_REFINE), GRAD's
     * by refine_mode. With ocr_vertical = on there is no spotter: on's job is GRAD's rows and on's check (#379's
     * measured rule; audit-vocr6-spotter2-rubi F1). A try of columns reads without it: it keeps a verified block that
     * fits beside it, for the next job of rows, and takes no part of the scratch or refinement for it (F2). */
    int want_spot = (r->speed & VJO_GAME_OCR_S_SPOT) && vmode != VJO_GAME_OCR_VERTICAL_ON, spot = 0, spot_rc = 0;
    unsigned spot_refine = want_spot && (r->speed & VJO_GAME_OCR_S_SPOT_REFINE) ? VJO_VOCR_REFINE_ALL : 0u;
    size_t spot_bytes = 0;
    if (!want_spot && drop_one(&spot_res)) { rc = VJO_E_OCR_UNAVAILABLE; goto done; } /* a no-op: it holds nothing */
    /* API 26: the gate's block is kept while the jobs ask for the gate (#418) */
    if (!(r->speed & VJO_GAME_OCR_S_GATE) && drop_one(&gate_res)) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if (want_spot && !vertical) {
        VjoPlatform spot_platform = {.file_open = file_open, .now_us = now_us};
        VjoGreySource spot_cancel = {&capture, r->width, r->height, strip_source, cancelled, line_buffer, NULL};
        uint64_t s0 = now_wide();
        unsigned read = 0;
        spot_rc = vjo_vocr_spot_load(&spot_platform, r->model_dir, &spot_res, &spot_cancel, &read);
        spot_read |= read;
        spot_load_us += (uint32_t)(now_wide() - s0);
        if (spot_rc == VJO_E_CANCELLED || spot_rc == VJO_E_OCR_UNAVAILABLE) { rc = spot_rc; goto done; }
        if (!spot_rc && !(spot_bytes = vjo_vocr_spot_bytes(&spot_res, r->width, r->height, r->fb_width, r->fb_height)))
            spot_rc = VJO_E_OOM; /* an arena over VJO_VOCR_SPOT_ARENA_MAX: never this pin's */
        spot = !spot_rc;
        if (!spot && drop_one(&spot_res)) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    } else if (want_spot) {
        spot = spot_res.verified && spot_blk.uid >= 0; /* a try of columns: kept if it fits (below), never read */
    }
    const int spot_rows = spot && !vertical; /* the spotter finds this try's lines */
    size_t refine_bytes = refine || (spot_rows && spot_refine) ? vjo_vocr_refine_bytes(r->width, r->height) : 0;
    size_t capture_bytes = vjo_vocr_capture_bytes(r->width, r->height);
    /* The models this job may run, in the order it tries them (API 21): a request
     * by tiers names its tiers, the largest first; one by name its model, then its
     * fallback, used only if it is another pinned model needing less memory. A
     * job of columns (API 47) runs the vertical model alone, whatever its tiers, with
     * a request by name's reserve (GAME_RESERVE: 1 MiB, as every size's since PR #280;
     * VOCR5-S's shape had the Small size's 4 MiB before). */
    Candidate cand[CANDIDATES];
    unsigned n = 0, top = 0;
    tiered = r->tiers != 0 && !vertical;
    if (vertical) {
        cand[n++] = (Candidate){vjo_vocr_model(VJO_VOCR_VERTICAL_NAME), 0, VJO_GAME_OCR_OPEN_VERTICAL, 0};
    } else if (tiered) {
        for (unsigned t = VJO_VOCR_TIERS; t >= 1u; t--)
            if (r->tiers & (1u << (t - 1u))) {
                if (!top) top = t;
                cand[n++] = (Candidate){vjo_vocr_model(tier_names[t - 1u]), t, VJO_GAME_OCR_OPEN_TIER + t - 1u, 0};
            }
    } else {
        cand[n++] = (Candidate){vjo_vocr_model(r->model), 0, VJO_GAME_OCR_OPEN_MODEL, 0};
        if (r->fallback[0])
            cand[n++] = (Candidate){vjo_vocr_model(r->fallback), 0, VJO_GAME_OCR_OPEN_FALLBACK, 0};
    }
    for (unsigned i = 0; i < n; i++) {
        cand[i].bytes = cand[i].m ? vjo_vocr_job_bytes(cand[i].m->name, r->width, r->height) : 0;
        if (cand[i].bytes) cand[i].bytes += refine_bytes;
        else if (tiered) { rc = VJO_E_OCR_MODEL; goto done; } /* a tier this build's runtime does not pin */
    }
    if (!cand[0].bytes || !capture_bytes) { rc = cand[0].m ? VJO_E_OCR_REGION : VJO_E_OCR_MODEL; goto done; }
    if (n == 2 && !tiered &&
        (!cand[1].bytes || cand[1].m == cand[0].m ||
         PAGE_ROUND(cand[1].bytes) + PAGE_ROUND(cand[1].m->bytes) >= PAGE_ROUND(cand[0].bytes) + PAGE_ROUND(cand[0].m->bytes)))
        n = 1;
    /* The self-test runs the fallback on purpose (its smallest-size stage): the model's
     * check, for the record, then the smallest one's. */
    int forced = (r->flags & VJO_GAME_OCR_F_FORCE_FALLBACK) && n > 1;
    if (forced) { cand[1] = cand[n - 1u]; n = 2; }
    /* Weights of a model this job cannot use: useless now, and two are never held. Auto's job of rows keeps
     * the vertical model's, which its columns use if its rows look like vertical text (the core reads no
     * weights before that check, and drops them before it reads the rows' model). */
    if (resident.model && !candidate_of(cand, n, resident.model) &&
        !(check && resident.model == vjo_vocr_model(VJO_VOCR_VERTICAL_NAME)) && drop_one(&resident)) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    /* The LM: the largest tier the request allows that fits (API 20); none
     * allowed, and an LM still resident from an earlier setting goes. */
    unsigned lm_max = (r->flags >> VJO_GAME_OCR_LM_SHIFT) & VJO_GAME_OCR_LM_MASK;
    const VjoVocrLm *lm = NULL;
    if (lm_max > VJO_VOCR_LM_TIERS) lm_max = VJO_VOCR_LM_TIERS;
    if (!lm_max && (lm_res.data || lm_blk.uid >= 0) && drop_one(&lm_res)) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* The confidence cascade (API 20): the request's time ceiling, and a larger
     * model beside the job's (cascade_of); none asked for, and a resident one goes. */
    uint32_t casc_us = vertical ? 0u /* no larger model reads columns */
                                : ((r->flags >> VJO_GAME_OCR_CASCADE_SHIFT) & VJO_GAME_OCR_CASCADE_MASK) *
                                      VJO_GAME_OCR_CASCADE_UNIT_US;
    const VjoVocrModel *casc = NULL;
    if (!casc_us && (cascade_res.data || cascade_blk.uid >= 0) && drop_one(&cascade_res)) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    /* Reported also when refused: each title's real free memory. Each model is
     * checked in turn, its cascade and LM first (check_with_lm); the first that
     * fits runs. The first one's need stays in model_need_* when another runs. */
    unsigned used = 0, try_from = 0;
    int fits = 0;
    uint32_t first_workspace = 0, first_weights = 0, first_release = 0;
retry_smaller:
    out->free_user = free_user();
    for (unsigned i = try_from; i < n; i++) {
        casc = cascade_of(&cand[i], r, top, casc_us);
        fits = check_with_lm(out, cand[i].m, cand[i].bytes, capture_bytes, lm_max, cand[i].tier, &casc, &lm);
        /* Each check is a step (include/vjo_game_ocr.h VJO_GSTEP_CHECK). A smaller
         * model's (arg 1) is the record that it runs when it passed; for a request
         * by name its sum says whether the self-test forced it (no FALLBACK step,
         * so the first job of a PSP game, the refinement's step besides the LM's
         * and the cascade's, fits the log); by tiers its kind names the tier and
         * its sum the own blocks counted as free. */
        step_add(VJO_GSTEP_CHECK,
                 (lm ? lm->tier : 0u) | (casc ? VJO_GCHECK_CASCADE : 0u) |
                     (tiered ? cand[i].tier << 4 | (forced && i ? VJO_GCHECK_FORCED : 0u) : 0u),
                 i ? 1u : 0u, fits ? 0 : 1, out->need_workspace + out->need_weights,
                 tiered ? out->release_bytes : forced && i ? 1u : 0u, 0, 0);
        used = i;
        if (!i) {
            first_workspace = out->need_workspace;
            first_weights = out->need_weights;
            first_release = out->release_bytes;
        }
        if (fits && !(forced && !i)) break; /* the model's check passed, or a smaller one runs */
    }
    if (used) {
        /* The first model did not fit this game now (or the self-test skipped it); a smaller one runs. */
        out->fallback = tiered ? top - cand[used].tier : 1u;
        out->model_need_workspace = first_workspace;
        out->model_need_weights = first_weights;
        out->model_release_bytes = first_release;
    }
    const VjoVocrModel *use = cand[used].m;
    size_t use_bytes = cand[used].bytes;
    job_reserve = out->reserve_bytes; /* the blocks the job allocates keep its reserve too */
    /* API 54: the spotter's part of the scratch once the check passed, in the room left beside the tier, its LM and
     * the cascade (the check counted the spotter's block as one the job gives back, so it never takes their room),
     * before the helpers'; without that room the job reads with GRAD and gives the block back. */
    if (spot && fits) {
        const uint32_t held = held_bytes(&spot_res);
        const size_t block = use_bytes + extra_job_bytes(use, lm, casc);
        size_t with = vjo_vocr_job_bytes_spot(use->name, r->width, r->height, spot_rows ? spot_bytes : 0) + refine_bytes;
        uint32_t grow = with > use_bytes ? PAGE_ROUND(block + (with - use_bytes)) - PAGE_ROUND(block) : 0u;
        if (with > refine_bytes && out->release_bytes >= held &&
            (int64_t)out->free_user + out->release_bytes - held - (int64_t)out->reserve_bytes >=
                (int64_t)out->need_workspace + out->need_weights + grow) {
            use_bytes = with > use_bytes ? with : use_bytes;
            out->need_workspace += grow;
            out->release_bytes -= held; /* kept: not given back */
        } else {
            spot = 0;
            if (spot_rows) spot_rc = VJO_E_OOM; /* a try of columns gives the block back without a refusal */
        }
    }
    if (want_spot && !spot && drop_one(&spot_res)) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* Helpers (ocr_threads, API 22) only when they fit beside everything else the
     * check chose (the tier's model, its LM, the cascade), keeping the same reserve
     * as that check (out->reserve_bytes: 1 MiB for every tier since PR #280): one
     * fewer at a time, down to none, which is the serial job, so helpers never make a
     * job step down a tier. Their line slots end the workspace block. */
    unsigned want_helpers = r->speed & VJO_GAME_OCR_S_HELPERS_MASK;
    unsigned use_helpers = 0;
    size_t helper_bytes = 0, helpers_need_bytes = 0;
    if (want_helpers > VJO_VOCR_HELPERS_MAX) want_helpers = VJO_VOCR_HELPERS_MAX;
    /* API 55, the pause (ocr_pause, docs/game-pause.md): its frame block, the region's grey rows, when it fits beside
     * everything the check chose, with that check's reserve, and without the resident blocks the check counted as
     * released (so it never makes the job give back another model's weights); asked for before the helpers, so a game
     * short of memory may read its lines one after the other for it, never with a smaller model, LM or cascade. */
    uint32_t frame_need = 0;
    /* never in the PSP emulator's process (#355's review H2): its frames come from its pad calls, with no display
     * call to wait in (the kernel refuses the copy: VJO_GAME_STATIC_FB), so the pause is a no-op there: no block, no
     * copy, no step, the job read as without it */
    if (fits && (r->speed & VJO_GAME_OCR_S_PAUSE) && !psp_title) {
        pz.asked = 1;
        frame_need = pause_capped ? 0 : PAGE_ROUND((uint32_t)r->width * r->height);
        if (pause_capped) {
            pz.rc = VJO_ERR_TIMEOUT; /* an earlier copy reached the cap: VJO_GPAUSE_CAPPED */
        } else if ((int64_t)out->free_user - (int64_t)out->reserve_bytes >=
                   (int64_t)out->need_workspace + out->need_weights + frame_need) {
            out->need_workspace += frame_need;
        } else {
            frame_need = 0;
            pz.rc = VJO_E_OOM;
        }
    }
    if (fits) {
        const size_t block = use_bytes + extra_job_bytes(use, lm, casc);
        /* free memory beside everything the job takes so far (its frame block too), and what releasing another
         * model's blocks would add */
        const int64_t room = (int64_t)out->free_user - (int64_t)out->reserve_bytes - (int64_t)out->need_workspace -
                             (int64_t)out->need_weights;
        int64_t release = (int64_t)out->release_bytes;
        /* API 55 (the audit's F1): the frame block never makes the job give back another model's blocks. They count
         * for the helpers only where, without the frame block, its helpers would need them too: the frame block may
         * cost a helper instead (the release below then finds the job fits beside them) */
        if (frame_need) {
            const unsigned h = helpers_fit(use->name, block, want_helpers, room + (int64_t)frame_need + release);
            if (!h || room + (int64_t)frame_need >= (int64_t)helpers_need(use->name, block, h)) release = 0;
        }
        use_helpers = helpers_fit(use->name, block, want_helpers, room + release);
        if (use_helpers) {
            helper_bytes = vjo_vocr_helpers_bytes(use->name, use_helpers);
            helpers_need_bytes = helpers_need(use->name, block, use_helpers);
            out->need_workspace += (uint32_t)helpers_need_bytes;
        }
    }
    stats.workspace = use_bytes;
    if (!fits && vertical) { /* no room for its columns (auto; on since API 53): its rows */
        step_add(VJO_GSTEP_FIND, 5, 0, VJO_E_OOM, 0, 0, 0, 0);
        vertical = 0;
        goto again;
    }
    if (!fits) {
        /* Too little memory: give the game back the resident weights and LM too. */
        if (drop_resident()) rc = VJO_E_OCR_UNAVAILABLE;
        else rc = VJO_E_OOM;
        /* No stream can ever agree with OCR in this game now: a Rubi verified site's newest text answers,
         * unconfirmed (core/hook_select.h vjo_hook_select_no_memory), with its capture pass beside the
         * same VJO_GAME_RESERVE_HOOK_ONLY. A pass that fails keeps the refusal. */
        if (rc == VJO_E_OOM && hk.installed) {
            uint32_t no_mem_flags = 0, id = vjo_hook_select_no_memory(hook_pick, hk.cand, hk.n, &no_mem_flags);
            if (id && no_memory_answer(r, out, &capture, &stats, id, no_mem_flags)) rc = VJO_OK;
        }
        goto done;
    }
    /* The check counted another model's weights, or another LM (or one this job
     * has no room for), as free: release them. Another model's weights stay
     * while the job fits beside them with its own reserve (out->reserve_bytes: its
     * size's from API 21, 1 MiB for every size since PR #280), until it reads its own (never two: the core drops
     * them first), so a job that reads no line keeps them: the self-test's
     * fallback stage on a screen without text keeps the user's model
     * (docs/first-ocr.md). */
    if (resident.model && resident.model != use &&
        (int64_t)out->free_user - (int64_t)out->reserve_bytes < (int64_t)out->need_workspace + out->need_weights &&
        drop_one(&resident)) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if ((lm_res.data || lm_blk.uid >= 0) && lm_res.lm != lm && drop_one(&lm_res)) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    if ((cascade_res.data || cascade_blk.uid >= 0) && cascade_res.model != casc && drop_one(&cascade_res)) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    size_t lm_extra = extra_job_bytes(use, lm, casc);
    /* the core's extras end where the greedy text kept for the log begins */
    size_t core_extra = lm_extra ? lm_extra - VJO_GAME_OCR_TEXT_BYTES : 0;
    void *capture_base = NULL, *base = NULL;
    SceUID leaked;
    uint32_t held, mem;
    capture_uid = game_memblock_alloc(GL_CAPTURE, VJO_GITEM_CAPTURE, "VjoGameVocrCapture", (uint32_t)capture_bytes,
                                      LADDER_ALL, &capture_base, &leaked, &held, &mem);
    if (capture_uid < 0) { capture_uid = leaked; rc = VJO_E_OOM; goto done; }
    capture_bytes_held = held;
    capture_memory = mem;
    workspace_uid = game_memblock_alloc(GL_WORKSPACE, VJO_GITEM_WORKSPACE, "VjoGameVocr",
                                        PAGE_ROUND(use_bytes + lm_extra + helper_bytes), LADDER_ALL, &base, &leaked, &held,
                                        &mem);
    if (workspace_uid < 0) { workspace_uid = leaked; rc = VJO_E_OOM; goto done; }
    workspace_bytes_held = held;
    workspace_memory = mem;
    if (frame_need) { /* API 55: refused, the job reads as without the pause */
        void *fb = NULL;
        SceUID fu;
        steps_quiet = 1;
        fu = game_memblock_alloc(GL_FRAME, VJO_GITEM_FRAME, "VjoGameVocrFrame", (uint32_t)r->width * r->height,
                                 LADDER_ALL, &fb, &leaked, &held, &mem);
        steps_quiet = 0;
        if (fu < 0) {
            frame_uid = leaked; /* a block whose base could not be read and that could not be freed: cleanup's */
            pz.rc = fu < -1 ? fu : VJO_E_OOM; /* the last rung's native error */
        } else {
            frame_uid = fu;
            frame_bytes_held = pz.bytes = held;
            pz.mem = mem;
            frame_base = fb;
            pz.held = 1;
        }
    }
    VjoPlatform platform = {.file_open=file_open, .now_us=now_us};
    VjoGreySource image = {&capture, r->width, r->height, strip_source, cancelled, line_buffer, frozen_region};
    VjoVocrPolicy policy;
    request_policy(r, &policy);
    if (retried) {
        /* API 47, auto's next try: the same job, so the same stable-text wait, as the core's own tries share it
         * (docs/text-stability.md): at most ocr_settle_max_ms over all of them (a press's first job has none).
         * The try before has just read the region (and waited first where the request asked): this one reads it
         * at once, its first pass the job's capture, and recognizes once the rows have held still settle_us,
         * within what is left of the wait (1 us: none, it reads anyway). */
        const uint32_t max = policy.settle_max_us ? policy.settle_max_us : VJO_VOCR_SETTLE_MAX_US;
        policy.settle_first = 0;
        if (policy.settle_us) policy.settle_max_us = tried_settle_us < max ? max - tried_settle_us : 1u;
    }
    if (lm_extra) { /* the LM (and cascade) extras, the helpers' line slots and then the greedy text */
        policy.raw = (char *)base + use_bytes + core_extra + helper_bytes;
        policy.raw_cap = VJO_GAME_OCR_TEXT_BYTES;
    }
    if (lm) {
        policy.lm = lm;
        policy.lm_res = &lm_res;
    }
    if (casc) {
        policy.cascade = casc;
        policy.cascade_res = &cascade_res;
        policy.cascade_us = casc_us;
        policy.cascade_conf = VJO_GAME_CASCADE_CONF;
    }
    policy.refine = refine;
    policy.vertical_check = check; /* API 47, auto (API 53, on: VJO_VOCR_CHECK_ON): the rows checked first */
    policy.short_lines = 1; /* #194: a box's short last line */
    if (spot_rows) { /* API 54: the spotter finds the lines; the region is cut from the game's frame, its scale's */
        policy.spot = 1;
        policy.spot_res = &spot_res;
        policy.spot_refine = spot_refine;
        policy.spot_frame_w = r->fb_width;
        policy.spot_frame_h = r->fb_height;
    }
    /* vita-vn-ocr #109: a numeral or a 「 the line lost before a counter (ルッ月 -> 九ヶ月 with
     * core/textnorm.c's rule), from the top-K the LM extras hold; nothing without them */
    policy.counter_tau = VJO_VOCR_COUNTER_TAU;
    policy.counter_open = VJO_VOCR_COUNTER_OPEN;
    /* The cache's entries are those of jobs with this LM (bits 0-2), cascade (bit 3),
     * cascade ceiling (ms, bits 4-15: at most 3,750), cascade model (since API 21 the
     * next tier up of the job's, so a Small job's is Normal's model and a Normal one's Large's: its
     * index among the pinned models + 1, bits 16-23), refinement (bits 24-28), the counter
     * re-decode (bit 29), the head gate (bit 30) and the two-level head (bit 31; API 42: a line either read can
     * differ in rare frames). */
    unsigned casc_index = 0;
    if (casc) {
        unsigned count = 0;
        const VjoVocrModel *all = vjo_vocr_models(&count);
        for (unsigned k = 0; k < count && k < 0xFFu; k++)
            if (&all[k] == casc) casc_index = k + 1u;
    }
    policy.cache_variant = (lm ? lm->tier : 0u) | (casc ? 8u : 0u) | (uint32_t)(casc_us / 1000u) << 4 |
                           (uint32_t)casc_index << 16 | (spot_rows ? spot_refine : refine) << 24 |
                           (policy.counter_tau || policy.counter_open ? 1u << 29 : 0u) |
                           (policy.head_gate ? 1u << 30 : 0u) | (policy.two_level ? 1u << 31 : 0u);
    helper_pool.helpers = use_helpers;
    helper_creates = helper_create_failures = 0;
    open_which = cand[used].which;
    open_cascade = casc != NULL;
    job_model = use; /* #385's audit F5: the blocks the core may still take (line_buffer) */
    job_lm = lm;
    job_to_come = out->need_weights + use_helpers * VJO_BUDGET_GAME_HELPER_STACK; /* #418: before them, the gate's */
    rc = vjo_vocr_ocr_policy(&platform, r->model_dir, use->name, &image, &resident, base,
                             use_bytes + core_extra + helper_bytes,
                             capture_base, capture_bytes_held, &policy, out->text, sizeof(out->text), &stats);
    job_model = NULL;
    job_lm = NULL;
    /* #324: the check passed, yet the model's weights could not be had: out of memory before any were read or
     * held (every job-level VJO_E_OOM comes before them: core/vocr_ocr.c's weights block, the finder's arena, the
     * workspace or the capture; an LM's or the cascade's only sets lm_rc / cascade_rc). resident_alloc checks the
     * game's free memory again once this job's own blocks are taken, and the console reports that memory in whole
     * MiB (every sample of あおかな, PCSG00700, 2026-10-26: 1, 2, 3 or 4 MiB), so the second look can see a whole MiB
     * less than the check did: a job that passed with less than 1 MiB to spare finds its weights refused, whatever
     * its tier's reserve. A smaller model may still fit: give back this job's blocks and check again from the next
     * one (retry_smaller), each at most once, as a model that did not fit the check would have been. A job of
     * columns has one model: its rows instead (API 53, #379, below). */
    const int smaller = rc == VJO_E_OOM && !stats.stuck && !forced && used + 1u < n && !cancelled(NULL);
    /* API 47, auto: the rows looked like vertical text (nothing was read: no weights, no line); API 53, on (#379):
     * the rows did not look like horizontal text (the same); or the columns' model cannot be read here (absent,
     * short, failing its hash: this game's jobs read rows from now on), or, its check passed, its weights could not
     * be had (auto's and on's columns: the rows, whose sizes check again). The same request again, as columns or as
     * rows (again). Either retry is the same job (#324's audit): after this try's blocks are freed, its capture,
     * finder time and stable-text wait are the job's too, the next try waits only for what is left of it (retried),
     * the freed blocks' steps leave the log (steps_drop_blocks; a try of rows whose rows went to the columns, its
     * memory checks too: #385's audit F7), and the capture is kept: its PASS and SIG steps are
     * logged once, its source mode stays. A try reached so checks nothing: at most rows, columns, rows. */
    if (smaller || (!stats.stuck && ((rc == VJO_E_VERTICAL && policy.vertical_check) || (rc == VJO_E_OCR_MODEL && vertical) ||
                                     (rc == VJO_E_OOM && vertical && !cancelled(NULL))))) {
        if (smaller) {
            try_from = used + 1u;
        } else if (rc == VJO_E_VERTICAL) {
            step_add(VJO_GSTEP_FIND, 2, stats.lines, 0, 0, stats.dark, stats.find_us, 0);
            /* API 54: why the spotter left the rows to auto's check, for the columns' try's spot step */
            if (want_spot) spot_handed = spot_rc ? spot_rc : stats.spot_rc; /* VJO_E_VERTICAL, or its refusal */
        } else if (rc == VJO_E_OOM) {
            step_add(VJO_GSTEP_FIND, 5, stats.lines, rc, 0, 0, stats.find_us, 0);
        } else {
            step_add(VJO_GSTEP_FIND, 3, stats.lines, io_error, 0, 0, stats.find_us, 0);
            if (!io_error || io_error == VJO_ERR_FORMAT || (uint32_t)io_error == 0x80010002u)
                columns_bad = 1; /* the file is absent, the wrong length or fails its hash (the Shell's file_bad);
                                  * a timeout or busy storage is this job's only */
            io_error = stat_error = 0; /* the rows' try reports its own model's I/O, as a job of rows does */
        }
        tried_strips += stats.strips;
        tried_us += stats.capture_us;
        tried_find_us += stats.find_us;
        tried_settle_us += stats.settle_us;
        tried_settle_passes += stats.settle_passes;
        retried = 1;
        if (!smaller) { /* (a smaller model's try is of rows: a job of columns has one model) */
            vertical = rc == VJO_E_VERTICAL;
            check = 0;
        }
        sceClibMemset(&stats, 0, sizeof(stats));
        if (cleanup()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
        steps_drop_blocks(try_steps, vertical); /* a try of rows that hands over to the columns: its checks too */
        if (smaller) {
            /* API 55: a new check, so a new try of the pause: its own frame block (cleanup gave this one back) and
             * copy. The game waited for this try's copy too: the step's us sums them, as again: does for auto's. */
            paused_us += pz.us;
            sceClibMemset(&pz, 0, sizeof(pz));
            goto retry_smaller;
        }
        goto again;
    }
    /* #280's audit F1: the check passed, yet the last model's weights could not be had (no smaller model is left to
     * retry). With every size's reserve 1 MiB (#280), アマツツミ's measured 2 MiB (PCSG01198, 2026-10-23) passes Small's
     * check at the full screen (2,068,480 B), and on a console that reports free memory in whole MiB (#324) its weights
     * can then be refused once the job's blocks are taken (while less than 2 MiB and those blocks, 192 KiB at the full
     * screen, is really free; or the game took memory meanwhile): no OCR can run in this game now, as when the check
     * fails, so a Rubi verified site's newest text answers unconfirmed here too (with the 4 MiB reserve before #280 the
     * check failed there). This try's blocks and the resident ones go back to the game first, as on that path; its
     * captures and finder time stay the job's, and its blocks' steps leave the log (steps_drop_blocks). Never while a
     * block may still be written (a stuck helper, a capture the display hook may still own: blocked). */
    if (rc == VJO_E_OOM && !stats.stuck && !blocked && hk.installed && !cancelled(NULL)) {
        uint32_t no_mem_flags = 0, id = vjo_hook_select_no_memory(hook_pick, hk.cand, hk.n, &no_mem_flags);
        if (id) {
            const size_t workspace = stats.workspace;
            tried_strips += stats.strips;
            tried_us += stats.capture_us;
            tried_find_us += stats.find_us;
            sceClibMemset(&stats, 0, sizeof(stats));
            stats.workspace = workspace;
            pause_step_log(paused_us); /* API 55: this try's copy and the game's wait, before its block goes back */
            if (cleanup() || drop_resident()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
            steps_drop_blocks(try_steps, 0);
            if (no_memory_answer(r, out, &capture, &stats, id, no_mem_flags)) rc = VJO_OK;
            goto done;
        }
    }
    /* ocr-first: a job that read no line (or only blank ones) reads what the
     * session's first job with text would otherwise read while the user waits
     * (docs/first-ocr.md), as its check allowed; not the self-test's forced
     * fallback, which would put the fallback in the user's model's place, nor (API 43) in a game whose trusted
     * hook supplies its lines: nothing is read ahead there. Best
     * effort, so its rc is not the job's: a cancellation ends the job below,
     * a block it could not release is cleanup()'s, a file it could not read the
     * next text job's (io_error says which). */
    if (!rc && !stats.model_loaded && !stats.model_cached && !forced && !(trust && hk.trusted) &&
        !(check && resident.model == vjo_vocr_model(VJO_VOCR_VERTICAL_NAME))) /* auto keeps the columns' model */
        vjo_vocr_warm(&platform, r->model_dir, use->name, &image, &resident, &policy, &stats);
    /* A native capture error (the kernel's VJO_ERR_*, an SDK code) ends the job
     * as a source failure; its exact code is in the step log (VJO_GSTEP_PASS). */
    if (rc < -199) rc = VJO_E_SOURCE;
    if (stats.tears) /* bytes: when the last try began (us since the job's start), its tears before it */
        step_add(VJO_GSTEP_TEAR, 0, stats.tears, (int32_t)stats.tries,
                 stats.tries > 1 ? stats.retry_at - (uint32_t)job_started_us : 0u, 0, 0, 0);
    if (capture.failed) step_add(VJO_GSTEP_PASS, capture.mode, capture.pass_strips, capture.failed, capture.pass_bytes,
                                 capture.last.fb_format, capture.pass_us, 0);
    /* kind: 1 columns; API 53, 4: on's rows, which its check found horizontal (the log's line for on's rows) */
    step_add(VJO_GSTEP_FIND, stats.columns ? 1u : policy.vertical_check == VJO_VOCR_CHECK_ON ? 4u : 0u, stats.lines,
             rc == VJO_E_OCR_REGION ? rc : 0, stats.rotate_skipped, stats.dark, stats.find_us, 0);
    if (policy.settle_us)
        step_add(VJO_GSTEP_SETTLE, 0, stats.settle_passes + tried_settle_passes, /* the job's whole wait (API 47) */
                 stats.settled ? VJO_GSETTLE_HELD : stats.settle_timeout ? VJO_GSETTLE_TIMEOUT : VJO_GSETTLE_NONE,
                 stats.settle_changes, (stats.cycling_rows > 0xFFFFu ? 0xFFFFu : stats.cycling_rows) << 16 |
                 (stats.cycles > 0xFFFFu ? 0xFFFFu : stats.cycles),
                 stats.settle_us + tried_settle_us ? stats.settle_us + tried_settle_us : 1u, 0);
    pause_step_log(paused_us); /* API 55 */
    for (unsigned i = 0; i < stats.blank && i < VJO_VOCR_BLANK_LOGGED; i++) {
        const VjoVocrBlankStat *b = &stats.blank_line[i];
        step_add(VJO_GSTEP_BLANK, 0, b->index, (int32_t)stats.blank, (uint32_t)b->width << 16 | b->height,
                 (uint32_t)(b->ink.hi - b->ink.lo) << 16 | b->ink.edges, stats.blank_us ? stats.blank_us : 1u, 0);
    }
    if ((stats.spot_ran ? spot_refine : refine) && stats.lines) /* each count at most VJO_VOCR_MAX_LINES: one try's */
        step_add(VJO_GSTEP_REFINE, stats.spot_ran ? VJO_GAME_OCR_REFINE_ON : refine_mode, stats.refine_lines,
                 stats.refine_rc,
                 stats.refine_boxes << 16 | (stats.refine_capped & 0xFFu) << 8 | (stats.refine_split & 0xFFu),
                 (uint32_t)stats.refine_rows, stats.refine_us + 1u, 0);
    if (stats.model_loaded || stats.model_cached) /* arg 1: read by the warm-up */
        step_add(VJO_GSTEP_LOAD, stats.model_loaded ? VJO_GAME_OCR_WEIGHTS_READ : VJO_GAME_OCR_WEIGHTS_RESIDENT,
                 stats.warmed, 0, held_bytes(&resident), 0, stats.load_us, 1);
    if (stats.lines)
        step_add(VJO_GSTEP_RECOGNIZE, head_gated_percent(&stats), stats.recognized, rc, stats.frames,
                 stats.tall_skipped | stats.cache_long << 8, stats.recognize_us, 0);
    if (lm && (stats.lines || stats.lm_loaded || (stats.warmed && stats.lm_rc))) { /* also the warm-up's read */
        unsigned low = 0;
        for (unsigned i = 0; i < stats.recognized && i < VJO_VOCR_MAX_LINES; i++)
            low += (stats.line[i].flags & VJO_VOCR_LINE_CONF) < VJO_GAME_OCR_LOW_CONF;
        step_add(VJO_GSTEP_LM, stats.lm ? stats.lm->tier : lm->tier, stats.lm_changed, stats.lm_rc,
                 (stats.lm_loaded ? VJO_GLM_READ : stats.lm_cached ? VJO_GLM_RESIDENT : 0u) | low << 16 | stats.lm_lines,
                 stats.lm_load_us, stats.lm_us + 1u, 0);
    }
    if (casc && (stats.lines || stats.cascade_loaded || (stats.warmed && stats.cascade_rc))) cascade_step(&stats);
    if (!rc) {
        /* What is shown and looked up is normalised; the recognizer's own (greedy)
         * text follows it in the same buffer, for the Shell's log. An empty text too:
         * this zeroes what an earlier try of the job left after its NUL (a try
         * that read a line, then a retry that found the box empty), which the
         * Shell would otherwise log as the recognizer's text. */
        VjoTextNorm k;
        uint64_t t0 = now_wide();
        unsigned changes = vjo_text_normalize_keep(out->text, sizeof(out->text), lm_extra ? policy.raw : NULL,
                                                   norm_flags(r), &k);
#define CLAMP8(v) ((v) > 255u ? 255u : (v))
        /* also a text that only dropped lines emptied (core/textnorm.h): the step says why it is empty */
        if (out->text[0] || changes)
            step_add(VJO_GSTEP_NORM, CLAMP8(k.garbage), changes, 0, k.width,
                     CLAMP8(k.tilde) << 24 | CLAMP8(k.dash) << 16 | CLAMP8(k.ellipsis) << 8 |
                     (k.long_vowel > 15u ? 15u : k.long_vowel) << 4 | (k.kanji_one > 15u ? 15u : k.kanji_one),
                     (uint32_t)(now_wide() - t0) + 1u, 0);
#undef CLAMP8
    }
    /* Native hooks: a stream whose text agrees with OCR's replaces it (core/hook_select.h). */
    if (!rc && hk.installed) {
        unsigned score = 0;
        /* #413's audit F2: read without the stable-text wait (a press since #413), its text may be a line typing out */
        uint32_t id = vjo_hook_select_after(&hk.sel, hook_pick, out->text, text_len(out->text, sizeof(out->text)),
                                            hk.cand, hk.n, &score, &hk.flags, !(r->flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu));
        hk.score = score;
        if (id) {
            hook_show(out, hook_cand(id), 1, norm_flags(r));
            hk.from = id;
        }
    }
    if (stats.lines && (want_helpers || stats.cached)) {
        /* the recognized lines' share of a core: the lowest and the mean (percent; 0 unknown) */
        uint32_t lo = 100u, sum = 0, n = 0;
        for (unsigned i = 0; i < stats.recognized && i < VJO_VOCR_MAX_LINES; i++) {
            const VjoVocrLineStat *ls = &stats.line[i];
            uint32_t share;
            if (ls->cached || !ls->us || !ls->cpu_us) continue;
            share = (uint32_t)((uint64_t)ls->cpu_us * 100u / ls->us);
            share = share > 100u ? 100u : share;
            lo = share < lo ? share : lo;
            sum += share;
            n++;
        }
        step_add(VJO_GSTEP_PAR, 0, stats.helpers, (int32_t)stats.dispatched, (uint32_t)helpers_need_bytes,
                 (stats.run_failures > 0xFFu ? 0xFFu : stats.run_failures) << 24 |
                 (stats.cached > 0xFFu ? 0xFFu : stats.cached) << 16 | (n ? lo : 0u) << 8 | (n ? sum / n : 0u),
                 stats.recognize_us ? stats.recognize_us : 1u, 0);
    }
    /* API 26: the text-presence gate, after every other step, so that a full log drops it first (a job it
     * stopped has few steps): score + 129, threshold + 129 << 16, VJO_GLM_READ when it read its weights. */
    if (policy.gate && (stats.gate_ran || stats.gate_rc))
        step_add(VJO_GSTEP_GATE, VJO_VOCR_GATE_MODE, stats.gate_stopped, stats.gate_rc,
                 (uint32_t)(stats.gate_score + 129) | (uint32_t)(VJO_VOCR_GATE_T + 129) << 16 |
                     (stats.gate_read ? VJO_GLM_READ : 0u), stats.gate_rows, stats.gate_us + 1u, 0);
    /* API 52 (#358): a job of rows that read the whole frame says where its dialogue is (core/region_suggest.h);
     * the Shell offers that box in a game with no region of its own. Its lines' region pixels are the frame's
     * here; host/page_ocr.c measures the same call (docs/first-box.md). */
    if (!rc && !r->x && !r->y && r->width == r->fb_width && r->height == r->fb_height)
        out->suggest_rows = vjo_vocr_suggest(&stats, r->fb_width, r->fb_height);
    /* API 54: the spotter's step, the last one (a full log drops it first): whether it found the lines or why GRAD
     * did, the boxes, its scratch and weights (read or kept), and its weights' load time in ms << 16 */
    if (want_spot) /* a try of columns: the rows' try's reason (spot_handed); a try of rows: its own */
        spot_step(&stats, vertical ? spot_handed : spot_rc, spot_read, spot_load_us);
    /* A helper that could not be joined may still run in the workspace: keep it,
     * and take no more jobs (core/vocr_ocr.h VjoVocrPool). */
    if (stats.stuck) { blocked = 1; hook_owns_workspace = 1; }
done:
    out->workspace_bytes = (uint32_t)stats.workspace;
    out->lines = stats.lines; out->find_us = stats.find_us + tried_find_us;
    out->load_us = stats.load_us; out->recognize_us = stats.recognize_us;
    out->weights = stats.model_loaded ? VJO_GAME_OCR_WEIGHTS_READ :
                   stats.model_cached ? VJO_GAME_OCR_WEIGHTS_RESIDENT : VJO_GAME_OCR_WEIGHTS_NONE;
    out->frames = stats.frames;
    out->recognized = stats.recognized < VJO_GAME_OCR_LINES ? stats.recognized : VJO_GAME_OCR_LINES;
    for (uint32_t i = 0; i < out->recognized; i++) {
        out->line[i].width = stats.line[i].width;
        out->line[i].height = stats.line[i].height;
        out->line[i].frames = stats.line[i].frames;
        out->line[i].flags = stats.line[i].flags;
        out->line[i].us = stats.line[i].us;
    }
    if (!(r && (r->flags & (VJO_GAME_OCR_F_PROBE | VJO_GAME_OCR_F_SHOT)))) { /* (theirs are their own) */
        out->source = capture.pass_strips ? capture.mode : VJO_GAME_OCR_SRC_NONE;
        out->strip_rows = stats.strip_rows;
        /* API 55: a paused job's captures are the game's frames it read (its rows calls, the frame copy one), not its
         * reads of the frame block */
        out->captures = pause_frames ? capture.pass_strips : stats.strips + tried_strips;
        out->tears = stats.tears;
        out->capture_us = stats.capture_us + tried_us;
        out->region_sum = stats.region_sum;
        if (capture.last.fb_width) {
            out->fb_width = capture.last.fb_width; out->fb_height = capture.last.fb_height;
            out->fb_pitch = capture.last.fb_pitch; out->fb_format = capture.last.fb_format;
        }
    }
    out->capture_memory = capture_memory;
    out->capture_bytes = capture_bytes_held;
    out->cleanup_status = cleanup();
    out->resident_bytes = held_bytes(&resident);
    out->io_error = io_error;
    out->stat_error = stat_error;
    if (out->cleanup_status) { blocked = 1; rc = VJO_E_OCR_UNAVAILABLE; }
    else if (cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (!rc) {
        hooks_cache_save(); /* only a completed capture/OCR with successful primary cleanup */
        out->cleanup_status |= cleanup(); /* drain an optional cache operation before publishing */
        if (out->cleanup_status) { blocked = 1; rc = VJO_E_OCR_UNAVAILABLE; }
        else if (cancelled(NULL)) rc = VJO_E_CANCELLED;
        out->io_error = io_error;
    }
    if (rc) { /* a failed result carries no text, and no box (API 52) */
        sceClibMemset(out->text, 0, sizeof(out->text));
        out->suggest_rows = 0;
    }
    hooks_result(out, rc);
    out->rc = rc; active_seq = 0;
    log_to = NULL;
    return rc;
}

/* A vjoOcrTake the kernel held for at least this long, waiting for a job
 * (kernel/game_ocr.c TAKE_WAIT_US, 100 ms), is followed by the next round at
 * once, whatever it answered (none came, or the game stopped being the active
 * one meanwhile: the next Register sees it). One that answers sooner (a job,
 * an error, a kernel that does not wait) is followed by the worker's own
 * 100 ms sleep, as before, so the loop never spins. */
#define TAKE_WAITED_US 50000u

/* ocr-first: the resident weights, LM and cascade model go back to a game that
 * is not the active one (another app in front of it, its title disabled, the
 * Shell restarted) once that has lasted INACTIVE_DROP_US of its running time; a
 * suspended game's time stops, so a suspend and resume keeps them. A moment's
 * change of foreground (a system dialog, the Shell placing the game again after
 * a resume) dropped them
 * and the next job read them all again: #81's run read H15 four times in one
 * game process (docs/first-ocr.md). 0 and BUSY are the active game's answers.
 * The process time stops while the process is suspended [SDK: Kernel-Reference,
 * sceKernelGetProcessTimeWide], so only time the game runs counts. */
#define INACTIVE_DROP_US 5000000u
static uint64_t inactive_since;

static int inactive_long(int registered)
{
    uint64_t now = now_wide();
    if (!registered || registered == VJO_ERR_BUSY) {
        inactive_since = 0;
        return 0;
    }
    if (!inactive_since) inactive_since = now;
    return now - inactive_since >= INACTIVE_DROP_US;
}

/* API 41, the hook watch (docs/native-hooks.md, "A new line starts the read"). Each round of the loop
 * between jobs (vjoOcrTake waits for one at most 100 ms) reads, without their locks, the sequences of the
 * streams a job answers without recognition: Luna's sites that have held text, and the learned source
 * (hk.sel.locked, while its slot still holds that source). One newer than every text the last job's
 * snapshot saw (hk.watch_seen) is a new line: the kernel gives the region a new scene and wakes the Shell
 * (vjoOcrHookLine), so its background read starts now rather than once the pixels settle. At most one
 * report every HOOK_LINE_GAP_US; a refused one is tried again at the next round.
 *
 * API 49 (docs/native-hooks.md, "No wait for a trusted line"): when the last user job asked for trusted hooks
 * (VJO_GAME_OCR_S_TRUST, hook_trust = on), a trusted source's line is reported at once, VJO_HOOK_LINE_TRUSTED:
 * a site that answers without recognition from its first line (no job need have seen it), and the learned
 * source once vjo_hook_select_trusted answers from it (a restored one by its identity, before a job has found
 * its slot). No gap: the same text again, decoded, from that stream or another watched one, is no new line, and
 * a write the decoder leaves empty is none either. While a trusted source is watched the loop polls every
 * HOOK_WATCH_US (vjoOcrTake's wait, the value hooks_watch returns) instead of 100 ms. */
#define HOOK_LINE_GAP_US 200000u
#ifndef HOOK_WATCH_US
#define HOOK_WATCH_US 16000u /* host/hook_latency_bench.c measures 4-33 ms; docs/native-hooks.md */
#endif
static uint32_t hooks_watch(void)
{
    uint32_t want = hk.luna_seen, best = 0, best_id = 0, bytes = 0, pace, h;
    /* lt: the learned source is trusted (vjo_hook_select_trusted's rule for it) */
    int trusted = 0, lt = hk.trust_on && hk.sel.interval >= VJO_HOOK_TRUST_INTERVAL && hk.sel.strong && !hk.sel.misses;
    uint64_t now;
    size_t len;
    if (hk.sel.interval && hk.sel.locked && hk.sel.locked <= VJO_HOOK_STREAMS)
        want |= VJO_HOOK_STREAM_BIT(hk.sel.locked);
    if (!hk.installed || !IMPORT_READY(vjoOcrHookLine) || (!want && !hk.trust_on)) return 0;
    for (uint32_t id = 1; id <= VJO_HOOK_STREAMS; id++) {
        uint32_t key = 0, kind = 0, n = 0, seq, bit = VJO_HOOK_STREAM_BIT(id);
        int own;
        if (!(want & bit) && !hk.trust_on) continue;
        seq = vjo_hooks_seq(id, &key, &kind, &n);
        if (!seq || !n) continue;
        /* API 49: a site from its first line, as hooks_snapshot counts them (Luna's; a Rubi verified one with trust) */
        if (hk.trust_on && kind == VJO_HOOK_KIND_SITE && !(hk.luna_seen & bit)) {
            if (vjo_hooks_verified(id)) hk.luna_seen |= bit;
            else if (!(hk.sites_known & bit) && vjo_hooks_snap(id, &hk.snap)) {
                hk.sites_known |= bit; /* its table entry never changes */
                if (hk.snap.site) hk.luna_seen |= bit;
            }
        }
        own = hk.sel.interval && key == hk.sel.locked_key && (kind == VJO_HOOK_KIND_SITE) == !!hk.sel.locked_site &&
              (hk.sel.locked ? hk.sel.locked == id : hk.trust_on);
        if (!(hk.luna_seen & bit) && !own) continue; /* another caller took the learned source's slot: its next job tells */
        if ((int32_t)(seq - hk.watch_seen) <= 0) continue;
        if (!best || (int32_t)(seq - best) > 0) {
            best = seq;
            best_id = id;
            bytes = n;
            trusted = (hk.trust_on && (hk.luna_seen & bit)) || lt;
        }
    }
    /* the fast pace from before a trusted source's first text: a game with table sites installed (its dialogue's), or
     * a trusted learned source (one restored from the cache included) */
    pace = (hk.trust_on && (hk.installed >> 16)) || lt ? HOOK_WATCH_US : 0u;
    if (!best) return pace;
    now = now_wide();
    if (trusted) {
        /* the line's text as a job would show it (hk.pool is free between jobs: each job's snapshot fills it); a
         * stream a game thread is writing is read at the next round */
        if (!vjo_hooks_snap(best_id, &hk.snap)) return pace;
        len = vjo_hooks_text(&hk.snap, hk.pool, HOOK_TEXT_CAP);
        h = vjo_fnv1a(VJO_FNV_INIT, (const unsigned char *)hk.pool, (unsigned)len);
        if (len && (h != hk.watch_hash || len != hk.watch_len)) {
            if (vjoOcrHookLine(best, bytes | VJO_HOOK_LINE_TRUSTED) < 0) return pace; /* a job in the way */
            hk.watch_hash = h;
            hk.watch_len = (uint32_t)len;
            hk.watch_sent = now;
        }
    } else {
        if (hk.watch_sent && now - hk.watch_sent < HOOK_LINE_GAP_US) return pace;
        if (vjoOcrHookLine(best, bytes) < 0) return pace; /* not the active game now, or a job in the way */
        hk.watch_sent = now;
    }
    hk.watch_seen = best;
    return pace;
}

static int worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint32_t pace = 0; /* API 49: the trusted watch's poll, 0 = the 100 ms one */
    while (!__atomic_load_n(&quitting, __ATOMIC_ACQUIRE) && !blocked) {
        int nap = 1;
        if (claim_outstanding) {
            if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
        } else {
            int registered = vjoOcrRegister(), gone = inactive_long(registered);
            if (!registered) {
                static VjoGameOcrRequest request; /* not on the 32 KiB stack */
                uint64_t asked;
                int took;
                /* API 41: a new hooked line, reported before waiting for the next job; API 49: the wait itself is
                 * HOOK_WATCH_US while a trusted source is watched (the kernel reads it from request.seq) */
                pace = hooks_watch();
                asked = now_wide();
                sceClibMemset(&request, 0, sizeof(request));
                request.size = sizeof(request);
                request.seq = pace;
                took = vjoOcrTake(&request);
                if (!took) {
                    claim_outstanding = 1;
                    vjo_game_ocr_run_request(&request, &pending_result);
                    if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
                    nap = claim_outstanding; /* done: the next job may be queued already */
                } else if (now_wide() - asked >= (pace ? pace / 2u : TAKE_WAITED_US)) {
                    nap = 0; /* the kernel held the take: its wait was the pace */
                }
            } else if (gone &&
                       (resident.data || weights_blk.uid >= 0 || lm_res.data || lm_blk.uid >= 0 || cascade_res.data ||
                        cascade_blk.uid >= 0 || spot_res.data || spot_blk.uid >= 0 || gate_res.data ||
                        gate_blk.uid >= 0) &&
                       drop_resident()) {
                /* Not the active game for INACTIVE_DROP_US of its running time (behind
                 * another app, its title disabled): the weights go back to it. */
                blocked = 1;
            }
        }
        if (nap) sceKernelDelayThread(claim_outstanding ? 50000 : 100000);
    }
    return 0;
}

int vjo_game_ocr_worker_start(void)
{
    if (worker >= 0 || blocked) return VJO_E_OCR_UNAVAILABLE;
    if (!IMPORT_READY(vjoGetVersion) || !IMPORT_READY(vjoOcrRegister) || !IMPORT_READY(vjoOcrTake) ||
        !IMPORT_READY(vjoOcrCancelled) || !IMPORT_READY(vjoOcrCaptureRows) || !IMPORT_READY(vjoOcrComplete) ||
        !IMPORT_READY(vjoOcrModelOpen) || !IMPORT_READY(vjoOcrModelRead) || !IMPORT_READY(vjoOcrModelClose) ||
        !IMPORT_READY(sceKernelAllocMemBlock) || !IMPORT_READY(sceKernelGetMemBlockBase) ||
        !IMPORT_READY(sceKernelFreeMemBlock) || vjoGetVersion() != VJO_API_VERSION) return VJO_E_OCR_UNAVAILABLE;
    __atomic_store_n(&quitting, 0, __ATOMIC_RELEASE);
    if (!g_game_ledger.defs) ledger_init();
    /* The scheduler is fixed-priority, not time-sharing: a recognition
     * (hundreds of ms per line) at the default priority would make a game
     * thread of the same priority wait for a core. At the lowest user
     * priority every game thread preempts this one [SDK: Kernel-Overview,
     * 'Notes on Priority and CPU Affinity Mask Combinations']. */
    worker = sceKernelCreateThread("VjoGameVocr", worker_main, WORKER_PRIORITY, WORKER_STACK_BYTES, 0,
                                   WORKER_AFFINITY, NULL);
    if (worker < 0) return VJO_E_OCR_UNAVAILABLE;
    vjo_ledger_add(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK);
    if (sceKernelStartThread(worker, 0, NULL) < 0) {
        if (sceKernelDeleteThread(worker) >= 0) { worker = -1; vjo_ledger_sub(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK); }
        return VJO_E_OCR_UNAVAILABLE;
    }
    worker_started = 1;
    return VJO_OK;
}

int vjo_game_ocr_worker_stop(void)
{
    __atomic_store_n(&quitting, 1, __ATOMIC_RELEASE);
    /* The worker first: a job in flight joins its own helpers (quitting cancels
     * their lines), and only the worker's thread touches helper_uid[] meanwhile. */
    if (worker >= 0) {
        SceUInt timeout = JOIN_US;
        if (worker_started && sceKernelWaitThreadEnd(worker, NULL, &timeout) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker_started = 0;
        if (sceKernelDeleteThread(worker) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker = -1;
        vjo_ledger_sub(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK);
    }
    /* Then a helper left by a job whose join failed: its code is this module's,
     * so the module stays until it has ended. Once every one has, nothing runs
     * in that job's workspace any more and cleanup() may free it. */
    for (unsigned k = 1; k <= VJO_VOCR_HELPERS_MAX; k++)
        if (helper_uid[k] >= 0 && pool_join(NULL, k) < 0) return VJO_E_OCR_UNAVAILABLE;
    if (hook_owns_workspace) {
        hook_owns_workspace = 0;
        vjo_vocr_unstick();
    }
    if (cleanup()) return VJO_E_OCR_UNAVAILABLE;
    /* The game is exiting or the plugin stops: no weights or LM stay behind, nor an SC3 charset (the worker's
     * thread, which alone reads it, has ended). */
    if (drop_resident()) return VJO_E_OCR_UNAVAILABLE;
    if (sc3_blk.uid >= 0) (void)vjo_hooks_sc3_set(NULL, 0);
    if (resident_release((void *)&sc3_desc)) return VJO_E_OCR_UNAVAILABLE;
    if (claim_outstanding) {
        pending_result.cleanup_status = 0; pending_result.rc = VJO_E_CANCELLED;
        sceClibMemset(pending_result.text, 0, sizeof(pending_result.text));
        if (vjoOcrComplete(&pending_result)) return VJO_E_OCR_UNAVAILABLE;
        claim_outstanding = 0;
    }
    blocked = 0;
    return VJO_OK;
}

#ifndef VJO_GAME_OCR_NO_MODULE_ENTRY
/* The boot guard's verdict for this boot (core/boot_guard.h): in a safe boot
 * the kernel has no hooks and nothing of ours may stay in any process. An
 * older kernel without the syscall means a normal boot. */
static int safe_boot(void)
{
    VjoBootInfo info = {.size=sizeof(info)};
    if (!IMPORT_READY(vjoBootInfo)) return 0;
    return vjoBootInfo(&info) == 0 && info.size == sizeof(info) && info.mode == VJO_BOOT_SAFE;
}

/* This process is Sony's PSP emulator, whatever its title ID (an Adrenaline
 * bubble's is its own): its main module ScePspemu is loaded, as
 * AdrBubbleBooter's adrbubblebooter.suprx checks (src/vita/adrbubblebooter/
 * main.c:57-60). taiHEN loads plugins after a process's modules. */
/* #138: the kernel loads the worker into an enabled game at its start
 * (kernel/loader.c), and for one release the game's own *TITLEID line, which
 * older installs wrote, loads it again right after (taiHEN runs a section's
 * lines once the kernel's hook has returned). A process keeps one: the copy that
 * starts when another is already loaded gives its pages back at once.
 *
 * A lookup by name finds the oldest module of that name: taiHEN walks the
 * process's module list from its end, the main module, the first loaded
 * (taiHEN module.c:253, taihen.h TAI_MAIN_MODULE; from the source, not seen on
 * a console). This copy is another one's younger twin when none of that
 * module's segments holds this copy's code. Nothing is compared across APIs,
 * and a lookup that fails or reports no segment answers "no other copy": both
 * copies then stay, and the kernel refuses the second one's thread
 * (kernel/game_ocr.c vjoOcrRegister). So this can never unload the only copy. */
static inline int outside_segments(const uintptr_t *base, const uint32_t *size, unsigned n, uintptr_t addr)
{
    int known = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!size[i]) continue;
        known = 1;
        if (addr - base[i] < size[i]) return 0; /* the oldest copy is this one */
    }
    return known;
}

#ifdef VJO_HOST
#define pspemu_process vjo_test_pspemu_process /* host/tests/game_stubs/psp2/kernel/modulemgr.h */
#define another_copy vjo_test_another_copy
#define vjoGetTextSource vjo_test_get_text_source
#else
static int pspemu_process(void)
{
    tai_module_info_t info;
    info.size = sizeof(info);
    return IMPORT_READY(taiGetModuleInfo) && taiGetModuleInfo("ScePspemu", &info) >= 0;
}

static int another_copy(void)
{
    tai_module_info_t tai;
    SceKernelModuleInfo mi;
    uintptr_t base[4];
    uint32_t size[4];
    if (!IMPORT_READY(taiGetModuleInfo) || !IMPORT_READY(sceKernelGetModuleInfo)) return 0;
    tai.size = sizeof(tai);
    if (taiGetModuleInfo(VJO_GAME_OCR_MODULE, &tai) < 0) return 0;
    sceClibMemset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (sceKernelGetModuleInfo(tai.modid, &mi) < 0) return 0;
    for (unsigned i = 0; i < 4; i++) {
        base[i] = (uintptr_t)mi.segments[i].vaddr;
        size[i] = mi.segments[i].memsz;
    }
    return outside_segments(base, size, 4, (uintptr_t)&another_copy & ~(uintptr_t)1);
}
#endif

#ifdef VJO_HOST
/* The host tests that include this file run no game: no hooks (game_ocr/hooks.c is the Vita's, and
 * host/tests/test_hooks_engine.c's); the stub records the word module_start passes. */
#define vjo_hooks_start(title, mode, psp) vjo_test_hooks_start(title, mode, psp)
#define vjo_hooks_stop() 0
#endif

/* API 31's text source word (docs/native-hooks.md), as the Shell gave it to the kernel: a
 * VJO_TEXT_SOURCE_* in its low byte (VJO_TEXT_SOURCE_MODE_MASK), plus VJO_TEXT_SOURCE_NO_SITES
 * (hook_sites = off): which native hooks vjo_hooks_start installs in this game. OCR (no hooks) when the
 * kernel's import is not resolved (an older kernel), or when its low byte is out of range or any other bit
 * is set. */
static unsigned text_source(void)
{
    /* another API's kernel (whose jobs vjo_game_ocr_worker_start refuses too) is not asked (hooks/api) */
    if (IMPORT_READY(vjoGetTextSource) && IMPORT_READY(vjoGetVersion) && vjoGetVersion() == VJO_API_VERSION) {
        int word = vjoGetTextSource();
        if (word >= 0 && ((unsigned)word & VJO_TEXT_SOURCE_MODE_MASK) < VJO_TEXT_SOURCE_COUNT &&
            !((unsigned)word & ~(VJO_TEXT_SOURCE_MODE_MASK | VJO_TEXT_SOURCE_NO_SITES)))
            return (unsigned)word; /* the whole word: vjo_hooks_start reads the flag */
    }
    return VJO_TEXT_SOURCE_OCR;
}

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    char title[VJO_TITLE_ID_SIZE] = {0};
    SceUID pid = sceKernelGetProcessId();
    int rc = -1;
    if (safe_boot()) return SCE_KERNEL_START_NO_RESIDENT;
    if (IMPORT_READY(sceKernelGetProcessTitleId)) rc = sceKernelGetProcessTitleId(pid, title, sizeof(title));
    if ((rc < 0 || !title[0]) && IMPORT_READY(sceAppMgrAppParamGetString))
        rc = sceAppMgrAppParamGetString(pid, 12, title, sizeof(title));
    title[sizeof(title)-1] = 0;
    /* *ALL also reaches SceShell and the system apps: there the module gives
     * its pages back at once (NO_RESIDENT unloads it without module_stop
     * [SDK: Kernel-Overview]), instead of holding its pages in SceShell's
     * partition for code that never runs (#59). An unreadable title is
     * treated the same way: nothing is started, nothing stays. Candidate
     * games keep an idle worker until the Shell confirms their games.ini
     * policy; one per process (another_copy). */
    if (rc < 0 || !vjo_game_is_candidate(title) || another_copy()) return SCE_KERNEL_START_NO_RESIDENT;
    psp_title = pspemu_process() || vjo_game_is_psp(title);
    /* The native text hooks, before the game's main thread runs: none in a safe boot (above), none
     * unless the text source allows them here (game_ocr/hooks.c); never fails the start. */
    sceClibMemcpy(hk.cache_key.title, title, sizeof(title));
    vjo_hooks_start(title, text_source(), psp_title);
    vjo_game_ocr_worker_start();
    /* Import/start failures leave the host app usable. Retained thread objects
     * keep the module loaded until module_stop can safely delete them. */
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp)
{
    int busy;
    (void)args; (void)argp;
    /* The hooks first (newest first), so no game thread enters our code again; a thread still in it
     * keeps the module (STOP_FAIL), as the worker's thread does. */
    busy = vjo_hooks_stop();
    if (vjo_game_ocr_worker_stop()) busy = 1;
    return busy ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}
#endif
