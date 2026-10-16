/* The Overlay app's Anki export (settings/anki_export.c) with its package
 * writer (core/apkg.c, core/sqlw.c, core/zipw.c, core/lapis.c) on real files
 * in a temporary directory, through the Vita file API mapped onto POSIX, with
 * failed writes, flushes, closes, renames, syncs and reads injected. The
 * queue's records are written as the overlay's Enter writes them
 * (vjo_queue_encode) or as earlier builds did (versions 1 and 2).
 *
 * `--write DIR` writes the packages of host/tests/test_apkg.py instead (and
 * DIR/expected.json, what they must hold), which opens them with Python's
 * zipfile and sqlite3 and imports them into Anki itself when the `anki`
 * package is installed. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define main acutest_main
#include "acutest.h"
#undef main
#include "anki_queue.h"
#include "apkg.h"
#include "jpegsw.h"
#include "lapis.h"
#include "sqlw.h"
#include "zipw.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

/* ---------------- the memory card: real files, with failures ---------------- */

static char logs[16384];
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs)) {
        logs[n] = '\n';
        logs[n + 1] = '\0';
    }
}

/* Restart steps (test_anki_restart.py) run this binary in fresh processes:
 * _exit at the chosen storage boundary skips every cleanup. */
static const char *crash_point;
static int moves;
static void crash_at(const char *point)
{
    if (crash_point && !strcmp(crash_point, point))
        _exit(86);
}

static struct {
    long write_after;       /* bytes written before every later write fails (-1: never) */
    long written;
    int short_writes;       /* each write takes at most 1000 bytes */
    int fail_flush, fail_close, fail_sync, fail_open_read;
    const char *fail_rename; /* a rename whose source contains this fails */
    int corrupt_read;        /* the package's reads give one changed byte (a bad memory card) */
    int short_reads;         /* each read gives at most 1000 bytes */
    int fail_json_read_at, json_reads; /* the Nth opening of a record for reading fails */
    const char *fail_stat;   /* a getstat whose path contains this fails, not as missing (EIO) */
    const char *fail_remove; /* a remove whose path contains this fails */
    int fd;                  /* the package's (the last one opened for writing) */
    int syncs;
} io;

int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd;
    if (io.fail_open_read && flags == SCE_O_RDONLY && strstr(p, ".apkg"))
        return -1;
    if (flags == SCE_O_RDONLY && strstr(p, ".json") && io.fail_json_read_at &&
        ++io.json_reads == io.fail_json_read_at)
        return -1;
    fd = open(p, flags, mode);
    if (fd >= 0 && (flags & O_CREAT) && strstr(p, ".apkg"))
        io.fd = fd;
    return fd < 0 ? (errno == ENOENT ? (int)0x80010002u : -1) : fd;
}
int sceIoClose(int fd)
{
    int rc = close(fd);
    if (fd == io.fd && io.fail_close) {
        io.fd = -1;
        return -1;
    }
    return rc;
}
int sceIoRead(int fd, void *p, size_t n) { return (int)read(fd, p, n); }
int sceIoWrite(int fd, const void *p, size_t n) { return (int)write(fd, p, n); }
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off)
{
    if (io.write_after >= 0 && io.written + (long)n > io.write_after) {
        /* the bytes up to the limit reach the card, then the card is full */
        long room = io.write_after - io.written;
        if (room > 0) {
            ssize_t k = pwrite(fd, p, (size_t)room, off);
            io.written += k > 0 ? (long)k : 0;
            return (int)k;
        }
        return -1;
    }
    if (io.short_writes && n > 1000)
        n = 1000;
    {
        ssize_t k = pwrite(fd, p, n, off);
        if (k > 0)
            io.written += (long)k;
        if (io.written > 8192)
            crash_at("apkg_partial");
        return (int)k;
    }
}
int sceIoPread(int fd, void *p, size_t n, SceOff off)
{
    ssize_t k;
    if (io.short_reads && n > 1000)
        n = 1000;
    k = pread(fd, p, n, off);
    if (io.corrupt_read && k > 0 && off <= 5000 && off + k > 5000)
        ((uint8_t *)p)[5000 - off] ^= 0x40;
    return (int)k;
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return mkdir(p, mode); }
/* A file's time as the Vita gives it (UTC: VitaShell converts st_mtime to local time to show it). */
static void date_of(time_t t, SceDateTime *d)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    *d = (SceDateTime){(unsigned short)(tm.tm_year + 1900), (unsigned short)(tm.tm_mon + 1), (unsigned short)tm.tm_mday,
                       (unsigned short)tm.tm_hour, (unsigned short)tm.tm_min, (unsigned short)tm.tm_sec, 0};
}
static int file_stat(const char *p, SceIoStat *st)
{
    struct stat s;
    if (stat(p, &s))
        return errno == ENOENT ? (int)0x80010002u : -1;
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    date_of(s.st_ctim.tv_sec, &st->st_ctime);
    date_of(s.st_atim.tv_sec, &st->st_atime);
    date_of(s.st_mtim.tv_sec, &st->st_mtime);
    return 0;
}
int sceIoGetstat(const char *p, SceIoStat *st)
{
    if (io.fail_stat && strstr(p, io.fail_stat))
        return (int)0x80010005u; /* EIO: a file that cannot be checked */
    return file_stat(p, st);
}
int sceIoRename(const char *from, const char *to)
{
    int rc;
    if (io.fail_rename && strstr(from, io.fail_rename))
        return -1;
    if (strstr(to, "/exported/"))
        crash_at(moves ? "moving" : "verified"); /* before the first move, and later ones */
    if (strstr(from, ".apkg.tmp"))
        crash_at("checked"); /* the package verified, still under its temporary name */
    rc = rename(from, to);
    if (!rc && strstr(from, ".apkg.tmp"))
        crash_at("apkg_renamed");
    if (!rc && strstr(to, "/exported/")) {
        moves++;
        crash_at(strstr(to, ".jpg") ? "moved_jpg" : "moved_json");
    }
    return rc;
}
int sceIoRemove(const char *p)
{
    size_t n = strlen(p);
    int rc;
    if (io.fail_remove && strstr(p, io.fail_remove))
        return (int)0x80010005u;
    rc = unlink(p);
    /* a screenshot removed from the queue itself (never exported/): after the first, a stop */
    if (!rc && !strstr(p, "/exported/") && strstr(p, VJO_QUEUE_DIR "/") &&
        ((n > 4 && !strcmp(p + n - 4, ".jpg")) || (n > 8 && !strcmp(p + n - 8, ".jpg.tmp"))))
        crash_at("swept");
    return rc < 0 ? (errno == ENOENT ? (int)0x80010002u : -1) : 0;
}
int sceIoSyncByFd(int fd, int flag)
{
    int rc;
    (void)flag;
    rc = io.fail_flush && fd == io.fd ? -1 : fsync(fd);
    if (!rc && fd == io.fd)
        crash_at("apkg_flushed");
    return rc;
}
int sceIoSync(const char *device, unsigned flags)
{
    (void)device, (void)flags;
    io.syncs++;
    return io.fail_sync ? -1 : 0;
}
static DIR *dirs[4];
static char dir_paths[4][256];
int sceIoDopen(const char *p)
{
    for (int i = 0; i < 4; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p)))
                return -1;
            snprintf(dir_paths[i], sizeof(dir_paths[i]), "%s", p);
            return 100 + i;
        }
    return -1;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e = readdir(dirs[fd - 100]);
    char path[600];
    if (!e)
        return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", dir_paths[fd - 100], e->d_name);
    TEST_ASSERT(file_stat(path, &ent->d_stat) == 0); /* (the entry's own: io.fail_stat is for getstat calls) */
    return 1;
}
int sceIoDclose(int fd)
{
    int rc = closedir(dirs[fd - 100]);
    dirs[fd - 100] = NULL;
    return rc;
}
/* 2026-10-12 09:30:05 on the console, 1791790205 s UTC */
static SceDateTime clock_local = {2026, 10, 12, 9, 30, 5, 0};
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    *t = clock_local;
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = 62135596800000000ull + 1791790205ull * 1000000ull;
    return 0;
}

#include "../../settings/anki_export.c"

/* ---------------- the queue ---------------- */

static uint8_t mem[VJO_EXPORT_MEM] __attribute__((aligned(16)));
static VjoArena A;
static char status_line[128];
static int stop_at_status = -1, statuses;

static void ui_status(void *ud, const char *msg)
{
    (void)ud;
    snprintf(status_line, sizeof(status_line), "%s", msg);
    statuses++;
}
static int ui_stopped(void *ud)
{
    (void)ud;
    return stop_at_status >= 0 && statuses >= stop_at_status;
}
static const VjoExportUi ui = {NULL, ui_status, ui_stopped};

static void rm_rf(const char *path)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    TEST_ASSERT(system(cmd) == 0);
}

static void reset(void)
{
    memset(&io, 0, sizeof(io));
    io.write_after = -1;
    io.fd = -1;
    logs[0] = status_line[0] = '\0';
    stop_at_status = -1;
    statuses = 0;
    rm_rf(VJO_EXPORT_QUEUE_DIR);
    rm_rf(VJO_EXPORT_DIR);
    TEST_ASSERT(mkdir(VJO_EXPORT_QUEUE_DIR, 0777) == 0);
    memset(mem, 0xA5, sizeof(mem));
    vjo_arena_init(&A, mem, sizeof(mem));
}

