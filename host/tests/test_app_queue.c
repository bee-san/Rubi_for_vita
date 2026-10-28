/* The Rubi app's queue writer (settings/app_queue.c: a card mined on the Look up page, #313) on real files in a
 * temporary directory, through the Vita file API mapped onto POSIX, with every storage call made to fail in turn:
 * a card is saved as the overlay saves one (shell/anki_queue.c), once; the same card queued, exported or found by
 * AnkiConnect before is not saved again; a failure at any step leaves no card and no temporary file, and the cards
 * saved before as they were. `--restart-step DIR ACTION [CRASH]` runs one step of host/tests/test_anki_restart.py:
 * a save stopped (the process gone) at each storage boundary. */
#define main acutest_main
#include "acutest.h"
#undef main
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "test_tmpdir.h"
#include "../../settings/app_queue.h"

/* ---------------- the memory card: real files (fakes/sceio_posix.h), failures and stops ---------------- */

static int fail_open, fail_write, fail_flush, fail_close, fail_rename, fail_sync, fail_stat, short_writes;
static int syncs, renames;
static const char *crash_point;
static char write_path[256], last_rename[256];
static int write_fd = -1;
static char logs[4096];

static void crash_at(const char *point)
{
    if (crash_point && !strcmp(crash_point, point))
        _exit(86); /* no cleanup: the process stops there */
}
static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !strcmp(s + n - k, suffix);
}
#define SCEIO_OWN (SCEIO_OPEN | SCEIO_CLOSE | SCEIO_WRITE | SCEIO_SYNCBYFD | SCEIO_RENAME | SCEIO_SYNC | SCEIO_GETSTAT)
#include "fakes/sceio_posix.h"
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd;
    if (fail_open)
        return (int)0x80010005u;
    fd = sceio_open(p, flags, mode);
    if (fd >= 0 && (flags & O_WRONLY)) {
        write_fd = fd;
        snprintf(write_path, sizeof(write_path), "%s", p);
    }
    return fd;
}
int sceIoClose(int fd)
{
    int rc = sceio_close(fd);
    return fail_close ? (int)0x80010005u : rc;
}
int sceIoWrite(int fd, const void *p, size_t n)
{
    int wrote;
    if (fail_write)
        return (int)0x8001001Cu; /* ENOSPC */
    if (short_writes && n > 7)
        n = 7;
    wrote = sceio_write(fd, p, n);
    if (wrote > 0 && fd == write_fd && ends_with(write_path, ".json.tmp"))
        crash_at("json_partial");
    return wrote;
}
int sceIoSyncByFd(int fd, int flag)
{
    if (fail_flush)
        return (int)0x80010005u;
    int rc = sceio_syncbyfd(fd, flag);
    if (!rc && fd == write_fd && ends_with(write_path, ".json.tmp"))
        crash_at("json_flushed");
    return rc;
}
int sceIoRename(const char *a, const char *b)
{
    int rc;
    if (fail_rename)
        return (int)0x80010005u;
    rc = sceio_rename(a, b);
    if (!rc) {
        renames++;
        snprintf(last_rename, sizeof(last_rename), "%s", b);
        if (ends_with(b, ".json"))
            crash_at("json_renamed");
    }
    return rc;
}
int sceIoSync(const char *device, unsigned flags)
{
    /* The mount-level metadata barrier, as the card's own directories' fsyncs (test_shell_anki.c's): not the
     * machine's sync(), which waits for every disk of it. A stop is a process's, not a power cut, so what the code
     * wrote is there for the next process either way. */
    static const char *const dirs[] = {VJO_QUEUE_DIR, "ux0:data/VitaJPOverlay", "ux0:data"};
    (void)flags;
    syncs++;
    TEST_CHECK(!strcmp(device, "ux0:"));
    if (fail_sync)
        return (int)0x80010005u;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
        int fd = open(dirs[i], O_RDONLY | O_DIRECTORY); /* (one a test made a file is not a directory: skipped) */
        if (fd >= 0) {
            (void)fsync(fd);
            close(fd);
        }
    }
    if (ends_with(last_rename, ".json"))
        crash_at("json_committed");
    return 0;
}
int sceIoGetstat(const char *p, SceIoStat *st)
{
    if (fail_stat)
        return (int)0x80010005u;
    return sceio_getstat(p, st);
}
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs))
        logs[n] = '\n', logs[n + 1] = '\0';
}

/* ---------------- cards ---------------- */

static uint8_t mem[256 * 1024];
static VjoArena arena;

/* A card as the Look up page makes one: the entry, the typed text or the line, no picture. */
static VjoAnkiNote card(const char *spelling, const char *sentence, const char *game)
{
    static const char *meanings[1] = {"[JMdict]\na meaning"}, *pos[1] = {"n"};
    VjoAnkiNote n;
    memset(&n, 0, sizeof(n));
    n.spelling = spelling;
    n.reading = "ねこ";
    n.meanings = meanings;
    n.n_meanings = 1;
    n.pos = pos;
    n.n_pos = 1;
    n.sentence = n.sentence_furigana = sentence;
    n.hl_start = n.sf_hl_start = sentence[0] ? 0 : -1;
    n.hl_end = n.sf_hl_end = sentence[0] ? (int)strlen(spelling) : -1;
    n.misc = game ? "CLANNAD (2026-10-28 00:30)" : "2026-10-27 15:04";
    n.game = game;
    n.title_id = game ? "PCSG00415" : NULL;
    n.platform = game ? "vita" : NULL;
    n.ocr_engine = game ? "vocr" : NULL;
    n.ocr_model = game ? "VOCR5_N_w8.vocr" : NULL;
    n.dictionary = "JMdict";
    n.unix_time = 1792000000;
    return n;
}

