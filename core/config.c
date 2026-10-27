#include "config.h"

#include <stddef.h>
#include <string.h>

#include "../include/vjo_vocr_sizes.h"
#include "port.h"
#include "local_dict.h"
#include "textfilter.h"
#include "utf.h"

static const char *const trigger_names[VJO_TRIGGER_COUNT] VJO_DATA_TABLE(config_trigger_names_1) = {
    "select", "start", "l+r", "select+l", "select+r", "rear_double_tap",
};

const char *vjo_trigger_name(int trigger)
{
    if (trigger < 0 || trigger >= VJO_TRIGGER_COUNT)
        return "?";
    return trigger_names[trigger];
}

void vjo_config_defaults(VjoConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->dictionary = VJO_DICT_LOCAL;
    vjo_snprintf(c->local_dictionary_dir, sizeof(c->local_dictionary_dir), "ux0:data/VitaJPOverlay/dictionaries");
    vjo_snprintf(c->local_dictionaries, sizeof(c->local_dictionaries), "plaintext.vjdict,vndb-names.vjdict");
    c->local_dictionary_results = 2; /* a word's entry, then the same word as a name (vndb-names) */
    c->non_japanese_filter = VJO_FILTER_LINES;
    c->japanese_only_selection = 0; /* OFF: the cursor can select any character (the owner, 2026-10-20; ON before) */
    c->highlight_mode = VJO_HIGHLIGHT_WORD;
    c->frequency_badge = 1;
    c->font_size_ja = 18;
    c->font_size_en = 14;
    c->toggle_button = VJO_TRIGGER_L_R;
    c->subtitle_button = VJO_TRIGGER_SELECT_R;
    c->ocr_mode = VJO_OCR_AUTO;
    c->text_source = VJO_SOURCE_AUTO; /* try native text hooks in every enabled Vita game */
    vjo_snprintf(c->ocr_model_dir, sizeof(c->ocr_model_dir), "ux0:data/VitaJPOverlay/ocr");
    /* Normal (VOCR5-N) where the game's memory allows; a smaller tier otherwise. */
    c->vocr_tier = VJO_VOCR_TIER_DEFAULT;
    c->ocr_settle_ms = 150;
    c->ocr_settle_max_ms = 2000;
    c->ocr_skip_blank_lines = 0;
    c->ocr_lm = 2;
    c->ocr_cascade_ms = 2000;
    c->ocr_refine_lines = 2;
    c->ocr_helpers = 2; /* ocr_threads = 3 */
    c->ocr_boost_clock = 1; /* 444 MHz while a job recognizes; never lowers a clock (game_ocr/main.c) */
    c->ocr_settle_press_ms = 1000; /* a job the open overlay waits for (docs/first-ocr.md) */
    c->stick_lookup_ms = 200; /* core/stick_cursor.h VJO_STICK_CURSOR_REST_MS */
    c->log_file = 1; /* log.txt is on unless log_file = off */
    c->anki_screenshot = 1; /* a card keeps a JPEG of the game's screen unless anki_screenshot = off */
    c->anki_screenshot_trim_bars = 1; /* its black bars trimmed unless anki_screenshot_trim_bars = off (docs/anki.md) */
    c->ocr_text_gate = VJO_TEXT_GATE_DEFAULT; /* docs/vita-vn-ocr.md, "Text-presence gate" */
    c->ocr_line_fix = VJO_LINE_FIX_DEFAULT;   /* docs/vita-vn-ocr.md, "Line finder fixes" */
    c->hook_sites = 1; /* the table sites with the CALL source; off: the CALL source alone */
    c->ocr_head_gate = VJO_HEAD_GATE_DEFAULT; /* docs/vita-vn-ocr.md, "Head gate" and "Two-level head" */
    c->hook_trust = 1; /* a trusted hook answers without OCR; off: Rubi 0.9.7's selection (docs/native-hooks.md) */
    c->ocr_vertical = VJO_VERTICAL_DEFAULT; /* docs/vita-vn-ocr.md, "Vertical text" */
    c->ocr_pause = 0; /* the game is not paused for its frame unless ocr_pause = on (docs/game-pause.md) */
}

