#include "config.h"

#include <stddef.h>
#include <string.h>

#include "../include/vjo_vocr_sizes.h"
#include "port.h"
#include "local_dict.h"
#include "textfilter.h"
#include "utf.h"

static const char *const trigger_names[VJO_TRIGGER_COUNT] = {
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
    vjo_snprintf(c->local_dictionaries, sizeof(c->local_dictionaries), "plaintext.vjdict");
    c->local_dictionary_results = 1;
    c->non_japanese_filter = VJO_FILTER_LINES;
    c->japanese_only_selection = 1;
    c->highlight_mode = VJO_HIGHLIGHT_WORD;
    c->font_size_ja = 18;
    c->font_size_en = 14;
    c->toggle_button = VJO_TRIGGER_L_R;
    c->subtitle_button = VJO_TRIGGER_SELECT_R;
    c->ocr_mode = VJO_OCR_AUTO;
    c->ocr_backend = VJO_OCR_VOCR;
    c->text_source = VJO_SOURCE_OCR;
    vjo_snprintf(c->ocr_model_dir, sizeof(c->ocr_model_dir), "ux0:data/VitaJPOverlay/ocr");
    /* Normal (VOCR3-H15) where the game's memory allows; a smaller tier otherwise. */
    c->vocr_tier = VJO_VOCR_TIER_DEFAULT;
    c->ocr_settle_ms = 150;
    c->ocr_settle_max_ms = 2000;
    c->ocr_skip_blank_lines = 0;
    c->ocr_lm = 2;
    c->ocr_cascade_ms = 2000;
    c->ocr_refine_lines = 2;
    c->ocr_helpers = 2; /* ocr_threads = 3 */
    c->ocr_boost_clock = 1; /* 444 MHz while a job recognizes; never lowers a clock (game_ocr/main.c) */
    c->log_file = 1; /* log.txt is on unless log_file = off */
}

const char *vjo_config_default_text(void)
{
    return "; Vita JP Overlay settings. Changes apply the next time the overlay opens.\n"
           "\n"
           "; Dictionary used for word lookups: local (the files below)\n"
           "dictionary = local\n"
           "\n"
           "; Local Yomitan dictionaries: convert on a computer, then copy .vjdict files here.\n"
           "local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n"
           "; Up to 8 filenames, separated by commas, in priority order.\n"
           "local_dictionaries = plaintext.vjdict\n"
           "; Dictionaries shown per selected word, 1-8: the best match first, then each\n"
           "; other dictionary's entry in the order above. L/R tap pages; one is in memory.\n"
           "local_dictionary_results = 1\n"
           "\n"
           "; Drop OCR lines without Japanese characters: none | lines\n"
           "non_japanese_filter = lines\n"
           "\n"
           "; Left/right character lookup skips Latin text, numbers and punctuation: on | off.\n"
           "; Select Japanese kana and Han characters; this does not tokenize words.\n"
           "japanese_only_selection = on\n"
           "\n"
           "; Highlight the dictionary match or only the cursor character: word | character.\n"
           "; Word skips the matched source span; character steps one character.\n"
           "highlight_mode = word\n"
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
           "; Text is read from screenshots using OCR.\n"
           "text_source = ocr\n"
           "\n"
           "; OCR engine: vocr (offline on the Vita; the Overlay app names it after its\n"
           "; model's generation, e.g. VOCR3: docs/vita-vn-ocr.md) | lens (Google, online)\n"
           "; Local OCR never falls back to uploading the screen.\n"
           "ocr_backend = vocr\n"
           "ocr_model_dir = ux0:data/VitaJPOverlay/ocr\n"
           "; vocr model size (the Overlay app's Model size): small (fastest) | normal\n"
           "; (the default, balanced) | large (most accurate)\n"
           "vocr_tier = normal\n"
           "; vocr: read text once still this long (ms, 0 = off), waiting at most the max\n"
           "ocr_settle_ms = 150\n"
           "ocr_settle_max_ms = 2000\n"
           "; vocr: skip found lines with no ink (an even area, a smooth gradient): on | off\n"
           "; (off: it finds next to none at native resolution; see docs/vita-vn-ocr.md)\n"
           "ocr_skip_blank_lines = off\n"
           "; vocr: largest LM: 4000k | 2000k | 1000k | 500k | off\n"
           "ocr_lm = 1000k\n"
           "; vocr: re-read doubtful lines with the next model size up within (ms, 0 = off)\n"
           "ocr_cascade_ms = 2000\n"
           "; vocr: refine lines: psp | on | off\n"
           "ocr_refine_lines = psp\n"
           "; vocr: lines read at once (1-3), 444 MHz while reading (on | off)\n"
           "ocr_threads = 3\n"
           "ocr_boost_clock = on\n"
           "\n"
           "; auto: recognize text in the background when the region changes (lens, vocr)\n"
           "; on_press: recognize only when the overlay is opened\n"
           "ocr_mode = auto\n"
           "\n"
           "; Debugging: IP of a computer running tools/udp_log_listener.py (empty = off)\n"
           "log_host =\n"
           "\n"
           "; Debugging: write ux0:data/VitaJPOverlay/log.txt (max 1 MB + one rotated file): on | off\n"
           "log_file = on\n"
           "\n"
           "; ---- Anki (optional): set it up in the Overlay app's Anki page ----\n"
           "; on: enter (× or ○, as the Vita's settings assign it) in the overlay saves the\n"
           "; shown word as a card in a queue on the memory card, even with Wi-Fi off; △ or\n"
           "; the Overlay app sends the queue to AnkiConnect (Kiku note type, deck Kiku;\n"
           "; docs/anki.md). off: enter and △ do nothing.\n"
           "anki_mining = off\n"
           "; AnkiConnect's computer: IP[:port] (default 8765), or auto: the Overlay app finds it\n"
           "anki_host =\n"
           "; Optional word audio when sending: a Yomitan audio source URL with {term}\n"
           "anki_audio_url =\n";
}

