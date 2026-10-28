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
/* VOCR6 (bee-san/vita-vn-ocr #125, docs/vocr6.md "The follow-up: stages 4c and 4d", eval/vocr6_protocol.json
 * stage 4 amendments 6 and 7; pre-release v0.4.1-vocr6.2, from vita-vn-ocr #157's merge c933e15): the three tiers'
 * recognizers, VOCR5-S's, VOCR5-N's and VOCR5-L's shapes (FL10's topology with 4,987 classes; H15's, and late-wide
 * with channels 64/128/256/480/576, with 5,631 each) fine-tuned again on the owner's hooked PS Vita and PSP games:
 * Small at half the learning rate (stage 4d), Normal and Large for 45,000 steps (stage 4c). Each blob carries the
 * speed sections vita-vn-ocr's rules (#120, #122) kept for it, and a job runs those its ocr_head_gate asks for
 * (core/vocr_ocr.c run_opts): Small's blob is plain (the gate rule kept no gate for it, so ocr_head_gate does
 * nothing for Small), Normal's has a 1,792 B VOCG head gate (ε = 1e-4, VjoVocrPolicy.head_gate) and #122's two-level
 * head (docs/head2level.md: its output rows and vocab in frequency order, then a 1,984 B VOC2 trailer,
 * VjoVocrPolicy.two_level), Large's the two-level head alone. The release names carry the set's ".2"
 * (VOCR6.2_*): v0.4.0-vocr6.1's VOCR6_N_w8_2l.vocr and VOCR6_L_w8_2l.vocr, the Normal and Large tiers before
 * (and VOCR5_S_w8g.vocr, Small's), keep theirs and are not pinned any more, so one file name is one set of bytes.
 * Each blob's own sections drive these paths: a later set's blobs bring theirs, and only these pins change. */
#define VJO_VOCR6_S_NAME "VOCR6.2_S_w8.vocr"
#define VJO_VOCR6_S_BYTES 819792u
#define VJO_VOCR6_N_NAME "VOCR6.2_N_w8g2.vocr"
#define VJO_VOCR6_N_BYTES 1189408u
#define VJO_VOCR6_L_NAME "VOCR6.2_L_w8_2l.vocr"
#define VJO_VOCR6_L_BYTES 2475680u
/* The runtime's two structs a job's workspace holds beside its arena (vocr_model and vocr_ctx, each 16-byte
 * aligned: core/vocr_ocr.c's layout) as the console's compiler lays them out at the pinned runtime: 3,920 B at
 * vita-vn-ocr f8a1856, whose vocr.h has #130's attention op (1,872 + 2,048 B; 3,872 B at 47cf3a3, 3,760 B at
 * 817d203; measured: arm-vita-eabi-gcc, sizeof). The Vita build checks it
 * (core/vocr_ocr.c), so a runtime pin that changes the structs fails until this is updated; the host tests
 * reckon the console's job with it (host/tests/test_vocr.c), as a 64-bit host's structs are larger. */
#define VJO_VOCR_VITA_STRUCTS 3920u
/* VOCR6-V (bee-san/vita-vn-ocr #159, pre-release v0.4.2-vertical6.1): vertical text, VOCR5-V's shape (VOCR5-S's:
 * 4,987 classes), trained on columns turned 90 degrees anticlockwise; VOCR5-V (#99, v0.3.2-vertical.1) fine-tuned
 * again on the owner's hooked vertical game (Yunohana SpRING!) and new synthetic columns. It is no model size: a job
 * runs it, whatever the size, where config.ini ocr_vertical has a job read columns (VjoGameOcrRequest.speed
 * VJO_GAME_OCR_S_VERTICAL), and opens it as VJO_GAME_OCR_OPEN_VERTICAL. Pinned where every model is
 * (core/vocr_ocr.c, core/vocr_models.c, kernel/game_ocr.c, tools/prepare_ocr.py). */
