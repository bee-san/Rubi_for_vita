/* Host-only SDK boundary for exercising the real shell control policy.
 * No threads or captures are started; time and job notifications are supplied
 * by the regression harness. The Vita build never includes these headers. */
#ifndef VJO_TEST_VITA_STUBS_H
#define VJO_TEST_VITA_STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef unsigned int SceSize;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR_PAT 2
#define sceClibMemcpy memcpy
#define sceClibMemmove memmove
#define sceClibMemset memset
#define sceClibSnprintf snprintf
#define sceClibStrnlen strnlen
#define sceClibStrcmp strcmp
#define sceClibStrncmp strncmp
uint64_t sceKernelGetProcessTimeWide(void);
uint64_t sceKernelGetSystemTimeWide(void); /* the log's clock (kernel.txt's): defined by the test that uses it */
int sceKernelDelayThread(unsigned us);
static inline int sceKernelPowerTick(int type) { return 0; }
int sceKernelSetEventFlag(SceUID uid, unsigned int bits);
#ifdef VJO_TEST_MUTEX_STUBS
int sceKernelLockMutex(SceUID uid, int count, void *timeout);
int sceKernelUnlockMutex(SceUID uid, int count);
#else
static inline int sceKernelLockMutex(SceUID uid, int count, void *timeout) { return 0; }
static inline int sceKernelUnlockMutex(SceUID uid, int count) { return 0; }
#endif
#ifdef VJO_TEST_MEMORY_STUBS
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned int size, void *opt);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);
#else
static inline SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned int size, void *opt) { return -1; }
static inline int sceKernelGetMemBlockBase(SceUID uid, void **base) { return -1; }
static inline int sceKernelFreeMemBlock(SceUID uid) { return 0; }
#endif
static inline int sceKernelWaitEventFlag(SceUID uid, unsigned int bits, unsigned int mode,
                                        unsigned int *out, void *timeout) { return -1; }
#ifdef VJO_TEST_CREATE_STUBS
SceUID sceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt);
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int priority,
                             unsigned int stack, unsigned int attr, int affinity, void *opt);
int sceKernelStartThread(SceUID uid, SceSize size, void *args);
int sceKernelDeleteThread(SceUID uid);
int sceKernelDeleteMutex(SceUID uid);
#else
static inline SceUID sceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt) { return -1; }
#endif
static inline SceUID sceKernelCreateEventFlag(const char *name, unsigned int attr, unsigned int bits, void *opt) { return -1; }
#ifndef VJO_TEST_CREATE_STUBS
static inline SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int priority,
                                          unsigned int stack, unsigned int attr, int affinity, void *opt) { return -1; }
static inline int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return -1; }
#endif
#ifdef VJO_TEST_THREAD_STUBS
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout);
#else
static inline int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return -1; }
#endif
#ifndef VJO_TEST_CREATE_STUBS
static inline int sceKernelDeleteThread(SceUID uid) { return 0; }
static inline int sceKernelDeleteMutex(SceUID uid) { return 0; }
#endif
static inline int sceKernelDeleteEventFlag(SceUID uid) { return 0; }
static inline int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
#endif
