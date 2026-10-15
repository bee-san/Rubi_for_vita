/* Release-model lengths shared by game validation and the bounded kernel reader. */
#ifndef VJO_VOCR_SIZES_H
#define VJO_VOCR_SIZES_H
#define VJO_VOCR_FL10_BYTES 657664u
#define VJO_VOCR_H15_BYTES 1155968u
#define VJO_VOCR_F20_BYTES 1558016u
/* VOCR3 (bee-san/vita-vn-ocr #67, release v0.2.5-vocr3.1): recognizers retrained in
 * the shapes of H15 and FL10, so the same sizes and arenas; the default and its
 * fallback. The v0.2.0 H15_w8.vocr and FL10_w8.vocr stay accepted. Each model's
 * generation and shape: core/vocr_models.c. The VOCR2 trial (VOCR2_FL10_w8.vocr,
 * v0.7.12 to v0.8.0-test.1) was removed; ocr_backend = vocr2 loads as vocr. */
#define VJO_VOCR3_H15_NAME "VOCR3_H15_w8.vocr"
#define VJO_VOCR3_FL10_NAME "VOCR3_FL10_w8.vocr"
#define VJO_VOCR_MODEL_ROOT "ux0:data/VitaJPOverlay/ocr"
/* The character LMs of the beam + LM decoder (bee-san/vita-vn-ocr #82, pre-release
 * v0.2.6-ctclm.2, CC BY-SA 4.0), tiers 1-4, in VJO_VOCR_MODEL_ROOT; the largest
 * is the most the game worker reads. A request names the largest tier it allows
 * (VjoGameOcrRequest.flags); the worker opens one at most. */
#define VJO_VOCR_LM_TIERS 4u
#define VJO_VOCR_LM1_NAME "vocr_lm_o3_500k.vlm"
#define VJO_VOCR_LM1_BYTES 499824u
#define VJO_VOCR_LM2_NAME "vocr_lm_o3_1000k.vlm"
#define VJO_VOCR_LM2_BYTES 987600u
#define VJO_VOCR_LM3_NAME "vocr_lm_o3_2000k.vlm"
#define VJO_VOCR_LM3_BYTES 1971568u
#define VJO_VOCR_LM4_NAME "vocr_lm_o3_4000k.vlm"
#define VJO_VOCR_LM4_BYTES 3781072u
#define VJO_VOCR_LM_MAX_BYTES VJO_VOCR_LM4_BYTES
/* The confidence cascade's larger recognizer (bee-san/vita-vn-ocr pre-release
 * v0.2.5-vocr4.1, VOCR4-W20: F20's shape, so F20's bytes), in VJO_VOCR_MODEL_ROOT.
 * A request whose flags give a cascade ceiling may open it (VJO_GAME_OCR_OPEN_CASCADE). */
#define VJO_VOCR4_W20_NAME "VOCR4_W20_w8.vocr"
#define VJO_VOCR_CASCADE_NAME VJO_VOCR4_W20_NAME
#define VJO_VOCR_CASCADE_BYTES VJO_VOCR_F20_BYTES
#endif
