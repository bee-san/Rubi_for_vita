/* The real CSV appender, over real temporary files and fault-injected Vita I/O. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "../../core/history_lines.h"
#include "../../core/textfilter.h"

static char directory[PATH_MAX], path[PATH_MAX + 16];
static unsigned opens, writes, short_bytes, read_opens, reads, read_limit;
static int open_error, write_error_after, sync_error, close_error, rollback_error, read_open_error, read_error;
static SceOff last_size;

static int native(int rc) { return rc < 0 ? -errno : rc; }
int sceIoOpen(const char *file, int flags, unsigned mode)
{
    TEST_CHECK(!strcmp(file, "ux0:data/VitaJPOverlay/history.csv"));
    if (flags == SCE_O_RDONLY) { /* line history's reader */
        read_opens++;
        return read_open_error ? (int)0x80010002 : native(open(path, O_RDONLY));
    }
    TEST_CHECK(flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND));
    opens++;
    return open_error ? -71 : native(open(path, flags, mode));
}
SceOff sceIoLseek(int fd, SceOff off, int whence)
{
    TEST_CHECK((whence == SCE_SEEK_END && off == 0) || (whence == SCE_SEEK_SET && off >= 0));
    return lseek(fd, off, whence);
}
/* read_limit: at most this many bytes a read (a short read); read_error: the n-th read fails */
static off_t read_eof_at; /* nonzero: the file ends there for the reader (an append rolled back) */
int sceIoRead(int fd, void *p, size_t n)
{
    reads++;
    TEST_CHECK(n > 0 && n <= VJO_HISTORY_WINDOW + 1);
    if (read_error && reads >= (unsigned)read_error) return -76;
    if (read_limit && n > read_limit) n = read_limit;
    if (read_eof_at) {
        off_t at = lseek(fd, 0, SEEK_CUR);
        if (at >= read_eof_at) return 0;
        if (at + (off_t)n > read_eof_at) n = (size_t)(read_eof_at - at);
    }
    return native((int)read(fd, p, n));
}
int sceIoWrite(int fd, const void *p, size_t n)
{
    TEST_CHECK(n > 0 && n <= 192); /* fixed write buffer, even for maximum text */
    writes++;
    if (write_error_after && writes >= (unsigned)write_error_after) return -72;
    if (short_bytes && n > short_bytes) n = short_bytes;
    return native((int)write(fd, p, n));
}
int sceIoSyncByFd(int fd, int flags)
{
    TEST_CHECK(flags == 0);
    return sync_error ? -73 : native(fsync(fd));
}
int sceIoClose(int fd)
{
    int rc = native(close(fd));
    return close_error ? -74 : rc;
}
int sceIoChstat(const char *file, SceIoStat *st, int flags)
{
    TEST_CHECK(!strcmp(file, "ux0:data/VitaJPOverlay/history.csv"));
    TEST_CHECK(flags == SCE_CST_SIZE);
    return rollback_error ? -75 : native(truncate(path, st->st_size));
}

#include "../../shell/history.c"

static void reset(void)
{
    if (path[0]) unlink(path);
    if (!directory[0]) {
        TEST_ASSERT(vjo_test_tmpdir_new(directory, sizeof(directory)) != NULL);
        TEST_ASSERT(snprintf(path, sizeof(path), "%s/history.csv", directory) < (int)sizeof(path));
    }
    vjo_history_reset();
    opens = writes = short_bytes = read_opens = reads = read_limit = 0;
    open_error = write_error_after = sync_error = close_error = rollback_error = read_open_error = read_error = 0;
    last_size = 0;
    read_eof_at = 0;
}

