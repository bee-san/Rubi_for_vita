/* Standalone LiveArea settings app: no plugin imports and no network. Its
 * Anki page (anki_page.c) exports the queued cards as an .apkg file. */
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>
#include "anki_page.h"
#include "app_check.h"
#include "catalog.h"
#include "engine.h"
#include "icons.h"
#include "install_page.h"
#include "letter.h"
#include "lookup_page.h"
#include "preferences.h"
#include "psp.h"
#include "search.h"
#include "stats_page.h"
#include "text_layout.h"
#include "../core/hook_titles.h"
#include "../core/local_dict.h"
#include "../core/utf.h"
#include "../vita/games_storage.h"
#include "../vita/buttons_vita.h"
#include "../core/stick_scroll.h"
/* The app never calls vita2d_fini: as a system application it would clear a
 * display buffer libvita2d never set (the end of main). A call fails to build. */
#pragma GCC poison vita2d_fini

#define ROWS 5
#define TAB_Y 52
#define TAB_H 48
#define DEFAULT_Y 112
#define DEFAULT_H 54
#define GAMES_Y 174
#define GAME_H 40
#define GAME_PITCH 44
#define GAME_NAV_Y 398
#define GAME_NAV_H 36
#define OCR_Y 112
#define OCR_H 94
/* The OCR card: the local engine's Model size, three buttons (settings/engine.h),
 * the row's title at the left, the card's status line below them. VOCR is the
 * only engine since the overlay went offline (owner 2026-10-15): Google Lens and
 * its engine row are gone. Offsets from the card's top. */
#define OCR_ROW_TITLE_X 42
#define OCR_SIZE_X 232
#define OCR_SIZE_DY 6
#define OCR_SIZE_W 220
#define OCR_SIZE_H 54
#define OCR_SIZE_PITCH 233
#define OCR_STATUS_DY 85
/* The Read text card (config.ini ocr_mode): its title, then its two choices side
 * by side, each a button with its one-line explanation below its name. */
#define READ_H 102
#define READ_BUTTON_X 42
#define READ_BUTTON_DY 42
#define READ_BUTTON_W 433
#define READ_BUTTON_H 54
#define READ_BUTTON_PITCH 445
/* The compact preference cards share one layout: title and choices on the
 * first row, help on the second. */
#define CARD_H 68
#define CARD_TITLE_Y 30
#define CARD_HELP_Y 60
#define CARD_BADGE_Y 6
#define CARD_BADGE_H 34
#define RESULT_BADGE_X 540
#define RESULT_BADGE_W 44
#define RESULT_BADGE_PITCH 48
#define SOURCE_BADGE_X 440 /* API 31: Text source's three choices */
#define SOURCE_BADGE_W 152
#define SOURCE_BADGE_PITCH 160
/* The rows Up/Down select, the page's cards top to bottom: how text is read
 * (the Model size, Read text, Speed boost), then the three lookup cards. They
 * are more than the room between the tabs and the status line: the page
 * scrolls a card at a time, the selected card always whole; a card cut at the
 * bottom shows there is more, and a bar at the right where the view is. */
#define PREF_SIZE 0
#define PREF_READ 1
#define PREF_BOOST 2
#define PREF_SELECTION 3
#define PREF_HIGHLIGHT 4
#define PREF_RESULTS 5
#define PREF_SOURCE 6 /* API 31: Text source (config.ini text_source) */
/* API 43: OCR skipped where it has nothing to read: a text box without text (ocr_text_gate) and a line a trusted hook
 * has (hook_trust) */
#define PREF_TRUST 7
#define PREF_GATE 8
#define PREF_VERTICAL 9 /* API 47: Vertical text (config.ini ocr_vertical) */
#define PREF_SUGGEST 10 /* #358: Suggest a text box (region.ini's line for every game: preferences.h), the last card */
#define PREFERENCES 11
#define PREF_GAP 4
#define PREF_VIEW_Y OCR_Y /* the cards' room: below the tabs */
#define PREF_VIEW_END 422 /* to above the status line */
#define PREF_SCROLL_X 942
#define PREF_SCROLL_W 6
/* The OCR page's one-line status sits below the cards. */
#define PREFERENCE_FOOTER_Y (PREF_VIEW_END + 4)
#define PREFERENCE_FOOTER_H 30
#define BG RGBA8(9, 15, 23, 255)
#define CARD RGBA8(21, 31, 44, 255)
#define SELECTED RGBA8(35, 56, 70, 255)
#define WHITE RGBA8(238, 244, 255, 255)
#define DIM RGBA8(193, 207, 222, 255)
#define ACCENT RGBA8(91, 220, 207, 255)
#define OFF RGBA8(244, 160, 167, 255)
static VjoInstalledGame library[VJO_GAMES_MAX];
static VjoGames policy, saved;
/* #333: ocr_line_fix.ini's table (each game's own line finder, core/games.h vjo_line_fix_parse), as last read or
 * saved; line_fix_writable 0 while the file cannot be read (nothing is written over it then). */
static VjoGames line_fix, line_fix_saved;
static int line_fix_writable = 1;
/* #333: a game's own page (its ON/OFF and its line finder), opened from the Games list by holding Enter, or a
 * touch, on its row for GAME_HOLD_US: the library index of its game, -1 for the list. game_row: the selected
 * setting. A press or touch let go sooner toggles the game as before, on its release. */
#define GAME_HOLD_US 500000u
#define GAME_SETTINGS 2 /* ON/OFF, the line finder */
static int game_view = -1, game_row;
static int held_game = -1, touched_game = -1; /* the game Enter / a touch went down on, -1 */
static uint64_t held_since, touched_since;
/* Two bounded copies of a Shell-readable config.ini, in the app process. */
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;
static vita2d_pvf *font;
/* Calibrated from full-width glyph advances, not PVF's nominal line height.
 * This is an em target; firmware glyph bearings/bitmap bounds can differ. */
static float font_em = 18.0f;
/* Icons are kept for the current five-game page only. A frame loads at most one; a game
 * whose icon cannot be loaded shows the placeholder until the next scan. */
#define ICON_WINDOW ROWS
typedef char icon_window_fits_the_cache[ICON_WINDOW <= VJO_ICON_SLOTS ? 1 : -1];
static VjoIconCache icon_cache;
static unsigned char icon_failed[VJO_GAMES_MAX];
static unsigned char icon_file[VJO_ICON_FILE_MAX + VJO_ICON_FILE_PAD];
/* Each game's title rows, laid out once a scan; other wrapped text once per
 * distinct string (settings/text_layout.c). */
static VjoTextLayout title_layout[VJO_GAMES_MAX];
static unsigned char title_ready[VJO_GAMES_MAX];
static VjoTextMemo text_memo;
static int visible[VJO_GAMES_MAX], total, count, selected, offset, filter, truncated;
static int writable = 1;
static char game_query[VJO_SEARCH_BYTES];
static char status[256];
static VjoConfig preferences;
/* #358: Suggest a text box, from region.ini (settings/preferences.h vjo_preferences_suggest): 1 on, 0 off, -1 not read */
static int suggest_on = -1;
static int page, preference_selected, preferences_writable;
static VjoEngineCard engine; /* the OCR card: the chosen size, its model files */
static char preference_status[160];
static int status_error, preference_error;

static float text_scale(float pixels) { return pixels / font_em; }

/* libvita2d's glyph atlas (vita2d_pvf.c, a8f15ab): one 512 x 512 texture per font holds every glyph the font has
 * drawn or measured, at its own 18-pixel em, and never lets one go; once it is full a new glyph is neither drawn nor
 * measured (atlas_add_glyph fails: the character is skipped, its width 0). The Look up page lays out dictionary
 * entries and the starts of 200 earlier lines, kanji by the hundred. So the app draws with one font and measures with
 * another (`measure_font`, never drawn with), and counts the distinct characters each has had; before a text that
 * would take a font past GLYPH_SWITCH it loads that font again, with an empty atlas, and goes on with the new one.
 * The replaced fonts are freed between frames, once the GPU is done with the frames that drew (font_tick). The
 * drawing font switches once a frame at most (a frame draws the screen's text: a few hundred characters); the
 * measuring font as often as a layout needs, up to MEASURE_RETIRED times a frame (2,800 new characters: an
 * entry of 8 KiB of kanji has fewer than 2,000 distinct ones; past that the font goes on, its atlas full). The atlas holds 502 to 639 glyphs before it refuses one (an estimate:
 * libvita2d's packer simulated with the glyphs of host/fixtures/word_lookup's lines and English, at estimated glyph
 * sizes); 350 keeps libvita2d's glyph table at 512 entries (int_htab.c resizes past 70 % full), whose 4 KiB its
 * int_htab_free does not give back: each switch leaves 4 KiB of the 8 MiB heap behind (an estimate from its source).
 * A font's atlas texture is made through the link's wrapper (settings/vita2d_textures.c), so freeing it frees
 * nothing else. */
#define GLYPH_SLOTS 1024
#define GLYPH_SWITCH 350
#define MEASURE_RETIRED 8
typedef struct {
    uint32_t seen[GLYPH_SLOTS]; /* code point + 1 of each character the font has had; 0 free; half full at most */
    int count;
} GlyphSet;
static GlyphSet drawn_glyphs, measured_glyphs;
static vita2d_pvf *measure_font; /* text_width's font; NULL (it could not be loaded): the drawing font measures */
static vita2d_pvf *font_retired, *measure_retired[MEASURE_RETIRED]; /* replaced, until the GPU is done */
static int measure_retired_n, font_tried, font_switches, measure_switches;

/* The characters of s the set has not had, added to it: their number. A full set (GLYPH_SLOTS / 2, past
 * GLYPH_SWITCH) counts a character it has no room for as new: its font switches, and the set is emptied. */
static int glyphs_add(GlyphSet *g, const char *s)
{
    size_t n = strlen(s), i = 0;
    int fresh = 0;
    while (i < n) {
        uint32_t key = vjo_utf8_next(s, n, &i) + 1, slot = (key * 2654435761u) >> 22;
        while (g->seen[slot] && g->seen[slot] != key)
            slot = (slot + 1) % GLYPH_SLOTS;
        if (g->seen[slot])
            continue;
        if (g->count >= GLYPH_SLOTS / 2)
            return fresh + 1;
        g->seen[slot] = key;
        g->count++;
        fresh++;
    }
    return fresh;
}

/* Before s is drawn: a drawing font with an empty atlas when s's new characters would take this one past
 * GLYPH_SWITCH (once a frame: a font that cannot be loaded is tried again at the next). */
static void glyphs_for(const char *s)
{
    int before = drawn_glyphs.count;
    if (!glyphs_add(&drawn_glyphs, s) || drawn_glyphs.count <= GLYPH_SWITCH || !before || font_retired || font_tried)
        return;
    font_tried = 1;
    vita2d_pvf *next = vita2d_load_default_pvf();
    if (!next)
        return;
    font_retired = font; /* it holds the glyphs drawn before s: GLYPH_SWITCH at most */
    font = next;
    memset(&drawn_glyphs, 0, sizeof(drawn_glyphs));
    glyphs_add(&drawn_glyphs, s);
    font_switches++;
}

/* Before s is measured: the same for the measuring font, as often as there is a slot for the font it replaces. */
static vita2d_pvf *measured_for(const char *s)
{
    int before;
    vita2d_pvf *next;
    if (!measure_font) {
        glyphs_for(s); /* (one font: it measures and draws) */
        return font;
    }
    before = measured_glyphs.count;
    if (!glyphs_add(&measured_glyphs, s) || measured_glyphs.count <= GLYPH_SWITCH || !before ||
        measure_retired_n == MEASURE_RETIRED || !(next = vita2d_load_default_pvf()))
        return measure_font;
    measure_retired[measure_retired_n++] = measure_font;
    measure_font = next;
    memset(&measured_glyphs, 0, sizeof(measured_glyphs));
    glyphs_add(&measured_glyphs, s);
    measure_switches++;
    return measure_font;
}

