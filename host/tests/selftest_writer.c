/* The real self-test writer (shell/selftest.c) on the host, to produce v2
 * reports exactly as the Shell writes them, for tools/analyse_selftest.py's
 * tests (tools/tests/test_analyse_selftest.py) and its committed fixtures.
 *
 *   vjo-selftest-writer OUT_DIR pass|fail|incomplete|routes|diagnostics|waited|notext|waiting
 *
 * writes OUT_DIR/selftest-report.txt. The figures are synthetic: they exercise
 * the report's format and the analyser's verdicts, they measure nothing.
 * `diagnostics`: OCR works in every stage while every diagnostic a passing run can
 * warn does (probe, capture<64KiB, kernel-pool, kernel-memory; tearing and game-memory
 * cannot warn when OCR passes): each
 * kernel memory route refused at step 3, as #75's first guard refused every
 * kernel object on a console (the audit's B1), the display hook unable to write
 * the game's memory (the kernel strip used), and USER_RW refusing the
 * full-screen stage's capture block (CDRAM's 256 KiB block instead).
 * `waited`: three settled screens without text in the region come first (their
 * wait records; the second one changed under every read), then the stages pass. `notext`: as #81's run, no screen with
 * text in the region: after VJO_SELFTEST_WAITS waits the stages run anyway,
 * their region stages find no line and the full screen five. `waiting`: the
 * report ends while the stages still wait (the marker stays). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>

typedef struct {
    SceSize size;
    int size_user, size_cdram, size_phycont;
} SceKernelFreeMemorySizeInfo;
#define sceClibVsnprintf vsnprintf

static char out_dir[512];
static uint64_t now_us = 1000;
static unsigned report_appends; /* opens of the report to append: one a record (ocr-first) */
static int memory_routes_fail;
static int diagnostics_warn; /* "diagnostics": every diagnostic a passing run can warn does, OCR works */

static void host_path(const char *path, char *buf, size_t cap)
{
    const char *base = strrchr(path, '/');
    snprintf(buf, cap, "%s/%s", out_dir, base ? base + 1 : path);
}
int sceIoOpen(const char *path, int flags, unsigned mode)
{
    char p[1024];
    host_path(path, p, sizeof(p));
    if ((flags & SCE_O_APPEND) && strstr(path, "selftest-report.txt")) report_appends++;
    return open(p, flags, mode);
}
int sceIoClose(int fd) { return close(fd); }
int sceIoWrite(int fd, const void *data, size_t len) { return (int)write(fd, data, len); }
int sceIoGetstat(const char *path, SceIoStat *st)
{
    char p[1024];
    struct stat s;
    host_path(path, p, sizeof(p));
    if (stat(p, &s) < 0) return (int)0x80010002u;
    st->st_mode = s.st_mode;
    return 0;
}
int sceIoRemove(const char *path)
{
    char p[1024];
    host_path(path, p, sizeof(p));
    return unlink(p);
}
uint64_t sceKernelGetProcessTimeWide(void) { return now_us; }
/* SceShell as #74 measured it after boot: nothing free above the 2 MiB margin. */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *f)
{
    f->size_user = now_us < 2000000 ? 7340032 : -2097152;
    f->size_cdram = now_us < 2000000 ? 4194304 : 0;
    f->size_phycont = 0;
    return 0;
}

#include "../../shell/selftest.c"

VjoLedger g_shell_ledger;
uint32_t vjo_shell_heap_free_size(void) { return 2346312; }
static unsigned log_bytes; /* what the report mirrored into status.txt and log.txt */
void vjo_log_raw(const char *line, int n) { (void)line; log_bytes += (unsigned)n; }
void vjo_log(const char *fmt, ...) { (void)fmt; }
int vjoBootInfo(VjoBootInfo *b)
{
    memset(b, 0, sizeof(*b));
    b->size = sizeof(*b);
    b->seq = 2;
    return 0;
}
/* The record so far is in the report before the kernel's memory routes run (a crash in
 * them keeps it): its last line is the sample's ledger line. */
