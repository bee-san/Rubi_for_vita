/* Standalone LiveArea settings app: no network, no plugin imports. */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>
#include "catalog.h"
#include "preferences.h"
#include "../core/local_dict.h"
#include "../core/utf.h"
#include "../include/vjo_vocr_sizes.h"
#include "../vita/games_storage.h"

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
#define OCR_CHOICE_X 42
#define OCR_CHOICE_Y (OCR_Y + 42)
#define OCR_CHOICE_W 284
#define OCR_CHOICE_H 48
#define OCR_CHOICE_PITCH 296
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
#define PREFERENCES 4
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
static vita2d_texture *icons[ROWS];
static int visible[VJO_GAMES_MAX], total, count, selected, offset, filter, truncated;
static int icons_offset = -1, writable = 1;
static char status[160];
static VjoConfig preferences;
static int page, preference_selected, preferences_writable;
static int model_found[3];
static char preference_status[160];
static int status_error, preference_error;
static const struct {
    int backend;
    const char *name;
    const char *description;
} ocr_choices[] = {
    {VJO_OCR_VOCR, "VOCR", "Local OCR: dialogue region, automatic or manual."},
    {VJO_OCR_VOCR2, "VOCR2", "Experimental local OCR. Trial results vary by game."},
    {VJO_OCR_LENS, "Google Lens", "Wi-Fi required. Sends the selected image to Google."}
};
#define OCR_CHOICES ((int)(sizeof(ocr_choices) / sizeof(ocr_choices[0])))

static int ocr_choice_index(int backend)
{
    for (int i = 0; i < OCR_CHOICES; i++)
        if (ocr_choices[i].backend == backend) return i;
    return -1;
}

static int next_ocr_choice(int direction)
{
    int index = ocr_choice_index(preferences.ocr_backend);
    if (index < 0) return VJO_OCR_VOCR;
    return ocr_choices[(index + direction + OCR_CHOICES) % OCR_CHOICES].backend;
}

static float text_scale(float pixels) { return pixels / font_em; }

static void text(int x, int y, unsigned color, float pixels, const char *s)
{ vita2d_pvf_draw_text(font, x, y, color, text_scale(pixels), s); }

static int text_width(float pixels, const char *s)
{ return vita2d_pvf_text_width(font, text_scale(pixels), s); }

/* A line ends only at a decoded UTF-8 boundary. PVF advances, rather than
 * byte counts, decide how many Japanese/Latin glyphs fit in the card. */
static size_t fit_line(const char *s, float pixels, int width, char out[512])
{
    size_t len = strlen(s), end = 0, space = 0;
    for (size_t i = 0; i < len;) {
        size_t begin = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (cp == '\n' || i > 508) break; /* Reserve the final "..." and NUL. */
        memcpy(out, s, i);
        out[i] = 0;
        if (text_width(pixels, out) > width) break;
        end = i;
        if (cp == ' ') space = begin;
    }
    if (s[end] && s[end] != '\n' && space) end = space;
    memcpy(out, s, end);
    out[end] = 0;
    return end;
}

