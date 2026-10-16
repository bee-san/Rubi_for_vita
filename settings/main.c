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
#include "letter.h"
#include "preferences.h"
#include "text_layout.h"
#include "../core/local_dict.h"
#include "../core/utf.h"
#include "../vita/games_storage.h"
#include "../vita/buttons_vita.h"
#include "../core/stick_scroll.h"
/* The app never calls vita2d_fini: as a system application it would clear a
 * display buffer libvita2d never set (the end of main). A call fails to build. */
#pragma GCC poison vita2d_fini

#define ROWS 3
#define TAB_Y 52
#define TAB_H 48
#define DEFAULT_Y 112
#define DEFAULT_H 54
#define GAMES_Y 174
#define GAME_H 80
#define GAME_PITCH 88
#define OCR_Y 112
#define OCR_H 146
/* The OCR card's two rows: the two engine buttons (settings/engine.h), then under
 * the local engine its Model size, three buttons; each row's title at the left,
 * the card's status line below them. */
#define OCR_ROW_TITLE_X 42
#define OCR_CHOICE_X 232
#define OCR_CHOICE_Y (OCR_Y + 6)
#define OCR_CHOICE_W 337
#define OCR_CHOICE_H 48
#define OCR_CHOICE_PITCH 349
#define OCR_SIZE_ROW_Y (OCR_Y + 56) /* the second row's top: touches below it focus the size */
#define OCR_SIZE_Y (OCR_Y + 58)
#define OCR_SIZE_W 220
#define OCR_SIZE_H 54
#define OCR_SIZE_PITCH 233
#define OCR_STATUS_Y (OCR_Y + 137)
/* The three compact preference cards share one layout: title and choices on
 * the first row, help on the second. */
#define CARD_H 68
#define CARD_TITLE_Y 30
#define CARD_HELP_Y 60
#define CARD_BADGE_Y 6
#define CARD_BADGE_H 34
#define SELECTION_Y 262
#define SELECTION_H CARD_H
#define HIGHLIGHT_Y 334
#define HIGHLIGHT_H CARD_H
#define RESULTS_Y 406
#define RESULTS_H CARD_H
#define RESULT_BADGE_X 540
#define RESULT_BADGE_W 44
#define RESULT_BADGE_PITCH 48
/* The OCR page's one-line status sits below the fourth card. */
#define PREFERENCE_FOOTER_Y 478
#define PREFERENCE_FOOTER_H 30
/* The rows Up/Down select: the engine, its Model size, then the three cards. */
#define PREFERENCES 5
#define PREF_ENGINE 0
#define PREF_SIZE 1
#define PREF_SELECTION 2
#define PREF_HIGHLIGHT 3
#define PREF_RESULTS 4
#define BG RGBA8(9, 15, 23, 255)
#define CARD RGBA8(21, 31, 44, 255)
#define SELECTED RGBA8(35, 56, 70, 255)
#define WHITE RGBA8(238, 244, 255, 255)
#define DIM RGBA8(193, 207, 222, 255)
#define ACCENT RGBA8(91, 220, 207, 255)
#define OFF RGBA8(244, 160, 167, 255)
static VjoInstalledGame library[VJO_GAMES_MAX];
static VjoGames policy, saved;
/* Two bounded copies of a Shell-readable config.ini, in the app process. */
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;
static vita2d_pvf *font;
/* Calibrated from full-width glyph advances, not PVF's nominal line height.
 * This is an em target; firmware glyph bearings/bitmap bounds can differ. */
static float font_em = 18.0f;
/* Icons are kept by title ID for the window: the visible rows, a page below
 * and a page above. A frame loads at most one (settings/icons.c); a game
 * whose icon cannot be loaded shows the placeholder until the next scan. */
#define ICON_WINDOW (3 * ROWS)
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
static int scroll_direction = 1, writable = 1; /* the way the list last moved: 1 down, -1 up */
static char status[160];
static VjoConfig preferences;
static int page, preference_selected, preferences_writable;
static VjoEngineCard engine; /* the OCR engine card: the local engine's name, its model files */
static char preference_status[160];
static int status_error, preference_error;

static float text_scale(float pixels) { return pixels / font_em; }

static void text(int x, int y, unsigned color, float pixels, const char *s)
{ vita2d_pvf_draw_text(font, x, y, color, text_scale(pixels), s); }

