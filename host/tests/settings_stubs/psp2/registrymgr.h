/* Host-only SceRegMgr boundary for the Settings app harness: the test defines
 * the function and the value it reads. */
#ifndef VJO_TEST_SETTINGS_REGISTRYMGR_H
#define VJO_TEST_SETTINGS_REGISTRYMGR_H
int sceRegMgrGetKeyInt(const char *category, const char *name, int *buf);
#endif
