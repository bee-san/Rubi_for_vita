/* Release metadata of the local OCR models this build runs (vocr_models.h). */
#include "vocr_models.h"
#include "../include/vjo_vocr_sizes.h"

#include <string.h>

#include "port.h"

#ifdef VJO_VOCR_MODEL_INFO_TEST
/* Host tests only: another package's table (a later generation as the default). */
#include VJO_VOCR_MODEL_INFO_TEST
#else
static const VjoVocrModelInfo infos[] = {
    /* v0.2.5-vocr3.1 (vita-vn-ocr #67): the default and its fallback. */
    {VJO_VOCR3_H15_NAME, "VOCR3", "H15", VJO_VOCR_H15_BYTES},
    {VJO_VOCR3_FL10_NAME, "VOCR3", "FL10", VJO_VOCR_FL10_BYTES},
    /* v0.2.0, the first released generation (vita-vn-ocr calls its FL10 "VOCR1"). */
    {"H15_w8.vocr", "VOCR1", "H15", VJO_VOCR_H15_BYTES},
    {"FL10_w8.vocr", "VOCR1", "FL10", VJO_VOCR_FL10_BYTES},
    {"F20_w8.vocr", "VOCR1", "F20", VJO_VOCR_F20_BYTES},
    /* v0.2.5-vocr4.1 (vita-vn-ocr #78): the confidence cascade's larger model. */
    {VJO_VOCR4_W20_NAME, "VOCR4", "W20", VJO_VOCR_CASCADE_BYTES},
};
#endif
#define N_INFOS (sizeof(infos) / sizeof(infos[0]))

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
