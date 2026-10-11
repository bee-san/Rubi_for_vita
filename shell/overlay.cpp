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
 * front SceShell's regions are inactive. The region selector activates
 * SceShell's default touch region while it is open (as SceShell's own menus
 * over a game must) and reads the touches through paf's input device. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "paf_ui.h"
#include "header_highlight.h"
#include "word_navigation.h"
extern "C" {
#include "../core/utf.h"
#include "../core/text_hit.h"
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
#define BODY_UNITS 4096
#define MAX_SPANS 96
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

static unsigned s_seen_version;
static unsigned s_list_seq; /* the shown list */
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
static VjoWordNavigation s_word_navigation;

/* logged when the overlay closes */
static int64_t s_frame_max_us;
static int s_frames, s_moves, s_stick_max; /* largest stick deflection from centre */
static int s_mode;
static uint32_t s_prev_buttons;
static int s_prev_touch;

/* header state */
static uint32_t s_hl_start, s_hl_len; /* highlighted range now drawn */
static bool s_hl_dirty;              /* style writes awaiting a successful refresh */
static int s_follow_frames;           /* frames left to scroll the header to the word */
static int s_highlight_retry_frames;  /* bounded retries while text layout catches up */

/* region selection (screen pixels) */
static int s_touch_region_on; /* SceShell's default front touch region activated by us */
static int s_drag, s_has_rect;
static int s_x0, s_y0, s_x1, s_y1;
static int64_t s_square_down_us;
static int s_square_clear_armed;

static uint8_t s_ui_mem[96 * 1024];
static VjoArena s_ui;

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
    vjo_view_lock();
    l = g_view.list;
    s_highlight_mode = g_view.highlight_mode;
    if (s_highlight_mode != VJO_HIGHLIGHT_WORD)
        s_word_navigation.count = 0;
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
            s_word_navigation.count = 0;
            s_text_generation = g_view.text_generation;
            s_cursor_byte = (uint32_t)(g_view.japanese_only_selection
                ? vjo_utf8_japanese_cursor_step(s_cursor_text, s_cursor_text_len, g_view.cursor_byte, 0)
                : vjo_utf8_cursor_step(s_cursor_text, s_cursor_text_len, g_view.cursor_byte, 0));
        }
        s_japanese_only_selection = g_view.japanese_only_selection;
        cursor_matches = g_view.cursor_byte == s_cursor_byte;
    } else {
        rebuild_header |= s_local_cursor_mode;
        s_local_cursor_mode = 0;
        s_cursor_text_len = 0;
        s_cursor_text[0] = 0;
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
    if (s_selected >= s_n_entries)
        s_selected = s_n_entries ? s_n_entries - 1 : 0;
    vjo_arena_reset(&s_ui);

    if (rebuild_header) {
        if (vjo_styled_init(&hdr, &s_ui, HEADER_UNITS, 1) == 0) {
            if (s_local_cursor_mode)
                vjo_styled_puts(&hdr, s_cursor_text, VJO_RGB_TEXT, ja);
            else
                vjo_styled_header(&hdr, l, ja);
            have_hdr = 1;
        }
        if (!s_local_cursor_mode)
            for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++)
                if (!vjo_entry_header_range(l, i, &s_rng_start[i], &s_rng_len[i]))
                    s_rng_len[i] = 0;
    }

    /* body: status, the selected entry (or "no entries"), config warnings */
    if (vjo_styled_init(&body, &s_ui, BODY_UNITS, MAX_SPANS) == 0) {
        have_body = 1;
        if (!cursor_matches)
            vjo_styled_puts(&body, "Looking up words…", VJO_RGB_DIM, en);
        else if (g_view.status[0])
            vjo_styled_puts(&body, g_view.status, g_view.status_is_error ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        if (s_n_entries) {
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            vjo_styled_entry(&body, l, s_selected, ja, en, 0);
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

    set_rich(s_hint, s_local_cursor_mode
                        ? (s_highlight_mode == VJO_HIGHLIGHT_WORD
                            ? "<font color=\"#8a94a6\">Tap character / ◀ ▶ word · stick scroll · □ region · ○ close</font>"
                            : s_japanese_only_selection
                            ? "<font color=\"#8a94a6\">Tap / ◀ ▶ Japanese character · stick scroll · □ region · ○ close</font>"
                            : "<font color=\"#8a94a6\">Tap / ◀ ▶ character · stick scroll · □ region · ○ close</font>")
                        : "<font color=\"#8a94a6\">◀ ▶ ▲ ▼ word · stick scroll · □ region · ○ close</font>");
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
    if (have_body)
        text_set(s_body.text, &body);
    pane_reset(&s_body);
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

static void show_region_rect(void)
{
    int x = s_x0 < s_x1 ? s_x0 : s_x1, y = s_y0 < s_y1 ? s_y0 : s_y1;
    int w = s_x0 < s_x1 ? s_x1 - s_x0 : s_x0 - s_x1, hgt = s_y0 < s_y1 ? s_y1 - s_y0 : s_y0 - s_y1;
    if (!s_region_rect)
        return;
    if (w < 2 || hgt < 2) {
        s_region_rect->Hide();
        return;
    }
    /* paf layout: origin at the screen centre, y up */
    s_region_rect->SetSize((float)w, (float)hgt, 0.0f);
    s_region_rect->SetPos((float)(x + w / 2 - SCREEN_W / 2), (float)(SCREEN_H / 2 - (y + hgt / 2)), 0.0f);
    s_region_rect->Show();
}

/* Front touches go to SceShell while on (and not to the game). */
static void touch_region(int on)
{
    int rc;
    if (on == s_touch_region_on)
        return;
    rc = inputdevice::touchscreen::ActivateDefaultRegion(inputdevice::touchscreen::PORT_FRONT, on != 0);
    if (rc >= 0)
        s_touch_region_on = on;
    vjo_log("touch region %s: 0x%X", on ? "on" : "off", rc);
}

static void enter_region_mode(void)
{
    s_mode = MODE_REGION;
    touch_region(1);
    s_drag = 0;
    s_has_rect = 0;
    s_square_down_us = sceKernelGetProcessTimeWide();
    s_square_clear_armed = 1;
    if (s_panel)
        s_panel->Hide();
    if (s_region_layer)
        s_region_layer->Show();
    if (s_region_rect)
        s_region_rect->Hide();
    set_rich(s_region_hint, "<font color=\"#ffffff\">Drag on the screen to select the text area · "
                            "× confirm · ○ cancel · hold □ for full screen</font>");
}

static void leave_region_mode(void)
{
    s_mode = MODE_OVERLAY;
    if (s_region_layer)
        s_region_layer->Hide();
    if (s_panel)
        s_panel->Show();
}

static void open_page(void)
{
    Plugin::PageOpenParam param;
    param.overwrite_draw_priority = 6; /* above the game, like PSVshellPlus' HUD */
    s_page = s_plugin->PageOpen("vjo_page_overlay", param);
    if (!s_page) {
        vjo_log("overlay page failed to open");
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
        set_rich(s_hint, "<font color=\"#8a94a6\">◀ ▶ ▲ ▼ word · stick scroll · □ region · ○ close</font>");
    }
    s_mode = MODE_OVERLAY;
    s_selected = 0;
    s_hl_len = 0;
    s_hl_dirty = false;
    s_highlight_retry_frames = 0;
    s_n_entries = 0;
    s_header_has_text = 0;
    s_frames = s_moves = 0;
    s_frame_max_us = 0;
    s_stick_max = 0;
    s_prev_buttons = ~0u; /* ignore buttons already held when opening */
    s_prev_touch = 1;
    touch_region(1); /* own front input throughout the overlay, not just crops */
}

static void close_page(void)
{
    Plugin::PageCloseParam param;
    if (s_mode == MODE_REGION)
        leave_region_mode();
    touch_region(0);
    param.fade = false;
    s_plugin->PageClose("vjo_page_overlay", param);
    s_page = NULL;
    s_panel = s_region_layer = s_region_rect = NULL;
    vjo_log("overlay closed: %d frames, %d word moves, stick %d, slowest frame %d ms", s_frames, s_moves,
            s_stick_max, (int)(s_frame_max_us / 1000));
    s_header.view = s_header.box = s_body.view = s_body.box = NULL;
    s_header.text = s_body.text = NULL;
    s_separator = s_band_mid = NULL;
    s_hint = s_region_hint = NULL;
    s_n_entries = 0;
    s_header_has_text = 0;
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
    int rc = ((ui::Text *)ctx)->GetCharInfo(unit, info);
    if (rc >= 0) { *x = info.x; *line = info.line; }
    return rc;
}

static void input_header_touch(void)
{
    const inputdevice::touchscreen::Data *pd = NULL;
    inputdevice::touchscreen::GetData(inputdevice::touchscreen::PORT_FRONT, 0, &pd);
    int down = pd && pd->state != inputdevice::touchscreen::Data::STATE_NONE &&
                    pd->state != inputdevice::touchscreen::Data::STATE_RELEASE;
    int pressed = down && !s_prev_touch;
    s_prev_touch = down;
    if (!pressed || !s_touch_region_on || !s_local_cursor_mode || !s_header.text || !s_header_has_text)
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
    s_word_navigation.count = 0; /* a tap starts a new navigation path */
    s_moves++;
    vjo_log("header touch cursor=%u generation=%u", s_cursor_byte, s_text_generation);
    vjo_post_cursor(s_cursor_byte, s_text_generation);
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
        if (direction && s_highlight_mode == VJO_HIGHLIGHT_WORD)
            cursor = (uint32_t)vjo_word_cursor_step(s_word_navigation, s_cursor_text,
                s_cursor_text_len, s_cursor_byte, s_match_end, direction, s_japanese_only_selection);
        else if (direction)
            cursor = (uint32_t)(s_japanese_only_selection
                ? vjo_utf8_japanese_cursor_step(s_cursor_text, s_cursor_text_len, s_cursor_byte, direction)
                : vjo_utf8_cursor_step(s_cursor_text, s_cursor_text_len, s_cursor_byte, direction));
        if (cursor != s_cursor_byte) {
            s_cursor_byte = cursor;
            s_moves++;
            /* The command detaches the previous definition immediately;
             * only its asynchronous lookup touches dictionary storage. */
            vjo_post_cursor(cursor, s_text_generation);
            render(0);
        }
    } else {
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
            s_moves++;
            render(0);
        }
    }
    /* either stick scrolls a long definition */
    dy = (int)in->ly - 128;
    if (dy > -STICK_DEADZONE && dy < STICK_DEADZONE)
        dy = (int)in->ry - 128;
    if ((dy < 0 ? -dy : dy) > s_stick_max)
        s_stick_max = dy < 0 ? -dy : dy;
    if (dy <= -STICK_DEADZONE || dy >= STICK_DEADZONE)
        pane_scroll_to(&s_body, s_body.scroll + (float)dy * STICK_SCROLL_PX);
    if (pressed & SCE_CTRL_CIRCLE)
        vjo_post_command(VJO_CMD_CLOSED, NULL);
    if (pressed & SCE_CTRL_SQUARE)
        enter_region_mode();
}

static void input_region(uint32_t held, uint32_t pressed)
{
    const inputdevice::touchscreen::Data *pd = NULL;
    int touching = 0, x = 0, y = 0;
    /* paf: screen pixels from the top left */
    inputdevice::touchscreen::GetData(inputdevice::touchscreen::PORT_FRONT, 0, &pd);
    if (pd && pd->state != inputdevice::touchscreen::Data::STATE_NONE &&
        pd->state != inputdevice::touchscreen::Data::STATE_RELEASE) {
        float fx = 0.0f, fy = 0.0f;
        int rc = inputdevice::touchscreen::GetPointFromTopleft(inputdevice::touchscreen::PORT_FRONT, 0, fx, fy);
        touching = 1;
        x = rc >= 0 ? (int)fx : pd->x;
        y = rc >= 0 ? (int)fy : pd->y;
    }
    if (touching) {
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x >= SCREEN_W)
            x = SCREEN_W - 1;
        if (y >= SCREEN_H)
            y = SCREEN_H - 1;
        if (!s_prev_touch || !s_drag) {
            s_drag = 1;
            s_x0 = s_x1 = x;
            s_y0 = s_y1 = y;
            vjo_log("region drag from %d,%d", x, y);
        } else {
            s_x1 = x;
            s_y1 = y;
        }
        s_has_rect = (s_x0 != s_x1) && (s_y0 != s_y1);
        show_region_rect();
    } else {
        if (s_drag)
            vjo_log("region drag to %d,%d", s_x1, s_y1);
        s_drag = 0;
    }
    s_prev_touch = touching;

    /* hold □ (from entering region mode) for 1 s -> full screen */
    if (!(held & SCE_CTRL_SQUARE))
        s_square_clear_armed = 0;
    if (pressed & SCE_CTRL_SQUARE) {
        s_square_clear_armed = 1;
        s_square_down_us = sceKernelGetProcessTimeWide();
    }
    if (s_square_clear_armed && sceKernelGetProcessTimeWide() - s_square_down_us >= CLEAR_HOLD_US) {
        s_square_clear_armed = 0;
        leave_region_mode();
        vjo_post_command(VJO_CMD_CLEAR_REGION, NULL);
        return;
    }
    if ((pressed & SCE_CTRL_CROSS) && s_has_rect) {
        VjoRect r;
        int x = s_x0 < s_x1 ? s_x0 : s_x1, y = s_y0 < s_y1 ? s_y0 : s_y1;
        int w = s_x0 < s_x1 ? s_x1 - s_x0 : s_x0 - s_x1, h = s_y0 < s_y1 ? s_y1 - s_y0 : s_y0 - s_y1;
        r.x = (uint16_t)(x * 65536 / SCREEN_W);
        r.y = (uint16_t)(y * 65536 / SCREEN_H);
        r.w = (uint16_t)((w * 65536 / SCREEN_W) > 65535 ? 65535 : w * 65536 / SCREEN_W);
        r.h = (uint16_t)((h * 65536 / SCREEN_H) > 65535 ? 65535 : h * 65536 / SCREEN_H);
        leave_region_mode();
        vjo_post_command(VJO_CMD_SET_REGION, &r);
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE)
        leave_region_mode();
}

