/* Full-screen ScePaf overlay: header (OCR text, selected word highlighted),
 * separator, the selected word's definition, and the touch region selector.
 * Runs entirely on the paf main thread via MainThreadCallList.
 *
 * Header and body are ui::Text widgets in a fixed-size box inside a
 * ui::ScrollView (as in GrapheneCt's NetStream, the text takes the box's
 * width and grows to fit). The view only clips: its content is the box, which
 * never changes size, so the view never re-centres or eases the text (it did
 * both when the Text was its direct child). Sizes and colors are set per
 * range with SetStyleAttribute (in pixels), and the Text reports where text
 * is (GetCharInfo/GetLineInfo), so nothing is estimated. Moving the
 * selection only recolors two ranges of the header. Each Text is placed
 * inside its box every frame from its laid-out size: paf positions are
 * centre-based and y-up, so the text's top sits at the box's top when
 * pos.y = (box height - text height) / 2, plus the scroll offset. ScrollView
 * DoSnap and Text GetNextDownHit are not used: pressing ▼ with them crashed
 * the console.
 *
 * Subtitles: a second page, the strip (strip.cpp), shows while the overlay
 * is closed.
 *
 * Touch: SceTouch routes the front panel by region, and while a game is in
 * front SceShell's regions are inactive. The open page activates SceShell's
 * default touch region (as SceShell's own menus over a game must) and reads
 * the touches through paf's input device: header taps and the region
 * selector. Closing gives it back as SceShell wants it for what is in front
 * (touch_hold.h, #104): deactivated over the game, untouched once the game
 * has left the foreground to SceShell's LiveArea. The selector is also drawn
 * with the left stick and the d-pad (#81): a cursor, enter for each
 * corner (core/region_select.c holds the state; this file draws it).
 *
 * Over the recognized text the left stick moves the cursor in the arrows' four
 * directions, faster while held and capped, and its word is looked up only
 * once the cursor rests there (#138, core/stick_cursor.h); the d-pad and taps
 * look up at once. The right stick scrolls a long definition. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "paf_ui.h"
#include "header_highlight.h"
#include "touch_hold.h"
#include "word_navigation.h"
#include "wrap_fix.h"
extern "C" {
#include "../core/utf.h"
#include "../core/text_hit.h"
#include "../include/vjo_ledger_budget.h"
}

#include "../core/region_select.h"
#include "../core/stick_cursor.h"
#include "../vita/buttons_vita.h"
/* The region selector and the enter/cancel buttons take the pad's own button bits. */
static_assert(VJO_RSEL_UP == SCE_CTRL_UP && VJO_RSEL_RIGHT == SCE_CTRL_RIGHT && VJO_RSEL_DOWN == SCE_CTRL_DOWN &&
              VJO_RSEL_LEFT == SCE_CTRL_LEFT && VJO_BUTTON_CIRCLE == SCE_CTRL_CIRCLE &&
              VJO_BUTTON_CROSS == SCE_CTRL_CROSS, "region selector buttons are SCE_CTRL_* bits");
/* The console's enter and cancel buttons: read at start and whenever the
 * overlay opens (zero until then; nothing reads them before). Not a dynamic
 * initializer: this module has no C++ runtime to run one. */
static VjoButtons s_btn;

static void read_buttons(void)
{
    s_btn = vjo_buttons_read();
    __atomic_store_n(&g_view.enter_circle, s_btn.enter == VJO_BUTTON_CIRCLE, __ATOMIC_RELEASE);
}

using namespace paf;

/* The paf text info classes have virtual destructors, whose deleting
 * variants reference sized delete (no C++ runtime is linked; these objects
 * live on the stack). */
void operator delete(void *p, unsigned int)
{
    sce_paf_free(p);
}

/* Layout (paf coordinates: origin at the screen centre, y up). The header
 * fits its text, 1..HEADER_MAX_LINES lines; the separator and the body follow
 * it, and the body keeps its bottom (shell/rco/vitajpoverlay.xml holds the
 * initial sizes: header 128, body 320). */
#define HEADER_TOP 260.0f
#define HEADER_MAX_LINES 3
#define HEADER_SEP_GAP 19.0f /* header bottom -> separator */
#define SEP_BODY_GAP 17.0f   /* separator -> body top */
#define BODY_BOTTOM -224.0f
#define HEADER_H_INITIAL 128.0f
#define BODY_H_INITIAL 320.0f
#define HEADER_UNITS 4096 /* UTF-16 units */
#define BODY_UNITS VJO_BODY_UNITS
#define MAX_SPANS VJO_BODY_SPANS
#define STICK_DEADZONE 40
#define STICK_SCROLL_PX 0.12f /* per frame per unit of stick deflection */
#define LAYOUT_RETRY_FRAMES 6 /* layout may land a frame or two after SetString */
#define CLEAR_HOLD_US 1000000
#define MAX_RANGES VJO_MAX_ENTRIES /* entries whose header highlight range is cached */

enum { MODE_OVERLAY = 0, MODE_REGION = 1 };

static Plugin *s_plugin;
static ui::Scene *s_page;
/* A Text in a fixed-size box inside a clipping ScrollView (see the top). */
struct Pane {
    ui::Widget *view, *box;
    ui::Text *text;
    float h;       /* box height */
    float scroll;  /* content y at the box top */
    float applied; /* last pos.y given to the Text; NOT_PLACED = place afresh */
};
#define NOT_PLACED (-1e9f)
static Pane s_header = {NULL, NULL, NULL, HEADER_H_INITIAL, 0.0f, NOT_PLACED};
static Pane s_body = {NULL, NULL, NULL, BODY_H_INITIAL, 0.0f, NOT_PLACED};
static ui::Widget *s_separator, *s_band_mid;
static ui::RichText *s_hint, *s_region_hint;
static ui::Widget *s_panel, *s_region_layer, *s_region_rect;

static unsigned s_seen_version, s_seen_anki_version;
static unsigned s_list_seq; /* the shown list (Anki requests name it) */
static int s_anki_enabled;  /* enter saves the shown word to the Anki queue */
static int s_selected;
/* Copied from the view under its lock, so nothing outside render() reads
 * the result memory (the control thread frees it when a game exits) and no
 * paf call runs while the lock is held. */
static int s_n_entries;
static uint32_t s_rng_start[MAX_RANGES], s_rng_len[MAX_RANGES]; /* len 0 = no highlight */
/* Local lookups select a source character, including one with no entry.
 * This owned copy survives lookup-result arena resets and never borrows
 * a worker pointer outside the view lock. */
static char s_cursor_text[VJO_STRIP_MAX];
static size_t s_cursor_text_len;
static int s_local_cursor_mode, s_header_has_text;
static int s_japanese_only_selection;
static int s_highlight_mode;
static uint32_t s_cursor_byte, s_text_generation;
static uint32_t s_match_end; /* raw UTF-8 end of the current matched source word */
static int s_dict_page, s_dict_pages; /* local: shown/requested dictionary page, pages at the cursor */
static VjoRowNav s_nav; /* the arrow keys: Right's history, Up/Down's column, counts */
static uint32_t s_tap; /* the L or R being tapped: dictionary pages */
static int s_ja_px;    /* the header's point size: a row's last cell */

