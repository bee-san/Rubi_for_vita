#include "config.h"

#include <stddef.h>
#include <string.h>

#include "../include/vjo_vocr_sizes.h"
#include "port.h"
#include "games.h"
#include "local_dict.h"
#include "textfilter.h"
#include "utf.h"

/* enum VjoTrigger's names, NULL-ended for set_choice */
static const char *const trigger_names[VJO_TRIGGER_COUNT + 1] VJO_DATA_TABLE(config_trigger_names_1) = {
    "select", "start", "l+r", "select+l", "select+r", "rear_double_tap", NULL,
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
    /* Normal (VOCR6-N) where the game's memory allows; a smaller tier otherwise. */
    c->vocr_tier = VJO_VOCR_TIER_DEFAULT;
    c->ocr_settle_ms = 150;
    c->ocr_settle_max_ms = 2000;
    c->ocr_skip_blank_lines = 0;
    c->ocr_lm = 2;
    c->ocr_cascade_ms = 2000;
    c->ocr_refine_lines = 2;
    c->ocr_helpers = 2; /* ocr_threads = 3 */
    c->ocr_boost_clock = 1; /* 444 MHz while a job recognizes; never lowers a clock (game_ocr/main.c) */
    c->stick_lookup_ms = 200; /* core/stick_cursor.h VJO_STICK_CURSOR_REST_MS */
    c->log_file = 1; /* log.txt is on unless log_file = off */
    c->anki_screenshot = 1; /* a card keeps a JPEG of the game's screen unless anki_screenshot = off */
    c->anki_screenshot_trim_bars = 1; /* its black bars trimmed unless anki_screenshot_trim_bars = off (docs/anki.md) */
    c->ocr_text_gate = VJO_TEXT_GATE_DEFAULT; /* docs/vita-vn-ocr.md, "Text-presence gate" */
    c->ocr_line_fix = VJO_LINE_FIX_DEFAULT;   /* docs/vita-vn-ocr.md, "Line finder fixes" */
    c->hook_sites = 1; /* the table sites with the CALL source; off: the CALL source alone */
    c->ocr_head_gate = VJO_HEAD_GATE_DEFAULT; /* docs/vita-vn-ocr.md, "Head gate" and "Two-level head" */
    c->ocr_finder = VJO_FINDER_DEFAULT; /* API 54: docs/vita-vn-ocr.md, "The spotter finder" */
    c->ocr_spotter_refine = VJO_SPOTTER_REFINE_DEFAULT;
    c->hook_trust = 1; /* a trusted hook answers without OCR; off: Rubi 0.9.7's selection (docs/native-hooks.md) */
    c->ocr_vertical = VJO_VERTICAL_DEFAULT; /* docs/vita-vn-ocr.md, "Vertical text" */
    c->ocr_wait_hint = 1; /* the open overlay says why it waits (docs/text-stability.md, #358) */
    c->names_first = VJO_NAMES_DEFAULT; /* docs/local-dictionaries.md, "Names and words" (#321) */
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
           "; A character's name spelled as the text (わたし) before a word spelled otherwise\n"
           "; (私): auto (the name second when the word is among the 1,023 most frequent) |\n"
           "; on (names first, as Rubi 0.9.8) | off (the word first)\n"
           "names_first = " VJO_NAMES_DEFAULT_TEXT "\n"
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
           "; vocr: read text once still this long (ms, 0 = off), waiting at most the max;\n"
           "; a press reads at once, its retries after a read that changed under every\n"
           "; try (an animated icon?) as above. ocr_settle_press_ms: older builds only\n"
           "ocr_settle_ms = 150\n"
           "ocr_settle_max_ms = 2000\n"
           "ocr_settle_press_ms = 1000\n"
           "; vocr: once two recognitions in a row found the text changing under every read\n"
           "; (an animated icon in the region?), the open overlay says why it is still\n"
           "; recognizing and what helps: on | off\n"
           "ocr_wait_hint = on\n"
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
           "; vocr: the line finder: spotter (vita-vn-ocr's text spotter) | grad (Rubi's;\n"
           "; also the spotter's fallback), and the spotter's boxes refined: on | off.\n"
           "; A game's own Line finder (its page in the Rubi app: ocr_line_fix.ini)\n"
           "; replaces ocr_finder and ocr_line_fix in that game\n"
           "ocr_finder = " VJO_FINDER_DEFAULT_TEXT "\n"
           "ocr_spotter_refine = " VJO_SPOTTER_REFINE_DEFAULT_TEXT "\n"
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
           "; on reads columns, but rows where they are lines of text | off reads rows\n"
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

/* A dictionary value of a removed service: jpdb.io and jiten.moe, the online dictionaries, and the Hachidori relay
 * on a computer. The parser takes it without a warning, as local (the local dictionaries are the only ones); the Rubi
 * app's next save (vjo_config_removed_line) and the installer (tools/migrate_config.py REPLACED_VALUES) write local. */
static int removed_dictionary(const char *v)
{
    return vjo_ieq(v, "jpdb") || vjo_ieq(v, "jiten") || vjo_ieq(v, "hachidori");
}

/* config.ini's removed settings, the one list (core/config.h vjo_config_removed): each entry is its kind, then its
 * name. tools/migrate_config.py keeps a copy (SECRETS, REMOVED, REMOVED_PREFIXES) that
 * tools/tests/test_migrate_config.py checks against this list. */
#define REMOVED(kind, name) kind name "\0"
static const char removed_keys[] =
    /* The API keys of the removed online dictionaries (jpdb.io, jiten.moe; api_key: jpdb's older name), each a
     * prefix: the key's line or a typo of it, whose text may hold the key (whatever follows the name: "=", ":", a
     * space, the key itself). No setting's name starts with one of them. Such a line is ignored without a warning, so
     * no secret is ever shown or logged; the Rubi app's next save and the installer delete it. */
    REMOVED("S", "jpdb_api_key") REMOVED("S", "jiten_api_key") REMOVED("S", "api_key")
    /* The removed Anki settings, ignored without a warning whatever follows their name: the field mapping a fixed
     * note type replaced (anki_deck, anki_note_type, anki_tags, anki_field_*), and AnkiConnect's address and the
     * word audio source (anki_host, anki_audio_url, whose URL can hold a key), gone with AnkiConnect: the Rubi app
     * exports the cards as an .apkg. The installer removes them and says so; the app's next save drops them. */
    REMOVED("a", "anki_deck") REMOVED("a", "anki_note_type") REMOVED("a", "anki_tags") REMOVED("a", "anki_host")
    REMOVED("a", "anki_audio_url") REMOVED("A", "anki_field_")
    /* The engine choice (VOCR is the only engine) and the UDP log's computer (the log went with the network): every
     * save drops their line */
    REMOVED("e", "ocr_backend") REMOVED("e", "log_host")
    /* Older settings (the relay's hachidori_host too): the saves keep their line, the install drops it */
    REMOVED("o", "frequency_filter") REMOVED("o", "font_size") REMOVED("o", "hw_jpeg") REMOVED("o", "combo_delay_ms")
    REMOVED("o", "hachidori_host")
    /* The model and its fallback, which one model size replaced (vocr_tier, API 21): the four-preference save drops
     * them; an older build's vocr_model still sets the size (set_kv) */
    REMOVED("m", "vocr_fallback_model") REMOVED("t", "vocr_model")
    /* The dictionary choice: the local dictionaries are the only ones; the parser still reads the line (another
     * value than local or a removed dictionary's warns), and a removed dictionary's line is saved as local */
    REMOVED("d", "dictionary");
#undef REMOVED

const char *vjo_config_removed(const char *s, size_t n)
{
    for (const char *k = removed_keys; *k; k += strlen(k) + 1) {
        size_t len = strlen(k + 1);
        if ((*k < 'a' ? n >= len : n == len) && vjo_ieq_prefix(s, k + 1))
            return k;
    }
    return NULL;
}

/* The kind (VJO_REMOVED_*) of the n bytes at s, a line's first word, among the removed settings, or 0. */
static int removed_kind(const char *s, size_t n)
{
    const char *r = vjo_config_removed(s, n);
    return r ? VJO_REMOVED_KIND(r) : 0;
}

int vjo_config_removed_line(const char *line, size_t n, const char **replacement)
{
    size_t b = vjo_config_line_start(line, n), e, v;
    int kind;
    *replacement = NULL;
    e = b;
    /* the first word ends at '=', a space or the line's end (a name alone on its line too) */
    while (e < n && line[e] != '=' && line[e] != '\n' && !is_space(line[e])) e++;
    kind = removed_kind(line + b, e - b);
    /* an API key's line, or a removed Anki setting's (anki_audio_url's URL can hold a key) */
    if (kind == VJO_REMOVED_SECRET || kind == VJO_REMOVED_ANKI)
        return VJO_LINE_DROP;
    v = e;
    while (v < n && is_space(line[v])) v++;
    /* the engine choice: VOCR is the only engine; the UDP log went with the network (core/config.h) */
    if (kind == VJO_REMOVED_SETTING && v < n && line[v] == '=')
        return VJO_LINE_DROP_SETTING;
    if (kind != VJO_REMOVED_DICTIONARY || v >= n || line[v] != '=')
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
 * write vocr_tier, never vocr_model, so no VOCR5 or VOCR6 file name reaches this. */
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

/* A time in ms within [lo, hi]. */
static void set_ms(VjoConfig *c, const char *key, const char *val, int *dst, int lo, int hi)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= lo && v <= hi)
        *dst = v;
    else
        warn(c, "%s: invalid value (ms, out of range)", key);
}

