/* The Rubi app's Anki page itself (settings/anki_page.c; #163 c3), on the host. The page draws
 * through main.c's helpers (VjoPageUi), recorded here; its progressive card options and Anki actions, which
 * scroll a row at a time, are checked for their layout and clipping, Up/Down and the left stick,
 * Left/Right, Enter and touch on each, each settings row's save into a
 * real config.ini (settings/preferences.c, read back by the app and by the Shell's own loader,
 * shell/files.c) or the app's own app.ini (settings/app_prefs.c), and its export through the real settings/anki_export.c on a temporary memory
 * card, on the page's worker thread, which these stubs start when a test says (a join runs a
 * thread not yet run). The harnesses that drive main.c (test_settings_app.c, test_settings_games.c)
 * keep settings_stubs/anki_page_stub.c, so that they never run an export.
 *
 * Text widths here are the settings harness's stub metrics (Latin 10, other characters 18 units
 * per 18-unit em), not the firmware's PVF fonts. Each test runs in its own process (acutest's
 * default): the page keeps its state in statics, as on the console. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/rtc.h>
#include <vita2d.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../core/anki_queue.h"
#include "../../settings/anki_export.h"
#include "../../settings/anki_page.h"
#include "../../settings/app_prefs.h"
#include "../../settings/preferences.h"
#include "settings_preview.h"
#include "../../settings/anki_features.h"

void vjo_config_load(VjoConfig *cfg, VjoArena *scratch); /* shell/files.c: what the overlay reads */

/* Dynamic visible rows share the app's scrolling viewport and fixed status box. */
#define VIEW_Y 112
#define VIEW_END 412
#define FOOTER_Y 416
#define FOOTER_H 92
#define TABS_END 100       /* main.c: TAB_Y 52 + TAB_H 48 */
#define CONTROLS_TOP 511   /* main.c draws the controls line's 24 px text at baseline 535 */
#define CARD_X 24          /* main.c's focus_card: 24..936 */
#define CARD_END 936
#define WHITE 1u
#define DIM 2u
#define ACCENT 3u
#define OFF 4u
#define CARD 5u

/* ---------------- the memory card: real files under a temporary directory (fakes/sceio_posix.h) ---------------- */

#define SCEIO_OWN (SCEIO_OPEN | SCEIO_RENAME | SCEIO_DOPEN)
#include "fakes/sceio_posix.h"
static const char *fail_open; /* an open of a path containing this fails (EIO) */
static int fail_rename, renames, writes_opened;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (fail_open && strstr(p, fail_open))
        return (int)0x80010005u;
    if (flags & (O_WRONLY | O_RDWR))
        writes_opened++;
    return sceio_open(p, flags, mode);
}
int sceIoRename(const char *a, const char *b)
{
    if (fail_rename)
        return (int)0x80010005u;
    renames++;
    return sceio_rename(a, b);
}
static const char *fail_dopen; /* a directory that cannot be read */
int sceIoDopen(const char *p)
{
    if (fail_dopen && !strcmp(p, fail_dopen))
        return (int)0x80010005u;
    return sceio_dopen(p);
}
/* 2026-10-12 09:30:05 on the console */
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    *t = (SceDateTime){2026, 10, 12, 9, 30, 5, 0};
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = 62135596800000000ull + 1791790205ull * 1000000ull;
    return 0;
}

uint64_t sceKernelGetProcessTimeWide(void) { return 0; } /* (the page's log lines time its words and exports) */

/* ---------------- the page's thread, run when the test says ---------------- */

