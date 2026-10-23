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
#include "../../core/reading_chars.h"
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
        int fd = read_open_error ? -1 : open(path, O_RDONLY);
        return read_open_error || (fd < 0 && errno == ENOENT) ? (int)0x80010002 : native(fd); /* no file: the SDK's */
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
/* read_limit: at most this many bytes a read (a short read); read_error: the n-th read fails; on_read: a
 * hook before each (the total's count: an append between two of its windows, as VjoNet's would) */
static void (*on_read)(void);
static off_t read_eof_at; /* nonzero: the file ends there for the reader (an append rolled back) */
int sceIoRead(int fd, void *p, size_t n)
{
    if (on_read) on_read();
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

int vjoNotifyShell(void); /* the kernel's (include/vjo_api.h declares it for the Vita build only) */
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
/* the debug build's count line (history.c): its clock, and the log, which this program does not keep */
uint64_t sceKernelGetProcessTimeWide(void) { return 0; }
void vjo_log(const char *fmt, ...) { (void)fmt; }

#include "../../shell/history.c"

/* The total's (docs/ocr-history.md#total-characters-read): the view's lock, the Shell's wakeup and
 * shell/files.c's file access, on the test's directory, for history_total.bin only */
VjoView g_view;
static int view_held, view_locks, notifies, total_reads, total_writes, total_write_error;
void vjo_view_lock(void) { TEST_CHECK(!view_held); view_held = 1; view_locks++; }
void vjo_view_unlock(void) { TEST_CHECK(view_held); view_held = 0; }
int vjoNotifyShell(void) { notifies++; return 0; }
static const char *total_file(void)
{
    static char p[PATH_MAX + 32];
    snprintf(p, sizeof(p), "%s/history_total.bin", directory);
    return p;
}
/* shell/files.c's, on the test's directory: history_total.bin is the only file they are given */
char *vjo_file_read(VjoArena *a, const char *file, size_t *len)
{
    TEST_CHECK(!strcmp(file, "ux0:data/VitaJPOverlay/history_total.bin"));
    total_reads++;
    FILE *f = fopen(total_file(), "rb");
    char *buf = f ? (char *)vjo_arena_alloc(a, 65) : NULL;
    size_t n = buf ? fread(buf, 1, 64, f) : 0;
    if (f) fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}
int vjo_file_write(const char *file, const void *data, size_t len)
{
    TEST_CHECK(!strcmp(file, "ux0:data/VitaJPOverlay/history_total.bin"));
    total_writes++;
    if (total_write_error) return -1;
    FILE *f = fopen(total_file(), "wb");
    TEST_ASSERT(f && fwrite(data, 1, len, f) == len);
    fclose(f);
    return 0;
}
int vjo_file_exists(const char *file)
{
    TEST_CHECK(!strcmp(file, "ux0:data/VitaJPOverlay/history_total.bin"));
    return access(total_file(), F_OK) == 0;
}


static void reset(void)
{
    if (path[0]) unlink(path);
    if (!directory[0]) {
        TEST_ASSERT(vjo_test_tmpdir_new(directory, sizeof(directory)) != NULL);
        TEST_ASSERT(snprintf(path, sizeof(path), "%s/history.csv", directory) < (int)sizeof(path));
    }
    vjo_history_reset();
    memset(&total, 0, sizeof(total)); /* the Shell's start: the total not read yet */
    unlink(total_file());
    on_read = NULL;
    notifies = total_reads = total_writes = total_write_error = 0;
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
    check_cell(csv, 0, 41, "characters");
    check_cell(csv, 1, 41, "8"); /* 日本語 です はい 次: not 「」, the comma, the quotes or the breaks */
    check_cell(csv, 0, 42, "line_source");
    check_cell(csv, 1, 42, "ocr"); /* text_from 0: OCR's line */
    check_cell(csv, 1, 33, "0");   /* `source`, the capture route (VJO_GAME_OCR_SRC_*), is another column */
    TEST_CHECK(!*row_start(csv, 2));
}

/* 0.8.0's header (#187): the columns a file it began holds, which every row
 * still has in these positions. The characters column (0.9.0) and the
 * line_source column are appended after them. */
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

/* A file an older Rubi began: its header line, as that Rubi wrote it */
static void begin_file(const char *header, const char *more)
{
    reset();
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(write(fd, header, strlen(header)) == (ssize_t)strlen(header));
    TEST_ASSERT(write(fd, more, strlen(more)) == (ssize_t)strlen(more));
    TEST_ASSERT(write(fd, "\n", 1) == 1);
    close(fd);
}

static void test_header_keeps_080_columns(void)
{
    char csv[16384], wanted[1024];
    reset();
    VjoHistoryRecord r = record("「また明日。」");
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    snprintf(wanted, sizeof(wanted), "%s,characters,line_source\n", header_080);
    TEST_CHECK(!strncmp(csv, wanted, strlen(wanted)));
    TEST_MSG("header: %.*s", (int)(strchr(csv, '\n') - csv), csv);
    TEST_CHECK(cells(csv, 0) == 43 && cells(csv, 1) == 43);
    check_cell(csv, 1, 41, "4"); /* また明日 */
    check_cell(csv, 1, 42, "ocr");

    /* A file 0.8.0 began keeps its header, and one 0.9 began (with characters) its own: the rows
     * appended now have more cells than it names, the 42nd the characters and the 43rd the line's
     * source (docs/ocr-history.md). */
    for (int began = 0; began < 2; began++) {
        begin_file(header_080, began ? ",characters" : "");
        TEST_CHECK(vjo_history_append(&r) == 1);
        read_history(csv, sizeof(csv));
        TEST_CHECK(cells(csv, 0) == (began ? 42u : 41u) && cells(csv, 1) == 43);
        check_cell(csv, 1, 3, "「また明日。」");
        check_cell(csv, 1, 41, "4"); /* また明日 */
        check_cell(csv, 1, 42, "ocr");
        TEST_CHECK(!strstr(strchr(csv, '\n') + 1, "uuid,given_identifier"));
    }
}

/* line_source (docs/ocr-history.md#line-source): hook when a native text hook's stream supplied the
 * line (text_from: a table site's 1..8, a string copy's 9..20, or any other nonzero value, as the
 * engine column has said since API 31), ocr when OCR read it (0), the same in both columns. Records
 * of every length put both cells at every offset of the writer's 192-byte buffer. A line that repeats
 * the last one is not written again when another source supplies it: one record, the first's source. */
static void test_line_source_column(void)
{
    static const uint32_t from[] = {0u, 1u, VJO_HOOK_SITE_STREAMS, VJO_HOOK_SITE_STREAMS + 1u, VJO_HOOK_STREAMS,
                                    VJO_HOOK_STREAMS + 1u, 0xFFFFFFFFu};
    static char csv[256 * 1024];
    char text[2 * 192 + 2];
    unsigned rows = 0;
    reset();
    for (unsigned n = 1; n <= 2 * 192 + 1; n++) {
        VjoHistoryRecord r = record("");
        memset(text, 'a', n);
        text[n] = 0;
        snprintf(result.text, sizeof(result.text), "%s", text);
        result.text_from = from[n % (sizeof(from) / sizeof(from[0]))];
        result.hook_flags = result.text_from ? VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR : 0u;
        r.time_us += n;
        TEST_ASSERT(vjo_history_append(&r) == 1);
    }
    read_history(csv, sizeof(csv));
    const char *p = row_start(csv, 1);
    unsigned char engine_at[192] = {0}, source_at[192] = {0}; /* the writer's buffer offsets the cells began at */
    for (unsigned n = 1; n <= 2 * 192 + 1; n++) { /* each row once, cell by cell */
        int hooked = from[n % (sizeof(from) / sizeof(from[0]))] != 0;
        char value[512], engine[16] = "", count[16] = "", source[16] = "";
        const char *append = n == 1 ? csv : p; /* the append's first byte: the first's is the header's */
        unsigned k = 0;
        int sep;
        do {
            /* a cell's literal (the engine's, line_source's) starts with the comma before the cell */
            if (k == 7) engine_at[(size_t)(p - 1 - append) % sizeof(engine_at)] = 1;
            if (k == 42) source_at[(size_t)(p - 1 - append) % sizeof(source_at)] = 1;
            sep = cell(&p, value, sizeof(value));
            if (k == 7) snprintf(engine, sizeof(engine), "%.15s", value);
            if (k == 41) snprintf(count, sizeof(count), "%.15s", value);
            if (k == 42) snprintf(source, sizeof(source), "%.15s", value);
            k++;
        } while (sep == ',');
        TEST_CHECK_(sep == '\n' && k == 43, "record %u: %u cells", n, k);
        TEST_CHECK_(!strcmp(engine, hooked ? "hook" : "vocr") && !strcmp(source, hooked ? "hook" : "ocr") &&
                    (unsigned)atoi(count) == n, "record %u: engine %s, characters %s, line_source %s", n, engine,
                    count, source);
        rows++;
    }
    TEST_CHECK(rows == 2 * 192 + 1 && !*p);
    for (size_t at = 0; at < sizeof(source_at); at++)
        TEST_CHECK_(engine_at[at] && source_at[at], "a cell at the buffer's byte %zu: engine %d, line_source %d", at,
                    engine_at[at], source_at[at]);

    /* the source is not part of the repeat check: OCR's line, then the same line from a hook */
    reset();
    VjoHistoryRecord r = record("同じ台詞");
    TEST_CHECK(vjo_history_append(&r) == 1);
    result.text_from = VJO_HOOK_SITE_STREAMS + 1u;
    r.time_us++;
    TEST_CHECK(vjo_history_append(&r) == 0);
    strcpy(result.text, "次の台詞");
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 3, "同じ台詞"); check_cell(csv, 1, 42, "ocr");
    check_cell(csv, 2, 3, "次の台詞"); check_cell(csv, 2, 42, "hook");
    TEST_CHECK(!*row_start(csv, 3));
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
    check_cell(csv, 1, 42, "ocr");
    TEST_CHECK(!*row_start(csv, 2));

    reset(); /* the most kana the text holds: 1,365 of 3 bytes, a hook's, in writes of 7 bytes */
    for (size_t i = 0; i + 3 < sizeof(longest); i += 3) memcpy(longest + i, "あ", 3);
    longest[sizeof(longest) - 1] = 0;
    r = record(longest);
    result.text_from = VJO_HOOK_SITE_STREAMS;
    short_bytes = 7;
    TEST_CHECK(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 3, longest); check_cell(csv, 1, 7, "hook"); check_cell(csv, 1, 41, "1365");
    check_cell(csv, 1, 42, "hook");
    TEST_CHECK(cells(csv, 1) == 43 && !*row_start(csv, 2));
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
    /* What a record takes, as the writer writes it: a textbox's three rows of 32 characters (3 B each),
     * the recognizer's raw text as long, three lines' metrics; what the window reaches back */
    {
        char line[400] = "", one[] = "昨日の夜は遅くまで台所で料理の本を読んでいたので今朝は眠かった。"; /* invented */
        size_t n = 0;
        for (int row = 0; row < 3; row++) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s", row ? "\n" : "", one);
        }
        memset(result.text, 0, sizeof(result.text));
        memcpy(result.text, line, n);
        memcpy(result.text + n + 1, line, n); /* the raw text after the normalised one */
        result.recognized = 3;
        result.line[2] = result.line[1];
        TEST_ASSERT(vjo_history_append(&r) == 1);
        TEST_CHECK(stat(path, &st) == 0);
        size_t header = 0;
        char csv[1024];
        read_history(csv, sizeof(csv));
        header = (size_t)(strchr(csv, '\n') - csv) + 1;
        printf("measure: a record of a %zu-byte line (three rows) and its raw text: %zu B; the %u B window holds %zu\n",
               n, (size_t)st.st_size - header, VJO_HISTORY_WINDOW, VJO_HISTORY_WINDOW / ((size_t)st.st_size - header));
        TEST_CHECK((size_t)st.st_size - header > 2 * n && (size_t)st.st_size - header < 2 * n + 400);
        reset();
        r = record("x");
    }
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

