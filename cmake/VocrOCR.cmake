# vita-vn-ocr backend (ocr_backend = vocr): bee-san/vita-vn-ocr's plain-C99
# recognizer and dialogue line finder, plus core/vocr_ocr.c. The runtime is not
# copied into this repository: point VJO_VOCR_SOURCE_DIR at a checkout of the
# pinned commit (or place its runtime/ files under third_party/vita-vn-ocr/).
# Every file is checked against the hash it was evaluated with.
set(VJO_VOCR_COMMIT a5d5aad68bb8e719e44e2e50b5836cf9afeae796)
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
# the host tests check that both give the same lines.
set(_vocr_pins
  vocr.c d1fe1f7be4b479ec96d5ca0c1c44c4c3cb7edcf66dcf7abced332601fdcdeb33
  vocr.h c4948a25be4063cd1e22c2011a24c21d95c6a06c2360348452876a24288e09dd
  vocr_lines.c 90c30a1c1e1707b818c2bd919243ee589700ff44ceac8b8d7a842b9d55d8d9ca
  vocr_lines.h 4f768c9a7aaa1f9e3b8f16990be23c3b098b675cf6316df7eb20dc6198b9f37d
  vocr_lines_rows.c 3207edb85d1cd7c79899390bb2ee028839dd120ca5d2278a28edd5fa9ca2ee6e
  vocr_lines_rows.h 60967239238df1ecca767e95c9c752f4c0b9efba097a59034ae2326717ad3465)
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
add_library(vjo_vocr_rt STATIC "${_vocr_rt}/vocr.c" "${_vocr_rt}/vocr_lines.c" "${_vocr_rt}/vocr_lines_rows.c")
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
  # A few hundred bytes of text once per job: built for size (the worker's pages).
  set_source_files_properties("${VJO_ROOT}/core/textnorm.c" PROPERTIES COMPILE_OPTIONS "-Os")
endif()
