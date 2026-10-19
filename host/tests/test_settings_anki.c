/* The Rubi app's Anki page itself (settings/anki_page.c; #163 c3), on the host. The page draws
 * through main.c's helpers (VjoAnkiUi), recorded here; its four rows are checked for their layout,
 * Up/Down and the left stick, Left/Right, Enter and touch on each, each settings row's save into a
 * real config.ini (settings/preferences.c, read back by the app and by the Shell's own loader,
 * shell/files.c), and its export through the real settings/anki_export.c on a temporary memory
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
#include "../../settings/anki_afv.h"
#include "../../settings/anki_export.h"
#include "../../settings/anki_page.h"
#include "../../settings/preferences.h"

void vjo_config_load(VjoConfig *cfg, VjoArena *scratch); /* shell/files.c: what the overlay reads */

/* settings/anki_page.c's layout: five rows (#220 added Send to Anki for Vita), then the 3-line status
 * box above main.c's controls line. */
#define ROWS 5
static const int row_top[ROWS] = {104, 167, 230, 293, 356}, row_height[ROWS] = {59, 59, 59, 59, 59};
#define FOOTER_Y 419
#define FOOTER_H 88
#define TABS_END 100       /* main.c: TAB_Y 52 + TAB_H 48 */
#define CONTROLS_TOP 511   /* main.c draws the controls line's 24 px text at baseline 535 */
#define CARD_X 24          /* main.c's focus_card: 24..936 */
#define CARD_END 936
#define WHITE 1u
#define DIM 2u
#define ACCENT 3u
#define OFF 4u

/* ---------------- the memory card: real files under a temporary directory ---------------- */

static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
static const char *fail_open; /* an open of a path containing this fails (EIO) */
static int fail_rename, renames, writes_opened;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (fail_open && strstr(p, fail_open))
        return (int)0x80010005u;
    if (flags & (O_WRONLY | O_RDWR))
        writes_opened++;
    return native(open(p, flags, mode));
}
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t n) { return native((int)read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n) { return native((int)write(fd, p, n)); }
int sceIoPread(int fd, void *p, size_t n, SceOff off) { return native((int)pread(fd, p, n, off)); }
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off) { return native((int)pwrite(fd, p, n, off)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
static void date_of(time_t t, SceDateTime *d)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    *d = (SceDateTime){(unsigned short)(tm.tm_year + 1900), (unsigned short)(tm.tm_mon + 1), (unsigned short)tm.tm_mday,
                       (unsigned short)tm.tm_hour, (unsigned short)tm.tm_min, (unsigned short)tm.tm_sec, 0};
}
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    if (stat(p, &s))
        return native(-1);
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    date_of(s.st_ctim.tv_sec, &st->st_ctime);
    date_of(s.st_atim.tv_sec, &st->st_atime);
    date_of(s.st_mtim.tv_sec, &st->st_mtime);
    return 0;
}
int sceIoRename(const char *a, const char *b)
{
    if (fail_rename)
        return (int)0x80010005u;
    renames++;
    return native(rename(a, b));
}
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
int sceIoSyncByFd(int fd, int flag) { (void)flag; return native(fsync(fd)); }
static DIR *dirs[4];
static char dir_paths[4][256];
static const char *fail_dopen; /* a directory that cannot be read */
int sceIoDopen(const char *p)
{
    if (fail_dopen && !strcmp(p, fail_dopen))
        return (int)0x80010005u;
    for (int i = 0; i < 4; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p)))
                return native(-1);
            snprintf(dir_paths[i], sizeof(dir_paths[i]), "%s", p);
            return 100 + i;
        }
    return -1;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e = readdir(dirs[fd - 100]);
    char path[600];
    if (!e)
        return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", dir_paths[fd - 100], e->d_name);
    TEST_ASSERT(sceIoGetstat(path, &ent->d_stat) == 0);
    return 1;
}
int sceIoDclose(int fd)
{
    int rc = closedir(dirs[fd - 100]);
    dirs[fd - 100] = NULL;
    return rc;
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

/* ---------------- the page's thread, run when the test says ---------------- */

static int (*thread_entry)(SceSize, void *), (*pending)(SceSize, void *);
static int threads_created, threads_deleted, thread_priority, thread_affinity, fail_create;
static unsigned thread_stack;
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { (void)n; (void)a; (void)c; (void)o; return 0x100; }
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
int sceKernelStartThread(SceUID u, SceSize s, void *a) { (void)u; (void)s; (void)a; pending = thread_entry; return 0; }
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
/* settings/anki_afv.c's readers wait for a file being replaced (3 x 100 ms): counted, not slept. */
static int delays;
int sceKernelDelayThread(unsigned us) { (void)us; delays++; return 0; }
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
void vita2d_start_drawing(void) { frames_begun++; }
void vita2d_clear_screen(void) {}
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) { frames_shown++; }

/* ---------------- what a frame draws (main.c's helpers, recorded) ---------------- */

