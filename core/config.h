/* config.ini parsing (ux0:data/VitaJPOverlay/config.ini). */
#ifndef VJO_CONFIG_H
#define VJO_CONFIG_H

#include <stddef.h>

#include "../include/vjo_api.h"

enum { VJO_OCR_AUTO = 0, VJO_OCR_ON_PRESS = 1 };
/* One OCR engine: bee-san/vita-vn-ocr on the Vita CPU, in the game
 * (VJO_WITH_VOCR). The overlay is offline only (owner 2026-10-15): Google Lens
 * was removed, and with it the ocr_backend setting. A config.ini line naming an
 * engine (ocr_backend = vocr, lens, or a removed trial: vocr2, meiki, ncnn) is
 * ignored, Lens and the trials with a config warning; the Rubi app's next
 * save and the installer (tools/migrate_config.py) drop the line
 * (vjo_config_removed_line). The self-test still reports backend=2, the old
 * VOCR id, so its reports keep their form. log_host, the UDP log's computer,
 * went with the network too: a set one gives one config warning, an empty one
 * none, and the app's next save and the installer drop the line; log.txt and
 * status.txt stay. */
/* text_source (API 31, docs/native-hooks.md): the kernel's VJO_TEXT_SOURCE_* (include/vjo_api.h), which
 * the Shell pushes with vjoSetTextSource whenever it reads the config; a game reads it as it starts. */
enum { VJO_SOURCE_OCR = VJO_TEXT_SOURCE_OCR, VJO_SOURCE_AUTO = VJO_TEXT_SOURCE_AUTO,
       VJO_SOURCE_HOOKS = VJO_TEXT_SOURCE_HOOKS };