static int files(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        n += ends_with(e->d_name, suffix);
    closedir(d);
    return n;
}

static void reset(void)
{
    TEST_ASSERT(vjo_test_remove_tree("ux0:data") == 0);
    TEST_ASSERT(mkdir("ux0:data", 0777) == 0);
    fail_open = fail_write = fail_flush = fail_close = fail_rename = fail_sync = fail_stat = short_writes = 0;
    syncs = renames = 0;
    logs[0] = '\0';
    vjo_arena_init(&arena, mem, sizeof(mem));
}

/* The record on the card is the note's version 4 encoding, without a picture, under its SHA-256 ID. */
static void check_record(const char *id, const VjoAnkiNote *n)
{
    char path[160], data[VJO_QUEUE_TEXT_MAX + 1], actual[VJO_QUEUE_ID_SIZE];
    FILE *f;
    size_t len;
    const char *want = vjo_queue_encode(&arena, n, 0);
    VjoAnkiNote back;
    int picture = 1;
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "rb")) != NULL);
    len = fread(data, 1, sizeof(data) - 1, f);
    fclose(f);
    data[len] = '\0';
    TEST_CHECK(want && !strcmp(data, want));
    vjo_queue_id(data, len, actual);
    TEST_CHECK(!strcmp(actual, id) && vjo_queue_valid_id(id));
    TEST_CHECK(vjo_queue_decode(&arena, data, len, &back, &picture) == 4 && !picture);
    TEST_CHECK(!strcmp(back.spelling, n->spelling) && !strcmp(back.sentence, n->sentence));
}

static void a_card_is_saved_once_as_the_overlay_saves_one(void)
{
    VjoAnkiNote typed = card("猫", "", NULL), line = card("猫", "猫が鳴いた", "CLANNAD");
    char id[VJO_QUEUE_ID_SIZE], again[VJO_QUEUE_ID_SIZE];
    reset();
    TEST_CHECK(vjo_app_queue_save(&arena, &typed, id) == VJO_APP_QUEUE_SAVED);
    TEST_CHECK(arena.used == 0); /* the arena as it was */
    check_record(id, &typed);
    TEST_CHECK(syncs == 1 && renames == 1 && files(VJO_QUEUE_DIR, ".json") == 1 && files(VJO_QUEUE_DIR, ".tmp") == 0);
    TEST_CHECK(strstr(logs, "saved from the Look up page") && !strstr(logs, "猫")); /* no looked-up word logged */
    /* the same card again: queued already; another card: a second record */
    TEST_CHECK(vjo_app_queue_save(&arena, &typed, again) == VJO_APP_QUEUE_EXISTS && !strcmp(again, id));
    TEST_CHECK(vjo_app_queue_save(&arena, &line, again) == VJO_APP_QUEUE_SAVED && strcmp(again, id));
    check_record(again, &line);
    TEST_CHECK(files(VJO_QUEUE_DIR, ".json") == 2);
    /* exported meanwhile (the export moves it into exported/): ARCHIVED, and nothing written */
    TEST_ASSERT(mkdir(VJO_QUEUE_EXPORTED_DIR, 0777) == 0);
    char from[160], to[160];
    snprintf(from, sizeof(from), VJO_QUEUE_DIR "/%s.json", id);
    snprintf(to, sizeof(to), VJO_QUEUE_EXPORTED_DIR "/%s.json", id);
    TEST_ASSERT(rename(from, to) == 0);
    renames = 0;
    TEST_CHECK(vjo_app_queue_save(&arena, &typed, NULL) == VJO_APP_QUEUE_ARCHIVED && renames == 0);
    /* the same card with a picture, saved by an earlier build or the overlay: the same card */
    VjoAnkiNote other = card("犬", "", NULL);
    const char *json = vjo_queue_encode(&arena, &other, 1);
    char pic[VJO_QUEUE_ID_SIZE], path[160];
    vjo_queue_id(json, strlen(json), pic);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", pic);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f && fputs(json, f) >= 0 && !fclose(f));
    vjo_arena_reset(&arena);
    TEST_CHECK(vjo_app_queue_save(&arena, &other, again) == VJO_APP_QUEUE_EXISTS && !strcmp(again, pic));
    /* AnkiConnect had it (an earlier build's <id>.duplicate.json) */
    VjoAnkiNote third = card("鳥", "", NULL);
    json = vjo_queue_encode(&arena, &third, 0);
    vjo_queue_id(json, strlen(json), pic);
    vjo_arena_reset(&arena);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.duplicate.json", pic);
    TEST_ASSERT((f = fopen(path, "wb")) && !fclose(f));
    TEST_CHECK(vjo_app_queue_save(&arena, &third, NULL) == VJO_APP_QUEUE_ARCHIVED);
    /* short writes are written on */
    VjoAnkiNote fourth = card("魚", "魚を食べた", "CLANNAD");
    short_writes = 1;
    TEST_CHECK(vjo_app_queue_save(&arena, &fourth, id) == VJO_APP_QUEUE_SAVED);
    short_writes = 0;
    check_record(id, &fourth);
}

