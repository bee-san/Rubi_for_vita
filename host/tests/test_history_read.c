/* The Rubi app's history.csv reader (core/history_read.c: the Look up page's earlier lines, #313, and the Stats
 * page, #312) against the Shell's own (core/history_lines.c), which keeps its rules private and must not change.
 * On random files written as shell/history.c writes them, with the damage a power loss leaves (a record cut, zeros,
 * the next boot's record run on from it) and with the times the Shell refuses, read whole and in windows: both find
 * the same records, the same lines and times, and give the same totals and reading time (the Triangle view's);
 * and newest first, the same game's lines as the overlay's line history. Then the row's cells, one by one. */
#include "acutest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "history_lines.h"
#include "history_read.h"
#include "textfilter.h"
#include "utf.h"

/* ---- random files, as shell/history.c writes them ---- */

static uint32_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static int pick(int n) { return (int)(rnd() % (uint32_t)n); }

static char file[1 << 18];
static size_t flen;
static void put(const char *s, size_t n)
{
    if (flen + n < sizeof(file)) {
        memcpy(file + flen, s, n);
        flen += n;
    }
}
static void puts_(const char *s) { put(s, strlen(s)); }
/* A field as shell/history.c's field() writes it: quoted, '"' doubled. */
static void field(const char *s, size_t n)
{
    put("\"", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"')
            put("\"", 1);
        put(s + i, 1);
    }
    put("\"", 1);
}

#define GAMES 4
static const struct { const char *platform, *tid, *name; } games[GAMES] = {
    {"vita", "PCSG00415", "CLANNAD"},
    {"psp", "ULJM05000", "PSP \"Game\", 2\nnd line"},
    {"vita", "PCSG00002", ""}, /* no name: the writer writes its ID */
    {"vita", "PCSG004150", "似たID"}, /* a title ID that starts as another's */
};
static const char *const pieces[] = {
    "日本語の文", "「はい」", "、", "。", "ABC", "x", "\"", ",", "\n", " ", "　", "漢字かな", "ー", "１２３",
    "「", "」", "\"\"", ",\"", "\n\"", "\"\n", "\",", "\r\n", "ｶﾀｶﾅ", "😀", "a\"b,c", "\n\n",
};
static uint64_t clock_us;

/* The line, written into text (its bytes: at most cap). */
static size_t random_line(char *text, size_t cap)
{
    size_t n = 0;
    for (int k = pick(9); k > 0; k--) {
        const char *s = pieces[pick((int)(sizeof(pieces) / sizeof(pieces[0])))];
        size_t m = strlen(s);
        if (n + m >= cap)
            break;
        memcpy(text + n, s, m);
        n += m;
    }
    return n;
}

/* One record: the writer's 43 cells (41 or 42 as older versions wrote), its time valid or one the Shell refuses,
 * now and then damaged. */
