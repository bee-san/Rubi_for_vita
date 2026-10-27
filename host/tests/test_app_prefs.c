/* The Rubi app's own settings file, ux0:data/VitaJPOverlay/app.ini (settings/app_prefs.c), on real files in a
 * temporary memory card: its lines read as config.ini's are (a BOM, CRLF, comments, any case, unknown keys and
 * values), every row of its table saved and read back, a save that keeps every other line verbatim and applies none
 * of config.ini's removed-key rules, the backup a save keeps and a load's use of it, and the failures that must
 * leave the file as it was. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "../../settings/app_prefs.h"
#include "../../settings/preferences.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned char memory[64 * 1024];
static VjoArena arena;
static int read_fail, write_fail, rename_fail, sync_fail, renames, writes;
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (read_fail && flags == SCE_O_RDONLY && strstr(p, "app.ini")) return (int)0x80010013u;
    if (flags & SCE_O_WRONLY) writes++;
    return native(open(p, flags, mode));
}
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t len) { return native((int)read(fd, p, len > 5 ? 5 : len)); } /* short reads */
int sceIoWrite(int fd, const void *p, size_t len)
{
    if (write_fail) return -1;
    return native((int)write(fd, p, len > 3 ? 3 : len)); /* short writes */
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{
    struct stat st;
    int rc = stat(p, &st);
    if (!rc) out->st_mode = st.st_mode;
    return native(rc);
}
int sceIoRename(const char *a, const char *b)
{
    if (++renames == rename_fail) return -1;
    return native(rename(a, b));
}
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return sync_fail ? -1 : 0; }

#define APP VJO_APP_PREFS_PATH
#define BAK VJO_APP_PREFS_PATH ".bak"
#define TMP VJO_APP_PREFS_PATH ".tmp"

/* A new empty memory card for each test, the working directory. */
static void fresh(void)
{
    char dir[PATH_MAX];
    TEST_ASSERT(vjo_test_tmpdir_new(dir, sizeof(dir)) != NULL);
    TEST_ASSERT(chdir(dir) == 0);
    TEST_ASSERT(mkdir("ux0:data", 0777) == 0 && mkdir("ux0:data/VitaJPOverlay", 0777) == 0);
    read_fail = write_fail = rename_fail = sync_fail = renames = writes = 0;
    vjo_arena_init(&arena, memory, sizeof(memory));
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(text, 1, strlen(text), f) == strlen(text));
    fclose(f);
}

