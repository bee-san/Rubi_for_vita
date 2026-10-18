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
#include "../core/fbgrey.h"
#include "../core/jpegsw.h"
#include "../core/textnorm.h"
#include "../core/vocr_ocr.h"
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
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static SceUID worker = -1, workspace_uid = -1, weights_uid = -1, capture_uid = -1, line_uid = -1, lm_uid = -1;
static SceUID cascade_uid = -1;
/* API 23: a shot's block (its colour strip, the encoder and the JPEG), only while a shot runs. */
static SceUID shot_uid = -1;
static uint32_t shot_bytes_held;
/* A rows call returned BUSY: the display hook may still write into the capture
 * (or tall-line) block, so neither is freed (they go with the process). */
static int hook_owns_capture;
/* A helper thread could not be joined (core/vocr_ocr.h VjoVocrStats.stuck): it may
 * still run in the job's workspace, so that block is never freed either. */
static int hook_owns_workspace;
static uint32_t workspace_bytes_held, weights_bytes_held, capture_bytes_held, line_bytes_held, lm_bytes_held;
static uint32_t cascade_bytes_held;
static uint32_t capture_memory, workspace_memory; /* VJO_GAME_OCR_MEM_* of the blocks held */
static void *line_base;

/* The game worker's memory ledger (core/ledger.h): what we hold in the game's
 * partition. The game has no file access, so it is read through the result
 * fields (workspace_bytes, resident_bytes, capture_bytes, the step log) rather
 * than printed here. */
enum { GL_MODULE = 0, GL_STACK, GL_WEIGHTS, GL_WORKSPACE, GL_CAPTURE, GL_LINE, GL_LM, GL_CASCADE, GL_HELPERS, GL_SHOT,
       GL_COUNT };
static const VjoLedgerItemDef ledger_defs[GL_COUNT] = {
    [GL_MODULE] = {"OCR worker pages", VJO_LEDGER_POOL_GAME},
    [GL_STACK] = {"VjoGameVocr stack+TLS", VJO_LEDGER_POOL_GAME},
    [GL_WEIGHTS] = {"VjoGameVocrWeights", VJO_LEDGER_POOL_GAME},
    [GL_WORKSPACE] = {"VjoGameVocr workspace", VJO_LEDGER_POOL_GAME},
    [GL_CAPTURE] = {"VjoGameVocrCapture", VJO_LEDGER_POOL_GAME},
    [GL_LINE] = {"VjoGameVocrLine", VJO_LEDGER_POOL_GAME},
    [GL_LM] = {"VjoGameVocrLM", VJO_LEDGER_POOL_GAME},
    [GL_CASCADE] = {"VjoGameVocrCascade", VJO_LEDGER_POOL_GAME},
    [GL_HELPERS] = {"VjoGameVocrHelper stacks+TLS", VJO_LEDGER_POOL_GAME},
    [GL_SHOT] = {"VjoGameShot", VJO_LEDGER_POOL_GAME},
};
VjoLedger g_game_ledger;