static void record(void)
{
    char text[256], cell[96];
    size_t start = flen, n = random_line(text, sizeof(text));
    int g = pick(GAMES), version = pick(4), time = pick(24), cells = version == 0 ? 41 : version == 1 ? 42 : 43;
    clock_us += (uint64_t)pick(40000000) + 1;
    put("\"", 1);
    puts_(games[g].platform);
    put("-", 1);
    puts_(games[g].tid);
    puts_("\",\"");
    snprintf(cell, sizeof(cell), "vjo-%llu-%zu", (unsigned long long)clock_us, flen);
    puts_(cell);
    puts_("\",");
    if (games[g].name[0])
        field(games[g].name, strlen(games[g].name));
    else
        field(games[g].tid, strlen(games[g].tid));
    put(",", 1);
    field(text, n);
    /* the time: the writer's ",%llu.%06u,", or one the Shell reads as 0 (or as damage: a quoted one) */
    unsigned long long s = (unsigned long long)(clock_us / 1000000u);
    unsigned frac = (unsigned)(clock_us % 1000000u);
    switch (time) {
    case 0: snprintf(cell, sizeof(cell), ",1234567890123.%06u,", frac); break;     /* 13 digits */
    case 1: snprintf(cell, sizeof(cell), ",%llu,", s); break;                      /* no fraction */
    case 2: snprintf(cell, sizeof(cell), ",%llu.%05u,", s, frac / 10); break;     /* five digits */
    case 3: snprintf(cell, sizeof(cell), ",%llu.%07u,", s, frac * 10); break;     /* seven */
    case 4: snprintf(cell, sizeof(cell), ",%llu.%06ux,", s, frac); break;         /* not followed by ',' */
    case 5: snprintf(cell, sizeof(cell), ",\"%llu.%06u\",", s, frac); break;      /* quoted: damaged */
    case 6: snprintf(cell, sizeof(cell), ",999999999999.999999,"); break;         /* the 12 digits' end */
    case 7: snprintf(cell, sizeof(cell), ",0.000000,"); break;                     /* 0: no time */
    default: snprintf(cell, sizeof(cell), ",%llu.%06u,", s, frac); break;
    }
    puts_(cell);
    field(games[g].platform, strlen(games[g].platform));
    put(",", 1);
    field(games[g].tid, strlen(games[g].tid));
    puts_(pick(3) ? ",\"vocr\"," : ",\"hook\",");
    field("VOCR5_N_w8.vocr", 15);
    put(",", 1);
    field(text, n); /* raw_line */
    for (int k = 10; k < 40; k++) {
        snprintf(cell, sizeof(cell), ",%u", (unsigned)rnd() % 100000u);
        puts_(cell);
    }
    puts_(",\"12:34:56:7:890|1:2:3:4:5\"");
    if (cells >= 42) {
        snprintf(cell, sizeof(cell), ",%u", vjo_line_characters(text, n));
        puts_(cell);
    }
    if (cells >= 43)
        puts_(pick(2) ? ",\"ocr\"" : ",\"hook\"");
    puts_(pick(12) ? "\n" : "\r\n");
    /* a power loss: the record cut, maybe zeros after it, and the next boot's record run on from there */
    if (!pick(25) && flen > start + 1) {
        flen = start + 1 + (size_t)pick((int)(flen - start - 1));
        for (int z = pick(4); z > 0; z--)
            put("\0", 1);
    }
}

static const char *const headers[] = {
    "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,monotonic_us,"
    "elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,"
    "find_us,load_us,recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,capture_bytes,strip_rows,"
    "workspace_bytes,resident_bytes,fallback,line_metrics,characters,line_source\n",
    "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,monotonic_us,"
    "elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,"
    "find_us,load_us,recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,capture_bytes,strip_rows,"
    "workspace_bytes,resident_bytes,fallback,line_metrics\n", /* 0.8.0's */
    "uuid,given_ide", /* cut by a power loss in the first append: the record after it on its line */
    "",
};

static void random_file(int records)
{
    flen = 0;
    clock_us = 1791790200000000ull + (uint64_t)pick(1000000000);
    puts_(headers[pick(4)]);
    for (int i = 0; i < records; i++)
        record();
}

/* ---- the totals: the Shell's count against one made from the reader's rows ---- */

typedef struct {
    uint32_t chars;
    size_t first, last;
    VjoReadingTime time;
    VjoReadingScope scope;
} Count;

static int same_count(const Count *a, const Count *b)
{
    return a->chars == b->chars && a->first == b->first && a->last == b->last &&
           !memcmp(&a->time, &b->time, sizeof(a->time)) && a->scope.chars == b->scope.chars &&
           a->scope.active_us == b->scope.active_us && a->scope.timed_chars == b->scope.timed_chars;
}

/* core/history_lines.c's count of a window (vjo_history_count_scope), on its own copy. */
static Count shell_count(const char *buf, size_t len, int whole, int cut, const VjoReadingScope *scope)
{
    static char copy[sizeof(file)];
    Count c;
    memset(&c, 0, sizeof(c));
    c.scope = *scope;
    memcpy(copy, buf, len);
    c.chars = vjo_history_count_scope(copy, len, whole, cut, &c.first, &c.last, &c.time, &c.scope);
    return c;
}

/* The same from the reader: each record from its start to the next one's (the newest left out when the window's end
 * may cut it), its line counted and its time added as the Shell's count does, a damaged one breaking the time. */
