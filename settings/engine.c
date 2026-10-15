/* The Overlay app's OCR engine card (engine.h). */
#include "engine.h"

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

void vjo_engine_card(const VjoConfig *cfg, VjoEngineCard *card)
{
    memset(card, 0, sizeof(*card));
    card->tier = cfg->vocr_tier >= 1 && cfg->vocr_tier <= (int)VJO_VOCR_TIERS ? cfg->vocr_tier
                                                                             : (int)VJO_VOCR_TIER_DEFAULT;
    card->model = tier_model(card->tier);
    for (int t = 1; t <= (int)VJO_VOCR_TIERS; t++) {
        const VjoVocrModelInfo *m = tier_model(t);
        card->found[t] = m && found(cfg->ocr_model_dir, m);
    }
    /* Named after the model that loads: a job leaves a tier whose file is missing
     * or not its release size out and runs the next smaller one (kernel/game_ocr.c
     * refuses it, shell/worker.c run_vocr). With no file there nothing runs: the
     * button keeps the chosen tier's name and the status line is red. */
    for (int t = card->tier; t >= 1 && !card->runs; t--)
        if (card->found[t]) card->runs = tier_model(t);
    const VjoVocrModelInfo *named = card->runs ? card->runs : card->model;
    snprintf(card->local, sizeof(card->local), "%s", named ? named->generation : "VOCR");
}

int vjo_engine_index(int backend)
{
    return backend == VJO_OCR_LENS;
}

int vjo_engine_backend(int index)
{
    return index ? VJO_OCR_LENS : VJO_OCR_VOCR;
}

const char *vjo_engine_name(const VjoEngineCard *card, int index)
{
    return index ? "Google Lens" : card->local;
}

const char *vjo_engine_description(int index)
{
    return index ? "Wi-Fi required. Sends the selected image to Google."
                 : "Local OCR: dialogue region, automatic or manual.";
}

int vjo_engine_status(const VjoConfig *cfg, const VjoEngineCard *card, char *out, size_t cap)
{
    const VjoVocrTierInfo *tier = vjo_vocr_tier_info((unsigned)card->tier);
    char model[24];
    if (vjo_engine_index(cfg->ocr_backend)) {
        snprintf(out, cap, "%s", vjo_engine_description(1));
        return 0;
    }
    if (!tier || !card->model) {
        snprintf(out, cap, "This model size is not in this build.");
        return 1;
    }
    vjo_vocr_model_label(card->model, model, sizeof(model));
    if (!card->found[card->tier]) {
        /* A broken install, not a memory fallback: named, never what runs instead. */
        snprintf(out, cap, "%s missing. Reinstall the package.", model);
        return 1;
    }
    snprintf(out, cap, "%s: %s. %s found.", tier->name, tier->tagline, model);
    return 0;
}

int vjo_engine_tier_detail(int tier, char *out, size_t cap)
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
