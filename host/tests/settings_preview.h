/* Optional host-only screenshot recorder. Page bodies run their actual C draw/input code over synthetic files.
 * Shared card/badge/footer helpers use main.c's geometry and text_layout.c; the renderer supplies the common
 * header from main.c's own capture. Nothing in this header is built into a Vita module. */
#ifndef VJO_TEST_SETTINGS_PREVIEW_H
#define VJO_TEST_SETTINGS_PREVIEW_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../settings/page_ui.h"
#include "../../settings/text_layout.h"
#include "../../settings/themes.h"
#include "../../settings/chrome.h"

typedef struct { int x, y, w, h, clip, top, bottom, left, right; unsigned colour; } PreviewRect;
typedef struct { int x, y, clip, top, bottom, left, right; unsigned colour; float pixels; char text[512]; } PreviewText;
static struct {
    int active, rects, texts, clip, left, top, right, bottom;
    const char *page;
    PreviewRect rect[512];
    PreviewText text[160];
} vp;

static inline int vp_available(void) { const char *p = getenv("VJO_THEME_PREVIEW_DIR"); return p && *p; }
static inline void vp_begin(const char *page)
{
    memset(&vp, 0, sizeof(vp));
    vp.active = vp_available();
    vp.page = page; /* NULL: the full frame, with main.c's header already recorded */
}
static inline void vp_clip(int x0, int y0, int x1, int y1)
{
    if (!vp.active) return;
    vp.clip = x1 > x0;
    vp.left = x0; vp.top = y0; vp.right = x1; vp.bottom = y1;
}
static inline void vp_rect(int x, int y, int w, int h, unsigned colour)
{
    if (!vp.active) return;
    TEST_ASSERT(vp.rects < 512);
    vp.rect[vp.rects++] = (PreviewRect){x, y, w, h, vp.clip, vp.top, vp.bottom, vp.left, vp.right, colour};
}
static inline void vp_text(int x, int y, unsigned colour, float pixels, const char *s)
{
    if (!vp.active || vp.texts >= 160) return;
    PreviewText *t = &vp.text[vp.texts++];
    *t = (PreviewText){x, y, vp.clip, vp.top, vp.bottom, vp.left, vp.right, colour, pixels, ""};
    snprintf(t->text, sizeof(t->text), "%s", s);
}
static inline int vp_measure(float pixels, const char *s)
{
    int units = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 128) units += 10;
        else if ((*p & 0xC0) != 0x80) units += 18;
    return (int)(units * pixels / 18.0f + 0.5f);
}
static inline void vp_wrapped(int x, int y, int w, unsigned colour, float px, int line_height, int lines, const char *s)
{
    VjoTextLayout rows;
    char line[512];
    if (!vp.active) return;
    vjo_text_layout(s, w, px, lines, vp_measure, &rows);
    for (int r = 0; r < rows.rows; r++) {
        vjo_text_layout_row(s, &rows, r, line);
        vp_text(x, y + r * line_height, colour, px, line);
    }
}
/* Small symbolic colours let the same recorder serve the existing unit harnesses' PageUi tables. */
static inline unsigned vp_colour(unsigned c)
{
    const VjoAppTheme *t = vjo_app_theme(VJO_THEME_MINT);
    const unsigned roles[] = {t->bg, t->text, t->dim, t->accent, t->off, t->card, t->selected};
    return c < sizeof(roles) / sizeof(roles[0]) ? roles[c] : c;
}
static inline void vp_card(int y, int h, int focus)
{ vjo_chrome_panel(vp_rect, 24, y, 912, h, vjo_app_theme(VJO_THEME_MINT), focus, 0); }
static inline void vp_badge(int x, int y, int w, int h, const char *label, int active)
{
    vjo_chrome_button(vp_rect, x, y, w, h, vjo_app_theme(VJO_THEME_MINT), active);
    if ((!strcmp(label, "ON") || !strcmp(label, "OFF")) && (w == 108 || w == 128))
        vjo_chrome_check(vp_rect, x + 9, y + (h - 18) / 2, vp_colour(active ? 0 : 1), active);
    vp_text(x + (w - vp_measure(26, label)) / 2, y + h / 2 + 9, active ? 0 : 1, 26, label);
}
static inline void vp_hints(const char *s) { vp_wrapped(128, 535, 808, 2, 20, 26, 1, s); }
static inline void vp_footer(int y, int h, int text_y, int lines, const char *message, int error, const char *controls)
{
    vp_rect(24, y, 912, h, 5);
    vp_clip(24, y + 2, 936, y + h);
    vp_wrapped(40, text_y, 880, error ? 4 : 1, 24, 28, lines, message);
    vp_clip(0, 0, 0, 0);
    vp_hints(controls);
}
static inline void vp_header(void) {} /* main.c's captured header is composed by the renderer */
static inline VjoPageUi vp_ui(const VjoButtons *buttons)
{
    VjoPageUi ui = {.text = vp_text, .text_width = vp_measure, .wrapped = vp_wrapped, .rect = vp_rect,
        .card = vp_card, .badge = vp_badge, .footer = vp_footer, .hints = vp_hints, .header = vp_header,
        .clip = vp_clip, .white = 1, .dim = 2, .accent = 3, .off = 4, .track = 5, .selected = 6, .buttons = buttons};
    return ui;
}
static inline void vp_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fputc('\\', f);
        if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}
static inline void vp_finish(const char *name)
{
    char path[4096];
    if (!vp.active) return;
    TEST_ASSERT(snprintf(path, sizeof(path), "%s/%s.json", getenv("VJO_THEME_PREVIEW_DIR"), name) < (int)sizeof(path));
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    fprintf(f, "{\"clear\":%u,", vjo_app_theme(VJO_THEME_MINT)->bg);
    if (vp.page) { fprintf(f, "\"page\":"); vp_json_string(f, vp.page); fputc(',', f); }
    fprintf(f, "\"rects\":[");
    for (int k = 0; k < vp.rects; k++) {
        const PreviewRect *r = &vp.rect[k];
        fprintf(f, "%s[%d,%d,%d,%d,%u,%d,%d,%d,%d,%d]", k ? "," : "", r->x, r->y, r->w, r->h,
            vp_colour(r->colour), r->clip, r->top, r->bottom, r->left, r->right);
    }
    fprintf(f, "],\"texts\":[");
    for (int k = 0; k < vp.texts; k++) {
        const PreviewText *t = &vp.text[k];
        fprintf(f, "%s[%d,%d,%u,%.6f,%d,%d,%d,%d,%d,", k ? "," : "", t->x, t->y, vp_colour(t->colour),
            t->pixels / 18.0f, t->clip, t->top, t->bottom, t->left, t->right);
        vp_json_string(f, t->text); fputc(']', f);
    }
    TEST_ASSERT(fprintf(f, "]}\n") > 0 && fclose(f) == 0);
}
#endif
