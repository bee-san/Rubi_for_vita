/* Sony Reader-inspired application chrome, drawn with bounded rectangles: no textures, allocations or animation.
 * The host preview uses these same helpers. References: docs/colour-schemes/README.md. */
#ifndef VJO_SETTINGS_CHROME_H
#define VJO_SETTINGS_CHROME_H
#include "themes.h"

typedef void (*VjoChromeRect)(int x, int y, int w, int h, unsigned colour);
static inline unsigned vjo_chrome_mix(unsigned a, unsigned b, unsigned amount)
{
    unsigned out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned x = (a >> shift) & 255, y = (b >> shift) & 255;
        out |= ((x * (256 - amount) + y * amount) >> 8) << shift;
    }
    return out;
}
static inline void vjo_chrome_round(VjoChromeRect draw, int x, int y, int w, int h, unsigned colour)
{
    /* An eight-pixel radius, with short controls using a square bevel rather than invalid geometry. */
    static const unsigned char inset[8] = {5, 3, 2, 1, 1, 0, 0, 0};
    if (w < 16 || h < 16) { draw(x, y, w, h, colour); return; }
    draw(x, y + 8, w, h - 16, colour);
    for (int r = 0; r < 8; r++) {
        draw(x + inset[r], y + r, w - 2 * inset[r], 1, colour);
        draw(x + inset[r], y + h - r - 1, w - 2 * inset[r], 1, colour);
    }
}
static inline void vjo_chrome_background(VjoChromeRect draw, const VjoAppTheme *t)
{
    /* Reader's quiet central light, recreated with 32 bands and 32 alpha-blended edge strips.
     * libvita2d's normal blend mode uses source alpha; the host renderer does the same. */
    int paper = t == vjo_app_theme(VJO_THEME_PAPER);
    for (int band = 0; band < 32; band++) {
        int y = 4 + 540 * band / 32, end = 4 + 540 * (band + 1) / 32;
        int distance = band > 19 ? band - 19 : 19 - band;
        unsigned light = paper ? (unsigned)distance : (unsigned)(28 - distance);
        draw(0, y, 960, end - y, vjo_chrome_mix(t->bg, paper ? t->dim : t->text, light));
    }
    for (int edge = 0; edge < 16; edge++) {
        unsigned alpha = (unsigned)(16 - edge) * 9;
        unsigned shade = (t->bg & 0xFFFFFFu) | (alpha << 24);
        draw(edge * 30, 4, 30, 540, shade);
        draw(930 - edge * 30, 4, 30, 540, shade);
    }
}
static inline void vjo_chrome_panel(VjoChromeRect draw, int x, int y, int w, int h,
                                    const VjoAppTheme *t, int focus, int menu)
{
    unsigned surface = focus ? t->selected : t->card;
    if (focus || !menu) draw(x, y, w, h, surface);
    /* Thin separators replace the stacked boxes; the controller's focus keeps a clear edge. */
    draw(x, y + h - 1, w, 1, vjo_chrome_mix(t->bg, t->dim, 64));
    if (focus) {
        draw(x, y, 4, h, t->accent);
        draw(x + 4, y + h - 2, w - 4, 1, vjo_chrome_mix(surface, t->text, 80));
    }
}
static inline void vjo_chrome_button(VjoChromeRect draw, int x, int y, int w, int h,
                                     const VjoAppTheme *t, int active)
{
    unsigned fill = active ? t->accent : t->selected;
    draw(x, y, w, h, fill);
    for (int band = 0; band < 6; band++) {
        int top = y + 1 + (h / 2 - 1) * band / 6;
        int end = y + 1 + (h / 2 - 1) * (band + 1) / 6;
        draw(x + 1, top, w - 2, end - top,
             vjo_chrome_mix(fill, t->text, (unsigned)(active ? 54 - band * 6 : 70 - band * 8)));
    }
    draw(x + 5, y + h - 2, w - 10, 1, vjo_chrome_mix(fill, t->bg, 100));
}
static inline void vjo_chrome_toolbar(VjoChromeRect draw, int x, int y, int w, int h,
                                      const VjoAppTheme *t, int active)
{
    unsigned fill = active ? t->selected : t->card;
    vjo_chrome_round(draw, x, y, w, h, fill);
    for (int band = 0; band < 8; band++) {
        int top = y + 1 + (h / 2 - 1) * band / 8;
        int end = y + 1 + (h / 2 - 1) * (band + 1) / 8;
        draw(x + 8, top, w - 16, end - top,
             vjo_chrome_mix(fill, t->text, (unsigned)(92 - band * 6)));
    }
    draw(x + 8, y, w - 16, 1, vjo_chrome_mix(fill, t->text, 110));
    draw(x + 8, y + h - 1, w - 16, 1, vjo_chrome_mix(fill, t->text, 40));
    if (active) draw(x + 12, y + h - 4, w - 24, 2, t->accent);
}
static inline void vjo_chrome_back(VjoChromeRect draw, const VjoAppTheme *t)
{
    /* A silver circular corner control, with the Back label alongside it. */
    for (int row = 0; row < 40; row++) {
        int dy = row * 2 + 1 - 40, span = 40;
        while (span * span + dy * dy > 40 * 40) span -= 2;
        unsigned light = row < 20 ? (unsigned)(132 - row * 3) : (unsigned)(48 - (row - 20));
        draw(16 + (40 - span) / 2, 504 + row, span, 1, vjo_chrome_mix(t->selected, t->text, light));
    }
}
static inline void vjo_chrome_category_icon(VjoChromeRect draw, int kind, int x, int y, const VjoAppTheme *t)
{
    unsigned ink = t->dim;
    if (kind == 0) { /* gamepad */
        vjo_chrome_round(draw, x, y + 5, 32, 22, ink);
        draw(x + 5, y + 14, 11, 3, t->bg); draw(x + 9, y + 10, 3, 11, t->bg);
        draw(x + 23, y + 11, 3, 3, t->bg); draw(x + 26, y + 17, 3, 3, t->bg);
    } else if (kind == 1) { /* open book */
        draw(x, y + 3, 14, 24, ink); draw(x + 17, y + 3, 14, 24, ink);
        draw(x + 3, y + 5, 8, 18, t->bg); draw(x + 20, y + 5, 8, 18, t->bg);
        draw(x + 3, y + 9, 7, 2, ink); draw(x + 20, y + 9, 7, 2, ink);
    } else if (kind == 2) { /* cards */
        draw(x + 5, y + 2, 25, 23, ink); draw(x, y + 7, 25, 23, ink);
        draw(x + 2, y + 9, 21, 19, t->bg); draw(x + 5, y + 13, 15, 2, ink);
        draw(x + 5, y + 19, 11, 2, ink);
    } else if (kind == 3) { /* colour swatches */
        draw(x, y + 5, 8, 23, t->accent); draw(x + 11, y + 2, 8, 26, ink);
        draw(x + 22, y + 9, 8, 19, vjo_chrome_mix(t->accent, t->text, 100));
    } else { /* download tray */
        draw(x + 13, y + 2, 5, 16, ink); draw(x + 9, y + 13, 13, 4, ink);
        draw(x + 12, y + 17, 7, 3, ink); draw(x + 14, y + 20, 3, 2, ink);
        draw(x + 1, y + 23, 30, 6, ink); draw(x + 4, y + 23, 24, 3, t->bg);
    }
}
static inline void vjo_chrome_check(VjoChromeRect draw, int x, int y, unsigned ink, int checked)
{
    draw(x, y, 18, 2, ink); draw(x, y + 16, 18, 2, ink);
    draw(x, y, 2, 18, ink); draw(x + 16, y, 2, 18, ink);
    if (checked) {
        draw(x + 4, y + 8, 3, 3, ink); draw(x + 6, y + 10, 3, 3, ink);
        draw(x + 8, y + 7, 3, 5, ink); draw(x + 10, y + 5, 3, 4, ink);
    }
}
#endif
