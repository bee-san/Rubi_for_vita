/* The reading streak (core/streak.h: the overlay's Triangle line) and its seed (core/streak_scan.h: the Rubi app's,
 * from history.csv): the rule on a day rollover, gaps, a time zone moved either way, a clock set back by hours or by
 * days, the first use, the current streak as the view shows it on a later day, the record's checks; the seed's scan
 * of records as the Shell's writer writes them, in any windows the app reads, and its merge with the Shell's record. */
#include "acutest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/streak.h"
#include "../../core/streak_scan.h"

#define D 20389 /* 2025-10-28, a day of the Vita's clock */
#define DAY_US 86400000000ull

static VjoStreak fresh(void)
{
    VjoStreak s;
    vjo_streak_init(&s);
    return s;
}
/* The record after a line on each of n days, in that order. */
static VjoStreak after(const int32_t *days, int n)
{
    VjoStreak s = fresh();
    for (int i = 0; i < n; i++)
        vjo_streak_add(&s, days[i]);
    return s;
}
#define AFTER(...) after((const int32_t[]){__VA_ARGS__}, (int)(sizeof((int32_t[]){__VA_ARGS__}) / sizeof(int32_t)))
#define CHECK_STREAK(s, d, cur, best)                                                                              \
    TEST_CHECK_((s).day == (d) && (s).current == (cur) && (s).longest == (best) && vjo_streak_valid(&(s)),          \
                "day %d current %u longest %u, want %d %u %u", (int)(s).day, (unsigned)(s).current,                 \
                (unsigned)(s).longest, (int)(d), (unsigned)(cur), (unsigned)(best))

static void first_use(void)
{
    VjoStreak s = fresh();
    TEST_CHECK(s.magic == VJO_STREAK_MAGIC && s.day == VJO_STREAK_NONE && !s.current && !s.longest && !s.seeded);
    TEST_CHECK(vjo_streak_valid(&s) && sizeof(s) == 16);
    TEST_CHECK(vjo_streak_current(&s, D) == 0); /* nothing read yet: current 0, longest 0 */
    TEST_CHECK(vjo_streak_add(&s, VJO_STREAK_NONE) == 0 && s.day == VJO_STREAK_NONE); /* no clock: nothing */
    TEST_CHECK(vjo_streak_add(&s, D) == 1);
    CHECK_STREAK(s, D, 1, 1);
    TEST_CHECK(vjo_streak_current(&s, D) == 1);
}

static void a_day_rolls_over(void)
{
    VjoStreak s = AFTER(D);
    TEST_CHECK(vjo_streak_add(&s, D) == 0); /* the same day again: counted, nothing to write */
    CHECK_STREAK(s, D, 1, 1);
    TEST_CHECK(vjo_streak_add(&s, D + 1) == 1);
    CHECK_STREAK(s, D + 1, 2, 2);
    TEST_CHECK(vjo_streak_add(&s, D + 1) == 0);
    TEST_CHECK(vjo_streak_add(&s, D + 2) == 1);
    CHECK_STREAK(s, D + 2, 3, 3);
    /* as the view shows it: today, tomorrow (today has no line yet), and the day after that (it ended) */
    TEST_CHECK(vjo_streak_current(&s, D + 2) == 3 && vjo_streak_current(&s, D + 3) == 3);
    TEST_CHECK(vjo_streak_current(&s, D + 4) == 0 && vjo_streak_current(&s, D + 400) == 0);
    TEST_CHECK(s.longest == 3); /* the longest stays shown when the current one ended */
}

static void a_gap_starts_again_and_keeps_the_longest(void)
{
    VjoStreak s = AFTER(D, D + 1, D + 2, D + 4);
    CHECK_STREAK(s, D + 4, 1, 3);
    vjo_streak_add(&s, D + 5);
    vjo_streak_add(&s, D + 6);
    CHECK_STREAK(s, D + 6, 3, 3);
    vjo_streak_add(&s, D + 7);
    CHECK_STREAK(s, D + 7, 4, 4); /* past the old longest */
    s = AFTER(D, D + 1000);
    CHECK_STREAK(s, D + 1000, 1, 1);
    TEST_CHECK(vjo_streak_current(&s, D + 1000) == 1);
}

