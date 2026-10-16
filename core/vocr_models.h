/* The local OCR models this build runs, by file name in ocr_model_dir, with the
 * metadata of the bee-san/vita-vn-ocr release that published each: its exact
 * size, its shape and the model generation it belongs to (VOCR1 for v0.2.0's
 * models, VOCR3 for v0.2.5-vocr3.1's, VOCR4 for v0.2.5-vocr4.1's, VOCR5 for
 * v0.3.0's), and the two estimates the Overlay app shows for a model size: its
 * time per textbox and the game memory a job of it needs. A .vocr blob carries no
 * generation of its own (its 64-byte header is all layout, CRC and checkpoint
 * hash), so the pin carries it. The game worker pins the same names with their
 * SHA-256 and arenas (core/vocr_ocr.c), the kernel's model broker with their
 * sizes (kernel/game_ocr.c) and the packager with their release, which it writes
 * into package-manifest.json (tools/prepare_ocr.py); tools/tests/test_vocr_defaults.py
 * and host/tests/test_vocr.c check that they agree.
 *
 * The model tiers (API 21, include/vjo_vocr_sizes.h): Small, Normal and Large,
 * each a pinned model; tools/model_tiers.json is the same table. The Overlay
 * app offers them as its Model size and names the local OCR engine after the
 * generation of the model that runs (settings/engine.c): "VOCR5" for this
 * package's three, and a new generation's name as soon as its models are pinned
 * here, with no change to the app. Plain C without the OCR runtime: the app
 * links it. */
#ifndef VJO_VOCR_MODELS_H
#define VJO_VOCR_MODELS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;        /* file name in ocr_model_dir */
    const char *generation;  /* "VOCR5": the model generation, the local engine's name */
    const char *shape;       /* "N": the recognizer's shape (FL10 < H15 < F20 = W20; VOCR5's S < N < L) */
    uint32_t bytes;          /* the release file's exact size */
    /* Estimates (tools/model_tiers.json "est_box_basis"): ms to read a typical
     * full textbox (3 lines of about 640 px, greedy) on the console on one core,
     * calibrated to VOCR3-H15's measured 5.0-5.2 ms per CTC frame (vita-vn-ocr
     * docs/ctc-lm-decode.md, high bound; VOCR5's: eval/vocr5_protocol.json); and
     * the game's free USER bytes a first
     * job of it at the full screen needs besides the reserve: its page-rounded
     * weights, workspace and capture block, without the LM, the cascade or the
     * PSP refinement, which a job leaves out first when memory is short (the game
     * worker's check; host/tests/test_vocr.c computes it from core/vocr_ocr.c). */
    uint32_t box_ms;
    uint32_t job_bytes;
} VjoVocrModelInfo;

typedef struct {
    const char *id;          /* "small": config.ini vocr_tier */
    const char *name;        /* "Small": the Overlay app's button */
    const char *tagline;     /* "fastest" */
    const char *model;       /* the tier's pinned model file (include/vjo_vocr_sizes.h VJO_VOCR_TIERn_NAME) */
    uint32_t reserve;        /* the game memory a job of the tier leaves free (VJO_GAME_RESERVE_*) */
} VjoVocrTierInfo;

/* The entry for a file name, or NULL for a model this build does not run. */
const VjoVocrModelInfo *vjo_vocr_model_info(const char *name);
/* Every entry, for tools and tests; *count is set. */
const VjoVocrModelInfo *vjo_vocr_model_infos(unsigned *count);
/* "VOCR3-H15": generation and shape, for messages (cap >= 2). Returns the
 * length, or -1 when it did not fit (out is then ""). */
int vjo_vocr_model_label(const VjoVocrModelInfo *info, char *out, size_t cap);
/* Tier t (1..VJO_VOCR_TIERS, smallest first), or NULL. */
const VjoVocrTierInfo *vjo_vocr_tier_info(unsigned tier);
/* The game's free USER bytes a first full-screen job of tier t needs, its
 * reserve included (its model's job_bytes + reserve); 0 for an unknown tier. */
uint32_t vjo_vocr_tier_need(unsigned tier);

#ifdef __cplusplus
}
#endif
#endif