/* Between frames: the fonts a switch replaced are freed once the GPU has finished the frames that drew. */
static void font_tick(void)
{
    font_tried = 0;
    if (!font_retired && !measure_retired_n)
        return;
    vita2d_wait_rendering_done();
    if (font_retired) {
        vita2d_free_pvf(font_retired);
        font_retired = NULL;
        vjo_log("font: an empty glyph atlas to draw with after %d characters (%d time%s)", GLYPH_SWITCH,
                font_switches, font_switches == 1 ? "" : "s");
    }
    if (measure_retired_n) {
        while (measure_retired_n)
            vita2d_free_pvf(measure_retired[--measure_retired_n]);
        vjo_log("font: an empty glyph atlas to measure with after %d characters (%d time%s)", GLYPH_SWITCH,
                measure_switches, measure_switches == 1 ? "" : "s");
    }
}

static void text(int x, int y, unsigned color, float pixels, const char *s)
{
    glyphs_for(s);
    vita2d_pvf_draw_text(font, x, y, color, text_scale(pixels), s);
}

static int text_width(float pixels, const char *s)
{
    vita2d_pvf *with = measured_for(s);
    return vita2d_pvf_text_width(with, text_scale(pixels), s);
}

static void draw_rows(int x, int y, unsigned color, float pixels, int line_height, const char *s,
                      const VjoTextLayout *layout)
{
    char line[512];
    for (int r = 0; r < layout->rows; r++) {
        vjo_text_layout_row(s, layout, r, line);
        text(x, y + r * line_height, color, pixels, line);
    }
}

/* s in at most `lines` rows of `width` pixels, the last cut with "...". The
 * layout measures every prefix of a row, so it is made once per distinct
 * text and kept (settings/text_layout.c); the drawn rows are the same. */
static void wrapped_text(int x, int y, int width, unsigned color,
                         float pixels, int line_height, int lines, const char *s)
{
    VjoTextLayout scratch;
    draw_rows(x, y, color, pixels, line_height, s,
              vjo_text_memo_layout(&text_memo, s, width, pixels, lines, text_width, &scratch));
}

static void centered_text(int x, int y, int width, unsigned color,
                          float pixels, const char *s)
{ text(x + (width - text_width(pixels, s)) / 2, y, color, pixels, s); }

static void focus_card(int y, int height, int focus)
{
    vita2d_draw_rectangle(24, y, 912, height, focus ? SELECTED : CARD);
    if (focus) {
        vita2d_draw_rectangle(24, y, 4, height, ACCENT);
        vita2d_draw_rectangle(24, y, 912, 2, ACCENT);
        vita2d_draw_rectangle(24, y + height - 2, 912, 2, ACCENT);
        vita2d_draw_rectangle(932, y, 4, height, ACCENT);
    }
}

static void badge(int x, int y, int width, int height, const char *label, int active)
{
    vita2d_draw_rectangle(x, y, width, height, active ? ACCENT : SELECTED);
    centered_text(x, y + height / 2 + 9, width, active ? BG : WHITE, 26, label);
}

/* The app's version, beside its title: the release's (vita/CMakeLists.txt
 * VJO_VERSION) and, in the debug build, "debug" (docs/release-variants.md). */
#ifndef VJO_VERSION_STRING
#define VJO_VERSION_STRING "dev"
#endif
#if VJO_DEBUG
#define VJO_APP_VERSION "v" VJO_VERSION_STRING " debug"
#else
#define VJO_APP_VERSION "v" VJO_VERSION_STRING
#endif

/* The page tabs, left to right; Select goes to the next (docs/one-vpk-install.md has the Install page, #312 the
 * Stats page, #313 the Look up page). A new page is an entry here and a label in tab_label: the tab bar lays itself
 * out (tab_layout). */
enum { PAGE_GAMES, PAGE_OCR, PAGE_ANKI, PAGE_LOOKUP, PAGE_STATS, PAGE_INSTALL, PAGES };
static const char *const tab_label[PAGES] = {"Games", "OCR & lookup", "Anki", "Look up", "Stats", "Install"};
/* The tab bar, from x 24 to 936 under the title row: tabs TAB_GAP apart, at the most TAB_PX pixels of label.
 * While every label fits an equal share (912 / PAGES, less the gap), the tabs are equal (four were 220 px at a
 * pitch of 228). Else each tab is its label's width with TAB_PAD each side, the room left shared out equally; and
 * when even that does not fit, the labels are smaller (TAB_MIN_PX at the least; past that the tabs are equal and
 * their labels cut at their edges). Laid out once the font is loaded, for the drawing and the touches both. */
#define TAB_LEFT 24
#define TAB_ROOM 912
#define TAB_GAP 8
#define TAB_PAD 10
#define TAB_PX 28
#define TAB_MIN_PX 20
static int tab_x[PAGES], tab_w[PAGES];
static float tab_px = TAB_PX;
/* When this app and the overlay plugins do not match (app_check.h), a red
 * band in place of the title row says so on every page, in two rows. */
#define BANNER_Y 4
#define BANNER_H 46
#define BANNER_TEXT_Y 24
#define BANNER_LINE_H 22
static char app_banner[192];
static int tab_clip; /* a label is wider than its tab: the tabs' labels are cut at their edges */

static void tab_layout(void)
{
    int share = TAB_ROOM / PAGES - TAB_GAP, room = TAB_ROOM - (PAGES - 1) * TAB_GAP, need = 0, equal = 1;
    int label[PAGES];
    for (float px = TAB_PX; px >= TAB_MIN_PX; px -= 2) {
        tab_px = px;
        need = 0;
        equal = 1;
        for (int i = 0; i < PAGES; i++) {
            int width = text_width(px, tab_label[i]);
            label[i] = width + 2 * TAB_PAD;
            need += label[i];
            equal &= width <= share;
        }
        if (equal || need <= room)
            break;
    }
    if (need > room)
        equal = 1; /* nothing fits: equal tabs, each label cut at its edges */
    tab_clip = 0;
    for (int i = 0, x = TAB_LEFT, left = room - need; i < PAGES; i++) {
        /* equal shares, else the label's width and its part of what is left (the first ones a pixel more) */
        int w = equal ? share : label[i] + left / PAGES + (i < left % PAGES);
        tab_x[i] = x;
        tab_w[i] = w;
        tab_clip |= label[i] - 2 * TAB_PAD > w;
        x += equal ? TAB_ROOM / PAGES : w + TAB_GAP;
    }
}

/* The tab at x, y, or -1. */
static int tab_at(int x, int y)
{
    if (y < TAB_Y || y >= TAB_Y + TAB_H)
        return -1;
    for (int i = 0; i < PAGES; i++)
        if (x >= tab_x[i] && x < tab_x[i] + tab_w[i])
            return i;
    return -1;
}

static void header(void)
{
    static int version_x; /* after the title, measured once */
    vita2d_draw_rectangle(0, 0, 960, 4, ACCENT);
    if (app_banner[0]) {
        vita2d_draw_rectangle(0, BANNER_Y, 960, BANNER_H, OFF);
        wrapped_text(24, BANNER_TEXT_Y, 912, BG, 20, BANNER_LINE_H, 2, app_banner);
    } else {
        wrapped_text(24, 35, 690, WHITE, 32, 34, 1, "Rubi");
        if (!version_x) version_x = 24 + text_width(32, "Rubi") + 14;
        text(version_x, 34, DIM, 22, VJO_APP_VERSION);
        text(744, 34, DIM, 24, "Select: page");
    }
    for (int i = 0; i < PAGES; i++) {
        vita2d_draw_rectangle(tab_x[i], TAB_Y, tab_w[i], TAB_H, page == i ? ACCENT : CARD);
        if (tab_clip) { /* a label wider than its tab, even at TAB_MIN_PX: cut at its edges */
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(tab_x[i], TAB_Y, tab_x[i] + tab_w[i], TAB_Y + TAB_H);
        }
        centered_text(tab_x[i], TAB_Y + (TAB_H + (int)(tab_px * 0.65f)) / 2, tab_w[i], page == i ? BG : WHITE, tab_px,
                      tab_label[i]);
        if (tab_clip)
            vita2d_disable_clipping();
    }
}

/* Status box (first baseline text_y, lines of 28 px). */
static void status_box(int y, int height, int text_y, int lines, const char *message, int error)
{
    vita2d_draw_rectangle(24, y, 912, height, CARD);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(24, y + 2, 936, y + height);
    wrapped_text(40, text_y, 880, error ? OFF : WHITE, 24, 28, lines, message);
    vita2d_disable_clipping();
}

/* Status box and the control hints. */
static void footer_at(int y, int height, int text_y, int lines, const char *message, int error,
                      const char *controls)
{
    status_box(y, height, text_y, lines, message, error);
    wrapped_text(24, 535, 912, DIM, 24, 26, 1, controls);
}

static void footer(const char *message, int error, const char *controls)
{
    footer_at(440, 64, 467, 2, message, error, controls);
}

/* The console's enter and cancel buttons (read once at start) and the hints that name them. */
static VjoButtons buttons;
static char preference_hints[96], game_hints[96], games_empty_hints[96];
/* #333: the selected game row's "Hold ×: settings" (at a fixed x, so a frame measures no text for it) and the game
 * page's hints */
#define LINE_FIX_TAG_X 360
#define HOLD_HINT_X 600
static char hold_hint[32], game_page_hints[96];

static void read_buttons(void)
{
    buttons = vjo_buttons_read();
    snprintf(preference_hints, sizeof(preference_hints), "Up/Down: setting   Left/Right or %s: choose   %s: exit",
             buttons.enter_glyph, buttons.cancel_glyph);
    snprintf(game_hints, sizeof(game_hints), "%s: toggle   L/R: page   Left/Right: letter   %s: exit",
             buttons.enter_glyph, buttons.cancel_glyph);
    snprintf(games_empty_hints, sizeof(games_empty_hints), "Square: filter   Start: refresh   Select: page   %s: exit",
             buttons.cancel_glyph);
    snprintf(hold_hint, sizeof(hold_hint), "Hold %s: settings", buttons.enter_glyph);
    snprintf(game_page_hints, sizeof(game_page_hints), "Up/Down: setting   Left/Right or %s: choose   %s: back",
             buttons.enter_glyph, buttons.cancel_glyph);
}

static const VjoAnkiUi anki_ui = {text, wrapped_text, focus_card, badge, footer_at, header,
                                  WHITE, DIM, ACCENT, OFF, CARD, &buttons, text_width};

static void rect(int x, int y, int width, int height, unsigned color) { vita2d_draw_rectangle(x, y, width, height, color); }
/* #312: the Stats page (settings/stats_page.c), drawn with these helpers */
static const VjoStatsUi stats_ui = {text, text_width, wrapped_text, focus_card, badge, footer_at, header, rect,
                                    WHITE, DIM, ACCENT, OFF, CARD, SELECTED, &buttons};
static void hints_row(const char *controls) { wrapped_text(24, 535, 912, DIM, 24, 26, 1, controls); }
static const VjoInstallDraw install_ui = {text, text_width, rect, focus_card, badge, hints_row, header,
                                          WHITE, DIM, ACCENT, OFF, SELECTED, &buttons};

static void clip(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0) {
        vita2d_disable_clipping();
        return;
    }
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(x0, y0, x1, y1);
}
static const VjoLookupUi lookup_ui = {text, text_width, wrapped_text, rect, focus_card, badge, footer_at, header, clip,
                                      WHITE, DIM, ACCENT, OFF, CARD, SELECTED, &buttons};

static void release_icon(void *texture) { vita2d_free_texture(texture); }

/* Frees every cached icon once the GPU has finished the frames that drew them. */
static void clear_icons(void)
{
    if (vjo_icon_cache_count(&icon_cache)) vita2d_wait_rendering_done();
    vjo_icon_cache_clear(&icon_cache, release_icon);
}

/* An icon file as a texture, or NULL: read whole in one bounded read,
 * checked (settings/icons.c), then decoded by libvita2d from memory. */
