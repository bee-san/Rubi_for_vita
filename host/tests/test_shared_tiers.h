/* Host tests only (VJO_VOCR_TIERS_TEST, include/vjo_vocr_sizes.h): a package whose
 * Normal and Large sizes have one model, VOCR4-W20, as vita-vn-ocr's VOCR5 protocol
 * allows (eval/vocr5_protocol.json, amendment 3: Normal is the better of VOCR5-N and
 * VOCR4-W20, and Large keeps VOCR4-W20 unless VOCR5-L beats it), and whose Small is
 * VOCR3-FL10. Every model here is one this build pins (core/vocr_ocr.c,
 * kernel/game_ocr.c), whatever its own sizes are. */
#define VJO_VOCR_TIER1_NAME VJO_VOCR3_FL10_NAME
#define VJO_VOCR_TIER1_BYTES VJO_VOCR_FL10_BYTES
#define VJO_VOCR_TIER2_NAME VJO_VOCR4_W20_NAME
#define VJO_VOCR_TIER2_BYTES VJO_VOCR_F20_BYTES
#define VJO_VOCR_TIER3_NAME VJO_VOCR4_W20_NAME
#define VJO_VOCR_TIER3_BYTES VJO_VOCR_F20_BYTES
