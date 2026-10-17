/* Host-only thread calls for test_settings_sysapp_gfx.c (the SDK's psp2/kernel/threadmgr.h). */
#ifndef VJO_TEST_SYSAPP_GFX_THREADMGR_H
#define VJO_TEST_SYSAPP_GFX_THREADMGR_H
typedef int SceUID;
typedef unsigned int SceUInt;
int sceKernelDelayThread(SceUInt delay);
int sceKernelGetThreadCurrentPriority(void);
int sceKernelGetThreadCpuAffinityMask(SceUID thid);
#endif
