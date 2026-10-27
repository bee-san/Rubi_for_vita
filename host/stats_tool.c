/* The Rubi app's Stats tab counts (core/stats.c) on the host, for tools/tests/test_stats_tab.py and the parse-time
 * measurement of #312 (the host's time, not a console's: the app runs on the Vita's CPU3):
 *
 *   vjo-stats [--tz MINUTES] [--rule adaptive|15s] [--window BYTES] FILE
 *       counts FILE as the tab does (windows of BYTES, 64 KiB by default, each with the byte before it) and prints
 *       JSON: each game's totals, every day's (all games), the totals of every game and the count's facts
 *   vjo-stats --bench N [--window BYTES] FILE
 *       counts FILE from its start N times and prints the median time in ns, the bytes and the records; then the
 *       time of one record appended to a cached count (the tab's usual case)
 *   vjo-stats --synthetic BYTES FILE
 *       writes a history.csv of about BYTES in the writer's format: four games, lines of 5 to 60 characters
 *       read 2 s to 2 minutes apart, ten hours a day, a fixed seed (no game's real text)
 *
 * Days are UTC plus --tz minutes, as the tab's are the Vita's local days. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/history_lines.h"
#include "../core/stats.h"

static VjoStats s;

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "vjo-stats: cannot read %s\n", path);
        exit(2);
    }
    buf = malloc((size_t)n + 2);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "vjo-stats: cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* The tab's count of file[0, len) from s->covered, in windows (the page's loop: settings/stats_page.c). */
static void count(const char *file, size_t len, size_t window, char *buf)
{
    uint64_t pos = s.covered;
    while (pos < len) {
        uint64_t base = pos ? pos - 1 : 0, to = pos + window < len ? pos + window : len;
        int end = to < len ? VJO_STATS_SCAN_MORE : file[len - 1] == '\n' ? VJO_STATS_SCAN_END : VJO_STATS_SCAN_CUT;
        memcpy(buf, file + base, to - base);
        uint64_t next = vjo_stats_scan(&s, buf, to - base, base, end);
        if (next == pos)
            break;
        pos = next;
    }
    s.history_bytes = len;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void json_string(const char *str)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 0x20)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}

static void date(int32_t day)
{
    int y, m, d;
    if (day == INT32_MIN) {
        printf("null");
        return;
    }
    vjo_stats_civil(day, &y, &m, &d);
    printf("\"%04d-%02d-%02d\"", y, m, d);
}

static void counts(const VjoStatsCount *n)
{
    printf("\"chars\": %llu, \"active_us\": %llu, \"timed_chars\": %llu", (unsigned long long)n->chars,
           (unsigned long long)n->active_us, (unsigned long long)n->timed_chars);
}

static void report(void)
{
    uint64_t lines;
    VjoStatsCount all = vjo_stats_all(&s, &lines);
    printf("{\"records\": %llu, \"damaged\": %llu, \"untimed\": %llu, \"covered\": %llu, \"dropped\": %llu,\n",
           (unsigned long long)s.records, (unsigned long long)s.damaged, (unsigned long long)s.untimed,
           (unsigned long long)s.covered, (unsigned long long)s.dropped);
    printf(" \"all\": {");
    counts(&all);
    printf(", \"lines\": %llu},\n \"games\": [", (unsigned long long)lines);
    for (uint32_t g = 0; g < s.games; g++) {
        VjoStatsCount n = vjo_stats_game_total(&s, (int)g);
        printf("%s\n  {\"uuid\": ", g ? "," : "");
        json_string(s.game[g].uuid);
        printf(", \"name\": ");
        json_string(s.game[g].name);
        printf(", ");
        counts(&n);
        printf(", \"lines\": %llu, \"first_day\": ", (unsigned long long)s.game[g].lines);
        date(s.game[g].first_day);
        printf(", \"last_day\": ");
        date(s.game[g].last_day);
        VjoStatsCount last = vjo_stats_game_day(&s, (int)g, s.game[g].last_day);
        printf(", \"last\": {");
        counts(&last);
        printf("}, \"pace\": %u}", s.game[g].time.pace);
    }
    printf("],\n \"days\": [");
    int first = 1;
    for (uint32_t k = 0; k < s.days;) {
        int32_t day = s.day[k].day;
        VjoStatsCount n;
        vjo_stats_days_range(&s, day, 1, &n);
        while (k < s.days && s.day[k].day == day)
            k++;
        printf("%s\n  {\"day\": ", first ? "" : ",");
        date(day);
        printf(", ");
        counts(&n);
        printf("}");
        first = 0;
    }
    printf("]}\n");
}

