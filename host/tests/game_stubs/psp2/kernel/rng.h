/* Host stand-in for VitaSDK's psp2/kernel/rng.h (the test defines the function). */
#ifndef VJO_GAME_TEST_RNG_H
#define VJO_GAME_TEST_RNG_H
#include <psp2/host_stubs.h>
int sceKernelGetRandomNumber(void *output, SceSize size);
#endif
