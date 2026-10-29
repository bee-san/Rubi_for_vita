/* The Rubi app's three palettes. Packed like vita2d RGBA8 (AABBGGRR).
 * App-only: no palette data or preference parsing is added to SceShell. */
#ifndef VJO_SETTINGS_THEMES_H
#define VJO_SETTINGS_THEMES_H

#include <stdint.h>

enum { VJO_THEME_DEFAULT, VJO_THEME_MINT, VJO_THEME_PAPER, VJO_THEMES };
typedef struct {
    const char *name;
    uint32_t bg, card, selected, text, dim, accent, off;
    uint32_t highlight, reading, rank, definition; /* Look up's styled dictionary entries */
} VjoAppTheme;

#define VJO_THEME_RGB(r, g, b) (0xFF000000u | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))
static const VjoAppTheme vjo_app_themes[VJO_THEMES] = {
    {"Default", VJO_THEME_RGB(15, 16, 17), VJO_THEME_RGB(25, 27, 29), VJO_THEME_RGB(43, 47, 49),
     VJO_THEME_RGB(240, 242, 244), VJO_THEME_RGB(184, 187, 191), VJO_THEME_RGB(255, 210, 74),
     VJO_THEME_RGB(244, 160, 167), VJO_THEME_RGB(255, 210, 74), VJO_THEME_RGB(159, 208, 255),
     VJO_THEME_RGB(138, 148, 166), VJO_THEME_RGB(184, 192, 204)},
    {"Mint", VJO_THEME_RGB(15, 16, 17), VJO_THEME_RGB(25, 27, 29), VJO_THEME_RGB(43, 47, 49),
     VJO_THEME_RGB(240, 242, 244), VJO_THEME_RGB(184, 187, 191), VJO_THEME_RGB(91, 220, 207),
     VJO_THEME_RGB(244, 160, 167), VJO_THEME_RGB(255, 210, 74), VJO_THEME_RGB(159, 208, 255),
     VJO_THEME_RGB(138, 148, 166), VJO_THEME_RGB(184, 192, 204)},
    {"Paper", VJO_THEME_RGB(247, 242, 232), VJO_THEME_RGB(255, 252, 245), VJO_THEME_RGB(232, 223, 207),
     VJO_THEME_RGB(48, 45, 40), VJO_THEME_RGB(102, 95, 83), VJO_THEME_RGB(56, 106, 89),
     VJO_THEME_RGB(161, 59, 72), VJO_THEME_RGB(128, 85, 13), VJO_THEME_RGB(48, 94, 131),
     VJO_THEME_RGB(102, 95, 83), VJO_THEME_RGB(81, 75, 67)},
};
#undef VJO_THEME_RGB

static inline const VjoAppTheme *vjo_app_theme(int index)
{
    return &vjo_app_themes[index >= 0 && index < VJO_THEMES ? index : VJO_THEME_DEFAULT];
}
#endif
