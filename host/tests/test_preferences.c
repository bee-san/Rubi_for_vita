#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "../../settings/preferences.h"
#include "../../shell/shell.h"
#include "../../core/local_dict.h"
#include "../../core/regions.h" /* #358: VJO_REGION_* of the Shell's region.ini loads */
#include "../../include/vjo_vocr_sizes.h"
#include "config_golden.h" /* #418: the saves' golden file */
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char memory[64 * 1024];
static VjoArena arena;
static int read_fail, write_fail, close_fail, sync_fail, syncs, rename_fail, rename_fail_after, renames;
static int write_fd = -1, handles;
static size_t written;
static int whole_writes; /* #358: a write takes all its bytes, as the Shell's one-call vjo_file_write needs */
static int expected_results = 2; /* the fixture's local_dictionary_results */
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
    int n = (int)write(fd, p, len > 7 && !whole_writes ? 7 : len);
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
    "; Keep this comment\r\nocr_backend = lens\nocr_mode = on_press\n"
    "japanese_only_selection = on\n"
    "highlight_mode = word\n"
    "dictionary = local\nlocal_dictionaries = daijirin.vjdict,jmdict.vjdict\n"
    ""
    "ocr_model_dir = ux0:data/custom-ocr\nvocr_model = H15_w8.vocr\n"
    "hachidori_host = 192.168.1.20\n" /* the removed relay's: ignored, kept as written */
    "font_size_ja = 22\nunknown_future_setting = unchanged\n";

static void put_file(const char *path, const char *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    TEST_ASSERT(fd >= 0); TEST_ASSERT(write(fd, data, n) == (ssize_t)n); TEST_ASSERT(close(fd) == 0);
}
static void reset(void)
{
    static int initialized;
    expected_results = 2;
    if (!initialized) {
        char dir[PATH_MAX];
        TEST_ASSERT(vjo_test_tmpdir_new(dir, sizeof(dir)) != NULL); TEST_ASSERT(chdir(dir) == 0);
        mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); initialized = 1;
    }
    unlink(VJO_PREFERENCES_PATH); unlink(VJO_PREFERENCES_PATH ".bak"); unlink(VJO_PREFERENCES_PATH ".tmp");
    read_fail = write_fail = close_fail = sync_fail = syncs = rename_fail = rename_fail_after = renames = handles = 0;
    write_fd = -1; written = 0;
    vjo_arena_init(&arena, memory, sizeof(memory));
}
/* The saved choices (the fixture's tier is normal, from its vocr_model line). */
static void check_choices(int tier, int japanese, int highlight_mode)
{
    VjoConfig cfg;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) >= 0);
    TEST_CHECK(cfg.vocr_tier == tier && cfg.japanese_only_selection == japanese);
    TEST_CHECK(cfg.highlight_mode == highlight_mode);
    TEST_CHECK(vjo_local_names_count(cfg.local_dictionaries) == expected_results);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.font_size_ja == 22);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
    TEST_CHECK(arena.used == 0 && handles == 0);
}
static void test_patch_preserves_unrelated_bytes(void)
{
    /* (the removed online dictionaries' API key lines go: test_removed_dictionaries_rewritten_by_saves;
     * so do the removed engine choice's: test_engine_choice_loads_as_vocr_and_saves_drop_it) */
    const char input[] = "\xEF\xBB\xBF  OCR_BACKEND=meiki\r\n"
        "; comment about ocr_backend=x\r\njpdb_api_key = drop-this\r\n"
        "ocr_backend = lens\njapanese_only_selection=off\n"
        "JAPANESE_ONLY_SELECTION = on\nHIGHLIGHT_MODE=character\n"
        "highlight_mode = word\nfuture = raw;#value";
    const char expected[] = "\xEF\xBB\xBF; comment about ocr_backend=x\r\n"
        "japanese_only_selection = on\nhighlight_mode = word\nfuture = raw;#value\n"
        "vocr_tier = normal\n";
    char out[1024];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    TEST_MSG("%s", out);
    n = vjo_config_patch_preferences("font_size_ja=18", 15, VJO_VOCR_TIER_NORMAL, 0, VJO_HIGHLIGHT_WORD, out, sizeof(out));
    TEST_CHECK(n > 0 && !strcmp(out, "font_size_ja=18\njapanese_only_selection = off\nhighlight_mode = word\n"
                                     "vocr_tier = normal\n"));
    TEST_MSG("%s", out);
    /* The engine is no choice (VOCR is the only one): a save never writes it. */
    TEST_CHECK(!strstr(out, "ocr_backend"));
    TEST_CHECK(vjo_config_patch_preferences("", 0, VJO_VOCR_TIER_NORMAL, 1, 2, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences(input, sizeof(input)-1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, out, 5) < 0);
    TEST_CHECK(vjo_config_patch_preferences("bad\0key", 7, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences("", 0, -1, 1, VJO_HIGHLIGHT_WORD, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences("", 0, VJO_VOCR_TIER_NORMAL, 2, VJO_HIGHLIGHT_WORD, out, sizeof(out)) < 0);
}

static void test_dictionary_results_patch_and_parse(void)
{
    const char input[] = "; local_dictionary_results = 5 in a comment\n"
        "LOCAL_DICTIONARY_RESULTS=7\nlocal_dictionaries = a.vjdict,b.vjdict\n"
        "local_dictionary_results = 3\nocr_backend = vocr\n"
        "japanese_only_selection = on\nhighlight_mode = word\n";
    const char expected[] = "; local_dictionary_results = 5 in a comment\n"
        "local_dictionaries = a.vjdict,b.vjdict\n"
        "japanese_only_selection = on\nhighlight_mode = word\nvocr_tier = normal\n";
    char out[512];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL,
                                        1, VJO_HIGHLIGHT_WORD, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    const char *old[] = {"1", "8", "0", "two", ""};
    for (unsigned i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
        VjoConfig cfg;
        char ini[128];
        snprintf(ini, sizeof(ini), "local_dictionary_results = %s\n", old[i]);
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, ini, strlen(ini));
        TEST_CHECK(!cfg.n_warnings);
    }
    TEST_CHECK(!strstr(vjo_config_default_text(), "local_dictionary_results"));
}
/* API 21: the Model size is owned like the other choices. The app's save writes
 * vocr_tier and drops an older build's vocr_model and vocr_fallback_model lines
 * (any case), keeps every other byte; an older file's vocr_model sets the tier
 * by its shape until a vocr_tier line says otherwise, in either order. */
static void test_model_tier_patch_parse_and_legacy_models(void)
{
    const char input[] = "; vocr_model = in a comment stays\nVOCR_MODEL = FL10_w8.vocr\nocr_backend = vocr\n"
        "vocr_fallback_model = none\nVocr_Tier=Large\nvocr_tier = small\nfont_size_ja = 22\n";
    const char expected[] = "; vocr_model = in a comment stays\nvocr_tier = normal\n"
        "font_size_ja = 22\njapanese_only_selection = on\nhighlight_mode = word\n";
    char out[512];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1,
                                         VJO_HIGHLIGHT_WORD, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    TEST_MSG("%s", out);
    for (int tier = -1; tier <= 5; tier++) { /* only the three tiers can be written */
        n = vjo_config_patch_preferences("", 0, tier, 1, VJO_HIGHLIGHT_WORD, out, sizeof(out));
        TEST_CHECK_((n > 0) == (tier >= 1 && tier <= 3), "tier %d: %d", tier, n);
        if (n > 0) {
            char line[32];
            snprintf(line, sizeof(line), "vocr_tier = %s\n", vjo_vocr_tier_id(tier));
            TEST_CHECK(strstr(out, line) != NULL);
        }
    }
    /* parsed: the default, each value, and what an older vocr_model maps to */
    static const struct { const char *ini; int tier, warnings; } cases[] = {
        {"", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_tier = small\n", VJO_VOCR_TIER_SMALL, 0},
        {"vocr_tier = NORMAL\n", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_tier = large\n", VJO_VOCR_TIER_LARGE, 0},
        {"vocr_tier = huge\n", VJO_VOCR_TIER_NORMAL, 1},
        {"vocr_tier =\n", VJO_VOCR_TIER_NORMAL, 1},
        {"vocr_model = FL10_w8.vocr\n", VJO_VOCR_TIER_SMALL, 0},
        {"vocr_model = VOCR3_FL10_w8.vocr\n", VJO_VOCR_TIER_SMALL, 0},
        {"vocr_model = VOCR2_FL10_w8.vocr\n", VJO_VOCR_TIER_SMALL, 0},
        {"vocr_model = H15_w8.vocr\n", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_model = VOCR3_H15_w8.vocr\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_model = F20_w8.vocr\n", VJO_VOCR_TIER_LARGE, 0},
        {"vocr_model = VOCR4_W20_w8.vocr\n", VJO_VOCR_TIER_LARGE, 0},
        {"vocr_model = VOCR3_W20_w8.vocr\n", VJO_VOCR_TIER_LARGE, 0},
        /* another name gives no size, without a warning (a removed setting, #418) */
        {"vocr_model = unknown_w8.vocr\n", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_model = ../H15_w8.vocr\n", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_model = H15_w8.bin\n", VJO_VOCR_TIER_NORMAL, 0},
        /* a vocr_tier line wins over a vocr_model one, before or after it */
        {"vocr_tier = small\nvocr_model = VOCR4_W20_w8.vocr\n", VJO_VOCR_TIER_SMALL, 0},
        {"vocr_model = VOCR4_W20_w8.vocr\nvocr_tier = small\n", VJO_VOCR_TIER_SMALL, 0},
        {"vocr_model = FL10_w8.vocr\nvocr_tier = huge\n", VJO_VOCR_TIER_SMALL, 1},
        {"vocr_fallback_model = none\n", VJO_VOCR_TIER_NORMAL, 0},
        {"vocr_fallback_model = ../x.vocr\n", VJO_VOCR_TIER_NORMAL, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoConfig cfg;
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, cases[i].ini, strlen(cases[i].ini));
        TEST_CHECK_(cfg.vocr_tier == cases[i].tier && cfg.n_warnings == cases[i].warnings,
                    "'%s': tier %d, %d warnings (%s)", cases[i].ini, cfg.vocr_tier, cfg.n_warnings,
                    cfg.n_warnings ? cfg.warnings[0] : "");
    }
    /* the helpers: values, models, shapes */
    TEST_CHECK(!strcmp(vjo_vocr_tier_id(1), "small") && !strcmp(vjo_vocr_tier_id(2), "normal") &&
               !strcmp(vjo_vocr_tier_id(3), "large") && !vjo_vocr_tier_id(0) && !vjo_vocr_tier_id(4));
    TEST_CHECK(!strcmp(vjo_vocr_tier_model(1), VJO_VOCR_TIER1_NAME) && !strcmp(vjo_vocr_tier_model(2), VJO_VOCR_TIER2_NAME) &&
               !strcmp(vjo_vocr_tier_model(3), VJO_VOCR_TIER3_NAME) && !vjo_vocr_tier_model(0) && !vjo_vocr_tier_model(4));
    TEST_CHECK(vjo_vocr_tier_parse("Small") == 1 && !vjo_vocr_tier_parse("") && !vjo_vocr_tier_parse(NULL));
    TEST_CHECK(!vjo_vocr_tier_of_model(NULL) && !vjo_vocr_tier_of_model("meiki-stream-int8.mnn"));
    /* the default file names the option at its default */
    TEST_CHECK(strstr(vjo_config_default_text(), "\nvocr_tier = normal\n") != NULL);
    TEST_CHECK(!strstr(vjo_config_default_text(), "vocr_model") && !strstr(vjo_config_default_text(), "vocr_fallback_model"));
    /* through the file: an older file's choice shows, and the next save writes the tier in its place */
    reset();
    static const char older[] = "ocr_backend = vocr\nvocr_model = FL10_w8.vocr\nvocr_fallback_model = none\n"
                                "font_size_ja = 22\n";
    put_file(VJO_PREFERENCES_PATH, older, sizeof(older) - 1);
    VjoConfig cfg;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_SMALL && !cfg.n_warnings);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    char text[512];
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    ssize_t got = read(fd, text, sizeof(text) - 1); close(fd); TEST_ASSERT(got > 0); text[got] = 0;
    TEST_CHECK(strstr(text, "vocr_tier = large\n") && !strstr(text, "vocr_model") && strstr(text, "font_size_ja = 22\n") &&
               !strstr(text, "ocr_backend"));
    TEST_MSG("%s", text);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE && cfg.font_size_ja == 22 && arena.used == 0 && handles == 0);
}

static void test_save_switch_reload_retains_other_settings(void)
{
    reset(); VjoConfig cfg;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 0); /* the defaults */
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD);
    put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_CHARACTER, &arena) == 0);
    check_choices(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_CHARACTER);
    char out[sizeof(original)+128];
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    ssize_t n = read(fd, out, sizeof(out)-1); close(fd); TEST_ASSERT(n > 0); out[n] = 0;
    TEST_CHECK(strstr(out, "; Keep this comment\r\n") && strstr(out, "unknown_future_setting = unchanged\n"));
    TEST_CHECK(!strstr(out, "ocr_backend")); /* the fixture's Lens choice went with the first save */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0); check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
