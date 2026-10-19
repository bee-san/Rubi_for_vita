#include "anki_page.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "anki_afv.h"
#include "anki_export.h"
#include "anki_index_request.h"
#include "preferences.h"
#include "../core/stick_scroll.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

#define WORKER_STACK (64 * 1024)

/* Five rows (API 23 added Screenshots, #220 Send to Anki for Vita), each a title and a line of help,
 * then the 3-line status box, which ends above the controls line main.c draws at y 535. */
#define ROW_Y(r) (104 + (r) * 63)
#define ROW_H 59
#define TITLE_DY 27
#define HELP_DY 52
#define BADGE_DY 5
#define BADGE_H 28
#define MINING_Y ROW_Y(0)
#define SHOT_Y ROW_Y(1) /* API 23: the card's screenshot on/off */
#define EXPORT_Y ROW_Y(2)
#define ALL_Y ROW_Y(3)
#define AFV_Y ROW_Y(4)  /* #220: the cards to the Anki for Vita app, through its inbox */
#define FOOTER_Y 419
#define FOOTER_H 88
#define ROWS 5
#define AFV_ROW 4

enum { JOB_EXPORT, JOB_EXPORT_ALL, JOB_SEND, JOB_RESULTS };

static VjoConfig cfg;           /* the settings shown (main thread) */
static int writable, selected;
static int stick_move;          /* the left stick's move for the next frame (vjo_anki_page_stick) */
static VjoExportCounts counts = {-1, -1};
static char note[200];          /* the page's own status (saves) */
static int note_error;

/* Anki for Vita (anki_afv.h): installed (its interface.json, read when the page is shown), and its
 * status.json, which the worker reads (a reader may wait for a file being replaced: 3 x 100 ms). */
static int afv_installed;
static VjoAfvStatus afv_status;

/* The worker (lock): one job at a time, an export, a hand-over or the reading of Anki for Vita's
 * results. */
static SceUID lock = -1, worker = -1;
static int job, running, finished, cancelled;
static char progress[160], result[480];
static int result_error;
static VjoAfvStatus job_status; /* status.json as the last Anki for Vita job read it (job_status_read) */
static int job_status_read;
static unsigned char export_mem[VJO_EXPORT_MEM] __attribute__((aligned(16)));

/* log.txt: settings/applog.c (settings/anki_export.c logs its storage errors there too). */

/* ---------------- the worker ---------------- */

static void set_progress(void *ud, const char *msg)
{
    (void)ud;
    sceKernelLockMutex(lock, 1, NULL);
    snprintf(progress, sizeof(progress), "%s", msg);
    sceKernelUnlockMutex(lock, 1);
}

static int stopped(void *ud)
{
    int c;
    (void)ud;
    sceKernelLockMutex(lock, 1, NULL);
    c = cancelled;
    sceKernelUnlockMutex(lock, 1);
    return c;
}

/* Anki for Vita's results (when the page is shown): what changed, for the footer, or nothing. */
/* status.json, for the page (after a results pass or a hand-over: what the app said last). */
static void read_status(VjoArena *arena)
{
    VjoAfvStatus st;
    vjo_afv_status_read(arena, &st);
    sceKernelLockMutex(lock, 1, NULL);
    job_status = st;
    job_status_read = 1;
    sceKernelUnlockMutex(lock, 1);
}

static void run_results(VjoArena *arena, const VjoExportUi *ui)
{
    VjoAfvResults r;
    char msg[360];
    int rc = vjo_afv_results(arena, ui, &r);
    read_status(arena);
    vjo_afv_results_text(&r, msg, sizeof(msg));
    vjo_log("anki for vita: results rc=%d: %d in Anki, %d duplicates, %d refused, %d waiting, %d unreadable, "
            "%d failed, %d removed, %d swept, %d more, arena peak %u", rc, r.in_anki, r.duplicate, r.rejected,
            r.waiting, r.malformed, r.failed, r.removed, r.swept, r.more, (unsigned)arena->peak);
    sceKernelLockMutex(lock, 1, NULL);
    if (msg[0]) { /* (else the last result stays) */
        snprintf(result, sizeof(result), "%s", msg);
        result_error = r.rejected || r.failed || r.malformed;
    }
    sceKernelUnlockMutex(lock, 1);
}