static const char *get(const char *path)
{
    static char text[8192];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

static int load(VjoAppPrefs *p)
{
    vjo_arena_init(&arena, memory, sizeof(memory));
    return vjo_app_prefs_load(p, &arena);
}

static int save1(const char *key, int value)
{
    const char *keys[1] = {key};
    int values[1] = {value};
    vjo_arena_init(&arena, memory, sizeof(memory));
    return vjo_app_prefs_save(keys, values, 1, &arena);
}

static void test_defaults_without_a_file(void)
{
    VjoAppPrefs p;
    fresh();
    memset(&p, 0x5A, sizeof(p));
    TEST_CHECK(load(&p) == 0);
    TEST_CHECK(p.anki_duplicates == VJO_DUPLICATES_TAG);
    TEST_CHECK(get(APP) == NULL && writes == 0); /* a load writes nothing */
}

static void test_lines_read_as_config_ini_reads_them(void)
{
    static const struct { const char *text; int duplicates; } cases[] = {
        {"anki_duplicates = skip\n", VJO_DUPLICATES_SKIP},
        {"\xEF\xBB\xBF" "anki_duplicates = keep\r\n", VJO_DUPLICATES_KEEP}, /* Notepad's BOM and CRLF */
        {"ANKI_Duplicates=SKIP", VJO_DUPLICATES_SKIP},                       /* any case, no spaces, no newline */
        {"  anki_duplicates\t=\tskip   ; a comment\n", VJO_DUPLICATES_SKIP},
        {"anki_duplicates = keep # a comment\n", VJO_DUPLICATES_KEEP},
        {"; anki_duplicates = skip\n# anki_duplicates = keep\n", VJO_DUPLICATES_TAG}, /* comments */
        {"anki_duplicates = maybe\n", VJO_DUPLICATES_TAG},                  /* an unknown value: the default */
        {"anki_duplicates = skip;not a comment\n", VJO_DUPLICATES_TAG},     /* ';' without a space is the value's */
        {"anki_duplicates skip\n", VJO_DUPLICATES_TAG},                     /* no '=' */
        {"anki_duplicates_x = skip\nx_anki_duplicates = keep\n", VJO_DUPLICATES_TAG},
        {"anki_duplicates = skip\nanki_duplicates = keep\n", VJO_DUPLICATES_KEEP}, /* the last line wins */
        {"anki_duplicates = skip\nanki_duplicates = nonsense\n", VJO_DUPLICATES_SKIP},
        {"unknown_key = 1\n\n\nanki_duplicates = keep\n", VJO_DUPLICATES_KEEP},
        {"anki_duplicates =\n", VJO_DUPLICATES_TAG},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoAppPrefs p;
        vjo_app_prefs_defaults(&p);
        vjo_app_prefs_parse(&p, cases[i].text, strlen(cases[i].text));
        TEST_CHECK_(p.anki_duplicates == cases[i].duplicates, "case %u: %d", i, p.anki_duplicates);
    }
}

static void test_every_row_saves_and_loads_each_value(void)
{
    const char *key;
    fresh();
    TEST_CHECK(vjo_app_prefs_key(0) != NULL);
    for (int i = 0; (key = vjo_app_prefs_key(i)); i++) {
        VjoAppPrefs p;
        int n = 0;
        for (const char *c = key; *c; c++)
            TEST_CHECK_(!(*c >= 'A' && *c <= 'Z') && *c != ' ', "%s: a lowercase key", key);
        vjo_app_prefs_defaults(&p);
        TEST_CHECK(vjo_app_prefs_get(&p, key) >= 0);
        while (vjo_app_prefs_value(key, n)) {
            TEST_CHECK_(strlen(vjo_app_prefs_value(key, n)) < 32, "%s: a short value name", key);
            n++;
        }
        TEST_CHECK_(n >= 2, "%s: %d values", key, n);
        for (int v = n - 1; v >= 0; v--) {
            TEST_CHECK_(save1(key, v) == 0, "%s = %d", key, v);
            TEST_CHECK(load(&p) == 0);
            TEST_CHECK_(vjo_app_prefs_get(&p, key) == v, "%s = %d read back", key, v);
        }
        TEST_CHECK(vjo_app_prefs_value(key, n) == NULL && vjo_app_prefs_value(key, -1) == NULL);
    }
    TEST_CHECK(vjo_app_prefs_get(NULL, "no_such_key") == -1);
    TEST_CHECK(vjo_app_prefs_value("no_such_key", 0) == NULL);
}

static void test_a_save_keeps_every_other_line(void)
{
    /* config.ini's removed-key rules (an API key's line, ocr_backend, anki_host) do not apply to app.ini */
    static const char before[] =
        "\xEF\xBB\xBF; my notes\r\n"
        "jpdb_api_key = 0123\n"
        "ocr_backend = lens\n"
        "anki_host = 192.168.0.2\n"
        "Anki_Duplicates = keep ; was keep\n"
        "a future key = 7\n"
        "anki_duplicates = tag\n"
        "last line without a newline";
    static const char after[] =
        "\xEF\xBB\xBF; my notes\r\n"
        "jpdb_api_key = 0123\n"
        "ocr_backend = lens\n"
        "anki_host = 192.168.0.2\n"
        "anki_duplicates = skip\n"
        "a future key = 7\n"
        "last line without a newline";
    VjoAppPrefs p;
    fresh();
    put(APP, before);
    TEST_CHECK(load(&p) == 0 && p.anki_duplicates == VJO_DUPLICATES_TAG);
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == 0);
    TEST_CHECK(strcmp(get(APP), after) == 0);
    TEST_MSG("%s", get(APP));
    TEST_CHECK(strcmp(get(BAK), before) == 0); /* the old file is the backup */
    TEST_CHECK(get(TMP) == NULL);
    TEST_CHECK(load(&p) == 0 && p.anki_duplicates == VJO_DUPLICATES_SKIP);
    /* a key the file has not got is appended, after a last line without a newline */
    put(APP, "x = 1");
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_KEEP) == 0);
    TEST_CHECK(strcmp(get(APP), "x = 1\nanki_duplicates = keep\n") == 0);
    /* and into no file at all */
    TEST_CHECK(unlink(APP) == 0 && unlink(BAK) == 0);
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_TAG) == 0);
    TEST_CHECK(strcmp(get(APP), "anki_duplicates = tag\n") == 0);
    TEST_CHECK(get(BAK) == NULL);
}