/* host[:port], the n bytes at s: a name or IP address, and a port
 * (default_port when there is none). Returns 0, or -1 when invalid. */
static int host_port(const char *s, size_t n, char *host, size_t cap, int default_port, int *port)
{
    const char *colon = memchr(s, ':', n);
    size_t hn = colon ? (size_t)(colon - s) : n;
    int v = 0;
    if (hn == 0 || hn >= cap)
        return -1;
    for (size_t i = 0; i < hn; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' ||
              ch == '-'))
            return -1;
    }
    if (colon) {
        const char *p = colon + 1, *end = s + n;
        if (p == end)
            return -1;
        for (; p < end; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            v = v * 10 + (*p - '0');
            if (v > 65535)
                return -1;
        }
        if (v == 0)
            return -1;
    }
    *port = colon ? v : default_port;
    memcpy(host, s, hn);
    host[hn] = '\0';
    return 0;
}

int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port)
{
    if (!*setting)
        return VJO_ANKI_OFF;
    if (vjo_ieq(setting, "auto"))
        return VJO_ANKI_AUTO;
    if (host_port(setting, strlen(setting), host, cap, VJO_ANKI_PORT, port) < 0)
        return -1;
    return VJO_ANKI_MANUAL;
}

int vjo_anki_audio_endpoint(const char *url, char *host, int *port, int *tls, const char **path)
{
    size_t n = 0;
    int term = 0;
    if (!strncmp(url, "https://", 8)) {
        *tls = 1;
        url += 8;
    } else if (!strncmp(url, "http://", 7)) {
        *tls = 0;
        url += 7;
    } else {
        return -1;
    }
    while (url[n] && url[n] != '/' && url[n] != '?')
        n++;
    for (const char *s = url + n; *s; s++) {
        if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 0x7F || *s == '#')
            return -1; /* not one URL (a space, a fragment), or not ASCII */
        term |= !strncmp(s, "{term}", 6);
    }
    if (!term || host_port(url, n, host, VJO_HOST_MAX, *tls ? 443 : 80, port) < 0)
        return -1;
    *path = url + n;
    return 0;
}