static void run_send(VjoArena *arena, const VjoExportUi *ui)
{
    VjoAfvSendResult r;
    char msg[440];
    vjo_afv_send(arena, ui, &r, msg, sizeof(msg));
    if (r.installed)
        read_status(arena);
    vjo_log("anki for vita: send rc=%d: %d handed over, %d already, %d refused before, %d unreadable, %d too "
            "large, %d failed, %d without screenshots, %d left, %d stopped, arena peak %u", r.rc, r.sent, r.already,
            r.refused, r.unreadable, r.too_large, r.failed, r.missing_picture, r.left, r.stopped,
            (unsigned)arena->peak);
    sceKernelLockMutex(lock, 1, NULL);
    snprintf(result, sizeof(result), "%s", msg);
    result_error = r.rc != 0 || r.unreadable || r.too_large || r.failed || r.stopped || r.missing_picture ||
                   r.results.rejected || r.results.failed || r.results.malformed;
    sceKernelUnlockMutex(lock, 1);
}

static int worker_main(SceSize args, void *argp)
{
    static const VjoExportUi ui = {NULL, set_progress, stopped};
    VjoArena arena;
    VjoExportResult r;
    char msg[360];
    int what;
    size_t n;
    (void)args;
    (void)argp;
    sceKernelLockMutex(lock, 1, NULL);
    what = job;
    sceKernelUnlockMutex(lock, 1);
    vjo_arena_init(&arena, export_mem, sizeof(export_mem));
    if (what == JOB_SEND || what == JOB_RESULTS) {
        if (what == JOB_SEND)
            run_send(&arena, &ui);
        else
            run_results(&arena, &ui);
        sceKernelLockMutex(lock, 1, NULL);
        progress[0] = '\0';
        finished = 1;
        sceKernelUnlockMutex(lock, 1);
        return 0;
    }
    vjo_export_run(&arena, what == JOB_EXPORT_ALL, &ui, &r, msg, sizeof(msg));
    vjo_log("anki: export (%s) rc=%d: %d cards, %d moved, %d kept, %d unreadable, %d too large, %d left, "
            "%d stopped, %d swept, %u bytes, arena peak %u, %d without screenshots",
            what == JOB_EXPORT_ALL ? "all" : "queued", r.rc, r.cards, r.moved, r.kept, r.unreadable, r.too_large,
            r.left, r.stopped, r.swept, (unsigned)r.bytes, (unsigned)arena.peak, r.missing_picture);
    sceKernelLockMutex(lock, 1, NULL);
    n = (size_t)snprintf(result, sizeof(result), "%s", msg);
    if (r.rc == 0 && r.cards && n < sizeof(result))
        snprintf(result + n, sizeof(result) - n, " Copy it from %s/ by VitaShell's FTP or USB (or the SD2Vita "
                 "card, when it is ux0:) and open it in Anki.", VJO_EXPORT_DIR);
    result_error = r.rc != 0 || r.kept || r.unreadable || r.too_large || r.missing_picture;
    progress[0] = '\0';
    finished = 1;
    sceKernelUnlockMutex(lock, 1);
    return 0;
}