static void ledger_init(void)
{
    vjo_ledger_init(&g_game_ledger, ledger_defs, GL_COUNT);
    vjo_ledger_add(&g_game_ledger, GL_MODULE, VJO_BUDGET_GAME_MODULE_PAGES);
}

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
        if (uid >= 0) blocks_held++; /* from here its FREE step has a slot */
        step_add(VJO_GSTEP_ALLOC, ladder[i].mem, gitem, uid, rounded, 0, 0, 1);
        if (uid < 0) continue;
        if (sceKernelGetMemBlockBase(uid, base) < 0 || !*base || ((uintptr_t)*base & 15u)) {
            *base = NULL;
            if (sceKernelFreeMemBlock(uid) < 0) *leaked = uid;
            else blocks_held--;
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
    step_add(VJO_GSTEP_FREE, 0, gitem, rc, bytes, 0, 0, 1);
    if (rc < 0) return -1;
    if (blocks_held) blocks_held--;
    vjo_ledger_sub(&g_game_ledger, item, bytes);
    return 0;
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
/* API 21: the job is a request by tiers (its models open by their tier), and the
 * reserve its memory check kept (include/vjo_ledger_budget.h VJO_GAME_RESERVE_*),
 * which its resident blocks' allocations keep too. */
static int tiered;
static uint32_t job_reserve = GAME_RESERVE;
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
static void *weights_alloc(void *ud, size_t bytes);
static int weights_release(void *ud);
/* Verified weights kept between jobs while this game is the active one. */
static VjoVocrResident resident = {weights_alloc, weights_release, NULL, NULL, NULL, 0, 0, 0};
/* The same for the beam + LM decoder's LM (API 20). */
static void *lm_alloc(void *ud, size_t bytes);
static int lm_release(void *ud);
static VjoVocrLmResident lm_res = {lm_alloc, lm_release, NULL, NULL, NULL, 0, 0, 0};
static void *cascade_alloc(void *ud, size_t bytes);
static int cascade_release(void *ud);
/* The confidence cascade's larger model, kept like the weights (API 20). */
static VjoVocrResident cascade_res = {cascade_alloc, cascade_release, NULL, NULL, NULL, 0, 0, 0};
/* API 26: the text-presence gate's weights (VJO_VOCR_GATE_BYTES), read from the card once and kept while its
 * file is unchanged (core/vocr_ocr.h); in .bss, where the worker's data pages have room. */
static VjoVocrGateResident gate_res;
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

static int strip_source(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    Capture *c = ud;
    int rc;
    if (cancelled(NULL)) return VJO_E_CANCELLED;
    rc = capture_rows(c, y, n, dst, stride, flags);
    /* The first pass is in: one step for it, and the kernel's publication (the
     * job's first; a retry's is the same source, its count is the TEAR step's). */
    if (rc > 0 && (flags & VJO_STRIP_LAST) && !c->logged) {
        c->logged = 1;
        step_add(VJO_GSTEP_PASS, c->mode, c->pass_strips, 0, c->pass_bytes, c->last.fb_format, c->pass_us, 0);
        step_add(VJO_GSTEP_SIG, c->mode, 0, 0, c->last.fb_pitch, c->last.fb_width << 16 | c->last.fb_height, 0, 0);
    }
    return rc;
}

/* The core never asks for more than the ledger counts. */
typedef char vjo_line_budget_check[VJO_VOCR_LINE_MAX_BYTES <= VJO_BUDGET_GAME_LINE ? 1 : -1];
typedef char vjo_capture_budget_check[VJO_VOCR_CAPTURE_LIMIT <= VJO_BUDGET_GAME_CAPTURE ? 1 : -1];
typedef char vjo_lm_budget_check[PAGE_ROUND(VJO_VOCR_LM_MAX_BYTES) <= VJO_BUDGET_GAME_LM ? 1 : -1];
/* The weights and the cascade's model: at most the largest pinned model each (since API 21
 * the cascade's is the next tier up, so Normal's is Large's, the largest). */
typedef char vjo_weights_budget_check[PAGE_ROUND(VJO_VOCR_MAX_BYTES) <= VJO_BUDGET_GAME_WEIGHTS ? 1 : -1];
typedef char vjo_cascade_budget_check[PAGE_ROUND(VJO_VOCR_MAX_BYTES) <= VJO_BUDGET_GAME_CASCADE &&
                                      VJO_VOCR_CASCADE_BYTES <= VJO_VOCR_MAX_BYTES ? 1 : -1];

/* One block for a line taller than a strip (VjoGreySource.line_buffer). */
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
static int file_read(void *ud, uint64_t off, void *dst, size_t bytes)
{
    (void)ud;
    if (!model_owned || !dst || off > UINT32_MAX || bytes > UINT32_MAX-off || cancelled(NULL)) return -1;
    for (size_t used = 0; used < bytes;) {
        if (cancelled(NULL)) return -1;
        size_t n = bytes-used < VJO_GAME_OCR_MODEL_CHUNK ? bytes-used : VJO_GAME_OCR_MODEL_CHUNK;
        uint64_t started = sceKernelGetProcessTimeWide();
        int got;
        do {
            if (cancelled(NULL)) return -1;
            got = vjoOcrModelRead(model_seq, (uint32_t)(off+used), (uint32_t)n, (unsigned char *)dst+used);
            if (got != VJO_ERR_BUSY) break;
            if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { got = VJO_ERR_TIMEOUT; break; }
            sceKernelDelayThread(MODEL_POLL_US);
        } while (1);
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
        uint64_t started = sceKernelGetProcessTimeWide();
        int rc;
        do {
            /* Cleanup must drain even after cancellation or active_seq reset. */
            rc = vjoOcrModelClose(model_seq);
            if (rc != VJO_ERR_BUSY) break;
            if (sceKernelGetProcessTimeWide()-started >= (shot_file ? SHOT_FILE_WAIT_US : MODEL_WAIT_US)) {
                rc = VJO_ERR_TIMEOUT;
                break;
            }
            sceKernelDelayThread(MODEL_POLL_US);
        } while (1);
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

static int file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    if (model_owned || !path || !out || cancelled(NULL)) return -1;
    /* Reserve ownership before enqueue: a timed-out Open may still finish.
     * The kernel resolves only this claimed job's pinned model path. */
    model_seq = active_seq;
    model_owned = 1;
    uint64_t started = sceKernelGetProcessTimeWide();
    VjoGameOcrModelInfo info;
    uint32_t which = which_file(path);
    int size;
    do {
        if (cancelled(NULL)) return -1;
        sceClibMemset(&info, 0, sizeof(info));
        size = vjoOcrModelOpen(model_seq, which, &info);
        if (size != VJO_ERR_BUSY) break;
        if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { size = VJO_ERR_TIMEOUT; break; }
        sceKernelDelayThread(MODEL_POLL_US);
    } while (1);
    if (size <= 0) {
        /* API 26: the gate's weights are optional: a job that cannot read them reads as without the gate (its
         * step says why), so their failed open is drained at once rather than holding the job's own files. */
        if (which == VJO_GAME_OCR_OPEN_GATE) {
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

/* VjoVocrResident.alloc: one page-rounded USER_RW block for the weights, under
 * the same reserve as the job workspace. */
static void *weights_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes), held, mem;
    void *base = NULL;
    SceUID leaked;
    if (weights_uid >= 0 || !bytes || bytes > VJO_VOCR_MAX_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    weights_uid = game_memblock_alloc(GL_WEIGHTS, VJO_GITEM_WEIGHTS, "VjoGameVocrWeights", rounded, LADDER_USER,
                                      &base, &leaked, &held, &mem);
    if (weights_uid < 0) {
        /* A block that cannot be released stays recorded; cleanup retries it. */
        weights_uid = leaked;
        return NULL;
    }
    weights_bytes_held = held;
    return base;
}

static int weights_release(void *ud)
{
    (void)ud;
    if (weights_uid < 0) return 0;
    if (game_memblock_free(GL_WEIGHTS, VJO_GITEM_WEIGHTS, weights_uid, weights_bytes_held) < 0) return -1;
    weights_uid = -1;
    weights_bytes_held = 0;
    return 0;
}

/* Releases the resident weights, and a block whose base lookup failed. */
static int drop_weights(void)
{
    if (vjo_vocr_resident_drop(&resident)) return -1;
    return weights_release(NULL);
}

/* VjoVocrLmResident.alloc: one page-rounded USER_RW block for the LM, under the
 * same reserve. */
static void *lm_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes), held, mem;
    void *base = NULL;
    SceUID leaked;
    if (lm_uid >= 0 || !bytes || bytes > VJO_VOCR_LM_MAX_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    lm_uid = game_memblock_alloc(GL_LM, VJO_GITEM_LM, "VjoGameVocrLM", rounded, LADDER_USER, &base, &leaked, &held,
                                 &mem);
    if (lm_uid < 0) {
        lm_uid = leaked;
        return NULL;
    }
    lm_bytes_held = held;
    return base;
}

static int lm_release(void *ud)
{
    (void)ud;
    if (lm_uid < 0) return 0;
    if (game_memblock_free(GL_LM, VJO_GITEM_LM, lm_uid, lm_bytes_held) < 0) return -1;
    lm_uid = -1;
    lm_bytes_held = 0;
    return 0;
}

/* Releases the resident LM (and a block whose base lookup failed). */
static int drop_lm(void)
{
    if (vjo_vocr_lm_drop(&lm_res)) return -1;
    return lm_release(NULL);
}

/* VjoVocrResident.alloc of the cascade's model: one page-rounded USER_RW block,
 * under the same reserve. */
static void *cascade_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes), held, mem;
    void *base = NULL;
    SceUID leaked;
    if (cascade_uid >= 0 || !bytes || bytes > VJO_VOCR_MAX_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    cascade_uid = game_memblock_alloc(GL_CASCADE, VJO_GITEM_CASCADE, "VjoGameVocrCascade", rounded, LADDER_USER,
                                      &base, &leaked, &held, &mem);
    if (cascade_uid < 0) {
        cascade_uid = leaked;
        return NULL;
    }
    cascade_bytes_held = held;
    return base;
}

static int cascade_release(void *ud)
{
    (void)ud;
    if (cascade_uid < 0) return 0;
    if (game_memblock_free(GL_CASCADE, VJO_GITEM_CASCADE, cascade_uid, cascade_bytes_held) < 0) return -1;
    cascade_uid = -1;
    cascade_bytes_held = 0;
    return 0;
}

static int drop_cascade(void)
{
    if (vjo_vocr_resident_drop(&cascade_res)) return -1;
    return cascade_release(NULL);
}

/* Everything kept between jobs: the weights, the LM and the cascade's model. */
static int drop_resident(void)
{
    int rc = drop_weights();
    rc |= drop_lm();
    return drop_cascade() || rc ? -1 : 0;
}

static uint32_t cascade_resident_bytes(void)
{
    return cascade_res.model && cascade_uid >= 0 ? PAGE_ROUND(cascade_res.model->bytes) : 0;
}

static uint32_t lm_resident_bytes(void)
{
    return lm_res.lm && lm_uid >= 0 ? PAGE_ROUND(lm_res.lm->bytes) : 0;
}

static uint32_t resident_bytes(void)
{
    return resident.model && weights_uid >= 0 ? PAGE_ROUND(resident.model->bytes) : 0;
}

/* Resident blocks with verified contents: no job frees them but on a path that
 * drops them before its recognition, so their FREE steps need no slot. */
static unsigned blocks_kept(void)
{
    return (resident.model && weights_uid >= 0) + (lm_res.lm && lm_uid >= 0) + (cascade_res.model && cascade_uid >= 0);
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

/* The reserve a job of model tier `tier` keeps (include/vjo_ledger_budget.h): the
 * largest tier it holds, so the cascade's (the next tier up) when it has one; a
 * request by name (tier 0) keeps the flat GAME_RESERVE. */
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
    out->release_bytes = (resident.model && resident.model != m ? resident_bytes() : 0) +
                         (lm_res.lm && lm_res.lm != lm ? lm_resident_bytes() : 0) +
                         (cascade_res.model && cascade_res.model != casc ? cascade_resident_bytes() : 0);
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
    file_close(NULL);
    if (model_owned) rc = VJO_E_OCR_UNAVAILABLE;
    /* Between jobs a weights (or LM) block holds verified bytes or nothing. */
    if (!resident.model && (resident.data || weights_uid >= 0) && drop_weights()) rc = VJO_E_OCR_UNAVAILABLE;
    if (!lm_res.lm && (lm_res.data || lm_uid >= 0) && drop_lm()) rc = VJO_E_OCR_UNAVAILABLE;
    if (!cascade_res.model && (cascade_res.data || cascade_uid >= 0) && drop_cascade()) rc = VJO_E_OCR_UNAVAILABLE;
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
 * SceShell, which learns the JPEG's size from the result. */
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
 * VJO_GAME_OCR_SHOT_MAX), with the arena's alignment and the buffer's spare byte. */
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
    uint64_t started = now_wide();
    for (;;) {
        int rc;
        if (cancelled(NULL)) return VJO_E_CANCELLED;
        rc = src ? vjoOcrShotWrite(model_seq, offset, bytes, src) : vjoOcrShotSave(model_seq, bytes);
        if (rc != VJO_ERR_BUSY) return rc;
        if (now_wide() - started >= wait_us) return VJO_ERR_TIMEOUT;
        sceKernelDelayThread(MODEL_POLL_US);
    }
}

static int run_shot(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    const uint32_t w = r->width, h = r->height, stride = w * 4u, need = SHOT_BLOCK_BYTES(w);
    Capture c = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 1, 0, 1}; /* fixed: shot_rows picks the rung */
    VjoArena a;
    VjoBuf jpg = {0};
    void *base = NULL;
    SceUID leaked;
    uint32_t held, mem, off = 0, chunks = 0;
    int rc, quality = SHOT_QUALITY;
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
    for (unsigned t = 1; t <= SHOT_TRIES; t++) {
        uint64_t t1 = now_wide();
        uint32_t strips = c.pass_strips;
        uint8_t *band;
        VjoJpeg *e = NULL;
        vjo_arena_init(&a, base, held);
        rc = VJO_E_OOM;
        if ((band = (uint8_t *)vjo_arena_alloc(&a, (size_t)SHOT_HOOK_ROWS * stride)) &&
            (e = vjo_jpeg_begin(&a, w, h, quality, &jpg)))
            rc = VJO_OK;
        for (uint32_t y = 0; !rc && y < h;) {
            uint32_t n = h - y < SHOT_HOOK_ROWS ? h - y : SHOT_HOOK_ROWS;
            int got = shot_rows(&c, y, n, band, stride);
            if (got != (int)n) {
                rc = got;
                break;
            }
            for (uint32_t k = 0; !rc && k < n; k += VJO_JPEG_BAND)
                if (vjo_jpeg_rows(e, band + (size_t)k * stride, n - k < VJO_JPEG_BAND ? n - k : VJO_JPEG_BAND,
                                  stride) < 0 || jpg.len > VJO_GAME_OCR_SHOT_MAX)
                    rc = VJO_E_TOO_LARGE;
            y += n;
        }
        if (!rc && (vjo_jpeg_end(e) < 0 || jpg.len > VJO_GAME_OCR_SHOT_MAX)) rc = VJO_E_TOO_LARGE;
        step_add(VJO_GSTEP_SHOT, (unsigned)quality, t, rc, (uint32_t)jpg.len, c.pass_strips - strips,
                 (uint32_t)(now_wide() - t1), 0);
        out->shot_quality = (uint32_t)quality;
        out->shot_tries = t;
        if (rc != VJO_E_TOO_LARGE) break;
        quality = quality > 50 ? 50 : 35;
    }
    out->shot_encode_us = (uint32_t)(now_wide() - t0);
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

int vjo_game_ocr_run_request(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    if (!r || !out) return VJO_E_SOURCE;
    if (!g_game_ledger.defs) ledger_init();
    sceClibMemset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->seq = r->seq;
    active_seq = r->seq; io_error = 0; stat_error = 0; open_which = VJO_GAME_OCR_OPEN_MODEL; open_cascade = 0;
    tiered = 0; job_reserve = GAME_RESERVE;
    log_to = out;
    job_started_us = now_wide();
    int rc = VJO_E_SOURCE;
    VjoVocrStats stats = {0};
    Capture capture = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 0, 0, 0};
    if (blocked || workspace_uid >= 0 || capture_uid >= 0 || shot_uid >= 0 || model_owned) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    /* The refinement's mode (bits 12-13): 3 is refused here, as unknown bits are. */
    unsigned refine_mode = (r->flags >> VJO_GAME_OCR_REFINE_SHIFT) & VJO_GAME_OCR_REFINE_MASK;
    /* API 23: a shot carries no model, tiers, speed or other flag but F_FULL */
    int shot = (r->flags & VJO_GAME_OCR_F_SHOT) != 0;
    if (r->size != sizeof(*r) || !r->seq || !r->capture_seq || (r->flags & ~VJO_GAME_OCR_F_ALL) ||
        refine_mode > VJO_GAME_OCR_REFINE_PSP || (r->tiers & ~VJO_VOCR_TIERS_ALL) ||
        (r->speed & ~VJO_GAME_OCR_S_ALL) ||
        !r->width || r->width > VJO_OCR_MAX_WIDTH || !r->height || r->height > VJO_OCR_MAX_HEIGHT ||
        !r->fb_width || r->fb_width > VJO_OCR_MAX_WIDTH || !r->fb_height || r->fb_height > VJO_OCR_MAX_HEIGHT ||
        r->x > r->fb_width - r->width || r->y > r->fb_height - r->height ||
        (shot ? r->tiers || r->speed || (r->flags & ~(VJO_GAME_OCR_F_SHOT | VJO_GAME_OCR_F_FULL))
              : !r->model_dir[0] || !r->model[0]) ||
        r->model_dir[sizeof(r->model_dir)-1] || r->model[sizeof(r->model)-1] ||
        r->fallback[sizeof(r->fallback)-1]) goto done;
    out->region_x = r->x; out->region_y = r->y; out->width = r->width; out->height = r->height;
    out->fb_width = r->fb_width; out->fb_height = r->fb_height;
    if (cancelled(NULL)) { rc = VJO_E_CANCELLED; goto done; }
    step_add(VJO_GSTEP_MEM, 0, 0, 0, 0, 0, 0, 1);
    if (shot) { rc = run_shot(r, out); goto done; } /* both builds: an Anki card's screenshot (API 23) */
#if VJO_DEBUG
    if (r->flags & VJO_GAME_OCR_F_PROBE) { rc = run_probe(r, out); goto done; }
#else
    if (r->flags & VJO_GAME_OCR_F_PROBE) goto done; /* no probe in the normal build: VJO_E_SOURCE */
#endif
    /* The finder's boxes refined (API 20): the mode, here or in a PSP game. */
    unsigned refine = refine_mode == VJO_GAME_OCR_REFINE_ON || (refine_mode == VJO_GAME_OCR_REFINE_PSP && psp_title)
                      ? VJO_VOCR_REFINE_ALL : 0u;
    size_t refine_bytes = refine ? vjo_vocr_refine_bytes(r->width, r->height) : 0;
    size_t capture_bytes = vjo_vocr_capture_bytes(r->width, r->height);
    /* The models this job may run, in the order it tries them (API 21): a request
     * by tiers names its tiers, the largest first; one by name its model, then its
     * fallback, used only if it is another pinned model needing less memory. */
    Candidate cand[CANDIDATES];
    unsigned n = 0, top = 0;
    tiered = r->tiers != 0;
    if (tiered) {
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
    /* Weights of a model this job cannot use: useless now, and two are never held. */
    if (resident.model && !candidate_of(cand, n, resident.model) && drop_weights()) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    /* The LM: the largest tier the request allows that fits (API 20); none
     * allowed, and an LM still resident from an earlier setting goes. */
    unsigned lm_max = (r->flags >> VJO_GAME_OCR_LM_SHIFT) & VJO_GAME_OCR_LM_MASK;
    const VjoVocrLm *lm = NULL;
    if (lm_max > VJO_VOCR_LM_TIERS) lm_max = VJO_VOCR_LM_TIERS;
    if (!lm_max && (lm_res.data || lm_uid >= 0) && drop_lm()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* The confidence cascade (API 20): the request's time ceiling, and a larger
     * model beside the job's (cascade_of); none asked for, and a resident one goes. */
    uint32_t casc_us = ((r->flags >> VJO_GAME_OCR_CASCADE_SHIFT) & VJO_GAME_OCR_CASCADE_MASK) *
                       VJO_GAME_OCR_CASCADE_UNIT_US;
    const VjoVocrModel *casc = NULL;
    if (!casc_us && (cascade_res.data || cascade_uid >= 0) && drop_cascade()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* Reported also when refused: each title's real free memory. Each model is
     * checked in turn, its cascade and LM first (check_with_lm); the first that
     * fits runs. The first one's need stays in model_need_* when another runs. */
    out->free_user = free_user();
    unsigned used = 0;
    int fits = 0;
    uint32_t first_workspace = 0, first_weights = 0, first_release = 0;
    for (unsigned i = 0; i < n; i++) {
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
    /* Helpers (ocr_threads, API 22) only when they fit beside everything else the
     * check chose (the tier's model, its LM, the cascade), keeping the same reserve
     * as that check (the tier's, out->reserve_bytes: 4, 8 or 16 MiB by the largest
     * model the job holds): one fewer at a time, down to none, which is the serial
     * job, so helpers never make a job step down a tier. Their line slots end the
     * workspace block. */
    unsigned want_helpers = r->speed & VJO_GAME_OCR_S_HELPERS_MASK;
    unsigned use_helpers = 0;
    size_t helper_bytes = 0, helpers_need_bytes = 0;
    if (want_helpers > VJO_VOCR_HELPERS_MAX) want_helpers = VJO_VOCR_HELPERS_MAX;
    if (fits) {
        const size_t block = use_bytes + extra_job_bytes(use, lm, casc);
        for (use_helpers = want_helpers; use_helpers; use_helpers--) {
            size_t extra = helpers_need(use->name, block, use_helpers);
            if (extra && (int64_t)out->free_user + out->release_bytes - (int64_t)out->reserve_bytes >=
                             (int64_t)out->need_workspace + out->need_weights + (int64_t)extra)
                break;
        }
        if (use_helpers) {
            helper_bytes = vjo_vocr_helpers_bytes(use->name, use_helpers);
            helpers_need_bytes = helpers_need(use->name, block, use_helpers);
            out->need_workspace += (uint32_t)helpers_need_bytes;
        }
    }
    stats.workspace = use_bytes;
    if (!fits) {
        /* Too little memory: give the game back the resident weights and LM too. */
        if (drop_resident()) rc = VJO_E_OCR_UNAVAILABLE;
        else rc = VJO_E_OOM;
        goto done;
    }
    /* The check counted another model's weights, or another LM (or one this job
     * has no room for), as free: release them. Another model's weights stay
     * while the job fits beside them with its own reserve (its size's since API
     * 21, out->reserve_bytes), until it reads its own (never two: the core drops
     * them first), so a job that reads no line keeps them: the self-test's
     * fallback stage on a screen without text keeps the user's model
     * (docs/first-ocr.md). */
    if (resident.model && resident.model != use &&
        (int64_t)out->free_user - (int64_t)out->reserve_bytes < (int64_t)out->need_workspace + out->need_weights &&
        drop_weights()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if ((lm_res.data || lm_uid >= 0) && lm_res.lm != lm && drop_lm()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if ((cascade_res.data || cascade_uid >= 0) && cascade_res.model != casc && drop_cascade()) {
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
    VjoPlatform platform = {.file_open=file_open, .now_us=now_us};
    VjoGreySource image = {&capture, r->width, r->height, strip_source, cancelled, line_buffer};
    VjoVocrPolicy policy;
    request_policy(r, &policy);
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
    policy.short_lines = 1; /* #194: a box's short last line */
    /* The cache's entries are those of jobs with this LM (bits 0-2), cascade (bit 3),
     * cascade ceiling (ms, bits 4-15: at most 3,750), cascade model (since API 21 the
     * next tier up of the job's, so a Small job's is Normal's model and a Normal one's Large's: its
     * index among the pinned models + 1, bits 16-23) and refinement (bits 24-28). */
    unsigned casc_index = 0;
    if (casc) {
        unsigned count = 0;
        const VjoVocrModel *all = vjo_vocr_models(&count);
        for (unsigned k = 0; k < count && k < 0xFFu; k++)
            if (&all[k] == casc) casc_index = k + 1u;
    }
    policy.cache_variant = (lm ? lm->tier : 0u) | (casc ? 8u : 0u) | (uint32_t)(casc_us / 1000u) << 4 |
                           (uint32_t)casc_index << 16 | refine << 24;
    helper_pool.helpers = use_helpers;
    helper_creates = helper_create_failures = 0;
    open_which = cand[used].which;
    open_cascade = casc != NULL;
    rc = vjo_vocr_ocr_policy(&platform, r->model_dir, use->name, &image, &resident, base,
                             use_bytes + core_extra + helper_bytes,
                             capture_base, capture_bytes_held, &policy, out->text, sizeof(out->text), &stats);
    /* ocr-first: a job that read no line (or only blank ones) reads what the
     * session's first job with text would otherwise read while the user waits
     * (docs/first-ocr.md), as its check allowed; not the self-test's forced
     * fallback, which would put the fallback in the user's model's place. Best
     * effort, so its rc is not the job's: a cancellation ends the job below,
     * a block it could not release is cleanup()'s, a file it could not read the
     * next text job's (io_error says which). */
    if (!rc && !stats.model_loaded && !stats.model_cached && !forced)
        vjo_vocr_warm(&platform, r->model_dir, use->name, &image, &resident, &policy, &stats);
    /* A native capture error (the kernel's VJO_ERR_*, an SDK code) ends the job
     * as a source failure; its exact code is in the step log (VJO_GSTEP_PASS). */
    if (rc < -199) rc = VJO_E_SOURCE;
    if (stats.tears) /* bytes: when the last try began (us since the job's start), its tears before it */
        step_add(VJO_GSTEP_TEAR, 0, stats.tears, (int32_t)stats.tries,
                 stats.tries > 1 ? stats.retry_at - (uint32_t)job_started_us : 0u, 0, 0, 0);
    if (capture.failed) step_add(VJO_GSTEP_PASS, capture.mode, capture.pass_strips, capture.failed, capture.pass_bytes,
                                 capture.last.fb_format, capture.pass_us, 0);
    step_add(VJO_GSTEP_FIND, 0, stats.lines, rc == VJO_E_OCR_REGION ? rc : 0, 0, stats.dark, stats.find_us, 0);
    if (policy.settle_us)
        step_add(VJO_GSTEP_SETTLE, 0, stats.settle_passes,
                 stats.settled ? VJO_GSETTLE_HELD : stats.settle_timeout ? VJO_GSETTLE_TIMEOUT : VJO_GSETTLE_NONE,
                 stats.settle_changes, (stats.cycling_rows > 0xFFFFu ? 0xFFFFu : stats.cycling_rows) << 16 |
                 (stats.cycles > 0xFFFFu ? 0xFFFFu : stats.cycles), stats.settle_us ? stats.settle_us : 1u, 0);
    for (unsigned i = 0; i < stats.blank && i < VJO_VOCR_BLANK_LOGGED; i++) {
        const VjoVocrBlankStat *b = &stats.blank_line[i];
        step_add(VJO_GSTEP_BLANK, 0, b->index, (int32_t)stats.blank, (uint32_t)b->width << 16 | b->height,
                 (uint32_t)(b->ink.hi - b->ink.lo) << 16 | b->ink.edges, stats.blank_us ? stats.blank_us : 1u, 0);
    }
    if (refine && stats.lines) /* each count at most VJO_VOCR_MAX_LINES: the boxes of one try */
        step_add(VJO_GSTEP_REFINE, refine_mode, stats.refine_lines, stats.refine_rc,
                 stats.refine_boxes << 16 | (stats.refine_capped & 0xFFu) << 8 | (stats.refine_split & 0xFFu),
                 (uint32_t)stats.refine_rows, stats.refine_us + 1u, 0);
    if (stats.model_loaded || stats.model_cached) /* arg 1: read by the warm-up */
        step_add(VJO_GSTEP_LOAD, stats.model_loaded ? VJO_GAME_OCR_WEIGHTS_READ : VJO_GAME_OCR_WEIGHTS_RESIDENT,
                 stats.warmed, 0, resident_bytes(), 0, stats.load_us, 1);
    if (stats.lines)
        step_add(VJO_GSTEP_RECOGNIZE, 0, stats.recognized, rc, stats.frames, stats.tall_skipped, stats.recognize_us, 0);
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
        unsigned changes = vjo_text_normalize_keep(out->text, sizeof(out->text), lm_extra ? policy.raw : NULL, &k);
#define CLAMP8(v) ((v) > 255u ? 255u : (v))
        if (out->text[0])
            step_add(VJO_GSTEP_NORM, 0, changes, 0, k.width,
                     CLAMP8(k.tilde) << 24 | CLAMP8(k.dash) << 16 | CLAMP8(k.ellipsis) << 8 |
                     (k.long_vowel > 15u ? 15u : k.long_vowel) << 4 | (k.kanji_one > 15u ? 15u : k.kanji_one),
                     (uint32_t)(now_wide() - t0) + 1u, 0);
#undef CLAMP8
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
    /* A helper that could not be joined may still run in the workspace: keep it,
     * and take no more jobs (core/vocr_ocr.h VjoVocrPool). */
    if (stats.stuck) { blocked = 1; hook_owns_workspace = 1; }
done:
    out->workspace_bytes = (uint32_t)stats.workspace;
    out->lines = stats.lines; out->find_us = stats.find_us;
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
        out->captures = stats.strips;
        out->tears = stats.tears;
        out->capture_us = stats.capture_us;
        out->region_sum = stats.region_sum;
        if (capture.last.fb_width) {
            out->fb_width = capture.last.fb_width; out->fb_height = capture.last.fb_height;
            out->fb_pitch = capture.last.fb_pitch; out->fb_format = capture.last.fb_format;
        }
    }
    out->capture_memory = capture_memory;
    out->capture_bytes = capture_bytes_held;
    out->cleanup_status = cleanup();
    out->resident_bytes = resident_bytes();
    out->io_error = io_error;
    out->stat_error = stat_error;
    if (out->cleanup_status) { blocked = 1; rc = VJO_E_OCR_UNAVAILABLE; }
    else if (cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (rc) sceClibMemset(out->text, 0, sizeof(out->text));
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

static int worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!__atomic_load_n(&quitting, __ATOMIC_ACQUIRE) && !blocked) {
        int nap = 1;
        if (claim_outstanding) {
            if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
        } else {
            int registered = vjoOcrRegister(), gone = inactive_long(registered);
            if (!registered) {
                static VjoGameOcrRequest request; /* not on the 32 KiB stack */
                uint64_t asked = now_wide();
                int took;
                sceClibMemset(&request, 0, sizeof(request));
                request.size = sizeof(request);
                took = vjoOcrTake(&request);
                if (!took) {
                    claim_outstanding = 1;
                    vjo_game_ocr_run_request(&request, &pending_result);
                    if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
                    nap = claim_outstanding; /* done: the next job may be queued already */
                } else if (now_wide() - asked >= TAKE_WAITED_US) {
                    nap = 0; /* the kernel held the take: its wait was the pace */
                }
            } else if (gone &&
                       (resident.data || weights_uid >= 0 || lm_res.data || lm_uid >= 0 || cascade_res.data ||
                        cascade_uid >= 0) &&
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
    /* The game is exiting or the plugin stops: no weights or LM stay behind. */
    if (drop_resident()) return VJO_E_OCR_UNAVAILABLE;
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
    vjo_game_ocr_worker_start();
    /* Import/start failures leave the host app usable. Retained thread objects
     * keep the module loaded until module_stop can safely delete them. */
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return vjo_game_ocr_worker_stop() ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}
#endif