static void put_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, len, f) == len);
    fclose(f);
}

/* A record into the queue (or the archive) under its ID; the ID. */
static const char *queue_json(const char *json, int archive)
{
    static char id[VJO_QUEUE_ID_SIZE];
    char path[256];
    vjo_queue_id(json, strlen(json), id);
    if (archive)
        mkdir(VJO_EXPORT_ARCHIVE_DIR, 0777);
    snprintf(path, sizeof(path), "%s/%s.json", archive ? VJO_EXPORT_ARCHIVE_DIR : VJO_EXPORT_QUEUE_DIR, id);
    put_file(path, json, strlen(json));
    return id;
}

/* A screenshot: JPEG markers around a pattern (Anki does not decode it). */
static size_t jpeg(uint8_t *out, int seed, size_t len)
{
    out[0] = 0xFF, out[1] = 0xD8, out[2] = 0xFF, out[3] = 0xE0;
    for (size_t i = 4; i + 2 < len; i++)
        out[i] = (uint8_t)(i * 31u + (unsigned)seed * 7u);
    out[len - 2] = 0xFF, out[len - 1] = 0xD9;
    return len;
}

/* A real screenshot (API 23), as the game worker makes one: its encoder
 * (core/jpegsw.c) on a 960 x 544 picture of a light gradient with dark
 * strokes, at its quality, 70; the same bytes every time. */
#define REAL_PICTURE 100 /* Card.picture: this JPEG instead of jpeg()'s pattern */
static int real_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    (void)ud;
    for (uint32_t r = 0; r < n; r++)
        for (uint32_t x = 0; x < 960; x++) {
            uint8_t *p = dst + (r * 960u + x) * 4u;
            uint32_t y = row + r, ink = y > 380 && y < 520 && (x / 3) % 9 < 2 && (y / 6) % 5 < 4;
            p[0] = (uint8_t)(ink ? 25 : 180 + x * 60 / 960);
            p[1] = (uint8_t)(ink ? 25 : 160 + y * 80 / 544);
            p[2] = (uint8_t)(ink ? 25 : 200 - x * 80 / 960);
            p[3] = 0xFF;
        }
    return (int)n;
}
static size_t real_jpeg(const uint8_t **out)
{
    static uint8_t mem[512 * 1024];
    VjoArena a;
    VjoBuf b;
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(vjo_jpeg_encode(&a, 960, 544, 960 * 4, real_rows, NULL, 70, &b) == 0);
    *out = b.data;
    return b.len;
}

typedef struct {
    const char *spelling, *reading, *sentence, *meaning, *dictionary, *misc, *game;
    int hl_start, hl_end, rank, picture;
    int64_t time;
    const char *source;
    int pitch;
    const char *more_dict, *more_meaning;
} Card;

/* A version-3 record, as the overlay's Enter writes it; its ID. */
static const char *queue_card(const Card *c, int archive)
{
    const char *m[1] = {c->meaning}, *more_m[1] = {c->more_meaning}, *pos[1] = {"n"};
    VjoAnkiGloss more = {c->more_dict, c->spelling, c->reading, more_m, 1};
    VjoAnkiNote n;
    size_t mark = vjo_arena_mark(&A);
    char *json;
    const char *id;
    memset(&n, 0, sizeof(n));
    n.spelling = c->spelling;
    n.reading = c->reading;
    n.rank = c->rank ? c->rank : VJO_NO_RANK;
    n.source = c->source;
    n.meanings = m;
    n.n_meanings = 1;
    n.sentence = c->sentence;
    n.hl_start = c->hl_start;
    n.hl_end = c->hl_end;
    n.sentence_furigana = c->sentence;
    n.sf_hl_start = c->hl_start;
    n.sf_hl_end = c->hl_end;
    n.pos = pos;
    n.n_pos = 1;
    if (c->pitch) {
        n.pitch[0] = c->pitch - 1;
        n.n_pitch = 1;
    }
    n.misc = c->misc;
    n.dictionary = c->dictionary;
    if (c->more_dict) {
        n.more = &more;
        n.n_more = 1;
    }
    n.game = c->game;
    n.title_id = "PCSG00000";
    n.platform = "vita";
    n.unix_time = c->time;
    n.ocr_engine = "vocr";
    n.ocr_model = "VOCR3_H15_w8.vocr";
    TEST_ASSERT((json = vjo_queue_encode(&A, &n, c->picture)) != NULL);
    id = queue_json(json, archive);
    if (c->picture) {
        uint8_t pic[300];
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.jpg", archive ? VJO_EXPORT_ARCHIVE_DIR : VJO_EXPORT_QUEUE_DIR, id);
        if (c->picture == REAL_PICTURE) {
            const uint8_t *real;
            size_t n = real_jpeg(&real);
            put_file(path, real, n);
        } else {
            put_file(path, pic, jpeg(pic, c->picture, sizeof(pic)));
        }
    }
    vjo_arena_release(&A, mark);
    return id;
}

static const Card cat = {"猫", "ねこ", "猫が好き", "cat", "JMdict", "SOME GAME (2026-10-11 17:11)", "SOME GAME",
                         0, 3, 1234, 0, 1791738660, "Jiten", 1, NULL, NULL};
static const Card dog = {"犬", "いぬ", "「犬だ！」\nと言った", "dog", "JMdict", "SOME GAME (2026-10-11 17:12)",
                         "SOME GAME", 3, 6, 0, 0, 1791738720, NULL, 0, "Daijirin4", "いぬ。四足の哺乳類。"};
static const Card eat = {"食べる", "たべる", "パンを食べた", "to eat <&> \"quoted\"", "Sankoku8",
                         "SOME GAME (2026-10-11 17:12)", "SOME GAME", 9, 15, 0, 0, 1791738720, NULL, 2, NULL, NULL};

static const char v1[] = "{\"version\":1,\"spelling\":\"猫\",\"reading\":\"ねこ\",\"sentence\":\"猫が\\n好き\","
                         "\"rank\":1234,\"hl_start\":0,\"hl_end\":3,\"picture\":false,\"meanings\":[\"cat\"]}\n";
static const char v2[] =
    "{\"version\":2,\"spelling\":\"部屋\",\"reading\":\"へや\","
    "\"sentence\":\"隣の部屋で、妙に背の高い先生と話をして\",\"rank\":99999,\"hl_start\":6,\"hl_end\":12,"
    "\"picture\":false,\"meanings\":[\"[Daijirin4 Plain]\\nA room or chamber of a house.; A sumo stable.\"],"
    "\"sentence_furigana\":\"隣の部屋で、妙に背の高い先生と話をして\",\"sf_hl_start\":6,\"sf_hl_end\":12,"
    "\"source\":null,\"pitch\":[],\"pos\":[],\"misc\":\"SOME GAME (2026-10-11 17:11)\"}\n";

static int count_dir(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && strlen(e->d_name) > strlen(suffix) &&
            !strcmp(e->d_name + strlen(e->d_name) - strlen(suffix), suffix))
            n++;
    closedir(d);
    return n;
}

/* Every file under the queue, with its bytes: a snapshot to compare. */
static char *snapshot(void)
{
    static char buf[1 << 16];
    FILE *p = popen("cd 'ux0:data/VitaJPOverlay' && find anki_queue -type f | sort | xargs -r sha256sum", "r");
    size_t n;
    TEST_ASSERT(p != NULL);
    n = fread(buf, 1, sizeof(buf) - 1, p);
    buf[n] = '\0';
    pclose(p);
    return buf;
}

static int run(int all, VjoExportResult *r, char *msg)
{
    vjo_arena_init(&A, mem, sizeof(mem));
    return vjo_export_run(&A, all, &ui, r, msg, 300);
}

/* ---------------- the package, read here ---------------- */

typedef struct {
    FILE *f;
} Pkg;
static int pkg_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    Pkg *p = (Pkg *)ctx;
    return fseek(p->f, (long)off, SEEK_SET) || fread(dst, 1, n, p->f) != n ? -1 : 0;
}

/* The package's entries (at most cap), or -1. */
static int read_pkg(const char *path, VjoZipEntry *e, int cap)
{
    Pkg p = {fopen(path, "rb")};
    VjoZipIo zio = {&p, NULL, pkg_read};
    uint8_t buf[4096];
    long size;
    int n;
    if (!p.f)
        return -1;
    fseek(p.f, 0, SEEK_END);
    size = ftell(p.f);
    n = vjo_zip_read(&zio, (uint64_t)size, e, cap);
    for (int i = 0; i < n; i++)
        if (vjo_zip_check_crc(&zio, &e[i], buf, sizeof(buf)) < 0)
            n = -2;
    fclose(p.f);
    return n;
}

/* ---------------- tests ---------------- */

