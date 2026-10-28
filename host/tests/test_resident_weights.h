/* Synthetic "release files" for test_game_resident.c: small sizes, real names. */
#ifndef VJO_TEST_RESIDENT_WEIGHTS_H
#define VJO_TEST_RESIDENT_WEIGHTS_H
#include <stdint.h>
#define TEST_FL10_BYTES 100000u
#define TEST_FL10_SEED 0x1234u
/* test_game_hooks.c's FL10 file (test_hook_models.h): the same bytes, at VOCR6-S's size (VJO_VOCR6_S_BYTES) */
#define TEST_HOOK_FL10_BYTES 819792u
#define TEST_H15_BYTES 180000u
#define TEST_H15_SEED 0xBEEFu
static inline unsigned char test_weight_byte(uint32_t i, uint32_t seed)
{
    return (unsigned char)((i * 2654435761u + seed) >> 24);
}
#endif
