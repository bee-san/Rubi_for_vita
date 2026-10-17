/* libvita2d's start as a system application, for host/tests/test_settings_sysapp_gfx.c: the
 * pinned libvita2d.a's vita2d_init_internal up to its first use of the GPU library (xerpi
 * libvita2d a8f15ab, libvita2d/source/vita2d.c:233-273; the archive's literal pool has its
 * flags 0x0A, 2 pending swaps, 4 bytes of callback data and a 2 MiB parameter buffer, and its
 * loop has no pause: docs/vita-memory.md section 5.4, "Starting with no game"). Its own
 * object, so that the link's --wrap reaches its calls as it reaches libvita2d.a's on the Vita;
 * the system it calls is the test's. */
#include <psp2/gxm.h>
#include <psp2/sharedfb.h>
#include <string.h>

int vjo_model_budget_info(void);               /* !sceAppMgrGetBudgetInfo(&info): a system application */
void vjo_model_gpu_used(const char *what);     /* the GPU library is used: it must have started */

static SceSharedFbInfo shared_fb_info;

static void display_callback(const void *data) { (void)data; }

void vjo_model_vita2d_init(void *display[2])
{
    int system_app_mode = vjo_model_budget_info();
    SceGxmInitializeParams initializeParams;
    memset(&initializeParams, 0, sizeof(SceGxmInitializeParams));
    initializeParams.flags = system_app_mode ? 0x0A : 0;
    initializeParams.displayQueueMaxPendingCount = 2;
    initializeParams.displayQueueCallback = display_callback;
    initializeParams.displayQueueCallbackDataSize = 4;
    initializeParams.parameterBufferSize = system_app_mode ? 2 * 1024 * 1024 : SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    /* libvita2d keeps the result for a DEBUG line only (vita2d.c:246-247) */
    (void)(system_app_mode ? sceGxmVshInitialize(&initializeParams) : sceGxmInitialize(&initializeParams));
    while (system_app_mode) {
        SceUID shared_fb = sceSharedFbOpen(1);
        memset(&shared_fb_info, 0, sizeof(SceSharedFbInfo));
        sceSharedFbGetInfo(shared_fb, &shared_fb_info);
        if (shared_fb_info.index == 1) sceSharedFbClose(shared_fb);
        else {
            vjo_model_gpu_used("sceGxmMapMemory(shared framebuffer)");
            display[0] = shared_fb_info.fb_base;
            display[1] = shared_fb_info.fb_base2;
            memset(&shared_fb_info, 0, sizeof(SceSharedFbInfo));
            break;
        }
    }
    vjo_model_gpu_used("gpu_alloc(VDM ring buffer)"); /* vita2d.c:265 on */
}
