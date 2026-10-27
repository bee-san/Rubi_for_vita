/* Host tests: the model sizes' pins as their release publishes them, the one place a C test
 * states them (tools/tests/test_vocr_defaults.py TIER_PINS is the same table for the Python
 * tests; a pin update edits both). Per size, smallest first: its file, exact bytes, W8I arena,
 * generation and the label the Rubi app shows. Small is VOCR5-S (vita-vn-ocr
 * eval/vocr5_protocol.json, release v0.3.0) with its head gate (API 42, vita-vn-ocr #117,
 * v0.3.6-head2level.1's copy of v0.3.5-headgate.1's file); Normal and Large are VOCR6-N and VOCR6-L
 * (vita-vn-ocr #125, docs/vocr6.md, v0.4.0-vocr6.1) with #122's two-level head and no gate: the arena with
 * ocr_head_gate's default, on (each blob's own paths). A size whose new candidate did not beat its incumbent
 * keeps that model (VOCR5-S beside VOCR6). The C pins themselves
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
    {"VOCR5_S_w8g.vocr", 821584u, 124880u, "VOCR5", "VOCR5-S"},
    {"VOCR6_N_w8_2l.vocr", 1187616u, 184400u, "VOCR6", "VOCR6-N"},
    {"VOCR6_L_w8_2l.vocr", 2475680u, 262480u, "VOCR6", "VOCR6-L"},
};

#endif