static int text_width(float pixels, const char *s)
{ return vita2d_pvf_text_width(font, text_scale(pixels), s); }

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

/* The page tabs: Games, OCR & lookup, Anki. */
#define PAGES 3
#define TAB_X(i) (24 + (i) * 304)
#define TAB_W 296
/* When this app and the overlay plugins do not match (app_check.h), a red
 * band in place of the title row says so on every page, in two rows. */
#define BANNER_Y 4
#define BANNER_H 46
#define BANNER_TEXT_Y 24
#define BANNER_LINE_H 22
static char app_banner[192];

static void header(void)
{
    static const char *const tabs[PAGES] = {"Games", "OCR & lookup", "Anki"};
    vita2d_draw_rectangle(0, 0, 960, 4, ACCENT);
    if (app_banner[0]) {
        vita2d_draw_rectangle(0, BANNER_Y, 960, BANNER_H, OFF);
        wrapped_text(24, BANNER_TEXT_Y, 912, BG, 20, BANNER_LINE_H, 2, app_banner);
    } else {
        wrapped_text(24, 35, 690, WHITE, 32, 34, 1, "Overlay");
        text(744, 34, DIM, 24, "Select: page");
    }
    for (int i = 0; i < PAGES; i++) {
        vita2d_draw_rectangle(TAB_X(i), TAB_Y, TAB_W, TAB_H, page == i ? ACCENT : CARD);
        centered_text(TAB_X(i), TAB_Y + 33, TAB_W, page == i ? BG : WHITE, 28, tabs[i]);
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

static void read_buttons(void)
{
    buttons = vjo_buttons_read();
    snprintf(preference_hints, sizeof(preference_hints), "Up/Down: setting   Left/Right or %s: choose   %s: exit",
             buttons.enter_glyph, buttons.cancel_glyph);
    snprintf(game_hints, sizeof(game_hints), "%s: toggle   L/R: page   Left/Right: letter   %s: exit",
             buttons.enter_glyph, buttons.cancel_glyph);
    snprintf(games_empty_hints, sizeof(games_empty_hints), "Square: filter   Start: refresh   Select: page   %s: exit",
             buttons.cancel_glyph);
}

static const VjoAnkiUi anki_ui = {text, wrapped_text, focus_card, badge, footer_at, header,
                                  WHITE, DIM, ACCENT, OFF, &buttons};

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

/* Each game's title in two rows of 620 pixels, laid out on first use. */
static const VjoTextLayout *title_rows(int game)
{
    if (!title_ready[game]) {
        vjo_text_layout(library[game].title, 620, 26, 2, text_width, &title_layout[game]);
        title_ready[game] = 1;
    }
    return &title_layout[game];
}

/* The window's library entries, most wanted first: the visible rows, then
 * the page the list last moved towards, then the other, nearest row first. */
static int icon_window(int out[ICON_WINDOW])
{
    int n = 0;
    for (int r = 0; r < ROWS && offset + r < count; r++) out[n++] = visible[offset + r];
    for (int side = 0; side < 2; side++) {
        int down = (side == 0) == (scroll_direction > 0);
        for (int r = 0; r < ROWS; r++) {
            int i = down ? offset + ROWS + r : offset - 1 - r;
            if (i >= 0 && i < count) out[n++] = visible[i];
        }
    }
    return n;
}

/* A Games frame's work before it draws: at most one icon load, for the first
 * window entry without one. When the cache is full, the least recently used
 * icon outside the window is freed first, after the GPU has finished with
 * it. With no icon to load, one title of the window is laid out instead. */
static void prepare_window(void)
{
    int window[ICON_WINDOW], n = icon_window(window);
    const char *keep[ICON_WINDOW];
    for (int i = 0; i < n; i++) keep[i] = library[window[i]].id;
    for (int i = 0; i < n; i++) {
        const VjoInstalledGame *g = &library[window[i]];
        if (icon_failed[window[i]] || vjo_icon_cache_get(&icon_cache, g->id)) continue;
        if (vjo_icon_cache_count(&icon_cache) == VJO_ICON_SLOTS) {
            void *old = vjo_icon_cache_evict(&icon_cache, keep, n);
            if (!old) return; /* not reached: the window fits the cache */
            vita2d_wait_rendering_done();
            vita2d_free_texture(old);
        }
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

static void rebuild(void)
{
    count = 0;
    for (int i = 0; i < total; i++) {
        int on = vjo_games_enabled(&policy, library[i].id);
        if (!filter || (filter == 1 && on) || (filter == 2 && !on)) visible[count++] = i;
    }
    if (selected >= count) selected = count ? count - 1 : 0;
    if (selected < offset) offset = selected;
    if (selected >= offset + ROWS) offset = selected - ROWS + 1;
    if (!count) offset = 0;
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
        snprintf(status, sizeof(status), "Saved. Resume or restart your game to apply.");
    }
    rebuild();
}

static void toggle(void)
{
    if (!count) return;
    const char *id = library[visible[selected]].id;
    if (vjo_games_set(&policy, id, !vjo_games_enabled(&policy, id)) < 0) {
        status_error = 1;
        snprintf(status, sizeof(status), "Settings are full (512 title overrides). No change saved.");
        return;
    }
    save();
}

static void toggle_default(void)
{
    policy.default_enabled = !policy.default_enabled;
    save();
}

static void change_filter(void)
{
    filter = (filter + 1) % 3;
    selected = offset = 0;
    scroll_direction = 1;
    rebuild();
}

/* The catalog is read here only: at start and on Start. Icons and title
 * layouts belong to the old list and go with it. */
static void scan(void)
{
    clear_icons();
    memset(icon_failed, 0, sizeof(icon_failed));
    memset(title_ready, 0, sizeof(title_ready));
    total = vjo_catalog_scan(library, VJO_GAMES_MAX, &truncated);
    rebuild();
    if (truncated) { status_error = 0; snprintf(status, sizeof(status), "Showing the first 512 installed titles."); }
}

static void move(int delta)
{
    if (!count) return;
    selected += delta;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    if (selected < offset) offset = selected;
    if (selected >= offset + ROWS) offset = selected - ROWS + 1;
    scroll_direction = delta < 0 ? -1 : 1;
}

/* L/R: the view moves a page and the selection keeps its row. Where the view
 * cannot move further, the selection goes to the first or last game. */
static void page_jump(int direction)
{
    if (!count) return;
    int last = count > ROWS ? count - ROWS : 0, top = offset + direction * ROWS;
    if (top > last) top = last;
    if (top < 0) top = 0;
    scroll_direction = direction;
    if (direction > 0 ? top <= offset : top >= offset) {
        selected = direction > 0 ? count - 1 : 0;
        if (selected < offset) offset = selected;
        if (selected >= offset + ROWS) offset = selected - ROWS + 1;
        return;
    }
    selected += top - offset;
    offset = top;
}

/* The letter of the list's index-th game (settings/letter.c). */
static uint32_t initial_at(int index) { return vjo_title_letter(library[visible[index]].title); }

/* Left/Right: the first game of the previous or next letter (the catalog
 * lists each letter's games together, settings/letter.c), shown at the top.
 * Left inside a letter goes to that letter's first game; Right in the last
 * letter goes to the last game. */
static void letter_jump(int direction)
{
    if (!count) return;
    uint32_t letter = initial_at(selected);
    int i = selected, last = count > ROWS ? count - ROWS : 0;
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
    scroll_direction = direction;
    selected = i;
    offset = selected < last ? selected : last;
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

static void check_models(void)
{
    /* The local engine's model sizes: which tiers' files are there, and the
     * generation of the model that loads. */
    vjo_engine_card(&preferences, &engine);
}

static void load_preferences(void)
{
    int rc = vjo_preferences_load(&preferences, &scratch);
    preferences_writable = rc >= 0;
    preference_error = rc < 0;
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0
        ? "Cannot read config.ini. Choices have not been changed."
        : rc > 0 ? "Backup recovered. Changes save automatically."
                 : "Changes apply when you next open the overlay.");
    check_models();
}

static void change_preference(int backend, int tier, int japanese_only, int highlight_mode, int dictionary_results)
{
    if (!preferences_writable) {
        preference_error = 1;
        snprintf(preference_status, sizeof(preference_status), "Cannot read config.ini. Choices have not been changed.");
        return;
    }
    int rc = vjo_preferences_save(backend, tier, japanese_only, highlight_mode, dictionary_results, &scratch);
    /* Re-read even on failure: final sync can fail after publication. */
    int read_rc = vjo_preferences_load(&preferences, &scratch);
    preferences_writable = read_rc >= 0;
    preference_error = rc < 0 || read_rc < 0;
    check_models();
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0 || read_rc < 0
        ? "Save not confirmed. Check the displayed choices, then retry."
        : "Saved. Resume your game, then reopen the overlay.");
}

/* One preference changed, the others as saved. */
static void change_backend(int backend)
{
    change_preference(backend, preferences.vocr_tier, preferences.japanese_only_selection,
                      preferences.highlight_mode, preferences.local_dictionary_results);
}

/* Model size: enter cycles Small -> Normal -> Large; Left/Right step within it. */
static void set_tier(int tier)
{
    if (tier < 1 || tier > (int)VJO_VOCR_TIERS || tier == preferences.vocr_tier) return;
    change_preference(preferences.ocr_backend, tier, preferences.japanese_only_selection,
                      preferences.highlight_mode, preferences.local_dictionary_results);
}

/* Dictionaries shown: enter cycles 1..8; Left/Right step within it. */
static void set_dictionary_results(int results)
{
    if (results < 1 || results > VJO_LOCAL_MAX_DICTS || results == preferences.local_dictionary_results) return;
    change_preference(preferences.ocr_backend, preferences.vocr_tier, preferences.japanese_only_selection,
                      preferences.highlight_mode, results);
}

static void toggle_preference(void)
{
    if (preference_selected == PREF_RESULTS) {
        set_dictionary_results(preferences.local_dictionary_results % VJO_LOCAL_MAX_DICTS + 1);
        return;
    }
    if (preference_selected == PREF_SIZE) {
        set_tier(preferences.vocr_tier % (int)VJO_VOCR_TIERS + 1);
        return;
    }
    change_preference(preference_selected == PREF_ENGINE
        ? vjo_engine_backend(!vjo_engine_index(preferences.ocr_backend)) /* the other engine */
        : preferences.ocr_backend,
        preferences.vocr_tier,
        preference_selected == PREF_SELECTION ? !preferences.japanese_only_selection : preferences.japanese_only_selection,
        preference_selected == PREF_HIGHLIGHT ? !preferences.highlight_mode : preferences.highlight_mode,
        preferences.local_dictionary_results);
}

/* Left/Right choose the left/right option of the selected card. */
static void set_preference(int right)
{
    if (preference_selected == PREF_RESULTS) {
        set_dictionary_results(preferences.local_dictionary_results + (right ? 1 : -1));
        return;
    }
    if (preference_selected == PREF_SIZE) {
        set_tier(preferences.vocr_tier + (right ? 1 : -1));
        return;
    }
    change_preference(preference_selected == PREF_ENGINE ? vjo_engine_backend(right)
                                                         : preferences.ocr_backend,
        preferences.vocr_tier,
        preference_selected == PREF_SELECTION ? right : preferences.japanese_only_selection,
        preference_selected == PREF_HIGHLIGHT ? (right ? VJO_HIGHLIGHT_CHARACTER : VJO_HIGHLIGHT_WORD)
                                              : preferences.highlight_mode,
        preferences.local_dictionary_results);
}

/* The OCR card: its background and, when one of its two rows is selected, the
 * accent frame with the left bar beside that row only. */
static void focus_ocr_card(int row)
{
    int focus = row == PREF_ENGINE || row == PREF_SIZE;
    int bar_y = row == PREF_SIZE ? OCR_SIZE_ROW_Y : OCR_Y, bar_h = row == PREF_SIZE ? OCR_Y + OCR_H - OCR_SIZE_ROW_Y
                                                                                     : OCR_SIZE_ROW_Y - OCR_Y;
    vita2d_draw_rectangle(24, OCR_Y, 912, OCR_H, focus ? SELECTED : CARD);
    if (focus) {
        vita2d_draw_rectangle(24, bar_y, 4, bar_h, ACCENT);
        vita2d_draw_rectangle(24, OCR_Y, 912, 2, ACCENT);
        vita2d_draw_rectangle(24, OCR_Y + OCR_H - 2, 912, 2, ACCENT);
        vita2d_draw_rectangle(932, OCR_Y, 4, OCR_H, ACCENT);
    }
}

/* A Model size button: the tier's name and, smaller, its estimates. Muted (no
 * fill, the chosen one outlined) while Google Lens is the engine: the choice is
 * the local engine's, kept for when it is used again. */
static void size_button(int tier, int x, int chosen, int muted)
{
    const VjoVocrTierInfo *t = vjo_vocr_tier_info((unsigned)tier);
    char detail[32];
    int active = chosen && !muted;
    vita2d_draw_rectangle(x, OCR_SIZE_Y, OCR_SIZE_W, OCR_SIZE_H, active ? ACCENT : SELECTED);
    if (chosen && muted) {
        vita2d_draw_rectangle(x, OCR_SIZE_Y, OCR_SIZE_W, 2, ACCENT);
        vita2d_draw_rectangle(x, OCR_SIZE_Y + OCR_SIZE_H - 2, OCR_SIZE_W, 2, ACCENT);
        vita2d_draw_rectangle(x, OCR_SIZE_Y, 2, OCR_SIZE_H, ACCENT);
        vita2d_draw_rectangle(x + OCR_SIZE_W - 2, OCR_SIZE_Y, 2, OCR_SIZE_H, ACCENT);
    }
    vjo_engine_tier_detail(tier, detail, sizeof(detail));
    centered_text(x, OCR_SIZE_Y + 24, OCR_SIZE_W, active ? BG : muted ? DIM : WHITE, 24, t ? t->name : "?");
    centered_text(x, OCR_SIZE_Y + 47, OCR_SIZE_W, active ? BG : DIM, 22, detail);
}

static void draw_preferences(void)
{
    int choice = vjo_engine_index(preferences.ocr_backend);
    vita2d_start_drawing();
    vita2d_clear_screen();
    header();
    focus_ocr_card(preference_selected);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, OCR_Y + 2, 932, OCR_Y + OCR_H - 2);
    text(OCR_ROW_TITLE_X, OCR_CHOICE_Y + OCR_CHOICE_H / 2 + 9, WHITE, 28, "OCR engine");
    for (int i = 0; i < VJO_ENGINES; i++)
        badge(OCR_CHOICE_X + i * OCR_CHOICE_PITCH, OCR_CHOICE_Y,
              OCR_CHOICE_W, OCR_CHOICE_H, vjo_engine_name(&engine, i),
              preferences_writable && choice == i);
    /* Under the local engine, its Model size (estimates from the models' metadata). */
    text(OCR_ROW_TITLE_X, OCR_SIZE_Y + 24, preferences_writable && !choice ? WHITE : DIM, 28, "Model size");
    text(OCR_ROW_TITLE_X, OCR_SIZE_Y + 47, DIM, 22, "estimated"); /* the buttons' time and memory */
    for (int t = 1; t <= (int)VJO_VOCR_TIERS; t++)
        size_button(t, OCR_CHOICE_X + (t - 1) * OCR_SIZE_PITCH, preferences_writable && preferences.vocr_tier == t,
                    choice != 0);
    char label[120];
    unsigned color = DIM;
    if (!preferences_writable) {
        snprintf(label, sizeof(label), "Cannot read config.ini. Repair it before changing settings.");
        color = OFF;
    } else if (vjo_engine_status(&preferences, &engine, label, sizeof(label))) {
        color = OFF;
    }
    wrapped_text(42, OCR_STATUS_Y, 870, color, 24, 26, 1, label);
    vita2d_disable_clipping();
    focus_card(SELECTION_Y, SELECTION_H, preference_selected == PREF_SELECTION);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, SELECTION_Y + 2, 932, SELECTION_Y + SELECTION_H - 2);
    text(42, SELECTION_Y + CARD_TITLE_Y, WHITE, 28, "Japanese characters only");
    badge(812, SELECTION_Y + CARD_BADGE_Y, 108, CARD_BADGE_H, !preferences_writable ? "--" :
          preferences.japanese_only_selection ? "ON" : "OFF",
          preferences_writable && preferences.japanese_only_selection);
    text(42, SELECTION_Y + CARD_HELP_Y, DIM, 24, "ON skips Latin, numbers, punctuation and Hangul.");
    vita2d_disable_clipping();
    focus_card(HIGHLIGHT_Y, HIGHLIGHT_H, preference_selected == PREF_HIGHLIGHT);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, HIGHLIGHT_Y + 2, 932, HIGHLIGHT_Y + HIGHLIGHT_H - 2);
    text(42, HIGHLIGHT_Y + CARD_TITLE_Y, WHITE, 28, "Highlight");
    badge(532, HIGHLIGHT_Y + CARD_BADGE_Y, 176, CARD_BADGE_H, "Whole word",
          preferences_writable && preferences.highlight_mode == VJO_HIGHLIGHT_WORD);
    badge(724, HIGHLIGHT_Y + CARD_BADGE_Y, 196, CARD_BADGE_H, "Character",
          preferences_writable && preferences.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    text(42, HIGHLIGHT_Y + CARD_HELP_Y, DIM, 24, "Word skips matched text. Character moves one character.");
    vita2d_disable_clipping();
    focus_card(RESULTS_Y, RESULTS_H, preference_selected == PREF_RESULTS);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, RESULTS_Y + 2, 932, RESULTS_Y + RESULTS_H - 2);
    text(42, RESULTS_Y + CARD_TITLE_Y, WHITE, 28, "Dictionaries shown");
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        char number[4];
        snprintf(number, sizeof(number), "%d", i + 1);
        badge(RESULT_BADGE_X + i * RESULT_BADGE_PITCH, RESULTS_Y + CARD_BADGE_Y, RESULT_BADGE_W, CARD_BADGE_H,
              number, preferences_writable && preferences.local_dictionary_results == i + 1);
    }
    char results_help[120];
    int enabled = vjo_local_names_count(preferences.local_dictionaries);
    if (!preferences_writable)
        snprintf(results_help, sizeof(results_help), "Cannot read the saved choice.");
    else if (!enabled)
        snprintf(results_help, sizeof(results_help), "Set local_dictionaries in config.ini first.");
    else if (preferences.local_dictionary_results == 1)
        snprintf(results_help, sizeof(results_help), "Best match only. %d %s enabled.",
                 enabled, enabled == 1 ? "dictionary" : "dictionaries");
    else
        snprintf(results_help, sizeof(results_help), "Best match, then others with an L/R tap. %d enabled.", enabled);
    text(42, RESULTS_Y + CARD_HELP_Y, DIM, 24, results_help);
    vita2d_disable_clipping();
    footer_at(PREFERENCE_FOOTER_Y, PREFERENCE_FOOTER_H, PREFERENCE_FOOTER_Y + 22, 1,
              preference_status, preference_error, preference_hints);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* The room kept at the right of the Games page's hints for "512/512". */