static void test_sqlite_records_and_varints(void)
{
    static const struct {
        uint64_t v;
        const char *bytes;
        int n;
    } vec[] = {
        {0, "\x00", 1},
        {127, "\x7f", 1},
        {128, "\x81\x00", 2},
        {16383, "\xff\x7f", 2},
        {16384, "\x81\x80\x00", 3},
        {0x7FFFFFFFFFFFFFFFull, "\xbf\xff\xff\xff\xff\xff\xff\xff\xff", 9},
        {0xFFFFFFFFFFFFFFFFull, "\xff\xff\xff\xff\xff\xff\xff\xff\xff", 9},
    };
    uint8_t buf[16], *rec;
    size_t len;
    for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        uint64_t back = 1;
        int n = vjo_sqlw_varint(buf, vec[i].v);
        TEST_CHECK(n == vec[i].n && !memcmp(buf, vec[i].bytes, (size_t)n));
        TEST_CHECK(vjo_sqlw_get_varint(buf, (size_t)n, &back) == n && back == vec[i].v);
        TEST_MSG("%llu: %d bytes", (unsigned long long)vec[i].v, n);
    }
    /* the payload kept on a 4096-byte leaf, as SQLite computes it (U-35 = 4061, M = 489) */
    TEST_CHECK(vjo_sqlw_local_payload(4061) == 4061);
    TEST_CHECK(vjo_sqlw_local_payload(4062) == 489); /* K = 489 + 3573 % 4092 = 4062 > X: M bytes */
    TEST_CHECK(vjo_sqlw_local_payload(100000) == 489 + (100000 - 489) % 4092); /* K = 1415 <= X */
    TEST_CHECK(vjo_sqlw_local_payload(8000) == 489 + (8000 - 489) % 4092); /* K = 3908 */
    /* a record: the header's serial types, then the body (NULL, 1, -129, "abc") */
    vjo_arena_init(&A, mem, sizeof(mem));
    {
        VjoSqlwVal v[4] = {{VJO_SQLW_NULL, 0, NULL, 0}, {VJO_SQLW_INT, 1, NULL, 0}, {VJO_SQLW_INT, -129, NULL, 0},
                           {VJO_SQLW_TEXT, 0, "abc", 3}};
        TEST_ASSERT(vjo_sqlw_record(&A, v, 4, &rec, &len) == 0);
        TEST_CHECK(len == 10 && !memcmp(rec, "\x05\x00\x09\x02\x13\xff\x7f" "abc", 10));
    }
}

static void test_zip_crc_and_reader(void)
{
    TEST_CHECK(vjo_crc32(0, "123456789", 9) == 0xCBF43926u); /* the CRC-32 check value */
    TEST_CHECK(vjo_crc32(vjo_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    TEST_CHECK(vjo_crc32_combine(vjo_crc32(0, "1234", 4), vjo_crc32(0, "56789", 5), 5) == 0xCBF43926u);
    TEST_CHECK(vjo_crc32_combine(0xCBF43926u, 0, 0) == 0xCBF43926u);
    {
        static uint8_t big[3 * 4096 + 7];
        for (size_t i = 0; i < sizeof(big); i++)
            big[i] = (uint8_t)(i * 131u + 7u);
        TEST_CHECK(vjo_crc32_combine(vjo_crc32(0, big, 4096), vjo_crc32(0, big + 4096, sizeof(big) - 4096),
                                     sizeof(big) - 4096) == vjo_crc32(0, big, sizeof(big)));
    }
    {
        uint16_t t, d;
        vjo_zip_dos_time(2026, 10, 12, 9, 30, 5, &t, &d);
        TEST_CHECK(t == (9 << 11 | 30 << 5 | 2) && d == ((2026 - 1980) << 9 | 10 << 5 | 12));
        vjo_zip_dos_time(0, 0, 0, 0, 0, 0, &t, &d); /* no clock: 1980-01-01 */
        TEST_CHECK(t == 0 && d == (1 << 5 | 1));
    }
}

static void test_guid_and_media_name(void)
{
    uint8_t id[16] = {0};
    char g[VJO_APKG_GUID_MAX], name[48];
    vjo_apkg_guid(id, g);
    TEST_CHECK(!strcmp(g, "a")); /* 0 */
    id[7] = 91;
    vjo_apkg_guid(id, g);
    TEST_CHECK(!strcmp(g, "ba")); /* 91 = 1*91 + 0 */
    memset(id, 0xFF, 8);
    vjo_apkg_guid(id, g);
    TEST_CHECK(strlen(g) == 10); /* 2^64-1 in base 91 */
    vjo_apkg_media_name((const uint8_t *)"abc", 3, name);
    /* SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223 b00361a396177a9cb410ff61f20015ad */
    TEST_CHECK(!strcmp(name, "vitajp_ba7816bf8f01cfea414140de5dae2223.jpg"));
}

static void test_lapis_fields(void)
{
    const char *m[2] = {"[JMdict]\ncat", "feline"}, *pos[1] = {"n"};
    VjoAnkiNote n;
    char *v;
    vjo_arena_init(&A, mem, sizeof(mem));
    memset(&n, 0, sizeof(n));
    n.spelling = "猫";
    n.reading = "ねこ";
    n.rank = VJO_NO_RANK;
    n.meanings = m;
    n.n_meanings = 2;
    n.sentence = "猫が<好き>";
    n.hl_start = 0;
    n.hl_end = 3;
    n.sentence_furigana = n.sentence; /* no readings: Lapis's {{furigana:Sentence}} */
    n.sf_hl_start = 0;
    n.sf_hl_end = 3;
    n.pos = pos;
    n.n_pos = 1;
    n.pitch[0] = 1;
    n.n_pitch = 1;
    n.misc = "SOME GAME (2026-10-11 17:11)";
    TEST_CHECK(VJO_LAPIS_FIELDS == 22 && !strcmp(vjo_lapis_field[0], "Expression") &&
               !strcmp(vjo_lapis_field[VJO_LAPIS_FREQ_SORT], "FreqSort") &&
               !strcmp(vjo_lapis_field[VJO_LAPIS_MISC_INFO], "MiscInfo"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_EXPRESSION, &n, NULL), "猫"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_EXPRESSION_FURIGANA, &n, NULL), "猫[ねこ]"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_EXPRESSION_READING, &n, NULL), "ねこ"));
    v = vjo_lapis_field_value(&A, VJO_LAPIS_SENTENCE, &n, NULL);
    TEST_CHECK(!strcmp(v, "<b>猫</b>が&lt;好き&gt;"));
    TEST_MSG("%s", v);
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_SENTENCE_FURIGANA, &n, NULL), ""));
    v = vjo_lapis_field_value(&A, VJO_LAPIS_MAIN_DEFINITION, &n, NULL);
    TEST_CHECK(!strcmp(v, "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol><li data-dictionary=\"JMdict\">"
                          "<i>(JMdict)</i> <span><ul><li>cat</li><li>feline</li></ul></span></li></ol></div>"));
    TEST_MSG("%s", v);
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_GLOSSARY, &n, NULL), "")); /* one dictionary */
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_FREQ_SORT, &n, NULL), "9999999"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_FREQUENCY, &n, NULL), ""));
    TEST_CHECK(strstr(vjo_lapis_field_value(&A, VJO_LAPIS_PITCH_POSITION, &n, NULL), "<span>1</span>") != NULL);
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_MISC_INFO, &n, NULL), "SOME GAME (2026-10-11 17:11)"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_PICTURE, &n, NULL), ""));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_PICTURE, &n, "vitajp_x.jpg"), "<img src=\"vitajp_x.jpg\">"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_EXPRESSION_AUDIO, &n, NULL), ""));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_HINT, &n, NULL), ""));
    n.rank = 384;
    n.source = "Jiten";
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_FREQ_SORT, &n, NULL), "384"));
    TEST_CHECK(!strcmp(vjo_lapis_field_value(&A, VJO_LAPIS_FREQUENCY, &n, NULL),
                       "<ul style=\"text-align: left;\"><li>Jiten: 384</li></ul>"));
    {
        size_t len;
        char *all = vjo_lapis_fields(&A, &n, NULL, &len);
        int seps = 0;
        TEST_ASSERT(all != NULL);
        for (size_t i = 0; i < len; i++)
            seps += all[i] == 0x1F;
        TEST_CHECK(seps == 21 && len == strlen(all));
    }
    /* a separator inside a dictionary's text cannot make a field more */
    {
        const char *odd[1] = {"a\x1f" "b"};
        size_t len;
        int seps = 0;
        char *all;
        n.meanings = odd;
        n.n_meanings = 1;
        all = vjo_lapis_fields(&A, &n, NULL, &len);
        for (size_t i = 0; i < len; i++)
            seps += all[i] == 0x1F;
        TEST_CHECK(seps == 21);
    }
}

