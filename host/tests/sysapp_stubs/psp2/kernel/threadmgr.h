/* Host-only thread types for test_settings_sysapp.c, as the SDK declares them
 * (psp2common/kernel/threadmgr.h). */
#ifndef VJO_TEST_SYSAPP_THREADMGR_H
#define VJO_TEST_SYSAPP_THREADMGR_H
typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
typedef struct SceKernelThreadOptParam {
    SceSize size;
    SceUInt attr;
} SceKernelThreadOptParam;
#endif
