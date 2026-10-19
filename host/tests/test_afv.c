/* The Rubi app's side of Anki for Vita (settings/anki_afv.c, #220), on real files in a temporary
 * memory card, through the Vita file API mapped onto POSIX:
 *   - the rename refuses an existing destination, as the Vita's sceIoRename does
 *     (SCE_ERROR_ERRNO_EEXIST), so a file that would be replaced shows as a refused rename;
 *   - any one storage call (an open for writing, a write, a flush, a close, a rename, a removal, a
 *     sync, a mkdir) can be made to fail, or the process stopped just before it (a power cut's
 *     visible state: what was renamed is there, nothing else changes);
 *   - every read of the outbox is recorded, so a file that must not be read is seen not to be.
 * The queue's records are written as the overlay's Enter writes them (vjo_queue_encode), and each
 * note is compared with what the .apkg export makes of the same card (the GUID, every Lapis field).
 *
 * `--write DIR` writes a queue, its inbox and its .apkg into DIR for host/tests/test_afv.py, which
 * compares each inbox note with the package's note (GUID, fields, tags, media, note type) with
 * Python's zipfile, sqlite3 and json, and checks the notes against Anki for Vita's interface rules. */
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
#include "test_tmpdir.h"
#include "anki_queue.h"
#include "apkg.h"
#include "errors.h"
#include "json.h"
#include "lapis.h"
#include "../../settings/anki_afv.h"
#include "../../settings/anki_export.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#define EIO_RC ((int)0x80010005u)
#define ENOENT_RC ((int)0x80010002u)
#define EEXIST_RC ((int)0x80010011u)
#define ROOT VJO_AFV_DIR
#define INBOX VJO_AFV_INBOX_DIR
#define OUTBOX VJO_AFV_OUTBOX_DIR

/* ---------------- the memory card ---------------- */

static char logs[32768];
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

static struct {
    int calls;            /* storage calls that change the card, so far */
    int fail_at;          /* that call fails (0: none) */
    int stop_at;          /* the process stops before that call (0: none), exit status 86 */
    const char *fail_read; /* an open for reading of a path containing this fails (EIO) */
    const char *fail_rename; /* a rename whose source contains this fails (EIO) */
    int delays;           /* the readers' waits (sceKernelDelayThread) */
    const char *appear_at_delay; /* a file that a writer puts in place during the first wait */
    const char *appear_text;
    int refused;          /* renames refused: the destination was there */
    int outbox_reads;     /* opens of files under the outbox */
    char read_paths[64][200];
    int syncs;
} io;

static int native(int rc) { return rc < 0 ? (errno == ENOENT ? ENOENT_RC : errno == EEXIST ? EEXIST_RC : EIO_RC) : rc; }

/* A call that changes the card: counted, and stopped or failed when the test says. */
static int change(void)
{
    io.calls++;
    if (io.stop_at && io.calls == io.stop_at)
        _exit(86);
    return io.fail_at && io.calls == io.fail_at;
}

int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (flags & (O_WRONLY | O_RDWR)) {
        if (change())
            return EIO_RC;
    } else {
        if (io.fail_read && strstr(p, io.fail_read))
            return EIO_RC;
        if (strstr(p, OUTBOX "/") && io.outbox_reads < 64)
            snprintf(io.read_paths[io.outbox_reads++], sizeof(io.read_paths[0]), "%s", p);
    }
    return native(open(p, flags, mode));
}
static int fail_close;
int sceIoClose(int fd)
{
    int rc = close(fd);
    if (fail_close) { /* (the close of a file written: fail_close set by the write) */
        fail_close = 0;
        return EIO_RC;
    }
    return native(rc);
}
int sceIoRead(int fd, void *p, size_t n) { return native((int)read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n)
{
    if (change())
        return EIO_RC;
    if (n > 777) /* short writes: the writer loops */
        n = 777;
    return native((int)write(fd, p, n));
}
int sceIoPread(int fd, void *p, size_t n, SceOff off) { return native((int)pread(fd, p, n, off)); }
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off) { return native((int)pwrite(fd, p, n, off)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode)
{
    if (change())
        return EIO_RC;
    return native(mkdir(p, mode));
}
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
        return native(-1);
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    date_of(s.st_ctim.tv_sec, &st->st_ctime);
    date_of(s.st_atim.tv_sec, &st->st_atime);
    date_of(s.st_mtim.tv_sec, &st->st_mtime);
    return 0;
}
int sceIoGetstat(const char *p, SceIoStat *st) { return file_stat(p, st); }
int sceIoRename(const char *from, const char *to)
{
    struct stat s;
    if (change() || (io.fail_rename && strstr(from, io.fail_rename)))
        return EIO_RC;
    if (!stat(to, &s)) { /* the Vita's rename refuses an existing name */
        io.refused++;
        return EEXIST_RC;
    }
    return native(rename(from, to));
}
int sceIoRemove(const char *p)
{
    if (change())
        return EIO_RC;
    return native(unlink(p));
}
int sceIoSyncByFd(int fd, int flag)
{
    (void)flag;
    if (change())
        return EIO_RC;
    /* (no fsync: a stopped process's writes stay in the page cache, so a stop's state is the same;
     * the host's fsync would only slow the suite) */
    return fcntl(fd, F_GETFD) < 0 ? EIO_RC : 0;
}
int sceIoSync(const char *device, unsigned flags)
{
    (void)device, (void)flags;
    if (change())
        return EIO_RC;
    io.syncs++;
    return 0;
}
static DIR *dirs[4];
static char dir_paths[4][256];
int sceIoDopen(const char *p)
{
    for (int i = 0; i < 4; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p)))
                return native(-1);
            snprintf(dir_paths[i], sizeof(dir_paths[i]), "%s", p);
            return 100 + i;
        }
    return EIO_RC;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e = readdir(dirs[fd - 100]);
    char path[600];
    if (!e)
        return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", dir_paths[fd - 100], e->d_name);
    if (file_stat(path, &ent->d_stat) < 0)
        memset(&ent->d_stat, 0, sizeof(ent->d_stat));
    return 1;
}
int sceIoDclose(int fd)
{
    int rc = closedir(dirs[fd - 100]);
    dirs[fd - 100] = NULL;
    return rc;
}
static void put_text(const char *path, const char *text);
/* A reader's wait: counted, not slept; a writer may finish a replace meanwhile. */
int sceKernelDelayThread(unsigned us)
{
    (void)us;
    if (io.delays++ == 0 && io.appear_at_delay)
        put_text(io.appear_at_delay, io.appear_text);
    return 0;
}
/* 2026-10-12 09:30:05 on the console, 1791790205 s UTC */
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    *t = (SceDateTime){2026, 10, 12, 9, 30, 5, 0};
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = 62135596800000000ull + 1791790205ull * 1000000ull;
    return 0;
}

/* ---------------- files ---------------- */

static void put_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_(f != NULL, "%s", path);
    TEST_ASSERT(fwrite(data, 1, len, f) == len);
    fclose(f);
}
static void put_text(const char *path, const char *text) { put_file(path, text, strlen(text)); }

/* A whole file (NUL-terminated) in a static buffer of its own; its length, or -1. */
static long get_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return (long)n;
}
static int there(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}
static int files_in(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && strlen(e->d_name) >= strlen(suffix) &&
            !strcmp(e->d_name + strlen(e->d_name) - strlen(suffix), suffix))
            n++;
    closedir(d);
    return n;
}

/* ---------------- the queue ---------------- */

static uint8_t mem[VJO_EXPORT_MEM] __attribute__((aligned(16)));
static VjoArena A;
static int stop_after = -1, statuses, stop_checks;
static void ui_status(void *ud, const char *msg) { (void)ud, (void)msg; statuses++; }
/* Cancel: true from the (stop_after + 1)th question on. */
static int ui_stopped(void *ud) { (void)ud; return stop_after >= 0 && ++stop_checks > stop_after; }
static const VjoExportUi ui = {NULL, ui_status, ui_stopped};

typedef struct {
    const char *spelling, *reading, *sentence, *meaning, *dictionary, *misc;
    int hl_start, hl_end, rank, picture, pitch;
    int64_t time;
    const char *source, *more_dict, *more_meaning;
} Card;