enum { VJO_HIGHLIGHT_WORD = 0, VJO_HIGHLIGHT_CHARACTER = 1 };
/* dictionary: the local .vjdict files, the only dictionary (VJO_DICT_NAME in
 * messages and the log). The self-test reports the value (config
 * dictionary=); keep the ID unchanged. The removed ones' IDs stay unused: 0
 * and 1 jpdb.io and jiten.moe, the online dictionaries with an API key
 * (removed after v0.8.0-test.2), 2 the Hachidori relay on a computer
 * (hachidori_host, removed after them). A config.ini naming one loads as
 * local with a warning; the Rubi app's next save and the installer write
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
    int frequency_badge;      /* 0/1: the popup shows the entry's frequency rank ("#1234") */
    int overlay_on_text;      /* 0/1, API 46: the overlay is drawn on the game's own text (docs/overlay-on-text.md) */
    int font_size_ja;         /* 8..40: header, headwords, readings */
    int font_size_en;         /* 8..40: meanings, rank, messages */
    int toggle_button;        /* enum VjoTrigger */
    int subtitle_button;      /* enum VjoTrigger, != toggle_button */
    int ocr_mode;             /* VJO_OCR_* */
    int text_source;          /* VJO_SOURCE_*: auto (the default) | hooks | ocr; applies at a game's next start */
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
    int log_file;             /* 0/1 */
    /* Anki (docs/anki.md): the Rubi app exports the queue as Lapis notes; nothing else to set. */
    int anki_mining;          /* 0/1: enter saves the shown word to the offline queue */
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
    /* Anki (API 23): a card saved with anki_mining keeps a JPEG of the game's screen
     * (on by default; the game worker takes it, docs/anki.md). Last, as above. */
    int anki_screenshot;      /* 0/1 */
    /* The left stick's lookup (#138, core/stick_cursor.h): a word the stick
     * moved the cursor to is looked up once the cursor has rested this long
     * (0: as soon as the stick stops); the d-pad and taps look up at once.
     * Last, as above. */
    int stick_lookup_ms;      /* 0..1000 */
    /* Local OCR (vocr, API 26): the text-presence gate in front of the line finder
     * (vita-vn-ocr #100, core/vocr_ocr.h VjoVocrPolicy.gate): a region it says holds no
     * text (a hidden text box, a CG screen) returns no line. Last, as above. */
    int ocr_text_gate;        /* 0/1 */
    /* Native text hooks (API 31, audit-hooks F3): 0 (config.ini hook_sites = off) installs no table site in
     * any game, only the CALL source (the string copies), so a console run can tell the two apart; the
     * Shell sends it as VJO_TEXT_SOURCE_NO_SITES in the text source's word. Last, as above. */
    int hook_sites;           /* 0/1, on by default */
    /* Anki (API 32): a card's screenshot is of the picture between the frame's black bars, a
     * letterbox or pillarbox the game worker finds as it reads the frame (on by default;
     * core/shottrim.h, docs/anki.md "Black bars"). Last, as above. */
    int anki_screenshot_trim_bars; /* 0/1 */
    /* Reading statistics (the overlay's Triangle view, docs/ocr-history.md#reading-time): how a line's
     * reading time is measured, VJO_READING_* (core/history_lines.h): 0 adaptive (the default), 1 15s
     * (Rubi 0.9.7 and older). The Shell takes it once, as it starts. Last, as above. */
    int reading_time;
    /* Local OCR (vocr, API 42): vita-vn-ocr #117's speed work (core/vocr_ocr.h VjoVocrPolicy.head_gate, two_level),
     * VJO_HEAD_* bits: VJO_HEAD_GATE skips the recognizer's output head on the frames the model's gate calls blank,
     * VJO_HEAD_TWO_LEVEL computes a two-level blob's rare rows only on the frames its escape picks. config.ini
     * ocr_head_gate = on (both, the default) | gate | two_level | off (neither: the full head on every frame, as
     * before); the Shell sends the bits as they are (include/vjo_game_ocr.h VJO_GAME_OCR_S_HEAD_SHIFT). One key, not
     * two: the normal Shell's code had 100 B of its last page left (docs/memory-ledger.md). Last, as above. */
    int ocr_head_gate;        /* VJO_HEAD_* bits, 0-3 */
    /* Native text hooks (API 43, docs/native-hooks.md "Trusted hooks"): 1 (the default) lets a trusted hook (a Luna
     * or Rubi verified site, or a regular source after four OCR agreements) answer without OCR; 0 (hook_trust = off)
     * selects as Rubi 0.9.7 did. The Shell sends it as VJO_GAME_OCR_S_TRUST in each job's speed word. Last, as above. */
    int hook_trust;           /* 0/1, on by default */
    /* Vertical text (vita-vn-ocr #99, API 47; docs/vita-vn-ocr.md "Vertical text"): VJO_VERTICAL_*, the
     * request's VJO_GAME_OCR_S_VERTICAL field as it is (include/vjo_game_ocr.h). Last, as above. */
    int ocr_vertical;
    /* Local OCR (vocr, API 50): the line finder's fixes (bee-san/vita-vn-ocr #129; core/vocr_ocr.h
     * VjoVocrPolicy.line_fix): 1 (on, the default) its measured default, VOCR_FIX_DEFAULT (COLGAP); 2 (junk) also
     * the frame-strip rule, for regions drawn with a margin; 3 (all) also the touching-rows split; 0 (off) the
     * frozen finder. Last, as above. */
    int ocr_line_fix;         /* 0 off, 1 on, 2 junk, 3 all */
    /* The pause for the frame (API 51, #348; docs/game-pause.md): 1 (ocr_pause = on) has each of the user's jobs copy
     * its region from one frame, in that frame's display call, which the game waits in for as long (at most
     * VJO_GAME_OCR_PAUSE_MAX_US), and read every row from that copy; 0 (off, the default) reads strip by strip, as
     * before. The Shell sends it as VJO_GAME_OCR_S_PAUSE in each job's speed word. Last, as above. */
    int ocr_pause;            /* 0/1, off by default */
} VjoConfig;

/* ocr_text_gate's default (docs/vita-vn-ocr.md, "Text-presence gate": why). */
#define VJO_TEXT_GATE_DEFAULT 0
#define VJO_TEXT_GATE_DEFAULT_TEXT "off"
/* ocr_head_gate's bits, its values' indices (off, gate, two_level, on), and its default (docs/vita-vn-ocr.md,
 * "Head gate" and "Two-level head": why). */