/* The day of a time in a zone, as the Shell's clock and the app's seed have it (core/streak.h vjo_streak_day). */
static void days_of_times_in_zones(void)
{
    uint64_t midnight = (uint64_t)D * DAY_US; /* 00:00 UTC of day D */
    TEST_CHECK(vjo_streak_day(midnight, 0) == D && vjo_streak_day(midnight - 1, 0) == D - 1);
    TEST_CHECK(vjo_streak_day(midnight + DAY_US - 1, 0) == D);
    /* 23:30 UTC is the next day in Tokyo (+9 h) and the same day in New York (-4 h), 00:30 UTC the day before there */
    TEST_CHECK(vjo_streak_day(midnight + 23 * 3600000000ull + 1800000000ull, 9 * 60) == D + 1);
    TEST_CHECK(vjo_streak_day(midnight + 23 * 3600000000ull + 1800000000ull, -4 * 60) == D);
    TEST_CHECK(vjo_streak_day(midnight + 1800000000ull, -4 * 60) == D - 1);
    /* half-hour zones (India +5:30, Newfoundland -3:30) and the far ones (+14, -12) */
    TEST_CHECK(vjo_streak_day(midnight + 18 * 3600000000ull + 1800000000ull, 5 * 60 + 30) == D + 1);
    TEST_CHECK(vjo_streak_day(midnight + 18 * 3600000000ull + 1800000000ull - 1, 5 * 60 + 30) == D);
    TEST_CHECK(vjo_streak_day(midnight + 3 * 3600000000ull + 1800000000ull, -(3 * 60 + 30)) == D);
    TEST_CHECK(vjo_streak_day(midnight + 10 * 3600000000ull, 14 * 60) == D + 1);
    TEST_CHECK(vjo_streak_day(midnight + 11 * 3600000000ull, -12 * 60) == D - 1);
    /* an invalid time (0, as history.csv's reader gives one), and one far past any day */
    TEST_CHECK(vjo_streak_day(0, 0) == VJO_STREAK_NONE && vjo_streak_day(UINT64_MAX, 0) == VJO_STREAK_NONE);
    TEST_CHECK(vjo_streak_day(1, -60) == -1); /* before 1970 in that zone: day -1 (floor) */
}

static void a_time_zone_moved(void)
{
    /* west across midnight: the newest day counted was tomorrow of the new clock's today; a line today changes
     * nothing, the view shows the streak still going, and the next days go on from it */
    VjoStreak s = AFTER(D - 2, D - 1, D);
    TEST_CHECK(vjo_streak_add(&s, D - 1) == 0);
    CHECK_STREAK(s, D, 3, 3);
    TEST_CHECK(vjo_streak_current(&s, D - 1) == 3);
    TEST_CHECK(vjo_streak_add(&s, D) == 0 && vjo_streak_add(&s, D + 1) == 1);
    CHECK_STREAK(s, D + 1, 4, 4);
    /* east across midnight: from 23:00 in London to 08:00 the next day in Tokyo, the next day: one more */
    uint64_t london_23h = (uint64_t)D * DAY_US + 23 * 3600000000ull;
    s = fresh();
    vjo_streak_add(&s, vjo_streak_day(london_23h, 0));
    vjo_streak_add(&s, vjo_streak_day(london_23h + 3600000000ull, 9 * 60));
    CHECK_STREAK(s, D + 1, 2, 2);
    /* and back west within the same hour: London's day again, the day before the newest: nothing */
    TEST_CHECK(vjo_streak_add(&s, vjo_streak_day(london_23h + 2 * 3600000000ull, 0)) == 0);
    CHECK_STREAK(s, D + 1, 2, 2);
}

