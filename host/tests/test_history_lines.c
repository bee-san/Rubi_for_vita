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
    /* A, a blank screen, A again: the writer keeps both A records (the blank reset its fingerprint), and
     * with A the current text both are it: the newest is the current text, the older one the line after
     * nothing kept yet, so a duplicate of it: neither is an earlier line */
    {
        static const char *const want[] = {"X"};
        reset("A");
        record(UUID, "X");
        record(UUID, "A");
        record(UUID, "A");
        scan_all(VJO_FILTER_NONE);
        check_lines(want, 1);
    }
    /* The rule's documented cost (docs/line-history.md): the current text with no record of its own
     * and an older record of the same text: the lines after that record are taken for newer ones */
    {
        static const char *const want[] = {"X"};
        reset("うん");
        record(UUID, "X");
        record(UUID, "うん");
        record(UUID, "B"); /* the line before the current うん, which was not appended */
        scan_all(VJO_FILTER_NONE);
        check_lines(want, 1);
    }
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
            const char *line = strstr(saved + last, "三つ目\",1"); /* the line's field, the comma, the time's digit */
            int complete = line && (size_t)(line - saved) + strlen("三つ目\",1") <= last + keep;
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

/* A power loss (or a crash) cuts a record anywhere, and the next boot's append follows right after
 * the cut, with no newline between (docs/ocr-history.md: an incomplete final record). Whatever the
 * cut, only real lines are shown, newest first, and the line before the damage is kept: never another
 * field's text (the game's name) or bytes of the two records run together (audit-line-history F1). */
static void test_a_cut_record_and_the_next_append(void)
{
    static const char *const real[] = {"三つ目", "二つ目\"です\",ね", "一つ目"}; /* C, B, A: newest first */
    char b[2048];
    size_t a_end, b_len, cases = 0, c_kept = 0;
    reset("");
    record(UUID, "一つ目");
    a_end = strlen(csv);
    record(UUID, real[1]);
    b_len = strlen(csv) - a_end;
    TEST_ASSERT(b_len < sizeof(b));
    memcpy(b, csv + a_end, b_len);
    for (size_t k = 0; k < b_len; k++, cases++) {
        int last = -1;
        reset("今");
        record(UUID, "一つ目");
        memcpy(csv + a_end, b, k); /* B cut after k bytes */
        csv[a_end + k] = 0;
        record(UUID, real[0]); /* C, appended at the cut */
        scan_all(VJO_FILTER_NONE);
        TEST_CHECK_(h.n >= 1 && !strcmp(h.line[h.n], "一つ目"), "cut at %zu: %d lines, the oldest '%s'", k, h.n,
                    h.n ? h.line[h.n] : "");
        for (int n = 1; n <= h.n; n++) {
            int which = -1;
            for (int w = 0; w < 3; w++)
                if (!strcmp(h.line[n], real[w]))
                    which = w;
            TEST_CHECK_(which > last, "cut at %zu: line -%d '%s' is no real line, or out of order", k, n, h.line[n]);
            last = which;
        }
        c_kept += h.n && !strcmp(h.line[1], real[0]);
    }
    TEST_MSG("%zu cuts, the next append's line kept after %zu of them", cases, c_kept);
    TEST_CHECK(cases > 100 && c_kept >= 1); /* (kept when the cut ends after a newline inside a field) */
    /* another damage than a cut (a byte of the medium): a separator that is no comma makes the record
     * one to skip, as a reader cannot tell which field it stood for */
    reset("今");
    record(UUID, "一つ目");
    a_end = strlen(csv);
    record(UUID, "壊れた");
    {
        char *sep = strstr(csv + a_end, "\",\"CLANNAD");
        TEST_ASSERT(sep != NULL);
        sep[1] = 'X'; /* the comma between the id and the name */
    }
    record(UUID, "三つ目");
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 2 && !strcmp(h.line[1], "三つ目") && !strcmp(h.line[2], "一つ目"));
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

static void test_saved_engine_follows_its_line(void)
{
    reset("今");
    record(UUID, "古いOCR");
    record(UUID, "フック");
    char *engine = strstr(csv, "\"フック\",1792220400.250123,\"vita\",\"PCSG00415\",\"vocr\"");
    TEST_ASSERT(engine != NULL);
    engine = strstr(engine, "\"vocr\"");
    memcpy(engine + 1, "hook", 4);
    record(UUID, "今");
    scan_all(VJO_FILTER_NONE);
    TEST_ASSERT(h.n == 2);
    TEST_CHECK(!strcmp(h.line[1], "フック") && h.origin[1] == VJO_LINE_HOOK);
    TEST_CHECK(!strcmp(h.line[2], "古いOCR") && h.origin[2] == VJO_LINE_OCR);

    reset("今");
    strcpy(csv, "\"" UUID "\",\"id\",\"game\",\"昔\",1\n");
    scan_all(VJO_FILTER_NONE);
    TEST_CHECK(h.n == 1 && h.origin[1] == VJO_LINE_UNKNOWN);
}

