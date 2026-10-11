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
#include "../include/vjo_vocr_sizes.h"
#include "../vita/games_storage.h"

#define ROWS 5
#define BG RGBA8(13, 18, 31, 255)
#define CARD RGBA8(24, 33, 52, 255)
#define SELECTED RGBA8(32, 51, 77, 255)
#define WHITE RGBA8(238, 244, 255, 255)
#define DIM RGBA8(149, 169, 194, 255)
#define ACCENT RGBA8(91, 220, 207, 255)
#define OFF RGBA8(244, 160, 167, 255)
static VjoInstalledGame library[VJO_GAMES_MAX];
static VjoGames policy, saved;
/* Two bounded copies of a Shell-readable config.ini, in the app process. */
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;
static vita2d_pvf *font;
static vita2d_texture *icons[ROWS];
static int visible[VJO_GAMES_MAX], total, count, selected, offset, filter, truncated;
static int icons_offset = -1, writable = 1;
static char status[160];
static VjoConfig preferences;
static int page, preference_selected, preferences_writable;
static int model_found[2];
static char preference_status[160];

static void text(int x, int y, unsigned color, float size, const char *s)
{ vita2d_pvf_draw_text(font, x, y, color, size, s); }

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
        policy = saved;
        snprintf(status, sizeof(status), "Cannot read settings. Repair games.ini first.");
    } else if (vjo_games_file_save(&policy, &scratch) < 0) {
        policy = saved;
        /* A final sync can fail after publication; show the actual disk state. */
        if (vjo_games_file_load(&policy, &scratch) >= 0) saved = policy;
        snprintf(status, sizeof(status), "Save could not be confirmed. Check your choices and retry.");
    } else {
        saved = policy;
        snprintf(status, sizeof(status), "Saved. Close this app, then restart or resume your game.");
    }
    rebuild();
}

