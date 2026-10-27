/* The Look up page's earlier lines (settings/lookup_history.c, #313): history.csv read back from its end, a window
 * at a time, on real files through the Vita file API mapped onto POSIX. The games, most recent first; the newest
 * game's lines, newest first, as the overlay's line history keeps them (filtered, trimmed, none empty, none the same
 * as the one after it), at most 200; windows of every size, the scan's 8 MiB, a chosen game, a file that changed;
 * a line read again whole, with what its card records. */
#define main acutest_main
#include "acutest.h"
#undef main
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include "test_tmpdir.h"
#include "../../settings/lookup_history.h"
#include "textfilter.h"

static uint64_t clock_us;
static unsigned clock_step;
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us += clock_step; }
static int opens, preads;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = open(p, flags, mode);
    if (fd < 0)
        return (int)(0x80010000u | (unsigned)errno);
    opens++;
    return fd;
}
int sceIoClose(int fd) { opens--; return close(fd); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoPread(int fd, void *p, size_t n, SceOff off)
{
    preads++;
    return (int)pread(fd, p, n > 5000 ? 5000 : n, off); /* short reads: read again */
}
static char logs[2048];
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
}

/* ---------------- history.csv, as shell/history.c writes it ---------------- */

#define HEADER "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene\n"
static FILE *csv;
static uint64_t csv_us = 1791790200000000ull; /* 2026-10-12 */

static void field(const char *s)
{
    fputc('"', csv);
    for (; *s; s++) {
        if (*s == '"')
            fputc('"', csv);
        fputc(*s, csv);
    }
    fputc('"', csv);
}

/* A record of game (platform-title ID, its name), its line, by OCR or a hook; `pad` bytes of padding in its raw line
 * (records of every size). */
static void record_pad(const char *platform, const char *tid, const char *name, const char *line, int hook, int pad)
{
    char uuid[32], id[64];
    snprintf(uuid, sizeof(uuid), "%s-%s", platform, tid);
    csv_us += 3000000;
    snprintf(id, sizeof(id), "vjo-%llu-%ld", (unsigned long long)csv_us, ftell(csv));
    field(uuid);
    fputc(',', csv);
    field(id);
    fputc(',', csv);
    field(name);
    fputc(',', csv);
    field(line);
    fprintf(csv, ",%llu.%06u,", (unsigned long long)(csv_us / 1000000), (unsigned)(csv_us % 1000000));
    field(platform);
    fputc(',', csv);
    field(tid);
    fputs(hook ? ",\"hook\"," : ",\"vocr\",", csv);
    field("VOCR5_N_w8.vocr");
    fputs(",\"", csv);
    for (int i = 0; i < pad; i++)
        fputc('x', csv);
    fputs("\",17\n", csv);
}
static void record(const char *game, const char *line) /* "CLANNAD" (vita-PCSG00415) or "AIR" (psp-ULJM05000) */
{
    int clannad = !strcmp(game, "CLANNAD");
    record_pad(clannad ? "vita" : "psp", clannad ? "PCSG00415" : "ULJM05000", game, line, 0, 0);
}
static void new_file(void)
{
    csv = fopen(VJO_LOOKUP_HISTORY_PATH, "wb");
    TEST_ASSERT(csv != NULL);
    fputs(HEADER, csv);
}
static void close_file(void)
{
    TEST_ASSERT(fclose(csv) == 0);
}
static void reset(void)
{
    TEST_ASSERT(vjo_test_remove_tree("ux0:data") == 0);
    TEST_ASSERT(mkdir("ux0:data", 0777) == 0 && mkdir("ux0:data/VitaJPOverlay", 0777) == 0);
    clock_step = 0;
    logs[0] = '\0';
}
static void scan(void)
{
    vjo_lookup_history_open(VJO_FILTER_LINES);
    while (vjo_lookup_history_step(12000))
        ;
}
static void check_lines(const char *const *want, int n)
{
    const VjoLookupLine *lines;
    int got = vjo_lookup_history_lines(&lines);
    TEST_CHECK_(got == n, "%d lines, want %d", got, n);
    for (int i = 0; i < n && i < got; i++)
        TEST_CHECK_(!strcmp(lines[i].preview, want[i]), "line %d: '%s', want '%s'", i, lines[i].preview, want[i]);
}