#define COUNTER_W 140

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
        int on = vjo_games_enabled(&policy, g->id);
        vita2d_texture *icon = vjo_icon_cache_get(&icon_cache, g->id);
        focus_card(y, GAME_H, index == selected);
        if (icon) vita2d_draw_texture_scale(icon, 38, y + 8,
            64.0f / vita2d_texture_get_width(icon), 64.0f / vita2d_texture_get_height(icon));
        else {
            vita2d_draw_rectangle(38, y + 8, 64, 64, SELECTED);
            centered_text(38, y + 47, 64, ACCENT, 26, "JP");
        }
        /* Titles and IDs have separate columns, including on two-line titles. */
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(120, y + 4, 748, y + GAME_H - 4);
        draw_rows(120, y + 30, WHITE, 26, 32, g->title, title_rows(visible[index]));
        vita2d_disable_clipping();
        badge(788, y + 7, 128, 38, !writable ? "--" : on ? "ON" : "OFF", writable && on);
        text(920 - text_width(24, g->id), y + 70, DIM, 24, g->id);
    }
    if (!count) wrapped_text(42, GAMES_Y + 42, 870, WHITE, 28, 34, 2,
                             "No games in this filter. Press Square to change it.");
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
        vjo_log("start: %s; heap %d B", words, _newlib_heap_size_user);
    else
        vjo_log("start: no system-application budget (sceAppMgrGetBudgetInfo 0x%08X); heap %d B", (unsigned)rc,
                _newlib_heap_size_user);
}

