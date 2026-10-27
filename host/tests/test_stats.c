/* The Rubi app's Stats tab, its counts (core/stats.c, #312) on the host:
 * - each game's numbers equal the Shell's count of the same history.csv with that game as its scope
 *   (core/history_lines.c vjo_history_count_scope, which the Triangle view shows: this game's total, and today
 *   in this game), for both reading_time rules, on random files with every kind of record the writer and a
 *   power loss make: quotes, line breaks in the text, other games between, gaps from 0.1 s to days, invalid
 *   times, clocks set back, records cut short and run into the next append;
 * - the same counts whatever the windows the file is read in, and from a cache written at any of them;
 * - a record whose end is not read yet (no line break at the file's end) waits, and counts once complete;
 * - days are the local calendar's in the counts' time zone, a line read across midnight is its first day's;
 * - the simulated timelines of tools/reading_time_sim.py --fixtures give their known counts;
 * - the cache (stats_cache.bin) round trip and every refusal; the goal, the streak and the calendar. */
#include "acutest.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/history_lines.h"
#include "../../core/stats.h"

static VjoStats s, s2;

/* ---------------- history.csv as the Shell's writer makes it (shell/history.c) ---------------- */

typedef struct {
    char *buf;
    size_t len, cap;
} Text;

static void put(Text *t, const char *s, size_t n)
{
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->buf = realloc(t->buf, t->cap);
        TEST_ASSERT(t->buf != NULL);
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = 0;
}
static void puts_(Text *t, const char *s) { put(t, s, strlen(s)); }
static void putf(Text *t, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    put(t, tmp, (size_t)n);
}
static void field(Text *t, const char *s)
{
    puts_(t, "\"");
    for (; *s; s++) {
        if (*s == '"')
            puts_(t, "\"");
        put(t, s, 1);
    }
    puts_(t, "\"");
}

static const char HEADER[] =
    "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,"
    "scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,"
    "fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,"
    "recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,"
    "capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,characters,line_source\n";

/* One record of the writer's (its first five columns as it writes them, the rest of its columns shortened);
 * time_style: 0 the writer's, 1 five fractional digits (an invalid time), 2 no fraction at all. */
static void record(Text *t, const char *platform, const char *tid, const char *name, const char *text, uint64_t us,
                   int time_style)
{
    char uuid[40];
    snprintf(uuid, sizeof(uuid), "%s-%s", platform, tid);
    if (!t->len)
        puts_(t, HEADER);
    putf(t, "\"%s\",\"vjo-%llu-%llu\",", uuid, (unsigned long long)us, (unsigned long long)t->len);
    field(t, name);
    puts_(t, ",");
    field(t, text);
    if (time_style == 1)
        putf(t, ",%llu.%05u,", (unsigned long long)(us / 1000000), (unsigned)(us % 1000000 / 10));
    else if (time_style == 2)
        putf(t, ",%llu,", (unsigned long long)(us / 1000000));
    else
        putf(t, ",%llu.%06u,", (unsigned long long)(us / 1000000), (unsigned)(us % 1000000));
    field(t, platform);
    puts_(t, ",");
    field(t, tid);
    puts_(t, ",\"vocr\",\"VOCR5_N_w8g2.vocr\",");
    field(t, text);
    puts_(t, ",1,2,3,4,5,0,0,960,544,960,544,960,0,1,1,9,1,2,3,4,1,0,0,0,0,0,0,0,0,0,\"1:2:3:4:5\",");
    putf(t, "%u,\"ocr\"\n", vjo_line_characters(text, strlen(text)));
}

/* ---------------- the Shell's count, the reference ---------------- */

typedef struct {
    uint64_t chars, active, timed;           /* the game's total */
    uint64_t day_chars, day_active, day_timed; /* its scope's day */
} Shell;

/* The Shell's count of game uuid's records in the whole file (one window, as a count from the start), with
 * its day [start, end) as the scope's (the Triangle view's "today"). */
static Shell shell_count(const Text *t, const char *uuid, uint64_t start, uint64_t end, int cut)
{
    char *copy = malloc(t->len + 1);
    size_t first, last;
    VjoReadingTime time;
    VjoReadingScope scope;
    Shell r;
    memcpy(copy, t->buf, t->len + 1);
    memset(&time, 0, sizeof(time));
    memset(&scope, 0, sizeof(scope));
    snprintf(scope.uuid, sizeof(scope.uuid), "%s", uuid);
    scope.start_us = start;
    scope.end_us = end;
    r.chars = vjo_history_count_scope(copy, t->len, 1, cut, &first, &last, &time, &scope);
    r.active = time.active_us;
    r.timed = time.timed_chars;
    r.day_chars = scope.chars;
    r.day_active = scope.active_us;
    r.day_timed = scope.timed_chars;
    free(copy);
    return r;
}

/* ---------------- scanning ---------------- */

/* The whole file as the page reads it: windows of `window` bytes (each with the byte before it), from the
 * cache's `covered`. */
static void scan_windows(VjoStats *st, const Text *t, size_t window)
{
    char *buf = malloc(window + 2);
    uint64_t pos = st->covered;
    int at_end_nl = t->len && t->buf[t->len - 1] == '\n';
    while (pos < t->len) {
        uint64_t base = pos ? pos - 1 : 0, to = pos + window < t->len ? pos + window : t->len;
        memcpy(buf, t->buf + base, to - base);
        int end = to < t->len ? VJO_STATS_SCAN_MORE : at_end_nl ? VJO_STATS_SCAN_END : VJO_STATS_SCAN_CUT;
        uint64_t next = vjo_stats_scan(st, buf, to - base, base, end);
        TEST_ASSERT(next >= pos);
        if (next == pos)
            break; /* (the cut newest record waits) */
        pos = next;
    }
    free(buf);
}

static int find_game(const VjoStats *st, const char *uuid)
{
    for (uint32_t g = 0; g < st->games; g++)
        if (!strcmp(st->game[g].uuid, uuid))
            return (int)g;
    return -1;
}

static const char *const kana[] = {"あ", "い", "う", "え", "お", "か", "き", "く", "け", "こ", "漢", "字", "ー"};

/* xorshift32: the same files on every host */
static int rnd(unsigned *seed)
{
    unsigned x = *seed ? *seed : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return (int)(x >> 1);
}

