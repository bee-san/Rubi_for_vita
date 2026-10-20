/* psp2/kernel/modulemgr.h as game_ocr/hooks.c uses it (its host test, host/tests/test_hooks_engine.c):
 * the SDK's SceKernelModuleInfo fields it reads, in the SDK's order. */
#ifndef VJO_TEST_HOOKS_MODULEMGR_H
#define VJO_TEST_HOOKS_MODULEMGR_H
#include <psp2/host_stubs.h>

typedef struct SceKernelSegmentInfo {
    SceSize size;
    SceUInt perms;
    void *vaddr;
    SceSize memsz;
    SceSize filesz;
    SceUInt res;
} SceKernelSegmentInfo;

typedef struct SceKernelModuleInfo {
    SceSize size;
    SceUID modid;
    uint16_t modattr;
    uint8_t modver[2];
    char module_name[28];
    SceUInt unk28;
    void *start_entry, *stop_entry, *exit_entry, *exidx_top, *exidx_btm, *extab_top, *extab_btm, *tlsInit;
    SceSize tlsInitSize, tlsAreaSize;
    char path[256];
    SceKernelSegmentInfo segments[4];
    SceUInt state;
} SceKernelModuleInfo;

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info);

#endif
