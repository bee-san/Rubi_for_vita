#include "anki_page.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "anki_export.h"
#include "app_prefs.h"
#include "preferences.h"
#include "../core/lapis.h"
#include "../core/stick_scroll.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

#define WORKER_STACK (64 * 1024)

/* Ten rows (API 23 added Screenshots, API 32 their black bars, #311 Card type, Game tag and Sentence readings,
 * #314 Duplicates and Known words), then the 3-line status box,
 * which ends above the controls line main.c draws at y 535: the export rows' help is one line.
 * The rows are more than the room above the box: they scroll a row at a time, the selected one
 * always whole, as the OCR page's cards do (a row cut at the room's bottom shows there is more,
 * and a bar at the right where the view is); a touch on a cut row only selects it. */
#define VIEW_Y 112
#define VIEW_END 412   /* the rows' room ends above the status box */
#define ROW_GAP 4
#define CARD_H 68
#define ACTION_H 76
#define FOOTER_Y 416
#define FOOTER_H 92
#define SCROLL_X 942
#define SCROLL_W 6
/* (the rows before ROW_CARD_TYPE are config.ini's switches; from it to ROW_DUPES app.ini's settings, #311 and #314;
 * then the actions) */
enum { ROW_MINING, ROW_SHOT, ROW_TRIM, ROW_CARD_TYPE, ROW_GAME_TAG, ROW_READINGS, ROW_DUPES, ROW_KNOWN, ROW_EXPORT,
       ROW_ALL, ROWS };
static const int row_height[ROWS] = {CARD_H, CARD_H, CARD_H,   CARD_H,   CARD_H,
                                     CARD_H, CARD_H, ACTION_H, ACTION_H, ACTION_H};
/* The worker's jobs (`all`): the queue's export, every card's, and (#314 stage C) known_words.txt's import */
enum { JOB_QUEUED, JOB_ALL, JOB_KNOWN };
static int top_row; /* the first row in view */
/* A choice row's badges (#314 Duplicates), as the OCR page's three-choice cards (main.c's Text source): from x 440,
 * 160 px apart, 152 px wide, each choice shown and the chosen one lit. */
#define CHOICE_X 440
#define CHOICE_PITCH 160
#define CHOICE_W 152
static const char *const dupes_name[3] = {"Tag", "Skip", "Keep"}; /* VJO_DUPLICATES_*'s order */
static const char *const dupes_help[3] = {"A word you mined before gets Anki's duplicate tag.",
                                          "A word you mined before stays queued, not exported.",
                                          "Every card is exported, with no duplicate check."};
/* #311 Card type: a choice row of four, its badges from x 376, 136 px apart, 128 px wide (the last ends where
 * Duplicates' does, x 912). Its values: core/lapis.h VJO_CARD_*'s order, app.ini's anki_card_type. */
#define TYPE_X 376
#define TYPE_PITCH 136
#define TYPE_W 128
static const char *const type_name[VJO_CARD_TYPES] = {"Word", "Sentence", "Both", "Click"};
static const char *const type_help[VJO_CARD_TYPES] = {"The card's front shows the word.",
                                                     "The card's front shows the word's sentence.",
                                                     "The card's front shows the word and its sentence.",
                                                     "The card's front shows the word; a tap, its sentence."};
/* #314: the words the page reads itself, at most, when it is shown (the export reads every one): the new cards' since
 * it last looked, on the main thread, which waits for them (log.txt's "anki: words counted in N ms"; not measured on
 * a console) */
#define WORDS_ON_SHOW 64

static VjoConfig cfg;           /* the settings shown (main thread) */
static VjoAppPrefs app;         /* the app's own (app.ini, settings/app_prefs.h), shown (main thread) */
static int writable, app_writable, selected;
static int stick_move;          /* the left stick's move for the next frame (vjo_anki_page_stick) */
static VjoExportCounts counts = {-1, -1};
static int mined = -1;          /* #314: queued cards whose word an earlier card has (-1: not known) */
static int known_n = -1;        /* #314 stage C: the known words' keys (anki_known.bin; -1: none, -2: damaged) */
static char note[200];          /* the page's own status (saves) */
static int note_error;

/* The worker (lock): one job at a time, an export or the known words' import. They both use export_mem, as
 * count_mined does on the main thread, which therefore runs only while no job does. */