/* A random line: kana and kanji, ASCII letters and digits, punctuation, quotes, line breaks. */
static void random_text(char *out, size_t cap, unsigned *seed)
{
    size_t n = 0;
    int parts = 1 + rnd(seed) % 12;
    out[0] = 0;
    for (int i = 0; i < parts && n + 16 < cap; i++) {
        switch (rnd(seed) % 9) {
        case 0: n += (size_t)snprintf(out + n, cap - n, "\""); break;
        case 1: n += (size_t)snprintf(out + n, cap - n, "\n"); break;
        case 2: n += (size_t)snprintf(out + n, cap - n, "\n\""); break; /* a quote that starts a row */
        case 3: n += (size_t)snprintf(out + n, cap - n, "ABC12"); break;
        case 4: n += (size_t)snprintf(out + n, cap - n, "「…」、"); break;
        default: n += (size_t)snprintf(out + n, cap - n, "%s", kana[rnd(seed) % 13]);
        }
    }
}

typedef struct {
    const char *platform, *tid, *name;
} Game;
static const Game games[] = {{"vita", "PCSG00415", "CLANNAD"}, {"vita", "PCSG00062", "TIME TRAVELERS"},
                             {"psp", "ULJM05000", "夕凪\"ステップ\""}, {"vita", "PCSG00007", "リトルバスターズ！"}};

/* A random history: n records of up to 4 games; with `damage`, invalid times, clocks set back and records a power
 * loss cut short, run into the next append. Its days are around 2026-10 in UTC. Returns the newest time. */
static uint64_t random_history(Text *t, int n, unsigned seed, int damage)
{
    static const double gaps[] = {0.1, 0.9, 1, 1.5, 3, 5, 8, 15, 15.000001, 30, 42, 80, 150, 299, 301, 900, 1800,
                                  7200, 30000, 86400, 200000};
    uint64_t us = 1792220400250123ull, newest = 0; /* 2026-10-17T07:00:00.250123Z */
    int game = 0;
    for (int i = 0; i < n; i++) {
        char text[256];
        if (rnd(&seed) % 5 == 0)
            game = rnd(&seed) % 4;
        us += (uint64_t)(gaps[rnd(&seed) % (sizeof(gaps) / sizeof(gaps[0]))] * 1e6) + rnd(&seed) % 1000;
        if (damage && rnd(&seed) % 40 == 0)
            us -= 3600000000ull; /* the clock set back an hour */
        random_text(text, sizeof(text), &seed);
        int style = damage && rnd(&seed) % 30 == 0 ? 1 + rnd(&seed) % 2 : 0;
        size_t at = t->len;
        record(t, games[game].platform, games[game].tid, games[game].name, text, us, style);
        if (damage && rnd(&seed) % 35 == 0 && t->len - at > 20) { /* cut short by a power loss */
            t->len = at + 1 + (size_t)rnd(&seed) % (t->len - at - 2);
            t->buf[t->len] = 0;
        }
        if (us > newest)
            newest = us;
    }
    return newest;
}

/* Every game's total and its newest day equal the Shell's count of the file, each game as its scope. */
static void check_parity(const Text *t, const char *what, int32_t tz)
{
    int cut = !t->len || t->buf[t->len - 1] != '\n';
    for (size_t k = 0; k < sizeof(games) / sizeof(games[0]); k++) {
        char uuid[VJO_STATS_UUID];
        snprintf(uuid, sizeof(uuid), "%s-%s", games[k].platform, games[k].tid);
        int g = find_game(&s, uuid);
        if (g < 0)
            continue;
        int32_t day = s.game[g].last_day;
        uint64_t start = (uint64_t)((int64_t)day * VJO_STATS_DAY_US - (int64_t)tz * 60000000ll);
        Shell want = shell_count(t, uuid, start, start + VJO_STATS_DAY_US, cut);
        VjoStatsCount total = vjo_stats_game_total(&s, g), today = vjo_stats_game_day(&s, g, day);
        TEST_CHECK_(total.chars == want.chars && total.active_us == want.active && total.timed_chars == want.timed,
                    "%s %s total: %llu %llu %llu, the Shell's %llu %llu %llu", what, uuid,
                    (unsigned long long)total.chars, (unsigned long long)total.active_us,
                    (unsigned long long)total.timed_chars, (unsigned long long)want.chars,
                    (unsigned long long)want.active, (unsigned long long)want.timed);
        TEST_CHECK_(today.chars == want.day_chars && today.active_us == want.day_active &&
                    today.timed_chars == want.day_timed,
                    "%s %s today: %llu %llu %llu, the Shell's %llu %llu %llu", what, uuid,
                    (unsigned long long)today.chars, (unsigned long long)today.active_us,
                    (unsigned long long)today.timed_chars, (unsigned long long)want.day_chars,
                    (unsigned long long)want.day_active, (unsigned long long)want.day_timed);
    }
}

static void test_parity_with_the_shell(void)
{
    for (int rule = VJO_READING_ADAPTIVE; rule <= VJO_READING_GAPS_15S; rule++) {
        vjo_reading_rule = (uint32_t)rule;
        for (unsigned seed = 1; seed <= 60; seed++) {
            Text t = {0};
            char what[64];
            int32_t tz = (int32_t)(seed % 5) * 180 - 300; /* -5 h to +7 h */
            random_history(&t, 50 + (int)(seed * 37 % 700), seed, seed % 3 != 0);
            snprintf(what, sizeof(what), "rule %d seed %u", rule, seed);
            vjo_stats_reset(&s, tz, (uint32_t)rule);
            scan_windows(&s, &t, 2048 + seed * 997 % 9000);
            check_parity(&t, what, tz);
            free(t.buf);
        }
    }
    vjo_reading_rule = VJO_READING_ADAPTIVE;
}