static void warn(VjoConfig *c, const char *fmt, const char *a, const char *b)
{
    if (c->n_warnings >= VJO_CONFIG_MAX_WARNINGS)
        return;
    vjo_snprintf(c->warnings[c->n_warnings++], sizeof(c->warnings[0]), fmt, a, b);
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
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
 * Hachidori relay on a computer. Such a file loads as local; the Overlay
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
 * the Overlay app's next save and the installer (tools/migrate_config.py
 * SECRETS) delete it. */
static int removed_secret(const char *s, size_t n)
{
    static const char *const keys[] = {"jpdb_api_key", "jiten_api_key", "api_key"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (n >= strlen(keys[i]) && vjo_ieq_prefix(s, keys[i]))
            return 1;
    return 0;
}

int vjo_config_removed_line(const char *line, size_t n, const char **replacement)
{
    size_t b = 0, e, v;
    *replacement = NULL;
    while (b < n && is_space(line[b])) b++;
    if (removed_secret(line + b, n - b))
        return VJO_LINE_DROP;
    e = b;
    while (e < n && line[e] != '=' && !is_space(line[e])) e++;
    v = e;
    while (v < n && is_space(line[v])) v++;
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

static int valid_anki_host(const char *v)
{
    char host[64];
    int port;
    return vjo_anki_endpoint(v, host, sizeof(host), &port) >= 0;
}

static int valid_audio_url(const char *v)
{
    char host[VJO_HOST_MAX];
    const char *path;
    int port, tls;
    return !*v || vjo_anki_audio_endpoint(v, host, &port, &tls, &path) == 0;
}

/* An ocr_backend value of a removed engine: why it no longer runs (the config
 * warning), or NULL. Such a file loads as vocr; the Overlay app's next save
 * and the installer (tools/migrate_config.py REPLACED_VALUES) rewrite it. */
static const char *removed_engine(const char *v)
{
    static const char *const removed[][2] = {
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
static const char *const tier_ids[VJO_VOCR_TIERS] = {"small", "normal", "large"};
static const char *const tier_models[VJO_VOCR_TIERS] = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME,
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
 * trial's VOCR2_FL10_w8.vocr, F20_w8.vocr, VOCR4_W20_w8.vocr). */
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
    const char *expected;        /* the warning's description of a valid value */
    int quiet;                   /* the warning leaves out the value (long, or secret) */
} StrSetting;

#define STR_SETTING(name, required, raw) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), required, raw, NULL, NULL, 0}
#define CHECKED_SETTING(name, raw, valid, expected, quiet) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), 0, raw, valid, expected, quiet}
static const StrSetting str_settings[] = {
    STR_SETTING(local_dictionary_dir, 1, 1),
    STR_SETTING(ocr_model_dir, 1, 1),
    CHECKED_SETTING(local_dictionaries, 0, vjo_local_names_valid, "1-8 relative .vjdict filenames separated by commas", 1),
    STR_SETTING(log_host, 0, 0),
    CHECKED_SETTING(anki_host, 0, valid_anki_host, "empty, auto, or IP[:port]", 0),
    CHECKED_SETTING(anki_audio_url, 1, valid_audio_url, "empty, or http(s)://host[:port]/path with {term}", 1),
};
#define N_STR_SETTINGS ((int)(sizeof(str_settings) / sizeof(str_settings[0])))

static const StrSetting *find_str_setting(const char *key)
{
    for (int i = 0; i < N_STR_SETTINGS; i++)
        if (vjo_ieq(key, str_settings[i].key))
            return &str_settings[i];
    return NULL;
}

/* Settings of the Anki field mapping, which a fixed Kiku note replaced:
 * anki_deck, anki_note_type, anki_tags, anki_field_* (any case). */
static int removed_anki_key(const char *key)
{
    return vjo_ieq(key, "anki_deck") || vjo_ieq(key, "anki_note_type") || vjo_ieq(key, "anki_tags") ||
           vjo_ieq_prefix(key, "anki_field_");
}

static int keeps_comment_chars(const char *key)
{
    const StrSetting *ss = find_str_setting(key);
    return ss && ss->raw;
}

static void set_str(VjoConfig *c, const char *key, const char *val, char *dst, size_t size)
{
    if (strlen(val) >= size)
        warn(c, "%s: value too long%s", key, "");
    else
        memcpy(dst, val, strlen(val) + 1);
}

static void set_font(VjoConfig *c, const char *key, const char *val, int *dst)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= 8 && v <= 40)
        *dst = v;
    else
        warn(c, "%s: invalid value '%s' (8-40)", key, val);
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
    warn(c, "%s: invalid value '%s'", key, val);
}

static void set_ms(VjoConfig *c, const char *key, const char *val, int *dst, int lo, int hi)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= lo && v <= hi)
        *dst = v;
    else
        warn(c, "%s: invalid value '%s' (ms, out of range)", key, val);
}

