/* Release-model lengths shared by game validation and the bounded kernel reader. */
#ifndef VJO_VOCR_SIZES_H
#define VJO_VOCR_SIZES_H
#define VJO_VOCR_FL10_BYTES 657664u
#define VJO_VOCR_H15_BYTES 1155968u
#define VJO_VOCR_F20_BYTES 1558016u
/* VOCR3 (bee-san/vita-vn-ocr #67, release v0.2.5-vocr3.1): recognizers retrained in
 * the shapes of H15 and FL10, so the same sizes and arenas; the default and its
 * fallback. The v0.2.0 H15_w8.vocr and FL10_w8.vocr stay accepted. */
#define VJO_VOCR3_H15_NAME "VOCR3_H15_w8.vocr"
#define VJO_VOCR3_FL10_NAME "VOCR3_FL10_w8.vocr"
/* Experimental VOCR2: separately trained FL10-sized model and textbox finder.
 * Opt-in only: its strict per-title promotion guard did not pass. */
#define VJO_VOCR2_ENABLED 1
#define VJO_VOCR2_ARTIFACT_NAME "VOCR2_FL10_w8.vocr"
#define VJO_VOCR2_BYTES VJO_VOCR_FL10_BYTES
#define VJO_VOCR2_MODEL_NAME VJO_VOCR2_ARTIFACT_NAME
#define VJO_VOCR_MODEL_ROOT "ux0:data/VitaJPOverlay/ocr"
#endif
