/* The on-device self-test (docs/self-test.md). Armed by one file,
 * ux0:data/VitaJPOverlay/selftest.txt (tools/install_ftp.sh --self-test),
 * it follows the next boot and the first game started: at each step it
 * writes lines to ux0:data/VitaJPOverlay/selftest-report.txt, with the free
 * memory of every pool we can read, by every route, and the ledger. The report
 * is appended a record at a time (one append per file, see Group), so a crash
 * leaves the records written; the report is what the owner copies back for
 * tools/analyse_selftest.py.
 *
 * v2 (API 19): once a job of the game has read text in the user's region, a
 * background job of its own (shell/worker.c selftest_job_due) runs every stage
 * in turn on that screen, with the overlay closed: the user's region with the
 * fallback model forced (the smallest size's read from the card), the user's region with the
 * configured model, the full screen through the strips (weights kept), the
 * capture probe (each capture path and memory type, one strip each), and the
 * user's region again (the warm time). It shows nothing: the user's text and
 * its lookup are the earlier job's. Opening the overlay or a new screen stops
 * it, and the stages run again from the first after the next job that reads
 * text: the user's scans never wait for them (test.2's first scans did, 9-15 s:
 * docs/first-ocr.md). A first stage that still finds no line waits
 * (vjo_selftest_stage_wait, at most VJO_SELFTEST_WAITS screens). Each stage
 * ends with its own record, passed or not, then the next one starts; after the
 * last the test is done and the marker removed, so a failure never leaves it
 * armed (#74: the API 18 test stayed armed after a failed capture and kept
 * forcing the full-screen region). It changes one thing while armed: OCR runs
 * automatically. The region is the user's. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "../core/boot_guard.h"
#include "../core/fbgrey.h"
#include "../include/vjo_import.h"
#include "../include/vjo_ledger_budget.h"
#include "../include/vjo_vocr_sizes.h"
#include "shell.h"

#define MARKER VJO_DATA_DIR "/selftest.txt"
#define REPORT VJO_DATA_DIR "/selftest-report.txt"
#define REPORT_CAP (48 * 1024)

#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static int armed;        /* the marker was present at this boot */
static int finished;     /* every stage has its record: the test is complete */
static int report_bytes; /* -1: the report could not be opened */
static int64_t t0;

/* A record's lines (a stage's end, a memory sample) go out with one append per
 * file (the report, status.txt, log.txt), not one per line: an append costs the
 * console 12-46 ms (#81's self-test timestamps), and test.2's stage records,
 * each line appended to the three files in turn, took 0.4-2.4 s each, in the
 * job's thread (docs/first-ocr.md). A crash loses at most the record being
 * written. The group lives on its caller's stack. */
#define GROUP_BYTES 2048
typedef struct {
    int used;
    char buf[GROUP_BYTES];
} Group;

/* On the stack of the thread that writes a record: the chains in docs/memory-ledger.md count it. */
typedef char vjo_selftest_group_on_the_stack[sizeof(Group) <= 2048 + sizeof(int) ? 1 : -1];

static void flush(Group *g)
{
    SceUID fd;
    if (!g->used)
        return;
    if ((fd = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666)) >= 0) {
        if (sceIoWrite(fd, g->buf, g->used) == g->used)
            report_bytes += g->used;
        sceIoClose(fd);
    }
    vjo_log_raw(g->buf, g->used); /* status.txt and log.txt too */
    g->used = 0;
}

/* One line of the report into g; a full group goes out first. */
static void report(Group *g, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;
    if (!armed || report_bytes < 0 || report_bytes + g->used > REPORT_CAP)
        return;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    if (g->used + n > GROUP_BYTES)
        flush(g);
    sceClibMemcpy(g->buf + g->used, line, n);
    g->used += n;
}

static int64_t ms_since_start(void)
{
    return ((int64_t)sceKernelGetProcessTimeWide() - t0) / 1000;
}

/* One memory sample. SceShell's budget as the user API reports it: each
 * figure is the free size minus a system margin [SDK: Kernel-Reference], so a
 * negative one is under the margin (valid=0: not a free size; #74 read
 * -2097152, i.e. nothing free above a 2 MiB margin), not an error. Then the
 * PAF heap, the Shell ledger, and every kernel route (kernel/meminfo.c), each
 * with its own result. */