static VjoGameOcrResult result;
static VjoHistoryRecord record(const char *line)
{
    VjoHistoryRecord r = {0};
    memset(&result, 0, sizeof(result));
    snprintf(result.text, sizeof(result.text), "%s", line);
    result.seq = 42;
    result.region_x = 7; result.region_y = 8; result.width = 900; result.height = 125;
    result.fb_width = 960; result.fb_height = 544;
    result.lines = result.recognized = 2;
    result.line[0] = (VjoGameOcrLine){800, 30, 50, 950, 300000};
    result.line[1] = (VjoGameOcrLine){400, 30, 25, 1023, 200000};
    result.find_us = 11000; result.recognize_us = 500000; result.frames = 75;
    result.region_sum = 1234567890;
    strcpy(r.name, "CLANNAD"); strcpy(r.title_id, "PCSG00415");
    r.platform = "vita"; r.model = "VOCR5_N_w8.vocr"; r.result = &result;
    r.time_us = 1792220400250123ull; r.monotonic_us = 9876543210ull;
    r.scene = 17; r.region_seq = 23; r.elapsed_us = 550000;
    return r;
}

static size_t read_history(char *data, size_t cap)
{
    int fd = open(path, O_RDONLY);
    TEST_ASSERT(fd >= 0);
    ssize_t n = read(fd, data, cap - 1);
    TEST_ASSERT(n >= 0);
    data[n] = 0;
    close(fd);
    last_size = n;
    return (size_t)n;
}

/* Parse actual RFC 4180 quoting, including quoted newlines and doubled quotes. */
static int cell(const char **cursor, char *out, size_t cap)
{
    const char *p = *cursor;
    size_t n = 0;
    int quoted = *p == '"';
    if (quoted) p++;
    while (*p) {
        if (quoted && *p == '"') {
            if (p[1] == '"') p++;
            else { p++; break; }
        } else if (!quoted && (*p == ',' || *p == '\n')) break;
        TEST_ASSERT(n + 1 < cap);
        out[n++] = *p++;
    }
    out[n] = 0;
    int sep = (unsigned char)*p;
    if (*p) p++;
    *cursor = p;
    return sep;
}

static const char *row_start(const char *csv, unsigned row)
{
    char value[8192];
    while (row-- && *csv) while (cell(&csv, value, sizeof(value)) == ',') {}
    return csv;
}

static void check_cell(const char *csv, unsigned row, unsigned col, const char *expected)
{
    char value[8192];
    const char *p = row_start(csv, row);
    do { cell(&p, value, sizeof(value)); } while (col--);
    TEST_CHECK(!strcmp(value, expected));
    TEST_MSG("row %u: wanted '%s', got '%s'", row, expected, value);
}

static void test_csv_fields_and_metadata(void)
{
    char csv[16384];
    reset();
    VjoHistoryRecord r = record("「日本語,です」\n\"はい\"\r次");
    strcpy(r.name, "Game, \"title\"\n日本語");
    size_t len = strlen(result.text);
    strcpy(result.text + len + 1, "raw,\"語\"\ntext");
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    check_cell(csv, 0, 0, "uuid"); check_cell(csv, 0, 4, "time");
    check_cell(csv, 1, 0, "vita-PCSG00415");
    check_cell(csv, 1, 1, "vjo-1792220400250123-0");
    check_cell(csv, 1, 2, r.name); check_cell(csv, 1, 3, result.text);
    check_cell(csv, 1, 4, "1792220400.250123");
    check_cell(csv, 1, 5, "vita"); check_cell(csv, 1, 6, "PCSG00415");
    check_cell(csv, 1, 7, "vocr"); check_cell(csv, 1, 8, "VOCR5_N_w8.vocr");
    check_cell(csv, 1, 9, "raw,\"語\"\ntext");
    check_cell(csv, 1, 10, "17"); check_cell(csv, 1, 11, "23");
    check_cell(csv, 1, 12, "9876543210"); check_cell(csv, 1, 13, "550000");
    check_cell(csv, 1, 14, "42"); check_cell(csv, 1, 15, "7");
    check_cell(csv, 1, 17, "900"); check_cell(csv, 1, 23, "2");
    check_cell(csv, 1, 28, "500000");
    check_cell(csv, 1, 40, "800:30:50:950:300000|400:30:25:1023:200000");
    TEST_CHECK(!*row_start(csv, 2));
}