/* An OCR choice saved by the app keeps every anki_* line verbatim, and the
 * removed settings of older builds (AnkiConnect's anki_host, the field
 * mapping's anki_deck, anki_note_type, anki_field_*) load without a warning. */
static int read_back(const char *path, char *text, size_t cap);
static int lines_with(const char *text, const char *prefix);

/* The app's saves of config.ini, each patching its own keys: 0 the OCR page's
 * four choices (the arguments), 1 the Anki page's (mining, screenshots and, API 32, their black
 * bars trimmed: on), 2 the reading settings (Read text: only on a press; Speed boost while reading: on). */
#define APP_SAVES 3
static int app_save(int which, int tier, int japanese_only, int highlight_mode, int results)
{
    return which == 1 ? vjo_preferences_save_anki(1, 1, 1, &arena)
         : which == 2 ? vjo_preferences_save_reading(VJO_OCR_ON_PRESS, 1, &arena)
                      : vjo_preferences_save(tier, japanese_only, highlight_mode, &arena);
}

/* The removed Anki settings (AnkiConnect's address and audio source, the old
 * field mapping) load without a warning, and either save of the app drops
 * their lines (core/config.h vjo_config_removed_line), the backup included:
 * anki_audio_url's URL can hold a key. anki_mining and every other line stay,
 * and anki_screenshot: the OCR page's and the reading settings' saves keep it as
 * it was (off), the Anki page's writes its own (on), and (API 32) anki_screenshot_trim_bars,
 * which only the Anki page's writes; #163 c2. */
static void test_settings_save_drops_removed_anki_settings(void)
{
    static const char anki[] =
        "; mine\nocr_backend = vocr\nanki_mining = on\nanki_screenshot = off\nanki_host = 192.168.1.23:8766\n"
        "anki_deck = Mining #1 ; main\n"
        "anki_note_type = Lapis;v2\nanki_field_picture =\nANKI_FIELD_WORD = Word\n"
        "anki_audio_url = https://a.invalid/?term={term}&key=secret-url\nanki_audio_url https://a.invalid/?k=secret-2\n"
        "anki_host\nfont_size_ja = 22\n";
    for (int save = 0; save < APP_SAVES; save++) {
        char out[sizeof(anki) + 256];
        VjoConfig cfg;
        reset(); put_file(VJO_PREFERENCES_PATH, anki, sizeof(anki)-1);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.n_warnings == 0 && cfg.anki_mining == 1 && cfg.anki_screenshot == 0);
        TEST_MSG("%d %s", cfg.n_warnings, cfg.warnings[0]);
        TEST_ASSERT(app_save(save, VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER, 3) == 0);
        TEST_CHECK(arena.used == 0 && handles == 0);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) > 0);
        TEST_CHECK(!strstr(out, "anki_host") && !strstr(out, "anki_deck") && !strstr(out, "anki_note_type") &&
                   !strstr(out, "anki_field_") && !strstr(out, "ANKI_FIELD_") && !strstr(out, "anki_audio_url") &&
                   !strstr(out, "secret") && !strstr(out, "ocr_backend")); /* the engine choice went too */
        TEST_CHECK(strstr(out, "; mine\n") && strstr(out, "anki_mining = on\n") && strstr(out, "font_size_ja = 22\n"));
        TEST_CHECK_(lines_with(out, "anki_screenshot =") == 1 &&
                        strstr(out, save == 1 ? "\nanki_screenshot = on\n" : "\nanki_mining = on\nanki_screenshot = off\n"),
                    "save %d: anki_screenshot: %s", save, out);
        TEST_CHECK_(lines_with(out, "anki_screenshot_trim_bars") == (save == 1) &&
                        (save != 1 || strstr(out, "\nanki_screenshot_trim_bars = on\n")),
                    "save %d: anki_screenshot_trim_bars: %s", save, out);
        TEST_MSG("save %d: %s", save, out);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH ".bak", out, sizeof(out)) > 0);
        TEST_CHECK_(!strstr(out, "secret") && !strstr(out, "anki_audio_url"), "save %d: config.ini.bak: %s", save, out);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.anki_mining == 1 && cfg.n_warnings == 0 && cfg.font_size_ja == 22 &&
                   (save || (cfg.vocr_tier == VJO_VOCR_TIER_LARGE && vjo_local_names_count(cfg.local_dictionaries) == 2)) &&
                   (save != 2 || (cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.ocr_boost_clock == 1)));
        TEST_CHECK_(cfg.anki_screenshot == (save == 1), "save %d: anki_screenshot loads as %d", save,
                    cfg.anki_screenshot);
        TEST_CHECK(cfg.anki_screenshot_trim_bars == 1); /* on: the default, or the Anki page's own line */
    }
}

/* #163 c1: the saves' slack. save_keys allocates bytes + bytes / 4 + 160 B for the patched file;
 * the most a save needs beyond bytes + bytes / 4 is for a 1-byte file without a final newline:
 * 74 B for the Anki page's three keys (anki_mining, anki_screenshot, API 32's
 * anki_screenshot_trim_bars, all off), 44 B for the reading settings' two (ocr_mode = on_press,
 * ocr_boost_clock = off), with the NUL (audit-overlay-anki-screenshot-c).
 * Each case is saved, compared byte for byte, and read back by the app and by the Shell's own loader. */
typedef struct { const char *name, *before, *after; int warnings; } SlackCase; /* (the Shell's, of the case's own lines) */
static void check_slack(int anki, const SlackCase *cases, int n)
{
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    char out[512];
    VjoConfig cfg;
    for (int i = 0; i < n; i++) {
        int rc;
        reset(); put_file(VJO_PREFERENCES_PATH, cases[i].before, strlen(cases[i].before));
        rc = anki ? vjo_preferences_save_anki(0, 0, 0, &arena) : vjo_preferences_save_reading(VJO_OCR_ON_PRESS, 0, &arena);
        TEST_CHECK_(rc == 0 && arena.used == 0 && handles == 0, "%s: %s save rc %d", cases[i].name,
                    anki ? "the Anki page's" : "the reading", rc);
        TEST_CHECK_(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) == (int)strlen(cases[i].after) &&
                        !strcmp(out, cases[i].after), "%s: saved\n%s", cases[i].name, out);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK_(anki ? !cfg.anki_mining && !cfg.anki_screenshot && !cfg.anki_screenshot_trim_bars
                         : cfg.ocr_mode == VJO_OCR_ON_PRESS && !cfg.ocr_boost_clock,
                    "%s: the app loads them back", cases[i].name);
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell); /* shell/files.c: what the overlay runs with */
        TEST_CHECK_((anki ? !cfg.anki_mining && !cfg.anki_screenshot && !cfg.anki_screenshot_trim_bars
                          : cfg.ocr_mode == VJO_OCR_ON_PRESS && !cfg.ocr_boost_clock) &&
                        cfg.n_warnings == cases[i].warnings && shell.used == 0,
                    "%s: the Shell loads them back (%d warnings)", cases[i].name, cfg.n_warnings);
    }
}

/* The patch the Anki page's save uses: its keys replaced where they are, a
 * duplicate dropped, a missing one appended; every other byte kept. */