static vita2d_texture *load_icon(const char *path)
{
    long size = path[0] ? vjo_icon_read(path, icon_file) : VJO_ICON_ABSENT;
    if (size < 0 || vjo_icon_png_check(icon_file, (size_t)size, NULL, NULL) != VJO_ICON_OK) return NULL;
    vita2d_texture *texture = vita2d_load_PNG_buffer(icon_file);
    /* libvita2d mallocs a texture without clearing it, and a plain one never
     * sets these two: vita2d_free_texture would destroy a stale render
     * target or free a stale block left in that memory (libvita2d #28). */
    if (texture) {
        texture->gxm_rtgt = NULL;
        texture->depth_UID = 0;
    }
    return texture;
}

/* Compact cards: one title line; the ID has its own line below it. */
static const VjoTextLayout *title_rows(int game)
{
    if (!title_ready[game]) {
        vjo_text_layout(library[game].title, 650, 22, 1, text_width, &title_layout[game]);
        title_ready[game] = 1;
    }
    return &title_layout[game];
}

/* The current page only: no off-page icon reads or title layout. */
static int icon_window(int out[ICON_WINDOW])
{
    int n = 0;
    for (int r = 0; r < ROWS && offset + r < count; r++) out[n++] = visible[offset + r];
    return n;
}

/* Release off-page icons after the GPU is done, then load at most one visible icon.
 * Only the current page has textures or title-layout work. */
static void prepare_window(void)
{
    int window[ICON_WINDOW], n = icon_window(window);
    const char *keep[ICON_WINDOW];
    for (int i = 0; i < n; i++) keep[i] = library[window[i]].id;
    void *old = vjo_icon_cache_evict(&icon_cache, keep, n);
    if (old) {
        vita2d_wait_rendering_done();
        do { vita2d_free_texture(old); } while ((old = vjo_icon_cache_evict(&icon_cache, keep, n)));
    }
    for (int i = 0; i < n; i++) {
        const VjoInstalledGame *g = &library[window[i]];
        if (icon_failed[window[i]] || vjo_icon_cache_get(&icon_cache, g->id)) continue;
        vita2d_texture *texture = load_icon(g->icon);
        if (!texture) icon_failed[window[i]] = 1;
        else if (vjo_icon_cache_put(&icon_cache, g->id, texture) < 0)
            vita2d_free_texture(texture); /* not reached; never drawn */
        return;
    }
    for (int i = 0; i < n; i++)
        if (!title_ready[window[i]]) {
            title_rows(window[i]);
            return;
        }
}

/* A PSP game's row: a bubble, Adrenaline, the shared row, or a title in no
 * folder whose ID is a PSP game's (PSPEMUCFW, a disc ID: core/games.h
 * vjo_game_is_psp), as the Shell and the kernel's loader key them. */
static int psp_row(const VjoInstalledGame *g)
{ return vjo_catalog_psp(g) || (g->kind == VJO_ROW_MISSING && vjo_game_is_psp(g->id)); }

/* A row's ON/OFF: a PSP game with no rule of its own follows the shared PSP row. */
static int row_on(const VjoInstalledGame *g) { return vjo_games_enabled_as(&policy, g->id, psp_row(g)); }


static void rebuild(void)
{
    count = 0;
    for (int i = 0; i < total; i++) {
        int on = row_on(&library[i]);
        if ((!filter || (filter == 1 && on) || (filter == 2 && !on)) &&
            vjo_search_matches(library[i].title, library[i].id, game_query)) visible[count++] = i;
    }
    if (selected >= count) selected = count ? count - 1 : 0;
    offset = selected / ROWS * ROWS;
}

static void save(void)
{
    if (!writable) {
        status_error = 1;
        policy = saved;
        snprintf(status, sizeof(status), "Cannot read settings. Repair games.ini first.");
    } else if (vjo_games_file_save(&policy, &scratch) < 0) {
        status_error = 1;
        policy = saved;
        /* A final sync can fail after publication; show the actual disk state. */
        if (vjo_games_file_load(&policy, &scratch) >= 0) saved = policy;
        snprintf(status, sizeof(status), "Save not confirmed. Check your choices, then retry.");
    } else {
        status_error = 0;
        saved = policy;
        snprintf(status, sizeof(status), "Saved. It applies when the game next starts or resumes.");
    }
    rebuild();
}

/* Enter: the game is turned ON or OFF. No taiHEN configuration changes: the
 * kernel plugin loads the OCR worker into every game that is ON when it starts
 * (#138, Agent A), PSP bubbles and Adrenaline included. index: the game's
 * library row (the list's selection, or the game page's game). */
static void toggle_game(int index)
{
    VjoInstalledGame *g = &library[index];
    int on = row_on(g);
    if (vjo_games_set(&policy, g->id, !on) < 0) {
        status_error = 1;
        snprintf(status, sizeof(status), "Settings are full (512 title overrides). No change saved.");
        return;
    }
    save();
}

static void toggle(void)
{
    if (count) toggle_game(visible[selected]);
}

/* The rules with this choice ("= enabled" or "= disabled"), ", " between them
 * while they fit in cap ("and N more" after): by title (titles != 0: a game's
 * row title, its ID when its row is a missing title's or it has none; the
 * Shell's games only) or by ID (every rule, for log.txt). Their number. */
static int rule_list(int enabled, int titles, char *out, size_t cap)
{
    int n = 0, more = 0;
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < policy.count; i++) {
        const VjoGameRule *r = &policy.rules[i];
        if (r->enabled != enabled || (titles && !vjo_game_is_candidate(r->title_id))) continue;
        const char *label = r->title_id;
        for (int k = 0; titles && k < total; k++)
            if (library[k].kind != VJO_ROW_MISSING && !strcmp(library[k].id, r->title_id)) label = library[k].title;
        n++;
        size_t need = strlen(label) + (used ? 2 : 0);
        if (more || used + need + 16 >= cap) { more++; continue; } /* 16: room for " and N more" */
        used += (size_t)snprintf(out + used, cap - used, "%s%s", used ? ", " : "", label);
    }
    if (more) snprintf(out + used, cap - used, "%sand %d more", used ? " " : "", more);
    return n;
}

/* Triangle, or the Default row touched. ON -> OFF disables all (#138): no game
 * stays ON by a choice of its own (core/games.h vjo_games_disable_all); the
 * games turned OFF one by one stay OFF. OFF -> ON turns on every game but
 * those. The status names the games either way, and log.txt the rules. */
static void toggle_default(void)
{
    char names[160], ids[160];
    int disable = policy.default_enabled != 0;
    /* disable: the ON ones it turns OFF; else those staying OFF */
    int n = rule_list(disable, 1, names, sizeof(names)), rules = rule_list(disable, 0, ids, sizeof(ids));
    if (disable) vjo_games_disable_all(&policy);
    else policy.default_enabled = 1;
    save();
    if (status_error) return; /* save() said why, and put the saved choices back */
    if (disable)
        vjo_log("games: Default OFF, %d own ON choice%s removed%s%s", rules, rules == 1 ? "" : "s", rules ? ": " : "", ids);
    else
        vjo_log("games: Default ON, %d own OFF choice%s kept%s%s", rules, rules == 1 ? "" : "s", rules ? ": " : "", ids);
    if (disable && n)
        snprintf(status, sizeof(status), "Saved: every game is OFF from its next start or resume. Turned OFF too: %s.", names);
    else if (disable)
        snprintf(status, sizeof(status), "Saved: every game is OFF from its next start or resume. Turn ON the ones you want.");
    else if (n)
        snprintf(status, sizeof(status), "Saved: every game but %d is ON from its next start or resume. Still OFF: %s.", n, names);
    else
        snprintf(status, sizeof(status), "Saved: every game is ON from its next start or resume.");
}

static void change_filter(void)
{
    filter = (filter + 1) % 3;
    selected = offset = 0;
    rebuild();
}

/* ---- #333: a game's own page: its ON/OFF and its line finder (config.ini's ocr_finder and ocr_line_fix for this
 * game alone) ---- */

/* The line finder's choices on the page, left to right: Default (no row: config.ini's, or a PSP game the shared PSP
 * row's), the spotter, then Rubi's finder with each ocr_line_fix value: in the values' order (core/games.h
 * VJO_LINE_FINDER_*), so a choice's value is its place less one (-1 for Default); and each value's label. */
#define LINE_FINDER_CHOICES (VJO_LINE_FINDERS + 1)
static const char *const finder_labels[VJO_LINE_FINDERS] = {"Spotter", "Off", "On", "Junk", "All"};
static int choice_value(int choice) { return choice - 1; } /* choice 0: Default */
static int choice_of(int value) { return value + 1; }

/* The game's own row in ocr_line_fix.ini (-1: none), and the value its jobs use as the Shell reads them
 * (shell/game.c): its own, else a PSP game's shared row's (NPXS10028), else -1, config.ini's. */
static int own_line_fix(const VjoInstalledGame *g) { return vjo_games_row_as(&line_fix, g->id, 0); }
static int used_line_fix(const VjoInstalledGame *g) { return vjo_games_row_as(&line_fix, g->id, psp_row(g)); }

static const char game_page_status[] = "This game's own settings. They apply when it next starts or resumes.";
static char list_status[sizeof(status)]; /* the list's status while the page shows its own */
static int list_status_error;

static void open_game_page(int index)
{
    game_view = index;
    game_row = 1; /* the line finder: what the page is for; Up goes to its ON/OFF */
    memcpy(list_status, status, sizeof(status));
    list_status_error = status_error;
    status_error = 0;
    snprintf(status, sizeof(status), "%s", game_page_status);
}

static void close_game_page(void)
{
    game_view = -1;
    if (!strcmp(status, game_page_status)) { /* nothing was saved there: the list's own status again */
        memcpy(status, list_status, sizeof(status));
        status_error = list_status_error;
    }
    rebuild(); /* the list as its filter has it now */
}

/* value: -1 Default (the row goes), else a VJO_LINE_FINDER_* value. Saved at once, as games.ini's choices are. */
static void set_line_fix(int value)
{
    const VjoInstalledGame *g = &library[game_view];
    if (value < -1 || value >= VJO_LINE_FINDERS || value == own_line_fix(g)) return;
    if (!line_fix_writable) {
        status_error = 1;
        snprintf(status, sizeof(status), "Cannot read ocr_line_fix.ini. Repair or delete it first; nothing was saved.");
        return;
    }
    if (vjo_line_fix_set(&line_fix, g->id, value) < 0) {
        status_error = 1;
        snprintf(status, sizeof(status), "512 games have a line finder of their own. No change saved.");
        return;
    }
    if (vjo_line_fix_file_save(&line_fix, &scratch) < 0) {
        status_error = 1;
        /* A final sync can fail after publication: show what the card holds. */
        line_fix = line_fix_saved;
        if (vjo_line_fix_file_load(&line_fix, &scratch) >= 0) line_fix_saved = line_fix;
        else line_fix = line_fix_saved;
        snprintf(status, sizeof(status), "Save not confirmed. Check your choice, then retry.");
        return;
    }
    status_error = 0;
    line_fix_saved = line_fix;
    vjo_log("games: %s's line finder: %s", g->id, value < 0 ? "Default" : vjo_line_finder_names[value]);
    snprintf(status, sizeof(status), "Saved. It applies when the game next starts or resumes.");
}

/* config.ini's line finder as a value (the spotter, or Rubi's finder with its ocr_line_fix), -1 when unreadable. */
static int config_line_finder(void)
{
    if (!preferences_writable) return -1;
    return preferences.ocr_finder == VJO_FINDER_SPOTTER ? VJO_LINE_FINDER_SPOTTER
                                                        : VJO_LINE_FINDER_GRAD(preferences.ocr_line_fix);
}