const char *vjo_config_default_text(void)
{
    return "; Rubi settings. Changes apply the next time the overlay opens.\n"
           "\n"
           "; Dictionary used for word lookups: local (the files below)\n"
           "dictionary = local\n"
           "\n"
           "; Local Yomitan dictionaries: convert on a computer, then copy .vjdict files here.\n"
           "local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n"
           "; The bundled word/grammar/kanji and PSP/Vita character-name dictionaries.\n"
           "; Up to 8 filenames, separated by commas, in priority order.\n"
           "local_dictionaries = plaintext.vjdict,vndb-names.vjdict\n"
           "; Dictionaries shown per selected word, 1-8: the best match first, then the same\n"
           "; word in each other dictionary, in the order above. L/R tap pages; one is in memory.\n"
           "local_dictionary_results = 2\n"
           "\n"
           "; Drop OCR lines without Japanese characters, and lines that are only a text\n"
           "; box's border read as long vowels and dashes: none | lines\n"
           "non_japanese_filter = lines\n"
           "\n"
           "; Left/right character lookup skips Latin text, numbers and punctuation: on | off.\n"
           "; Select Japanese kana and Han characters; this does not tokenize words.\n"
           "japanese_only_selection = off\n"
           "\n"
           "; Highlight the dictionary match or only the cursor character: word | character.\n"
           "; Word skips the matched source span; character steps one character.\n"
           "highlight_mode = word\n"
           "\n"
           "; The left stick moves the cursor through the text, faster while held; the word\n"
           "; there is looked up once the cursor has rested this long (ms, 0 = as soon as it\n"
           "; stops). The d-pad and taps look up at once.\n"
           "stick_lookup_ms = 200\n"
           "\n"
           "; Show each word's frequency rank after its reading: on | off. #1 is the most\n"
           "; frequent word; unranked: not in the frequency list its dictionary carries.\n"
           "frequency_badge = on\n"
           "\n"
           "; The overlay on the game's own text (testing): the word at the cursor is marked\n"
           "; where the game shows it, a tap on the game's text looks a word up, and the\n"
           "; popup sits beside the word: on | off (docs/overlay-on-text.md)\n"
           "overlay_on_text = off\n"
           "\n"
           "; Text size, 8-40: Japanese (sentence, words, readings) and English (meanings, messages)\n"
           "font_size_ja = 18\n"
           "font_size_en = 14\n"
           "\n"
           "; Opens/closes the overlay: a button combination, or a double tap on the rear\n"
           "; touchpad (buttons are hidden from the game; rear taps are not):\n"
           "; l+r | select | start | select+l | select+r | rear_double_tap\n"
           "toggle_button = l+r\n"
           "\n"
           "; Turns subtitles on/off: the recognized text in a strip at the top of the\n"
           "; screen while the game runs. Same values as toggle_button, but not the same one.\n"
           "; When one is part of the other (select and select+r), the shorter one acts\n"
           "; on release.\n"
           "subtitle_button = select+r\n"
           "\n"
           "; Where the text comes from (the Rubi app's Text source), from a game's next start:\n"
           "; auto (try native hooks in every enabled Vita game, with OCR fallback) |\n"
           "; hooks (legacy alias for auto) | ocr (screenshots only, no hooks)\n"
           "text_source = auto\n"
           "; Text hooks at table sites (Rubi's and Luna's entries): on | off (off: only\n"
           "; the string-copy hooks, to tell the two apart in a test)\n"
           "hook_sites = on\n"
           "; Trusted hooks replace OCR (the Rubi app's card of that name): on | off.\n"
           "; On: a Luna or Rubi verified site, or a string copy after four OCR\n"
           "; agreements, answers with no OCR at all; off: Rubi 0.9.7's checks\n"
           "hook_trust = on\n"
           "\n"
           "; OCR: vita-vn-ocr, offline on the Vita (docs/vita-vn-ocr.md)\n"
           "ocr_model_dir = ux0:data/VitaJPOverlay/ocr\n"
           "; vocr model size (the Rubi app's Model size): small (fastest) | normal\n"
           "; (the default, balanced) | large (most accurate)\n"
           "vocr_tier = normal\n"
           "; vocr: read text once still this long (ms, 0 = off), waiting at most the max,\n"
           "; or the press max when the open overlay waits\n"
           "ocr_settle_ms = 150\n"
           "ocr_settle_max_ms = 2000\n"
           "ocr_settle_press_ms = 1000\n"
           "; vocr: skip found lines with no ink (an even area, a smooth gradient): on | off\n"
           "; (off: it finds next to none at native resolution; see docs/vita-vn-ocr.md)\n"
           "ocr_skip_blank_lines = off\n"
           "; vocr: largest LM: 4000k | 2000k | 1000k | 500k | off\n"
           "ocr_lm = 1000k\n"
           "; vocr: re-read doubtful lines with the next model size up within (ms, 0 = off)\n"
           "ocr_cascade_ms = 2000\n"
           "; vocr: refine lines: psp | on | off\n"
           "ocr_refine_lines = psp\n"
           "; vocr: no lines where a text-presence gate sees no text (a hidden text box,\n"
           "; a CG screen): on | off\n"
           "ocr_text_gate = " VJO_TEXT_GATE_DEFAULT_TEXT "\n"
           "; vocr: skip the recognizer's output layer on blank frames (gate) and compute\n"
           "; its rare characters only where needed (two_level): on (both) | gate |\n"
           "; two_level | off\n"
           "ocr_head_gate = " VJO_HEAD_GATE_DEFAULT_TEXT "\n"
           "; vocr: line finder fixes: on | junk | all | off\n"
           "ocr_line_fix = " VJO_LINE_FIX_DEFAULT_TEXT "\n"
           "; vocr: lines read at once (1-3), 444 MHz while reading (on | off: the Rubi\n"
           "; app's Speed boost while reading)\n"
           "ocr_threads = 3\n"
           "ocr_boost_clock = on\n"
           "; vocr: pause the game a moment for each reading (the Rubi app's Pause the game to\n"
           "; read): the text box is copied from one frame, so an animated icon cannot change\n"
           "; it while it is read: on | off\n"
           "ocr_pause = off\n"
           "; vocr: vertical text (manga, vertical dialogue): auto reads the region's columns,\n"
           "; right to left, with the vertical model where its rows look like vertical text |\n"
           "; on reads columns in every game | off reads rows\n"
           "ocr_vertical = " VJO_VERTICAL_DEFAULT_TEXT "\n"
           "\n"
           "; The Rubi app's Read text. auto: recognize text in the background when the\n"
           "; region settles, so the popup is ready when you press; on_press: recognize only\n"
           "; when the overlay is opened (docs/auto-vs-press.md)\n"
           "ocr_mode = auto\n"
           "\n"
           "; Debugging: write ux0:data/VitaJPOverlay/log.txt (max 1 MB + one rotated file): on | off\n"
           "log_file = on\n"
           "\n"
           "; Reading time in the overlay's statistics (Triangle), from the Vita's next start:\n"
           "; adaptive (each line at most 2.5 times what your pace takes for it, as\n"
           "; GameSentenceMiner counts) | 15s (only pauses of 15 s or less, Rubi 0.9.7's rule)\n"
           "reading_time = adaptive\n"
           "\n"
           "; ---- Anki (optional): the Rubi app's Anki page turns it on and exports the cards ----\n"
           "; on: enter (× or ○, as the Vita's settings assign it) in the overlay saves the\n"
           "; shown word as a card in a queue on the memory card; the Rubi app exports the\n"
           "; queue as a Lapis .apkg file for Anki (docs/anki.md). off: enter does nothing.\n"
           "anki_mining = off\n"
           "; on: each card keeps a JPEG of the game's screen (about 60 KB) in its Picture\n"
           "anki_screenshot = on\n"
           "; on: without the black bars around the game's picture (a PSP game shown smaller\n"
           "; than the screen, a 4:3 picture): on | off\n"
           "anki_screenshot_trim_bars = on\n";
}

