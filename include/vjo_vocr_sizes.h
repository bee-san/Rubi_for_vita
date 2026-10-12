/* Release-model lengths shared by game validation and the bounded kernel reader. */
#ifndef VJO_VOCR_SIZES_H
#define VJO_VOCR_SIZES_H
#define VJO_VOCR_FL10_BYTES 657664u
#define VJO_VOCR_H15_BYTES 1155968u
#define VJO_VOCR_F20_BYTES 1558016u
/* Experimental VOCR2: separately trained FL10-sized model and textbox finder.
 * Opt-in only: its strict per-title promotion guard did not pass. */
#define VJO_VOCR2_ENABLED 1
#define VJO_VOCR2_ARTIFACT_NAME "VOCR2_FL10_w8.vocr"
#define VJO_VOCR2_BYTES VJO_VOCR_FL10_BYTES
#define VJO_VOCR2_MODEL_NAME VJO_VOCR2_ARTIFACT_NAME
/* Manual PP-OCRv6 small engine: int8 ncnn weights, head pruned to JIS X 0208
 * (tools/prepare_ppocrv6.py). The param is built into the game worker. */
#define VJO_PPOCRV6_MODEL_NAME "PP-OCRv6_small_jis_int8.bin"
#define VJO_PPOCRV6_MODEL_BYTES 3893668u
#define VJO_VOCR_MODEL_ROOT "ux0:data/VitaJPOverlay/ocr"
#endif