typedef struct { int x, y, width; unsigned color; float px; int lines; char s[480]; } Text; /* width 0: a single line */
typedef struct { int y, height, focus; } Card;
typedef struct { int x, y, width, height, active; char label[32]; } Badge;
static struct {
    Text text[32];
    Card card[8];
    Badge badge[8];
    int texts, cards, badges, headers, footers;
    int footer_y, footer_h, footer_text_y, footer_lines, footer_error;
    char footer[480], controls[160];
} drawn;

static void d_text(int x, int y, unsigned color, float px, const char *s)
{
    if (drawn.texts < 32)
        drawn.text[drawn.texts++] = (Text){x, y, 0, color, px, 1, ""}, snprintf(drawn.text[drawn.texts - 1].s, 480, "%s", s);
}
static void d_wrapped(int x, int y, int width, unsigned color, float px, int line_height, int lines, const char *s)
{
    (void)line_height;
    if (drawn.texts < 32)
        drawn.text[drawn.texts++] = (Text){x, y, width, color, px, lines, ""}, snprintf(drawn.text[drawn.texts - 1].s, 480, "%s", s);
}
static void d_card(int y, int height, int focus)
{
    if (drawn.cards < 8)
        drawn.card[drawn.cards++] = (Card){y, height, focus};
}
static void d_badge(int x, int y, int width, int height, const char *label, int active)
{
    if (drawn.badges < 8) {
        drawn.badge[drawn.badges] = (Badge){x, y, width, height, active, ""};
        snprintf(drawn.badge[drawn.badges++].label, 32, "%s", label);
    }
}
static void d_footer(int y, int height, int text_y, int lines, const char *message, int error, const char *controls)
{
    drawn.footers++;
    drawn.footer_y = y, drawn.footer_h = height, drawn.footer_text_y = text_y, drawn.footer_lines = lines;
    drawn.footer_error = error;
    snprintf(drawn.footer, sizeof(drawn.footer), "%s", message);
    snprintf(drawn.controls, sizeof(drawn.controls), "%s", controls);
}
static void d_header(void) { drawn.headers++; }

static VjoButtons buttons;
static const VjoAnkiUi ui = {d_text, d_wrapped, d_card, d_badge, d_footer, d_header, WHITE, DIM, ACCENT, OFF, &buttons};
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;

static void touch(uint32_t pressed, int x, int y)
{
    memset(&drawn, 0, sizeof(drawn));
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_anki_page_frame(&ui, pressed, x, y, &scratch);
}
static void frame(uint32_t pressed) { touch(pressed, -1, -1); }
static void show(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_anki_page_show(&scratch);
}
/* config.ini as another save of the app left it. */
static int save_now(int mining, int screenshot)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    return vjo_preferences_save_anki(mining, screenshot, &scratch);
}

/* The row drawn focused (-1: none, -2: more than one). */
static int focused(void)
{
    int f = -1;
    for (int i = 0; i < drawn.cards; i++)
        if (drawn.card[i].focus)
            f = f == -1 ? i : -2;
    return f;
}
/* Row r's badge (each row has one, drawn in row order); an empty one when none was drawn. */
static const Badge *badge(int r)
{
    static const Badge none = {0, 0, 0, 0, 0, "(none)"};
    return r >= 0 && r < drawn.badges ? &drawn.badge[r] : &none;
}
static const Text *text_starting(const char *prefix)
{
    for (int i = 0; i < drawn.texts; i++)
        if (!strncmp(drawn.text[i].s, prefix, strlen(prefix)))
            return &drawn.text[i];
    return NULL;
}
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

/* A card in the queue as the overlay's Enter saves it (version 4), with its screenshot or not; its ID. */
static const char *queue_card(const char *spelling, int picture)
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
    n.sentence = spelling;
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
    n.unix_time = 1791790200;
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

