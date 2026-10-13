/* The on-device self-test (docs/self-test.md). Armed by one file,
 * ux0:data/VitaJPOverlay/selftest.txt (tools/install_ftp.sh --self-test),
 * it follows the next boot and the first game started: at each step it
 * writes one line to ux0:data/VitaJPOverlay/selftest-report.txt, with the
 * free memory of every pool we can read and the ledger. The report is
 * appended line by line, so a crash leaves the steps reached. The marker is
 * removed once the OCR step is in, so the test runs once; the report is what
 * the owner copies back for tools/analyse_selftest.py.
 *
 * It changes two things while armed: the region is the full screen (the
 * largest a job can need) and OCR runs automatically, so the first settled
 * screen of the game is recognized without any input. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "../core/boot_guard.h"
#include "../include/vjo_import.h"
#include "../include/vjo_ledger_budget.h"
#include "shell.h"

#define MARKER VJO_DATA_DIR "/selftest.txt"
#define REPORT VJO_DATA_DIR "/selftest-report.txt"
#define REPORT_CAP (32 * 1024)

#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static int armed;        /* the marker was present at this boot */
static int ocr_seen;     /* the OCR step is in: the test is complete */
static int report_bytes; /* -1: the report could not be opened */
static int64_t t0;

static void report(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    SceUID fd;
    int n;
    if (!armed || report_bytes < 0 || report_bytes > REPORT_CAP)
        return;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    fd = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    if (sceIoWrite(fd, line, n) == n)
        report_bytes += n;
    sceIoClose(fd);
    vjo_log_raw(line, n); /* status.txt too */
}

static int64_t ms_since_start(void)
{
    return ((int64_t)sceKernelGetProcessTimeWide() - t0) / 1000;
}

/* One memory sample: SceShell's budget as the user API reports it (minus a
 * system margin), the PAF heap, the Shell ledger, and the kernel's exact
 * figures for its own and for SceShell's partition. */
static void sample(const char *step)
{
    SceKernelFreeMemorySizeInfo f = {.size = sizeof(f)};
    VjoMemInfo m;
    const VjoLedger *l = &g_shell_ledger;
    int rc = sceKernelGetFreeMemorySize(&f);
    report("mem %s shell_main=%d shell_cdram=%d shell_phycont=%d rc=0x%08X paf_free=%u", step,
           rc < 0 ? -1 : f.size_user, rc < 0 ? -1 : f.size_cdram, rc < 0 ? -1 : f.size_phycont,
           (unsigned)rc, (unsigned)vjo_shell_heap_free_size());
    report("ledger %s pages=%u/%u paf=%u/%u", step,
           (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAGES], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAGES],
           (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAF], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAF]);
    sceClibMemset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    if (!IMPORT_READY(vjoMemInfo) || (rc = vjoMemInfo(&m)) < 0) {
        report("kernel %s unavailable rc=0x%08X", step, (unsigned)rc);
        return;
    }
    report("kernel %s kd_total=%u kd_free=%u kd_rc=0x%08X shell_total=%u shell_free=%u shell_rc=0x%08X "
           "ledger=%u/%u limit=%u capture=%u", step, (unsigned)m.kernel_total, (unsigned)m.kernel_free,
           (unsigned)m.kernel_rc, (unsigned)m.caller_total, (unsigned)m.caller_free, (unsigned)m.caller_rc,
           (unsigned)m.ledger_current, (unsigned)m.ledger_peak, (unsigned)m.ledger_limit, (unsigned)m.capture_bytes);
}

int vjo_selftest_armed(void)
{
    return armed && !ocr_seen;
}

void vjo_selftest_init(const VjoConfig *cfg)
{
    SceIoStat st;
    VjoBootInfo b = {.size = sizeof(b)};
    SceUID fd;
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
    report("selftest v1 api=%d", VJO_API_VERSION);
    if (IMPORT_READY(vjoBootInfo) && vjoBootInfo(&b) == 0)
        report("boot seq=%u mode=%d prev=%d/%d/%d/%d failures=%d write=0x%08X", (unsigned)b.seq, b.mode,
               b.prev_mode, b.prev_stage, b.prev_inert, b.prev_fault, b.failures, (unsigned)b.write_status);
    report("config backend=%d model=%s fallback=%s dictionary=%d anki=%d", cfg->ocr_backend, cfg->vocr_model,
           cfg->vocr_fallback_model, cfg->dictionary, cfg->anki_mining);
    report("step boot t=%lld", (long long)ms_since_start());
    sample("boot");
}

void vjo_selftest_step(const char *step)
{
    if (!armed || ocr_seen)
        return;
    report("step %s t=%lld", step, (long long)ms_since_start());
    sample(step);
}

void vjo_selftest_game(const char *title, int enabled, int mode)
{
    if (!armed || ocr_seen)
        return;
    report("step game t=%lld title=%s enabled=%d mode=%d region=full", (long long)ms_since_start(), title, enabled, mode);
    sample("game");
}

void vjo_selftest_ocr(const VjoState *st, const VjoGameOcrResult *r, int rc, const char *model)
{
    if (!armed || ocr_seen)
        return;
    report("step ocr t=%lld rc=%d model=%s fallback=%u %ux%u lines=%u recognized=%u frames=%u", (long long)ms_since_start(),
           rc, model, (unsigned)r->fallback, (unsigned)st->width, (unsigned)st->height, (unsigned)r->lines,
           (unsigned)r->recognized, (unsigned)r->frames);
    report("game free_user=%d reserve=%u need_workspace=%u need_weights=%u release=%u workspace=%u resident=%u "
           "weights=%u io=0x%08X stat=0x%08X cleanup=%d capture_rc=0x%08X find_ms=%u load_ms=%u recognize_ms=%u",
           (int)r->free_user, (unsigned)r->reserve_bytes, (unsigned)r->need_workspace, (unsigned)r->need_weights,
           (unsigned)r->release_bytes, (unsigned)r->workspace_bytes, (unsigned)r->resident_bytes, (unsigned)r->weights,
           (unsigned)r->io_error, (unsigned)r->stat_error, r->cleanup_status, (unsigned)st->capture_result,
           (unsigned)(r->find_us / 1000), (unsigned)(r->load_us / 1000), (unsigned)(r->recognize_us / 1000));
    sample("ocr");
    report("step done t=%lld", (long long)ms_since_start());
    ocr_seen = 1;
    sceIoRemove(MARKER); /* one run per arming */
}

void vjo_selftest_fail(const char *what, int rc)
{
    if (!armed || ocr_seen)
        return;
    report("fail %s rc=%d t=%lld", what, rc, (long long)ms_since_start());
}
