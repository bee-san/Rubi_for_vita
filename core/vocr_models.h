/* The local OCR models this build runs, by file name in ocr_model_dir, with the
 * metadata of the bee-san/vita-vn-ocr release that published each: its exact
 * size, its shape and the model generation it belongs to (VOCR1 for v0.2.0's
 * models, VOCR3 for v0.2.5-vocr3.1's). A .vocr blob carries no generation of
 * its own (its 64-byte header is all layout, CRC and checkpoint hash), so the
 * pin carries it. The game worker pins the same names with their SHA-256 and
 * arenas (core/vocr_ocr.c), the kernel's model broker with their sizes
 * (kernel/game_ocr.c) and the packager with their release, which it writes into
 * package-manifest.json (tools/prepare_ocr.py); tools/tests/test_vocr_defaults.py
 * and host/tests/test_vocr.c check that they agree.
 *
 * The Overlay app names the local OCR engine after the generation of the model
 * it runs (settings/engine.c): "VOCR3" for this package's default, and a new
 * generation's name as soon as its model is pinned here and made the default,
 * with no change to the app. Plain C without the OCR runtime: the app links it. */
#ifndef VJO_VOCR_MODELS_H
#define VJO_VOCR_MODELS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;        /* file name in ocr_model_dir */
    const char *generation;  /* "VOCR3": the model generation, the local engine's name */
    const char *shape;       /* "H15": the recognizer's size class (FL10 < H15 < F20) */
    uint32_t bytes;          /* the release file's exact size */
} VjoVocrModelInfo;

/* The entry for a file name, or NULL for a model this build does not run. */
const VjoVocrModelInfo *vjo_vocr_model_info(const char *name);
/* Every entry, for tools and tests; *count is set. */
const VjoVocrModelInfo *vjo_vocr_model_infos(unsigned *count);
/* "VOCR3-H15": generation and shape, for messages (cap >= 2). Returns the
 * length, or -1 when it did not fit (out is then ""). */
int vjo_vocr_model_label(const VjoVocrModelInfo *info, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