static void wrapped_text(int x, int y, int width, unsigned color,
                         float pixels, int line_height, int lines, const char *s)
{
    char line[512];
    for (int row = 0; row < lines && *s; row++) {
        while (*s == ' ' || *s == '\r') s++;
        size_t end = fit_line(s, pixels, width, line);
        const char *remaining = s + end;
        while (*remaining == ' ' || *remaining == '\r' || *remaining == '\n') remaining++;
        if (row == lines - 1 && *remaining) {
            end = fit_line(s, pixels, width - text_width(pixels, "..."), line);
            strcat(line, "...");
        }
        text(x, y + row * line_height, color, pixels, line);
        if (!end && *s != '\n') break; /* Even one glyph can exceed a tiny region. */
        s += end;
        if (*s == '\n') s++;
    }
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

static void header(void)
{
    vita2d_draw_rectangle(0, 0, 960, 4, ACCENT);
    wrapped_text(24, 35, 690, WHITE, 32, 34, 1, "Overlay");
    text(744, 34, DIM, 24, "Select: page");
    for (int i = 0; i < 2; i++) {
        int x = i ? 486 : 24, width = i ? 450 : 446;
        vita2d_draw_rectangle(x, TAB_Y, width, TAB_H, page == i ? ACCENT : CARD);
        centered_text(x, TAB_Y + 33, width, page == i ? BG : WHITE, 28,
                      i ? "OCR & lookup" : "Games");
    }
}

/* Status box (first baseline text_y, lines of 28 px) and the control hints. */
static void footer_at(int y, int height, int text_y, int lines, const char *message, int error,
                      const char *controls)
{
    vita2d_draw_rectangle(24, y, 912, height, CARD);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(24, y + 2, 936, y + height);
    wrapped_text(40, text_y, 880, error ? OFF : WHITE, 24, 28, lines, message);
    vita2d_disable_clipping();
    wrapped_text(24, 535, 912, DIM, 24, 26, 1, controls);
}

static void footer(const char *message, int error, const char *controls)
{
    footer_at(440, 64, 467, 2, message, error, controls);
}

static void free_icons(void)
{
    for (int i = 0; i < ROWS; i++) {
        if (icons[i]) vita2d_free_texture(icons[i]);
        icons[i] = NULL;
    }
    icons_offset = -1;
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
    free_icons();
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
    rebuild();
}

static void scan(void)
{
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
    int prev = offset;
    if (selected < offset) offset = selected;
    if (selected >= offset + ROWS) offset = selected - ROWS + 1;
    if (prev != offset) free_icons();
}

static void check_models(void)
{
    /* VOCR, VOCR2, and VOCR's fallback (used when the game has too little
     * memory for vocr_model, or vocr_model is unusable). */
    const char *names[] = {preferences.vocr_model, VJO_VOCR2_MODEL_NAME, preferences.vocr_fallback_model};
    for (int i = 0; i < 3; i++) {
        char path[256];
        model_found[i] = 0;
        if (!names[i][0]) continue;
        int n = snprintf(path, sizeof(path), "%s/%s", preferences.ocr_model_dir, names[i]);
        if (n < 0 || n >= (int)sizeof(path)) continue;
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) continue;
        SceOff bytes = sceIoLseek(fd, 0, SCE_SEEK_END);
        model_found[i] = bytes > 0 && sceIoClose(fd) >= 0;
        if (bytes <= 0) sceIoClose(fd);
    }
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

static void change_preference(int backend, int japanese_only, int highlight_mode, int dictionary_results)
{
    if (!preferences_writable) {
        preference_error = 1;
        snprintf(preference_status, sizeof(preference_status), "Cannot read config.ini. Choices have not been changed.");
        return;
    }
    int rc = vjo_preferences_save(backend, japanese_only, highlight_mode, dictionary_results, &scratch);
    /* Re-read even on failure: final sync can fail after publication. */
    int read_rc = vjo_preferences_load(&preferences, &scratch);
    preferences_writable = read_rc >= 0;
    preference_error = rc < 0 || read_rc < 0;
    check_models();
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0 || read_rc < 0
        ? "Save not confirmed. Check the displayed choices, then retry."
        : "Saved. Resume your game, then reopen the overlay.");
}

/* Dictionaries shown: Cross cycles 1..8; Left/Right step within it. */
static void set_dictionary_results(int results)
{
    if (results < 1 || results > VJO_LOCAL_MAX_DICTS || results == preferences.local_dictionary_results) return;
    change_preference(preferences.ocr_backend, preferences.japanese_only_selection,
                      preferences.highlight_mode, results);
}