/* A warning names a setting (its key as typed, at most VJO_CONFIG_WARN_KEY bytes of it for an unknown one) or a
 * line's number, never a value: a line's value can hold a key or an address, and a warning is shown in the open
 * overlay and written to log.txt (audit-pr180-b N3). */
#define VJO_CONFIG_WARN_KEY 32 /* the bytes of an unknown setting's key a warning names */

static void warn(VjoConfig *c, const char *fmt, ...)
{
    va_list ap;
    if (c->n_warnings >= VJO_CONFIG_MAX_WARNINGS)
        return;
    va_start(ap, fmt);
    vjo_vsnprintf(c->warnings[c->n_warnings++], sizeof(c->warnings[0]), fmt, ap);
    va_end(ap);
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

/* The name a warning gives a key as typed: up to its first space, at most VJO_CONFIG_WARN_KEY bytes. A known
 * setting's name is shorter and has no space; an unknown key is whatever came before the '=' (a pasted token, a
 * URL), so only that much of it is shown (audit-pr185 B1). */
static const char *warn_name(char name[VJO_CONFIG_WARN_KEY + 1], const char *key)
{
    size_t k = 0;
    while (key[k] && k < VJO_CONFIG_WARN_KEY && !is_space(key[k])) {
        name[k] = key[k];
        k++;
    }
    name[k] = '\0';
    return name;
}

size_t vjo_config_line_start(const char *line, size_t n)
{
    size_t s = 0;
    for (;;) {
        if (s < n && is_space(line[s]))
            s++;
        else if (n - s >= 3 && (unsigned char)line[s] == 0xEF && (unsigned char)line[s + 1] == 0xBB &&
                 (unsigned char)line[s + 2] == 0xBF)
            s += 3;
        else
            return s;
    }
}

static int parse_int(const char *s, int *out)
{
    long v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return -1;
        v = v * 10 + (*s - '0');
    }
    *out = (int)v;
    return 0;
}

/* A dictionary value of a removed service: its name, for the config
 * warning, or NULL: jpdb.io and jiten.moe, the online dictionaries, and the
 * Hachidori relay on a computer. Such a file loads as local; the Rubi
 * app's next save (vjo_config_removed_line) and the installer
 * (tools/migrate_config.py REPLACED_VALUES) write local. */
static const char *removed_dictionary(const char *v)
{
    return vjo_ieq(v, "jpdb") ? "jpdb.io" : vjo_ieq(v, "jiten") ? "jiten.moe"
         : vjo_ieq(v, "hachidori") ? "the Hachidori relay" : NULL;
}

/* The API keys of the removed online dictionaries (api_key: jpdb's older
 * name) open the n bytes at s, in any case: the key's line, or a typo of
 * it, whose text may hold the key (whatever follows the name: "=", ":", a
 * space, the key itself). No setting's name starts with one of them. Such a
 * line is ignored without a warning, so no secret is ever shown or logged;
 * the Rubi app's next save and the installer (tools/migrate_config.py
 * SECRETS) delete it. */