/* The same file in any windows, from the start or from a cache written between two of them, counts the same. */
static void test_windows_and_resume(void)
{
    Text t = {0};
    vjo_reading_rule = 0;
    random_history(&t, 600, 77, 1);
    vjo_stats_reset(&s2, 540, 0);
    scan_windows(&s2, &t, 1 << 20);
    TEST_ASSERT(s2.covered == t.len && s2.records > 500);
    static const size_t windows[] = {2048, 3000, 4096, 8191, 65536, 1 << 20};
    for (size_t w = 0; w < sizeof(windows) / sizeof(windows[0]); w++) {
        vjo_stats_reset(&s, 540, 0);
        scan_windows(&s, &t, windows[w]);
        TEST_CHECK_(!memcmp(&s, &s2, sizeof(s)), "window %zu", windows[w]);
    }
    /* stopped at a window's end, cached, read back, and counted on from there */
    static unsigned char cache[2 << 20];
    /* (any byte: a file read while a record is appended, also at a line break inside its text) */
    for (size_t stop = 1000; stop < t.len; stop += t.len / 7 + 1) {
        Text part = {t.buf, stop, 0};
        vjo_stats_reset(&s, 540, 0);
        scan_windows(&s, &part, 4096);
        TEST_CHECK(s.covered <= stop);
        s.history_bytes = stop; /* (the page's: the file's size when it was counted) */
        TEST_CHECK_(s.damaged <= s2.damaged, "stopped at %zu: %llu damaged", stop, (unsigned long long)s.damaged);
        size_t n = vjo_stats_cache_write(&s, cache, sizeof(cache));
        TEST_ASSERT(n == vjo_stats_cache_size(&s));
        TEST_ASSERT(vjo_stats_cache_read(&s, cache, n, 540, 0) == VJO_STATS_CACHE_OK);
        s.history_bytes = 0;
        scan_windows(&s, &t, 4096);
        TEST_CHECK_(!memcmp(&s, &s2, sizeof(s)), "stopped at %zu", stop);
    }
    free(t.buf);
}

/* A count stopped at any byte, cached and resumed, gives one count's result: every byte of a small file with every
 * kind of damage, both rules (audit follow-up: a '"' at a line's start as a window's last byte, whose next byte says
 * whether it starts a record, was skipped when the window had no other record start, as a file of its header alone
 * does). */
static void test_resume_at_every_byte(void)
{
    static unsigned char cache[1 << 20];
    for (int rule = VJO_READING_ADAPTIVE; rule <= VJO_READING_GAPS_15S; rule++) {
        Text t = {0};
        int differ = 0;
        vjo_reading_rule = (uint32_t)rule;
        random_history(&t, 40, 1234 + (unsigned)rule, 1);
        vjo_stats_reset(&s2, 540, (uint32_t)rule);
        scan_windows(&s2, &t, 1 << 20);
        for (size_t stop = 1; stop < t.len; stop++) {
            Text part = {t.buf, stop, 0};
            char keep = t.buf[stop];
            t.buf[stop] = 0;
            vjo_stats_reset(&s, 540, (uint32_t)rule);
            scan_windows(&s, &part, 2048);
            t.buf[stop] = keep;
            s.history_bytes = stop; /* (the page's: the file's size when it was counted) */
            size_t n = vjo_stats_cache_write(&s, cache, sizeof(cache));
            TEST_ASSERT(n && vjo_stats_cache_read(&s, cache, n, 540, (uint32_t)rule) == VJO_STATS_CACHE_OK);
            s.history_bytes = 0;
            scan_windows(&s, &t, 2048);
            if (memcmp(&s, &s2, sizeof(s)) && differ++ < 3)
                TEST_CHECK_(0, "rule %d, stopped at %zu: %llu records, one count %llu", rule, stop,
                            (unsigned long long)s.records, (unsigned long long)s2.records);
        }
        TEST_CHECK_(!differ, "rule %d: %d of %zu stops differ", rule, differ, t.len - 1);
        free(t.buf);
    }
    vjo_reading_rule = VJO_READING_ADAPTIVE;
    /* (audit-rubi-rm2 F1) two counts in a row, at a record's start and one byte after it, then the rest: one count's
     * result, at every record start, both rules */
    for (int rule = VJO_READING_ADAPTIVE; rule <= VJO_READING_GAPS_15S; rule++) {
        Text t = {0};
        int chains = 0, differ = 0;
        vjo_reading_rule = (uint32_t)rule;
        random_history(&t, 40, 77 + (unsigned)rule, 1);
        vjo_stats_reset(&s2, 0, (uint32_t)rule);
        scan_windows(&s2, &t, 1 << 20);
        for (size_t b = 1; b + 1 < t.len; b++) {
            if (!(t.buf[b] == '"' && t.buf[b - 1] == '\n'))
                continue;
            chains++;
            vjo_stats_reset(&s, 0, (uint32_t)rule);
            for (size_t stop = b; stop <= b + 1; stop++) {
                Text part = {t.buf, stop, 0};
                char keep = t.buf[stop];
                t.buf[stop] = 0;
                scan_windows(&s, &part, 2048);
                t.buf[stop] = keep;
                s.history_bytes = stop;
                size_t n = vjo_stats_cache_write(&s, cache, sizeof(cache));
                TEST_ASSERT(n && vjo_stats_cache_read(&s, cache, n, 0, (uint32_t)rule) == VJO_STATS_CACHE_OK);
                s.history_bytes = 0;
            }
            scan_windows(&s, &t, 2048);
            if (memcmp(&s, &s2, sizeof(s)) && differ++ < 3)
                TEST_CHECK_(0, "rule %d: counts at %zu and %zu, then the rest: %llu records, one count %llu", rule, b,
                            b + 1, (unsigned long long)s.records, (unsigned long long)s2.records);
        }
        TEST_CHECK_(chains > 30 && !differ, "rule %d: %d of %d chains differ", rule, differ, chains);
        free(t.buf);
    }
    vjo_reading_rule = VJO_READING_ADAPTIVE;
    /* the same in the middle of a file: a window that holds no record start and ends in one's first byte */
    Text t = {0};
    char big[4000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    record(&t, "vita", "PCSG00415", "CLANNAD", big, 1792220400000000ull, 0);
    record(&t, "vita", "PCSG00415", "CLANNAD", "あいうえお", 1792220410000000ull, 0);
    size_t second = (size_t)(strstr(t.buf, "\"vita-PCSG00415\",\"vjo-1792220410000000") - t.buf);
    vjo_stats_reset(&s2, 0, 0);
    scan_windows(&s2, &t, 1 << 20);
    for (size_t at = second - 2100; at < second; at += 97) {
        vjo_stats_reset(&s, 0, 0);
        s.covered = at; /* (a count from a byte inside the long record: its windows end at every offset) */
        for (size_t w = 1; w < 2100 && s.covered < second; w += 1) {
            if (s.covered + w == second + 1) { /* the window's last byte is the second record's '"' */
                char buf[4096];
                uint64_t base = s.covered - 1;
                memcpy(buf, t.buf + base, (size_t)(second + 1 - base));
                vjo_stats_scan(&s, buf, (size_t)(second + 1 - base), base, VJO_STATS_SCAN_MORE);
                TEST_CHECK_(s.covered == second, "from %zu: the next window starts at %llu, not at the record's %zu", at,
                            (unsigned long long)s.covered, second);
                break;
            }
        }
    }
    free(t.buf);
}

/* A full day table (decades of days) drops its oldest rollup, so today still counts. */
static void test_a_full_day_table_drops_the_oldest(void)
{
    Text t = {0};
    uint64_t day0 = (uint64_t)vjo_stats_days_from_civil(2000, 1, 1) * 86400000000ull + 3600000000ull;
    for (int d = 0; d < VJO_STATS_DAYS + 3; d++)
        record(&t, "vita", "PCSG00415", "CLANNAD", "あ", day0 + (uint64_t)d * 86400000000ull, 0);
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &t, 1 << 20);
    int32_t first = vjo_stats_days_from_civil(2000, 1, 1), last = first + VJO_STATS_DAYS + 2;
    TEST_CHECK(s.days == VJO_STATS_DAYS && s.days_full == 3);
    TEST_CHECK(s.day[0].day == first + 3 && s.day[s.days - 1].day == last);
    TEST_CHECK(vjo_stats_game_day(&s, 0, last).chars == 1 && vjo_stats_game_day(&s, 0, first).chars == 0);
    TEST_CHECK(s.game[0].chars == VJO_STATS_DAYS + 3); /* the total keeps every record */
    /* and a record older than every day kept is not added */
    Text o = {0};
    record(&o, "vita", "PCSG00415", "CLANNAD", "い", day0, 0);
    size_t was = s.covered;
    (void)was;
    VjoStats copy = s;
    char buf[1024];
    size_t header = (size_t)(strchr(o.buf, '\n') - o.buf) + 1;
    memcpy(buf, o.buf + header - 1, o.len - header + 1);
    vjo_stats_scan(&s, buf, o.len - header + 1, copy.covered ? copy.covered : 1, VJO_STATS_SCAN_END);
    TEST_CHECK(s.days == VJO_STATS_DAYS && s.day[0].day == first + 3 && s.days_full == 4);
    free(o.buf);
    free(t.buf);
}