/* API 42: ocr_head_gate's name and its values' (core/config.h VJO_HEAD_*), in the data segment (core/port.h
 * VJO_DATA_TABLE): the normal Shell's code segment had 100 B of its last page left, and these 29 B with the key's
 * code took it 12 B past (docs/memory-ledger.md, "Head gate and two-level head"). */
static const char head_key[] VJO_DATA_TABLE(config_head_key) = "ocr_head_gate";
static const char head_gate_name[] VJO_DATA_TABLE(config_head_gate_name) = "gate";
static const char head_two_name[] VJO_DATA_TABLE(config_head_two_name) = "two_level";
/* API 47's key, in the data segment: the normal Shell's code room (docs/memory-ledger.md, "Vertical text"). */
static const char vertical_key[] VJO_DATA_TABLE(config_vertical_key) = "ocr_vertical";
/* #321's key, in the data segment too (docs/memory-ledger.md, "Lookups: #321"). */
static const char names_key[] VJO_DATA_TABLE(config_names_key) = "names_first";
/* API 54: the spotter's keys and the finder's names, in the data segment as the keys above (the Shell's code pages
 * keep their room) */
static const char finder_key[] VJO_DATA_TABLE(config_finder_key) = "ocr_finder";
static const char finder_grad[] VJO_DATA_TABLE(config_finder_grad) = "grad";
static const char finder_spotter[] VJO_DATA_TABLE(config_finder_spotter) = "spotter";
const char *const vjo_finder_names[] VJO_DATA_TABLE(config_finders_1) = {finder_grad, finder_spotter, NULL};
/* text_source's values, VJO_SOURCE_*'s order; also the names of the mode the game worker applied in the Shell's
 * hooks line (shell/worker.c hook_note). */
