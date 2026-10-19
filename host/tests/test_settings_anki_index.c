/* The Rubi app's request for Anki for Vita's duplicate index (settings/anki_index_request.c): what it
 * writes, when, and how it replaces one, on real files under a temporary directory, through the Vita's
 * file calls mapped onto POSIX, with a rename that refuses an existing name as the Vita's does
 * (SCE_ERROR_ERRNO_EEXIST) and failures injected at each call. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "../../settings/anki_index_request.h"
#include "../../settings/preferences.h"

/* ---------------- the memory card: real files ---------------- */

static int calls, fail_at, writes_opened, renames;
static int fail_now(void) { return fail_at && ++calls == fail_at; }
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (fail_now())
        return (int)0x80010005u;
    if (flags & (O_WRONLY | O_RDWR))
        writes_opened++;
    return native(open(p, flags, mode));
}
int sceIoClose(int fd) { int rc = close(fd); return fail_now() ? (int)0x80010005u : native(rc); }
int sceIoRead(int fd, void *p, size_t n) { return fail_now() ? (int)0x80010005u : native((int)read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n) { return fail_now() ? (int)0x80010005u : native((int)write(fd, p, n)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    if (fail_now())
        return (int)0x80010005u;
    if (stat(p, &s))
        return native(-1);
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    return 0;
}
int sceIoRename(const char *a, const char *b)
{
    struct stat s;
    if (fail_now())
        return (int)0x80010005u;
    if (stat(b, &s) == 0)
        return (int)0x80010011u; /* the Vita's rename refuses an existing name (EEXIST) */
    renames++;
    return native(rename(a, b));
}
int sceIoRemove(const char *p) { return fail_now() ? (int)0x80010005u : native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return fail_now() ? (int)0x80010005u : 0; }
int sceIoSyncByFd(int fd, int flag) { (void)flag; return fail_now() ? (int)0x80010005u : native(fsync(fd)); }

/* ---------------- fixtures ---------------- */

static uint8_t mem[256 * 1024];
static VjoArena arena;
static char msg[200];

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(text, 1, strlen(text), f) == strlen(text) && fclose(f) == 0);
}

static char *get(const char *path)
{
    static char buf[2][4096];
    static int k;
    char *out = buf[k++ % 2];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return NULL;
    n = fread(out, 1, sizeof(buf[0]) - 1, f);
    fclose(f);
    out[n] = '\0';
    return out;
}

static int exists(const char *path)
{
    struct stat s;
    return stat(path, &s) == 0;
}

#define DEFAULT_REQUEST "{\"v\": 1, \"notetypes\": [], \"key\": \"Expression\", \"reading\": \"ExpressionReading\"}\n"
#define TMP VJO_ANKI_REQUEST_PATH ".tmp"
#define OLD VJO_ANKI_REQUEST_PATH ".old"

/* A fresh card: Rubi's data, Anki for Vita's directory with interface.json (NULL: not installed). */
static void reset(const char *interface)
{
    unlink(VJO_PREFERENCES_PATH);
    unlink(VJO_ANKI_REQUEST_PATH);
    unlink(TMP);
    unlink(OLD);
    unlink(VJO_AFV_INTERFACE_PATH);
    rmdir(VJO_AFV_INDEX_DIR);
    if (interface)
        put(VJO_AFV_INTERFACE_PATH, interface);
    calls = fail_at = writes_opened = renames = 0;
    vjo_arena_init(&arena, mem, sizeof(mem));
}

static int request(void)
{
    int rc = vjo_anki_index_request(&arena, msg, sizeof(msg));
    TEST_CHECK(arena.used == 0); /* the scratch given back */
    return rc;
}

#define INTERFACE_1 "{\"v\": 1, \"app\": \"Anki for Vita\", \"app_version\": \"0.1.0\", \"interfaces\": [1]}"

/* ---------------- tests ---------------- */

