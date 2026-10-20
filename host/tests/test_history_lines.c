/* Line history's reader (core/history_lines.c): history.csv's records, as shell/history.c writes
 * them, read back from the file's last bytes, newest first, this game's only. */
#include "acutest.h"
#include <stdio.h>
#include <string.h>

#include "history_lines.h"
#include "textfilter.h"

#define UUID "vita-PCSG00415"
#define MATCH UUID "\","  /* the column as the scan matches it: its closing quote and the comma */
#define HEADER "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene\n"

static char csv[1 << 16];
static uint8_t lines_mem[16384];
static VjoArena lines;
static VjoHistory h;

/* A field as shell/history.c's field() writes it: quoted, '"' doubled. */
static void quoted(const char *s)
{
    size_t n = strlen(csv);
    csv[n++] = '"';
    for (; *s && n + 3 < sizeof(csv); s++) {
        if (*s == '"')
            csv[n++] = '"';
        csv[n++] = *s;
    }
    csv[n++] = '"';
    csv[n] = 0;
}

/* A record: the first four fields quoted, then the others the writer puts after them. */
static void record(const char *game, const char *line)
{
    static unsigned seq;
    char id[64];
    snprintf(id, sizeof(id), "vjo-%u-%zu", 1792220400u + seq++, strlen(csv));
    quoted(game);
    strcat(csv, ",");
    quoted(id);
    strcat(csv, ",");
    quoted("CLANNAD, \"Game\"\n2");
    strcat(csv, ",");
    quoted(line);
    strcat(csv, ",1792220400.250123,\"vita\",\"PCSG00415\",\"vocr\",\"VOCR5_N_w8.vocr\",");
    quoted(line); /* raw_line */
    strcat(csv, ",17\n");
}

static void reset(const char *current)
{
    csv[0] = 0;
    vjo_arena_init(&lines, lines_mem, sizeof(lines_mem));
    memset(&h, 0, sizeof(h));
    h.line[0] = current;
}

static void scan_all(int filter)
{
    vjo_history_scan(&h, &lines, csv, strlen(csv), 1, MATCH, filter);
}

static void check_lines(const char *const *want, int n)
{
    TEST_CHECK_(h.n == n, "%d lines, want %d", h.n, n);
    for (int i = 0; i < n && i < h.n; i++) {
        TEST_CHECK(!strcmp(h.line[i + 1], want[i]));
        TEST_MSG("line -%d: '%s', want '%s'", i + 1, h.line[i + 1], want[i]);
    }
}

/* The owner's example (2026-10-20): 昨日すき焼きを食べました on screen; Up shows ４７７８万 between
 * 昭和５２ and the current line. */
static void test_this_games_lines_newest_first(void)
{
    static const char *const want[] = {"４７７８万", "昭和５２", "最初の文"};
    reset("昨日すき焼きを食べました");
    strcpy(csv, HEADER);
    record(UUID, "最初の文");
    record("vita-PCSG00002", "他のゲーム");
    record(UUID, "昭和５２");
    record("psp-ULJM05000", "PSPのゲーム");
    record("vita-PCSG004150", "似たID"); /* a longer title ID: not this game */
    record(UUID, "４７７８万");
    record(UUID, "昨日すき焼きを食べました"); /* the current text, appended already: not an earlier line */
    scan_all(VJO_FILTER_LINES);
    check_lines(want, 3);
    TEST_CHECK(!strcmp(h.line[0], "昨日すき焼きを食べました"));
}

static void test_consecutive_duplicates_collapse_but_aba_stays(void)
{
    static const char *const want[] = {"B", "A", "B"};
    reset("A");
    record(UUID, "B");
    record(UUID, "A");
    record(UUID, "B"); /* the same text again after a blank screen (history.csv keeps such) */
    record(UUID, "B");
    record("vita-PCSG00002", "x"); /* another game's line between two of this game's */
    record(UUID, "B");
    record(UUID, "A");
    scan_all(VJO_FILTER_NONE);
    check_lines(want, 3);
}

/* A line newer than the current text's newest record (a refresh read it and was dropped as the visit
 * opened, the overlay keeping the text it showed) is no earlier line; an older record of the current
 * text is one, and with no record of the current text every line is kept. */
