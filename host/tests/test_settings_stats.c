/* The Rubi app's Stats tab (settings/stats_page.c, #312) on the host. The page draws through main.c's helpers
 * (VjoStatsUi), recorded here, over real files in a temporary memory card (ux0:data/VitaJPOverlay/history.csv in
 * the writer's format, the Anki queue's records, app.ini, stats_cache.bin), with its count on its worker thread,
 * which these stubs run when a test says. Checked: each game's row shows what the Triangle view shows for that
 * game (the Shell's own count, core/history_lines.c: today in this game and this game's total), the rows' layout
 * and scrolling, the goal saved in app.ini (never config.ini) with Left/Right and Enter, the days chosen on the 14
 * days and the year, touch, the cache read on and its every check (an append is counted from where it stopped; a
 * replaced, shorter or damaged file, another time zone or another reading_time counts again), the cards per game
 * (the queue's and the archive's records, each card once, read once), a history.csv that is not there, and a
 * count stopped at quit.
 *
 * Text widths are the settings harness's stub metrics (Latin 10, other characters 18 units per 18-unit em), not
 * the firmware's PVF fonts. Each test runs in its own process (acutest's default): the page keeps its state in
 * statics, as on the console. */
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
#include "../../core/history_lines.h"
#include "../../core/stats.h"
#include "../../settings/anki_page.h"
#include "../../settings/app_prefs.h"
#include "../../settings/stats_page.h"

/* settings/stats_page.c's layout */
#define VIEW_Y 112
#define VIEW_END 466
#define FOOTER_Y 470
#define FOOTER_H 30
#define WHITE 1u
#define DIM 2u
#define ACCENT 3u
#define OFF 4u
#define CARD 5u
#define SELECTED 6u
enum { ROW_TODAY, ROW_GOAL, ROW_DAYS, ROW_YEAR, ROW_ALL, ROW_GAMES };

/* ---------------- the memory card: real files under the suite's directory ---------------- */

static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
static const char *fail_open; /* an open of a path containing this fails (EIO) */
static int opens_of_cards, history_reads;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (fail_open && strstr(p, fail_open))
        return (int)0x80010005u;
    if (strstr(p, "anki_queue/") && strstr(p, ".json"))
        opens_of_cards++;
    return native(open(p, flags, mode));
}
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t n) { return native((int)read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n) { return native((int)write(fd, p, n)); }
int sceIoPread(int fd, void *p, size_t n, SceOff off)
{
    history_reads++;
    return native((int)pread(fd, p, n, off));
}
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off) { return native((int)pwrite(fd, p, n, off)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    if (stat(p, &s))
        return native(-1);
    memset(st, 0, sizeof(*st));
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    return 0;
}
int sceIoRename(const char *a, const char *b) { return native(rename(a, b)); }
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
int sceIoSyncByFd(int fd, int flag) { (void)flag; return native(fsync(fd)); }
static DIR *dirs[4];
static char dir_paths[4][256];
int sceIoDopen(const char *p)
{
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

/* ---------------- the clock: now, and the Vita's time zone ---------------- */

#define RTC_EPOCH 62135596800000000ull
static uint64_t now_us;         /* UTC */
static int32_t zone_minutes = 540; /* the Vita's time zone: Japan */
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = RTC_EPOCH + now_us;
    return 0;
}
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local)
{
    local->tick = utc->tick + (uint64_t)((int64_t)zone_minutes * 60000000ll);
    return 0;
}
int sceRtcGetCurrentClockLocalTime(SceDateTime *t) { memset(t, 0, sizeof(*t)); return 0; }
uint64_t sceKernelGetProcessTimeWide(void) { return now_us; }

/* ---------------- the page's thread, run when the test says ---------------- */