/* The window's edge: a record that starts exactly at the window's first byte (the file's size less
 * VJO_HISTORY_WINDOW) is read, as the byte before the window, a newline, says it starts a record; one
 * starting a byte later is read too, and one starting a byte earlier is cut by the window and skipped. */
static void lines_of_a_kilobyte(VjoHistoryRecord *r, int n)
{
    char pad[3 * 250 + 1];
    for (int k = 0; k < 250; k++)
        memcpy(pad + 3 * k, "あ", 3);
    pad[sizeof(pad) - 1] = 0;
    for (int i = 0; i < n; i++) { /* a short line and a long raw text after it: about 1 KB a record */
        memset(result.text, 0, sizeof(result.text));
        int len = snprintf(result.text, sizeof(result.text), "%d行目", i);
        memcpy(result.text + len + 1, pad, sizeof(pad) - 1);
        r->time_us += 1000000;
        TEST_ASSERT(vjo_history_append(r) == 1);
    }
}

static void test_reader_the_windows_first_byte(void)
{
    static char csv[160 * 1024];
    char want[64];
    for (int shift = -1; shift <= 1; shift++) {
        reset();
        VjoHistoryRecord r = record("x");
        lines_of_a_kilobyte(&r, 80); /* about 23 records in the window: fewer than a visit's 32 */
        size_t size = read_history(csv, sizeof(csv));
        int n = 30, oldest = -1;
        snprintf(want, sizeof(want), "\"%d行目", n);
        const char *line = strstr(csv, want);
        TEST_ASSERT(line != NULL);
        size_t start = (size_t)(line - csv);
        while (start && csv[start - 1] != '\n')
            start--; /* the record's first byte, after the newline the record before it ends with */
        TEST_ASSERT(start > 0 && csv[start] == '"' && start + VJO_HISTORY_WINDOW + 1 < size);
        /* the file ends VJO_HISTORY_WINDOW bytes after that start (less shift): its last record is cut */
        TEST_ASSERT(truncate(path, (off_t)(start + VJO_HISTORY_WINDOW - (size_t)shift)) == 0);
        int got = load("", "vita-PCSG00415");
        TEST_ASSERT(got > 0 && got < VJO_HISTORY_LINES);
        sscanf(visit.line[got], "%d", &oldest);
        /* shift 0: the record starts at the window's first byte; 1: a byte inside it; -1: before it */
        TEST_CHECK_(oldest == (shift < 0 ? n + 1 : n), "shift %d: the oldest line read is %d", shift, oldest);
    }
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
    /* the last record cut after its line's field, the comma and the time's first digit: the line is read;
     * cut before that digit, it is not (a record run into the next append has a '"' there) */
    size = read_history(csv, sizeof(csv));
    line = (size_t)(strstr(csv, "\"最後\",") - csv) + strlen("\"最後\",");
    TEST_ASSERT(truncate(path, (off_t)line + 1) == 0);
    TEST_CHECK(load("", "vita-PCSG00415") == 3 && !strcmp(visit.line[1], "最後"));
    TEST_ASSERT(truncate(path, (off_t)line) == 0);
    TEST_CHECK(load("", "vita-PCSG00415") == 2 && !strcmp(visit.line[1], longest));
    TEST_ASSERT(truncate(path, (off_t)line - 1) == 0);
    TEST_CHECK(load("", "vita-PCSG00415") == 2 && !strcmp(visit.line[1], longest));
    TEST_CHECK(size > line);
}

