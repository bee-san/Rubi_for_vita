/* The owner scenario's OCR job (#528, docs/owner-scenario.md): the owner's config.ini OCR lines (issue #528's
 * collection, current-device/rubi-config.ini) and the request the Shell sends for a job with them, packed as
 * shell/worker.c vocr_run_one packs it. host/tests/test_alloc_sim.c runs it through the game worker at the logged free
 * sizes; host/tests/test_shell_worker.c holds it against the Shell's own request (owner scenario request), so the two
 * cannot drift apart. Include after core/config.h and include/vjo_game_ocr.h. */
#ifndef VJO_OWNER_REQUEST_H
#define VJO_OWNER_REQUEST_H

#include <string.h>

static const char owner_config[] =
    "vocr_tier = large\nocr_lm = 1000k\nocr_cascade_ms = 2000\nocr_refine_lines = psp\nocr_threads = 3\n"
    "ocr_boost_clock = on\nocr_settle_ms = 150\nocr_settle_max_ms = 2000\nocr_skip_blank_lines = off\n"
    "ocr_text_gate = off\nnon_japanese_filter = lines\ntext_source = auto\nocr_mode = auto\n";

/* A user's job (not a self-test stage) of config c: tiers up to c's, flags F_RECT (API 58: a one-off scan; F_FULL
 * before) or 0, the screen settled (no F_SETTLE_FIRST: the kernel saw it settle), as the Shell's vocr_run_one packs it. */
static void owner_request(VjoGameOcrRequest *q, const VjoConfig *c, uint32_t flags, const char *model)
{
    int full = (flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_RECT)) != 0;
    memset(q, 0, sizeof(*q));
    q->size = sizeof(*q);
    q->tiers = (1u << c->vocr_tier) - 1u;
    q->flags = flags;
    if (c->ocr_skip_blank_lines) q->flags |= VJO_GAME_OCR_F_BLANK;
    q->flags |= (uint32_t)(c->ocr_lm & VJO_GAME_OCR_LM_MASK) << VJO_GAME_OCR_LM_SHIFT |
                (uint32_t)(c->ocr_refine_lines & VJO_GAME_OCR_REFINE_MASK) << VJO_GAME_OCR_REFINE_SHIFT;
    if (c->ocr_cascade_ms > 0) { /* in 250 ms steps, at least one */
        uint32_t casc = (uint32_t)(c->ocr_cascade_ms + 125) / 250u;
        q->flags |= (casc ? casc > VJO_GAME_OCR_CASCADE_MASK ? VJO_GAME_OCR_CASCADE_MASK : casc : 1u)
                    << VJO_GAME_OCR_CASCADE_SHIFT;
    }
    if (c->ocr_settle_ms > 0) {
        uint32_t settle = (uint32_t)(c->ocr_settle_ms + 5) / 10u;
        q->flags |= (settle ? settle : 1u) << VJO_GAME_OCR_SETTLE_SHIFT |
                    (uint32_t)(c->ocr_settle_max_ms + 25) / 50u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    }
    q->speed = (uint32_t)c->ocr_helpers | (uint32_t)c->ocr_boost_clock << 2 | (uint32_t)c->hook_trust << 20;
    if (c->ocr_text_gate) q->speed |= VJO_GAME_OCR_S_GATE;
    q->speed |= (uint32_t)c->ocr_head_gate << VJO_GAME_OCR_S_HEAD_SHIFT;
    if (c->ocr_finder || full)
        q->speed |= VJO_GAME_OCR_S_SPOT | (c->ocr_spotter_refine ? VJO_GAME_OCR_S_SPOT_REFINE : 0u);
    if (c->non_japanese_filter) q->speed |= VJO_GAME_OCR_S_DROP_BORDERS;
    q->speed |= (uint32_t)c->ocr_vertical << VJO_GAME_OCR_S_VERTICAL_SHIFT |
                (uint32_t)c->ocr_pause << VJO_GAME_OCR_S_PAUSE_SHIFT;
    if (c->ocr_line_fix)
        q->speed |= VJO_GAME_OCR_S_LINE_FIX | (c->ocr_line_fix >= 2 ? VJO_GAME_OCR_S_LINE_JUNK : 0u) |
                    (c->ocr_line_fix == 3 ? VJO_GAME_OCR_S_LINE_VALLEY : 0u);
    /* a user's read picks its text automatically; a diagnostic job (flags: the full screen) forces OCR */
    q->hook_pick = flags ? VJO_HOOK_PICK_OCR : VJO_HOOK_PICK_AUTO;
    strcpy(q->model_dir, VJO_VOCR_MODEL_ROOT);
    strcpy(q->model, model); /* the largest tier's model names the job */
}

#endif
