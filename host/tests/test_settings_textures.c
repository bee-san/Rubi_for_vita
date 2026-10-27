/* settings/vita2d_textures.c (#313): the wrapper of libvita2d's vita2d_create_empty_texture_format clears a plain
 * texture's render target and depth buffer, which libvita2d leaves as its heap memory had them (libvita2d #28), and
 * leaves the rest as libvita2d made it; a texture libvita2d could not make stays NULL. The link's --wrap and the
 * atlas's call of the wrapper are checked on the built app by tools/check_settings_self.py. */
#include "acutest.h"
#include <string.h>
#include <vita2d.h>
#include "../../settings/vita2d_textures.h"

static vita2d_texture made;
static int fail, calls;
static unsigned last_w, last_h, last_format;

/* libvita2d's own, as the link's --wrap names it: a texture in reused heap memory. */
vita2d_texture *__real_vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format)
{
    calls++;
    last_w = w, last_h = h, last_format = format;
    if (fail)
        return NULL;
    memset(&made, 0xA5, sizeof(made));
    made.width = w;
    made.height = h;
    return &made;
}

static void a_plain_texture_has_no_stale_render_target_or_depth_buffer(void)
{
    vita2d_texture *t = __wrap_vita2d_create_empty_texture_format(512, 512, 0x83000000u);
    TEST_ASSERT(t == &made && calls == 1 && last_w == 512 && last_h == 512 && last_format == 0x83000000u);
    TEST_CHECK(t->gxm_rtgt == NULL && t->depth_UID == 0);
    /* the rest as libvita2d made it */
    TEST_CHECK(t->width == 512 && t->height == 512 && t->live == (int)0xA5A5A5A5 && t->in_flight == (int)0xA5A5A5A5);
    fail = 1;
    TEST_CHECK(__wrap_vita2d_create_empty_texture_format(16, 16, 0) == NULL && calls == 2);
}

TEST_LIST = {
    {"a plain texture has no stale render target or depth buffer", a_plain_texture_has_no_stale_render_target_or_depth_buffer},
    {NULL, NULL},
};