/* The frame interval, once, over the first FRAME_LOG frames, and the main
 * memory then free: as a system application the app likely draws at 30 Hz,
 * two vblanks a frame (vita2d_end_drawing waits for one there and the loop
 * for another; an inference from libvita2d's source), which a console run's
 * log shows. The stick goes by the clock either way (core/stick_scroll.h). */
#define FRAME_LOG 60
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

int main(void)
{
    read_buttons();
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    int load = vjo_games_file_load(&policy, &scratch);
    writable = load >= 0;
    saved = policy;
    load_preferences();
    vjo_log_enable(preferences.log_file);
    log_start();
    vjo_app_banner(app_banner, sizeof(app_banner)); /* read once, at start */
    status_error = load < 0;
    snprintf(status, sizeof(status), "%s", load < 0 ? "Cannot read settings. Repair games.ini first." :
        load > 0 ? "Recovered saved choices from backup. Changes save automatically." :
        "Choices save automatically. Turn OFF games that need L+R.");
    if (vita2d_init() < 0) { sceKernelExitProcess(1); return 1; }
    font = vita2d_load_default_pvf();
    if (!font) { sceKernelExitProcess(1); return 1; } /* no vita2d_fini: see the end of main */
    const char *em_samples[] = {"あ", "漢", "Ｗ"};
    int em_width = 0;
    for (int i = 0; i < 3; i++) {
        int width = vita2d_pvf_text_width(font, 1.0f, em_samples[i]);
        if (width > em_width) em_width = width;
    }
    if (em_width > 0) font_em = (float)em_width;
    vita2d_set_clear_color(BG);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); /* the left stick scrolls, as in VitaShell */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    scan();
    uint32_t previous = 0;
    uint64_t repeat_at = 0;
    int touched = 0;
    VjoStickScroll stick_state;
    memset(&stick_state, 0, sizeof(stick_state));
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
            pad.ly = VJO_STICK_CENTER; /* a failed read: no buttons and the stick at rest, not pushed up */
        uint32_t pressed = pad.buttons & ~previous;
        uint64_t now = sceKernelGetProcessTimeWide();
        log_frame_rate(now);
        /* The left stick moves the selection as Up/Down would (core/stick_scroll.h),
         * stopping at a list's first and last row, by the clock: held, as many
         * rows a second at 30 Hz as at 60. Counted every frame; a page change
         * latches it, so a stick still pushed then moves nothing until it is
         * let go, as a held button is no new press. */
        int stick = vjo_stick_scroll(&stick_state, pad.ly, now);
        uint32_t direction = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LTRIGGER |
                                            SCE_CTRL_RTRIGGER | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if ((pressed & buttons.cancel) && !(page == 2 && vjo_anki_page_busy())) break;
        if (pressed & SCE_CTRL_SELECT) {
            page = (page + 1) % PAGES;
            repeat_at = now + 350000;
            /* The new page takes no move from a stick still pushed, this frame or until it is let go. */
            vjo_stick_latch(&stick_state);
            stick = 0;
            if (page == 2) vjo_anki_page_show(&scratch);
        }
        if (page == 2) {
            /* anki_page.c reads its own buttons and touches */
        } else if (page) {
            if (pressed & SCE_CTRL_UP) preference_selected = (preference_selected + PREFERENCES - 1) % PREFERENCES;
            if (pressed & SCE_CTRL_DOWN) preference_selected = (preference_selected + 1) % PREFERENCES;
            /* The d-pad first; the stick stops at the first and last card. */
            if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
                preference_selected = vjo_stick_step(preference_selected, PREFERENCES, stick);
            if (pressed & buttons.enter) toggle_preference();
            if (pressed & SCE_CTRL_LEFT) set_preference(0);
            if (pressed & SCE_CTRL_RIGHT) set_preference(1);
            if (pressed & SCE_CTRL_START) load_preferences();
        } else {
            if (pressed & buttons.enter) toggle();
            if (pressed & SCE_CTRL_TRIANGLE) toggle_default();
            if (pressed & SCE_CTRL_SQUARE) change_filter();
            if (pressed & SCE_CTRL_START) scan();
        }
        if (!page && direction && ((pressed & direction) || now >= repeat_at)) {
            /* A key just pressed acts at once; one held repeats. */
            list_key(pressed & direction ? pressed & direction : direction);
            repeat_at = now + ((pressed & direction) ? 350000 : 90000);
        } else if (!page && stick) {
            move(stick); /* the d-pad first (VitaShell takes Up first, from either) */
        }
        previous = pad.buttons;
        SceTouchData touch;
        memset(&touch, 0, sizeof(touch));
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int touch_x = -1, touch_y = -1;
        if (touch.reportNum && !touched) {
            int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
            int tab = y >= TAB_Y && y < TAB_Y + TAB_H && x >= 24 && x < 936 && (x - 24) % 304 < TAB_W
                      ? (x - 24) / 304 : -1;
            if (tab >= 0) {
                if (tab == 2 && page != 2) vjo_anki_page_show(&scratch);
                if (tab != page) { /* as Select */
                    vjo_stick_latch(&stick_state);
                    stick = 0;
                }
                page = tab;
            }
            else if (page == 2) { touch_x = x; touch_y = y; }
            else if (page && x >= 24 && x < 936 && y >= OCR_Y && y < OCR_SIZE_ROW_Y) {
                preference_selected = PREF_ENGINE;
                if (y >= OCR_CHOICE_Y && y < OCR_CHOICE_Y + OCR_CHOICE_H) {
                    for (int i = 0; i < VJO_ENGINES; i++) {
                        int left = OCR_CHOICE_X + i * OCR_CHOICE_PITCH;
                        if (x >= left && x < left + OCR_CHOICE_W) change_backend(vjo_engine_backend(i));
                    }
                }
            }
            else if (page && x >= 24 && x < 936 && y >= OCR_SIZE_ROW_Y && y < OCR_Y + OCR_H) {
                /* Touch a size to choose it; the row's title and the status line only focus. */
                preference_selected = PREF_SIZE;
                if (y >= OCR_SIZE_Y && y < OCR_SIZE_Y + OCR_SIZE_H) {
                    for (int t = 1; t <= (int)VJO_VOCR_TIERS; t++) {
                        int left = OCR_CHOICE_X + (t - 1) * OCR_SIZE_PITCH;
                        if (x >= left && x < left + OCR_SIZE_W) set_tier(t);
                    }
                }
            }
            else if (page && x >= 24 && x < 936 && y >= SELECTION_Y && y < SELECTION_Y + SELECTION_H) {
                preference_selected = PREF_SELECTION; toggle_preference();
            }
            else if (page && x >= 24 && x < 936 && y >= HIGHLIGHT_Y && y < HIGHLIGHT_Y + HIGHLIGHT_H) {
                preference_selected = PREF_HIGHLIGHT;
                int on_badges = y >= HIGHLIGHT_Y + CARD_BADGE_Y && y < HIGHLIGHT_Y + CARD_BADGE_Y + CARD_BADGE_H;
                if (on_badges && x >= 532 && x < 708)
                    set_preference(0);
                else if (on_badges && x >= 724 && x < 920)
                    set_preference(1);
                else toggle_preference();
            }
            else if (page && x >= 24 && x < 936 && y >= RESULTS_Y && y < RESULTS_Y + RESULTS_H) {
                /* Touch a number to choose it; the rest of the card only focuses. */
                preference_selected = PREF_RESULTS;
                if (y >= RESULTS_Y + CARD_BADGE_Y && y < RESULTS_Y + CARD_BADGE_Y + CARD_BADGE_H &&
                    x >= RESULT_BADGE_X && (x - RESULT_BADGE_X) % RESULT_BADGE_PITCH < RESULT_BADGE_W &&
                    (x - RESULT_BADGE_X) / RESULT_BADGE_PITCH < VJO_LOCAL_MAX_DICTS)
                    set_dictionary_results((x - RESULT_BADGE_X) / RESULT_BADGE_PITCH + 1);
            }
            else if (page) { /* no game actions on the OCR page */ }
            else if (x >= 24 && x < 560 && y >= DEFAULT_Y && y < DEFAULT_Y + DEFAULT_H) toggle_default();
            else if (x >= 572 && x < 936 && y >= DEFAULT_Y && y < DEFAULT_Y + DEFAULT_H) change_filter();
            else if (x >= 24 && x < 936 && y >= GAMES_Y && y < GAMES_Y + ROWS * GAME_PITCH) {
                int row = (y - GAMES_Y) / GAME_PITCH;
                if ((y - GAMES_Y) % GAME_PITCH < GAME_H && offset + row < count) {
                    selected = offset + row;
                    toggle();
                }
            }
        }
        touched = touch.reportNum != 0;
        if (page == 2) {
            vjo_anki_page_stick(stick);
            vjo_anki_page_frame(&anki_ui, pressed, touch_x, touch_y, &scratch);
        }
        else if (page) draw_preferences(); else draw();
        sceDisplayWaitVblankStart();
    }
    vjo_anki_page_quit();
    vita2d_wait_rendering_done();
    clear_icons();
    vita2d_free_pvf(font);
    /* No vita2d_fini. In a system application libvita2d draws into the shared
     * framebuffer's two buffers and never sets its third display buffer, but
     * vita2d_fini clears all three: a 2 MiB memset at address 0 (libvita2d
     * a8f15ab, vita2d.c:259-260 and 758-765). Ending the process frees what
     * libvita2d holds, as closing the app from its LiveArea page does. */
    sceKernelExitProcess(0);
    return 0;
}