static void test_lines_newer_than_the_current_text(void)
{
    static const char *const older[] = {"X"}, *const aba[] = {"B", "A"}, *const none[] = {"B", "X"};
    reset("A");
    record(UUID, "X");
    record(UUID, "A");
    record(UUID, "B"); /* newer than the text shown */
    scan_all(VJO_FILTER_NONE);
    check_lines(older, 1);
    TEST_CHECK(lines.used <= 8); /* B's copy went with it */
    reset("A");
    record(UUID, "A");
    record(UUID, "B");
    record(UUID, "A"); /* the current text, newest: the older A is an earlier line */
    scan_all(VJO_FILTER_NONE);
    check_lines(aba, 2);
    reset("A");
    record(UUID, "X");
    record(UUID, "B");
    scan_all(VJO_FILTER_NONE);
    check_lines(none, 2);
    /* the lines dropped leave no pointer behind: past line[n] the table is NULL, as the overlay reads
     * line[n + 1] for the line before the oldest */
    reset("A");
    record(UUID, "X");
    record(UUID, "A");
    record(UUID, "B");
    record(UUID, "C");
    record(UUID, "D");
    scan_all(VJO_FILTER_NONE);
    check_lines(older, 1);
    for (int k = 2; k < VJO_HISTORY_LINES + 2; k++)
        TEST_CHECK_(h.line[k] == NULL, "line[%d] left behind", k);
}

static void test_quotes_commas_and_wrapped_rows(void)
{
    static const char *const want[] = {"「はい、\"そう\"です」\n次の行", "一行目\n二行目\n三行目"};
    reset("");
    record(UUID, "一行目\n二行目\n三行目");
    record(UUID, "「はい、\"そう\"です」\n次の行");
    scan_all(VJO_FILTER_LINES);
    check_lines(want, 2);
    /* text inside a field that looks like a record of this game is none: its '"' are doubled */
    reset("");
    record(UUID, "本物");
    record(UUID, "x\n\"" UUID "\",\"vjo-1-2\",\"n\",\"偽物\",1\n");
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 2 && !strcmp(h.line[2], "本物") && strstr(h.line[1], "偽物"));
}

static void test_filter_and_trim_as_shown(void)
{
    static const char *const want[] = {"日本語", "前の文"};
    reset("今の文");
    record(UUID, "前の文");
    record(UUID, "Latin only"); /* no Japanese: shown as nothing, so not a line */
    record(UUID, "前の文\nAUTO SKIP"); /* the same once its Latin row is dropped */
    record(UUID, "  日本語\nMENU  ");
    record(UUID, " \t今の文 "); /* the current text once trimmed */
    scan_all(VJO_FILTER_LINES);
    check_lines(want, 2);
    /* without the filter every line is kept, trimmed */
    reset("今の文");
    record(UUID, "Latin only");
    record(UUID, "  日本語\nMENU  ");
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 2 && !strcmp(h.line[1], "日本語\nMENU") && !strcmp(h.line[2], "Latin only"));
}

static void test_records_cut_by_the_window_or_an_append(void)
{
    char saved[sizeof(csv)];
    size_t cut, last;
    reset("");
    record(UUID, "一つ目");
    cut = strlen(csv);
    record(UUID, "二つ目");
    last = strlen(csv);
    record(UUID, "三つ目");
    strcpy(saved, csv);
    /* the window starts inside the first record (buf[0] the byte before it): that record is skipped */
    vjo_history_scan(&h, &lines, csv + cut - 6, strlen(csv) - cut + 6, 0, MATCH, VJO_FILTER_NONE);
    TEST_CHECK(h.n == 2 && !strcmp(h.line[1], "三つ目") && !strcmp(h.line[2], "二つ目"));
    /* the window's first byte starts a record: the byte before it (a newline) says so */
    reset("");
    strcpy(csv, saved);
    vjo_history_scan(&h, &lines, csv + cut - 1, strlen(csv) - cut + 1, 0, MATCH, VJO_FILTER_NONE);
    TEST_CHECK(h.n == 2);
    /* ...and only then: as the window's very first byte, with no byte before it, it is not */
    reset("");
    strcpy(csv, saved);
    vjo_history_scan(&h, &lines, csv + cut, strlen(csv) - cut, 0, MATCH, VJO_FILTER_NONE);
    TEST_CHECK(h.n == 1 && !strcmp(h.line[1], "三つ目"));
    /* the last record being appended: cut anywhere, in its line or before the field after it */
    {
        size_t end = strlen(saved) - 1, kept_from = 0;
        for (size_t keep = 0; last + keep <= end; keep++) {
            reset("");
            strcpy(csv, saved);
            csv[last + keep] = 0; /* the file ends there */
            scan_all(VJO_FILTER_NONE);
            const char *line = strstr(saved + last, "三つ目\",1"); /* the line's field and the comma after it */
            int complete = line && (size_t)(line - saved) + strlen("三つ目\",") <= last + keep;
            TEST_CHECK_(h.n == (complete ? 3 : 2), "cut after %zu bytes: %d lines", keep, h.n);
            TEST_CHECK(!strcmp(h.line[1], complete ? "三つ目" : "二つ目"));
            if (complete && !kept_from)
                kept_from = keep;
        }
        TEST_CHECK(kept_from > 60); /* the line is read only once its field is whole */
    }
    /* a window inside one record: none read */
    reset("");
    strcpy(csv, saved);
    vjo_history_scan(&h, &lines, csv + 3, 20, 0, MATCH, VJO_FILTER_NONE);
    TEST_CHECK(h.n == 0);
}