static Count reader_count(const char *buf, size_t len, int whole, int cut, const VjoReadingScope *scope)
{
    static char copy[sizeof(file) + 1];
    Count c;
    VjoHistoryRow row;
    memset(&c, 0, sizeof(c));
    c.scope = *scope;
    c.first = c.last = len;
    memcpy(copy, buf, len);
    copy[len] = 0x7F; /* the byte past the window: never written */
    for (size_t s = vjo_history_next_start(copy, 0, len, whole); s < len;) {
        size_t e = vjo_history_next_start(copy, s + 1, len, whole);
        if (c.first == len)
            c.first = s;
        c.last = s;
        if (e == len && cut)
            break;
        if (!vjo_history_row(copy + s, copy + e, 5, &row)) {
            vjo_reading_scope_add(&c.time, &c.scope, row.time_us, vjo_history_game(row.uuid, row.len[0]),
                                  vjo_line_characters(row.line, row.len[3]));
            c.chars += c.time.last_chars;
        } else {
            c.time.last_us = 0; /* damaged records break the interval */
        }
        s = e;
    }
    TEST_CHECK(copy[len] == 0x7F);
    return c;
}

static void check_counts(const char *buf, size_t len, int whole, int cut, const VjoReadingScope *scope, int file_no)
{
    Count a = shell_count(buf, len, whole, cut, scope), b = reader_count(buf, len, whole, cut, scope);
    TEST_CHECK_(same_count(&a, &b),
                "file %d (%zu B, whole %d, cut %d, scope '%s'): the Shell counts %u chars, first %zu, last %zu, "
                "%llu us over %llu chars; the reader %u, %zu, %zu, %llu us over %llu", file_no, len, whole, cut,
                scope->uuid, a.chars, a.first, a.last, (unsigned long long)a.time.active_us,
                (unsigned long long)a.time.timed_chars, b.chars, b.first, b.last,
                (unsigned long long)b.time.active_us, (unsigned long long)b.time.timed_chars);
}

/* ---- newest first: the Shell's line history against the reader's walk back ---- */

static void check_scan(const char *buf, size_t len, int whole, int g, int filter, int file_no)
{
    static char shell_copy[sizeof(file)], reader_copy[sizeof(file) + 1];
    static uint8_t shell_mem[1 << 16], reader_mem[1 << 16];
    char match[48], uuid[32];
    VjoHistory h;
    VjoArena shell_lines, reader_lines;
    const char *kept[VJO_HISTORY_LINES + 1];
    int n = 0;
    snprintf(uuid, sizeof(uuid), "%s-%s", games[g].platform, games[g].tid);
    snprintf(match, sizeof(match), "%s\",", uuid);
    memset(&h, 0, sizeof(h));
    h.line[0] = "\x01 the current text, in no record";
    vjo_arena_init(&shell_lines, shell_mem, sizeof(shell_mem));
    memcpy(shell_copy, buf, len);
    vjo_history_scan(&h, &shell_lines, shell_copy, len, whole, match, filter);
    /* the reader: each record back from the window's end, this game's, its line as the overlay shows it (filtered,
     * trimmed), none empty and none the same as the one kept after it */
    vjo_arena_init(&reader_lines, reader_mem, sizeof(reader_mem));
    memcpy(reader_copy, buf, len);
    reader_copy[len] = 0x7F;
    kept[0] = h.line[0];
    for (size_t end = len, s; n < VJO_HISTORY_LINES && (s = vjo_history_prev_start(reader_copy, end, len, whole)) < len;
         end = s) {
        VjoHistoryRow row;
        if (vjo_history_row(reader_copy + s, reader_copy + end, 4, &row) || row.len[0] != strlen(uuid) ||
            memcmp(row.uuid, uuid, row.len[0]))
            continue;
        char *f = vjo_filter_lines(&reader_lines, row.line, filter), *t = f ? vjo_java_trim(f) : NULL;
        TEST_ASSERT(t != NULL);
        if (*t && strcmp(t, kept[n]))
            kept[++n] = t;
    }
    TEST_CHECK(reader_copy[len] == 0x7F);
    TEST_CHECK_(h.n == n, "file %d, %s, filter %d: the Shell keeps %d lines, the reader %d", file_no, uuid, filter, h.n,
                n);
    for (int i = 1; i <= n && i <= h.n; i++)
        TEST_CHECK_(!strcmp(h.line[i], kept[i]), "file %d, %s: line -%d '%s', the reader's '%s'", file_no, uuid, i,
                    h.line[i], kept[i]);
}

/* A window of the file's bytes [from, to) as shell/history.c reads one: the byte before it first, unless it starts
 * the file. */
static void window(size_t from, size_t to, const char **buf, size_t *len, int *whole)
{
    *whole = !from;
    *buf = file + (from ? from - 1 : 0);
    *len = to - (from ? from - 1 : 0);
}

