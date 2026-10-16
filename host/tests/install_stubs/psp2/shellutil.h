/* Host-only SceShellUtil boundary for the install engine (settings/install.c): the PS button and
 * power-off menu lock the install holds (host/tests/test_settings_install.c records the calls). */
#ifndef VJO_TEST_SHELLUTIL_H
#define VJO_TEST_SHELLUTIL_H
#include <psp2/host_stubs.h>
#define SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN 0x1
#define SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU 0x4
#define SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND 1
int sceShellUtilInitEvents(int unk);
int sceShellUtilLock(int type);
int sceShellUtilUnlock(int type);
#endif
