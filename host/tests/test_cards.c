/* The Rubi app's Cards page storage (#315, settings/cards.c) on real files in a temporary directory, through
 * the Vita file API mapped onto POSIX, with a failure injected at any one storage call that changes the
 * memory card (an open for writing, a write, a flush, a close of a file written, a rename, a removal, a
 * sync). The cards are records as the overlay's Enter writes them (vjo_queue_encode), and the export is the
 * real one (settings/anki_export.c), which settles a stopped change first.
 *
 * A stop in a fresh process between any two steps is test_anki_restart.py's (vjo-apkg-tests --restart-step);
 * here a failure at each step of a change, and the intent files' recovery from each state a stop leaves,
 * built by hand. Each test runs in its own process (acutest's default). */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include "../../core/anki_queue.h"
#include "../../core/errors.h"
#include "../../core/json.h"
#include "../../core/word_key.h"
#include "../../settings/anki_export.h"
#include "../../settings/cards.h"

/* ---------------- the memory card: real files, one failure when asked ---------------- */

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

/* Every call that changes the card is counted; the fail_at-th fails (0: none), as does every one of the
 * kind fail_call on a path containing fail_path. */
static int changes, fail_at;
static const char *fail_call, *fail_path, *fail_stat;
static int writing[64]; /* fds opened for writing */
#define EIO_RC ((int)0x80010005u)
static int failing(const char *call, const char *path)
{
    changes++;
    if (fail_at && changes == fail_at)
        return 1;
    return fail_call && !strcmp(fail_call, call) && (!fail_path || (path && strstr(path, fail_path)));
}
static int native(long n) { return n < 0 ? (errno == ENOENT ? (int)0x80010002u : EIO_RC) : (int)n; }

int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd;
    if ((flags & (O_WRONLY | O_RDWR)) && failing("open", p))
        return EIO_RC;
    if (!(flags & (O_WRONLY | O_RDWR)) && fail_call && !strcmp(fail_call, "read-open") && strstr(p, fail_path))
        return EIO_RC;
    fd = open(p, flags, mode);
    if (fd >= 0 && fd < 64)
        writing[fd] = !!(flags & (O_WRONLY | O_RDWR));
    return native(fd);
}
int sceIoClose(int fd)
{
    int was = fd >= 0 && fd < 64 && writing[fd], rc;
    if (fd >= 0 && fd < 64)
        writing[fd] = 0;
    rc = close(fd);
    if (was && failing("close", NULL))
        return EIO_RC;
    return native(rc);
}
int sceIoRead(int fd, void *p, size_t n) { return native(read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n)
{
    if (failing("write", NULL))
        return EIO_RC;
    return native(write(fd, p, n));
}
int sceIoPread(int fd, void *p, size_t n, SceOff off) { return native(pread(fd, p, n, off)); }
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off)
{
    if (failing("write", NULL))
        return EIO_RC;
    return native(pwrite(fd, p, n, off));
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
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
    memset(st, 0, sizeof(*st));
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    date_of(s.st_mtim.tv_sec, &st->st_mtime);
    return 0;
}
int sceIoGetstat(const char *p, SceIoStat *st)
{
    if (fail_stat && strstr(p, fail_stat))
        return EIO_RC;
    return file_stat(p, st);
}
int sceIoRename(const char *from, const char *to)
{
    struct stat s;
    if (failing("rename", from))
        return EIO_RC;
    /* the codebase never renames over a file: a rename onto one is a test failure */
    TEST_CHECK_(stat(to, &s) != 0, "rename %s over %s", from, to);
    return native(rename(from, to));
}
int sceIoRemove(const char *p)
{
    if (failing("remove", p))
        return EIO_RC;
    return native(unlink(p));
}
int sceIoSyncByFd(int fd, int flag)
{
    (void)flag;
    if (failing("flush", NULL))
        return EIO_RC;
    return native(fsync(fd));
}
int sceIoSync(const char *device, unsigned flags)
{
    (void)device, (void)flags;
    return failing("sync", NULL) ? EIO_RC : 0;
}
static DIR *dirs[8];
static char dir_paths[8][256];
static const char *fail_dopen;
int sceIoDopen(const char *p)
{
    if (fail_dopen && !strcmp(p, fail_dopen))
        return EIO_RC;
    for (int i = 0; i < 8; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p)))
                return native(-1);
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
    memset(ent, 0, sizeof(*ent));
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", dir_paths[fd - 100], e->d_name);
    TEST_ASSERT(file_stat(path, &ent->d_stat) == 0);
    return 1;
}
int sceIoDclose(int fd)
{
    int rc = closedir(dirs[fd - 100]);
    dirs[fd - 100] = NULL;
    return rc;
}
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    *t = (SceDateTime){2026, 10, 27, 18, 0, 0, 0};
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = 62135596800000000ull + 1793037600ull * 1000000ull;
    return 0;
}

/* ---------------- cards ---------------- */

static unsigned char mem[VJO_EXPORT_MEM] __attribute__((aligned(16)));
static VjoArena A;
static void arena(void) { vjo_arena_init(&A, mem, sizeof(mem)); }

static void reset(void)
{
    char data[PATH_MAX];
    TEST_ASSERT(snprintf(data, sizeof(data), "%s/ux0:data", vjo_test_tmpdir_path) < (int)sizeof(data));
    TEST_ASSERT(vjo_test_remove_tree(data) == 0);
    TEST_ASSERT(mkdir("ux0:data", 0777) == 0 && mkdir("ux0:data/VitaJPOverlay", 0777) == 0);
    TEST_ASSERT(mkdir(VJO_QUEUE_DIR, 0777) == 0);
    changes = fail_at = 0;
    fail_call = fail_path = fail_stat = fail_dopen = NULL;
    logs[0] = '\0';
    arena();
}

static void put(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, len, f) == len);
    fclose(f);
}
static long get(const char *path, char *out, size_t cap)
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
static int in(const char *dir, const char *hex, const char *ext)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.%s", dir, hex, ext);
    return there(path);
}
static int files(const char *dir, const char *suffix)
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

/* A picture: JPEG markers around a pattern (seed). */
static size_t jpeg(uint8_t *out, int seed, size_t len)
{
    out[0] = 0xFF, out[1] = 0xD8, out[2] = 0xFF, out[3] = 0xE0;
    for (size_t i = 4; i + 2 < len; i++)
        out[i] = (uint8_t)(i * 13u + (unsigned)seed * 5u);
    out[len - 2] = 0xFF, out[len - 1] = 0xD9;
    return len;
}

typedef struct {
    const char *spelling, *reading, *sentence, *game, *misc;
    int hl_start, hl_end, picture; /* picture: the JPEG's seed (0: none) */
    uint32_t time;
    const char *meaning;
} Card;