static const Card cat = {"猫", "ねこ", "猫が好き", "cat", "JMdict", "SOME GAME (2026-10-11 17:11)", 0, 3, 1234, 1, 1,
                         1791738660, "Jiten", NULL, NULL};
static const Card dog = {"犬", "いぬ", "「犬だ！」\nと言った", "dog", "JMdict", "SOME GAME (2026-10-11 17:12)", 3, 6, 0, 2,
                         0, 1791738720, NULL, "Daijirin4", "いぬ。四足の哺乳類。"};
static const Card eat = {"食べる", "たべる", "パンを食べた", "to eat <&> \"quoted\" \x1F unit", "Sankoku8",
                         "SOME GAME (2026-10-11 17:12)", 9, 15, 0, 0, 3, 1791738720, NULL, NULL, NULL};

/* A screenshot: JPEG markers around a pattern (nothing decodes it). */
static size_t jpeg_of(uint8_t *out, int seed, size_t len)
{
    out[0] = 0xFF, out[1] = 0xD8, out[2] = 0xFF, out[3] = 0xE0;
    for (size_t i = 4; i + 2 < len; i++)
        out[i] = (uint8_t)(i * 31u + (unsigned)seed * 7u);
    out[len - 2] = 0xFF, out[len - 1] = 0xD9;
    return len;
}

/* The record of a card, as the overlay's Enter writes it (version 4), into the queue; its ID. */
static const char *queue_card(const Card *c, char id[VJO_QUEUE_ID_SIZE])
{
    const char *m[1] = {c->meaning}, *more_m[1] = {c->more_meaning}, *pos[1] = {"n"};
    VjoAnkiGloss more = {c->more_dict, c->spelling, c->reading, more_m, 1};
    VjoAnkiNote n;
    size_t mark = vjo_arena_mark(&A);
    char *json, path[256];
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
    n.sf_hl_start = n.sf_hl_end = -1;
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
    n.game = "SOME GAME";
    n.title_id = "PCSG00000";
    n.platform = "vita";
    n.unix_time = c->time;
    n.ocr_engine = "vocr";
    n.ocr_model = "VOCR5_N_w8.vocr";
    TEST_ASSERT((json = vjo_queue_encode(&A, &n, c->picture)) != NULL);
    vjo_queue_id(json, strlen(json), id);
    mkdir(VJO_QUEUE_DIR, 0777);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    put_text(path, json);
    if (c->picture) {
        uint8_t pic[3000];
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        put_file(path, pic, jpeg_of(pic, c->picture, sizeof(pic)));
    }
    vjo_arena_release(&A, mark);
    return id;
}

static void installed(const char *interface_json)
{
    mkdir(ROOT, 0777);
    put_text(ROOT "/interface.json", interface_json);
}

static void reset(void)
{
    char data[PATH_MAX];
    memset(&io, 0, sizeof(io));
    fail_close = 0;
    logs[0] = '\0';
    stop_after = -1;
    statuses = stop_checks = 0;
    TEST_ASSERT(snprintf(data, sizeof(data), "%s/ux0:data", vjo_test_tmpdir_path) < (int)sizeof(data));
    TEST_ASSERT(vjo_test_remove_tree(data) == 0);
    TEST_ASSERT(mkdir("ux0:data", 0777) == 0 && mkdir("ux0:data/VitaJPOverlay", 0777) == 0);
    TEST_ASSERT(mkdir(VJO_QUEUE_DIR, 0777) == 0);
    memset(mem, 0xA5, sizeof(mem));
    vjo_arena_init(&A, mem, sizeof(mem));
}

static int send_now(VjoAfvSendResult *r, char *msg, size_t cap)
{
    vjo_arena_init(&A, mem, sizeof(mem));
    return vjo_afv_send(&A, &ui, r, msg, cap);
}

static int results_now(VjoAfvResults *r)
{
    vjo_arena_init(&A, mem, sizeof(mem));
    return vjo_afv_results(&A, &ui, r);
}

/* ---------------- what a card's note must be ---------------- */

/* The card's note as the .apkg export makes it: its decoded record, its screenshot's media name. */
static int card_note(const char *id, VjoAnkiNote *note, char media[48], int *has_media)
{
    char path[256];
    static char json[VJO_QUEUE_TEXT_MAX + 1];
    static uint8_t pic[VJO_QUEUE_PICTURE_MAX + 1];
    long len;
    int picture = 0;
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    if ((len = get_file(path, json, sizeof(json))) < 0) {
        snprintf(path, sizeof(path), VJO_QUEUE_EXPORTED_DIR "/%s.json", id);
        if ((len = get_file(path, json, sizeof(json))) < 0)
            return -1;
    }
    if (vjo_queue_decode(&A, vjo_arena_strndup(&A, json, (size_t)len), (size_t)len, note, &picture) < 0)
        return -1;
    *has_media = 0;
    if (picture) {
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        if ((len = get_file(path, (char *)pic, sizeof(pic))) < 0) {
            snprintf(path, sizeof(path), VJO_QUEUE_EXPORTED_DIR "/%s.jpg", id);
            len = get_file(path, (char *)pic, sizeof(pic));
        }
        if (len > 0) {
            vjo_apkg_media_name(pic, (size_t)len, media);
            *has_media = 1;
        }
    }
    return 0;
}

/* inbox/rubi/<id>.json, checked against the interface and against the card: every key and every
 * Lapis field. The note's media files and the note type's file are there and whole. */