static void a_clock_moved_backwards(void)
{
    /* by hours across midnight (the day before the newest): counted already */
    VjoStreak s = AFTER(D, D + 1);
    TEST_CHECK(vjo_streak_add(&s, D) == 0);
    CHECK_STREAK(s, D + 1, 2, 2);
    /* a clock that was set days ahead and is put right: the view shows the streak ended (its newest day is not within
     * a day of today), the next line starts it again from the real today, and the longest is kept */
    s = AFTER(D + 30, D + 31, D + 32, D + 33);
    TEST_CHECK(vjo_streak_current(&s, D) == 0 && s.longest == 4);
    TEST_CHECK(vjo_streak_add(&s, D) == 1);
    CHECK_STREAK(s, D, 1, 4);
    TEST_CHECK(vjo_streak_current(&s, D) == 1);
    vjo_streak_add(&s, D + 1);
    CHECK_STREAK(s, D + 1, 2, 4);
    /* by exactly two days: not the day before, so a new streak from it */
    s = AFTER(D, D + 1, D + 2);
    TEST_CHECK(vjo_streak_add(&s, D) == 1);
    CHECK_STREAK(s, D, 1, 3);
    /* a clock reset to the past (a flat battery: 2000-01-01) and set right again the same day: the streak starts
     * again from the reset day, then again from the real day (a day far after the reset day) */
    s = AFTER(D, D + 1);
    vjo_streak_add(&s, 10957);
    CHECK_STREAK(s, 10957, 1, 2);
    vjo_streak_add(&s, D + 2);
    CHECK_STREAK(s, D + 2, 1, 2);
}

static void the_current_streak_as_the_view_shows_it(void)
{
    VjoStreak s = AFTER(D, D + 1, D + 2, D + 3, D + 4);
    TEST_CHECK(vjo_streak_current(&s, D + 4) == 5);  /* today */
    TEST_CHECK(vjo_streak_current(&s, D + 5) == 5);  /* yesterday's: today has no line yet */
    TEST_CHECK(vjo_streak_current(&s, D + 3) == 5);  /* tomorrow's: a time zone moved west */
    TEST_CHECK(vjo_streak_current(&s, D + 6) == 0);  /* ended */
    TEST_CHECK(vjo_streak_current(&s, D + 2) == 0);  /* the clock set back by days */
    TEST_CHECK(vjo_streak_current(&s, VJO_STREAK_NONE) == 0); /* no clock */
    VjoStreak none = fresh();
    TEST_CHECK(vjo_streak_current(&none, VJO_STREAK_NONE) == 0 && vjo_streak_current(&none, D) == 0);
}

static void the_record_s_checks(void)
{
    VjoStreak s = AFTER(D, D + 1), bad;
    TEST_CHECK(vjo_streak_valid(&s));
    bad = s, bad.magic ^= 1;
    TEST_CHECK(!vjo_streak_valid(&bad));
    bad = s, bad.current = 0;
    TEST_CHECK(!vjo_streak_valid(&bad));
    bad = s, bad.current = 3; /* more than the longest */
    TEST_CHECK(!vjo_streak_valid(&bad));
    bad = fresh(), bad.longest = 1; /* no day, but a streak */
    TEST_CHECK(!vjo_streak_valid(&bad));
    bad = s, bad.seeded = VJO_STREAK_SEEDED; /* the seed's mark changes nothing else */
    TEST_CHECK(vjo_streak_valid(&bad));
    /* the file's bytes: the magic "RSR1", then the day, current, longest and the seed's mark, little-endian */
    unsigned char bytes[16];
    memcpy(bytes, &s, sizeof(bytes));
    TEST_CHECK(!memcmp(bytes, "RSR1", 4) && bytes[8] == 2 && bytes[9] == 0 && bytes[10] == 2 && bytes[11] == 0);
    /* 65,535 days in a row at the most (179 years): it stays there */
    s.day = D, s.current = s.longest = 0xFFFF;
    vjo_streak_add(&s, D + 1);
    CHECK_STREAK(s, D + 1, 0xFFFF, 0xFFFF);
}

/* ---------------- the seed: history.csv's records, as the Shell's writer writes them ---------------- */