static void test_nothing_without_anki_for_vita(void)
{
    static const char *const not_one[] = {"", "{}", "[1]", "{\"v\": 1, \"interfaces\": [2]}",
                                          "{\"v\": 1, \"interfaces\": 1}", "{\"v\": 1, \"interfaces\": [\"1\"]}",
                                          "{\"v\": 1, \"interfaces\": [1.5]}", "not json"};
    reset(NULL);
    TEST_CHECK(request() == VJO_ANKI_REQUEST_NO_APP && !exists(VJO_AFV_INDEX_DIR) && writes_opened == 0);
    for (size_t i = 0; i < sizeof(not_one) / sizeof(not_one[0]); i++) {
        reset(not_one[i]);
        TEST_CHECK_(request() == VJO_ANKI_REQUEST_NO_APP && writes_opened == 0, "%s", not_one[i]);
    }
    reset("{\"v\": 1, \"interfaces\": [3, 2, 1]}");
    TEST_CHECK(request() == VJO_ANKI_REQUEST_WRITTEN);
}

static void test_the_default_request_once(void)
{
    reset(INTERFACE_1);
    TEST_CHECK(request() == VJO_ANKI_REQUEST_WRITTEN);
    TEST_CHECK(!strcmp(get(VJO_ANKI_REQUEST_PATH), DEFAULT_REQUEST));
    TEST_MSG("%s", get(VJO_ANKI_REQUEST_PATH));
    TEST_CHECK(!exists(TMP) && !exists(OLD) && strstr(msg, "requested (Expression, ExpressionReading)"));
    /* again: the same request, nothing written */
    writes_opened = renames = 0;
    TEST_CHECK(request() == VJO_ANKI_REQUEST_SAME && writes_opened == 0 && renames == 0);
}

static void test_the_fields_config_ini_names(void)
{
    reset(INTERFACE_1);
    put(VJO_PREFERENCES_PATH, "anki_mining = on\n anki_duplicate_field = Word ; JPMN's\r\n"
                              "ANKI_DUPLICATE_READING_FIELD=WordReading\n");
    TEST_CHECK(request() == VJO_ANKI_REQUEST_WRITTEN);
    TEST_CHECK(!strcmp(get(VJO_ANKI_REQUEST_PATH),
                       "{\"v\": 1, \"notetypes\": [], \"key\": \"Word\", \"reading\": \"WordReading\"}\n"));
    /* other fields: the request replaced (the old one renamed aside, then removed) */
    put(VJO_PREFERENCES_PATH, "anki_duplicate_field = Vocab \"Kanji\"\\x\nanki_duplicate_reading_field = none\n");
    renames = 0;
    TEST_CHECK(request() == VJO_ANKI_REQUEST_WRITTEN && renames == 2 && !exists(OLD) && !exists(TMP));
    TEST_CHECK(!strcmp(get(VJO_ANKI_REQUEST_PATH),
                       "{\"v\": 1, \"notetypes\": [], \"key\": \"Vocab \\\"Kanji\\\"\\\\x\", \"reading\": \"\"}\n"));
    TEST_MSG("%s", get(VJO_ANKI_REQUEST_PATH));
    /* off: nothing done, the request left as it is */
    put(VJO_PREFERENCES_PATH, "anki_duplicate_check = OFF\n");
    writes_opened = 0;
    TEST_CHECK(request() == VJO_ANKI_REQUEST_OFF && writes_opened == 0 && exists(VJO_ANKI_REQUEST_PATH));
}

static void test_the_config_lines(void)
{
    VjoAnkiRequestConfig c;
    static const char text[] =
        "\xEF\xBB\xBF; a comment\n"
        "# anki_duplicate_field = Commented\n"
        "anki_duplicate_check = off # inline\n"
        "\t\xEF\xBB\xBF" "anki_duplicate_field\t=\tKanji\r\n"
        "anki_duplicate_reading_field = \n"
        "anki_duplicate_check = maybe\n"
        "anki_duplicate_field = \n"
        "anki_duplicate_field = ab\tcd\n"
        "anki_duplicate_field = 0123456789012345678901234567890123456789012345678901234567890123\n";
    vjo_anki_request_config(&c, "", 0);
    TEST_CHECK(c.on && !strcmp(c.field, "Expression") && !strcmp(c.reading, "ExpressionReading") && !c.warnings);
    vjo_anki_request_config(&c, text, sizeof(text) - 1);
    TEST_CHECK(!c.on && !strcmp(c.field, "Kanji") && !c.reading[0]);
    TEST_CHECK(c.warnings == 4); /* maybe, an empty field, a tab in a name, 64 bytes */
    TEST_MSG("on %d field %s reading %s warnings %d", c.on, c.field, c.reading, c.warnings);
}

