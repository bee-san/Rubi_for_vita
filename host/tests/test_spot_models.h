/* core/vocr_ocr.c's model table for the spotter's host tests (test_vocr_spot.c, test_game_spot.c) only: FL10's
 * name, test_resident_weights.h's synthetic bytes and hash (as test_resident_models.h), but a recognizer arena of
 * 4 KiB, under the spotter's, so that a job with the spotter has a larger scratch than one without (the workspace's
 * growth, and a job without room for it, which reads with GRAD). */
#include "test_resident_weights.h"
static const VjoVocrModel models[] = {
    {"FL10_w8.vocr", TEST_FL10_BYTES,
     {0x1e,0x9a,0xc4,0xb6,0xa5,0x55,0xe6,0xea,0xb6,0x9a,0x98,0xfe,0x98,0xf4,0x83,0xed,
      0xd7,0xe5,0xb6,0x4e,0x9d,0x57,0x8f,0x9b,0xd3,0x26,0x82,0x53,0x31,0x4b,0x20,0x9b},
     4096, VOCR_LINES_GRAD},
};