static void check_note(const char *id)
{
    static char text[VJO_AFV_NOTE_MAX + 1], other[VJO_AFV_NOTE_MAX + 1];
    char path[256], media[48], guid[VJO_APKG_GUID_MAX];
    uint8_t raw[16];
    VjoAnkiNote note;
    VjoJson j;
    long len;
    int has_media, t, fields;
    size_t mark = vjo_arena_mark(&A), expect_len;
    snprintf(path, sizeof(path), INBOX "/%s.json", id);
    TEST_ASSERT_((len = get_file(path, text, sizeof(text))) > 0, "%s", path);
    TEST_ASSERT(card_note(id, &note, media, &has_media) == 0);
    /* byte for byte what the writer makes of the card */
    {
        const char *want = vjo_afv_note_json(&A, id, &note, has_media ? media : NULL, vjo_afv_created(note.unix_time),
                                             &expect_len);
        TEST_ASSERT(want != NULL);
        TEST_CHECK_((size_t)len == expect_len && !memcmp(text, want, expect_len), "%s is not the card's note", path);
    }
    TEST_ASSERT(vjo_json_parse(&A, text, (size_t)len, &j) == 0 && vjo_json_is_type(&j, 0, JSMN_OBJECT));
    TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, 0, "v")), "1"));
    TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, 0, "id")), id));
    for (int i = 0; i < 16; i++)
        raw[i] = (uint8_t)strtol((char[3]){id[2 * i], id[2 * i + 1], 0}, NULL, 16);
    vjo_apkg_guid(raw, guid); /* the .apkg's GUID of the card (core/apkg.c, as its package writes it) */
    TEST_CHECK_(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, 0, "guid")), guid), "guid of %s", id);
    t = vjo_json_obj_get(&j, 0, "notetype");
    TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, t, "name")), "Lapis"));
    TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, t, "id")), "1667218449922"));
    TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, t, "definition")), "lapis-1.7.0.json"));
    TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, 0, "deck")), "Vita JP Overlay"));
    t = vjo_json_obj_get(&j, 0, "tags");
    TEST_CHECK(vjo_json_is_type(&j, t, JSMN_ARRAY) && j.t[t].size == 1 &&
               !strcmp(vjo_json_str(&A, &j, t + 1), "vita-jp-overlay"));
    {
        char created[24];
        snprintf(created, sizeof(created), "%lld", (long long)vjo_afv_created(note.unix_time));
        TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, 0, "created")), created));
    }
    t = vjo_json_obj_get(&j, 0, "duplicate_check");
    t = vjo_json_obj_get(&j, t, "fields");
    TEST_CHECK(vjo_json_is_type(&j, t, JSMN_ARRAY) && j.t[t].size == 2 &&
               !strcmp(vjo_json_str(&A, &j, vjo_json_arr_get(&j, t, 0)), "Expression") &&
               !strcmp(vjo_json_str(&A, &j, vjo_json_arr_get(&j, t, 1)), "ExpressionReading"));
    /* every Lapis field: the package's value (vjo_lapis_fields' separator rule), given when not empty */
    t = vjo_json_obj_get(&j, 0, "fields");
    TEST_ASSERT(vjo_json_is_type(&j, t, JSMN_OBJECT));
    fields = 0;
    for (int f = 0; f < VJO_LAPIS_FIELDS; f++) {
        char *want = vjo_lapis_field_value(&A, f, &note, has_media ? media : NULL);
        int v = vjo_json_obj_get(&j, t, vjo_lapis_field[f]);
        TEST_ASSERT(want != NULL);
        for (char *p = want; *p; p++)
            if (*p == 0x1F)
                *p = ' ';
        if (!want[0] && f != VJO_LAPIS_EXPRESSION && f != VJO_LAPIS_EXPRESSION_READING) {
            TEST_CHECK_(v < 0, "%s: the empty field %s is given", id, vjo_lapis_field[f]);
            continue;
        }
        fields++;
        TEST_CHECK_(v >= 0 && !strcmp(vjo_json_str(&A, &j, v), want), "%s: field %s", id, vjo_lapis_field[f]);
    }
    TEST_CHECK_(j.t[t].size == fields, "%s: %d fields, %d expected", id, j.t[t].size, fields);
    /* the screenshot: named by its SHA-256, the queue's bytes */
    t = vjo_json_obj_get(&j, 0, "media");
    if (has_media) {
        static char pic[VJO_QUEUE_PICTURE_MAX + 1], file[VJO_QUEUE_PICTURE_MAX + 1];
        long a, b;
        TEST_CHECK(vjo_json_is_type(&j, t, JSMN_ARRAY) && j.t[t].size == 1 &&
                   !strcmp(vjo_json_str(&A, &j, t + 1), media));
        snprintf(path, sizeof(path), VJO_AFV_MEDIA_DIR "/%s", media);
        a = get_file(path, file, sizeof(file));
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        if ((b = get_file(path, pic, sizeof(pic))) < 0) {
            snprintf(path, sizeof(path), VJO_QUEUE_EXPORTED_DIR "/%s.jpg", id);
            b = get_file(path, pic, sizeof(pic));
        }
        TEST_CHECK_(a > 0 && a == b && !memcmp(file, pic, (size_t)a), "%s: its media file is not its screenshot", id);
    } else {
        TEST_CHECK_(t < 0, "%s: media without a screenshot", id);
    }
    /* the note type, as core/lapis.c holds it */
    TEST_CHECK((len = get_file(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json", other, sizeof(other))) > 0 &&
               (size_t)len == strlen(vjo_lapis_notetype) && !memcmp(other, vjo_lapis_notetype, (size_t)len));
    vjo_arena_release(&A, mark);
}

/* Whatever a send left (stopped or failed anywhere): no file under a final name is partial, and a
 * note is there only with its media and its note type. */
static void check_inbox_whole(void)
{
    DIR *d = opendir(INBOX);
    struct dirent *e;
    static char text[VJO_AFV_NOTE_MAX + 1];
    if (!d)
        return;
    while ((e = readdir(d))) {
        char path[300], media_path[300], id[33];
        size_t n = strlen(e->d_name), mark = vjo_arena_mark(&A);
        VjoJson j;
        long len;
        int t;
        if (n != 37 || strcmp(e->d_name + 32, ".json"))
            continue;
        memcpy(id, e->d_name, 32);
        id[32] = '\0';
        check_note(id); /* whole, the card's, with its media and note type */
        snprintf(path, sizeof(path), INBOX "/%s", e->d_name);
        TEST_ASSERT((len = get_file(path, text, sizeof(text))) > 0);
        TEST_ASSERT(vjo_json_parse(&A, text, (size_t)len, &j) == 0);
        if ((t = vjo_json_obj_get(&j, 0, "media")) >= 0) {
            snprintf(media_path, sizeof(media_path), VJO_AFV_MEDIA_DIR "/%s", vjo_json_str(&A, &j, t + 1));
            TEST_CHECK_(there(media_path), "%s names %s, which is not there", path, media_path);
        }
        vjo_arena_release(&A, mark);
    }
    closedir(d);
    /* a media file under its final name is a whole JPEG */
    if ((d = opendir(VJO_AFV_MEDIA_DIR))) {
        while ((e = readdir(d))) {
            char path[300];
            static uint8_t pic[VJO_QUEUE_PICTURE_MAX + 1];
            long len;
            if (strncmp(e->d_name, "vitajp_", 7) || strcmp(e->d_name + strlen(e->d_name) - 4, ".jpg"))
                continue;
            snprintf(path, sizeof(path), VJO_AFV_MEDIA_DIR "/%s", e->d_name);
            len = get_file(path, (char *)pic, sizeof(pic));
            TEST_CHECK_(len > 4 && pic[0] == 0xFF && pic[1] == 0xD8 && pic[len - 2] == 0xFF && pic[len - 1] == 0xD9,
                        "%s is not a whole JPEG", path);
        }
        closedir(d);
    }
    {
        static char nt[300000];
        long len = get_file(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json", nt, sizeof(nt));
        TEST_CHECK_(len < 0 || ((size_t)len == strlen(vjo_lapis_notetype) && !memcmp(nt, vjo_lapis_notetype, (size_t)len)),
                    "a partial note type file");
    }
}

/* ---------------- the writer ---------------- */

static void without_the_app_nothing_is_written(void)
{
    VjoAfvSendResult r;
    char msg[400], id[VJO_QUEUE_ID_SIZE];
    reset();
    queue_card(&cat, id);
    TEST_CHECK(send_now(&r, msg, sizeof(msg)) != VJO_OK && !r.installed && !r.sent);
    TEST_CHECK_(strstr(msg, "not installed") && strstr(msg, VJO_AFV_WHERE), "%s", msg);
    TEST_CHECK(!there(ROOT) && io.calls == 0); /* not even a directory */
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1);
    /* its directory alone, no interface.json: not installed either */
    mkdir(ROOT, 0777);
    TEST_CHECK(send_now(&r, msg, sizeof(msg)) != VJO_OK && !r.installed);
    TEST_CHECK(!there(INBOX) && io.calls == 0);
}

static void an_old_or_damaged_interface_writes_nothing(void)
{
    static const char *const bad[] = {
        "{\"v\":1,\"app\":\"Anki for Vita\",\"interfaces\":[2]}",
        "{\"v\":1,\"interfaces\":[]}",
        "{\"v\":1,\"interfaces\":[\"1\"]}",
        "{\"v\":1,\"interfaces\":[1.0]}",
        "{\"v\":1,\"interfaces\":[10]}",
        "{\"v\":1,\"interfaces\":1}",
        "{\"v\":1}",
        "[1]",
        "{\"v\":1,\"interfaces\":[1]",
        "",
        "not json at all",
    };
    VjoAfvSendResult r;
    char msg[400], id[VJO_QUEUE_ID_SIZE];
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        reset();
        queue_card(&cat, id);
        installed(bad[k]);
        TEST_CHECK_(send_now(&r, msg, sizeof(msg)) != VJO_OK && !r.installed && !r.sent, "interface.json %s", bad[k]);
        TEST_CHECK_(!there(INBOX) && io.calls == 0, "interface.json %s: something was written", bad[k]);
    }
    /* a later app that also speaks 1 is one */
    reset();
    queue_card(&cat, id);
    installed("{\"v\":1,\"app\":\"Anki for Vita\",\"app_version\":\"0.9.0\",\"interfaces\":[2,1]}");
    TEST_CHECK(send_now(&r, msg, sizeof(msg)) == VJO_OK && r.installed && r.sent == 1);
    check_note(id);
}

