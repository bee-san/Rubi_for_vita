/* Host stand-in for VitaSDK's psp2/sysmodule.h (the test defines the functions). */
#ifndef VJO_GAME_TEST_SYSMODULE_H
#define VJO_GAME_TEST_SYSMODULE_H
#include <psp2/host_stubs.h>
#define SCE_SYSMODULE_LOADED 0
#define SCE_SYSMODULE_ERROR_UNLOADED ((int)0x805A1001u)
typedef enum SceSysmoduleModuleId { SCE_SYSMODULE_NET = 0x0001 } SceSysmoduleModuleId;
int sceSysmoduleLoadModule(SceSysmoduleModuleId id);
int sceSysmoduleUnloadModule(SceSysmoduleModuleId id);
int sceSysmoduleIsLoaded(SceSysmoduleModuleId id);
#endif