static void test_export_writes_a_dated_verified_package_and_clears_the_queue(void)
{
    VjoExportResult r;
    VjoZipEntry e[8];
    VjoExportCounts c;
    char msg[300];
    reset();
    queue_card(&cat, 0);
    queue_card(&dog, 0);
    queue_card(&eat, 0);
    vjo_export_count(&c);
    TEST_CHECK(c.queued == 3 && c.exported == 0);
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_MSG("%s\n%s", msg, logs);
    TEST_CHECK(r.cards == 3 && r.moved == 3 && !r.kept && !r.unreadable && !r.too_large && !r.left);
    TEST_CHECK(!strcmp(r.name, "2026-10-12_093005-3cards.apkg"));
    TEST_CHECK(!strcmp(r.path, "ux0:data/VitaJPOverlay/anki/2026-10-12_093005-3cards.apkg"));
    TEST_CHECK(strstr(msg, "Exported 3 cards (") && strstr(msg, "to 2026-10-12_093005-3cards.apkg."));
    TEST_MSG("%s", msg);
    TEST_CHECK(read_pkg(r.path, e, 8) == 2 && !strcmp(e[0].name, "collection.anki2") && !strcmp(e[1].name, "media"));
    TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".tmp") == 0 && count_dir(VJO_EXPORT_DIR, ".apkg") == 1);
    vjo_export_count(&c);
    TEST_CHECK(c.queued == 0 && c.exported == 3);
    TEST_CHECK(count_dir(VJO_EXPORT_QUEUE_DIR, ".json") == 0 && count_dir(VJO_EXPORT_ARCHIVE_DIR, ".json") == 3);
    TEST_CHECK(strstr(logs, "verified") != NULL);
    /* nothing more to export; all cards: the three again */
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 0 && strstr(msg, "No cards queued"));
    TEST_CHECK(run(1, &r, msg) == VJO_OK && r.cards == 3 && r.moved == 0);
    TEST_CHECK(!strcmp(r.name, "2026-10-12_093005-all-3cards.apkg"));
    /* the same second again: another name */
    TEST_CHECK(run(1, &r, msg) == VJO_OK && !strcmp(r.name, "2026-10-12_093005-all-3cards-2.apkg"));
    TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".apkg") == 3);
}

static void test_one_card_and_every_record_version(void)
{
    VjoExportResult r;
    char msg[300];
    reset();
    queue_json(v1, 0);
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 1 && !strcmp(r.name, "2026-10-12_093005-1card.apkg"));
    TEST_CHECK(strstr(msg, "Exported 1 card (") != NULL);
    queue_json(v2, 0);
    queue_card(&cat, 0);
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 2 && r.moved == 2);
    TEST_MSG("%s\n%s", msg, logs);
}

/* Any failure before the package is verified: no package, the queue as it was. */
static void test_a_failed_write_never_clears_the_queue(void)
{
    VjoExportResult r;
    char msg[300], before[1 << 16];
    long size;
    int tried = 0;
    reset();
    for (int i = 0; i < 40; i++) {
        Card c = cat;
        char word[16];
        snprintf(word, sizeof(word), "猫%d", i);
        c.spelling = word;
        c.time += i;
        c.picture = i % 9 == 0 ? 1 + i : 0;
        queue_card(&c, 0);
    }
    snprintf(before, sizeof(before), "%s", snapshot());
    /* how large the package is */
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    size = (long)r.bytes;
    rm_rf(VJO_EXPORT_DIR);
    rm_rf(VJO_EXPORT_ARCHIVE_DIR);
    reset();
    for (int i = 0; i < 40; i++) {
        Card c = cat;
        char word[16];
        snprintf(word, sizeof(word), "猫%d", i);
        c.spelling = word;
        c.time += i;
        c.picture = i % 9 == 0 ? 1 + i : 0;
        queue_card(&c, 0);
    }
    TEST_ASSERT(!strcmp(snapshot(), before));
    /* the memory card full after every 1/23 of the package, and at its first and last bytes */
    for (long at = 0; at <= size; at += at < 64 ? 13 : size / 23) {
        memset(&io, 0, sizeof(io));
        io.write_after = at;
        io.fd = -1;
        TEST_CHECK(run(0, &r, msg) != VJO_OK);
        TEST_CHECK(!strcmp(snapshot(), before));
        TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".apkg") == 0 && count_dir(VJO_EXPORT_DIR, ".tmp") == 0);
        TEST_CHECK(strstr(msg, "The queue is unchanged") != NULL);
        TEST_MSG("full at %ld of %ld: %s", at, size, msg);
        tried++;
    }
    TEST_CHECK(tried > 20);
    /* a failed flush, close, rename, sync; a file that does not read back as written */
    for (int k = 0; k < 6; k++) {
        memset(&io, 0, sizeof(io));
        io.write_after = -1;
        io.fd = -1;
        io.fail_flush = k == 0;
        io.fail_close = k == 1;
        io.fail_rename = k == 2 ? ".apkg.tmp" : NULL;
        io.fail_sync = k == 3;
        io.corrupt_read = k == 4;
        io.fail_open_read = k == 5;
        TEST_CHECK(run(0, &r, msg) != VJO_OK);
        TEST_CHECK(!strcmp(snapshot(), before));
        TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".apkg") == 0 && count_dir(VJO_EXPORT_DIR, ".tmp") == 0);
        TEST_MSG("case %d: %s", k, msg);
    }
    /* short writes are written out in full */
    memset(&io, 0, sizeof(io));
    io.write_after = -1;
    io.fd = -1;
    io.short_writes = 1;
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 40 && r.moved == 40 && (long)r.bytes == size);
}

/* 30 cards, every fifth with a screenshot. */
static void queue_thirty(void)
{
    for (int i = 0; i < 30; i++) {
        Card c = dog;
        char word[16];
        snprintf(word, sizeof(word), "犬%d", i);
        c.spelling = word;
        c.picture = i % 5 == 0 ? 3 + i : 0;
        queue_card(&c, 0);
    }
}

/* A stop at any progress step: before the package is checked and named, nothing
 * changes; after it, the package stays, and every card is in it and either out
 * of the queue or still queued (the clearing stops between two cards): none is
 * lost, none is in both places. */
static void test_a_stop_anywhere_loses_no_card(void)
{
    VjoExportResult r;
    char msg[300], before[1 << 16];
    int total, cancelled = 0, partly = 0;
    reset();
    queue_thirty();
    snprintf(before, sizeof(before), "%s", snapshot());
    TEST_ASSERT(run(0, &r, msg) == VJO_OK && r.moved == 30);
    total = statuses;
    for (int at = 1; at <= total + 1; at++) {
        reset();
        queue_thirty();
        TEST_ASSERT(!strcmp(snapshot(), before));
        stop_at_status = at;
        int rc = run(0, &r, msg);
        int queued = count_dir(VJO_EXPORT_QUEUE_DIR, ".json"), archived = count_dir(VJO_EXPORT_ARCHIVE_DIR, ".json");
        if (rc == VJO_E_CANCELLED) {
            cancelled++;
            TEST_CHECK_(!strcmp(snapshot(), before) && count_dir(VJO_EXPORT_DIR, ".apkg") == 0 &&
                            count_dir(VJO_EXPORT_DIR, ".tmp") == 0,
                        "stopped at status %d of %d: %s", at, total, msg);
            continue;
        }
        TEST_CHECK_(rc == VJO_OK && r.cards == 30 && count_dir(VJO_EXPORT_DIR, ".apkg") == 1 &&
                        count_dir(VJO_EXPORT_DIR, ".tmp") == 0 && r.moved + r.stopped == 30 && queued == r.stopped &&
                        archived == r.moved && count_dir(VJO_EXPORT_QUEUE_DIR, ".jpg") +
                                                       count_dir(VJO_EXPORT_ARCHIVE_DIR, ".jpg") == 6,
                    "stopped at status %d of %d: rc %d, %d moved, %d stopped, %d queued, %d archived: %s", at, total,
                    rc, r.moved, r.stopped, queued, archived, msg);
        if (r.stopped && r.moved)
            partly++;
    }
    TEST_CHECK(cancelled >= 9 && partly >= 1);
    TEST_MSG("%d statuses: %d stops changed nothing, %d stopped the clearing between cards", total, cancelled, partly);
    stop_at_status = -1;
}

/* A package that does not read back as written never gets its final name: it is checked
 * under its temporary name first (audit-overlay-apkg-b D1; the order before N2's fix renamed,
 * synced, then checked). A child process stops at the rename of the .apkg.tmp, should it come. */
static void test_a_package_that_reads_back_wrong_never_gets_its_name(void)
{
    VjoExportResult r;
    char msg[300];
    for (int bad = 0; bad < 2; bad++) {
        pid_t pid;
        int status = 0;
        reset();
        queue_card(&cat, 0);
        queue_card(&dog, 0);
        fflush(stdout);
        if ((pid = fork()) == 0) {
            io.corrupt_read = bad == 0;
            io.fail_open_read = bad == 1;
            crash_point = "apkg_renamed";
            vjo_export_run(&A, 0, &ui, &r, msg, sizeof(msg));
            _exit(0);
        }
        TEST_ASSERT(pid > 0 && waitpid(pid, &status, 0) == pid);
        TEST_CHECK_(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "%s: the package was renamed to its final name before it was checked (exit %d)",
                    bad ? "the read-back cannot be opened" : "a byte reads back changed", WEXITSTATUS(status));
        TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".apkg") == 0 && count_dir(VJO_EXPORT_DIR, ".tmp") == 0 &&
                   count_dir(VJO_EXPORT_QUEUE_DIR, ".json") == 2);
    }
}

/* A Cancel at any progress step before the clearing (reading, writing, checking) makes no
 * file: none leaves a package of which no card left the queue, which a Cancel during the check
 * would, were the check to ignore it and only the clearing honour it (audit-overlay-apkg-b D2). */