static void set_trigger(VjoConfig *c, const char *key, const char *val, int *dst)
{
    for (int i = 0; i < VJO_TRIGGER_COUNT; i++) {
        if (vjo_ieq(val, trigger_names[i])) {
            *dst = i;
            return;
        }
    }
    warn(c, "%s: invalid value '%s'", key, val);
}

static void set_kv(VjoConfig *c, const char *key, const char *val)
{
    const StrSetting *ss;
    if (vjo_ieq(key, "dictionary")) {
        /* the local dictionaries are the only ones: c->dictionary stays VJO_DICT_LOCAL */
        const char *removed = removed_dictionary(val);
        if (removed)
            warn(c, "%s: %s was removed; using local", key, removed);
        else if (!vjo_ieq(val, "local"))
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "frequency_filter") || vjo_ieq(key, "font_size") || vjo_ieq(key, "hw_jpeg") ||
               vjo_ieq(key, "combo_delay_ms") || vjo_ieq(key, "hachidori_host") || removed_anki_key(key) ||
               removed_secret(key, strlen(key))) {
        /* removed settings (tools/migrate_config.py REMOVED): no warning (the relay's
         * hachidori_host too: dictionary = hachidori's warning is the relay's one line) */
    } else if (vjo_ieq(key, "non_japanese_filter")) {
        if (vjo_ieq(val, "lines"))
            c->non_japanese_filter = VJO_FILTER_LINES;
        else if (vjo_ieq(val, "none"))
            c->non_japanese_filter = VJO_FILTER_NONE;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "japanese_only_selection")) {
        if (vjo_ieq(val, "on")) c->japanese_only_selection = 1;
        else if (vjo_ieq(val, "off")) c->japanese_only_selection = 0;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "highlight_mode")) {
        if (vjo_ieq(val, "word")) c->highlight_mode = VJO_HIGHLIGHT_WORD;
        else if (vjo_ieq(val, "character")) c->highlight_mode = VJO_HIGHLIGHT_CHARACTER;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "local_dictionary_results")) {
        int v;
        if (parse_int(val, &v) == 0 && v >= 1 && v <= VJO_LOCAL_MAX_DICTS)
            c->local_dictionary_results = v;
        else
            warn(c, "%s: invalid value '%s' (1-8)", key, val);
    } else if (vjo_ieq(key, "font_size_ja")) {
        set_font(c, key, val, &c->font_size_ja);
    } else if (vjo_ieq(key, "font_size_en")) {
        set_font(c, key, val, &c->font_size_en);
    } else if (vjo_ieq(key, "toggle_button")) {
        set_trigger(c, key, val, &c->toggle_button);
    } else if (vjo_ieq(key, "subtitle_button")) {
        set_trigger(c, key, val, &c->subtitle_button);
    } else if (vjo_ieq(key, "text_source")) {
        if (vjo_ieq(val, "auto")) c->text_source = VJO_SOURCE_OCR;
        else if (vjo_ieq(val, "hooks")) {
            c->text_source = VJO_SOURCE_OCR;
            warn(c, "%s: hooks are unavailable in this OCR build", key, "");
        }
        else if (vjo_ieq(val, "ocr")) c->text_source = VJO_SOURCE_OCR;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_backend")) {
        const char *removed;
        if (vjo_ieq(val, "lens"))
            c->ocr_backend = VJO_OCR_LENS;
        else if (vjo_ieq(val, "vocr"))
            c->ocr_backend = VJO_OCR_VOCR;
        else if ((removed = removed_engine(val)) != NULL) {
            c->ocr_backend = VJO_OCR_VOCR;
            warn(c, "%s: %s; using vocr", key, removed);
        }
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "vocr_tier")) {
        int t = vjo_vocr_tier_parse(val);
        if (t) {
            c->vocr_tier = t;
            c->vocr_tier_explicit = 1;
        } else
            warn(c, "%s: invalid value '%s' (small, normal or large)", key, val);
    } else if (vjo_ieq(key, "vocr_model")) {
        /* An older build's model choice: its tier, unless a vocr_tier line says otherwise. */
        int t = valid_vocr_model(val) ? vjo_vocr_tier_of_model(val) : 0;
        if (!t)
            warn(c, "%s: '%s' is not a model of this build; using vocr_tier", key, val);
        else if (!c->vocr_tier_explicit)
            c->vocr_tier = t;
    } else if (vjo_ieq(key, "vocr_fallback_model")) {
        /* removed: a job falls back to the next smaller tier (tools/migrate_config.py REMOVED) */
    } else if (vjo_ieq(key, "ocr_settle_ms")) {
        set_ms(c, key, val, &c->ocr_settle_ms, 0, 1000);
    } else if (vjo_ieq(key, "ocr_settle_max_ms")) {
        set_ms(c, key, val, &c->ocr_settle_max_ms, 100, 10000);
    } else if (vjo_ieq(key, "ocr_cascade_ms")) {
        set_ms(c, key, val, &c->ocr_cascade_ms, 0, 3750);
    } else if (vjo_ieq(key, "ocr_lm")) {
        static const char *const tiers[] = {"off", "500k", "1000k", "2000k", "4000k", NULL};
        set_choice(c, key, val, &c->ocr_lm, tiers);
    } else if (vjo_ieq(key, "ocr_refine_lines")) {
        static const char *const modes[] = {"off", "on", "psp", NULL};
        set_choice(c, key, val, &c->ocr_refine_lines, modes);
    } else if (vjo_ieq(key, "ocr_skip_blank_lines")) {
        if (vjo_ieq(val, "on")) c->ocr_skip_blank_lines = 1;
        else if (vjo_ieq(val, "off")) c->ocr_skip_blank_lines = 0;
        else warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_threads")) {
        static const char *const threads[] = {"1", "2", "3", NULL}; /* stored as the helpers: 0-2 */
        set_choice(c, key, val, &c->ocr_helpers, threads);
    } else if (vjo_ieq(key, "ocr_boost_clock")) {
        static const char *const off_on[] = {"off", "on", NULL};
        set_choice(c, key, val, &c->ocr_boost_clock, off_on);
    } else if (vjo_ieq(key, "ocr_mode")) {
        if (vjo_ieq(val, "auto"))
            c->ocr_mode = VJO_OCR_AUTO;
        else if (vjo_ieq(val, "on_press"))
            c->ocr_mode = VJO_OCR_ON_PRESS;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((ss = find_str_setting(key)) != NULL) {
        char *dst = (char *)c + ss->offset;
        if (!*val && ss->required)
            warn(c, "%s: empty (keeping %s)", key, dst);
        else
            set_str(c, key, val, dst, ss->size);
        if (ss->valid && !ss->valid(dst)) {
            if (ss->quiet) {
                warn(c, "%s: invalid value (%s)", key, ss->expected);
            } else {
                char what[96];
                vjo_snprintf(what, sizeof(what), "'%s' (%s)", val, ss->expected);
                warn(c, "%s: invalid value %s", key, what);
            }
            dst[0] = '\0';
        }
    } else if (vjo_ieq(key, "log_file")) {
        if (vjo_ieq(val, "on"))
            c->log_file = 1;
        else if (vjo_ieq(val, "off"))
            c->log_file = 0;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "anki_mining")) {
        if (vjo_ieq(val, "on"))
            c->anki_mining = 1;
        else if (vjo_ieq(val, "off"))
            c->anki_mining = 0;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else {
        warn(c, "unknown setting '%s'%s", key, "");
    }
}

