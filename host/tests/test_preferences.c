#include "acutest.h"
#include "../../settings/preferences.h"
#include "../../shell/shell.h"
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned char memory[64 * 1024];
static VjoArena arena;
static int read_fail, write_fail, close_fail, sync_fail, syncs, rename_fail, rename_fail_after, renames;
static int write_fd = -1, handles;
static size_t written;
static int native(int n) { return n < 0 ? (int)(0x80010000u | errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (read_fail && flags == SCE_O_RDONLY) return (int)0x80010013u;
    int fd = open(p, flags, mode);
    if (fd >= 0) { handles++; if (flags & SCE_O_WRONLY) write_fd = fd; }
    return native(fd);
}
int sceIoClose(int fd)
{
    int n = close(fd); if (!n) handles--;
    int failed = close_fail && fd == write_fd;
    if (fd == write_fd) write_fd = -1;
    return failed ? -1 : native(n);
}
int sceIoRead(int fd, void *p, size_t len) { return native(read(fd, p, len > 13 ? 13 : len)); }
int sceIoWrite(int fd, const void *p, size_t len)
{
    if (write_fail && written >= 21) return -1;
    int n = (int)write(fd, p, len > 7 ? 7 : len);
    if (n > 0) written += n;
    return native(n);
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{ struct stat st; int rc = stat(p, &st); if (!rc) out->st_mode = st.st_mode; return native(rc); }
int sceIoRename(const char *a, const char *b)
{ ++renames; if (renames == rename_fail || (rename_fail_after && renames >= rename_fail_after)) return -1;
  return native(rename(a, b)); }
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return ++syncs == sync_fail ? -1 : 0; }

static const char original[] =
    "; Keep this comment\r\nocr_backend = meiki\nocr_mode = on_press\n"
    "japanese_only_selection = on\n"
    "dictionary = local\nlocal_dictionaries = daijirin.vjdict,jmdict.vjdict\n"
    "ocr_model_dir = ux0:data/custom-ocr\nvocr_model = H15_w8.vocr\n"
    "jpdb_api_key = example-jpdb\njiten_api_key = example-jiten\n"
    "font_size_ja = 22\nunknown_future_setting = unchanged\n";

static void put_file(const char *path, const char *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    TEST_ASSERT(fd >= 0); TEST_ASSERT(write(fd, data, n) == (ssize_t)n); TEST_ASSERT(close(fd) == 0);
}
static void reset(void)
{
    static int initialized;
    if (!initialized) {
        char dir[] = "/tmp/vjo-preferences-XXXXXX";
        TEST_ASSERT(mkdtemp(dir)); TEST_ASSERT(chdir(dir) == 0);
        mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); initialized = 1;
    }
    unlink(VJO_PREFERENCES_PATH); unlink(VJO_PREFERENCES_PATH ".bak"); unlink(VJO_PREFERENCES_PATH ".tmp");
    read_fail = write_fail = close_fail = sync_fail = syncs = rename_fail = rename_fail_after = renames = handles = 0;
    write_fd = -1; written = 0;
    vjo_arena_init(&arena, memory, sizeof(memory));
}
static void check_choices(int backend, int japanese)
{
    VjoConfig cfg;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) >= 0);
    TEST_CHECK(cfg.ocr_backend == backend && cfg.japanese_only_selection == japanese);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.font_size_ja == 22);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr") && !strcmp(cfg.vocr_model, "H15_w8.vocr"));
    TEST_CHECK(!strcmp(cfg.api_key[VJO_DICT_JPDB], "example-jpdb"));
    TEST_CHECK(!strcmp(cfg.api_key[VJO_DICT_JITEN], "example-jiten"));
    TEST_CHECK(arena.used == 0 && handles == 0);
}
static void test_patch_preserves_unrelated_bytes(void)
{
    const char input[] = "\xEF\xBB\xBF  OCR_BACKEND=meiki\r\n"
        "; comment about ocr_backend=x\r\njpdb_api_key = keep-this\r\n"
        "ocr_backend = lens\njapanese_only_selection=off\n"
        "JAPANESE_ONLY_SELECTION = on\nfuture = raw;#value";
    const char expected[] = "\xEF\xBB\xBFocr_backend = vocr\n"
        "; comment about ocr_backend=x\r\njpdb_api_key = keep-this\r\n"
        "japanese_only_selection = on\nfuture = raw;#value";
    char out[1024];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_OCR_VOCR, 1, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    n = vjo_config_patch_preferences("font_size_ja=18", 15, VJO_OCR_MEIKI, 0, out, sizeof(out));
    TEST_CHECK(n > 0 && strstr(out, "font_size_ja=18\nocr_backend = meiki\n"));
    TEST_CHECK(strstr(out, "japanese_only_selection = off\n") != NULL);
    TEST_CHECK(vjo_config_patch_preferences(input, sizeof(input)-1, VJO_OCR_VOCR, 1, out, 5) < 0);
    TEST_CHECK(vjo_config_patch_preferences("bad\0key", 7, VJO_OCR_VOCR, 1, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences("", 0, -1, 1, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences("", 0, VJO_OCR_VOCR, 2, out, sizeof(out)) < 0);
}
static void test_save_switch_reload_retains_other_settings(void)
{
    reset(); VjoConfig cfg;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_VOCR && cfg.japanese_only_selection == 1);
    put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    TEST_ASSERT(vjo_preferences_save(VJO_OCR_VOCR, 1, &arena) == 0);
    check_choices(VJO_OCR_VOCR, 1);
    TEST_ASSERT(vjo_preferences_save(VJO_OCR_MEIKI, 0, &arena) == 0);
    check_choices(VJO_OCR_MEIKI, 0);
    char out[sizeof(original)+128];
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    ssize_t n = read(fd, out, sizeof(out)-1); close(fd); TEST_ASSERT(n > 0); out[n] = 0;
    TEST_CHECK(strstr(out, "; Keep this comment\r\n") && strstr(out, "unknown_future_setting = unchanged\n"));
    TEST_ASSERT(vjo_preferences_save(VJO_OCR_VOCR, 1, &arena) == 0); check_choices(VJO_OCR_VOCR, 1);
}
static void test_save_failure_preserves_or_recovers_actual_choices(void)
{
    for (int fault = 0; fault < 6; fault++) {
        reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
        if (fault == 0) write_fail = 1;
        if (fault == 1) close_fail = 1;
        if (fault == 2) sync_fail = 1;
        if (fault == 3) rename_fail = 1;
        if (fault == 4) rename_fail = 2;
        if (fault == 5) sync_fail = 2; /* published, but final confirmation failed */
        TEST_CHECK(vjo_preferences_save(VJO_OCR_VOCR, 0, &arena) < 0);
        check_choices(fault == 5 ? VJO_OCR_VOCR : VJO_OCR_MEIKI, fault == 5 ? 0 : 1);
        TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) == 0);
    }
    reset(); put_file(VJO_PREFERENCES_PATH ".bak", original, sizeof(original)-1);
    VjoConfig cfg; TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1);
    TEST_ASSERT(vjo_preferences_save(VJO_OCR_VOCR, 1, &arena) == 0); check_choices(VJO_OCR_VOCR, 1);
}
static void test_unreadable_or_invalid_config_never_overwritten(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1); read_fail = 1;
    VjoConfig cfg;
    TEST_CHECK(vjo_preferences_load(&cfg, &arena) < 0);
    TEST_CHECK(vjo_preferences_save(VJO_OCR_VOCR, 1, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0 && handles == 0);
    read_fail = 0; check_choices(VJO_OCR_MEIKI, 1);
    put_file(VJO_PREFERENCES_PATH, "broken\0config", 13);
    TEST_CHECK(vjo_preferences_save(VJO_OCR_VOCR, 1, &arena) < 0);
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
    memset(memory, '#', VJO_PREFERENCES_TEXT_MAX);
    put_file(VJO_PREFERENCES_PATH, (char *)memory, VJO_PREFERENCES_TEXT_MAX);
    TEST_CHECK(vjo_preferences_save(VJO_OCR_VOCR, 1, &arena) < 0);
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
}
static void test_backup_only_loads_actual_shell_without_default_publication(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    rename_fail_after = 2; /* Fail both temp publication and primary restoration. */
    TEST_ASSERT(vjo_preferences_save(VJO_OCR_VOCR, 0, &arena) < 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) < 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH ".bak", F_OK) == 0);
    VjoConfig cfg;
    TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.japanese_only_selection == 1);
    unsigned char shell_mem[VJO_CONFIG_TEXT_MAX + 1];
    VjoArena shell; vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    size_t writes_before = written; int renames_before = renames;
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.japanese_only_selection == 1);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr") && !strcmp(cfg.vocr_model, "H15_w8.vocr"));
    TEST_CHECK(!strcmp(cfg.api_key[VJO_DICT_JPDB], "example-jpdb"));
    TEST_CHECK(written == writes_before && renames == renames_before);
    TEST_CHECK(shell.used == 0 && shell.peak <= sizeof(shell_mem) && handles == 0);
    char text[sizeof(original)]; int fd = open(VJO_PREFERENCES_PATH ".bak", O_RDONLY);
    ssize_t n = read(fd, text, sizeof(text)); close(fd);
    TEST_CHECK(n == sizeof(original)-1 && !memcmp(text, original, n));
    /* A present but unreadable/corrupt backup must never be overwritten. */
    read_fail = 1; vjo_config_load(&cfg, &shell);
    TEST_CHECK(written == writes_before && renames == renames_before && handles == 0);
    read_fail = 0; put_file(VJO_PREFERENCES_PATH ".bak", "bad\0config", 10);
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(written == writes_before && renames == renames_before && handles == 0);
}
static void test_maximum_published_config_fits_actual_shell_scratch(void)
{
    reset(); char near[VJO_CONFIG_TEXT_MAX + 1];
    memset(near, '#', sizeof(near)); memcpy(near, original, sizeof(original)-1);
    near[VJO_CONFIG_TEXT_MAX-1] = '\n'; near[VJO_CONFIG_TEXT_MAX] = 0;
    put_file(VJO_PREFERENCES_PATH, near, VJO_CONFIG_TEXT_MAX);
    TEST_ASSERT(vjo_preferences_save(VJO_OCR_MEIKI, 1, &arena) == 0);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    VjoConfig cfg; vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.japanese_only_selection == 1);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.api_key[VJO_DICT_JITEN], "example-jiten"));
    TEST_CHECK(shell.peak == sizeof(shell_mem) && shell.used == 0 && handles == 0);
    /* Adding one byte would exceed the reader, so the original stays intact. */
    written = 0; renames = 0;
    TEST_CHECK(vjo_preferences_save(VJO_OCR_MEIKI, 0, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0);
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.japanese_only_selection == 1);
}
TEST_LIST = {
    {"preference_patch_preserves_comments_BOM_keys_and_collapses_duplicates", test_patch_preserves_unrelated_bytes},
    {"saved_backend_and_selection_choices_survive_switch_reload", test_save_switch_reload_retains_other_settings},
    {"partial_write_close_sync_and_rename_failures_retain_confirmable_choices", test_save_failure_preserves_or_recovers_actual_choices},
    {"unreadable_embedded_NUL_and_oversize_config_are_not_overwritten", test_unreadable_or_invalid_config_never_overwritten},
    {"backup_only_config_loads_in_actual_Shell_without_overwriting_saved_keys", test_backup_only_loads_actual_shell_without_default_publication},
    {"largest_published_config_round_trips_through_actual_24KiB_Shell_loader", test_maximum_published_config_fits_actual_shell_scratch},
    {NULL, NULL}
};
