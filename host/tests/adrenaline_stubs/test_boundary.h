#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef uintptr_t tai_hook_ref_t;
typedef struct { unsigned size; } tai_module_info_t;
#define SCE_O_RDONLY 0
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_STOP_SUCCESS 0
#define sceClibStrnlen strnlen
#define sceClibStrncmp strncmp
#define sceClibStrncpy strncpy
#define sceClibSnprintf snprintf
static int continue_call(tai_hook_ref_t ref, ...);
#define TAI_CONTINUE(type, ref, ...) ((type)continue_call(ref, __VA_ARGS__))
SceUID sceIoOpen(const char *, int, unsigned);
int sceIoRead(SceUID, void *, SceSize);
int sceIoClose(SceUID);
char *sceLsdbGetName(void *);
int taiGetModuleInfo(const char *, tai_module_info_t *);
SceUID taiHookFunctionImport(tai_hook_ref_t *, const char *, uint32_t, uint32_t, void *);
int taiHookRelease(SceUID, tai_hook_ref_t);
int sceAppMgrDestroyAppByName(const char *);