static void start(int what)
{
    int rc;
    if (running)
        return;
    if (lock < 0 && (lock = sceKernelCreateMutex("VjoAnkiPage", 0, 0, NULL)) < 0)
        return;
    job = what;
    finished = cancelled = 0;
    snprintf(progress, sizeof(progress), "%s",
             what == JOB_RESULTS ? "Reading Anki for Vita's results…" : "Reading the cards…");
    /* CPU3 at a system priority, as the main thread (settings/sysapp.c). */
    worker = sceKernelCreateThread("VjoAnkiExport", worker_main, 0x10000100, WORKER_STACK, 0,
                                   SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
    rc = worker < 0 ? worker : sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (worker >= 0)
            sceKernelDeleteThread(worker);
        worker = -1;
        snprintf(note, sizeof(note), "Could not start %s (0x%08X).",
                 what == JOB_SEND ? "the hand-over" : what == JOB_RESULTS ? "reading the results" : "the export",
                 (unsigned)rc);
        note_error = 1;
        return;
    }
    running = 1;
    if (what != JOB_RESULTS) /* (a page's own note stays while the results are read) */
        note[0] = '\0';
}

/* Is Anki for Vita there: its interface.json lists version 1. Its status comes from the worker. */
static void read_afv(VjoArena *scratch)
{
    afv_installed = vjo_afv_speaks_1(scratch);
    if (!afv_installed)
        memset(&afv_status, 0, sizeof(afv_status));
}

/* The worker is done: join it, count the cards again and take Anki for Vita's status it read. */
static void reap(void)
{
    int done;
    if (!running)
        return;
    sceKernelLockMutex(lock, 1, NULL);
    done = finished;
    sceKernelUnlockMutex(lock, 1);
    if (!done)
        return;
    sceKernelWaitThreadEnd(worker, NULL, NULL);
    sceKernelDeleteThread(worker);
    worker = -1;
    running = 0;
    vjo_export_count(&counts);
    sceKernelLockMutex(lock, 1, NULL);
    if (job_status_read)
        afv_status = job_status;
    job_status_read = 0;
    sceKernelUnlockMutex(lock, 1);
}

static void cancel(void)
{
    if (!running)
        return;
    sceKernelLockMutex(lock, 1, NULL);
    cancelled = 1;
    sceKernelUnlockMutex(lock, 1);
}

/* ---------------- settings ---------------- */

static void save(int mining, int screenshot, VjoArena *scratch)
{
    if (!writable) {
        snprintf(note, sizeof(note), "Cannot read config.ini. Choices have not been changed.");
        note_error = 1;
        return;
    }
    int rc = vjo_preferences_save_anki(mining, screenshot, scratch);
    int read_rc = vjo_preferences_load(&cfg, scratch);
    writable = read_rc >= 0;
    note_error = rc < 0 || read_rc < 0;
    snprintf(note, sizeof(note), "%s", note_error ? "Save not confirmed. Check the choices shown, then retry."
                                                  : "Saved. The overlay uses it when it next opens.");
}

void vjo_anki_page_show(VjoArena *scratch)
{
    /* (the log switch is the worker's too: it changes only while no export runs) */
    if (!running)
        vjo_log_enable(0);
    writable = vjo_preferences_load(&cfg, scratch) >= 0;
    if (!running) {
        char line[160];
        int request;
        vjo_log_enable(cfg.log_file);
        vjo_export_count(&counts);
        read_afv(scratch);
        /* Anki for Vita's duplicate index for the overlay: Rubi's request, rewritten only when it would
         * say something else (anki_index_request.h; also at the app's start) */
        request = vjo_anki_index_request(scratch, line, sizeof(line));
        if (request == VJO_ANKI_REQUEST_WRITTEN || request == VJO_ANKI_REQUEST_FAILED)
            vjo_log("%s", line);
        /* its results: the cards it has taken leave the queue (on the worker: many files) */
        if (afv_installed)
            start(JOB_RESULTS);
    }
}

/* ---------------- input and drawing ---------------- */

int vjo_anki_page_busy(void)
{
    /* Reading the results is not a job the user started: Cancel leaves the app meanwhile, and
     * vjo_anki_page_quit stops it at the next file. */
    return running && job != JOB_RESULTS;
}