const char *const vjo_text_source_names[] VJO_DATA_TABLE(config_sources_1) = {"ocr", "auto", "hooks", NULL};

/* API 54: one off/on choice table for every off/on setting (their index is the value: off 0, on 1), and the
 * settings that parsed off/on by hand read it through set_choice, the same values and warning: the Shell's code
 * pages keep room for the spotter's settings (docs/memory-ledger.md, "The spotter finder"). */
static const char *const off_on[] VJO_DATA_TABLE(config_off_on) = {"off", "on", NULL};
/* The other choices with no rule of their own, by value (VJO_FILTER_*, VJO_HIGHLIGHT_*, VJO_OCR_*, enum VjoTrigger),
 * read through the same table (#418: they parsed by hand), the same values and warning. */
static const char *const filters[] = {"none", "lines", NULL};
static const char *const highlights[] = {"word", "character", NULL};
static const char *const ocr_modes[] = {"auto", "on_press", NULL};
typedef char vjo_config_choices_are_their_values[VJO_FILTER_NONE == 0 && VJO_FILTER_LINES == 1 &&
                                                 VJO_HIGHLIGHT_WORD == 0 && VJO_HIGHLIGHT_CHARACTER == 1 &&
                                                 VJO_OCR_AUTO == 0 && VJO_OCR_ON_PRESS == 1 ? 1 : -1];