static void every_field_is_the_package_s(void)
{
    VjoAfvSendResult r;
    char msg[400], ids[3][VJO_QUEUE_ID_SIZE];
    reset();
    installed("{\"v\":1,\"app\":\"Anki for Vita\",\"app_version\":\"0.1.0\",\"interfaces\":[1]}");
    queue_card(&cat, ids[0]); /* a screenshot, a rank, a pitch accent */
    queue_card(&dog, ids[1]); /* another screenshot, a second dictionary (Glossary), a line break */
    queue_card(&eat, ids[2]); /* no screenshot; HTML to escape, Anki's field separator in a meaning */
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 3 && !r.already && !r.failed && !r.unreadable && !r.too_large, "%s", msg);
    TEST_CHECK_(strstr(msg, "Handed 3 cards to Anki for Vita") != NULL, "%s", msg);
    for (int i = 0; i < 3; i++)
        check_note(ids[i]);
    TEST_CHECK(files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 2 && files_in(INBOX, ".tmp") == 0 &&
               files_in(VJO_AFV_MEDIA_DIR, ".tmp") == 0 && files_in(VJO_AFV_NOTETYPES_DIR, ".json") == 1);
    /* the cards stay queued (the .apkg export keeps working) until their result is final */
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 3 && files_in(VJO_QUEUE_DIR, ".jpg") == 2);
    TEST_CHECK(io.refused == 0); /* no rename was ever onto a name that is there */
    TEST_CHECK(io.syncs >= 2);   /* the media and the note type synced before the notes, then the notes */
    /* a second send hands nothing over again and writes nothing */
    io.calls = 0;
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 0 && r.already == 3 && strstr(msg, "3 were in its inbox already"), "%s", msg);
    TEST_CHECK(io.refused == 0);
    /* the field separator: a space, as in the package's flds */
    {
        static char text[VJO_AFV_NOTE_MAX + 1];
        char path[256];
        snprintf(path, sizeof(path), INBOX "/%s.json", ids[2]);
        TEST_ASSERT(get_file(path, text, sizeof(text)) > 0);
        TEST_CHECK(!strstr(text, "\\u001f") && strstr(text, "unit"));
    }
}

/* The ordering: every note's files are there before the note is, and nothing under a final name is
 * ever partial, wherever the process stops; the next send finishes the work and replaces nothing. */
static void a_stop_anywhere_leaves_whole_files_only(void)
{
    char ids[3][VJO_QUEUE_ID_SIZE];
    int completed = 0;
    for (int k = 1; k <= 120 && !completed; k++) {
        VjoAfvSendResult r;
        char msg[400];
        pid_t pid;
        int status = 0;
        reset();
        installed("{\"v\":1,\"interfaces\":[1]}");
        queue_card(&cat, ids[0]);
        queue_card(&dog, ids[1]);
        queue_card(&eat, ids[2]);
        fflush(NULL);
        if ((pid = fork()) == 0) {
            io.stop_at = k;
            send_now(&r, msg, sizeof(msg));
            _exit(0);
        }
        TEST_ASSERT(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status));
        completed = WEXITSTATUS(status) == 0;
        TEST_CHECK_(completed || WEXITSTATUS(status) == 86, "step %d: exit %d", k, WEXITSTATUS(status));
        check_inbox_whole();
        /* the next send, from what was left */
        io.calls = io.refused = 0;
        TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
        TEST_CHECK_(r.sent + r.already == 3 && !r.failed, "step %d: %s", k, msg);
        TEST_CHECK_(io.refused == 0, "step %d: a rename onto a name that was there", k);
        for (int i = 0; i < 3; i++)
            check_note(ids[i]);
        TEST_CHECK_(files_in(INBOX, ".tmp") == 0 && files_in(VJO_AFV_MEDIA_DIR, ".tmp") == 0 &&
                        files_in(VJO_AFV_NOTETYPES_DIR, ".tmp") == 0, "step %d: a .tmp left after the next send", k);
        TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 3);
    }
    TEST_CHECK(completed); /* (every step of a whole send was stopped once) */
}

/* A storage call that fails anywhere: the send says so, leaves the cards queued and no partial or
 * temporary file, and the next send hands everything over. */
static void a_failure_anywhere_changes_nothing_else(void)
{
    char ids[3][VJO_QUEUE_ID_SIZE];
    int clean = 0;
    for (int k = 1; k <= 120 && !clean; k++) {
        VjoAfvSendResult r;
        char msg[400];
        reset();
        installed("{\"v\":1,\"interfaces\":[1]}");
        queue_card(&cat, ids[0]);
        queue_card(&dog, ids[1]);
        queue_card(&eat, ids[2]);
        io.fail_at = k;
        send_now(&r, msg, sizeof(msg));
        clean = io.calls < k; /* no call failed: the whole send ran */
        TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 3);
        check_inbox_whole();
        TEST_CHECK_(clean || r.rc != VJO_OK || r.failed || r.sent < 3 || strstr(logs, "failed"),
                    "step %d failed unseen: %s", k, msg);
        io.fail_at = 0;
        io.refused = 0;
        TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
        TEST_CHECK_(r.sent + r.already == 3 && !r.failed && io.refused == 0, "step %d, then: %s", k, msg);
        for (int i = 0; i < 3; i++)
            check_note(ids[i]);
    }
    TEST_CHECK(clean);
}

/* An inbox file is never rewritten: a note there is left alone, and so are a media file and the note
 * type's file, whatever they hold. A card whose screenshot's name holds other bytes, and every card
 * while the note type's file is not Rubi's and one of its notes is there, are not handed over. */
static void nothing_in_the_inbox_is_replaced(void)
{
    VjoAfvSendResult r;
    char msg[400], ids[2][VJO_QUEUE_ID_SIZE], path[256], media[48];
    static char text[VJO_AFV_NOTE_MAX + 1];
    VjoAnkiNote note;
    int has_media;
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, ids[0]);
    queue_card(&dog, ids[1]);
    mkdir(ROOT "/inbox", 0777);
    mkdir(INBOX, 0777);
    mkdir(VJO_AFV_MEDIA_DIR, 0777);
    mkdir(VJO_AFV_NOTETYPES_DIR, 0777);
    TEST_ASSERT(card_note(ids[1], &note, media, &has_media) == 0 && has_media);
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[0]);
    snprintf(text, sizeof(text), "{\"v\":1,\"an older\":\"note\",\"media\":[\"%s\"]}", media);
    put_text(path, text); /* (it names the media file below: no sweep removes that) */
    snprintf(path, sizeof(path), VJO_AFV_MEDIA_DIR "/%s", media);
    put_text(path, "not the screenshot");
    put_text(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json", "{\"another\":\"note type\"}");
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) != VJO_OK); /* the note type is not Rubi's, and a note is there */
    TEST_CHECK_(r.sent == 0 && strstr(msg, "Nothing was handed over") && io.refused == 0, "%s", msg);
    TEST_CHECK(strstr(logs, "lapis-1.7.0.json holds other bytes than Rubi's: left as it is") != NULL);
    TEST_ASSERT(unlink(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json") == 0); /* (the user removes it) */
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 0 && r.already == 1 && r.failed == 1 && io.refused == 0, "%s", msg);
    TEST_CHECK(strstr(logs, "holds other bytes than Rubi's: left as it is") != NULL);
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[1]);
    TEST_CHECK(!there(path)); /* its screenshot's name holds other bytes: not handed over */
    TEST_CHECK(get_file(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json", text, sizeof(text)) > 0 &&
               (size_t)strlen(text) == strlen(vjo_lapis_notetype));
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[0]);
    {
        char before[200];
        snprintf(before, sizeof(before), "{\"v\":1,\"an older\":\"note\",\"media\":[\"%s\"]}", media);
        TEST_CHECK(get_file(path, text, sizeof(text)) > 0 && !strcmp(text, before));
    }
    snprintf(path, sizeof(path), VJO_AFV_MEDIA_DIR "/%s", media);
    TEST_CHECK(get_file(path, text, sizeof(text)) > 0 && !strcmp(text, "not the screenshot"));
    /* with no note of Rubi's in the inbox, a note type file that is not Rubi's is taken back */
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&eat, ids[0]);
    mkdir(ROOT "/inbox", 0777);
    mkdir(INBOX, 0777);
    mkdir(VJO_AFV_NOTETYPES_DIR, 0777);
    put_text(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json", "{\"another\":\"note type\"}");
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 1 && strstr(logs, "was not Rubi's, and no note names it: written again"), "%s", msg);
    check_note(ids[0]);
    /* two cards with the same screenshot: one media file, checked, both handed over */
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    {
        Card twin = cat;
        twin.spelling = "猫猫";
        queue_card(&cat, ids[0]);
        queue_card(&twin, ids[1]);
    }
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 2 && !r.failed && files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 1, "%s", msg);
    check_note(ids[0]);
    check_note(ids[1]);
}