static void activate(int row, VjoArena *scratch)
{
    /* The settings rows also while Anki for Vita's results are read (the worker never touches
     * config.ini); the jobs' rows once the worker is free. */
    if (running && (job != JOB_RESULTS || row >= 2))
        return;
    if (row == 0)
        save(!cfg.anki_mining, cfg.anki_screenshot, scratch);
    else if (row == 1)
        save(cfg.anki_mining, !cfg.anki_screenshot, scratch);
    else if (row == AFV_ROW && !afv_installed) {
        snprintf(note, sizeof(note), "Anki for Vita is not installed (or too old for Rubi). Get it from "
                 VJO_AFV_WHERE ": it syncs your cards with Anki.");
        note_error = 0;
    } else
        start(row == AFV_ROW ? JOB_SEND : row == 3 ? JOB_EXPORT_ALL : JOB_EXPORT);
}

void vjo_anki_page_stick(int move)
{
    stick_move = move;
}

static void count_text(char *out, size_t cap, int n, const char *what)
{
    if (n < 0)
        snprintf(out, cap, "Unreadable");
    else
        snprintf(out, cap, "%d card%s %s", n, n == 1 ? "" : "s", what);
}

/* A row: its card, title and help line (the help wrapped to one line). */
static void row(const VjoAnkiUi *ui, int y, int focus, unsigned color, const char *title, const char *help)
{
    ui->card(y, ROW_H, focus);
    ui->text(42, y + TITLE_DY, color, 28, title);
    ui->wrapped(42, y + HELP_DY, 870, ui->dim, 22, 24, 1, help);
}

