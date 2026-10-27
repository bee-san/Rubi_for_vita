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
/* The character LMs of the beam + LM decoder (bee-san/vita-vn-ocr #82, #127, pre-release
 * v0.3.7-lm6.2, CC BY-SA 4.0: tiers 2 and 4 VOCR6's, 1 and 3 ctclm.2's), tiers 1-4, in VJO_VOCR_MODEL_ROOT; the largest
 * is the most the game worker reads. A request names the largest tier it allows
 * (VjoGameOcrRequest.flags); the worker opens one at most. */
#define VJO_VOCR_LM_TIERS 4u
#define VJO_VOCR_LM1_NAME "vocr_lm_o3_500k.vlm"
#define VJO_VOCR_LM1_BYTES 499824u
#define VJO_VOCR_LM2_NAME "vocr_lm6_o3_1000k.vlm"
#define VJO_VOCR_LM2_BYTES 982320u
#define VJO_VOCR_LM3_NAME "vocr_lm_o3_2000k.vlm"
#define VJO_VOCR_LM3_BYTES 1971568u
#define VJO_VOCR_LM4_NAME "vocr_lm6_o3_4000k.vlm"
#define VJO_VOCR_LM4_BYTES 3603552u
#define VJO_VOCR_LM_MAX_BYTES VJO_VOCR_LM4_BYTES
/* VOCR4-W20 (bee-san/vita-vn-ocr pre-release v0.2.5-vocr4.1, F20's shape, so F20's
 * bytes), in VJO_VOCR_MODEL_ROOT: the Large tier before VOCR5, and the confidence
 * cascade's larger recognizer of a request by name (the host tools; a request by
 * tiers reads again with the next tier up). Such a request whose flags give a
 * cascade ceiling may open it (VJO_GAME_OCR_OPEN_CASCADE). */
#define VJO_VOCR4_W20_NAME "VOCR4_W20_w8.vocr"
#define VJO_VOCR_CASCADE_NAME VJO_VOCR4_W20_NAME
#define VJO_VOCR_CASCADE_BYTES VJO_VOCR_F20_BYTES
/* VOCR5-S (bee-san/vita-vn-ocr eval/vocr5_protocol.json, release v0.3.0): the Small tier's
 * recognizer, FL10's topology with 4,987 classes (exact train/export.py size). API 42: with its
 * head gate (vita-vn-ocr #117, docs/headgate.md, pre-release v0.3.5-headgate.1, carried byte for
 * byte by v0.3.6-head2level.1): v0.3.0's bytes unchanged, then a 1,792 B VOCG gate section
 * (VjoVocrPolicy.head_gate). VOCR6-S was not adopted (vita-vn-ocr docs/vocr6.md: other games'
 * development score resolved worse), so Small keeps it. */
#define VJO_VOCR5_S_NAME "VOCR5_S_w8g.vocr"
#define VJO_VOCR5_S_BYTES 821584u
/* VOCR6 (bee-san/vita-vn-ocr #125, docs/vocr6.md, eval/vocr6_protocol.json stage 4; pre-release
 * v0.4.0-vocr6.1): the Normal and Large tiers' recognizers, VOCR5-N's and VOCR5-L's shapes (H15's
 * topology, and late-wide with channels 64/128/256/480/576; 5,631 classes each) fine-tuned again on the
 * owner's hooked PS Vita and PSP games. Each with #122's two-level head (docs/head2level.md: its output
 * rows and vocab in frequency order, then a 1,984 B VOC2 trailer, VjoVocrPolicy.two_level) and no head
 * gate (#120's rule rejected it on VOCR6's logits: vocr6-final-gate, vita-vn-ocr #146), so with
 * ocr_head_gate = on they run the two-level head alone. Each blob's own sections drive these paths: a
 * later generation's blobs bring theirs, and only these pins change. */
#define VJO_VOCR6_N_NAME "VOCR6_N_w8_2l.vocr"
#define VJO_VOCR6_N_BYTES 1187616u
#define VJO_VOCR6_L_NAME "VOCR6_L_w8_2l.vocr"
#define VJO_VOCR6_L_BYTES 2475680u
/* The runtime's two structs a job's workspace holds beside its arena (vocr_model and vocr_ctx, each 16-byte
 * aligned: core/vocr_ocr.c's layout) as the console's compiler lays them out at the pinned runtime: 3,920 B at
 * vita-vn-ocr f8a1856, whose vocr.h has #130's attention op (1,872 + 2,048 B; 3,872 B at 47cf3a3, 3,760 B at
 * 817d203; measured: arm-vita-eabi-gcc, sizeof). The Vita build checks it
 * (core/vocr_ocr.c), so a runtime pin that changes the structs fails until this is updated; the host tests
 * reckon the console's job with it (host/tests/test_vocr.c), as a 64-bit host's structs are larger. */