/* About `bytes` of history.csv as the writer writes it (shell/history.c), from a fixed seed. */
static int synthetic(uint64_t bytes, const char *path)
{
    static const char *const kana[] = {"あ", "い", "う", "え", "お", "か", "き", "く", "け", "こ", "さ", "し", "す",
                                       "せ", "そ", "漢", "字", "読", "書", "ー"};
    static const char *const tids[] = {"PCSG00415", "PCSG00062", "PCSG00206", "PCSG00007"};
    static const char *const names[] = {"CLANNAD", "TIME TRAVELERS", "グリザイアの果実", "リトルバスターズ！"};
    FILE *f = fopen(path, "wb");
    uint64_t us = 1767225600000000ull, written = 0, x = 88172645463325252ull, day_start = us; /* 2026-01-01 */
    int game = 0;
    if (!f)
        return 2;
    written += (uint64_t)fprintf(f, "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,"
                                    "scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,"
                                    "fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,"
                                    "recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,"
                                    "capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,"
                                    "characters,line_source\n");
    while (written < bytes) {
        char text[512];
        size_t n = 0;
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        int chars = 5 + (int)(x % 56);
        n += (size_t)snprintf(text + n, sizeof(text) - n, "「");
        for (int i = 0; i < chars; i++)
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s", kana[(x >> (i % 50)) % 20]);
        n += (size_t)snprintf(text + n, sizeof(text) - n, "」");
        if (x % 11 == 0)
            game = (int)((x >> 20) % 4);
        us += 2000000ull + (x >> 8) % 118000000ull;
        if (us - day_start > 36000000000ull) { /* ten hours a day */
            day_start += 86400000000ull;
            us = day_start;
        }
        written += (uint64_t)fprintf(f, "\"vita-%s\",\"vjo-%llu-%llu\",\"%s\",\"%s\",%llu.%06u,\"vita\",\"%s\",\"vocr\","
                                        "\"VOCR5_N_w8g2.vocr\",\"%s\",1,2,3,4,5,0,0,960,544,960,544,960,0,1,1,9,1,2,3,4,"
                                        "1,0,0,0,0,0,0,0,0,0,\"640:32:120:3:41000\",%d,\"ocr\"\n",
                                     tids[game], (unsigned long long)us, (unsigned long long)written, names[game], text,
                                     (unsigned long long)(us / 1000000), (unsigned)(us % 1000000), tids[game], text,
                                     chars);
    }
    fclose(f);
    printf("%llu bytes\n", (unsigned long long)written);
    return 0;
}

int main(int argc, char **argv)
{
    int32_t tz = 0;
    size_t window = 64 * 1024, len;
    int bench = 0, i;
    const char *path = NULL;
    vjo_reading_rule = VJO_READING_ADAPTIVE;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tz") && i + 1 < argc)
            tz = (int32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rule") && i + 1 < argc)
            vjo_reading_rule = !strcmp(argv[++i], "15s") ? VJO_READING_GAPS_15S : VJO_READING_ADAPTIVE;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc)
            window = (size_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc)
            bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--synthetic") && i + 2 < argc)
            return synthetic(strtoull(argv[i + 1], NULL, 10), argv[i + 2]);
        else
            path = argv[i];
    }
    if (!path || window < 2) {
        fprintf(stderr, "usage: vjo-stats [--tz MINUTES] [--rule adaptive|15s] [--window BYTES] [--bench N] FILE\n");
        return 2;
    }
    char *file = read_all(path, &len), *buf = malloc(window + 1);
    if (!bench) {
        vjo_stats_reset(&s, tz, vjo_reading_rule);
        count(file, len, window, buf);
        report();
        return 0;
    }
    uint64_t *ns = calloc((size_t)bench, sizeof(*ns));
    for (int k = 0; k < bench; k++) {
        uint64_t t0 = now_ns();
        vjo_stats_reset(&s, tz, vjo_reading_rule);
        count(file, len, window, buf);
        ns[k] = now_ns() - t0;
    }
    qsort(ns, (size_t)bench, sizeof(*ns), cmp_u64);
    /* one record appended to the count of the rest (the cache's usual case): the whole file but its last record
     * counted, then that record */
    size_t last = len - 1;
    while (last > 0 && !(file[last] == '"' && file[last - 1] == '\n'))
        last--;
    vjo_stats_reset(&s, tz, vjo_reading_rule);
    count(file, last, window, buf);
    uint64_t t0 = now_ns();
    count(file, len, window, buf);
    uint64_t append_ns = now_ns() - t0;
    printf("{\"bytes\": %zu, \"records\": %llu, \"median_ns\": %llu, \"min_ns\": %llu, \"max_ns\": %llu, "
           "\"append_ns\": %llu, \"cache_bytes\": %zu}\n",
           len, (unsigned long long)s.records, (unsigned long long)ns[bench / 2], (unsigned long long)ns[0],
           (unsigned long long)ns[bench - 1], (unsigned long long)append_ns, vjo_stats_cache_size(&s));
    return 0;
}
