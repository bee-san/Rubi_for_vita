/* config.ini parsing (ux0:data/VitaJPOverlay/config.ini). */
#ifndef VJO_CONFIG_H
#define VJO_CONFIG_H

#include <stddef.h>

#include "../include/vjo_api.h"

enum { VJO_OCR_AUTO = 0, VJO_OCR_ON_PRESS = 1 };
/* One OCR engine: bee-san/vita-vn-ocr on the Vita CPU, in the game (VJO_WITH_VOCR); the overlay is offline only.
 * The settings of removed features (the engine choice ocr_backend, the UDP log's log_host, the online dictionaries'
 * API keys, AnkiConnect's, ...) are core/config.c's list (vjo_config_removed): the parser ignores their lines without
 * a warning (dictionary and vocr_model are still read: a removed dictionary's value and a vocr_model that gives no
 * size without one), and the Rubi app's next save and the installer (tools/migrate_config.py) drop or rewrite them
 * (vjo_config_removed_line). The self-test still reports backend=2, the old VOCR id, so its reports keep their
 * form. */
/* text_source (API 31, docs/native-hooks.md): the kernel's VJO_TEXT_SOURCE_* (include/vjo_api.h), which
 * the Shell pushes with vjoSetTextSource whenever it reads the config; a game reads it as it starts. */
enum { VJO_SOURCE_OCR = VJO_TEXT_SOURCE_OCR, VJO_SOURCE_AUTO = VJO_TEXT_SOURCE_AUTO,
       VJO_SOURCE_HOOKS = VJO_TEXT_SOURCE_HOOKS };
/* text_source's names by value (core/config.c; the Shell's hooks line names the worker's mode with them) */
extern const char *const vjo_text_source_names[];
enum { VJO_HIGHLIGHT_WORD = 0, VJO_HIGHLIGHT_CHARACTER = 1 };
/* The dictionary: the local .vjdict files, the only one (VJO_DICT_NAME in
 * messages and the log). The debug self-test reports its ID (config
 * dictionary=3, read by tools/analyse_selftest.py); keep it unchanged. The
 * removed ones' IDs stay unused: 0 and 1 jpdb.io and jiten.moe, the online
 * dictionaries with an API key, 2 the Hachidori relay on a computer. A
 * config.ini naming one loads as local without a warning (another value but
 * local warns); the Rubi app's next save and the installer write local
 * (vjo_config_removed_line, tools/migrate_config.py). */
enum { VJO_DICT_LOCAL = 3 };
#define VJO_DICT_NAME "Local dictionaries"

#define VJO_CONFIG_MAX_WARNINGS 4
/* The Shell reads the text plus NUL into its existing 24 KiB scratch arena.
 * Settings must never publish a file larger than that reader can load. */
#define VJO_CONFIG_TEXT_MAX (24 * 1024 - 1)