static int removed_secret(const char *s, size_t n)
{
    static const char *const keys[] VJO_DATA_TABLE(config_keys_1) = {"jpdb_api_key", "jiten_api_key", "api_key"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (n >= strlen(keys[i]) && vjo_ieq_prefix(s, keys[i]))
            return 1;
    return 0;
}

static int removed_anki_name(const char *s, size_t n);

int vjo_config_removed_line(const char *line, size_t n, const char **replacement)
{
    size_t b = vjo_config_line_start(line, n), e, v;
    *replacement = NULL;
    if (removed_secret(line + b, n - b))
        return VJO_LINE_DROP;
    e = b;
    /* the first word ends at '=', a space or the line's end (a name alone on its line too) */
    while (e < n && line[e] != '=' && line[e] != '\n' && !is_space(line[e])) e++;
    /* a removed Anki setting goes too (anki_audio_url's URL can hold a key) */
    if (removed_anki_name(line + b, e - b))
        return VJO_LINE_DROP;
    v = e;
    while (v < n && is_space(line[v])) v++;
    /* the engine choice: VOCR is the only engine; the UDP log went with the network (core/config.h) */
    if (((e - b == 11 && vjo_ieq_prefix(line + b, "ocr_backend")) ||
         (e - b == 8 && vjo_ieq_prefix(line + b, "log_host"))) && v < n && line[v] == '=')
        return VJO_LINE_DROP_SETTING;
    if (e - b != 10 || !vjo_ieq_prefix(line + b, "dictionary") || v >= n || line[v] != '=')
        return VJO_LINE_KEEP;
    for (v++; v < n && is_space(line[v]); v++) {}
    /* the value up to an inline comment, a space or the line's end */
    for (e = v; e < n && line[e] != '\n' && line[e] != ';' && line[e] != '#' && !is_space(line[e]); e++) {}
    if (e - v < 16) { /* longer than any removed dictionary's value */
        char value[16];
        memcpy(value, line + v, e - v);
        value[e - v] = '\0';
        if (removed_dictionary(value)) {
            *replacement = "dictionary = local\n";
            return VJO_LINE_REPLACE;
        }
    }
    return VJO_LINE_KEEP;
}

/* An ocr_backend value of a removed engine: why it no longer runs (the config
 * warning), or NULL. The setting itself is gone (VOCR is the only engine): the
 * Rubi app's next save and the installer (tools/migrate_config.py) drop the line. */
static const char *removed_engine(const char *v)
{
    static const char *const removed[][2] VJO_DATA_TABLE(config_removed_1) = {
        {"lens", "Google Lens was removed (the overlay is offline)"}, /* the upstream's engine, removed 2026-10-15 */
        {"vocr2", "VOCR2 was removed"},  /* the experimental trial, v0.7.12 to v0.8.0-test.1 */
        {"meiki", "Meiki was removed"},  /* the manual single-line trial, v0.7.6 to v0.7.12 */
        {"ncnn", "ncnn runs only in host tools"}, /* vjo-ocr --backend ncnn; never on the Vita */
    };
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++)
        if (vjo_ieq(v, removed[i][0]))
            return removed[i][1];
    return NULL;
}

/* An older build's vocr_model: a plain file name ending in .vocr, as those
 * builds accepted it (now only its tier is read from it). */
static int valid_vocr_model(const char *v)
{
    size_t n = strlen(v);
    if (n <= 5 || n >= 64 || strcmp(v + n - 5, ".vocr"))
        return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = v[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              ch == '.' || ch == '_' || ch == '-'))
            return 0;
    }
    return 1;
}

/* ---- the model tiers (include/vjo_vocr_sizes.h) ---- */
static const char *const tier_ids[VJO_VOCR_TIERS] VJO_DATA_TABLE(config_tier_ids_1) = {"small", "normal", "large"};
static const char *const tier_models[VJO_VOCR_TIERS] VJO_DATA_TABLE(config_tier_models_1) = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME,
                                                        VJO_VOCR_TIER3_NAME};

const char *vjo_vocr_tier_id(int tier)
{
    return tier >= 1 && tier <= (int)VJO_VOCR_TIERS ? tier_ids[tier - 1] : NULL;
}

int vjo_vocr_tier_parse(const char *id)
{
    for (int t = 1; id && t <= (int)VJO_VOCR_TIERS; t++)
        if (vjo_ieq(id, tier_ids[t - 1]))
            return t;
    return 0;
}

const char *vjo_vocr_tier_model(int tier)
{
    return tier >= 1 && tier <= (int)VJO_VOCR_TIERS ? tier_models[tier - 1] : NULL;
}

/* An older build's vocr_model by the shape its name carries: every release
 * named its recognizers by shape (FL10_w8.vocr, VOCR3_H15_w8.vocr, the VOCR2
 * trial's VOCR2_FL10_w8.vocr, F20_w8.vocr, VOCR4_W20_w8.vocr). Builds since API 21
 * write vocr_tier, never vocr_model, so no VOCR5 file name reaches this. */
int vjo_vocr_tier_of_model(const char *name)
{
    if (!name) return 0;
    if (strstr(name, "FL10")) return VJO_VOCR_TIER_SMALL;
    if (strstr(name, "H15")) return VJO_VOCR_TIER_NORMAL;
    if (strstr(name, "W20") || strstr(name, "F20")) return VJO_VOCR_TIER_LARGE;
    return 0;
}

/* Plain string settings. Paths and the audio URL may contain ';' and '#'
 * (raw: no inline comment). */
typedef struct {
    const char *key;
    size_t offset, size;
    int required; /* an empty value keeps the default, with a warning */
    int raw;
    int (*valid)(const char *v); /* NULL = anything; an invalid value turns the setting off ("") */
    const char *expected;        /* the warning's description of a valid value (a warning never quotes the value) */
} StrSetting;

#define STR_SETTING(name, required, raw) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), required, raw, NULL, NULL}
#define CHECKED_SETTING(name, raw, valid, expected) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), 0, raw, valid, expected}
static const StrSetting str_settings[] VJO_DATA_TABLE(config_str_settings) = {
    STR_SETTING(local_dictionary_dir, 1, 1),
    STR_SETTING(ocr_model_dir, 1, 1),
    CHECKED_SETTING(local_dictionaries, 0, vjo_local_names_valid, "1-8 relative .vjdict filenames separated by commas"),
};
#define N_STR_SETTINGS ((int)(sizeof(str_settings) / sizeof(str_settings[0])))

