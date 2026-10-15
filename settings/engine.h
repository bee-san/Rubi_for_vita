/* The Overlay app's OCR engine card: exactly two engines. Button 0 is the local
 * engine (ocr_backend = vocr), named after the generation of the model it runs,
 * read from that model's pinned metadata (core/vocr_models.h): "VOCR3" for this
 * package, the next generation's name once its model is the default. Button 1
 * is Google Lens. The automatic fallback to a smaller model when a game's memory
 * is short stays internal (the game worker's, per job): the card names neither
 * model size, only reports a missing file. */
#ifndef VJO_SETTINGS_ENGINE_H
#define VJO_SETTINGS_ENGINE_H

#include <stddef.h>

#include "../core/config.h"
#include "../core/vocr_models.h"

#define VJO_ENGINES 2

typedef struct {
    char local[16];                   /* button 0: the generation, or "VOCR" when no pinned model is set */
    const VjoVocrModelInfo *model;    /* vocr_model's pin; NULL: this build does not run it */
    const VjoVocrModelInfo *fallback; /* vocr_fallback_model's pin; NULL: none, or not run */
    int model_found, fallback_found;  /* the file is in ocr_model_dir at its pinned size */
} VjoEngineCard;

/* The card for cfg; reads only the two model files' sizes. */
void vjo_engine_card(const VjoConfig *cfg, VjoEngineCard *card);
/* Button of a backend: 1 for Google Lens, 0 for the local engine (the only
 * other backend a parsed config.ini can hold); and the backend of a button. */
int vjo_engine_index(int backend);
int vjo_engine_backend(int index);
/* A button's label ("VOCR3" | "Google Lens") and the help shown when it is selected. */
const char *vjo_engine_name(const VjoEngineCard *card, int index);
const char *vjo_engine_description(int index);
/* The line under the help for the selected engine: which model runs, or what
 * is missing. Returns 1 when it reports a problem (drawn in the error colour). */
int vjo_engine_status(const VjoConfig *cfg, const VjoEngineCard *card, char *out, size_t cap);

#endif