static void test_adjacent_dedup_and_header(void)
{
    char csv[16384];
    reset();
    VjoHistoryRecord r = record("最初の文");
    TEST_CHECK(vjo_history_append(&r) == 1);
    unsigned before = opens;
    r.scene++; r.region_seq++; r.time_us++;
    TEST_CHECK(vjo_history_append(&r) == 0 && opens == before);
    strcpy(result.text, "次の文");
    TEST_CHECK(vjo_history_append(&r) == 1);
    strcpy(result.text, "最初の文");
    TEST_CHECK(vjo_history_append(&r) == 1);
    strcpy(r.title_id, "PCSG00002");
    TEST_CHECK(vjo_history_append(&r) == 1); /* same sentence in a different game */
    before = opens;
    strcpy(result.text, " \n\t");
    TEST_CHECK(vjo_history_append(&r) == 0 && opens == before);
    strcpy(result.text, "最初の文");
    TEST_CHECK(vjo_history_append(&r) == 1); /* a blank screen separates actual occurrences */
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 3, "最初の文"); check_cell(csv, 2, 3, "次の文");
    check_cell(csv, 3, 3, "最初の文"); check_cell(csv, 4, 0, "vita-PCSG00002");
    check_cell(csv, 5, 3, "最初の文");
    TEST_CHECK(!*row_start(csv, 6));
    TEST_CHECK(!strstr(strchr(csv, '\n') + 1, "uuid,given_identifier"));
}

static void test_maximum_text_and_short_writes(void)
{
    char csv[16384], longest[VJO_GAME_OCR_TEXT_BYTES];
    reset();
    memset(longest, '"', sizeof(longest) - 1); longest[sizeof(longest) - 1] = 0;
    VjoHistoryRecord r = record(longest);
    short_bytes = 7;
    TEST_CHECK(vjo_history_append(&r) == 1);
    TEST_CHECK(writes > 100);
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 3, longest); check_cell(csv, 1, 9, "");
    TEST_CHECK(!*row_start(csv, 2));
}

static void test_failures_roll_back_and_retry(void)
{
    char before[16384], after[16384];
    for (int failure = 0; failure < 4; failure++) {
        reset();
        VjoHistoryRecord r = record("saved first");
        TEST_CHECK(vjo_history_append(&r) == 1);
        read_history(before, sizeof(before));
        strcpy(result.text, "failed next");
        if (failure == 0) open_error = 1;
        if (failure == 1) { short_bytes = 4; write_error_after = (int)writes + 2; }
        if (failure == 2) sync_error = 1;
        if (failure == 3) close_error = 1;
        TEST_CHECK(vjo_history_append(&r) < 0);
        read_history(after, sizeof(after));
        TEST_CHECK(!strcmp(before, after));
        open_error = write_error_after = sync_error = close_error = short_bytes = 0;
        TEST_CHECK(vjo_history_append(&r) == 1); /* failed writes did not poison dedup */
        read_history(after, sizeof(after));
        check_cell(after, 2, 3, "failed next");
        TEST_CHECK(!*row_start(after, 3));
    }
    reset();
    VjoHistoryRecord r = record("first write fails");
    write_error_after = 2;
    TEST_CHECK(vjo_history_append(&r) < 0);
    struct stat st;
    TEST_CHECK(stat(path, &st) == 0 && st.st_size == 0);
    write_error_after = 0;
    TEST_CHECK(vjo_history_append(&r) == 1);
}