/* config.ini's two Anki lines as written, and as the app and the Shell load them back. */
static void check_saved(int mining, int screenshot)
{
    char text[VJO_CONFIG_TEXT_MAX + 1], want[64];
    static unsigned char shell_mem[24 * 1024];
    VjoArena shell;
    VjoConfig cfg;
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0);
    snprintf(want, sizeof(want), "anki_mining = %s\n", mining ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_mining") == 1 && strstr(text, want), "config.ini: want %s", want);
    snprintf(want, sizeof(want), "anki_screenshot = %s\n", screenshot ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_screenshot") == 1 && strstr(text, want), "config.ini: want %s", want);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    TEST_ASSERT(vjo_preferences_load(&cfg, &scratch) == 0);
    TEST_CHECK(cfg.anki_mining == mining && cfg.anki_screenshot == screenshot);
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    vjo_config_load(&cfg, &shell);
    TEST_CHECK_(cfg.anki_mining == mining && cfg.anki_screenshot == screenshot && !cfg.n_warnings,
                "the Shell loads anki_mining %d, anki_screenshot %d (%d warnings)", cfg.anki_mining,
                cfg.anki_screenshot, cfg.n_warnings);
}
/* Rows 0 and 1's badges show the saved states, and the page's note says it was saved. */
static void check_shown(int mining, int screenshot)
{
    frame(0);
    TEST_CHECK_(!strcmp(badge(0)->label, mining ? "ON" : "OFF") && badge(0)->active == mining,
                "Card mining shows %s", badge(0)->label);
    TEST_CHECK_(!strcmp(badge(1)->label, screenshot ? "ON" : "OFF") && badge(1)->active == screenshot,
                "Screenshots shows %s", badge(1)->label);
    TEST_CHECK_(!strcmp(drawn.footer, "Saved. The overlay uses it when it next opens.") && !drawn.footer_error,
                "the note: %s", drawn.footer);
}

/* ---------------- tests ---------------- */

/* Four rows below the tabs, none over another, each row's text and badge in it, the footer below
 * them and above main.c's controls line; one row focused; the hints name the console's buttons. */
static void the_rows_and_the_footer_do_not_overlap(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_card("猫", 1);
    show();
    TEST_CHECK(log_enabled == 1); /* config.ini's log_file (on by default), set when the page is shown */
    frame(0);
    TEST_ASSERT(drawn.cards == ROWS && drawn.badges == ROWS && drawn.headers == 1 && drawn.footers == 1);
    TEST_CHECK(frames_begun == 1 && frames_shown == 1);
    for (int r = 0; r < ROWS; r++) {
        const Card *c = &drawn.card[r];
        TEST_CHECK_(c->y == row_top[r] && c->height == row_height[r], "row %d at %d+%d", r, c->y, c->height);
        TEST_CHECK_(r ? c->y >= drawn.card[r - 1].y + drawn.card[r - 1].height : c->y >= TABS_END,
                    "row %d at %d overlaps what is above it", r, c->y);
        /* its badge inside it, right of its title */
        const Badge *b = badge(r);
        TEST_CHECK_(b->y >= c->y && b->y + b->height <= c->y + c->height && b->x >= CARD_X + 4 &&
                        b->x + b->width <= CARD_END - 4, "row %d's badge outside its card", r);
    }
    TEST_CHECK(focused() == 0);
    /* every text inside exactly one row, from its cap height to its descent (the footer's are the footer's) */
    for (int i = 0; i < drawn.texts; i++) {
        const Text *t = &drawn.text[i];
        int top = t->y - (int)(t->px * 0.8f), bottom = t->y + (int)(t->px * 0.25f), in = 0;
        for (int r = 0; r < ROWS; r++)
            in += top >= row_top[r] && bottom <= row_top[r] + row_height[r];
        TEST_CHECK_(in == 1 && t->x >= CARD_X + 4 && (!t->width || t->x + t->width <= CARD_END - 4),
                    "'%s' at %d,%d is not inside one row", t->s, t->x, t->y);
        /* nothing runs into a badge or a text on its line: a title or count ends before it (stub metrics) */
        for (int k = 0; k < drawn.badges; k++) {
            const Badge *b = &drawn.badge[k];
            if (!t->width && top < b->y + b->height && bottom > b->y && t->x < b->x)
                TEST_CHECK_(t->x + width_of(t->s, t->px) <= b->x, "'%s' runs into the badge '%s'", t->s, b->label);
        }
        for (int k = 0; k < drawn.texts; k++) {
            const Text *u = &drawn.text[k];
            int u_top = u->y - (int)(u->px * 0.8f), u_bottom = u->y + (int)(u->px * 0.25f);
            if (k != i && !t->width && top < u_bottom && bottom > u_top && t->x < u->x)
                TEST_CHECK_(t->x + width_of(t->s, t->px) <= u->x, "'%s' runs into '%s'", t->s, u->s);
        }
    }
    /* the footer: below the last row, three 28 px lines inside it, above the controls line */
    TEST_CHECK(drawn.footer_y == FOOTER_Y && drawn.footer_h == FOOTER_H && drawn.footer_lines == 3);
    TEST_CHECK(drawn.footer_y >= row_top[ROWS - 1] + row_height[ROWS - 1] &&
               drawn.footer_y + drawn.footer_h <= CONTROLS_TOP);
    TEST_CHECK(drawn.footer_text_y - 24 >= drawn.footer_y + 2 &&
               drawn.footer_text_y + (drawn.footer_lines - 1) * 28 + 6 <= drawn.footer_y + drawn.footer_h);
    TEST_CHECK(!strcmp(drawn.controls, "Up/Down: choose   ×: select   Select: page   ○: exit"));
    TEST_CHECK(strstr(drawn.footer, "ux0:data/VitaJPOverlay/anki/") && !drawn.footer_error);
    /* the rows' texts and states: mining off and screenshots on by default, one card queued */
    TEST_CHECK(text_starting("Card mining") && text_starting("Screenshots") && text_starting("Export all cards"));
    TEST_CHECK(text_starting("× in the overlay saves the word"));
    TEST_CHECK(text_starting("Export cards (1 queued)") && text_starting("1 card in all"));
    TEST_CHECK(!strcmp(badge(0)->label, "OFF") && !strcmp(badge(1)->label, "ON") && !strcmp(badge(2)->label, "Export") &&
               !strcmp(badge(3)->label, "Export all"));
    /* Anki for Vita not installed: its row is not offered, and says where to get the app */
    {
        const Text *afv = text_starting("Send to Anki for Vita"), *help = text_starting("Not installed (or too old)");
        TEST_CHECK(afv && afv->color == OFF && !strcmp(badge(4)->label, "--") && !badge(4)->active);
        TEST_CHECK(help && strstr(help->s, VJO_AFV_WHERE) && width_of(help->s, help->px) <= help->width);
    }
    /* every help line fits its one line (stub metrics) */
    for (int i = 0; i < drawn.texts; i++)
        if (drawn.text[i].width)
            TEST_CHECK_(width_of(drawn.text[i].s, drawn.text[i].px) <= drawn.text[i].width, "'%s' is cut",
                        drawn.text[i].s);
    /* the widest counts still fit before their badges (both at x 760) */
    for (int k = 0; k < 2; k++) {
        const char *s = k ? "20000 cards in all" : "Export cards (20000 queued)";
        int x = k ? 500 : 42, px = k ? 24 : 28;
        TEST_CHECK_(x + width_of(s, (float)px) <= badge(2 + k)->x, "'%s' runs into its badge", s);
    }
    /* the Japanese button assignment: ○ enters */
    reset(VJO_BUTTON_ASSIGN_CIRCLE);
    show();
    frame(0);
    TEST_CHECK(!strcmp(drawn.controls, "Up/Down: choose   ○: select   Select: page   ×: exit"));
    TEST_CHECK(text_starting("○ in the overlay saves the word"));
}

/* Up/Down go round the four rows; the left stick moves one row a step and stops at the first and
 * the last; in a frame with both, the d-pad's press wins. Moving saves nothing. */
static void up_down_and_the_stick_choose_a_row(void)
{
    static const struct { uint32_t pressed; int stick, row; } steps[] = {
        {SCE_CTRL_DOWN, 0, 1}, {SCE_CTRL_DOWN, 0, 2}, {SCE_CTRL_DOWN, 0, 3}, {SCE_CTRL_DOWN, 0, 4},
        {SCE_CTRL_DOWN, 0, 0}, {SCE_CTRL_UP, 0, 4},   {SCE_CTRL_UP, 0, 3},   {0, 1, 4},
        {0, 1, 4},             {0, -1, 3},            {0, -4, 0},            {0, -1, 0},
        {SCE_CTRL_DOWN, 1, 1}, {SCE_CTRL_UP, 2, 0},   {0, 2, 2},             {SCE_CTRL_UP, -1, 1},
        {0, 0, 1},
    };
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    frame(0);
    TEST_CHECK(focused() == 0);
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        vjo_anki_page_stick(steps[i].stick);
        frame(steps[i].pressed);
        TEST_CHECK_(focused() == steps[i].row, "step %u: row %d focused, want %d", i, focused(), steps[i].row);
    }
    TEST_CHECK(!there(VJO_PREFERENCES_PATH) && writes_opened == 0 && threads_created == 0);
}

