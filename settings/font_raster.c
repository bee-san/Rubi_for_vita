/* Settings only: libvita2d a8f15ab rasterizes PVF at 18 px (10.125 pt,
 * 128 dpi), then magnifies that bitmap for our 18-32 px UI. Rasterize at
 * twice the resolution instead. main.c measures the new em and uses the
 * same scale for drawing and layout, so the UI keeps its intended size.
 *
 * Wrap the public firmware call and atlas constructor, without reaching
 * into libvita2d's private font structs. Doubling BOTH the packer's bounds
 * and texture keeps its capacity; enlarging just the texture would not.
 * The same four atlas lifetimes remain (docs/vita-memory.md section 5.4).
 */
#include <psp2/pvf.h>
#include <vita2d.h>
#include "../include/vjo_ledger_budget.h"

ScePvfError __real_scePvfSetCharSize(ScePvfFontId font, float h, float v);
void *__real_texture_atlas_create(int width, int height, SceGxmTextureFormat format);
void __real_vita2d_texture_set_filters(vita2d_texture *texture, SceGxmTextureFilter min,
                                     SceGxmTextureFilter mag);

ScePvfError __wrap_scePvfSetCharSize(ScePvfFontId font, float h, float v)
{
    return __real_scePvfSetCharSize(font, h * 2.0f, v * 2.0f);
}

void *__wrap_texture_atlas_create(int width, int height, SceGxmTextureFormat format)
{
    if (width == 512 && height == 512 && format == SCE_GXM_TEXTURE_FORMAT_U8_R111)
        width = height = VJO_APP_FONT_ATLAS_EDGE;
    return __real_texture_atlas_create(width, height, format);
}

void __wrap_vita2d_texture_set_filters(vita2d_texture *texture, SceGxmTextureFilter min,
                                     SceGxmTextureFilter mag)
{
    /* libvita2d uses POINT for minification. Our larger glyphs are reduced
     * on screen, so sample their coverage smoothly in that direction too.
     * Icon/image textures retain their existing filters. */
    if (vita2d_texture_get_format(texture) == SCE_GXM_TEXTURE_FORMAT_U8_R111)
        min = SCE_GXM_TEXTURE_FILTER_LINEAR;
    __real_vita2d_texture_set_filters(texture, min, mag);
}