/* A record whose end is not in the file yet (being appended, or cut by a power loss with nothing after it)
 * waits; once the rest is there it counts, once. One a power loss cut and the next append ran into is damaged:
 * not counted, and the line before it gets no time, as in the Shell. */
static void test_cut_records(void)
{
    Text t = {0}, whole = {0};
    record(&whole, "vita", "PCSG00415", "CLANNAD", "あいうえお", 1792220400000000ull, 0);
    record(&whole, "vita", "PCSG00415", "CLANNAD", "かきくけこさしすせそ", 1792220410000000ull, 0);
    record(&whole, "vita", "PCSG00415", "CLANNAD", "たちつてと", 1792220420000000ull, 0);
    vjo_stats_reset(&s2, 0, 0);
    scan_windows(&s2, &whole, 1 << 16);
    TEST_ASSERT(s2.records == 3 && s2.game[0].chars == 20);
    size_t last = (size_t)(strstr(whole.buf, "\"vita-PCSG00415\",\"vjo-1792220420000000") - whole.buf);
    for (size_t cut = last + 1; cut < whole.len; cut += 7) {
        t.buf = whole.buf;
        t.len = cut;
        char keep = whole.buf[cut];
        whole.buf[cut] = 0;
        vjo_stats_reset(&s, 0, 0);
        scan_windows(&s, &t, 2048);
        whole.buf[cut] = keep;
        /* (a cut that leaves only the third record's first byte: its start is not known yet, so the second
         * record's end is not either, and it waits too, as in the Shell's count) */
        size_t second = (size_t)(strstr(whole.buf, "\"vita-PCSG00415\",\"vjo-1792220410000000") - whole.buf);
        int lone = cut == last + 1;
        TEST_CHECK_(s.records == (lone ? 1u : 2u) && s.covered == (lone ? second : last) &&
                    s.game[0].chars == (lone ? 5u : 15u), "cut at %zu: %llu records", cut,
                    (unsigned long long)s.records);
        scan_windows(&s, &whole, 2048); /* the rest appended */
        TEST_CHECK_(!memcmp(&s, &s2, sizeof(s)), "cut at %zu, then whole", cut);
    }
    /* cut short, then the next boot's append right after it */
    Text p = {0};
    record(&p, "vita", "PCSG00415", "CLANNAD", "あいうえお", 1792220400000000ull, 0);
    size_t at = p.len;
    record(&p, "vita", "PCSG00415", "CLANNAD", "かきくけこさしすせそ", 1792220410000000ull, 0);
    p.len = at + 40;
    /* the next boot's record runs on from the cut one (no line break between): both are one damaged record */
    record(&p, "vita", "PCSG00415", "CLANNAD", "たちつてと", 1792220420000000ull, 0);
    record(&p, "vita", "PCSG00415", "CLANNAD", "なにぬねの", 1792220430000000ull, 0);
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &p, 2048);
    TEST_CHECK_(s.records == 2 && s.damaged == 1 && s.game[0].chars == 10, "%llu %llu %llu",
                (unsigned long long)s.records, (unsigned long long)s.damaged, (unsigned long long)s.game[0].chars);
    TEST_CHECK(s.game[0].time.active_us == 0); /* the line before the damaged one: no time */
    vjo_reading_rule = 0;
    check_parity(&p, "power cut", 0);
    /* a record being appended can end in a line break inside its text: it waits, and counts once it is whole */
    Text q = {0};
    record(&q, "vita", "PCSG00415", "CLANNAD", "あいうえお", 1792220400000000ull, 0);
    record(&q, "vita", "PCSG00415", "CLANNAD", "かきく\nけこ", 1792220410000000ull, 0);
    size_t nl = (size_t)(strstr(q.buf, "かきく\n") - q.buf) + strlen("かきく\n");
    Text part = {q.buf, nl, 0};
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &part, 2048);
    TEST_CHECK_(s.records == 1 && s.damaged == 0 && s.covered < nl, "%llu records, %llu damaged",
                (unsigned long long)s.records, (unsigned long long)s.damaged);
    scan_windows(&s, &q, 2048);
    TEST_CHECK(s.records == 2 && s.damaged == 0 && s.game[0].chars == 10 && s.covered == q.len);
    free(q.buf);
    free(whole.buf);
    free(p.buf);
}