/* Enter on a settings row turns it the other way; Left/Right turn it OFF/ON, and do nothing when
 * it is that already; on the export rows they do nothing. Each save writes that row's key, and the
 * other row's as it was, where the app and the Shell read them. */
static void enter_and_left_right_save_each_row_s_key(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show(); /* no config.ini yet: mining off, screenshots on (the defaults) */
    frame(0);
    TEST_CHECK(!strcmp(badge(0)->label, "OFF") && !strcmp(badge(1)->label, "ON"));
    frame(SCE_CTRL_CROSS); /* row 0 */
    check_saved(1, 1);
    check_shown(1, 1);
    frame(SCE_CTRL_CROSS);
    check_saved(0, 1);
    check_shown(0, 1);
    frame(SCE_CTRL_RIGHT);
    check_saved(1, 1);
    renames = 0;
    frame(SCE_CTRL_RIGHT); /* ON already: nothing written */
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_LEFT);
    check_saved(0, 1);
    renames = 0;
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_DOWN); /* row 1 */
    frame(SCE_CTRL_CROSS);
    check_saved(0, 0);
    check_shown(0, 0);
    frame(SCE_CTRL_RIGHT);
    check_saved(0, 1);
    renames = 0;
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_LEFT);
    check_saved(0, 0);
    frame(SCE_CTRL_UP); /* row 0 again: its own key only */
    frame(SCE_CTRL_RIGHT);
    check_saved(1, 0);
    check_shown(1, 0);
    /* the export rows and Anki for Vita's: Left/Right start nothing and save nothing */
    for (int r = 2; r < ROWS; r++) {
        frame(SCE_CTRL_DOWN);
        frame(SCE_CTRL_DOWN * (r == 2));
        TEST_CHECK(focused() == r);
        renames = 0;
        frame(SCE_CTRL_LEFT);
        frame(SCE_CTRL_RIGHT);
        TEST_CHECK(renames == 0 && threads_created == 0 && !vjo_anki_page_busy());
    }
    check_saved(1, 0);
    /* with ○ as enter (a Japanese console), × is no Enter */
    reset(VJO_BUTTON_ASSIGN_CIRCLE);
    show();
    frame(SCE_CTRL_DOWN); /* from the last row round to row 0 */
    TEST_CHECK(focused() == 0);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!there(VJO_PREFERENCES_PATH));
    frame(SCE_CTRL_CIRCLE);
    check_saved(1, 1);
}

