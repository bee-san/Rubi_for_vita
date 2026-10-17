/* Host-only shared framebuffer boundary for test_settings_sysapp_gfx.c: the SDK's structure
 * (psp2/sharedfb.h, 0x58 bytes; index is the one-based buffer the system handed over, from
 * SceShell's render info at 0x34) and the calls libvita2d makes. */
#ifndef VJO_TEST_SYSAPP_GFX_SHAREDFB_H
#define VJO_TEST_SYSAPP_GFX_SHAREDFB_H
#include <stddef.h>
typedef int SceUID;
typedef struct SceSharedFbInfo {
    void *fb_base;
    int fb_size;
    void *fb_base2;
    int unk0[6];
    int stride;
    int width;
    int height;
    int unk1;
    int index;
    int unk2[4];
    int vsync;
    int unk3[3];
} SceSharedFbInfo;
/* A 32-bit console's layout: the host's pointers are wider, so only the field order is the SDK's. */
SceUID sceSharedFbOpen(int index);
int sceSharedFbClose(SceUID fb_id);
int sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info);
#endif