static char csv[1 << 16];
static size_t csv_len;
static const char HEADER[] = "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene\n";
/* A record as shell/history.c writes it: the four quoted cells (a '"' doubled), the time with six digits, and some
 * of the cells after it. line NULL: a record a power loss cut in its text, right after a line break in it (the next
 * boot's record starts on the next line); us 0: an invalid time. */
static void record(const char *game, const char *line, uint64_t us)
{
    char text[2048], *o = text;
    o += sprintf(o, "\"vita-%s\",\"vjo-%llu-%zu\",\"%s\",\"", game, (unsigned long long)us, csv_len, game);
    for (const char *p = line ? line : "切れた行\n"; *p; p++) {
        if (*p == '"')
            *o++ = '"';
        *o++ = *p;
    }
    if (!line) {
        /* (nothing after the line break: the record ends there) */
    } else if (us) {
        o += sprintf(o, "\",%llu.%06u,\"vita\",\"%s\",\"vocr\",\"VOCR6-N\",\"%s\",3,\"1:2:3\",4,ocr\n",
                     (unsigned long long)(us / 1000000), (unsigned)(us % 1000000), game, line);
    } else {
        o += sprintf(o, "\",,\"vita\",\"%s\",\"vocr\",\"VOCR6-N\",\"\",3\n", game);
    }
    TEST_ASSERT(csv_len + (size_t)(o - text) < sizeof(csv));
    memcpy(csv + csv_len, text, (size_t)(o - text));
    csv_len += (size_t)(o - text);
}
static void start_file(void)
{
    memcpy(csv, HEADER, sizeof(HEADER) - 1);
    csv_len = sizeof(HEADER) - 1;
}
/* The local noon of day d in a zone tz_minutes east of UTC, as Unix microseconds. */
static uint64_t noon(int32_t d, int32_t tz) { return (uint64_t)d * DAY_US + 12 * 3600000000ull - (uint64_t)((int64_t)tz * 60000000ll); }

/* The app's reading loop (settings/streak_seed.c count_history) over csv with windows of `window` bytes. */
static VjoStreak seed(size_t window, int32_t tz)
{
    static char buf[(1 << 16) + 1];
    VjoStreak s = fresh();
    size_t pos = 0;
    while (pos < csv_len) {
        size_t base = pos ? pos - 1 : 0, to = csv_len - pos > window ? pos + window : csv_len, n = to - base, next;
        memcpy(buf, csv + base, n);
        next = base + vjo_streak_scan(&s, buf, n, !base, to == csv_len && buf[n - 1] == '\n', tz);
        if (next <= pos) {
            if (to == csv_len)
                break;
            next = to;
        }
        pos = next;
    }
    return s;
}

static void the_seed_replays_history_csv(void)
{
    const int32_t tz = 9 * 60; /* Tokyo */
    start_file();
    record("PCSG00415", "「おはよう」", noon(D - 10, tz));
    record("PCSG00415", "二日目, \"quoted\"\nand a line break", noon(D - 9, tz));
    record("PCSG00062", "三日目の行", noon(D - 8, tz)); /* another game: all games count */
    record("PCSG00062", "…！？", noon(D - 6, tz));      /* no character (punctuation only): no day */
    record("PCSG00062", "", noon(D - 6, tz) + 1000000);   /* an empty line: no day */
    record("PCSG00062", "時間なし", 0);                   /* an invalid time: no day */
    record("PCSG00415", NULL, noon(D - 6, tz));          /* a record a power loss cut (damaged) */
    record("PCSG00415", "五日目", noon(D - 3, tz));
    record("PCSG00415", "六日目", noon(D - 2, tz));
    record("PCSG00415", "ABC123", noon(D - 1, tz) - 3600000000ull * 11 - 1); /* 00:59:59 local: still that day */
    record("PCSG00415", "七日目", noon(D, tz));
    VjoStreak whole = seed(sizeof(csv), tz);
    CHECK_STREAK(whole, D, 4, 4); /* days D-10..D-8 (3), then D-3..D (4) */
    /* the same in any window the app could read, from one longer than the longest record up */
    for (size_t w = 200; w < csv_len + 2; w += 1 + w / 16) {
        VjoStreak s = seed(w, tz);
        TEST_CHECK_(!memcmp(&s, &whole, sizeof(s)), "window %zu: day %d current %u longest %u", w, (int)s.day,
                    (unsigned)s.current, (unsigned)s.longest);
    }
    /* the same records in UTC: each noon in Tokyo is 03:00 UTC, the same day, but the 00:59:59 line (15:59:59 UTC)
     * is the day before (D - 2), so D - 1 has no line: D - 3 and D - 2, then D alone */
    VjoStreak utc = seed(sizeof(csv), 0);
    CHECK_STREAK(utc, D, 1, 3);
    /* 12 hours west of UTC every line is a day earlier, and the 00:59:59 one two: D-11..D-9, then D-4..D-1 */
    VjoStreak west = seed(sizeof(csv), -12 * 60);
    CHECK_STREAK(west, D - 1, 4, 4);
}