/* Native text uses the same CSV writer and reader as OCR. The appended characters and line_source
 * columns and the hook engine marker must not change how a quoted, wrapped dialogue is recovered for
 * the popup. */
static void test_reader_hook_rows_with_characters(void)
{
    static const char hooked[] = "「日本語,\"はい\"」\n続き";
    char csv[16384];
    reset();
    VjoHistoryRecord r = record("前の行");
    TEST_ASSERT(vjo_history_append(&r) == 1);
    r = record(hooked);
    result.text_from = VJO_HOOK_SITE_STREAMS + 1u;
    result.hook_flags = VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR;
    strcpy(result.text + strlen(result.text) + 1, "raw OCR text is not the displayed line");
    r.time_us++;
    TEST_ASSERT(vjo_history_append(&r) == 1);
    r = record("今の行");
    r.time_us += 2;
    TEST_ASSERT(vjo_history_append(&r) == 1);
    read_history(csv, sizeof(csv));
    check_cell(csv, 1, 7, "vocr"); check_cell(csv, 1, 42, "ocr");
    check_cell(csv, 2, 3, hooked);
    check_cell(csv, 2, 7, "hook");
    check_cell(csv, 2, 41, "7"); /* 日本語 はい 続き; neither quotes nor the comma nor the newline */
    check_cell(csv, 2, 42, "hook");
    check_cell(csv, 3, 7, "vocr"); check_cell(csv, 3, 42, "ocr");
    TEST_CHECK(cells(csv, 1) == 43 && cells(csv, 2) == 43 && cells(csv, 3) == 43);
    TEST_CHECK(load("今の行", "vita-PCSG00415") == 2 && read_opens == 1 && reads == 1);
    TEST_CHECK(!strcmp(visit.line[1], hooked) && !strcmp(visit.line[2], "前の行"));
}