#define VJO_HEAD_GATE 1
#define VJO_HEAD_TWO_LEVEL 2
#define VJO_HEAD_GATE_DEFAULT (VJO_HEAD_GATE | VJO_HEAD_TWO_LEVEL)
#define VJO_HEAD_GATE_DEFAULT_TEXT "on"
/* ocr_vertical (vita-vn-ocr #99; docs/vita-vn-ocr.md "Vertical text"): off reads every region's rows, as
 * before; on reads its columns, right to left, with the vertical model, in every game; auto reads its rows
 * unless the line finder's rows look like vertical text, then its columns. The values are the request's
 * VJO_GAME_OCR_S_VERTICAL field's (include/vjo_game_ocr.h), so the Shell sends the setting as it is. */
#define VJO_VERTICAL_OFF 0
#define VJO_VERTICAL_ON 1
#define VJO_VERTICAL_AUTO 2
/* auto (docs/vita-vn-ocr.md "Vertical text": why): on horizontal text it is off's job, bit for bit, wherever its rule
 * sees rows (measured on the host, never on a console). */
#define VJO_VERTICAL_DEFAULT VJO_VERTICAL_AUTO
#define VJO_VERTICAL_DEFAULT_TEXT "auto"
/* ocr_line_fix's default (docs/vita-vn-ocr.md, "Line finder fixes": the measurements). */
#define VJO_LINE_FIX_DEFAULT 1
#define VJO_LINE_FIX_DEFAULT_TEXT "on"

void vjo_config_defaults(VjoConfig *c);
/* Parses INI text over the defaults; invalid values keep the default and
 * add a warning. */
void vjo_config_parse(VjoConfig *c, const char *text, size_t len);
/* The commented default file: docs/config.example.ini, the installer's and the
 * package's template, and what the Rubi app's first save publishes when
 * there is no config.ini (settings/preferences.c). SceShell never writes it:
 * without a file the Shell plugin runs on vjo_config_defaults, the same values. */
const char *vjo_config_default_text(void);

/* Changes only the app's four OCR/lookup preferences (vocr_tier,
 * japanese_only_selection, highlight_mode, local_dictionary_results), retaining
 * every other INI line verbatim but the vocr_model and vocr_fallback_model
 * lines of older builds, which vocr_tier replaces, and those of removed
 * features (vjo_config_removed_line: dropped or replaced). Duplicate owned
 * keys are collapsed. Input/output must not overlap. Returns output bytes (not
 * including NUL), or -1 for invalid choices, embedded NUL, or insufficient
 * capacity. */
int vjo_config_patch_preferences(const char *text, size_t len, int vocr_tier,
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

/* Where a line's name starts: after its spaces, tabs and CRs and its UTF-8 BOMs, in any order (Notepad's BOM at
 * the file's start, and one a hand-joined file leaves at a later line's). The parser, the app's saves and the
 * install's patches read a line's name from there, so a BOM-led line is the same setting to all of them
 * (audit-pr180-b N3; audit-pr183 N1). */
size_t vjo_config_line_start(const char *line, size_t n);

/* A config.ini line (n bytes, its newline included or not) of a removed
 * feature, which the Rubi app's saves rewrite as they patch the file:
 * VJO_LINE_DROP for the API keys of the removed online dictionaries (a line
 * whose first word starts with jpdb_api_key, jiten_api_key or api_key, jpdb's
 * older name, in any case, with or without '=') and for the removed Anki
 * settings (vjo_config_removed_anki_key: anki_audio_url's URL can hold a key),
 * which are secrets; VJO_LINE_DROP_SETTING for a removed setting's line, which
 * is not (the engine choice and the UDP log: an ocr_backend or log_host line,
 * whatever its value);
 * VJO_LINE_REPLACE with *replacement ("dictionary = local\n") for dictionary =
 * jpdb, jiten or hachidori, else VJO_LINE_KEEP. */
enum { VJO_LINE_KEEP = 0, VJO_LINE_DROP = 1, VJO_LINE_REPLACE = 2, VJO_LINE_DROP_SETTING = 3 };
int vjo_config_removed_line(const char *line, size_t n, const char **replacement);

const char *vjo_trigger_name(int trigger);

/* A removed Anki setting (anki_host, anki_audio_url, the old field mapping):
 * 1 when key is one, which loads without a warning on every path (a line
 * without '=', a value too long). The installer and the Rubi app's next
 * save (vjo_config_removed_line) remove it from config.ini. */
int vjo_config_removed_anki_key(const char *key);

#endif