/* Local days: a record is its local day's in the counts' zone; a line read across midnight is credited to the
 * day it began, which keeps its characters (the next line's go to the next day). */
static void test_days_and_midnight(void)
{
    Text t = {0};
    /* 2026-10-27 14:59:55 UTC = 23:59:55 in Japan (+9 h); the next line 10 s later, after midnight there */
    uint64_t t0 = (uint64_t)vjo_stats_days_from_civil(2026, 10, 27) * 86400000000ull + 14ull * 3600000000ull +
                  59ull * 60000000ull + 55000000ull;
    record(&t, "vita", "PCSG00415", "CLANNAD", "あいうえおかきくけこ", t0, 0);
    record(&t, "vita", "PCSG00415", "CLANNAD", "さしすせそ", t0 + 10000000ull, 0);
    record(&t, "vita", "PCSG00415", "CLANNAD", "たちつてと", t0 + 20000000ull, 0);
    vjo_reading_rule = 0;
    vjo_stats_reset(&s, 540, 0);
    scan_windows(&s, &t, 4096);
    int32_t d27 = vjo_stats_days_from_civil(2026, 10, 27), d28 = d27 + 1;
    VjoStatsCount a = vjo_stats_game_day(&s, 0, d27), b = vjo_stats_game_day(&s, 0, d28);
    TEST_CHECK(a.chars == 10 && a.active_us == 10000000ull && a.timed_chars == 10);
    TEST_CHECK(b.chars == 10 && b.active_us == 10000000ull && b.timed_chars == 5);
    TEST_CHECK(s.game[0].first_day == d27 && s.game[0].last_day == d28);
    /* in UTC every line is the 27th's */
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &t, 4096);
    a = vjo_stats_game_day(&s, 0, d27);
    TEST_CHECK(a.chars == 20 && a.active_us == 20000000ull && s.game[0].last_day == d27);
    /* the days of every game together */
    VjoStatsCount days[3];
    vjo_stats_days_range(&s, d27 - 1, 3, days);
    TEST_CHECK(days[0].chars == 0 && days[1].chars == 20 && days[2].chars == 0);
    TEST_CHECK(vjo_stats_day_of(&s, 0) == INT32_MIN);
    /* a negative zone, and a day before 1970 */
    s.tz_minutes = -300;
    TEST_CHECK(vjo_stats_day_of(&s, 1) == -1);
    TEST_CHECK(vjo_stats_day_of(&s, 5ull * 3600000000ull) == 0);
    free(t.buf);
}

/* Invalid times: the record counts for its game's total, on no day, and breaks the interval; another game's
 * record breaks it too; a clock set back gives no time. */
static void test_breaks(void)
{
    Text t = {0};
    uint64_t t0 = 1792220400000000ull;
    vjo_reading_rule = 0;
    record(&t, "vita", "PCSG00415", "CLANNAD", "あいうえお", t0, 0);
    record(&t, "vita", "PCSG00415", "CLANNAD", "かきくけこ", t0 + 10000000ull, 1); /* five fractional digits */
    record(&t, "vita", "PCSG00415", "CLANNAD", "さしすせそ", t0 + 20000000ull, 0);
    record(&t, "vita", "PCSG00062", "TT", "たちつてと", t0 + 30000000ull, 0);
    record(&t, "vita", "PCSG00415", "CLANNAD", "なにぬねの", t0 + 40000000ull, 0);
    record(&t, "vita", "PCSG00415", "CLANNAD", "はひふへほ", t0 + 30000000ull, 0); /* set back */
    record(&t, "vita", "PCSG00415", "CLANNAD", "まみむめも", t0 + 35000000ull, 0);
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &t, 4096);
    int g = find_game(&s, "vita-PCSG00415");
    TEST_ASSERT(g >= 0);
    TEST_CHECK(s.game[g].chars == 30 && s.game[g].lines == 6 && s.untimed == 1);
    TEST_CHECK(vjo_stats_game_day(&s, g, vjo_stats_day_of(&s, t0)).chars == 25);
    /* only the line before the last: 5 s */
    TEST_CHECK(s.game[g].time.active_us == 5000000ull && s.game[g].time.timed_chars == 5);
    check_parity(&t, "breaks", 0);
    free(t.buf);
}

/* tools/reading_time_sim.py's timelines (host/fixtures/reading_time): their known counts with each rule, as
 * host/tests/test_shell_history.c checks the Shell's. */
