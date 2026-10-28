/* Vita-inspired application chrome, drawn with bounded rectangles: no textures, allocations or animation.
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
    /* Dark schemes have a deeper tint; Paper keeps a pale background. */
    unsigned depth = t == vjo_app_theme(VJO_THEME_PAPER) ? 24 : 82;
    for (int band = 0; band < 32; band++) {
        int y = 4 + 540 * band / 32, end = 4 + 540 * (band + 1) / 32;
        draw(0, y, 960, end - y, vjo_chrome_mix(t->bg, t->accent, 8 + depth * band / 31));
    }
}
static inline void vjo_chrome_panel(VjoChromeRect draw, int x, int y, int w, int h,
                                    const VjoAppTheme *t, int focus, int rounded)
{
    unsigned surface = focus ? t->selected : t->card;
    if (rounded) {
        vjo_chrome_round(draw, x, y, w, h, focus ? t->accent : vjo_chrome_mix(t->card, t->dim, 40));
        vjo_chrome_round(draw, x + 2, y + 2, w - 4, h - 4, surface);
    } else {
        draw(x, y, w, h, surface);
        if (focus) {
            draw(x, y, 4, h, t->accent); draw(x, y, w, 2, t->accent);
            draw(x, y + h - 2, w, 2, t->accent); draw(x + w - 4, y, 4, h, t->accent);
        }
    }
    draw(x + 8, y + 3, w - 16, 2, vjo_chrome_mix(surface, t->text, 40));
    draw(x + 8, y + 5, w - 16, 2, vjo_chrome_mix(surface, t->text, 20));
}
static inline void vjo_chrome_button(VjoChromeRect draw, int x, int y, int w, int h,
                                     const VjoAppTheme *t, int active)
{
    unsigned fill = active ? t->accent : t->selected;
    draw(x, y, w, h, fill);
    draw(x + 5, y + 2, w - 10, 2, vjo_chrome_mix(fill, t->text, active ? 40 : 30));
    draw(x + 5, y + h - 2, w - 10, 1, vjo_chrome_mix(fill, t->bg, 100));
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
