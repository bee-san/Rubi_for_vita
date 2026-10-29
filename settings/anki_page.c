#include "anki_page.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "anki_export.h"
#include "app_prefs.h"
#include "app_worker.h"
#include "preferences.h"
#include "anki_features.h"
#include "../core/lapis.h"
#include "../core/stick_scroll.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

/* Progressive card options and a separate Actions page, then the 3-line status box,
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
       ROW_ALL, ROW_ACTIONS, ROWS };
static const int row_height[ROWS] = {CARD_H, CARD_H, CARD_H,   CARD_H,   CARD_H,
                                     CARD_H, CARD_H, ACTION_H, ACTION_H, ACTION_H, CARD_H};
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
/* Card types follow core/lapis.h VJO_CARD_* order. The long reveal label has a wider badge. */
static const int type_x[VJO_CARD_TYPES] = {340, 444, 580, 684};
static const int type_width[VJO_CARD_TYPES] = {96, 128, 96, 236};
static const char *const type_name[VJO_CARD_TYPES] = {"Word", "Sentence", "Both", "Reveal sentence"};
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
static int actions_view, row_ids[ROWS], row_count;
static int stick_move;          /* the left stick's move for the next frame (vjo_anki_page_stick) */
static VjoExportCounts counts = {-1, -1};
static int mined = -1;          /* #314: queued cards whose word an earlier card has (-1: not known) */
static int known_n = -1;        /* #314 stage C: the known words' keys (anki_known.bin; -1: none, -2: damaged) */
static char note[200];          /* the page's own status (saves) */
static int note_error;

/* The worker (lock): one job at a time, an export or the known words' import. They both use export_mem, as
 * count_mined does on the main thread, which therefore runs only while no job does. */
static VjoAppWorker worker = VJO_APP_WORKER_INIT("VjoAnkiPage");
static int all;
static VjoAppPrefs job_app;     /* the app's settings the export follows, as the page had them at its start */
static VjoConfig job_dicts;     /* #311: config.ini's dictionaries for its sentence readings, the same */
static int job_dicts_known;     /* config.ini could be read (else the export looks nothing up) */
static char progress[160], result[480];
static int result_error;
static unsigned char export_mem[VJO_EXPORT_MEM] __attribute__((aligned(16)));

/* log.txt: settings/applog.c (settings/anki_export.c logs its storage errors there too). */

/* ---------------- the worker ---------------- */

static void count_mined(void);

/* Visible positions are separate from the retained row implementations. Old trimming choices are never exposed;
 * app-managed mining uses automatic trimming. Known-word import stays disabled until #521 is implemented. */
static void build_rows(void)
{
    row_count = 0;
    if (actions_view) {
        if (VJO_KNOWN_WORDS_ENABLED) row_ids[row_count++] = ROW_KNOWN;
        row_ids[row_count++] = ROW_ALL;
    } else {
        row_ids[row_count++] = ROW_MINING;
        if (cfg.anki_mining) {
            row_ids[row_count++] = ROW_SHOT;
            row_ids[row_count++] = ROW_CARD_TYPE;
            row_ids[row_count++] = ROW_GAME_TAG;
            row_ids[row_count++] = ROW_READINGS;
            row_ids[row_count++] = ROW_DUPES;
        }
        row_ids[row_count++] = ROW_EXPORT;
        row_ids[row_count++] = ROW_ACTIONS;
    }
    if (selected >= row_count) selected = row_count - 1;
    if (top_row > selected) top_row = selected;
}

const char *vjo_anki_page_title(void) { return actions_view ? "Anki / Actions" : "Anki"; }
int vjo_anki_page_back(void)
{
    if (!actions_view || worker.running) return 0;
    actions_view = selected = top_row = 0;
    build_rows();
    return 1;
}

static void set_progress(void *ud, const char *msg)
{
    (void)ud;
    vjo_app_worker_lock(&worker);
    snprintf(progress, sizeof(progress), "%s", msg);
    vjo_app_worker_unlock(&worker);
}

