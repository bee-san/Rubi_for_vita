#ifndef VJO_GAME_HOST_STUBS
#define VJO_GAME_HOST_STUBS
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef unsigned SceSize, SceUInt;
typedef int64_t SceOff;
typedef struct { unsigned size; int size_user, size_cdram, size_phycont; } SceKernelFreeMemorySizeInfo;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 1
#define SCE_O_RDONLY 0
#define SCE_SEEK_SET SEEK_SET
#define SCE_SEEK_END SEEK_END
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_STOP_SUCCESS 0
#define SCE_KERNEL_STOP_FAIL 1
#define sceClibMemset memset
uint64_t sceKernelGetProcessTimeWide(void);
SceUID sceKernelGetProcessId(void);
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *);
SceUID sceKernelAllocMemBlock(const char *, int, unsigned, void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, unsigned, unsigned, int, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelWaitThreadEnd(SceUID, int *, void *);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(unsigned);
int sceAppMgrAppParamGetString(int, int, char *, SceSize);
SceUID sceIoOpen(const char *, int, unsigned);
SceOff sceIoLseek(SceUID, SceOff, int);
int sceIoRead(SceUID, void *, SceSize);
int sceIoClose(SceUID);
int vjoGetVersion(void);
#endif