/* logged when the overlay closes: the debug build's counts and timings
 * (docs/release-variants.md); the normal build keeps none */
#if VJO_DEBUG
static int64_t s_frame_max_us, s_key_max_us; /* slowest frame; slowest arrow key's row walk */
static int s_frames, s_moves, s_stick_max; /* largest stick deflection from centre (either stick) */
#define COUNT_MOVE() s_moves++
#else
#define COUNT_MOVE() ((void)0)
#endif
static int s_mode;
static uint32_t s_prev_buttons;
static int s_prev_touch;

/* header state */
static uint32_t s_hl_start, s_hl_len; /* highlighted range now drawn */
static bool s_hl_dirty;              /* style writes awaiting a successful refresh */
static int s_follow_frames;           /* frames left to scroll the header to the word */
static int s_highlight_retry_frames;  /* bounded retries while text layout catches up */

/* region selection (screen pixels) */
static VjoTouchHold s_touch; /* SceShell's default front touch region, activated by the page (touch_hold.h) */
static VjoRegionSelect s_rsel;
static ui::Widget *s_region_cursor;
static int s_region_drawn[6];          /* cursor x, y; box x, y, w, h (w 0: none) as drawn */
static int s_hint_box[3];              /* the region hint's corner set, w, h */
static int s_refused[2];               /* the box size enter was refused for: too small */
static int64_t s_hint_us;
static int64_t s_square_down_us;
static int s_square_clear_armed;

/* The text-building arena, taken from the PAF heap while the overlay page is
 * open and given back when it closes, so no page of SceShell's partition is
 * held for a page that is closed almost all the time (#59; it was a 96 KiB
 * static buffer). Its size is what render() allocates from it: two styled
 * texts of HEADER_UNITS/BODY_UNITS UTF-16 units plus their spans, 17,968 B,
 * rounded up to 24 KiB (host test: test_overlay_ui_arena_fits). A refused
 * allocation means the page does not open this time (fail closed). */
#define UI_MEM_BYTES VJO_BUDGET_PAF_UI
static uint8_t *s_ui_mem;
static VjoArena s_ui;

/* paf breaks Japanese anywhere; a break it put where core/linebreak.h forbids is moved to the
 * last place the line may break (wrap_fix.h, core/linebreak.h), a break a
 * frame once the text is laid out. The shown texts stay in s_ui until the
 * next render, the header's across the renders that keep it (s_hdr_mark).
 * The header's breaks are kept as the recognized text's units they come
 * before: the cursor, taps, arrows and the highlight count the text's own
 * units (header_char_position, header_highlight). */
static VjoStyled s_hdr_st, s_body_st;
static VjoWrap s_hdr_wrap, s_body_wrap;
static size_t s_hdr_mark;
static uint32_t s_hdr_brk[VJO_WRAP_BREAKS];
static int s_hdr_nbrk;

/* The step of a paf text's re-wrap, for the overlay's texts and the strip's. */
uint32_t vjo_wrap_paf(VjoWrap *w, ui::Text *t, const uint16_t *u, uint32_t n, uint32_t room)
{
    return vjo_wrap_step<graph::TextLayoutCharInfo>(w, t, u, n, room);
}

/* A header unit of the recognized text as shown, the breaks put in. */
static __attribute__((noinline)) uint32_t header_shown(uint32_t unit)
{
    return vjo_wrap_shown(s_hdr_brk, s_hdr_nbrk, unit);
}

/* The left stick over the text: its state, its due lookup, stick_lookup_ms and the
 * close line's counts (core/stick_cursor.h). */
static VjoStickCursor s_stick;

static void set_rich(ui::RichText *w, const char *s)
{
    if (w)
        w->SetCText(s, sce_paf_strlen(s));
}

/* Places the pane's Text so that content y = scroll is at the box's top.
 * The widget takes the box's width (adjust 1) with the text left-aligned in
 * it, so x stays 0; its height is the laid-out height (adjust 2) and paf
 * positions are centre-based, so y comes from that height. Runs every frame
 * (cheap) and only calls SetPos on a change. */
static void pane_place(Pane *p)
{
    float h = text_height(p->text), y;
    if (!p->text || h <= 0.0f)
        return;
    y = (p->h - h) / 2.0f + p->scroll;
    if (y != p->applied) {
        p->applied = y;
        p->text->SetPos(0.0f, y, 0.0f);
    }
}

/* Scrolls to content y, kept within the laid-out text. */
static void pane_scroll_to(Pane *p, float y)
{
    float max = text_height(p->text) - p->h;
    if (y > max)
        y = max;
    if (y < 0.0f)
        y = 0.0f;
    p->scroll = y;
}

/* New text or a new box: scrolled to the top and placed afresh. */
static void pane_reset(Pane *p)
{
    p->scroll = 0.0f;
    p->applied = NOT_PLACED;
}

/* Sizes the view and its box (same size: the view only clips) with the
 * given top edge; the Text is placed afresh in the new box. */
static void pane_size(Pane *p, float h, float top)
{
    p->h = h;
    if (p->view) {
        p->view->SetSize(BOX_W, h, 0.0f);
        p->view->SetPos(0.0f, top - h / 2.0f, 0.0f);
    }
    if (p->box)
        p->box->SetSize(BOX_W, h, 0.0f);
    p->applied = NOT_PLACED;
}

/* Recolors the selected character/word: the old range back to white, the new one
 * yellow. Preserve the string and scroll position, but refresh the drawing. */
static bool header_highlight(void)
{
    uint32_t start = 0, len = 0;
    if (s_local_cursor_mode) {
        if (s_highlight_mode == VJO_HIGHLIGHT_WORD && s_n_entries && s_rng_len[0]) {
            start = s_rng_start[0];
            len = s_rng_len[0];
        } else if (s_cursor_byte < s_cursor_text_len)
            vjo_utf8_cursor_range(s_cursor_text, s_cursor_text_len, s_cursor_byte, &start, &len);
        if (len) { /* the text's units as shown, with the breaks put in */
            uint32_t end = header_shown(start + len - 1) + 1;
            start = header_shown(start);
            len = end - start;
        }
    } else if (s_selected < s_n_entries && s_selected < MAX_RANGES && s_rng_len[s_selected]) {
        start = s_rng_start[s_selected];
        len = s_rng_len[s_selected];
    }
    if (!vjo_header_recolor(s_header.text, graph::TextStyleAttribute_Color,
                            s_hl_start, s_hl_len, s_hl_dirty, start, len,
                            rgba(VJO_RGB_TEXT), rgba(VJO_RGB_HIGHLIGHT)))
        return false;
    s_follow_frames = LAYOUT_RETRY_FRAMES;
    return true;
}