/* A record into dir (the queue or exported/) as Enter writes it, its picture beside it; its ID. */
static const char *card_in(const char *dir, const Card *c)
{
    static char id[VJO_QUEUE_ID_SIZE];
    static unsigned char m2[64 * 1024];
    const char *m[2] = {c->meaning ? c->meaning : "[JMdict Plain]\na meaning", "another meaning"}, *pos[1] = {"n"};
    char path[256];
    VjoArena a;
    VjoAnkiNote n;
    char *json;
    vjo_arena_init(&a, m2, sizeof(m2));
    memset(&n, 0, sizeof(n));
    n.spelling = c->spelling;
    n.reading = c->reading;
    n.rank = VJO_NO_RANK;
    n.meanings = m;
    n.n_meanings = 2;
    n.sentence = c->sentence;
    n.hl_start = c->hl_start;
    n.hl_end = c->hl_end;
    n.sentence_furigana = c->sentence;
    n.sf_hl_start = c->hl_start;
    n.sf_hl_end = c->hl_end;
    n.pos = pos;
    n.n_pos = 1;
    n.misc = c->misc ? c->misc : "SOME GAME (2026-10-12 09:30)";
    n.dictionary = "JMdict";
    n.game = c->game ? c->game : "SOME GAME";
    n.title_id = "PCSG00000";
    n.platform = "vita";
    n.unix_time = c->time ? c->time : 1791790200;
    n.ocr_engine = "vocr";
    n.ocr_model = "VOCR5_N_w8g2.vocr";
    TEST_ASSERT((json = vjo_queue_encode(&a, &n, !!c->picture)) != NULL);
    vjo_queue_id(json, strlen(json), id);
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    put(path, json, strlen(json));
    if (c->picture) {
        uint8_t pic[400];
        snprintf(path, sizeof(path), "%s/%s.jpg", dir, id);
        put(path, pic, jpeg(pic, c->picture, sizeof(pic)));
    }
    return id;
}
static const char *queue(const Card *c) { return card_in(VJO_QUEUE_DIR, c); }

static const Card cat = {"猫", "ねこ", "猫が好き", NULL, NULL, 0, 3, 1, 1791790200, NULL};
static const Card dog = {"犬", "いぬ", "「犬だ！」\nと言った", NULL, NULL, 3, 6, 0, 1791790260, NULL};
static const Card eat = {"食べる", "たべる", "パンを食べた", "OTHER GAME", "OTHER GAME (2026-10-12 09:32)", 9, 15, 2,
                         1791790320, NULL};

static void id_of(const char *hex, uint8_t id[16]) { TEST_ASSERT(vjo_cards_unhex(hex, id) == 0); }

/* The record of hex in dir, decoded (its strings in A). */
static int record(const char *dir, const char *hex, VjoAnkiNote *n, int *picture)
{
    static char json[VJO_QUEUE_TEXT_MAX + 1];
    char path[256], actual[VJO_QUEUE_ID_SIZE];
    long len;
    snprintf(path, sizeof(path), "%s/%s.json", dir, hex);
    if ((len = get(path, json, sizeof(json))) <= 0)
        return -1;
    vjo_queue_id(json, (size_t)len, actual);
    TEST_CHECK_(!strcmp(actual, hex), "%s: its name is not its checksum", path);
    return vjo_queue_decode(&A, json, (size_t)len, n, picture);
}

/* No record in the queue or exported/ names a picture that is not there. */
static void every_picture_is_there(void)
{
    static const char *const dirs_[2] = {VJO_QUEUE_DIR, VJO_QUEUE_EXPORTED_DIR};
    for (int k = 0; k < 2; k++) {
        DIR *d = opendir(dirs_[k]);
        struct dirent *e;
        if (!d)
            continue;
        while ((e = readdir(d))) {
            char hex[VJO_QUEUE_ID_SIZE];
            VjoAnkiNote n;
            int picture = 0;
            if (strlen(e->d_name) != 37 || strcmp(e->d_name + 32, ".json"))
                continue;
            memcpy(hex, e->d_name, 32);
            hex[32] = '\0';
            arena();
            TEST_ASSERT(record(dirs_[k], hex, &n, &picture) >= 0);
            TEST_CHECK_(!picture || in(VJO_QUEUE_DIR, hex, "jpg") || in(VJO_QUEUE_EXPORTED_DIR, hex, "jpg"),
                        "%s/%s names a picture that is not there", dirs_[k], hex);
        }
        closedir(d);
    }
}

static VjoCardRow table[VJO_CARDS_MAX];
static VjoCardList list;
static int list_of(int which, const VjoCardRow *bound, int cap)
{
    memset(&list, 0, sizeof(list));
    list.rows = table;
    list.cap = cap;
    arena();
    return vjo_cards_list(&A, which, bound, &list, NULL);
}

/* settings/app_platform.c's clock (#311: the export's sentence readings, none here: no dictionaries) */
uint64_t sceKernelGetProcessTimeWide(void) { return 0; }

static int export_now(int all, VjoExportResult *r, char *msg)
{
    arena();
    return vjo_export_run(&A, all, NULL, NULL, &(VjoExportUi){NULL, NULL, NULL}, r, msg, 300);
}

/* ---------------- tests ---------------- */

/* The list: newest first (equal times by word, reading and ID); each row's word, reading, game, date and
 * picture; a record that is not exportable shown, marked; two cards of one word marked. */