static unsigned unflushed_samples;
static void check_flushed_before_meminfo(void)
{
    char p[1024], buf[65536];
    FILE *f;
    size_t n;
    host_path("selftest-report.txt", p, sizeof(p));
    f = fopen(p, "rb");
    n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    {
        const char *last = strrchr(buf, '\n');
        last = last ? last + 1 : buf;
        if (strncmp(last, "ledger ", 7)) unflushed_samples++;
    }
}
int vjoMemInfo(VjoMemInfo *m)
{
    check_flushed_before_meminfo();
    m->ledger_current = m->ledger_peak = 131072;
    m->ledger_limit = 225280; /* the debug build's VJO_LIMIT_KERNEL since API 24 (synthetic figures) */
    if (diagnostics_warn) { /* #75's first guard on a console: every kernel object refused at step 3 */
        m->kernel_rc = m->caller_rc = m->cas_rc = VJO_ERR_ARG;
        m->kernel_step = m->caller_step = m->cas_step = VJO_MEMSTEP_RANGE;
        m->as_rc = VJO_ERR_FORMAT; m->as_step = VJO_MEMSTEP_RANGE; m->as_skipped = 2;
        return 0;
    }
    if (memory_routes_fail) { /* the block route stops at its memory-block info, the kernel's
                                 address space is refused and the caller's is read */
        m->kernel_rc = m->caller_rc = (int32_t)0x80020001u;
        m->kernel_step = m->caller_step = VJO_MEMSTEP_INFO;
        m->as_caller = 1;
    } else {
        m->kernel_total = 37748736; m->kernel_free = 9437184; m->kernel_step = 0;
        m->caller_total = 52428800; m->caller_free = 98304;
    }
    m->cas_main_total = 52428800; m->cas_main_free = 98304;
    m->cas_cdram_total = 4194304; m->cas_cdram_free = 0;
    m->cas_phycont_total = 0; m->cas_phycont_free = 0;
    m->as_parts = 2;
    m->as_total[0] = 37748736; m->as_free[0] = 9437184;
    m->as_total[1] = 16777216; m->as_free[1] = 4194304;
    m->as_skipped = 1; m->as_step = VJO_MEMSTEP_RANGE; /* one slot outside the kernel half, not read */
    return 0;
}

static VjoGameOcrStep *add(VjoGameOcrResult *r, unsigned id, unsigned kind, unsigned arg, int32_t rc, uint32_t bytes,
                           int32_t free_main, uint32_t us)
{
    VjoGameOcrStep *s = &r->step[r->n_steps++];
    s->id = (uint8_t)id; s->kind = (uint8_t)kind; s->arg = (uint16_t)arg;
    s->rc = rc; s->bytes = bytes; s->free_main = free_main; s->free_cdram = 117440512; s->us = us;
    return s;
}

/* A region job's result: the capture in strips through the hook into USER;
 * kstrip: through the kernel strip after the hook could not write (the ladder's
 * last rung); cdram: USER_RW refused the capture block, CDRAM gave one. */
/* serial: textbox2, which the self-test runs without helpers (shell/selftest.c); the other
 * region stages have the configured two helpers and log their PAR step. */
