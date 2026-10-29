/* The app palettes, sharing theme IDs and text roles with the overlay.
 * Packed like vita2d RGBA8 (AABBGGRR). */
#ifndef VJO_SETTINGS_THEMES_H
#define VJO_SETTINGS_THEMES_H

#include <stdint.h>

#include "../core/colour_scheme.h"
typedef struct {
    const char *name;
    uint32_t bg, card, selected, text, dim, accent, off;
    uint32_t highlight, reading, rank, definition; /* Look up's styled dictionary entries */
} VjoAppTheme;

#define VJO_THEME_RGB(r, g, b) (0xFF000000u | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))
#define VJO_THEME_PACK(rgb) VJO_THEME_RGB(((rgb) >> 16) & 255, ((rgb) >> 8) & 255, (rgb) & 255)
static const VjoAppTheme vjo_app_themes[VJO_THEMES] = {
    {"Default", VJO_THEME_RGB(15, 16, 17), VJO_THEME_RGB(25, 27, 29), VJO_THEME_RGB(43, 47, 49),
     VJO_THEME_RGB(240, 242, 244), VJO_THEME_RGB(184, 187, 191), VJO_THEME_RGB(255, 210, 74),
     VJO_THEME_RGB(244, 160, 167), VJO_THEME_RGB(255, 210, 74), VJO_THEME_RGB(159, 208, 255),
     VJO_THEME_RGB(138, 148, 166), VJO_THEME_RGB(184, 192, 204)},
    {"Mint", VJO_THEME_RGB(15, 16, 17), VJO_THEME_RGB(25, 27, 29), VJO_THEME_RGB(43, 47, 49),
     VJO_THEME_RGB(240, 242, 244), VJO_THEME_RGB(184, 187, 191), VJO_THEME_PACK(VJO_MINT_ACCENT),
     VJO_THEME_RGB(244, 160, 167), VJO_THEME_PACK(VJO_MINT_ACCENT), VJO_THEME_RGB(159, 208, 255),
     VJO_THEME_RGB(138, 148, 166), VJO_THEME_RGB(184, 192, 204)},
    {"Paper", VJO_THEME_PACK(VJO_PAPER_BG), VJO_THEME_RGB(255, 252, 245), VJO_THEME_RGB(232, 223, 207),
     VJO_THEME_PACK(VJO_PAPER_TEXT), VJO_THEME_PACK(VJO_PAPER_DIM), VJO_THEME_PACK(VJO_PAPER_ACCENT),
     VJO_THEME_PACK(VJO_PAPER_ERROR), VJO_THEME_PACK(VJO_PAPER_HIGHLIGHT), VJO_THEME_PACK(VJO_PAPER_READING),
     VJO_THEME_PACK(VJO_PAPER_DIM), VJO_THEME_PACK(VJO_PAPER_DEFINITION)},
};
#undef VJO_THEME_RGB
#undef VJO_THEME_PACK

static inline const VjoAppTheme *vjo_app_theme(int index)
{
    return &vjo_app_themes[index >= 0 && index < VJO_THEMES ? index : VJO_THEME_DEFAULT];
}
#endif