/* A touch inside a row chooses it and acts as Enter does, from its top edge to the line above its
 * bottom; between the rows, outside the cards' width, above them and on the footer, nothing. */
static void touch_chooses_and_acts_on_each_row(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_card("猫", 1);
    show();
    frame(0);
    /* the settings rows: each touch turns the row the other way */
    touch(0, 480, row_top[0]);
    TEST_CHECK(focused() == 0);
    check_saved(1, 1);
    touch(0, 935, row_top[0] + row_height[0] - 1);
    check_saved(0, 1);
    touch(0, 24, row_top[1]);
    TEST_CHECK(focused() == 1);
    check_saved(0, 0);
    touch(0, 480, row_top[1] + row_height[1] - 1);
    check_saved(0, 1);
    /* nothing: the gaps, the sides, above, the footer */
    static const int miss[][2] = {{480, 163}, {480, 166}, {480, 226}, {480, 229}, {480, 289}, {480, 292},
                                  {480, 352}, {480, 355}, {480, 415}, {480, 418}, {480, 450}, {480, 103},
                                  {480, 60}, {23, 140}, {936, 140}, {23, 300}, {936, 360}};
    renames = 0;
    for (unsigned i = 0; i < sizeof(miss) / sizeof(miss[0]); i++) {
        touch(0, miss[i][0], miss[i][1]);
        TEST_CHECK_(focused() == 1 && renames == 0 && threads_created == 0, "a touch at %d,%d acted", miss[i][0],
                    miss[i][1]);
    }
    /* the export rows: a touch at each one's top and bottom edges starts that export */
    for (int k = 0; k < 4; k++) {
        static const char *const result[4] = {"to 2026-10-12_093005-1card.apkg.", "No cards queued",
                                              "to 2026-10-12_093005-all-1card.apkg.",
                                              "to 2026-10-12_093005-all-1card-2.apkg."};
        int r = 2 + k / 2, y = k % 2 ? row_top[r] + row_height[r] - 1 : row_top[r];
        touch(0, k % 2 ? 935 : 480, y);
        TEST_CHECK_(focused() == r && vjo_anki_page_busy() && threads_created == k + 1,
                    "a touch at y %d did not start row %d's export", y, r);
        run_worker();
        frame(0);
        TEST_CHECK_(!vjo_anki_page_busy() && strstr(drawn.footer, result[k]), "row %d at y %d: %s", r, y, drawn.footer);
    }
    TEST_CHECK(threads_deleted == 4);
    /* Anki for Vita's row, without the app: a touch says where to get it and starts nothing */
    touch(0, 480, row_top[4] + 30);
    TEST_CHECK(focused() == 4 && !vjo_anki_page_busy() && threads_created == 4);
    TEST_CHECK_(strstr(drawn.footer, "not installed") && strstr(drawn.footer, VJO_AFV_WHERE) && !drawn.footer_error,
                "%s", drawn.footer);
}

/* Enter on an export row runs the export on the page's worker thread (64 KiB, the system core):
 * meanwhile the page says so, its rows act on nothing, and Cancel stops it; once it ends the page
 * shows the result and counts the cards again. */