enum { CHOICE_OFF_ON, CHOICE_FILTER, CHOICE_HIGHLIGHT, CHOICE_OCR_MODE, CHOICE_TRIGGER };
static const char *const *const choices[] = {off_on, filters, highlights, ocr_modes, trigger_names};
/* #355's review H1: the off/on settings with no rule of their own (an unknown value keeps the field, with
 * set_choice's warning), one table and one loop in place of a branch each, and #358's and API 54's keys read-only
 * data again: SceShell's code room for API 55's ocr_pause (144 B of the normal Shell's code and 32 B of its data,
 * 152 B and 40 B of the debug one's on main efbee95 [measured: tools/check_memory_ledger.py's module rows, #355's
 * head with and without this table, both builds]; docs/memory-ledger.md, "The pause for the frame"). #418 reads
 * the other choices with no rule of their own through it too (non_japanese_filter, highlight_mode, ocr_mode and the
 * two buttons, which parsed by hand: each entry names its names, choices[]). The keys' order is their branches' was;
 * each key is matched whole (vjo_ieq), so none can take another's. */
static const struct {
    const char *key;
    uint16_t offset;
    uint8_t choice; /* its names: choices[] */
} switches[] = {
    {"non_japanese_filter", offsetof(VjoConfig, non_japanese_filter), CHOICE_FILTER}, /* #418 */
    {"japanese_only_selection", offsetof(VjoConfig, japanese_only_selection), CHOICE_OFF_ON},
    {"highlight_mode", offsetof(VjoConfig, highlight_mode), CHOICE_HIGHLIGHT},        /* #418 */
    {"frequency_badge", offsetof(VjoConfig, frequency_badge), CHOICE_OFF_ON},
    {"toggle_button", offsetof(VjoConfig, toggle_button), CHOICE_TRIGGER},            /* #418 */
    {"subtitle_button", offsetof(VjoConfig, subtitle_button), CHOICE_TRIGGER},        /* #418 */
    {"ocr_text_gate", offsetof(VjoConfig, ocr_text_gate), CHOICE_OFF_ON},
    {"ocr_spotter_refine", offsetof(VjoConfig, ocr_spotter_refine), CHOICE_OFF_ON}, /* API 54 */
    {"ocr_pause", offsetof(VjoConfig, ocr_pause), CHOICE_OFF_ON},                  /* API 55, off by default */
    {"overlay_on_text", offsetof(VjoConfig, overlay_on_text), CHOICE_OFF_ON},      /* API 46 (draft #326), off by default */
    {"ocr_skip_blank_lines", offsetof(VjoConfig, ocr_skip_blank_lines), CHOICE_OFF_ON},
    {"ocr_boost_clock", offsetof(VjoConfig, ocr_boost_clock), CHOICE_OFF_ON},
    {"ocr_wait_hint", offsetof(VjoConfig, ocr_wait_hint), CHOICE_OFF_ON},          /* #358 */
    {"ocr_mode", offsetof(VjoConfig, ocr_mode), CHOICE_OCR_MODE},                   /* #418 */
    {"log_file", offsetof(VjoConfig, log_file), CHOICE_OFF_ON},
    {"anki_mining", offsetof(VjoConfig, anki_mining), CHOICE_OFF_ON},
    {"anki_screenshot", offsetof(VjoConfig, anki_screenshot), CHOICE_OFF_ON},
    {"anki_screenshot_trim_bars", offsetof(VjoConfig, anki_screenshot_trim_bars), CHOICE_OFF_ON},
};