static const StrSetting *find_str_setting(const char *key)
{
    for (int i = 0; i < N_STR_SETTINGS; i++)
        if (vjo_ieq(key, str_settings[i].key))
            return &str_settings[i];
    return NULL;
}

/* Removed Anki settings (any case), ignored without a warning: the field
 * mapping a fixed note type replaced (anki_deck, anki_note_type, anki_tags,
 * anki_field_*), and AnkiConnect's address and the word audio source
 * (anki_host, anki_audio_url), gone with AnkiConnect: the Rubi app exports
 * the cards as an .apkg. The installer (tools/migrate_config.py) removes them
 * from config.ini and says so, and the Rubi app's next save drops their
 * lines (vjo_config_removed_line). The n bytes at s are the name: a line's
 * first word. */
static int removed_anki_name(const char *s, size_t n)
{
    static const char *const names[] VJO_DATA_TABLE(config_names_1) = {"anki_deck", "anki_note_type", "anki_tags", "anki_host", "anki_audio_url"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (n == strlen(names[i]) && vjo_ieq_prefix(s, names[i]))
            return 1;
    return n >= 11 && vjo_ieq_prefix(s, "anki_field_");
}

int vjo_config_removed_anki_key(const char *key)
{
    return removed_anki_name(key, strlen(key));
}

static int keeps_comment_chars(const char *key)
{
    const StrSetting *ss = find_str_setting(key);
    return ss && ss->raw;
}

static void set_str(VjoConfig *c, const char *key, const char *val, char *dst, size_t size)
{
    if (strlen(val) >= size)
        warn(c, "%s: value too long", key);
    else
        memcpy(dst, val, strlen(val) + 1);
}

static void set_font(VjoConfig *c, const char *key, const char *val, int *dst)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= 8 && v <= 40)
        *dst = v;
    else
        warn(c, "%s: invalid value (8-40)", key);
}

/* A time in ms within [lo, hi]. */
/* One of names (NULL-ended), stored as its index. */
static void set_choice(VjoConfig *c, const char *key, const char *val, int *dst, const char *const *names)
{
    for (int i = 0; names[i]; i++)
        if (vjo_ieq(val, names[i])) {
            *dst = i;
            return;
        }
    warn(c, "%s: invalid value", key);
}

static void set_ms(VjoConfig *c, const char *key, const char *val, int *dst, int lo, int hi)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= lo && v <= hi)
        *dst = v;
    else
        warn(c, "%s: invalid value (ms, out of range)", key);
}

static void set_trigger(VjoConfig *c, const char *key, const char *val, int *dst)
{
    for (int i = 0; i < VJO_TRIGGER_COUNT; i++) {
        if (vjo_ieq(val, trigger_names[i])) {
            *dst = i;
            return;
        }
    }
    warn(c, "%s: invalid value", key);
}

/* API 42: ocr_head_gate's name and its values' (core/config.h VJO_HEAD_*), in the data segment (core/port.h
 * VJO_DATA_TABLE): the normal Shell's code segment had 100 B of its last page left, and these 29 B with the key's
 * code took it 12 B past (docs/memory-ledger.md, "Head gate and two-level head"). */
static const char head_key[] VJO_DATA_TABLE(config_head_key) = "ocr_head_gate";
static const char head_gate_name[] VJO_DATA_TABLE(config_head_gate_name) = "gate";
static const char head_two_name[] VJO_DATA_TABLE(config_head_two_name) = "two_level";
/* API 47's key, in the data segment: the normal Shell's code room (docs/memory-ledger.md, "Vertical text"). */
static const char vertical_key[] VJO_DATA_TABLE(config_vertical_key) = "ocr_vertical";