static void region_job(VjoGameOcrResult *r, int full, int weights, int fallback, int fail_workspace, int kstrip,
                       int cdram, int serial)
{
    unsigned w = full ? 960 : 917, h = full ? 544 : 125, rows = full ? 59 : 62;
    unsigned cap = full ? 61440 : 61440, ws = 188416, lines = full ? 5 : 3;
    unsigned src = kstrip ? VJO_GAME_OCR_SRC_KSTRIP : VJO_GAME_OCR_SRC_HOOK;
    unsigned cmem = cdram ? VJO_GAME_OCR_MEM_CDRAM : VJO_GAME_OCR_MEM_USER;
    unsigned held = cdram ? 262144 : cap;
    int32_t freem = 91226112;
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
    r->width = w; r->height = h; r->region_x = full ? 0 : 22; r->region_y = full ? 0 : 400;
    r->fb_width = 960; r->fb_height = 544; r->fb_pitch = 960; r->fb_format = 0;
    r->free_user = freem; r->reserve_bytes = 4194304; /* 0.8.0's H15 figures: its 4 MiB reserve (1 MiB since #280) */
    r->need_workspace = ws + cap; r->need_weights = weights == 1 ? (fallback ? 659456 : 1159168) : 0;
    add(r, VJO_GSTEP_MEM, 0, 0, 0, 0, freem, 10);
    add(r, VJO_GSTEP_CHECK, 0, 0, 0, r->need_workspace + r->need_weights + r->reserve_bytes, freem, 20);
    if (fallback) /* the fallback's check, which the self-test forced */
        add(r, VJO_GSTEP_CHECK, 0, 1, 0, r->need_workspace + r->need_weights + r->reserve_bytes, freem, 20)->sum = 1;
    if (cdram) {
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, (int32_t)0x80024302u, cap, freem, 30);
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_CDRAM, VJO_GITEM_CAPTURE, 0x41, held, freem, 31)->free_cdram -= 262144;
    } else
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, 0x41, cap, freem -= (int32_t)cap, 30);
    if (fail_workspace) {
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WORKSPACE, (int32_t)0x80024302u, ws, freem, 40);
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_CDRAM, VJO_GITEM_WORKSPACE, (int32_t)0x80024302u, 262144, freem, 50);
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_PHYCONT, VJO_GITEM_WORKSPACE, (int32_t)0x80024302u, 1048576, freem, 60);
        add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, 0, cap, freem += (int32_t)cap, 70);
        r->rc = VJO_E_OOM; r->capture_memory = VJO_GAME_OCR_MEM_USER; r->capture_bytes = cap;
        return;
    }
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WORKSPACE, 0x42, ws, freem -= (int32_t)ws, 40);
    if (weights == 1)
        add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WEIGHTS, 0x43, r->need_weights,
            freem -= (int32_t)r->need_weights, 50);
    add(r, VJO_GSTEP_FB, VJO_GAME_OCR_SRC_HOOK, 960, 0, 960, freem, 16000)->sum = 0;
    if (kstrip) add(r, VJO_GSTEP_PASS, VJO_GAME_OCR_SRC_HOOK, 0, VJO_ERR_COPY, 0, freem, 16000);
    add(r, VJO_GSTEP_PASS, src, (h + rows - 1) / rows, 0, w * h, freem, full ? 160000 : 34000);
    add(r, VJO_GSTEP_SIG, src, 0, 0, 960, freem, 0)->sum = 960u << 16 | 544u;
    add(r, VJO_GSTEP_FIND, 0, lines, 0, 0, freem, full ? 48000 : 9000);
    if (weights == 1) add(r, VJO_GSTEP_LOAD, 0, 0, 0, r->need_weights, freem, fallback ? 610000 : 1050000);
    add(r, VJO_GSTEP_RECOGNIZE, 0, lines, 0, 0, freem, serial ? 1500000 : full ? 1400000 : 820000);
    if (!serial) /* two helpers, two lines on them, 95-98 % of a core each */
        add(r, VJO_GSTEP_PAR, 0, 2, 2, 548864, freem, serial ? 1500000 : full ? 1400000 : 820000)->sum = 95u << 8 | 98u;
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WORKSPACE, 0, ws, freem += (int32_t)ws, 0);
    if (cdram) add(r, VJO_GSTEP_FREE, cmem, VJO_GITEM_CAPTURE, 0, held, freem, 0);
    else add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, 0, cap, freem += (int32_t)cap, 0);
    r->source = src; r->capture_memory = cmem; r->capture_bytes = held;
    r->strip_rows = rows; r->captures = 2u * ((h + rows - 1) / rows) + lines; r->capture_us = full ? 160000 : 34000;
    r->workspace_bytes = ws; r->resident_bytes = weights ? (fallback ? 659456 : 1159168) : 0;
    r->weights = (uint32_t)weights; r->fallback = (uint32_t)fallback;
    r->lines = lines; r->recognized = lines; r->frames = 110 * lines;
    r->find_us = full ? 48000 : 9000; r->load_us = weights == 1 ? (fallback ? 610000 : 1050000) : 0;
    r->recognize_us = serial ? 1500000 : full ? 1400000 : 820000; r->region_sum = 0x5EED0000u | lines;
    snprintf(r->text, sizeof(r->text), "%s", full ? "full screen" : fallback ? "fallback text" : "region text");
    for (unsigned i = 0; i < lines; i++) {
        r->line[i].width = (uint16_t)(w - 40 - 60 * i); r->line[i].height = 30;
        r->line[i].frames = 110; r->line[i].us = (r->recognize_us / lines);
    }
}

