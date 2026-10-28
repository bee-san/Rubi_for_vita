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
/* The three tiers' blobs (include/vjo_vocr_sizes.h, v0.4.1-vocr6.2), each with the paths ocr_head_gate = on (the
 * default) gives it: VOCR6-S plain (its blob has no section), VOCR6-N with its head gate (ε = 1e-4) and two-level
 * head, VOCR6-L with its two-level head. Their times are vocr6-followup's gate fits (vita-vn-ocr #157, docs/vocr6.md
 * "Speed: the follow-up's gate fits"; eval/vocr6_results_20261027.json "follow_up_gate", vita-vn-ocr's
 * eval/vocr6_gate.sh, vocr6-final-gate's driver of #146): vita/a9_layers.py under qemu on the shipped blobs,
 * calibrated to VOCR3-H15's measured console rate, one core at 333 MHz without credit for the weights' prefetch, a
 * nominal box of 3 lines of 80 CTC frames + 0.1 s for the finder, at the development share of frames each path
 * skips: Small 627 ms (plain: VOCR5-S with its gate was 519 ms), Normal 918 -> 774 ms with its gate and two-level
 * head, Large 2,073 -> 1,880 ms with its two-level head. Estimates, not console measurements. */
#define VOCR6_S_BOX_MS 627u
#define VOCR6_N_BOX_MS 774u
#define VOCR6_L_BOX_MS 1880u
#define VOCR6_S_JOB_BYTES 1019904u  /* 823,296 + 131,072 + 65,536 (VOCR5-S's pages: 1,792 B fewer weights, 48 B less arena) */
#define VOCR6_N_JOB_BYTES 1445888u  /* 1,191,936 + 188,416 + 65,536 (its gate section: a weights page more than v0.4.0's) */
#define VOCR6_L_JOB_BYTES 2813952u  /* 2,478,080 + 270,336 + 65,536 (v0.4.0's VOCR6-L's pages) */
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
    /* The three tiers: VOCR6-S, VOCR6-N and VOCR6-L (vita-vn-ocr v0.4.1-vocr6.2, docs/vocr6.md "The follow-up"),
     * each with its blob's own paths. One generation (each file names its own: VOCR6 for all three). */
    {VJO_VOCR6_S_NAME, "VOCR6", "S", VJO_VOCR6_S_BYTES, VOCR6_S_BOX_MS, VOCR6_S_JOB_BYTES},
    {VJO_VOCR6_N_NAME, "VOCR6", "N", VJO_VOCR6_N_BYTES, VOCR6_N_BOX_MS, VOCR6_N_JOB_BYTES},
    {VJO_VOCR6_L_NAME, "VOCR6", "L", VJO_VOCR6_L_BYTES, VOCR6_L_BOX_MS, VOCR6_L_JOB_BYTES},
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