static void test_restart_and_large_offset(void)
{
    char csv[16384], wanted[96];
    reset();
    VjoHistoryRecord r = record("再開");
    TEST_CHECK(vjo_history_append(&r) == 1);
    size_t original = read_history(csv, sizeof(csv));
    vjo_history_reset(); /* a fresh plugin process keeps the existing records untouched */
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 1, "vjo-1792220400250123-0");
    snprintf(wanted, sizeof(wanted), "vjo-%llu-%llu", (unsigned long long)r.time_us,
             (unsigned long long)original);
    check_cell(csv, 2, 1, wanted);

    reset();
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    TEST_ASSERT(fd >= 0);
    SceOff huge = (SceOff)4294967500ull;
    TEST_ASSERT(ftruncate(fd, huge) == 0); /* sparse: history size needs no matching RAM */
    close(fd);
    TEST_CHECK(vjo_history_append(&r) == 1);
    fd = open(path, O_RDONLY);
    ssize_t n = pread(fd, csv, sizeof(csv) - 1, huge);
    TEST_ASSERT(n > 0); csv[n] = 0; close(fd);
    check_cell(csv, 0, 1, "vjo-1792220400250123-4294967500");
    check_cell(csv, 0, 3, "再開");
    TEST_CHECK(!strstr(csv, "uuid,given_identifier"));
}

static void test_empty_and_invalid(void)
{
    reset();
    VjoHistoryRecord r = record(" \t\r\n");
    TEST_CHECK(vjo_history_append(&r) == 0 && !opens);
    strcpy(result.text, "日本語");
    r.time_us = 0;
    TEST_CHECK(vjo_history_append(&r) < 0 && !opens);
    r.time_us = 1; result.rc = -1;
    TEST_CHECK(vjo_history_append(&r) < 0 && !opens);
    result.rc = 0; memset(result.text, 'a', sizeof(result.text));
    TEST_CHECK(vjo_history_append(&r) < 0 && !opens);
    strcpy(result.text, "日本語"); memset(r.title_id, 'a', sizeof(r.title_id));
    TEST_CHECK(vjo_history_append(&r) < 0 && !opens);
    TEST_CHECK(vjo_history_append(NULL) < 0 && !opens);
}

/* ---- line history: the reader (docs/line-history.md) ---- */

static uint8_t visit_mem[16384], scratch_mem[24 * 1024] __attribute__((aligned(8)));
static VjoHistory visit;
static VjoArena visit_lines, scratch;

static int load(const char *current, const char *uuid)
{
    memset(&visit, 0, sizeof(visit));
    visit.line[0] = current;
    vjo_arena_init(&visit_lines, visit_mem, sizeof(visit_mem));
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_alloc(&scratch, 24); /* something of the caller's below the reader's window */
    char match[32];
    snprintf(match, sizeof(match), "%s\",", uuid); /* the column, its closing quote and comma (worker.c) */
    int rc = vjo_history_load(&visit, &visit_lines, match, VJO_FILTER_LINES, &scratch);
    TEST_CHECK(scratch.used == 24); /* the window is released */
    return rc;
}

/* n lines "<prefix><i>行目「台詞」" (and a Latin row the overlay does not show) of game tid, appended
 * by the real writer, each a different line */
static void append_lines(VjoHistoryRecord *r, const char *tid, const char *prefix, int from, int n)
{
    strcpy(r->title_id, tid);
    for (int i = from; i < from + n; i++) {
        memset(result.text, 0, sizeof(result.text)); /* no raw text after it */
        snprintf(result.text, sizeof(result.text), "%s%d行目「台詞」\nAUTO", prefix, i);
        r->time_us += 1000000;
        TEST_ASSERT(vjo_history_append(r) == 1);
    }
}