static void test_anki_page_save(void)
{
    static const SlackCase slack[] = {
        {"an empty config.ini", "", "anki_mining = off\nanki_screenshot = off\nanki_screenshot_trim_bars = off\n", 0},
        {"one byte, no final newline", "x",
         "x\nanki_mining = off\nanki_screenshot = off\nanki_screenshot_trim_bars = off\n", 1},
        {"a line without a final newline", "font_size_ja = 22",
         "font_size_ja = 22\nanki_mining = off\nanki_screenshot = off\nanki_screenshot_trim_bars = off\n", 0},
        {"CRLF, both keys twice",
         "anki_mining = on\r\nfont_size_ja = 22\r\nANKI_SCREENSHOT=on\r\nanki_mining = off\r\nanki_screenshot = on\r\n",
         "anki_mining = off\nfont_size_ja = 22\r\nanki_screenshot = off\nanki_screenshot_trim_bars = off\n", 0},
        {"API 32: each of the three keys twice, the trim key's first last",
         "anki_screenshot_trim_bars=on\nanki_mining = on\nanki_screenshot = on\nANKI_SCREENSHOT_TRIM_BARS = on\n"
         "anki_mining = on\nanki_screenshot = on\n",
         "anki_screenshot_trim_bars = off\nanki_mining = off\nanki_screenshot = off\n", 0},
    };
    static const char before[] =
        "\xEF\xBB\xBF; Keep this comment\r\nanki_mining = off ; old\r\nocr_backend = vocr\n"
        "LOG_FILE=on\nocr_model_dir = ux0:m;x\nanki_mining = on\nfont_size_ja = 22";
    static const char *const keys[] = {"anki_mining", "log_file"};
    const char *values[] = {"on", "off"};
    char out[512];
    VjoConfig cfg;
    int len = vjo_config_patch_keys(before, sizeof(before) - 1, keys, values, 2, out, sizeof(out));
    TEST_CHECK(len > 0 && !strcmp(out, "\xEF\xBB\xBF; Keep this comment\r\nanki_mining = on\n"
                                       "log_file = off\nocr_model_dir = ux0:m;x\nfont_size_ja = 22"));
    TEST_MSG("%s", out);
    /* a BOM right before the first line's key: kept, and that line replaced (no stale duplicate) */
    len = vjo_config_patch_keys("\xEF\xBB\xBF" "anki_mining = off\nx = 1\n", 27, keys, values, 2, out, sizeof(out));
    TEST_CHECK(len > 0 && !strcmp(out, "\xEF\xBB\xBF" "anki_mining = on\nx = 1\nlog_file = off\n"));
    TEST_MSG("%s", out);
    /* missing keys go at the end, after a line break */
    len = vjo_config_patch_keys("a = 1", 5, keys, values, 2, out, sizeof(out));
    TEST_CHECK(len > 0 && !strcmp(out, "a = 1\nanki_mining = on\nlog_file = off\n"));
    len = vjo_config_patch_keys("", 0, keys, values, 2, out, sizeof(out));
    TEST_CHECK(len > 0 && !strcmp(out, "anki_mining = on\nlog_file = off\n"));
    /* refused: no room, a line break in a value, an embedded NUL */
    TEST_CHECK(vjo_config_patch_keys(before, sizeof(before) - 1, keys, values, 2, out, 40) == -1);
    values[1] = "off\nanki_mining = off";
    TEST_CHECK(vjo_config_patch_keys("", 0, keys, values, 2, out, sizeof(out)) == -1);
    values[1] = "off";
    TEST_CHECK(vjo_config_patch_keys("a\0b", 3, keys, values, 2, out, sizeof(out)) == -1);
    /* through the file: what the overlay then loads */
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    TEST_ASSERT(vjo_preferences_save_anki(1, 1, 1, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    /* (the fixture's one warning is its unknown_future_setting) */
    TEST_CHECK(cfg.anki_mining == 1 && cfg.n_warnings == 1 && cfg.font_size_ja == 22 &&
               cfg.vocr_tier == VJO_VOCR_TIER_NORMAL);
    TEST_MSG("mining %d warnings %d: %s", cfg.anki_mining, cfg.n_warnings, cfg.warnings[0]);
    TEST_CHECK(cfg.anki_screenshot == 1 && cfg.anki_screenshot_trim_bars == 1); /* API 23, 32: on, the defaults the save wrote */
    /* the cards' screenshots off: their own line, read back as such */
    TEST_ASSERT(vjo_preferences_save_anki(1, 0, 1, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.anki_mining == 1 && cfg.anki_screenshot == 0 && cfg.anki_screenshot_trim_bars == 1 &&
               cfg.n_warnings == 1);
    /* API 32: their black bars kept, then trimmed again */
    TEST_ASSERT(vjo_preferences_save_anki(1, 1, 0, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.anki_mining == 1 && cfg.anki_screenshot == 1 && cfg.anki_screenshot_trim_bars == 0 &&
               cfg.n_warnings == 1);
    TEST_ASSERT(vjo_preferences_save_anki(0, 1, 1, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.anki_mining == 0 && cfg.anki_screenshot == 1 && cfg.anki_screenshot_trim_bars == 1 &&
               cfg.n_warnings == 1 && cfg.font_size_ja == 22);
    {
        char text[sizeof(original) + 160];
        int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
        ssize_t n = read(fd, text, sizeof(text) - 1);
        close(fd);
        TEST_ASSERT(n > 0);
        text[n] = 0;
        /* the fixture without its removed engine line, the setting appended */
        const char *engine = strstr(original, "ocr_backend = lens\n");
        TEST_ASSERT(engine != NULL);
        size_t head = (size_t)(engine - original), line = strlen("ocr_backend = lens\n");
        size_t tail = sizeof(original) - 1 - head - line;
        /* (API 23, 32: and the cards' screenshots and their black bars, on unless the page turned them off) */
        TEST_CHECK(!strncmp(text, original, head) && !strncmp(text + head, engine + line, tail) &&
                   !strcmp(text + head + tail, "anki_mining = off\nanki_screenshot = on\nanki_screenshot_trim_bars = on\n"));
        TEST_MSG("%s", text);
    }
    /* a failed write leaves the old file */
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    write_fail = 1;
    TEST_CHECK(vjo_preferences_save_anki(1, 1, 1, &arena) < 0);
    write_fail = 0;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(!cfg.anki_mining);
    /* #163 c1: the smallest files, whose save grows the most */
    check_slack(1, slack, (int)(sizeof(slack) / sizeof(slack[0])));
}

/* The lines of text that begin with prefix. */
static int lines_with(const char *text, const char *prefix)
{
    int n = 0;
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        n += !strncmp(p, prefix, strlen(prefix));
    return n;
}

/* The fixture as the reading settings' save writes it: without its removed
 * engine line (a save drops it), its ocr_mode line replaced where it is, and
 * ocr_boost_clock, which it has none of, at the end. */
static void reading_expected(char *out, size_t cap, const char *mode_line, const char *boost_line)
{
    const char *engine = strstr(original, "ocr_backend = lens\n"), *mode = strstr(original, "ocr_mode = on_press\n");
    TEST_ASSERT(engine && mode == engine + strlen("ocr_backend = lens\n"));
    snprintf(out, cap, "%.*s%s%s%s", (int)(engine - original), original, mode_line,
             mode + strlen("ocr_mode = on_press\n"), boost_line);
}

/* The OCR page's Read text and Speed boost while reading (config.ini ocr_mode,
 * ocr_boost_clock): each step saved where its line is, every other byte as it
 * was, read back by the app and by the Shell's own loader; the other saves keep
 * them and they keep the others; a value outside the two is refused before
 * anything is read or written; a failed save leaves the saved choice. */
static void test_reading_settings_save(void)
{
    static const struct { int mode, boost; const char *mode_line, *boost_line; } steps[] = {
        {VJO_OCR_AUTO, 1, "ocr_mode = auto\n", "ocr_boost_clock = on\n"},
        {VJO_OCR_AUTO, 0, "ocr_mode = auto\n", "ocr_boost_clock = off\n"},
        {VJO_OCR_ON_PRESS, 0, "ocr_mode = on_press\n", "ocr_boost_clock = off\n"},
        {VJO_OCR_ON_PRESS, 1, "ocr_mode = on_press\n", "ocr_boost_clock = on\n"},
    };
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    char out[sizeof(original) + 256], want[sizeof(original) + 256];
    VjoConfig cfg;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    /* the fixture: on_press, and no ocr_boost_clock line, so the default (on) */
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.ocr_boost_clock == 1);
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        TEST_ASSERT(vjo_preferences_save_reading(steps[i].mode, steps[i].boost, &arena) == 0);
        TEST_CHECK(arena.used == 0 && handles == 0);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) > 0);
        reading_expected(want, sizeof(want), steps[i].mode_line, steps[i].boost_line);
        TEST_CHECK_(!strcmp(out, want), "step %u:\n%s", i, out);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.ocr_mode == steps[i].mode && cfg.ocr_boost_clock == steps[i].boost);
        vjo_config_load(&cfg, &shell); /* shell/files.c: what the overlay runs with after the resume */
        TEST_CHECK_(cfg.ocr_mode == steps[i].mode && cfg.ocr_boost_clock == steps[i].boost && shell.used == 0,
                    "step %u: the Shell read ocr_mode %d, ocr_boost_clock %d", i, cfg.ocr_mode, cfg.ocr_boost_clock);
        /* (the fixture's one warning is its unknown_future_setting) */
        TEST_CHECK(cfg.n_warnings == 1 && strstr(cfg.warnings[0], "unknown_future_setting"));
        TEST_CHECK(cfg.font_size_ja == 22 && vjo_local_names_count(cfg.local_dictionaries) == 2 && cfg.vocr_tier == VJO_VOCR_TIER_NORMAL &&
                   !strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
    }
    /* the OCR page's other choices and the Anki page's keep them, and they keep those */
    TEST_ASSERT(vjo_preferences_save_reading(VJO_OCR_ON_PRESS, 0, &arena) == 0);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER, &arena) == 0);
    TEST_ASSERT(vjo_preferences_save_anki(1, 0, 1, &arena) == 0); /* (API 23: the cards' screenshots off) */
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.ocr_boost_clock == 0 && cfg.vocr_tier == VJO_VOCR_TIER_LARGE &&
               !cfg.japanese_only_selection && cfg.highlight_mode == VJO_HIGHLIGHT_CHARACTER &&
               vjo_local_names_count(cfg.local_dictionaries) == 2 && cfg.anki_mining == 1 && cfg.anki_screenshot == 0);
    TEST_ASSERT(vjo_preferences_save_reading(VJO_OCR_AUTO, 1, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_AUTO && cfg.ocr_boost_clock == 1 && cfg.vocr_tier == VJO_VOCR_TIER_LARGE &&
               !cfg.japanese_only_selection && cfg.highlight_mode == VJO_HIGHLIGHT_CHARACTER &&
               vjo_local_names_count(cfg.local_dictionaries) == 2 && cfg.anki_mining == 1 && cfg.anki_screenshot == 0);
    TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) > 0);
    TEST_CHECK(lines_with(out, "ocr_mode =") == 1 && strstr(out, "\nocr_mode = auto\n") && /* each key once */
               lines_with(out, "ocr_boost_clock =") == 1 && strstr(out, "\nocr_boost_clock = on\n"));
    TEST_MSG("%s", out);
    /* any other value: refused before a read or a write, the file as it was */
    static const int bad[][2] = {{2, 1}, {-1, 1}, {VJO_OCR_AUTO, 2}, {VJO_OCR_ON_PRESS, -1}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        written = 0; renames = 0;
        TEST_CHECK(vjo_preferences_save_reading(bad[i][0], bad[i][1], &arena) < 0);
        TEST_CHECK(written == 0 && renames == 0 && handles == 0 && arena.used == 0);
    }
    TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, want, sizeof(want)) > 0);
    TEST_CHECK(!strcmp(out, want));
    /* the file's own spellings: a BOM, CRLF, capitals and duplicates; the first line of each
     * key replaced where it is, the later ones dropped, every other line kept */
    {
        static const char mixed[] = "\xEF\xBB\xBF" "OCR_MODE = on_press\r\nfont_size_ja = 22\r\nocr_mode = auto\n"
                                    "Ocr_Boost_Clock=on ; mine\nocr_boost_clock = off\n";
        reset(); put_file(VJO_PREFERENCES_PATH, mixed, sizeof(mixed) - 1);
        TEST_ASSERT(vjo_preferences_save_reading(VJO_OCR_AUTO, 0, &arena) == 0);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) > 0);
        TEST_CHECK(!strcmp(out, "\xEF\xBB\xBF" "ocr_mode = auto\nfont_size_ja = 22\r\nocr_boost_clock = off\n"));
        TEST_MSG("%s", out);
    }
    /* no config.ini yet: the commented template with its two lines set */
    reset();
    TEST_ASSERT(vjo_preferences_save_reading(VJO_OCR_ON_PRESS, 0, &arena) == 0);
    {
        static char saved[VJO_CONFIG_TEXT_MAX + 1], expect[VJO_CONFIG_TEXT_MAX + 1];
        const char *t = vjo_config_default_text();
        const char *boost = strstr(t, "\nocr_boost_clock = on\n"), *mode = strstr(t, "\nocr_mode = auto\n");
        TEST_ASSERT(boost && mode && boost < mode);
        const char *after_boost = boost + strlen("\nocr_boost_clock = on\n");
        snprintf(expect, sizeof(expect), "%.*socr_boost_clock = off\n%.*socr_mode = on_press\n%s",
                 (int)(boost + 1 - t), t, (int)(mode + 1 - after_boost), after_boost,
                 mode + strlen("\nocr_mode = auto\n"));
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, saved, sizeof(saved)) > 0);
        TEST_CHECK(!strcmp(saved, expect));
        TEST_MSG("%s", saved);
        VjoConfig shell_cfg;
        vjo_config_load(&shell_cfg, &shell);
        TEST_CHECK(shell_cfg.ocr_mode == VJO_OCR_ON_PRESS && !shell_cfg.ocr_boost_clock && !shell_cfg.n_warnings &&
                   shell.used == 0);
    }
    /* a failed write or publication: the saved choice stays */
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    write_fail = 1;
    TEST_CHECK(vjo_preferences_save_reading(VJO_OCR_AUTO, 0, &arena) < 0);
    write_fail = 0;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.ocr_boost_clock == 1 && handles == 0 && arena.used == 0);
    rename_fail = 2; renames = 0; /* the new file's rename: the old primary is put back */
    TEST_CHECK(vjo_preferences_save_reading(VJO_OCR_AUTO, 0, &arena) < 0);
    rename_fail = 0;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.ocr_boost_clock == 1 && handles == 0 && arena.used == 0);
    /* an unreadable config.ini is never overwritten */
    read_fail = 1; written = 0;
    TEST_CHECK(vjo_preferences_save_reading(VJO_OCR_AUTO, 0, &arena) < 0);
    read_fail = 0;
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
    TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) == (int)sizeof(original) - 1 && !strcmp(out, original));
    /* #163 c1: the smallest files, whose save grows the most */
    {
        static const SlackCase slack[] = {
            {"an empty config.ini", "", "ocr_mode = on_press\nocr_boost_clock = off\n", 0},
            {"one byte, no final newline", "x", "x\nocr_mode = on_press\nocr_boost_clock = off\n", 1},
            {"a line without a final newline", "font_size_ja = 22",
             "font_size_ja = 22\nocr_mode = on_press\nocr_boost_clock = off\n", 0},
            {"CRLF, both keys twice",
             "ocr_mode = auto\r\nfont_size_ja = 22\r\nOCR_BOOST_CLOCK=on\r\nocr_mode = on_press\r\nocr_boost_clock = on\r\n",
             "ocr_mode = on_press\nfont_size_ja = 22\r\nocr_boost_clock = off\n", 0},
        };
        check_slack(0, slack, (int)(sizeof(slack) / sizeof(slack[0])));
    }
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
        TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_CHARACTER, &arena) < 0);
        check_choices(fault == 5 ? VJO_VOCR_TIER_SMALL : VJO_VOCR_TIER_NORMAL, fault == 5 ? 0 : 1,
                      fault == 5 ? VJO_HIGHLIGHT_CHARACTER : VJO_HIGHLIGHT_WORD);
        TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) == 0);
    }
    reset(); put_file(VJO_PREFERENCES_PATH ".bak", original, sizeof(original)-1);
    VjoConfig cfg; TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0); check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_tier_choice_loads_shell_and_retains_other_preferences(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    const int tiers[] = {VJO_VOCR_TIER_SMALL, VJO_VOCR_TIER_LARGE, VJO_VOCR_TIER_NORMAL, VJO_VOCR_TIER_LARGE,
                         VJO_VOCR_TIER_SMALL};
    for (unsigned i = 0; i < sizeof(tiers)/sizeof(tiers[0]); i++) {
        TEST_ASSERT(vjo_preferences_save(tiers[i], 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
        check_choices(tiers[i], 1, VJO_HIGHLIGHT_WORD);
        VjoConfig cfg; vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.vocr_tier == tiers[i]);
        TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.highlight_mode == VJO_HIGHLIGHT_WORD);
        TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
        /* the fixture's ocr_backend = lens went with the first save: the Shell no longer warns */
        TEST_CHECK(cfg.n_warnings == 1 && strstr(cfg.warnings[0], "unknown_future_setting"));
        TEST_CHECK(shell.used == 0 && handles == 0);
    }
    /* Changing the other controls must keep the size. */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER, &arena) == 0);
    check_choices(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER);
    VjoConfig cfg; vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE);
    TEST_CHECK(!cfg.japanese_only_selection && cfg.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    /* A failed save never replaces the last saved choice. */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    write_fail = 1; written = 0;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 1, VJO_HIGHLIGHT_WORD, &arena) < 0);
    write_fail = 0;
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_unreadable_or_invalid_config_never_overwritten(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1); read_fail = 1;
    VjoConfig cfg;
    TEST_CHECK(vjo_preferences_load(&cfg, &arena) < 0);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0 && handles == 0);
    read_fail = 0; check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
    put_file(VJO_PREFERENCES_PATH, "broken\0config", 13);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) < 0);
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
    memset(memory, '#', VJO_PREFERENCES_TEXT_MAX);
    put_file(VJO_PREFERENCES_PATH, (char *)memory, VJO_PREFERENCES_TEXT_MAX);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) < 0);
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
}
static void test_highlight_only_switch_loads_actual_shell_without_other_changes(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    for (int i = 0; i < 4; i++) {
        int highlight_mode = i % 2 ? VJO_HIGHLIGHT_WORD : VJO_HIGHLIGHT_CHARACTER;
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, highlight_mode, &arena) == 0);
        check_choices(VJO_VOCR_TIER_NORMAL, 1, highlight_mode);
        VjoConfig cfg; vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.highlight_mode == highlight_mode);
        TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
        TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
        TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
        TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS);
        TEST_CHECK(shell.used == 0 && handles == 0);
    }
    /* Reject invalid mode before any write or rename, preserving the last choice. */
    written = 0; renames = 0;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, -1, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0 && arena.used == 0 && handles == 0);
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_dictionary_results_switch_loads_actual_shell(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original) - 1);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    char out[4096];
    TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) > 0);
    TEST_CHECK(!strstr(out, "local_dictionary_results"));
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}