static void update_header_highlight(void)
{
    s_highlight_retry_frames = header_highlight() ? 0 : LAYOUT_RETRY_FRAMES;
}

/* Scrolls the header just enough to show the highlighted word's whole line.
 * The line comes from the Text (GetCharInfo); the line height is the laid
 * out height over the line count (the line frame's fields turned out not to
 * be x, y, w, h: on the device line 0 was "0, 0, 0, 746" for a 746 px line).
 * Returns 1 when done. */
static int header_follow(void)
{
    graph::TextLayoutCharInfo ci;
    uint32_t lines = 0;
    float height, lh, top, scroll = s_header.scroll;
    ui::Text *t = s_header.text;
    if (!t || !s_hl_len)
        return 1;
    if (t->GetCharInfo(s_hl_start, ci) < 0 || t->GetLineCount(lines) < 0 || !lines)
        return 0; /* not laid out yet */
    height = text_height(t);
    if (height <= 0.0f)
        return 0;
    lh = height / (float)lines;
    top = (float)ci.line * lh;
    if (top + lh > scroll + s_header.h)
        scroll = top + lh - s_header.h;
    if (top < scroll)
        scroll = top;
    pane_scroll_to(&s_header, scroll);
    return 1;
}

/* new_content: a new result (header text rebuilt and scrolled to the top);
 * otherwise only the selection moved. The text is built from the view under
 * its lock; the paf calls happen after it is released. */
/* Keep previews readable without changing the captured source. Collapse
 * line/control whitespace and omit invisible formatting; bound by complete
 * codepoints, rather than cutting a Japanese sentence after 80 bytes. */
static void render(int new_content)
{
    VjoStyled hdr, body;
    int ja, en, have_hdr = 0, have_body = 0, rebuild_header, cursor_matches = 1;
    const VjoEntryList *l;

    font_px(&ja, &en);
    s_ja_px = ja;
    vjo_view_lock();
    l = g_view.list;
    s_highlight_mode = g_view.highlight_mode;
    s_stick.rest_us = (uint32_t)g_view.stick_lookup_ms * 1000u;
    if (s_highlight_mode != VJO_HIGHLIGHT_WORD)
        s_nav.words.count = 0;
    s_match_end = 0;
    rebuild_header = new_content || !s_header_has_text;
    if (g_view.local_cursor_mode) {
        int new_text = !s_local_cursor_mode || s_text_generation != g_view.text_generation;
        int copy_text = new_text || (!s_cursor_text_len && l && l->header && l->header[0]);
        /* A lookup publication changes list_seq/version, not the sentence
         * or requested cursor. Keep header scroll and selection in place. */
        rebuild_header = new_text || copy_text || !s_header_has_text;
        s_local_cursor_mode = 1;
        if (copy_text) {
            size_t n = 0;
            if (l && l->header) {
                while (n + 1 < sizeof(s_cursor_text) && l->header[n]) {
                    s_cursor_text[n] = l->header[n];
                    n++;
                }
            }
            s_cursor_text[n] = 0;
            s_cursor_text_len = n;
        }
        if (new_text || copy_text) {
            s_nav.words.count = 0;
            s_nav.goal.at = 0;
            vjo_stick_cursor_new_text(&s_stick); /* a due word was in the text this replaces */
            s_text_generation = g_view.text_generation;
            s_cursor_byte = (uint32_t)(g_view.japanese_only_selection
                ? vjo_utf8_japanese_cursor_step(s_cursor_text, s_cursor_text_len, g_view.cursor_byte, 0)
                : vjo_utf8_cursor_step(s_cursor_text, s_cursor_text_len, g_view.cursor_byte, 0));
        }
        s_japanese_only_selection = g_view.japanese_only_selection;
        cursor_matches = g_view.cursor_byte == s_cursor_byte;
        vjo_stick_cursor_shown(&s_stick, cursor_matches); /* back on the shown word: nothing to look up */
        s_dict_page = g_view.dict_page;
        s_dict_pages = cursor_matches ? g_view.dict_pages : 0;
    } else {
        rebuild_header |= s_local_cursor_mode;
        s_local_cursor_mode = 0;
        s_cursor_text_len = 0;
        s_cursor_text[0] = 0;
        s_dict_page = s_dict_pages = 0;
    }
    s_n_entries = l ? l->n_entries : 0;
    if (s_local_cursor_mode) {
        s_n_entries = cursor_matches && s_n_entries ? 1 : 0;
        s_selected = 0;
        /* Copy only the current result's range while its arena is locked.
         * An in-flight or unsuccessful lookup falls back to the character. */
        if (!s_n_entries || !vjo_entry_header_range(l, 0, &s_rng_start[0], &s_rng_len[0]))
            s_rng_len[0] = 0;
        if (s_n_entries && l->entries[0].hl_start == (int)s_cursor_byte &&
            l->entries[0].hl_end > (int)s_cursor_byte &&
            l->entries[0].hl_end <= (int)s_cursor_text_len)
            s_match_end = (uint32_t)l->entries[0].hl_end;
    }
    s_list_seq = g_view.list_seq;
    s_anki_enabled = g_view.anki_enabled;
    if (s_selected >= s_n_entries)
        s_selected = s_n_entries ? s_n_entries - 1 : 0;
    if (rebuild_header)
        vjo_arena_reset(&s_ui);
    else
        vjo_arena_release(&s_ui, s_hdr_mark); /* the header's shown text stays */

    if (rebuild_header) {
        s_hdr_nbrk = 0;
        s_hdr_wrap.frames = 0;
        if (vjo_styled_init(&hdr, &s_ui, HEADER_UNITS, 1) == 0) {
            if (s_local_cursor_mode)
                vjo_styled_puts(&hdr, s_cursor_text, VJO_RGB_TEXT, ja);
            else
                vjo_styled_header(&hdr, l, ja);
            have_hdr = 1;
            s_hdr_st = hdr;
            if (s_local_cursor_mode) /* the cursor's text, whose units are mapped */
                vjo_wrap_arm(&s_hdr_wrap);
        }
        s_hdr_mark = vjo_arena_mark(&s_ui);
        if (!s_local_cursor_mode)
            for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++)
                if (!vjo_entry_header_range(l, i, &s_rng_start[i], &s_rng_len[i]))
                    s_rng_len[i] = 0;
    }

    /* body: page, the app notice, status, the selected entry (or "no entries"), config warnings */
    if (vjo_styled_init(&body, &s_ui, BODY_UNITS, MAX_SPANS) == 0) {
        have_body = 1;
        vjo_styled_dict_page(&body, s_dict_page, s_dict_pages, en);
        if (const char *notice = vjo_view_notice()) { /* the Rubi app is not the plugins' (once) */
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, notice, VJO_RGB_ERROR, en);
        }
        if ((!cursor_matches || g_view.status[0]) && body.len)
            vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
        if (!cursor_matches)
            vjo_styled_puts(&body, "Looking up words…", VJO_RGB_DIM, en);
        else if (g_view.status[0])
            vjo_styled_puts(&body, g_view.status, g_view.status_is_error ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        if (s_anki_enabled) {
            /* the offline queue: what the Rubi app exports */
            char queue[80];
            if (g_view.anki_pending == -1)
                sceClibSnprintf(queue, sizeof(queue), "Anki queue unavailable: check ux0 storage");
            else if (g_view.anki_pending < 0)
                sceClibSnprintf(queue, sizeof(queue), "Anki queue · export in the Rubi app");
            else
                sceClibSnprintf(queue, sizeof(queue), "Anki queue: %d · export in the Rubi app",
                                g_view.anki_pending);
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            vjo_styled_puts(&body, queue, g_view.anki_pending == -1 ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        }
        if (g_view.anki_status[0] && (g_view.anki_status_seq == 0 || g_view.anki_status_seq == s_list_seq)) {
            /* by VJO_ANKI_STATUS_* */
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            vjo_styled_puts(&body, g_view.anki_status,
                            g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        }
        if (s_n_entries) {
            int mark = g_view.anki_marks_seq == s_list_seq && s_selected < VJO_MAX_ENTRIES
                       ? g_view.anki_mark[s_selected] : VJO_ANKI_MARK_NONE;
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            /* The config warnings below keep their room, at their longest: a
             * long entry is clipped in its definition instead (vjo_styled_entry;
             * the fullest body with an entry of the bundle takes at most 66 of
             * the 96 spans, host test every_bundled_entry_fits_the_popup). */
            uint32_t room = (uint32_t)g_view.n_warnings * (1 + 12 + sizeof(g_view.warnings[0]));
            body.cap -= room;
            vjo_styled_entry(&body, l, s_selected, ja, en, VJO_ANKI_MARK_TICKED(mark), g_view.frequency_badge);
            body.cap += room;
        } else if (l && !g_view.status[0] && cursor_matches) {
            vjo_styled_puts(&body, s_local_cursor_mode
                ? (s_japanese_only_selection && s_cursor_byte >= s_cursor_text_len
                    ? "No Japanese characters in this text."
                    : "No dictionary entry starts at this character.")
                : "No dictionary entries for this text.", VJO_RGB_DIM, en);
        }
        for (int i = 0; i < g_view.n_warnings; i++) {
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, "config.ini: ", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, g_view.warnings[i], VJO_RGB_ERROR, en);
        }
    }
    vjo_view_unlock();

    /* Dictionary paging is taught by the body's "L/R Dictionary 2 of 3" line,
     * shown only while paging is available. The longest hint, Character mode
     * with Japanese-only and Anki, is 82 characters with the sticks' parts (#138;
     * 72 before): whether it fits the 920 px line is for the console run. */
    {
        char hint[192];
        const char *nav = s_local_cursor_mode
            ? (s_highlight_mode == VJO_HIGHLIGHT_WORD ? "Tap / ◀▶▲▼ / stick word"
               : s_japanese_only_selection ? "Tap / ◀▶▲▼ / stick Japanese letter" : "Tap / ◀▶▲▼ / stick character")
            : "◀ ▶ ▲ ▼ word";
        char anki[32] = "";
        if (s_anki_enabled)
            sceClibSnprintf(anki, sizeof(anki), "%s queue · ", s_btn.enter_glyph);
        sceClibSnprintf(hint, sizeof(hint),
                        "<font color=\"#8a94a6\">%s · %sR stick scroll · □ region · %s close</font>", nav, anki,
                        s_btn.cancel_glyph);
        set_rich(s_hint, hint);
    }
    if (rebuild_header) {
        if (have_hdr) {
            text_set(s_header.text, &hdr);
            s_header_has_text = 1;
        }
        pane_reset(&s_header);
        s_hl_len = 0;
        s_hl_dirty = false;
    }
    update_header_highlight();
    if (have_body) {
        text_set(s_body.text, &body);
        s_body_st = body;
        vjo_wrap_arm(&s_body_wrap);
    } else
        s_body_wrap.frames = 0;
    pane_reset(&s_body);
}