/* Cancel between two cards: the ones staged are taken back (no .tmp left), every card stays queued,
 * and the next send hands the rest over. */
static void a_stop_hands_over_nothing_more(void)
{
    VjoAfvSendResult r;
    char msg[400], ids[3][VJO_QUEUE_ID_SIZE];
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, ids[0]);
    queue_card(&dog, ids[1]);
    queue_card(&eat, ids[2]);
    stop_after = 1; /* Cancel once the first card is staged */
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.stopped >= 2 && r.sent + r.stopped == 3, "%s", msg);
    TEST_CHECK_(strstr(msg, "Stopped") != NULL, "%s", msg);
    TEST_CHECK(files_in(INBOX, ".tmp") == 0 && files_in(VJO_QUEUE_DIR, ".json") == 3);
    stop_after = -1;
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK(files_in(INBOX, ".json") == 3);
    for (int i = 0; i < 3; i++)
        check_note(ids[i]);
}

/* A damaged record, a missing screenshot, a note over the interface's 64 KiB. */
static void damaged_cards_stay_queued(void)
{
    VjoAfvSendResult r;
    char msg[400], ids[3][VJO_QUEUE_ID_SIZE], path[256];
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, ids[0]);
    queue_card(&dog, ids[1]);
    queue_card(&eat, ids[2]);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", ids[0]);
    put_text(path, "{\"version\":4,\"edited\":true}"); /* its checksum is no longer its name */
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", ids[1]);
    TEST_ASSERT(unlink(path) == 0);
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 2 && r.unreadable == 1 && r.missing_picture == 1, "%s", msg);
    TEST_CHECK_(strstr(msg, "1 handed over without its screenshot") && strstr(msg, "1 stay queued; see log.txt"),
                "%s", msg);
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[0]);
    TEST_CHECK(!there(path));
    check_note(ids[1]); /* no media, no Picture */
    TEST_CHECK(files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 0);
    /* a note over 64 KiB: its card stays queued and the next cards go */
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    {
        static char big[10001];
        Card c = eat;
        for (size_t i = 0; i + 1 < sizeof(big); i++)
            big[i] = "<&>\""[i % 4]; /* each HTML-escaped into 4-6 bytes, in two fields: a note over 64 KiB */
        big[sizeof(big) - 1] = '\0';
        c.meaning = big;
        c.sentence = big;
        c.hl_start = 0;
        c.hl_end = 4;
        queue_card(&c, ids[0]);
    }
    queue_card(&cat, ids[1]);
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.too_large == 1 && r.sent == 1, "%s", msg);
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[0]);
    TEST_CHECK(!there(path) && strstr(logs, "over 64 KiB"));
    check_note(ids[1]);
}

/* ---------------- the outbox ---------------- */

static void result(const char *id, const char *suffix, const char *fmt, ...)
{
    char path[256], text[8192];
    va_list ap;
    mkdir(ROOT "/outbox", 0777);
    mkdir(OUTBOX, 0777);
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    snprintf(path, sizeof(path), OUTBOX "/%s.json%s", id, suffix);
    put_text(path, text);
}
#define OK_RESULT(id, status, synced)                                                                              \
    result(id, "", "{\"v\":1,\"id\":\"%s\",\"status\":\"%s\",\"synced\":%s,\"note_id\":1792399207504,"              \
                   "\"time\":1792400000,\"message\":\"\"}\n", id, status, synced)

static int queued(const char *id)
{
    char path[256];
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    return there(path);
}
static int archived(const char *id)
{
    char path[256];
    snprintf(path, sizeof(path), VJO_QUEUE_EXPORTED_DIR "/%s.json", id);
    return there(path);
}
static int has_result(const char *id)
{
    char path[256];
    snprintf(path, sizeof(path), OUTBOX "/%s.json", id);
    return there(path);
}

static void each_result_moves_its_card(void)
{
    static const Card *const kinds[] = {&cat, &dog, &eat};
    char ids[7][VJO_QUEUE_ID_SIZE], path[256];
    VjoAfvResults r;
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    for (int i = 0; i < 7; i++) {
        Card c = *kinds[i % 3];
        char word[24];
        snprintf(word, sizeof(word), "%s%d", c.spelling, i);
        c.spelling = word;
        c.picture = i % 3 == 2 ? 0 : 1 + i;
        queue_card(&c, ids[i]);
    }
    OK_RESULT(ids[0], "added", "true");
    OK_RESULT(ids[1], "exists", "true");
    OK_RESULT(ids[2], "added", "false");
    OK_RESULT(ids[3], "duplicate", "false");
    result(ids[4], "", "{\"v\":1,\"id\":\"%s\",\"status\":\"rejected\",\"synced\":false,\"note_id\":0,"
                       "\"time\":1792400000,\"message\":\"a field name is not the note type's\"}", ids[4]);
    /* a replace stopped between its two renames: only the .old */
    result(ids[5], ".old", "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true}", ids[5]);
    OK_RESULT(ids[6], "exists", "false");
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK_(r.in_anki == 3 && r.duplicate == 1 && r.rejected == 1 && r.waiting == 2 && !r.malformed && !r.failed,
                "%d in Anki, %d duplicates, %d refused, %d waiting, %d malformed, %d failed", r.in_anki, r.duplicate,
                r.rejected, r.waiting, r.malformed, r.failed);
    /* in Anki: archived as an export archives a card, its screenshot with it; the result removed */
    TEST_CHECK(!queued(ids[0]) && archived(ids[0]) && !has_result(ids[0]));
    snprintf(path, sizeof(path), VJO_QUEUE_EXPORTED_DIR "/%s.jpg", ids[0]);
    TEST_CHECK(there(path));
    TEST_CHECK(!queued(ids[1]) && archived(ids[1]) && !has_result(ids[1]));
    TEST_CHECK(!queued(ids[5]) && archived(ids[5]));
    snprintf(path, sizeof(path), OUTBOX "/%s.json.old", ids[5]);
    TEST_CHECK(!there(path));
    /* a leftover .old beside a final result goes with it */
    {
        VjoAfvResults again;
        char old_path[256];
        OK_RESULT(ids[2], "added", "true");
        snprintf(old_path, sizeof(old_path), OUTBOX "/%s.json.old", ids[2]);
        put_text(old_path, "{\"v\":1,\"status\":\"added\",\"synced\":false}");
        TEST_ASSERT(results_now(&again) == VJO_OK);
        TEST_CHECK(again.in_anki == 1 && !queued(ids[2]) && archived(ids[2]) && !has_result(ids[2]) &&
                   !there(old_path));
        OK_RESULT(ids[2], "added", "false"); /* (as it was, for the checks below; its card is archived) */
    }
    /* not synced yet: queued, its result kept */
    TEST_CHECK(queued(ids[6]) && has_result(ids[6]));
    /* a duplicate: the queue's "found in Anki" state, which no export takes */
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.duplicate.json", ids[3]);
    TEST_CHECK(!queued(ids[3]) && there(path) && !has_result(ids[3]) && !archived(ids[3]));
    /* refused: still queued, its result kept for the page, which says why */
    TEST_CHECK(queued(ids[4]) && has_result(ids[4]));
    TEST_CHECK_(!strcmp(r.message, "a field name is not the note type's"), "%s", r.message);
    {
        char text[400];
        vjo_afv_results_text(&r, text, sizeof(text));
        TEST_CHECK_(strstr(text, "3 cards are in Anki now") && strstr(text, "1 card was in your collection already") &&
                        strstr(text, "1 refused, still queued: a field name is not the note type's"),
                    "%s", text);
    }
    {
        VjoExportCounts c;
        vjo_export_count(&c);
        TEST_CHECK(c.queued == 2 && c.exported == 4);
    }
    /* the same results again change nothing more (ids[2]'s, not final, of an archived card: left) */
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(r.in_anki == 0 && r.duplicate == 0 && r.rejected == 1 && r.waiting == 1 && has_result(ids[2]));
}

/* A refused card stays queued (for an .apkg) and is never handed in again under its id: the app
 * keeps its own record, and an inbox file whose result is final is removed unread. */