/* A region job on a screen with no text in the region (#81: the game's opening
 * notice, its lines above the region): the finder finds no line, so the model
 * file is not opened and nothing is recognized. */
static void empty_region_job(VjoGameOcrResult *r, int fallback, int resident)
{
    unsigned cap = 61440, ws = fallback ? 131072 : 188416;
    int32_t freem = 91226112;
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
    r->width = 917; r->height = 125; r->region_x = 22; r->region_y = 400;
    r->fb_width = 960; r->fb_height = 544; r->fb_pitch = 960; r->fb_format = 0;
    r->free_user = freem; r->reserve_bytes = 4194304; /* 0.8.0's H15 figures: its 4 MiB reserve (1 MiB since #280) */
    r->need_workspace = ws + cap; r->need_weights = resident ? 0 : fallback ? 659456 : 1159168;
    r->fallback = (uint32_t)fallback;
    r->resident_bytes = resident ? 1159168 : 0;
    add(r, VJO_GSTEP_MEM, 0, 0, 0, 0, freem, 10);
    add(r, VJO_GSTEP_CHECK, 0, 0, 0, r->need_workspace + r->need_weights + r->reserve_bytes, freem, 20);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, 0x41, cap, freem - (int32_t)cap, 30);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WORKSPACE, 0x42, ws, freem - (int32_t)(cap + ws), 40);
    add(r, VJO_GSTEP_PASS, VJO_GAME_OCR_SRC_HOOK, 3, 0, 917 * 125, freem - (int32_t)(cap + ws), 34000);
    add(r, VJO_GSTEP_SIG, VJO_GAME_OCR_SRC_HOOK, 0, 0, 960, freem - (int32_t)(cap + ws), 0)->sum = 960u << 16 | 544u;
    add(r, VJO_GSTEP_FIND, 0, 0, 0, 0, freem - (int32_t)(cap + ws), 9000);
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WORKSPACE, 0, ws, freem - (int32_t)cap, 0);
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, 0, cap, freem, 0);
    r->source = VJO_GAME_OCR_SRC_HOOK; r->capture_memory = VJO_GAME_OCR_MEM_USER; r->capture_bytes = cap;
    r->strip_rows = 62; r->captures = 6; r->capture_us = 34000; r->workspace_bytes = ws;
    r->find_us = 9000; r->region_sum = 0x5EED0000u;
}

/* The probe: each capture path into each memory type, one strip each; with
 * hook_fails the display hook cannot write the game's memory. */