static void toggle(void)
{
    if (!count) return;
    const char *id = library[visible[selected]].id;
    if (vjo_games_set(&policy, id, !vjo_games_enabled(&policy, id)) < 0) {
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
    if (truncated) snprintf(status, sizeof(status), "Showing the first 512 installed titles.");
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
    const char *names[] = {preferences.vocr_model, VJO_MEIKI_MODEL_NAME};
    for (int i = 0; i < 2; i++) {
        char path[256];
        int n = snprintf(path, sizeof(path), "%s/%s", preferences.ocr_model_dir, names[i]);
        model_found[i] = 0;
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
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0
        ? "Cannot read config.ini. Choices have not been changed."
        : rc > 0 ? "Recovered config backup. Changes save automatically."
                 : "Changes apply the next time the overlay opens.");
    check_models();
}

static void change_preference(int backend, int japanese_only)
{
    if (!preferences_writable) {
        snprintf(preference_status, sizeof(preference_status), "Cannot read config.ini. Choices have not been changed.");
        return;
    }
    int rc = vjo_preferences_save(backend, japanese_only, &scratch);
    /* Re-read even on failure: final sync can fail after publication. */
    int read_rc = vjo_preferences_load(&preferences, &scratch);
    preferences_writable = read_rc >= 0;
    check_models();
    snprintf(preference_status, sizeof(preference_status), "%s", rc < 0 || read_rc < 0
        ? "Save could not be confirmed. Check the displayed choices and retry."
        : "Saved. Close this app, resume your game, then reopen the overlay.");
}

static void toggle_preference(void)
{
    change_preference(preference_selected == 0
        ? (preferences.ocr_backend == VJO_OCR_MEIKI ? VJO_OCR_VOCR : VJO_OCR_MEIKI)
        : preferences.ocr_backend,
        preference_selected == 1 ? !preferences.japanese_only_selection : preferences.japanese_only_selection);
}

static void draw_preferences(void)
{
    const char *backend = !preferences_writable ? "Unavailable" : preferences.ocr_backend == VJO_OCR_MEIKI ? "Meiki" :
                          preferences.ocr_backend == VJO_OCR_VOCR ? "VOCR" : "Other (config.ini)";
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, 960, 4, ACCENT);
    text(30, 46, WHITE, 1.45f, "JP Overlay Settings");
    text(30, 74, DIM, 0.85f, "OCR and character lookup");
    text(747, 45, ACCENT, 0.85f, "[Select] Games");
    for (int i = 0; i < 2; i++) {
        int y = 106 + i * 112;
        vita2d_draw_rectangle(24, y, 912, 96, preference_selected == i ? SELECTED : CARD);
        if (preference_selected == i) vita2d_draw_rectangle(24, y, 4, 96, ACCENT);
    }
    text(42, 140, WHITE, 1.05f, "OCR model");
    text(760, 140, ACCENT, 1.0f, backend);
    text(42, 168, DIM, 0.83f, "Left: VOCR (default)    Right: Meiki    X / Touch: Switch");
    text(42, 192, DIM, 0.72f, "VOCR: dialogue region. Meiki: one horizontal line, manual capture only.");
    text(42, 252, WHITE, 1.05f, "Japanese-only character selection");
    text(810, 252, preferences.japanese_only_selection ? ACCENT : OFF, 1.0f,
         !preferences_writable ? "--" : preferences.japanese_only_selection ? "ON" : "OFF");
    text(42, 282, DIM, 0.83f, "Left / Right skips Latin text, numbers, punctuation and Hangul.");
    text(42, 304, DIM, 0.72f, "Turn OFF to select every character. OCR text stays visible.");
    char label[120];
    snprintf(label, sizeof(label), "VOCR model file: %s    Meiki model file: %s",
             model_found[0] ? "found" : "missing", model_found[1] ? "found" : "missing");
    text(42, 358, model_found[0] && model_found[1] ? DIM : OFF, 0.82f, label);
    text(42, 386, DIM, 0.75f, "Meiki also needs its engine installed for the game. File checks do not run OCR.");
    text(30, 483, preferences_writable ? DIM : OFF, 0.78f, preference_status);
    text(30, 519, WHITE, 0.78f, "Up / Down: Navigate    X / Touch: Change    Select: Games    Circle: Exit");
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
    vita2d_draw_rectangle(0, 0, 960, 4, ACCENT);
    text(30, 46, WHITE, 1.45f, "JP Overlay Settings");
    char label[80];
    snprintf(label, sizeof(label), "%d installed titles  /  %s", total, filters[filter]);
    text(30, 74, DIM, 0.85f, label);
    text(757, 45, ACCENT, 0.85f, "[Square] Filter");
    text(610, 74, ACCENT, 0.82f, "[Select] OCR and lookup");
    vita2d_draw_rectangle(24, 91, 912, 61, CARD);
    text(42, 117, WHITE, 1.0f, policy.default_enabled ? "All games by default" : "Selected games only");
    text(42, 140, DIM, 0.75f, "Per-game choices override this default. [Triangle] Change");
    text(790, 125, ACCENT, 0.9f, policy.default_enabled ? "DEFAULT ON" : "DEFAULT OFF");
    for (int r = 0; r < ROWS && offset + r < count; r++) {
        int index = offset + r, y = 162 + r * 60;
        const VjoInstalledGame *g = &library[visible[index]];
        int on = vjo_games_enabled(&policy, g->id);
        vita2d_draw_rectangle(24, y, 912, 56, index == selected ? SELECTED : CARD);
        if (index == selected) vita2d_draw_rectangle(24, y, 4, 56, ACCENT);
        if (icons[r]) vita2d_draw_texture_scale(icons[r], 38, y + 6,
            44.0f / vita2d_texture_get_width(icons[r]), 44.0f / vita2d_texture_get_height(icons[r]));
        else {
            vita2d_draw_rectangle(38, y + 6, 44, 44, SELECTED);
            text(49, y + 33, ACCENT, 0.9f, "JP");
        }
        /* Scissor long/Japanese names without cutting their UTF-8 bytes. */
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(98, y, 767, y + 56);
        text(98, y + 24, WHITE, 0.98f, g->title);
        text(98, y + 45, DIM, 0.72f, g->id);
        vita2d_disable_clipping();
        text(783, y + 33, on ? ACCENT : OFF, 0.9f, on ? "ON" : "OFF");
        vita2d_draw_rectangle(841, y + 18, 68, 22, on ? ACCENT : DIM);
        vita2d_draw_fill_circle(on ? 895 : 855, y + 29, 8, BG);
    }
    if (!count) text(42, 211, DIM, 1.0f, "No games in this filter. Press Square to show all.");
    text(30, 483, writable ? DIM : OFF, 0.78f, status);
    text(30, 519, WHITE, 0.78f, "D-pad / L-R: Navigate    X / Touch: Toggle    Start: Refresh    Circle: Exit");
    if (count) {
        snprintf(label, sizeof(label), "%d/%d", selected + 1, count);
        text(851, 483, ACCENT, 0.8f, label);
    }
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
    snprintf(status, sizeof(status), "%s", load < 0 ? "Cannot read settings. Repair games.ini first." :
        load > 0 ? "Recovered saved choices from backup. Changes save automatically." :
        "Turn OFF Uncharted to keep L+R free. Changes save automatically.");
    if (vita2d_init() < 0) { sceKernelExitProcess(1); return 1; }
    font = vita2d_load_default_pvf();
    if (!font) { vita2d_fini(); sceKernelExitProcess(1); return 1; }
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
            if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) preference_selected = !preference_selected;
            if (pressed & SCE_CTRL_CROSS) toggle_preference();
            if (pressed & SCE_CTRL_LEFT) change_preference(preference_selected == 0 ? VJO_OCR_VOCR : preferences.ocr_backend,
                preference_selected == 1 ? 0 : preferences.japanese_only_selection);
            if (pressed & SCE_CTRL_RIGHT) change_preference(preference_selected == 0 ? VJO_OCR_MEIKI : preferences.ocr_backend,
                preference_selected == 1 ? 1 : preferences.japanese_only_selection);
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
            if ((page && x >= 740 && y >= 28 && y < 60) ||
                (!page && x >= 600 && y >= 60 && y < 85)) page = !page;
            else if (page && x >= 24 && x <= 936 && y >= 106 && y < 314) {
                int row = (y - 106) / 112;
                if ((y - 106) % 112 < 96) { preference_selected = row; toggle_preference(); }
            }
            else if (page) { /* no game actions on the OCR page */ }
            else if (x >= 24 && x <= 936 && y >= 91 && y < 152) toggle_default();
            else if (x >= 750 && y < 80) change_filter();
            else if (x >= 24 && x <= 936 && y >= 162 && y < 462) {
                int row = (y - 162) / 60;
                if ((y - 162) % 60 < 56 && offset + row < count) {
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