static SceUID lock = -1, worker = -1;
static int all, running, finished, cancelled;
static VjoAppPrefs job_app;     /* the app's settings the export follows, as the page had them at its start */
static VjoConfig job_dicts;     /* #311: config.ini's dictionaries for its sentence readings, the same */
static int job_dicts_known;     /* config.ini could be read (else the export looks nothing up) */
static char progress[160], result[480];
static int result_error;
static unsigned char export_mem[VJO_EXPORT_MEM] __attribute__((aligned(16)));

/* log.txt: settings/applog.c (settings/anki_export.c logs its storage errors there too). */

/* ---------------- the worker ---------------- */

static void count_mined(void);

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
    VjoAppPrefs prefs;
    uint64_t t0;
    char msg[360];
    int what;
    size_t n;
    (void)args;
    (void)argp;
    sceKernelLockMutex(lock, 1, NULL);
    what = all;
    prefs = job_app;
    sceKernelUnlockMutex(lock, 1);
    vjo_arena_init(&arena, export_mem, sizeof(export_mem));
    t0 = sceKernelGetProcessTimeWide();
    if (what == JOB_KNOWN) { /* #314 stage C */
        VjoKnownResult k;
        vjo_known_import(&arena, &ui, &k, msg, sizeof(msg));
        vjo_log("anki: known words import rc=%d: %d words, %d keys, %d not words, %d over, arena peak %u, %u ms", k.rc,
                k.words, k.keys, k.skipped, k.over, (unsigned)arena.peak,
                (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000u));
        sceKernelLockMutex(lock, 1, NULL);
        snprintf(result, sizeof(result), "%s", msg);
        result_error = k.rc != 0 || k.skipped || k.over;
        progress[0] = '\0';
        finished = 1;
        sceKernelUnlockMutex(lock, 1);
        return 0;
    }
    /* (job_dicts: written before the thread starts, read only here) */
    vjo_export_run(&arena, what == JOB_ALL, &prefs, job_dicts_known ? &job_dicts : NULL, &ui, &r, msg, sizeof(msg));
    vjo_log("anki: export (%s) rc=%d: %d cards, %d moved, %d kept, %d unreadable, %d too large, %d left, "
            "%d stopped, %d swept, %u bytes, arena peak %u, %d without screenshots, %d duplicates (%s)%s, %d known%s, "
            "%u ms",
            what == JOB_ALL ? "all" : "queued", r.rc, r.cards, r.moved, r.kept, r.unreadable, r.too_large, r.left, r.stopped,
            r.swept, (unsigned)r.bytes, (unsigned)arena.peak, r.missing_picture, r.duplicates,
            vjo_app_prefs_value("anki_duplicates", prefs.anki_duplicates), r.unchecked ? ", not checked" : "", r.known,
            r.known_unchecked ? ", known not checked" : "", (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000u));
    sceKernelLockMutex(lock, 1, NULL);
    n = (size_t)snprintf(result, sizeof(result), "%s", msg);
    if (r.rc == 0 && r.cards && n < sizeof(result))
        snprintf(result + n, sizeof(result) - n, " Copy it from %s/ by VitaShell's FTP or USB (or the SD2Vita "
                 "card, when it is ux0:) and open it in Anki.", VJO_EXPORT_DIR);
    result_error = r.rc != 0 || r.kept || r.unreadable || r.too_large || r.missing_picture || r.unchecked ||
                   r.known_unchecked;
    progress[0] = '\0';
    finished = 1;
    sceKernelUnlockMutex(lock, 1);
    return 0;
}

/* job (JOB_*) on the worker, its progress line first. 0, or <0: the SDK's error (-1: a job runs). */
static int launch(int job, const char *first)
{
    int rc;
    if (running)
        return -1;
    if (lock < 0 && (lock = sceKernelCreateMutex("VjoAnkiPage", 0, 0, NULL)) < 0)
        return lock;
    all = job;
    job_app = app;
    finished = cancelled = 0;
    snprintf(progress, sizeof(progress), "%s", first);
    /* CPU3 at a system priority, as the main thread (settings/sysapp.c). */
    worker = sceKernelCreateThread("VjoAnkiExport", worker_main, 0x10000100, WORKER_STACK, 0, SCE_KERNEL_CPU_MASK_SYSTEM,
                                   NULL);
    rc = worker < 0 ? worker : sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (worker >= 0)
            sceKernelDeleteThread(worker);
        worker = -1;
        return rc;
    }
    running = 1;
    return 0;
}