typedef struct {
    char local_dictionary_dir[160];
    char local_dictionaries[512]; /* comma-separated relative .vjdict filenames */
    int non_japanese_filter;  /* VJO_FILTER_NONE | VJO_FILTER_LINES */
    int japanese_only_selection; /* 0/1: local cursor skips non-Japanese characters */
    int highlight_mode;       /* VJO_HIGHLIGHT_*: match span or selected character */
    int frequency_badge;      /* 0/1: the popup shows the entry's frequency rank ("#1234") */
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
     * decides per job). An older file's vocr_model sets it when no vocr_tier line
     * does (the FL10 family small, H15 normal, F20/W20 large); vocr_fallback_model
     * is no longer read. */
    int vocr_tier;
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
    /* The fields below came later, each appended after the one before, so that no earlier field moved (the Shell's
     * code size); a new one goes last too. */
    /* Local OCR (vocr, API 22): ocr_threads, the threads that recognize a
     * region's lines at once (1 = one line after another), kept as the helper
     * threads beside the job's own (ocr_threads - 1, VjoGameOcrRequest.speed's
     * bits 0-1); and the ARM clock at 444 MHz while a job recognizes (on by
     * default; a clock already at 444 or more is left as it is; core/vocr_ocr.h
     * VjoVocrPool, busy). */
    int ocr_helpers;          /* 0..2 */
    int ocr_boost_clock;      /* 0/1 */
    /* (config.ini's ocr_settle_press_ms, a press's wait before #413, is still accepted, 0 or 100..10000, and read
     * by nothing since: docs/text-stability.md.) */
    /* Anki (API 23): a card saved with anki_mining keeps a JPEG of the game's screen
     * (on by default; the game worker takes it, docs/anki.md). */
    int anki_screenshot;      /* 0/1 */
    /* The left stick's lookup (#138, core/stick_cursor.h): a word the stick
     * moved the cursor to is looked up once the cursor has rested this long
     * (0: as soon as the stick stops); the d-pad and taps look up at once. */
    int stick_lookup_ms;      /* 0..1000 */
    /* Local OCR (vocr, API 26): the text-presence gate in front of the line finder
     * (vita-vn-ocr #100, core/vocr_ocr.h VjoVocrPolicy.gate): a region it says holds no
     * text (a hidden text box, a CG screen) returns no line. */
    int ocr_text_gate;        /* 0/1 */
    /* Native text hooks (API 31, audit-hooks F3): 0 (config.ini hook_sites = off) installs no table site in
     * any game, only the CALL source (the string copies), so a console run can tell the two apart; the
     * Shell sends it as VJO_TEXT_SOURCE_NO_SITES in the text source's word. */
    int hook_sites;           /* 0/1, on by default */
    /* Anki (API 32): a card's screenshot is of the picture between the frame's black bars, a
     * letterbox or pillarbox the game worker finds as it reads the frame (on by default;
     * core/shottrim.h, docs/anki.md "Black bars"). */
    int anki_screenshot_trim_bars; /* 0/1 */
    /* Reading statistics (the overlay's Triangle view, docs/ocr-history.md#reading-time): how a line's
     * reading time is measured, VJO_READING_* (core/history_lines.h): 0 adaptive (the default), 1 15s
     * (Rubi 0.9.7 and older). The Shell takes it once, as it starts. */
    int reading_time;
    /* Local OCR (vocr, API 42): vita-vn-ocr #117's speed work (core/vocr_ocr.h VjoVocrPolicy.head_gate, two_level),
     * VJO_HEAD_* bits: VJO_HEAD_GATE skips the recognizer's output head on the frames the model's gate calls blank,
     * VJO_HEAD_TWO_LEVEL computes a two-level blob's rare rows only on the frames its escape picks. config.ini
     * ocr_head_gate = on (both, the default) | gate | two_level | off (neither: the full head on every frame, as
     * before); the Shell sends the bits as they are (include/vjo_game_ocr.h VJO_GAME_OCR_S_HEAD_SHIFT). */
    int ocr_head_gate;        /* VJO_HEAD_* bits, 0-3 */
    /* Native text hooks (API 43, docs/native-hooks.md "Trusted hooks"): 1 (the default) lets a trusted hook (a Luna
     * or Rubi verified site, or a regular source after four OCR agreements) answer without OCR; 0 (hook_trust = off)
     * selects as Rubi 0.9.7 did. The Shell sends it as VJO_GAME_OCR_S_TRUST in each job's speed word. */
    int hook_trust;           /* 0/1, on by default */
    /* Vertical text (vita-vn-ocr #99, API 47; docs/vita-vn-ocr.md "Vertical text"): VJO_VERTICAL_*, the
     * request's VJO_GAME_OCR_S_VERTICAL field as it is (include/vjo_game_ocr.h). */
    int ocr_vertical;
    /* Local OCR (vocr, API 50): the line finder's fixes (bee-san/vita-vn-ocr #129; core/vocr_ocr.h
     * VjoVocrPolicy.line_fix): 1 (on, the default) its measured default, VOCR_FIX_DEFAULT (COLGAP); 2 (junk) also
     * the frame-strip rule, for regions drawn with a margin; 3 (all) also the touching-rows split; 0 (off) the
     * frozen finder. */
    int ocr_line_fix;         /* 0 off, 1 on, 2 junk, 3 all */
    /* Why the open overlay waits (#358; docs/text-stability.md "Why the overlay waits"): 1 (the default) has its try
     * after two jobs in a row that ended VJO_E_SCREEN_CHANGED (the text changed under every read) say why it still is
     * recognizing and what helps, instead of "Recognizing…"; 0 (ocr_wait_hint = off): "Recognizing…" all along, as
     * before. Read by the Shell only. */
    int ocr_wait_hint;        /* 0/1, on by default */
    /* Lookups (#321; docs/local-dictionaries.md "Names and words"): at one span, whether an entry spelled as the
     * text in a later dictionary (a character's name, わたし) comes before a word the first dictionary spells
     * otherwise (私): VJO_NAMES_*, auto (the default) puts the word first when it is among the 1,023 most
     * frequent. Read by the Shell's lookups only. */
    int names_first;
    /* Local OCR (vocr, API 54): the line finder of a job, VJO_FINDER_GRAD (Rubi's GRAD with the refinement and
     * the short-line probe, as every build before) or VJO_FINDER_SPOTTER (vita-vn-ocr's text spotter (#149, #128),
     * core/vocr_ocr.h VjoVocrPolicy.spot: its boxes are the lines; a job that cannot have its weights reads with
     * GRAD); and whether the spotter's boxes are refined (VOCR_REFINE_DEFAULT; GRAD's follow ocr_refine_lines). */
    int ocr_finder;           /* VJO_FINDER_* */
    int ocr_spotter_refine;   /* 0/1 */
    /* The pause for the frame (API 55, #348; docs/game-pause.md): 1 (ocr_pause = on) has each of the user's jobs copy
     * its region from one frame, in that frame's display call, which the game waits in for as long (at most
     * VJO_GAME_OCR_PAUSE_MAX_US), and read every row from that copy; 0 (off, the default) reads strip by strip, as
     * before. The Shell sends it as VJO_GAME_OCR_S_PAUSE in each job's speed word. */
    int ocr_pause;            /* 0/1, off by default */
} VjoConfig;