/* A shown text's step of its re-wrap (wrap_fix.h): a break put in where paf
 * broke where core/linebreak.h forbids, and the text set again. 1 when one was. */
static int wrap_step(VjoWrap *w, ui::Text *t, VjoStyled *st)
{
    uint32_t b = vjo_wrap_paf(w, t, st->text, st->len, st->cap - st->len);
    if (!b || !vjo_styled_insert(st, b, '\n'))
        return 0;
    vjo_wrap_put(w, b);
    text_set(t, st);
    return 1;
}

static void wrap_frame(void)
{
    uint32_t b = wrap_step(&s_hdr_wrap, s_header.text, &s_hdr_st) ? s_hdr_wrap.last : 0;
    if (b) {
        vjo_wrap_add(s_hdr_brk, &s_hdr_nbrk, b);
        s_hl_len = 0; /* the text was set again: no highlight drawn */
        s_hl_dirty = false;
        update_header_highlight();
    }
    if (wrap_step(&s_body_wrap, s_body.text, &s_body_st))
        pane_scroll_to(&s_body, s_body.scroll);
}

static void layout_apply(float header_h)
{
    float sep = HEADER_TOP - header_h - HEADER_SEP_GAP;
    float body_top = sep - SEP_BODY_GAP;
    pane_size(&s_header, header_h, HEADER_TOP);
    pane_size(&s_body, body_top - BODY_BOTTOM, body_top);
    /* the mid band covers the gap between the boxes, centred on the separator */
    if (s_separator)
        s_separator->SetPos(0.0f, sep, 0.0f);
    if (s_band_mid)
        s_band_mid->SetPos(0.0f, sep, 0.0f);
    if (s_hl_len)
        s_follow_frames = LAYOUT_RETRY_FRAMES;
}

/* Fits the header to its text: 1..HEADER_MAX_LINES laid-out lines. Cheap
 * (two queries); applies only on a change. */
static void header_fit(void)
{
    uint32_t lines = 0;
    float h = text_height(s_header.text), want;
    if (!s_header.text || h <= 0.0f || s_header.text->GetLineCount(lines) < 0 || !lines)
        return;
    want = h / (float)lines * (float)(lines < HEADER_MAX_LINES ? lines : HEADER_MAX_LINES);
    if (want > HEADER_H_INITIAL)
        want = HEADER_H_INITIAL;
    want = (float)(int)(want + 0.5f);
    if (want != s_header.h) {
        layout_apply(want);
        pane_scroll_to(&s_body, s_body.scroll); /* keep the body scroll in range */
    }
}

/* The selector's cursor and box (paf layout: origin at the screen centre,
 * y up); paf is told only what changed. */
