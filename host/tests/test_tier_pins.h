/* Host tests: the model sizes' pins as their release publishes them, the one place a C test
 * states them (tools/tests/test_vocr_defaults.py TIER_PINS is the same table for the Python
 * tests; a pin update edits both). Per size, smallest first: its file, exact bytes, W8I arena,
 * generation and the label the Overlay app shows. VOCR5's are vita-vn-ocr
 * eval/vocr5_protocol.json's "tiers" (release v0.3.0); a size whose VOCR5 candidate did not beat
 * its incumbent keeps that VOCR4 model (v0.2.5-vocr4.1). The C pins themselves
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
    {"VOCR5_S_w8.vocr", 819792u, 124832u, "VOCR5", "VOCR5-S"},
    {"VOCR5_N_w8.vocr", 1185632u, 182944u, "VOCR5", "VOCR5-N"},
    {"VOCR5_L_w8.vocr", 2473696u, 261024u, "VOCR5", "VOCR5-L"},
};

#endif
