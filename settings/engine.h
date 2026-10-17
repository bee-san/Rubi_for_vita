/* The Overlay app's OCR card: the local engine's Model size. VOCR is the only
 * engine since the overlay went offline (owner 2026-10-15 15:27Z: Google Lens
 * removed). The row offers the three tiers (Small, Normal, Large; config.ini
 * vocr_tier), each with its estimated time per textbox and the game memory it
 * needs, from the models' pinned metadata (core/vocr_models.h); the card's last
 * line names the chosen size's model, whose name carries its generation
 * ("VOCR5-N", and the next generation's once its models are pinned). When a
 * game has too little free memory for the chosen size a job runs a smaller
 * one: the game worker decides per job, the Shell logs it, and the app says
 * nothing of it (owner 2026-10-15 04:54Z); the card only reports a missing file. */
#ifndef VJO_SETTINGS_ENGINE_H
#define VJO_SETTINGS_ENGINE_H

#include <stddef.h>

#include "../core/config.h"
#include "../core/vocr_models.h"
#include "../include/vjo_vocr_sizes.h"

typedef struct {
    int tier;                         /* the chosen model size, VJO_VOCR_TIER_* */
    const VjoVocrModelInfo *model;    /* the chosen tier's model; NULL: this build does not pin it */
    int found[VJO_VOCR_TIERS + 1];    /* tier t's file is in ocr_model_dir at its pinned size */
} VjoEngineCard;

/* The card for cfg; reads only the tiers' model files' sizes. */
void vjo_engine_card(const VjoConfig *cfg, VjoEngineCard *card);
/* The card's last line: the model size and its model (found), or what is
 * missing. Returns 1 when it reports a problem (drawn in the error colour). */
int vjo_engine_status(const VjoEngineCard *card, char *out, size_t cap);
/* A Model size button's second line: "1.3 s, 1.4 MB", the tier's estimated
 * time per 3-line textbox and the game memory its model takes in a job (its
 * weights, workspace and capture block at the full screen; core/vocr_models.h),
 * or "" when its model is not pinned. */
int vjo_engine_tier_detail(int tier, char *out, size_t cap);

#endif