static void random_files_read_as_the_shell_reads_them(void)
{
    int files = 0, records = 0, damaged = 0, invalid_time = 0;
    for (uint32_t seed = 1; seed <= 300; seed++) {
        rng = seed * 2654435761u + 1;
        random_file(1 + pick(60));
        files++;
        /* what the files hold (the generator's coverage) */
        {
            static char copy[sizeof(file)];
            memcpy(copy, file, flen);
            for (size_t s = vjo_history_next_start(copy, 0, flen, 1); s < flen;) {
                size_t e = vjo_history_next_start(copy, s + 1, flen, 1);
                VjoHistoryRow row;
                records++;
                if (vjo_history_row(copy + s, copy + e, 5, &row))
                    damaged++;
                else
                    invalid_time += !row.time_us;
                s = e;
            }
        }
        for (uint32_t rule = 0; rule < 2; rule++) {
            VjoReadingScope scope;
            vjo_reading_rule = rule;
            memset(&scope, 0, sizeof(scope));
            check_counts(file, flen, 1, 0, &scope, (int)seed);
            check_counts(file, flen, 1, 1, &scope, (int)seed);
            /* one game's day (the Triangle's scope: its uuid, local midnight's bounds) */
            int g = pick(GAMES);
            snprintf(scope.uuid, sizeof(scope.uuid), "%s-%s", games[g].platform, games[g].tid);
            scope.start_us = clock_us - (uint64_t)pick(800000000);
            scope.end_us = scope.start_us + 86400000000ull;
            check_counts(file, flen, 1, 0, &scope, (int)seed);
            /* windows, as the Shell's total reads them: each with the byte before it */
            for (int w = 0; w < 4 && flen; w++) {
                size_t from = (size_t)pick((int)flen), to = from + 1 + (size_t)pick((int)(flen - from));
                const char *buf;
                size_t len;
                int whole;
                window(from, to, &buf, &len, &whole);
                memset(&scope, 0, sizeof(scope));
                check_counts(buf, len, whole, to < flen, &scope, (int)seed);
            }
        }
        vjo_reading_rule = 0;
        for (int g = 0; g < GAMES; g++) {
            check_scan(file, flen, 1, g, VJO_FILTER_NONE, (int)seed);
            check_scan(file, flen, 1, g, VJO_FILTER_LINES, (int)seed);
            if (flen > 2) { /* the file's last bytes, as the overlay's Up reads them */
                size_t from = (size_t)pick((int)flen);
                const char *buf;
                size_t len;
                int whole;
                window(from, flen, &buf, &len, &whole);
                check_scan(buf, len, whole, g, VJO_FILTER_LINES, (int)seed);
            }
        }
    }
    /* the files had every kind of record in them */
    TEST_CHECK_(files == 300 && records > 6000 && damaged > 100 && invalid_time > 1000,
                "%d files, %d records, %d damaged, %d with a time the Shell refuses", files, records, damaged,
                invalid_time);
    TEST_MSG("%d files, %d records, %d damaged, %d with a time the Shell refuses", files, records, damaged,
             invalid_time);
}

/* ---- one row ---- */

static char buf[8192];

static void row_of(const char *text, int want, VjoHistoryRow *row, int expect_rc)
{
    size_t n = strlen(text);
    memcpy(buf, text, n);
    buf[n] = 0x7F; /* past the record: never written */
    TEST_CHECK_(vjo_history_row(buf, buf + n, want, row) == expect_rc, "'%s'", text);
    TEST_CHECK(buf[n] == 0x7F);
}