/* ---- the reading total (docs/ocr-history.md#total-characters-read) ---- */

static uint8_t count_mem[24 * 1024] __attribute__((aligned(8)));
static VjoArena count_scratch;
/* A Triangle press, then the control thread's wakeups until the count ends: the windows it took. The
 * scratch is the control thread's 24 KiB, with something of the caller's below the count's window. */
static unsigned count_total(void)
{
    unsigned windows = 0;
    vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
    vjo_arena_alloc(&count_scratch, 24);
    vjo_history_total_want();
    /* each wakeup's step: at once while it says so, else at the next poll while the count goes on */
    do {
        TEST_ASSERT(++windows < 100000);
    } while (vjo_history_total_step(&count_scratch) || total.state == TOTAL_COUNTING);
    TEST_CHECK(count_scratch.used == 24 && !view_held && !total.fd);
    return windows;
}
static uint64_t shown(void) /* the view's number, read back without its commas */
{
    char buf[32], digits[32];
    size_t n = 0;
    vjo_view_lock();
    const char *t = vjo_history_total_text(buf, sizeof(buf));
    vjo_view_unlock();
    for (; *t; t++) if (*t != ',') digits[n++] = *t;
    digits[n] = 0;
    TEST_CHECK(n && strspn(digits, "0123456789") == n);
    TEST_MSG("the view says '%s'", t);
    return strtoull(digits, NULL, 10);
}
static off_t file_size(void)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}
/* What the lines of the records the writer appended count: kana, kanji, ASCII letters and digits, a
 * character outside the BMP and an emoji (not counted), quotes, commas and line breaks */
static const char *const total_lines[] = {
    "「また明日、一緒に見に行こう!」", "10時に駅で待ってるね。", "𠮟られた😀", "\"引用\",と、改行\\n\nAB12",
    "ｶﾀｶﾅ ﾃｽﾄ", "♪〜♪", "ー々〆ヶ", "Hello, world",
};
static uint64_t append_total_lines(VjoHistoryRecord *r, int n)
{
    static int seq; /* each a different text, so none is the record before it again */
    uint64_t chars = 0;
    for (int i = 0; i < n; i++, seq++) {
        const char *line = total_lines[seq % (int)(sizeof(total_lines) / sizeof(total_lines[0]))];
        memset(result.text, 0, sizeof(result.text));
        snprintf(result.text, sizeof(result.text), "%s %d", line, seq);
        chars += vjo_line_characters(result.text, strlen(result.text));
        r->time_us += 1000000;
        int rc = vjo_history_append(r);
        TEST_ASSERT_(rc == 1, "append %d of %d: %d", i, n, rc);
    }
    return chars;
}

static void test_total_counts_the_file_once_then_adds(void)
{
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 300); /* more than one window */
    TEST_CHECK(file_size() > 3 * VJO_HISTORY_WINDOW);
    vjo_view_lock();
    TEST_CHECK(!strcmp(vjo_history_total_text((char[32]){0}, 32), "counting…"));
    vjo_view_unlock();
    unsigned windows = count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && total.covered == (uint32_t)file_size());
    TEST_MSG("total %llu, want %llu; %u of %lld B", (unsigned long long)total.chars, (unsigned long long)want,
             (unsigned)total.covered, (long long)file_size());
    TEST_CHECK(windows >= 4 && shown() == want && notifies == 1);
    TEST_CHECK(total_writes == 1 && total.saved == total.covered); /* kept in history_total.bin */
    /* every append adds its line; another press reads nothing */
    unsigned before = reads, opened = read_opens;
    want += append_total_lines(&r, 50);
    vjo_history_total_want();
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && total.covered == (uint32_t)file_size());
    TEST_CHECK(!vjo_history_total_step(&count_scratch) && reads == before && read_opens == opened && notifies == 1);
    TEST_CHECK(shown() == want);
    /* a game's exit keeps it */
    vjo_history_total_keep();
    TEST_CHECK(total_writes == 2 && total.saved == total.covered);
    vjo_history_total_keep(); /* nothing added since */
    TEST_CHECK(total_writes == 2);
}

/* The Shell starts again: the total history_total.bin keeps, and only what was appended since counted, when
 * a press asks */
static void test_total_kept_between_starts(void)
{
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 120);
    count_total();
    TEST_CHECK(total.chars == want && total_writes == 1);
    uint32_t kept = total.covered;
    memset(&total, 0, sizeof(total)); /* a restart: nothing known, the writer adds nothing */
    want += append_total_lines(&r, 7);
    TEST_CHECK(total.state == TOTAL_UNREAD && !total.chars);
    TEST_CHECK(!vjo_history_total_step(&count_scratch) && total.state == TOTAL_UNREAD); /* nothing asked */
    unsigned before = reads, opened = read_opens, kept_reads = total_reads;
    TEST_CHECK(count_total() == 1); /* a press: one window, the 7 records after the kept bytes */
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && total.covered == (uint32_t)file_size());
    TEST_CHECK(reads - before == 1 && read_opens - opened == 1 && total_reads - kept_reads == 1);
    TEST_MSG("%u reads, %u opens, %u of history_total.bin", reads - before, read_opens - opened,
             total_reads - kept_reads);
    TEST_CHECK(total.covered > kept && total.covered - kept < VJO_HISTORY_WINDOW);
    TEST_CHECK(total_writes == 2 && total.saved == total.covered);
    /* nothing appended since it was kept: a press reads the byte before the end, and writes nothing */
    memset(&total, 0, sizeof(total));
    before = reads;
    count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && reads - before == 1 && total_writes == 2);
}