static void the_newest_games_lines_newest_first(void)
{
    static const char *const clannad[] = {"昨日すき焼きを食べました", "名前 「台詞だ」", "猫", "犬", "猫", "最初の文"};
    static const char *const air[] = {"夏の日", "空"};
    const VjoLookupGame *games;
    reset();
    /* none yet */
    vjo_lookup_history_open(VJO_FILTER_LINES);
    TEST_CHECK(vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_NONE && vjo_lookup_history_error() == (int)0x80010002u);
    TEST_CHECK(!vjo_lookup_history_step(12000) && vjo_lookup_history_game() == -1);
    new_file();
    record("CLANNAD", "最初の文");
    record("AIR", "空");
    record("CLANNAD", "猫");
    record("CLANNAD", "犬");
    record("CLANNAD", "猫");
    record("CLANNAD", "猫"); /* the same again (after a blank screen, history.csv keeps such): once */
    record("AIR", "夏の日");
    record("CLANNAD", "English only"); /* no Japanese: not shown (non_japanese_filter = lines) */
    record("CLANNAD", "  ");
    record("CLANNAD", "名前\n「台詞だ」");
    record("CLANNAD", "  昨日すき焼きを食べました  ");
    close_file();
    scan();
    TEST_CHECK(vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_READ && !opens);
    TEST_CHECK(vjo_lookup_history_games(&games) == 2 && !strcmp(games[0].uuid, "vita-PCSG00415") &&
               !strcmp(games[0].name, "CLANNAD") && !strcmp(games[1].uuid, "psp-ULJM05000"));
    TEST_CHECK(vjo_lookup_history_game() == 0);
    check_lines(clannad, 6);
    const VjoLookupLine *lines;
    vjo_lookup_history_lines(&lines);
    TEST_CHECK(lines[0].time_us == csv_us && lines[1].time_us == csv_us - 3000000 && lines[0].offset > lines[1].offset);
    /* the other game */
    vjo_lookup_history_select(1);
    TEST_CHECK(vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_READING);
    while (vjo_lookup_history_step(12000))
        ;
    TEST_CHECK(vjo_lookup_history_game() == 1);
    check_lines(air, 2);
    vjo_lookup_history_select(5); /* no such game: nothing changes */
    TEST_CHECK(vjo_lookup_history_game() == 1 && vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_READ);
}

/* A line read again whole: its text as history.csv has it (not filtered: the page filters it), and what a card
 * from it records; a file that changed meanwhile gives none. */
static void a_line_read_again_whole(void)
{
    char text[VJO_LOOKUP_TEXT_MAX + 1];
    VjoLookupSource src;
    reset();
    new_file();
    record("CLANNAD", "最初の文");
    record_pad("vita", "PCSG00415", "CLANNAD", "Name\n「猫が\"鳴いた\"」", 1, 0);
    close_file();
    scan();
    TEST_ASSERT(vjo_lookup_history_line(0, text, sizeof(text), &src) == 0);
    TEST_CHECK(!strcmp(text, "Name\n「猫が\"鳴いた\"」") && !src.typed);
    TEST_CHECK(!strcmp(src.game, "CLANNAD") && !strcmp(src.title_id, "PCSG00415") && !strcmp(src.platform, "vita"));
    TEST_CHECK(!strcmp(src.engine, "hook") && !strcmp(src.model, "VOCR5_N_w8.vocr") && src.time_us == csv_us);
    TEST_ASSERT(vjo_lookup_history_line(1, text, sizeof(text), &src) == 0);
    TEST_CHECK(!strcmp(text, "最初の文") && !strcmp(src.engine, "vocr"));
    TEST_CHECK(vjo_lookup_history_line(2, text, sizeof(text), &src) == -1 && vjo_lookup_history_line(-1, text, 8, &src) == -1);
    /* a short buffer: cut on a character */
    TEST_ASSERT(vjo_lookup_history_line(1, text, 5, &src) == 0);
    TEST_CHECK(!strcmp(text, "最"));
    /* the file replaced by another: the record is not there any more */
    new_file();
    record("AIR", "まったく別の行です、ずっと長い");
    close_file();
    TEST_CHECK(vjo_lookup_history_line(0, text, sizeof(text), &src) == -1 && !opens);
}

/* At most 200 lines, the newest; and windows of every size: a record that runs across a window's start is read
 * whole in the next window; every record found once. */
