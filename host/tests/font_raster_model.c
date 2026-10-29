/* The pinned libvita2d PVF/atlas initialization calls in a separate object:
 * --wrap must intercept these just as it intercepts libvita2d.a on Vita.
 * No firmware rasterization or GPU quality is simulated here. */
#include <psp2/pvf.h>
#include <vita2d.h>
void *texture_atlas_create(int width, int height, SceGxmTextureFormat format);

void *vjo_font_model_load(ScePvfFontId font, ScePvfError *error)
{
    *error = scePvfSetCharSize(font, 10.125f, 10.125f);
    return texture_atlas_create(512, 512, SCE_GXM_TEXTURE_FORMAT_U8_R111);
}

void vjo_font_model_filter(vita2d_texture *texture)
{
    vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_LINEAR);
}

void *vjo_font_model_other_atlas(int width, int height, SceGxmTextureFormat format)
{
    return texture_atlas_create(width, height, format);
}