/* Each storage call failing in turn: no card, no temporary file, the earlier cards untouched, the failure logged. */
static void a_failed_save_leaves_no_card(void)
{
    int *fails[] = {&fail_open, &fail_write, &fail_flush, &fail_close, &fail_rename, &fail_sync, &fail_stat};
    static const char *const calls[] = {"open", "write", "flush", "close", "rename", "sync", "getstat"};
    for (unsigned i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        VjoAnkiNote first = card("猫", "", NULL), next = card("犬", "犬だ", "CLANNAD");
        char id[VJO_QUEUE_ID_SIZE];
        reset();
        TEST_ASSERT(vjo_app_queue_save(&arena, &first, id) == VJO_APP_QUEUE_SAVED);
        *fails[i] = 1;
        int rc = vjo_app_queue_save(&arena, &next, NULL);
        *fails[i] = 0;
        TEST_CHECK_(rc == VJO_APP_QUEUE_FAILED && strstr(logs, calls[i]), "%s: rc %d, log %s", calls[i], rc, logs);
        TEST_CHECK_(files(VJO_QUEUE_DIR, ".tmp") == 0, "%s: a temporary file left", calls[i]);
        /* a failed final sync may leave the renamed record (the Shell's rule): never acknowledged until synced */
        TEST_CHECK_(files(VJO_QUEUE_DIR, ".json") == (i == 5 ? 2 : 1), "%s: %d records", calls[i],
                    files(VJO_QUEUE_DIR, ".json"));
        check_record(id, &first);
        if (i == 5) { /* the retry finds it, syncs, and says it is queued */
            TEST_CHECK(vjo_app_queue_save(&arena, &next, NULL) == VJO_APP_QUEUE_EXISTS);
        }
    }
    /* the queue's place is a file: nothing saved */
    reset();
    TEST_ASSERT(mkdir("ux0:data/VitaJPOverlay", 0777) == 0);
    FILE *f = fopen(VJO_QUEUE_DIR, "wb");
    TEST_ASSERT(f && !fclose(f));
    VjoAnkiNote n = card("猫", "", NULL);
    TEST_CHECK(vjo_app_queue_save(&arena, &n, NULL) == VJO_APP_QUEUE_FAILED && strstr(logs, "is not a directory"));
    /* over VJO_QUEUE_TEXT_MAX of text */
    static char big[VJO_QUEUE_TEXT_MAX];
    memset(big, 'a', sizeof(big) - 1);
    const char *meanings[1] = {big};
    reset();
    n = card("猫", "", NULL);
    n.meanings = meanings;
    TEST_CHECK(vjo_app_queue_save(&arena, &n, NULL) == VJO_APP_QUEUE_TOO_LARGE && arena.used == 0);
    TEST_CHECK(files(VJO_QUEUE_DIR, ".json") == 0);
}

TEST_LIST = {
    {"a card is saved once, as the overlay saves one", a_card_is_saved_once_as_the_overlay_saves_one},
    {"a failed save leaves no card", a_failed_save_leaves_no_card},
    {NULL, NULL},
};

/* ---------------- restart steps (test_anki_restart.py) ---------------- */

/* DIR ACTION [CRASH]: save (a typed 猫) or save-other (猫 of an earlier line). Prints one JSON line: the status as
 * the page says it, and the cards queued. */
static int restart_step(int argc, char **argv)
{
    VjoAnkiNote n;
    int rc;
    if (argc < 4 || chdir(argv[2]) < 0)
        return 2;
    mkdir("ux0:data", 0777);
    if (argc > 4 && argv[4][0])
        crash_point = argv[4];
    short_writes = 1; /* genuinely incomplete writes at json_partial */
    vjo_arena_init(&arena, mem, sizeof(mem));
    if (!strcmp(argv[3], "save"))
        n = card("猫", "", NULL);
    else if (!strcmp(argv[3], "save-other"))
        n = card("猫", "猫が鳴いた", "CLANNAD");
    else
        return 2;
    rc = vjo_app_queue_save(&arena, &n, NULL);
    printf("{\"status\":\"%s\",\"count\":%d}\n",
           rc == VJO_APP_QUEUE_SAVED ? "Saved" : rc == VJO_APP_QUEUE_EXISTS ? "Already queued" : "Failed",
           files(VJO_QUEUE_DIR, ".json"));
    return rc < 0;
}

int main(int argc, char **argv)
{
    const char *dir;
    if (argc >= 2 && !strcmp(argv[1], "--restart-step"))
        return restart_step(argc, argv);
    dir = vjo_test_tmpdir("app-queue");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