/* ---- the total's count (docs/ocr-history.md#total-characters-read): every game's lines, in windows ---- */

static uint32_t count(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last)
{
    static char copy[sizeof(csv)];
    memcpy(copy, buf, len); /* the count unescapes in place: count a copy, so csv stays as written */
    return vjo_history_chars(copy, len, whole, cut, first, last);
}

static void test_count_every_games_lines(void)
{
    size_t first, last;
    reset("");
    strcpy(csv, HEADER);
    record(UUID, "「また明日、一緒に見に行こう!」");     /* 12 */
    record("vita-PCSG00002", "10時に駅で待ってるね。"); /* 11: another game's count too */
    record("psp-ULJM05000", "\"引用\",と、\n改行AB");  /* 7: quotes, a comma, a wrapped row */
    record(UUID, "♪〜♪ 😀 𠮟");                        /* 1: 𠮟 only */
    size_t len = strlen(csv);
    TEST_CHECK(count(csv, len, 1, 0, &first, &last) == 12 + 11 + 7 + 1);
    TEST_CHECK(first == strlen(HEADER) && last < len && csv[last] == '"' && csv[last - 1] == '\n');
    /* cut: the newest is left for the next window */
    TEST_CHECK(count(csv, len, 1, 1, &first, &last) == 12 + 11 + 7);
    /* the bytes before a record: buf[0] the byte before the window, not read as a record's first */
    TEST_CHECK(count(csv + first - 1, len - first + 1, 0, 0, &first, &last) == 31 && first == 1);
    /* nothing but the header, or nothing at all */
    TEST_CHECK(count(csv, strlen(HEADER), 1, 0, &first, &last) == 0 && first == strlen(HEADER) &&
               last == strlen(HEADER));
    TEST_CHECK(count(csv, 0, 1, 0, &first, &last) == 0 && first == 0 && last == 0);
}

/* The windows a count reads (shell/history.c vjo_history_total_step): each from the newest record start of
 * the one before, with the byte before it; together they count every record once, whatever the size */
static void test_count_in_windows(void)
{
    reset("");
    strcpy(csv, HEADER);
    uint32_t want = 0;
    for (int i = 0; i < 300; i++) {
        char line[64];
        snprintf(line, sizeof(line), i % 3 ? "行%d「台詞」\nAB" : "\"%d\",と言った。", i);
        record(i % 2 ? UUID : "vita-PCSG00002", line);
        want += vjo_line_characters(line, strlen(line));
    }
    size_t len = strlen(csv);
    for (size_t window = 700; window <= 9000; window += 1650) {
        uint32_t got = 0;
        size_t at = 0, first, last;
        while (at < len) {
            size_t to = len - at > window ? at + window : len, base = at ? at - 1 : 0;
            int cut = to < len;
            got += count(csv + base, to - base, !at, cut, &first, &last);
            if (!cut)
                break;
            at = last < to - base && base + last > at ? base + last : to;
        }
        TEST_CHECK(got == want);
        TEST_MSG("%zu B windows: %u, want %u", window, got, want);
    }
}

/* What a count does not take for a record: a line cut before the time after it, a quote inside a field, a
 * record run on from a cut one without a line break */
static void test_count_cut_and_damaged_records(void)
{
    size_t first, last;
    reset("");
    strcpy(csv, HEADER);
    record(UUID, "一二三");
    strcat(csv, "\"" UUID "\",\"vjo-1-2\",\"game\",\"切れた行"); /* cut inside its line */
    record(UUID, "四五");                                         /* runs on from it: no record start */
    strcat(csv, "\"" UUID "\",\"vjo-1-2\",\"game\",\"時刻なし\",x\n"); /* no time after its line */
    record(UUID, "六");
    TEST_CHECK(count(csv, strlen(csv), 1, 0, &first, &last) == 3 + 1);
}

TEST_LIST = {
    {"saved engine follows its line, missing metadata stays unknown", test_saved_engine_follows_its_line},
    {"this game's lines, newest first, the current text not again", test_this_games_lines_newest_first},
    {"consecutive duplicates collapse, A/B/A stays", test_consecutive_duplicates_collapse_but_aba_stays},
    {"lines newer than the current text are no earlier lines", test_lines_newer_than_the_current_text},
    {"quotes, commas and a textbox's rows", test_quotes_commas_and_wrapped_rows},
    {"filtered and trimmed as the overlay shows a text", test_filter_and_trim_as_shown},
    {"records cut by the window or an append are skipped", test_records_cut_by_the_window_or_an_append},
    {"a record cut by a power loss and the next append: no garbage", test_a_cut_record_and_the_next_append},
    {"at most VJO_HISTORY_LINES, and only what fits", test_bounds_lines_and_memory},
    {"a file with no header, or no record", test_a_file_without_header_or_records},
    {"total: every game's lines, the newest left for the next window", test_count_every_games_lines},
    {"total: windows of any size count every record once", test_count_in_windows},
    {"total: cut and damaged records", test_count_cut_and_damaged_records},
    {NULL, NULL}
};