/* history_total.bin that is not one, or not of this history.csv: the whole file is counted */
static void test_total_recovers_from_a_bad_kept_total(void)
{
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 80);
    count_total();
    TEST_CHECK(total.chars == want);
    TotalFile good;
    FILE *f = fopen(total_file(), "rb");
    TEST_ASSERT(f && fread(&good, 1, sizeof(good), f) == sizeof(good));
    fclose(f);
    TEST_CHECK(good.chars == want && good.covered == (uint32_t)file_size());
    struct { const char *what; TotalFile file; size_t len; } bad[] = {
        {"a torn file", good, 9},
        {"a longer file", good, sizeof(good) + 1},
        {"a wrong check", {good.chars + 1, good.covered, good.check}, sizeof(good)},
        {"bytes past the file's end", {good.chars, good.covered + 1000, 0}, sizeof(good)},
        /* another file's total (its characters wrong for this one) whose bytes end inside a record here */
        {"bytes inside a record", {12345, good.covered - 100, 0}, sizeof(good)},
        {"bytes inside the header", {12345, 40, 0}, sizeof(good)},
        {"a total of nothing", {0, 0, 0}, sizeof(good)},
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        if (!bad[k].file.check) bad[k].file.check = total_check(bad[k].file.chars, bad[k].file.covered);
        f = fopen(total_file(), "wb");
        TEST_ASSERT(f && fwrite(&bad[k].file, 1, bad[k].len, f) == bad[k].len);
        fclose(f);
        memset(&total, 0, sizeof(total));
        count_total();
        TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && total.covered == (uint32_t)file_size());
        TEST_MSG("%s: %llu, want %llu", bad[k].what, (unsigned long long)total.chars, (unsigned long long)want);
    }
}

/* The total of another history.csv than the one now there (moved away, replaced, cut), whatever its size */
static void test_total_of_a_replaced_file(void)
{
    reset();
    VjoHistoryRecord r = record("x");
    append_total_lines(&r, 60);
    count_total();
    /* the file moved away: Rubi begins a new one, and the writer's first record is not the next one */
    TEST_ASSERT(unlink(path) == 0);
    vjo_history_reset();
    uint64_t want = append_total_lines(&r, 3);
    TEST_CHECK(total.state == TOTAL_COUNTING); /* the writer asked for a count */
    vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
    for (unsigned k = 0; k < 1000 && (vjo_history_total_step(&count_scratch) || total.state == TOTAL_COUNTING); k++) {}
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && total.covered == (uint32_t)file_size());
    /* a longer one in its place while the Shell was off, with no record where the kept bytes end: here the
     * same file with that record's first byte changed, so that it is no record (and counts nothing) */
    uint32_t kept = total.covered;
    vjo_history_total_keep();
    memset(&total, 0, sizeof(total)); /* (the writer adds nothing to an unknown total) */
    uint64_t first = append_total_lines(&r, 1);
    want += append_total_lines(&r, 39);
    {
        int fd = open(path, O_RDWR);
        char c = 'X';
        TEST_ASSERT(fd >= 0 && pwrite(fd, &c, 1, kept) == 1);
        close(fd);
    }
    count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && total.covered == (uint32_t)file_size() && total.chars == want);
    TEST_MSG("%llu, want %llu (the changed record's %llu not counted)", (unsigned long long)total.chars,
             (unsigned long long)want, (unsigned long long)first);
}

/* VjoNet appends while the control thread counts: every record once, none twice */
static VjoHistoryRecord *racing;
static int race_left;
static uint64_t race_chars;
static void append_in_a_read(void)
{
    if (race_left-- > 0) {
        void (*hook)(void) = on_read;
        on_read = NULL; /* (the append itself reads nothing) */
        race_chars += append_total_lines(racing, 1);
        on_read = hook;
    }
}
static void test_total_with_appends_during_the_count(void)
{
    for (int left = 1; left <= 6; left += 5) {
        reset();
        VjoHistoryRecord r = record("x");
        uint64_t want = append_total_lines(&r, 150);
        racing = &r;
        race_left = left;
        race_chars = 0;
        on_read = append_in_a_read;
        count_total();
        on_read = NULL;
        TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want + race_chars);
        TEST_CHECK(total.covered == (uint32_t)file_size());
        TEST_MSG("%d appends: %llu, want %llu", left, (unsigned long long)total.chars,
                 (unsigned long long)(want + race_chars));
    }
    /* a count that starts while a record is half written (the writer's seq odd): that record is left
     * for another round, and the total is still every record's once */
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 30);
    total.seq = 1; /* as between the writer's two increments */
    vjo_history_total_want();
    vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
    TEST_CHECK(vjo_history_total_step(&count_scratch) == 0); /* not again at once: at the next wakeup */
    TEST_CHECK(total.state == TOTAL_COUNTING && total.chars < want && total.covered < (uint32_t)file_size());
    TEST_CHECK(!total.fd && total_writes == 0);
    uint32_t newest = total.covered; /* the record that may have been half written: not counted yet */
    TEST_CHECK(vjo_history_total_step(&count_scratch) == 0 && total.state == TOTAL_COUNTING && total.covered == newest);
    total.seq = 2; /* the append's end */
    TEST_CHECK(vjo_history_total_step(&count_scratch) == 0);
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && total.covered == (uint32_t)file_size());
}

