/* The Rubi app's seed of the overlay's reading streak (settings/streak_seed.c) on a temporary memory card: the record
 * is seeded once from history.csv (its days in the Vita's time zone), then never again; what the Shell added before it
 * is kept; a seed the app's exit stops writes nothing; a history.csv that cannot be read is retried at the next start;
 * stats_cache.bin (0.9.8 test builds' Stats tab) is removed. The seed's thread runs when the test says. */
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
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../core/streak.h"
#include "../../settings/streak_seed.h"

#define DIR_ "ux0:data/VitaJPOverlay"
#define RECORD DIR_ "/reading-streak.bin"
#define HISTORY DIR_ "/history.csv"
#define STATS_CACHE DIR_ "/stats_cache.bin"
#define D 20389 /* the Vita's today: 2025-10-28 */
#define DAY_US 86400000000ull
#define RTC_UNIX_EPOCH 62135596800000000ull

/* ---------------- the memory card: real files (fakes/sceio_posix.h), an open that fails ---------------- */

#define SCEIO_OWN (SCEIO_OPEN)
#include "fakes/sceio_posix.h"
static const char *fail_open; /* an open of this path fails (EIO) */
static int history_opens;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (fail_open && !strcmp(p, fail_open))
        return (int)0x80010005u;
    if (!strcmp(p, HISTORY))
        history_opens++;
    return sceio_open(p, flags, mode);
}

/* ---------------- the clock: today noon in a time zone the test sets ---------------- */

static int tz_minutes = 9 * 60; /* Tokyo */
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = RTC_UNIX_EPOCH + (uint64_t)D * DAY_US + 3 * 3600000000ull; /* 12:00 in Tokyo */
    return 0;
}
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local)
{
    local->tick = utc->tick + (uint64_t)((int64_t)tz_minutes * 60000000ll);
    return 0;
}

/* ---------------- the seed's thread, run when the test says ---------------- */