static void test_a_cancel_while_checking_makes_no_file(void)
{
    VjoExportResult r;
    char msg[300];
    int total, cancelled = 0;
    reset();
    queue_card(&cat, 0);
    queue_card(&dog, 0);
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    total = statuses;
    for (int k = 1; k <= total + 1; k++) {
        reset();
        queue_card(&cat, 0);
        queue_card(&dog, 0);
        stop_at_status = k;
        int rc = run(0, &r, msg);
        cancelled += rc == VJO_E_CANCELLED;
        TEST_CHECK_(!(rc == VJO_OK && r.cards == 2 && r.moved == 0 && r.stopped == 2),
                    "stop at status %d of %d: a package was made, and both cards stay queued: %s", k, total, msg);
    }
    TEST_CHECK(cancelled > 0);
    stop_at_status = -1;
}

static int card_in(const char *dir, const char *id, const char *ext)
{
    char path[300];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s.%s", dir, id, ext);
    return stat(path, &st) == 0;
}

/* A card that cannot be read again while the package is written is left out
 * of it, and stays queued; the package's name counts the notes it holds. */
static void test_a_card_left_out_of_the_package_stays_queued(void)
{
    VjoExportResult r;
    char msg[300], ids[3][VJO_QUEUE_ID_SIZE];
    int queued = 0, archived = 0;
    reset();
    snprintf(ids[0], sizeof(ids[0]), "%s", queue_card(&cat, 0));
    snprintf(ids[1], sizeof(ids[1]), "%s", queue_card(&dog, 0));
    snprintf(ids[2], sizeof(ids[2]), "%s", queue_card(&eat, 0));
    /* three reads while listing, then one per card while writing: the fifth is the second written */
    io.fail_json_read_at = 5;
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_MSG("%s\n%s", msg, logs);
    for (int i = 0; i < 3; i++) {
        queued += card_in(VJO_EXPORT_QUEUE_DIR, ids[i], "json");
        archived += card_in(VJO_EXPORT_ARCHIVE_DIR, ids[i], "json");
    }
    TEST_CHECK(r.cards == 2 && r.too_large == 1 && r.moved == 2 && queued == 1 && archived == 2);
    TEST_CHECK(!strcmp(r.name, "2026-10-12_093005-2cards.apkg") && strstr(msg, "1 unreadable or too large stay"));
    TEST_MSG("%s: %s", r.name, msg);
    /* the next export takes it */
    io.fail_json_read_at = 0;
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 1 && r.moved == 1 && count_dir(VJO_EXPORT_QUEUE_DIR, ".json") == 0);
}

/* More cards than a package holds (VJO_APKG_MAX_CARDS): the oldest go, the
 * newest stay queued for the next export; real files, every one listed. */
static void test_more_cards_than_a_package_holds(void)
{
    enum { N = VJO_APKG_MAX_CARDS + 5 };
    VjoExportResult r;
    char msg[300], newest[5][VJO_QUEUE_ID_SIZE];
    reset();
    for (int i = 0; i < N; i++) {
        Card c = cat;
        char word[24];
        snprintf(word, sizeof(word), "猫%d", i);
        c.spelling = word;
        c.time = 1791000000 + (N - 1 - i) * 7 % N; /* saved out of order */
        vjo_arena_init(&A, mem, sizeof(mem));
        const char *id = queue_card(&c, 0);
        if (c.time >= 1791000000 + VJO_APKG_MAX_CARDS)
            snprintf(newest[c.time - 1791000000 - VJO_APKG_MAX_CARDS], VJO_QUEUE_ID_SIZE, "%s", id);
    }
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.cards == VJO_APKG_MAX_CARDS && r.left == 5 && r.moved == VJO_APKG_MAX_CARDS &&
               strstr(msg, "5 newer stay queued: export again."));
    TEST_MSG("%d cards, %d left, %d moved: %s", r.cards, r.left, r.moved, msg);
    for (int i = 0; i < 5; i++)
        TEST_CHECK(card_in(VJO_EXPORT_QUEUE_DIR, newest[i], "json"));
    printf("measure: %d cards: %u bytes, arena peak %u of %u\n", N, (unsigned)r.bytes, (unsigned)A.peak,
           (unsigned)sizeof(mem));
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 5 && r.left == 0 && r.moved == 5);
    /* everything again: a package holds the oldest VJO_APKG_MAX_CARDS, and says so */
    TEST_CHECK(run(1, &r, msg) == VJO_OK && r.cards == VJO_APKG_MAX_CARDS && r.left == 5 &&
               strstr(msg, "5 newer are not in it: a package holds 20000 cards at most."));
    TEST_MSG("%s", msg);
}

/* An interrupted export's temporary file, of any name, goes with the next. */
static void test_an_interrupted_exports_file_goes(void)
{
    VjoExportResult r;
    char msg[300];
    reset();
    queue_card(&cat, 0);
    mkdir(VJO_EXPORT_DIR, 0777);
    put_file(VJO_EXPORT_DIR "/2026-10-12_093004.apkg.tmp", "PK", 2); /* another second's */
    put_file(VJO_EXPORT_DIR "/2026-10-11_120000-all-3cards.apkg.tmp", "PK", 2); /* an older build's name */
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".tmp") == 0 && count_dir(VJO_EXPORT_DIR, ".apkg") == 1);
    TEST_CHECK(strstr(logs, "2026-10-12_093004.apkg.tmp: an interrupted export's, removed") != NULL);
    TEST_MSG("%s", logs);
}

/* Reads that return less than asked (any memory card may): still checked whole. */
static void test_short_reads_are_read_again(void)
{
    VjoExportResult r;
    char msg[300];
    reset();
    queue_thirty();
    io.short_reads = 1;
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 30 && r.moved == 30 && r.media == 6);
    TEST_MSG("%s\n%s", msg, logs);
}

/* A queued card whose record is archived already: the queued copy goes when the
 * archived one is the same record; a damaged one there is replaced by it. */
static void test_an_archived_copy_is_kept_only_when_it_is_the_card(void)
{
    VjoExportResult r;
    char msg[300], id[VJO_QUEUE_ID_SIZE], path[300], actual[VJO_QUEUE_ID_SIZE];
    FILE *f;
    size_t n;
    static char json[VJO_QUEUE_TEXT_MAX];
    reset();
    snprintf(id, sizeof(id), "%s", queue_card(&cat, 0));
    queue_card(&cat, 1); /* the same record, archived */
    TEST_ASSERT(run(0, &r, msg) == VJO_OK && r.moved == 1 && count_dir(VJO_EXPORT_QUEUE_DIR, ".json") == 0);
    reset();
    snprintf(id, sizeof(id), "%s", queue_card(&dog, 0));
    TEST_ASSERT(mkdir(VJO_EXPORT_ARCHIVE_DIR, 0777) == 0);
    snprintf(path, sizeof(path), VJO_EXPORT_ARCHIVE_DIR "/%s.json", id);
    put_file(path, "{\"damaged\":1}", 13);
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.moved == 1 && count_dir(VJO_EXPORT_QUEUE_DIR, ".json") == 0 &&
               strstr(logs, "the archived copy is damaged: replaced by the queued one") != NULL);
    TEST_ASSERT((f = fopen(path, "rb")) != NULL);
    n = fread(json, 1, sizeof(json), f);
    fclose(f);
    vjo_queue_id(json, n, actual);
    TEST_CHECK(!strcmp(actual, id)); /* the archive holds the good record now */
}

static void test_damaged_cards_stay_and_the_rest_are_exported(void)
{
    VjoExportResult r;
    VjoExportCounts c;
    char msg[300], path[256];
    const char *id;
    Card pic = eat;
    reset();
    queue_card(&cat, 0);
    /* an edited record: its checksum no longer matches its name */
    id = queue_card(&dog, 0);
    snprintf(path, sizeof(path), VJO_EXPORT_QUEUE_DIR "/%s.json", id);
    {
        FILE *f = fopen(path, "r+b");
        TEST_ASSERT(f != NULL);
        fseek(f, 20, SEEK_SET);
        fputc('X', f);
        fclose(f);
    }
    /* a card whose screenshot is missing */
    pic.picture = 5;
    id = queue_card(&pic, 0);
    snprintf(path, sizeof(path), VJO_EXPORT_QUEUE_DIR "/%s.jpg", id);
    TEST_ASSERT(unlink(path) == 0);
    /* not cards: a temp file, an archived duplicate, a name that is no ID */
    put_file(VJO_EXPORT_QUEUE_DIR "/0123456789abcdef0123456789abcdef.json.tmp", "{", 1);
    put_file(VJO_EXPORT_QUEUE_DIR "/0123456789abcdef0123456789abcdef.duplicate.json", v1, strlen(v1));
    put_file(VJO_EXPORT_QUEUE_DIR "/notes.json", v1, strlen(v1));
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.cards == 1 && r.moved == 1 && r.unreadable == 2);
    TEST_CHECK(strstr(msg, "2 unreadable or too large stay queued.") != NULL);
    TEST_MSG("%s", msg);
    TEST_CHECK(strstr(logs, "checksum or format mismatch") && strstr(logs, "is missing or not a JPEG"));
    vjo_export_count(&c);
    TEST_CHECK(c.queued == 2 && c.exported == 1); /* the two damaged ones */
    /* only damaged ones: nothing made */
    TEST_CHECK(run(0, &r, msg) == VJO_E_PARSE && strstr(msg, "2 cards could not be read"));
    TEST_CHECK(count_dir(VJO_EXPORT_DIR, ".apkg") == 1);
}

