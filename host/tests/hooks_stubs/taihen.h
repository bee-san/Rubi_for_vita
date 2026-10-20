/* taiHEN's user API (taihen.h of the VitaSDK, taiHEN 309b380) as game_ocr/hooks.c uses it, for the
 * engine's host test (host/tests/test_hooks_engine.c defines the functions). The types, the helpers
 * and TAI_CONTINUE are the SDK's own; only this test's include path has this directory. */
#ifndef VJO_TEST_HOOKS_TAIHEN_H
#define VJO_TEST_HOOKS_TAIHEN_H
#include <psp2/host_stubs.h>

#define TAI_MAIN_MODULE ((void *)0)

typedef struct _tai_module_info {
    size_t size;
    SceUID modid;
    uint32_t module_nid;
    char name[27];
    uintptr_t exports_start, exports_end, imports_start, imports_end;
} tai_module_info_t;

typedef struct _tai_hook_args {
    size_t size;
    const char *module;
    uint32_t library_nid;
    uint32_t func_nid;
    const void *hook_func;
} tai_hook_args_t;

typedef struct _tai_offset_args {
    size_t size;
    SceUID modid;
    int segidx;
    uint32_t offset;
    int thumb;
    const void *source;
    size_t source_size;
} tai_offset_args_t;

typedef uintptr_t tai_hook_ref_t;

struct _tai_hook_user {
    uintptr_t next;
    void *func;
    void *old;
};

SceUID taiHookFunctionImportForUser(tai_hook_ref_t *p_hook, tai_hook_args_t *args);
SceUID taiHookFunctionOffsetForUser(tai_hook_ref_t *p_hook, tai_offset_args_t *args);
int taiGetModuleInfo(const char *module, tai_module_info_t *info);
int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook);

static inline SceUID taiHookFunctionImport(tai_hook_ref_t *p_hook, const char *module, uint32_t import_library_nid,
                                           uint32_t import_func_nid, const void *hook_func)
{
    tai_hook_args_t args;
    args.size = sizeof(args);
    args.module = module;
    args.library_nid = import_library_nid;
    args.func_nid = import_func_nid;
    args.hook_func = hook_func;
    return taiHookFunctionImportForUser(p_hook, &args);
}

static inline SceUID taiHookFunctionOffset(tai_hook_ref_t *p_hook, SceUID modid, int segidx, uint32_t offset,
                                           int thumb, const void *hook_func)
{
    tai_offset_args_t args;
    args.size = sizeof(args);
    args.modid = modid;
    args.segidx = segidx;
    args.offset = offset;
    args.thumb = thumb;
    args.source = hook_func;
    return taiHookFunctionOffsetForUser(p_hook, &args);
}

#define TAI_CONTINUE(type, hook, ...) ({ \
    struct _tai_hook_user *cur, *next; \
    cur = (struct _tai_hook_user *)(hook); \
    next = (struct _tai_hook_user *)cur->next; \
    (next == NULL) ? ((type(*)())cur->old)(__VA_ARGS__) : ((type(*)())next->func)(__VA_ARGS__); \
})

#endif