static void show_region(void)
{
    int c[6], same = 1;
    vjo_rsel_cursor(&s_rsel, &c[0], &c[1]);
    if (!vjo_rsel_box(&s_rsel, &c[2], &c[3], &c[4], &c[5]))
        c[2] = c[3] = c[4] = c[5] = 0;
    for (int i = 0; i < 6; i++) {
        same &= c[i] == s_region_drawn[i];
        s_region_drawn[i] = c[i];
    }
    if (same)
        return;
    if (s_region_cursor) /* its centre on the cursor pixel's centre */
        s_region_cursor->SetPos(c[0] + 0.5f - SCREEN_W / 2, SCREEN_H / 2 - 0.5f - c[1], 0.0f);
    if (!s_region_rect)
        return;
    if (c[4] < 2 || c[5] < 2) {
        s_region_rect->Hide();
        return;
    }
    s_region_rect->SetSize((float)c[4], (float)c[5], 0.0f);
    s_region_rect->SetPos(c[2] + c[4] / 2.0f - SCREEN_W / 2, SCREEN_H / 2 - c[3] - c[5] / 2.0f, 0.0f);
    s_region_rect->Show();
}

/* What enter and cancel do next, and the box's size: under the 32 x 32 a region
 * needs it says so, red once enter was refused. A changing size is redrawn at
 * most ten times a second. */
static void region_hint(int force)
{
    int x, y, w = 0, h = 0, corner = vjo_rsel_box(&s_rsel, &x, &y, &w, &h);
    int64_t now = sceKernelGetProcessTimeWide();
    char hint[192]; /* the longest is 144 B */
    if (!force && corner == s_hint_box[0] &&
        ((w == s_hint_box[1] && h == s_hint_box[2]) || now - s_hint_us < 100000))
        return;
    s_hint_box[0] = corner;
    s_hint_box[1] = w;
    s_hint_box[2] = h;
    s_hint_us = now;
    if (!corner)
        sceClibSnprintf(hint, sizeof(hint), "<font color=\"#ffffff\">Stick / ◀▶▲▼ move · %s first corner · "
                                            "or drag on screen · %s cancel · hold □ full screen</font>",
                        s_btn.enter_glyph, s_btn.cancel_glyph);
    else if (w < VJO_RSEL_MIN || h < VJO_RSEL_MIN)
        sceClibSnprintf(hint, sizeof(hint), "<font color=\"%s\">Box %d x %d (min. %d x %d)</font>"
                        "<font color=\"#ffffff\"> · stick / ◀▶▲▼ opposite corner · %s undo corner</font>",
                        w == s_refused[0] && h == s_refused[1] ? "#ff8a80" : "#b8c0cc", w, h, VJO_RSEL_MIN,
                        VJO_RSEL_MIN, s_btn.cancel_glyph);
    else
        sceClibSnprintf(hint, sizeof(hint), "<font color=\"#ffd24a\">Box %d x %d</font><font color=\"#ffffff\">"
                        " · stick / ◀▶▲▼ opposite corner · %s save · %s undo corner</font>", w, h,
                        s_btn.enter_glyph, s_btn.cancel_glyph);
    set_rich(s_region_hint, hint);
}

/* Makes a write of touch_hold.h's rules: front touches go to SceShell while
 * the page holds the region (and not to the game). */
static void touch_write(int action)
{
    int rc;
    if (action == VJO_TOUCH_NO_WRITE)
        return;
    rc = inputdevice::touchscreen::ActivateDefaultRegion(inputdevice::touchscreen::PORT_FRONT,
                                                         action == VJO_TOUCH_ACTIVATE);
    vjo_touch_written(&s_touch, action, rc);
#if VJO_DEBUG
    vjo_log("touch region %s: 0x%X t=%u ms", action == VJO_TOUCH_ACTIVATE ? "on" : "off", rc, vjo_log_ms());
#else
    if (rc < 0) /* the normal build logs a refusal only */
        vjo_log("touch region %s refused 0x%X", action == VJO_TOUCH_ACTIVATE ? "on" : "off", rc);
#endif
}

static void enter_region_mode(void)
{
    s_mode = MODE_REGION;
    touch_write(vjo_touch_hold(&s_touch));
    s_square_down_us = sceKernelGetProcessTimeWide();
    s_square_clear_armed = 1;
    vjo_rsel_begin(&s_rsel, s_square_down_us);
    if (s_panel)
        s_panel->Hide();
    if (s_region_layer)
        s_region_layer->Show();
    s_region_drawn[0] = -1; /* draw afresh */
    s_refused[0] = -1;
    show_region();
    region_hint(1);
}

static void leave_region_mode(void)
{
    s_mode = MODE_OVERLAY;
    vjo_stick_cursor_latch(&s_stick); /* a stick still drawing the box does not move the cursor */
    if (s_region_layer)
        s_region_layer->Hide();
    if (s_panel)
        s_panel->Show();
}

