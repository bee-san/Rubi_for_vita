/* The Overlay app's OCR engine card: exactly two engines, and under the local
 * one its Model size. Button 0 is the local engine (ocr_backend = vocr), named
 * after the generation of the model it runs, read from that model's pinned
 * metadata (core/vocr_models.h): "VOCR3" for this package's Small and Normal,
 * "VOCR4" for its Large, and the next generation's name once its models are
 * pinned. Button 1 is Google Lens. The Model size row offers the three tiers
 * (Small, Normal, Large; config.ini vocr_tier), each with its estimated time
 * per textbox and the game memory it needs, from the same metadata. When a
 * game has too little free memory for the chosen size a job runs a smaller
 * one: the game worker decides per job, the Shell logs it, and the app says
 * nothing of it (owner 2026-10-15 04:54Z); the card only reports a missing file. */
#ifndef VJO_SETTINGS_ENGINE_H
#define VJO_SETTINGS_ENGINE_H

#include <stddef.h>

#include "../core/config.h"
#include "../core/vocr_models.h"
#include "../include/vjo_vocr_sizes.h"

#define VJO_ENGINES 2

typedef struct {
    char local[16];                   /* button 0: the generation, or "VOCR" when no pinned model is set */
    int tier;                         /* the chosen model size, VJO_VOCR_TIER_* */
    const VjoVocrModelInfo *model;    /* the chosen tier's model; NULL: this build does not pin it */
    /* The model a job runs when the game has room for it: the chosen tier's when
     * its file is found, else the largest smaller tier's found (a job leaves a
     * missing tier out, shell/worker.c), else NULL: nothing runs. */
    const VjoVocrModelInfo *runs;
    int found[VJO_VOCR_TIERS + 1];    /* tier t's file is in ocr_model_dir at its pinned size */
} VjoEngineCard;

/* The card for cfg; reads only the tiers' model files' sizes. */
void vjo_engine_card(const VjoConfig *cfg, VjoEngineCard *card);
/* Button of a backend: 1 for Google Lens, 0 for the local engine (the only
 * other backend a parsed config.ini can hold); and the backend of a button. */
int vjo_engine_index(int backend);
int vjo_engine_backend(int index);
/* A button's label ("VOCR3" | "Google Lens") and the help shown when it is selected. */
const char *vjo_engine_name(const VjoEngineCard *card, int index);
const char *vjo_engine_description(int index);
/* The card's last line for the selected engine: the model size and its model
 * (found), what is missing, or Lens's help. Returns 1 when it reports a problem
 * (drawn in the error colour). */
int vjo_engine_status(const VjoConfig *cfg, const VjoEngineCard *card, char *out, size_t cap);
/* A Model size button's second line: "1.3 s, 1.4 MB", the tier's estimated
 * time per 3-line textbox and the game memory its model takes in a job (its
 * weights, workspace and capture block at the full screen; core/vocr_models.h),
 * or "" when its model is not pinned. */
int vjo_engine_tier_detail(int tier, char *out, size_t cap);

#endif