static void sample(Group *g, const char *step)
{
    SceKernelFreeMemorySizeInfo f = {.size = sizeof(f)};
    VjoMemInfo m;
    const VjoLedger *l = &g_shell_ledger;
    int rc = sceKernelGetFreeMemorySize(&f);
    report(g, "mem %s rc=0x%08X main=%d cdram=%d phycont=%d valid=%d paf_free=%u", step, (unsigned)rc,
           rc < 0 ? 0 : f.size_user, rc < 0 ? 0 : f.size_cdram, rc < 0 ? 0 : f.size_phycont,
           rc >= 0 && f.size_user >= 0 && f.size_cdram >= 0 && f.size_phycont >= 0,
           (unsigned)vjo_shell_heap_free_size());
    report(g, "ledger %s pages=%u/%u paf=%u/%u", step,
           (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAGES], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAGES],
           (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAF], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAF]);
    flush(g); /* a crash in the kernel's memory routes (#75's B1 broke them once) keeps the lines before */
    sceClibMemset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    if (!IMPORT_READY(vjoMemInfo) || (rc = vjoMemInfo(&m)) < 0) {
        report(g, "kmem %s route=none rc=0x%08X", step, (unsigned)rc);
        return;
    }
    report(g, "kmem %s route=block part=kernel rc=0x%08X step=%d total=%u free=%u", step, (unsigned)m.kernel_rc,
           (int)m.kernel_step, (unsigned)m.kernel_total, (unsigned)m.kernel_free);
    report(g, "kmem %s route=block part=shell rc=0x%08X step=%d total=%u free=%u", step, (unsigned)m.caller_rc,
           (int)m.caller_step, (unsigned)m.caller_total, (unsigned)m.caller_free);
    report(g, "kmem %s route=cas part=shell rc=0x%08X step=%d main=%u/%u cdram=%u/%u phycont=%u/%u", step,
           (unsigned)m.cas_rc, (int)m.cas_step, (unsigned)m.cas_main_free, (unsigned)m.cas_main_total,
           (unsigned)m.cas_cdram_free, (unsigned)m.cas_cdram_total, (unsigned)m.cas_phycont_free,
           (unsigned)m.cas_phycont_total);
    {
        char parts[160];
        size_t n = 0;
        parts[0] = 0;
        for (uint32_t i = 0; i < m.as_parts && i < VJO_MEMINFO_PARTS; i++) {
            int w = sceClibSnprintf(parts + n, sizeof(parts) - n, " p%u=%u/%u", (unsigned)i, (unsigned)m.as_free[i],
                                    (unsigned)m.as_total[i]);
            if (w < 0 || (size_t)w >= sizeof(parts) - n) break;
            n += (size_t)w;
        }
        report(g, "kmem %s route=as part=%s rc=0x%08X step=%d parts=%u skipped=%u%s", step,
               m.as_caller ? "shell" : "kernel", (unsigned)m.as_rc, (int)m.as_step, (unsigned)m.as_parts,
               (unsigned)m.as_skipped, parts);
    }
    report(g, "kernel %s ledger=%u/%u limit=%u", step, (unsigned)m.ledger_current, (unsigned)m.ledger_peak,
           (unsigned)m.ledger_limit);
}

int vjo_selftest_armed(void)
{
    return armed && !finished;
}

void vjo_selftest_init(const VjoConfig *cfg)
{
    SceIoStat st;
    VjoBootInfo b = {.size = sizeof(b)};
    SceUID fd;
    Group g;
    t0 = (int64_t)sceKernelGetProcessTimeWide();
    if (sceIoGetstat(MARKER, &st) < 0)
        return;
    armed = 1;
    fd = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        report_bytes = -1;
        vjo_log("self-test: report not writable 0x%08X", (unsigned)fd);
        return;
    }
    sceIoClose(fd);
    g.used = 0;
    report(&g, "selftest v2 api=%d", VJO_API_VERSION);
    if (IMPORT_READY(vjoBootInfo) && vjoBootInfo(&b) == 0)
        report(&g, "boot seq=%u mode=%d prev=%d/%d/%d/%d failures=%d write=0x%08X", (unsigned)b.seq, b.mode,
               b.prev_mode, b.prev_stage, b.prev_inert, b.prev_fault, b.failures, (unsigned)b.write_status);
    /* model: the configured tier's (API 21); fallback: the smallest tier's, which
     * the fallback stage runs ("-": the configured tier is the smallest). */
    /* backend=2: the old VOCR id, kept so that reports keep their form (VOCR is the only engine) */
    report(&g, "config backend=%d model=%s fallback=%s tier=%s dictionary=%d anki=%d", 2,
           vjo_vocr_tier_model(cfg->vocr_tier) ? vjo_vocr_tier_model(cfg->vocr_tier) : "-",
           cfg->vocr_tier > (int)VJO_VOCR_TIER_SMALL ? vjo_vocr_tier_model(VJO_VOCR_TIER_SMALL) : "-",
           vjo_vocr_tier_id(cfg->vocr_tier) ? vjo_vocr_tier_id(cfg->vocr_tier) : "-", cfg->dictionary,
           cfg->anki_mining);
    report(&g, "step boot t=%lld", (long long)ms_since_start());
    sample(&g, "boot");
    flush(&g);
}

