/* Host-only SceAppMgr boundary for the install engine: app0:'s unmount (settings/install.c). */
#ifndef VJO_TEST_INSTALL_APPMGR_H
#define VJO_TEST_INSTALL_APPMGR_H
#include <psp2/host_stubs.h>
int sceAppMgrUmount(const char *mount_point);
#endif