static void test_screenshots_are_one_file_each(void)
{
    VjoExportResult r;
    VjoZipEntry e[8];
    char msg[300];
    Card a = cat, b = dog, c = eat;
    reset();
    a.picture = 7;
    b.picture = 7; /* the same screenshot */
    c.picture = 8;
    queue_card(&a, 0);
    queue_card(&b, 0);
    queue_card(&c, 0);
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.cards == 3 && r.media == 2);
    TEST_CHECK(read_pkg(r.path, e, 8) == 4 && !strcmp(e[1].name, "0") && !strcmp(e[2].name, "1") &&
               !strcmp(e[3].name, "media") && e[1].size == 300);
    /* the screenshots go with their cards */
    TEST_CHECK(count_dir(VJO_EXPORT_QUEUE_DIR, ".jpg") == 0 && count_dir(VJO_EXPORT_ARCHIVE_DIR, ".jpg") == 3);
    /* all cards: their screenshots from the archive */
    TEST_CHECK(run(1, &r, msg) == VJO_OK && r.cards == 3 && r.media == 2);
}

/* A move that fails leaves the card queued; the next package has it again (the same GUID). */
static void test_a_failed_move_keeps_the_card_queued(void)
{
    VjoExportResult r;
    VjoExportCounts c;
    char msg[300];
    const char *id;
    char keep[VJO_QUEUE_ID_SIZE];
    reset();
    queue_card(&cat, 0);
    id = queue_card(&dog, 0);
    snprintf(keep, sizeof(keep), "%s", id);
    io.fail_rename = keep;
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.cards == 2 && r.moved == 1 && r.kept == 1 && strstr(msg, "1 stay queued"));
    vjo_export_count(&c);
    TEST_CHECK(c.queued == 1 && c.exported == 1);
    io.fail_rename = NULL;
    TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards == 1 && r.moved == 1);
    vjo_export_count(&c);
    TEST_CHECK(c.queued == 0 && c.exported == 2);
    /* a card in both places (a copy left by an interrupted move) is one note, and the queued copy goes */
    queue_card(&cat, 0);
    TEST_CHECK(run(1, &r, msg) == VJO_OK && r.cards == 2 && r.moved == 1);
    vjo_export_count(&c);
    TEST_CHECK(c.queued == 0 && c.exported == 2);
}

/* ---------------- #161: screenshots no card names ---------------- */

/* A card's JPEG is committed before the record that names it (shell/anki_queue.c), so a stop between
 * the two, or during the JPEG's own write (its .jpg.tmp), leaves a picture no card names. Once a
 * package is verified, the export removes these from anki_queue/ (never exported/): a .jpg no record
 * names, queued, exported or an earlier build's .duplicate.json, and every .jpg.tmp; each only when it
 * is at least 10 minutes older than that package (VJO_EXPORT_ORPHAN_AGE_S): a save still finishing
 * has its record within seconds of its JPEG. */
#define Q VJO_EXPORT_QUEUE_DIR "/"
#define X VJO_EXPORT_ARCHIVE_DIR "/"
#define HOURS(n) ((n) * 3600L)
#define MINUTES(n) ((n) * 60L)

static int there(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* A file's time set to age seconds ago (negative: ahead). */
static void age_file(const char *path, long age)
{
    struct timespec t[2];
    TEST_ASSERT(clock_gettime(CLOCK_REALTIME, &t[0]) == 0);
    t[0].tv_sec -= age;
    t[0].tv_nsec = 0;
    t[1] = t[0];
    TEST_ASSERT(utimensat(AT_FDCWD, path, t, 0) == 0);
}

/* A screenshot-like file of that age. */
static void put_aged(const char *path, long age)
{
    uint8_t pic[300];
    put_file(path, pic, jpeg(pic, 3, sizeof(pic)));
    age_file(path, age);
}

/* log.txt's lines for removed orphans. */
static int orphans_logged(void)
{
    int n = 0;
    for (const char *p = logs; (p = strstr(p, ": no card names it, removed")); p++)
        n++;
    return n;
}

static void test_screenshots_no_card_names_go_after_a_verified_package(void)
{
    static const char *const gone[] = {
        Q "0123456789abcdef0123456789abcd01.jpg",     /* a save stopped between its JPEG and its record */
        Q "0123456789abcdef0123456789abcd02.jpg.tmp", /* a JPEG's write that stopped */
        Q "0123456789abcdef0123456789abcd03.jpg",     /* 11 minutes old */
    };
    static const long gone_age[] = {HOURS(2), HOURS(2), MINUTES(11)};
    static const char *const kept[] = {
        Q "0123456789abcdef0123456789abcd11.jpg",     /* an earlier build's card AnkiConnect had: kept */
        X "0123456789abcdef0123456789abcd12.jpg",     /* exported/ is never swept (a move stopped half way) */
        Q "0123456789abcdef0123456789abcd13.jpg",     /* a minute old: its record may still come */
        Q "0123456789abcdef0123456789abcd14.jpg",     /* 9 minutes */
        Q "0123456789abcdef0123456789abcd15.jpg.tmp", /* a minute: a JPEG still being written */
        Q "0123456789abcdef0123456789abcd16.jpg",     /* a day ahead (the clock set back since) */
        Q "0123456789abcdef0123456789abcd17.jpg",     /* 1970: no time */
        Q "notes.jpg",                                /* not a card's name */
        Q "0123456789abcdef0123456789abcd1.jpg",
        Q "0123456789ABCDEF0123456789ABCD18.jpg",
        Q "0123456789abcdef0123456789abcd19.JPG",
        Q "0123456789abcdef0123456789abcd20.jpg.bak",
        Q "0123456789abcdef0123456789abcd21.jpeg",
        Q "0123456789abcdef0123456789abcd22.jpg.tmp.old",
        Q "0123456789abcdef0123456789abcd23.jpg/0123456789abcdef0123456789abcd24.jpg", /* in a directory */
    };
    static const long kept_age[] = {HOURS(2), HOURS(2), MINUTES(1), MINUTES(9), MINUTES(1), -HOURS(24), 0,
                                    HOURS(2), HOURS(2), HOURS(2), HOURS(2), HOURS(2), HOURS(2), HOURS(2),
                                    HOURS(2)};
    VjoExportResult r;
    char msg[300], path[300], cat_jpg[300], bad_jpg[300], bad_tmp[300], copy_jpg[300];
    const char *id;
    Card pic = cat, bad = eat, archived = dog;
    reset();
    TEST_ASSERT(mkdir(VJO_EXPORT_ARCHIVE_DIR, 0777) == 0);
    /* the queue's cards: one with its screenshot, one without, one damaged with its screenshot */
    pic.picture = 4;
    id = queue_card(&pic, 0);
    snprintf(cat_jpg, sizeof(cat_jpg), X "%s.jpg", id);
    queue_card(&dog, 0);
    bad.picture = 6;
    id = queue_card(&bad, 0);
    snprintf(path, sizeof(path), Q "%s.json", id);
    {
        FILE *f = fopen(path, "r+b"); /* edited: its checksum no longer its name, so it stays queued */
        TEST_ASSERT(f != NULL);
        fseek(f, 30, SEEK_SET);
        fputc('Y', f);
        fclose(f);
    }
    age_file(path, HOURS(2));
    snprintf(bad_jpg, sizeof(bad_jpg), Q "%s.jpg", id);
    age_file(bad_jpg, HOURS(2));
    snprintf(bad_tmp, sizeof(bad_tmp), Q "%s.jpg.tmp", id); /* beside its card's record: never a picture */
    put_aged(bad_tmp, HOURS(2));
    /* an exported card whose JPEG copy stayed beside the queue (its record is in exported/) */
    archived.spelling = "犬小屋";
    id = queue_card(&archived, 1);
    snprintf(copy_jpg, sizeof(copy_jpg), Q "%s.jpg", id);
    put_aged(copy_jpg, HOURS(2));
    put_file(Q "0123456789abcdef0123456789abcd11.duplicate.json", v1, strlen(v1));
    TEST_ASSERT(mkdir(Q "0123456789abcdef0123456789abcd23.jpg", 0777) == 0); /* a directory of that name */
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++)
        put_aged(gone[i], gone_age[i]);
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        put_aged(kept[i], kept_age[i]);
    {
        struct timespec t[2] = {{0, 0}, {0, 0}}; /* 1970-01-01 */
        TEST_ASSERT(utimensat(AT_FDCWD, kept[6], t, 0) == 0);
    }
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_MSG("%s\n%s", msg, logs);
    TEST_CHECK(r.cards == 2 && r.moved == 2 && r.unreadable == 1);
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++)
        TEST_CHECK_(!there(gone[i]), "%s is still there", gone[i]);
    TEST_CHECK_(!there(bad_tmp), "%s is still there", bad_tmp);
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        TEST_CHECK_(there(kept[i]), "%s was removed", kept[i]);
    /* the cards' own: exported, still queued (damaged), and a copy whose record is exported */
    TEST_CHECK(there(cat_jpg) && there(bad_jpg) && there(copy_jpg));
    TEST_CHECK_(orphans_logged() == 4, "%d removals logged:\n%s", orphans_logged(), logs);
    /* the next package finds none left */
    logs[0] = '\0';
    TEST_CHECK(run(1, &r, msg) == VJO_OK && r.cards == 3 && orphans_logged() == 0);
    TEST_MSG("%s\n%s", msg, logs);
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        TEST_CHECK_(there(kept[i]), "%s was removed by the second export", kept[i]);
    TEST_CHECK(there(cat_jpg) && there(bad_jpg) && there(copy_jpg));
}

