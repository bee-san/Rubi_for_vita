/* Styled text for ScePaf ui::Text widgets: UTF-16 text plus spans of size
 * and color, applied with Text::SetStyleAttribute. Sizes are in pixels and
 * text positions come from the widget itself (GetCharInfo/GetLineInfo), so
 * nothing here estimates layout. */
#ifndef VJO_STYLED_H
#define VJO_STYLED_H

#include <stdint.h>

#include "arena.h"
#include "entries.h"

#define VJO_RGB_TEXT      0xFFFFFFu
#define VJO_RGB_HIGHLIGHT 0xFFD24Au
#define VJO_RGB_READING   0x9FD0FFu
#define VJO_RGB_RANK      0x8A94A6u
#define VJO_RGB_DIM       0xB8C0CCu
#define VJO_RGB_ERROR     0xFF8A80u
#define VJO_RGB_OK        0x7CD992u /* in Anki */
/* The in-Anki mark: √ (JIS X 0208, in the system font; ✓ is not and
 * renders as "_"). */
#define VJO_ANKI_MARK     "√"

typedef struct {
    uint32_t start, len; /* UTF-16 units */
    uint32_t rgb;
    int px;
} VjoSpan;

/* The overlay's body text (shell/overlay.cpp, its UI arena): UTF-16 units and
 * spans. The host tests render every bundled entry within them. */
#define VJO_BODY_UNITS 4096
#define VJO_BODY_SPANS 96
/* The header's spans: one for the text; an earlier line (line history) adds its dimmed neighbours,
 * two spans each (the row and the line break between); the current text's source badge one (the
 * overlay shows none on an earlier line: two spans at most then). */
#define VJO_HEADER_SPANS 5

typedef struct {
    uint16_t *text;      /* UTF-16, NUL-terminated */
    uint32_t len, cap;
    VjoSpan *spans;
    int n_spans, cap_spans;
    int truncated;
} VjoStyled;

/* config font_size_ja/_en (8..40, Android sp as on a ~400 dpi phone) ->
 * pixels: ~1.5 px per sp gives a comparable size on the Vita's 220 dpi
 * screen. */
int vjo_font_px(int font_size);

/* Fixed capacity from the arena; text beyond it is dropped (truncated = 1).
 * Returns -1 on OOM. */
int vjo_styled_init(VjoStyled *s, VjoArena *a, uint32_t cap_units, int cap_spans);
/* Appends UTF-8 text (n bytes) as one span. */
void vjo_styled_add(VjoStyled *s, const char *utf8, size_t n, uint32_t rgb, int px);
void vjo_styled_puts(VjoStyled *s, const char *utf8, uint32_t rgb, int px);
/* Puts one UTF-16 unit before unit `at` (0 < at <= len), in the span of the
 * character before it (a break the overlay puts in: shell/wrap_fix.h).
 * 0 when the text has no room. */
int vjo_styled_insert(VjoStyled *s, uint32_t at, uint16_t unit);

/* UTF-16 index of byte offset `off` in UTF-8 text. */
uint32_t vjo_utf16_index(const char *utf8, size_t off);

/* Header: the OCR text in one span (highlights are applied separately). */
void vjo_styled_header(VjoStyled *s, const VjoEntryList *l, int ja_px);

/* Where the overlay's current text came from (VjoView.source_badge, config source_badge): not shown (the
 * badge turned off, no text read yet), recognized by OCR, or a native text hook's (VjoGameOcrResult's
 * text_from, docs/native-hooks.md); _ICON: the same in the system's symbols (source_badge = icons). */
enum { VJO_BADGE_NONE = 0, VJO_BADGE_OCR = 1, VJO_BADGE_HOOK = 2, VJO_BADGE_OCR_ICON = 4, VJO_BADGE_HOOK_ICON = 5 };
/* The badge of a text whose source is VJO_BADGE_OCR, VJO_BADGE_HOOK or VJO_BADGE_NONE (not known) in config
 * source_badge's style: 0 off (none), 1 on (the ASCII badges), 2 icons (their _ICON forms; 3, an unknown
 * source's, puts nothing too). */
#define VJO_BADGE_STYLED(style, source) ((style) ? (source) + 3 * ((style) - 1) : VJO_BADGE_NONE)

/* Header of the overlay's local text: the text in VJO_RGB_TEXT and, for an earlier line (line history,
 * docs/line-history.md), the last row of the line before it above it and the first row of the line
 * after it below, dimmed (VJO_RGB_RANK; NULL or "": none, as for the current text). Returns the units
 * before the text (a source badge put in first included): where the cursor's, taps' and highlight's
 * units start. */
uint32_t vjo_styled_history(VjoStyled *s, const char *before, const char *text, const char *after, int ja_px);

/* The current text's source badge (VJO_BADGE_*, config source_badge; the owner, 2026-10-21): "[HOOK]" or
 * "[OCR]", or with source_badge = icons the system's anchor (U+E1BB) or eye (U+E541) of its extra font
 * music_arib.pvf, drawn only if SceShell's PAF loaded that font (unverified: `_` otherwise); dimmed
 * (VJO_RGB_RANK, the frequency badge's colour) at px; VJO_BADGE_NONE (or a value that is no badge) puts
 * nothing. The overlay puts it first, right before vjo_styled_history's text and only for the current
 * text (an earlier line's source is not read back); no space follows it, and the overlay's re-wrap never
 * breaks right after it (shell/wrap_fix.h VjoWrap.keep), so Rubi never leaves it alone on a row. */
void vjo_styled_badge(VjoStyled *s, int badge, int px);

/* UTF-16 range of an entry's highlight in the header; 0 if it has none. */
int vjo_entry_header_range(const VjoEntryList *l, int entry, uint32_t *start, uint32_t *len);

/* Body: the selected entry — headword and reading at ja_px, then, with
 * show_rank (config frequency_badge), the frequency badge (vjo_rank_badge:
 * " #1234", " unranked" or nothing) and the meanings at en_px; a green
 * VJO_ANKI_MARK after the badge when in_anki. The text is shown as the
 * dictionary wrote it. An inflected occurrence adds a blue "Conjugation:"
 * line below the headword, its reasons in base-to-surface order; an exact
 * match has no such line. The definition follows it as usual. The
 * entry has no dictionary or source name of its own. Bee's
 * Ultimate Dictionary's kanji lines (vjo_kanji_block) end it, one a line,
 * "漢=meaning, meaning", each kanji at ja_px in VJO_RGB_TEXT (two spans a line;
 * past the spans left, as written, unstyled), after a blank line when a
 * definition comes before them (the newlines that end the definition are
 * shown as that one blank line). An entry longer than the room
 * left (s's cap; overlay.cpp lowers it by what follows the entry) is clipped
 * at the end of its definition, at a whole character, and marked "…": the
 * kanji lines stay whole. (A grammar summary comes first in the bundle's
 * entries, so it is kept.) */
void vjo_styled_entry(VjoStyled *s, const VjoEntryList *l, int selected, int ja_px, int en_px, int in_anki,
                      int show_rank);

/* Local paging: "L/R Dictionary 2 of 3" (page is 0-based) when pages > 1;
 * nothing otherwise. */
void vjo_styled_dict_page(VjoStyled *s, int page, int pages, int en_px);

#endif
