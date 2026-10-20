/* psp2/io/fcntl.h as game_ocr/hooks.c uses it (its host test, host/tests/test_hooks_engine.c): the open
 * flags of the debug build's hooks.log and sceIoWrite, beside game_stubs' sceIoOpen/Read/Close. */
#ifndef VJO_TEST_HOOKS_FCNTL_H
#define VJO_TEST_HOOKS_FCNTL_H
#include <psp2/host_stubs.h>

#define SCE_O_WRONLY 0x0002
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400

int sceIoWrite(SceUID fd, const void *data, SceSize size);

#endif
