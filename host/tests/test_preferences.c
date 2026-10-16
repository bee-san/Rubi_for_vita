#include "acutest.h"
#include "../../settings/preferences.h"
#include "../../shell/shell.h"
#include "../../include/vjo_vocr_sizes.h"
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
    "; Keep this comment\r\nocr_backend = lens\nocr_mode = on_press\n"
    "japanese_only_selection = on\n"
    "highlight_mode = word\n"
    "dictionary = local\nlocal_dictionaries = daijirin.vjdict,jmdict.vjdict\n"
    "local_dictionary_results = 2\n"
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
        char dir[] = "/tmp/vjo-preferences-XXXXXX";
        TEST_ASSERT(mkdtemp(dir)); TEST_ASSERT(chdir(dir) == 0);
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
    TEST_CHECK(cfg.local_dictionary_results == expected_results);
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.font_size_ja == 22);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
    TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL);
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
        "local_dictionary_results = 1\nvocr_tier = normal\n";
    char out[1024];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    TEST_MSG("%s", out);
    n = vjo_config_patch_preferences("font_size_ja=18", 15, VJO_VOCR_TIER_NORMAL, 0, VJO_HIGHLIGHT_WORD, 1, out, sizeof(out));
    TEST_CHECK(n > 0 && !strcmp(out, "font_size_ja=18\njapanese_only_selection = off\nhighlight_mode = word\n"
                                     "local_dictionary_results = 1\nvocr_tier = normal\n"));
    TEST_MSG("%s", out);
    /* The engine is no choice (VOCR is the only one): a save never writes it. */
    TEST_CHECK(!strstr(out, "ocr_backend"));
    TEST_CHECK(vjo_config_patch_preferences("", 0, VJO_VOCR_TIER_NORMAL, 1, 2, 1, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences(input, sizeof(input)-1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, out, 5) < 0);
    TEST_CHECK(vjo_config_patch_preferences("bad\0key", 7, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences("", 0, -1, 1, VJO_HIGHLIGHT_WORD, 1, out, sizeof(out)) < 0);
    TEST_CHECK(vjo_config_patch_preferences("", 0, VJO_VOCR_TIER_NORMAL, 2, VJO_HIGHLIGHT_WORD, 1, out, sizeof(out)) < 0);
}

static void test_dictionary_results_patch_and_parse(void)
{
    /* Owned like the other choices: replaced in place, duplicates collapsed,
     * comments and unrelated keys untouched, invalid counts never written. */
    const char input[] = "; local_dictionary_results = 5 in a comment\n"
        "LOCAL_DICTIONARY_RESULTS=7\nlocal_dictionaries = a.vjdict,b.vjdict\n"
        "local_dictionary_results = 3\nocr_backend = vocr\n"
        "japanese_only_selection = on\nhighlight_mode = word\n";
    const char expected[] = "; local_dictionary_results = 5 in a comment\n"
        "local_dictionary_results = 8\nlocal_dictionaries = a.vjdict,b.vjdict\n"
        "japanese_only_selection = on\nhighlight_mode = word\nvocr_tier = normal\n";
    char out[512];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 8, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    for (int bad = -1; bad <= 9; bad += 10) {
        TEST_CHECK(vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD,
                                                bad, out, sizeof(out)) < 0);
    }
    TEST_CHECK(vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD,
                                            0, out, sizeof(out)) < 0);
    VjoConfig cfg;
    vjo_config_defaults(&cfg);
    TEST_CHECK(cfg.local_dictionary_results == 1);
    vjo_config_parse(&cfg, out, (size_t)n);
    TEST_CHECK(cfg.local_dictionary_results == 8 && !cfg.n_warnings);
    const char *invalid[] = {"local_dictionary_results = 0\n", "local_dictionary_results = 9\n",
                             "local_dictionary_results = two\n", "local_dictionary_results =\n",
                             "local_dictionary_results = -1\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, invalid[i], strlen(invalid[i]));
        TEST_CHECK(cfg.local_dictionary_results == 1 && cfg.n_warnings == 1);
        TEST_MSG("%s", invalid[i]);
    }
    /* The default file documents the option with the preserved default. */
    vjo_config_defaults(&cfg);
    vjo_config_parse(&cfg, vjo_config_default_text(), strlen(vjo_config_default_text()));
    TEST_CHECK(cfg.local_dictionary_results == 1 && !cfg.n_warnings);
    TEST_CHECK(strstr(vjo_config_default_text(), "\nlocal_dictionary_results = 1\n") != NULL);
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
        "font_size_ja = 22\njapanese_only_selection = on\nhighlight_mode = word\nlocal_dictionary_results = 1\n";
    char out[512];
    int n = vjo_config_patch_preferences(input, sizeof(input) - 1, VJO_VOCR_TIER_NORMAL, 1,
                                         VJO_HIGHLIGHT_WORD, 1, out, sizeof(out));
    TEST_CHECK(n == sizeof(expected) - 1 && !strcmp(out, expected));
    TEST_MSG("%s", out);
    for (int tier = -1; tier <= 5; tier++) { /* only the three tiers can be written */
        n = vjo_config_patch_preferences("", 0, tier, 1, VJO_HIGHLIGHT_WORD, 1, out, sizeof(out));
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
        {"vocr_model = unknown_w8.vocr\n", VJO_VOCR_TIER_NORMAL, 1},
        {"vocr_model = ../H15_w8.vocr\n", VJO_VOCR_TIER_NORMAL, 1},
        {"vocr_model = H15_w8.bin\n", VJO_VOCR_TIER_NORMAL, 1},
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
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 1, VJO_HIGHLIGHT_WORD, 1, &arena) == 0);
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
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD);
    put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) == 0);
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_CHARACTER, expected_results, &arena) == 0);
    check_choices(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_CHARACTER);
    char out[sizeof(original)+128];
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    ssize_t n = read(fd, out, sizeof(out)-1); close(fd); TEST_ASSERT(n > 0); out[n] = 0;
    TEST_CHECK(strstr(out, "; Keep this comment\r\n") && strstr(out, "unknown_future_setting = unchanged\n"));
    TEST_CHECK(!strstr(out, "ocr_backend")); /* the fixture's Lens choice went with the first save */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) == 0); check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