static void test_a_save_refuses_what_the_table_has_not_got(void)
{
    const char *keys[3] = {"anki_duplicates", "anki_duplicates", "no_such_key"};
    int values[3] = {VJO_DUPLICATES_SKIP, VJO_DUPLICATES_KEEP, 0};
    fresh();
    put(APP, "anki_duplicates = keep\n");
    TEST_CHECK(save1("no_such_key", 0) == -1);
    TEST_CHECK(save1("anki_duplicates", 3) == -1);
    TEST_CHECK(save1("anki_duplicates", -1) == -1);
    TEST_CHECK(save1(NULL, 0) == -1);
    vjo_arena_init(&arena, memory, sizeof(memory));
    TEST_CHECK(vjo_app_prefs_save(keys, values, 2, &arena) == -1); /* the same setting twice */
    TEST_CHECK(vjo_app_prefs_save(keys, values, 0, &arena) == -1);
    TEST_CHECK(vjo_app_prefs_save(keys, values, 9, &arena) == -1);
    TEST_CHECK(writes == 0 && renames == 0);
    TEST_CHECK(strcmp(get(APP), "anki_duplicates = keep\n") == 0);
}

static void test_failures_leave_the_file_as_it_was(void)
{
    VjoAppPrefs p;
    fresh();
    put(APP, "anki_duplicates = keep\n");
    /* unreadable: the load gives the defaults and says so; a save never rewrites it */
    read_fail = 1;
    TEST_CHECK(load(&p) == -1 && p.anki_duplicates == VJO_DUPLICATES_TAG);
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == -1);
    TEST_CHECK(writes == 0);
    read_fail = 0;
    /* a failed write, sync or first rename: the old file stays */
    write_fail = 1;
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == -1);
    write_fail = 0;
    sync_fail = 1;
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == -1);
    sync_fail = 0;
    renames = 0;
    rename_fail = 1; /* app.ini -> app.ini.bak */
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == -1);
    TEST_CHECK(strcmp(get(APP), "anki_duplicates = keep\n") == 0);
    /* the second rename (app.ini.tmp -> app.ini) fails: the old file is put back */
    renames = 0;
    rename_fail = 2;
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == -1);
    TEST_CHECK(strcmp(get(APP), "anki_duplicates = keep\n") == 0);
    rename_fail = 0;
    TEST_CHECK(load(&p) == 0 && p.anki_duplicates == VJO_DUPLICATES_KEEP);
}

static void test_a_save_stopped_between_its_renames(void)
{
    /* app.ini moved to app.ini.bak, the new one not yet in place: a load reads the backup, a save starts from it */
    VjoAppPrefs p;
    fresh();
    put(BAK, "anki_duplicates = skip\nmine = 1\n");
    TEST_CHECK(load(&p) == 1 && p.anki_duplicates == VJO_DUPLICATES_SKIP);
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_KEEP) == 0);
    TEST_CHECK(strcmp(get(APP), "anki_duplicates = keep\nmine = 1\n") == 0);
    TEST_CHECK(load(&p) == 0 && p.anki_duplicates == VJO_DUPLICATES_KEEP);
}

static void test_config_ini_is_never_touched(void)
{
    static const char config[] = "anki_mining = on\nocr_backend = lens\n";
    VjoAppPrefs p;
    fresh();
    put(VJO_PREFERENCES_PATH, config);
    TEST_CHECK(save1("anki_duplicates", VJO_DUPLICATES_SKIP) == 0);
    TEST_CHECK(load(&p) == 0 && p.anki_duplicates == VJO_DUPLICATES_SKIP);
    TEST_CHECK(strcmp(get(VJO_PREFERENCES_PATH), config) == 0);
    TEST_CHECK(get(VJO_PREFERENCES_PATH ".bak") == NULL);
}

TEST_LIST = {
    {"defaults_without_a_file", test_defaults_without_a_file},
    {"lines_read_as_config_ini_reads_them", test_lines_read_as_config_ini_reads_them},
    {"every_row_saves_and_loads_each_value", test_every_row_saves_and_loads_each_value},
    {"a_save_keeps_every_other_line", test_a_save_keeps_every_other_line},
    {"a_save_refuses_what_the_table_has_not_got", test_a_save_refuses_what_the_table_has_not_got},
    {"failures_leave_the_file_as_it_was", test_failures_leave_the_file_as_it_was},
    {"a_save_stopped_between_its_renames", test_a_save_stopped_between_its_renames},
    {"config_ini_is_never_touched", test_config_ini_is_never_touched},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("app-prefs")) return 2;
    return acutest_main(argc, argv);
}