static int stopped(void *ud)
{
    (void)ud;
    return vjo_app_worker_stopped(&worker);
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
    vjo_app_worker_lock(&worker);
    what = all;
    prefs = job_app;
    vjo_app_worker_unlock(&worker);
    vjo_arena_init(&arena, export_mem, sizeof(export_mem));
    t0 = sceKernelGetProcessTimeWide();
    if (what == JOB_KNOWN) { /* #314 stage C */
        VjoKnownResult k;
        vjo_known_import(&arena, &ui, &k, msg, sizeof(msg));
        vjo_log("anki: known words import rc=%d: %d words, %d keys, %d not words, %d over, arena peak %u, %u ms", k.rc,
                k.words, k.keys, k.skipped, k.over, (unsigned)arena.peak,
                (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000u));
        vjo_app_worker_lock(&worker);
        snprintf(result, sizeof(result), "%s", msg);
        result_error = k.rc != 0 || k.skipped || k.over;
        progress[0] = '\0';
        worker.finished = 1;
        vjo_app_worker_unlock(&worker);
        return 0;
    }
    /* (job_dicts: written before the thread starts, read only here) */
    vjo_export_run(&arena, what == JOB_ALL, &prefs, job_dicts_known ? &job_dicts : NULL, &ui, &r, msg, sizeof(msg));
    vjo_log("anki: export (%s) rc=%d: %d cards, %d moved, %d kept, %d unreadable, %d too large, %d left, "
            "%d stopped, %d swept, %u bytes, arena peak %u, %d without screenshots, %d duplicates (%s)%s, %d known%s, "
            "%d restored, %d mismatched, %u ms",
            what == JOB_ALL ? "all" : "queued", r.rc, r.cards, r.moved, r.kept, r.unreadable, r.too_large, r.left, r.stopped,
            r.swept, (unsigned)r.bytes, (unsigned)arena.peak, r.missing_picture, r.duplicates,
            vjo_app_prefs_value("anki_duplicates", prefs.anki_duplicates), r.unchecked ? ", not checked" : "", r.known,
            r.known_unchecked ? ", known not checked" : "", r.restored, r.mismatched,
            (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000u));
    vjo_app_worker_lock(&worker);
    n = (size_t)snprintf(result, sizeof(result), "%s", msg);
    if (r.rc == 0 && r.cards && n < sizeof(result))
        snprintf(result + n, sizeof(result) - n, " Copy it from %s/ by VitaShell's FTP or USB (or the SD2Vita "
                 "card, when it is ux0:) and open it in Anki.", VJO_EXPORT_DIR);
    result_error = r.rc != 0 || r.kept || r.unreadable || r.too_large || r.missing_picture || r.unchecked ||
                   r.known_unchecked;
    progress[0] = '\0';
    worker.finished = 1;
    vjo_app_worker_unlock(&worker);
    return 0;
}

/* job (JOB_*) on the worker (settings/app_worker.h), its progress line first. 0, or <0: the SDK's error (-1: a job
 * runs). */
static int launch(int job, const char *first)
{
    int rc;
    if (worker.running || (job == JOB_KNOWN && !VJO_KNOWN_WORDS_ENABLED))
        return -1;
    if ((rc = vjo_app_worker_prepare(&worker)) < 0)
        return rc;
    all = job;
    job_app = app;
    snprintf(progress, sizeof(progress), "%s", first);
    return vjo_app_worker_start(&worker, "VjoAnkiExport", worker_main);
}

/* job: JOB_QUEUED, JOB_ALL or JOB_KNOWN */
static void start(int job)
{
    int rc;
    if (worker.running)
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
    if (!vjo_app_worker_reap(&worker))
        return;
    vjo_export_count(&counts);
    known_n = VJO_KNOWN_WORDS_ENABLED ? vjo_known_count() : -1;
    count_mined(); /* (Skip leaves the duplicates queued) */
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
    actions_view = selected = top_row = 0;
    /* (the log switch is the worker's too: it changes only while no export runs) */
    if (!worker.running)
        vjo_log_enable(0);
    writable = vjo_preferences_load(&cfg, scratch) >= 0;
    app_writable = vjo_app_prefs_load(&app, scratch) >= 0;
    if (!worker.running) {
        vjo_log_enable(cfg.log_file);
        vjo_export_count(&counts);
        known_n = VJO_KNOWN_WORDS_ENABLED ? vjo_known_count() : -1;
        count_mined();
    }
    build_rows();
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
    return worker.running;
}