static void two_hundred_lines_across_windows(void)
{
    const VjoLookupLine *lines;
    char line[64], want[64];
    reset();
    new_file();
    for (int i = 0; i < 260; i++) {
        snprintf(line, sizeof(line), "行%03d", i);
        record_pad("vita", "PCSG00415", "CLANNAD", line, 0, (i * 7919) % 9000); /* up to 9 KB a record */
        if (i % 13 == 0)
            record("AIR", "空");
    }
    close_file();
    clock_step = 5000; /* a step's 12 ms: a window or three */
    vjo_lookup_history_open(VJO_FILTER_LINES);
    int steps = 0;
    while (vjo_lookup_history_step(12000))
        steps++;
    TEST_CHECK_(steps >= 4, "%d steps", steps);
    TEST_ASSERT(vjo_lookup_history_lines(&lines) == VJO_LOOKUP_LINES);
    for (int i = 0; i < VJO_LOOKUP_LINES; i++) {
        snprintf(want, sizeof(want), "行%03d", 259 - i);
        TEST_CHECK_(!strcmp(lines[i].preview, want), "line %d: '%s', want '%s'", i, lines[i].preview, want);
    }
    const VjoLookupGame *games;
    TEST_CHECK(vjo_lookup_history_games(&games) == 2 && vjo_lookup_history_read() == vjo_lookup_history_size());
    /* the oldest kept, read again whole */
    char text[VJO_LOOKUP_TEXT_MAX + 1];
    VjoLookupSource src;
    TEST_CHECK(vjo_lookup_history_line(VJO_LOOKUP_LINES - 1, text, sizeof(text), &src) == 0 && !strcmp(text, "行060"));
}

/* The scan reads back 8 MiB at most: a game whose lines are all before that is not listed. A file that grew is read
 * again, the chosen game kept; one whose chosen game is gone, the newest game's. */
static void the_scan_s_reach_and_a_file_that_changed(void)
{
    const VjoLookupGame *games;
    reset();
    new_file();
    record_pad("psp", "ULJM05000", "AIR", "古いゲーム", 0, 0);
    for (int i = 0; ftell(csv) < (long)VJO_LOOKUP_SCAN_MAX + 200000; i++)
        record_pad("vita", "PCSG00415", "CLANNAD", "長い", 0, 8000);
    record("CLANNAD", "最後");
    close_file();
    scan();
    TEST_CHECK(vjo_lookup_history_games(&games) == 1 && !strcmp(games[0].name, "CLANNAD"));
    TEST_CHECK(vjo_lookup_history_read() <= VJO_LOOKUP_SCAN_MAX);
    /* the same file: nothing read again */
    int before = preads;
    vjo_lookup_history_open(VJO_FILTER_LINES);
    TEST_CHECK(preads == before && vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_READ);
    /* a small file of two games, the second chosen; then a line appended: read again, the choice kept */
    reset();
    new_file();
    record("CLANNAD", "一");
    record("AIR", "二");
    close_file();
    scan();
    TEST_CHECK(vjo_lookup_history_games(&games) == 2 && vjo_lookup_history_game() == 0 &&
               !strcmp(games[0].name, "AIR"));
    vjo_lookup_history_select(1);
    while (vjo_lookup_history_step(12000))
        ;
    TEST_CHECK(!strcmp(games[vjo_lookup_history_game()].name, "CLANNAD"));
    csv = fopen(VJO_LOOKUP_HISTORY_PATH, "ab");
    record("AIR", "三");
    close_file();
    scan();
    TEST_CHECK(!strcmp(games[vjo_lookup_history_game()].name, "CLANNAD"));
    static const char *const one[] = {"一"};
    check_lines(one, 1);
    /* replaced by a file without that game: the newest game's lines */
    new_file();
    record("AIR", "四");
    close_file();
    scan();
    static const char *const four[] = {"四"};
    TEST_CHECK(vjo_lookup_history_game() == 0 && !strcmp(games[0].name, "AIR"));
    check_lines(four, 1);
    /* gone: none */
    TEST_ASSERT(unlink(VJO_LOOKUP_HISTORY_PATH) == 0);
    vjo_lookup_history_open(VJO_FILTER_LINES);
    TEST_CHECK(vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_NONE);
}

/* Damaged records (a power loss's cut record, run into the next boot's append) are skipped, as the Shell skips
 * them; the header line is no record. */
static void damaged_records_are_skipped(void)
{
    static const char *const want[] = {"次の起動", "前の行"};
    reset();
    new_file();
    record("CLANNAD", "前の行");
    fputs("\"vita-PCSG00415\",\"vjo-1-2\",\"CLANNAD\",\"切れた", csv); /* cut: no line break */
    record("CLANNAD", "消えた"); /* run on from it: one damaged record with the cut one */
    record("CLANNAD", "次の起動");
    close_file();
    scan();
    check_lines(want, 2);
}

TEST_LIST = {
    {"the newest game's lines, newest first", the_newest_games_lines_newest_first},
    {"a line read again whole, with its card's facts", a_line_read_again_whole},
    {"200 lines, across windows of every size", two_hundred_lines_across_windows},
    {"the scan's reach, and a file that changed", the_scan_s_reach_and_a_file_that_changed},
    {"damaged records are skipped", damaged_records_are_skipped},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("lookup-history");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
