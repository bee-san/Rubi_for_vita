#include "anki_page.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "net.h"
#include "preferences.h"
#include "../core/anki_app.h"
#include "../shell/anki_queue.h"

#define HOST_PATH "ux0:data/VitaJPOverlay/anki_host.txt"
#define LOG_PATH "ux0:data/VitaJPOverlay/log.txt"
#define LOG_CAP (1024 * 1024) /* the Shell rotates log.txt; the app only appends below this */
#define WORK_MEM (512 * 1024)  /* a card with a 192 KiB screenshot, its request and the reply */
#define WORKER_STACK (128 * 1024)

#define MINING_Y 112
#define HOST_Y 188
#define TEST_Y 264
#define SEND_Y 368
#define CARD_H 68
#define ACTION_H 96
#define ROWS 4

enum { ACT_NONE = 0, ACT_TEST, ACT_SEND };

static VjoConfig cfg;           /* the settings shown (main thread) */
static int writable, selected, queued = -1;
static char saved_host[64];     /* anki_host.txt, for auto */
static char note[200];          /* the page's own status (saves, keyboard) */
static int note_error;

/* The worker (lock): one action at a time. */
static SceUID lock = -1, worker = -1;
static int action, running, finished;
static VjoConfig job_cfg;
static char progress[200], result[2][240]; /* result[0]: test, [1]: send */
static int result_error[2];
static unsigned char work_mem[WORK_MEM] __attribute__((aligned(16)));
static int log_on;

/* ---------------- log.txt (shell/anki_queue.c logs its storage errors) ---------------- */

void vjo_log(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    SceIoStat st;
    int n = snprintf(line, sizeof(line), "overlay app: ");
    SceUID fd;
    if (!log_on || n < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    n = (int)strlen(line);
    line[n++] = '\n';
    if (sceIoGetstat(LOG_PATH, &st) >= 0 && st.st_size + n > LOG_CAP)
        return;
    if ((fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666)) >= 0) {
        sceIoWrite(fd, line, (SceSize)n);
        sceIoClose(fd);
    }
}

/* ---------------- anki_host.txt ---------------- */

static int load_host(void *ud, char *line, size_t cap)
{
    SceUID fd = sceIoOpen(HOST_PATH, SCE_O_RDONLY, 0);
    int n;
    (void)ud;
    if (fd < 0)
        return -1;
    n = sceIoRead(fd, line, (SceSize)(cap - 1));
    sceIoClose(fd);
    if (n <= 0)
        return -1;
    line[n] = '\0';
    return 0;
}

static void save_host(void *ud, const char *line)
{
    SceUID fd;
    (void)ud;
    sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    if ((fd = sceIoOpen(HOST_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) >= 0) {
        sceIoWrite(fd, line, (SceSize)strlen(line));
        sceIoClose(fd);
    }
    vjo_log("anki: search found %s", line);
}

static void read_saved_host(void)
{
    char line[80];
    int port;
    saved_host[0] = '\0';
    if (load_host(NULL, line, sizeof(line)) == 0) {
        size_t n = strlen(line);
        while (n && (unsigned char)line[n - 1] <= ' ')
            line[--n] = '\0';
        if (vjo_anki_endpoint(line, saved_host, sizeof(saved_host), &port) == VJO_ANKI_MANUAL)
            snprintf(saved_host + strlen(saved_host), sizeof(saved_host) - strlen(saved_host), ":%d", port);
        else
            saved_host[0] = '\0';
    }
}

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
    (void)ud;
    return vjo_app_net_cancelled();
}