void vjo_selftest_step(const char *step)
{
    Group g;
    if (!armed || finished)
        return;
    g.used = 0;
    report(&g, "step %s t=%lld", step, (long long)ms_since_start());
    sample(&g, step);
    flush(&g);
}

void vjo_selftest_game(const char *title, int enabled, int mode, const VjoRect *region)
{
    Group g;
    if (!armed || finished)
        return;
    g.used = 0;
    report(&g, "step game t=%lld title=%s enabled=%d mode=%d region=%s rect=%u,%u,%u,%u", (long long)ms_since_start(),
           title, enabled, mode, region && region->w ? "user" : "none", region ? region->x : 0u,
           region ? region->y : 0u, region ? region->w : 0u, region ? region->h : 0u);
    sample(&g, "game");
    flush(&g);
}

/* ---- the stages ---- */
static const struct {
    const char *name;
    uint32_t flags;
} stages[] = {
    /* In this order: the last stage leaves the configured size's model resident, so the
     * user's next job reads no weights; the probe, which also reads the frame
     * from user mode, after the main stages.
     * No stage uses the line cache, so each recognizes what it reads; textbox2
     * (STAGE_SERIAL) reads textbox's screen again serially, with the weights
     * resident, so the pair compares the configured helpers (ocr_threads) with
     * one thread (vjo_selftest_stage_speed). */
    {"fallback", VJO_GAME_OCR_F_FORCE_FALLBACK}, /* the user's region with the fallback model (the smallest size's read) */
    {"textbox", 0},                           /* the user's region; the configured size's weights read */
    {"fullscreen", VJO_GAME_OCR_F_FULL},      /* the whole frame, through the strips; the weights kept */
    {"probe", VJO_GAME_OCR_F_PROBE},          /* each capture path and memory type, one strip each */
    {"textbox2", 0},                          /* the user's region again, serially: the warm time; the job's text */
};
#define STAGE_SERIAL 4u /* textbox2 */

unsigned vjo_selftest_stages(void)
{
    return vjo_selftest_armed() ? (unsigned)(sizeof(stages) / sizeof(stages[0])) : 0u;
}

const char *vjo_selftest_stage_name(unsigned i)
{
    return i < sizeof(stages) / sizeof(stages[0]) ? stages[i].name : "?";
}

uint32_t vjo_selftest_stage_flags(unsigned i)
{
    return i < sizeof(stages) / sizeof(stages[0]) ? stages[i].flags : 0u;
}

uint32_t vjo_selftest_stage_speed(unsigned i, uint32_t configured)
{
    return (i == STAGE_SERIAL ? configured & ~VJO_GAME_OCR_S_HELPERS_MASK : configured) | VJO_GAME_OCR_S_NOCACHE;
}

void vjo_selftest_stage_begin(unsigned i, const char *model, const char *fallback, uint32_t speed)
{
    Group g;
    if (!vjo_selftest_armed())
        return;
    g.used = 0;
    report(&g, "stage %s begin t=%lld flags=0x%X model=%s fallback=%s speed=0x%X", vjo_selftest_stage_name(i),
           (long long)ms_since_start(), (unsigned)vjo_selftest_stage_flags(i), model, fallback[0] ? fallback : "-",
           (unsigned)speed);
    flush(&g);
}

/* #81's run began 2 s after the game started, on its opening notice: the five
 * lines the full screen found lay above the region, and all three region
 * stages found none (the same pixels each time; the region's later jobs read
 * text). So the stages need text in the region: a first stage that found no
 * line, or whose screen changed under every read, closes its begin with a wait
 * record instead, and the stages run on the next settled screen. */
