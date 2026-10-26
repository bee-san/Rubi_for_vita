/* Release metadata of the local OCR models this build runs, and its model tiers (vocr_models.h). */
#include "vocr_models.h"
#include "../include/vjo_vocr_sizes.h"
#include "../include/vjo_ledger_budget.h"

#include <string.h>

#include "port.h"

/* Estimates by shape (vocr_models.h): ms per 3-line textbox, calibrated
 * (vita-vn-ocr docs/ctc-lm-decode.md, main 57b6622, "Per typical full textbox ...
 * calibrated", greedy box, high bound: FL10 0.68-0.74 s, H15 1.20-1.25 s, W20
 * 1.71-1.72 s), and the job bytes of the game worker's check at 960x544: page-
 * rounded weights + workspace + capture block (host/tests/test_vocr.c). */
#define FL10_BOX_MS 740u
#define H15_BOX_MS 1250u
#define W20_BOX_MS 1720u
#define FL10_JOB_BYTES 856064u  /* 659,456 + 131,072 + 65,536 */
#define H15_JOB_BYTES 1413120u  /* 1,159,168 + 188,416 + 65,536 */
#define W20_JOB_BYTES 1871872u  /* 1,560,576 + 245,760 + 65,536 (F20's shape) */
/* VOCR5's three shapes (vita-vn-ocr eval/vocr5_protocol.json "estimates": one core,
 * vita-vn-ocr #85's runtime without credit for its prefetch, 333 MHz, a box of
 * 240 frames + 0.1 s for the finder, calibrated to VOCR3-H15's measured console
 * rate: 631 / 921 / 2,077 ms), with the head gate on (API 42, ocr_head_gate's default;
 * vita-vn-ocr docs/headgate.md: its estimated saving per such box, -122 / -137 / -163 ms,
 * by the same method) subtracted, and for Normal and Large the two-level head on too
 * (ocr_head_gate = on, the default, has both; vita-vn-ocr docs/head2level.md: its saving beyond the
 * gate's, at the development share of frames that escape, -62 / -94 ms) subtracted.
 * Estimates, not console measurements. */
#define VOCR5_S_BOX_MS 509u
#define VOCR5_N_BOX_MS 722u
#define VOCR5_L_BOX_MS 1820u
#define VOCR5_S_JOB_BYTES 1019904u  /* 823,296 + 131,072 + 65,536 */
#define VOCR5_N_JOB_BYTES 1445888u  /* 1,191,936 + 188,416 + 65,536 (the w8g2 blob: one weights page more) */
#define VOCR5_L_JOB_BYTES 2813952u  /* 2,478,080 + 270,336 + 65,536 (and its arena: one workspace page more) */
/* VOCR5-V (vita-vn-ocr #99), the vertical model: VOCR5-S's shape without its head gate (its blob has none), so
 * v0.3.0's VOCR5-S estimate for the same CTC frames, 631 ms; its workspace adds the rotation buffer
 * (core/vocr_ocr.h VJO_VOCR_ROTATE_BYTES, 57,344 B). No model size runs it, so the Rubi app shows neither figure. */
#define VOCR5_V_BOX_MS 631u
#define VOCR5_V_JOB_BYTES 1077248u  /* 823,296 + 188,416 + 65,536 */