static int (*thread_entry)(SceSize, void *), (*pending)(SceSize, void *);
static int threads_created, thread_priority, thread_affinity;
static unsigned thread_stack;
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { (void)n; (void)a; (void)c; (void)o; return 0x100; }
int sceKernelLockMutex(SceUID u, int c, void *t) { (void)u; (void)c; (void)t; return 0; }
int sceKernelUnlockMutex(SceUID u, int c) { (void)u; (void)c; return 0; }
SceUID sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned int s, unsigned int a, int af, void *o)
{
    (void)n; (void)a; (void)o;
    thread_entry = e;
    thread_priority = p;
    thread_stack = s;
    thread_affinity = af;
    return 0x200 + ++threads_created;
}
int sceKernelStartThread(SceUID u, SceSize s, void *a) { (void)u; (void)s; (void)a; pending = thread_entry; return 0; }
static void run_worker(void)
{
    int (*e)(SceSize, void *) = pending;
    pending = NULL;
    if (e)
        e(0, NULL);
}
int sceKernelWaitThreadEnd(SceUID u, int *s, void *t) { (void)u; (void)s; (void)t; run_worker(); return 0; }
int sceKernelDeleteThread(SceUID u) { (void)u; return 0; }
int sceKernelDeleteMutex(SceUID u) { (void)u; return 0; }

/* ---------------- the app's log and its display ---------------- */

static char logs[16384];
void vjo_log_enable(int on) { (void)on; }
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
static int clipping;
void vita2d_enable_clipping(void) { clipping = 1; }
void vita2d_disable_clipping(void) { clipping = 0; }
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void vita2d_start_drawing(void) {}
void vita2d_clear_screen(void) {}
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) {}

/* ---------------- what a frame draws (main.c's helpers, recorded) ---------------- */

typedef struct { int x, y; unsigned color; float px; int clipped; char s[320]; } Text; /* clipped: drawn inside a row cut at the room's end */
typedef struct { int y, height, focus; } Card;
typedef struct { int x, y, width, height; unsigned color; } Rect;
static struct {
    Text text[96];
    Card card[16];
    Rect rect[600];
    int texts, cards, rects, footers;
    int footer_y, footer_h, footer_error;
    char footer[320], controls[160], badge[64];
    int badge_active;
} drawn;
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
static void d_text(int x, int y, unsigned color, float px, const char *s)
{
    if (drawn.texts < 96) {
        drawn.text[drawn.texts] = (Text){x, y, color, px, clipping, ""};
        snprintf(drawn.text[drawn.texts++].s, 320, "%s", s);
    }
}
static int d_width(float px, const char *s) { return width_of(s, px); }
static void d_wrapped(int x, int y, int width, unsigned color, float px, int line_height, int lines, const char *s)
{
    (void)width; (void)line_height; (void)lines;
    d_text(x, y, color, px, s);
}
static void d_card(int y, int height, int focus)
{
    if (drawn.cards < 16)
        drawn.card[drawn.cards++] = (Card){y, height, focus};
}
static void d_badge(int x, int y, int width, int height, const char *label, int active)
{
    (void)x; (void)y; (void)width; (void)height;
    snprintf(drawn.badge, sizeof(drawn.badge), "%s", label);
    drawn.badge_active = active;
}
static void d_footer(int y, int height, int text_y, int lines, const char *message, int error, const char *controls)
{
    (void)text_y; (void)lines;
    drawn.footers++;
    drawn.footer_y = y, drawn.footer_h = height, drawn.footer_error = error;
    snprintf(drawn.footer, sizeof(drawn.footer), "%s", message);
    snprintf(drawn.controls, sizeof(drawn.controls), "%s", controls);
}
static int headers;
static void d_header(void) { headers++; }
static void d_rect(int x, int y, int width, int height, unsigned color)
{
    if (drawn.rects < 600)
        drawn.rect[drawn.rects++] = (Rect){x, y, width, height, color};
}

static VjoButtons buttons = {SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, "Cross", "Circle"};
static const VjoStatsUi ui = {d_text, d_width, d_wrapped, d_card, d_badge, d_footer, d_header, d_rect,
                              WHITE, DIM, ACCENT, OFF, CARD, SELECTED, &buttons};
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;

static void touch(uint32_t pressed, int x, int y)
{
    memset(&drawn, 0, sizeof(drawn));
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_stats_page_frame(&ui, pressed, x, y, &scratch);
}
static void frame(uint32_t pressed) { touch(pressed, -1, -1); }
/* The page shown, its count run, and a frame drawn. */
static void show(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_stats_page_show(&scratch);
    run_worker();
    frame(0);
}
static const Text *text_with(const char *part)
{
    for (int i = 0; i < drawn.texts; i++)
        if (strstr(drawn.text[i].s, part))
            return &drawn.text[i];
    return NULL;
}
static int focused_row_top(void)
{
    for (int i = 0; i < drawn.cards; i++)
        if (drawn.card[i].focus)
            return drawn.card[i].y;
    return -1;
}