static void open_page(void)
{
    Plugin::PageOpenParam param;
    read_buttons(); /* the setting can change between openings */
    if (!s_ui_mem) {
        s_ui_mem = (uint8_t *)vjo_shell_heap_alloc_for(VJO_SL_PAF_UI, UI_MEM_BYTES);
        if (!s_ui_mem) {
            vjo_log("overlay not opened: no Paf memory for its text (%u KiB)", UI_MEM_BYTES >> 10);
            return;
        }
    }
    vjo_arena_init(&s_ui, s_ui_mem, UI_MEM_BYTES);
    param.overwrite_draw_priority = 6; /* above the game, like PSVshellPlus' HUD */
    s_page = s_plugin->PageOpen("vjo_page_overlay", param);
    if (!s_page) {
        vjo_log("overlay page failed to open");
        vjo_shell_heap_free(VJO_SL_PAF_UI, s_ui_mem, UI_MEM_BYTES);
        s_ui_mem = NULL;
        vjo_arena_init(&s_ui, NULL, 0);
        return;
    }
    s_panel = s_page->FindChild("vjo_panel");
    s_header.text = (ui::Text *)s_page->FindChild("vjo_header");
    s_body.text = (ui::Text *)s_page->FindChild("vjo_body");
    s_header.view = s_page->FindChild("vjo_header_view");
    s_header.box = s_page->FindChild("vjo_header_box");
    s_body.view = s_page->FindChild("vjo_body_view");
    s_body.box = s_page->FindChild("vjo_body_box");
    s_separator = s_page->FindChild("vjo_separator");
    s_band_mid = s_page->FindChild("vjo_band_mid");
    s_header.h = HEADER_H_INITIAL; /* the page opens with the RCO layout */
    s_body.h = BODY_H_INITIAL;
    pane_reset(&s_header);
    pane_reset(&s_body);
    s_hint = (ui::RichText *)s_page->FindChild("vjo_hint");
    {
        /* The views get these empty scrollbars instead of their default
         * ones, which flashed whenever the text moved. */
        static const char *const sbars[] = {"vjo_header_sbar_v", "vjo_header_sbar_h", "vjo_body_sbar_v",
                                            "vjo_body_sbar_h"};
        for (unsigned i = 0; i < sizeof(sbars) / sizeof(sbars[0]); i++) {
            ui::Widget *w = s_page->FindChild(sbars[i]);
            if (w)
                w->Hide();
        }
    }
    s_region_layer = s_page->FindChild("vjo_region_layer");
    s_region_rect = s_page->FindChild("vjo_region_rect");
    s_region_cursor = s_page->FindChild("vjo_region_cursor");
    s_region_hint = (ui::RichText *)s_page->FindChild("vjo_region_hint");
    if (s_region_layer)
        s_region_layer->Hide();
    /* Wrapped text with Japanese line-breaking rules. */
    {
        ui::Text *texts[2] = {s_header.text, s_body.text};
        for (int i = 0; i < 2; i++) {
            if (!texts[i])
                continue;
            texts[i]->SetLayoutAttribute(graph::TextLayoutAttribute_WordWrap, true);
            texts[i]->SetLayoutAttribute(graph::TextLayoutAttribute_Kinsoku, true);
        }
    }
    {
        char hint[128]; /* 95 B with a 3-byte glyph */
        sceClibSnprintf(hint, sizeof(hint),
                        "<font color=\"#8a94a6\">◀ ▶ ▲ ▼ word · R stick scroll · □ region · %s close</font>",
                        s_btn.cancel_glyph);
        set_rich(s_hint, hint);
    }
    s_mode = MODE_OVERLAY;
    s_selected = 0;
    s_hl_len = 0;
    s_hl_dirty = false;
    s_highlight_retry_frames = 0;
    s_n_entries = 0;
    s_header_has_text = 0;
#if VJO_DEBUG
    s_frames = s_moves = 0;
    s_stick.moves = s_stick.lookups = 0;
#endif
    s_nav.row_moves = s_nav.wraps = s_nav.no_layout = 0;
    /* A stick already pushed when opening moves nothing until let go; its other
     * state is taken afresh then. A word it left due at the close is dropped by
     * the next render (close_page: the text is taken afresh). */
    vjo_stick_cursor_latch(&s_stick);
    s_tap = 0;
#if VJO_DEBUG
    s_frame_max_us = s_key_max_us = 0;
    s_stick_max = 0;
#endif
    s_prev_buttons = ~0u; /* ignore buttons already held when opening */
    s_prev_touch = 1;
    touch_write(vjo_touch_hold(&s_touch)); /* own front input throughout the overlay, not just crops */
}

/* handback: the control thread's VJO_TOUCH_* for this close (worker.c
 * close_overlay_for). The hold is given back for what is in front now
 * (touch_hold.h, #104): the kernel's foreground, read here, changes in the
 * process event hook itself, before the control thread handles the change. */
static void close_page(int handback)
{
    Plugin::PageCloseParam param;
    VjoState st;
    int rc, write;
    if (s_mode == MODE_REGION)
        leave_region_mode();
    sceClibMemset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    rc = vjoGetState(&st);
    write = vjo_touch_release(&s_touch, handback, rc, st.game_pid);
    if (write == VJO_TOUCH_LEFT)
        vjo_dlog("touch region left to SceShell t=%u ms", vjo_log_ms()); /* the game left the foreground */
    else
        touch_write(write);
    param.fade = false;
    s_plugin->PageClose("vjo_page_overlay", param);
    s_page = NULL;
    s_panel = s_region_layer = s_region_rect = NULL;
#if VJO_DEBUG
    vjo_log("overlay closed: %d frames, %d word moves, stick %d, slowest frame %d ms, %d row moves, %d wraps, "
            "%d without layout, slowest key %d us, %d stick moves, %d deferred lookups after %u ms", s_frames,
            s_moves, s_stick_max, (int)(s_frame_max_us / 1000), s_nav.row_moves, s_nav.wraps, s_nav.no_layout,
            (int)s_key_max_us, s_stick.moves, s_stick.lookups, s_stick.rest_us / 1000u);
#endif
    /* The next open keeps this cursor only when it is the worker's and no word
     * of the stick's is due (core/stick_cursor.h vjo_stick_cursor_kept); else it
     * takes the worker's, as for new text: the one whose word the worker shows,
     * or looks up. A post the worker refused (Cancel's command still pending, or
     * the overlay closing) leaves them apart without anything due. */
    vjo_view_lock();
    if (!vjo_stick_cursor_kept(&s_stick, s_cursor_byte, g_view.cursor_byte))
        s_local_cursor_mode = 0;
    vjo_view_unlock();
    s_header.view = s_header.box = s_body.view = s_body.box = NULL;
    s_header.text = s_body.text = NULL;
    s_separator = s_band_mid = NULL;
    s_hint = s_region_hint = NULL;
    s_region_cursor = NULL;
    s_n_entries = 0;
    s_header_has_text = 0;
    s_hdr_wrap.frames = s_body_wrap.frames = s_hdr_nbrk = 0; /* their texts go with the arena */
    vjo_arena_init(&s_ui, NULL, 0);
    vjo_shell_heap_free(VJO_SL_PAF_UI, s_ui_mem, UI_MEM_BYTES);
    s_ui_mem = NULL;
}

/* ▲/▼: the word on the nearest line above/below that has one, closest in x
 * to the selected word; from the header's own layout (GetCharInfo). */
static int entry_vertical(int up)
{
    graph::TextLayoutCharInfo ci;
    uint32_t cur_line;
    float cur_x;
    int best = s_selected;
    uint32_t best_line = 0;
    float best_dx = 0.0f;
    if (!s_header.text || !s_hl_len || s_header.text->GetCharInfo(s_hl_start, ci) < 0)
        return s_selected;
    cur_line = ci.line;
    cur_x = ci.x;
    for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++) {
        float dx;
        if (i == s_selected || !s_rng_len[i] || s_header.text->GetCharInfo(s_rng_start[i], ci) < 0)
            continue;
        if (up ? ci.line >= cur_line : ci.line <= cur_line)
            continue;
        dx = ci.x > cur_x ? ci.x - cur_x : cur_x - ci.x;
        /* nearest line first, then nearest x */
        if (best == s_selected || (up ? ci.line > best_line : ci.line < best_line) ||
            (ci.line == best_line && dx < best_dx)) {
            best = i;
            best_line = ci.line;
            best_dx = dx;
        }
    }
    return best;
}

static int header_char_position(void *ctx, uint32_t unit, float *x, uint32_t *line)
{
    graph::TextLayoutCharInfo info;
    int rc = ((ui::Text *)ctx)->GetCharInfo(header_shown(unit), info);
    if (rc >= 0) { *x = info.x; *line = info.line; }
    return rc;
}

/* The header's displayed rows for the cursor keys: its own laid-out lines and
 * character starts (GetCharInfo), as for taps; 0 until laid out. */
static int header_rows(VjoTextRows *rows)
{
    uint32_t lines = 0;
    if (!s_header.text || !s_header_has_text || s_header.text->GetLineCount(lines) < 0 || !lines)
        return 0;
    rows->position = header_char_position;
    rows->ctx = s_header.text;
    rows->lines = lines;
    rows->terminal_width = (float)s_ja_px;
    return 1;
}

