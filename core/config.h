/* config.ini parsing (ux0:data/VitaJPOverlay/config.ini). */
#ifndef VJO_CONFIG_H
#define VJO_CONFIG_H

#include <stddef.h>

#include "../include/vjo_api.h"

enum { VJO_OCR_AUTO = 0, VJO_OCR_ON_PRESS = 1 };
/* ocr_backend: the two OCR engines. VJO_OCR_VOCR runs bee-san/vita-vn-ocr on
 * the Vita CPU, in the game (VJO_WITH_VOCR); the Overlay app names it after its
 * model's generation (core/vocr_models.h). VJO_OCR_LENS is Google Lens. The
 * self-test reports the value (config backend=); keep the IDs unchanged. The
 * removed engines' IDs stay unused: 1 ncnn (host experiments), 3 the Meiki
 * trial, 4 the VOCR2 trial; config.ini values naming them load as vocr. */
enum { VJO_OCR_LENS = 0, VJO_OCR_VOCR = 2 };
enum { VJO_SOURCE_OCR = 0, VJO_SOURCE_AUTO = 1, VJO_SOURCE_HOOKS = 2 };
enum { VJO_HIGHLIGHT_WORD = 0, VJO_HIGHLIGHT_CHARACTER = 1 };
/* dictionary: the local .vjdict files, the only dictionary (VJO_DICT_NAME in
 * messages and the log). The self-test reports the value (config
 * dictionary=); keep the ID unchanged. The removed ones' IDs stay unused: 0
 * and 1 jpdb.io and jiten.moe, the online dictionaries with an API key
 * (removed after v0.8.0-test.2), 2 the Hachidori relay on a computer
 * (hachidori_host, removed after them). A config.ini naming one loads as
 * local with a warning; the Overlay app's next save and the installer write
 * local, and drop the API keys (vjo_config_removed_line, tools/
 * migrate_config.py); hachidori_host is ignored, and the installer drops it. */
enum { VJO_DICT_LOCAL = 3 };
#define VJO_DICT_NAME "Local dictionaries"

#define VJO_CONFIG_MAX_WARNINGS 4
/* The Shell reads the text plus NUL into its existing 24 KiB scratch arena.
 * Settings must never publish a file larger than that reader can load. */
#define VJO_CONFIG_TEXT_MAX (24 * 1024 - 1)

typedef struct {
    int dictionary;           /* VJO_DICT_LOCAL, the only one */
    char local_dictionary_dir[160];
    char local_dictionaries[512]; /* comma-separated relative .vjdict filenames */
    int local_dictionary_results; /* 1..8: dictionaries paged with an L/R tap per selected word */
    int non_japanese_filter;  /* VJO_FILTER_NONE | VJO_FILTER_LINES */
    int japanese_only_selection; /* 0/1: local cursor skips non-Japanese characters */
    int highlight_mode;       /* VJO_HIGHLIGHT_*: match span or selected character */
    int font_size_ja;         /* 8..40: header, headwords, readings */
    int font_size_en;         /* 8..40: meanings, rank, messages */
    int toggle_button;        /* enum VjoTrigger */
    int subtitle_button;      /* enum VjoTrigger, != toggle_button */
    int ocr_mode;             /* VJO_OCR_* */
    int ocr_backend;          /* VJO_OCR_VOCR | VJO_OCR_LENS */
    int text_source;          /* compatibility setting, always screenshot OCR in this build */
    char ocr_model_dir[160];
    /* The local engine's model size (API 21): VJO_VOCR_TIER_SMALL, _NORMAL (the
     * default) or _LARGE of include/vjo_vocr_sizes.h; config.ini vocr_tier = small |
     * normal | large. Each is a pinned model (vjo_vocr_tier_model). A job runs it,
     * or the next smaller one the game's free memory has room for (the game worker
     * decides per job). An older file's vocr_model sets it when vocr_tier is absent
     * (the FL10 family small, H15 normal, F20/W20 large); vocr_fallback_model is
     * no longer read. */
    int vocr_tier;
    int vocr_tier_explicit;   /* 1: a vocr_tier line set it (an older vocr_model line no longer does) */
    /* Local OCR (vocr): the region's rows hold still this long before a job
     * reads its lines (0 = off), waiting at most ocr_settle_max_ms per job; and
     * found lines without text-like ink are skipped (core/vocr_ocr.h). */
    int ocr_settle_ms;        /* 0..1000 */
    int ocr_settle_max_ms;    /* 100..10000 */
    int ocr_skip_blank_lines; /* 0/1 */
    /* Local OCR (vocr): the largest character LM the beam + LM decoder may use,
     * 1-4 (0.5, 1, 2, 4 MB), the game worker taking the largest that fits; 0: off,
     * greedy decoding (core/vocr_ocr.h VjoVocrPolicy.lm). */
    int ocr_lm;
    int ocr_cascade_ms;       /* 0 (off) or 250..3750: the cascade's time ceiling per recognition */
    int ocr_refine_lines;     /* 0 off, 1 on, 2 PSP games only (VJO_GAME_OCR_REFINE_*) */
    char log_host[64];        /* "" = UDP log off */
    int log_file;             /* 0/1 */
    /* Anki (core/anki.h): one note type, Kiku, in one deck; nothing else to set. */
    int anki_mining;          /* 0/1: × saves cards to the offline queue, △ sends it */
    char anki_host[64];       /* see vjo_anki_endpoint */
    char anki_audio_url[256]; /* "" = no word audio; see vjo_anki_audio_endpoint */
    char warnings[VJO_CONFIG_MAX_WARNINGS][96];
    int n_warnings;
    /* Local OCR (vocr, API 22): ocr_threads, the threads that recognize a
     * region's lines at once (1 = one line after another), kept as the helper
     * threads beside the job's own (ocr_threads - 1, VjoGameOcrRequest.speed's
     * bits 0-1); and the ARM clock at 444 MHz while a job recognizes (on by
     * default; a clock already at 444 or more is left as it is; core/vocr_ocr.h
     * VjoVocrPool, busy). Last, so that no other field moves (the Shell's code
     * size). */
    int ocr_helpers;          /* 0..2 */
    int ocr_boost_clock;      /* 0/1 */
    /* Local OCR (vocr): the most a job the open overlay waits for spends waiting
     * for the rows to hold still (0: ocr_settle_max_ms as for any job); the
     * smaller of the two applies (docs/first-ocr.md). */
    int ocr_settle_press_ms;  /* 0 or 100..10000 */
} VjoConfig;