/* The line finder card's help: what the game's own choice does, or what Default gives it. */
static void line_fix_help(const VjoInstalledGame *g, char *out, size_t cap)
{
    static const char *const what[VJO_LINE_FINDERS] = {
        "Spotter: vita-vn-ocr's text spotter finds the lines.",
        "Off: Rubi's finder without its fixes, as before.",
        "On: Rubi's finder; a line's parts across a wide gap stay one line.",
        "Junk: Rubi's finder, no box frames as lines. Box with a margin.",
        "All: Junk, and touching rows split. Box with a margin."};
    int own = own_line_fix(g), shared = -1, config = config_line_finder();
    if (own >= 0) {
        snprintf(out, cap, "%s", what[own]);
        return;
    }
    if (psp_row(g) && strcmp(g->id, VJO_PSP_EMULATOR)) shared = vjo_games_row_as(&line_fix, VJO_PSP_EMULATOR, 0);
    if (shared >= 0) snprintf(out, cap, "Default: %s, as PSP games without their own.", finder_labels[shared]);
    else if (config == VJO_LINE_FINDER_SPOTTER) snprintf(out, cap, "Default: config.ini's, the spotter.");
    else if (config >= 0) snprintf(out, cap, "Default: config.ini's, Rubi's finder, %s.", finder_labels[config]);
    else snprintf(out, cap, "Default: config.ini's (it cannot be read).");
}

/* Left/Right on the page: the setting's previous or next choice (OFF/ON on its ON/OFF card). */
static void game_page_step(int right)
{
    if (game_row == 0) {
        if (row_on(&library[game_view]) != right) toggle_game(game_view);
        return;
    }
    int choice = choice_of(own_line_fix(&library[game_view])) + (right ? 1 : -1);
    if (choice >= 0 && choice < LINE_FINDER_CHOICES) set_line_fix(choice_value(choice));
}

/* Enter on the page: ON/OFF toggles; the line finder goes to its next choice, after All back to Default. */
static void game_page_enter(void)
{
    if (game_row == 0) toggle_game(game_view);
    else set_line_fix(choice_value((choice_of(own_line_fix(&library[game_view])) + 1) % LINE_FINDER_CHOICES));
}

/* The catalog is read here only: at start and on Start. Icons and title
 * layouts belong to the old list and go with it. */
static void scan(void)
{
    clear_icons();
    memset(icon_failed, 0, sizeof(icon_failed));
    memset(title_ready, 0, sizeof(title_ready));
    total = vjo_catalog_scan(library, VJO_GAMES_MAX, &truncated, &policy);
    rebuild();
    if (truncated) { status_error = 0; snprintf(status, sizeof(status), "Showing the first 512 installed titles."); }
}

static void move(int delta)
{
    if (!count) return;
    selected += delta;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    offset = selected / ROWS * ROWS;
}

/* L/R: non-overlapping five-game pages, keeping the selected row when it exists.
 * At a boundary, select the first/last game. The final page can have fewer than five. */
static void page_jump(int direction)
{
    if (!count) return;
    int last = (count - 1) / ROWS * ROWS, top = offset + direction * ROWS;
    if (top > last) top = last;
    if (top < 0) top = 0;
    if (direction > 0 ? top <= offset : top >= offset) {
        selected = direction > 0 ? count - 1 : 0;
        return;
    }
    selected += top - offset;
    if (selected >= count) selected = count - 1;
    offset = top;
}

/* The letter of the list's index-th game (settings/letter.c). */
static uint32_t initial_at(int index) { return vjo_title_letter(library[visible[index]].title); }

/* Left/Right: the first game of the previous or next letter (the catalog
 * lists each letter's games together, settings/letter.c), in its five-game page.
 * Left inside a letter goes to that letter's first game; Right in the last
 * letter goes to the last game. */
static void letter_jump(int direction)
{
    if (!count) return;
    uint32_t letter = initial_at(selected);
    int i = selected;
    if (direction > 0) {
        while (i < count && initial_at(i) == letter) i++;
        if (i == count) i = count - 1;
    } else {
        while (i > 0 && initial_at(i - 1) == letter) i--;
        if (i == selected && i > 0) {
            letter = initial_at(--i);
            while (i > 0 && initial_at(i - 1) == letter) i--;
        }
    }
    selected = i;
    offset = selected / ROWS * ROWS;
}

/* A list key just pressed, or held long enough to repeat: one move a frame. */
static void list_key(uint32_t keys)
{
    if (keys & SCE_CTRL_UP) move(-1);
    else if (keys & SCE_CTRL_DOWN) move(1);
    else if (keys & SCE_CTRL_LTRIGGER) page_jump(-1);
    else if (keys & SCE_CTRL_RTRIGGER) page_jump(1);
    else if (keys & SCE_CTRL_LEFT) letter_jump(-1);
    else if (keys & SCE_CTRL_RIGHT) letter_jump(1);
}

/* The first enabled dictionary's own name (page 0 bytes 192..255 of its
 * file: Bee's Ultimate Dictionary names itself so, core/local_dict.h), for the
 * Dictionaries shown card; "" when it has none or is missing. Read with the
 * preferences: one 256-byte read, never per frame. */
static char dictionary_title[VJO_LOCAL_TITLE_SIZE];

static void read_dictionary_title(void)
{
    const char *names = preferences.local_dictionaries;
    char path[256];
    uint8_t page0[256];
    size_t n = 0;
    int fd, got;
    dictionary_title[0] = '\0';
    if (vjo_local_names_count(names) <= 0)
        return;
    while (*names == ' ' || *names == '\t')
        names++;
    while (names[n] && names[n] != ',' && names[n] != ' ' && names[n] != '\t')
        n++;
    got = snprintf(path, sizeof(path), "%s/%.*s", preferences.local_dictionary_dir, (int)n, names);
    if (got < 0 || got >= (int)sizeof(path) || (fd = sceIoOpen(path, SCE_O_RDONLY, 0)) < 0)
        return;
    got = sceIoRead(fd, page0, sizeof(page0));
    sceIoClose(fd);
    if (got > 0)
        vjo_local_title(page0, (size_t)got, dictionary_title);
}

static void check_models(void)
{
    /* The local engine's model sizes: which tiers' files are there (the card
     * names the chosen size's model, or reports it missing); the bundled
     * dictionary's name. */
    vjo_engine_card(&preferences, &engine);
    read_dictionary_title();
}

static void load_preferences(void)
{
    int rc = vjo_preferences_load(&preferences, &scratch);
    suggest_on = vjo_preferences_suggest(&scratch);
    preferences_writable = rc >= 0;
    preference_error = rc < 0;
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0
        ? "Cannot read config.ini. Choices have not been changed."
        : rc > 0 ? "Backup recovered. Changes save automatically."
                 : "Changes apply when you next open the overlay.");
    check_models();
}

/* Nothing is saved while config.ini cannot be read; says so. */
static int preferences_locked(void)
{
    if (preferences_writable) return 0;
    preference_error = 1;
    snprintf(preference_status, sizeof(preference_status), "Cannot read config.ini. Choices have not been changed.");
    return 1;
}

/* After a save (rc): the file read again for the displayed choices, even on
 * failure (a final sync can fail after publication). */
static void preference_saved(int rc)
{
    int read_rc = vjo_preferences_load(&preferences, &scratch);
    preferences_writable = read_rc >= 0;
    preference_error = rc < 0 || read_rc < 0;
    check_models();
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0 || read_rc < 0
        ? "Save not confirmed. Check the displayed choices, then retry."
        : "Saved. It applies when the overlay next opens.");
}

static void change_preference(int tier, int japanese_only, int highlight_mode, int dictionary_results)
{
    if (preferences_locked()) return;
    preference_saved(vjo_preferences_save(tier, japanese_only, highlight_mode, dictionary_results, &scratch));
}

/* Read text (ocr_mode) and Speed boost while reading (ocr_boost_clock), saved
 * together; the Shell reads them when the game resumes and each time the
 * overlay opens. */
static void set_reading(int ocr_mode, int boost_clock)
{
    if (ocr_mode == preferences.ocr_mode && boost_clock == preferences.ocr_boost_clock) return;
    if (preferences_locked()) return;
    preference_saved(vjo_preferences_save_reading(ocr_mode, boost_clock, &scratch));
}

/* API 31, Text source (config.ini text_source): its three choices left to right, Automatic, Hooks in
 * every game, OCR only; a game reads it as it starts, so the save says so. */
static const int source_choice[3] = {VJO_SOURCE_AUTO, VJO_SOURCE_HOOKS, VJO_SOURCE_OCR};

static int source_index(int source) { return source == VJO_SOURCE_HOOKS ? 1 : source == VJO_SOURCE_OCR ? 2 : 0; }

static void set_text_source(int index)
{
    if (index < 0 || index > 2 || source_choice[index] == preferences.text_source) return;
    if (preferences_locked()) return;
    preference_saved(vjo_preferences_save_text_source(source_choice[index], &scratch));
    if (!preference_error)
        snprintf(preference_status, sizeof(preference_status), "Saved. It applies when a game next starts.");
}

/* API 43: Skip OCR on screens without text (ocr_text_gate) and Trusted hooks replace OCR (hook_trust), saved
 * together; they apply when the overlay next opens. */
static void set_skips(int text_gate, int hook_trust)
{
    if (text_gate == preferences.ocr_text_gate && hook_trust == preferences.hook_trust) return;
    if (preferences_locked()) return;
    preference_saved(vjo_preferences_save_skips(text_gate, hook_trust, &scratch));
}

/* #358, Suggest a text box (docs/first-box.md): in region.ini, not config.ini, so it is saved whatever config.ini's
 * state; the Shell reads region.ini as a game starts or resumes, so the save says so. */
static void set_suggest(int on)
{
    int rc;
    if (on == suggest_on || suggest_on < 0) return;
    rc = vjo_preferences_save_suggest(on, &scratch);
    suggest_on = vjo_preferences_suggest(&scratch);
    preference_error = rc < 0 || suggest_on < 0;
    snprintf(preference_status, sizeof(preference_status), "%s", preference_error
        ? "Save not confirmed. Check the displayed choices, then retry."
        : "Saved. It applies when the game next starts or resumes.");
}

/* API 47, Vertical text (config.ini ocr_vertical, docs/vita-vn-ocr.md "Vertical text"): its three choices left to
 * right, OFF (rows, as before), Automatic (columns where the rows look like vertical text), ON (columns, but rows
 * where the text box plainly holds rows of text: API 53, #379); the Shell reads it each time the overlay opens. */
static const int vertical_choice[3] = {VJO_VERTICAL_OFF, VJO_VERTICAL_AUTO, VJO_VERTICAL_ON};

static int vertical_index(int mode) { return mode == VJO_VERTICAL_AUTO ? 1 : mode == VJO_VERTICAL_ON ? 2 : 0; }

static void set_vertical(int index)
{
    if (index < 0 || index > 2 || vertical_choice[index] == preferences.ocr_vertical) return;
    if (preferences_locked()) return;
    preference_saved(vjo_preferences_save_vertical(vertical_choice[index], &scratch));
}

/* Model size: enter cycles Small -> Normal -> Large; Left/Right step within it. */
static void set_tier(int tier)
{
    if (tier < 1 || tier > (int)VJO_VOCR_TIERS || tier == preferences.vocr_tier) return;
    change_preference(tier, preferences.japanese_only_selection, preferences.highlight_mode,
                      preferences.local_dictionary_results);
}

/* Dictionaries shown: enter cycles 1..8; Left/Right step within it. */
static void set_dictionary_results(int results)
{
    if (results < 1 || results > VJO_LOCAL_MAX_DICTS || results == preferences.local_dictionary_results) return;
    change_preference(preferences.vocr_tier, preferences.japanese_only_selection, preferences.highlight_mode,
                      results);
}