void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    char line[260], busy_text[160], controls[96], last[480], help[200];
    int stick = stick_move, busy, err = 0, total, what;
    stick_move = 0;
    reap();
    busy = running;
    what = job;
    if (pressed & SCE_CTRL_UP)
        selected = (selected + ROWS - 1) % ROWS;
    if (pressed & SCE_CTRL_DOWN)
        selected = (selected + 1) % ROWS;
    /* The d-pad first; the stick stops at the first and last row. */
    if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
        selected = vjo_stick_step(selected, ROWS, stick);
    if (pressed & ui->buttons->enter)
        activate(selected, scratch);
    /* Left/Right: OFF/ON on the two settings' rows */
    if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && selected < 2 && !(busy && what != JOB_RESULTS) &&
        !!(pressed & SCE_CTRL_RIGHT) != (selected ? cfg.anki_screenshot : cfg.anki_mining))
        activate(selected, scratch);
    /* Cancel stops a job (Anki for Vita's results too, though Cancel then leaves the app as well) */
    if ((pressed & ui->buttons->cancel) && busy)
        cancel();
    if (touch_y >= 0 && touch_x >= 24 && touch_x < 936) {
        for (int r = 0; r < ROWS; r++)
            if (touch_y >= ROW_Y(r) && touch_y < ROW_Y(r) + ROW_H) {
                selected = r;
                activate(r, scratch);
            }
    }
    busy = running;
    what = job;
    busy_text[0] = last[0] = '\0';
    if (lock >= 0) {
        sceKernelLockMutex(lock, 1, NULL);
        snprintf(busy_text, sizeof(busy_text), "%s", progress);
        snprintf(last, sizeof(last), "%s", result);
        err = result_error;
        sceKernelUnlockMutex(lock, 1);
    }
    total = counts.queued >= 0 && counts.exported >= 0 ? counts.queued + counts.exported : -1;

    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    /* Card mining */
    snprintf(line, sizeof(line), "%s in the overlay saves the word to a queue on the memory card.",
             ui->buttons->enter_glyph);
    row(ui, MINING_Y, selected == 0, ui->white, "Card mining", line);
    ui->badge(812, MINING_Y + BADGE_DY, 108, BADGE_H, !writable ? "--" : cfg.anki_mining ? "ON" : "OFF",
              writable && cfg.anki_mining);
    /* The card's screenshot (anki_screenshot, API 23) */
    row(ui, SHOT_Y, selected == 1, ui->white, "Screenshots", "Each card keeps a picture of the game screen (about 60 KB).");
    ui->badge(812, SHOT_Y + BADGE_DY, 108, BADGE_H, !writable ? "--" : cfg.anki_screenshot ? "ON" : "OFF",
              writable && cfg.anki_screenshot);
    /* Export the queued cards */
    if (counts.queued < 0)
        snprintf(line, sizeof(line), "Export cards (queue unreadable)");
    else
        snprintf(line, sizeof(line), "Export cards (%d queued)", counts.queued);
    row(ui, EXPORT_Y, selected == 2, counts.queued < 0 ? ui->off : ui->white, line,
        busy && what == JOB_EXPORT ? busy_text : "Writes a new .apkg; the queue clears once it is checked.");
    ui->badge(760, EXPORT_Y + BADGE_DY, 160, BADGE_H, busy && what == JOB_EXPORT ? "Exporting…" : "Export",
              busy && what == JOB_EXPORT);
    /* Export every card */
    row(ui, ALL_Y, selected == 3, ui->white, "Export all cards",
        busy && what == JOB_EXPORT_ALL ? busy_text : "Every card again, the exported too: Anki adds only new notes.");
    count_text(line, sizeof(line), total, "in all");
    ui->text(500, ALL_Y + TITLE_DY, total < 0 ? ui->off : ui->dim, 24, line);
    ui->badge(760, ALL_Y + BADGE_DY, 160, BADGE_H, busy && what == JOB_EXPORT_ALL ? "Exporting…" : "Export all",
              busy && what == JOB_EXPORT_ALL);
    /* The queued cards to the Anki for Vita app (#220): offered only when it is installed */
    if (!afv_installed)
        snprintf(help, sizeof(help), "Not installed (or too old): get it at %s", VJO_AFV_WHERE);
    else if (busy && (what == JOB_SEND || what == JOB_RESULTS))
        snprintf(help, sizeof(help), "%s", busy_text);
    else {
        SceRtcTick tick;
        int64_t now = sceRtcGetCurrentTick(&tick) >= 0 && tick.tick > 62135596800000000ull
                          ? (int64_t)((tick.tick - 62135596800000000ull) / 1000000u)
                          : 0;
        vjo_afv_status_text(&afv_status, now, help, sizeof(help));
    }
    row(ui, AFV_Y, selected == AFV_ROW, afv_installed ? ui->white : ui->off, "Send to Anki for Vita", help);
    ui->badge(760, AFV_Y + BADGE_DY, 160, BADGE_H, !afv_installed ? "--" : busy && what == JOB_SEND ? "Sending…" : "Send",
              busy && what == JOB_SEND);
    /* The last result, else how to get the file to Anki */
    if (busy && what != JOB_RESULTS)
        snprintf(controls, sizeof(controls), "%s: stop   Select: page", ui->buttons->cancel_glyph);
    else
        snprintf(controls, sizeof(controls), "Up/Down: choose   %s: select   Select: page   %s: exit",
                 ui->buttons->enter_glyph, ui->buttons->cancel_glyph);
    if (note[0])
        ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 26, 3, note, note_error, controls);
    else if (last[0] && (!busy || what == JOB_RESULTS))
        ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 26, 3, last, err, controls);
    else
        ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 26, 3,
                   "Copy the .apkg from ux0:data/VitaJPOverlay/anki/ with VitaShell's FTP or USB (Select in "
                   "VitaShell), or with an SD2Vita as ux0: its card in a reader, then open it in Anki.",
                   0, controls);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void vjo_anki_page_quit(void)
{
    if (running) {
        cancel();
        sceKernelWaitThreadEnd(worker, NULL, NULL);
        sceKernelDeleteThread(worker);
        worker = -1;
        running = 0;
    }
}