static void test_backup_only_loads_actual_shell_without_default_publication(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    rename_fail_after = 2; /* Fail both temp publication and primary restoration. */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_WORD, &arena) < 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) < 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH ".bak", F_OK) == 0);
    VjoConfig cfg;
    TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD);
    unsigned char shell_mem[VJO_CONFIG_TEXT_MAX + 1];
    VjoArena shell; vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    size_t writes_before = written; int renames_before = renames;
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
    TEST_CHECK(cfg.font_size_ja == 22);
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
    /* The file as the app writes it (vocr_tier, no vocr_model, ocr_backend or
     * API key line: a save drops those), so a save of the same choices keeps
     * its length. */
    static const char saved[] =
        "; Keep this comment\r\nocr_mode = on_press\n"
        "japanese_only_selection = on\n"
        "highlight_mode = word\n"
        "dictionary = local\nlocal_dictionaries = daijirin.vjdict,jmdict.vjdict\n"
        ""
        "ocr_model_dir = ux0:data/custom-ocr\nvocr_tier = normal\n"
        "hachidori_host = 192.168.1.20\n" /* the removed relay's: kept as written */
        "font_size_ja = 22\nunknown_future_setting = unchanged\n";
    reset(); char near[VJO_CONFIG_TEXT_MAX + 1];
    memset(near, '#', sizeof(near)); memcpy(near, saved, sizeof(saved)-1);
    near[VJO_CONFIG_TEXT_MAX-1] = '\n'; near[VJO_CONFIG_TEXT_MAX] = 0;
    put_file(VJO_PREFERENCES_PATH, near, VJO_CONFIG_TEXT_MAX);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    VjoConfig cfg; vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
    TEST_CHECK(shell.peak == sizeof(shell_mem) && shell.used == 0 && handles == 0);
    /* Adding one byte (on -> off) would exceed the reader, so the original stays intact. */
    written = 0; renames = 0;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 0, VJO_HIGHLIGHT_WORD, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0);
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
}
static int read_back(const char *path, char *text, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, text, cap - 1) : -1;
    if (fd >= 0) close(fd);
    text[n > 0 ? n : 0] = 0;
    return (int)n;
}
/* jpdb.io and jiten.moe, the online dictionaries, were removed: a config.ini
 * that chose one loads as local with a warning, in the app and in the Shell,
 * and either save of the app writes dictionary = local and drops their API
 * keys (secrets: never kept, not in the backup either), keeping every other
 * line. A local choice stays as it is (the relay's: test_removed_relay_*). */
