/* The exact page recoloring loop is also exercised with host widgets. No new PAF widgets or buffers. */
#ifndef VJO_COLOUR_WIDGETS_H
#define VJO_COLOUR_WIDGETS_H
#include "../core/colour_scheme.h"
#include "../core/port.h"

/* Mint uses the original dark surfaces. Paper replaces them with opaque light surfaces. */
static const char *const vjo_colour_surfaces[] VJO_DATA_TABLE(theme_surfaces) = {
    "vjo_panel", "vjo_band_top", "vjo_band_mid", "vjo_band_footer", "vjo_strip", "vjo_separator"
};

template<class Page, class Color>
void vjo_colour_widgets(Page *page, int scheme, Color (*color)(uint32_t, float))
{
    if (scheme != VJO_THEME_PAPER) return;
    const Color colors[] = {color(VJO_PAPER_BG, 1.0f), color(VJO_PAPER_TEXT, 0.3f)};
    for (unsigned i = 0; i < sizeof(vjo_colour_surfaces)/sizeof(vjo_colour_surfaces[0]); i++) {
        auto *w = page->FindChild(vjo_colour_surfaces[i]);
        if (w) w->SetColor(colors[i == 5]);
    }
}
#endif
