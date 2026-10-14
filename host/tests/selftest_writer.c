/* The real self-test writer (shell/selftest.c) on the host, to produce v2
 * reports exactly as the Shell writes them, for tools/analyse_selftest.py's
 * tests (tools/tests/test_analyse_selftest.py) and its committed fixtures.
 *
 *   vjo-selftest-writer OUT_DIR pass|fail|incomplete
 *
 * writes OUT_DIR/selftest-report.txt. The figures are synthetic: they exercise
 * the report's format and the analyser's verdicts, they measure nothing. */
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
static int memory_routes_fail;

static void host_path(const char *path, char *buf, size_t cap)
{
    const char *base = strrchr(path, '/');
    snprintf(buf, cap, "%s/%s", out_dir, base ? base + 1 : path);
}
int sceIoOpen(const char *path, int flags, unsigned mode)
{
    char p[1024];
    host_path(path, p, sizeof(p));
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
void vjo_log_raw(const char *line, int n) { (void)line; (void)n; }
void vjo_log(const char *fmt, ...) { (void)fmt; }
int vjoBootInfo(VjoBootInfo *b)
{
    memset(b, 0, sizeof(*b));
    b->size = sizeof(*b);
    b->seq = 2;
    return 0;
}
int vjoMemInfo(VjoMemInfo *m)
{
    m->ledger_current = m->ledger_peak = 131072;
    m->ledger_limit = 2220032;
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

/* A region job's result: the capture in strips through the hook into USER. */
static void region_job(VjoGameOcrResult *r, int full, int weights, int fallback, int fail_workspace)
{
    unsigned w = full ? 960 : 917, h = full ? 544 : 125, rows = full ? 59 : 62;
    unsigned cap = full ? 61440 : 61440, ws = 188416, lines = full ? 5 : 3;
    int32_t freem = 91226112;
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
    r->width = w; r->height = h; r->region_x = full ? 0 : 22; r->region_y = full ? 0 : 400;
    r->fb_width = 960; r->fb_height = 544; r->fb_pitch = 960; r->fb_format = 0;
    r->free_user = freem; r->reserve_bytes = 4194304;
    r->need_workspace = ws + cap; r->need_weights = weights == 1 ? (fallback ? 659456 : 1159168) : 0;
    add(r, VJO_GSTEP_MEM, 0, 0, 0, 0, freem, 10);
    add(r, VJO_GSTEP_CHECK, 0, 0, 0, r->need_workspace + r->need_weights + r->reserve_bytes, freem, 20);
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
    add(r, VJO_GSTEP_PASS, VJO_GAME_OCR_SRC_HOOK, (h + rows - 1) / rows, 0, w * h, freem, full ? 160000 : 34000);
    add(r, VJO_GSTEP_SIG, VJO_GAME_OCR_SRC_HOOK, 0, 0, 960, freem, 0)->sum = 960u << 16 | 544u;
    add(r, VJO_GSTEP_FIND, 0, lines, 0, 0, freem, full ? 48000 : 9000);
    if (weights == 1) add(r, VJO_GSTEP_LOAD, 0, 0, 0, r->need_weights, freem, fallback ? 610000 : 1050000);
    if (fallback) add(r, VJO_GSTEP_FALLBACK, 0, 1, 0, 0, freem, 0);
    add(r, VJO_GSTEP_RECOGNIZE, 0, lines, 0, 0, freem, full ? 1400000 : 820000);
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_WORKSPACE, 0, ws, freem += (int32_t)ws, 0);
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_CAPTURE, 0, cap, freem += (int32_t)cap, 0);
    r->source = VJO_GAME_OCR_SRC_HOOK; r->capture_memory = VJO_GAME_OCR_MEM_USER; r->capture_bytes = cap;
    r->strip_rows = rows; r->captures = 2u * ((h + rows - 1) / rows) + lines; r->capture_us = full ? 160000 : 34000;
    r->workspace_bytes = ws; r->resident_bytes = weights ? (fallback ? 659456 : 1159168) : 0;
    r->weights = (uint32_t)weights; r->fallback = (uint32_t)fallback;
    r->lines = lines; r->recognized = lines; r->frames = 110 * lines;
    r->find_us = full ? 48000 : 9000; r->load_us = weights == 1 ? (fallback ? 610000 : 1050000) : 0;
    r->recognize_us = full ? 1400000 : 820000; r->region_sum = 0x5EED0000u | lines;
    for (unsigned i = 0; i < lines; i++) {
        r->line[i].width = (uint16_t)(w - 40 - 60 * i); r->line[i].height = 30;
        r->line[i].frames = 110; r->line[i].us = (r->recognize_us / lines);
    }
}

