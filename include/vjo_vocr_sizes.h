/* Release-model lengths shared by game validation and the bounded kernel reader. */
#ifndef VJO_VOCR_SIZES_H
#define VJO_VOCR_SIZES_H
#define VJO_VOCR_FL10_BYTES 657664u
#define VJO_VOCR_H15_BYTES 1155968u
#define VJO_VOCR_F20_BYTES 1558016u
/* VOCR3 (bee-san/vita-vn-ocr #67, release v0.2.5-vocr3.1): recognizers retrained in
 * the shapes of H15 and FL10, so the same sizes and arenas; the Normal and Small
 * tiers before VOCR5. They and v0.2.0's H15_w8.vocr and FL10_w8.vocr stay accepted
 * by name (host tools; no tier names them, no package ships them). Each model's
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
/* VOCR4-W20 (bee-san/vita-vn-ocr pre-release v0.2.5-vocr4.1, F20's shape, so F20's
 * bytes), in VJO_VOCR_MODEL_ROOT: the Large tier before VOCR5, and the confidence
 * cascade's larger recognizer of a request by name (the host tools; a request by
 * tiers reads again with the next tier up). Such a request whose flags give a
 * cascade ceiling may open it (VJO_GAME_OCR_OPEN_CASCADE). */
#define VJO_VOCR4_W20_NAME "VOCR4_W20_w8.vocr"
#define VJO_VOCR_CASCADE_NAME VJO_VOCR4_W20_NAME
#define VJO_VOCR_CASCADE_BYTES VJO_VOCR_F20_BYTES
/* VOCR5 (bee-san/vita-vn-ocr eval/vocr5_protocol.json, release v0.3.0): the three
 * model tiers' recognizers, each in its own shape, so each its own size (exact
 * train/export.py sizes): Small in FL10's topology with 4,987 classes, Normal in
 * H15's with 5,631, Large late-wide (channels 64/128/256/480/576) with 5,631. */
#define VJO_VOCR5_S_NAME "VOCR5_S_w8.vocr"
#define VJO_VOCR5_S_BYTES 819792u
#define VJO_VOCR5_N_NAME "VOCR5_N_w8.vocr"
#define VJO_VOCR5_N_BYTES 1185632u
#define VJO_VOCR5_L_NAME "VOCR5_L_w8.vocr"
#define VJO_VOCR5_L_BYTES 2473696u
/* API 21: the three model tiers the Overlay app offers (config.ini vocr_tier),
 * smallest first; tools/model_tiers.json is the same table (its "tiers", in
 * this order), and tools/tests/test_vocr_defaults.py checks the two agree. A
 * tier is a pinned blob: this package's Small is VOCR5-S, its Normal (the
 * default) VOCR5-N and its Large VOCR5-L. A job runs the largest tier the
 * request allows that fits the game's free memory beside that tier's reserve
 * (include/vjo_ledger_budget.h VJO_GAME_RESERVE_*), else the next smaller one;
 * the confidence cascade reads doubtful lines again with the next tier up, when
 * that fits too. A later generation's three blobs (or an earlier one's, for a
 * tier whose new candidate did not beat it) replace these through the pins
 * alone: these six lines, the model's pins where every model is pinned
 * (core/vocr_ocr.c, core/vocr_models.c, kernel/game_ocr.c, tools/prepare_ocr.py)
 * and tools/model_tiers.json. */
#define VJO_VOCR_TIERS 3u
#define VJO_VOCR_TIER_SMALL 1u
#define VJO_VOCR_TIER_NORMAL 2u
#define VJO_VOCR_TIER_LARGE 3u
#define VJO_VOCR_TIER_DEFAULT VJO_VOCR_TIER_NORMAL
#define VJO_VOCR_TIERS_ALL 0x7u /* VjoGameOcrRequest.tiers: bit t - 1 for tier t */
#ifdef VJO_VOCR_TIERS_TEST
/* Host tests only (host/tests/test_shared_tiers.h): another package's tier map,
 * two sizes with one model. */
#include VJO_VOCR_TIERS_TEST
#else
#define VJO_VOCR_TIER1_NAME VJO_VOCR5_S_NAME
#define VJO_VOCR_TIER1_BYTES VJO_VOCR5_S_BYTES
#define VJO_VOCR_TIER2_NAME VJO_VOCR5_N_NAME
#define VJO_VOCR_TIER2_BYTES VJO_VOCR5_N_BYTES
#define VJO_VOCR_TIER3_NAME VJO_VOCR5_L_NAME
#define VJO_VOCR_TIER3_BYTES VJO_VOCR5_L_BYTES
#endif
/* The largest recognizer this build pins (the Large tier's): the most the game
 * worker's weights block and its cascade block take (game_ocr/main.c weights_alloc,
 * cascade_alloc; include/vjo_ledger_budget.h VJO_BUDGET_GAME_WEIGHTS and
 * VJO_BUDGET_GAME_CASCADE hold it page-rounded). Every pinned model is at most
 * this (core/vocr_ocr.c checks the macros it pins at compile time;
 * host/tests/test_ledger.c every entry of its table). */
#define VJO_VOCR_MAX_BYTES VJO_VOCR_TIER3_BYTES
#endif
