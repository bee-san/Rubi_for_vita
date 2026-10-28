/* core/vocr_ocr.c's model table for test_game_hooks.c only: the resident test's synthetic "FL10" release file (its
 * name, arena and bytes, test_weight_byte(i, TEST_FL10_SEED)), at VOCR6-S's size (TEST_HOOK_FL10_BYTES), so a
 * job's memory windows are a real Small job's: its weights block (823,296 B) and workspace come beside the OCR
 * reserve, and a game with room for the hook-only pass (VJO_GAME_RESERVE_HOOK_ONLY and a strip) has none for OCR,
 * as on a console, whatever the OCR reserve (with the resident test's 100,000 B file, a 512 KiB OCR reserve left no
 * such game). Its SHA-256 is that of those bytes (python3: hashlib.sha256(bytes(((i * 2654435761 + 0x1234) &
 * 0xFFFFFFFF) >> 24 for i in range(819792)))). */
#include "test_resident_weights.h"
static const VjoVocrModel models[] = {
    {"FL10_w8.vocr", TEST_HOOK_FL10_BYTES,
     {0x20,0x77,0xfd,0x63,0xfb,0x0e,0x08,0x7b,0x98,0xc1,0x4e,0xd3,0x0e,0x2e,0xd5,0xa6,
      0x89,0xf1,0x88,0xd4,0x25,0xb9,0x0e,0xb5,0x01,0xd4,0x6e,0x26,0xcf,0xe2,0xc2,0xa6},
     124832, VOCR_LINES_GRAD},
};