static void an_export_runs_on_the_worker_and_the_page_follows_it(void)
{
    char before[VJO_CONFIG_TEXT_MAX + 1], after[VJO_CONFIG_TEXT_MAX + 1];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_card("猫", 1);
    queue_card("犬", 0);
    show();
    frame(0);
    TEST_CHECK(text_starting("Export cards (2 queued)") && text_starting("2 cards in all"));
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 1 && thread_stack == 64 * 1024 &&
               thread_affinity == SCE_KERNEL_CPU_MASK_SYSTEM && thread_priority == 0x10000100);
    frame(0);
    TEST_CHECK(!strcmp(badge(2)->label, "Exporting…") && badge(2)->active && !strcmp(badge(3)->label, "Export all") &&
               !badge(3)->active);
    TEST_CHECK(text_starting("Reading the cards…") && text_starting("Every card again") &&
               !strcmp(drawn.controls, "○: stop   Select: page"));
    /* busy: no save, no second export, from Enter, Left/Right or a touch */
    TEST_ASSERT(save_now(0, 1) == 0);
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, before, sizeof(before)) > 0);
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == 0); /* (the selection still moves) */
    frame(SCE_CTRL_CROSS);
    frame(SCE_CTRL_RIGHT);
    touch(0, 480, row_top[1] + 10);
    touch(0, 480, row_top[3] + 10);
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, after, sizeof(after)) > 0);
    TEST_CHECK(!strcmp(before, after) && threads_created == 1 && vjo_anki_page_busy());
    /* the export ends: the page joins it and counts again */
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && threads_deleted == 1);
    TEST_CHECK(strstr(drawn.footer, "Exported 2 cards (") && strstr(drawn.footer, "to 2026-10-12_093005-2cards.apkg.") &&
               strstr(drawn.footer, "Copy it from ux0:data/VitaJPOverlay/anki/ by VitaShell's FTP or USB") &&
               !drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(text_starting("Export cards (0 queued)") && text_starting("2 cards in all"));
    TEST_CHECK(!strcmp(badge(2)->label, "Export") && !badge(2)->active);
    TEST_CHECK(there(VJO_EXPORT_DIR "/2026-10-12_093005-2cards.apkg") && files_in(VJO_QUEUE_DIR, ".json") == 0 &&
               files_in(VJO_QUEUE_EXPORTED_DIR, ".json") == 2 && files_in(VJO_QUEUE_EXPORTED_DIR, ".jpg") == 1);
    TEST_CHECK(strstr(logs, "anki: export (queued) rc=0: 2 cards, 2 moved, 0 kept, 0 unreadable, 0 too large, 0 left, "
                            "0 stopped, 0 swept, ") != NULL);
    TEST_MSG("%s", logs);
    /* Export all cards: the two again (the stick stops at the last row, Anki for Vita's; Up then) */
    vjo_anki_page_stick(3);
    frame(0);
    TEST_CHECK(focused() == 4);
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == 3);
    frame(SCE_CTRL_CROSS);
    frame(0);
    TEST_CHECK(!strcmp(badge(3)->label, "Exporting…") && badge(3)->active && !strcmp(badge(2)->label, "Export") &&
               !badge(2)->active);
    /* the progress on its own row only; the last export's result is not the footer while this one runs */
    TEST_CHECK(text_starting("Reading the cards…") && text_starting("Writes a new .apkg"));
    TEST_CHECK_(!strstr(drawn.footer, "Exported") && strstr(drawn.footer, "Copy the .apkg from"), "busy: %s", drawn.footer);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 2 cards (") && strstr(drawn.footer, "to 2026-10-12_093005-all-2cards.apkg."));
    TEST_MSG("%s", drawn.footer);
    /* Cancel: stopped, nothing changed */
    queue_card("鳥", 0);
    show();
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_CROSS); /* row 2 */
    TEST_CHECK(focused() == 2 && vjo_anki_page_busy());
    frame(SCE_CTRL_CIRCLE);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && !strcmp(drawn.footer, "Stopped. Nothing was changed.") && drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 2);
    /* Cancel with nothing running does nothing */
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(!vjo_anki_page_busy() && threads_created == 3);
    /* leaving the app joins a running export, cancelled: the queue as it was */
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 4);
    vjo_anki_page_quit();
    TEST_CHECK(!vjo_anki_page_busy() && !pending && threads_deleted == 4);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 2);
    /* with ○ as enter, ○ during an export does not stop it, and × does */
    buttons = vjo_buttons(VJO_BUTTON_ASSIGN_CIRCLE);
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(focused() == 2 && vjo_anki_page_busy() && threads_created == 5);
    frame(0);
    TEST_CHECK(!strcmp(drawn.controls, "×: stop   Select: page"));
    frame(SCE_CTRL_CIRCLE);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && strstr(drawn.footer, "Exported 1 card (") && !drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 0 && files_in(VJO_EXPORT_DIR, ".apkg") == 3);
    queue_card("魚", 0);
    show();
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 6);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && !strcmp(drawn.footer, "Stopped. Nothing was changed.") && drawn.footer_error);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 3);
}

/* An unreadable config.ini is never written: its rows show "--" and say why; a save that does not
 * complete says so; a queue that cannot be read is shown as such; a thread that cannot start says so. */
static void a_missing_screenshot_is_reported_after_export(void)
{
    char path[256];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", queue_card("猫", 1));
    TEST_ASSERT(unlink(path) == 0);
    show();
    touch(0, 480, row_top[2] + 10);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && text_starting("Export cards (0 queued)"));
    TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") &&
               strstr(drawn.footer, "1 exported without a screenshot") && drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(strstr(logs, "1 without screenshots") != NULL);
}

