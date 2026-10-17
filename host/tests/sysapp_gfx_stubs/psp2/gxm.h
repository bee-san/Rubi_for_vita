/* Host-only libgxm boundary for test_settings_sysapp_gfx.c: the SDK's initialize parameters
 * (psp2/gxm.h, 0x14 bytes), its flags and the error codes the tests use. */
#ifndef VJO_TEST_SYSAPP_GFX_GXM_H
#define VJO_TEST_SYSAPP_GFX_GXM_H
typedef unsigned int SceSize;
typedef void(SceGxmDisplayQueueCallback)(const void *callbackData);
#define SCE_GXM_INITIALIZE_FLAG_PB_LPDDR 0x00000001u
#define SCE_GXM_INITIALIZE_FLAG_SHARED_SYNC 0x00000002u
#define SCE_GXM_INITIALIZE_FLAG_SHAREDPB_CREATE 0x00000004u
#define SCE_GXM_INITIALIZE_FLAG_SHAREDPB_OPEN 0x00000008u
#define SCE_GXM_ERROR_UNINITIALIZED ((int)0x805B0000)
#define SCE_GXM_ERROR_ALREADY_INITIALIZED ((int)0x805B0001)
#define SCE_GXM_ERROR_INVALID_VALUE ((int)0x805B0003)
#define SCE_GXM_ERROR_DRIVER ((int)0x805B0017)
#define SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE (16 * 1024 * 1024)
typedef struct SceGxmInitializeParams {
    unsigned int flags;
    unsigned int displayQueueMaxPendingCount;
    SceGxmDisplayQueueCallback *displayQueueCallback;
    unsigned int displayQueueCallbackDataSize;
    SceSize parameterBufferSize;
} SceGxmInitializeParams;
int sceGxmVshInitialize(const SceGxmInitializeParams *params);
int sceGxmInitialize(const SceGxmInitializeParams *params);
#endif