/* The d-pad's and a tap's lookup, at once: it drops one the stick left due. */
static void post_cursor_now(void)
{
    s_stick.due = 0;
    vjo_post_cursor(s_cursor_byte, s_text_generation);
}

static void input_header_touch(void)
{
    const inputdevice::touchscreen::Data *pd = NULL;
    inputdevice::touchscreen::GetData(inputdevice::touchscreen::PORT_FRONT, 0, &pd);
    int down = pd && pd->state != inputdevice::touchscreen::Data::STATE_NONE &&
                    pd->state != inputdevice::touchscreen::Data::STATE_RELEASE;
    int pressed = down && !s_prev_touch;
    s_prev_touch = down;
    if (!pressed || !s_touch.held || !s_local_cursor_mode || !s_header.text || !s_header_has_text)
        return;
    float x = 0, y = 0;
    uint32_t lines = 0;
    if (inputdevice::touchscreen::GetPointFromTopleft(inputdevice::touchscreen::PORT_FRONT, 0, x, y) < 0 ||
        s_header.text->GetLineCount(lines) < 0 || !lines) return;
    int ja, en;
    font_px(&ja, &en);
    /* The header has one font size, so its measured line count and bounds
     * give the same pitch used by header_follow. Never query the undocumented
     * LineInfo frame fields or the crashing GetNextDownHit helper. */
    VjoTextHitLayout layout = {(SCREEN_W - BOX_W) / 2.0f, SCREEN_H / 2.0f - HEADER_TOP,
        BOX_W, s_header.h, text_height(s_header.text), s_header.scroll, (float)ja,
        lines, header_char_position, s_header.text};
    size_t byte = 0;
    if (!vjo_text_hit(s_cursor_text, s_cursor_text_len, &layout, x, y,
                      s_japanese_only_selection, &byte)) return;
    if (byte == s_cursor_byte) return;
    s_cursor_byte = (uint32_t)byte;
    s_nav.words.count = 0; /* a tap starts a new navigation path */
    COUNT_MOVE();
    vjo_dlog("header touch cursor=%u generation=%u", s_cursor_byte, s_text_generation);
    post_cursor_now();
    render(0);
}

static void input_overlay(const VjoInput *in, uint32_t pressed)
{
    input_header_touch();
    int n = s_n_entries;
    int next = s_selected;
    int dy;
    if (s_local_cursor_mode) {
        uint32_t cursor = s_cursor_byte;
        int direction = pressed & SCE_CTRL_LEFT ? -1 : pressed & SCE_CTRL_RIGHT ? 1 : 0;
        int vertical = pressed & SCE_CTRL_UP ? -1 : pressed & SCE_CTRL_DOWN ? 1 : 0;
        /* The left stick's step this frame (core/stick_cursor.h), when no arrow
         * is pressed: its four directions are the arrows' (a diagonal its
         * dominant axis's). Its word is looked up only once the cursor rests. */
        uint64_t now = (uint64_t)sceKernelGetProcessTimeWide();
        int stick = vjo_stick_cursor_step(vjo_stick_cursor(&s_stick, in->lx, in->ly, now), &direction, &vertical);
        /* L or R tapped alone (never the L + R chord): seen every frame. */
        int tap = vjo_shoulder_tap(s_tap, in->buttons & 0xFFFFu, pressed, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER);
        if (direction || vertical) {
            /* The header's displayed rows (core/text_rows.h): Left/Right in
             * reading order across rows, stopping at the text's ends (a
             * one-row text wraps for the arrows), Up/Down change rows,
             * soft-wrapped ones included. */
            VjoTextRows rows;
            size_t to;
#if VJO_DEBUG
            int64_t t0 = sceKernelGetProcessTimeWide();
#endif
            vjo_text_row_key(&s_nav, s_cursor_text, s_cursor_text_len, header_rows(&rows) ? &rows : NULL, cursor,
                             s_highlight_mode == VJO_HIGHLIGHT_WORD ? s_match_end : 0, direction, vertical,
                             s_japanese_only_selection, stick != 0, &to);
#if VJO_DEBUG
            t0 = sceKernelGetProcessTimeWide() - t0; /* the walk's console cost, for the close log */
            if (!stick && t0 > s_key_max_us) /* the slowest key is the arrows' (the stick's moves are its own) */
                s_key_max_us = t0;
#endif
            cursor = (uint32_t)to;
        }
        if (stick)
            vjo_stick_cursor_moved(&s_stick, cursor != s_cursor_byte, now);
        if (cursor != s_cursor_byte) {
            s_cursor_byte = cursor;
            /* The command detaches the previous definition immediately;
             * only its asynchronous lookup touches dictionary storage. */
            if (!stick) {
                COUNT_MOVE();
                post_cursor_now();
            }
            render(0);
        } else {
            /* A tapped shoulder: another dictionary's entry at this cursor.
             * The worker reads only that page's definition into the same
             * result arena. */
            int page = s_dict_page + tap;
            if (tap && page >= 0 && page < s_dict_pages && vjo_post_page(page, s_text_generation)) {
                COUNT_MOVE();
                render(0);
            }
        }
        if (vjo_stick_cursor_lookup(&s_stick, now))
            vjo_post_cursor(s_cursor_byte, s_text_generation);
    } else {
        /* No text to move through (recognizing, an error): a stick held until
         * it comes moves nothing then, as a page change latches the app's lists. */
        vjo_stick_cursor_latch(&s_stick);
        if ((pressed & SCE_CTRL_RIGHT) && s_selected + 1 < n)
            next = s_selected + 1;
        if ((pressed & SCE_CTRL_LEFT) && s_selected > 0)
            next = s_selected - 1;
        if (pressed & SCE_CTRL_UP)
            next = entry_vertical(1);
        if (pressed & SCE_CTRL_DOWN)
            next = entry_vertical(0);
        if (next != s_selected) {
            s_selected = next;
            COUNT_MOVE();
            render(0);
        }
    }
    /* the right stick scrolls a long definition (the left one moves the cursor) */
    dy = (int)in->ry - 128;
#if VJO_DEBUG
    {
        const uint8_t axes[3] = {in->lx, in->ly, in->ry};
        for (int i = 0; i < 3; i++)
            if (vjo_stick_away(axes[i]) > s_stick_max)
                s_stick_max = vjo_stick_away(axes[i]);
    }
#endif
    if (dy <= -STICK_DEADZONE || dy >= STICK_DEADZONE)
        pane_scroll_to(&s_body, s_body.scroll + (float)dy * STICK_SCROLL_PX);
    /* Enter saves the shown word (in a character lookup, the match at the
     * cursor) to the offline queue; the Rubi app exports it. */
    if ((pressed & s_btn.enter) && s_anki_enabled && s_selected < s_n_entries)
        vjo_anki_post_add(s_list_seq, s_selected);
    if (pressed & s_btn.cancel)
        vjo_post_command(VJO_CMD_CLOSED, NULL);
    if (pressed & SCE_CTRL_SQUARE)
        enter_region_mode();
}