static int (*thread_entry)(SceSize, void *), (*pending)(SceSize, void *);
static int threads_created, threads_deleted, thread_priority, thread_affinity, fail_create;
static int fail_mutex, fail_start; /* #418 (P10): the worker's lock, or its thread's start, fails */
static unsigned thread_stack;
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o)
{
    (void)n; (void)a; (void)c; (void)o;
    return fail_mutex ? (int)0x80020002u : 0x100;
}
int sceKernelLockMutex(SceUID u, int c, void *t) { (void)u; (void)c; (void)t; return 0; }
int sceKernelUnlockMutex(SceUID u, int c) { (void)u; (void)c; return 0; }
SceUID sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned int s, unsigned int a, int af, void *o)
{
    (void)n; (void)a; (void)o;
    if (fail_create)
        return (int)0x80020001u;
    thread_entry = e;
    thread_priority = p;
    thread_stack = s;
    thread_affinity = af;
    return 0x200 + ++threads_created;
}
int sceKernelStartThread(SceUID u, SceSize s, void *a)
{
    (void)u; (void)s; (void)a;
    if (fail_start)
        return (int)0x80020003u;
    pending = thread_entry;
    return 0;
}
/* The worker runs now: an export from its first step to its last. */
static void run_worker(void)
{
    int (*e)(SceSize, void *) = pending;
    pending = NULL;
    if (e)
        e(0, NULL);
}
int sceKernelWaitThreadEnd(SceUID u, int *s, void *t) { (void)u; (void)s; (void)t; run_worker(); return 0; }
int sceKernelDeleteThread(SceUID u) { (void)u; threads_deleted++; return 0; }
int sceKernelDeleteMutex(SceUID u) { (void)u; return 0; }

/* ---------------- the app's log and its display ---------------- */

static char logs[8192];
static int log_enabled = -1;
void vjo_log_enable(int on) { log_enabled = on; }
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs)) {
        logs[n] = '\n';
        logs[n + 1] = '\0';
    }
}
static int frames_begun, frames_shown;
/* the clipping the page sets around a row cut at the room's bottom, as each drawing call sees it */
static int clipping, clip_x0, clip_y0, clip_x1, clip_y1;
void vita2d_enable_clipping(void) { clipping = 1; }
void vita2d_disable_clipping(void) { clipping = 0; vp_clip(0, 0, 0, 0); }
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1) { clip_x0 = x0, clip_y0 = y0, clip_x1 = x1, clip_y1 = y1; vp_clip(x0, y0, x1, y1); }
void vita2d_start_drawing(void) { frames_begun++; }
void vita2d_clear_screen(void) {}
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) { frames_shown++; }

/* ---------------- what a frame draws (main.c's helpers, recorded) ---------------- */

/* clip: the clipping on (its rectangle's bottom; 0: none) when it was drawn */
typedef struct { int x, y, width; unsigned color; float px; int lines, clip; char s[480]; } Text; /* width 0: a single line */
typedef struct { int y, height, focus, clip; } Card;
typedef struct { int x, y, width, height, active, clip; char label[32]; } Badge;
typedef struct { int x, y, width, height; unsigned color; } Rect;
static struct {
    Text text[96];
    Card card[8];
    Badge badge[16];
    Rect rect[64];
    int texts, cards, badges, rects, headers, footers;
    int footer_y, footer_h, footer_text_y, footer_lines, footer_error, footer_clip;
    char footer[480], controls[160];
} drawn;
/* The Anki page clips only a row cut at the room's bottom, to the room. Clips other than the room's are counted, and
 * a frame may have none. */
static int other_clips;
static int clip_now(void)
{
    if (!clipping) return 0;
    if (!(clip_x0 == 24 && clip_x1 == 936 && clip_y1 == VIEW_END))
        other_clips++;
    return clip_y1;
}
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
    if (drawn.rects < 64) drawn.rect[drawn.rects++] = (Rect){(int)x, (int)y, (int)w, (int)h, color};
}

static void d_text(int x, int y, unsigned color, float px, const char *s)
{
    if (drawn.texts < 96)
        drawn.text[drawn.texts++] = (Text){x, y, 0, color, vjo_app_text_pixels(px), 1, clip_now(), ""},
        snprintf(drawn.text[drawn.texts - 1].s, 480, "%s", s);
}
static void d_wrapped(int x, int y, int width, unsigned color, float px, int line_height, int lines, const char *s)
{
    (void)line_height;
    if (drawn.texts < 96)
        drawn.text[drawn.texts++] = (Text){x, y, width, color, vjo_app_text_pixels(px), lines, clip_now(), ""},
        snprintf(drawn.text[drawn.texts - 1].s, 480, "%s", s);
}
static void d_card(int y, int height, int focus)
{
    if (drawn.cards < 8)
        drawn.card[drawn.cards++] = (Card){y, height, focus, clip_now()};
}
static void d_badge(int x, int y, int width, int height, const char *label, int active)
{
    if (drawn.badges < 16) {
        drawn.badge[drawn.badges] = (Badge){x, y, width, height, active, clip_now(), ""};
        snprintf(drawn.badge[drawn.badges++].label, 32, "%s", label);
    }
}
static void d_footer(int y, int height, int text_y, int lines, const char *message, int error, const char *controls)
{
    drawn.footers++;
    drawn.footer_y = y, drawn.footer_h = height, drawn.footer_text_y = text_y, drawn.footer_lines = lines;
    drawn.footer_error = error;
    drawn.footer_clip = clipping;
    snprintf(drawn.footer, sizeof(drawn.footer), "%s", message);
    snprintf(drawn.controls, sizeof(drawn.controls), "%s", controls);
}
static void d_header(void) { drawn.headers++; }