static void the_list_is_newest_first_with_its_marks(void)
{
    char ids[5][VJO_QUEUE_ID_SIZE], path[256], json[VJO_QUEUE_TEXT_MAX + 1];
    Card again = cat, late = dog;
    reset();
    snprintf(ids[0], 33, "%s", queue(&cat));
    snprintf(ids[1], 33, "%s", queue(&dog));
    snprintf(ids[2], 33, "%s", queue(&eat));
    again.sentence = "猫だ"; /* the same word again, another sentence */
    again.hl_end = 3;
    again.picture = 0;
    again.time = 1791790200;
    snprintf(ids[3], 33, "%s", queue(&again));
    late.spelling = "鳥";
    late.reading = "とり";
    late.time = 1791790400;
    snprintf(ids[4], 33, "%s", queue(&late));
    /* an edited record: its name is no longer its checksum */
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", ids[1]);
    TEST_ASSERT(get(path, json, sizeof(json)) > 0);
    *strstr(json, "と言った") = 'X';
    put(path, json, strlen(json));
    /* files that are no cards: a temporary record, a picture alone, an intent */
    put(VJO_QUEUE_DIR "/0123456789abcdef0123456789abcdef.json.tmp", "{", 1);
    put(VJO_QUEUE_DIR "/0123456789abcdef0123456789abcdef.jpg", "\xFF\xD8\xFF\xD9", 4);
    TEST_ASSERT(list_of(VJO_CARDS_QUEUED, NULL, VJO_CARDS_MAX) == 0);
    TEST_ASSERT(list.n == 5 && list.total == 5 && list.unreadable == 1 && !list.newer && !list.older);
    /* newest first: 鳥 (…400), 食べる (…320), 犬 (…260), then the two 猫 at …200 by ID (the same word) */
    TEST_CHECK(!strcmp(table[0].word, "鳥") && !strcmp(table[1].word, "食べる") && !strcmp(table[2].word, "犬"));
    TEST_CHECK(!strcmp(table[3].word, "猫") && !strcmp(table[4].word, "猫"));
    TEST_CHECK(memcmp(table[3].id, table[4].id, 16) < 0);
    TEST_CHECK(!strcmp(table[1].reading, "たべる") && table[1].game < list.n_games &&
               !strcmp(list.games[table[1].game], "OTHER GAME") && !strcmp(table[1].date, "2026-10-12 09:32"));
    TEST_CHECK((table[1].flags & VJO_CARD_PICTURE) && !(table[0].flags & VJO_CARD_PICTURE));
    /* the edited one: shown with its word, not exportable */
    TEST_CHECK((table[2].flags & VJO_CARD_UNREADABLE) && !strcmp(table[2].word, "犬"));
    /* the two 猫 are marked, the others not */
    for (int i = 0; i < 5; i++)
        TEST_CHECK_(!!(table[i].flags & VJO_CARD_DUPLICATE) == (i >= 3), "row %d: duplicate %d", i,
                    !!(table[i].flags & VJO_CARD_DUPLICATE));
    TEST_CHECK(vjo_cards_order(&table[0], &table[1]) < 0 && vjo_cards_order(&table[1], &table[0]) > 0 &&
               vjo_cards_order(&table[2], &table[2]) == 0);
    /* exported/: none yet, and a missing directory is an empty list */
    TEST_ASSERT(list_of(VJO_CARDS_EXPORTED, NULL, VJO_CARDS_MAX) == 0 && list.n == 0 && list.total == 0);
    /* a directory that cannot be read */
    fail_dopen = VJO_QUEUE_DIR;
    TEST_CHECK(list_of(VJO_CARDS_QUEUED, NULL, VJO_CARDS_MAX) == VJO_E_LOCAL_IO);
}

/* Duplicates by #314's word key (core/word_key.h): コーヒー and こーひー are one word, a card without a reading is its
 * spelling's, another reading is another word; the row's key is the word's. */
static void duplicates_follow_the_word_key(void)
{
    Card a = dog, b = dog, c = dog, d = dog;
    reset();
    a.spelling = "コーヒー", a.reading = "コーヒー", a.time = 1791790100;
    b.spelling = "コーヒー", b.reading = "こーひー", b.time = 1791790200;
    c.spelling = "かな", c.reading = "", c.time = 1791790300;
    d.spelling = "かな", d.reading = "かな", d.time = 1791790400;
    queue(&a);
    queue(&b);
    queue(&c);
    queue(&d);
    {
        Card e = dog;
        e.spelling = "かな", e.reading = "カナ", e.time = 1791790500; /* (katakana as hiragana: the same as かな) */
        queue(&e);
    }
    {
        Card f = dog;
        f.spelling = "方", f.reading = "かた", f.time = 1791790600;
        queue(&f);
        f.reading = "ほう", f.time = 1791790700; /* another reading: another word */
        queue(&f);
    }
    TEST_ASSERT(list_of(VJO_CARDS_QUEUED, NULL, VJO_CARDS_MAX) == 0 && list.n == 7);
    for (int i = 0; i < list.n; i++) {
        const VjoCardRow *r = &table[i];
        int want = strcmp(r->word, "方") != 0;
        TEST_CHECK_(!!(r->flags & VJO_CARD_DUPLICATE) == want, "%s (%s): duplicate %d", r->word, r->reading,
                    !!(r->flags & VJO_CARD_DUPLICATE));
        TEST_CHECK(r->key == vjo_word_key(r->word, r->reading));
    }
}

/* More cards than the table: the newest cap, then each window the next cap older than the last one's last
 * row (equal times split between windows by the order). */
static void a_long_list_is_read_a_window_at_a_time(void)
{
    VjoCardRow bound;
    char seen[16][48];
    int n_seen = 0;
    reset();
    for (int i = 0; i < 8; i++) {
        Card c = dog;
        char word[16];
        snprintf(word, sizeof(word), "語%d", i);
        c.spelling = word;
        c.time = 1791790000 + (uint32_t)(i / 2) * 60; /* pairs at the same minute */
        queue(&c);
    }
    TEST_ASSERT(list_of(VJO_CARDS_QUEUED, NULL, 3) == 0);
    TEST_ASSERT(list.n == 3 && list.total == 8 && list.newer == 0 && list.older == 5);
    TEST_CHECK(!strcmp(table[0].word, "語6") && !strcmp(table[1].word, "語7") && !strcmp(table[2].word, "語4"));
    for (int w = 0; w < 3; w++) {
        for (int i = 0; i < list.n; i++)
            snprintf(seen[n_seen++], 48, "%s", table[i].word);
        if (!list.older)
            break;
        bound = table[list.n - 1];
        TEST_ASSERT(list_of(VJO_CARDS_QUEUED, &bound, 3) == 0);
        TEST_CHECK(list.newer == 3 * (w + 1) && list.total == 8);
    }
    /* every card once, newest first */
    static const char *const order[8] = {"語6", "語7", "語4", "語5", "語2", "語3", "語0", "語1"};
    TEST_ASSERT(n_seen == 8);
    for (int i = 0; i < 8; i++)
        TEST_CHECK_(!strcmp(seen[i], order[i]), "card %d: %s, want %s", i, seen[i], order[i]);
}

/* A card's view: its word and reading, game and date, its sentence with the word's offsets, its meanings
 * one a line without their dictionary's name line, its picture's flag; an edited one shown and marked; a
 * card that is gone said so. */