/* ---------------- history.csv as the Shell's writer makes it (shell/history.c) ---------------- */

static char history[1 << 20];
static size_t history_len;

static void field(const char *s)
{
    history[history_len++] = '"';
    for (; *s; s++) {
        if (*s == '"')
            history[history_len++] = '"';
        history[history_len++] = *s;
    }
    history[history_len++] = '"';
}
static void record(const char *tid, const char *name, const char *text, uint64_t us)
{
    if (!history_len)
        history_len += (size_t)snprintf(history, sizeof(history),
            "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,"
            "monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,fb_format,"
            "lines,recognized,frames,find_us,load_us,recognize_us,capture_us,captures,tears,region_sum,source,"
            "capture_memory,capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,"
            "characters,line_source\n");
    history_len += (size_t)snprintf(history + history_len, sizeof(history) - history_len, "\"vita-%s\",\"vjo-%llu-%zu\",",
                                    tid, (unsigned long long)us, history_len);
    field(name);
    history[history_len++] = ',';
    field(text);
    history_len += (size_t)snprintf(history + history_len, sizeof(history) - history_len,
                                    ",%llu.%06u,\"vita\",\"%s\",\"vocr\",\"m\",", (unsigned long long)(us / 1000000),
                                    (unsigned)(us % 1000000), tid);
    field(text);
    history_len += (size_t)snprintf(history + history_len, sizeof(history) - history_len,
                                    ",1,2,3,4,5,0,0,960,544,960,544,960,0,1,1,9,1,2,3,4,1,0,0,0,0,0,0,0,0,0,\"1:2\",%u,"
                                    "\"ocr\"\n", vjo_line_characters(text, strlen(text)));
}
static void write_history(void)
{
    FILE *f = fopen("ux0:data/VitaJPOverlay/history.csv", "wb");
    TEST_ASSERT(f && fwrite(history, 1, history_len, f) == history_len && !fclose(f));
}

/* 2026-10-27 in Japan: 00:00 there is 15:00 UTC the day before */
static uint64_t jst(int day, int hour, int minute, int second)
{
    return (uint64_t)((int64_t)vjo_stats_days_from_civil(2026, 10, day) * 86400 + hour * 3600 + minute * 60 + second -
                      9 * 3600) * 1000000ull;
}

/* Lines of three games over three days (Japan): CLANNAD each day, TIME TRAVELERS on the 25th and today. */
static void three_games(void)
{
    static const char *const lines[] = {"「朝だよ、起きて」", "ABC あいうえお", "「\"引用\"」と言った", "かきくけこさしすせそ",
                                        "漢字\n二行目", "たちつてと"};
    for (int d = 25; d <= 27; d++)
        for (int i = 0; i < 12; i++) {
            record("PCSG00415", "CLANNAD", lines[i % 6], jst(d, 21, i, i * 3));
            if (d != 26 && i % 3 == 0)
                record("PCSG00062", "TIME TRAVELERS", lines[(i + 1) % 6], jst(d, 21, i, i * 3 + 20));
        }
    record("PCSG00007", "リトルバスターズ！", "「理樹」", jst(20, 10, 0, 0));
    record("PCSG00007", "リトルバスターズ！", "「恭介」", jst(20, 10, 0, 12));
    write_history();
}

static void reset(void)
{
    char data[PATH_MAX], overlay[PATH_MAX];
    TEST_ASSERT(snprintf(data, sizeof(data), "%s/ux0:data", vjo_test_tmpdir_path) < (int)sizeof(data));
    TEST_ASSERT(snprintf(overlay, sizeof(overlay), "%s/VitaJPOverlay", data) < (int)sizeof(overlay));
    TEST_ASSERT(vjo_test_remove_tree(data) == 0);
    TEST_ASSERT(mkdir(data, 0777) == 0 && mkdir(overlay, 0777) == 0);
    history_len = 0;
    logs[0] = '\0';
    now_us = jst(27, 23, 30, 0);
    zone_minutes = 540;
    vjo_reading_rule = VJO_READING_ADAPTIVE;
}