/* job: JOB_QUEUED, JOB_ALL or JOB_KNOWN */
static void start(int job)
{
    int rc;
    if (running)
        return;
    job_dicts = cfg; /* (#311: no job runs, so none reads them now) */
    job_dicts_known = writable;
    if ((rc = launch(job, job == JOB_KNOWN ? "Reading known_words.txt…" : "Reading the cards…")) < 0) {
        snprintf(note, sizeof(note), "Could not start the %s (0x%08X).", job == JOB_KNOWN ? "import" : "export",
                 (unsigned)rc);
        note_error = 1;
        return;
    }
    note[0] = '\0';
}

/* The worker is done: join it; after an export or an import the cards are counted again. */
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
    known_n = vjo_known_count();
    count_mined(); /* (Skip leaves the duplicates queued) */
}

static void cancel(void)
{
    if (!running)
        return;
    sceKernelLockMutex(lock, 1, NULL);
    cancelled = 1;
    sceKernelUnlockMutex(lock, 1);
}

/* #314: the queue's cards whose word an earlier card has, for the export row (in the export's memory: no export
 * runs). Not known (-1) when more of their words than WORDS_ON_SHOW are still to be read; the next export reads them. */
static void count_mined(void)
{
    VjoArena arena;
    VjoWords w;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int rc;
    vjo_arena_init(&arena, export_mem, sizeof(export_mem));
    rc = vjo_export_words(&arena, NULL, WORDS_ON_SHOW, &w);
    mined = rc ? -1 : w.duplicates;
    /* (the main thread's time: the page waits for it) */
    vjo_log("anki: words counted in %u ms: rc %d, %d read, %d already mined%s", (unsigned)((sceKernelGetProcessTimeWide() -
            t0) / 1000u), rc, rc ? 0 : w.read, mined, rc == 1 ? " (more to read: the next export reads them)" : "");
}

/* ---------------- settings ---------------- */

static void save(int mining, int screenshot, int trim, VjoArena *scratch)
{
    if (!writable) {
        snprintf(note, sizeof(note), "Cannot read config.ini. Choices have not been changed.");
        note_error = 1;
        return;
    }
    int rc = vjo_preferences_save_anki(mining, screenshot, trim, scratch);
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
    app_writable = vjo_app_prefs_load(&app, scratch) >= 0;
    if (!running) {
        vjo_log_enable(cfg.log_file);
        vjo_export_count(&counts);
        known_n = vjo_known_count();
        count_mined();
    }
}

/* An app.ini setting (#314 Duplicates, #311 Card type, Game tag and Sentence readings; the overlay never reads the
 * file): key's new value, saved and read back; nothing when it is outside 0..n-1 or already so. */
static void save_app(const char *key, int value, int n, VjoArena *scratch)
{
    int rc, read_rc;
    if (value < 0 || value >= n || value == vjo_app_prefs_get(&app, key))
        return;
    if (!app_writable) {
        snprintf(note, sizeof(note), "Cannot read app.ini. The choice has not been changed.");
        note_error = 1;
        return;
    }
    rc = vjo_app_prefs_save(&key, &value, 1, scratch);
    read_rc = vjo_app_prefs_load(&app, scratch);
    app_writable = read_rc >= 0;
    note_error = rc < 0 || read_rc < 0 || vjo_app_prefs_get(&app, key) != value;
    snprintf(note, sizeof(note), "%s", note_error ? "Save not confirmed. Check the choice shown, then retry."
                                                  : "Saved. The next export uses it.");
}

static void set_duplicates(int value, VjoArena *scratch)
{
    save_app("anki_duplicates", value, 3, scratch);
}

static void set_card_type(int value, VjoArena *scratch)
{
    save_app("anki_card_type", value, VJO_CARD_TYPES, scratch);
}

/* Game tag's and Sentence readings' keys (#311) */
static const char *switch_key(int row)
{
    return row == ROW_GAME_TAG ? "anki_game_tag" : "anki_sentence_readings";
}

/* ---------------- input and drawing ---------------- */

int vjo_anki_page_busy(void)
{
    return running;
}

int vjo_anki_page_holds_cancel(void)
{
    return running;
}