static void a_card_opens_with_its_sentence_definition_and_picture(void)
{
    static VjoCardView v;
    Card c = eat;
    uint8_t id[16];
    char path[256], json[VJO_QUEUE_TEXT_MAX + 1];
    const uint8_t *pic;
    size_t len;
    reset();
    c.meaning = "[Sankoku8 Plain]\nto eat";
    id_of(queue(&c), id);
    arena();
    TEST_ASSERT(vjo_cards_load(&A, VJO_CARDS_QUEUED, id, &v) == 0);
    TEST_CHECK(!strcmp(v.word, "食べる") && !strcmp(v.reading, "たべる") && !strcmp(v.game, "OTHER GAME") &&
               !strcmp(v.date, "2026-10-12 09:32") && !strcmp(v.dictionary, "JMdict"));
    TEST_CHECK(!strcmp(v.sentence, "パンを食べた") && v.hl_start == 9 && v.hl_end == 15 && !v.sentence_cut);
    TEST_CHECK_(!strcmp(v.definition, "to eat\nanother meaning"), "definition: %s", v.definition);
    TEST_CHECK((v.flags & VJO_CARD_PICTURE) && !(v.flags & VJO_CARD_UNREADABLE) && v.which == VJO_CARDS_QUEUED);
    arena();
    TEST_CHECK((pic = vjo_cards_picture(&A, VJO_CARDS_QUEUED, id, &len)) != NULL && len == 400 && pic[0] == 0xFF);
    /* its picture in exported/ (an export stopped between the card's two moves): found there */
    {
        char hex[VJO_QUEUE_ID_SIZE], from[256], to[256];
        vjo_cards_hex(id, hex);
        mkdir(VJO_QUEUE_EXPORTED_DIR, 0777);
        snprintf(from, sizeof(from), VJO_QUEUE_DIR "/%s.jpg", hex);
        snprintf(to, sizeof(to), VJO_QUEUE_EXPORTED_DIR "/%s.jpg", hex);
        TEST_ASSERT(rename(from, to) == 0);
        arena();
        TEST_CHECK(vjo_cards_picture(&A, VJO_CARDS_QUEUED, id, &len) != NULL);
        /* damaged: no end marker */
        put(to, "\xFF\xD8\xFF\xE0garbage", 11);
        arena();
        TEST_CHECK(vjo_cards_picture(&A, VJO_CARDS_QUEUED, id, &len) == NULL);
        /* edited: shown, marked, its word still read */
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", hex);
        TEST_ASSERT(get(path, json, sizeof(json)) > 0);
        *strstr(json, "パン") = 'Y';
        put(path, json, strlen(json));
        arena();
        TEST_ASSERT(vjo_cards_load(&A, VJO_CARDS_QUEUED, id, &v) == 0);
        TEST_CHECK((v.flags & VJO_CARD_UNREADABLE) && v.why == VJO_CARD_WHY_CHECKSUM && !strcmp(v.word, "食べる"));
        /* gone */
        TEST_ASSERT(unlink(path) == 0);
        arena();
        TEST_CHECK(vjo_cards_load(&A, VJO_CARDS_QUEUED, id, &v) == VJO_E_LOCAL_IO && v.why == VJO_CARD_WHY_GONE);
    }
}

/* Delete: the record into deleted/ first, then the picture. The export, the queue's count and the Shell's
 * "already queued" (its find(): the queue, exported/ and .duplicate.json) see it no more; deleted twice (the
 * same card mined again), the older copy in deleted/ goes first: no rename over a file. */
static void delete_moves_the_record_then_its_picture(void)
{
    uint8_t id[16];
    char hex[VJO_QUEUE_ID_SIZE], msg[300];
    VjoExportResult r;
    reset();
    snprintf(hex, sizeof(hex), "%s", queue(&cat));
    id_of(hex, id);
    queue(&dog);
    arena();
    TEST_ASSERT(vjo_cards_delete(&A, id) == 0);
    TEST_CHECK(!in(VJO_QUEUE_DIR, hex, "json") && !in(VJO_QUEUE_DIR, hex, "jpg"));
    TEST_CHECK(in(VJO_CARDS_DELETED_DIR, hex, "json") && in(VJO_CARDS_DELETED_DIR, hex, "jpg"));
    TEST_CHECK(vjo_cards_deleted_files() == 1);
    /* the export takes the other card only, and never reads deleted/ */
    TEST_ASSERT(export_now(1, &r, msg) == 0);
    TEST_CHECK_(r.cards == 1 && files(VJO_QUEUE_EXPORTED_DIR, ".json") == 1 && !in(VJO_QUEUE_EXPORTED_DIR, hex, "json"),
                "%s", msg);
    TEST_CHECK(in(VJO_CARDS_DELETED_DIR, hex, "json"));
    /* the same card again (mined again), deleted again */
    queue(&cat);
    arena();
    TEST_ASSERT(vjo_cards_delete(&A, id) == 0);
    TEST_CHECK(in(VJO_CARDS_DELETED_DIR, hex, "json") && !in(VJO_QUEUE_DIR, hex, "json") && files(VJO_QUEUE_DIR, ".jpg") == 0);
    /* a card that is not there */
    arena();
    TEST_CHECK(vjo_cards_delete(&A, id) == VJO_E_LOCAL_IO && strstr(logs, "not in the queue"));
}

/* A delete that fails at its first move changes nothing; at its second (the picture), the card is gone and
 * its picture is one no card names, which the export removes once it is 10 minutes old. A queued card whose
 * picture an export stopped half way left in exported/ takes that one along. */
static void a_delete_that_fails_loses_nothing(void)
{
    uint8_t id[16];
    char hex[VJO_QUEUE_ID_SIZE], from[256], to[256];
    reset();
    snprintf(hex, sizeof(hex), "%s", queue(&cat));
    id_of(hex, id);
    fail_call = "rename";
    fail_path = ".json";
    arena();
    TEST_CHECK(vjo_cards_delete(&A, id) == VJO_E_LOCAL_IO);
    TEST_CHECK(in(VJO_QUEUE_DIR, hex, "json") && in(VJO_QUEUE_DIR, hex, "jpg"));
    fail_path = ".jpg";
    arena();
    TEST_CHECK(vjo_cards_delete(&A, id) == 0 && strstr(logs, "its picture stays in the queue"));
    TEST_CHECK(!in(VJO_QUEUE_DIR, hex, "json") && in(VJO_QUEUE_DIR, hex, "jpg") && in(VJO_CARDS_DELETED_DIR, hex, "json"));
    every_picture_is_there();
    /* the half-moved picture */
    fail_call = fail_path = NULL;
    reset();
    snprintf(hex, sizeof(hex), "%s", queue(&eat));
    id_of(hex, id);
    mkdir(VJO_QUEUE_EXPORTED_DIR, 0777);
    snprintf(from, sizeof(from), VJO_QUEUE_DIR "/%s.jpg", hex);
    snprintf(to, sizeof(to), VJO_QUEUE_EXPORTED_DIR "/%s.jpg", hex);
    TEST_ASSERT(rename(from, to) == 0);
    arena();
    TEST_ASSERT(vjo_cards_delete(&A, id) == 0);
    TEST_CHECK(!in(VJO_QUEUE_EXPORTED_DIR, hex, "jpg") && in(VJO_CARDS_DELETED_DIR, hex, "jpg"));
}

/* Empty deleted: its cards' records and pictures go for good; the intent of a change not yet settled, and
 * files that are no card's, stay. One that cannot be removed is said, and the rest go. */
