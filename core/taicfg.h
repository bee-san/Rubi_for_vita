/* taiHEN's configuration (ux0:tai/config.txt, ur0:tai/config.txt): the
 * overlay's lines, edited the way taiHEN reads the file (docs/one-vpk-install.md,
 * "The config.txt editor"). taiHEN 309b380 builds DaveeFTW/taihen-parser 6d817fd,
 * vendored in third_party/taihen-parser, whose rules this follows:
 *  - a line ends at CR or LF only; its leading and trailing bytes of the six C
 *    spaces are ignored; an empty line or one starting with '#' is a comment;
 *  - "*NAME" opens a section, "*!NAME" one that stops the reading for its title
 *    at the next section header; NAME is compared exactly; ALL matches every
 *    title but KERNEL; any other line is a plugin path;
 *  - parsing stops at the first raw line of 256 bytes or more (lexer.c:127-135)
 *    and at a NUL (the text is read as a C string).
 * The editor's result is checked with taiHEN's own parser (vjo_tai_edit), and
 * tools/patch_tai_config.py, the PC installer's copy of these rules, writes the
 * same bytes (tools/tests/test_tai_config.py runs both on one set of fixtures).
 * Pure functions over bounded texts: no file access, no allocation. */
#ifndef VJO_TAICFG_H
#define VJO_TAICFG_H
#include <stddef.h>

#define VJO_TAI_KERNEL "ur0:tai/VitaJPOverlay_Kernel.skprx"
#define VJO_TAI_SHELL "ur0:tai/VitaJPOverlay_Shell.suprx"
#define VJO_TAI_WORKER "ur0:tai/VitaJPOverlay_OCR.suprx"
#define VJO_TAI_TEXT_PLUGIN "ur0:tai/VitaJPOverlay_Text.suprx" /* removed long ago; its lines go */
#define VJO_TAI_LINE_MAX 256          /* taihen-parser lexer.h CONFIG_MAX_LINE_LENGTH */
#define VJO_TAI_TEXT_MAX (64 * 1024)  /* the largest configuration the editor takes */
/* The prefix a line of ours gets when it is commented out (as every earlier installer wrote it). */
#define VJO_TAI_DISABLED "# Mainline OCR: "
/* The line before a section the editor adds; and #124's, before a section the Rubi app added for
 * an Adrenaline bubble. Either lets a later edit remove the section it marks. */
#define VJO_TAI_NOTE "# VitaJPOverlay (added by its installer)"
#define VJO_TAI_APP_NOTE "# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)"

enum { VJO_TAI_INSTALL = 0, VJO_TAI_UNINSTALL = 1 };
/* Where the game OCR worker comes from (docs/one-vpk-install.md, "Which games get the worker"):
 * the kernel plugin loads it (no worker line; the ones older installs left are removed), or taiHEN
 * loads it from a *TITLEID line per game (the rules before #138's Agent A). */
enum { VJO_TAI_WORKER_KERNEL = 0, VJO_TAI_WORKER_LINES = 1 };

/* Refusals (< 0); the report's why says what to do. */
enum {
    VJO_TAI_E_ARGS = -1,      /* a NULL, a mode or an op the editor does not know */
    VJO_TAI_E_TOO_LARGE = -2, /* over VJO_TAI_TEXT_MAX */
    VJO_TAI_E_NUL = -3,       /* a NUL byte: taiHEN stops reading there */
    VJO_TAI_E_LONG_LINE = -4, /* a raw line of 256 bytes or more: taiHEN stops reading there */
    VJO_TAI_E_EMPTY = -5,     /* nothing in it: taiHEN may not start with it */
    VJO_TAI_E_NO_SECTION = -6,/* no section and no path: taiHEN refuses the file and loads nothing */
    VJO_TAI_E_TITLE = -7,     /* lines mode: a title the worker cannot serve */
    VJO_TAI_E_SPACE = -8,     /* the result does not fit the output (or VJO_TAI_TEXT_MAX) */
    VJO_TAI_E_CHECK = -9      /* taiHEN would not load the result as it must (a *! section, say) */
};

typedef struct {
    int changed;          /* the result differs from the text */
    int added;            /* lines of ours added (the kernel, the Shell, lines mode's workers) */
    int sections_added;   /* sections added at the end (with their note) */
    int commented;        /* lines of ours commented out */
    int removed;          /* lines of ours removed */
    int headers_removed;  /* emptied section headers removed (with the note or blank line before) */
    int padded;           /* one line end added: the result's length was a multiple of 4,096 */
    int bom_hides_section;/* the first line is a section header behind a byte-order mark */
    int line;             /* a refusal's line (1-based), else 0 */
    char removed_sections[96]; /* the sections whose worker line went, comma-separated (cut with "...") */
    char why[224];        /* a refusal: what is wrong and what to do */
} VjoTaiReport;

/* How taiHEN loads the overlay's plugins for one title in a text (NUL-terminated): each count is
 * how many of its lines taiHEN loads, in any spelling (core/taicfg.c ours()). */
typedef struct {
    int kernel, shell, worker, text;
} VjoTaiLoads;

/* A title ID the worker may be registered for in lines mode: nine capital letters and digits, as
 * tools/patch_tai_config.py's game_title requires (taiHEN README, 'config.txt'). */
int vjo_tai_title_ok(const char *id);

/* Edits text[0, len) (any bytes but NUL) for op in mode (titles: lines mode's games to add, any
 * case, NULL when none) into out (cap bytes, NUL-terminated). Returns the result's length, or a
 * VJO_TAI_E_* with r->why (nothing to write). The result keeps every byte of the text that is not
 * a line of ours, each line's own line end, and loads every other plugin for exactly the titles
 * and in the order the text did; taiHEN's parser checks that, and that it loads the kernel plugin
 * once for KERNEL, the Shell once for main and the worker as the mode says (none of them after
 * VJO_TAI_UNINSTALL). Editing the result again gives the same bytes. */
int vjo_tai_edit(const char *text, size_t len, int op, int mode, const char *const *titles, int ntitles,
                 char *out, size_t cap, VjoTaiReport *r);

/* What taiHEN loads of ours for title in text (NUL-terminated; taiHEN's own parser). */
void vjo_tai_loads(const char *text, const char *title, VjoTaiLoads *out);

/* text[0, len) has a kernel line of ours that is not a comment (wherever it is): a backup without
 * one is a configuration from before the overlay. */
int vjo_tai_has_kernel_line(const char *text, size_t len);

/* The storage plugin (StorageMgr, gamesd, YAMT) a *KERNEL line of text[0, len) loads, into name
 * (its file name), or 0: with one, ux0 is the SD card only once it ran, so taiHEN reads ur0's
 * configuration at boot. */
int vjo_tai_storage_plugin(const char *text, size_t len, char *name, size_t cap);
#endif