/* The setting a switch's key names (its field, its names), or 0 when key is no switch's. */
static int switch_field(VjoConfig *c, const char *key, int **field, const char *const **names)
{
    for (size_t i = 0; i < sizeof(switches) / sizeof(switches[0]); i++)
        if (vjo_ieq(key, switches[i].key)) {
            *field = (int *)((char *)c + switches[i].offset);
            *names = choices[switches[i].choice];
            return 1;
        }
    return 0;
}

/* tier_set: a vocr_tier line set the size (an older vocr_model line then no longer does), the parse's own */
static void set_kv(VjoConfig *c, const char *key, const char *val, int *tier_set)
{
    const StrSetting *ss;
    const char *const *names;
    int *sw;
    if (vjo_ieq(key, "dictionary")) {
        /* the local dictionaries are the only ones: a removed one's value (jpdb, jiten, hachidori) loads as local
         * without a warning (#418), any other value but local warns */
        if (!vjo_ieq(val, "local") && !removed_dictionary(val))
            warn(c, "%s: invalid value", key);
    } else if (switch_field(c, key, &sw, &names)) {
        set_choice(c, key, val, sw, names);
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
    } else if (vjo_ieq(key, "text_source")) {
        c->text_source = VJO_SOURCE_AUTO; /* an unknown value: auto, with the warning */
        set_choice(c, key, val, &c->text_source, vjo_text_source_names);
    } else if (vjo_ieq(key, "hook_sites") || vjo_ieq(key, "hook_trust")) {
        /* hook_sites or (API 43) hook_trust: their sixth letters differ, in either case */
        int *dst = (key[5] | 0x20) == 's' ? &c->hook_sites : &c->hook_trust;
        *dst = 1; /* an unknown value: on, with the warning */
        set_choice(c, key, val, dst, off_on);
    } else if (vjo_ieq(key, "reading_time")) {
        static const char *const rules[] VJO_DATA_TABLE(config_reading_time_1) = {"adaptive", "15s", NULL}; /* VJO_READING_* */
        c->reading_time = 0; /* an unknown value: adaptive, with the warning */
        set_choice(c, key, val, &c->reading_time, rules);
    } else if (vjo_ieq(key, "vocr_tier")) {
        int t = vjo_vocr_tier_parse(val);
        if (t) {
            c->vocr_tier = t;
            *tier_set = 1;
        } else
            warn(c, "%s: invalid value (small, normal or large)", key);
    } else if (vjo_ieq(key, "vocr_model")) {
        /* An older build's model choice (removed, core/config.c's list): its tier, unless a vocr_tier line says
         * otherwise; any other value is ignored without a warning, as every removed setting is. */
        int t = valid_vocr_model(val) ? vjo_vocr_tier_of_model(val) : 0;
        if (t && !*tier_set)
            c->vocr_tier = t;
    } else if (vjo_ieq(key, "ocr_settle_ms")) {
        set_ms(c, key, val, &c->ocr_settle_ms, 0, 1000);
    } else if (vjo_ieq(key, "ocr_settle_max_ms")) {
        set_ms(c, key, val, &c->ocr_settle_max_ms, 100, 10000);
    } else if (vjo_ieq(key, "ocr_settle_press_ms")) {
        /* read by nothing since #413 (opening the overlay reads without the wait), still checked: 0 or 100..10000 */
        int v;
        if (parse_int(val, &v) || (v && (v < 100 || v > 10000)))
            warn(c, "%s: invalid value (ms, out of range)", key);
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
    } else if (vjo_ieq(key, head_key)) {
        /* each value's index is its VJO_HEAD_* bits */
        static const char *const heads[] VJO_DATA_TABLE(config_heads_1) = {"off", head_gate_name, head_two_name, "on",
                                                                             NULL};
        set_choice(c, key, val, &c->ocr_head_gate, heads);
    } else if (vjo_ieq(key, "ocr_line_fix")) {
        /* off, on, junk, all: #333's per-game names after the spotter's (core/games.h), one table */
        set_choice(c, key, val, &c->ocr_line_fix, vjo_line_finder_names + 1);
    } else if (vjo_ieq(key, finder_key)) { /* API 54 */
        set_choice(c, key, val, &c->ocr_finder, vjo_finder_names);
    } else if (vjo_ieq(key, "ocr_threads")) {
        static const char *const threads[] VJO_DATA_TABLE(config_threads_1) = {"1", "2", "3", NULL}; /* stored as the helpers: 0-2 */
        set_choice(c, key, val, &c->ocr_helpers, threads);
    } else if (vjo_ieq(key, vertical_key) || vjo_ieq(key, names_key)) {
        /* VJO_VERTICAL_* and (#321) VJO_NAMES_*, the same values: ocr_vertical or names_first by the first letter */
        static const char *const modes[] VJO_DATA_TABLE(config_vertical_1) = {"off", "on", "auto", NULL};
        set_choice(c, key, val, (key[0] | 0x20) == 'n' ? &c->names_first : &c->ocr_vertical, modes);
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
    } else {
        char name[VJO_CONFIG_WARN_KEY + 1];
        warn(c, "unknown setting '%s'", warn_name(name, key));
    }
}

