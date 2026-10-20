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

static char directory[PATH_MAX], path[PATH_MAX + 16];
static unsigned opens, writes, short_bytes;
static int open_error, write_error_after, sync_error, close_error, rollback_error;
static SceOff last_size;

static int native(int rc) { return rc < 0 ? -errno : rc; }
int sceIoOpen(const char *file, int flags, unsigned mode)
{
    TEST_CHECK(!strcmp(file, "ux0:data/VitaJPOverlay/history.csv"));
    TEST_CHECK(flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND));
    opens++;
    return open_error ? -71 : native(open(path, flags, mode));
}
SceOff sceIoLseek(int fd, SceOff off, int whence)
{
    TEST_CHECK(whence == SCE_SEEK_END && off == 0);
    return lseek(fd, off, whence);
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
    opens = writes = short_bytes = 0;
    open_error = write_error_after = sync_error = close_error = rollback_error = 0;
    last_size = 0;
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
    check_cell(csv, 0, 41, "characters");
    check_cell(csv, 1, 41, "8"); /* 日本語 です はい 次: not 「」, the comma, the quotes or the breaks */
    TEST_CHECK(!*row_start(csv, 2));
}

/* 0.8.0's header (#187): the columns a file it began holds, which every row
 * still has in these positions. The characters column is appended after them. */
static const char header_080[] =
    "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,"
    "scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,"
    "fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,"
    "recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,"
    "capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics";

static unsigned cells(const char *csv, unsigned row)
{
    char value[8192];
    const char *p = row_start(csv, row);
    unsigned n = 1;
    if (!*p) return 0;
    while (cell(&p, value, sizeof(value)) == ',') n++;
    return n;
}

static void test_header_keeps_080_columns(void)
{
    char csv[16384], wanted[1024];
    reset();
    VjoHistoryRecord r = record("「また明日。」");
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    snprintf(wanted, sizeof(wanted), "%s,characters\n", header_080);
    TEST_CHECK(!strncmp(csv, wanted, strlen(wanted)));
    TEST_MSG("header: %.*s", (int)(strchr(csv, '\n') - csv), csv);
    TEST_CHECK(cells(csv, 0) == 42 && cells(csv, 1) == 42);
    check_cell(csv, 1, 41, "4"); /* また明日 */

    /* A file 0.8.0 began keeps its header: the rows appended now have one cell
     * more than it names, the 42nd the characters (docs/ocr-history.md). */
    reset();
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(write(fd, header_080, strlen(header_080)) == (ssize_t)strlen(header_080));
    TEST_ASSERT(write(fd, "\n", 1) == 1);
    close(fd);
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    TEST_CHECK(cells(csv, 0) == 41 && cells(csv, 1) == 42);
    check_cell(csv, 1, 3, "「また明日。」");
    check_cell(csv, 1, 41, "4"); /* また明日 */
    TEST_CHECK(!strstr(strchr(csv, '\n') + 1, "uuid,given_identifier"));
}

/* What the count is for each kind of text (core/reading_chars.h), in the file
 * and from the function itself. */
static void test_characters_column(void)
{
    static const struct { const char *text; unsigned characters; } cases[] = {
        {"ひらがなカタカナ", 8},
        {"漢字と仮名", 5},
        {"OK, 2人で100円!", 9},           /* ASCII letters and digits count; the comma, space and ! do not */
        {"「えっ……？」『うん』", 4},     /* brackets, ellipses and marks do not */
        {"コーヒー・タイム", 7},          /* ー counts, the middle dot not */
        {"人々　ヶ月〆", 5},              /* 々 ヶ 〆 count, the ideographic space not */
        {"ｶﾀｶﾅ", 4},                      /* half-width katakana */
        {"\xF0\xA0\xAE\x9Fる", 2},        /* 𠮟: one character, 4 bytes, a surrogate pair in UTF-16 */
        {"\xF0\x9F\x98\x80笑", 1},        /* an emoji is a symbol */
        {"一行目\n二行目\r\n三", 7},     /* wrapped lines: the breaks count nothing */
        {"♪〜〜♪", 0},                    /* text, but no character: written, with 0 */
        {"\xC1\x81\xE0\x81\x81", 0},     /* 'A' in overlong forms */
        {"\xF0\x83\x81\x82", 0},          /* あ in an overlong form */
        {"\xED\xA0\x80\xF4\x90\x80\x80\xFF", 0}, /* a surrogate, past U+10FFFF, an invalid byte */
        {"あ\x80い\xE3\x81", 2},           /* a stray continuation byte, a cut sequence at the end */
    };
    char csv[65536], wanted[16];
    reset();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = strlen(cases[i].text);
        TEST_CHECK(vjo_reading_characters(cases[i].text, n) == cases[i].characters);
        TEST_MSG("case %zu: %u, wanted %u", i, (unsigned)vjo_reading_characters(cases[i].text, n),
                 cases[i].characters);
        VjoHistoryRecord r = record(cases[i].text);
        r.time_us += i;
        TEST_CHECK(vjo_history_append(&r) == 1);
    }
    read_history(csv, sizeof(csv));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(wanted, sizeof(wanted), "%u", cases[i].characters);
        check_cell(csv, (unsigned)i + 1, 3, cases[i].text);
        check_cell(csv, (unsigned)i + 1, 41, wanted);
    }
    TEST_CHECK(vjo_reading_characters("", 0) == 0);
    TEST_CHECK(vjo_reading_characters("あい", 3) == 1); /* only len bytes: い is cut */
    TEST_CHECK(vjo_reading_characters("あ\0い", 7) == 2); /* len bytes, not a string: a NUL counts nothing */
    /* The line is the game worker's normalised text, in which the full-width
     * forms are ASCII (core/textnorm.h): they count as the letters they are. */
    char fw[64] = "ＯＫ！１２３ＡＢｃ";
    TEST_CHECK(vjo_reading_characters(fw, strlen(fw)) == 0);
    vjo_text_normalize(fw, NULL);
    TEST_CHECK(!strcmp(fw, "OK!123ABc") && vjo_reading_characters(fw, strlen(fw)) == 8);
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
    check_cell(csv, 1, 3, longest); check_cell(csv, 1, 9, ""); check_cell(csv, 1, 41, "0");
    TEST_CHECK(!*row_start(csv, 2));

    reset(); /* the most kana the text holds: 1,365 of 3 bytes */
    for (size_t i = 0; i + 3 < sizeof(longest); i += 3) memcpy(longest + i, "あ", 3);
    longest[sizeof(longest) - 1] = 0;
    r = record(longest);
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 3, longest); check_cell(csv, 1, 41, "1365");
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

TEST_LIST = {
    {"CSV quotes UTF-8 and complete OCR metadata", test_csv_fields_and_metadata},
    {"0.8.0's columns stay; characters is appended, also to its files", test_header_keeps_080_columns},
    {"characters: letters and digits, not marks, bytes or breaks", test_characters_column},
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