static int (*pending)(SceSize, void *), (*entry)(SceSize, void *);
static int created, deleted, stack, affinity, fail_create;
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { (void)n, (void)a, (void)c, (void)o; return 0x100; }
SceUID sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned int s, unsigned int a, int af, void *o)
{
    (void)n, (void)p, (void)a, (void)o;
    if (fail_create)
        return (int)0x80020001u;
    entry = e;
    stack = (int)s;
    affinity = af;
    return 0x200 + ++created;
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
int sceKernelDeleteThread(SceUID u) { (void)u; deleted++; return 0; }
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

/* ---------------- the card's files ---------------- */

static void reset(void)
{
    unlink(RECORD), unlink(RECORD ".new"), unlink(HISTORY), unlink(STATS_CACHE);
    mkdir("ux0:data", 0777);
    mkdir(DIR_, 0777);
    pending = entry = NULL;
    created = deleted = fail_create = history_opens = 0;
    fail_open = NULL;
    tz_minutes = 9 * 60;
    logs[0] = '\0';
    vjo_streak_seed_quit(); /* (the app's state from the test before: none running) */
    created = deleted = 0;
}
/* history.csv with a line at noon in Tokyo on each day given (0 ends), as the Shell's writer writes them */
static void history(const int32_t *days)
{
    FILE *f = fopen(HISTORY, "wb");
    TEST_ASSERT(f != NULL);
    fputs("uuid,given_identifier,name,line,time,platform,title_id,engine\n", f);
    for (int i = 0; days[i]; i++) {
        uint64_t us = (uint64_t)days[i] * DAY_US + 3 * 3600000000ull;
        fprintf(f, "\"vita-PCSG00415\",\"vjo-%llu-%d\",\"CLANNAD\",\"「%d日目」\",%llu.%06u,\"vita\",\"PCSG00415\",\"vocr\"\n",
                (unsigned long long)us, i, i, (unsigned long long)(us / 1000000), (unsigned)(us % 1000000));
    }
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
static void write_record(const VjoStreak *s)
{
    FILE *f = fopen(RECORD, "wb");
    TEST_ASSERT(f && fwrite(s, sizeof(*s), 1, f) == 1);
    fclose(f);
}
static int there(const char *p) { return access(p, F_OK) == 0; }

static void the_record_is_seeded_once(void)
{
    VjoStreak s;
    reset();
    history((const int32_t[]){D - 5, D - 4, D - 2, D - 1, 0});
    FILE *f = fopen(STATS_CACHE, "wb"); /* the Stats tab's cache, read by nothing now */
    TEST_ASSERT(f && fputs("RubiStat", f) >= 0);
    fclose(f);
    vjo_streak_seed_start();
    /* on its own thread (16 KiB, CPU3), not the main thread's: nothing written before it runs */
    TEST_CHECK(created == 1 && stack == 16 * 1024 && affinity == SCE_KERNEL_CPU_MASK_SYSTEM && !there(RECORD));
    run_seed();
    TEST_ASSERT(read_record(&s));
    TEST_CHECK_(vjo_streak_valid(&s) && s.seeded == VJO_STREAK_SEEDED && s.day == D - 1 && s.current == 2 &&
                    s.longest == 2,
                "day %d current %u longest %u seeded %u", (int)s.day, (unsigned)s.current, (unsigned)s.longest,
                (unsigned)s.seeded);
    TEST_CHECK(vjo_streak_current(&s, D) == 2); /* the Triangle view today: yesterday's streak, still going */
    TEST_CHECK(!there(STATS_CACHE) && !there(RECORD ".new"));
    TEST_CHECK_(strstr(logs, "streak: seeded from ") && strstr(logs, "(UTC+540 min): current 2, longest 2") &&
                    strstr(logs, "stats_cache.bin removed"),
                "%s", logs);
    /* the next start: seeded, so no thread and no read of history.csv */
    vjo_streak_seed_quit();
    history_opens = 0;
    vjo_streak_seed_start();
    run_seed();
    TEST_CHECK(created == 1 && history_opens == 0);
    vjo_streak_seed_quit(); /* (no thread: nothing to join) */
    TEST_CHECK(deleted == 1);
}

static void what_the_shell_added_is_kept(void)
{
    VjoStreak live, s;
    reset();
    /* the Shell counted D and D + 1 (its record, not seeded) and history.csv lost them (moved away); before that it
     * has D - 3 .. D - 1 */
    history((const int32_t[]){D - 3, D - 2, D - 1, 0});
    vjo_streak_init(&live);
    vjo_streak_add(&live, D);
    vjo_streak_add(&live, D + 1);
    write_record(&live);
    vjo_streak_seed_start();
    run_seed();
    TEST_ASSERT(read_record(&s));
    TEST_CHECK_(s.seeded == VJO_STREAK_SEEDED && s.day == D + 1 && s.current == 5 && s.longest == 5,
                "day %d current %u longest %u", (int)s.day, (unsigned)s.current, (unsigned)s.longest);
    /* the usual case: history.csv has the Shell's lines too: the seed stands, the same days */
    reset();
    history((const int32_t[]){D - 3, D - 2, D - 1, D, 0});
    vjo_streak_init(&live);
    vjo_streak_add(&live, D);
    write_record(&live);
    vjo_streak_seed_start();
    run_seed();
    TEST_ASSERT(read_record(&s));
    TEST_CHECK(s.seeded == VJO_STREAK_SEEDED && s.day == D && s.current == 4 && s.longest == 4);
    /* a damaged record is replaced by the seed */
    reset();
    history((const int32_t[]){D, 0});
    write_record(&(VjoStreak){0x12345678u, D + 9, 7, 7, 0});
    vjo_streak_seed_start();
    run_seed();
    TEST_ASSERT(read_record(&s));
    TEST_CHECK(s.seeded == VJO_STREAK_SEEDED && s.day == D && s.current == 1 && s.longest == 1);
}

static void a_seed_with_no_history_marks_the_record(void)
{
    VjoStreak s;
    reset();
    vjo_streak_seed_start(); /* no history.csv (nothing read yet, or Only when I press L + R) */
    run_seed();
    TEST_ASSERT(read_record(&s));
    TEST_CHECK(vjo_streak_valid(&s) && s.seeded == VJO_STREAK_SEEDED && s.day == VJO_STREAK_NONE && !s.longest);
    TEST_CHECK(vjo_streak_current(&s, D) == 0);
}

static void a_stopped_or_failed_seed_writes_nothing(void)
{
    VjoStreak s;
    /* the app closed before the seed ran: it stops at its first window, writes nothing, and its thread is joined */
    reset();
    history((const int32_t[]){D - 1, D, 0});
    vjo_streak_seed_start();
    vjo_streak_seed_quit();
    TEST_CHECK(created == 1 && deleted == 1 && !there(RECORD) && !pending);
    /* history.csv cannot be read: nothing written, said in log.txt; the next start seeds */
    reset();
    history((const int32_t[]){D - 1, D, 0});
    fail_open = HISTORY;
    vjo_streak_seed_start();
    run_seed();
    TEST_CHECK(!there(RECORD) && strstr(logs, "streak: history.csv could not be read (0x80010005)"));
    fail_open = NULL;
    vjo_streak_seed_quit();
    vjo_streak_seed_start();
    run_seed();
    TEST_CHECK(read_record(&s) && s.seeded == VJO_STREAK_SEEDED && s.day == D && s.current == 2);
    /* the record cannot be written: said, and the next start seeds again */
    reset();
    history((const int32_t[]){D, 0});
    fail_open = RECORD ".new";
    vjo_streak_seed_start();
    run_seed();
    TEST_CHECK(!there(RECORD) && strstr(logs, "could not be written (0x80010005): seeded at the next start"));
    /* the thread cannot start: said; nothing written */
    reset();
    fail_create = 1;
    vjo_streak_seed_start();
    TEST_CHECK(!pending && !there(RECORD) && strstr(logs, "streak: the seed could not start (0x80020001)"));
}

static void the_days_are_the_vita_s_time_zone_s(void)
{
    VjoStreak s;
    /* the lines at 03:00 UTC: in Tokyo each its own day; 5 hours west of UTC each the day before, still in a row */
    reset();
    history((const int32_t[]){D - 2, D - 1, D, 0});
    tz_minutes = -5 * 60;
    vjo_streak_seed_start();
    run_seed();
    TEST_ASSERT(read_record(&s));
    TEST_CHECK_(s.day == D - 1 && s.current == 3 && s.longest == 3, "day %d current %u", (int)s.day, (unsigned)s.current);
    TEST_CHECK(strstr(logs, "(UTC-300 min)") != NULL);
}

TEST_LIST = {
    {"the_record_is_seeded_once", the_record_is_seeded_once},
    {"what_the_shell_added_is_kept", what_the_shell_added_is_kept},
    {"a_seed_with_no_history_marks_the_record", a_seed_with_no_history_marks_the_record},
    {"a_stopped_or_failed_seed_writes_nothing", a_stopped_or_failed_seed_writes_nothing},
    {"the_days_are_the_vita_s_time_zone_s", the_days_are_the_vita_s_time_zone_s},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    /* the memory card, removed when the suite ends (acutest runs each test in a child: test_tmpdir.h) */
    const char *dir = vjo_test_tmpdir("settings-streak");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
