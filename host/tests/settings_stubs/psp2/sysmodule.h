#ifndef VJO_TEST_SYSMODULE_H
#define VJO_TEST_SYSMODULE_H
#define SCE_SYSMODULE_IME 0x00CF
int sceSysmoduleIsLoaded(unsigned short id);
int sceSysmoduleLoadModule(unsigned short id);
int sceSysmoduleUnloadModule(unsigned short id);
#endif