static void fixture_text(char *out, size_t cap, const char *id, unsigned chars)
{
    static const char *const k[] = {"あ", "い", "う", "え", "お", "か", "き", "く", "け", "こ", "さ", "し", "す",
                                    "せ", "そ", "た", "ち", "つ", "て", "と", "な", "に", "ぬ", "ね", "の"};
    static const char *const kind[] = {"■", "□", "▲", "△"};
    static const char *const digit[] = {"⓪", "①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨"};
    uint32_t h = 2166136261u;
    size_t n = 0;
    for (const char *p = id; *p; p++)
        h = (h ^ (unsigned char)*p) * 16777619u;
    n += (size_t)snprintf(out + n, cap - n, "「");
    for (unsigned i = 0; i < chars; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s", k[(h + i * 7u) % 25u]);
    n += (size_t)snprintf(out + n, cap - n, "」%s", kind[strchr("LPXM", id[0]) - "LPXM"]);
    for (const char *p = id + 1; *p; p++)
        n += (size_t)snprintf(out + n, cap - n, "%s", digit[*p - '0']);
}
static uint64_t header_value(const char *text, const char *key, const char *name)
{
    const char *line = strstr(text, key), *v = line ? strstr(line, name) : NULL;
    TEST_ASSERT_(v != NULL && v < strchr(line, '\n'), "%s %s", key, name);
    return strtoull(v + strlen(name), NULL, 10);
}
static void test_timeline_fixtures(void)
{
    static const char *const names[] = {"clean-learner", "afk-breaks-learner", "suspend-standby-learner",
                                        "menus-backlog-learner", "skip-mode-learner", "auto-mode-learner",
                                        "everyday-learner", "everyday-hook-learner", "everyday-beginner",
                                        "everyday-fluent"};
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        char file[4096];
        static char text[64 * 1024];
        snprintf(file, sizeof(file), "%s/reading_time/%s.tsv", VJO_FIXTURES, names[k]);
        FILE *in = fopen(file, "rb");
        TEST_ASSERT_(in != NULL, "%s", file);
        size_t len = fread(text, 1, sizeof(text) - 1, in);
        fclose(in);
        text[len] = 0;
        Text t = {0};
        char previous[512] = "";
        for (char *line = text; *line; line = strchr(line, '\n') + 1) {
            if (*line != '#') {
                char id[24], body[512];
                unsigned long long us;
                unsigned chars;
                TEST_ASSERT(sscanf(line, "%llu\t%u\t%23s", &us, &chars, id) == 3);
                if (strcmp(id, "-")) {
                    fixture_text(body, sizeof(body), id, chars);
                    if (strcmp(body, previous)) /* the writer keeps no text twice in a row */
                        record(&t, "vita", "PCSG00415", "CLANNAD", body, us, 0);
                    snprintf(previous, sizeof(previous), "%s", body);
                } else
                    previous[0] = 0;
            }
            if (!strchr(line, '\n'))
                break;
        }
        for (int rule = VJO_READING_ADAPTIVE; rule <= VJO_READING_GAPS_15S; rule++) {
            const char *key = rule == VJO_READING_ADAPTIVE ? "# adaptive:" : "# 15s:";
            vjo_reading_rule = (uint32_t)rule;
            vjo_stats_reset(&s, 0, (uint32_t)rule);
            scan_windows(&s, &t, 65536);
            VjoStatsCount n = vjo_stats_game_total(&s, 0);
            TEST_CHECK_(n.chars == header_value(text, key, "characters=") &&
                        n.active_us == header_value(text, key, "active_us=") &&
                        n.timed_chars == header_value(text, key, "timed_chars="),
                        "%s %s: %llu %llu %llu", names[k], key, (unsigned long long)n.chars,
                        (unsigned long long)n.active_us, (unsigned long long)n.timed_chars);
            if (rule == VJO_READING_ADAPTIVE)
                TEST_CHECK(s.game[0].time.pace == header_value(text, key, "pace="));
            /* the days add up to the total: every credit is some day's */
            VjoStatsCount sum = {0, 0, 0};
            for (uint32_t d = 0; d < s.days; d++) {
                sum.chars += s.day[d].n.chars;
                sum.active_us += s.day[d].n.active_us;
                sum.timed_chars += s.day[d].n.timed_chars;
            }
            TEST_CHECK(!memcmp(&sum, &n, sizeof(n)));
        }
        free(t.buf);
    }
    vjo_reading_rule = VJO_READING_ADAPTIVE;
}

/* Files of older builds: 0.8.0's header (no characters column), 0.9.0's (no line_source), and a header a power
 * loss cut: the same counts. */
static void test_older_headers(void)
{
    Text t = {0};
    random_history(&t, 300, 5, 0);
    vjo_stats_reset(&s2, 0, 0);
    scan_windows(&s2, &t, 4096);
    static const char *const cut[] = {",characters,line_source\n", ",line_source\n"};
    for (int k = 0; k < 2; k++) {
        Text o = {0};
        char *end = strstr(t.buf, cut[k]);
        put(&o, t.buf, (size_t)(end - t.buf));
        puts_(&o, "\n");
        const char *body = strchr(t.buf, '\n') + 1;
        puts_(&o, body);
        vjo_stats_reset(&s, 0, 0);
        scan_windows(&s, &o, 4096);
        TEST_CHECK_(s.records == s2.records && !memcmp(s.game, s2.game, sizeof(s.game)) &&
                    !memcmp(s.day, s2.day, sizeof(s.day)), "header %d", k);
        free(o.buf);
    }
    /* a header a power loss cut: the next boot's record runs on from it, not at a line's start, and is lost with
     * it, as in the Shell's count; every later one counts */
    Text o = {0};
    puts_(&o, "uuid,given_identifier,name,li");
    puts_(&o, strchr(t.buf, '\n') + 1);
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &o, 4096);
    TEST_CHECK(s.records == s2.records - 1);
    check_parity(&o, "cut header", 0);
    free(o.buf);
    free(t.buf);
}

/* ---------------- the cache ---------------- */

static void test_cache(void)
{
    static unsigned char bytes[2 << 20];
    Text t = {0};
    random_history(&t, 400, 11, 1);
    vjo_stats_reset(&s2, 60, 1);
    vjo_reading_rule = 1;
    scan_windows(&s2, &t, 4096);
    vjo_reading_rule = 0;
    s2.tail_len = 200;
    s2.tail_check = 12345;
    s2.head_len = 256;
    s2.head_check = 678;
    s2.history_bytes = t.len + 10;
    s2.card[0].game = 1;
    s2.cards = 1;
    size_t n = vjo_stats_cache_write(&s2, bytes, sizeof(bytes));
    TEST_ASSERT(n > 0 && n == vjo_stats_cache_size(&s2));
    TEST_CHECK(vjo_stats_cache_write(&s2, bytes, n - 1) == 0);
    TEST_CHECK(vjo_stats_cache_read(&s, bytes, n, 60, 1) == VJO_STATS_CACHE_OK);
    TEST_CHECK(!memcmp(&s, &s2, sizeof(s)));
    /* each refusal resets the counts for a count from the start */
    TEST_CHECK(vjo_stats_cache_read(&s, bytes, n, 60, 0) == VJO_STATS_CACHE_RULE && s.covered == 0 && !s.games);
    TEST_CHECK(vjo_stats_cache_read(&s, bytes, n, 0, 1) == VJO_STATS_CACHE_ZONE && s.covered == 0 && s.tz_minutes == 0);
    TEST_CHECK(vjo_stats_cache_read(&s, bytes, n - 1, 60, 1) == VJO_STATS_CACHE_DAMAGED);
    TEST_CHECK(vjo_stats_cache_read(&s, NULL, 0, 60, 1) == VJO_STATS_CACHE_DAMAGED);
    for (size_t at = 0; at < n; at += 1 + n / 97) {
        bytes[at] ^= 0x20;
        int rc = vjo_stats_cache_read(&s, bytes, n, 60, 1);
        TEST_CHECK_(rc != VJO_STATS_CACHE_OK && s.covered == 0 && s.games == 0 && s.open == -1, "byte %zu: %d", at, rc);
        bytes[at] ^= 0x20;
    }
    bytes[8] ^= 1; /* another version */
    TEST_CHECK(vjo_stats_cache_read(&s, bytes, n, 60, 1) == VJO_STATS_CACHE_VERSION);
    free(t.buf);
}