/* The Shell's count of game tid in history.csv: its total, and today's (the Triangle view's two blocks). */
static void shell_count(const char *tid, VjoStatsCount *total, VjoStatsCount *day)
{
    static char copy[1 << 20];
    size_t first, last;
    VjoReadingTime time;
    VjoReadingScope scope;
    memcpy(copy, history, history_len);
    memset(&time, 0, sizeof(time));
    memset(&scope, 0, sizeof(scope));
    snprintf(scope.uuid, sizeof(scope.uuid), "vita-%s", tid);
    scope.start_us = jst(27, 0, 0, 0);
    scope.end_us = jst(28, 0, 0, 0);
    total->chars = vjo_history_count_scope(copy, history_len, 1, 0, &first, &last, &time, &scope);
    total->active_us = time.active_us;
    total->timed_chars = time.timed_chars;
    day->chars = scope.chars;
    day->active_us = scope.active_us;
    day->timed_chars = scope.timed_chars;
}
/* "1,234 characters, 12 min, 6,170/hour": the Triangle view's three numbers (its characters/hour, the timed lines'
 * characters over their time), as the page writes them */
static void summary(char *out, size_t cap, const VjoStatsCount *n)
{
    char chars[32], speed[32], time[32];
    uint64_t minutes = n->active_us / 60000000ull;
    vjo_history_number(chars, sizeof(chars), n->chars);
    if (minutes < 60)
        snprintf(time, sizeof(time), "%u min", (unsigned)minutes);
    else
        snprintf(time, sizeof(time), "%llu h %02u min", (unsigned long long)(minutes / 60), (unsigned)(minutes % 60));
    if (!n->active_us) {
        snprintf(out, cap, "%s characters, %s", chars, time);
        return;
    }
    vjo_history_number(speed, sizeof(speed), n->timed_chars * 3600000000ull / n->active_us);
    snprintf(out, cap, "%s characters, %s, %s/hour", chars, time, speed);
}

/* ---------------- the tests ---------------- */

static void each_game_shows_the_triangle_views_numbers(void)
{
    static const char *const tids[] = {"PCSG00415", "PCSG00062", "PCSG00007"};
    static const char *const names[] = {"CLANNAD", "TIME TRAVELERS", "リトルバスターズ！"};
    for (int rule = VJO_READING_ADAPTIVE; rule <= VJO_READING_GAPS_15S; rule++) {
        reset();
        three_games();
        FILE *ini = fopen("ux0:data/VitaJPOverlay/config.ini", "wb");
        fprintf(ini, "reading_time = %s\n", rule ? "15s" : "adaptive");
        fclose(ini);
        show();
        TEST_CHECK(thread_priority == 0x10000100 && thread_affinity == SCE_KERNEL_CPU_MASK_SYSTEM &&
                   thread_stack == 64 * 1024);
        vjo_reading_rule = (uint32_t)rule;
        /* the games come newest first: CLANNAD and TIME TRAVELERS today, リトルバスターズ！ on the 20th */
        for (int g = 0; g < 3; g++) {
            VjoStatsCount total, day;
            char want[200], line[260];
            shell_count(tids[g], &total, &day);
            /* the column, scrolled down to the game's row */
            for (int i = 0; i < 12; i++) {
                frame(0);
                if (text_with(names[g]) && text_with("In all:") && focused_row_top() >= 0) {
                    const Text *t = text_with(names[g]);
                    int game_row = 0;
                    for (int k = 0; k < drawn.cards; k++)
                        if (drawn.card[k].focus && drawn.card[k].y <= t->y && t->y < drawn.card[k].y + drawn.card[k].height)
                            game_row = 1;
                    if (game_row)
                        break;
                }
                frame(SCE_CTRL_DOWN);
            }
            summary(want, sizeof(want), &total);
            snprintf(line, sizeof(line), "In all: %s", want);
            TEST_CHECK_(text_with(line) != NULL, "rule %d %s: want \"%s\"", rule, names[g], line);
            if (day.chars) {
                summary(want, sizeof(want), &day);
                snprintf(line, sizeof(line), "Today: %s", want);
            } else
                snprintf(line, sizeof(line), "Today: nothing yet");
            TEST_CHECK_(text_with(line) != NULL, "rule %d %s: want \"%s\"", rule, names[g], line);
        }
        TEST_CHECK(text_with("last read Tue 20 Oct") != NULL && text_with("read today") != NULL);
        /* config.ini's rule is the count's: the log names the records */
        TEST_CHECK(strstr(logs, "stats: history.csv ") != NULL);
    }
}