/* A string's width in the stub metrics: Latin 10/18 of the size, other characters 18/18. */
static int width_of(const char *s, float px)
{
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x80)
            w += px * 10 / 18;
        else if ((*p & 0xC0) == 0xC0)
            w += px;
    return (int)(w + 0.5f);
}

static VjoButtons buttons;
/* (the page's own rectangles, its scroll bar's, are recorded with the ones it draws itself) */
static void d_rect(int x, int y, int w, int h, unsigned color) { vita2d_draw_rectangle(x, y, w, h, color); }
static const VjoPageUi ui = {.text = d_text, .wrapped = d_wrapped, .rect = d_rect, .card = d_card, .badge = d_badge,
                             .footer = d_footer, .header = d_header, .white = WHITE, .dim = DIM, .accent = ACCENT,
                             .off = OFF, .track = CARD, .buttons = &buttons};

int sceDisplayWaitVblankStart(void) { return 0; }
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;

static void touch(uint32_t pressed, int x, int y)
{
    memset(&drawn, 0, sizeof(drawn));
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    other_clips = 0;
    vjo_anki_page_frame(&ui, pressed, x, y, &scratch);
    TEST_CHECK_(!other_clips, "the Anki page clipped %d times outside its room", other_clips);
}
static void frame(uint32_t pressed) { touch(pressed, -1, -1); }
static void show(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_anki_page_show(&scratch);
}
static const Text *text_starting(const char *prefix)
{
    for (int i = 0; i < drawn.texts; i++)
        if (!strncmp(drawn.text[i].s, prefix, strlen(prefix)))
            return &drawn.text[i];
    return NULL;
}

/* ---------------- the card's files ---------------- */

static int there(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}
static int read_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}
static int count_lines(const char *text, const char *prefix)
{
    int n = 0;
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        n += !strncmp(p, prefix, strlen(prefix));
    return n;
}
static int files_in(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (strlen(e->d_name) > strlen(suffix) && !strcmp(e->d_name + strlen(e->d_name) - strlen(suffix), suffix))
            n++;
    closedir(d);
    return n;
}
static void reset(int assign)
{
    /* an empty ux0:data/VitaJPOverlay in the suite's directory, removed and made in-process (no shell) */
    char data[PATH_MAX], overlay[PATH_MAX];
    TEST_ASSERT(snprintf(data, sizeof(data), "%s/ux0:data", vjo_test_tmpdir_path) < (int)sizeof(data));
    TEST_ASSERT(snprintf(overlay, sizeof(overlay), "%s/VitaJPOverlay", data) < (int)sizeof(overlay));
    TEST_ASSERT(vjo_test_remove_tree(data) == 0);
    TEST_ASSERT(mkdir(data, 0777) == 0 && mkdir(overlay, 0777) == 0);
    fail_open = fail_dopen = NULL;
    fail_rename = fail_create = renames = writes_opened = 0;
    logs[0] = '\0';
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    buttons = vjo_buttons(assign);
}

/* A card in the queue as the overlay's Enter saves it (version 4), with its screenshot or not, from line `sentence`
 * (NULL: the word alone) a minute after the first (`minute`); its ID. */
