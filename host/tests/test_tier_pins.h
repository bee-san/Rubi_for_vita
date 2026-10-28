/* Host tests: the model sizes' pins as their release publishes them, the one place a C test
 * states them (tools/tests/test_vocr_defaults.py TIER_PINS is the same table for the Python
 * tests; a pin update edits both). Per size, smallest first: its file, exact bytes, W8I arena,
 * generation and the label the Rubi app shows. The three are VOCR6-S, VOCR6-N and VOCR6-L (vita-vn-ocr #125,
 * docs/vocr6.md "The follow-up", pre-release v0.4.1-vocr6.2): Small's blob plain, Normal's with #120's head gate
 * (ε = 1e-4) and #122's two-level head, Large's with the two-level head alone; the arena with ocr_head_gate's
 * default, on (each blob's own paths). A size whose new candidate did not beat its incumbent keeps that model
 * (the test builds v0.9.8-test.4 and test.5 had VOCR5-S beside VOCR6's Normal and Large). The C pins themselves
 * (include/vjo_vocr_sizes.h, core/vocr_ocr.c, core/vocr_models.c, kernel/game_ocr.c) are checked
 * against this table. */
#ifndef VJO_TEST_TIER_PINS_H
#define VJO_TEST_TIER_PINS_H

typedef struct {
    const char *name;
    unsigned bytes, arena;
    const char *generation, *label;
} TestTierPin;

static const TestTierPin test_tier_pins[3] = {
    {"VOCR6.2_S_w8.vocr", 819792u, 124832u, "VOCR6", "VOCR6-S"},
    {"VOCR6.2_N_w8g2.vocr", 1189408u, 184448u, "VOCR6", "VOCR6-N"},
    {"VOCR6.2_L_w8_2l.vocr", 2475680u, 262480u, "VOCR6", "VOCR6-L"},
};

#endif