static void a_refused_card_stays_queued(void)
{
    VjoAfvSendResult s;
    VjoAfvResults r;
    char msg[400], id[VJO_QUEUE_ID_SIZE], path[256];
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, id);
    result(id, "", "{\"v\":1,\"id\":\"%s\",\"status\":\"rejected\",\"synced\":false,\"note_id\":0,"
                   "\"message\":\"a media file is missing\"}", id);
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(r.rejected == 1 && !strcmp(r.message, "a media file is missing"));
    TEST_ASSERT(send_now(&s, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(s.sent == 0 && s.refused == 1 && s.results.rejected == 1, "%s", msg);
    TEST_CHECK_(strstr(msg, "1 refused by Anki for Vita before stays queued: export it as an .apkg") != NULL, "%s", msg);
    snprintf(path, sizeof(path), INBOX "/%s.json", id);
    TEST_CHECK(!there(path) && has_result(id) && queued(id));
    /* an .apkg export takes it; its result goes then */
    {
        VjoExportResult x;
        vjo_arena_init(&A, mem, sizeof(mem));
        TEST_ASSERT(vjo_export_run(&A, 0, &ui, &x, msg, sizeof(msg)) == VJO_OK && x.cards == 1);
    }
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(!has_result(id) && archived(id) && r.removed == 1 && r.rejected == 0);
}

/* A final result whose move fails: the card stays queued, is never handed over again, and the next
 * pass moves it. */
static void a_final_result_not_applied_is_not_sent_again(void)
{
    VjoAfvSendResult s;
    VjoAfvResults r;
    char msg[400], id[VJO_QUEUE_ID_SIZE], path[256], record[64];
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, id);
    OK_RESULT(id, "added", "true");
    snprintf(record, sizeof(record), "%s.json", id);
    io.fail_rename = record;
    TEST_ASSERT(send_now(&s, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(s.sent == 0 && s.results.failed == 1 && s.failed == 1, "%s", msg);
    snprintf(path, sizeof(path), INBOX "/%s.json", id);
    TEST_CHECK(!there(path) && queued(id) && has_result(id));
    io.fail_rename = NULL;
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(r.in_anki == 1 && archived(id) && !queued(id) && !has_result(id));
}

/* Rubi's inbox media go once every note naming them is final (the app removes a note's file then);
 * one two cards share stays while the other's note is there; the note type's file goes when no note
 * is left; a media file no note names (an interrupted send's) goes too. */
static void the_media_go_once_their_notes_are_final(void)
{
    VjoAfvSendResult s;
    VjoAfvResults r;
    char msg[400], ids[3][VJO_QUEUE_ID_SIZE], path[256], media[48];
    VjoAnkiNote note;
    int has_media;
    Card twin = dog;
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, ids[0]);
    twin.spelling = "犬犬";
    twin.picture = cat.picture; /* the same screenshot as the first */
    queue_card(&twin, ids[1]);
    queue_card(&eat, ids[2]); /* none */
    TEST_ASSERT(send_now(&s, msg, sizeof(msg)) == VJO_OK && s.sent == 3);
    TEST_CHECK(files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 1);
    TEST_ASSERT(card_note(ids[0], &note, media, &has_media) == 0 && has_media);
    /* the app takes the first card: its note's file goes, its result is final */
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[0]);
    TEST_ASSERT(unlink(path) == 0);
    OK_RESULT(ids[0], "added", "true");
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(r.in_anki == 1 && r.swept == 0);
    snprintf(path, sizeof(path), VJO_AFV_MEDIA_DIR "/%s", media);
    TEST_CHECK(there(path)); /* the twin's note still names it */
    /* then the twin, refused */
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[1]);
    TEST_ASSERT(unlink(path) == 0);
    result(ids[1], "", "{\"v\":1,\"id\":\"%s\",\"status\":\"rejected\",\"synced\":false,\"note_id\":0,"
                       "\"message\":\"a field name is not the note type's\"}", ids[1]);
    /* an interrupted send's media file, which no note names */
    put_text(VJO_AFV_MEDIA_DIR "/vitajp_0123456789abcdef0123456789abcdef.jpg", "x");
    put_text(VJO_AFV_MEDIA_DIR "/not-rubis.png", "x"); /* not Rubi's name: left */
    TEST_ASSERT(send_now(&s, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(s.refused == 1 && s.already == 1 && s.results.swept == 2, "%s (swept %d)", msg, s.results.swept);
    snprintf(path, sizeof(path), VJO_AFV_MEDIA_DIR "/%s", media);
    TEST_CHECK(!there(path) && !there(VJO_AFV_MEDIA_DIR "/vitajp_0123456789abcdef0123456789abcdef.jpg") &&
               there(VJO_AFV_MEDIA_DIR "/not-rubis.png"));
    TEST_CHECK(there(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json")); /* the last card's note names it */
    /* the last: its note goes, and with no note left, the note type's file */
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[2]);
    TEST_ASSERT(unlink(path) == 0);
    OK_RESULT(ids[2], "exists", "true");
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK_(r.in_anki == 1 && r.swept == 1 && !there(VJO_AFV_NOTETYPES_DIR "/lapis-1.7.0.json"), "swept %d",
                r.swept);
    /* a note that cannot be read keeps every media file */
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, ids[0]);
    queue_card(&dog, ids[1]);
    TEST_ASSERT(send_now(&s, msg, sizeof(msg)) == VJO_OK && s.sent == 2);
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[0]);
    TEST_ASSERT(unlink(path) == 0);
    OK_RESULT(ids[0], "added", "true");
    snprintf(path, sizeof(path), INBOX "/%s.json", ids[1]);
    io.fail_read = path;
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(r.in_anki == 1 && r.swept == 0 && files_in(VJO_AFV_MEDIA_DIR, ".jpg") == 2);
}

/* A file being replaced: <name>, else <name>.old, else, after up to 3 waits, absent. */
static void a_reader_waits_for_a_replace(void)
{
    VjoAfvStatus st;
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    vjo_arena_init(&A, mem, sizeof(mem));
    vjo_afv_status_read(&A, &st);
    TEST_CHECK_(!st.known && io.delays == 3, "%d waits", io.delays);
    io.delays = 0;
    io.appear_at_delay = ROOT "/status.json";
    io.appear_text = "{\"v\":1,\"signed_in\":true,\"server\":\"h\",\"last_sync\":5,\"last_result\":\"ok\","
                     "\"pending\":0}";
    vjo_afv_status_read(&A, &st);
    TEST_CHECK_(st.known && st.signed_in && io.delays == 1, "%d waits", io.delays);
    io.delays = 0;
    TEST_ASSERT(rename(ROOT "/status.json", ROOT "/status.json.old") == 0);
    vjo_afv_status_read(&A, &st);
    TEST_CHECK(st.known && io.delays == 0);
}

