/* Host-only process calls for test_settings_sysapp_gfx.c (the SDK's psp2/kernel/processmgr.h). */
#ifndef VJO_TEST_SYSAPP_GFX_PROCESSMGR_H
#define VJO_TEST_SYSAPP_GFX_PROCESSMGR_H
#include <stdint.h>
uint64_t sceKernelGetProcessTimeWide(void);
int sceKernelExitProcess(int status);
#endif
