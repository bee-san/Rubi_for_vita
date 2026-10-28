#include "install_page.h"
#include "install.h"
#include "applog.h"
#include "../include/vjo_api.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

/* The installer writes worker lines or not by where the game OCR worker comes from (#138, Agent A):
 * this app cannot be built without knowing it (docs/one-vpk-install.md, R2). */
#ifndef VJO_KERNEL_LOADS_WORKER
#error "VJO_KERNEL_LOADS_WORKER (include/vjo_api.h) is not defined: the one-VPK install needs the kernel loader's capability"
#endif
#ifndef VJO_VERSION_STRING
#define VJO_VERSION_STRING "dev"
#endif
/* The worker mode, in the app's own strings: tools/package_vpk.py reads it from the built eboot.bin and
 * writes it into the payload manifest, which this app then requires (docs/one-vpk-install.md, C2). */
#if VJO_KERNEL_LOADS_WORKER
#define VJO_WORKER_MARK "vjoworker=kernel"
#else
#define VJO_WORKER_MARK "vjoworker=lines"
#endif

#define TITLE_Y 108
#define TITLE_H 46
#define BOX_Y 160
#define BOX_H 300
#define BOX_X 40
#define BOX_W 880
#define TEXT_PX 22.0f
#define LINE_H 25
#define ACTIONS_Y 466
#define ACTIONS_H 40
#define WORKER_STACK (64 * 1024)

static const VjoInstallBuild build = {VJO_API_VERSION, VJO_VERSION_STRING, VJO_VARIANT,
                                      VJO_KERNEL_LOADS_WORKER ? VJO_TAI_WORKER_KERNEL : VJO_TAI_WORKER_LINES};