static void test_reader_this_games_lines_from_the_files_end(void)
{
    char want[96];
    struct stat st;
    reset();
    VjoHistoryRecord r = record("x");
    append_lines(&r, "PCSG00415", "前", 0, 60);
    append_lines(&r, "PCSG00002", "他", 0, 40); /* another game, in between */
    append_lines(&r, "PCSG00415", "", 0, 20);
    TEST_CHECK(stat(path, &st) == 0 && st.st_size > VJO_HISTORY_WINDOW + 4096); /* more than is read */
    int n = load("19行目「台詞」", "vita-PCSG00415");
    TEST_CHECK(n == visit.n && n > 20 && n <= VJO_HISTORY_LINES && read_opens == 1 && reads == 1);
    /* the newest record is the current text: -1 is the line before it, then the earlier ones, as far
     * back as the window reaches (the oldest record in it, cut by its start, is not read) */
    for (int k = 1; k <= n; k++) {
        int i = 19 - k;
        if (i >= 0)
            snprintf(want, sizeof(want), "%d行目「台詞」", i);
        else
            snprintf(want, sizeof(want), "前%d行目「台詞」", 60 + i);
        TEST_CHECK(!strcmp(visit.line[k], want)); /* without the Latin row, as the overlay shows the text */
        TEST_MSG("-%d: '%s', want '%s'", k, visit.line[k], want);
    }
    /* the other game's own lines, the same way: as many as a visit keeps; a game with none */
    TEST_CHECK(load("", "vita-PCSG00002") == VJO_HISTORY_LINES && !strcmp(visit.line[1], "他39行目「台詞」") &&
               !strcmp(visit.line[VJO_HISTORY_LINES], "他8行目「台詞」"));
    TEST_CHECK(load("", "vita-PCSG00003") == 0 && visit.n == 0);
    /* reads short of what was asked gather the window all the same */
    read_limit = 1000;
    reads = 0;
    TEST_CHECK(load("19行目「台詞」", "vita-PCSG00415") == n && !strcmp(visit.line[1], "18行目「台詞」") && reads > 20);
}

static void test_reader_reads_a_bounded_tail(void)
{
    struct stat st;
    reset();
    VjoHistoryRecord r = record("x");
    append_lines(&r, "PCSG00415", "古", 0, 5);
    append_lines(&r, "PCSG00002", "他", 0, 300); /* far more than the window holds */
    TEST_CHECK(stat(path, &st) == 0 && st.st_size > 2 * VJO_HISTORY_WINDOW);
    TEST_CHECK(load("", "vita-PCSG00415") == 0 && visit.n == 0);
    TEST_CHECK(reads == 1); /* one window, the file's last bytes: this game's lines are older */
    /* a game with fewer lines in the file than a visit holds: all of them, the file read whole */
    reset();
    r = record("x");
    append_lines(&r, "PCSG00415", "", 0, 3);
    TEST_CHECK(load("", "vita-PCSG00415") == 3 && reads == 1);
    TEST_CHECK(!strcmp(visit.line[1], "2行目「台詞」") && !strcmp(visit.line[3], "0行目「台詞」"));
}

static void test_reader_failures(void)
{
    char pad[520], want[600];
    struct stat st;
    reset();
    TEST_CHECK(load("", "vita-PCSG00415") < 0 && visit.n == 0); /* no history.csv yet */
    /* lines of about 530 B: the window holds fewer records than a visit's 32 */
    for (int i = 0; i < 170; i++)
        memcpy(pad + 3 * i, "あ", 3);
    pad[510] = 0;
    VjoHistoryRecord r = record("x");
    append_lines(&r, "PCSG00415", pad, 0, 60);
    read_open_error = 1;
    TEST_CHECK(load("", "vita-PCSG00415") == (int)0x80010002 && visit.n == 0);
    read_open_error = 0;
    /* a read that fails: nothing read, nothing kept */
    reads = 0;
    read_error = 1;
    TEST_CHECK(load("", "vita-PCSG00415") == 0 && visit.n == 0 && reads == 1);
    read_error = 0;
    /* the file shorter than its size said (an append rolled back meanwhile): the window is read short
     * and its last record, cut in its line, skipped */
    TEST_CHECK(stat(path, &st) == 0);
    reads = 0;
    read_eof_at = st.st_size - 400;
    int n = load("", "vita-PCSG00415");
    snprintf(want, sizeof(want), "%s58行目「台詞」", pad);
    TEST_CHECK(n > 5 && n < VJO_HISTORY_LINES && reads == 2 && !strcmp(visit.line[1], want)); /* the data, then the end */
    read_eof_at = 0;
    snprintf(want, sizeof(want), "%s59行目「台詞」", pad);
    TEST_CHECK(load("", "vita-PCSG00415") == n + 1 && !strcmp(visit.line[1], want));
    /* a scratch too small for the window: nothing read, nothing held */
    read_opens = 0;
    memset(&visit, 0, sizeof(visit));
    visit.line[0] = "";
    vjo_arena_init(&visit_lines, visit_mem, sizeof(visit_mem));
    vjo_arena_init(&scratch, scratch_mem, VJO_HISTORY_WINDOW);
    TEST_CHECK(vjo_history_load(&visit, &visit_lines, "vita-PCSG00415\",", VJO_FILTER_LINES, &scratch) < 0 &&
               visit.n == 0 && scratch.used == 0 && !read_opens);
}

