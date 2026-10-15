#ifndef VJO_TEST_GAME_MODULEMGR_H
#define VJO_TEST_GAME_MODULEMGR_H
#include <psp2/host_stubs.h>
/* game_ocr/main.c's PSP emulator check (taiGetModuleInfo("ScePspemu") on the
 * Vita): a test sets vjo_test_pspemu for a process with that module. */
static int vjo_test_pspemu;
static inline int vjo_test_pspemu_process(void) { return vjo_test_pspemu; }
#endif