/* An OCR choice saved by the app keeps every anki_* line verbatim, and the
 * removed settings of older builds (AnkiConnect's anki_host, the field
 * mapping's anki_deck, anki_note_type, anki_field_*) load without a warning. */
static int read_back(const char *path, char *text, size_t cap);

/* The removed Anki settings (AnkiConnect's address and audio source, the old
 * field mapping) load without a warning, and either save of the app drops
 * their lines (core/config.h vjo_config_removed_line), the backup included:
 * anki_audio_url's URL can hold a key. anki_mining and every other line stay. */
static void test_settings_save_drops_removed_anki_settings(void)
{
    static const char anki[] =
        "; mine\nocr_backend = vocr\nanki_mining = on\nanki_host = 192.168.1.23:8766\nanki_deck = Mining #1 ; main\n"
        "anki_note_type = Lapis;v2\nanki_field_picture =\nANKI_FIELD_WORD = Word\n"
        "anki_audio_url = https://a.invalid/?term={term}&key=secret-url\nanki_audio_url https://a.invalid/?k=secret-2\n"
        "anki_host\nfont_size_ja = 22\n";
    for (int save = 0; save < 2; save++) {
        char out[sizeof(anki) + 256];
        VjoConfig cfg;
        reset(); put_file(VJO_PREFERENCES_PATH, anki, sizeof(anki)-1);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.n_warnings == 0 && cfg.anki_mining == 1);
        TEST_MSG("%d %s", cfg.n_warnings, cfg.warnings[0]);
        TEST_ASSERT((save ? vjo_preferences_save_anki(1, &arena)
                          : vjo_preferences_save(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER, 3, &arena)) == 0);
        TEST_CHECK(arena.used == 0 && handles == 0);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, out, sizeof(out)) > 0);
        TEST_CHECK(!strstr(out, "anki_host") && !strstr(out, "anki_deck") && !strstr(out, "anki_note_type") &&
                   !strstr(out, "anki_field_") && !strstr(out, "ANKI_FIELD_") && !strstr(out, "anki_audio_url") &&
                   !strstr(out, "secret") && !strstr(out, "ocr_backend")); /* the engine choice went too */
        TEST_CHECK(strstr(out, "; mine\n") && strstr(out, "anki_mining = on\n") && strstr(out, "font_size_ja = 22\n"));
        TEST_MSG("save %d: %s", save, out);
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH ".bak", out, sizeof(out)) > 0);
        TEST_CHECK_(!strstr(out, "secret") && !strstr(out, "anki_audio_url"), "save %d: config.ini.bak: %s", save, out);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.anki_mining == 1 && cfg.n_warnings == 0 && cfg.font_size_ja == 22 &&
                   (save || (cfg.vocr_tier == VJO_VOCR_TIER_LARGE && cfg.local_dictionary_results == 3)));
    }
}

