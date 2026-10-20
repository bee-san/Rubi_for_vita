/* psp2/kernel/clib.h as game_ocr/hooks.c uses it (its host test, host/tests/test_hooks_engine.c):
 * game_stubs' sceClib* and sceClibStrnlen. */
#ifndef VJO_TEST_HOOKS_CLIB_H
#define VJO_TEST_HOOKS_CLIB_H
#include <psp2/host_stubs.h>

#define sceClibStrnlen strnlen

#endif
