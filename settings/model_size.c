/* The Rubi app's OCR card, its Model size (model_size.h). */
#include "model_size.h"

#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

/* The model's file is in dir at exactly its pinned size: the game worker
 * refuses a file of any other size. */
static int found(const char *dir, const VjoVocrModelInfo *info)
{
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, info->name);
    if (n < 0 || n >= (int)sizeof(path)) return 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    SceOff bytes = sceIoLseek(fd, 0, SCE_SEEK_END);
    int closed = sceIoClose(fd) >= 0;
    return closed && bytes == (SceOff)info->bytes;
}

static const VjoVocrModelInfo *tier_model(int tier)
{
    const VjoVocrTierInfo *t = vjo_vocr_tier_info((unsigned)tier);
    return t ? vjo_vocr_model_info(t->model) : NULL;
}

void vjo_model_size_card(const VjoConfig *cfg, VjoModelSizeCard *card)
{
    memset(card, 0, sizeof(*card));
    card->tier = cfg->vocr_tier >= 1 && cfg->vocr_tier <= (int)VJO_VOCR_TIERS ? cfg->vocr_tier
                                                                             : (int)VJO_VOCR_TIER_DEFAULT;
    card->model = tier_model(card->tier);
    for (int t = 1; t <= (int)VJO_VOCR_TIERS; t++) {
        const VjoVocrModelInfo *m = tier_model(t);
        card->found[t] = m && found(cfg->ocr_model_dir, m);
    }
}

int vjo_model_size_status(const VjoModelSizeCard *card, char *out, size_t cap)
{
    const VjoVocrTierInfo *tier = vjo_vocr_tier_info((unsigned)card->tier);
    char model[24];
    if (!tier || !card->model) {
        snprintf(out, cap, "This model size is not in this build.");
        return 1;
    }
    vjo_vocr_model_label(card->model, model, sizeof(model));
    if (!card->found[card->tier]) {
        /* A broken install, not a memory fallback: named, never what runs instead
         * (a job leaves a missing tier out and runs the next smaller one found). */
        snprintf(out, cap, "%s missing. Reinstall the package.", model);
        return 1;
    }
    snprintf(out, cap, "%s: %s. %s found.", tier->name, tier->tagline, model);
    return 0;
}

int vjo_model_size_detail(int tier, char *out, size_t cap)
{
    const VjoVocrModelInfo *m = tier_model(tier);
    /* tenths, rounded: ms to s, bytes to MB (10^6, as the tiers' ceilings are given) */
    unsigned s10 = m ? (m->box_ms + 50u) / 100u : 0u, mb10 = m ? (m->job_bytes + 50000u) / 100000u : 0u;
    if (!m || !m->box_ms || !m->job_bytes) {
        if (cap) out[0] = '\0';
        return 0;
    }
    return snprintf(out, cap, "%u.%u s, %u.%u MB", s10 / 10u, s10 % 10u, mb10 / 10u, mb10 % 10u);
}
