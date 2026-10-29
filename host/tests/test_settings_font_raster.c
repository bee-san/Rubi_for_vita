#include "acutest.h"
#include <psp2/pvf.h>
#include <vita2d.h>
#include "../../include/vjo_ledger_budget.h"

void *vjo_font_model_load(ScePvfFontId font, ScePvfError *error);
void vjo_font_model_filter(vita2d_texture *texture);
void *vjo_font_model_other_atlas(int width, int height, SceGxmTextureFormat format);

static vita2d_texture texture;
static ScePvfFontId last_font;
static float last_h, last_v;
static int atlas_w, atlas_h, atlas_fail;
static SceGxmTextureFormat atlas_format;
static SceGxmTextureFilter filter_min, filter_mag;
static ScePvfError firmware_error;

ScePvfError scePvfSetCharSize(ScePvfFontId font, float h, float v)
{ last_font = font; last_h = h; last_v = v; return firmware_error; }

void *texture_atlas_create(int width, int height, SceGxmTextureFormat format)
{
    atlas_w = width; atlas_h = height; atlas_format = format;
    return atlas_fail ? NULL : &texture;
}

SceGxmTextureFormat vita2d_texture_get_format(const vita2d_texture *t)
{ TEST_CHECK(t == &texture); return atlas_format; }

void vita2d_texture_set_filters(vita2d_texture *t, SceGxmTextureFilter min, SceGxmTextureFilter mag)
{ TEST_CHECK(t == &texture); filter_min = min; filter_mag = mag; }

static void higher_resolution_font_and_matching_atlas(void)
{
    ScePvfError error = 1;
    TEST_CHECK(vjo_font_model_load(&texture, &error) == &texture);
    TEST_CHECK(error == 0 && last_font == &texture);
    TEST_CHECK(last_h == 20.25f && last_v == 20.25f);
    /* At libvita2d's 128 dpi: 36 px rather than 18 px. Both drawing and
     * measurement calibrate against these glyphs; 32 px headers shrink. */
    TEST_CHECK(last_h * 128.0f / 72.0f == 36.0f);
    TEST_CHECK(32.0f / (last_h * 128.0f / 72.0f) < 1.0f);
    TEST_CHECK(atlas_w == 1024 && atlas_h == 1024);
    TEST_CHECK(atlas_format == SCE_GXM_TEXTURE_FORMAT_U8_R111);
    TEST_CHECK(VJO_BUDGET_APP_FONT_ATLASES == 4 * atlas_w * atlas_h);
    vjo_font_model_filter(&texture);
    TEST_CHECK(filter_min == SCE_GXM_TEXTURE_FILTER_LINEAR && filter_mag == SCE_GXM_TEXTURE_FILTER_LINEAR);
}

static void other_textures_unchanged_and_failures_propagated(void)
{
    vjo_font_model_other_atlas(512, 512, 123);
    TEST_CHECK(atlas_w == 512 && atlas_h == 512 && atlas_format == 123);
    vjo_font_model_filter(&texture);
    TEST_CHECK(filter_min == SCE_GXM_TEXTURE_FILTER_POINT && filter_mag == SCE_GXM_TEXTURE_FILTER_LINEAR);
    vjo_font_model_other_atlas(128, 256, SCE_GXM_TEXTURE_FORMAT_U8_R111);
    TEST_CHECK(atlas_w == 128 && atlas_h == 256);
    firmware_error = 0x80460001u;
    atlas_fail = 1;
    ScePvfError error = 0;
    TEST_CHECK(vjo_font_model_load(&texture, &error) == NULL);
    TEST_CHECK(error == firmware_error);
}

TEST_LIST = {
    {"higher resolution font and matching atlas", higher_resolution_font_and_matching_atlas},
    {"other textures unchanged and failures propagated", other_textures_unchanged_and_failures_propagated},
    {NULL, NULL},
};
