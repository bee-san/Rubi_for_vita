/* The Rubi app's seed through its real window loop (settings/streak_seed.c count_history, 64 KiB windows
 * read with sceIoPread) on history.csv files of many windows as the Shell's writer writes them, LF and CRLF, a newest
 * record not yet whole, a record longer than a window and one of a window less a byte; and vjo_streak_merge's cases
 * where the Shell's record and history.csv's both end on one day, or their runs overlap (#501's audit, F1 and F2). The
 * expected streak is the days of the records the Shell would have counted, replayed through vjo_streak_add (no window
 * logic in it). */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../core/streak.h"
#include "../../core/streak_scan.h"
#include "../../settings/streak_seed.h"

#define DIR_ "ux0:data/VitaJPOverlay"
#define RECORD DIR_ "/reading-streak.bin"
#define HISTORY DIR_ "/history.csv"
#define D 20389 /* the Vita's today: 2025-10-28 */
#define DAY_US 86400000000ull
#define RTC_UNIX_EPOCH 62135596800000000ull
#define WINDOW (64 * 1024) /* settings/streak_seed.c's */

/* ---------------- the memory card (real files), the clock (UTC), the seed's thread run when the test says ---------- */
#include "fakes/sceio_posix.h"
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = RTC_UNIX_EPOCH + (uint64_t)D * DAY_US + 12 * 3600000000ull;
    return 0;
}
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local)
{
    local->tick = utc->tick; /* UTC */
    return 0;
}
static int (*pending)(SceSize, void *), (*entry)(SceSize, void *);
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { (void)n, (void)a, (void)c, (void)o; return 0x100; }
SceUID sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned int s, unsigned int a, int af, void *o)
{
    (void)n, (void)p, (void)s, (void)a, (void)af, (void)o;
    entry = e;
    return 0x201;
}
int sceKernelStartThread(SceUID u, SceSize s, void *a) { (void)u, (void)s, (void)a; pending = entry; return 0; }
static void run_seed(void)
{
    int (*e)(SceSize, void *) = pending;
    pending = NULL;
    if (e)
        e(0, NULL);
}
int sceKernelWaitThreadEnd(SceUID u, int *s, void *t) { (void)u, (void)s, (void)t; run_seed(); return 0; }
int sceKernelDeleteThread(SceUID u) { (void)u; return 0; }
int sceKernelDeleteMutex(SceUID u) { (void)u; return 0; }
static char logs[4096];
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs))
        logs[n] = '\n', logs[n + 1] = '\0';
}

/* ---------------- history.csv, written as the Shell's writer writes it ---------------- */

static char *csv;
static size_t csv_len, csv_cap;
static VjoStreak want; /* the days of the records the Shell counted, replayed */
static void put(const char *s, size_t n)
{
    if (csv_len + n > csv_cap) {
        csv_cap = (csv_len + n) * 2;
        csv = realloc(csv, csv_cap);
        TEST_ASSERT(csv != NULL);
    }
    memcpy(csv + csv_len, s, n);
    csv_len += n;
}
static void start_file(void)
{
    static const char header[] = "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene\n";
    csv_len = 0;
    put(header, sizeof(header) - 1);
    vjo_streak_init(&want);
}
/* A record on day `day` at hour h UTC, its line `line` (counted when it has a character and a valid time), its line
 * end eol ("\n" or "\r\n"), padded with numeric cells to about `pad` bytes as the 43 columns make it. */