/* The patch the Anki page's save uses: its keys replaced where they are, a
 * duplicate dropped, a missing one appended; every other byte kept. */
static void test_anki_page_save(void)
{
    static const char before[] =
        "\xEF\xBB\xBF; Keep this comment\r\nanki_mining = off ; old\r\nocr_backend = vocr\n"
        "LOG_FILE=on\nlog_host = 10.0.0.1;x\nanki_mining = on\nfont_size_ja = 22";
    static const char *const keys[] = {"anki_mining", "log_file"};
    const char *values[] = {"on", "off"};
    char out[512];
    VjoConfig cfg;
    int len = vjo_config_patch_keys(before, sizeof(before) - 1, keys, values, 2, out, sizeof(out));
    TEST_CHECK(len > 0 && !strcmp(out, "\xEF\xBB\xBF; Keep this comment\r\nanki_mining = on\n"
                                       "log_file = off\nlog_host = 10.0.0.1;x\nfont_size_ja = 22"));
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
    TEST_ASSERT(vjo_preferences_save_anki(1, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    /* (the fixture's one warning is its unknown_future_setting) */
    TEST_CHECK(cfg.anki_mining == 1 && cfg.n_warnings == 1 && cfg.font_size_ja == 22 &&
               cfg.vocr_tier == VJO_VOCR_TIER_NORMAL);
    TEST_MSG("mining %d warnings %d: %s", cfg.anki_mining, cfg.n_warnings, cfg.warnings[0]);
    TEST_ASSERT(vjo_preferences_save_anki(0, &arena) == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.anki_mining == 0 && cfg.n_warnings == 1 && cfg.font_size_ja == 22);
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
        TEST_CHECK(!strncmp(text, original, head) && !strncmp(text + head, engine + line, tail) &&
                   !strcmp(text + head + tail, "anki_mining = off\n"));
        TEST_MSG("%s", text);
    }
    /* a failed write leaves the old file */
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    write_fail = 1;
    TEST_CHECK(vjo_preferences_save_anki(1, &arena) < 0);
    write_fail = 0;
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(!cfg.anki_mining);
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
        TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_CHARACTER, expected_results, &arena) < 0);
        check_choices(fault == 5 ? VJO_VOCR_TIER_SMALL : VJO_VOCR_TIER_NORMAL, fault == 5 ? 0 : 1,
                      fault == 5 ? VJO_HIGHLIGHT_CHARACTER : VJO_HIGHLIGHT_WORD);
        TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) == 0);
    }
    reset(); put_file(VJO_PREFERENCES_PATH ".bak", original, sizeof(original)-1);
    VjoConfig cfg; TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) == 0); check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_tier_choice_loads_shell_and_retains_other_preferences(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    const int tiers[] = {VJO_VOCR_TIER_SMALL, VJO_VOCR_TIER_LARGE, VJO_VOCR_TIER_NORMAL, VJO_VOCR_TIER_LARGE,
                         VJO_VOCR_TIER_SMALL};
    for (unsigned i = 0; i < sizeof(tiers)/sizeof(tiers[0]); i++) {
        TEST_ASSERT(vjo_preferences_save(tiers[i], 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) == 0);
        check_choices(tiers[i], 1, VJO_HIGHLIGHT_WORD);
        VjoConfig cfg; vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.vocr_tier == tiers[i] && cfg.dictionary == VJO_DICT_LOCAL);
        TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS && cfg.highlight_mode == VJO_HIGHLIGHT_WORD);
        TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
        /* the fixture's ocr_backend = lens went with the first save: the Shell no longer warns */
        TEST_CHECK(cfg.n_warnings == 1 && strstr(cfg.warnings[0], "unknown_future_setting"));
        TEST_CHECK(shell.used == 0 && handles == 0);
    }
    /* Changing the other controls must keep the size. */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER, expected_results, &arena) == 0);
    check_choices(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_CHARACTER);
    VjoConfig cfg; vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_LARGE && cfg.dictionary == VJO_DICT_LOCAL);
    TEST_CHECK(!cfg.japanese_only_selection && cfg.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    /* A failed save never replaces the last saved choice. */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) == 0);
    write_fail = 1; written = 0;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) < 0);
    write_fail = 0;
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_unreadable_or_invalid_config_never_overwritten(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1); read_fail = 1;
    VjoConfig cfg;
    TEST_CHECK(vjo_preferences_load(&cfg, &arena) < 0);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0 && handles == 0);
    read_fail = 0; check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
    put_file(VJO_PREFERENCES_PATH, "broken\0config", 13);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) < 0);
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
    memset(memory, '#', VJO_PREFERENCES_TEXT_MAX);
    put_file(VJO_PREFERENCES_PATH, (char *)memory, VJO_PREFERENCES_TEXT_MAX);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) < 0);
    TEST_CHECK(written == 0 && handles == 0 && arena.used == 0);
}
static void test_highlight_only_switch_loads_actual_shell_without_other_changes(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    for (int i = 0; i < 4; i++) {
        int highlight_mode = i % 2 ? VJO_HIGHLIGHT_WORD : VJO_HIGHLIGHT_CHARACTER;
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, highlight_mode, expected_results, &arena) == 0);
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
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, -1, expected_results, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0 && arena.used == 0 && handles == 0);
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_dictionary_results_switch_loads_actual_shell(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    for (int results = 1; results <= 8; results++) {
        expected_results = results;
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, results, &arena) == 0);
        check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
        VjoConfig cfg; vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.local_dictionary_results == results && cfg.vocr_tier == VJO_VOCR_TIER_NORMAL);
        TEST_CHECK(cfg.japanese_only_selection == 1 && cfg.highlight_mode == VJO_HIGHLIGHT_WORD);
        TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
        TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
        TEST_CHECK(shell.used == 0 && handles == 0);
    }
    /* An out-of-range count is rejected before any write; 8 stays saved. */
    written = 0; renames = 0;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 9, &arena) < 0);
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 0, &arena) < 0);
    TEST_CHECK(written == 0 && renames == 0 && arena.used == 0 && handles == 0);
    check_choices(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD);
}
static void test_backup_only_loads_actual_shell_without_default_publication(void)
{
    reset(); put_file(VJO_PREFERENCES_PATH, original, sizeof(original)-1);
    rename_fail_after = 2; /* Fail both temp publication and primary restoration. */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_SMALL, 0, VJO_HIGHLIGHT_WORD, expected_results, &arena) < 0);
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
    TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.font_size_ja == 22);
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
        "local_dictionary_results = 2\n"
        "ocr_model_dir = ux0:data/custom-ocr\nvocr_tier = normal\n"
        "hachidori_host = 192.168.1.20\n" /* the removed relay's: kept as written */
        "font_size_ja = 22\nunknown_future_setting = unchanged\n";
    reset(); char near[VJO_CONFIG_TEXT_MAX + 1];
    memset(near, '#', sizeof(near)); memcpy(near, saved, sizeof(saved)-1);
    near[VJO_CONFIG_TEXT_MAX-1] = '\n'; near[VJO_CONFIG_TEXT_MAX] = 0;
    put_file(VJO_PREFERENCES_PATH, near, VJO_CONFIG_TEXT_MAX);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, expected_results, &arena) == 0);
    unsigned char shell_mem[24 * 1024]; VjoArena shell;
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    VjoConfig cfg; vjo_config_load(&cfg, &shell);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && cfg.japanese_only_selection == 1);
    TEST_CHECK(!strcmp(cfg.local_dictionaries, "daijirin.vjdict,jmdict.vjdict"));
    TEST_CHECK(!strcmp(cfg.ocr_model_dir, "ux0:data/custom-ocr"));
    TEST_CHECK(shell.peak == sizeof(shell_mem) && shell.used == 0 && handles == 0);
    /* Adding one byte (on -> off) would exceed the reader, so the original stays intact. */
    written = 0; renames = 0;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 0, VJO_HIGHLIGHT_WORD, expected_results, &arena) < 0);
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
    for (int save = 0; save < 2; save++) {
        char text[sizeof(legacy) + 256];
        VjoConfig cfg;
        unsigned char shell_mem[24 * 1024]; VjoArena shell;
        reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 1 &&
                   !strcmp(cfg.warnings[0], "dictionary: jpdb.io was removed; using local"));
        TEST_ASSERT((save ? vjo_preferences_save_anki(1, &arena)
                          : vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena)) == 0);
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
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 0 && cfg.ocr_mode == VJO_OCR_ON_PRESS &&
                   cfg.font_size_ja == 22);
        TEST_CHECK(!save || cfg.anki_mining == 1);
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 0 && shell.used == 0);
    }
    /* a failed first publication leaves the old file and saves nothing */
    reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
    write_fail = 1;
    TEST_CHECK(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena) < 0);
    write_fail = 0;
    {
        char text[sizeof(legacy) + 16];
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) == (int)sizeof(legacy) - 1 &&
                   !strcmp(text, legacy));
    }
    /* A UTF-8 BOM (Notepad) before a key on the very first line (audit B1): both saves find
     * the key after the BOM, as their patches do, so neither keeps it in the backup. */
    for (int save = 0; save < 2; save++) {
        static const char bom[] = "\xEF\xBB\xBF" "jpdb_api_key = secret-bom\nocr_mode = on_press\nfont_size_ja = 22\n";
        char text[256];
        reset(); put_file(VJO_PREFERENCES_PATH, bom, sizeof(bom) - 1);
        TEST_ASSERT((save ? vjo_preferences_save_anki(1, &arena)
                          : vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena)) == 0);
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
            int rc = vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena);
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
        int rc = vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena);
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
        TEST_CHECK(vjo_preferences_load(&cfg, &arena) == 1 && cfg.dictionary == VJO_DICT_LOCAL);
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena) == 0);
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0 && !strstr(text, "secret") &&
                   strstr(text, "dictionary = local\n"));
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) > 0 && !strstr(text, "secret"));
    }
    /* only the dictionary is replaced; no key: one publication, the backup is the old file */
    reset(); put_file(VJO_PREFERENCES_PATH, "dictionary = JITEN\nfont_size_ja = 22\n", 37);
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena) == 0);
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
                                             VJO_HIGHLIGHT_WORD, 1, out, sizeof(out));
        TEST_CHECK(n > 0 && !strncmp(out, "Dictionary = Local ; mine\n", 25));
        n = vjo_config_patch_preferences("Dictionary = Hachidori ; relay\n", 31, VJO_VOCR_TIER_NORMAL, 1,
                                         VJO_HIGHLIGHT_WORD, 1, out, sizeof(out));
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
    for (int save = 0; save < 2; save++) {
        char text[sizeof(legacy) + 256];
        VjoConfig cfg;
        unsigned char shell_mem[24 * 1024]; VjoArena shell;
        reset(); put_file(VJO_PREFERENCES_PATH, legacy, sizeof(legacy) - 1);
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 1 &&
                   !strcmp(cfg.warnings[0], "Dictionary: the Hachidori relay was removed; using local"));
        TEST_MSG("%d warnings, first: %s", cfg.n_warnings, cfg.n_warnings ? cfg.warnings[0] : "");
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 1 && shell.used == 0);
        renames = 0;
        TEST_ASSERT((save ? vjo_preferences_save_anki(1, &arena)
                          : vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 1, VJO_HIGHLIGHT_WORD, 1, &arena)) == 0);
        TEST_CHECK(arena.used == 0 && handles == 0 && renames == 2); /* one publication */
        TEST_ASSERT(read_back(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0);
        TEST_CHECK(strstr(text, "; Keep this comment\r\ndictionary = local\nhachidori_host = 192.168.1.20:19633\n"
                                "ocr_mode = on_press\nfont_size_ja = 22\n") != NULL);
        TEST_CHECK(!strstr(text, "achidori ;") && !strstr(text, "Dictionary = "));
        TEST_MSG("%s", text);
        TEST_CHECK(read_back(VJO_PREFERENCES_PATH ".bak", text, sizeof(text)) == (int)sizeof(legacy) - 1 &&
                   !strcmp(text, legacy));
        TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 0 && cfg.ocr_mode == VJO_OCR_ON_PRESS &&
                   cfg.font_size_ja == 22);
        TEST_CHECK(!save || cfg.anki_mining == 1);
        vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
        vjo_config_load(&cfg, &shell);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && cfg.n_warnings == 0 && shell.used == 0);
    }
}
/* The engine is no setting any more (VOCR is the only one; Google Lens was
 * removed, the overlay is offline). A config.ini that chose one loads as VOCR,
 * in the app and in the Shell: vocr says nothing, a removed engine (Google
 * Lens, the VOCR2 and Meiki trials, the host tools' ncnn) says so; and the
 * app's next save drops the line, keeping every other one. */