#ifdef VJO_VOCR_MODEL_INFO_TEST
/* Host tests only: another package's tables (a later generation's tiers). */
#include VJO_VOCR_MODEL_INFO_TEST
#else
static const VjoVocrModelInfo infos[] = {
    /* v0.3.0 (vita-vn-ocr eval/vocr5_protocol.json): the Small, Normal and Large tiers, with their head gate
     * (v0.3.5-headgate.1), Normal and Large with their two-level head too (v0.3.6-head2level.1; API 42). */
    {VJO_VOCR5_S_NAME, "VOCR5", "S", VJO_VOCR5_S_BYTES, VOCR5_S_BOX_MS, VOCR5_S_JOB_BYTES},
    {VJO_VOCR5_N_NAME, "VOCR5", "N", VJO_VOCR5_N_BYTES, VOCR5_N_BOX_MS, VOCR5_N_JOB_BYTES},
    {VJO_VOCR5_L_NAME, "VOCR5", "L", VJO_VOCR5_L_BYTES, VOCR5_L_BOX_MS, VOCR5_L_JOB_BYTES},
    /* v0.2.5-vocr3.1 (vita-vn-ocr #67): the Normal and Small tiers before VOCR5. */
    {VJO_VOCR3_H15_NAME, "VOCR3", "H15", VJO_VOCR_H15_BYTES, H15_BOX_MS, H15_JOB_BYTES},
    {VJO_VOCR3_FL10_NAME, "VOCR3", "FL10", VJO_VOCR_FL10_BYTES, FL10_BOX_MS, FL10_JOB_BYTES},
    /* v0.2.0, the first released generation (vita-vn-ocr calls its FL10 "VOCR1"). */
    {"H15_w8.vocr", "VOCR1", "H15", VJO_VOCR_H15_BYTES, H15_BOX_MS, H15_JOB_BYTES},
    {"FL10_w8.vocr", "VOCR1", "FL10", VJO_VOCR_FL10_BYTES, FL10_BOX_MS, FL10_JOB_BYTES},
    {"F20_w8.vocr", "VOCR1", "F20", VJO_VOCR_F20_BYTES, W20_BOX_MS, W20_JOB_BYTES},
    /* v0.2.5-vocr4.1 (vita-vn-ocr #78): the Large tier before VOCR5, the confidence
     * cascade's model of a request by name (the host tools). */
    {VJO_VOCR4_W20_NAME, "VOCR4", "W20", VJO_VOCR_CASCADE_BYTES, W20_BOX_MS, W20_JOB_BYTES},
    /* vita-vn-ocr #99: the vertical model (config.ini ocr_vertical), no model size's. */
    {VJO_VOCR5_V_NAME, "VOCR5", "V", VJO_VOCR5_V_BYTES, VOCR5_V_BOX_MS, VOCR5_V_JOB_BYTES},
};
/* The Rubi app's Model size choices, smallest first (include/vjo_vocr_sizes.h,
 * tools/model_tiers.json). */
static const VjoVocrTierInfo tiers[VJO_VOCR_TIERS] = {
    {"small", "Small", "fastest", VJO_VOCR_TIER1_NAME, VJO_GAME_RESERVE_SMALL},
    {"normal", "Normal", "default, balanced", VJO_VOCR_TIER2_NAME, VJO_GAME_RESERVE_NORMAL},
    {"large", "Large", "most accurate", VJO_VOCR_TIER3_NAME, VJO_GAME_RESERVE_LARGE},
};
#endif
#define N_INFOS (sizeof(infos) / sizeof(infos[0]))
typedef char vjo_one_entry_per_tier[sizeof(tiers) / sizeof(tiers[0]) == VJO_VOCR_TIERS ? 1 : -1];

const VjoVocrModelInfo *vjo_vocr_model_infos(unsigned *count)
{
    if (count) *count = (unsigned)N_INFOS;
    return infos;
}

const VjoVocrModelInfo *vjo_vocr_model_info(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < N_INFOS; i++)
        if (!strcmp(name, infos[i].name)) return &infos[i];
    return NULL;
}

int vjo_vocr_model_label(const VjoVocrModelInfo *info, char *out, size_t cap)
{
    int n;
    if (!out || !cap) return -1;
    out[0] = '\0';
    if (!info) return -1;
    n = vjo_snprintf(out, cap, "%s-%s", info->generation, info->shape);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

const VjoVocrTierInfo *vjo_vocr_tier_info(unsigned tier)
{
    return tier >= 1 && tier <= VJO_VOCR_TIERS ? &tiers[tier - 1] : NULL;
}

uint32_t vjo_vocr_tier_need(unsigned tier)
{
    const VjoVocrTierInfo *t = vjo_vocr_tier_info(tier);
    const VjoVocrModelInfo *m = t ? vjo_vocr_model_info(t->model) : NULL;
    return m && m->job_bytes ? m->job_bytes + t->reserve : 0;
}
