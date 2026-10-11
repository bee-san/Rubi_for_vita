Vendored sources:
- bearssl  https://www.bearssl.org/git/BearSSL  7bea48e5e850ab4cafbe68d3765cdaba13a86d6f (MIT)
- jsmn     https://github.com/zserge/jsmn       25647e692c7906b96ffd2b05ca54c097948e879c (MIT)
- acutest  https://github.com/mity/acutest      31751b4089c93b46a9fd8a8183a695f772de66de (MIT)

The standalone Settings app builds libpng 1.6.58 from
https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.58,
SHA-256 `a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf`.
This project-private static target uses the same VitaSDK compiler and headers
as the app; the SDK installation is not modified. Source is fetched into the
build directory. [Libpng notice](licenses/libpng.txt),
[native runtime compatibility check](../docs/settings-png-runtime.md).

Fetched for the optional/default-enabled local OCR build:

- ncnn: https://github.com/Tencent/ncnn at
  `3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82` (BSD-3-Clause plus bundled notices).
  The PP-OCRv5 character dictionary comes from the same revision's
  `examples/ppocrv5_dict.h` (Copyright 2025 Tencent, BSD-3-Clause).
  Full notices: [licenses/ncnn.txt](licenses/ncnn.txt).
- Converted PP-OCRv5 mobile recognizer graph/weights:
  https://github.com/nihui/ncnn-android-ppocrv5 at
  `671ac4a72299a86ddee160131ba88fed748df425`, under `app/src/main/assets/`.
  Model conversion credited to nihui; no Android application code is included.
- PP-OCRv5 model originates in https://github.com/PaddlePaddle/PaddleOCR
  (Apache-2.0): [licenses/PaddleOCR.txt](licenses/PaddleOCR.txt).

`tools/prepare_ocr.py` records expected asset sizes and SHA-256 hashes. The
model weights are downloaded, not committed. The generated graph/vocabulary
header is a build artifact. This port changes ncnn's build configuration,
replaces its global-allocation source with placement operators only, and
replaces shape-expression evaluation with a rejecting implementation; it
does not modify the checked-out upstream sources.

Optional vita-vn-ocr backend (`-DVJO_WITH_VOCR=ON`, not vendored):

- Runtime: https://github.com/bee-san/vita-vn-ocr at
  `72cf4af0905091a0ee329d336889ccf0a8cd38c8`, `runtime/vocr.c`, `vocr.h`,
  `vocr_lines.c` and `vocr_lines.h` (GPL-3.0-or-later). The build takes them from
  `VJO_VOCR_SOURCE_DIR` and checks their SHA-256 (`cmake/VocrOCR.cmake`).
- Weights: the int8 `.vocr` assets of that repository's release v0.2.0
  (GPL-3.0-or-later), downloaded by `tools/prepare_ocr.py --backend vocr` and
  checked by size and SHA-256, never committed. Their pretraining used line
  crops from NVIDIA OCR-Synthetic-Multilingual-v1 (Japanese subset, revision
  `69696a1cc543ef3a0f8e9892a89c17293e915263`), © NVIDIA Corporation, licensed
  under CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/); the release
  notes carry the full attribution.

The optional single-line Meiki trial uses MeikiOCR 0.3.5 at
`52aa607ecdc1038c7bf390f37cd9a8b306e8a08a` (Apache-2.0) and the existing
streamed-head int8 conversion of its recognition model (LGPL-3.0). MNN 3.2.0
at `ebdada82634300956e08bd4056ecfeb1e4f23b32` (Apache-2.0) runs only inside
the isolated game-process engine. See [Meiki dependencies](../docs/meiki-dependencies.json),
[Meiki notices](licenses/Meiki.txt), [resize notices](licenses/Meiki-resize.txt)
and [MNN notices](licenses/MNN.txt). Weights are release assets, never source files.

The distributed Meiki recognizer's exact model license is
[LGPL-3.0](licenses/Meiki-model-LGPL-3.0.txt), with the complete GPL-3.0
text at the repository/package [LICENSE](../LICENSE). MNN's embedded
[Flatbuffers Apache-2.0](licenses/MNN-Flatbuffers-Apache-2.0.txt) and
[Half MIT](licenses/MNN-Half-MIT.txt) notices are included separately.
`Meiki.txt` describes Apache-2.0 code, not the model's LGPL license.