static void empty_deleted_removes_its_cards_for_good(void)
{
    uint8_t id[16];
    char a[VJO_QUEUE_ID_SIZE], b[VJO_QUEUE_ID_SIZE];
    int removed = -1;
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    snprintf(b, sizeof(b), "%s", queue(&dog));
    id_of(a, id);
    arena();
    TEST_ASSERT(vjo_cards_delete(&A, id) == 0);
    id_of(b, id);
    arena();
    TEST_ASSERT(vjo_cards_delete(&A, id) == 0);
    put(VJO_CARDS_DELETED_DIR "/0123456789abcdef0123456789abcdef.fix", "fedcba9876543210fedcba9876543210\n", 33);
    put(VJO_CARDS_DELETED_DIR "/notes.txt", "mine", 4);
    TEST_CHECK(vjo_cards_deleted_files() == 2);
    fail_call = "remove";
    fail_path = b;
    arena();
    TEST_CHECK(vjo_cards_empty_deleted(&A, &removed) == VJO_E_LOCAL_IO && removed == 2); /* a's two files */
    TEST_CHECK(!in(VJO_CARDS_DELETED_DIR, a, "json") && in(VJO_CARDS_DELETED_DIR, b, "json"));
    fail_call = fail_path = NULL;
    arena();
    TEST_CHECK(vjo_cards_empty_deleted(&A, &removed) == 0 && removed == 1);
    TEST_CHECK(vjo_cards_deleted_files() == 0 && there(VJO_CARDS_DELETED_DIR "/notes.txt") &&
               there(VJO_CARDS_DELETED_DIR "/0123456789abcdef0123456789abcdef.fix"));
    /* nothing to empty */
    arena();
    TEST_CHECK(vjo_cards_empty_deleted(&A, &removed) == 0 && removed == 0);
}

/* The word in a new sentence: the same occurrence when it occurs as often as before, else the nearest one;
 * line breaks inside it gone or new; not there: none. */
static void the_word_is_found_again_in_the_new_sentence(void)
{
    static const struct {
        const char *old;
        int start, end;
        const char *now;
        int want_start, want_end;
    } cases[] = {
        {"猫が好き", 0, 3, "白い猫が好きだ", 6, 9},
        {"猫と猫", 6, 9, "猫と猫だ", 6, 9},               /* the second of two, as often as before */
        {"猫と猫", 6, 9, "犬と猫と猫と猫", 6, 9},         /* more often: the nearest to where it was */
        {"猫と猫", 6, 9, "猫と犬と犬と犬と猫", 24, 27},   /* as often: the second again, the first nearer */
        {"食\nべる", 0, 10, "食べる", 0, 9},                  /* a line break inside it, gone */
        {"食べる", 0, 9, "食\nべる", 0, 10},                  /* a new one inside it */
        {"猫が好き", 0, 3, "犬が好き", -1, -1},             /* gone */
        {"猫が好き", -1, -1, "猫が好き", -1, -1},           /* no word before */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int s = -1, e = -1, rc = vjo_cards_find_word(cases[i].old, cases[i].start, cases[i].end, cases[i].now, &s, &e);
        TEST_CHECK_(cases[i].want_start < 0 ? rc == -1 : rc == 0 && s == cases[i].want_start && e == cases[i].want_end,
                    "case %u: rc %d, %d..%d, want %d..%d", i, rc, s, e, cases[i].want_start, cases[i].want_end);
    }
}

/* Fix sentence: a new record, every byte as before but the sentence, the word's offsets in it and (the same
 * text) the sentence with readings, so a new ID; its picture the old one's bytes; the old record and its
 * picture in deleted/, no intent left. The export makes it a note with the new sentence and the picture. */
static void a_fixed_sentence_is_a_new_card_with_the_old_picture(void)
{
    static char before[VJO_QUEUE_TEXT_MAX + 1], after[VJO_QUEUE_TEXT_MAX + 1];
    uint8_t id[16];
    char old[VJO_QUEUE_ID_SIZE], now[VJO_QUEUE_ID_SIZE], path[256], msg[300], pic1[401], pic2[401];
    VjoCardChange ch;
    VjoAnkiNote n;
    VjoExportResult r;
    int picture = 0;
    reset();
    snprintf(old, sizeof(old), "%s", queue(&cat));
    id_of(old, id);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", old);
    TEST_ASSERT(get(path, before, sizeof(before)) > 0);
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "白い猫が\n好きだ", &ch) == 0);
    TEST_CHECK(ch.done == VJO_CARDS_CHANGED && ch.picture && ch.word_found && !ch.picture_lost && !ch.old_left);
    vjo_cards_hex(ch.id, now);
    TEST_CHECK(strcmp(now, old) != 0);
    TEST_CHECK(in(VJO_QUEUE_DIR, now, "json") && in(VJO_QUEUE_DIR, now, "jpg") && !in(VJO_QUEUE_DIR, old, "json") &&
               !in(VJO_QUEUE_DIR, old, "jpg"));
    TEST_CHECK(in(VJO_CARDS_DELETED_DIR, old, "json") && in(VJO_CARDS_DELETED_DIR, old, "jpg"));
    TEST_CHECK(files(VJO_CARDS_DELETED_DIR, ".fix") == 0 && files(VJO_QUEUE_DIR, ".tmp") == 0);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", now);
    TEST_ASSERT(get(path, pic1, sizeof(pic1)) == 400);
    snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.jpg", old);
    TEST_ASSERT(get(path, pic2, sizeof(pic2)) == 400 && !memcmp(pic1, pic2, 400));
    /* the record: decodes, the new sentence and offsets, the readings' text the same; the rest as before */
    arena();
    TEST_ASSERT(record(VJO_QUEUE_DIR, now, &n, &picture) == VJO_QUEUE_VERSION && picture);
    TEST_CHECK(!strcmp(n.sentence, "白い猫が\n好きだ") && n.hl_start == 6 && n.hl_end == 9);
    TEST_CHECK(n.sentence_furigana && !strcmp(n.sentence_furigana, n.sentence) && n.sf_hl_start == 6 && n.sf_hl_end == 9);
    TEST_CHECK(!strcmp(n.spelling, "猫") && n.unix_time == 1791790200 && n.n_meanings == 2);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", now);
    TEST_ASSERT(get(path, after, sizeof(after)) > 0);
    {
        /* the bytes before the sentence and after the readings' offsets are the old record's */
        const char *b0 = strstr(before, "\"sentence\":"), *a0 = strstr(after, "\"sentence\":");
        const char *b1 = strstr(before, ",\"source\":"), *a1 = strstr(after, ",\"source\":");
        TEST_ASSERT(b0 && a0 && b1 && a1);
        TEST_CHECK(b0 - before == a0 - after && !memcmp(before, after, (size_t)(b0 - before)) && !strcmp(b1, a1));
    }
    every_picture_is_there();
    /* exported: the new card, with its picture */
    TEST_ASSERT(export_now(0, &r, msg) == 0);
    TEST_CHECK_(r.cards == 1 && r.media == 1 && !r.missing_picture && in(VJO_QUEUE_EXPORTED_DIR, now, "json") &&
                in(VJO_QUEUE_EXPORTED_DIR, now, "jpg"), "%s", msg);
}