/* A settings row's switch: the first three rows', as config.ini has it; Game tag's and Sentence readings', as
 * app.ini has them. */
static int row_on(int row)
{
    if (row == ROW_GAME_TAG)
        return app.anki_game_tag;
    if (row == ROW_READINGS)
        return app.anki_sentence_readings;
    return row == ROW_MINING ? cfg.anki_mining : row == ROW_SHOT ? cfg.anki_screenshot : cfg.anki_screenshot_trim_bars;
}

static void activate(int row, VjoArena *scratch)
{
    if (running)
        return;
    if (row < ROW_CARD_TYPE)
        save(row == ROW_MINING ? !cfg.anki_mining : cfg.anki_mining,
             row == ROW_SHOT ? !cfg.anki_screenshot : cfg.anki_screenshot,
             row == ROW_TRIM ? !cfg.anki_screenshot_trim_bars : cfg.anki_screenshot_trim_bars, scratch);
    else if (row == ROW_CARD_TYPE) /* Enter: the next choice, round */
        set_card_type((app.anki_card_type + 1) % VJO_CARD_TYPES, scratch);
    else if (row == ROW_GAME_TAG || row == ROW_READINGS)
        save_app(switch_key(row), !row_on(row), 2, scratch);
    else if (row == ROW_DUPES) /* Enter: the next choice, round */
        set_duplicates((app.anki_duplicates + 1) % 3, scratch);
    else
        start(row == ROW_KNOWN ? JOB_KNOWN : row == ROW_ALL ? JOB_ALL : JOB_QUEUED);
}