static void the_rows_scroll_inside_their_room(void)
{
    reset();
    three_games();
    show();
    TEST_CHECK(drawn.footers == 1 && drawn.footer_y == FOOTER_Y && drawn.footer_h == FOOTER_H);
    TEST_CHECK(strstr(drawn.footer, "Counted ") && strstr(drawn.footer, "Start: count again") && !drawn.footer_error);
    int rows = ROW_GAMES + 3;
    for (int r = 0; r < rows; r++) {
        int focused = 0;
        for (int k = 0; k < drawn.cards; k++) {
            TEST_CHECK_(drawn.card[k].y >= VIEW_Y && drawn.card[k].y < VIEW_END, "row %d card %d at %d", r, k,
                        drawn.card[k].y);
            focused += drawn.card[k].focus;
            if (drawn.card[k].focus) /* the chosen row is always whole */
                TEST_CHECK(drawn.card[k].y + drawn.card[k].height <= VIEW_END);
        }
        TEST_CHECK_(focused == 1, "row %d: %d focused", r, focused);
        for (int k = 0; k < drawn.texts; k++)
            TEST_CHECK_(drawn.text[k].clipped || drawn.text[k].y < FOOTER_Y || drawn.text[k].y > FOOTER_Y + FOOTER_H,
                        "%s at %d", drawn.text[k].s, drawn.text[k].y);
        if (r < rows - 1)
            frame(SCE_CTRL_DOWN);
    }
    /* the last row is the oldest game's; Down from it goes round to Today, Up from Today back to it */
    TEST_CHECK(text_with("リトルバスターズ！") != NULL);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(text_with("Today") && focused_row_top() == VIEW_Y);
    frame(SCE_CTRL_UP);
    TEST_CHECK(text_with("リトルバスターズ！") != NULL);
    frame(SCE_CTRL_DOWN);
    /* the stick stops at the first row */
    for (int i = 0; i < 12; i++)
        frame(SCE_CTRL_UP);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused_row_top() == VIEW_Y);
    vjo_stats_page_stick(-1);
    frame(0);
    TEST_CHECK(focused_row_top() == VIEW_Y && text_with("Today") != NULL);
}

static int app_ini_says(const char *line)
{
    char text[512] = "";
    FILE *f = fopen("ux0:data/VitaJPOverlay/app.ini", "rb");
    if (!f)
        return 0;
    text[fread(text, 1, sizeof(text) - 1, f)] = 0;
    fclose(f);
    return strstr(text, line) != NULL;
}