static void toggle_preference(void)
{
    if (preference_selected == PREF_SOURCE) {
        set_text_source((source_index(preferences.text_source) + 1) % 3);
        return;
    }
    if (preference_selected == PREF_VERTICAL) {
        set_vertical((vertical_index(preferences.ocr_vertical) + 1) % 3);
        return;
    }
    if (preference_selected == PREF_RESULTS) {
        set_dictionary_results(preferences.local_dictionary_results % VJO_LOCAL_MAX_DICTS + 1);
        return;
    }
    if (preference_selected == PREF_SIZE) {
        set_tier(preferences.vocr_tier % (int)VJO_VOCR_TIERS + 1);
        return;
    }
    if (preference_selected == PREF_READ) {
        set_reading(preferences.ocr_mode == VJO_OCR_AUTO ? VJO_OCR_ON_PRESS : VJO_OCR_AUTO,
                    preferences.ocr_boost_clock);
        return;
    }
    if (preference_selected == PREF_BOOST) {
        set_reading(preferences.ocr_mode, !preferences.ocr_boost_clock);
        return;
    }
    if (preference_selected == PREF_GATE || preference_selected == PREF_TRUST) {
        set_skips(preferences.ocr_text_gate ^ (preference_selected == PREF_GATE),
                  preferences.hook_trust ^ (preference_selected == PREF_TRUST));
        return;
    }
    if (preference_selected == PREF_SUGGEST) {
        set_suggest(!suggest_on);
        return;
    }
    change_preference(preferences.vocr_tier,
        preference_selected == PREF_SELECTION ? !preferences.japanese_only_selection : preferences.japanese_only_selection,
        preference_selected == PREF_HIGHLIGHT ? !preferences.highlight_mode : preferences.highlight_mode,
        preferences.local_dictionary_results);
}

/* Left/Right choose the left/right option of the selected card (OFF/ON on the
 * ON/OFF cards). */
static void set_preference(int right)
{
    if (preference_selected == PREF_SOURCE) {
        set_text_source(source_index(preferences.text_source) + (right ? 1 : -1));
        return;
    }
    if (preference_selected == PREF_VERTICAL) {
        set_vertical(vertical_index(preferences.ocr_vertical) + (right ? 1 : -1));
        return;
    }
    if (preference_selected == PREF_RESULTS) {
        set_dictionary_results(preferences.local_dictionary_results + (right ? 1 : -1));
        return;
    }
    if (preference_selected == PREF_SIZE) {
        set_tier(preferences.vocr_tier + (right ? 1 : -1));
        return;
    }
    if (preference_selected == PREF_READ) {
        set_reading(right ? VJO_OCR_ON_PRESS : VJO_OCR_AUTO, preferences.ocr_boost_clock);
        return;
    }
    if (preference_selected == PREF_BOOST) {
        set_reading(preferences.ocr_mode, right);
        return;
    }
    if (preference_selected == PREF_GATE || preference_selected == PREF_TRUST) {
        set_skips(preference_selected == PREF_GATE ? right : preferences.ocr_text_gate,
                  preference_selected == PREF_TRUST ? right : preferences.hook_trust);
        return;
    }
    if (preference_selected == PREF_SUGGEST) {
        set_suggest(right);
        return;
    }
    change_preference(preferences.vocr_tier,
        preference_selected == PREF_SELECTION ? right : preferences.japanese_only_selection,
        preference_selected == PREF_HIGHLIGHT ? (right ? VJO_HIGHLIGHT_CHARACTER : VJO_HIGHLIGHT_WORD)
                                              : preferences.highlight_mode,
        preferences.local_dictionary_results);
}

/* ---- the page's column of cards (PREF_*), a card at a time in view ---- */
static const int pref_height[PREFERENCES] = {OCR_H, READ_H, CARD_H, CARD_H, CARD_H, CARD_H, CARD_H, CARD_H, CARD_H,
                                             CARD_H, CARD_H};
static int pref_top; /* the first card in view */

/* Card i's top in the column (the first card's: 0). */
static int pref_offset(int i)
{
    int y = 0;
    for (int k = 0; k < i; k++) y += pref_height[k] + PREF_GAP;
    return y;
}

/* Card i's top on screen. */
static int pref_y(int i) { return PREF_VIEW_Y + pref_offset(i) - pref_offset(pref_top); }

/* Card i is all in view. */
static int pref_whole(int i) { return i >= pref_top && pref_y(i) + pref_height[i] <= PREF_VIEW_END; }

/* Scrolls by the fewest cards that bring card i all into view. */
static void pref_show(int i)
{
    if (i < pref_top) pref_top = i;
    while (pref_top < i && pref_y(i) + pref_height[i] > PREF_VIEW_END) pref_top++;
}

/* The card shown at y in the cards' room, or -1 (a gap between two). */
static int pref_card_at(int y)
{
    for (int i = pref_top; i < PREFERENCES && pref_y(i) < PREF_VIEW_END; i++)
        if (y >= pref_y(i) && y < pref_y(i) + pref_height[i]) return i;
    return -1;
}

/* A Model size button: the tier's name and, smaller, its estimates. */
static void size_button(int tier, int x, int y, int chosen)
{
    const VjoVocrTierInfo *t = vjo_vocr_tier_info((unsigned)tier);
    char detail[32];
    vita2d_draw_rectangle(x, y, OCR_SIZE_W, OCR_SIZE_H, chosen ? ACCENT : SELECTED);
    vjo_engine_tier_detail(tier, detail, sizeof(detail));
    centered_text(x, y + 24, OCR_SIZE_W, chosen ? BG : WHITE, 24, t ? t->name : "?");
    centered_text(x, y + 47, OCR_SIZE_W, chosen ? BG : DIM, 22, detail);
}

static void draw_size_card(int y)
{
    /* The Model size (estimates from the models' metadata). */
    text(OCR_ROW_TITLE_X, y + OCR_SIZE_DY + 24, preferences_writable ? WHITE : DIM, 28, "Model size");
    text(OCR_ROW_TITLE_X, y + OCR_SIZE_DY + 47, DIM, 22, "estimated"); /* the buttons' time and memory */
    for (int t = 1; t <= (int)VJO_VOCR_TIERS; t++)
        size_button(t, OCR_SIZE_X + (t - 1) * OCR_SIZE_PITCH, y + OCR_SIZE_DY,
                    preferences_writable && preferences.vocr_tier == t);
    char label[120];
    unsigned color = DIM;
    if (!preferences_writable) {
        snprintf(label, sizeof(label), "Cannot read config.ini. Repair it before changing settings.");
        color = OFF;
    } else if (vjo_engine_status(&engine, label, sizeof(label))) {
        color = OFF;
    }
    wrapped_text(42, y + OCR_STATUS_DY, 870, color, 24, 26, 1, label);
}

/* The buttons that open the overlay (config.ini toggle_button, l+r unless
 * changed) as the Read text card names them; NULL for the rear touchpad's
 * double tap. */
static const char *open_buttons(int trigger)
{
    switch (trigger) {
    case VJO_TRIGGER_SELECT: return "Select";
    case VJO_TRIGGER_START: return "Start";
    case VJO_TRIGGER_SELECT_L: return "Select + L";
    case VJO_TRIGGER_SELECT_R: return "Select + R";
    case VJO_TRIGGER_REAR_DOUBLE_TAP: return NULL;
    default: return "L + R";
    }
}

/* A Read text choice: its name and, smaller, what it means for the popup. */
static void read_button(int x, int y, const char *name, const char *meaning, int chosen)
{
    vita2d_draw_rectangle(x, y, READ_BUTTON_W, READ_BUTTON_H, chosen ? ACCENT : SELECTED);
    centered_text(x, y + 24, READ_BUTTON_W, chosen ? BG : WHITE, 22, name);
    centered_text(x, y + 47, READ_BUTTON_W, chosen ? BG : DIM, 20, meaning);
}

/* Read text: recognized in the background when the screen settles (auto: the
 * popup ready at the press), or only once the overlay is opened (on_press:
 * nothing runs before; the press then waits for the reading). docs/auto-vs-press.md. */
static void draw_read_card(int y)
{
    const char *keys = open_buttons(preferences.toggle_button), *verb = keys ? "press" : "tap";
    char name[48], ready[48], waits[48];
    if (keys) snprintf(name, sizeof(name), "Only when I press %s", keys);
    else snprintf(name, sizeof(name), "Only when I double-tap the back");
    snprintf(ready, sizeof(ready), "The popup is ready when you %s", verb);
    snprintf(waits, sizeof(waits), "Nothing runs until you %s", verb);
    text(42, y + CARD_TITLE_Y, preferences_writable ? WHITE : DIM, 28, "Read text");
    read_button(READ_BUTTON_X, y + READ_BUTTON_DY, "Automatically (when it changes)", ready,
                preferences_writable && preferences.ocr_mode == VJO_OCR_AUTO);
    read_button(READ_BUTTON_X + READ_BUTTON_PITCH, y + READ_BUTTON_DY, name, waits,
                preferences_writable && preferences.ocr_mode == VJO_OCR_ON_PRESS);
}

/* Speed boost while reading: the game worker raises the ARM clock to 444 MHz
 * while a job recognizes and puts it back after; a clock already at 444 MHz or
 * more is left as it is (game_ocr/main.c). */
static void draw_boost_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Speed boost while reading (444 MHz)");
    badge(812, y + CARD_BADGE_Y, 108, CARD_BADGE_H, !preferences_writable ? "--" :
          preferences.ocr_boost_clock ? "ON" : "OFF", preferences_writable && preferences.ocr_boost_clock);
    text(42, y + CARD_HELP_Y, DIM, 24, "ON reads faster: 444 MHz only while text is read.");
}

static void draw_selection_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Japanese characters only");
    badge(812, y + CARD_BADGE_Y, 108, CARD_BADGE_H, !preferences_writable ? "--" :
          preferences.japanese_only_selection ? "ON" : "OFF",
          preferences_writable && preferences.japanese_only_selection);
    text(42, y + CARD_HELP_Y, DIM, 24, "ON skips Latin, numbers, punctuation and Hangul.");
}

static void draw_highlight_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Highlight");
    badge(532, y + CARD_BADGE_Y, 176, CARD_BADGE_H, "Whole word",
          preferences_writable && preferences.highlight_mode == VJO_HIGHLIGHT_WORD);
    badge(724, y + CARD_BADGE_Y, 196, CARD_BADGE_H, "Character",
          preferences_writable && preferences.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    text(42, y + CARD_HELP_Y, DIM, 24, "Word skips matched text. Character moves one character.");
}

static void draw_results_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Dictionaries shown");
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        char number[4];
        snprintf(number, sizeof(number), "%d", i + 1);
        badge(RESULT_BADGE_X + i * RESULT_BADGE_PITCH, y + CARD_BADGE_Y, RESULT_BADGE_W, CARD_BADGE_H,
              number, preferences_writable && preferences.local_dictionary_results == i + 1);
    }
    char results_help[120];
    int enabled = vjo_local_names_count(preferences.local_dictionaries);
    if (!preferences_writable)
        snprintf(results_help, sizeof(results_help), "Cannot read the saved choice.");
    else if (!enabled)
        snprintf(results_help, sizeof(results_help), "Set local_dictionaries in config.ini first.");
    /* one file has nothing to page to, whatever the count */
    else if (enabled == 1 && dictionary_title[0])
        snprintf(results_help, sizeof(results_help), "Best match only: %s.", dictionary_title);
    else if (preferences.local_dictionary_results == 1 || enabled == 1)
        snprintf(results_help, sizeof(results_help), "Best match only. %d %s enabled.",
                 enabled, enabled == 1 ? "dictionary" : "dictionaries");
    else
        snprintf(results_help, sizeof(results_help), "Best match, then the others below it. %d enabled.", enabled);
    text(42, y + CARD_HELP_Y, DIM, 24, results_help);
}

/* API 31: where the overlay's text comes from (docs/native-hooks.md). The saved Every game choice is
 * retained for compatibility; Automatic now tries the same native hooks in every enabled Vita game. */
static void draw_source_card(int y)
{
    static const char *const names[3] = {"Automatic", "Every game", "OCR only"};
    static const char *const help[3] = {"Try hooks in every enabled Vita game, else OCR. From the next start.",
                                        "Same as Automatic. Applies when a game next starts.",
                                        "Screenshots only, no hooks. Applies when a game next starts."};
    int chosen = source_index(preferences.text_source);
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Text source");
    for (int i = 0; i < 3; i++)
        badge(SOURCE_BADGE_X + i * SOURCE_BADGE_PITCH, y + CARD_BADGE_Y, SOURCE_BADGE_W, CARD_BADGE_H, names[i],
              preferences_writable && chosen == i);
    text(42, y + CARD_HELP_Y, DIM, 24, preferences_writable ? help[chosen] : "Cannot read the saved choice.");
}