/* names_first's values, in ocr_vertical's order (off, on, auto: config.c parses both with one table), and its
 * default (the measurement: docs/local-dictionaries.md "Names and words"). */
#define VJO_NAMES_OFF 0
#define VJO_NAMES_ON 1
#define VJO_NAMES_AUTO 2
#define VJO_NAMES_DEFAULT VJO_NAMES_AUTO
#define VJO_NAMES_DEFAULT_TEXT "auto"

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
/* API 54: ocr_finder (config.ini grad | spotter) and ocr_spotter_refine (off | on), and their defaults
 * (docs/vita-vn-ocr.md, "The spotter finder": vita-vn-ocr #149's pick, measured on the page path, reads every
 * unseen-game set better than GRAD and is the default; GRAD stays its fallback and ocr_finder = grad the switch;
 * each default is one line here and its text the next). */
enum { VJO_FINDER_GRAD = 0, VJO_FINDER_SPOTTER = 1 };
/* ocr_finder's names by value (core/config.c; the Shell's game start line names the finder with them, #333) */
extern const char *const vjo_finder_names[];
#define VJO_FINDER_DEFAULT VJO_FINDER_SPOTTER
#define VJO_FINDER_DEFAULT_TEXT "spotter"
#define VJO_SPOTTER_REFINE_DEFAULT 0
#define VJO_SPOTTER_REFINE_DEFAULT_TEXT "off"

void vjo_config_defaults(VjoConfig *c);
/* Parses INI text over the defaults; invalid values keep the default and
 * add a warning. */
void vjo_config_parse(VjoConfig *c, const char *text, size_t len);
/* The commented default file: docs/config.example.ini, the installer's and the
 * package's template, and what the Rubi app's first save publishes when
 * there is no config.ini (settings/preferences.c). SceShell never writes it:
 * without a file the Shell plugin runs on vjo_config_defaults, the same values. */
const char *vjo_config_default_text(void);

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
 * feature, which the Rubi app's saves rewrite as they patch the file
 * (vjo_config_removed's kinds, by the line's first word): VJO_LINE_DROP for
 * the API keys of the removed online dictionaries and the removed Anki
 * settings (anki_audio_url's URL can hold a key), which are secrets, whatever
 * follows the name; VJO_LINE_DROP_SETTING for a removed setting's line with
 * '=', which is not (the engine choice and the UDP log: an ocr_backend or
 * log_host line, whatever its value); VJO_LINE_REPLACE with *replacement
 * ("dictionary = local\n") for dictionary = jpdb, jiten or hachidori, else
 * VJO_LINE_KEEP. */
enum { VJO_LINE_KEEP = 0, VJO_LINE_DROP = 1, VJO_LINE_REPLACE = 2, VJO_LINE_DROP_SETTING = 3 };
int vjo_config_removed_line(const char *line, size_t n, const char **replacement);

const char *vjo_trigger_name(int trigger);

/* config.ini's removed settings, one list (core/config.c removed_keys) for the parser, the Rubi app's saves and
 * install (settings/preferences.c, settings/install.c) and the installer from a computer (tools/migrate_config.py,
 * whose copy tools/tests/test_migrate_config.py checks against it). vjo_config_removed gives the entry of the n
 * bytes at s, a line's first word, in any case: its kind (VJO_REMOVED_KIND, below), then its name, a string; or
 * NULL for a word that names none. A kind in upper case is a prefix: a word that starts with its name is it. */
const char *vjo_config_removed(const char *s, size_t n);
#define VJO_REMOVED_KIND(entry) ((entry)[0] | 0x20)
#define VJO_REMOVED_SECRET 's'     /* the removed online dictionaries' API keys: a line that starts with one is never
                                      shown or kept, and named by this name alone */
#define VJO_REMOVED_ANKI 'a'       /* a removed Anki setting: its line goes whatever follows the name */
#define VJO_REMOVED_SETTING 'e'    /* the engine choice and the UDP log's computer: dropped by every save */
#define VJO_REMOVED_OLDER 'o'      /* an older setting: the saves keep its line, the install drops it */
#define VJO_REMOVED_MODEL 'm'      /* vocr_fallback_model: dropped by the four-preference save (vocr_tier) */
#define VJO_REMOVED_TIER 't'       /* vocr_model: as VJO_REMOVED_MODEL; the parser still reads its size */
#define VJO_REMOVED_DICTIONARY 'd' /* dictionary: a removed dictionary's value is saved as local */

#endif