/* ---------------- the goal, the streak, the calendar ---------------- */

/* s with one game read on these days (local days, UTC zone): with reading_time = 15s, lines of `chars`
 * characters 10 s apart, each credited 10 s, minutes x 6 + 1 of them a day (the day's last line gets no time:
 * the next is a day later). */
static void days_read(const int32_t *days, int n, unsigned chars, unsigned minutes)
{
    Text t = {0};
    char text[4096];
    size_t k = 0;
    for (unsigned i = 0; i < chars; i++)
        k += (size_t)snprintf(text + k, sizeof(text) - k, "あ");
    for (int i = 0; i < n; i++) {
        uint64_t start = (uint64_t)days[i] * 86400000000ull + 3600000000ull;
        for (unsigned m = 0; m <= minutes * 6; m++)
            record(&t, "vita", "PCSG00415", "CLANNAD", text, start + m * 10000000ull, 0);
    }
    vjo_reading_rule = VJO_READING_GAPS_15S;
    vjo_stats_reset(&s, 0, VJO_READING_GAPS_15S);
    scan_windows(&s, &t, 1 << 20);
    vjo_reading_rule = VJO_READING_ADAPTIVE;
    free(t.buf);
}

static void test_goal_and_streak(void)
{
    int32_t today = vjo_stats_days_from_civil(2026, 10, 27);
    int32_t days[] = {today - 10, today - 9, today - 8, today - 7, today - 5, today - 2, today - 1};
    int current, best;
    days_read(days, 7, 200, 2); /* 13 lines of 200: 2,600 characters and 2 minutes a day */
    vjo_stats_streak(&s, today, VJO_GOAL_OFF, &current, &best);
    TEST_CHECK_(current == 2 && best == 4, "%d %d", current, best); /* today not read yet: yesterday's run */
    vjo_stats_streak(&s, today - 1, VJO_GOAL_OFF, &current, &best);
    TEST_CHECK(current == 2 && best == 4);
    vjo_stats_streak(&s, today + 1, VJO_GOAL_OFF, &current, &best);
    TEST_CHECK(current == 0 && best == 4); /* yesterday was not read either */
    vjo_stats_streak(&s, today - 6, VJO_GOAL_OFF, &current, &best);
    TEST_CHECK_(current == 4 && best == 4, "%d %d", current, best); /* the days after it do not count */
    vjo_stats_streak(&s, today, VJO_GOAL_2K, &current, &best);
    TEST_CHECK(current == 2 && best == 4);
    vjo_stats_streak(&s, today, VJO_GOAL_5K, &current, &best);
    TEST_CHECK(current == 0 && best == 0);
    vjo_stats_streak(&s, today, VJO_GOAL_15MIN, &current, &best);
    TEST_CHECK(current == 0 && best == 0);
    VjoStatsCount day = vjo_stats_game_day(&s, 0, today - 1);
    TEST_CHECK_(day.chars == 2600 && day.active_us == 120000000ull && day.timed_chars == 2400, "%llu %llu",
                (unsigned long long)day.chars, (unsigned long long)day.active_us);
    TEST_CHECK(vjo_stats_goal_met(&day, VJO_GOAL_2K) && !vjo_stats_goal_met(&day, VJO_GOAL_5K));
    TEST_CHECK(vjo_stats_goal_progress(&day, VJO_GOAL_2K) == 1300 && vjo_stats_goal_progress(&day, VJO_GOAL_5K) == 520);
    TEST_CHECK(vjo_stats_goal_progress(&day, VJO_GOAL_15MIN) == 133 && vjo_stats_goal_progress(&day, VJO_GOAL_OFF) == 0);
    /* today read: the run goes on to today */
    int32_t more[] = {today - 2, today - 1, today};
    days_read(more, 3, 10, 1);
    vjo_stats_streak(&s, today, VJO_GOAL_OFF, &current, &best);
    TEST_CHECK(current == 3 && best == 3);
    /* (audit-rubi-rm2 F4.5) days after today (a clock set back, then forward): in no streak, nor in the best */
    int32_t ahead[] = {today - 2, today - 1, today, today + 1, today + 2, today + 3};
    days_read(ahead, 6, 10, 1);
    vjo_stats_streak(&s, today, VJO_GOAL_OFF, &current, &best);
    TEST_CHECK_(current == 3 && best == 3, "%d %d", current, best);
    vjo_stats_streak(&s, today, VJO_GOAL_15MIN, &current, &best);
    TEST_CHECK(current == 0 && best == 0);
    VjoStatsCount none = {0, 0, 0}, two = {2000, 0, 0}, mins = {0, 30 * 60000000ull, 0};
    TEST_CHECK(!vjo_stats_goal_met(&none, VJO_GOAL_OFF) && vjo_stats_goal_met(&two, VJO_GOAL_2K));
    TEST_CHECK(vjo_stats_goal_met(&mins, VJO_GOAL_30MIN) && !vjo_stats_goal_met(&mins, VJO_GOAL_60MIN));
    TEST_CHECK(vjo_stats_goal_chars(VJO_GOAL_50K) == 50000 && vjo_stats_goal_minutes(VJO_GOAL_120MIN) == 120 &&
               !vjo_stats_goal_chars(VJO_GOALS) && !vjo_stats_goal_minutes(-1));
}

static void test_calendar(void)
{
    int y, m, d;
    TEST_CHECK(vjo_stats_days_from_civil(1970, 1, 1) == 0 && vjo_stats_weekday(0) == 4);
    TEST_CHECK(vjo_stats_days_from_civil(2000, 3, 1) == 11017 && vjo_stats_days_from_civil(2026, 1, 1) == 20454);
    TEST_CHECK(vjo_stats_weekday(vjo_stats_days_from_civil(2026, 10, 27)) == 2); /* a Tuesday */
    TEST_CHECK(vjo_stats_weekday(-1) == 3);
    for (int32_t day = -800000; day < 800000; day += 13) {
        vjo_stats_civil(day, &y, &m, &d);
        TEST_ASSERT_(vjo_stats_days_from_civil(y, m, d) == day && m >= 1 && m <= 12 && d >= 1 && d <= 31, "%d", day);
    }
    vjo_stats_civil(vjo_stats_days_from_civil(2024, 2, 29), &y, &m, &d);
    TEST_CHECK(y == 2024 && m == 2 && d == 29);
}