static void malformed_results_are_ignored(void)
{
    static const char *const bad[] = {
        "",
        "not json",
        "[1]",
        "{\"v\":\"1\",\"id\":\"%s\",\"status\":\"added\",\"synced\":true}",
        "{\"v\":1.0,\"id\":\"%s\",\"status\":\"added\",\"synced\":true}",
        "{\"id\":\"%s\",\"status\":\"added\",\"synced\":true}",
        "{\"v\":1,\"id\":\"0123456789abcdef0123456789abcdef\",\"status\":\"added\",\"synced\":true}",
        "{\"v\":1,\"status\":\"added\",\"synced\":true}",
        "{\"v\":1,\"id\":\"%s\",\"status\":\"ADDED\",\"synced\":true}",
        "{\"v\":1,\"id\":\"%s\",\"status\":\"kept\",\"synced\":true}",
        "{\"v\":1,\"id\":\"%s\",\"synced\":true}",
        "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":\"yes\"}",
        "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true} {}",
        "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true",
        "{\"v\":1,\"id\":\"%s\\u0000\",\"status\":\"added\",\"synced\":true}",
        "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true,\"pad\":\"%4200s\"}",
    };
    char id[VJO_QUEUE_ID_SIZE];
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        VjoAfvResults r;
        reset();
        installed("{\"v\":1,\"interfaces\":[1]}");
        queue_card(&cat, id);
        result(id, "", bad[k], id, "");
        TEST_ASSERT(results_now(&r) == VJO_OK);
        TEST_CHECK_(r.malformed == 1 && !r.in_anki && !r.duplicate && !r.rejected && !r.waiting, "result %u: %s", k,
                    bad[k]);
        TEST_CHECK_(queued(id) && has_result(id), "result %u changed something", k);
        TEST_CHECK(strstr(logs, "not a version-1 result for its card, ignored") != NULL);
    }
    /* another version's: absent (left alone, not counted) */
    {
        VjoAfvResults r;
        reset();
        installed("{\"v\":1,\"interfaces\":[1]}");
        queue_card(&cat, id);
        result(id, "", "{\"v\":2,\"id\":\"%s\",\"status\":\"added\",\"synced\":true}", id);
        TEST_ASSERT(results_now(&r) == VJO_OK);
        TEST_CHECK(!r.malformed && !r.in_anki && queued(id) && has_result(id));
        TEST_CHECK(strstr(logs, "another version's result, left alone") != NULL);
    }
    /* a duplicate key; invalid UTF-8 (an overlong '/', a surrogate, a lone continuation byte) */
    {
        static const char *const bad2[] = {
            "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true,\"status\":\"rejected\"}",
            "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true,\"message\":\"\xC0\xAF\"}",
            "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true,\"message\":\"\xED\xA0\x80\"}",
            "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true,\"message\":\"\x80\"}",
        };
        for (unsigned k = 0; k < sizeof(bad2) / sizeof(bad2[0]); k++) {
            VjoAfvResults r;
            reset();
            installed("{\"v\":1,\"interfaces\":[1]}");
            queue_card(&cat, id);
            result(id, "", bad2[k], id);
            TEST_ASSERT(results_now(&r) == VJO_OK);
            TEST_CHECK_(r.malformed == 1 && !r.in_anki && queued(id), "result %u", k);
        }
    }
    /* binary bytes, and a NUL inside */
    {
        VjoAfvResults r;
        char path[256];
        static const char nul[] = "{\"v\":1,\"id\":\"x\"\0,\"status\":\"added\"}";
        reset();
        installed("{\"v\":1,\"interfaces\":[1]}");
        queue_card(&cat, id);
        mkdir(ROOT "/outbox", 0777);
        mkdir(OUTBOX, 0777);
        snprintf(path, sizeof(path), OUTBOX "/%s.json", id);
        put_file(path, nul, sizeof(nul));
        TEST_ASSERT(results_now(&r) == VJO_OK);
        TEST_CHECK(r.malformed == 1 && queued(id));
    }
}

/* Only <32 lowercase hex>.json files (and their .old) of queued cards are ever opened, and nothing
 * else is moved or removed: other names, directories, other cards' results. */
static void hostile_names_in_the_outbox_are_never_read(void)
{
    char id[VJO_QUEUE_ID_SIZE], upper[VJO_QUEUE_ID_SIZE], path[400], other[VJO_QUEUE_ID_SIZE];
    VjoAfvResults r;
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    queue_card(&cat, id);
    for (int i = 0; i < 32; i++)
        upper[i] = (char)(id[i] >= 'a' ? id[i] - 32 : id[i]);
    upper[32] = '\0';
    OK_RESULT(upper, "added", "true");                       /* upper case: another name */
    result(id, ".tmp", "{\"v\":1,\"id\":\"%s\",\"status\":\"added\",\"synced\":true}", id); /* a writer's .tmp */
    snprintf(path, sizeof(path), OUTBOX "/%.31s.json", id);  /* 31 digits */
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/%sa.json", id);    /* 33 */
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/%s.JSON", id);
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/%s.json ", id);
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/..%.30s.json", id);
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/%s.json\n", id);
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/%0200d.json", 7); /* long */
    put_text(path, "{}");
    snprintf(path, sizeof(path), OUTBOX "/g123456789abcdef0123456789abcdef.json"); /* not hex */
    put_text(path, "{}");
    /* a directory under a result's name, inside it a file the reader must never reach */
    strcpy(other, "fedcba9876543210fedcba9876543210");
    snprintf(path, sizeof(path), OUTBOX "/%s.json", other);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), OUTBOX "/%s.json/%s.json", other, id);
    put_text(path, "{}");
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK_(r.in_anki == 0 && r.malformed == 0 && queued(id), "%d %d", r.in_anki, r.malformed);
    TEST_CHECK_(io.outbox_reads == 0, "%d outbox files opened, the first %s", io.outbox_reads, io.read_paths[0]);
    TEST_CHECK(files_in(OUTBOX, ".json") >= 3); /* (all still there) */
    /* a well-formed result whose id is no card of Rubi's: never read, left alone */
    strcpy(other, "0123456789abcdef0123456789abcdef");
    OK_RESULT(other, "added", "true");
    io.outbox_reads = 0;
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK_(io.outbox_reads == 0 && has_result(other) && r.in_anki == 0, "%d reads", io.outbox_reads);
    /* once its card is archived (an .apkg export took it meanwhile), its final result goes */
    OK_RESULT(id, "added", "true");
    {
        VjoExportResult x;
        char msg[400];
        vjo_arena_init(&A, mem, sizeof(mem));
        TEST_ASSERT(vjo_export_run(&A, 0, &ui, &x, msg, sizeof(msg)) == VJO_OK && x.cards == 1);
    }
    TEST_ASSERT(results_now(&r) == VJO_OK);
    TEST_CHECK(!has_result(id) && archived(id) && r.removed == 1 && has_result(other));
}

/* ---------------- status.json ---------------- */

static void the_status_is_read_and_said(void)
{
    VjoAfvStatus s;
    char text[200];
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    vjo_arena_init(&A, mem, sizeof(mem));
    vjo_afv_status_read(&A, &s);
    TEST_CHECK(!s.known);
    vjo_afv_status_text(&s, 1792400300, text, sizeof(text));
    TEST_CHECK_(strstr(text, "next sync") != NULL, "%s", text);
    put_text(ROOT "/status.json", "{\"v\": 1, \"signed_in\": true, \"server\": \"anki.example.org\", \"last_sync\": "
                                  "1792400000,\n \"last_result\": \"ok\", \"pending\": 3}");
    vjo_afv_status_read(&A, &s);
    TEST_CHECK(s.known && s.signed_in && !strcmp(s.server, "anki.example.org") && s.last_sync == 1792400000 &&
               !strcmp(s.last_result, "ok") && s.pending == 3);
    vjo_afv_status_text(&s, 1792400300, text, sizeof(text));
    TEST_CHECK_(!strcmp(text, "anki.example.org: last sync 5 min ago, ok; 3 waiting."), "%s", text);
    vjo_afv_status_text(&s, 1792400000 + 3 * 3600, text, sizeof(text));
    TEST_CHECK_(strstr(text, "3 h ago") != NULL, "%s", text);
    vjo_afv_status_text(&s, 1792400000 + 4 * 86400, text, sizeof(text));
    TEST_CHECK_(strstr(text, "4 days ago") != NULL, "%s", text);
    vjo_afv_status_text(&s, 1792399000, text, sizeof(text)); /* the clock behind: no "ago" */
    TEST_CHECK_(!strcmp(text, "anki.example.org: last sync, ok; 3 waiting."), "%s", text);
    /* not signed in; never synced; a full sync to choose */
    put_text(ROOT "/status.json", "{\"v\":1,\"signed_in\":false,\"server\":\"\",\"last_sync\":0,\"last_result\":\"ok\","
                                  "\"pending\":0}");
    vjo_afv_status_read(&A, &s);
    vjo_afv_status_text(&s, 1792400300, text, sizeof(text));
    TEST_CHECK_(strstr(text, "Not signed in") != NULL, "%s", text);
    put_text(ROOT "/status.json", "{\"v\":1,\"signed_in\":true,\"server\":\"sync.example.net\",\"last_sync\":0,"
                                  "\"last_result\":\"error\",\"pending\":1}");
    vjo_afv_status_read(&A, &s);
    vjo_afv_status_text(&s, 1792400300, text, sizeof(text));
    TEST_CHECK_(!strcmp(text, "Signed in to sync.example.net; no sync yet; 1 waiting."), "%s", text);
    put_text(ROOT "/status.json", "{\"v\":1,\"signed_in\":true,\"server\":\"a\\u0007b\",\"last_sync\":1792400290,"
                                  "\"last_result\":\"full_sync_needed\",\"pending\":0,\"later\":[1,2]}");
    vjo_afv_status_read(&A, &s);
    vjo_afv_status_text(&s, 1792400300, text, sizeof(text));
    TEST_CHECK_(!strcmp(text, "a?b: last sync under a minute ago: choose a full sync's direction in it."), "%s", text);
    /* a replace stopped between its renames: the .old */
    TEST_ASSERT(rename(ROOT "/status.json", ROOT "/status.json.old") == 0);
    vjo_afv_status_read(&A, &s);
    TEST_CHECK(s.known && !strcmp(s.last_result, "full_sync_needed"));
    /* not a status */
    static const char *const bad[] = {"{\"v\":2,\"signed_in\":true}", "[]", "{\"signed_in\":true}", "", "{\"v\":1"};
    unlink(ROOT "/status.json.old");
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        put_text(ROOT "/status.json", bad[k]);
        vjo_afv_status_read(&A, &s);
        TEST_CHECK_(!s.known, "status %s", bad[k]);
    }
}