#define DROPPED_PREFERENCE (-2) /* a line the app's save removes (preference_key) */

static int preference_key(const char *line, size_t n)
{
    size_t begin = 0, end;
    char key[40];
    while (begin < n && is_space(line[begin])) begin++;
    end = begin;
    while (end < n && line[end] != '=' && !is_space(line[end])) end++;
    size_t key_len = end - begin;
    if (!key_len || key_len >= sizeof(key)) return -1;
    while (end < n && is_space(line[end])) end++;
    if (end == n || line[end] != '=') return -1;
    memcpy(key, line + begin, key_len);
    key[key_len] = 0;
    return vjo_ieq(key, "ocr_backend") ? 0 :
           vjo_ieq(key, "japanese_only_selection") ? 1 :
           vjo_ieq(key, "highlight_mode") ? 2 :
           vjo_ieq(key, "local_dictionary_results") ? 3 :
           vjo_ieq(key, "vocr_tier") ? 4 :
           /* an older build's model choice, which vocr_tier replaces: dropped */
           vjo_ieq(key, "vocr_model") || vjo_ieq(key, "vocr_fallback_model") ? DROPPED_PREFERENCE : -1;
}

#define OWNED_PREFERENCES 5

int vjo_config_patch_preferences(const char *text, size_t len, int backend, int vocr_tier,
                                 int japanese_only, int highlight_mode,
                                 int dictionary_results, char *out, size_t cap)
{
    const char *values[OWNED_PREFERENCES];
    char results_line[40], tier_line[32];
    size_t i = 0, used = 0;
    unsigned seen = 0;
    static const char *const backend_values[] = {
        [VJO_OCR_LENS] = "ocr_backend = lens\n", [VJO_OCR_VOCR] = "ocr_backend = vocr\n"
    };
    if ((!text && len) || len > VJO_CONFIG_TEXT_MAX || !out || !cap ||
        (backend != VJO_OCR_LENS && backend != VJO_OCR_VOCR) || !vjo_vocr_tier_id(vocr_tier) ||
        (japanese_only != 0 && japanese_only != 1) ||
        (highlight_mode != VJO_HIGHLIGHT_WORD && highlight_mode != VJO_HIGHLIGHT_CHARACTER) ||
        dictionary_results < 1 || dictionary_results > VJO_LOCAL_MAX_DICTS ||
        (len && memchr(text, 0, len)))
        return -1;
    values[0] = backend_values[backend];
    values[1] = japanese_only ? "japanese_only_selection = on\n" : "japanese_only_selection = off\n";
    values[2] = highlight_mode == VJO_HIGHLIGHT_WORD
        ? "highlight_mode = word\n" : "highlight_mode = character\n";
    vjo_snprintf(results_line, sizeof(results_line), "local_dictionary_results = %d\n", dictionary_results);
    values[3] = results_line;
    vjo_snprintf(tier_line, sizeof(tier_line), "vocr_tier = %s\n", vjo_vocr_tier_id(vocr_tier));
    values[4] = tier_line;
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
            if (removed == VJO_LINE_DROP) continue;
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
    /* Skip a UTF-8 BOM (Notepad). */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        i = 3;
    while (i < len) {
        char line[1024], key[64], val[sizeof(((VjoConfig *)0)->local_dictionaries)]; /* the longest value */
        size_t n = 0, s, e, eq;
        int cut = 0;
        while (i < len && text[i] != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = text[i];
            else
                cut = 1;
            i++;
        }
        i++;
        line[n] = '\0';

        s = 0;
        while (s < n && is_space(line[s]))
            s++;
        if (s == n || line[s] == ';' || line[s] == '#')
            continue;
        {
            char *p = strchr(line + s, '=');
            if (!p) {
                /* a removed API key's line is not quoted: its value may be the key */
                if (!removed_secret(line + s, n - s))
                    warn(c, "ignored line '%s'%s", line + s, "");
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
            if (!removed_secret(key, strlen(key))) /* a removed setting stays silent */
                warn(c, "%s: value too long%s", key, "");
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