static void set_kv(VjoConfig *c, const char *key, const char *val)
{
    const StrSetting *ss;
    if (vjo_ieq(key, "dictionary")) {
        /* the local dictionaries are the only ones: c->dictionary stays VJO_DICT_LOCAL */
        const char *removed = removed_dictionary(val);
        if (removed)
            warn(c, "%s: %s was removed; using local", key, removed);
        else if (!vjo_ieq(val, "local"))
            warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "log_host")) {
        /* the UDP log was removed with the network (core/config.h): said once when one was set */
        if (val[0])
            warn(c, "%s: the UDP log was removed (the overlay is offline); log.txt stays", key);
    } else if (vjo_ieq(key, "frequency_filter") || vjo_ieq(key, "font_size") || vjo_ieq(key, "hw_jpeg") ||
               vjo_ieq(key, "combo_delay_ms") || vjo_ieq(key, "hachidori_host") || vjo_config_removed_anki_key(key) ||
               removed_secret(key, strlen(key))) {
        /* removed settings (tools/migrate_config.py REMOVED): no warning (the relay's
         * hachidori_host too: dictionary = hachidori's warning is the relay's one line) */
    } else if (vjo_ieq(key, "non_japanese_filter")) {
        if (vjo_ieq(val, "lines"))
            c->non_japanese_filter = VJO_FILTER_LINES;
        else if (vjo_ieq(val, "none"))
            c->non_japanese_filter = VJO_FILTER_NONE;
        else
            warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "japanese_only_selection")) {
        if (vjo_ieq(val, "on")) c->japanese_only_selection = 1;
        else if (vjo_ieq(val, "off")) c->japanese_only_selection = 0;
        else warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "highlight_mode")) {
        if (vjo_ieq(val, "word")) c->highlight_mode = VJO_HIGHLIGHT_WORD;
        else if (vjo_ieq(val, "character")) c->highlight_mode = VJO_HIGHLIGHT_CHARACTER;
        else warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "overlay_on_text")) {
        static const char *const off_on[] VJO_DATA_TABLE(config_off_on_4) = {"off", "on", NULL};
        set_choice(c, key, val, &c->overlay_on_text, off_on);
    } else if (vjo_ieq(key, "frequency_badge")) {
        if (vjo_ieq(val, "on")) c->frequency_badge = 1;
        else if (vjo_ieq(val, "off")) c->frequency_badge = 0;
        else warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "local_dictionary_results")) {
        int v;
        if (parse_int(val, &v) == 0 && v >= 1 && v <= VJO_LOCAL_MAX_DICTS)
            c->local_dictionary_results = v;
        else
            warn(c, "%s: invalid value (1-8)", key);
    } else if (vjo_ieq(key, "font_size_ja")) {
        set_font(c, key, val, &c->font_size_ja);
    } else if (vjo_ieq(key, "font_size_en")) {
        set_font(c, key, val, &c->font_size_en);
    } else if (vjo_ieq(key, "toggle_button")) {
        set_trigger(c, key, val, &c->toggle_button);
    } else if (vjo_ieq(key, "subtitle_button")) {
        set_trigger(c, key, val, &c->subtitle_button);
    } else if (vjo_ieq(key, "text_source")) {
        static const char *const sources[] VJO_DATA_TABLE(config_sources_1) = {"ocr", "auto", "hooks", NULL}; /* VJO_SOURCE_*'s order */
        c->text_source = VJO_SOURCE_AUTO; /* an unknown value: auto, with the warning */
        set_choice(c, key, val, &c->text_source, sources);
    } else if (vjo_ieq(key, "hook_sites") || vjo_ieq(key, "hook_trust")) {
        static const char *const off_on[] VJO_DATA_TABLE(config_off_on_1) = {"off", "on", NULL};
        /* hook_sites or (API 43) hook_trust: their sixth letters differ, in either case */
        int *dst = (key[5] | 0x20) == 's' ? &c->hook_sites : &c->hook_trust;
        *dst = 1; /* an unknown value: on, with the warning */
        set_choice(c, key, val, dst, off_on);
    } else if (vjo_ieq(key, "reading_time")) {
        static const char *const rules[] VJO_DATA_TABLE(config_reading_time_1) = {"adaptive", "15s", NULL}; /* VJO_READING_* */
        c->reading_time = 0; /* an unknown value: adaptive, with the warning */
        set_choice(c, key, val, &c->reading_time, rules);
    } else if (vjo_ieq(key, "ocr_backend")) {
        /* The removed engine choice (core/config.h): vocr is the only engine. */
        const char *removed = removed_engine(val);
        if (removed)
            warn(c, "%s: %s; using vocr", key, removed);
        else if (!vjo_ieq(val, "vocr"))
            warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "vocr_tier")) {
        int t = vjo_vocr_tier_parse(val);
        if (t) {
            c->vocr_tier = t;
            c->vocr_tier_explicit = 1;
        } else
            warn(c, "%s: invalid value (small, normal or large)", key);
    } else if (vjo_ieq(key, "vocr_model")) {
        /* An older build's model choice: its tier, unless a vocr_tier line says otherwise. */
        int t = valid_vocr_model(val) ? vjo_vocr_tier_of_model(val) : 0;
        if (!t)
            warn(c, "%s: not a model of this build; using vocr_tier", key);
        else if (!c->vocr_tier_explicit)
            c->vocr_tier = t;
    } else if (vjo_ieq(key, "vocr_fallback_model")) {
        /* removed: a job falls back to the next smaller tier (tools/migrate_config.py REMOVED) */
    } else if (vjo_ieq(key, "ocr_settle_ms")) {
        set_ms(c, key, val, &c->ocr_settle_ms, 0, 1000);
    } else if (vjo_ieq(key, "ocr_settle_max_ms")) {
        set_ms(c, key, val, &c->ocr_settle_max_ms, 100, 10000);
    } else if (vjo_ieq(key, "ocr_settle_press_ms")) {
        int keep = c->ocr_settle_press_ms; /* 0, or 100..10000: under 25 ms the request's 50 ms field is 0, the 2 s default */
        set_ms(c, key, val, &c->ocr_settle_press_ms, 0, 10000);
        if (c->ocr_settle_press_ms && c->ocr_settle_press_ms < 100) {
            c->ocr_settle_press_ms = keep;
            warn(c, "%s: invalid value (ms, out of range)", key);
        }
    } else if (vjo_ieq(key, "ocr_cascade_ms")) {
        set_ms(c, key, val, &c->ocr_cascade_ms, 0, 3750);
    } else if (vjo_ieq(key, "stick_lookup_ms")) {
        set_ms(c, key, val, &c->stick_lookup_ms, 0, 1000);
    } else if (vjo_ieq(key, "ocr_lm")) {
        static const char *const tiers[] VJO_DATA_TABLE(config_tiers_1) = {"off", "500k", "1000k", "2000k", "4000k", NULL};
        set_choice(c, key, val, &c->ocr_lm, tiers);
    } else if (vjo_ieq(key, "ocr_refine_lines")) {
        static const char *const modes[] VJO_DATA_TABLE(config_modes_1) = {"off", "on", "psp", NULL};
        set_choice(c, key, val, &c->ocr_refine_lines, modes);
    } else if (vjo_ieq(key, "ocr_text_gate") || vjo_ieq(key, "ocr_pause")) {
        static const char *const off_on[] VJO_DATA_TABLE(config_off_on_2) = {"off", "on", NULL};
        /* ocr_text_gate or (API 51) ocr_pause: their fifth letters differ, in either case */
        set_choice(c, key, val, (key[4] | 0x20) == 'p' ? &c->ocr_pause : &c->ocr_text_gate, off_on);
    } else if (vjo_ieq(key, head_key)) {
        /* each value's index is its VJO_HEAD_* bits */
        static const char *const heads[] VJO_DATA_TABLE(config_heads_1) = {"off", head_gate_name, head_two_name, "on",
                                                                             NULL};
        set_choice(c, key, val, &c->ocr_head_gate, heads);
    } else if (vjo_ieq(key, "ocr_line_fix")) {
        static const char *const fixes[] VJO_DATA_TABLE(config_line_fix_1) = {"off", "on", "junk", "all", NULL};
        set_choice(c, key, val, &c->ocr_line_fix, fixes);
    } else if (vjo_ieq(key, "ocr_skip_blank_lines")) {
        if (vjo_ieq(val, "on")) c->ocr_skip_blank_lines = 1;
        else if (vjo_ieq(val, "off")) c->ocr_skip_blank_lines = 0;
        else warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "ocr_threads")) {
        static const char *const threads[] VJO_DATA_TABLE(config_threads_1) = {"1", "2", "3", NULL}; /* stored as the helpers: 0-2 */
        set_choice(c, key, val, &c->ocr_helpers, threads);
    } else if (vjo_ieq(key, "ocr_boost_clock")) {
        static const char *const off_on[] VJO_DATA_TABLE(config_off_on_3) = {"off", "on", NULL};
        set_choice(c, key, val, &c->ocr_boost_clock, off_on);
    } else if (vjo_ieq(key, vertical_key)) {
        static const char *const modes[] VJO_DATA_TABLE(config_vertical_1) = {"off", "on", "auto", NULL}; /* VJO_VERTICAL_* */
        set_choice(c, key, val, &c->ocr_vertical, modes);
    } else if (vjo_ieq(key, "ocr_mode")) {
        if (vjo_ieq(val, "auto"))
            c->ocr_mode = VJO_OCR_AUTO;
        else if (vjo_ieq(val, "on_press"))
            c->ocr_mode = VJO_OCR_ON_PRESS;
        else
            warn(c, "%s: invalid value", key);
    } else if ((ss = find_str_setting(key)) != NULL) {
        char *dst = (char *)c + ss->offset;
        if (!*val && ss->required)
            warn(c, "%s: empty (ignored)", key);
        else
            set_str(c, key, val, dst, ss->size);
        if (ss->valid && !ss->valid(dst)) {
            warn(c, "%s: invalid value (%s)", key, ss->expected);
            dst[0] = '\0';
        }
    } else if (vjo_ieq(key, "log_file")) {
        if (vjo_ieq(val, "on"))
            c->log_file = 1;
        else if (vjo_ieq(val, "off"))
            c->log_file = 0;
        else
            warn(c, "%s: invalid value", key);
    } else if (vjo_ieq(key, "anki_mining") || vjo_ieq(key, "anki_screenshot") ||
               vjo_ieq(key, "anki_screenshot_trim_bars")) {
        int *dst = vjo_ieq(key, "anki_mining") ? &c->anki_mining
                   : vjo_ieq(key, "anki_screenshot") ? &c->anki_screenshot : &c->anki_screenshot_trim_bars;
        if (vjo_ieq(val, "on"))
            *dst = 1;
        else if (vjo_ieq(val, "off"))
            *dst = 0;
        else
            warn(c, "%s: invalid value", key);
    } else {
        char name[VJO_CONFIG_WARN_KEY + 1];
        warn(c, "unknown setting '%s'", warn_name(name, key));
    }
}

