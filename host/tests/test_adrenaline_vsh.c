#include "acutest.h"
#define ADR_VSH_HOST 1
#include "../../compat/adrenaline_vsh/main.c"

static int original_type, original_rc, have_database, have_boot, have_inf, short_boot;
static unsigned char boot_magic[4];
static unsigned opens, closes, installs[4], releases[4], destroys;
static const char *title;

static int continue_call(tai_hook_ref_t ref, ...) { return ref == 3 ? original_type : original_rc; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{
    (void)flags; (void)mode; opens++;
    return (strstr(path, "boot.bin") && have_boot) || (strstr(path, "boot.inf") && have_inf) ? 7 : -1;
}
int sceIoRead(SceUID fd, void *out, SceSize size)
{ TEST_CHECK(fd == 7 && size == 4); memcpy(out, boot_magic, 4); return short_boot ? 3 : 4; }
int sceIoClose(SceUID fd) { TEST_CHECK(fd == 7); closes++; return 0; }
char *sceLsdbGetName(void *entry) { (void)entry; return (char *)title; }
int taiGetModuleInfo(const char *name, tai_module_info_t *info)
{ TEST_CHECK(!strcmp(name, "SceLsdb") && info->size == sizeof(*info)); return have_database ? 0 : -1; }
SceUID taiHookFunctionImport(tai_hook_ref_t *ref, const char *module, uint32_t library, uint32_t nid, void *fn)
{
    (void)library; (void)nid; (void)fn;
    unsigned i = (unsigned)(ref - refs);
    TEST_ASSERT(i < 4 && !strcmp(module, "SceShell"));
    *ref = i; installs[i]++; return (int)(10 + i);
}
int taiHookRelease(SceUID uid, tai_hook_ref_t ref)
{ TEST_CHECK(uid == (int)(10 + ref)); releases[ref]++; return 0; }
int sceAppMgrDestroyAppByName(const char *name) { TEST_CHECK(name[0]); destroys++; return 0; }
static void reset(void)
{
    for (int i = 0; i < 4; i++) { hooks[i] = -1; refs[i] = (unsigned)i; }
    memset(installs, 0, sizeof(installs)); memset(releases, 0, sizeof(releases));
    memset(app_titleid, 0, sizeof(app_titleid));
    original_type = 1; original_rc = have_database = have_boot = have_inf = short_boot = 0;
    opens = closes = destroys = 0; title = "PCSG00415";
    memcpy(boot_magic, "ABB", 4);
}
static void catalogue_and_bubbles(void)
{
    reset();
    TEST_CHECK(get_type(NULL) == 1);
    have_boot = 1; TEST_CHECK(get_type(NULL) == 2 && closes == 1);
    short_boot = 1; TEST_CHECK(get_type(NULL) == 1);
    short_boot = 0; boot_magic[0] = 'X'; TEST_CHECK(get_type(NULL) == 1);
    have_inf = 1; TEST_CHECK(get_type(NULL) == 2);
    reset(); title = "PSPEMU004"; TEST_CHECK(get_type(NULL) == 2 && !opens);
    title = "PSPEMUCFW"; TEST_CHECK(get_type(NULL) == 2 && !opens);
    title = NULL; TEST_CHECK(get_type(NULL) == 1 && !opens);
    title = ""; TEST_CHECK(get_type(NULL) == 1 && !opens);
    title = "1234567890123456789"; TEST_CHECK(get_type(NULL) == 1 && !opens);
    title = "PSPEMU004"; original_type = 4; TEST_CHECK(get_type(NULL) == 4 && !opens);
}
static void hook_lifecycle(void)
{
    reset();
    TEST_CHECK(module_start(0, NULL) == 0 && installs[0] == 1 && installs[1] == 1 && !installs[3] && !destroys);
    TEST_CHECK(load_module(0x80000008, 0, NULL, NULL) == 0 && installs[2] == 1);
    TEST_CHECK(paf_load(0, "vs0:vsh/shell/livespace_db.suprx", 0, 0, 0) == 0 && installs[3] == 1);
    hook_database(); TEST_CHECK(installs[3] == 1);
    original_rc = -7;
    TEST_CHECK(unload_module(0x80000008, 0, NULL, NULL) == -7 && !releases[2] && !releases[3]);
    original_rc = 0;
    TEST_CHECK(unload_module(0x80000008, 0, NULL, NULL) == 0 && hooks[2] == -1 && releases[2] == 1);
    TEST_CHECK(hooks[3] == -1 && releases[3] == 1);
    TEST_CHECK(load_module(0x80000008, 0, NULL, NULL) == 0 && installs[2] == 2);
    TEST_CHECK(paf_load(0, "vs0:vsh/shell/livespace_db.suprx", 0, 0, 0) == 0 && installs[3] == 2);
    TEST_CHECK(module_stop(0, NULL) == 0 && releases[0] == 1 && releases[1] == 1 && releases[2] == 2 && releases[3] == 2);
    module_stop(0, NULL); TEST_CHECK(releases[2] == 2);
    reset(); have_database = 1;
    TEST_CHECK(module_start(0, NULL) == 0 && installs[3] == 1 && !installs[0] && !installs[1]);
    reset(); original_rc = -7;
    TEST_CHECK(load_module(0x80000008, 0, NULL, NULL) == -7 && !installs[2]);
    TEST_CHECK(paf_load(0, "vs0:vsh/shell/livespace_db.suprx", 0, 0, 0) == -7 && !installs[3]);
}
TEST_LIST = {{"PSP and bubble catalogue types", catalogue_and_bubbles}, {"PAF and database hook lifecycle", hook_lifecycle}, {NULL, NULL}};
