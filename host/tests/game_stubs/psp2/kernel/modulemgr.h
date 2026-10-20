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
/* API 31: the kernel's text source as module_start reads it (vjoGetTextSource): a test sets
 * vjo_test_text_source (VJO_TEXT_SOURCE_*, or a value this build does not know). */
static int vjo_test_text_source, vjo_test_text_source_calls;
static inline int vjo_test_get_text_source(void) { vjo_test_text_source_calls++; return vjo_test_text_source; }
/* What module_start passes to vjo_hooks_start (game_ocr/main.c's host macro): the text source word
 * (VJO_TEXT_SOURCE_* and its NO_SITES flag) and the calls. */
static unsigned vjo_test_hooks_word, vjo_test_hooks_starts;
static inline void vjo_test_hooks_start(const char *title, unsigned word, int psp)
{
    (void)title;
    (void)psp;
    vjo_test_hooks_word = word;
    vjo_test_hooks_starts++;
}
#endif