void vjo_config_parse(VjoConfig *c, const char *text, size_t len)
{
    size_t i = 0;
    unsigned line_no = 0; /* a line without '=' is named by its number: its text could be a key */
    int tier_set = 0;
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
            /* A removed setting's line (core/config.c's list), whatever follows its name ('=', a space, an API key, a
             * URL that can hold a key, a value too long), is ignored before anything of it could reach a warning; but
             * the two whose value still means something are read as any line is: an older build's vocr_model still
             * sets the model size, and dictionary is still checked (set_kv). */
            size_t w = s;
            int kind;
            while (w < n && line[w] != '=' && !is_space(line[w]))
                w++;
            kind = removed_kind(line + s, w - s);
            if (kind && kind != VJO_REMOVED_TIER && kind != VJO_REMOVED_DICTIONARY)
                continue;
        }
        {
            char *p = strchr(line + s, '=');
            if (!p) {
                /* named by its number, never quoted: its text may be a key */
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
            char name[VJO_CONFIG_WARN_KEY + 1];
            warn(c, "%s: value too long", warn_name(name, key));
            continue;
        }
        memcpy(val, line + s, e - s);
        val[e - s] = '\0';
        set_kv(c, key, val, &tier_set);
    }
    /* The toggle keeps its button (it was there first). */
    if (c->toggle_button == c->subtitle_button) {
        c->subtitle_button = c->toggle_button == VJO_TRIGGER_SELECT_R ? VJO_TRIGGER_SELECT_L : VJO_TRIGGER_SELECT_R;
        warn(c, "toggle_button and subtitle_button are both %s: subtitle_button is %s",
             trigger_names[c->toggle_button], trigger_names[c->subtitle_button]);
    }
}
