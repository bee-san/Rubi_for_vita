/* The Overlay app's graphics start as a system application: libvita2d's two calls into the
 * system, through the link's wrappers. This first version passes both on unchanged, as main's
 * app makes them (the tests' baseline, host/tests/test_settings_sysapp_gfx.c). */
#include "sysapp_gfx.h"
#include <psp2/gxm.h>
#include <psp2/sharedfb.h>

static VjoSysappGfx gfx = {.fb_id = -1};

const VjoSysappGfx *vjo_sysapp_gfx(void) { return &gfx; }

int __real_sceGxmVshInitialize(const SceGxmInitializeParams *params);
int __wrap_sceGxmVshInitialize(const SceGxmInitializeParams *params)
{
    return __real_sceGxmVshInitialize(params);
}

int __real_sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info);
int __wrap_sceSharedFbGetInfo(SceUID fb_id, SceSharedFbInfo *info)
{
    return __real_sceSharedFbGetInfo(fb_id, info);
}