static void a_record_s_cells_unescaped_in_place(void)
{
    VjoHistoryRow row;
    row_of("\"vita-PCSG00415\",\"vjo-1-0\",\"CLANNAD, \"\"Game\"\"\n2\",\"「はい、\"\"そう\"\"です」\n次\","
           "1791790205.250123,\"vita\",\"PCSG00415\",\"vocr\",\"VOCR5_N_w8.vocr\",\"raw\",17,,3,\"1:2:3:4:5\",9,\"ocr\"\n",
           48, &row, 0);
    TEST_CHECK(!strcmp(row.uuid, "vita-PCSG00415") && row.len[0] == 14 && row.cell[0] == row.uuid);
    TEST_CHECK(!strcmp(row.id, "vjo-1-0") && !strcmp(row.name, "CLANNAD, \"Game\"\n2"));
    TEST_CHECK(!strcmp(row.line, "「はい、\"そう\"です」\n次") && row.len[3] == strlen(row.line));
    TEST_CHECK(row.time_us == 1791790205250123ull && !strcmp(row.cell[4], "1791790205.250123"));
    static const char *const after[] = {"vita", "PCSG00415", "vocr", "VOCR5_N_w8.vocr", "raw", "17", "", "3",
                                        "1:2:3:4:5", "9", "ocr"};
    TEST_ASSERT(row.cells == 5 + 11);
    for (int k = 0; k < 11; k++)
        TEST_CHECK_(!strcmp(row.cell[5 + k], after[k]) && row.len[5 + k] == strlen(after[k]), "cell %d: '%s'", 5 + k,
                    row.cell[5 + k]);
    TEST_CHECK(row.cell[16] == NULL);
    /* as many cells as asked: the first four and the time always */
    row_of("\"a\",\"b\",\"c\",\"d\",1.000000,\"e\",\"f\"\n", 4, &row, 0);
    TEST_CHECK(row.cells == 4 && row.cell[4] == NULL && row.time_us == 1000000 && !strcmp(row.line, "d"));
    row_of("\"a\",\"b\",\"c\",\"d\",1.000000,\"e\",\"f\"\n", 6, &row, 0);
    TEST_CHECK(row.cells == 6 && !strcmp(row.cell[5], "e") && row.cell[6] == NULL);
    /* a CRLF line end; a record without a line break (the file's last, cut): its last bare cell is not kept */
    row_of("\"a\",\"b\",\"c\",\"d\",1.000000,7\r\n", 48, &row, 0);
    TEST_CHECK(row.cells == 6 && !strcmp(row.cell[5], "7"));
    row_of("\"a\",\"b\",\"c\",\"d\",1.000000,\"e\",7", 48, &row, 0);
    TEST_CHECK(row.cells == 6 && !strcmp(row.cell[5], "e"));
    row_of("\"a\",\"b\",\"c\",\"d\",1.000000,\"e", 48, &row, 0); /* a quoted cell cut: not kept */
    TEST_CHECK(row.cells == 5);
    /* zeros a power loss left in a line: counted by its length, as the Shell does */
    memcpy(buf, "\"a\",\"b\",\"c\",\"d\0\0e\",1.000000,\n", 29);
    TEST_CHECK(vjo_history_row(buf, buf + 29, 48, &row) == 0 && row.len[3] == 4 && !memcmp(row.line, "d\0\0e", 4));
}

/* The Shell's rules: the first four cells quoted, each followed by ',', the fourth's ',' by a digit; the time up to
 * 12 digits, '.', six digits and ','; anything else is no time, not damage. */
static void damage_and_time_by_the_shell_s_rules(void)
{
    VjoHistoryRow row;
    static const char *const damaged[] = {
        "", "\"", "a\",\"b\",\"c\",\"d\",1.000000,\n", "\"a\",b,\"c\",\"d\",1.000000,\n",
        "\"a\"\"b\",\"c\",\"d\",1.000000,\n" /* (three cells: the first is a"b) */, "\"a\",\"b\",\"c\",\"d\"1.000000,\n",
        "\"a\",\"b\",\"c\",\"d\",\"1.000000\",\n", "\"a\",\"b\",\"c\",\"d\",x\n", "\"a\",\"b\",\"c\",\"d\",",
        "\"a\",\"b\",\"c\",\"d", "\"a\",\"b\",\"c\"", "\"vita-X\",\"id\",\"name\",\"cut li\"vita-Y\",\"id2\",\"n\",\"l\",1.0,\n",
    };
    for (unsigned i = 0; i < sizeof(damaged) / sizeof(damaged[0]); i++) {
        row_of(damaged[i], 48, &row, -1);
        TEST_CHECK_(row.cells == 0 && !row.line, "'%s'", damaged[i]);
    }
    static const struct { const char *time; uint64_t us; } times[] = {
        {"1791790205.250123,", 1791790205250123ull}, {"999999999999.999999,", 999999999999999999ull},
        {"0.000001,", 1}, {"1234567890123.000000,", 0}, {"1791790205,", 0}, {"1791790205.25012,", 0},
        {"1791790205.2501234,", 0}, {"1791790205.250123x,", 0}, {"1791790205.250123\n", 0}, {"1.000000", 0},
        {"1e9.000000,", 0},
    };
    for (unsigned i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        char text[96];
        snprintf(text, sizeof(text), "\"a\",\"b\",\"c\",\"d\",%s", times[i].time);
        row_of(text, 48, &row, 0);
        TEST_CHECK_(row.time_us == times[i].us && !strcmp(row.line, "d"), "time '%s': %llu", times[i].time,
                    (unsigned long long)row.time_us);
    }
}