static void the_seed_waits_for_a_record_being_written(void)
{
    start_file();
    record("PCSG00415", "一日目", noon(D, 0));
    record("PCSG00415", "二日目", noon(D + 1, 0));
    csv_len--; /* the newest record's line break not written yet: it waits (the Shell adds its day) */
    VjoStreak s = seed(sizeof(csv), 0);
    CHECK_STREAK(s, D, 1, 1);
    csv[csv_len++] = '\n';
    s = seed(sizeof(csv), 0);
    CHECK_STREAK(s, D + 1, 2, 2);
    /* a file with only its header, and an empty one: nothing */
    start_file();
    s = seed(sizeof(csv), 0);
    TEST_CHECK(s.day == VJO_STREAK_NONE && !s.longest);
    csv_len = 0;
    s = seed(sizeof(csv), 0);
    TEST_CHECK(s.day == VJO_STREAK_NONE && !s.longest);
}

static void the_seed_keeps_what_the_shell_added(void)
{
    VjoStreak seed_rec = AFTER(D - 3, D - 2, D - 1), live, m;
    /* the usual case: history.csv has every line the Shell counted: the seed stands */
    live = AFTER(D - 1);
    m = seed_rec;
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D - 1, 3, 3);
    /* history.csv lost its newest lines (moved away): the Shell's newer day stands, joined when it began the day after
     * the seed's newest */
    live = AFTER(D, D + 1);
    m = seed_rec;
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D + 1, 5, 5);
    /* not joined: a gap between them */
    live = AFTER(D + 1, D + 2);
    m = seed_rec;
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D + 2, 2, 3);
    /* the Shell's longest is kept too */
    live = AFTER(D - 40, D - 39, D - 38, D - 37, D - 36, D + 5);
    m = seed_rec;
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D + 5, 1, 5);
    /* no history.csv: the Shell's record; no record, or a damaged one: the seed */
    m = fresh();
    live = AFTER(D, D + 1);
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D + 1, 2, 2);
    m = seed_rec;
    vjo_streak_merge(&m, NULL);
    CHECK_STREAK(m, D - 1, 3, 3);
    live.magic = 0;
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D - 1, 3, 3);
    live = fresh();
    vjo_streak_merge(&m, &live);
    CHECK_STREAK(m, D - 1, 3, 3);
}

TEST_LIST = {
    {"first_use", first_use},
    {"a_day_rolls_over", a_day_rolls_over},
    {"a_gap_starts_again_and_keeps_the_longest", a_gap_starts_again_and_keeps_the_longest},
    {"days_of_times_in_zones", days_of_times_in_zones},
    {"a_time_zone_moved", a_time_zone_moved},
    {"a_clock_moved_backwards", a_clock_moved_backwards},
    {"the_current_streak_as_the_view_shows_it", the_current_streak_as_the_view_shows_it},
    {"the_record_s_checks", the_record_s_checks},
    {"the_seed_replays_history_csv", the_seed_replays_history_csv},
    {"the_seed_waits_for_a_record_being_written", the_seed_waits_for_a_record_being_written},
    {"the_seed_keeps_what_the_shell_added", the_seed_keeps_what_the_shell_added},
    {NULL, NULL},
};