#define VJO_VOCR_VITA_STRUCTS 3920u
/* VOCR5-V (bee-san/vita-vn-ocr #99): vertical text, VOCR5-S's shape (4,987 classes), trained on
 * columns turned 90 degrees anticlockwise. It is no model size: a job runs it, whatever the size,
 * where config.ini ocr_vertical has a job read columns (VjoGameOcrRequest.speed VJO_GAME_OCR_S_VERTICAL), and
 * opens it as VJO_GAME_OCR_OPEN_VERTICAL. Pinned where every model is (core/vocr_ocr.c,
 * core/vocr_models.c, kernel/game_ocr.c, tools/prepare_ocr.py). */
#define VJO_VOCR5_V_NAME "VOCR5_V_w8.vocr"
#define VJO_VOCR5_V_BYTES 819792u
#define VJO_VOCR_VERTICAL_NAME VJO_VOCR5_V_NAME
#define VJO_VOCR_VERTICAL_BYTES VJO_VOCR5_V_BYTES
/* API 21: the three model tiers the Rubi app offers (config.ini vocr_tier),
 * smallest first; tools/model_tiers.json is the same table (its "tiers", in
 * this order), and tools/tests/test_vocr_defaults.py checks the two agree. A
 * tier is a pinned blob: this package's Small is VOCR5-S, its Normal (the
 * default) VOCR6-N and its Large VOCR6-L. A job runs the largest tier the
 * request allows that fits the game's free memory beside that tier's reserve
 * (include/vjo_ledger_budget.h VJO_GAME_RESERVE_*), else the next smaller one;
 * the confidence cascade reads doubtful lines again with the next tier up, when
 * that fits too. The tiers may come from two generations (Small VOCR5-S, Normal and
 * Large VOCR6): each blob carries its own generation in the pins (core/vocr_models.c,
 * tools/prepare_ocr.py VOCR_GENERATION). A later generation's three blobs (or an earlier one's, for a
 * tier whose new candidate did not beat it) replace these through the pins:
 * these six lines, the model's pins where every model is pinned
 * (core/vocr_ocr.c, core/vocr_models.c, kernel/game_ocr.c, tools/prepare_ocr.py),
 * tools/model_tiers.json, and a new release's attribution (tools/prepare_ocr.py
 * VOCR_MODEL_ATTRIBUTION_PIN). Two sizes with one model take one edit more: the
 * one-VPK payload (include/vjo_payload.h) names each size's file, and the
 * Rubi app refuses a payload that names a file twice, so its list must name
 * the shared file once, with VJO_PAYLOAD_KERNEL_INDEX and test_one_vpk.py's
 * count one less (tools/package_vpk.py refuses the list until it does). */
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
#define VJO_VOCR_TIER2_NAME VJO_VOCR6_N_NAME
#define VJO_VOCR_TIER2_BYTES VJO_VOCR6_N_BYTES
#define VJO_VOCR_TIER3_NAME VJO_VOCR6_L_NAME
#define VJO_VOCR_TIER3_BYTES VJO_VOCR6_L_BYTES
#endif
/* The largest recognizer this build pins (the Large tier's): the most the game
 * worker's weights block and its cascade block take (game_ocr/main.c weights_alloc,
 * cascade_alloc; include/vjo_ledger_budget.h VJO_BUDGET_GAME_WEIGHTS and
 * VJO_BUDGET_GAME_CASCADE hold it page-rounded). Every pinned model is at most
 * this (core/vocr_ocr.c checks the macros it pins at compile time;
 * host/tests/test_ledger.c every entry of its table). */
#define VJO_VOCR_MAX_BYTES VJO_VOCR_TIER3_BYTES
/* API 26: the text-presence gate's int8 weights (vita-vn-ocr #100, pre-release v0.3.2-textgate.1, whose
 * SHA256SUMS lists them; tools/prepare_ocr.py pins the file, core/vocr_ocr.c its SHA-256): a file beside the
 * models, read through the kernel's broker (VJO_GAME_OCR_OPEN_GATE) by a job with VJO_GAME_OCR_S_GATE. */
#define VJO_VOCR_GATE_NAME "textgate_p8_c8_w8.vtd"
#define VJO_VOCR_GATE_BYTES 1540u

#endif