/* The word not in the new sentence: the card keeps no bold (offsets -1), and the export's Sentence has no
 * <b>. A sentence with readings other than the sentence (a removed online dictionary's) goes: null. */
static void a_fix_without_the_word_has_no_bold(void)
{
    static VjoCardView v;
    uint8_t id[16];
    char now[VJO_QUEUE_ID_SIZE];
    VjoCardChange ch;
    VjoAnkiNote n;
    char *field;
    int picture;
    reset();
    id_of(queue(&dog), id);
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "鳥が鳴いた", &ch) == 0);
    TEST_CHECK(ch.done == VJO_CARDS_CHANGED && !ch.word_found && !ch.picture);
    vjo_cards_hex(ch.id, now);
    arena();
    TEST_ASSERT(record(VJO_QUEUE_DIR, now, &n, &picture) >= 0 && !picture);
    TEST_CHECK(n.hl_start == -1 && n.hl_end == -1 && !strcmp(n.sentence, "鳥が鳴いた"));
    TEST_ASSERT((field = vjo_anki_field_value(&A, VJO_LAPIS_SENTENCE, &n)) != NULL);
    TEST_CHECK_(!strcmp(field, "鳥が鳴いた"), "Sentence: %s", field);
    arena();
    TEST_ASSERT(vjo_cards_load(&A, VJO_CARDS_QUEUED, ch.id, &v) == 0 && v.hl_start == -1);
    /* readings that are not the sentence's own text */
    {
        static unsigned char m2[64 * 1024];
        const char *m[1] = {"a meaning"};
        VjoArena a2;
        VjoAnkiNote w;
        char *json, hex[VJO_QUEUE_ID_SIZE], path[256];
        vjo_arena_init(&a2, m2, sizeof(m2));
        memset(&w, 0, sizeof(w));
        w.spelling = "猫";
        w.reading = "ねこ";
        w.meanings = m;
        w.n_meanings = 1;
        w.sentence = "猫が好き";
        w.hl_start = 0;
        w.hl_end = 3;
        w.sentence_furigana = "猫[ねこ]が好[す]き";
        w.sf_hl_start = 0;
        w.sf_hl_end = 9;
        w.unix_time = 1791790000;
        TEST_ASSERT((json = vjo_queue_encode(&a2, &w, 0)) != NULL);
        vjo_queue_id(json, strlen(json), hex);
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", hex);
        put(path, json, strlen(json));
        id_of(hex, id);
        arena();
        TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "猫が大好き", &ch) == 0 && ch.done == VJO_CARDS_CHANGED);
        vjo_cards_hex(ch.id, now);
        arena();
        TEST_ASSERT(record(VJO_QUEUE_DIR, now, &n, &picture) >= 0);
        TEST_CHECK(!n.sentence_furigana && n.sf_hl_start == -1 && n.hl_start == 0 && n.hl_end == 3);
    }
}

/* An edit that leaves the record as it was changes nothing (no file is written); a fix whose new card exists
 * already (queued or exported) only retires the old one; an empty sentence or one that makes the record
 * over 32 KiB is refused; a card that is not exportable is not changed. */
static void edits_that_change_nothing_and_cards_that_exist(void)
{
    static char big[VJO_QUEUE_TEXT_MAX + 8];
    uint8_t id[16];
    char a[VJO_QUEUE_ID_SIZE], fixed[VJO_QUEUE_ID_SIZE], path[256], copy[256];
    VjoCardChange ch;
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    id_of(a, id);
    changes = 0;
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "猫が好き", &ch) == 0);
    TEST_CHECK(ch.done == VJO_CARDS_UNCHANGED && changes == 0 && !memcmp(ch.id, id, 16));
    TEST_CHECK(in(VJO_QUEUE_DIR, a, "json") && !there(VJO_CARDS_DELETED_DIR));
    /* fixed once; then the same card mined again and fixed the same way: the new card is there already */
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch) == 0 && ch.done == VJO_CARDS_CHANGED);
    vjo_cards_hex(ch.id, fixed);
    snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.json", a);
    snprintf(copy, sizeof(copy), VJO_QUEUE_DIR "/%s.json", a);
    TEST_ASSERT(link(path, copy) == 0);
    snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.jpg", a);
    snprintf(copy, sizeof(copy), VJO_QUEUE_DIR "/%s.jpg", a);
    TEST_ASSERT(link(path, copy) == 0);
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch) == 0);
    {
        uint8_t want[16];
        id_of(fixed, want);
        TEST_CHECK(ch.done == VJO_CARDS_EXISTED && !memcmp(ch.id, want, 16));
    }
    TEST_CHECK(files(VJO_QUEUE_DIR, ".json") == 1 && in(VJO_QUEUE_DIR, fixed, "json") && in(VJO_QUEUE_DIR, fixed, "jpg") &&
               !in(VJO_QUEUE_DIR, a, "json") && !in(VJO_QUEUE_DIR, a, "jpg") && files(VJO_CARDS_DELETED_DIR, ".fix") == 0);
    /* the new card exported already: the same */
    {
        VjoExportResult r;
        char msg[300];
        TEST_ASSERT(export_now(0, &r, msg) == 0 && r.cards == 1 && in(VJO_QUEUE_EXPORTED_DIR, fixed, "json"));
        snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.json", a);
        snprintf(copy, sizeof(copy), VJO_QUEUE_DIR "/%s.json", a);
        TEST_ASSERT(link(path, copy) == 0);
        snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.jpg", a);
        snprintf(copy, sizeof(copy), VJO_QUEUE_DIR "/%s.jpg", a);
        TEST_ASSERT(link(path, copy) == 0);
        arena();
        TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch) == 0 && ch.done == VJO_CARDS_EXISTED);
        TEST_CHECK(files(VJO_QUEUE_DIR, ".json") == 0 && files(VJO_QUEUE_DIR, ".jpg") == 0 &&
                   files(VJO_QUEUE_EXPORTED_DIR, ".json") == 1);
    }
    /* refused: an empty sentence, one too large, a card not exportable */
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    id_of(a, id);
    arena();
    TEST_CHECK(vjo_cards_fix_sentence(&A, id, "", &ch) == VJO_E_PARSE);
    memset(big, 'a', VJO_QUEUE_TEXT_MAX);
    big[VJO_QUEUE_TEXT_MAX] = '\0';
    arena();
    TEST_CHECK(vjo_cards_fix_sentence(&A, id, big, &ch) == VJO_E_TOO_LARGE);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", a);
    {
        char json[VJO_QUEUE_TEXT_MAX + 1];
        TEST_ASSERT(get(path, json, sizeof(json)) > 0);
        *strstr(json, "好き") = 'Z';
        put(path, json, strlen(json));
    }
    arena();
    TEST_CHECK(vjo_cards_fix_sentence(&A, id, "白い猫", &ch) == VJO_E_PARSE);
    arena();
    TEST_CHECK(vjo_cards_drop_picture(&A, id, &ch) == VJO_E_PARSE);
    TEST_CHECK(in(VJO_QUEUE_DIR, a, "json") && in(VJO_QUEUE_DIR, a, "jpg") && files(VJO_QUEUE_DIR, ".json") == 1);
}