static void toggle_preference(void)
{
    if (preference_selected == 3) {
        set_dictionary_results(preferences.local_dictionary_results % VJO_LOCAL_MAX_DICTS + 1);
        return;
    }
    change_preference(preference_selected == 0
        ? next_ocr_choice(1)
        : preferences.ocr_backend,
        preference_selected == 1 ? !preferences.japanese_only_selection : preferences.japanese_only_selection,
        preference_selected == 2 ? !preferences.highlight_mode : preferences.highlight_mode,
        preferences.local_dictionary_results);
}

static void set_preference(int right)
{
    if (preference_selected == 3) {
        set_dictionary_results(preferences.local_dictionary_results + (right ? 1 : -1));
        return;
    }
    change_preference(preference_selected == 0 ? next_ocr_choice(right ? 1 : -1)
                                               : preferences.ocr_backend,
        preference_selected == 1 ? right : preferences.japanese_only_selection,
        preference_selected == 2 ? (right ? VJO_HIGHLIGHT_CHARACTER : VJO_HIGHLIGHT_WORD)
                                : preferences.highlight_mode,
        preferences.local_dictionary_results);
}

static void draw_preferences(void)
{
    int choice = ocr_choice_index(preferences.ocr_backend);
    vita2d_start_drawing();
    vita2d_clear_screen();
    header();
    focus_card(OCR_Y, OCR_H, preference_selected == 0);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, OCR_Y + 2, 932, OCR_Y + OCR_H - 2);
    text(42, OCR_Y + 31, WHITE, 28, "OCR engine");
    for (int i = 0; i < OCR_CHOICES; i++)
        badge(OCR_CHOICE_X + i * OCR_CHOICE_PITCH, OCR_CHOICE_Y,
              OCR_CHOICE_W, OCR_CHOICE_H, ocr_choices[i].name,
              preferences_writable && choice == i);
    text(42, OCR_Y + 111, DIM, 24, !preferences_writable
         ? "Cannot read the saved OCR choice."
         : choice < 0 ? "Custom engine configured. Select an engine above."
                      : ocr_choices[choice].description);
    char label[120];
    unsigned color = DIM;
    if (!preferences_writable) {
        snprintf(label, sizeof(label), "Repair config.ini before changing settings.");
        color = OFF;
    } else if (choice < 0) {
        snprintf(label, sizeof(label), "Left/Right or X chooses VOCR first.");
    } else if (preferences.ocr_backend == VJO_OCR_LENS) {
        snprintf(label, sizeof(label), "No model file needed. Uses your chosen dictionary.");
    } else {
        int model = preferences.ocr_backend == VJO_OCR_VOCR2 ? 1 : 0;
        if (!model && !model_found[0] && model_found[2])
            snprintf(label, sizeof(label), "%s model missing; uses %s. Game OCR worker required.",
                     ocr_choices[choice].name, preferences.vocr_fallback_model);
        else {
            snprintf(label, sizeof(label), "%s model %s. Game OCR worker required.",
                     ocr_choices[choice].name, model_found[model] ? "found" : "missing");
            if (!model_found[model]) color = OFF;
        }
    }
    wrapped_text(42, OCR_Y + 137, 870, color, 24, 26, 1, label);
    vita2d_disable_clipping();
    focus_card(SELECTION_Y, SELECTION_H, preference_selected == 1);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, SELECTION_Y + 2, 932, SELECTION_Y + SELECTION_H - 2);
    text(42, SELECTION_Y + CARD_TITLE_Y, WHITE, 28, "Japanese characters only");
    badge(812, SELECTION_Y + CARD_BADGE_Y, 108, CARD_BADGE_H, !preferences_writable ? "--" :
          preferences.japanese_only_selection ? "ON" : "OFF",
          preferences_writable && preferences.japanese_only_selection);
    text(42, SELECTION_Y + CARD_HELP_Y, DIM, 24, "ON skips Latin, numbers, punctuation and Hangul.");
    vita2d_disable_clipping();
    focus_card(HIGHLIGHT_Y, HIGHLIGHT_H, preference_selected == 2);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(28, HIGHLIGHT_Y + 2, 932, HIGHLIGHT_Y + HIGHLIGHT_H - 2);
    text(42, HIGHLIGHT_Y + CARD_TITLE_Y, WHITE, 28, "Highlight");
    badge(532, HIGHLIGHT_Y + CARD_BADGE_Y, 176, CARD_BADGE_H, "Whole word",
          preferences_writable && preferences.highlight_mode == VJO_HIGHLIGHT_WORD);
    badge(724, HIGHLIGHT_Y + CARD_BADGE_Y, 196, CARD_BADGE_H, "Character",
          preferences_writable && preferences.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    text(42, HIGHLIGHT_Y + CARD_HELP_Y, DIM, 24, "Word skips matched text. Character moves one character.");
    vita2d_disable_clipping();
    focus_card(RESULTS_Y, RESULTS_H, preference_selected == 3);
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
        snprintf(results_help, sizeof(results_help), "Best match, then others with Up/Down. %d enabled.", enabled);
    text(42, RESULTS_Y + CARD_HELP_Y, DIM, 24, results_help);
    vita2d_disable_clipping();
    footer_at(PREFERENCE_FOOTER_Y, PREFERENCE_FOOTER_H, PREFERENCE_FOOTER_Y + 22, 1,
              preference_status, preference_error, "Up/Down: setting   Left/Right or X: choose   O: exit");
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void draw(void)
{
    const char *filters[] = {"All games", "Enabled", "Disabled"};
    if (icons_offset != offset) {
        free_icons();
        for (int r = 0; r < ROWS && offset + r < count; r++) {
            const char *path = library[visible[offset + r]].icon;
            if (path[0]) icons[r] = vita2d_load_PNG_file(path);
        }
        icons_offset = offset;
    }
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
        focus_card(y, GAME_H, index == selected);
        if (icons[r]) vita2d_draw_texture_scale(icons[r], 38, y + 8,
            64.0f / vita2d_texture_get_width(icons[r]), 64.0f / vita2d_texture_get_height(icons[r]));
        else {
            vita2d_draw_rectangle(38, y + 8, 64, 64, SELECTED);
            centered_text(38, y + 47, 64, ACCENT, 26, "JP");
        }
        /* Titles and IDs have separate columns, including on two-line titles. */
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(120, y + 4, 748, y + GAME_H - 4);
        wrapped_text(120, y + 30, 620, WHITE, 26, 32, 2, g->title);
        vita2d_disable_clipping();
        badge(788, y + 7, 128, 38, !writable ? "--" : on ? "ON" : "OFF", writable && on);
        text(920 - text_width(24, g->id), y + 70, DIM, 24, g->id);
    }
    if (!count) wrapped_text(42, GAMES_Y + 42, 870, WHITE, 28, 34, 2,
                             "No games in this filter. Press Square to change it.");
    char controls[120];
    if (count) {
        snprintf(controls, sizeof(controls), "%d/%d   D-pad: move   X: toggle   L/R: page   O: exit", selected + 1, count);
    } else snprintf(controls, sizeof(controls), "Square: filter   Start: refresh   Select: page   O: exit");
    footer(status, status_error, controls);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

int main(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    int load = vjo_games_file_load(&policy, &scratch);
    writable = load >= 0;
    saved = policy;
    load_preferences();
    status_error = load < 0;
    snprintf(status, sizeof(status), "%s", load < 0 ? "Cannot read settings. Repair games.ini first." :
        load > 0 ? "Recovered saved choices from backup. Changes save automatically." :
        "Choices save automatically. Turn OFF games that need L+R.");
    if (vita2d_init() < 0) { sceKernelExitProcess(1); return 1; }
    font = vita2d_load_default_pvf();
    if (!font) { vita2d_fini(); sceKernelExitProcess(1); return 1; }
    const char *em_samples[] = {"あ", "漢", "Ｗ"};
    int em_width = 0;
    for (int i = 0; i < 3; i++) {
        int width = vita2d_pvf_text_width(font, 1.0f, em_samples[i]);
        if (width > em_width) em_width = width;
    }
    if (em_width > 0) font_em = (float)em_width;
    vita2d_set_clear_color(BG);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    scan();
    uint32_t previous = 0;
    uint64_t repeat_at = 0;
    int touched = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~previous;
        uint32_t direction = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
        uint64_t now = sceKernelGetProcessTimeWide();
        if (pressed & SCE_CTRL_CIRCLE) break;
        if (pressed & SCE_CTRL_SELECT) { page = !page; repeat_at = now + 350000; }
        if (page) {
            if (pressed & SCE_CTRL_UP) preference_selected = (preference_selected + PREFERENCES - 1) % PREFERENCES;
            if (pressed & SCE_CTRL_DOWN) preference_selected = (preference_selected + 1) % PREFERENCES;
            if (pressed & SCE_CTRL_CROSS) toggle_preference();
            if (pressed & SCE_CTRL_LEFT) set_preference(0);
            if (pressed & SCE_CTRL_RIGHT) set_preference(1);
            if (pressed & SCE_CTRL_START) load_preferences();
        } else {
            if (pressed & SCE_CTRL_CROSS) toggle();
            if (pressed & SCE_CTRL_TRIANGLE) toggle_default();
            if (pressed & SCE_CTRL_SQUARE) change_filter();
            if (pressed & SCE_CTRL_START) scan();
            if (pressed & SCE_CTRL_LTRIGGER) move(-ROWS);
            if (pressed & SCE_CTRL_RTRIGGER) move(ROWS);
        }
        if (!page && direction && ((pressed & direction) || now >= repeat_at)) {
            move(direction & SCE_CTRL_UP ? -1 : 1);
            repeat_at = now + ((pressed & direction) ? 350000 : 90000);
        }
        previous = pad.buttons;
        SceTouchData touch;
        memset(&touch, 0, sizeof(touch));
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        if (touch.reportNum && !touched) {
            int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
            if (y >= TAB_Y && y < TAB_Y + TAB_H && x >= 24 && x < 470) page = 0;
            else if (y >= TAB_Y && y < TAB_Y + TAB_H && x >= 486 && x < 936) page = 1;
            else if (page && x >= 24 && x < 936 && y >= OCR_Y && y < OCR_Y + OCR_H) {
                preference_selected = 0;
                if (y >= OCR_CHOICE_Y && y < OCR_CHOICE_Y + OCR_CHOICE_H) {
                    for (int i = 0; i < OCR_CHOICES; i++) {
                        int left = OCR_CHOICE_X + i * OCR_CHOICE_PITCH;
                        if (x >= left && x < left + OCR_CHOICE_W)
                            change_preference(ocr_choices[i].backend,
                                              preferences.japanese_only_selection, preferences.highlight_mode,
                                              preferences.local_dictionary_results);
                    }
                }
            }
            else if (page && x >= 24 && x < 936 && y >= SELECTION_Y && y < SELECTION_Y + SELECTION_H) {
                preference_selected = 1; toggle_preference();
            }
            else if (page && x >= 24 && x < 936 && y >= HIGHLIGHT_Y && y < HIGHLIGHT_Y + HIGHLIGHT_H) {
                preference_selected = 2;
                int on_badges = y >= HIGHLIGHT_Y + CARD_BADGE_Y && y < HIGHLIGHT_Y + CARD_BADGE_Y + CARD_BADGE_H;
                if (on_badges && x >= 532 && x < 708)
                    set_preference(0);
                else if (on_badges && x >= 724 && x < 920)
                    set_preference(1);
                else toggle_preference();
            }
            else if (page && x >= 24 && x < 936 && y >= RESULTS_Y && y < RESULTS_Y + RESULTS_H) {
                /* Touch a number to choose it; the rest of the card only focuses. */
                preference_selected = 3;
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
        if (page) draw_preferences(); else draw();
        sceDisplayWaitVblankStart();
    }
    vita2d_wait_rendering_done();
    free_icons();
    vita2d_free_pvf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
