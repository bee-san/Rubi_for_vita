#include "anki_page.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "anki_export.h"
#include "preferences.h"
#include "../core/stick_scroll.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

#define WORKER_STACK (64 * 1024)

#define MINING_Y 112
#define EXPORT_Y 188
#define ALL_Y 296
#define CARD_H 68
#define ACTION_H 100
#define ROWS 3

static VjoConfig cfg;           /* the settings shown (main thread) */
static int writable, selected;
static int stick_move;          /* the left stick's move for the next frame (vjo_anki_page_stick) */
static VjoExportCounts counts = {-1, -1};
static char note[200];          /* the page's own status (saves) */
static int note_error;

/* The worker (lock): one export at a time. */
static SceUID lock = -1, worker = -1;
static int all, running, finished, cancelled;
static char progress[160], result[480];
static int result_error;
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
    what = all;
    sceKernelUnlockMutex(lock, 1);
    vjo_arena_init(&arena, export_mem, sizeof(export_mem));
    vjo_export_run(&arena, what, &ui, &r, msg, sizeof(msg));
    vjo_log("anki: export (%s) rc=%d: %d cards, %d moved, %d kept, %d unreadable, %d too large, %d left, "
            "%d stopped, %u bytes, arena peak %u",
            what ? "all" : "queued", r.rc, r.cards, r.moved, r.kept, r.unreadable, r.too_large, r.left, r.stopped,
            (unsigned)r.bytes, (unsigned)arena.peak);
    sceKernelLockMutex(lock, 1, NULL);
    n = (size_t)snprintf(result, sizeof(result), "%s", msg);
    if (r.rc == 0 && r.cards && n < sizeof(result))
        snprintf(result + n, sizeof(result) - n, " Copy it from %s/ by VitaShell's FTP or USB (or the SD2Vita "
                 "card, when it is ux0:) and open it in Anki.", VJO_EXPORT_DIR);
    result_error = r.rc != 0 || r.kept || r.unreadable || r.too_large;
    progress[0] = '\0';
    finished = 1;
    sceKernelUnlockMutex(lock, 1);
    return 0;
}

static void start(int every)
{
    int rc;
    if (running)
        return;
    if (lock < 0 && (lock = sceKernelCreateMutex("VjoAnkiPage", 0, 0, NULL)) < 0)
        return;
    all = every;
    finished = cancelled = 0;
    snprintf(progress, sizeof(progress), "Reading the cards…");
    /* CPU3 at a system priority, as the main thread (settings/sysapp.c). */
    worker = sceKernelCreateThread("VjoAnkiExport", worker_main, 0x10000100, WORKER_STACK, 0,
                                   SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
    rc = worker < 0 ? worker : sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (worker >= 0)
            sceKernelDeleteThread(worker);
        worker = -1;
        snprintf(note, sizeof(note), "Could not start the export (0x%08X).", (unsigned)rc);
        note_error = 1;
        return;
    }
    running = 1;
    note[0] = '\0';
}

/* The worker is done: join it and count the cards again. */
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

static void save(int mining, VjoArena *scratch)
{
    if (!writable) {
        snprintf(note, sizeof(note), "Cannot read config.ini. Choices have not been changed.");
        note_error = 1;
        return;
    }
    int rc = vjo_preferences_save_anki(mining, scratch);
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
        vjo_log_enable(cfg.log_file);
        vjo_export_count(&counts);
    }
}

/* ---------------- input and drawing ---------------- */

int vjo_anki_page_busy(void)
{
    return running;
}