static void the_goal_is_saved_in_app_ini(void)
{
    reset();
    three_games();
    FILE *ini = fopen("ux0:data/VitaJPOverlay/config.ini", "wb");
    fprintf(ini, "log_file = on\n");
    fclose(ini);
    show();
    TEST_CHECK(text_with("No daily goal") != NULL && text_with("Streak: 3 days with reading, best 3") != NULL);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(!strcmp(drawn.badge, "Off") && !drawn.badge_active);
    frame(SCE_CTRL_LEFT); /* nothing before Off */
    TEST_CHECK(!strcmp(drawn.badge, "Off") && !app_ini_says("stats_daily_goal"));
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(!strcmp(drawn.badge, "2,000 characters") && drawn.badge_active && app_ini_says("stats_daily_goal = 2000_characters\n"));
    TEST_CHECK(strstr(drawn.footer, "Saved in app.ini") && !drawn.footer_error);
    for (int i = 0; i < 4; i++)
        frame(SCE_CTRL_RIGHT);
    TEST_CHECK(!strcmp(drawn.badge, "50,000 characters") && app_ini_says("= 50000_characters\n"));
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(!strcmp(drawn.badge, "15 minutes") && app_ini_says("= 15_minutes\n"));
    for (int i = 0; i < 6; i++)
        frame(SCE_CTRL_RIGHT); /* stops at 120 minutes */
    TEST_CHECK(!strcmp(drawn.badge, "120 minutes"));
    frame(SCE_CTRL_CROSS); /* Enter goes round */
    TEST_CHECK(!strcmp(drawn.badge, "Off") && app_ini_says("stats_daily_goal = off\n"));
    /* never config.ini: the overlay would show "unknown setting" for it */
    char cfg[256] = "";
    FILE *f = fopen("ux0:data/VitaJPOverlay/config.ini", "rb");
    cfg[fread(cfg, 1, sizeof(cfg) - 1, f)] = 0;
    fclose(f);
    TEST_CHECK(!strstr(cfg, "stats"));
    /* a goal of 2,000 characters: today's bar and the streak follow it (each day here has under 2,000) */
    frame(SCE_CTRL_RIGHT);
    frame(SCE_CTRL_UP);
    TEST_CHECK(text_with("of 2,000 characters") != NULL && text_with("Streak: 0 days at the goal, best 0") != NULL);
    /* the goal is the app's: another show reads it back, and app.ini's other lines stay */
    f = fopen("ux0:data/VitaJPOverlay/app.ini", "ab");
    fprintf(f, "; a comment\nother_key = 1\n");
    fclose(f);
    show();
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(!strcmp(drawn.badge, "2,000 characters"));
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(app_ini_says("; a comment\nother_key = 1\n") && app_ini_says("stats_daily_goal = 5000_characters\n"));
    /* a touch on the badge's sides */
    touch(0, 520, VIEW_Y + 104 + 4 + 20);
    TEST_CHECK(!strcmp(drawn.badge, "2,000 characters"));
    touch(0, 880, VIEW_Y + 104 + 4 + 20);
    TEST_CHECK(!strcmp(drawn.badge, "5,000 characters"));
}

static void the_days_and_the_year(void)
{
    reset();
    three_games();
    show();
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN); /* Last 14 days */
    TEST_CHECK(text_with("Tue 27 Oct: ") != NULL && strstr(drawn.controls, "Left/Right: day"));
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(text_with("Mon 26 Oct: ") != NULL);
    frame(SCE_CTRL_LTRIGGER); /* a week back */
    TEST_CHECK(text_with("Mon 19 Oct: 0 characters") != NULL);
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(text_with("Tue 20 Oct: ") != NULL && !text_with("Tue 20 Oct: 0 characters"));
    for (int i = 0; i < 20; i++)
        frame(SCE_CTRL_RIGHT); /* stops at today */
    TEST_CHECK(text_with("Tue 27 Oct: ") != NULL);
    /* fourteen bars, the 14th today's */
    int bars = 0;
    for (int k = 0; k < drawn.rects; k++)
        bars += drawn.rect[k].width == 46 && drawn.rect[k].height == 70 && drawn.rect[k].color == CARD; /* (a focused row's slots) */
    TEST_CHECK_(bars == 14, "%d bars", bars);
    frame(SCE_CTRL_DOWN); /* the year */
    TEST_CHECK(text_with("2026") != NULL && text_with("Tue 27 Oct: ") != NULL);
    int cells = 0;
    for (int k = 0; k < drawn.rects; k++)
        cells += drawn.rect[k].width == 12 && drawn.rect[k].height == 12;
    TEST_CHECK_(cells == 365, "%d cells", cells);
    frame(SCE_CTRL_RIGHT); /* no day after today */
    TEST_CHECK(text_with("Tue 27 Oct: ") != NULL);
    frame(SCE_CTRL_LTRIGGER);
    TEST_CHECK(text_with("Tue 20 Oct: 4 characters") != NULL); /* 「理樹」「恭介」 */
    /* a touch on a cell chooses its day: the year's first is a Thursday (row 4 of column 0) */
    const Text *year = text_with("2026");
    int card_top = year ? year->y - 30 : 0;
    touch(0, 72 + 3, card_top + 42 + 4 * 14 + 3);
    TEST_CHECK(text_with("Thu 1 Jan: 0 characters") != NULL);
}