static void input_region(const VjoInput *in, uint32_t held, uint32_t pressed)
{
    const inputdevice::touchscreen::Data *pd = NULL;
    VjoRselInput ri;
    int ev, x = 0, y = 0, w = 0, h = 0;
    sceClibMemset(&ri, 0, sizeof(ri));
    /* paf: screen pixels from the top left */
    inputdevice::touchscreen::GetData(inputdevice::touchscreen::PORT_FRONT, 0, &pd);
    if (pd && pd->state != inputdevice::touchscreen::Data::STATE_NONE &&
        pd->state != inputdevice::touchscreen::Data::STATE_RELEASE) {
        float fx = 0.0f, fy = 0.0f;
        int rc = inputdevice::touchscreen::GetPointFromTopleft(inputdevice::touchscreen::PORT_FRONT, 0, fx, fy);
        ri.touch = 1;
        ri.tx = rc >= 0 ? (int)fx : pd->x;
        ri.ty = rc >= 0 ? (int)fy : pd->y;
    }
    s_prev_touch = ri.touch;
    ri.held = held;
    ri.pressed = pressed;
    ri.enter = s_btn.enter;
    ri.cancel = s_btn.cancel;
    ri.lx = in->lx;
    ri.ly = in->ly;
    ri.now_us = sceKernelGetProcessTimeWide();
    ev = vjo_rsel_update(&s_rsel, &ri);

    /* hold □ (from entering region mode) for 1 s -> full screen */
    if (!(held & SCE_CTRL_SQUARE))
        s_square_clear_armed = 0;
    if (pressed & SCE_CTRL_SQUARE) {
        s_square_clear_armed = 1;
        s_square_down_us = ri.now_us;
    }
    if (s_square_clear_armed && ri.now_us - s_square_down_us >= CLEAR_HOLD_US) {
        s_square_clear_armed = 0;
        leave_region_mode();
        vjo_post_command(VJO_CMD_CLEAR_REGION, NULL);
        return;
    }
    vjo_rsel_box(&s_rsel, &x, &y, &w, &h);
    if (ev == VJO_RSEL_SAVE) {
        VjoRect r = vjo_rsel_rect(x, y, w, h);
        vjo_dlog("region box %d,%d %dx%d", x, y, w, h);
        leave_region_mode();
        vjo_post_command(VJO_CMD_SET_REGION, &r);
        return;
    }
    if (ev == VJO_RSEL_CANCEL) {
        vjo_dlog("region unchanged");
        leave_region_mode();
        return;
    }
    if (ev == VJO_RSEL_CORNER) /* by enter, or by a new touch */
        vjo_dlog("region corner %d,%d%s", x, y, pressed & s_btn.enter ? "" : " (touch)");
    else if (ev == VJO_RSEL_BACK)
        vjo_dlog("region corner undone");
    else if (ev == VJO_RSEL_SMALL) {
        vjo_dlog("region box %dx%d too small", w, h);
        s_refused[0] = w;
        s_refused[1] = h;
    }
    show_region();
    region_hint(ev != VJO_RSEL_NONE);
}

/* ---- frame ---- */

static void frame_body(unsigned version, unsigned anki_version);

static void frame(void *arg)
{
    int want_open, want_strip, handback;
    unsigned version, anki_version, strip_version;
    (void)arg;

    /* Runs on SceShell's paf main thread every frame: while closed, skip the
     * lock unless an open is published (a store under the lock; a stale read
     * only delays the open by a frame). */
    if (!s_page && !vjo_strip_is_open() && !__atomic_load_n(&g_view.open, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_view.strip_on, __ATOMIC_ACQUIRE))
        return;
    vjo_view_lock();
    want_open = g_view.open;
    handback = g_view.touch_handback;
    version = g_view.version;
    anki_version = g_view.anki_version;
    want_strip = g_view.strip_on && g_view.strip_text[0] && !want_open;
    strip_version = g_view.strip_version;
    vjo_view_unlock();

    /* the strip first: it closes before the overlay opens */
    vjo_strip_frame(s_plugin, want_strip, strip_version);

    if (want_open && !s_page) {
        open_page();
        if (!s_page) {
            /* No memory for the page: tell the control thread the overlay is
             * closed (it releases the game's input), instead of retrying
             * every frame. The next L+R tries again. */
            vjo_post_command(VJO_CMD_CLOSED, NULL);
            return;
        }
        s_seen_version = version - 1;
        s_seen_anki_version = anki_version;
    } else if (!want_open && s_page) {
        close_page(handback);
        return;
    }
    if (!s_page)
        return;
#if VJO_DEBUG
    {
        int64_t t0 = sceKernelGetProcessTimeWide();
        frame_body(version, anki_version);
        t0 = sceKernelGetProcessTimeWide() - t0;
        if (t0 > s_frame_max_us)
            s_frame_max_us = t0;
        s_frames++;
    }
#else
    frame_body(version, anki_version); /* the normal build times no frame (the debug build's close line) */
#endif
}

/* One frame with the overlay page open. */
static void frame_body(unsigned version, unsigned anki_version)
{
    if (version != s_seen_version) {
        s_seen_version = version;
        s_seen_anki_version = anki_version;
        render(1);
    } else if (anki_version != s_seen_anki_version) {
        /* Anki status or marks: the body only, keeping the selection and
         * the definition's scroll */
        float scroll = s_body.scroll;
        s_seen_anki_version = anki_version;
        render(0);
        s_body.scroll = scroll;
    } else if (s_highlight_retry_frames > 0) {
        if (header_highlight())
            s_highlight_retry_frames = 0;
        else if (--s_highlight_retry_frames == 0)
            vjo_log("header highlight update failed after layout retries");
    }
    if (s_follow_frames > 0 && (header_follow() || --s_follow_frames == 0))
        s_follow_frames = 0;
    wrap_frame();
    header_fit();
    pane_place(&s_header);
    pane_place(&s_body);

    {
        VjoInput in;
        uint32_t held, pressed;
        sceClibMemset(&in, 0, sizeof(in));
        in.lx = in.ly = in.rx = in.ry = 128; /* a failed read leaves both sticks at rest */
        vjoPollInput(&in);
        held = in.buttons;
        pressed = held & ~s_prev_buttons;
        s_prev_buttons = held;
        if (s_mode == MODE_OVERLAY)
            input_overlay(&in, pressed);
        else
            input_region(&in, held, pressed);
    }
}

extern "C" void vjo_overlay_init(void *plugin)
{
    s_plugin = (Plugin *)plugin;
    read_buttons(); /* before Anki's first message names the enter button */
    vjo_arena_init(&s_ui, NULL, 0); /* allocated when the page opens */
    common::MainThreadCallList::Register(frame, NULL);
}
