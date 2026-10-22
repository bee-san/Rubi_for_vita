Vendored sources:
- vndb-names https://github.com/bee-san/psp_and_vita_vndb_name_dict
  680a5e3a6fb4e53c060c08a4c29ed383eca3282e: unchanged combined PSP/Vita Yomitan ZIP,
  snapshot cutoff 2026-10-22; VNDB contributors, ODbL 1.0 / DbCL 1.0. Converted
  offline by tools/prepare_vndb.py; docs/vndb-names.json pins its checksums and
  docs/vndb-attribution.txt records the changes. Full licences: licenses/VNDB-*.txt.
- bearssl  https://www.bearssl.org/git/BearSSL  7bea48e5e850ab4cafbe68d3765cdaba13a86d6f (MIT):
           only SHA-1 and SHA-224/256 are kept (src/hash/sha1.c, src/hash/sha2small.c, the
           byte-order helpers src/codec/dec32be.c and enc32be.c, src/inner.h, src/config.h and
           inc/, unchanged); the rest went with the network, as the overlay is offline
- jsmn     https://github.com/zserge/jsmn       25647e692c7906b96ffd2b05ca54c097948e879c (MIT)
- acutest  https://github.com/mity/acutest      31751b4089c93b46a9fd8a8183a695f772de66de (MIT)
- lapis    https://github.com/donkuri/lapis       38238d0b1c072fe7d7cd121e1e4c838a959f8d6b, tag 1.7.0 (GPL-3.0):
           the Anki note type the Rubi app's .apkg export embeds (lapis/README.md)
- taihen-parser https://github.com/DaveeFTW/taihen-parser 6d817fd (MIT): taiHEN's own config.txt
           lexer and parser, unchanged, the Rubi app's check of every config.txt edit
           (core/taicfg.c, docs/one-vpk-install.md). Built as taiHEN 309b380 builds it (its
           inbuilt ctype and string functions: NO_CTYPE, NO_STRING). SHA-256, checked by
           tools/tests/test_tai_config.py:
             b34087aeb19a48190527d7866166a4dcb67d199333e609b5aa3f91cebb3ca42c  LICENSE
             c0dae41266cd15c9d999e5a3d825ee56c30e2d934ec7954691be411735fe9021  include/taihen/lexer.h
             c72e21ff3377ca5aba6afd8f78e301982dc1ea60416676cb8cce5426536dc5c8  include/taihen/parser.h
             6aed254d7872512f22c152f9edbf15529b27f5e041f5247dbe75c3352d5fe318  src/lexer.c
             5059be0048d4913657b1b9eb96be6242d4b88c1d83c8ee4828e07aaa54e89497  src/parser.c
- LunaTranslator https://github.com/HIllya51/LunaTranslator 51884b6c88cba8c42e98ce3e43c4f408017082a1,
           v12.1.2.12 (GPL-3.0): its PS Vita (Vita3K) hook table, read by tools/gen_luna_vita.py into
           core/hook_luna_table.inc, and its hookfuncs and filters, ported to C in core/hook_luna.c
           (native hooks, docs/hook-titles.md). vita3k_1.cpp, vita3k.cpp, ppsspp_1.cpp,
           stringfilters.cpp and LICENSE, unchanged, none compiled; their SHA-256 are in
           LunaTranslator/README.md and checked by the generator

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

The Meiki single-line trial (MeikiOCR, its LGPL-3.0 converted model and MNN)
was removed after v0.7.12; this tree no longer builds or ships it. Its pinned
dependencies and notices remain in the
[v0.7.12 tree](https://github.com/bee-san/Rubi_for_vita/tree/aff03cbb559a7eccdcfbd27b9c873ea7543f1deb/third_party)
and that release.