/* ---- frame ---- */

static void frame_body(unsigned version);

static void frame(void *arg)
{
    int want_open, want_strip;
    unsigned version, strip_version;
    (void)arg;

    /* Runs on SceShell's paf main thread every frame: while closed, skip the
     * lock unless an open is published (a store under the lock; a stale read
     * only delays the open by a frame). */
    if (!s_page && !vjo_strip_is_open() && !__atomic_load_n(&g_view.open, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_view.strip_on, __ATOMIC_ACQUIRE))
        return;
    vjo_view_lock();
    want_open = g_view.open;
    version = g_view.version;
    want_strip = g_view.strip_on && g_view.strip_text[0] && !want_open;
    strip_version = g_view.strip_version;
    vjo_view_unlock();

    /* the strip first: it closes before the overlay opens */
    vjo_strip_frame(s_plugin, want_strip, strip_version);

    if (want_open && !s_page) {
        open_page();
        s_seen_version = version - 1;
    } else if (!want_open && s_page) {
        close_page();
        return;
    }
    if (!s_page)
        return;
    {
        int64_t t0 = sceKernelGetProcessTimeWide();
        frame_body(version);
        t0 = sceKernelGetProcessTimeWide() - t0;
        if (t0 > s_frame_max_us)
            s_frame_max_us = t0;
        s_frames++;
    }
}