static void activate(int row, VjoArena *scratch)
{
    if (running)
        return;
    if (row == 0)
        save(!cfg.anki_mining, scratch);
    else
        start(row == 2);
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

void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    char line[260], busy_text[160], controls[96], last[480];
    int stick = stick_move, busy = running, err = 0, total;
    stick_move = 0;
    reap();
    busy = running;
    if (pressed & SCE_CTRL_UP)
        selected = (selected + ROWS - 1) % ROWS;
    if (pressed & SCE_CTRL_DOWN)
        selected = (selected + 1) % ROWS;
    /* The d-pad first; the stick stops at the first and last row. */
    if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
        selected = vjo_stick_step(selected, ROWS, stick);
    if (pressed & ui->buttons->enter)
        activate(selected, scratch);
    if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && selected == 0 && !busy &&
        !!(pressed & SCE_CTRL_RIGHT) != cfg.anki_mining)
        save(!!(pressed & SCE_CTRL_RIGHT), scratch);
    if ((pressed & ui->buttons->cancel) && busy)
        cancel();
    if (touch_y >= 0 && touch_x >= 24 && touch_x < 936) {
        static const int top[ROWS] = {MINING_Y, EXPORT_Y, ALL_Y};
        static const int height[ROWS] = {CARD_H, ACTION_H, ACTION_H};
        for (int r = 0; r < ROWS; r++)
            if (touch_y >= top[r] && touch_y < top[r] + height[r]) {
                selected = r;
                activate(r, scratch);
            }
    }
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
    ui->card(MINING_Y, CARD_H, selected == 0);
    ui->text(42, MINING_Y + 30, ui->white, 28, "Card mining");
    ui->badge(812, MINING_Y + 6, 108, 34, !writable ? "--" : cfg.anki_mining ? "ON" : "OFF",
              writable && cfg.anki_mining);
    snprintf(line, sizeof(line), "%s in the overlay saves the word to a queue on the memory card.",
             ui->buttons->enter_glyph);
    ui->text(42, MINING_Y + 60, ui->dim, 24, line);
    /* Export the queued cards */
    ui->card(EXPORT_Y, ACTION_H, selected == 1);
    if (counts.queued < 0)
        snprintf(line, sizeof(line), "Export cards (queue unreadable)");
    else
        snprintf(line, sizeof(line), "Export cards (%d queued)", counts.queued);
    ui->text(42, EXPORT_Y + 30, counts.queued < 0 ? ui->off : ui->white, 28, line);
    ui->badge(760, EXPORT_Y + 6, 160, 34, busy && !all ? "Exporting…" : "Export", busy && !all);
    if (busy && !all)
        ui->wrapped(42, EXPORT_Y + 62, 870, ui->dim, 22, 24, 2, busy_text);
    else
        ui->wrapped(42, EXPORT_Y + 62, 870, ui->dim, 22, 24, 2,
                    "A new .apkg of Lapis cards in ux0:data/VitaJPOverlay/anki/. The queue is cleared "
                    "once the file is written and checked.");
    /* Export every card */
    ui->card(ALL_Y, ACTION_H, selected == 2);
    ui->text(42, ALL_Y + 30, ui->white, 28, "Export all cards");
    count_text(line, sizeof(line), total, "in all");
    ui->text(500, ALL_Y + 30, total < 0 ? ui->off : ui->dim, 24, line);
    ui->badge(760, ALL_Y + 6, 160, 34, busy && all ? "Exporting…" : "Export all", busy && all);
    if (busy && all)
        ui->wrapped(42, ALL_Y + 62, 870, ui->dim, 22, 24, 2, busy_text);
    else
        ui->wrapped(42, ALL_Y + 62, 870, ui->dim, 22, 24, 2,
                    "Every card again, the exported ones too (a new device, a lost file): Anki adds only "
                    "the notes it does not have.");
    /* The last result, else how to get the file to Anki */
    if (busy)
        snprintf(controls, sizeof(controls), "%s: stop   Select: page", ui->buttons->cancel_glyph);
    else
        snprintf(controls, sizeof(controls), "Up/Down: choose   %s: select   Select: page   %s: exit",
                 ui->buttons->enter_glyph, ui->buttons->cancel_glyph);
    if (note[0])
        ui->footer(408, 92, 435, 3, note, note_error, controls);
    else if (last[0] && !busy)
        ui->footer(408, 92, 435, 3, last, err, controls);
    else
        ui->footer(408, 92, 435, 3,
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