static void failures_are_shown_and_change_nothing(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    TEST_ASSERT(save_now(1, 1) == 0);
    fail_open = "config.ini";
    show();
    frame(0);
    TEST_CHECK(!strcmp(badge(0)->label, "--") && !badge(0)->active && !strcmp(badge(1)->label, "--") &&
               !badge(1)->active);
    writes_opened = renames = 0;
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Cannot read config.ini. Choices have not been changed.") && drawn.footer_error);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_RIGHT);
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(writes_opened == 0 && renames == 0);
    fail_open = NULL;
    check_saved(1, 1);
    /* a save that does not complete: said, and the choice shown is the file's */
    show();
    frame(SCE_CTRL_UP);
    fail_rename = 1;
    frame(SCE_CTRL_CROSS);
    fail_rename = 0;
    TEST_CHECK(!strcmp(drawn.footer, "Save not confirmed. Check the choices shown, then retry.") && drawn.footer_error);
    frame(0);
    TEST_CHECK(!strcmp(badge(0)->label, "ON"));
    check_saved(1, 1);
    /* the queue unreadable */
    queue_card("猫", 0);
    fail_dopen = VJO_QUEUE_DIR;
    show();
    frame(0);
    const Text *t = text_starting("Export cards (queue unreadable)");
    TEST_CHECK(t && t->color == OFF && text_starting("Unreadable"));
    fail_dopen = NULL;
    /* the worker cannot start */
    fail_create = 1;
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == 2);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!vjo_anki_page_busy() && !strcmp(drawn.footer, "Could not start the export (0x80020001).") &&
               drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    fail_create = 0;
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1);
}

/* ---------------- Anki for Vita (#220) ---------------- */

static void put_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_(f != NULL, "%s", path);
    TEST_ASSERT(fwrite(text, 1, strlen(text), f) == strlen(text));
    fclose(f);
}
static void afv_installed(void)
{
    mkdir(VJO_AFV_DIR, 0777);
    put_text_file(VJO_AFV_DIR "/interface.json", "{\"v\": 1, \"app\": \"Anki for Vita\", \"app_version\": \"0.1.0\", "
                                                 "\"interfaces\": [1]}");
}

/* Installed: the page reads its results on the worker when it is shown (Cancel still leaves the app),
 * says how its last sync went, and asks for the duplicate index; Send hands the queued cards over
 * on the worker, which they stay in the queue for; once the app's results say a card is in Anki,
 * the next show moves it out of the queue. */
