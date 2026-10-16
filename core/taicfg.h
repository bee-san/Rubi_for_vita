/* The game OCR worker's registration in a taiHEN configuration, as the
 * Overlay app adds it for an Adrenaline bubble: one *TITLEID section with the
 * worker, appended, never *ALL (AGENTS.md rule 2). The rules are taiHEN's own
 * parser's (DaveeFTW/taihen-parser 6d817fd, the one taiHEN 309b380 builds):
 *  - a line ends at CR or LF; leading and trailing white space is ignored; an
 *    empty line or one starting with '#' is a comment;
 *  - "*NAME" opens a section, "*!NAME" one that stops parsing at the next
 *    section when NAME matched; NAME is compared exactly; ALL matches every
 *    application; a NAME may appear several times;
 *  - parsing stops at the first line of 256 characters or more (lexer.c:132)
 *    and at a NUL byte (the text is read as a C string).
 * Pure functions over a bounded text; tools/patch_tai_config.py, the PC
 * installer, reads the same sections back as registered games. */
#ifndef VJO_TAICFG_H
#define VJO_TAICFG_H
#include <stddef.h>

#define VJO_TAI_WORKER "ur0:tai/VitaJPOverlay_OCR.suprx"
#define VJO_TAI_LINE_MAX 256 /* taihen-parser lexer.h CONFIG_MAX_LINE_LENGTH */

enum {
    VJO_TAI_ABSENT = 0,    /* not loaded for the title: an appended section would be read */
    VJO_TAI_PRESENT = 1,   /* a *TITLE section loads the worker */
    VJO_TAI_UNDER_ALL = 2, /* *ALL loads it (an old install): a second copy must not be added */
    VJO_TAI_BLOCKED = -1   /* not loaded, and parsing would stop before an appended section */
};
/* A title ID the worker may be registered for: nine capital letters and
 * digits, as tools/patch_tai_config.py's game_title requires (taiHEN README,
 * 'config.txt'). So never ALL or KERNEL, taiHEN's reserved sections, nor main
 * (SceShell), whatever folder the memory card has. */
int vjo_tai_title_ok(const char *id);

/* What the configuration text[0, len) does for title (a valid title ID). */
int vjo_tai_worker_state(const char *text, size_t len, const char *title);

/* The lines that register the worker for title, to append to text[0, len):
 * a comment, "*TITLE" and the worker, in the file's own line ends (CRLF when
 * it has any), after a line end of its own when the text does not end with
 * one. One more line end is added when the file would otherwise grow to a
 * multiple of 4096 bytes, since taiHEN reads the file into a buffer of that
 * rounded size and needs a NUL after it (plugin.c:95-96). The block's
 * length, or -1 when it does not fit in cap or vjo_tai_title_ok refuses title. */
int vjo_tai_worker_block(char *out, size_t cap, const char *text, size_t len, const char *title);
#endif