static void test_removed_dictionaries_rewritten_by_saves(void)
{
    static const char legacy[] =
        "; Keep this comment\r\ndictionary = jpdb\r\nhachidori_host =\n"
        "; jpdb.io API key (needed for dictionary = jpdb): jpdb.io -> Settings -> API key\n"
        "jpdb_api_key = secret-jpdb\n\njiten_api_key = secret-jiten ; mine\napi_key = secret-old\n"
        "JPDB_API_KEY: secret-colon\nocr_backend = vocr\nocr_mode = on_press\nfont_size_ja = 22\n";
    for (int save = 0; save < APP_SAVES; save++) {
        char text[sizeof(legacy) + 256];
        VjoConfig cfg;
        unsigned char shell_mem[24 * 1024]; VjoArena shell;
        reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.n_warnings == 0); /* removed settings: silent (#418) */
        TEST_ASSERT(app_save(save, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1) == 0);
        TEST_CHECK(arena.used == 0 && handles == 0);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0);
        TEST_CHECK(!strstr(text, "secret") && !strstr(text, "api_key =") && !strstr(text, "\ndictionary = jpdb"));
        TEST_CHECK(strstr(text, "; Keep this comment\r\ndictionary = local\nhachidori_host =\n") != NULL);
        TEST_CHECK(strstr(text, "\nocr_mode = on_press\nfont_size_ja = 22\n") != NULL && !strstr(text, "ocr_backend"));
        /* comments are never rewritten: the old template's line stays, harmless */
        TEST_CHECK(strstr(text, "; jpdb.io API key (needed for dictionary = jpdb)") != NULL);
        TEST_MSG("%s", text);
        /* the backup the save keeps (the file it replaced) has no key either */
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) > 0);
        TEST_CHECK(!strstr(text, "secret") && strstr(text, "dictionary = local\n") && strstr(text, "font_size_ja = 22\n"));
        TEST_MSG("%s", text);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.n_warnings == 0 && cfg.ocr_mode == VJO_OCR_ON_PRESS &&
                   cfg.font_size_ja == 22);
        TEST_CHECK(save != 1 || cfg.anki_mining == 1);
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.n_warnings == 0 && shell.used == 0);
    }
    /* a failed first publication leaves the old file and saves nothing */
    reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
    write_fail = 1;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) < 0);
    write_fail = 0;
    {
        char text[sizeof(legacy) + 16];
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) == (int)sizeof(legacy) - 1 &&
                   !strcmp(text, legacy));
    }
    /* A UTF-8 BOM (Notepad) before a key on the very first line (audit B1): every save finds
     * the key after the BOM, as their patches do, so none keeps it in the backup. */
    for (int save = 0; save < APP_SAVES; save++) {
        static const char bom[] = "\xEF\xBB\xBF" "jpdb_api_key = secret-bom\nocr_mode = on_press\nfont_size_ja = 22\n";
        char text[256];
        reset(); put_file(VJO_PREFERENCES_PATH, bom, sizeof(bom) - 1);
        TEST_ASSERT(app_save(save, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1) == 0);
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0 && !strncmp(text, "\xEF\xBB\xBF", 3) &&
                   !strstr(text, "secret") && strstr(text, "font_size_ja = 22\n"));
        TEST_CHECK_(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) > 0 && !strstr(text, "secret"),
                    "save %d: no key in config.ini.bak after a BOM", save);
        TEST_MSG("config.ini.bak: %s", text);
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH ".tmp", text, sizeof(text)) <= 0 || !strstr(text, "secret"));
        TEST_CHECK(arena.used == 0 && handles == 0);
    }
    /* A one-shot fault during that first publication (its write's sync, either of its renames)
     * fails the save and keeps the old file as it was: the save never goes on to rotate the
     * keyed file into the backup (audit N6, M14). */
    {
        static const struct { int sync, rename; } first[] = {{1, 0}, {0, 1}, {0, 2}};
        for (unsigned k = 0; k < sizeof(first) / sizeof(first[0]); k++) {
            char text[sizeof(legacy) + 256];
            reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
            sync_fail = first[k].sync;
            rename_fail = first[k].rename;
            int rc = vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena);
            sync_fail = rename_fail = 0;
            TEST_CHECK_(rc < 0, "first-publication fault %u (sync %d, rename %d) fails the save", k, first[k].sync,
                        first[k].rename);
            TEST_CHECK_(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) == (int)sizeof(legacy) - 1 &&
                        !strcmp(text, legacy), "fault %u: config.ini as it was", k);
            TEST_CHECK_(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) <= 0 || !strstr(text, "secret"),
                        "fault %u: no keyed backup", k);
            TEST_CHECK(arena.used == 0 && handles == 0);
        }
    }
    /* Once the key-free text is published and the keyed backup deleted (syncs 1-3,
     * renames 1-2), a failure at any later step of the save leaves no key in any file:
     * sync 4 is the save's own write, renames 3-4 its publication. */
    static const struct { int sync, rename; } faults[] = {{4, 0}, {5, 0}, {0, 3}, {0, 4}};
    for (unsigned k = 0; k < sizeof(faults) / sizeof(faults[0]); k++) {
        static const char *const files[] = {VJO_PREFERENCES_PATH, VJO_PREFERENCES_PATH ".bak",
                                            VJO_PREFERENCES_PATH ".tmp"};
        char text[sizeof(legacy) + 256];
        reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
        put_file(VJO_PREFERENCES_PATH ".bak", legacy, sizeof(legacy) - 1); /* an older save's backup */
        sync_fail = faults[k].sync;
        rename_fail = faults[k].rename;
        int rc = vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena);
        TEST_CHECK_(rc < 0, "fault %u fails the save", k); /* sync 5: published, unconfirmed */
        sync_fail = rename_fail = 0;
        for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
            read_back(files[f], text, sizeof(text));
            TEST_CHECK_(!strstr(text, "secret"), "fault %u (sync %d, rename %d): no key in %s", k, faults[k].sync,
                        faults[k].rename, files[f]);
        }
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0 && strstr(text, "font_size_ja = 22\n"));
        TEST_CHECK(arena.used == 0 && handles == 0);
    }
    /* config.ini missing, its backup holding a key (recovered): the save writes a key-free
     * file and the keyed backup goes */
    {
        char text[sizeof(legacy) + 256];
        reset(); put_file(VJO_PREFERENCES_PATH ".bak", legacy, sizeof(legacy) - 1);
        VjoConfig cfg;
        TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1);
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0 && !strstr(text, "secret") &&
                   strstr(text, "dictionary = local\n"));
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) > 0 && !strstr(text, "secret"));
    }
    /* only the dictionary is replaced; no key: one publication, the backup is the old file */
    reset(); put_file(VJO_PREFERENCES_PATH, "dictionary = JITEN\nfont_size_ja = 22\n", 37);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    {
        char text[512];
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0 &&
                   !strncmp(text, "dictionary = local\nfont_size_ja = 22\n", 37));
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) == 37 &&
                   !strcmp(text, "dictionary = JITEN\nfont_size_ja = 22\n"));
    }
    /* a local choice is kept verbatim by both patches; the removed relay's is replaced */
    {
        static const char *const keys[] = {"anki_mining"};
        const char *values[] = {"on"};
        char out[256];
        int n = vjo_config_patch_preferences("Dictionary = Local ; mine\n", 25, VJO_VOCR_TIER_NORMAL, 1,
                                             VJO_HIGHLIGHT_WORD, out, sizeof(out));
        TEST_CHECK(n > 0 && !strncmp(out, "Dictionary = Local ; mine\n", 25));
        n = vjo_config_patch_preferences("Dictionary = Hachidori ; relay\n", 31, VJO_VOCR_TIER_NORMAL, 1,
                                         VJO_HIGHLIGHT_WORD, out, sizeof(out));
        TEST_CHECK(n > 0 && !strncmp(out, "dictionary = local\n", 19));
        TEST_MSG("%s", out);
        n = vjo_config_patch_keys("dictionary = local\njpdb_api_key = x\n", 36, keys, values, 1, out, sizeof(out));
        TEST_CHECK(n > 0 && !strcmp(out, "dictionary = local\nanki_mining = on\n"));
        n = vjo_config_patch_keys("dictionary = jpdb\nx = 1", 23, NULL, NULL, 0, out, sizeof(out));
        TEST_CHECK(n > 0 && !strcmp(out, "dictionary = local\nx = 1"));
        TEST_MSG("%s", out);
    }
}
/* The Hachidori relay on a computer was removed too: dictionary = hachidori
 * loads as local with one warning (hachidori_host, its computer, is ignored
 * without one), in the app and in the Shell; either save of the app writes
 * dictionary = local, in one publication (no secret: the backup is the file it
 * replaced, byte for byte), and keeps every other line as written, the
 * hachidori_host line too (the installer, tools/migrate_config.py, drops it):
 * a save drops only the lines that may hold a key, the API keys' and the
 * removed Anki settings' (test_settings_save_drops_removed_anki_settings). */
static void test_removed_relay_rewritten_by_saves(void)
{
    static const char legacy[] =
        "; Keep this comment\r\nDictionary = Hachidori ; relay\r\nhachidori_host = 192.168.1.20:19633\n"
        "ocr_mode = on_press\nfont_size_ja = 22\n";
    for (int save = 0; save < APP_SAVES; save++) {
        char text[sizeof(legacy) + 256];
        VjoConfig cfg;
        unsigned char shell_mem[24 * 1024]; VjoArena shell;
        reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.n_warnings == 0); /* removed settings: silent (#418) */
        TEST_MSG("%d warnings, first: %s", cfg.n_warnings, cfg.n_warnings ? cfg.warnings[0] : "");
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.n_warnings == 0 && shell.used == 0);
        renames = 0;
        TEST_ASSERT(app_save(save, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1) == 0);
        TEST_CHECK(arena.used == 0 && handles == 0 && renames == 2); /* one publication */
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0);
        TEST_CHECK(strstr(text, "; Keep this comment\r\ndictionary = local\nhachidori_host = 192.168.1.20:19633\n"
                                "ocr_mode = on_press\nfont_size_ja = 22\n") != NULL);
        TEST_CHECK(!strstr(text, "achidori ;") && !strstr(text, "Dictionary = "));
        TEST_MSG("%s", text);
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) == (int)sizeof(legacy) - 1 &&
                   !strcmp(text, legacy));
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.n_warnings == 0 && cfg.ocr_mode == VJO_OCR_ON_PRESS &&
                   cfg.font_size_ja == 22);
        TEST_CHECK(save != 1 || cfg.anki_mining == 1);
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.n_warnings == 0 && shell.used == 0);
    }
}
/* The engine is no setting any more (VOCR is the only one; Google Lens was
 * removed, the overlay is offline). A config.ini that chose one loads as VOCR,
 * in the app and in the Shell, without a warning whatever it names (vocr, Google
 * Lens, the VOCR2 and Meiki trials, the host tools' ncnn: a removed setting,
 * #418); and the app's next save drops the line, keeping every other one. */
static void test_engine_choice_loads_as_vocr_and_saves_drop_it(void)
{
    static const struct { const char *value, *warning; } engines[] = {
        {"vocr", NULL}, {"VOCR", NULL}, {"lens", NULL}, {"Lens", NULL}, {"vocr2", NULL}, {"meiki", NULL}, {"NCNN", NULL},
    };
    for (unsigned i = 0; i < sizeof(engines) / sizeof(engines[0]); i++) {
        char legacy[256];
        int len = snprintf(legacy, sizeof(legacy), "; Keep this comment\nocr_backend = %s\nocr_mode = on_press\n"
                           "japanese_only_selection = off\nvocr_tier = large\nfont_size_ja = 22\n", engines[i].value);
        reset();
        put_file(VJO_PREFERENCES_PATH, legacy, (size_t)len);
        VjoConfig cfg;
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE && !cfg.japanese_only_selection);
        TEST_CHECK(engines[i].warning ? cfg.n_warnings == 1 && !strcmp(cfg.warnings[0], engines[i].warning)
                                      : cfg.n_warnings == 0);
        TEST_MSG("%s: %s", engines[i].value, cfg.n_warnings ? cfg.warnings[0] : "(no warning)");
        unsigned char shell_mem[24 * 1024]; VjoArena shell;
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE && cfg.ocr_mode == VJO_OCR_ON_PRESS);
        TEST_CHECK(cfg.n_warnings == (engines[i].warning ? 1 : 0));
        TEST_CHECK(shell.used == 0 && handles == 0);
        /* Any Settings save drops the line; unrelated lines are kept. */
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_WORD, &arena) == 0);
        char out[sizeof(legacy) + 128];
        int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
        ssize_t n = read(fd, out, sizeof(out)-1); close(fd); TEST_ASSERT(n > 0); out[n] = 0;
        TEST_CHECK(!strstr(out, "ocr_backend") && !strncmp(out, "; Keep this comment\nocr_mode = on_press\n", 40));
        TEST_CHECK(strstr(out, "font_size_ja = 22\n") && strstr(out, "vocr_tier = large\n"));
        TEST_MSG("%s", out);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE && cfg.n_warnings == 0 && arena.used == 0);
    }
}
static size_t read_fixture(const char *name, char *out, size_t cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/host/tests/fixtures/%s", VJO_SOURCE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_(f != NULL, "fixture %s", path);
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return n;
}