/* One frame with the overlay page open. */
static void frame_body(unsigned version)
{
    if (version != s_seen_version) {
        s_seen_version = version;
        render(1);
    } else if (s_highlight_retry_frames > 0) {
        if (header_highlight())
            s_highlight_retry_frames = 0;
        else if (--s_highlight_retry_frames == 0)
            vjo_log("header highlight update failed after layout retries");
    }
    if (s_follow_frames > 0 && (header_follow() || --s_follow_frames == 0))
        s_follow_frames = 0;
    header_fit();
    pane_place(&s_header);
    pane_place(&s_body);

    {
        VjoInput in;
        uint32_t held, pressed;
        sceClibMemset(&in, 0, sizeof(in));
        in.ly = in.ry = 128;
        vjoPollInput(&in);
        held = in.buttons;
        pressed = held & ~s_prev_buttons;
        s_prev_buttons = held;
        if (s_mode == MODE_OVERLAY)
            input_overlay(&in, pressed);
        else
            input_region(held, pressed);
    }
}

extern "C" void vjo_overlay_init(void *plugin)
{
    s_plugin = (Plugin *)plugin;
    vjo_arena_init(&s_ui, s_ui_mem, sizeof(s_ui_mem));
    common::MainThreadCallList::Register(frame, NULL);
}