/* Row i's top in the column (the first row's: 0), and on screen. */
static int row_offset(int i)
{
    int y = 0;
    for (int k = 0; k < i; k++)
        y += row_height[k] + ROW_GAP;
    return y;
}
static int row_y(int i) { return VIEW_Y + row_offset(i) - row_offset(top_row); }
/* Row i is all in view. */
static int row_whole(int i) { return i >= top_row && row_y(i) + row_height[i] <= VIEW_END; }
/* Scrolls by the fewest rows that bring row i all into view. */
static void row_show(int i)
{
    if (i < top_row)
        top_row = i;
    while (top_row < i && row_y(i) + row_height[i] > VIEW_END)
        top_row++;
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

/* Row i at y (busy: an export runs; busy_text its progress; total every card). */
static void draw_row(const VjoAnkiUi *ui, int i, int y, int busy, const char *busy_text, int total)
{
    static const char *const titles[ROW_DUPES] = {"Card mining", "Screenshots",       "Trim black bars",
                                                  "Card type",   "Game tag",          "Sentence readings"};
    char line[260];
    ui->card(y, row_height[i], selected == i);
    if (i == ROW_CARD_TYPE) { /* #311: each choice shown, the chosen one lit */
        int type = app.anki_card_type >= 0 && app.anki_card_type < VJO_CARD_TYPES ? app.anki_card_type : 0;
        ui->text(42, y + 30, ui->white, 28, titles[i]);
        for (int k = 0; k < VJO_CARD_TYPES; k++)
            ui->badge(TYPE_X + k * TYPE_PITCH, y + 6, TYPE_W, 34, type_name[k], app_writable && type == k);
        ui->text(42, y + 60, ui->dim, 24, app_writable ? type_help[type] : "Cannot read app.ini.");
    } else if (i == ROW_GAME_TAG || i == ROW_READINGS) { /* #311: app.ini's switches */
        ui->text(42, y + 30, ui->white, 28, titles[i]);
        ui->badge(812, y + 6, 108, 34, !app_writable ? "--" : row_on(i) ? "ON" : "OFF", app_writable && row_on(i));
        ui->text(42, y + 60, ui->dim, 24,
                 i == ROW_GAME_TAG ? "Each card gets its game's tag, as Game::CLANNAD."
                                   : "Readings over the sentence's kanji, on the card's back.");
    } else if (i == ROW_DUPES) { /* #314: each choice shown, the chosen one lit */
        ui->text(42, y + 30, ui->white, 28, "Duplicates");
        for (int k = 0; k < 3; k++)
            ui->badge(CHOICE_X + k * CHOICE_PITCH, y + 6, CHOICE_W, 34, dupes_name[k],
                      app_writable && app.anki_duplicates == k);
        ui->text(42, y + 60, ui->dim, 24, app_writable ? dupes_help[app.anki_duplicates] : "Cannot read app.ini.");
    } else if (i < ROW_CARD_TYPE) {
        ui->text(42, y + 30, ui->white, 28, titles[i]);
        ui->badge(812, y + 6, 108, 34, !writable ? "--" : row_on(i) ? "ON" : "OFF", writable && row_on(i));
        if (i == ROW_MINING)
            snprintf(line, sizeof(line), "%s in the overlay saves the word to a queue on the memory card.",
                     ui->buttons->enter_glyph);
        else /* the card's screenshot (anki_screenshot, API 23) and its black bars (anki_screenshot_trim_bars, API 32) */
            snprintf(line, sizeof(line), "%s", i == ROW_SHOT ? "Each card keeps a picture of the game screen (about 60 KB)."
                                                             : "Cuts black borders from the picture (a PSP game shown small).");
        ui->text(42, y + 60, ui->dim, 24, line);
    } else if (i == ROW_KNOWN) { /* #314 stage C: known_words.txt into the app's index */
        int on = busy && all == JOB_KNOWN;
        ui->text(42, y + 30, ui->white, 28, "Known words");
        ui->badge(760, y + 6, 160, 34, on ? "Importing…" : "Import", on);
        if (known_n > 0)
            snprintf(line, sizeof(line), "%d known word%s: their cards get the tag known.", known_n,
                     known_n == 1 ? "" : "s");
        else
            snprintf(line, sizeof(line), "%s", !known_n            ? "known_words.txt had no words."
                                               : known_n == -2     ? "The known words cannot be read: Import again."
                                                                   : "Imports known_words.txt (tools/known_words.py makes it).");
        ui->wrapped(42, y + 62, 870, ui->dim, 22, 24, 1, on ? busy_text : line);
    } else if (i == ROW_EXPORT) { /* the queued cards */
        if (counts.queued < 0)
            snprintf(line, sizeof(line), "Export cards (queue unreadable)");
        else if (mined > 0 && !busy) /* #314 */
            snprintf(line, sizeof(line), "Export cards (%d queued, %d already mined)", counts.queued, mined);
        else
            snprintf(line, sizeof(line), "Export cards (%d queued)", counts.queued);
        ui->text(42, y + 30, counts.queued < 0 ? ui->off : ui->white, 28, line);
        ui->badge(760, y + 6, 160, 34, busy && all == JOB_QUEUED ? "Exporting…" : "Export", busy && all == JOB_QUEUED);
        ui->wrapped(42, y + 62, 870, ui->dim, 22, 24, 1,
                    busy && all == JOB_QUEUED ? busy_text : "Writes a new .apkg; the queue clears once it is checked.");
    } else { /* every card */
        ui->text(42, y + 30, ui->white, 28, "Export all cards");
        count_text(line, sizeof(line), total, "in all");
        ui->text(500, y + 30, total < 0 ? ui->off : ui->dim, 24, line);
        ui->badge(760, y + 6, 160, 34, busy && all == JOB_ALL ? "Exporting…" : "Export all", busy && all == JOB_ALL);
        ui->wrapped(42, y + 62, 870, ui->dim, 22, 24, 1,
                    busy && all == JOB_ALL ? busy_text : "Every card again, the exported too: Anki adds only new notes.");
    }
}

void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    char busy_text[160], controls[96], last[480];
    int stick = stick_move, busy, err = 0, total;
    stick_move = 0;
    reap();
    busy = running; /* an export or the import runs */
    if (pressed & SCE_CTRL_UP)
        selected = (selected + ROWS - 1) % ROWS;
    if (pressed & SCE_CTRL_DOWN)
        selected = (selected + 1) % ROWS;
    /* The d-pad first; the stick stops at the first and last row. */
    if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
        selected = vjo_stick_step(selected, ROWS, stick);
    if (pressed & ui->buttons->enter)
        activate(selected, scratch);
    /* Left/Right: OFF/ON on the switches' rows; the choice to the left or right on Card type and Duplicates */
    if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && selected < ROW_DUPES && selected != ROW_CARD_TYPE && !busy &&
        !!(pressed & SCE_CTRL_RIGHT) != row_on(selected))
        activate(selected, scratch);
    if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && selected == ROW_CARD_TYPE && !busy)
        set_card_type(app.anki_card_type + (pressed & SCE_CTRL_RIGHT ? 1 : -1), scratch);
    if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && selected == ROW_DUPES && !busy)
        set_duplicates(app.anki_duplicates + (pressed & SCE_CTRL_RIGHT ? 1 : -1), scratch);
    if ((pressed & ui->buttons->cancel) && busy)
        cancel();
    /* A touch on a row in view chooses it and acts as Enter does; on a row cut at the room's bottom it only
     * chooses it (which scrolls it into view). */
    if (touch_y >= VIEW_Y && touch_y < VIEW_END && touch_x >= 24 && touch_x < 936)
        for (int r = top_row; r < ROWS && row_y(r) < VIEW_END; r++)
            if (touch_y >= row_y(r) && touch_y < row_y(r) + row_height[r]) {
                int dy = touch_y - row_y(r), k = (touch_x - CHOICE_X) / CHOICE_PITCH;
                int t = (touch_x - TYPE_X) / TYPE_PITCH;
                selected = r;
                /* Duplicates and Card type: a touch on a choice takes it; the rest of the row only selects it */
                if (r == ROW_DUPES && row_whole(r) && !busy && dy >= 6 && dy < 40 && touch_x >= CHOICE_X &&
                    (touch_x - CHOICE_X) % CHOICE_PITCH < CHOICE_W && k < 3)
                    set_duplicates(k, scratch);
                else if (r == ROW_CARD_TYPE && row_whole(r) && !busy && dy >= 6 && dy < 40 && touch_x >= TYPE_X &&
                         (touch_x - TYPE_X) % TYPE_PITCH < TYPE_W && t < VJO_CARD_TYPES)
                    set_card_type(t, scratch);
                else if (r != ROW_DUPES && r != ROW_CARD_TYPE && row_whole(r))
                    activate(r, scratch);
                break;
            }
    busy_text[0] = last[0] = '\0';
    if (lock >= 0) {
        sceKernelLockMutex(lock, 1, NULL);
        snprintf(busy_text, sizeof(busy_text), "%s", busy ? progress : "");
        snprintf(last, sizeof(last), "%s", result);
        err = result_error;
        sceKernelUnlockMutex(lock, 1);
    }
    total = counts.queued >= 0 && counts.exported >= 0 ? counts.queued + counts.exported : -1;

    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    row_show(selected); /* the selected row all in view */
    for (int i = top_row; i < ROWS && row_y(i) < VIEW_END; i++) {
        int y = row_y(i), whole = row_whole(i);
        if (!whole) { /* cut at the bottom of the room: its card and its text */
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(24, y, 936, VIEW_END);
        }
        draw_row(ui, i, y, busy, busy_text, total);
        if (!whole)
            vita2d_disable_clipping();
    }
    { /* where the view is in the column: a thin bar right of the rows */
        int room = VIEW_END - VIEW_Y, all_rows = row_offset(ROWS - 1) + row_height[ROWS - 1];
        int from = row_offset(top_row), shown = all_rows - from < room ? all_rows - from : room;
        vita2d_draw_rectangle(SCROLL_X, VIEW_Y, SCROLL_W, room, ui->track);
        vita2d_draw_rectangle(SCROLL_X, VIEW_Y + room * from / all_rows, SCROLL_W, room * shown / all_rows, ui->dim);
    }
    /* The last result, else how to get the file to Anki */
    if (busy)
        snprintf(controls, sizeof(controls), "%s: stop   Select: page", ui->buttons->cancel_glyph);
    else
        snprintf(controls, sizeof(controls), "Up/Down: choose   %s: select   Select: page   %s: exit",
                 ui->buttons->enter_glyph, ui->buttons->cancel_glyph);
    if (note[0])
        ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 27, 3, note, note_error, controls);
    else if (last[0] && !busy)
        ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 27, 3, last, err, controls);
    else
        ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 27, 3,
                   "Copy the .apkg from ux0:data/VitaJPOverlay/anki/ with VitaShell's FTP or USB (Select in "
                   "VitaShell), or with an SD2Vita as ux0: its card in a reader, then open it in Anki.",
                   0, controls);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void vjo_anki_page_quit(void)
{
    if (running) { /* an export or an import stops between two steps */
        cancel();
        sceKernelWaitThreadEnd(worker, NULL, NULL);
        sceKernelDeleteThread(worker);
        worker = -1;
        running = 0;
    }
}