static int waits; /* settled screens the stages waited past */

int vjo_selftest_stage_wait(unsigned i, int rc, const VjoGameOcrResult *r)
{
    Group g;
    if (!vjo_selftest_armed() || i != 0 || !((rc == VJO_OK && !r->lines) || rc == VJO_E_SCREEN_CHANGED) ||
        waits >= VJO_SELFTEST_WAITS)
        return 0;
    waits++;
    g.used = 0;
    report(&g, "stage %s wait t=%lld rc=%d screens=%d/%d", vjo_selftest_stage_name(i), (long long)ms_since_start(), rc,
           waits, VJO_SELFTEST_WAITS);
    flush(&g);
    return 1;
}

static const char *const step_names[] = {
    "none", "mem", "check", "alloc", "free", "pass", "tear", "sig", "find", "load", "recognize", "fallback",
    "fb", "probe", "settle", "blank", "norm", "lm", "cascade", "refine", "par", "clock", "pace", "shot", "save",
    "gate", "trim"};
static const char *const mem_names[] = {"-", "user", "cdram", "phycont", "kernel"};
static const char *const src_names[] = {"-", "hook", "kstrip", "user"};
static const char *const item_names[] = {"-", "capture", "workspace", "weights", "probe", "line", "lm", "cascade",
                                         "shot"};
/* Every step and block has its name (a new one without fails the build). */
typedef char vjo_step_names_complete[sizeof(step_names) / sizeof(step_names[0]) == VJO_GSTEP_COUNT ? 1 : -1];
typedef char vjo_item_names_complete[sizeof(item_names) / sizeof(item_names[0]) == VJO_GITEM_COUNT ? 1 : -1];

/* One step of the game worker's log as `step/rc/bytes/free` text: what the
 * step was, its native result, its bytes, the game's free memory after it. */
int vjo_gstep_format(char *buf, size_t cap, const char *who, const VjoGameOcrStep *s)
{
    const char *name = s->id < VJO_GSTEP_COUNT ? step_names[s->id] : "?";
    const char *kind = "-", *arg = "";
    char argbuf[16], kindbuf[8];
    if (s->id == VJO_GSTEP_ALLOC || s->id == VJO_GSTEP_FREE) {
        kind = s->kind < 5 ? mem_names[s->kind] : "?";
        arg = s->arg < VJO_GITEM_COUNT ? item_names[s->arg] : "?";
    } else if (s->id == VJO_GSTEP_PROBE) {
        kind = s->kind < 4 ? src_names[s->kind] : "?";
        arg = s->arg < 5 ? mem_names[s->arg] : "?";
    } else if (s->id == VJO_GSTEP_PASS || s->id == VJO_GSTEP_SIG || s->id == VJO_GSTEP_FB) {
        kind = s->kind < 4 ? src_names[s->kind] : "?";
        sceClibSnprintf(argbuf, sizeof(argbuf), "%u", (unsigned)s->arg);
        arg = argbuf;
    } else {
        if (s->id == VJO_GSTEP_CHECK || s->id == VJO_GSTEP_LM || s->id == VJO_GSTEP_CASCADE ||
            s->id == VJO_GSTEP_REFINE || s->id == VJO_GSTEP_SHOT || s->id == VJO_GSTEP_SAVE ||
            s->id == VJO_GSTEP_GATE || s->id == VJO_GSTEP_NORM || s->id == VJO_GSTEP_SETTLE ||
            ((s->id == VJO_GSTEP_RECOGNIZE || s->id == VJO_GSTEP_FIND) && s->kind)) {
            /* API 20: the LM tier (check: | 8 with the cascade), the refinement's mode; API 23: the
             * shot's JPEG quality, the save's chunks; API 26: the gate's mode; API 36: the norm step's lines
             * of a border dropped (core/textnorm.h); API 42: the share of the frames the head gate skipped,
             * percent (a job without it, or that skipped none, "-" as before); API 47: find's 1 columns, 2 rows
             * that looked like vertical text, 3 auto's columns without the vertical model (rows, "-", as before);
             * #353: settle's line watch (VJO_GSETTLE_K_*; 0 without it) */
            sceClibSnprintf(kindbuf, sizeof(kindbuf), "%u", (unsigned)s->kind);
            kind = kindbuf;
        }
        sceClibSnprintf(argbuf, sizeof(argbuf), "%u", (unsigned)s->arg);
        arg = argbuf;
    }
    return sceClibSnprintf(buf, cap, "gstep %s %s kind=%s arg=%s rc=0x%08X bytes=%u sum=0x%08X main=%d cdram=%d "
                           "phycont=%d us=%u", who, name, kind, arg, (unsigned)s->rc, (unsigned)s->bytes,
                           (unsigned)s->sum, (int)s->free_main, (int)s->free_cdram, (int)s->free_phycont,
                           (unsigned)s->us);
}