static void probe_job(VjoGameOcrResult *r, int hook_fails)
{
    int32_t freem = 91226112;
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
    r->width = 917; r->height = 125; r->region_x = 22; r->region_y = 400;
    r->fb_width = 960; r->fb_height = 544; r->fb_pitch = 960;
    add(r, VJO_GSTEP_MEM, 0, 0, 0, 0, freem, 10);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_PROBE, 0x51, 61440, freem - 61440, 20);
    if (hook_fails)
        add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_HOOK, VJO_GAME_OCR_MEM_USER, VJO_ERR_COPY, 0, freem - 61440, 16500);
    else
        add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_HOOK, VJO_GAME_OCR_MEM_USER, 62, 56854, freem - 61440, 16500)->sum =
            0x1234ABCDu;
    add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_KSTRIP, VJO_GAME_OCR_MEM_USER, 62, 56854, freem - 61440, 16900)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_USER, VJO_GAME_OCR_MEM_USER, 62, 56854, freem - 61440, 900)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_PROBE, 0, 61440, freem, 0);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_CDRAM, VJO_GITEM_PROBE, 0x52, 262144, freem, 30);
    if (hook_fails)
        add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_HOOK, VJO_GAME_OCR_MEM_CDRAM, VJO_ERR_COPY, 0, freem, 16400);
    else
        add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_HOOK, VJO_GAME_OCR_MEM_CDRAM, 62, 56854, freem, 16400)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_KSTRIP, VJO_GAME_OCR_MEM_CDRAM, 62, 56854, freem, 16800)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_CDRAM, VJO_GITEM_PROBE, 0, 262144, freem, 0);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_PHYCONT, VJO_GITEM_PROBE, (int32_t)0x80024302u, 1048576, freem, 40);
    r->strip_rows = 62;
}