/* No verified package, nothing removed: no card to export, a failed write, a Cancel, a Cancel while
 * the queue is cleared. The next export that makes one removes them. */
static void test_only_a_verified_package_removes_them(void)
{
    static const char *const orphans[] = {Q "0123456789abcdef0123456789abcd01.jpg",
                                          Q "0123456789abcdef0123456789abcd02.jpg.tmp"};
    VjoExportResult r;
    char msg[300];
    int clearing;
    /* a Cancel at the clearing's second status stops it between two cards */
    reset();
    queue_thirty();
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    clearing = statuses - 2;
    for (int k = 0; k < 4; k++) {
        int rc;
        reset();
        if (k)
            queue_thirty();
        for (int i = 0; i < 2; i++)
            put_aged(orphans[i], HOURS(2));
        io.write_after = k == 1 ? 4096 : -1;              /* the memory card full */
        stop_at_status = k == 2 ? 1 : k == 3 ? clearing : -1; /* a Cancel while reading, while clearing */
        rc = run(0, &r, msg);
        TEST_CHECK_(k == 0 ? rc == VJO_OK && !r.cards : k == 3 ? rc == VJO_OK && r.stopped > 0 : rc != VJO_OK,
                    "case %d: rc %d, %d cards, %d stopped: %s", k, rc, r.cards, r.stopped, msg);
        for (int i = 0; i < 2; i++)
            TEST_CHECK_(there(orphans[i]), "case %d: %s removed without a verified package", k, orphans[i]);
        TEST_CHECK(orphans_logged() == 0);
        /* then a complete export removes them */
        stop_at_status = -1;
        io.write_after = -1;
        queue_card(&cat, 0);
        TEST_CHECK(run(0, &r, msg) == VJO_OK && r.cards >= 1);
        for (int i = 0; i < 2; i++)
            TEST_CHECK_(!there(orphans[i]), "case %d: %s left by the next export", k, orphans[i]);
        TEST_MSG("%s\n%s", msg, logs);
    }
}

/* A record that cannot be checked (a getstat that fails other than "missing") keeps its picture; a
 * removal that fails is logged, changes nothing else, and the next package's export removes it. */
static void test_an_uncheckable_record_keeps_its_picture(void)
{
    VjoExportResult r;
    char msg[300];
    reset();
    queue_card(&cat, 0);
    put_aged(Q "0123456789abcdef0123456789abcd01.jpg", HOURS(2));
    put_aged(Q "0123456789abcdef0123456789abcd02.jpg", HOURS(2));
    put_aged(Q "0123456789abcdef0123456789abcd03.jpg", HOURS(2));
    io.fail_stat = "/exported/0123456789abcdef0123456789abcd01.json";
    io.fail_remove = "0123456789abcdef0123456789abcd02.jpg";
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.cards == 1 && r.moved == 1 && strstr(msg, "Exported 1 card") && !strstr(msg, "stay"));
    TEST_MSG("%s\n%s", msg, logs);
    TEST_CHECK(there(Q "0123456789abcdef0123456789abcd01.jpg")); /* its record unknown: kept */
    TEST_CHECK(there(Q "0123456789abcdef0123456789abcd02.jpg")); /* the removal failed */
    TEST_CHECK(!there(Q "0123456789abcdef0123456789abcd03.jpg"));
    TEST_CHECK(strstr(logs, "anki export: remove " Q "0123456789abcdef0123456789abcd02.jpg failed 0x80010005") != NULL);
    io.fail_stat = io.fail_remove = NULL;
    queue_card(&dog, 0);
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(!there(Q "0123456789abcdef0123456789abcd01.jpg") && !there(Q "0123456789abcdef0123456789abcd02.jpg"));
}

/* Many cards: interior B-tree pages, notes on overflow pages, more than one level. */
static void test_large_queue(void)
{
    VjoExportResult r;
    char msg[300], big[9000];
    reset();
    for (size_t i = 0; i + 4 < sizeof(big); i += 3)
        memcpy(big + i, "語", 3);
    big[sizeof(big) - 4] = '\0';
    for (int i = 0; i < 700; i++) {
        Card c = eat;
        char word[24];
        snprintf(word, sizeof(word), "食べる%d", i);
        c.spelling = word;
        c.time = 1791738720 + i / 3; /* three cards a second */
        c.meaning = i % 50 == 0 ? big : c.meaning;
        queue_card(&c, 0);
    }
    TEST_ASSERT(run(0, &r, msg) == VJO_OK);
    TEST_CHECK(r.cards == 700 && r.moved == 700);
    TEST_MSG("%s\n%s", msg, logs);
    printf("measure: 700 cards: %u bytes, arena peak %u of %u\n", (unsigned)r.bytes, (unsigned)A.peak,
           (unsigned)sizeof(mem));
}

/* Over VJO_APKG_MAX_CARDS: the oldest go now, the rest stay for the next export. */
/* ---------------- packages for host/tests/test_apkg.py ---------------- */

/* The package, copied out of the test's directory (in /tmp) and removed there:
 * a rename() would fail when DIR is on another file system (EXDEV). */