void vjo_config_defaults(VjoConfig *c);
/* Parses INI text over the defaults; invalid values keep the default and
 * add a warning. */
void vjo_config_parse(VjoConfig *c, const char *text, size_t len);
/* Default file written on first run (with comments). */
const char *vjo_config_default_text(void);

/* Changes only the app's five OCR/lookup preferences (ocr_backend, vocr_tier,
 * japanese_only_selection, highlight_mode, local_dictionary_results), retaining
 * every other INI line verbatim but the vocr_model and vocr_fallback_model
 * lines of older builds, which vocr_tier replaces, and those of removed
 * features (vjo_config_removed_line: dropped or replaced). Duplicate owned
 * keys are collapsed. Input/output must not overlap. Returns output bytes (not
 * including NUL), or -1 for invalid choices, embedded NUL, or insufficient
 * capacity. */
int vjo_config_patch_preferences(const char *text, size_t len, int backend, int vocr_tier,
                                 int japanese_only, int highlight_mode,
                                 int dictionary_results, char *out, size_t cap);

/* The model tiers (include/vjo_vocr_sizes.h VJO_VOCR_TIER_*): a tier's
 * config.ini value ("small" | "normal" | "large", NULL outside 1..VJO_VOCR_TIERS),
 * the tier of a value (any case; 0 for none), the tier's pinned model file in
 * ocr_model_dir (NULL outside), and the tier an older build's vocr_model file
 * belongs to by its shape (FL10 small, H15 normal, F20 or W20 large; 0: none). */
const char *vjo_vocr_tier_id(int tier);
int vjo_vocr_tier_parse(const char *id);
const char *vjo_vocr_tier_model(int tier);
int vjo_vocr_tier_of_model(const char *name);

/* A config.ini line (n bytes, its newline included or not) of a removed
 * feature, which the Overlay app's saves rewrite as they patch the file:
 * VJO_LINE_DROP for the API keys of the removed online dictionaries (a line
 * whose first word starts with jpdb_api_key, jiten_api_key or api_key, jpdb's
 * older name, in any case, with or without '='), VJO_LINE_REPLACE with
 * *replacement ("dictionary = local\n") for dictionary = jpdb, jiten or
 * hachidori, else VJO_LINE_KEEP. */
enum { VJO_LINE_KEEP = 0, VJO_LINE_DROP = 1, VJO_LINE_REPLACE = 2 };
int vjo_config_removed_line(const char *line, size_t n, const char **replacement);

const char *vjo_trigger_name(int trigger);

/* anki_host (or the saved anki_host.txt), where △ sends the queue: "" =
 * nowhere (× still saves cards), "auto" = search the network, or
 * host[:port]. For VJO_ANKI_MANUAL, host (cap bytes) and port
 * are set. Returns -1 for an invalid value. */
#define VJO_ANKI_PORT 8765
enum { VJO_ANKI_OFF = 0, VJO_ANKI_AUTO = 1, VJO_ANKI_MANUAL = 2 };
int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port);

/* anki_audio_url: a Yomitan custom audio source,
 * http(s)://host[:port][/path][?query] with {term} (and usually {reading})
 * in it. Sets host (VJO_HOST_MAX bytes), port, tls and the path template
 * (in url, from the '/' or '?' after the host; "" for none). Returns 0, or
 * -1 for an invalid or empty value. */
#define VJO_HOST_MAX 128
int vjo_anki_audio_endpoint(const char *url, char *host, int *port, int *tls, const char **path);

#endif