void vjo_selftest_stage_end(unsigned i, int rc, const VjoGameOcrResult *r, uint32_t ms, const char *model)
{
    static const char *const weights[] = {"none", "read", "resident"};
    const char *name = vjo_selftest_stage_name(i);
    char line[256];
    Group g;
    if (!vjo_selftest_armed())
        return;
    g.used = 0;
    for (uint32_t k = 0; k < r->n_steps && k < VJO_GAME_OCR_STEPS; k++) {
        if (vjo_gstep_format(line, sizeof(line), name, &r->step[k]) > 0)
            report(&g, "%s", line);
    }
    for (uint32_t k = 0; k < r->recognized && k < VJO_GAME_OCR_LINES; k++)
        report(&g, "line %s i=%u w=%u h=%u frames=%u us=%u", name, (unsigned)k, r->line[k].width, r->line[k].height,
               r->line[k].frames, (unsigned)r->line[k].us);
    {   /* text: FNV-1a of the shown text, so two stages' readings compare without the text */
        uint32_t h = VJO_FNV_INIT;
        for (const unsigned char *t = (const unsigned char *)r->text; t < (const unsigned char *)r->text +
             sizeof(r->text) && *t; t++)
            h = (h ^ *t) * 16777619u;
        report(&g, "stage %s end rc=%d t=%lld ms=%u region=%ux%u+%u+%u fb=%ux%u/%u/0x%08X model=%s fallback=%u lines=%u "
               "recognized=%u frames=%u text=0x%08X", name, rc, (long long)ms_since_start(), (unsigned)ms,
               (unsigned)r->width, (unsigned)r->height, (unsigned)r->region_x, (unsigned)r->region_y,
               (unsigned)r->fb_width, (unsigned)r->fb_height, (unsigned)r->fb_pitch, (unsigned)r->fb_format, model,
               (unsigned)r->fallback, (unsigned)r->lines, (unsigned)r->recognized, (unsigned)r->frames, (unsigned)h);
    }
    report(&g, "result %s source=%s mem=%s capture=%u strip_rows=%u strips=%u tears=%u capture_ms=%u workspace=%u "
           "resident=%u weights=%s find_ms=%u load_ms=%u recognize_ms=%u", name,
           r->source < 4 ? src_names[r->source] : "?", r->capture_memory < 5 ? mem_names[r->capture_memory] : "?",
           (unsigned)r->capture_bytes, (unsigned)r->strip_rows, (unsigned)r->captures, (unsigned)r->tears,
           (unsigned)(r->capture_us / 1000), (unsigned)r->workspace_bytes, (unsigned)r->resident_bytes,
           r->weights < 3 ? weights[r->weights] : "?", (unsigned)(r->find_us / 1000), (unsigned)(r->load_us / 1000),
           (unsigned)(r->recognize_us / 1000));
    report(&g, "memory %s free_user=%d reserve=%u need_workspace=%u need_weights=%u release=%u io=0x%08X stat=0x%08X "
           "cleanup=%d sum=0x%08X", name, (int)r->free_user, (unsigned)r->reserve_bytes, (unsigned)r->need_workspace,
           (unsigned)r->need_weights, (unsigned)r->release_bytes, (unsigned)r->io_error, (unsigned)r->stat_error,
           r->cleanup_status, (unsigned)r->region_sum);
    flush(&g);
}

void vjo_selftest_done(void)
{
    Group g;
    if (!vjo_selftest_armed())
        return;
    g.used = 0;
    sample(&g, "done");
    report(&g, "step done t=%lld", (long long)ms_since_start());
    flush(&g);
    finished = 1;
    sceIoRemove(MARKER); /* one run per arming */
}