#define VJO_VOCR6_V_NAME "VOCR6_V_w8.vocr"
#define VJO_VOCR6_V_BYTES 819792u
#define VJO_VOCR_VERTICAL_NAME VJO_VOCR6_V_NAME
#define VJO_VOCR_VERTICAL_BYTES VJO_VOCR6_V_BYTES
/* API 21: the three model tiers the Rubi app offers (config.ini vocr_tier),
 * smallest first; tools/model_tiers.json is the same table (its "tiers", in
 * this order), and tools/tests/test_vocr_defaults.py checks the two agree. A
 * tier is a pinned blob: this package's Small, Normal (the default) and Large
 * are VOCR6-S, VOCR6-N and VOCR6-L (v0.4.1-vocr6.2). A job runs the largest tier the
 * request allows that fits the game's free memory beside that tier's reserve
 * (include/vjo_ledger_budget.h VJO_GAME_RESERVE_*), else the next smaller one;
 * the confidence cascade reads doubtful lines again with the next tier up, when
 * that fits too. The tiers may come from different generations (the test builds v0.9.8-test.4
 * and test.5 had Small VOCR5-S beside VOCR6's Normal and Large): each blob carries its own generation
 * in the pins (core/vocr_models.c, tools/prepare_ocr.py VOCR_GENERATION). A later set's three
 * blobs (or an earlier one's, for a tier whose new candidate did not beat it) replace these
 * through the pins: these six lines, the model's pins where every model is pinned
 * (core/vocr_ocr.c, core/vocr_models.c, kernel/game_ocr.c, tools/prepare_ocr.py),
 * tools/model_tiers.json, and a new release's attribution (tools/prepare_ocr.py
 * VOCR6_ATTRIBUTION_PIN, VOCR_MODEL_ATTRIBUTION_PIN). Two sizes with one model take one edit more: the
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
#define VJO_VOCR_TIER1_NAME VJO_VOCR6_S_NAME
#define VJO_VOCR_TIER1_BYTES VJO_VOCR6_S_BYTES
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

/* API 54: the text spotter's int8 weights (vita-vn-ocr #149's pick, docs/textspot8.md: a VTSP version-4 blob,
 * pre-release v0.3.10-textspot8.1, whose SHA256SUMS lists them), the game worker's default finder (config.ini
 * ocr_finder = spotter; grad turns it off): a file beside the models, read through the
 * kernel's broker (VJO_GAME_OCR_OPEN_SPOT) by a job with VJO_GAME_OCR_S_SPOT, and its release's ATTRIBUTION.md
 * (kept as ocr/SPOTTER-ATTRIBUTION.md). THE ONE PLACE of its pins: core/vocr_ocr.c (the SHA-256), the kernel
 * (name and bytes), include/vjo_payload.h (PUBLISHED), cmake/VocrOCR.cmake (FORM: the VOCR_TEXTSPOT6_NO_MS
 * default; the spotter's runtime pins), tools/prepare_ocr.py, package_release.py, package_vpk.py and e2e_package.py
 * all read these lines, one value each (tools/tests/test_spot_pins.py checks the form of each), so pinning
 * a new vita-vn-ocr pick changes one line per constant. An unpublished entry would be VJO_VOCR_SPOT_PUBLISHED 0
 * and a SHA-256 of zeros, which no file has: a job with the spotter then finds no weights and reads with GRAD, and
 * no package carries the spotter. TAG: the pre-release; NAME, BYTES,
 * SHA256: its weights; ATTRIBUTION_BYTES, _SHA256: its ATTRIBUTION.md; FORM: "trunk" (a VTDT trunk: the build
 * leaves out every multi-scale path, -DVOCR_TEXTSPOT6_NO_MS) or "ms" (a VTMS network). */
#ifdef VJO_VOCR_SPOT_TEST
/* Host tests only (host/tests/test_spot_pins.h): the committed random blob's pins, a job's spotter end to end. */
#include VJO_VOCR_SPOT_TEST
#else
#define VJO_VOCR_SPOT_TAG "v0.3.10-textspot8.1"
#define VJO_VOCR_SPOT_NAME "textspot8_t64n8c16_w8.vtsp"
#define VJO_VOCR_SPOT_BYTES 52176u
#define VJO_VOCR_SPOT_SHA256 "e129d41adfa02c658a7d13d6f98064e6ab6ce9a2bc8c7a56da700a21357b5e1c"
#define VJO_VOCR_SPOT_ATTRIBUTION_BYTES 4183u
#define VJO_VOCR_SPOT_ATTRIBUTION_SHA256 "fd44f500114ca2c8373e02bda1134753619e76de869ddf8ec86c14e008be5b84"
#define VJO_VOCR_SPOT_FORM "trunk"
#define VJO_VOCR_SPOT_PUBLISHED 1
#endif
/* The ceilings every pin of it must stay within (vita-vn-ocr eval/textspot6_protocol.json, the rule's
 * eligibility: a VTSP blob of at most 512,000 B, an arena of at most 262,144 B for a 960 x 544 frame), so the
 * memory ledger's budgets (include/vjo_ledger_budget.h VJO_BUDGET_GAME_SPOT, VJO_BUDGET_GAME_WORKSPACE) hold for
 * any pick the rule admits without a change of their own. core/vocr_ocr.c and game_ocr/main.c refuse more:
 * a job then reads with GRAD. */
#define VJO_VOCR_SPOT_MAX_BYTES 512000u
#define VJO_VOCR_SPOT_ARENA_MAX 262144u

#endif