/* #101: test.1's Rubi app saved its four preferences over test.2's
 * config.ini (host/tests/fixtures/config-old-app-test2.ini: ocr_backend =
 * vocr2). Today's Shell and app read it as the local engine without a warning
 * (#418: removed settings are silent) and no unknown setting (its vocr_model
 * gives the Normal size), and the app's
 * next save drops the engine line (VOCR is the only engine since Google Lens's
 * removal) and the two model lines, writes vocr_tier, rewrites the lines of
 * removed features as core/config.c vjo_config_removed_line says (the online
 * dictionaries' keys since #117, the relay's since #126, AnkiConnect's since
 * #125), and keeps every other line. */
static void test_old_app_config_loads_as_vocr_and_a_save_repairs_it(void)
{
    static char fixture[8192], expect[8192], out[8192];
    reset();
    size_t n = read_fixture("config-old-app-test2.ini", fixture, sizeof(fixture));
    TEST_ASSERT(strstr(fixture, "\nocr_backend = vocr2\n") != NULL);
    put_file(VJO_PREFERENCES_PATH, fixture, n);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    VjoConfig cfg;
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.n_warnings == 0);
    TEST_MSG("%d warnings, first '%s'", cfg.n_warnings, cfg.n_warnings ? cfg.warnings[0] : "");
    TEST_CHECK(cfg.anki_mining && cfg.highlight_mode == VJO_HIGHLIGHT_WORD && cfg.japanese_only_selection);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL); /* from vocr_model = VOCR3_H15 */
    TEST_CHECK(cfg.ocr_lm == 2 && cfg.ocr_cascade_ms == 2000 && cfg.ocr_refine_lines == 2);
    TEST_CHECK(shell.used == 0 && handles == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.n_warnings == 0);
    /* the app's next save: the engine line (a removed setting) and the two model lines gone and vocr_tier
     * added (API 21), the lines of removed features dropped or replaced as core/config.c
     * vjo_config_removed_line says (since #117 the online dictionaries' empty key lines); every other line kept */
    TEST_ASSERT(vjo_preferences_save(cfg.vocr_tier, cfg.japanese_only_selection, cfg.highlight_mode, &arena) == 0);
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    ssize_t got = read(fd, out, sizeof(out) - 1); close(fd);
    TEST_ASSERT(got > 0); out[got] = 0;
    size_t used = 0, dropped = 0, settings = 0;
    for (const char *line = fixture; *line;) {
        const char *nl = strchr(line, '\n'), *replacement = NULL;
        size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
        int kind = vjo_config_removed_line(line, len, &replacement);
        if (!strncmp(line, "vocr_model = ", 13) || !strncmp(line, "vocr_fallback_model = ", 22)) {
            dropped++;
        } else if (kind == VJO_LINE_DROP_SETTING) { /* the engine line and the UDP log's empty log_host */
            dropped++;
            settings++;
        } else if (kind == VJO_LINE_DROP) {
            dropped++;
        } else if (kind == VJO_LINE_REPLACE) {
            used += (size_t)snprintf(expect + used, sizeof(expect) - used, "%s", replacement);
        } else {
            used += (size_t)snprintf(expect + used, sizeof(expect) - used, "%.*s", (int)len, line);
        }
        line += len;
    }
    snprintf(expect + used, sizeof(expect) - used, "vocr_tier = normal\n");
    TEST_CHECK(dropped >= 6 && settings == 3); /* the engine line, log_host, the two model lines and (since
                                                 * #117) the two empty key lines at least */
    TEST_CHECK(!strcmp(out, expect));
    TEST_MSG("saved:\n%s", out);
    /* whatever the removed features, the user's own settings survive the save */
    static const char *const kept[] = {"\nanki_mining = on\n", "\nhighlight_mode = word\n", "\nfont_size_ja = 18\n",
                                       "\nlocal_dictionaries = plaintext.vjdict\n", "\nocr_lm = 1000k\n",
                                       "\nocr_refine_lines = psp\n", "\n; Vita JP Overlay settings."};
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        TEST_CHECK_(strstr(out, kept[i]) != NULL, "kept: %s", kept[i] + 1);
    TEST_CHECK(!strstr(out, "\nocr_backend") && !strstr(out, "\nvocr_model =") && /* its comment lines stay */
               !strstr(out, "\nvocr_fallback_model =") && !strstr(out, "\nlog_host"));
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.n_warnings == 0 && cfg.vocr_tier == VJO_VOCR_TIER_NORMAL);
}

/* SceShell never writes config.ini: with neither the file nor its backup it
 * runs on the defaults, which are the template's values (core/config.h). */
static void test_shell_runs_on_the_defaults_without_writing_config_ini(void)
{
    reset();
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    VjoConfig cfg, d, parsed;
    memset(&cfg, 0x5A, sizeof(cfg));
    vjo_config_load(&cfg, &shell);
    vjo_config_defaults(&d);
    TEST_CHECK(!memcmp(&cfg, &d, sizeof(cfg)));
    vjo_config_defaults(&parsed);
    vjo_config_parse(&parsed, vjo_config_default_text(), strlen(vjo_config_default_text()));
    TEST_CHECK(parsed.n_warnings == 0);
    TEST_CHECK(!memcmp(&parsed, &d, sizeof(d)));
    TEST_CHECK(written == 0 && renames == 0 && handles == 0 && shell.used == 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) != 0 && access(VJO_PREFERENCES_PATH ".tmp", F_OK) != 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH ".bak", F_OK) != 0);
    /* what creates it: the Rubi app's first save publishes the commented template with the choice (ON here:
     * the template's own line is off) */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, &arena) == 0);
    static char saved[VJO_CONFIG_TEXT_MAX + 1];
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    TEST_ASSERT(fd >= 0);
    ssize_t n = read(fd, saved, sizeof(saved) - 1); close(fd);
    TEST_ASSERT(n > 0); saved[n] = 0;
    TEST_CHECK(!strncmp(saved, vjo_config_default_text(), 40) && strstr(saved, "\njapanese_only_selection = on\n") &&
               !strstr(saved, "japanese_only_selection = off"));
    TEST_CHECK(!strstr(saved, "ocr_backend")); /* no engine choice: VOCR is the only engine */
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.japanese_only_selection == 1 && cfg.n_warnings == 0 && shell.used == 0);
}

/* The Shell's reads of the Rubi app's stamps (shell/files.c): the first
 * line, whole, through short reads; cut to the buffer; the SDK's error. */
static void test_file_line_and_exists(void)
{
    static const char path[] = "ux0:data/VitaJPOverlay/vjo_app.txt";
    char buf[64];
    reset();
    unlink(path);
    TEST_CHECK(vjo_file_line(path, buf, sizeof(buf)) == (int)0x80010002u && !buf[0]);
    TEST_CHECK(!vjo_file_exists(path));
    put_file(path, "vjoapp1 api=21 build=0123456789abcdef\nsecond line\n", 50);
    TEST_CHECK(vjo_file_line(path, buf, sizeof(buf)) == 50 && !strcmp(buf, "vjoapp1 api=21 build=0123456789abcdef"));
    TEST_CHECK(vjo_file_exists(path) && handles == 0);
    put_file(path, "vjoapp1 api=21 build=0123456789abcdef\r\n", 39);
    TEST_CHECK(vjo_file_line(path, buf, sizeof(buf)) == 39 && !strcmp(buf, "vjoapp1 api=21 build=0123456789abcdef"));
    memset(buf, 'x', sizeof(buf));
    put_file(path, "0123456789012345678901234567890123456789", 40);
    TEST_CHECK(vjo_file_line(path, buf, 17) == 16 && !strcmp(buf, "0123456789012345"));
    put_file(path, "", 0);
    TEST_CHECK(vjo_file_line(path, buf, sizeof(buf)) == 0 && !buf[0]);
    read_fail = 1;
    TEST_CHECK(vjo_file_line(path, buf, sizeof(buf)) == (int)0x80010013u && !buf[0]);
    read_fail = 0;
    TEST_CHECK(handles == 0);
    unlink(path);
}

/* A UTF-8 BOM at a later line's start (a hand-joined file) is no part of its name, for the parser and for every
 * writer alike (core/config.c vjo_config_line_start; audit-pr180-b N3, audit-pr183 N1): the app's save replaces a
 * BOM-led owned line and drops its copies, the install's keys find a BOM-led copy, and a BOM-led API key goes, so
 * a choice made in the app is the one the plugins read. */
static void test_a_bom_led_line_is_the_same_setting_to_every_writer(void)
{
    static char out[4096], out2[4096];
    VjoConfig cfg;
    /* the app's save: a BOM-led vocr_tier after a plain one; the app chooses Small */
    static const char a[] = "vocr_tier = normal\n" "\xEF\xBB\xBF" "vocr_tier = large\n"
                            "\xEF\xBB\xBF" "jpdb_api_key = SECRET1\n" "anki_mining = on\n";
    int n = vjo_config_patch_preferences(a, sizeof(a) - 1, VJO_VOCR_TIER_SMALL, 1, VJO_HIGHLIGHT_WORD, out, sizeof(out));
    TEST_ASSERT(n > 0);
    TEST_MSG("%s", out);
    TEST_CHECK(strstr(out, "vocr_tier = small\n") && !strstr(out, "large") && !strstr(out, "SECRET1"));
    vjo_config_defaults(&cfg);
    vjo_config_parse(&cfg, out, (size_t)n);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_SMALL && cfg.anki_mining == 1 && cfg.n_warnings == 0);
    /* the install's keys: a BOM-led copy of log_file is the same setting, so the key's new value is the one read */
    static const char b[] = "log_file = on\n" "\xEF\xBB\xBF" "log_file = off\n" "anki_mining = on\n";
    const char *keys[] = {"log_file"}, *values[] = {"on"};
    int m = vjo_config_patch_keys(b, sizeof(b) - 1, keys, values, 1, out2, sizeof(out2));
    TEST_ASSERT(m > 0);
    TEST_MSG("%s", out2);
    TEST_CHECK(strstr(out2, "log_file = on\n") && !strstr(out2, "log_file = off"));
    vjo_config_defaults(&cfg);
    vjo_config_parse(&cfg, out2, (size_t)m);
    TEST_CHECK(cfg.log_file == 1 && cfg.n_warnings == 0);
    /* the parser before either save: the BOM-led line is read, and the key's is silent */
    vjo_config_defaults(&cfg);
    vjo_config_parse(&cfg, a, sizeof(a) - 1);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE && cfg.n_warnings == 0);
}

/* #418 (cleanup package P2): the Rubi app's saves of a config.ini text (len bytes): the four-preference save made
 * with the text's own values (as the install's, settings/install.c plan_config_ini), the Anki page's keys, the
 * secrets-free copy a save publishes first (vjo_config_patch_keys with no key) and app.ini's patch, which has no
 * removed-key rules. */
