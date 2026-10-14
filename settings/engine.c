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

void vjo_engine_card(const VjoConfig *cfg, VjoEngineCard *card)
{
    const VjoVocrModelInfo *named;
    memset(card, 0, sizeof(*card));
    card->model = vjo_vocr_model_info(cfg->vocr_model);
    card->fallback = vjo_vocr_model_info(cfg->vocr_fallback_model); /* NULL for "" (none) */
    card->model_found = card->model && found(cfg->ocr_model_dir, card->model);
    card->fallback_found = card->fallback && found(cfg->ocr_model_dir, card->fallback);
    /* A vocr_model this build does not run is refused, and the fallback runs. */
    named = card->model ? card->model : card->fallback;
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
    char model[24], fallback[24];
    if (vjo_engine_index(cfg->ocr_backend)) {
        snprintf(out, cap, "No model file needed. Uses your chosen dictionary.");
        return 0;
    }
    vjo_vocr_model_label(card->model, model, sizeof(model));
    vjo_vocr_model_label(card->fallback, fallback, sizeof(fallback));
    if (card->model_found) {
        snprintf(out, cap, "%s found. Game OCR worker required.", model);
        return 0;
    }
    if (card->fallback_found && card->model) {
        /* The worker reads the fallback when the model's file is absent or the wrong size. */
        snprintf(out, cap, "%s missing; uses %s. Game OCR worker required.", model, fallback);
        return 0;
    }
    if (card->fallback_found)
        snprintf(out, cap, "vocr_model is not in this build; uses %s.", fallback);
    else if (card->model)
        snprintf(out, cap, "%s missing. Game OCR worker required.", model);
    else
        snprintf(out, cap, "vocr_model is not in this build. Set it in config.ini.");
    return 1;
}