/* Where records start: a '"' at a line's start whose next byte is neither '"' nor ','; buf[0] only when the window
 * starts the file (else it is the byte before the window). The walks forward and back find the same ones. */
static void record_starts_and_walks(void)
{
    static const char text[] = "\"r1\",\"x\n\"\"in\",\"\n\",1.000000,\n\"r2\",\"a\",\"b\",\"c\",1.000000,\n\"\"\n,\"\n\"r3";
    size_t len = sizeof(text) - 1, starts[8], n = 0;
    for (size_t i = 0; i < len; i++)
        if (vjo_history_record_start(text, i, len, 1))
            starts[n++] = i;
    TEST_ASSERT(n == 3 && starts[0] == 0 && text[starts[1] + 1] == 'r' && text[starts[2] + 1] == 'r');
    TEST_CHECK(!vjo_history_record_start(text, 0, len, 0)); /* not whole: buf[0] is the byte before the window */
    TEST_CHECK(!vjo_history_record_start(text, len - 1, len, 1) && !vjo_history_record_start(text, len, len, 1));
    TEST_CHECK(vjo_history_next_start(text, 0, len, 1) == 0 && vjo_history_next_start(text, 0, len, 0) == starts[1]);
    TEST_CHECK(vjo_history_next_start(text, starts[2] + 1, len, 1) == len);
    TEST_CHECK(vjo_history_prev_start(text, len, len, 1) == starts[2] && vjo_history_prev_start(text, starts[2], len, 1) ==
               starts[1] && vjo_history_prev_start(text, starts[1], len, 1) == 0);
    TEST_CHECK(vjo_history_prev_start(text, starts[1], len, 0) == len && vjo_history_prev_start(text, 0, len, 1) == len);
    TEST_CHECK(vjo_history_prev_start(text, len + 5, len, 1) == starts[2]);
}

static void header_columns(void)
{
    const char *h = headers[0], *old = headers[1];
    static const char *const names[] = {"uuid", "given_identifier", "name", "line", "time", "platform", "title_id",
                                        "engine", "model", "raw_line"};
    for (int i = 0; i < 10; i++)
        TEST_CHECK(vjo_history_column(h, strlen(h), names[i]) == i);
    TEST_CHECK(vjo_history_column(h, strlen(h), "line_metrics") == VJO_HISTORY_COL_LINE_METRICS);
    TEST_CHECK(vjo_history_column(h, strlen(h), "characters") == VJO_HISTORY_COL_CHARACTERS);
    TEST_CHECK(vjo_history_column(h, strlen(h), "line_source") == VJO_HISTORY_COL_LINE_SOURCE);
    TEST_CHECK(vjo_history_column(old, strlen(old), "line_metrics") == VJO_HISTORY_COL_LINE_METRICS);
    TEST_CHECK(vjo_history_column(old, strlen(old), "characters") == -1); /* 0.8.0's: by position instead */
    TEST_CHECK(vjo_history_column(h, strlen(h), "line_") == -1 && vjo_history_column(h, strlen(h), "") == -1);
    TEST_CHECK(vjo_history_column("uuid,given_ide", 14, "uuid") == 0);
    TEST_CHECK(vjo_history_column("uuid,given_ide", 14, "given_ide") == -1); /* cut: maybe a longer name */
    TEST_CHECK(vjo_history_column("\xEF\xBB\xBFuuid,name\r\n", 14, "name") == 1);
    TEST_CHECK(vjo_history_column("\"vita-X\",\"id\"\n", 14, "uuid") == -1); /* a file without a header */
    /* every column's place, as the writer writes it: the header names them where the cells are */
    TEST_CHECK(VJO_HISTORY_COLUMNS == 43 && vjo_history_column(h, strlen(h), "fallback") == 39);
}

TEST_LIST = {
    {"random files: the same records, lines, times and totals as the Shell's", random_files_read_as_the_shell_reads_them},
    {"a record's cells, unescaped in place", a_record_s_cells_unescaped_in_place},
    {"damage and the time, by the Shell's rules", damage_and_time_by_the_shell_s_rules},
    {"record starts and the walks forward and back", record_starts_and_walks},
    {"the header's columns", header_columns},
    {NULL, NULL},
};