static VjoInstallScan scan;   /* the main thread's, but while the worker runs (it scans again into it) */
static int scanned, action_index, scroll, stick_move;
static int actions[4], action_count;
/* what the box shows: the plan, the progress, or what the last action did */
enum { SHOW_PLAN, SHOW_RUNNING, SHOW_RESULT };
static int runs; /* finished runs (vjo_install_page_runs) */
static int showing;
static char lines[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
static int line_count;
static VjoTextLayout layout[VJO_INSTALL_LINES];
static unsigned char laid_out[VJO_INSTALL_LINES];

/* the worker (lock) */
static SceUID lock_uid = -1, worker = -1;
static int running, finished, running_action;
static char progress_step[120];
static uint64_t progress_done, progress_total;
static VjoInstallResult result;

static const char *action_name(int a)
{
    if (a == VJO_INSTALL_ACT_UNINSTALL) return "Uninstall";
    if (a == VJO_INSTALL_ACT_FINISH) return "Put it back";
    if (a == VJO_INSTALL_ACT_KEEP) return "Keep the current one";
    switch (scan.state) {
    case VJO_INSTALL_UPDATE: return "Update";
    case VJO_INSTALL_REPAIR: return "Repair";
    case VJO_INSTALL_INSTALLED: return "Check and repair";
    default: return "Install";
    }
}

static void set_lines(void)
{
    memset(laid_out, 0, sizeof(laid_out));
    scroll = 0;
    if (showing == SHOW_RESULT) {
        line_count = result.count;
        for (int i = 0; i < line_count; i++) snprintf(lines[i], VJO_INSTALL_LINE, "%s", result.lines[i]);
        return;
    }
    line_count = 0;
    if (scan.reason[0] && (scan.state == VJO_INSTALL_CANNOT || scan.state == VJO_INSTALL_NO_PAYLOAD ||
                           scan.state == VJO_INSTALL_INTERRUPTED)) {
        /* one line per sentence: a plan line holds VJO_INSTALL_LINE bytes in at most VJO_TEXT_ROWS rows */
        const char *at = scan.reason;
        while (*at && line_count < VJO_INSTALL_LINES - 1) {
            const char *end = strstr(at, ". ");
            int len = end ? (int)(end - at) + 1 : (int)strlen(at);
            snprintf(lines[line_count++], VJO_INSTALL_LINE, "%.*s", len, at);
            at += len;
            while (*at == ' ') at++;
        }
    }
    if (action_count)
        line_count += vjo_install_plan(&scan, actions[action_index], lines + line_count, VJO_INSTALL_LINES - line_count);
}

static void rescan(void)
{
    vjo_install_scan(&scan, &build);
    scanned = 1;
    action_count = 0;
    /* a stopped swap: putting its file back comes first when config.txt is HENkaku's default now and the
     * swap had started (h-encore rewrote it after the stop); keeping the current one when that is the
     * overlay's or the user's own, or when the swap never started (no .vjo-old: config.txt is as it was) */
    int keep_first = 0;
    for (int c = 0; c < 2; c++)
        keep_first |= scan.cfg[c].leftover && (scan.cfg[c].now_kind != VJO_TAI_NOW_STOCK || !(scan.cfg[c].swap_files & 1));
    int order[] = {keep_first ? VJO_INSTALL_ACT_KEEP : VJO_INSTALL_ACT_FINISH, keep_first ? VJO_INSTALL_ACT_FINISH : VJO_INSTALL_ACT_KEEP,
                   VJO_INSTALL_ACT_INSTALL, VJO_INSTALL_ACT_UNINSTALL};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (scan.actions & order[i]) actions[action_count++] = order[i];
    if (action_index >= action_count) action_index = 0;
    vjo_log("install page: %s (state %d, actions 0x%x; " VJO_WORKER_MARK ")", scan.title, scan.state, (unsigned)scan.actions);
    if (scan.reason[0]) /* why it cannot, as the page shows it (#194: the console run had only the title) */
        vjo_log("install page: %s", scan.reason);
}

int vjo_install_page_start(void)
{
    rescan();
    showing = SHOW_PLAN;
    action_index = 0;
    set_lines();
    return scan.state != VJO_INSTALL_INSTALLED && scan.state != VJO_INSTALL_NO_PAYLOAD;
}

int vjo_install_page_has_payload(void) { return scanned && scan.has_payload; }

void vjo_install_page_show(void)
{
    if (running) return;
    rescan();
    if (showing != SHOW_RESULT) {
        showing = SHOW_PLAN;
        set_lines();
    }
}

int vjo_install_page_busy(void) { return running; }
void vjo_install_page_stick(int move) { stick_move = move; }

/* ---------------- the worker ---------------- */

static void on_progress(void *ud, const char *step, uint64_t done, uint64_t total)
{
    (void)ud;
    sceKernelLockMutex(lock_uid, 1, NULL);
    snprintf(progress_step, sizeof(progress_step), "%s", step);
    progress_done = done;
    progress_total = total;
    sceKernelUnlockMutex(lock_uid, 1);
}

static int worker_main(SceSize args, void *argp)
{
    static const VjoInstallUi ui = {on_progress, NULL};
    (void)args;
    (void)argp;
    /* Into the page's result itself: the main thread reads it only once the run is joined (reap: showing is
     * SHOW_RUNNING until then, so neither set_lines nor the frame reads it while the worker writes). */
    vjo_install_run(&scan, running_action, &ui, &result);
    sceKernelLockMutex(lock_uid, 1, NULL);
    finished = 1;
    sceKernelUnlockMutex(lock_uid, 1);
    return 0;
}

static void start(void)
{
    if (running || !action_count) return;
    if (lock_uid < 0 && (lock_uid = sceKernelCreateMutex("VjoInstallPage", 0, 0, NULL)) < 0) return;
    running_action = actions[action_index];
    finished = 0;
    progress_step[0] = '\0';
    progress_done = progress_total = 0;
    /* CPU3 at a system priority, as the main thread (settings/sysapp.c) */
    worker = sceKernelCreateThread("VjoInstall", worker_main, 0x10000100, WORKER_STACK, 0, SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
    running = 1;
    showing = SHOW_RUNNING;
    int rc = worker < 0 ? worker : sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (worker >= 0) sceKernelDeleteThread(worker);
        worker = -1;
        running = 0;
        memset(&result, 0, sizeof(result));
        result.rc = -1;
        snprintf(result.lines[0], VJO_INSTALL_LINE, "Could not start (0x%08X): nothing was changed.", (unsigned)rc);
        result.count = 1;
        showing = SHOW_RESULT;
        set_lines();
    }
}

/* The worker is done: join it, scan the card again, and show what it did. */
static void reap(void)
{
    int done;
    if (!running) return;
    sceKernelLockMutex(lock_uid, 1, NULL);
    done = finished;
    sceKernelUnlockMutex(lock_uid, 1);
    if (!done) return;
    sceKernelWaitThreadEnd(worker, NULL, NULL);
    sceKernelDeleteThread(worker);
    worker = -1;
    running = 0;
    rescan();
    showing = SHOW_RESULT;
    runs++;
    set_lines();
}

int vjo_install_page_runs(void) { return runs; }

void vjo_install_page_quit(void)
{
    if (!running) return;
    sceKernelWaitThreadEnd(worker, NULL, NULL);
    sceKernelDeleteThread(worker);
    worker = -1;
    running = 0;
}

/* ---------------- input and drawing ---------------- */

static int entry_rows(const VjoInstallDraw *ui, int i)
{
    if (!laid_out[i]) {
        vjo_text_layout(lines[i], BOX_W, TEXT_PX, VJO_TEXT_ROWS, ui->measure, &layout[i]);
        laid_out[i] = 1;
    }
    return layout[i].rows ? layout[i].rows : 1;
}

#define TEXT_TOP (BOX_Y + 28)
#define TEXT_BOTTOM (BOX_Y + BOX_H - 6)
/* The box drawn from entry first: the index after the last entry it holds whole. */
static int shown_from(const VjoInstallDraw *ui, int first)
{
    int y = TEXT_TOP, i = first;
    for (; i < line_count; i++) {
        int rows = entry_rows(ui, i);
        if (i > first && y + (rows - 1) * LINE_H > TEXT_BOTTOM) break;
        y += rows * LINE_H + 6;
    }
    return i;
}

/* L and R: a box up or down (code audit note 2: a long plan is read a box at a time). R starts the box at
 * the first entry not shown whole; L at the earliest one from which the box still reaches the top's. */
static void page_scroll(const VjoInstallDraw *ui, int direction)
{
    if (direction > 0) {
        int end = shown_from(ui, scroll);
        if (end < line_count) scroll = end;
        return;
    }
    int top = scroll;
    while (top > 0 && shown_from(ui, top - 1) >= scroll) top--;
    scroll = top < scroll ? top : scroll > 0 ? scroll - 1 : 0;
}

/* The bar right of the text: where the view is in the whole, by entries (counting rows would lay out every
 * entry of a long plan at once: about four times the glyph measurements of main's first frame, host count). */
static void draw_scrollbar(const VjoInstallDraw *ui, int end)
{
    int total = line_count, seen = end - scroll;
    if (total <= 0 || seen >= total) return;
    int y = BOX_Y + 8, h = BOX_H - 16, thumb = h * seen / total;
    if (thumb < 12) thumb = 12;
    ui->rect(BOX_X + BOX_W + 4, y, 6, h, ui->selected);
    ui->rect(BOX_X + BOX_W + 4, y + (h - thumb) * scroll / (total - seen), 6, thumb, ui->dim);
}

static void draw_lines(const VjoInstallDraw *ui, int error)
{
    char row[512];
    int y = TEXT_TOP, end = shown_from(ui, scroll);
    for (int i = scroll; i < end; i++) {
        int rows = entry_rows(ui, i);
        for (int r = 0; r < rows; r++) {
            vjo_text_layout_row(lines[i], &layout[i], r, row);
            ui->text(BOX_X, y + r * LINE_H, error && i == 0 ? ui->off : i == 0 ? ui->white : ui->dim, TEXT_PX, row);
        }
        y += rows * LINE_H + 6;
    }
    if (end < line_count) ui->text(BOX_X + BOX_W - 40, BOX_Y + BOX_H - 8, ui->dim, 20, "more");
    draw_scrollbar(ui, end);
}

void vjo_install_page_frame(const VjoInstallDraw *ui, uint32_t pressed, int touch_x, int touch_y)
{
    int stick = stick_move;
    stick_move = 0;
    if (!scanned) vjo_install_page_start();
    reap();
    if (!running) {
        if (showing == SHOW_RESULT && (pressed & (ui->buttons->enter | SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
            showing = SHOW_PLAN; /* back to the plan of what the card is now */
            set_lines();
            pressed &= ~(ui->buttons->enter | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        }
        if (showing == SHOW_PLAN) {
            int moved = 0;
            if ((pressed & SCE_CTRL_LEFT) && action_index > 0) { action_index--; moved = 1; }
            if ((pressed & SCE_CTRL_RIGHT) && action_index + 1 < action_count && action_index < 2) { action_index++; moved = 1; }
            if (touch_y >= ACTIONS_Y && touch_y < ACTIONS_Y + ACTIONS_H)
                for (int i = 0; i < action_count; i++)
                    if (touch_x >= 24 + i * 300 && touch_x < 24 + i * 300 + 280) {
                        if (i == action_index) pressed |= ui->buttons->enter; /* a second touch starts it */
                        else { action_index = i; moved = 1; }
                    }
            if (moved) set_lines();
            else if (pressed & ui->buttons->enter) start();
        }
        if ((pressed & SCE_CTRL_UP) || stick < 0) { if (scroll > 0) scroll--; }
        if ((pressed & SCE_CTRL_DOWN) || stick > 0) { if (scroll + 1 < line_count) scroll++; }
        if (showing != SHOW_RUNNING && (pressed & SCE_CTRL_LTRIGGER)) page_scroll(ui, -1);
        if (showing != SHOW_RUNNING && (pressed & SCE_CTRL_RTRIGGER)) page_scroll(ui, 1);
    }
    /* a text longer than its box: Up/Down scroll it (L/R a box at a time), and the hints say so, in main's
     * words: the row with the longest action name is the app's longest hint, and its width in the console's
     * font is unmeasured */
    const char *scrolling = showing != SHOW_RUNNING && (scroll > 0 || shown_from(ui, scroll) < line_count)
                          ? "Up/Down: scroll   " : "";
    char step_text[120], controls[120];
    uint64_t done = 0, total = 0;
    if (running && lock_uid >= 0) {
        sceKernelLockMutex(lock_uid, 1, NULL);
        snprintf(step_text, sizeof(step_text), "%s", progress_step);
        done = progress_done;
        total = progress_total;
        sceKernelUnlockMutex(lock_uid, 1);
    }

    ui->header();
    /* the state */
    ui->card(TITLE_Y, TITLE_H, 0);
    ui->text(42, TITLE_Y + 33, scan.state == VJO_INSTALL_CANNOT || scan.state == VJO_INSTALL_INTERRUPTED ? ui->off : ui->white,
             26, running ? (running_action == VJO_INSTALL_ACT_UNINSTALL ? "Uninstalling..." : running_action == VJO_INSTALL_ACT_INSTALL
                            ? "Installing..." : "Working...") : scan.title);
    /* the plan, the progress, or the result */
    ui->card(BOX_Y, BOX_H, 0);
    if (showing == SHOW_RUNNING) {
        ui->text(BOX_X, BOX_Y + 40, ui->white, 24, step_text[0] ? step_text : "Starting");
        ui->rect(BOX_X, BOX_Y + 70, BOX_W, 18, ui->selected);
        if (total) {
            int w = (int)((uint64_t)BOX_W * (done > total ? total : done) / total);
            ui->rect(BOX_X, BOX_Y + 70, w, 18, ui->accent);
            char bytes[64];
            snprintf(bytes, sizeof(bytes), "%.1f of %.1f MB", (double)done / 1e6, (double)total / 1e6);
            ui->text(BOX_X, BOX_Y + 116, ui->dim, 22, bytes);
        }
        ui->text(BOX_X, BOX_Y + 160, ui->dim, 22, "Leave the Vita on and this app open: the PS button is held until it ends.");
    } else {
        draw_lines(ui, showing == SHOW_RESULT && result.rc < 0);
    }
    /* the actions */
    if (!running && showing == SHOW_PLAN)
        for (int i = 0; i < action_count && i < 3; i++)
            ui->badge(24 + i * 300, ACTIONS_Y, 280, ACTIONS_H, action_name(actions[i]), i == action_index);
    if (running)
        snprintf(controls, sizeof(controls), "Working: it cannot be stopped part way");
    else if (showing == SHOW_RESULT)
        snprintf(controls, sizeof(controls), "%s: back   %sSelect: page   %s: exit", ui->buttons->enter_glyph, scrolling,
                 ui->buttons->cancel_glyph);
    else if (action_count)
        snprintf(controls, sizeof(controls), "%s: %s   Left/Right: action   %s%s: exit", ui->buttons->enter_glyph,
                 action_name(actions[action_index]), scrolling, ui->buttons->cancel_glyph);
    else
        snprintf(controls, sizeof(controls), "%sSelect: page   %s: exit", scrolling, ui->buttons->cancel_glyph);
    ui->hints(controls);
}