static void test_engine_choice_loads_as_vocr_and_saves_drop_it(void)
{
    static const struct { const char *value, *warning; } engines[] = {
        {"vocr", NULL},
        {"VOCR", NULL},
        {"lens", "ocr_backend: Google Lens was removed (the overlay is offline); using vocr"},
        {"Lens", "ocr_backend: Google Lens was removed (the overlay is offline); using vocr"},
        {"vocr2", "ocr_backend: VOCR2 was removed; using vocr"},
        {"meiki", "ocr_backend: Meiki was removed; using vocr"},
        {"NCNN", "ocr_backend: ncnn runs only in host tools; using vocr"},
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
        TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_LARGE, 0, VJO_HIGHLIGHT_WORD, 1, &arena) == 0);
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

/* #101: test.1's Overlay app saved its four preferences over test.2's
 * config.ini (host/tests/fixtures/config-old-app-test2.ini: ocr_backend =
 * vocr2). Today's Shell and app read it as the local engine with one warning
 * and no unknown setting (its vocr_model gives the Normal size), and the app's
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
    TEST_CHECK(cfg.n_warnings == 1);
    TEST_CHECK(!strcmp(cfg.warnings[0], "ocr_backend: VOCR2 was removed; using vocr"));
    TEST_MSG("%d warnings, first '%s'", cfg.n_warnings, cfg.n_warnings ? cfg.warnings[0] : "");
    TEST_CHECK(cfg.anki_mining && cfg.highlight_mode == VJO_HIGHLIGHT_WORD && cfg.japanese_only_selection);
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_NORMAL && !cfg.vocr_tier_explicit); /* from vocr_model = VOCR3_H15 */
    TEST_CHECK(cfg.ocr_lm == 2 && cfg.ocr_cascade_ms == 2000 && cfg.ocr_refine_lines == 2);
    TEST_CHECK(shell.used == 0 && handles == 0);
    TEST_ASSERT(vjo_preferences_load(&cfg, &arena) == 0);
    TEST_CHECK(cfg.n_warnings == 1);
    /* the app's next save: the engine line (a removed setting) and the two model lines gone and vocr_tier
     * added (API 21), the lines of removed features dropped or replaced as core/config.c
     * vjo_config_removed_line says (since #117 the online dictionaries' empty key lines); every other line kept */
    TEST_ASSERT(vjo_preferences_save(cfg.vocr_tier, cfg.japanese_only_selection, cfg.highlight_mode,
                                     cfg.local_dictionary_results, &arena) == 0);
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
        } else if (kind == VJO_LINE_DROP_SETTING) { /* the engine line */
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
    TEST_CHECK(dropped >= 5 && settings == 1); /* the engine line, the two model lines and (since #117) the two
                                                 * empty key lines at least */
    TEST_CHECK(!strcmp(out, expect));
    TEST_MSG("saved:\n%s", out);
    /* whatever the removed features, the user's own settings survive the save */
    static const char *const kept[] = {"\nanki_mining = on\n", "\nhighlight_mode = word\n", "\nfont_size_ja = 18\n",
                                       "\nlocal_dictionaries = plaintext.vjdict\n", "\nocr_lm = 1000k\n",
                                       "\nocr_refine_lines = psp\n", "\n; Vita JP Overlay settings."};
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        TEST_CHECK_(strstr(out, kept[i]) != NULL, "kept: %s", kept[i] + 1);
    TEST_CHECK(!strstr(out, "\nocr_backend") && !strstr(out, "\nvocr_model =") && /* its comment lines stay */
               !strstr(out, "\nvocr_fallback_model ="));
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
    TEST_CHECK(parsed.n_warnings == 0 && parsed.vocr_tier_explicit); /* the template has a vocr_tier line */
    parsed.vocr_tier_explicit = d.vocr_tier_explicit; /* bookkeeping only: which line set the size */
    TEST_CHECK(!memcmp(&parsed, &d, sizeof(d)));
    TEST_CHECK(written == 0 && renames == 0 && handles == 0 && shell.used == 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) != 0 && access(VJO_PREFERENCES_PATH ".tmp", F_OK) != 0);
    TEST_CHECK(access(VJO_PREFERENCES_PATH ".bak", F_OK) != 0);
    /* what creates it: the Overlay app's first save publishes the commented template with the choice */
    TEST_ASSERT(vjo_preferences_save(VJO_VOCR_TIER_NORMAL, 0, VJO_HIGHLIGHT_WORD, 1, &arena) == 0);
    static char saved[VJO_CONFIG_TEXT_MAX + 1];
    int fd = open(VJO_PREFERENCES_PATH, O_RDONLY);
    TEST_ASSERT(fd >= 0);
    ssize_t n = read(fd, saved, sizeof(saved) - 1); close(fd);
    TEST_ASSERT(n > 0); saved[n] = 0;
    TEST_CHECK(!strncmp(saved, vjo_config_default_text(), 40) && strstr(saved, "\njapanese_only_selection = off\n"));
    TEST_CHECK(!strstr(saved, "ocr_backend")); /* no engine choice: VOCR is the only engine */
    vjo_config_load(&cfg, &shell);
    TEST_CHECK(!cfg.japanese_only_selection && cfg.n_warnings == 0 && shell.used == 0);
}