static void test_total_without_a_file_and_after_failures(void)
{
    reset();
    count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && !total.chars && !total.covered && shown() == 0);
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 2); /* the first record and the header: the total's bytes */
    TEST_CHECK(total.chars == want && total.covered == (uint32_t)file_size());
    /* a read that fails: unavailable, until a press tries again */
    memset(&total, 0, sizeof(total));
    read_error = 1;
    count_total();
    char buf[32];
    vjo_view_lock();
    TEST_CHECK(total.state == TOTAL_FAILED && !strcmp(vjo_history_total_text(buf, sizeof(buf)), "unavailable"));
    vjo_view_unlock();
    append_total_lines(&r, 1); /* the writer leaves an unknown total alone */
    TEST_CHECK(total.state == TOTAL_FAILED);
    read_error = 0;
    want += vjo_line_characters(result.text, strlen(result.text));
    count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want);
    /* short reads gather each window all the same; history_total.bin that cannot be written */
    memset(&total, 0, sizeof(total));
    unlink(total_file());
    read_limit = 1000;
    total_write_error = 1;
    count_total();
    total_write_error = 0;
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want && !total.saved);
    /* the Shell stops in the middle of a count: history.csv, open between its windows, is closed */
    memset(&total, 0, sizeof(total));
    vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
    append_total_lines(&r, 100); /* more than one window */
    vjo_history_total_want();
    TEST_CHECK(vjo_history_total_step(&count_scratch) == 1 && total.fd > 0);
    int open_fd = total.fd;
    vjo_history_total_stop();
    TEST_CHECK(!total.fd && fcntl(open_fd, F_GETFD) < 0 && errno == EBADF);
    vjo_history_total_stop(); /* nothing open: nothing to close */
    TEST_CHECK(!total.fd);
    /* a history.csv of 4 GiB or more has none */
    reset();
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    TEST_ASSERT(fd >= 0 && ftruncate(fd, (off_t)4294967500ull) == 0);
    close(fd);
    count_total();
    TEST_CHECK(total.state == TOTAL_FAILED);
    TEST_CHECK(vjo_history_append(&r) == 1 && total.state == TOTAL_FAILED);
    /* a known total whose next record takes the file past 4 GiB: not added (its end does not fit), counted */
    reset();
    fd = open(path, O_WRONLY | O_CREAT, 0600);
    TEST_ASSERT(fd >= 0 && ftruncate(fd, (off_t)4294967196ull) == 0); /* 100 B short of 4 GiB */
    close(fd);
    total.state = TOTAL_KNOWN;
    total.covered = 4294967196u;
    total.chars = 5;
    append_total_lines(&r, 1);
    TEST_CHECK(total.state == TOTAL_COUNTING && total.covered == 4294967196u && total.chars == 5);
}

/* The number as the Triangle view shows it: commas between thousands */
static void test_total_text(void)
{
    static const struct { uint64_t n; const char *text; } cases[] = {
        {0, "0"}, {7, "7"}, {999, "999"}, {1000, "1,000"}, {12345, "12,345"}, {123456, "123,456"},
        {1234567, "1,234,567"}, {1000000000, "1,000,000,000"},
        {18446744073709551615ull, "18,446,744,073,709,551,615"},
    };
    char buf[32];
    reset();
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        total.state = TOTAL_KNOWN;
        total.chars = cases[k].n;
        memset(buf, 'x', sizeof(buf));
        TEST_CHECK(!strcmp(vjo_history_total_text(buf, sizeof(buf)), cases[k].text));
        TEST_MSG("%llu: '%s'", (unsigned long long)cases[k].n, buf);
    }
    total.state = TOTAL_COUNTING;
    TEST_CHECK(!strcmp(vjo_history_total_text(buf, sizeof(buf)), "counting…"));
    total.state = TOTAL_UNREAD;
    TEST_CHECK(!strcmp(vjo_history_total_text(buf, sizeof(buf)), "counting…"));
}

/* An older Rubi's records (0.8.0: no characters column) count as the writer counts them now; a record cut
 * by a power loss counts as line history reads it (its line whole and the time after it) */
static void test_total_of_older_and_cut_records(void)
{
    reset();
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0);
    char csv[4096];
    int n = snprintf(csv, sizeof(csv), "%s\n"
        "\"vita-PCSG00415\",\"vjo-1-0\",\"CLANNAD\",\"「また明日。」\",1792220400.25,\"vita\",\"PCSG00415\",\"vocr\",\"m\",\"x\",1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,\"1:2:3:4:5\"\n"
        "\"vita-PCSG00415\",\"vjo-2-1\",\"CLANNAD\",\"cut 一二三\",17922", header_080);
    TEST_ASSERT(write(fd, csv, (size_t)n) == n);
    close(fd);
    VjoHistoryRecord r = record("今");
    TEST_CHECK(vjo_history_append(&r) == 1); /* the next boot's record after the cut one */
    count_total();
    /* また明日 (4) and the cut record's line (4: cut 一二三 → c u t 一 二 三 is 6) ... the next record runs
     * on from the cut one, so line history does not see it */
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == 4 + 6);
    TEST_MSG("%llu", (unsigned long long)total.chars);
}

/* ---- the total and power losses, a file shrinking, an append at the end (audit-total-chars A1-A6) ---- */

/* A fresh count of the file as it is now (as after a lost history_total.bin), the running total put back */
static uint64_t recount(void)
{
    uint64_t chars = total.chars;
    uint32_t covered = total.covered, saved = total.saved;
    int state = total.state;
    memset(&total, 0, sizeof(total));
    unlink(total_file());
    count_total();
    uint64_t fresh = total.chars;
    TEST_CHECK(total.state == TOTAL_KNOWN);
    total.chars = chars;
    total.covered = covered;
    total.saved = saved;
    total.state = state;
    return fresh;
}

static void write_raw(const char *bytes, size_t n, int append)
{
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0600);
    TEST_ASSERT(fd >= 0 && write(fd, bytes, n) == (ssize_t)n);
    close(fd);
}

/* A power loss in an append, then the next boot: a press counts the file, which ends in the cut record
 * (no line break after it), then the writer appends the next line right after the cut. The total is a
 * recount's, whenever the press came: the cut record is left for the round after the next append, which
 * counts it as a fresh count does (its line, when the line and the time's first digit were written),
 * and never the record that runs on from it (A1) */