/* The reader beside the writer's own records: its maximum text with quotes, and a record cut by a
 * power loss (the file's last bytes) */
static void test_reader_maximum_and_cut_records(void)
{
    static char csv[3 * VJO_GAME_OCR_TEXT_BYTES + 4096];
    char longest[VJO_GAME_OCR_TEXT_BYTES];
    size_t n = 0, size, line;
    reset();
    VjoHistoryRecord r = record("最初");
    TEST_CHECK(vjo_history_append(&r) == 1);
    for (int i = 0; i < 1023; i++, n += 4) /* 4,095 B: "あ\"" 1,023 times and "あ" */
        memcpy(longest + n, "あ\"", 4);
    memcpy(longest + n, "あ", 4);
    memset(result.text, 0, sizeof(result.text));
    strcpy(result.text, longest);
    TEST_CHECK(strlen(longest) == sizeof(longest) - 1 && vjo_history_append(&r) == 1);
    memset(result.text, 0, sizeof(result.text));
    strcpy(result.text, "最後");
    TEST_CHECK(vjo_history_append(&r) == 1);
    TEST_CHECK(load("最後", "vita-PCSG00415") == 2 && !strcmp(visit.line[1], longest) && !strcmp(visit.line[2], "最初"));
    /* the last record cut after its line's field and the comma after it: the line is read; cut before
     * that comma, it is not */
    size = read_history(csv, sizeof(csv));
    line = (size_t)(strstr(csv, "\"最後\",") - csv) + strlen("\"最後\",");
    TEST_ASSERT(truncate(path, (off_t)line) == 0);
    TEST_CHECK(load("", "vita-PCSG00415") == 3 && !strcmp(visit.line[1], "最後"));
    TEST_ASSERT(truncate(path, (off_t)line - 1) == 0);
    TEST_CHECK(load("", "vita-PCSG00415") == 2 && !strcmp(visit.line[1], longest));
    TEST_CHECK(size > line);
}

TEST_LIST = {
    {"CSV quotes UTF-8 and complete OCR metadata", test_csv_fields_and_metadata},
    {"line history: this game's lines, newest first, from the file's end", test_reader_this_games_lines_from_the_files_end},
    {"line history: a bounded tail of the file", test_reader_reads_a_bounded_tail},
    {"line history: no file, read errors, no scratch", test_reader_failures},
    {"line history: the writer's maximum text and cut records", test_reader_maximum_and_cut_records},
    {"adjacent rescans dedup but A/B/A and other games remain", test_adjacent_dedup_and_header},
    {"maximum text streams through bounded short writes", test_maximum_text_and_short_writes},
    {"I/O failures restore file and allow retry", test_failures_roll_back_and_retry},
    {"restart IDs and files beyond 4 GiB", test_restart_and_large_offset},
    {"blank and invalid records do not touch disk", test_empty_and_invalid},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("shell-history")) return 2; /* each test's history.csv in it, all removed at the end */
    return acutest_main(argc, argv);
}