static void record_eol(int32_t day, int h, const char *line, int valid_time, const char *eol, size_t pad)
{
    char text[1024];
    uint64_t us = (uint64_t)day * DAY_US + (uint64_t)h * 3600000000ull + 123456;
    int n = snprintf(text, sizeof(text), "\"vita-PCSG00415\",\"vjo-%llu-%zu\",\"CLANNAD\",\"%s\",", (unsigned long long)us,
                     csv_len, line);
    if (valid_time)
        n += snprintf(text + n, sizeof(text) - (size_t)n, "%llu.%06u", (unsigned long long)(us / 1000000),
                      (unsigned)(us % 1000000));
    n += snprintf(text + n, sizeof(text) - (size_t)n, ",\"vita\",\"PCSG00415\",\"vocr\",\"VOCR6-N\",\"%s\"", line);
    while ((size_t)n + 16 < pad && (size_t)n + 16 < sizeof(text))
        n += snprintf(text + n, sizeof(text) - (size_t)n, ",%d", 1000 + n % 7919);
    n += snprintf(text + n, sizeof(text) - (size_t)n, ",\"1:2:3\",4,\"ocr\"%s", eol);
    put(text, (size_t)n);
    if (valid_time && strcmp(line, "……") && line[0])
        vjo_streak_add(&want, vjo_streak_day(us, 0));
}
static void record(int32_t day, int h, const char *line) { record_eol(day, h, line, 1, "\n", 600); }
/* A record of exactly `bytes` bytes whose line is filler (a well-formed record, longer than any the writer makes) */
static void long_record(int32_t day, size_t bytes, int counted)
{
    char head[160];
    uint64_t us = (uint64_t)day * DAY_US + 9 * 3600000000ull;
    int n = snprintf(head, sizeof(head), "\"vita-PCSG00415\",\"vjo-%llu-%zu\",\"CLANNAD\",\"", (unsigned long long)us,
                     csv_len);
    char tail[96];
    int t = snprintf(tail, sizeof(tail), "\",%llu.%06u,\"vita\"\n", (unsigned long long)(us / 1000000),
                     (unsigned)(us % 1000000));
    TEST_ASSERT(bytes > (size_t)(n + t));
    put(head, (size_t)n);
    for (size_t i = 0; i < bytes - (size_t)(n + t); i++)
        put("x", 1);
    put(tail, (size_t)t);
    if (counted)
        vjo_streak_add(&want, vjo_streak_day(us, 0));
}
static void write_history(void)
{
    FILE *f = fopen(HISTORY, "wb");
    TEST_ASSERT(f && fwrite(csv, 1, csv_len, f) == csv_len);
    fclose(f);
}
static int read_record(VjoStreak *s)
{
    FILE *f = fopen(RECORD, "rb");
    size_t n = f ? fread(s, 1, sizeof(*s), f) : 0;
    if (f)
        fclose(f);
    return n == sizeof(*s);
}
static void reset(void)
{
    unlink(RECORD), unlink(RECORD ".new"), unlink(HISTORY);
    mkdir("ux0:data", 0777);
    mkdir(DIR_, 0777);
    pending = entry = NULL;
    logs[0] = '\0';
    vjo_streak_seed_quit();
}
/* The seed on history.csv as built: the record it writes */
static VjoStreak seeded(void)
{
    VjoStreak s;
    memset(&s, 0, sizeof(s));
    write_history();
    vjo_streak_seed_start();
    run_seed();
    vjo_streak_seed_quit();
    TEST_CHECK_(read_record(&s), "no record written: %s", logs);
    return s;
}
#define CHECK_SAME(s, w)                                                                                             \
    TEST_CHECK_((s).day == (w).day && (s).current == (w).current && (s).longest == (w).longest,                       \
                "seeded day %d current %u longest %u, the Shell counted day %d current %u longest %u", (int)(s).day,   \
                (unsigned)(s).current, (unsigned)(s).longest, (int)(w).day, (unsigned)(w).current,                    \
                (unsigned)(w).longest)

/* Many 64 KiB windows: ~150 days of three lines each (~270 KB), gaps, lines without a character and without a time */
static void build_many(const char *eol)
{
    start_file();
    for (int32_t d = D - 160; d <= D; d++) {
        if (d % 17 == 0 || d % 29 == 0)
            continue; /* a day without reading */
        record_eol(d, 1, "「おはよう」", 1, eol, 600);
        record_eol(d, 5, "……", 1, eol, 600);       /* no character: no day */
        record_eol(d, 7, "時間なし", 0, eol, 600);   /* an invalid time: no day */
        record_eol(d, 13, "二行目, \"\"quoted\"\"", 1, eol, 600);
    }
}

static void many_windows_lf(void)
{
    reset();
    build_many("\n");
    TEST_CHECK(csv_len > 4 * WINDOW);
    VjoStreak s = seeded();
    CHECK_SAME(s, want);
    TEST_MSG("history.csv %zu B", csv_len);
}

static void many_windows_crlf(void)
{
    reset();
    build_many("\r\n");
    VjoStreak s = seeded();
    CHECK_SAME(s, want);
}

