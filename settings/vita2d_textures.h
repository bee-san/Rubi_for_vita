/* settings/vita2d_textures.c: libvita2d's plain textures, their two stale fields cleared (libvita2d #28). */
#ifndef VJO_SETTINGS_VITA2D_TEXTURES_H
#define VJO_SETTINGS_VITA2D_TEXTURES_H
#include <vita2d.h>
/* The link's --wrap=vita2d_create_empty_texture_format: libvita2d's own, with gxm_rtgt and depth_UID cleared. */
vita2d_texture *__wrap_vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format);
#endif