static int worker_main(SceSize args, void *argp)
{
    VjoArena arena;
    VjoPlatform p;
    VjoAnkiAppOps ops = {NULL, vjo_app_net_local_ipv4, vjo_app_net_probe, load_host, save_host, stopped,
                         set_progress};
    char msg[240];
    int what, rc, err = 1, k;
    (void)args;
    (void)argp;
    sceKernelLockMutex(lock, 1, NULL);
    what = action;
    sceKernelUnlockMutex(lock, 1);
    k = what == ACT_SEND;
    if ((rc = vjo_app_net_init()) < 0) {
        snprintf(msg, sizeof(msg), "The network could not be started (0x%08X).", (unsigned)rc);
    } else if (!vjo_app_net_connected()) {
        snprintf(msg, sizeof(msg), "Wi-Fi is off or not connected: connect it in the Vita's Settings first.");
    } else {
        vjo_app_net_platform(&p);
        vjo_arena_init(&arena, work_mem, sizeof(work_mem));
        if (what == ACT_TEST) {
            VjoAnkiTest t;
            rc = vjo_anki_app_test(&arena, &p, &ops, &job_cfg, &t, msg, sizeof(msg));
            err = rc != VJO_OK || t.missing;
            vjo_log("anki: test %s:%d rc=%d kiku=%d missing=%d deck=%d searched=%d", t.host, t.port, rc, t.kiku,
                    t.missing, t.deck, t.searched);
        } else {
            VjoAnkiSend s;
            rc = vjo_anki_app_send(&arena, &p, &ops, &job_cfg, &s, msg, sizeof(msg));
            err = rc != VJO_OK || s.unreadable || s.too_large;
            vjo_log("anki: sent %d of %d, %d duplicates, %d unreadable, %d too large, rc=%d, arena peak %u", s.sent,
                    s.queued, s.duplicates, s.unreadable, s.too_large, rc, (unsigned)arena.peak);
        }
    }
    sceKernelLockMutex(lock, 1, NULL);
    snprintf(result[k], sizeof(result[k]), "%s", msg);
    result_error[k] = err;
    progress[0] = '\0';
    finished = 1;
    sceKernelUnlockMutex(lock, 1);
    return 0;
}

static void start(int what)
{
    if (running)
        return;
    if (!cfg.anki_host[0]) {
        snprintf(note, sizeof(note), "Set AnkiConnect's address first: your computer's IP, or auto.");
        note_error = 1;
        return;
    }
    if (lock < 0 && (lock = sceKernelCreateMutex("VjoAnkiPage", 0, 0, NULL)) < 0)
        return;
    job_cfg = cfg;
    action = what;
    finished = 0;
    snprintf(progress, sizeof(progress), "%s", what == ACT_TEST ? "Testing the connection…" : "Sending…");
    vjo_app_net_clear();
    worker = sceKernelCreateThread("VjoAnkiApp", worker_main, 0x10000100, WORKER_STACK, 0, 0, NULL);
    int rc = worker < 0 ? worker : sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (worker >= 0)
            sceKernelDeleteThread(worker);
        worker = -1;
        snprintf(note, sizeof(note), "Could not start (0x%08X).", (unsigned)rc);
        note_error = 1;
        return;
    }
    running = 1;
    note[0] = '\0';
}

/* The worker is done: join it and count the queue again. */
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
    queued = vjo_queue_count();
    read_saved_host();
}

/* ---------------- settings ---------------- */

static void save(int mining, const char *host, VjoArena *scratch)
{
    if (!writable) {
        snprintf(note, sizeof(note), "Cannot read config.ini. Choices have not been changed.");
        note_error = 1;
        return;
    }
    int rc = vjo_preferences_save_anki(mining, host, scratch);
    int read_rc = vjo_preferences_load(&cfg, scratch);
    writable = read_rc >= 0;
    note_error = rc < 0 || read_rc < 0;
    snprintf(note, sizeof(note), "%s", note_error ? "Save not confirmed. Check the choices shown, then retry."
                                                  : "Saved. The overlay uses it when it next opens.");
}

void vjo_anki_page_show(VjoArena *scratch)
{
    log_on = 0;
    writable = vjo_preferences_load(&cfg, scratch) >= 0;
    log_on = cfg.log_file;
    if (!running)
        queued = vjo_queue_count();
    read_saved_host();
}

/* ---------------- the keyboard ---------------- */

static SceWChar16 ime_title[64], ime_initial[64], ime_text[64];
static int ime_open, apputil_done;

static void to_utf16(SceWChar16 *out, size_t cap, const char *s)
{
    size_t i = 0;
    for (; s[i] && i < cap - 1; i++)
        out[i] = (unsigned char)s[i];
    out[i] = 0;
}