static const char *const golden_anki_keys[] = {"anki_mining", "anki_screenshot", "anki_screenshot_trim_bars"};
static const char *const golden_anki_values[] = {"on", "off", "on"};
static const char *const golden_save_names[] = {"preferences", "anki_keys", "clean", "app_ini"};
static int golden_save(int k, const char *text, size_t len, const VjoConfig *c, char *out, size_t cap)
{
    return k == 0 ? vjo_config_patch_preferences(text, len, c->vocr_tier, c->japanese_only_selection, c->highlight_mode, out, cap)
         : k == 1 ? vjo_config_patch_keys(text, len, golden_anki_keys, golden_anki_values, 3, out, cap)
         : k == 2 ? vjo_config_patch_keys(text, len, NULL, NULL, 0, out, cap)
                  : vjo_ini_patch_keys(text, len, golden_anki_keys, golden_anki_values, 3, out, cap);
}

/* The real files' saves: each output's bytes, a hash, and the lines it drops from and adds to the input. */
static void golden_saves_file(GoldenOut *o, const char *text, size_t len)
{
    static char out[2 * (VJO_CONFIG_TEXT_MAX + 1)];
    VjoConfig c;
    vjo_config_defaults(&c);
    vjo_config_parse(&c, text, len);
    for (int k = 0; k < 4; k++) {
        int n = golden_save(k, text, len, &c, out, sizeof(out));
        golden_printf(o, "  %s: %d bytes", golden_save_names[k], n);
        if (n >= 0) golden_printf(o, ", fnv1a %016llx", golden_hash(out, (size_t)n));
        golden_puts(o, "\n");
        if (n >= 0) golden_line_diff(o, text, len, out, (size_t)n);
    }
}

/* A record's line: what vjo_config_removed_line says of each of its lines when one is not K (keep), then each save
 * whose output is not the record with the save's own lines appended (the four preferences with the record's values,
 * or the Anki keys; none for the clean copy), after a newline where the record ends without one: the output's
 * difference from that, as the lines it drops and adds. A record with neither is counted, not listed. */
static unsigned golden_saves_quiet;
static void golden_saves_record(const char *record, size_t n, void *arg)
{
    static char out[2 * (VJO_CONFIG_TEXT_MAX + 1)], want[2 * (VJO_CONFIG_TEXT_MAX + 1)];
    GoldenOut *o = (GoldenOut *)arg;
    size_t mark = o->n, at;
    int noisy = 0, all_keep = 1;
    VjoConfig c;
    vjo_config_defaults(&c);
    vjo_config_parse(&c, record, n);
    golden_puts(o, "> ");
    golden_escape_short(o, record, n);
    at = o->n;
    golden_puts(o, " | removed_line:");
    for (size_t i = 0; i < n;) {
        size_t b = i;
        const char *r = NULL;
        while (i < n && record[i] != '\n') i++;
        if (i < n) i++;
        int kind = vjo_config_removed_line(record + b, i - b, &r);
        all_keep &= kind == VJO_LINE_KEEP;
        golden_printf(o, " %c", kind >= 0 && kind <= 3 ? "KDRS"[kind] : '?');
        if (r) {
            golden_puts(o, "(");
            golden_escape(o, r, strlen(r));
            golden_puts(o, ")");
        }
    }
    if (all_keep) {
        o->n = at;
        o->p[at] = '\0';
    } else
        noisy = 1;
    for (int k = 0; k < 4; k++) {
        int got = golden_save(k, record, n, &c, out, sizeof(out));
        size_t w = 0;
        if (got >= 0) {
            char tail[256];
            int t = k == 0 ? snprintf(tail, sizeof(tail), "japanese_only_selection = %s\nhighlight_mode = %s\n"
                                      "vocr_tier = %s\n",
                                      c.japanese_only_selection ? "on" : "off",
                                      c.highlight_mode == VJO_HIGHLIGHT_WORD ? "word" : "character",
                                      vjo_vocr_tier_id(c.vocr_tier))
                  : k == 2 ? 0
                           : snprintf(tail, sizeof(tail), "anki_mining = on\nanki_screenshot = off\n"
                                      "anki_screenshot_trim_bars = on\n");
            memcpy(want, record, n);
            w = n;
            if (t > 0 && n && record[n - 1] != '\n') want[w++] = '\n';
            if (t > 0) memcpy(want + w, tail, (size_t)t);
            w += t > 0 ? (size_t)t : 0;
            if ((size_t)got == w && !memcmp(out, want, w)) continue;
        }
        noisy = 1;
        golden_printf(o, " | %s:", golden_save_names[k]);
        if (got < 0) golden_printf(o, " %d", got);
        else golden_line_diff_inline(o, want, w, out, (size_t)got);
    }
    if (noisy) {
        golden_puts(o, "\n");
    } else {
        o->n = mark;
        o->p[mark] = '\0';
        golden_saves_quiet++;
    }
}

/* The saves' refusals and limits (P2's audit F4: the branches no record reaches): invalid choices, keys and values,
 * NULL and embedded NUL, and each output's room around its exact size: a capacity of the output's bytes + 1 (the
 * NUL), one less, one more, 3 and 4 (a BOM's), 0. */
static void golden_save_limits(GoldenOut *o)
{
    static char out[8192];
    static const char *const texts[] = {"", "\xEF\xBB\xBF", "\xEF\xBB\xBFvocr_tier = large\n",
                                        "anki_mining = on\nfont_size_ja = 20", "dictionary = jpdb\nlog_host = x\n"};
    static const char *const ok_keys[] = {"anki_mining", "anki_screenshot"}, *ok_values[] = {"on", "off"};
    static const char *const nl_values[] = {"on\n", "off"}, *cr_values[] = {"on", "o\rff"};
    static const char *const long_key[] = {"k123456789012345678901234567890123456789"}; /* 40 bytes */
    static const char *const key39[] = {"k12345678901234567890123456789012345678"};
    static const char *const one_value[] = {"x"};
    golden_puts(o, "= the saves' refusals and limits\n");
    for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
        const char *text = texts[t];
        size_t len = strlen(text);
        golden_puts(o, len ? "> " : "> (empty)");
        golden_escape(o, text, len);
        golden_puts(o, "\n");
        /* the four-preference save's choices, each invalid once */
        static const struct { int tier, ja, hl, results; } prefs[] = {
            {VJO_VOCR_TIER_NORMAL, 0, VJO_HIGHLIGHT_WORD, 2}, {0, 0, 0, 2}, {4, 0, 0, 2}, {2, 2, 0, 2}, {2, -1, 0, 2},
            {2, 0, 2, 2}, {2, 0, 0, 0}, {2, 0, 0, 9}, {3, 1, VJO_HIGHLIGHT_CHARACTER, 8}};
        golden_puts(o, "  preferences choices:");
        for (size_t k = 0; k < sizeof(prefs) / sizeof(prefs[0]); k++)
            golden_printf(o, " %d", vjo_config_patch_preferences(text, len, prefs[k].tier, prefs[k].ja, prefs[k].hl, out, sizeof(out)));
        golden_puts(o, "\n");
        /* the generic patch's keys and values */
        golden_printf(o, "  keys: n=-1 %d, n=9 %d, a value with LF %d, with CR %d, a 40-byte key %d, a 39-byte key %d\n",
                      vjo_config_patch_keys(text, len, ok_keys, ok_values, -1, out, sizeof(out)),
                      vjo_config_patch_keys(text, len, ok_keys, ok_values, 9, out, sizeof(out)),
                      vjo_config_patch_keys(text, len, ok_keys, nl_values, 2, out, sizeof(out)),
                      vjo_config_patch_keys(text, len, ok_keys, cr_values, 2, out, sizeof(out)),
                      vjo_config_patch_keys(text, len, long_key, one_value, 1, out, sizeof(out)),
                      vjo_config_patch_keys(text, len, key39, one_value, 1, out, sizeof(out)));
        /* each save's room: its exact size, one less, one more, a BOM's 3 and 4, and none */
        for (int k = 0; k < 4; k++) {
            int full = k == 0 ? vjo_config_patch_preferences(text, len, 3, 1, 1, out, sizeof(out))
                     : k == 1 ? vjo_config_patch_keys(text, len, ok_keys, ok_values, 2, out, sizeof(out))
                     : k == 2 ? vjo_config_patch_keys(text, len, NULL, NULL, 0, out, sizeof(out))
                              : vjo_ini_patch_keys(text, len, ok_keys, ok_values, 2, out, sizeof(out));
            size_t caps[] = {(size_t)full + 1, (size_t)full, (size_t)full + 2, 3, 4, 0};
            golden_printf(o, "  %s room (%d):", k == 0 ? "preferences" : k == 1 ? "keys" : k == 2 ? "clean" : "app_ini",
                          full);
            for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
                if (full < 0 && c < 3) continue;
                int n = k == 0 ? vjo_config_patch_preferences(text, len, 3, 1, 1, out, caps[c])
                      : k == 1 ? vjo_config_patch_keys(text, len, ok_keys, ok_values, 2, out, caps[c])
                      : k == 2 ? vjo_config_patch_keys(text, len, NULL, NULL, 0, out, caps[c])
                               : vjo_ini_patch_keys(text, len, ok_keys, ok_values, 2, out, caps[c]);
                golden_printf(o, " %zu:%d", caps[c], n);
            }
            golden_puts(o, "\n");
        }
    }
    /* NULL text with a length, a NULL output, an embedded NUL, a text over VJO_CONFIG_TEXT_MAX */
    static char big[VJO_CONFIG_TEXT_MAX + 2];
    memset(big, 'x', sizeof(big) - 1);
    golden_printf(o, "> (NULL, NUL, too long)\n  preferences: %d %d %d %d\n  keys: %d %d %d %d\n",
                  vjo_config_patch_preferences(NULL, 1, 2, 0, 0, out, sizeof(out)),
                  vjo_config_patch_preferences("a\0b", 3, 2, 0, 0, out, sizeof(out)),
                  vjo_config_patch_preferences("a", 1, 2, 0, 0, NULL, 10),
                  vjo_config_patch_preferences(big, sizeof(big) - 1, 2, 0, 0, out, sizeof(out)),
                  vjo_config_patch_keys(NULL, 1, NULL, NULL, 0, out, sizeof(out)),
                  vjo_config_patch_keys("a\0b", 3, NULL, NULL, 0, out, sizeof(out)),
                  vjo_config_patch_keys("a", 1, NULL, NULL, 0, NULL, 10),
                  vjo_config_patch_keys(big, sizeof(big) - 1, NULL, NULL, 0, out, sizeof(out)));
}

/* Their outputs for the fixtures, the template and each record of host/tests/fixtures/config-lines.txt, against
 * host/tests/fixtures/config-saves.golden (host/tests/config_golden.h). */