/* The Shell's reads of the Overlay app's stamps (shell/files.c): the first
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

TEST_LIST = {
    {"settings_save_drops_removed_anki_settings", test_settings_save_drops_removed_anki_settings},
    {"anki_page_saves_mining_and_address_keeping_every_other_byte", test_anki_page_save},
    {"preference_patch_preserves_comments_BOM_keys_and_collapses_duplicates", test_patch_preserves_unrelated_bytes},
    {"engine_choice_vocr_Lens_VOCR2_Meiki_ncnn_loads_as_VOCR_and_next_save_drops_it", test_engine_choice_loads_as_vocr_and_saves_drop_it},
    {"removed_jpdb_jiten_choice_loads_as_local_and_saves_drop_their_API_keys_and_backup", test_removed_dictionaries_rewritten_by_saves},
    {"removed_Hachidori_relay_choice_loads_as_local_with_one_warning_and_saves_write_local", test_removed_relay_rewritten_by_saves},
    {"dictionary_results_is_an_owned_validated_preference", test_dictionary_results_patch_and_parse},
    {"model_tier_is_owned_older_vocr_model_lines_map_to_it_and_go_on_save", test_model_tier_patch_parse_and_legacy_models},
    {"saved_size_and_selection_choices_survive_switch_reload", test_save_switch_reload_retains_other_settings},
    {"size_choice_loads_shell_and_retains_other_preferences", test_tier_choice_loads_shell_and_retains_other_preferences},
    {"highlight_only_changes_reload_in_actual_Shell_and_preserve_other_choices", test_highlight_only_switch_loads_actual_shell_without_other_changes},
    {"dictionary_count_changes_reload_in_actual_Shell_and_preserve_other_choices", test_dictionary_results_switch_loads_actual_shell},
    {"partial_write_close_sync_and_rename_failures_retain_confirmable_choices", test_save_failure_preserves_or_recovers_actual_choices},
    {"unreadable_embedded_NUL_and_oversize_config_are_not_overwritten", test_unreadable_or_invalid_config_never_overwritten},
    {"backup_only_config_loads_in_actual_Shell_without_overwriting_saved_keys", test_backup_only_loads_actual_shell_without_default_publication},
    {"largest_published_config_round_trips_through_actual_24KiB_Shell_loader", test_maximum_published_config_fits_actual_shell_scratch},
    {"old_app_config_from_101_loads_as_VOCR_with_one_warning_and_a_save_repairs_it", test_old_app_config_loads_as_vocr_and_a_save_repairs_it},
    {"shell_runs_on_the_defaults_and_never_writes_config_ini", test_shell_runs_on_the_defaults_without_writing_config_ini},
    {"shell_reads_a_stamp_file_s_first_line_whole", test_file_line_and_exists},
    {NULL, NULL}
};
