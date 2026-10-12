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

PP-OCRv6 small engine (`-DVJO_WITH_PPOCRV6=ON`, the default, linked into the
game worker; [docs/ppocrv6.md](../docs/ppocrv6.md)):

- Model: PaddleOCR PP-OCRv6 small rec (https://github.com/PaddlePaddle/PaddleOCR,
  Apache-2.0: [licenses/PaddleOCR.txt](licenses/PaddleOCR.txt)), as the ONNX file
  RapidOCR 3.10.0 publishes (https://www.modelscope.cn/models/RapidAI/RapidOCR,
  Apache-2.0), SHA-256
  `6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884`.
- `third_party/ppocrv6/` holds the derived ncnn param and the kept part of its
  character dictionary (Apache-2.0, from that model). The int8 weights derived from
  it are a release asset, rebuilt reproducibly by `tools/prepare_ppocrv6.py build`
  and never committed. Changes from the original: the CTC head pruned to JIS X 0208
  classes, the final Softmax removed, the graph converted by pnnx 20260704
  (https://github.com/pnnx/pnnx, a build tool, not shipped) and rewritten for ncnn,
  and the weights quantised to int8 by ncnn's tools.
- ncnn at the same revision as above, built with only the graph's layers and the
  int8 kernels; global C++ allocation for the worker comes from `core/ncnn_new.cpp`.

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

The Meiki single-line trial (MeikiOCR, its LGPL-3.0 converted model and MNN)
was removed after v0.7.12; this tree no longer builds or ships it. Its pinned
dependencies and notices remain in the
[v0.7.12 tree](https://github.com/bee-san/vita-jp-overlay/tree/aff03cbb559a7eccdcfbd27b9c873ea7543f1deb/third_party)
and that release.