/* ---------------- memory ---------------- */

/* The send's arena (the page's 2 MiB VJO_EXPORT_MEM, shared with the export): its peak for a send of
 * VJO_AFV_SEND_MAX cards (its fixed lists at their largest) and for the largest card (a record of
 * 32 KiB, a 192 KiB screenshot); "measure:" lines. */
static void measure_a_send(void)
{
    VjoAfvSendResult r;
    VjoAfvResults res;
    char msg[400], id[VJO_QUEUE_ID_SIZE];
    size_t peak;
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    for (int i = 0; i < VJO_AFV_SEND_MAX; i++) {
        Card c = eat;
        char word[24];
        snprintf(word, sizeof(word), "語%d", i);
        c.spelling = word;
        c.hl_start = 0;
        c.hl_end = 3;
        c.sentence = word;
        vjo_arena_init(&A, mem, sizeof(mem));
        queue_card(&c, id);
    }
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == VJO_AFV_SEND_MAX && !r.failed, "%s", msg);
    peak = A.peak;
    printf("measure: a send of %d cards: arena peak %u B of %u B\n", r.sent, (unsigned)peak, (unsigned)sizeof(mem));
    /* again, all of them in the inbox: its list too */
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 0 && r.already == VJO_AFV_SEND_MAX, "%s", msg);
    printf("measure: a send of none of them (all in the inbox): arena peak %u B\n", (unsigned)A.peak);
    if (A.peak > peak)
        peak = A.peak;
    TEST_CHECK(peak < sizeof(mem) / 2);
    /* the most results a pass reads: one each, not synced yet */
    {
        DIR *d = opendir(VJO_QUEUE_DIR);
        struct dirent *e;
        int n = 0;
        TEST_ASSERT(d != NULL);
        while ((e = readdir(d)) && n < VJO_AFV_RESULTS_MAX)
            if (strlen(e->d_name) == 37 && !strcmp(e->d_name + 32, ".json")) {
                char hex[33];
                memcpy(hex, e->d_name, 32);
                hex[32] = '\0';
                OK_RESULT(hex, "added", "false");
                n++;
            }
        closedir(d);
        TEST_ASSERT(n == VJO_AFV_RESULTS_MAX);
    }
    TEST_ASSERT(results_now(&res) == VJO_OK);
    TEST_CHECK(res.waiting == VJO_AFV_RESULTS_MAX);
    printf("measure: a results pass of %d results: arena peak %u B\n", res.waiting, (unsigned)A.peak);
    /* the largest card: a record near 32 KiB, a screenshot of 192 KiB */
    reset();
    installed("{\"v\":1,\"interfaces\":[1]}");
    {
        static char big[VJO_QUEUE_TEXT_MAX];
        static uint8_t pic[VJO_QUEUE_PICTURE_MAX];
        char path[256];
        Card c = cat;
        size_t n = 0;
        while (n + 4 < 9000) /* kana, 3 bytes each, in two fields of the record */
            memcpy(big + n, "あ", 3), n += 3;
        big[n] = '\0';
        c.meaning = big;
        c.sentence = big;
        c.hl_end = 3;
        queue_card(&c, id);
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        put_file(path, pic, jpeg_of(pic, 5, sizeof(pic)));
    }
    TEST_ASSERT(send_now(&r, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK_(r.sent == 1, "%s", msg);
    printf("measure: the largest card's send: arena peak %u B\n", (unsigned)A.peak);
    check_note(id);
}

/* ---------------- --write DIR: for test_afv.py ---------------- */

static int write_inbox(const char *dir)
{
    static const Card *const kinds[] = {&cat, &dog, &eat};
    char ids[6][VJO_QUEUE_ID_SIZE], msg[400];
    VjoAfvSendResult s;
    VjoExportResult x;
    FILE *f;
    if (chdir(dir) < 0)
        return 2;
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(VJO_QUEUE_DIR, 0777);
    memset(&io, 0, sizeof(io));
    vjo_arena_init(&A, mem, sizeof(mem));
    for (int i = 0; i < 6; i++) {
        Card c = *kinds[i % 3];
        char word[24];
        snprintf(word, sizeof(word), "%s%d", c.spelling, i);
        c.spelling = word;
        c.time += i;
        c.picture = i == 3 ? 3 : i == 4 ? 1 : c.picture; /* the first and the fifth: the same screenshot */
        queue_card(&c, ids[i]);
    }
    installed("{\"v\":1,\"app\":\"Anki for Vita\",\"app_version\":\"0.1.0\",\"interfaces\":[1]}");
    if (send_now(&s, msg, sizeof(msg)) != VJO_OK || s.sent != 6) {
        fprintf(stderr, "send: %s\n", msg);
        return 1;
    }
    vjo_arena_init(&A, mem, sizeof(mem));
    if (vjo_export_run(&A, 1, &ui, &x, msg, sizeof(msg)) != VJO_OK || x.cards != 6) {
        fprintf(stderr, "export: %s\n", msg);
        return 1;
    }
    if (!(f = fopen("expected.json", "w")))
        return 1;
    fprintf(f, "{\"package\":\"%s\",\"inbox\":\"%s\",\"ids\":[", x.path, INBOX);
    for (int i = 0; i < 6; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", ids[i]);
    fprintf(f, "]}\n");
    fclose(f);
    printf("%s\n", msg);
    return 0;
}

TEST_LIST = {
    {"without_the_app_nothing_is_written", without_the_app_nothing_is_written},
    {"an_old_or_damaged_interface_writes_nothing", an_old_or_damaged_interface_writes_nothing},
    {"every_field_is_the_package_s", every_field_is_the_package_s},
    {"a_stop_anywhere_leaves_whole_files_only", a_stop_anywhere_leaves_whole_files_only},
    {"a_failure_anywhere_changes_nothing_else", a_failure_anywhere_changes_nothing_else},
    {"nothing_in_the_inbox_is_replaced", nothing_in_the_inbox_is_replaced},
    {"a_stop_hands_over_nothing_more", a_stop_hands_over_nothing_more},
    {"damaged_cards_stay_queued", damaged_cards_stay_queued},
    {"each_result_moves_its_card", each_result_moves_its_card},
    {"a_refused_card_stays_queued", a_refused_card_stays_queued},
    {"a_final_result_not_applied_is_not_sent_again", a_final_result_not_applied_is_not_sent_again},
    {"the_media_go_once_their_notes_are_final", the_media_go_once_their_notes_are_final},
    {"a_reader_waits_for_a_replace", a_reader_waits_for_a_replace},
    {"malformed_results_are_ignored", malformed_results_are_ignored},
    {"hostile_names_in_the_outbox_are_never_read", hostile_names_in_the_outbox_are_never_read},
    {"the_status_is_read_and_said", the_status_is_read_and_said},
    {"measure_a_send", measure_a_send},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    char out[PATH_MAX] = "";
    const char *dir;
    if (argc == 3 && !strcmp(argv[1], "--write"))
        return realpath(argv[2], out) ? write_inbox(out) : 2;
    /* the memory card, removed when the suite ends (acutest runs each test in a child: test_tmpdir.h) */
    dir = vjo_test_tmpdir("afv");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