static const char *queue_line(const char *spelling, const char *sentence, int minute, int picture);
static const char *queue_card(const char *spelling, int picture) { return queue_line(spelling, NULL, 0, picture); }
static const char *queue_line(const char *spelling, const char *sentence, int minute, int picture)
{
    static char id[VJO_QUEUE_ID_SIZE];
    static unsigned char mem[64 * 1024];
    const char *m[1] = {"a meaning"}, *pos[1] = {"n"};
    char path[256];
    VjoArena a;
    VjoAnkiNote n;
    char *json;
    FILE *f;
    vjo_arena_init(&a, mem, sizeof(mem));
    memset(&n, 0, sizeof(n));
    n.spelling = spelling;
    n.reading = "よみ";
    n.rank = VJO_NO_RANK;
    n.meanings = m;
    n.n_meanings = 1;
    n.sentence = sentence ? sentence : spelling;
    n.hl_start = 0;
    n.hl_end = (int)strlen(spelling);
    n.sf_hl_start = n.sf_hl_end = -1;
    n.pos = pos;
    n.n_pos = 1;
    n.misc = "SOME GAME (2026-10-12 09:30)";
    n.dictionary = "JMdict";
    n.game = "SOME GAME";
    n.title_id = "PCSG00000";
    n.platform = "vita";
    n.unix_time = 1791790200 + 60 * minute;
    n.ocr_engine = "vocr";
    n.ocr_model = "VOCR3_H15_w8.vocr";
    TEST_ASSERT((json = vjo_queue_encode(&a, &n, picture)) != NULL);
    vjo_queue_id(json, strlen(json), id);
    mkdir(VJO_QUEUE_DIR, 0777);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    TEST_ASSERT(fwrite(json, 1, strlen(json), f) == strlen(json));
    fclose(f);
    if (picture) {
        static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xD9};
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL);
        TEST_ASSERT(fwrite(jpeg, 1, sizeof(jpeg), f) == sizeof(jpeg));
        fclose(f);
    }
    return id;
}