/* A write stopped between its renames: only the .old (undone: renamed back), or both (finished:
 * the .old removed). */
static void test_a_leftover_is_finished_or_undone(void)
{
    reset(INTERFACE_1);
    TEST_ASSERT(sceIoMkdir(VJO_AFV_INDEX_DIR, 0777) == 0);
    put(OLD, DEFAULT_REQUEST);
    TEST_CHECK(request() == VJO_ANKI_REQUEST_SAME && !exists(OLD) && exists(VJO_ANKI_REQUEST_PATH));
    put(OLD, "an older request\n");
    writes_opened = 0;
    TEST_CHECK(request() == VJO_ANKI_REQUEST_SAME && !exists(OLD) && writes_opened == 0);
    put(TMP, "a stopped write's");
    TEST_CHECK(request() == VJO_ANKI_REQUEST_SAME); /* (a .tmp is ignored, and replaced by the next write) */
    put(VJO_PREFERENCES_PATH, "anki_duplicate_field = Word\n");
    TEST_CHECK(request() == VJO_ANKI_REQUEST_WRITTEN && !exists(TMP) && !exists(OLD));
}

/* Every call failing in turn: a request is there afterwards (the old or the new one, whole), or none
 * only when there was none before; a .tmp never stays where it would be read. */
static void test_every_failure_leaves_a_whole_request(void)
{
    int k, failures = 0, olds = 0;
    for (k = 1; k < 200; k++) {
        int rc;
        const char *now;
        reset(INTERFACE_1);
        TEST_ASSERT(request() == VJO_ANKI_REQUEST_WRITTEN); /* the default one */
        put(VJO_PREFERENCES_PATH, "anki_duplicate_field = Word\n");
        calls = 0;
        fail_at = k;
        rc = request();
        fail_at = 0;
        now = get(VJO_ANKI_REQUEST_PATH);
        if (!now) { /* a reader then finds the .old (the interface) */
            now = get(OLD);
            olds++;
        }
        TEST_CHECK_(now && (!strcmp(now, DEFAULT_REQUEST) ||
                            !strcmp(now, "{\"v\": 1, \"notetypes\": [], \"key\": \"Word\", \"reading\": \"ExpressionReading\"}\n")),
                    "call %d: %s", k, now ? now : "(none)");
        if (calls < k) { /* nothing failed */
            TEST_CHECK(rc == VJO_ANKI_REQUEST_WRITTEN);
            break;
        }
        failures += rc == VJO_ANKI_REQUEST_FAILED;
        /* the next start finishes it */
        TEST_CHECK_(request() == VJO_ANKI_REQUEST_WRITTEN || !exists(OLD), "call %d: after", k);
        TEST_CHECK(request() == VJO_ANKI_REQUEST_SAME && !exists(OLD));
    }
    TEST_CHECK(k < 200 && failures > 5);
    printf("measure: request: %d file calls; a failure at each left a whole request (%d times the old one under .old)\n",
           k - 1, olds);
}

TEST_LIST = {
    {"nothing_without_anki_for_vita", test_nothing_without_anki_for_vita},
    {"the_default_request_once", test_the_default_request_once},
    {"the_fields_config_ini_names", test_the_fields_config_ini_names},
    {"the_config_lines", test_the_config_lines},
    {"a_leftover_is_finished_or_undone", test_a_leftover_is_finished_or_undone},
    {"every_failure_leaves_a_whole_request", test_every_failure_leaves_a_whole_request},
    {NULL, NULL}};

int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("settings-anki-index");
    if (!dir || chdir(dir) < 0 || mkdir("ux0:data", 0777) < 0 || mkdir("ux0:data/VitaJPOverlay", 0777) < 0 ||
        mkdir(VJO_AFV_DIR, 0777) < 0)
        return 2;
    return acutest_main(argc, argv);
}