/* API 43: a trusted hook (a Luna or Rubi verified site, or a string copy after four OCR agreements) answers with no
 * OCR at all (docs/native-hooks.md "Trusted hooks"); OFF checks hooks with OCR as Rubi 0.9.7 did. */
static void draw_trust_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Trusted hooks replace OCR");
    badge(812, y + CARD_BADGE_Y, 108, CARD_BADGE_H, !preferences_writable ? "--" : preferences.hook_trust ? "ON" : "OFF",
          preferences_writable && preferences.hook_trust);
    text(42, y + CARD_HELP_Y, DIM, 24, "ON: a hook Rubi trusts gives the line, with no OCR at all.");
}

/* API 43 (API 26's gate): a text box the text-presence gate sees no text in is not read (docs/vita-vn-ocr.md). */
static void draw_gate_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Skip OCR on screens without text");
    badge(812, y + CARD_BADGE_Y, 108, CARD_BADGE_H, !preferences_writable ? "--" :
          preferences.ocr_text_gate ? "ON" : "OFF", preferences_writable && preferences.ocr_text_gate);
    text(42, y + CARD_HELP_Y, DIM, 24, "ON: a hidden text box or a picture is not read.");
}

/* API 47: Japanese set in columns (manga in NOBORU, a vertical dialogue box), read right to left with the
 * vertical model (docs/vita-vn-ocr.md "Vertical text"). Same badges as Text source. */
static void draw_vertical_card(int y)
{
    static const char *const names[3] = {"OFF", "Automatic", "ON"};
    static const char *const help[3] = {"Rows only: horizontal text, as before.",
                                        "Columns where the text box holds vertical text.",
                                        "Columns, right to left, unless the text is in rows."};
    int chosen = vertical_index(preferences.ocr_vertical);
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Vertical text");
    for (int i = 0; i < 3; i++)
        badge(SOURCE_BADGE_X + i * SOURCE_BADGE_PITCH, y + CARD_BADGE_Y, SOURCE_BADGE_W, CARD_BADGE_H, names[i],
              preferences_writable && chosen == i);
    text(42, y + CARD_HELP_Y, DIM, 24, preferences_writable ? help[chosen] : "Cannot read the saved choice.");
}

/* #358: a game with no text box of its own (the first time in it) is offered the box the game worker finds around
 * its dialogue (docs/first-box.md). */
static void draw_suggest_card(int y)
{
    text(42, y + CARD_TITLE_Y, WHITE, 28, "Suggest a text box");
    badge(812, y + CARD_BADGE_Y, 108, CARD_BADGE_H, suggest_on < 0 ? "--" : suggest_on ? "ON" : "OFF", suggest_on > 0);
    text(42, y + CARD_HELP_Y, DIM, 24, suggest_on < 0 ? "Cannot read region.ini."
                                                      : "ON: a game without a box is offered the one Rubi finds.");
}

static void draw_card(int i, int y)
{
    switch (i) {
    case PREF_SIZE: draw_size_card(y); break;
    case PREF_READ: draw_read_card(y); break;
    case PREF_BOOST: draw_boost_card(y); break;
    case PREF_SELECTION: draw_selection_card(y); break;
    case PREF_HIGHLIGHT: draw_highlight_card(y); break;
    case PREF_SOURCE: draw_source_card(y); break;
    case PREF_TRUST: draw_trust_card(y); break;
    case PREF_GATE: draw_gate_card(y); break;
    case PREF_VERTICAL: draw_vertical_card(y); break;
    case PREF_SUGGEST: draw_suggest_card(y); break;
    default: draw_results_card(y); break;
    }
}

/* Where the view is in the column: a thin bar right of the cards. */
static void draw_scrollbar(void)
{
    int room = PREF_VIEW_END - PREF_VIEW_Y, total = pref_offset(PREFERENCES - 1) + pref_height[PREFERENCES - 1];
    int from = pref_offset(pref_top), shown = total - from < room ? total - from : room;
    if (total <= room) return;
    vita2d_draw_rectangle(PREF_SCROLL_X, PREF_VIEW_Y, PREF_SCROLL_W, room, CARD);
    vita2d_draw_rectangle(PREF_SCROLL_X, PREF_VIEW_Y + room * from / total, PREF_SCROLL_W, room * shown / total, DIM);
}

static void draw_preferences(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    header();
    pref_show(preference_selected); /* the selected card all in view */
    for (int i = pref_top; i < PREFERENCES && pref_y(i) < PREF_VIEW_END; i++) {
        int y = pref_y(i), end = y + pref_height[i] < PREF_VIEW_END ? y + pref_height[i] : PREF_VIEW_END;
        int whole = end == y + pref_height[i];
        if (!whole) { /* cut at the bottom of the room: its card as well as its text */
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(24, y, 936, end);
        }
        focus_card(y, pref_height[i], preference_selected == i);
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(28, y + 2, 932, whole ? end - 2 : end);
        draw_card(i, y);
        vita2d_disable_clipping();
    }
    draw_scrollbar();
    footer_at(PREFERENCE_FOOTER_Y, PREFERENCE_FOOTER_H, PREFERENCE_FOOTER_Y + 22, 1,
              preference_status, preference_error, preference_hints);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* A touch at x, y in the cards' room. A card not all in view (cut at the
 * bottom) is only selected, which scrolls it into view; on a card in view, a
 * touch acts as its controls do. */
static void touch_preference(int x, int y)
{
    int i = pref_card_at(y), dy;
    if (i < 0) return; /* a gap between two cards */
    dy = y - pref_y(i);
    preference_selected = i;
    if (!pref_whole(i)) return;
    switch (i) {
    case PREF_SIZE:
        /* Touch a size to choose it; the row's title and the status line only focus. */
        if (dy >= OCR_SIZE_DY && dy < OCR_SIZE_DY + OCR_SIZE_H)
            for (int t = 1; t <= (int)VJO_VOCR_TIERS; t++) {
                int left = OCR_SIZE_X + (t - 1) * OCR_SIZE_PITCH;
                if (x >= left && x < left + OCR_SIZE_W) set_tier(t);
            }
        break;
    case PREF_READ:
        /* Touch a choice to take it; the title row and the gap between them only focus. */
        if (dy >= READ_BUTTON_DY && dy < READ_BUTTON_DY + READ_BUTTON_H)
            for (int k = 0; k < 2; k++) {
                int left = READ_BUTTON_X + k * READ_BUTTON_PITCH;
                if (x >= left && x < left + READ_BUTTON_W)
                    set_reading(k ? VJO_OCR_ON_PRESS : VJO_OCR_AUTO, preferences.ocr_boost_clock);
            }
        break;
    case PREF_HIGHLIGHT: {
        int on_badges = dy >= CARD_BADGE_Y && dy < CARD_BADGE_Y + CARD_BADGE_H;
        if (on_badges && x >= 532 && x < 708)
            set_preference(0);
        else if (on_badges && x >= 724 && x < 920)
            set_preference(1);
        else toggle_preference();
        break;
    }
    case PREF_SOURCE:
        /* Touch a choice to take it; the rest of the card only focuses. */
        if (dy >= CARD_BADGE_Y && dy < CARD_BADGE_Y + CARD_BADGE_H && x >= SOURCE_BADGE_X &&
            (x - SOURCE_BADGE_X) % SOURCE_BADGE_PITCH < SOURCE_BADGE_W && (x - SOURCE_BADGE_X) / SOURCE_BADGE_PITCH < 3)
            set_text_source((x - SOURCE_BADGE_X) / SOURCE_BADGE_PITCH);
        break;
    case PREF_VERTICAL:
        /* As Text source: touch a choice to take it; the rest of the card only focuses. */
        if (dy >= CARD_BADGE_Y && dy < CARD_BADGE_Y + CARD_BADGE_H && x >= SOURCE_BADGE_X &&
            (x - SOURCE_BADGE_X) % SOURCE_BADGE_PITCH < SOURCE_BADGE_W && (x - SOURCE_BADGE_X) / SOURCE_BADGE_PITCH < 3)
            set_vertical((x - SOURCE_BADGE_X) / SOURCE_BADGE_PITCH);
        break;
    case PREF_RESULTS:
        /* Touch a number to choose it; the rest of the card only focuses. */
        if (dy >= CARD_BADGE_Y && dy < CARD_BADGE_Y + CARD_BADGE_H && x >= RESULT_BADGE_X &&
            (x - RESULT_BADGE_X) % RESULT_BADGE_PITCH < RESULT_BADGE_W &&
            (x - RESULT_BADGE_X) / RESULT_BADGE_PITCH < VJO_LOCAL_MAX_DICTS)
            set_dictionary_results((x - RESULT_BADGE_X) / RESULT_BADGE_PITCH + 1);
        break;
    default: /* the ON/OFF cards: Speed boost, Japanese characters only, (API 43) the two OCR skips, (#358) Suggest */
        toggle_preference();
    }
}

/* The room kept at the right of the Games page's hints for "512/512". */
#define COUNTER_W 140

static void game_nav_button(int x, int width, const char *label, int active)
{
    vita2d_draw_rectangle(x, GAME_NAV_Y, width, GAME_NAV_H, CARD);
    centered_text(x, GAME_NAV_Y + 24, width, active ? WHITE : DIM, 20, label);
}