/* config.ini's three Anki lines as written, and as the app and the Shell load them back. */
static void check_saved3(int mining, int screenshot, int trim)
{
    char text[VJO_CONFIG_TEXT_MAX + 1], want[64];
    static unsigned char shell_mem[24 * 1024];
    VjoArena shell;
    VjoConfig cfg;
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0);
    snprintf(want, sizeof(want), "anki_mining = %s\n", mining ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_mining") == 1 && strstr(text, want), "config.ini: want %s", want);
    snprintf(want, sizeof(want), "anki_screenshot = %s\n", screenshot ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_screenshot =") == 1 && strstr(text, want), "config.ini: want %s", want);
    snprintf(want, sizeof(want), "anki_screenshot_trim_bars = %s\n", trim ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_screenshot_trim_bars") == 1 && strstr(text, want), "config.ini: want %s", want);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    TEST_ASSERT(vjo_preferences_load(&cfg, &scratch) == 0);
    TEST_CHECK(cfg.anki_mining == mining && cfg.anki_screenshot == screenshot && cfg.anki_screenshot_trim_bars == trim);
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    vjo_config_load(&cfg, &shell);
    TEST_CHECK_(cfg.anki_mining == mining && cfg.anki_screenshot == screenshot && cfg.anki_screenshot_trim_bars == trim &&
                    !cfg.n_warnings,
                "the Shell loads anki_mining %d, anki_screenshot %d, anki_screenshot_trim_bars %d (%d warnings)",
                cfg.anki_mining, cfg.anki_screenshot, cfg.anki_screenshot_trim_bars, cfg.n_warnings);
}
static void check_saved(int mining, int screenshot) { check_saved3(mining, screenshot, 1); }
/* Find the focused row by its drawn title, so tests do not depend on hidden rows' numeric IDs. */
static const char *focused_title(void)
{
    for (int c = 0; c < drawn.cards; c++) if (drawn.card[c].focus)
        for (int t = 0; t < drawn.texts; t++)
            if (drawn.text[t].x == 42 && drawn.text[t].y == drawn.card[c].y + 30) return drawn.text[t].s;
    return "";
}
static void focus_title(const char *prefix)
{
    for (int n = 0; n < 20; n++) {
        if (!strncmp(focused_title(), prefix, strlen(prefix))) return;
        frame(SCE_CTRL_DOWN);
    }
    TEST_ASSERT_(0, "row not available: %s", prefix);
}
static const Badge *choice(const char *prefix, int k)
{
    const Text *title = text_starting(prefix);
    if (!title) return NULL;
    for (int b = 0; b < drawn.badges; b++)
        if (drawn.badge[b].y == title->y - 24 && !k--) return &drawn.badge[b];
    return NULL;
}
static void enable_mining(void)
{
    focus_title("Card mining");
    const Badge *b = choice("Card mining", 0); TEST_ASSERT(b);
    if (!b->active) frame(SCE_CTRL_RIGHT);
    frame(0);
}
static VjoAppPrefs saved_app(void)
{
    VjoAppPrefs p;
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    TEST_ASSERT(vjo_app_prefs_load(&p, &scratch) >= 0);
    return p;
}
static void mining_controls_are_progressive(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); show(); frame(0);
    TEST_CHECK(text_starting("Card mining") && text_starting("Export cards") && text_starting("Anki actions"));
    TEST_CHECK(!text_starting("Screenshots") && !text_starting("Card type"));
    TEST_CHECK(!there(VJO_PREFERENCES_PATH));
    enable_mining(); check_saved(1, 1);
    TEST_CHECK(text_starting("Screenshots") && text_starting("Card type") && !text_starting("Trim black bars"));
    focus_title("Screenshots"); frame(SCE_CTRL_LEFT); check_saved(1, 0);
    focus_title("Card mining"); frame(SCE_CTRL_LEFT); check_saved(0, 0);
    TEST_CHECK(!text_starting("Screenshots") && !text_starting("Card type"));
    TEST_CHECK(text_starting("Export cards") && text_starting("Anki actions"));
}
static void visible_rows_and_choices_fit(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); show(); frame(0); enable_mining();
    static const char *const rows[] = {"Card mining", "Screenshots", "Card type", "Game tag", "Sentence readings",
                                      "Duplicates", "Export cards", "Anki actions"};
    for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        focus_title(rows[r]);
        TEST_CHECK(!strncmp(focused_title(), rows[r], strlen(rows[r])));
        for (int c = 0; c < drawn.cards; c++) {
            const Card *card = &drawn.card[c];
            TEST_CHECK(card->y >= VIEW_Y && (card->clip || card->y + card->height <= VIEW_END));
        }
        for (int b = 0; b < drawn.badges; b++) {
            const Badge *badge = &drawn.badge[b];
            TEST_CHECK(badge->x >= 24 && badge->x + badge->width <= 936);
            TEST_CHECK_(width_of(badge->label, 26) <= badge->width - 8, "choice '%s' fits %d", badge->label, badge->width);
        }
        TEST_CHECK(drawn.footer_y == 416 && drawn.footer_y + drawn.footer_h == 508 && !drawn.footer_clip);
    }
    frame(SCE_CTRL_DOWN); TEST_CHECK(!strcmp(focused_title(), "Card mining"));
    vjo_anki_page_stick(-1); frame(0); TEST_CHECK(!strcmp(focused_title(), "Card mining"));
    frame(SCE_CTRL_UP); TEST_CHECK(!strcmp(focused_title(), "Anki actions"));
    vjo_anki_page_stick(1); frame(0); TEST_CHECK(!strcmp(focused_title(), "Anki actions"));
}
static void choices_save_by_controller_and_touch(void)
{
    reset(VJO_BUTTON_ASSIGN_CIRCLE); show(); frame(0); enable_mining();
    focus_title("Card type");
    const Badge *b = choice("Card type", 3); TEST_ASSERT(b && !strcmp(b->label, "Reveal sentence"));
    touch(0, b->x + b->width / 2, b->y + 16); TEST_CHECK(saved_app().anki_card_type == 3);
    frame(SCE_CTRL_LEFT); TEST_CHECK(saved_app().anki_card_type == 2);
    frame(SCE_CTRL_RIGHT); TEST_CHECK(saved_app().anki_card_type == 3);
    frame(buttons.enter); TEST_CHECK(saved_app().anki_card_type == 0);
    focus_title("Game tag"); frame(SCE_CTRL_LEFT); TEST_CHECK(saved_app().anki_game_tag == 0);
    frame(SCE_CTRL_RIGHT); TEST_CHECK(saved_app().anki_game_tag == 1);
    focus_title("Sentence readings"); frame(SCE_CTRL_LEFT); TEST_CHECK(saved_app().anki_sentence_readings == 0);
    frame(SCE_CTRL_RIGHT); TEST_CHECK(saved_app().anki_sentence_readings == 1);
    focus_title("Duplicates"); frame(SCE_CTRL_RIGHT); TEST_CHECK(saved_app().anki_duplicates == VJO_DUPLICATES_SKIP);
    b = choice("Duplicates", 2); TEST_ASSERT(b); touch(0, b->x + 20, b->y + 15);
    TEST_CHECK(saved_app().anki_duplicates == VJO_DUPLICATES_KEEP);
    frame(buttons.enter); TEST_CHECK(saved_app().anki_duplicates == VJO_DUPLICATES_TAG);
}
static void actions_go_back_and_export_all_without_mining(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); queue_card("猫", 0); show(); frame(0);
    focus_title("Anki actions"); frame(buttons.enter); frame(0);
    TEST_CHECK(!strcmp(vjo_anki_page_title(), "Anki / Actions") && vjo_anki_page_holds_cancel());
    TEST_CHECK(text_starting("Export all cards") && !text_starting("Known words"));
    frame(buttons.cancel); TEST_CHECK(!strcmp(vjo_anki_page_title(), "Anki") && !vjo_anki_page_holds_cancel());
    focus_title("Anki actions"); frame(buttons.enter); frame(0);
    TEST_CHECK(vjo_anki_page_back()); frame(0); TEST_CHECK(!strcmp(vjo_anki_page_title(), "Anki"));
    focus_title("Anki actions"); frame(buttons.enter); frame(0); frame(buttons.enter);
    TEST_ASSERT(vjo_anki_page_busy()); run_worker(); frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && strstr(drawn.footer, "Exported 1 card (") && !drawn.footer_error);
    TEST_CHECK(files_in(VJO_EXPORT_DIR, ".apkg") == 1);
}
static void export_follows_the_card_settings(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); queue_card("猫", 0); show(); frame(0); enable_mining();
    focus_title("Card type"); frame(SCE_CTRL_RIGHT);
    focus_title("Game tag"); frame(SCE_CTRL_LEFT);
    TEST_CHECK(saved_app().anki_card_type == 1 && saved_app().anki_game_tag == 0);
    focus_title("Export cards"); frame(buttons.enter); TEST_ASSERT(vjo_anki_page_busy());
    run_worker(); frame(0); TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") && !drawn.footer_error);
    static unsigned char bytes[128 * 1024];
    FILE *f = fopen(VJO_EXPORT_DIR "/2026-10-12_093005-1card.apkg", "rb"); TEST_ASSERT(f);
    size_t n = fread(bytes, 1, sizeof(bytes), f); fclose(f);
    int switches = 0, tags = 0;
    for (size_t k = 0; k + 6 <= n; k++) {
        switches += !memcmp(bytes + k, "\x1fx\x1f", 3); tags += !memcmp(bytes + k, "Game::", 6);
    }
    TEST_CHECK(switches == 1 && tags == 0);
}
static void busy_choices_are_blocked_and_cancel_keeps_the_queue(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); queue_card("猫", 0); show(); frame(0); enable_mining();
    char before[VJO_CONFIG_TEXT_MAX + 1], after[VJO_CONFIG_TEXT_MAX + 1];
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, before, sizeof(before)) > 0);
    focus_title("Export cards"); frame(buttons.enter); TEST_ASSERT(vjo_anki_page_busy());
    TEST_CHECK(thread_stack == 64 * 1024 && thread_affinity == 0x80000);
    focus_title("Screenshots"); frame(SCE_CTRL_LEFT); frame(buttons.enter);
    const Badge *b = choice("Screenshots", 0); TEST_ASSERT(b); touch(0, b->x + 10, b->y + 15);
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, after, sizeof(after)) > 0); TEST_CHECK(!strcmp(before, after));
    TEST_CHECK(vjo_anki_page_holds_cancel()); frame(buttons.cancel); run_worker(); frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 0);
}
static void known_import_is_disabled_but_retained(void)
{
    TEST_CHECK(VJO_KNOWN_WORDS_ENABLED == 0);
    reset(VJO_BUTTON_ASSIGN_CROSS);
    FILE *f = fopen(VJO_EXPORT_DATA_DIR "/known_words.txt", "wb"); TEST_ASSERT(f); fputs("猫\tよみ\n", f); fclose(f);
    /* An old imported index is retained, but neither consulted nor exposed by the shipping UI. */
    static unsigned char memory[VJO_EXPORT_MEM]; VjoArena a; VjoKnownResult known; char message[512];
    const VjoExportUi callbacks = {0}; vjo_arena_init(&a, memory, sizeof(memory));
    TEST_ASSERT(vjo_known_import(&a, &callbacks, &known, message, sizeof(message)) == 0);
    TEST_CHECK(vjo_known_count() == 1);
    char before[256], after[256]; int size = read_file(VJO_EXPORT_DATA_DIR "/anki_known.bin", before, sizeof(before));
    TEST_ASSERT(size > 0); queue_card("猫", 0); show(); frame(0);
    focus_title("Anki actions"); frame(buttons.enter); frame(0);
    TEST_CHECK(!text_starting("Known words") && !text_starting("Import"));
    frame(buttons.enter); run_worker(); frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") && !strstr(drawn.footer, "tagged known"));
    TEST_CHECK(!strstr(logs, "known not checked"));
    TEST_CHECK(read_file(VJO_EXPORT_DATA_DIR "/anki_known.bin", after, sizeof(after)) == size && !memcmp(before, after, (size_t)size));
}
static void unreadable_files_and_failed_saves_stay_intact(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); show(); frame(0); enable_mining();
    focus_title("Card type"); fail_rename = 1; frame(SCE_CTRL_RIGHT);
    TEST_CHECK(strstr(drawn.footer, "Save not confirmed") && saved_app().anki_card_type == 0);
    fail_rename = 0;
    TEST_ASSERT(mkdir(VJO_APP_PREFS_PATH, 0777) == 0); show(); frame(0);
    focus_title("Card type"); writes_opened = renames = 0; frame(SCE_CTRL_RIGHT); frame(buttons.enter);
    TEST_CHECK(writes_opened == 0 && renames == 0 && strstr(drawn.footer, "Cannot read app.ini"));
}
static void worker_start_failures_leave_cards_queued(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS); queue_card("猫", 0); show(); frame(0); focus_title("Export cards");
    fail_mutex = 1; frame(buttons.enter); TEST_CHECK(!vjo_anki_page_busy() && threads_created == 0);
    fail_mutex = 0; fail_create = 1; frame(buttons.enter); TEST_CHECK(!vjo_anki_page_busy() && threads_created == 0);
    fail_create = 0; fail_start = 1; frame(buttons.enter); TEST_CHECK(!vjo_anki_page_busy());
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 0);
}
static void settings_walkthrough(void)
{
    if (!vp_available()) return;
    reset(VJO_BUTTON_ASSIGN_CROSS); queue_card("猫", 1); show(); frame(0);
    VjoPageUi preview = vp_ui(&buttons);
    vp_begin(vjo_anki_page_title()); vjo_anki_page_frame(&preview, 0, -1, -1, &scratch); vp_finish("walk-anki-1-mining");
    enable_mining(); focus_title("Card type");
    vp_begin(vjo_anki_page_title()); vjo_anki_page_frame(&preview, 0, -1, -1, &scratch); vp_finish("walk-anki-2-card-options");
    focus_title("Duplicates");
    vp_begin(vjo_anki_page_title()); vjo_anki_page_frame(&preview, 0, -1, -1, &scratch); vp_finish("walk-anki-3-details");
    focus_title("Anki actions");
    vp_begin(vjo_anki_page_title()); vjo_anki_page_frame(&preview, 0, -1, -1, &scratch); vp_finish("walk-anki-4-export");
    frame(buttons.enter); frame(0);
    vp_begin(vjo_anki_page_title()); vjo_anki_page_frame(&preview, 0, -1, -1, &scratch); vp_finish("walk-anki-5-actions");
    vp.active = 0;
}
TEST_LIST = {
    {"settings_walkthrough", settings_walkthrough},
    {"mining_controls_are_progressive", mining_controls_are_progressive},
    {"visible_rows_and_choices_fit", visible_rows_and_choices_fit},
    {"choices_save_by_controller_and_touch", choices_save_by_controller_and_touch},
    {"actions_go_back_and_export_all_without_mining", actions_go_back_and_export_all_without_mining},
    {"export_follows_the_card_settings", export_follows_the_card_settings},
    {"busy_choices_are_blocked_and_cancel_keeps_the_queue", busy_choices_are_blocked_and_cancel_keeps_the_queue},
    {"known_import_is_disabled_but_retained", known_import_is_disabled_but_retained},
    {"unreadable_files_and_failed_saves_stay_intact", unreadable_files_and_failed_saves_stay_intact},
    {"worker_start_failures_leave_cards_queued", worker_start_failures_leave_cards_queued},
    {NULL, NULL}
};
int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("settings-anki");
    if (!dir || chdir(dir) < 0) return 2;
    return acutest_main(argc, argv);
}