/* The newest record not whole yet (no line break at the file's end) in the last window: its day waits */
static void newest_record_not_whole(void)
{
    reset();
    build_many("\n");
    record(D + 1, 9, "まだ書いている");
    csv_len--; /* its line break not written */
    VjoStreak w = want;
    /* (record() counted D + 1 into want: the seed must not) */
    TEST_CHECK(w.day == D + 1);
    VjoStreak s = seeded();
    TEST_CHECK_(s.day == D, "seeded day %d: a record without its line break was counted", (int)s.day);
}

/* A record longer than a window (70,000 B: none the writer makes) is skipped; the records after it count */
static void a_record_longer_than_a_window(void)
{
    reset();
    start_file();
    for (int32_t d = D - 9; d <= D - 5; d++)
        record(d, 3, "前の行");
    long_record(D - 4, 70000, 0); /* skipped by design: not in want */
    for (int32_t d = D - 4; d <= D; d++)
        record(d, 12, "後の行");
    VjoStreak s = seeded();
    CHECK_SAME(s, want);
}

/* A record of a window less one byte (65,535 B), cut by its window and so read from a window that starts with it:
 * the next record starts at that window's last byte. The writer makes no such record, but the one after it is the
 * writer's, and the Shell counted its day. */
static void a_record_of_a_window_less_a_byte(void)
{
    static const size_t lens[] = {WINDOW - 3, WINDOW - 2, WINDOW - 1, WINDOW, WINDOW + 1, 2 * WINDOW - 2,
                                  2 * WINDOW - 1, 2 * WINDOW, 3 * WINDOW - 1};
    for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        size_t len = lens[k];
        reset();
        start_file();
        for (int32_t d = D - 9; d <= D - 5; d++)
            record(d, 3, "前の行");
        long_record(D - 5, len, 1); /* (its day is D - 5's, already counted: it changes nothing either way) */
        record(D - 4, 12, "長い記録の次の行"); /* the only line of D - 4 */
        for (int32_t d = D - 3; d <= D; d++)
            record(d, 12, "後の行");
        VjoStreak s = seeded();
        TEST_CHECK_(s.day == want.day && s.current == want.current && s.longest == want.longest,
                    "a %zu B record: seeded current %u longest %u, the Shell counted current %u longest %u", len,
                    (unsigned)s.current, (unsigned)s.longest, (unsigned)want.current, (unsigned)want.longest);
    }
}

/* ---------------- vjo_streak_merge: history.csv's seed and the Shell's record (not seeded yet) ---------------- */

static VjoStreak run_of(int32_t first, int32_t last)
{
    VjoStreak s;
    vjo_streak_init(&s);
    for (int32_t d = first; d <= last; d++)
        vjo_streak_add(&s, d);
    return s;
}

/* history.csv lost its OLDER lines (moved away for GameSentenceMiner, a new file started) while the Shell's record
 * counted every day: both end today, the Shell's run is the longer, and both runs are days read */
static void merge_same_newest_day(void)
{
    VjoStreak seed = run_of(D - 2, D), live = run_of(D - 9, D);
    vjo_streak_merge(&seed, &live);
    TEST_CHECK_(seed.day == D && seed.current == 10 && seed.longest == 10,
                "merged day %d current %u longest %u: the Shell's 10 days in a row to today, want current 10",
                (int)seed.day, (unsigned)seed.current, (unsigned)seed.longest);
}

/* history.csv lost its newest lines (an older copy put back) and the Shell's run began before the seed's newest day:
 * the two runs overlap, D - 10 .. D in a row */
static void merge_overlapping_runs(void)
{
    VjoStreak seed = run_of(D - 10, D - 2), live = run_of(D - 4, D);
    vjo_streak_merge(&seed, &live);
    TEST_CHECK_(seed.day == D && seed.current == 11 && seed.longest == 11,
                "merged day %d current %u longest %u: D-10..D in a row, want current 11", (int)seed.day,
                (unsigned)seed.current, (unsigned)seed.longest);
}

TEST_LIST = {
    {"many_windows_lf", many_windows_lf},
    {"many_windows_crlf", many_windows_crlf},
    {"newest_record_not_whole", newest_record_not_whole},
    {"a_record_longer_than_a_window", a_record_longer_than_a_window},
    {"a_record_of_a_window_less_a_byte", a_record_of_a_window_less_a_byte},
    {"merge_same_newest_day", merge_same_newest_day},
    {"merge_overlapping_runs", merge_overlapping_runs},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("streak-seed-files");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
