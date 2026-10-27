/* Host-only vita2d boundary for the Settings app harness (host/tests/test_settings_app.c):
 * the drawing calls the app makes, recorded per frame. The Vita build never includes it. */
#ifndef VJO_TEST_VITA2D_H
#define VJO_TEST_VITA2D_H
#include <stdint.h>
#define RGBA8(r, g, b, a) ((((a) & 0xFFu) << 24) | (((b) & 0xFFu) << 16) | (((g) & 0xFFu) << 8) | ((r) & 0xFFu))
typedef struct vita2d_pvf vita2d_pvf;
/* libvita2d's struct has these two fields, which settings/main.c clears after
 * a load (the rest of it is the GPU's); the harness keeps its own after them. */
typedef struct vita2d_texture {
    void *gxm_rtgt;
    int depth_UID;
    unsigned width, height;
    char id[16];        /* the title ID tagged in the icon's tEXt chunk */
    int live, in_flight; /* allocated; drawn since the GPU was last waited for */
    unsigned char *pixels;
} vita2d_texture;
int vita2d_init(void);
int vita2d_fini(void);
void vita2d_set_clear_color(unsigned int color);
void vita2d_start_drawing(void);
void vita2d_end_drawing(void);
int vita2d_common_dialog_update(void);
void vita2d_swap_buffers(void);
void vita2d_clear_screen(void);
void vita2d_wait_rendering_done(void);
void vita2d_enable_clipping(void);
void vita2d_disable_clipping(void);
void vita2d_set_clip_rectangle(int x_min, int y_min, int x_max, int y_max);
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color);
vita2d_pvf *vita2d_load_default_pvf(void);
void vita2d_free_pvf(vita2d_pvf *font);
int vita2d_pvf_draw_text(vita2d_pvf *font, int x, int y, unsigned int color, float scale, const char *text);
int vita2d_pvf_text_width(vita2d_pvf *font, float scale, const char *text);
typedef unsigned int SceGxmTextureFormat;
vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format);
vita2d_texture *vita2d_load_PNG_file(const char *filename);
vita2d_texture *vita2d_load_PNG_buffer(const void *buffer);
void vita2d_free_texture(vita2d_texture *texture);
unsigned int vita2d_texture_get_width(const vita2d_texture *texture);
unsigned int vita2d_texture_get_height(const vita2d_texture *texture);
void vita2d_draw_texture_scale(const vita2d_texture *texture, float x, float y, float x_scale, float y_scale);
#endif