static void test_config_saves_golden(void)
{
    static const char *const files[] = {"host/tests/fixtures/config-old-app-test2.ini",
                                        "host/tests/fixtures/config-test3-owner.ini", "docs/config.example.ini"};
    GoldenOut o = {0};
    char path[512], why[GOLDEN_WHY];
    golden_puts(&o, "# The Rubi app's saves of the config fixtures (host/tests/test_preferences.c config_saves_golden; "
                    "VJO_UPDATE_GOLDEN=1 rewrites this file)\n");
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        size_t len;
        snprintf(path, sizeof(path), "%s/%s", VJO_SOURCE_DIR, files[i]);
        char *text = golden_read(path, &len);
        TEST_ASSERT_(text != NULL, "%s", path);
        golden_printf(&o, "= %s\n", files[i]);
        golden_saves_file(&o, text, len);
        free(text);
    }
    golden_puts(&o, "= each record's saves where they differ from keeping its lines and appending the save's own (the four "
                    "preferences with its values, the Anki keys; nothing for the clean copy)\n");
    golden_saves_quiet = 0;
    snprintf(path, sizeof(path), "%s/host/tests/fixtures/config-lines.txt", VJO_SOURCE_DIR);
    TEST_CHECK(golden_records(path, golden_saves_record, &o) > 300);
    golden_long_records(golden_saves_record, &o);
    golden_printf(&o, "= %u records keep every line in all four saves\n", golden_saves_quiet);
    golden_save_limits(&o);
    snprintf(path, sizeof(path), "%s/host/tests/fixtures/config-saves.golden", VJO_SOURCE_DIR);
    int rc = golden_check(&o, path, why, sizeof(why));
    TEST_CHECK_(rc >= 0, "%s", why);
    free(o.p);
}

/* region.ini's text ("-" without the file). */
static const char *region_text(void)
{
    static char text[256];
    FILE *f = fopen(VJO_PREFERENCES_REGION_PATH, "rb");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (!f) return "-";
    fclose(f);
    text[n] = 0;
    return text;
}

/* #358: Suggest a text box, kept in region.ini, through the actual Shell's region.ini code (shell/files.c): OFF is
 * the line for every game, "region = full", so that a game without its own reads its whole screen with a region found
 * (VJO_REGION_ALL: no box offered, shell/worker.c push_region); ON drops it (VJO_REGION_NONE). Every game's own line
 * is kept by both and by the Shell's own saves; a region.ini that cannot be read is never rewritten, and a failed
 * write or rename leaves the file as it was (a save cut short between its renames is put back by the next load). */
static void test_suggest_lives_in_region_ini_and_the_shell_reads_it(void)
{
    VjoRect r, q = {100, 40000, 30000, 20000};
    reset();
    unlink(VJO_PREFERENCES_REGION_PATH);
    unlink(VJO_PREFERENCES_REGION_PATH ".bak");
    TEST_CHECK(vjo_preferences_suggest(&arena) == 1); /* no file: on */
    TEST_CHECK(vjo_preferences_save_suggest(1, &arena) == 0 && !strcmp(region_text(), "-")); /* on: nothing written */
    TEST_CHECK(vjo_region_load("PCSG00415", &r, &arena) == VJO_REGION_NONE);
    TEST_CHECK(vjo_preferences_save_suggest(0, &arena) == 0 && vjo_preferences_suggest(&arena) == 0);
    TEST_CHECK_(!strcmp(region_text(), "region = full\n"), "'%s'", region_text());
    TEST_CHECK(vjo_region_load("PCSG00415", &r, &arena) == VJO_REGION_ALL && r.w == 0);
    /* the Shell saves a game's box: the line for every game stays */
    whole_writes = 1;
    TEST_CHECK(vjo_region_save("PCSG00415", &q, &arena) == 0);
    whole_writes = 0;
    TEST_CHECK(vjo_region_load("PCSG00415", &r, &arena) == VJO_REGION_GAME && !memcmp(&r, &q, sizeof(r)));
    TEST_CHECK(vjo_region_load("PCSB00001", &r, &arena) == VJO_REGION_ALL && vjo_preferences_suggest(&arena) == 0);
    /* ON: the line goes, the game's stays, and nothing else is left beside it */
    TEST_CHECK(vjo_preferences_save_suggest(1, &arena) == 0 && vjo_preferences_suggest(&arena) == 1);
    TEST_CHECK_(!strcmp(region_text(), "PCSG00415 = 100,40000,30000,20000\n"), "'%s'", region_text());
    TEST_CHECK(access(VJO_PREFERENCES_REGION_PATH ".bak", F_OK) != 0 && access(VJO_PREFERENCES_REGION_PATH ".tmp", F_OK) != 0);
    TEST_CHECK(vjo_region_load("PCSB00001", &r, &arena) == VJO_REGION_NONE);
    TEST_CHECK(vjo_region_load("PCSG00415", &r, &arena) == VJO_REGION_GAME);
    /* only 0 or 1 */
    TEST_CHECK(vjo_preferences_save_suggest(2, &arena) == -1 && vjo_preferences_suggest(&arena) == 1);
    /* unreadable: -1, never rewritten */
    read_fail = 1;
    TEST_CHECK(vjo_preferences_suggest(&arena) == -1 && vjo_preferences_save_suggest(0, &arena) == -1);
    read_fail = 0;
    TEST_CHECK(!strcmp(region_text(), "PCSG00415 = 100,40000,30000,20000\n"));
    /* a write that fails partway, or a rename: the file as it was */
    write_fail = 1;
    written = 0;
    TEST_CHECK(vjo_preferences_save_suggest(0, &arena) == -1);
    write_fail = 0;
    TEST_CHECK(!strcmp(region_text(), "PCSG00415 = 100,40000,30000,20000\n") && vjo_preferences_suggest(&arena) == 1);
    rename_fail = renames + 2; /* the new file's rename, after the old one's to the backup */
    TEST_CHECK(vjo_preferences_save_suggest(0, &arena) == -1);
    rename_fail = 0;
    TEST_CHECK(!strcmp(region_text(), "PCSG00415 = 100,40000,30000,20000\n") && vjo_preferences_suggest(&arena) == 1);
    /* cut short between its renames: only the backup there; the next load puts it back */
    TEST_CHECK(rename(VJO_PREFERENCES_REGION_PATH, VJO_PREFERENCES_REGION_PATH ".bak") == 0);
    TEST_CHECK(vjo_preferences_suggest(&arena) == 1 && !strcmp(region_text(), "PCSG00415 = 100,40000,30000,20000\n"));
    TEST_CHECK(vjo_preferences_save_suggest(0, &arena) == 0 &&
               !strcmp(region_text(), "PCSG00415 = 100,40000,30000,20000\nregion = full\n"));
    TEST_CHECK(handles == 0);
    unlink(VJO_PREFERENCES_REGION_PATH);
    unlink(VJO_PREFERENCES_REGION_PATH ".tmp");
}

static void put_theme(const char *path, const char *text) { put_file(path, text, strlen(text)); }
static void test_shell_colour_scheme(void)
{
    reset();
    VjoArena shell; unsigned char mem[24 * 1024];
    vjo_arena_init(&shell, mem, sizeof(mem));
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 0 && !shell.used && !handles);
    put_theme("ux0:data/VitaJPOverlay/app.ini", "colour_scheme = mint\nanki_card_type = sentence\n");
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 1 && !shell.used && !handles);
    unlink("ux0:data/VitaJPOverlay/app.ini");
    put_theme("ux0:data/VitaJPOverlay/app.ini.bak", "colour_scheme = paper\n");
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 2 && !shell.used && !handles);
    put_theme("ux0:data/VitaJPOverlay/app.ini", "colour_scheme = default\n");
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 0); /* primary beats backup */
    read_fail = 1;
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 0 && !shell.used && !handles);
    read_fail = 0;
    put_theme("ux0:data/VitaJPOverlay/app.ini", "colour_scheme = mint\n");
    TEST_ASSERT(vjo_arena_alloc(&shell, 64));
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 1 && shell.used == 64 && !handles);
    vjo_arena_init(&shell, mem, 8); /* storage opens, but scratch cannot hold the text */
    TEST_CHECK(vjo_colour_scheme_load(&shell) == 0 && !shell.used && !handles);
    unlink("ux0:data/VitaJPOverlay/app.ini"); unlink("ux0:data/VitaJPOverlay/app.ini.bak");
}

TEST_LIST = {
    {"shared theme load, backup, short reads and errors", test_shell_colour_scheme},
    {"settings_save_drops_removed_anki_settings", test_settings_save_drops_removed_anki_settings},
    {"a_bom_led_line_is_the_same_setting_to_every_writer", test_a_bom_led_line_is_the_same_setting_to_every_writer},
    {"anki_page_saves_mining_and_address_keeping_every_other_byte", test_anki_page_save},
    {"read_text_and_speed_boost_save_where_their_lines_are_and_the_Shell_reads_them", test_reading_settings_save},
    {"preference_patch_preserves_comments_BOM_keys_and_collapses_duplicates", test_patch_preserves_unrelated_bytes},
    {"engine_choice_vocr_Lens_VOCR2_Meiki_ncnn_loads_as_VOCR_and_next_save_drops_it", test_engine_choice_loads_as_vocr_and_saves_drop_it},
    {"removed_jpdb_jiten_choice_loads_as_local_and_saves_drop_their_API_keys_and_backup", test_removed_dictionaries_rewritten_by_saves},
    {"removed_Hachidori_relay_choice_loads_as_local_with_one_warning_and_saves_write_local", test_removed_relay_rewritten_by_saves},
    {"legacy_dictionary_count_is_ignored_and_removed", test_dictionary_results_patch_and_parse},
    {"model_tier_is_owned_older_vocr_model_lines_map_to_it_and_go_on_save", test_model_tier_patch_parse_and_legacy_models},
    {"saved_size_and_selection_choices_survive_switch_reload", test_save_switch_reload_retains_other_settings},
    {"size_choice_loads_shell_and_retains_other_preferences", test_tier_choice_loads_shell_and_retains_other_preferences},
    {"highlight_only_changes_reload_in_actual_Shell_and_preserve_other_choices", test_highlight_only_switch_loads_actual_shell_without_other_changes},
    {"saving_preferences_removes_legacy_dictionary_count", test_dictionary_results_switch_loads_actual_shell},
    {"partial_write_close_sync_and_rename_failures_retain_confirmable_choices", test_save_failure_preserves_or_recovers_actual_choices},
    {"unreadable_embedded_NUL_and_oversize_config_are_not_overwritten", test_unreadable_or_invalid_config_never_overwritten},
    {"backup_only_config_loads_in_actual_Shell_without_overwriting_saved_keys", test_backup_only_loads_actual_shell_without_default_publication},
    {"largest_published_config_round_trips_through_actual_24KiB_Shell_loader", test_maximum_published_config_fits_actual_shell_scratch},
    {"old_app_config_from_101_loads_as_VOCR_with_one_warning_and_a_save_repairs_it", test_old_app_config_loads_as_vocr_and_a_save_repairs_it},
    {"shell_runs_on_the_defaults_and_never_writes_config_ini", test_shell_runs_on_the_defaults_without_writing_config_ini},
    {"shell_reads_a_stamp_file_s_first_line_whole", test_file_line_and_exists},
    {"suggest_a_text_box_lives_in_region_ini_and_the_actual_Shell_reads_it", test_suggest_lives_in_region_ini_and_the_shell_reads_it},
    {"config_saves_golden", test_config_saves_golden},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("preferences")) return 2; /* each test's card in it, all removed at the end */
    return acutest_main(argc, argv);
}