static void cards_go_to_anki_for_vita_through_its_inbox(void)
{
    char ids[2][VJO_QUEUE_ID_SIZE], path[256];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    afv_installed();
    /* (the app's status, written 5 minutes before the console's clock) */
    put_text_file(VJO_AFV_DIR "/status.json", "{\"v\":1,\"signed_in\":true,\"server\":\"anki.example.org\","
                                              "\"last_sync\":1791789905,\"last_result\":\"ok\",\"pending\":0}");
    snprintf(ids[0], sizeof(ids[0]), "%s", queue_card("猫", 1));
    snprintf(ids[1], sizeof(ids[1]), "%s", queue_card("犬", 0));
    show();
    TEST_CHECK(threads_created == 1 && !vjo_anki_page_busy()); /* the results, not a job of the user's */
    frame(0);
    TEST_CHECK(text_starting("Reading Anki for Vita's results") && !strcmp(drawn.controls,
               "Up/Down: choose   ×: select   Select: page   ○: exit"));
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && threads_deleted == 1);
    {
        const Text *afv = text_starting("Send to Anki for Vita");
        TEST_CHECK(afv && afv->color == WHITE && !strcmp(badge(4)->label, "Send") && !badge(4)->active);
        TEST_CHECK_(text_starting("anki.example.org: last sync 5 min ago, ok.") != NULL, "status line missing");
    }
    /* Rubi's duplicate index request, written when the page was shown */
    TEST_CHECK(there(VJO_AFV_DIR "/index/rubi.request.json"));
    /* Send: on the worker, as an export */
    vjo_anki_page_stick(4);
    frame(0);
    TEST_CHECK(focused() == 4);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 2);
    frame(0);
    TEST_CHECK(!strcmp(badge(4)->label, "Sending…") && badge(4)->active && !strcmp(drawn.controls, "○: stop   Select: page"));
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy());
    TEST_CHECK_(strstr(drawn.footer, "Handed 2 cards to Anki for Vita: it adds them to your collection at its next sync.") &&
                    !drawn.footer_error, "%s", drawn.footer);
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), VJO_AFV_INBOX_DIR "/%s.json", ids[i]);
        TEST_CHECK(there(path));
    }
    TEST_CHECK(files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 1 && there(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json"));
    TEST_CHECK(text_starting("Export cards (2 queued)")); /* still queued: the .apkg export keeps working */
    TEST_CHECK(strstr(logs, "anki for vita: send rc=0: 2 handed over") != NULL);
    /* Anki for Vita syncs: the first is in Anki, the second refused */
    snprintf(path, sizeof(path), VJO_AFV_INBOX_DIR "/%s.json", ids[0]);
    TEST_ASSERT(unlink(path) == 0);
    snprintf(path, sizeof(path), VJO_AFV_INBOX_DIR "/%s.json", ids[1]);
    TEST_ASSERT(unlink(path) == 0);
    mkdir(VJO_AFV_DIR "/outbox", 0777);
    mkdir(VJO_AFV_OUTBOX_DIR, 0777);
    {
        char text[300];
        snprintf(path, sizeof(path), VJO_AFV_OUTBOX_DIR "/%s.json", ids[0]);
        snprintf(text, sizeof(text), "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true,\"note_id\":1,"
                                     "\"time\":1791790200,\"message\":\"\"}", ids[0]);
        put_text_file(path, text);
        snprintf(path, sizeof(path), VJO_AFV_OUTBOX_DIR "/%s.json", ids[1]);
        snprintf(text, sizeof(text), "{\"v\":1,\"id\":\"%s\",\"status\":\"rejected\",\"synced\":false,\"note_id\":0,"
                                     "\"time\":1791790200,\"message\":\"a field name is not the note type's\"}", ids[1]);
        put_text_file(path, text);
    }
    put_text_file(VJO_AFV_DIR "/status.json", "{\"v\":1,\"signed_in\":true,\"server\":\"anki.example.org\","
                                              "\"last_sync\":1791790175,\"last_result\":\"ok\",\"pending\":0}");
    show();
    TEST_CHECK(threads_created == 3);
    run_worker();
    frame(0);
    TEST_CHECK_(strstr(drawn.footer, "Anki for Vita: 1 card is in Anki now (out of the queue).") &&
                    strstr(drawn.footer, "1 refused, still queued: a field name is not the note type's") &&
                    drawn.footer_error, "%s", drawn.footer);
    TEST_CHECK(text_starting("Export cards (1 queued)") && text_starting("2 cards in all"));
    TEST_CHECK(text_starting("anki.example.org: last sync under a minute ago, ok."));
    snprintf(path, sizeof(path), VJO_QUEUE_EXPORTED_DIR "/%s.json", ids[0]);
    TEST_CHECK(there(path));
    /* its media file, which no note in the inbox names now, and the note type's, removed */
    TEST_CHECK(files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 0 && !there(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json"));
    /* the refused card is not handed in again; the .apkg export takes it */
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK_(strstr(drawn.footer, "1 refused by Anki for Vita before stays queued: export it as an .apkg") != NULL,
                "%s", drawn.footer);
    snprintf(path, sizeof(path), VJO_AFV_INBOX_DIR "/%s.json", ids[1]);
    TEST_CHECK(!there(path));
    /* leaving the app while the results are read: it stops at the next file and is joined */
    show();
    TEST_CHECK(threads_created == 5 && !vjo_anki_page_busy());
    vjo_anki_page_quit();
    TEST_CHECK(!vjo_anki_page_busy() && threads_deleted == 5);
}

/* Anki for Vita too old (no interface 1): its row is not offered, and nothing is written there. */
static void anki_for_vita_too_old_is_not_offered(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    mkdir(VJO_AFV_DIR, 0777);
    put_text_file(VJO_AFV_DIR "/interface.json", "{\"v\": 1, \"app\": \"Anki for Vita\", \"interfaces\": [2]}");
    queue_card("猫", 0);
    show();
    TEST_CHECK(threads_created == 0);
    vjo_anki_page_stick(4);
    frame(0);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(threads_created == 0 && !vjo_anki_page_busy());
    TEST_CHECK_(strstr(drawn.footer, "not installed (or too old for Rubi)") != NULL, "%s", drawn.footer);
    TEST_CHECK(!strcmp(badge(4)->label, "--") && !there(VJO_AFV_INBOX_DIR) && !there(VJO_AFV_DIR "/index"));
}

TEST_LIST = {
    {"the_rows_and_the_footer_do_not_overlap", the_rows_and_the_footer_do_not_overlap},
    {"up_down_and_the_stick_choose_a_row", up_down_and_the_stick_choose_a_row},
    {"enter_and_left_right_save_each_row_s_key", enter_and_left_right_save_each_row_s_key},
    {"touch_chooses_and_acts_on_each_row", touch_chooses_and_acts_on_each_row},
    {"an_export_runs_on_the_worker_and_the_page_follows_it", an_export_runs_on_the_worker_and_the_page_follows_it},
    {"a_missing_screenshot_is_reported_after_export", a_missing_screenshot_is_reported_after_export},
    {"failures_are_shown_and_change_nothing", failures_are_shown_and_change_nothing},
    {"cards_go_to_anki_for_vita_through_its_inbox", cards_go_to_anki_for_vita_through_its_inbox},
    {"anki_for_vita_too_old_is_not_offered", anki_for_vita_too_old_is_not_offered},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    /* the memory card, removed when the suite ends (acutest runs each test in a child: test_tmpdir.h) */
    const char *dir = vjo_test_tmpdir("settings-anki");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
