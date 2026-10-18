# vita-vn-ocr, the overlay's only OCR engine: bee-san/vita-vn-ocr's plain-C99
# recognizer and dialogue line finder, plus core/vocr_ocr.c. The runtime is not
# copied into this repository: point VJO_VOCR_SOURCE_DIR at a checkout of the
# pinned commit (or place its runtime/ files under third_party/vita-vn-ocr/).
# Every file is checked against the hash it was evaluated with.
set(VJO_VOCR_COMMIT dcf67696bccd3424552d551e040c0e6bd6ce6d82)
set(VJO_VOCR_SOURCE_DIR "" CACHE PATH "Checkout of github.com/bee-san/vita-vn-ocr at ${VJO_VOCR_COMMIT}")
option(VJO_VOCR_ALLOW_UNPINNED "Build vita-vn-ocr runtime files that differ from the pinned commit" OFF)

if(NOT VJO_VOCR_SOURCE_DIR AND EXISTS "${VJO_ROOT}/third_party/vita-vn-ocr/runtime/vocr.c")
  set(VJO_VOCR_SOURCE_DIR "${VJO_ROOT}/third_party/vita-vn-ocr")
endif()
if(NOT VJO_VOCR_SOURCE_DIR)
  message(FATAL_ERROR "VJO_WITH_VOCR needs the vita-vn-ocr runtime:\n"
    "  git clone https://github.com/bee-san/vita-vn-ocr /path/to/vita-vn-ocr\n"
    "  git -C /path/to/vita-vn-ocr checkout ${VJO_VOCR_COMMIT}\n"
    "then configure with -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr (docs/vita-vn-ocr.md).")
endif()

set(_vocr_rt "${VJO_VOCR_SOURCE_DIR}/runtime")
# vocr_lines_rows.c is the row-callback form of vocr_lines.c (API 19 reads the
# region from the game's framebuffer in strips); the game worker uses it, and
# the host tests check that both give the same lines. vocr_lm.c (vita-vn-ocr
# #82) is the CTC prefix beam search with a character n-gram LM over
# vocr_run()'s per-frame top-K, which also reports each line's confidence.
# vocr_lines_refine.c (vita-vn-ocr #83) refines the finder's boxes (tightens,
# drops frame rows and cursors, splits boxes holding several text rows).
# vocr_vertical.c (vita-vn-ocr #99) finds the columns of vertical text (GRAD on the
# region's transpose, right to left) and turns a column 90 degrees anticlockwise for
# the vertical recognizer (config.ini ocr_vertical, docs/vita-vn-ocr.md "Vertical text").
set(_vocr_pins
  vocr.c 3e1756138b2e1d9aff15abdda95b02e0c015a5ba0af99678bdd678377b73f559
  vocr.h 30bb32693e634a4c4b4cd5cd3a4da8d33f6882bf00348d0e1d8264ba08376271
  vocr_lines.c 90c30a1c1e1707b818c2bd919243ee589700ff44ceac8b8d7a842b9d55d8d9ca
  vocr_lines.h 4f768c9a7aaa1f9e3b8f16990be23c3b098b675cf6316df7eb20dc6198b9f37d
  vocr_lines_rows.c 3207edb85d1cd7c79899390bb2ee028839dd120ca5d2278a28edd5fa9ca2ee6e
  vocr_lines_rows.h 60967239238df1ecca767e95c9c752f4c0b9efba097a59034ae2326717ad3465
  vocr_lm.c 89e0494caf22090cab52131b8b9d489a26085300b61e59855cb9a0d4074dbd05
  vocr_lm.h 6d314c7fbc74c240b4e6d70c0dc0a9e1c98fcb196ecd6bc1aa2ccf5c8fb9fb86
  vocr_lines_refine.c f4fb08aa995379c5ee9cb746307ed2c2d357574d71e5556f2b40d8ce1584e797
  vocr_lines_refine.h 66fe3b716193d835e9ab1d614d71ad8e19de70db5ac8ec0f09b8e027b260dc6b
  vocr_vertical.c 2e938246e95d478f9b3e18387e81d397e5a18c0d7f5fe6ee528b834036ce9cb6
  vocr_vertical.h 6292e06392199d3e2632e10b4d4bae81acda53f84117cd4210a5da0cfd9fca6b)
while(_vocr_pins)
  list(POP_FRONT _vocr_pins _file _want)
  if(NOT EXISTS "${_vocr_rt}/${_file}")
    message(FATAL_ERROR "vita-vn-ocr: ${_vocr_rt}/${_file} is missing")
  endif()
  file(SHA256 "${_vocr_rt}/${_file}" _got)
  if(NOT _got STREQUAL _want)
    if(VJO_VOCR_ALLOW_UNPINNED)
      message(WARNING "vita-vn-ocr: runtime/${_file} differs from ${VJO_VOCR_COMMIT} (unpinned build)")
    else()
      message(FATAL_ERROR "vita-vn-ocr: runtime/${_file} has sha256 ${_got}, expected ${_want} "
        "(commit ${VJO_VOCR_COMMIT}). Check out that commit, or set VJO_VOCR_ALLOW_UNPINNED=ON.")
    endif()
  endif()
endwhile()
message(STATUS "vita-vn-ocr runtime: ${_vocr_rt} (pinned ${VJO_VOCR_COMMIT})")

# runtime/README.md, "Porting notes": C99, -O2, no FP contraction, so every
# platform computes the same bits; the Vita build adds the Cortex-A9 NEON flags.
# Its few libc calls (memcpy, memset, the fabsf/signbit builtins) need no
# newlib, which SceShell plugins link without.
add_library(vjo_vocr_rt STATIC "${_vocr_rt}/vocr.c" "${_vocr_rt}/vocr_lines.c" "${_vocr_rt}/vocr_lines_rows.c"
  "${_vocr_rt}/vocr_lm.c" "${_vocr_rt}/vocr_lines_refine.c" "${_vocr_rt}/vocr_vertical.c")
target_include_directories(vjo_vocr_rt PUBLIC "${_vocr_rt}")
target_compile_options(vjo_vocr_rt PRIVATE -std=c99 -O2 -ffp-contract=off -Wall -Wextra
  -ffunction-sections -fdata-sections)
if(VJO_BUILD_VITA)
  target_compile_options(vjo_vocr_rt PRIVATE -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
else()
  target_link_libraries(vjo_vocr_rt PUBLIC m)
endif()

# core/textnorm.c: the text the game worker returns, normalised (core/textnorm.h).
add_library(vjo_vocr STATIC "${VJO_ROOT}/core/vocr_ocr.c" "${VJO_ROOT}/core/textnorm.c")
target_link_libraries(vjo_vocr PUBLIC vjo_vocr_rt)
target_compile_options(vjo_vocr PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
if(VJO_BUILD_VITA)
  target_compile_options(vjo_vocr PRIVATE -O2 -fno-builtin -fshort-wchar)
  # The job's control code (the capture's row checks, the wait, residency, the LM
  # and cascade set-up, a few hundred bytes of text normalisation once per job):
  # built for size, the worker's pages. The per-pixel work, the recognizer and
  # the decoder, is the runtime's (vjo_vocr_rt, -O2).
  set_source_files_properties("${VJO_ROOT}/core/vocr_ocr.c" "${VJO_ROOT}/core/textnorm.c" PROPERTIES
    COMPILE_OPTIONS "-Os")
endif()