static int take_package(const char *from, const char *to)
{
    char buf[65536];
    size_t n;
    int ok = 1;
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    if (!out) {
        if (in)
            fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok &= fwrite(buf, 1, n, out) == n;
    ok &= !ferror(in);
    fclose(in);
    ok &= fclose(out) == 0;
    return ok && remove(from) == 0 ? 0 : -1;
}

static int write_packages(const char *dir)
{
    VjoExportResult r;
    char msg[300], path[PATH_MAX + 32];
    FILE *f;
    Card a = cat, b = dog, c = eat;
    const char *ids[8];
    char saved[8][VJO_QUEUE_ID_SIZE];
    int n = 0;
    reset();
    /* first: three cards, one with a screenshot (a real JPEG, the game worker's encoder's: API 23);
     * v1 and v2 records too */
    a.picture = REAL_PICTURE;
    ids[0] = queue_card(&a, 0);
    snprintf(saved[n++], VJO_QUEUE_ID_SIZE, "%s", ids[0]);
    snprintf(saved[n++], VJO_QUEUE_ID_SIZE, "%s", queue_card(&b, 0));
    snprintf(saved[n++], VJO_QUEUE_ID_SIZE, "%s", queue_json(v1, 0));
    snprintf(saved[n++], VJO_QUEUE_ID_SIZE, "%s", queue_json(v2, 0));
    if (run(0, &r, msg) != VJO_OK)
        return fprintf(stderr, "%s\n%s", msg, logs), 1;
    snprintf(path, sizeof(path), "%s/first.apkg", dir);
    if (take_package(r.path, path))
        return 1;
    /* then two more, one sharing the first's screenshot */
    c.picture = REAL_PICTURE;
    snprintf(saved[n++], VJO_QUEUE_ID_SIZE, "%s", queue_card(&c, 0));
    b.spelling = "犬小屋";
    b.reading = "いぬごや";
    b.time += 60;
    snprintf(saved[n++], VJO_QUEUE_ID_SIZE, "%s", queue_card(&b, 0));
    if (run(0, &r, msg) != VJO_OK)
        return fprintf(stderr, "%s\n%s", msg, logs), 1;
    snprintf(path, sizeof(path), "%s/second.apkg", dir);
    if (take_package(r.path, path))
        return 1;
    /* all of them */
    if (run(1, &r, msg) != VJO_OK)
        return fprintf(stderr, "%s\n%s", msg, logs), 1;
    snprintf(path, sizeof(path), "%s/all.apkg", dir);
    if (take_package(r.path, path))
        return 1;
    /* many: a deep B-tree */
    for (int i = 0; i < 700; i++) {
        Card x = eat;
        char word[24];
        snprintf(word, sizeof(word), "食べる%d", i);
        x.spelling = word;
        x.time = 1791800000 + i / 3;
        queue_card(&x, 0);
    }
    if (run(0, &r, msg) != VJO_OK)
        return fprintf(stderr, "%s\n%s", msg, logs), 1;
    snprintf(path, sizeof(path), "%s/many.apkg", dir);
    if (take_package(r.path, path))
        return 1;
    /* what the first two must hold: each card's ID (its GUID's source) */
    snprintf(path, sizeof(path), "%s/expected.json", dir);
    if (!(f = fopen(path, "w")))
        return 1;
    fprintf(f, "{\"first\":[");
    for (int i = 0; i < 4; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", saved[i]);
    fprintf(f, "],\"second\":[");
    for (int i = 4; i < n; i++)
        fprintf(f, "%s\"%s\"", i > 4 ? "," : "", saved[i]);
    fprintf(f, "],\"many\":700,\"notetype_id\":%lld,\"deck_id\":%lld,\"deck\":\"%s\"}\n", vjo_lapis_notetype_id,
            vjo_lapis_deck_id, vjo_lapis_deck_name);
    fclose(f);
    return 0;
}

TEST_LIST = {
    {"sqlite_records_and_varints", test_sqlite_records_and_varints},
    {"zip_crc_and_reader", test_zip_crc_and_reader},
    {"guid_and_media_name", test_guid_and_media_name},
    {"lapis_fields", test_lapis_fields},
    {"export_writes_a_dated_verified_package_and_clears_the_queue",
     test_export_writes_a_dated_verified_package_and_clears_the_queue},
    {"one_card_and_every_record_version", test_one_card_and_every_record_version},
    {"a_failed_write_never_clears_the_queue", test_a_failed_write_never_clears_the_queue},
    {"a_stop_anywhere_loses_no_card", test_a_stop_anywhere_loses_no_card},
    {"a_package_that_reads_back_wrong_never_gets_its_name", test_a_package_that_reads_back_wrong_never_gets_its_name},
    {"a_cancel_while_checking_makes_no_file", test_a_cancel_while_checking_makes_no_file},
    {"a_card_left_out_of_the_package_stays_queued", test_a_card_left_out_of_the_package_stays_queued},
    {"more_cards_than_a_package_holds", test_more_cards_than_a_package_holds},
    {"an_interrupted_exports_file_goes", test_an_interrupted_exports_file_goes},
    {"short_reads_are_read_again", test_short_reads_are_read_again},
    {"an_archived_copy_is_kept_only_when_it_is_the_card", test_an_archived_copy_is_kept_only_when_it_is_the_card},
    {"damaged_cards_stay_and_the_rest_are_exported", test_damaged_cards_stay_and_the_rest_are_exported},
    {"screenshots_are_one_file_each", test_screenshots_are_one_file_each},
    {"a_failed_move_keeps_the_card_queued", test_a_failed_move_keeps_the_card_queued},
    {"screenshots_no_card_names_go_after_a_verified_package", test_screenshots_no_card_names_go_after_a_verified_package},
    {"only_a_verified_package_removes_them", test_only_a_verified_package_removes_them},
    {"an_uncheckable_record_keeps_its_picture", test_an_uncheckable_record_keeps_its_picture},
    {"large_queue", test_large_queue},
    {NULL, NULL},
};

/* ---------------- SQLite files of every tree shape, for test_apkg.py ---------------- */

/* DIR: one table t(id INTEGER PRIMARY KEY, v TEXT) per file, rows-<N>-<BYTES>.sqlite: N
 * rows of BYTES of text each. v of row r is byte i: 'a' + (r * 7 + i) % 26. 2,100-byte rows fill
 * a leaf each (two do not fit), so N is the number of leaves: one interior page holds up to
 * VJO_SQLW_FANOUT (273) children, so 273 and 274, 546 to 548 cover one and more interior pages
 * on a level; rows from 4,062 bytes have overflow chains; 10,000 small rows share leaves. SQLite
 * itself checks them (PRAGMA integrity_check). */
static int sqlw_file_write(void *ctx, uint64_t off, const void *src, size_t n)
{
    FILE *f = (FILE *)ctx;
    return fseeko(f, (off_t)off, SEEK_SET) || fwrite(src, 1, n, f) != n ? -1 : 0;
}

static int write_sqlw_shapes(const char *dir)
{
    static const struct {
        int rows;
        size_t bytes;
    } shapes[] = {{0, 10},     {1, 10},       {1, 2100},  {272, 2100}, {273, 2100},  {274, 2100}, {275, 2100},
                  {546, 2100}, {547, 2100},   {548, 2100}, {3, 4057},  {3, 4058},    {3, 4062},   {2, 8000},
                  {2, 100000}, {10000, 10},   {600, 900}};
    static VjoSqlw w;
    static VjoSqlwTable t;
    static char text[100000];
    for (unsigned k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
        char path[600];
        uint32_t pages = 0;
        int rc;
        FILE *f;
        VjoSqlwOut out;
        snprintf(path, sizeof(path), "%s/rows-%d-%u.sqlite", dir, shapes[k].rows, (unsigned)shapes[k].bytes);
        if (!(f = fopen(path, "w+b")))
            return 1;
        out.ctx = f;
        out.write = sqlw_file_write;
        vjo_sqlw_init(&w, &out);
        rc = vjo_sqlw_table_begin(&w, &t, "t", "CREATE TABLE t (id integer primary key, v text not null)");
        for (int r = 1; !rc && r <= shapes[k].rows; r++) {
            VjoSqlwVal v[2] = {{VJO_SQLW_NULL, 0, NULL, 0}, {VJO_SQLW_TEXT, 0, text, shapes[k].bytes}};
            uint8_t *rec;
            size_t len;
            for (size_t i = 0; i < shapes[k].bytes; i++)
                text[i] = (char)('a' + ((size_t)r * 7u + i) % 26u);
            vjo_arena_init(&A, mem, sizeof(mem));
            rc = vjo_sqlw_record(&A, v, 2, &rec, &len);
            if (!rc)
                rc = vjo_sqlw_row(&w, r, rec, len);
        }
        if (!rc)
            rc = vjo_sqlw_table_end(&w);
        if (!rc)
            rc = vjo_sqlw_finish(&w, &pages);
        if (fclose(f) || rc) {
            fprintf(stderr, "%s: rc %d\n", path, rc);
            return 1;
        }
        printf("%s %u pages\n", path, (unsigned)pages);
    }
    return 0;
}

/* ---------------- restart steps (test_anki_restart.py) ---------------- */

/* DIR ACTION [CRASH]: queue (five cards, two with the same screenshot),
 * export, export-all, or inspect (what the queue, the archive and the
 * packages hold, and every complete package's GUIDs). Prints one JSON line. */
static int restart_step(int argc, char **argv)
{
    const char *what;
    VjoExportResult r;
    char msg[300];
    if (argc < 4 || chdir(argv[2]) < 0)
        return 2;
    what = argv[3];
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(VJO_EXPORT_QUEUE_DIR, 0777);
    if (argc > 4 && argv[4][0])
        crash_point = argv[4];
    memset(&io, 0, sizeof(io));
    io.write_after = -1;
    io.fd = -1;
    if (!strcmp(what, "queue")) {
        static const Card *base[] = {&cat, &dog, &eat};
        for (int i = 0; i < 5; i++) {
            Card c = *base[i % 3];
            char word[24];
            snprintf(word, sizeof(word), "%s%d", c.spelling, i);
            c.spelling = word;
            c.time += i;
            c.picture = i == 1 || i == 3 ? 9 : 0;
            vjo_arena_init(&A, mem, sizeof(mem));
            printf("%s\"%s\"", i ? "," : "{\"ids\":[", queue_card(&c, 0));
        }
        puts("]}");
        return 0;
    }
    if (!strcmp(what, "export") || !strcmp(what, "export-all")) {
        int rc = run(!strcmp(what, "export-all"), &r, msg);
        printf("{\"rc\":%d,\"cards\":%d,\"moved\":%d,\"kept\":%d,\"name\":\"%s\"}\n", rc, r.cards, r.moved,
               r.kept, r.name);
        return 0;
    }
    if (!strcmp(what, "inspect")) {
        VjoExportCounts c;
        DIR *d;
        struct dirent *e;
        int first = 1;
        vjo_export_count(&c);
        printf("{\"queued\":%d,\"exported\":%d,\"packages\":[", c.queued, c.exported);
        if ((d = opendir(VJO_EXPORT_DIR))) {
            while ((e = readdir(d)))
                if (e->d_name[0] != '.') {
                    printf("%s\"%s\"", first ? "" : ",", e->d_name);
                    first = 0;
                }
            closedir(d);
        }
        puts("]}");
        return 0;
    }
    return 2;
}

static char test_dir[128];
static pid_t test_dir_owner;
static void remove_test_dir(void)
{
    char cmd[200];
    if (getpid() != test_dir_owner) /* (acutest runs each test in a child) */
        return;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", test_dir);
}

int main(int argc, char **argv)
{
    char out[PATH_MAX] = ""; /* (realpath's buffer: glibc's _FORTIFY_SOURCE requires PATH_MAX) */
    if (argc >= 2 && !strcmp(argv[1], "--restart-step"))
        return restart_step(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "--write") && !realpath(argv[2], out))
        return 2;
    if (argc == 3 && !strcmp(argv[1], "--sqlw"))
        return write_sqlw_shapes(argv[2]);
    snprintf(test_dir, sizeof(test_dir), "/tmp/vjo-apkg-tests-XXXXXX");
    if (!mkdtemp(test_dir))
        return 2;
    test_dir_owner = getpid();
    atexit(remove_test_dir);
    if (chdir(test_dir) < 0 || mkdir("ux0:data", 0777) < 0 || mkdir("ux0:data/VitaJPOverlay", 0777) < 0)
        return 2;
    if (out[0])
        return write_packages(out);
    return acutest_main(argc, argv);
}