static void ime_start(void)
{
    SceImeDialogParam param;
    int rc;
    if (!apputil_done) {
        SceAppUtilInitParam init;
        SceAppUtilBootParam boot;
        SceCommonDialogConfigParam config;
        memset(&init, 0, sizeof(init));
        memset(&boot, 0, sizeof(boot));
        sceAppUtilInit(&init, &boot);
        sceCommonDialogConfigParamInit(&config);
        sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&config.language);
        sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&config.enterButtonAssign);
        sceCommonDialogSetConfigParam(&config);
        apputil_done = 1;
    }
    to_utf16(ime_title, 64, "AnkiConnect: your computer's IP[:port], or auto");
    to_utf16(ime_initial, 64, cfg.anki_host);
    memset(ime_text, 0, sizeof(ime_text));
    sceImeDialogParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    param.title = ime_title;
    param.maxTextLength = 63;
    param.initialText = ime_initial;
    param.inputTextBuffer = ime_text;
    if ((rc = sceImeDialogInit(&param)) < 0) {
        snprintf(note, sizeof(note), "The keyboard could not open (0x%08X).", (unsigned)rc);
        note_error = 1;
        return;
    }
    ime_open = 1;
}

static void ime_poll(VjoArena *scratch)
{
    SceImeDialogResult r;
    char typed[64], host[64];
    size_t i;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return;
    memset(&r, 0, sizeof(r));
    sceImeDialogGetResult(&r);
    sceImeDialogTerm();
    ime_open = 0;
    if (r.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return;
    for (i = 0; ime_text[i] && i < sizeof(typed) - 1; i++) {
        if (ime_text[i] >= 0x80) {
            snprintf(note, sizeof(note), "Type an IP address such as 192.168.1.23, or auto.");
            note_error = 1;
            return;
        }
        typed[i] = (char)ime_text[i];
    }
    typed[i] = '\0';
    if (vjo_anki_app_address(typed, host, sizeof(host)) < 0) {
        snprintf(note, sizeof(note), "\"%s\" is not an address: type an IP[:port] such as 192.168.1.23, or auto.",
                 typed);
        note_error = 1;
        return;
    }
    save(cfg.anki_mining, host, scratch);
    /* A new address is tested at once (for auto, that is the search). */
    if (!note_error && cfg.anki_host[0])
        start(ACT_TEST);
}

/* ---------------- input and drawing ---------------- */

int vjo_anki_page_modal(void)
{
    return ime_open;
}

int vjo_anki_page_busy(void)
{
    return running || ime_open;
}

static void activate(int row, VjoArena *scratch)
{
    if (running)
        return;
    if (row == 0)
        save(!cfg.anki_mining, cfg.anki_host, scratch);
    else if (row == 1)
        ime_start();
    else
        start(row == 2 ? ACT_TEST : ACT_SEND);
}

static void address_text(char *out, size_t cap)
{
    char host[64];
    int port;
    int mode = vjo_anki_endpoint(cfg.anki_host, host, sizeof(host), &port);
    if (mode == VJO_ANKI_AUTO)
        snprintf(out, cap, saved_host[0] ? "auto: %s" : "auto (not found yet)", saved_host);
    else if (mode == VJO_ANKI_MANUAL)
        snprintf(out, cap, "%s:%d", host, port);
    else
        snprintf(out, cap, "Not set");
}

void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    char line[240], busy_text[200];
    int busy_kind = -1;
    reap();
    if (ime_open) {
        ime_poll(scratch);
    } else {
        if (pressed & SCE_CTRL_UP)
            selected = (selected + ROWS - 1) % ROWS;
        if (pressed & SCE_CTRL_DOWN)
            selected = (selected + 1) % ROWS;
        if (pressed & SCE_CTRL_CROSS)
            activate(selected, scratch);
        if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && selected == 0 && !running &&
            !!(pressed & SCE_CTRL_RIGHT) != cfg.anki_mining)
            save(!!(pressed & SCE_CTRL_RIGHT), cfg.anki_host, scratch);
        if ((pressed & SCE_CTRL_CIRCLE) && running)
            vjo_app_net_cancel();
        if (touch_y >= 0 && touch_x >= 24 && touch_x < 936) {
            static const int top[ROWS] = {MINING_Y, HOST_Y, TEST_Y, SEND_Y};
            static const int height[ROWS] = {CARD_H, CARD_H, ACTION_H, ACTION_H};
            for (int r = 0; r < ROWS; r++)
                if (touch_y >= top[r] && touch_y < top[r] + height[r]) {
                    selected = r;
                    activate(r, scratch);
                }
        }
    }
    if (running) {
        sceKernelLockMutex(lock, 1, NULL);
        snprintf(busy_text, sizeof(busy_text), "%s", progress);
        busy_kind = action == ACT_SEND;
        sceKernelUnlockMutex(lock, 1);
    }

    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    /* Card mining */
    ui->card(MINING_Y, CARD_H, selected == 0);
    ui->text(42, MINING_Y + 30, ui->white, 28, "Card mining");
    ui->badge(812, MINING_Y + 6, 108, 34, !writable ? "--" : cfg.anki_mining ? "ON" : "OFF",
              writable && cfg.anki_mining);
    ui->text(42, MINING_Y + 60, ui->dim, 24, "× in the overlay saves the word to a queue on the memory card.");
    /* AnkiConnect address */
    ui->card(HOST_Y, CARD_H, selected == 1);
    ui->text(42, HOST_Y + 30, ui->white, 28, "AnkiConnect address");
    address_text(line, sizeof(line));
    ui->wrapped(420, HOST_Y + 30, 500, cfg.anki_host[0] ? ui->accent : ui->off, 26, 28, 1, line);
    ui->text(42, HOST_Y + 60, ui->dim, 24, "X: type your computer's IP[:port], or auto to search the network.");
    /* Test connection */
    ui->card(TEST_Y, ACTION_H, selected == 2);
    ui->text(42, TEST_Y + 30, ui->white, 28, "Test connection");
    ui->badge(760, TEST_Y + 6, 160, 34, busy_kind == 0 ? "Testing…" : "Test", busy_kind == 0);
    if (busy_kind == 0)
        ui->wrapped(42, TEST_Y + 60, 870, ui->dim, 22, 24, 2, busy_text);
    else
        ui->wrapped(42, TEST_Y + 60, 870, result[0][0] && result_error[0] ? ui->off : ui->dim, 22, 24, 2,
                    result[0][0] ? result[0] : "Checks AnkiConnect, the Kiku note type and the Kiku deck.");
    /* Send queued cards now */
    ui->card(SEND_Y, ACTION_H, selected == 3);
    ui->text(42, SEND_Y + 30, ui->white, 28, "Send queued cards now");
    if (queued < 0)
        snprintf(line, sizeof(line), "Queue unreadable");
    else
        snprintf(line, sizeof(line), "%d card%s queued", queued, queued == 1 ? "" : "s");
    ui->text(560, SEND_Y + 30, queued < 0 ? ui->off : ui->dim, 24, line);
    ui->badge(760, SEND_Y + 6, 160, 34, busy_kind == 1 ? "Sending…" : "Send", busy_kind == 1);
    if (busy_kind == 1)
        ui->wrapped(42, SEND_Y + 60, 870, ui->dim, 22, 24, 2, busy_text);
    else
        ui->wrapped(42, SEND_Y + 60, 870, result[1][0] && result_error[1] ? ui->off : ui->dim, 22, 24, 2,
                    result[1][0] ? result[1] : "As Kiku notes into the deck Kiku. △ in the overlay sends them too.");
    ui->footer(472, 36, 497, 1,
               note[0] ? note : "Anki needs AnkiConnect with webBindAddress 0.0.0.0, on a trusted home network.",
               note[0] && note_error,
               running ? "O: cancel   Select: page" : "Up/Down: choose   X: select   Select: page   O: exit");
    vita2d_end_drawing();
    if (ime_open)
        vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

void vjo_anki_page_quit(void)
{
    if (running) {
        vjo_app_net_cancel();
        sceKernelWaitThreadEnd(worker, NULL, NULL);
        sceKernelDeleteThread(worker);
        worker = -1;
        running = 0;
    }
    if (ime_open) {
        /* Term only once the dialog has finished (bounded: leaving anyway). */
        sceImeDialogAbort();
        for (int i = 0; i < 100 && sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED; i++) {
            vita2d_start_drawing();
            vita2d_end_drawing();
            vita2d_common_dialog_update();
            vita2d_swap_buffers();
        }
        sceImeDialogTerm();
        ime_open = 0;
    }
    vjo_app_net_term();
}