static void the_cache_is_read_on_and_checked(void)
{
    reset();
    three_games();
    show();
    struct stat st;
    TEST_CHECK(stat("ux0:data/VitaJPOverlay/stats_cache.bin", &st) == 0 && st.st_size > 0);
    TEST_CHECK(strstr(logs, "counted from 0 ") && strstr(logs, "the cache absent, then saved"));
    /* an append: counted from where the last count stopped */
    size_t was = history_len;
    record("PCSG00415", "CLANNAD", "「続き」", jst(27, 23, 0, 0));
    write_history();
    logs[0] = 0;
    show();
    char want[96];
    snprintf(want, sizeof(want), "counted from %zu ", was);
    TEST_CHECK_(strstr(logs, want) && strstr(logs, "the cache read, then saved"), "%s", logs);
    /* nothing new: nothing counted, nothing written */
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, " 0 B counted from") && strstr(logs, "then unchanged"), "%s", logs);
    /* a replaced file (the same size, other bytes) and a shorter one: counted again */
    history[history_len - 200] ^= 1;
    write_history();
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, "is not the one counted") && strstr(logs, "counted from 0 "), "%s", logs);
    history_len = was;
    write_history();
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, "is not the one counted") && strstr(logs, "counted from 0 "), "%s", logs);
    /* a damaged cache, another time zone, another reading_time: counted again */
    FILE *f = fopen("ux0:data/VitaJPOverlay/stats_cache.bin", "r+b");
    fseek(f, 100, SEEK_SET);
    fputc(0x55, f);
    fclose(f);
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, "the cache damaged") && strstr(logs, "counted from 0 "), "%s", logs);
    zone_minutes = 0;
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, "the cache of another time zone") && strstr(logs, "counted from 0 "), "%s", logs);
    f = fopen("ux0:data/VitaJPOverlay/config.ini", "wb");
    fprintf(f, "reading_time = 15s\n");
    fclose(f);
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, "the cache of another reading_time") && strstr(logs, "counted from 0 "), "%s", logs);
    /* a record still being written (no line break at the end): left for the next count, then counted once */
    record("PCSG00415", "CLANNAD", "「最後」", jst(27, 23, 10, 0));
    history_len -= 5;
    write_history();
    logs[0] = 0;
    show();
    TEST_CHECK(strstr(logs, "(0 records;") != NULL);
    history_len += 5;
    write_history();
    logs[0] = 0;
    show();
    TEST_CHECK_(strstr(logs, "(1 records;") != NULL, "%s", logs);
}

static void write_card(const char *dir, const char *id, const char *platform, const char *tid)
{
    char path[256];
    snprintf(path, sizeof(path), "ux0:data/VitaJPOverlay/anki_queue%s/%s.json", dir, id);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    if (platform)
        fprintf(f, "{\"version\":4,\"spelling\":\"猫\",\"game\":\"G\",\"title_id\":\"%s\",\"platform\":\"%s\",\"time\":1}",
                tid, platform);
    else
        fprintf(f, "{\"version\":4,\"spel");
    fclose(f);
}

static void cards_count_per_game(void)
{
    reset();
    three_games();
    TEST_ASSERT(mkdir("ux0:data/VitaJPOverlay/anki_queue", 0777) == 0 &&
                mkdir("ux0:data/VitaJPOverlay/anki_queue/exported", 0777) == 0);
    write_card("", "00000000000000000000000000000001", "vita", "PCSG00415");
    write_card("", "00000000000000000000000000000002", "vita", "PCSG00415");
    write_card("/exported", "00000000000000000000000000000002", "vita", "PCSG00415"); /* in both: once */
    write_card("/exported", "00000000000000000000000000000003", "vita", "PCSG00062");
    write_card("", "00000000000000000000000000000004", NULL, NULL); /* damaged: All games only */
    write_card("", "0000000000000000000000000000000z", "vita", "PCSG00062"); /* not a card's name */
    write_card("", "00000000000000000000000000000005", "vita", "PCSE99999"); /* a game with no line */
    opens_of_cards = 0;
    show();
    TEST_CHECK_(opens_of_cards == 5, "%d opens", opens_of_cards); /* each card once, also the one in both */
    TEST_CHECK_(strstr(logs, "5 cards, 5 read") != NULL, "%s", logs);
    for (int i = 0; i < 4; i++)
        frame(SCE_CTRL_DOWN); /* All games */
    TEST_CHECK(text_with("5 cards, ") != NULL);
    int found = 0;
    for (int i = 0; i < 6; i++) {
        frame(SCE_CTRL_DOWN);
        found += text_with("2 cards, ") != NULL && text_with("CLANNAD") != NULL;
        found += text_with("1 card, ") != NULL && text_with("TIME TRAVELERS") != NULL;
        found += text_with("vita-PCSE99999") != NULL && text_with("no lines read") != NULL && text_with("; 1 card") != NULL;
    }
    TEST_CHECK_(found >= 3, "%d", found);
    /* the next count reads again only the card whose record could not be read */
    opens_of_cards = 0;
    logs[0] = 0;
    show();
    TEST_CHECK_(opens_of_cards == 1 && strstr(logs, "5 cards, 1 read"), "%d: %s", opens_of_cards, logs);
    /* once it is whole, its game counts */
    write_card("", "00000000000000000000000000000004", "vita", "PCSG00062");
    logs[0] = 0;
    show();
    for (int i = 0; i < 6; i++) {
        frame(SCE_CTRL_DOWN);
        if (text_with("TIME TRAVELERS"))
            break;
    }
    TEST_CHECK(text_with("2 cards, ") != NULL && text_with("TIME TRAVELERS") != NULL);
}