int main(int argc, char **argv)
{
    static VjoGameOcrResult result;
    VjoConfig cfg;
    VjoRect region = {1536, 48188, 62600, 15059};
    const char *scenario;
    int stop_in_fullscreen, no_text, empty_screens;
    int fd;
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUT_DIR pass|fail|incomplete|routes|diagnostics|waited|notext|waiting|gstep\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[2], "gstep")) { /* the step formatter alone, on stdout: API 20's check, lm, cascade, refine kinds; API 26's gate */
        static const VjoGameOcrStep steps[] = {{.id = VJO_GSTEP_CHECK, .kind = 2 | 8, .bytes = 4096},
                                               {.id = VJO_GSTEP_LM, .kind = 2, .arg = 1, .us = 2100},
                                               {.id = VJO_GSTEP_CASCADE, .kind = 1, .arg = 1, .us = 550001},
                                               {.id = VJO_GSTEP_RECOGNIZE, .kind = 0, .arg = 1, .us = 820000},
                                               /* API 42: the head gate skipped 61% of the job's 240 frames */
                                               {.id = VJO_GSTEP_RECOGNIZE, .kind = 61, .arg = 3, .bytes = 240,
                                                .us = 640000},
                                               {.id = VJO_GSTEP_REFINE, .kind = 2, .arg = 16, .bytes = 9u << 16 | 2u << 8 | 7u,
                                                .sum = 410, .us = 1400},
                                               /* API 26: the gate stopped 2 lines, score -60, threshold -8,
                                                * its weights read in this job */
                                               {.id = VJO_GSTEP_GATE, .kind = 1, .arg = 2,
                                                .bytes = (uint32_t)(-60 + 129) | (uint32_t)(-8 + 129) << 16 | VJO_GLM_READ,
                                                .sum = 112, .us = 9001},
                                               /* the text normalised: a full-width form, a dash and a line of a
                                                * border dropped (its kind) */
                                               {.id = VJO_GSTEP_NORM, .kind = 1, .arg = 3, .bytes = 1, .sum = 1u << 16,
                                                .us = 41}};
        char line[256];
        for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
            if (vjo_gstep_format(line, sizeof(line), "job", &steps[i]) > 0) puts(line);
        return 0;
    }
    snprintf(out_dir, sizeof(out_dir), "%s", argv[1]);
    scenario = argv[2];
    stop_in_fullscreen = !strcmp(scenario, "incomplete"); /* the Vita stops during the full-screen stage */
    memory_routes_fail = !strcmp(scenario, "routes");
    diagnostics_warn = !strcmp(scenario, "diagnostics");
    /* Settled screens without text in the region before the stages run: three
     * (waited); the wait's bound, after which they run on a screen without any
     * (notext, #81's run); and a player who never reaches text (waiting). */
    no_text = !strcmp(scenario, "notext");
    empty_screens = !strcmp(scenario, "waited") ? 3 : no_text ? VJO_SELFTEST_WAITS : !strcmp(scenario, "waiting") ? 4 : 0;
    /* the marker tools/install_ftp.sh --self-test leaves */
    fd = sceIoOpen(MARKER, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return 1;
    sceIoClose(fd);
    memset(&cfg, 0, sizeof(cfg));
    cfg.vocr_tier = VJO_VOCR_TIER_NORMAL; /* the config record names its model and the smallest tier's */
    vjo_selftest_init(&cfg);
    now_us = 61000000;
    vjo_selftest_step("ok");
    now_us = 143000000;
    vjo_selftest_game("PCSG00415", 1, 1, &region);
    for (int k = 0; k < empty_screens; k++) { /* as shell/worker.c run_selftest_stages: the first stage, then the gate */
        vjo_selftest_stage_begin(0, "H15_w8.vocr", "FL10_w8.vocr", vjo_selftest_stage_speed(0, 2u));
        empty_region_job(&result, 1, 0);
        if (k == 1 && !no_text) { /* text typing out: the screen changed under every read */
            result.rc = VJO_E_SCREEN_CHANGED;
            result.tears = 3;
        }
        now_us += 600000;
        if (!vjo_selftest_stage_wait(0, result.rc, &result)) {
            fprintf(stderr, "screen %d without text did not wait\n", k + 1);
            return 3;
        }
        now_us += 4000000;
    }
    if (!strcmp(scenario, "waiting")) return 0; /* the marker stays: the stages still wait */
    if (!strncmp(scenario, "cap", 3)) { /* the 48 KiB cap: more stage records than fit (records of about 3.7 KB);
                                           capN: N step records of about 1 KB first, to move where the cap falls */
        for (int k = 0; k < (scenario[3] >= '0' && scenario[3] <= '9' ? scenario[3] - '0' : 0); k++)
            vjo_selftest_step("ok");
        for (unsigned k = 0; k < 20; k++) {
            vjo_selftest_stage_begin(1, "H15_w8.vocr", "FL10_w8.vocr", vjo_selftest_stage_speed(1, 2u));
            region_job(&result, 0, 1, 0, 0, 0, 0, 0);
            now_us += 2500000;
            vjo_selftest_stage_end(1, result.rc, &result, 2500, "H15_w8.vocr");
        }
        printf("report appends %u, log bytes %u, unflushed samples %u\n", report_appends, log_bytes,
               unflushed_samples);
        return 0;
    }
    for (unsigned i = 0; i < vjo_selftest_stages(); i++) {
        const char *name = vjo_selftest_stage_name(i);
        int fallback = !strcmp(name, "fallback"), full = !strcmp(name, "fullscreen");
        vjo_selftest_stage_begin(i, "H15_w8.vocr", "FL10_w8.vocr", vjo_selftest_stage_speed(i, 2u)); /* ocr_threads 3 */
        if (stop_in_fullscreen && full) return 0; /* nothing after the begin line */
        if (!strcmp(name, "probe")) probe_job(&result, diagnostics_warn);
        else if (no_text && !full) empty_region_job(&result, fallback, !strcmp(name, "textbox2"));
        else region_job(&result, full,
                        fallback || !strcmp(name, "textbox") ? 1 : 2, fallback,
                        !strcmp(scenario, "fail") && !strcmp(name, "textbox"), diagnostics_warn,
                        diagnostics_warn && full, !strcmp(name, "textbox2"));
        now_us += 2500000;
        if (vjo_selftest_stage_wait(i, result.rc, &result)) {
            fprintf(stderr, "stage %s waited\n", name);
            return 3;
        }
        vjo_selftest_stage_end(i, result.rc, &result, 2500, fallback ? "FL10_w8.vocr" : "H15_w8.vocr");
    }
    vjo_selftest_done();
    printf("report appends %u, log bytes %u, unflushed samples %u\n", report_appends, log_bytes,
           unflushed_samples); /* the fixtures' test reads them */
    return 0;
}