static void test_total_after_a_record_a_power_loss_cut(void)
{
    static const struct { const char *cut; uint64_t line; } cuts[] = {
        {"\"vita-PCSG00415\",\"vjo-2-1\",\"CLANNAD\",\"切れた行です\",17922", 6}, /* after the time's first digit */
        {"\"vita-PCSG00415\",\"vjo-2-1\",\"CLANNAD\",\"切れた", 0},              /* inside its line */
        {NULL, 0},                                                             /* whole but its line break */
    };
    for (int k = 0; k < 3; k++) {
        reset();
        VjoHistoryRecord r = record("x");
        uint64_t before = append_total_lines(&r, 40), line = cuts[k].line;
        if (cuts[k].cut) {
            write_raw(cuts[k].cut, strlen(cuts[k].cut), 1);
        } else { /* the writer's own record, its last byte lost */
            line = append_total_lines(&r, 1);
            TEST_ASSERT(truncate(path, file_size() - 1) == 0);
        }
        off_t cut_at = file_size();
        vjo_history_reset();
        count_total(); /* the next boot's first press: the cut record is not counted yet */
        TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == before && total.covered < (uint32_t)cut_at);
        TEST_CHECK(recount() == before);
        uint64_t next = append_total_lines(&r, 1); /* runs on from the cut: no record starts there */
        TEST_CHECK(total.state == TOTAL_COUNTING); /* the writer's record is not the next one after the total */
        vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
        for (int w = 0; w < 1000 && (vjo_history_total_step(&count_scratch) || total.state == TOTAL_COUNTING); w++) {}
        uint64_t running = total.chars, fresh = recount();
        TEST_CHECK_(total.state == TOTAL_KNOWN && running == fresh && running == before + line,
                    "case %d: running %llu, recount %llu, want %llu (the next line's %llu not counted)", k,
                    (unsigned long long)running, (unsigned long long)fresh, (unsigned long long)(before + line),
                    (unsigned long long)next);
        TEST_CHECK(total.covered == (uint32_t)file_size());
    }
}

/* A lost write's zeros between a cut record and the next boot's records: the cut record's line counts (its
 * time's first digit is there), the record after the zeros runs on from it and does not (A2) */
static void test_total_with_zeros_after_a_lost_write(void)
{
    static const char cut[] = "\"vita-PCSG00415\",\"vjo-2-1\",\"CLANNAD\",\"切れた行\",1792";
    static char zeros[700];
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t before = append_total_lines(&r, 30);
    write_raw(cut, strlen(cut), 1);
    write_raw(zeros, sizeof(zeros), 1);
    vjo_history_reset();
    uint64_t after_zeros = append_total_lines(&r, 1), then = append_total_lines(&r, 3);
    count_total();
    TEST_CHECK_(total.state == TOTAL_KNOWN && total.chars == before + 4 + then,
                "%llu, want %llu (+4 for 切れた行; the %llu after the zeros not)", (unsigned long long)total.chars,
                (unsigned long long)(before + 4 + then), (unsigned long long)after_zeros);
}

/* Every size of a file around the windows' edges (within 40 B of 1, 2 and 3 windows) and at every record's
 * end: the count's windows give a count of the whole file in one buffer, a file not ending in a line break
 * with its last record left out (A3) */
static void test_total_at_every_size_around_the_windows(void)
{
    static char full[4 * VJO_HISTORY_WINDOW + 8192], copy[sizeof(full)];
    unsigned bad = 0, checked = 0;
    reset();
    VjoHistoryRecord r = record("x");
    while (file_size() < (off_t)(3 * VJO_HISTORY_WINDOW + 4000)) append_total_lines(&r, 1);
    off_t size = file_size();
    int fd = open(path, O_RDONLY);
    TEST_ASSERT(fd >= 0 && read(fd, full, (size_t)size) == size);
    close(fd);
    for (off_t at = 1; at <= size; at++) {
        int near = 0;
        size_t first, last;
        for (int w = 1; w <= 3; w++)
            near |= at >= (off_t)w * VJO_HISTORY_WINDOW - 40 && at <= (off_t)w * VJO_HISTORY_WINDOW + 40;
        if (!near && full[at - 1] != '\n') continue;
        write_raw(full, (size_t)at, 0);
        memcpy(copy, full, (size_t)at);
        int ends = full[at - 1] == '\n';
        uint64_t want = vjo_history_chars(copy, (size_t)at, 1, !ends, &first, &last);
        uint32_t want_covered = ends ? (uint32_t)at : last < (size_t)at ? (uint32_t)last : 0;
        memset(&total, 0, sizeof(total));
        unlink(total_file());
        count_total();
        checked++;
        if (total.chars != want || total.covered != want_covered) {
            if (++bad < 5)
                TEST_MSG("%lld B: %llu in %u B, want %llu in %u B", (long long)at, (unsigned long long)total.chars,
                         (unsigned)total.covered, (unsigned long long)want, (unsigned)want_covered);
        }
    }
    TEST_CHECK_(bad == 0 && checked > 500, "%u of %u sizes of a %lld B file differ", bad, checked, (long long)size);
}

/* A new file's first append cut in its header, and the next boot's records: the one that runs on from the
 * cut header does not count (tools/reading_stats.py counts it), the others do (A4) */
static void test_total_after_a_cut_header(void)
{
    static const char cut[] = "uuid,given_identifier,name,li";
    reset();
    write_raw(cut, strlen(cut), 0);
    vjo_history_reset();
    VjoHistoryRecord r = record("x");
    uint64_t runs_on = append_total_lines(&r, 1), then = append_total_lines(&r, 4);
    count_total();
    TEST_CHECK_(total.state == TOTAL_KNOWN && total.chars == then && recount() == then, "%llu, want %llu (not %llu)",
                (unsigned long long)total.chars, (unsigned long long)then, (unsigned long long)runs_on);
    /* the same with a press before the next boot's appends: the cut header alone is a window cut with no record
     * in it, whose total ends where the window starts, so the writer does not add the record run on from it */
    reset();
    write_raw(cut, strlen(cut), 0);
    vjo_history_reset();
    count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && !total.chars && !total.covered);
    runs_on = append_total_lines(&r, 1);
    TEST_CHECK(total.state == TOTAL_COUNTING);
    vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
    for (int w = 0; w < 1000 && (vjo_history_total_step(&count_scratch) || total.state == TOTAL_COUNTING); w++) {}
    then = append_total_lines(&r, 2);
    TEST_CHECK_(total.state == TOTAL_KNOWN && total.chars == then && recount() == then, "%llu, want %llu (not %llu)",
                (unsigned long long)total.chars, (unsigned long long)then, (unsigned long long)runs_on);
}