/* ---------------- cards and the games' order ---------------- */

static void test_cards_and_order(void)
{
    uint8_t id[16];
    VjoStatsCard c[6];
    uint32_t per[VJO_STATS_GAMES];
    TEST_CHECK(vjo_stats_card_id("0123456789abcdef0123456789abcdef", id) == 0 && id[0] == 0x01 && id[15] == 0xef);
    TEST_CHECK(vjo_stats_card_id("0123456789ABCDEF0123456789abcdef", id) < 0);
    TEST_CHECK(vjo_stats_card_id("0123456789abcdef0123456789abcde", id) < 0);
    TEST_CHECK(vjo_stats_card_id("0123456789abcdef0123456789abcdef0", id) < 0);
    memset(c, 0, sizeof(c));
    for (int i = 0; i < 6; i++)
        c[i].id[0] = (uint8_t)(i % 4 ? 9 - i : 1);
    TEST_CHECK(vjo_stats_cards_sort(c, 6) == 5 && c[0].id[0] == 1 && c[1].id[0] == 4 && c[4].id[0] == 8);
    Text t = {0};
    record(&t, "vita", "PCSG00415", "CLANNAD", "あいうえお", 1792220400000000ull, 0);
    record(&t, "vita", "PCSG00062", "TIME TRAVELERS", "かきくけこさしすせそ", 1792220400000000ull + 86400000000ull, 0);
    record(&t, "psp", "ULJM05000", "夕凪", "かきくけこさしすせそ", 1792220400000000ull, 0);
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &t, 4096);
    TEST_CHECK(vjo_stats_card_game(&s, "vita", "PCSG00062") == 1);
    TEST_CHECK(vjo_stats_card_game(&s, "vita", "PCSE00001") == 3 && s.games == 4 && s.game[3].last_day == INT32_MIN);
    TEST_CHECK(vjo_stats_card_game(&s, "", "PCSE00001") == VJO_STATS_NO_GAME);
    memcpy(s.card, c, 4 * sizeof(c[0]));
    s.cards = 4;
    s.card[0].game = 1;
    s.card[1].game = 1;
    s.card[2].game = 3;
    s.card[3].game = VJO_STATS_NO_GAME;
    TEST_CHECK(vjo_stats_cards_per_game(&s, per) == 4 && per[1] == 2 && per[3] == 1 && per[0] == 0);
    uint8_t six[16] = {6};
    TEST_CHECK(vjo_stats_card_find(&s, six) == 2);
    six[0] = 5;
    TEST_CHECK(vjo_stats_card_find(&s, six) < 0);
    uint16_t order[VJO_STATS_GAMES];
    TEST_CHECK(vjo_stats_game_order(&s, order) == 4);
    /* the newest day first; then the most characters; a game with cards only last */
    TEST_CHECK(order[0] == 1 && order[1] == 2 && order[2] == 0 && order[3] == 3);
    uint64_t lines;
    VjoStatsCount all = vjo_stats_all(&s, &lines);
    TEST_CHECK(all.chars == 25 && lines == 3);
    free(t.buf);
}

/* uuids longer than the table keeps (23 bytes; another tool's, or an edited file's) that share those bytes are told
 * apart by the whole uuid's hash, as the Shell's count tells them apart (record_line's FNV-1a of the cell). */
static void test_long_uuids_stay_apart(void)
{
    Text t = {0};
    uint64_t t0 = 1792220400000000ull;
    for (int i = 0; i < 6; i++)
        record(&t, "vita", i % 2 ? "ABCDEFGHIJKLMNOPQR1" : "ABCDEFGHIJKLMNOPQR2", "G", i % 2 ? "あいう" : "かきくけこ",
               t0 + (uint64_t)i * 1000000ull, 0);
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &t, 4096);
    TEST_CHECK_(s.games == 2, "%u games", (unsigned)s.games);
    TEST_CHECK(s.games == 2 && s.game[0].chars + s.game[1].chars == 24 && s.game[0].chars != s.game[1].chars);
    TEST_CHECK(!strcmp(s.game[0].uuid, "vita-ABCDEFGHIJKLMNOPQR") && !strcmp(s.game[1].uuid, s.game[0].uuid));
    /* each game's lines alternate with the other's: no line gets a time, as in the Shell's count of each */
    TEST_CHECK(s.game[0].time.active_us == 0 && s.game[1].time.active_us == 0);
    free(t.buf);
}

/* More games than the table holds: their characters count in All games, and every other game's as before. */
static void test_full_tables(void)
{
    Text t = {0};
    for (int i = 0; i < VJO_STATS_GAMES + 3; i++) {
        char tid[16];
        snprintf(tid, sizeof(tid), "PCSG%05d", i);
        record(&t, "vita", tid, tid, "あいう", 1792220400000000ull + (uint64_t)i * 1000000ull, 0);
    }
    vjo_stats_reset(&s, 0, 0);
    scan_windows(&s, &t, 65536);
    uint64_t lines;
    VjoStatsCount all = vjo_stats_all(&s, &lines);
    TEST_CHECK(s.games == VJO_STATS_GAMES && s.dropped == 3 && all.chars == 3u * (VJO_STATS_GAMES + 3) &&
               lines == VJO_STATS_GAMES + 3);
    free(t.buf);
}

TEST_LIST = {
    {"each game equals the Shell's count: total and today, both rules, random files", test_parity_with_the_shell},
    {"any windows, and resumed from a cache at any of them, count the same", test_windows_and_resume},
    {"resumed from a count stopped at any byte: one count's result", test_resume_at_every_byte},
    {"a full day table drops the oldest day, not today's", test_a_full_day_table_drops_the_oldest},
    {"a record not yet whole waits; one a power loss cut is damaged", test_cut_records},
    {"local days in the counts' zone; a line across midnight is its first day's", test_days_and_midnight},
    {"invalid times, other games and clocks set back break the interval", test_breaks},
    {"tools/reading_time_sim.py's timelines: their known counts", test_timeline_fixtures},
    {"0.8.0's and 0.9.0's headers and a cut header count the same", test_older_headers},
    {"the cache: round trip and every refusal", test_cache},
    {"the goal and the streak", test_goal_and_streak},
    {"the calendar", test_calendar},
    {"cards per game and the games' order", test_cards_and_order},
    {"more games than the table", test_full_tables},
    {"uuids that share their first 23 bytes stay apart", test_long_uuids_stay_apart},
    {NULL, NULL}};
