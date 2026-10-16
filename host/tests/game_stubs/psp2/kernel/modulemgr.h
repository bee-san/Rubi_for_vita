#ifndef VJO_TEST_GAME_MODULEMGR_H
#define VJO_TEST_GAME_MODULEMGR_H
#include <psp2/host_stubs.h>
/* game_ocr/main.c's PSP emulator check (taiGetModuleInfo("ScePspemu") on the
 * Vita): a test sets vjo_test_pspemu for a process with that module. */
static int vjo_test_pspemu;
static inline int vjo_test_pspemu_process(void) { return vjo_test_pspemu; }
/* Its one-copy-per-process check (#138: taiGetModuleInfo finds an older
 * VitaJPOverlay_OCR than this one): a test sets vjo_test_another. */
static int vjo_test_another, vjo_test_another_calls;
static inline int vjo_test_another_copy(void) { vjo_test_another_calls++; return vjo_test_another; }
#endif