/* The probe: each capture path into each memory type, one strip each. */
static void probe_job(VjoGameOcrResult *r)
{
    int32_t freem = 91226112;
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
    r->width = 917; r->height = 125; r->region_x = 22; r->region_y = 400;
    r->fb_width = 960; r->fb_height = 544; r->fb_pitch = 960;
    add(r, VJO_GSTEP_MEM, 0, 0, 0, 0, freem, 10);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_USER, VJO_GITEM_PROBE, 0x51, 61440, freem - 61440, 20);
    add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_HOOK, VJO_GAME_OCR_MEM_USER, 62, 56854, freem - 61440, 16500)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_KSTRIP, VJO_GAME_OCR_MEM_USER, 62, 56854, freem - 61440, 16900)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_USER, VJO_GAME_OCR_MEM_USER, 62, 56854, freem - 61440, 900)->sum = 0x1234ABCDu;
    add(r, VJO_GSTEP_FREE, VJO_GAME_OCR_MEM_USER, VJO_GITEM_PROBE, 0, 61440, freem, 0);
    add(r, VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_CDRAM, VJO_GITEM_PROBE, 0x52, 262144, freem, 30);
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
    int stop_in_fullscreen;
    int fd;
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUT_DIR pass|fail|incomplete|routes\n", argv[0]);
        return 2;
    }
    snprintf(out_dir, sizeof(out_dir), "%s", argv[1]);
    scenario = argv[2];
    stop_in_fullscreen = !strcmp(scenario, "incomplete"); /* the Vita stops during the full-screen stage */
    memory_routes_fail = !strcmp(scenario, "routes");
    /* the marker tools/install_ftp.sh --self-test leaves */
    fd = sceIoOpen(MARKER, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return 1;
    sceIoClose(fd);
    memset(&cfg, 0, sizeof(cfg));
    cfg.ocr_backend = 2; cfg.dictionary = 3;
    snprintf(cfg.vocr_model, sizeof(cfg.vocr_model), "H15_w8.vocr");
    snprintf(cfg.vocr_fallback_model, sizeof(cfg.vocr_fallback_model), "FL10_w8.vocr");
    vjo_selftest_init(&cfg);
    now_us = 61000000;
    vjo_selftest_step("ok");
    now_us = 143000000;
    vjo_selftest_game("PCSG00415", 1, 1, &region);
    for (unsigned i = 0; i < vjo_selftest_stages(); i++) {
        const char *name = vjo_selftest_stage_name(i);
        int fallback = !strcmp(name, "fallback");
        vjo_selftest_stage_begin(i, "H15_w8.vocr", "FL10_w8.vocr");
        if (stop_in_fullscreen && !strcmp(name, "fullscreen")) return 0; /* nothing after the begin line */
        if (!strcmp(name, "probe")) probe_job(&result);
        else region_job(&result, !strcmp(name, "fullscreen"),
                        fallback || !strcmp(name, "textbox") ? 1 : 2, fallback,
                        !strcmp(scenario, "fail") && !strcmp(name, "textbox"));
        now_us += 2500000;
        vjo_selftest_stage_end(i, result.rc, &result, 2500, fallback ? "FL10_w8.vocr" : "H15_w8.vocr");
    }
    vjo_selftest_done();
    return 0;
}