static void draw(void)
{
    const char *filters[] = {"All games", "Enabled", "Disabled"};
    prepare_window();
    vita2d_start_drawing();
    vita2d_clear_screen();
    header();
    vita2d_draw_rectangle(24, DEFAULT_Y, 536, DEFAULT_H, CARD);
    text(42, DEFAULT_Y + 35, WHITE, 24, "Default:");
    badge(174, DEFAULT_Y + 8, 108, 38, !writable ? "--" : policy.default_enabled ? "ON" : "OFF",
          writable && policy.default_enabled);
    text(394, DEFAULT_Y + 35, DIM, 24, "Triangle");
    vita2d_draw_rectangle(572, DEFAULT_Y, 364, DEFAULT_H, CARD);
    text(590, DEFAULT_Y + 35, WHITE, 24, filters[filter]);
    text(824, DEFAULT_Y + 35, DIM, 24, "Square");
    for (int r = 0; r < ROWS && offset + r < count; r++) {
        int index = offset + r, y = GAMES_Y + r * GAME_PITCH;
        const VjoInstalledGame *g = &library[visible[index]];
        int on = row_on(g), hook;
        vita2d_texture *icon = vjo_icon_cache_get(&icon_cache, g->id);
        focus_card(y, GAME_H, index == selected);
        if (icon) vita2d_draw_texture_scale(icon, 38, y + 4,
            32.0f / vita2d_texture_get_width(icon), 32.0f / vita2d_texture_get_height(icon));
        else {
            vita2d_draw_rectangle(38, y + 4, 32, 32, SELECTED);
            centered_text(38, y + 25, 32, ACCENT, 18, "JP");
        }
        /* Titles and IDs have separate columns, including on two-line titles. */
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(90, y + 2, 748, y + GAME_H - 2);
        draw_rows(90, y + 21, WHITE, 22, 24, g->title, title_rows(visible[index]));
        vita2d_disable_clipping();
        badge(788, y + 5, 128, 30, !writable ? "--" : on ? "ON" : "OFF", writable && on);
        /* PSP games: the tag; API 31: a game with a native text hook entry (Rubi's own, or Luna's table) */
        if (psp_row(g))
            text(260, y + 36, ACCENT, 16, "PSP");
        else if ((hook = vjo_hook_title_listed(g->id)) != VJO_HOOK_TITLE_NONE)
            text(260, y + 36, ACCENT, 16, hook == VJO_HOOK_TITLE_RUBI ? "Hook" : "Luna hook");
        text(90, y + 36, DIM, 16, g->id);
        /* #333: a line finder of the game's own (or a PSP game's shared row's), and how to reach its page */
        int lf = used_line_fix(g);
        if (lf >= 0) {
            char tag[24];
            snprintf(tag, sizeof(tag), "Line finder: %s", finder_labels[lf]);
            text(LINE_FIX_TAG_X, y + 36, ACCENT, 16, tag);
        }
        if (index == selected) text(HOLD_HINT_X, y + 36, DIM, 16, hold_hint);
    }
    if (!count) wrapped_text(42, GAMES_Y + 42, 870, WHITE, 28, 34, 2,
                             game_query[0] ? "No matching games. Change or clear the search below."
                                           : "No games in this filter. Press Square to change it.");
    vita2d_draw_rectangle(24, GAME_NAV_Y, 420, GAME_NAV_H, CARD);
    wrapped_text(36, GAME_NAV_Y + 24, 396, WHITE, 20, 24, 1,
                 game_query[0] ? game_query : "L+R: search by title or ID");
    if (game_query[0]) game_nav_button(452, 92, "Clear", 1);
    game_nav_button(556, 112, "L: Prev", offset > 0);
    char pages[32];
    snprintf(pages, sizeof(pages), "Page %d/%d", count ? offset / ROWS + 1 : 0, (count + ROWS - 1) / ROWS);
    centered_text(676, GAME_NAV_Y + 24, 124, DIM, 18, pages);
    game_nav_button(808, 128, "R: Next", offset + ROWS < count);
    if (count) {
        /* The counter changes on every move; drawn apart, the hints' layout is kept. */
        char counter[16];
        status_box(440, 64, 467, 2, status, status_error);
        wrapped_text(24, 535, 912 - COUNTER_W, DIM, 24, 26, 1, game_hints);
        snprintf(counter, sizeof(counter), "%d/%d", selected + 1, count);
        text(936 - text_width(24, counter), 535, DIM, 24, counter);
    } else footer(status, status_error, games_empty_hints);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* #333: a game's own page, in the Games tab: the game's card (its title, ID and tag), then its two settings as the
 * OCR & lookup page's cards: its ON/OFF, and its line finder, whose six choices sit in a row below the card's title
 * (as Read text's two), its help line under them. */
#define GP_CARD_Y 112
#define GP_CARD_H 74
#define GP_ROW_Y 194                        /* the ON/OFF card's top */
#define GP_LF_Y (GP_ROW_Y + CARD_H + PREF_GAP) /* the line finder card's top */
#define GP_LF_H 112
#define LF_BADGE_X 42
#define LF_BADGE_DY 42
#define LF_BADGE_W 138
#define LF_BADGE_PITCH 146
#define LF_HELP_DY 104

static void draw_game_page(void)
{
    const VjoInstalledGame *g = &library[game_view];
    int on = row_on(g), chosen = choice_of(own_line_fix(g)), hook;
    char help[96];
    vita2d_start_drawing();
    vita2d_clear_screen();
    header();
    vita2d_draw_rectangle(24, GP_CARD_Y, 912, GP_CARD_H, CARD);
    wrapped_text(42, GP_CARD_Y + 32, 878, WHITE, 26, 28, 1, g->title);
    text(42, GP_CARD_Y + 62, DIM, 20, g->id);
    if (psp_row(g))
        text(220, GP_CARD_Y + 62, ACCENT, 20, "PSP");
    else if ((hook = vjo_hook_title_listed(g->id)) != VJO_HOOK_TITLE_NONE)
        text(220, GP_CARD_Y + 62, ACCENT, 20, hook == VJO_HOOK_TITLE_RUBI ? "Hook" : "Luna hook");
    focus_card(GP_ROW_Y, CARD_H, game_row == 0);
    text(42, GP_ROW_Y + CARD_TITLE_Y, WHITE, 28, "Rubi in this game");
    badge(812, GP_ROW_Y + CARD_BADGE_Y, 108, CARD_BADGE_H, !writable ? "--" : on ? "ON" : "OFF", writable && on);
    text(42, GP_ROW_Y + CARD_HELP_Y, DIM, 24, "OFF: no overlay and no OCR in this game.");
    focus_card(GP_LF_Y, GP_LF_H, game_row == 1);
    text(42, GP_LF_Y + CARD_TITLE_Y, WHITE, 28, "Line finder");
    for (int i = 0; i < LINE_FINDER_CHOICES; i++)
        badge(LF_BADGE_X + i * LF_BADGE_PITCH, GP_LF_Y + LF_BADGE_DY, LF_BADGE_W, CARD_BADGE_H,
              i ? finder_labels[choice_value(i)] : "Default", line_fix_writable && chosen == i);
    if (line_fix_writable) line_fix_help(g, help, sizeof(help));
    else snprintf(help, sizeof(help), "ocr_line_fix.ini cannot be read: repair or delete it.");
    text(42, GP_LF_Y + LF_HELP_DY, line_fix_writable ? DIM : OFF, 24, help);
    footer(status, status_error, game_page_hints);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* A touch at x, y on the game page: a card is selected; its ON/OFF card toggles, as the OCR page's do, and a line
 * finder choice is taken. */
static void touch_game_page(int x, int y)
{
    if (x < 24 || x >= 936) return;
    if (y >= GP_ROW_Y && y < GP_ROW_Y + CARD_H) {
        game_row = 0;
        toggle_game(game_view);
    } else if (y >= GP_LF_Y && y < GP_LF_Y + GP_LF_H) {
        int dy = y - GP_LF_Y, i = (x - LF_BADGE_X) / LF_BADGE_PITCH;
        game_row = 1;
        if (dy >= LF_BADGE_DY && dy < LF_BADGE_DY + CARD_BADGE_H && x >= LF_BADGE_X &&
            (x - LF_BADGE_X) % LF_BADGE_PITCH < LF_BADGE_W && i < LINE_FINDER_CHOICES)
            set_line_fix(choice_value(i));
    }
}

/* The native keyboard owns input until it closes: no list toggles, tab changes or app exit.
 * The IME's buffers are bounded stack memory in this app; icons stay unloaded during entry. */
static void draw_search(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    header();
    text(42, 148, WHITE, 24, "Search games by title or title ID");
    text(42, 184, DIM, 22, "Leave the search empty to show all games.");
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
    sceDisplayWaitVblankStart();
}

static void search_games(VjoStickScroll *stick, uint32_t *previous, int *touched, uint64_t *repeat_at)
{
    int rc = vjo_search_dialog(game_query, draw_search);
    if (rc > 0) {
        selected = offset = 0;
        rebuild();
    } else if (rc < 0) {
        status_error = 1;
        snprintf(status, sizeof(status), "Could not open search (0x%08X). Your search is unchanged.", (unsigned)rc);
        vjo_log("games: search failed 0x%08X", (unsigned)rc);
    }
    /* Consume the keyboard's closing button/touch, including Japanese enter/cancel assignments. */
    *previous = UINT32_MAX; /* first sample after the dialog cannot be a fresh press */
    *touched = 1;
    *repeat_at = sceKernelGetProcessTimeWide() + 350000;
    vjo_stick_latch(stick);
}

/* A page shown (Select, or its tab): the pages with a module of their own read again what they show. */
static void show_page(int p)
{
    if (page == PAGE_LOOKUP && p != PAGE_LOOKUP) vjo_lookup_page_leave(); /* its dictionaries' files closed */
    if (p == PAGE_ANKI) vjo_anki_page_show(&scratch);
    if (p == PAGE_LOOKUP) vjo_lookup_page_show(&scratch);
    if (p == PAGE_STATS) vjo_stats_page_show(&scratch);
    if (p == PAGE_INSTALL) vjo_install_page_show();
}

/* Cancel is the page's own, not the app's exit: the Anki page's while a job runs or its Cards page is open (#315:
 * Cancel stops the job or goes back to the Anki page), the Look up page's from an earlier line (back to the list). */
static int page_holds_cancel(void)
{
    return (page == PAGE_ANKI && vjo_anki_page_holds_cancel()) || (page == PAGE_LOOKUP && vjo_lookup_page_holds_cancel());
}

/* ---- what the app ran with, in log.txt (log_file = on, the default) ---- */

/* settings/sysapp.c: newlib's heap, one block of this size taken before main. */
extern int _newlib_heap_size_user;

/* The budget sceAppMgrGetBudgetInfo reports (it answers a system application
 * only) and its main memory now: s holds the line's words, "" when it failed. */
static int budget_words(char *s, size_t cap, int *rc)
{
    SceAppMgrBudgetInfo budget;
    memset(&budget, 0, sizeof(budget));
    budget.size = sizeof(budget);
    *rc = sceAppMgrGetBudgetInfo(&budget);
    s[0] = '\0';
    if (*rc < 0) return 0;
    snprintf(s, cap, "budget mode %d: main memory %u of %u B free, phycont %u of %u B, CDRAM %u of %u B",
             budget.app_mode, budget.free_user_rw, budget.total_user_rw_mem, budget.free_phycont_mem,
             budget.total_phycont_mem, budget.free_cdram_mem, budget.total_cdram_mem);
    return 1;
}

/* One line at start: the budget the system gives the app and its heap
 * (settings/sysapp.c and vita/CMakeLists.txt set both; a console run's log
 * shows what it got). */
static void log_start(void)
{
    char words[160];
    int rc;
    if (budget_words(words, sizeof(words), &rc))
        vjo_log("start: " VJO_APP_VERSION "; %s; heap %d B", words, _newlib_heap_size_user);
    else
        vjo_log("start: " VJO_APP_VERSION "; no system-application budget (sceAppMgrGetBudgetInfo 0x%08X); heap %d B",
                (unsigned)rc, _newlib_heap_size_user);
}

/* The frame interval, once, over the first FRAME_LOG frames, and the main
 * memory then free: as a system application the app likely draws at 30 Hz,
 * two vblanks a frame (vita2d_end_drawing waits for one there and the loop
 * for another; an inference from libvita2d's source), which a console run's
 * log shows. The stick goes by the clock either way (core/stick_scroll.h). */
#define FRAME_LOG 60
#if VJO_DEBUG /* the normal build times no frame (docs/release-variants.md) */
static void log_frame_rate(uint64_t now)
{
    static uint64_t first, last;
    static unsigned shortest = 0xFFFFFFFFu, longest;
    static int intervals = -1;
    if (intervals >= FRAME_LOG) return;
    if (intervals >= 0) {
        unsigned d = now > last ? (unsigned)(now - last) : 0;
        if (d < shortest) shortest = d;
        if (d > longest) longest = d;
    } else {
        first = now;
    }
    last = now;
    if (++intervals < FRAME_LOG) return;
    char words[160];
    int rc;
    unsigned total = now > first ? (unsigned)(now - first) : 0;
    budget_words(words, sizeof(words), &rc);
    vjo_log("frames: %d in %u us, %u us a frame (shortest %u, longest %u)%s%s", FRAME_LOG, total, total / FRAME_LOG,
            shortest, longest, words[0] ? "; " : "", words);
}
#endif

int main(void)
{
    read_buttons();
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    int load = vjo_games_file_load(&policy, &scratch);
    writable = load >= 0;
    saved = policy;
    /* #333: each game's own line finder; nothing is written over a file that cannot be read */
    int line_fix_load = vjo_line_fix_file_load(&line_fix, &scratch);
    line_fix_writable = line_fix_load >= 0;
    line_fix_saved = line_fix;
    load_preferences();
    vjo_log_enable(preferences.log_file);
    log_start();
    if (line_fix_load || line_fix.count)
        vjo_log("games: ocr_line_fix.ini %s: %d game%s with a line finder of their own",
                line_fix_load < 0 ? "unreadable, not changed" : line_fix_load ? "recovered from its backup" : "read",
                line_fix.count, line_fix.count == 1 ? "" : "s");
    /* The Install page (docs/one-vpk-install.md): the app opens on it when the overlay is not installed,
     * is another build's, is damaged or was interrupted, or cannot be installed yet. */
    int open_install = vjo_install_page_start();
    vjo_app_banner(app_banner, sizeof(app_banner), vjo_install_page_has_payload()); /* at start, and after a run */
    int install_runs = vjo_install_page_runs();
    status_error = load < 0;
    snprintf(status, sizeof(status), "%s", load < 0 ? "Cannot read settings. Repair games.ini first." :
        load > 0 ? "Recovered saved choices from backup. Changes save automatically." :
        "Choices save automatically. Turn OFF games that need L+R.");
    if (vita2d_init() < 0) { sceKernelExitProcess(1); return 1; }
    font = vita2d_load_default_pvf();
    if (!font) { sceKernelExitProcess(1); return 1; } /* no vita2d_fini: see the end of main */
    measure_font = vita2d_load_default_pvf(); /* the font text is measured with (NULL: the drawing font measures) */
    const char *em_samples[] = {"あ", "漢", "Ｗ"};
    int em_width = 0;
    for (int i = 0; i < 3; i++) {
        int width = vita2d_pvf_text_width(measured_for(em_samples[i]), 1.0f, em_samples[i]);
        if (width > em_width) em_width = width;
    }
    if (em_width > 0) font_em = (float)em_width;
    tab_layout(); /* by the labels' widths: the tab bar's drawing and its touches */
    vita2d_set_clear_color(BG);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); /* the left stick scrolls, as in VitaShell */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    scan();
    if (open_install) page = PAGE_INSTALL;
    uint32_t previous = 0;
    uint64_t repeat_at = 0;
    int touched = 0;
    int search_held = 0;
    VjoStickScroll stick_state;
    memset(&stick_state, 0, sizeof(stick_state));
    for (;;) {
        SceCtrlData pad;
        font_tick(); /* a font a switch replaced (its atlas full), freed now the GPU is done with it */
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
            pad.ly = VJO_STICK_CENTER; /* a failed read: no buttons and the stick at rest, not pushed up */
        uint32_t pressed = pad.buttons & ~previous;
        uint64_t now = sceKernelGetProcessTimeWide();
#if VJO_DEBUG
        log_frame_rate(now);
#endif
        /* The left stick moves the selection as Up/Down would (core/stick_scroll.h),
         * stopping at a list's first and last row, by the clock: held, as many
         * rows a second at 30 Hz as at 60. Counted every frame; a page change
         * latches it, so a stick still pushed then moves nothing until it is
         * let go, as a held button is no new press. */
        int stick = vjo_stick_scroll(&stick_state, pad.ly, now);
        uint32_t direction = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LTRIGGER |
                                            SCE_CTRL_RTRIGGER | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        /* An install runs: the app neither leaves nor changes page (install_page.h). */
        int installing = vjo_install_page_busy();
        uint32_t triggers = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
        if (page == PAGE_GAMES && game_view < 0 && (pad.buttons & triggers) == triggers) {
            direction = 0; /* L+R opens search once, never a page jump or a repeated dialog. */
            if (!search_held) {
                search_held = 1;
                held_game = touched_game = -1; /* a held Enter or touch is the dialog's now */
                search_games(&stick_state, &previous, &touched, &repeat_at);
                continue;
            }
        } else search_held = 0;
        if ((pressed & buttons.cancel) && page == PAGE_GAMES && game_view >= 0) { /* #333: the game page goes back */
            close_game_page();
            pressed &= ~buttons.cancel;
        }
        if ((pressed & buttons.cancel) && !page_holds_cancel() && !installing) break;
        if ((pressed & SCE_CTRL_SELECT) && !installing) {
            int next = (page + 1) % PAGES;
            repeat_at = now + 350000;
            /* The new page takes no move from a stick still pushed, this frame or until it is let go. */
            vjo_stick_latch(&stick_state);
            stick = 0;
            held_game = touched_game = -1;
            if (game_view >= 0) close_game_page(); /* #333: the Games tab opens on its list again */
            show_page(next); /* (it reads the page being left: Look up closes its dictionaries) */
            page = next;
        }
        if (page == PAGE_ANKI || page == PAGE_LOOKUP || page == PAGE_STATS || page == PAGE_INSTALL) {
            /* anki_page.c, lookup_page.c, stats_page.c and install_page.c read their own buttons and touches */
        } else if (page == PAGE_OCR) {
            if (pressed & SCE_CTRL_UP) preference_selected = (preference_selected + PREFERENCES - 1) % PREFERENCES;
            if (pressed & SCE_CTRL_DOWN) preference_selected = (preference_selected + 1) % PREFERENCES;
            /* The d-pad first; the stick stops at the first and last card. */
            if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
                preference_selected = vjo_stick_step(preference_selected, PREFERENCES, stick);
            if (pressed & buttons.enter) toggle_preference();
            if (pressed & SCE_CTRL_LEFT) set_preference(0);
            if (pressed & SCE_CTRL_RIGHT) set_preference(1);
            if (pressed & SCE_CTRL_START) load_preferences();
        } else if (game_view >= 0) { /* #333: a game's own page */
            if (pressed & SCE_CTRL_UP) game_row = (game_row + GAME_SETTINGS - 1) % GAME_SETTINGS;
            if (pressed & SCE_CTRL_DOWN) game_row = (game_row + 1) % GAME_SETTINGS;
            if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))) game_row = vjo_stick_step(game_row, GAME_SETTINGS, stick);
            if (pressed & buttons.enter) game_page_enter();
            if (pressed & SCE_CTRL_LEFT) game_page_step(0);
            if (pressed & SCE_CTRL_RIGHT) game_page_step(1);
        } else {
            /* #333: Enter toggles the selected game as it is let go; held GAME_HOLD_US, it opens the game's page.
             * A selection moved meanwhile does neither. */
            if ((pressed & buttons.enter) && count) {
                held_game = visible[selected];
                held_since = now;
            }
            if (held_game >= 0 && (!(pad.buttons & buttons.enter) || now - held_since >= GAME_HOLD_US)) {
                int same = count && visible[selected] == held_game;
                if (same && (pad.buttons & buttons.enter)) open_game_page(held_game);
                else if (same) toggle();
                held_game = -1;
            }
            if (pressed & SCE_CTRL_TRIANGLE) toggle_default();
            if (pressed & SCE_CTRL_SQUARE) change_filter();
            if (pressed & SCE_CTRL_START) scan();
        }
        if (page == PAGE_GAMES && game_view < 0 && direction && ((pressed & direction) || now >= repeat_at)) {
            /* A key just pressed acts at once; one held repeats. */
            list_key(pressed & direction ? pressed & direction : direction);
            repeat_at = now + ((pressed & direction) ? 350000 : 90000);
        } else if (page == PAGE_GAMES && game_view < 0 && stick) {
            move(stick); /* the d-pad first (VitaShell takes Up first, from either) */
        }
        previous = pad.buttons;
        SceTouchData touch;
        memset(&touch, 0, sizeof(touch));
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int touch_x = -1, touch_y = -1;
        if (touch.reportNum && !touched) {
            int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
            int tab = installing ? -1 : tab_at(x, y);
            if (tab >= 0) {
                if (tab != page) { /* as Select */
                    show_page(tab);
                    vjo_stick_latch(&stick_state);
                    stick = 0;
                    held_game = -1;
                }
                if (game_view >= 0) close_game_page(); /* #333: a tab leaves a game's page; the Games tab, for its list */
                page = tab;
            }
            else if (page == PAGE_ANKI || page == PAGE_LOOKUP || page == PAGE_STATS || page == PAGE_INSTALL) {
                touch_x = x;
                touch_y = y;
            }
            else if (page == PAGE_OCR && x >= 24 && x < 936 && y >= PREF_VIEW_Y && y < PREF_VIEW_END)
                touch_preference(x, y); /* the OCR & lookup page's cards */
            else if (page != PAGE_GAMES) { /* no game actions on the other pages */ }
            else if (game_view >= 0) touch_game_page(x, y); /* #333 */
            else if (y >= GAME_NAV_Y && y < GAME_NAV_Y + GAME_NAV_H) {
                if (x >= 24 && x < 444) {
                    search_games(&stick_state, &previous, &touched, &repeat_at);
                    search_held = 1;
                    continue;
                } else if (x >= 452 && x < 544 && game_query[0]) {
                    game_query[0] = 0;
                    selected = offset = 0;
                    rebuild();
                } else if (x >= 556 && x < 668) page_jump(-1);
                else if (x >= 808 && x < 936) page_jump(1);
            }
            else if (x >= 24 && x < 560 && y >= DEFAULT_Y && y < DEFAULT_Y + DEFAULT_H) toggle_default();
            else if (x >= 572 && x < 936 && y >= DEFAULT_Y && y < DEFAULT_Y + DEFAULT_H) change_filter();
            else if (x >= 24 && x < 936 && y >= GAMES_Y && y < GAMES_Y + ROWS * GAME_PITCH) {
                int row = (y - GAMES_Y) / GAME_PITCH;
                if ((y - GAMES_Y) % GAME_PITCH < GAME_H && offset + row < count) {
                    selected = offset + row;
                    touched_game = visible[selected]; /* #333: toggled as it is let go, below */
                    touched_since = now;
                }
            }
        }
        /* #333: a touch on a game's row toggles that game as it is let go; held GAME_HOLD_US, it opens its page. */
        if (touched_game >= 0 && (!touch.reportNum || now - touched_since >= GAME_HOLD_US)) {
            if (page == PAGE_GAMES && game_view < 0) {
                if (touch.reportNum) open_game_page(touched_game);
                else toggle_game(touched_game);
            }
            touched_game = -1;
        }
        touched = touch.reportNum != 0;
        if (vjo_install_page_runs() != install_runs) { /* an install changed the card: the banner says what is there now */
            install_runs = vjo_install_page_runs();
            vjo_app_banner(app_banner, sizeof(app_banner), vjo_install_page_has_payload());
        }
        if (page == PAGE_INSTALL) {
            vjo_install_page_stick(stick);
            vita2d_start_drawing();
            vita2d_clear_screen();
            vjo_install_page_frame(&install_ui, pressed, touch_x, touch_y);
            vita2d_end_drawing();
            vita2d_swap_buffers();
        } else if (page == PAGE_ANKI) {
            vjo_anki_page_stick(stick);
            if (vjo_anki_page_frame(&anki_ui, pressed, touch_x, touch_y, &scratch)) {
                /* the Cards page's keyboard ran (#315): its closing button and touch are no new press */
                previous = UINT32_MAX;
                touched = 1;
                repeat_at = sceKernelGetProcessTimeWide() + 350000;
                vjo_stick_latch(&stick_state);
            }
        } else if (page == PAGE_STATS) {
            vjo_stats_page_stick(stick);
            vjo_stats_page_frame(&stats_ui, pressed, touch_x, touch_y, &scratch);
        } else if (page == PAGE_LOOKUP) {
            VjoLookupInput in = {pressed, pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry, now, touch_x, touch_y, stick};
            if (vjo_lookup_page_frame(&lookup_ui, &in)) {
                /* the keyboard ran: its closing button and touch are no new press, as after the games' search */
                previous = UINT32_MAX;
                touched = 1;
                repeat_at = sceKernelGetProcessTimeWide() + 350000;
                vjo_stick_latch(&stick_state);
                continue;
            }
        }
        else if (page == PAGE_OCR) draw_preferences();
        else if (game_view >= 0) draw_game_page(); /* #333 */
        else draw();
        sceDisplayWaitVblankStart();
    }
    vjo_anki_page_quit();
    vjo_stats_page_quit();
    vjo_lookup_page_quit();
    vjo_install_page_quit();
    vita2d_wait_rendering_done();
    clear_icons();
    font_tick();
    if (measure_font)
        vita2d_free_pvf(measure_font);
    vita2d_free_pvf(font);
    /* No vita2d_fini. In a system application libvita2d draws into the shared
     * framebuffer's two buffers and never sets its third display buffer, but
     * vita2d_fini clears all three: a 2 MiB memset at address 0 (libvita2d
     * a8f15ab, vita2d.c:259-260 and 758-765). Ending the process frees what
     * libvita2d holds, as closing the app from its LiveArea page does. */
    sceKernelExitProcess(0);
    return 0;
}