/* Cards but no history.csv (Read text: Only when I press L + R keeps none): the cards count, and each record is read
 * once, not at every count. */
static void cards_without_history(void)
{
    reset();
    TEST_ASSERT(mkdir("ux0:data/VitaJPOverlay/anki_queue", 0777) == 0);
    write_card("", "00000000000000000000000000000001", "vita", "PCSG00415");
    write_card("", "00000000000000000000000000000002", "vita", "PCSG00415");
    opens_of_cards = 0;
    show();
    TEST_CHECK(opens_of_cards == 2 && strstr(drawn.footer, "Counted 0 lines of history.csv and 2 cards"));
    opens_of_cards = 0;
    show();
    TEST_CHECK_(opens_of_cards == 0, "%d", opens_of_cards);
    for (int i = 0; i < 5; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(text_with("vita-PCSG00415") != NULL && text_with("no lines read") != NULL && text_with("; 2 cards") != NULL);
}

static void no_history_yet(void)
{
    reset();
    /* the first frame, before the count is done: today's day, nothing counted yet */
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_stats_page_show(&scratch);
    frame(0);
    TEST_CHECK(text_with("Counting history.csv…") != NULL && strstr(drawn.footer, "Counting history.csv"));
    TEST_CHECK(text_with("Tue 27 Oct: 0 characters, 0 min") != NULL); /* today's bar, empty */
    run_worker();
    frame(0);
    show();
    TEST_CHECK(strstr(drawn.footer, "No reading yet") && !drawn.footer_error);
    TEST_CHECK(text_with("0 characters, 0 min") != NULL);
    /* history.csv that cannot be read: said, nothing else changed */
    three_games();
    show();
    fail_open = "history.csv";
    show();
    fail_open = NULL;
    TEST_CHECK(strstr(drawn.footer, "Cannot read history.csv (0x80010005)") && drawn.footer_error);
}

static void a_new_day_and_quit(void)
{
    reset();
    three_games();
    show();
    TEST_CHECK(text_with("read today") != NULL || 1);
    now_us = jst(28, 0, 30, 0); /* past midnight in Japan */
    frame(0);
    TEST_CHECK(text_with("Today") && text_with("0 characters, 0 min") != NULL);
    TEST_CHECK(text_with("Streak: 3 days with reading") != NULL); /* yesterday's run, today not read yet */
    /* quit while a count is pending: it stops at once, keeping what it has */
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_stats_page_show(&scratch);
    logs[0] = 0;
    vjo_stats_page_quit();
    TEST_CHECK(strstr(logs, "; stopped") != NULL);
}

TEST_LIST = {
    {"each_game_shows_the_triangle_views_numbers", each_game_shows_the_triangle_views_numbers},
    {"the_rows_scroll_inside_their_room", the_rows_scroll_inside_their_room},
    {"the_goal_is_saved_in_app_ini", the_goal_is_saved_in_app_ini},
    {"the_days_and_the_year", the_days_and_the_year},
    {"the_cache_is_read_on_and_checked", the_cache_is_read_on_and_checked},
    {"cards_count_per_game", cards_count_per_game},
    {"cards_without_history", cards_without_history},
    {"no_history_yet", no_history_yet},
    {"a_new_day_and_quit", a_new_day_and_quit},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("settings-stats");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