#define DROPPED_PREFERENCE (-2) /* a line the app's save removes (preference_key) */

static int preference_key(const char *line, size_t n)
{
    size_t begin = vjo_config_line_start(line, n), end;
    char key[40];
    end = begin;
    while (end < n && line[end] != '=' && !is_space(line[end])) end++;
    size_t key_len = end - begin;
    if (!key_len || key_len >= sizeof(key)) return -1;
    while (end < n && is_space(line[end])) end++;
    if (end == n || line[end] != '=') return -1;
    memcpy(key, line + begin, key_len);
    key[key_len] = 0;
    return vjo_ieq(key, "japanese_only_selection") ? 0 :
           vjo_ieq(key, "highlight_mode") ? 1 :
           vjo_ieq(key, "local_dictionary_results") ? 2 :
           vjo_ieq(key, "vocr_tier") ? 3 :
           /* an older build's model choice, which vocr_tier replaces: dropped */
           vjo_ieq(key, "vocr_model") || vjo_ieq(key, "vocr_fallback_model") ? DROPPED_PREFERENCE : -1;
}

#define OWNED_PREFERENCES 4

int vjo_config_patch_preferences(const char *text, size_t len, int vocr_tier,
                                 int japanese_only, int highlight_mode,
                                 int dictionary_results, char *out, size_t cap)
{
    const char *values[OWNED_PREFERENCES];
    char results_line[40], tier_line[32];
    size_t i = 0, used = 0;
    unsigned seen = 0;
    if ((!text && len) || len > VJO_CONFIG_TEXT_MAX || !out || !cap || !vjo_vocr_tier_id(vocr_tier) ||
        (japanese_only != 0 && japanese_only != 1) ||
        (highlight_mode != VJO_HIGHLIGHT_WORD && highlight_mode != VJO_HIGHLIGHT_CHARACTER) ||
        dictionary_results < 1 || dictionary_results > VJO_LOCAL_MAX_DICTS ||
        (len && memchr(text, 0, len)))
        return -1;
    values[0] = japanese_only ? "japanese_only_selection = on\n" : "japanese_only_selection = off\n";
    values[1] = highlight_mode == VJO_HIGHLIGHT_WORD
        ? "highlight_mode = word\n" : "highlight_mode = character\n";
    vjo_snprintf(results_line, sizeof(results_line), "local_dictionary_results = %d\n", dictionary_results);
    values[2] = results_line;
    vjo_snprintf(tier_line, sizeof(tier_line), "vocr_tier = %s\n", vjo_vocr_tier_id(vocr_tier));
    values[3] = tier_line;
    /* Keep a BOM even when the first line is an owned preference. */
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) {
        if (cap <= 3) return -1;
        memcpy(out, text, 3); used = i = 3;
    }
    while (i < len) {
        size_t begin = i;
        while (i < len && text[i] != '\n') i++;
        int key = preference_key(text + begin, i - begin);
        if (i < len) i++;
        const char *line = text + begin, *replacement;
        size_t bytes = i - begin;
        if (key == DROPPED_PREFERENCE) continue;
        if (key >= 0) {
            if (seen & (1u << key)) continue;
            seen |= 1u << key;
            line = values[key]; bytes = strlen(line);
        } else {
            int removed = vjo_config_removed_line(line, bytes, &replacement);
            if (removed == VJO_LINE_DROP || removed == VJO_LINE_DROP_SETTING) continue;
            if (removed == VJO_LINE_REPLACE) { line = replacement; bytes = strlen(line); }
        }
        if (bytes >= cap - used) return -1;
        memcpy(out + used, line, bytes); used += bytes;
    }
    for (int key = 0; key < OWNED_PREFERENCES; key++) {
        if (seen & (1u << key)) continue;
        if (used && out[used - 1] != '\n') {
            if (used + 1 >= cap) return -1;
            out[used++] = '\n';
        }
        size_t bytes = strlen(values[key]);
        if (bytes >= cap - used) return -1;
        memcpy(out + used, values[key], bytes); used += bytes;
    }
    if (used > VJO_CONFIG_TEXT_MAX) return -1;
    out[used] = 0;
    return (int)used;
}