int vjo_anki_page_holds_cancel(void)
{
    return worker.running || actions_view;
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
    if (worker.running)
        return;
    if (row == ROW_ACTIONS) {
        actions_view = 1;
        selected = top_row = 0;
        build_rows();
    } else if (row == ROW_TRIM || (row == ROW_KNOWN && !VJO_KNOWN_WORDS_ENABLED))
        return;
    else if (row < ROW_CARD_TYPE)
        save(row == ROW_MINING ? !cfg.anki_mining : cfg.anki_mining,
             row == ROW_SHOT ? !cfg.anki_screenshot : cfg.anki_screenshot,
             1, scratch);
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
        y += row_height[row_ids[k]] + ROW_GAP;
    return y;
}
static int row_y(int i) { return VIEW_Y + row_offset(i) - row_offset(top_row); }
/* Row i is all in view. */
static int row_whole(int i) { return i >= top_row && row_y(i) + row_height[row_ids[i]] <= VIEW_END; }
/* Scrolls by the fewest rows that bring row i all into view. */
static void row_show(int i)
{
    if (i < top_row)
        top_row = i;
    while (top_row < i && row_y(i) + row_height[row_ids[i]] > VIEW_END)
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
static void draw_row(const VjoPageUi *ui, int i, int y, int busy, const char *busy_text, int total)
{
    static const char *const titles[ROW_DUPES] = {"Card mining", "Screenshots",       "Trim black bars",
                                                  "Card type",   "Game tag",          "Sentence readings"};
    char line[260];
    ui->card(y, row_height[i], row_ids[selected] == i);
    if (i == ROW_CARD_TYPE) { /* #311: each choice shown, the chosen one lit */
        int type = app.anki_card_type >= 0 && app.anki_card_type < VJO_CARD_TYPES ? app.anki_card_type : 0;
        ui->text(42, y + 30, ui->white, 28, titles[i]);
        for (int k = 0; k < VJO_CARD_TYPES; k++)
            ui->badge(type_x[k], y + 6, type_width[k], 34, type_name[k], app_writable && type == k);
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
            snprintf(line, sizeof(line), "%s in the overlay queues the selected word for Anki.",
                     ui->buttons->enter_glyph);
        else /* the card's screenshot (anki_screenshot, API 23) and its black bars (anki_screenshot_trim_bars, API 32) */
            snprintf(line, sizeof(line), "%s", i == ROW_SHOT ? "Game screenshots are cropped automatically."
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
    } else if (i == ROW_ACTIONS) {
        ui->text(42, y + 30, ui->white, 28, "Anki actions");
        ui->text(42, y + 60, ui->dim, 24, "Export every saved card again.");
        ui->text(896, y + 30, ui->dim, 28, ">");
    } else { /* every card */
        ui->text(42, y + 30, ui->white, 28, "Export all cards");
        count_text(line, sizeof(line), total, "in all");
        ui->text(500, y + 30, total < 0 ? ui->off : ui->dim, 24, line);
        ui->badge(760, y + 6, 160, 34, busy && all == JOB_ALL ? "Exporting…" : "Export all", busy && all == JOB_ALL);
        ui->wrapped(42, y + 62, 870, ui->dim, 22, 24, 1,
                    busy && all == JOB_ALL ? busy_text : "Every card again, the exported too: Anki adds only new notes.");
    }
}

void vjo_anki_page_frame(const VjoPageUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    char busy_text[160], controls[96], last[480];
    int stick = stick_move, busy, err = 0, total;
    stick_move = 0;
    reap();
    busy = worker.running;
    build_rows();
    if ((pressed & ui->buttons->cancel) && !busy && vjo_anki_page_back()) {
        pressed = 0; stick = 0; touch_x = touch_y = -1;
    }
    if (pressed & SCE_CTRL_UP) selected = (selected + row_count - 1) % row_count;
    if (pressed & SCE_CTRL_DOWN) selected = (selected + 1) % row_count;
    if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))) selected = vjo_stick_step(selected, row_count, stick);
    int id = row_ids[selected], was_actions = actions_view;
    if (pressed & ui->buttons->enter) activate(id, scratch);
    if (actions_view != was_actions) { pressed = 0; touch_x = touch_y = -1; }
    build_rows();
    id = row_ids[selected];
    if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && !busy) {
        if ((id == ROW_MINING || id == ROW_SHOT || id == ROW_GAME_TAG || id == ROW_READINGS) &&
            !!(pressed & SCE_CTRL_RIGHT) != row_on(id)) activate(id, scratch);
        else if (id == ROW_CARD_TYPE) set_card_type(app.anki_card_type + (pressed & SCE_CTRL_RIGHT ? 1 : -1), scratch);
        else if (id == ROW_DUPES) set_duplicates(app.anki_duplicates + (pressed & SCE_CTRL_RIGHT ? 1 : -1), scratch);
    }
    if ((pressed & ui->buttons->cancel) && busy) vjo_app_worker_cancel(&worker);
    build_rows();
    if (touch_y >= VIEW_Y && touch_y < VIEW_END && touch_x >= 24 && touch_x < 936)
        for (int r = top_row; r < row_count && row_y(r) < VIEW_END; r++)
            if (touch_y >= row_y(r) && touch_y < row_y(r) + row_height[row_ids[r]]) {
                int dy = touch_y - row_y(r), k = (touch_x - CHOICE_X) / CHOICE_PITCH;
                id = row_ids[r];
                selected = r;
                if (row_whole(r) && !busy) {
                    if (id == ROW_DUPES) {
                        if (dy >= 6 && dy < 40 && touch_x >= CHOICE_X &&
                            (touch_x - CHOICE_X) % CHOICE_PITCH < CHOICE_W && k < 3) set_duplicates(k, scratch);
                    } else if (id == ROW_CARD_TYPE) {
                        for (int t = 0; t < VJO_CARD_TYPES; t++)
                            if (dy >= 6 && dy < 40 && touch_x >= type_x[t] && touch_x < type_x[t] + type_width[t])
                                set_card_type(t, scratch);
                    } else activate(id, scratch);
                }
                break;
            }
    build_rows();
    busy_text[0] = last[0] = '\0';
    if (worker.lock >= 0) {
        vjo_app_worker_lock(&worker);
        snprintf(busy_text, sizeof(busy_text), "%s", busy ? progress : "");
        snprintf(last, sizeof(last), "%s", result);
        err = result_error;
        vjo_app_worker_unlock(&worker);
    }
    total = counts.queued >= 0 && counts.exported >= 0 ? counts.queued + counts.exported : -1;
    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    row_show(selected);
    for (int r = top_row; r < row_count && row_y(r) < VIEW_END; r++) {
        int y = row_y(r), whole = row_whole(r);
        if (!whole) { vita2d_enable_clipping(); vita2d_set_clip_rectangle(24, y, 936, VIEW_END); }
        draw_row(ui, row_ids[r], y, busy, busy_text, total);
        if (!whole) vita2d_disable_clipping();
    }
    {
        int room = VIEW_END - VIEW_Y, all_rows = row_offset(row_count - 1) + row_height[row_ids[row_count - 1]];
        int from = row_offset(top_row), shown = all_rows - from < room ? all_rows - from : room;
        if (all_rows > room) vjo_page_scroll_bar(ui, SCROLL_X, VIEW_Y, SCROLL_W, room, from, shown, all_rows);
    }
    if (busy) snprintf(controls, sizeof(controls), "%s: stop   Select: look up", ui->buttons->cancel_glyph);
    else snprintf(controls, sizeof(controls), "Up/Down: choose   %s: select   Select: look up   %s: back",
                  ui->buttons->enter_glyph, ui->buttons->cancel_glyph);
    if (note[0]) ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 27, 3, note, note_error, controls);
    else if (last[0] && !busy) ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 27, 3, last, err, controls);
    else ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 27, 3,
                   "Copy the .apkg from ux0:data/VitaJPOverlay/anki/ with VitaShell's FTP or USB (Select in "
                   "VitaShell), or with an SD2Vita as ux0: its card in a reader, then open it in Anki.", 0, controls);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void vjo_anki_page_quit(void)
{
    /* an export or an import stops between two steps */
    vjo_app_worker_cancel(&worker);
    vjo_app_worker_join(&worker);
}