/* Its picture missing or damaged: the fixed card has none (as the export would send it), and says so. A
 * picture that cannot be read now (a read error) changes nothing. */
static void a_fix_of_a_card_whose_picture_is_lost(void)
{
    uint8_t id[16];
    char a[VJO_QUEUE_ID_SIZE], path[256], now[VJO_QUEUE_ID_SIZE];
    VjoCardChange ch;
    VjoAnkiNote n;
    int picture = 1;
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    id_of(a, id);
    fail_call = "read-open";
    fail_path = ".jpg";
    arena();
    TEST_CHECK(vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch) == VJO_E_LOCAL_IO);
    TEST_CHECK(in(VJO_QUEUE_DIR, a, "json") && files(VJO_QUEUE_DIR, ".json") == 1);
    fail_call = fail_path = NULL;
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", a);
    TEST_ASSERT(unlink(path) == 0);
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch) == 0);
    TEST_CHECK(ch.done == VJO_CARDS_CHANGED && !ch.picture && ch.picture_lost);
    vjo_cards_hex(ch.id, now);
    arena();
    TEST_ASSERT(record(VJO_QUEUE_DIR, now, &n, &picture) >= 0 && !picture && !in(VJO_QUEUE_DIR, now, "jpg"));
    every_picture_is_there();
}

/* Drop picture: a new record with "picture": false, its other bytes as before; the old record and its JPEG
 * in deleted/. A card without a picture has nothing to drop. */
static void drop_picture_makes_a_card_without_it(void)
{
    uint8_t id[16];
    char a[VJO_QUEUE_ID_SIZE], now[VJO_QUEUE_ID_SIZE], msg[300];
    VjoCardChange ch;
    VjoAnkiNote n;
    VjoExportResult r;
    int picture = 1;
    reset();
    snprintf(a, sizeof(a), "%s", queue(&eat));
    id_of(a, id);
    arena();
    TEST_ASSERT(vjo_cards_drop_picture(&A, id, &ch) == 0 && ch.done == VJO_CARDS_CHANGED && !ch.picture);
    vjo_cards_hex(ch.id, now);
    arena();
    TEST_ASSERT(record(VJO_QUEUE_DIR, now, &n, &picture) >= 0 && !picture && !strcmp(n.sentence, "パンを食べた"));
    TEST_CHECK(!in(VJO_QUEUE_DIR, now, "jpg") && !in(VJO_QUEUE_DIR, a, "jpg") && in(VJO_CARDS_DELETED_DIR, a, "jpg") &&
               in(VJO_CARDS_DELETED_DIR, a, "json"));
    /* nothing to drop now */
    arena();
    TEST_ASSERT(vjo_cards_drop_picture(&A, ch.id, &ch) == 0 && ch.done == VJO_CARDS_UNCHANGED);
    TEST_ASSERT(export_now(0, &r, msg) == 0);
    TEST_CHECK_(r.cards == 1 && r.media == 0 && !r.missing_picture, "%s", msg);
}

/* A fix that fails at any one of its storage calls leaves the old card or the new one (never neither), no
 * record naming a picture that is not there; the next recovery leaves exactly one of them and no intent; the
 * export then takes that one once. */
static void a_failure_at_any_step_of_a_change_loses_nothing(void)
{
    int steps;
    for (int kind = 0; kind < 2; kind++) {
        uint8_t id[16];
        char a[VJO_QUEUE_ID_SIZE];
        VjoCardChange ch;
        /* the steps a change takes, counted once without a failure */
        reset();
        snprintf(a, sizeof(a), "%s", queue(&cat));
        id_of(a, id);
        changes = 0;
        arena();
        TEST_ASSERT((kind ? vjo_cards_drop_picture(&A, id, &ch) : vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch)) == 0);
        steps = changes;
        TEST_ASSERT_(steps >= 6, "%d steps", steps);
        for (int k = 1; k <= steps; k++) {
            char now[VJO_QUEUE_ID_SIZE], msg[300];
            VjoExportResult r;
            int rc, old_there, new_there;
            reset();
            snprintf(a, sizeof(a), "%s", queue(&cat));
            id_of(a, id);
            queue(&dog);
            changes = 0;
            fail_at = k;
            arena();
            rc = kind ? vjo_cards_drop_picture(&A, id, &ch) : vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch);
            fail_at = 0;
            vjo_cards_hex(ch.done == VJO_CARDS_CHANGED ? ch.id : id, now);
            old_there = in(VJO_QUEUE_DIR, a, "json");
            /* the new card's ID, whatever happened: the change done again without a failure tells */
            new_there = rc == 0 && strcmp(now, a) && in(VJO_QUEUE_DIR, now, "json");
            TEST_CHECK_(old_there || new_there, "kind %d, step %d of %d (rc %d): neither card", kind, k, steps, rc);
            every_picture_is_there();
            arena();
            vjo_cards_recover(&A);
            TEST_CHECK_(files(VJO_CARDS_DELETED_DIR, ".fix") == 0, "kind %d, step %d: an intent left", kind, k);
            TEST_CHECK_(files(VJO_QUEUE_DIR, ".json") == 2, "kind %d, step %d: %d cards queued", kind, k,
                        files(VJO_QUEUE_DIR, ".json"));
            TEST_CHECK_(files(VJO_QUEUE_DIR, ".tmp") == 0, "kind %d, step %d: a .tmp left", kind, k);
            every_picture_is_there();
            TEST_ASSERT(export_now(0, &r, msg) == 0);
            TEST_CHECK_(r.cards == 2 && files(VJO_QUEUE_EXPORTED_DIR, ".json") == 2, "kind %d, step %d: %s", kind, k, msg);
        }
    }
}

/* What a stop leaves, settled by the recovery (before a list, an export or a change): a new record there
 * finishes the change (the old record and its picture into deleted/), none undoes it (the new card's files
 * go, the old card stays); an intent's own temporary file goes, a damaged intent goes and leaves the cards;
 * an intent that cannot be settled is kept, and the export says so and changes nothing. */