void vjo_config_parse(VjoConfig *c, const char *text, size_t len)
{
    size_t i = 0;
    unsigned line_no = 0; /* a line without '=' is named by its number: its text could be a key */
    while (i < len) {
        char line[1024], key[64], val[sizeof(((VjoConfig *)0)->local_dictionaries)]; /* the longest value */
        size_t n = 0, s, e, eq;
        int cut = 0;
        line_no++;
        while (i < len && text[i] != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = text[i];
            else
                cut = 1;
            i++;
        }
        i++;
        line[n] = '\0';

        s = vjo_config_line_start(line, n);
        if (s == n || line[s] == ';' || line[s] == '#')
            continue;
        {
            /* A removed Anki setting's line, whatever follows its name ('=', a space, a
             * URL that can hold a key, a value too long), is ignored before anything of
             * it could reach a warning. */
            size_t w = s;
            while (w < n && line[w] != '=' && !is_space(line[w]))
                w++;
            if (removed_anki_name(line + s, w - s))
                continue;
        }
        {
            char *p = strchr(line + s, '=');
            if (!p) {
                /* named by its number, never quoted: its text may be a key (a removed API key's is silent) */
                if (!removed_secret(line + s, n - s))
                    warn(c, "ignored line %u (no '=')", line_no);
                continue;
            }
            eq = (size_t)(p - line);
        }
        e = eq;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (e - s >= sizeof(key))
            e = s + sizeof(key) - 1;
        memcpy(key, line + s, e - s);
        key[e - s] = '\0';

        /* Inline comment in the value: ';' or '#' preceded by whitespace
         * (then a cut only shortened the comment). */
        if (!keeps_comment_chars(key)) {
            for (e = eq + 1; e < n; e++) {
                if ((line[e] == ';' || line[e] == '#') && is_space(line[e - 1])) {
                    n = e;
                    cut = 0;
                    break;
                }
            }
        }
        s = eq + 1;
        while (s < n && is_space(line[s]))
            s++;
        e = n;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (cut || e - s >= sizeof(val)) {
            if (!removed_secret(key, strlen(key))) { /* a removed setting stays silent */
                char name[VJO_CONFIG_WARN_KEY + 1];
                warn(c, "%s: value too long", warn_name(name, key));
            }
            continue;
        }
        memcpy(val, line + s, e - s);
        val[e - s] = '\0';
        set_kv(c, key, val);
    }
    /* The toggle keeps its button (it was there first). */
    if (c->toggle_button == c->subtitle_button) {
        c->subtitle_button = c->toggle_button == VJO_TRIGGER_SELECT_R ? VJO_TRIGGER_SELECT_L : VJO_TRIGGER_SELECT_R;
        warn(c, "toggle_button and subtitle_button are both %s: subtitle_button is %s",
             trigger_names[c->toggle_button], trigger_names[c->subtitle_button]);
    }
}
