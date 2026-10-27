/* libvita2d a8f15ab mallocs a texture without clearing it, and sets its render target and depth buffer only for a
 * render target (vita2d_texture.c _vita2d_create_empty_texture_format_advanced), but vita2d_free_texture destroys the
 * one and frees the other whenever they are not zero (libvita2d #28): freeing a plain texture made in reused heap
 * memory can destroy a stale render target or free another block. settings/main.c clears the two of each icon it
 * loads; a font's glyph atlas (texture_atlas_create) is libvita2d's own, never in the app's hands, and the app frees
 * fonts while it runs (main.c font_tick: a font whose atlas is full is replaced). So the link wraps
 * vita2d_create_empty_texture_format for libvita2d's other files (vita/CMakeLists.txt --wrap; the build checks that
 * the atlas calls the wrapper, tools/check_settings_self.py): every plain texture made through it has the two
 * cleared, as by right. A render target (vita2d_create_empty_texture_rendertarget) is made apart and keeps its own. */
#include <stddef.h>
#include <vita2d.h>
#include "vita2d_textures.h"

vita2d_texture *__real_vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format);

vita2d_texture *__wrap_vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format)
{
    vita2d_texture *texture = __real_vita2d_create_empty_texture_format(w, h, format);
    if (texture) {
        texture->gxm_rtgt = NULL;
        texture->depth_UID = 0;
    }
    return texture;
}