/* The file shrinks under a count (a read meets its end early: an append rolled back, a file replaced over
 * FTP): the count stops with "unavailable" instead of reading nothing for ever, and the next press counts
 * again (A5) */
static void test_total_when_the_file_shrinks_during_a_count(void)
{
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 150);
    vjo_arena_init(&count_scratch, count_mem, sizeof(count_mem));
    vjo_history_total_want();
    TEST_CHECK(vjo_history_total_step(&count_scratch) == 1); /* the first window */
    read_eof_at = VJO_HISTORY_WINDOW + 100;                   /* the file ends there for the reader */
    int steps = 0;
    while (steps++ < 1000 && (vjo_history_total_step(&count_scratch) || total.state == TOTAL_COUNTING)) {}
    TEST_CHECK_(total.state == TOTAL_FAILED && !total.fd && steps < 10, "state %d after %d steps", total.state, steps);
    read_eof_at = 0;
    count_total();
    TEST_CHECK(total.state == TOTAL_KNOWN && total.chars == want);
}

/* A press after a restart with nothing appended since the total was kept: the count's one round reads the
 * byte before the end, and VjoNet appends a line meanwhile, a record that starts where the total's bytes
 * end. It is counted once: by the next round, not by the writer as well, which adds nothing while a count
 * runs (A6) */
static int a6_left;
static VjoHistoryRecord *a6_record;
static uint64_t a6_chars;
static void append_once_in_a_read(void)
{
    if (a6_left-- > 0) {
        on_read = NULL;
        a6_chars += append_total_lines(a6_record, 1);
        on_read = append_once_in_a_read;
    }
}
static void test_total_with_an_append_during_a_round_at_the_end(void)
{
    reset();
    VjoHistoryRecord r = record("x");
    uint64_t want = append_total_lines(&r, 20);
    count_total(); /* kept in history_total.bin */
    TEST_CHECK(total.chars == want && total_writes == 1);
    memset(&total, 0, sizeof(total)); /* a restart */
    a6_record = &r;
    a6_left = 1;
    a6_chars = 0;
    on_read = append_once_in_a_read;
    count_total();
    on_read = NULL;
    TEST_CHECK_(total.state == TOTAL_KNOWN && total.chars == want + a6_chars && total.covered == (uint32_t)file_size(),
                "%llu, want %llu", (unsigned long long)total.chars, (unsigned long long)(want + a6_chars));
}

TEST_LIST = {
    {"CSV quotes UTF-8 and complete OCR metadata", test_csv_fields_and_metadata},
    {"0.8.0's and 0.9's columns stay; characters and line_source are appended, also to their files",
     test_header_keeps_080_columns},
    {"line_source: hook or ocr, as the engine says, at every buffer offset", test_line_source_column},
    {"characters: letters and digits, not marks, bytes or breaks", test_characters_column},
    {"line history: this game's lines, newest first, from the file's end", test_reader_this_games_lines_from_the_files_end},
    {"line history: a bounded tail of the file", test_reader_reads_a_bounded_tail},
    {"line history: no file, read errors, no scratch", test_reader_failures},
    {"line history: the writer's maximum text and cut records", test_reader_maximum_and_cut_records},
    {"line history: a record at the window's first byte", test_reader_the_windows_first_byte},
    {"line history: hook dialogue with characters, quotes and wrapped lines", test_reader_hook_rows_with_characters},
    {"adjacent rescans dedup but A/B/A and other games remain", test_adjacent_dedup_and_header},
    {"maximum text streams through bounded short writes", test_maximum_text_and_short_writes},
    {"I/O failures restore file and allow retry", test_failures_roll_back_and_retry},
    {"restart IDs and files beyond 4 GiB", test_restart_and_large_offset},
    {"blank and invalid records do not touch disk", test_empty_and_invalid},
    {"total: the file counted once, in windows, then each append added", test_total_counts_the_file_once_then_adds},
    {"total: kept between starts, only what was appended since counted", test_total_kept_between_starts},
    {"total: a torn, foreign or wrong history_total.bin is counted again", test_total_recovers_from_a_bad_kept_total},
    {"total: a replaced history.csv is counted again", test_total_of_a_replaced_file},
    {"total: appends while it is counted, a record half written", test_total_with_appends_during_the_count},
    {"total: no file, failed reads, short reads, 4 GiB", test_total_without_a_file_and_after_failures},
    {"total: the view's number with its commas", test_total_text},
    {"total: an older Rubi's records and a cut one", test_total_of_older_and_cut_records},
    {"total: a record a power loss cut, then the next boot's: a recount's, whenever pressed",
     test_total_after_a_record_a_power_loss_cut},
    {"total: a lost write's zeros after a cut record", test_total_with_zeros_after_a_lost_write},
    {"total: every size around the windows' edges", test_total_at_every_size_around_the_windows},
    {"total: a new file's header cut, then records", test_total_after_a_cut_header},
    {"total: the file shrinks during a count", test_total_when_the_file_shrinks_during_a_count},
    {"total: an append during a round at the file's end, counted once", test_total_with_an_append_during_a_round_at_the_end},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("shell-history")) return 2; /* each test's history.csv in it, all removed at the end */
    return acutest_main(argc, argv);
}