static void recovery_settles_each_stopped_change(void)
{
    uint8_t id[16];
    char a[VJO_QUEUE_ID_SIZE], b[VJO_QUEUE_ID_SIZE], intent[256], line[40], path[256], msg[300];
    VjoCardChange ch;
    VjoExportResult r;
    /* the new card made, the old one not yet moved (a stop between steps 3 and 4) */
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    id_of(a, id);
    arena();
    TEST_ASSERT(vjo_cards_fix_sentence(&A, id, "白い猫が好き", &ch) == 0);
    vjo_cards_hex(ch.id, b);
    /* put the old card back as it was before step 4, and its intent */
    snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.json", a);
    snprintf(intent, sizeof(intent), VJO_QUEUE_DIR "/%s.json", a);
    TEST_ASSERT(rename(path, intent) == 0);
    snprintf(path, sizeof(path), VJO_CARDS_DELETED_DIR "/%s.jpg", a);
    snprintf(intent, sizeof(intent), VJO_QUEUE_DIR "/%s.jpg", a);
    TEST_ASSERT(rename(path, intent) == 0);
    snprintf(intent, sizeof(intent), VJO_CARDS_DELETED_DIR "/%s.fix", a);
    snprintf(line, sizeof(line), "%s\n", b);
    put(intent, line, 33);
    TEST_ASSERT(list_of(VJO_CARDS_QUEUED, NULL, VJO_CARDS_MAX) == 0); /* (a list does not settle it: the page's job does) */
    TEST_CHECK(list.n == 2);
    arena();
    TEST_CHECK(vjo_cards_recover(&A) == 0 && !there(intent));
    TEST_CHECK(!in(VJO_QUEUE_DIR, a, "json") && !in(VJO_QUEUE_DIR, a, "jpg") && in(VJO_QUEUE_DIR, b, "json") &&
               in(VJO_CARDS_DELETED_DIR, a, "json") && in(VJO_CARDS_DELETED_DIR, a, "jpg"));
    /* the new card not made (a stop between steps 2 and 3): undone, its picture removed */
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    mkdir(VJO_CARDS_DELETED_DIR, 0777);
    snprintf(intent, sizeof(intent), VJO_CARDS_DELETED_DIR "/%s.fix", a);
    put(intent, "fedcba9876543210fedcba9876543210\n", 33);
    put(VJO_QUEUE_DIR "/fedcba9876543210fedcba9876543210.jpg", "\xFF\xD8\xFF\xD9", 4);
    put(VJO_QUEUE_DIR "/fedcba9876543210fedcba9876543210.jpg.tmp", "\xFF\xD8", 2);
    put(VJO_QUEUE_DIR "/fedcba9876543210fedcba9876543210.json.tmp", "{", 1);
    arena();
    TEST_CHECK(vjo_cards_recover(&A) == 0 && !there(intent) && files(VJO_QUEUE_DIR, ".json") == 1 &&
               in(VJO_QUEUE_DIR, a, "json") && in(VJO_QUEUE_DIR, a, "jpg") && files(VJO_QUEUE_DIR, ".jpg") == 1 &&
               files(VJO_QUEUE_DIR, ".tmp") == 0);
    /* an intent's own .tmp, and a damaged intent */
    put(VJO_CARDS_DELETED_DIR "/0123456789abcdef0123456789abcdef.fix.tmp", "0123", 4);
    put(intent, "not an id", 9);
    arena();
    TEST_CHECK(vjo_cards_recover(&A) == 0 && files(VJO_CARDS_DELETED_DIR, ".fix") == 0 &&
               files(VJO_CARDS_DELETED_DIR, ".tmp") == 0 && in(VJO_QUEUE_DIR, a, "json"));
    /* one that cannot be settled: kept; the export changes nothing and says so */
    put(intent, "fedcba9876543210fedcba9876543210\n", 33);
    fail_call = "remove";
    fail_path = ".fix";
    arena();
    TEST_CHECK(vjo_cards_recover(&A) == 1 && there(intent));
    TEST_CHECK(export_now(0, &r, msg) == VJO_E_LOCAL_IO && strstr(msg, "could not be finished") &&
               in(VJO_QUEUE_DIR, a, "json") && !there(VJO_EXPORT_DIR));
    /* deleted/ that cannot be read: the same */
    fail_call = fail_path = NULL;
    fail_dopen = VJO_CARDS_DELETED_DIR;
    arena();
    TEST_CHECK(vjo_cards_recover(&A) == 1);
    fail_dopen = NULL;
    TEST_ASSERT(export_now(0, &r, msg) == 0 && r.cards == 1 && !there(intent));
    /* no deleted/ at all: nothing to do */
    reset();
    arena();
    TEST_CHECK(vjo_cards_recover(&A) == 0);
}

/* The export never reads deleted/: a deleted card is in no package, Export all cards included. */
static void the_export_ignores_deleted_cards(void)
{
    uint8_t id[16];
    char a[VJO_QUEUE_ID_SIZE], msg[300];
    VjoExportResult r;
    reset();
    snprintf(a, sizeof(a), "%s", queue(&cat));
    queue(&dog);
    queue(&eat);
    id_of(a, id);
    arena();
    TEST_ASSERT(vjo_cards_delete(&A, id) == 0);
    TEST_ASSERT(export_now(0, &r, msg) == 0 && r.cards == 2);
    TEST_ASSERT(export_now(1, &r, msg) == 0 && r.cards == 2 && !in(VJO_QUEUE_EXPORTED_DIR, a, "json"));
    TEST_CHECK(in(VJO_CARDS_DELETED_DIR, a, "json") && in(VJO_CARDS_DELETED_DIR, a, "jpg"));
}

TEST_LIST = {
    {"the_list_is_newest_first_with_its_marks", the_list_is_newest_first_with_its_marks},
    {"duplicates_follow_the_word_key", duplicates_follow_the_word_key},
    {"a_long_list_is_read_a_window_at_a_time", a_long_list_is_read_a_window_at_a_time},
    {"a_card_opens_with_its_sentence_definition_and_picture", a_card_opens_with_its_sentence_definition_and_picture},
    {"delete_moves_the_record_then_its_picture", delete_moves_the_record_then_its_picture},
    {"a_delete_that_fails_loses_nothing", a_delete_that_fails_loses_nothing},
    {"empty_deleted_removes_its_cards_for_good", empty_deleted_removes_its_cards_for_good},
    {"the_word_is_found_again_in_the_new_sentence", the_word_is_found_again_in_the_new_sentence},
    {"a_fixed_sentence_is_a_new_card_with_the_old_picture", a_fixed_sentence_is_a_new_card_with_the_old_picture},
    {"a_fix_without_the_word_has_no_bold", a_fix_without_the_word_has_no_bold},
    {"edits_that_change_nothing_and_cards_that_exist", edits_that_change_nothing_and_cards_that_exist},
    {"a_fix_of_a_card_whose_picture_is_lost", a_fix_of_a_card_whose_picture_is_lost},
    {"drop_picture_makes_a_card_without_it", drop_picture_makes_a_card_without_it},
    {"a_failure_at_any_step_of_a_change_loses_nothing", a_failure_at_any_step_of_a_change_loses_nothing},
    {"recovery_settles_each_stopped_change", recovery_settles_each_stopped_change},
    {"the_export_ignores_deleted_cards", the_export_ignores_deleted_cards},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("cards");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