static void test_bounds_lines_and_memory(void)
{
    char line[64];
    reset("今");
    for (int i = 0; i < VJO_HISTORY_LINES + 10; i++) {
        snprintf(line, sizeof(line), "%d行目", i);
        record(UUID, line);
    }
    scan_all(VJO_FILTER_LINES);
    TEST_CHECK(h.n == VJO_HISTORY_LINES && h.line[VJO_HISTORY_LINES + 1] == NULL);
    snprintf(line, sizeof(line), "%d行目", 9 + VJO_HISTORY_LINES);
    TEST_CHECK(!strcmp(h.line[1], line));
    snprintf(line, sizeof(line), "%d行目", 10);
    TEST_CHECK(!strcmp(h.line[VJO_HISTORY_LINES], line));
    for (int k = 1; k <= h.n; k++) /* each line at its own length, in the arena */
        TEST_CHECK((const uint8_t *)h.line[k] >= lines_mem && (const uint8_t *)h.line[k] < lines_mem + lines.used);
    TEST_CHECK(lines.used < 32 * 16 + 64);
    /* the lines' arena full: the list ends at the last line that fits, without a gap */
    for (int filter = 0; filter < 2; filter++) {
        reset("今");
        vjo_arena_init(&lines, lines_mem, 100);
        record(UUID, "古い");
        record(UUID, "とても長い行とても長い行とても長い行とても長い行とても長い行とても長い行とても長い行");
        record(UUID, "新しい");
        scan_all(filter ? VJO_FILTER_LINES : VJO_FILTER_NONE);
        TEST_CHECK_(h.n == 1 && !strcmp(h.line[1], "新しい"), "filter %d: %d lines", filter, h.n);
        TEST_CHECK(lines.used <= 16);
    }
    /* the longest worker text, all quotes (doubled in the file) */
    {
        static char longest[4096];
        memset(longest, '"', sizeof(longest) - 1);
        reset("");
        record(UUID, longest);
        scan_all(VJO_FILTER_NONE);
        TEST_CHECK(h.n == 1 && strlen(h.line[1]) == sizeof(longest) - 1 && h.line[1][0] == '"');
    }
}

static void test_a_file_without_header_or_records(void)
{
    reset("");
    record(UUID, "最初"); /* at the file's first byte */
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 1);
    /* a record with more columns after the ones read (a later writer's, e.g. a characters count at the
     * end): the same line, as only the first four columns are read (the scan unescapes in place: a
     * file of its own) */
    {
        size_t n;
        reset("");
        record(UUID, "最初");
        n = strlen(csv);
        TEST_ASSERT(n > 4 && csv[n - 1] == '\n');
        strcpy(csv + n - 1, ",4,\"x\"\n");
        record(UUID, "次");
        n = strlen(csv);
        strcpy(csv + n - 1, ",1\n");
        scan_all(VJO_FILTER_NONE);
        TEST_CHECK(h.n == 2 && !strcmp(h.line[1], "次") && !strcmp(h.line[2], "最初"));
    }
    reset("");
    strcpy(csv, HEADER);
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 0);
    reset("");
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 0);
}

TEST_LIST = {
    {"this game's lines, newest first, the current text not again", test_this_games_lines_newest_first},
    {"consecutive duplicates collapse, A/B/A stays", test_consecutive_duplicates_collapse_but_aba_stays},
    {"lines newer than the current text are no earlier lines", test_lines_newer_than_the_current_text},
    {"quotes, commas and a textbox's rows", test_quotes_commas_and_wrapped_rows},
    {"filtered and trimmed as the overlay shows a text", test_filter_and_trim_as_shown},
    {"records cut by the window or an append are skipped", test_records_cut_by_the_window_or_an_append},
    {"at most VJO_HISTORY_LINES, and only what fits", test_bounds_lines_and_memory},
    {"a file with no header, or no record", test_a_file_without_header_or_records},
    {NULL, NULL}
};
