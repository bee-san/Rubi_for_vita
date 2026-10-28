# tools/vita3k_smoke: the real game worker's job path as a Vita homebrew app, for Vita3K
# (tools/vita3k_smoke.sh builds and runs it). Not a release artifact; nothing here ships.
#
#   cmake -S tools/vita3k_smoke -B build/vita3k-smoke -DCMAKE_BUILD_TYPE=Release \
#     -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr [-DVJO_VOCR_SPOT_SOURCE_DIR=/path/to/vita-vn-ocr-spot]
#   cmake --build build/vita3k-smoke        -> vjo_smoke.vpk
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()
project(vjo_smoke C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_STANDARD 99)  # as vita/CMakeLists.txt (taiHEN's TAI_CONTINUE casts to a function of unspecified arguments)

set(VITA_APP_NAME "VJO capture smoke")
set(VITA_TITLEID "VJOSMK001")
set(VITA_VERSION "01.00")
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# The build variant (docs/release-variants.md), as vita/CMakeLists.txt's: the game worker's sources.
option(VJO_DEBUG "Build the debug variant (the test builds'); OFF: the normal release build" ON)
if(VJO_DEBUG)
  add_compile_definitions(VJO_DEBUG=1)
else()
  add_compile_definitions(VJO_DEBUG=0)
endif()
set(VJO_VOCR_SOURCE_DIR "" CACHE PATH "vita-vn-ocr checkout (runtime/)")
if(NOT EXISTS "${VJO_VOCR_SOURCE_DIR}/runtime/vocr_lines_rows.c")
  message(FATAL_ERROR "VJO_VOCR_SOURCE_DIR must name a vita-vn-ocr checkout with runtime/vocr_lines_rows.c")
endif()
set(RT ${VJO_VOCR_SOURCE_DIR}/runtime)
# API 54: the text spotter (core/vocr_ocr.c includes vocr_textspot6.h), pinned at its own commit until the recognizer's
# holds it (cmake/VocrOCR.cmake VJO_VOCR_SPOT_COMMIT): that checkout, else the recognizer's when it has the files, else
# .refs/vita-vn-ocr-spot (cmake/VocrOCR.cmake's candidates; it checks their hashes, this app does not)
set(VJO_VOCR_SPOT_SOURCE_DIR "" CACHE PATH "vita-vn-ocr checkout at cmake/VocrOCR.cmake's VJO_VOCR_SPOT_COMMIT")
if(VJO_VOCR_SPOT_SOURCE_DIR)
  set(SPOT_RT ${VJO_VOCR_SPOT_SOURCE_DIR}/runtime)
elseif(EXISTS "${RT}/vocr_textspot6.c")
  set(SPOT_RT ${RT})
else()
  set(SPOT_RT ${ROOT}/.refs/vita-vn-ocr-spot/runtime)
endif()
if(NOT EXISTS "${SPOT_RT}/vocr_textspot6.c")
  message(FATAL_ERROR "no text spotter in ${SPOT_RT}: set VJO_VOCR_SPOT_SOURCE_DIR (cmake/VocrOCR.cmake VJO_VOCR_SPOT_COMMIT)")
endif()
set(ARCH -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)

# runtime/README.md "Porting notes": C99, -O2, no FP contraction, so every platform computes the same bits
add_library(smoke_vocr_rt STATIC ${RT}/vocr.c ${RT}/vocr_lm.c ${RT}/vocr_lines.c ${RT}/vocr_lines_rows.c
  ${RT}/vocr_lines_fix.c ${RT}/vocr_lines_refine.c ${RT}/vocr_textgate.c ${RT}/vocr_vertical.c ${RT}/vocr_post.c
  ${RT}/vocr_charbox.c ${SPOT_RT}/vocr_textspot6.c) # vocr_charbox.c: test/combined, draft #326
target_include_directories(smoke_vocr_rt PUBLIC ${RT} ${SPOT_RT})
target_compile_options(smoke_vocr_rt PRIVATE ${ARCH} -std=c99 -O2 -ffp-contract=off)
# the pinned spotter's form (include/vjo_vocr_sizes.h VJO_VOCR_SPOT_FORM), as cmake/VocrOCR.cmake builds it
file(STRINGS ${ROOT}/include/vjo_vocr_sizes.h _spot_form REGEX "^#define VJO_VOCR_SPOT_FORM \"trunk\"$")
if(_spot_form)
  target_compile_definitions(smoke_vocr_rt PUBLIC VOCR_TEXTSPOT6_NO_MS=1)
endif()
add_library(smoke_bearssl STATIC ${ROOT}/third_party/bearssl/src/hash/sha2small.c
  ${ROOT}/third_party/bearssl/src/codec/dec32be.c ${ROOT}/third_party/bearssl/src/codec/enc32be.c)
target_include_directories(smoke_bearssl PRIVATE ${ROOT}/third_party/bearssl/inc ${ROOT}/third_party/bearssl/src)
target_compile_options(smoke_bearssl PRIVATE ${ARCH} -O2 -w)

# the game worker as vita/CMakeLists.txt links it (VitaJPOverlay_OCR, with cmake/VocrOCR.cmake's vjo_vocr), beside
# main.c's kernel in process
add_executable(vjo_smoke main.c ${ROOT}/game_ocr/main.c ${ROOT}/core/vocr_ocr.c ${ROOT}/core/ledger.c
  ${ROOT}/core/games.c ${ROOT}/core/textnorm.c ${ROOT}/core/region_suggest.c ${ROOT}/core/jpegsw.c
  ${ROOT}/core/shottrim.c ${ROOT}/core/arena.c ${ROOT}/game_ocr/hooks.c ${ROOT}/game_ocr/hooks_observer.S
  ${ROOT}/core/hook_place.c ${ROOT}/core/hook_stream.c ${ROOT}/core/hook_luna.c ${ROOT}/core/hook_re.c
  ${ROOT}/core/hook_text.c ${ROOT}/core/hook_sc3.c ${ROOT}/core/sfo.c ${ROOT}/core/hook_select.c
  ${ROOT}/core/hook_cache.c
  # test/combined: draft #318's PSP text and draft #326's char map (its writer and the UTF-8 it reads)
  ${ROOT}/core/psp_text.c ${ROOT}/core/charmap.c ${ROOT}/core/charmap_build.c ${ROOT}/core/utf.c)
target_include_directories(vjo_smoke PRIVATE ${ROOT}/core ${ROOT}/include ${RT} ${ROOT}/third_party/bearssl/inc)
target_compile_definitions(vjo_smoke PRIVATE VJO_GAME_OCR_NO_MODULE_ENTRY=1 VJO_WITH_VOCR=1)
target_compile_options(vjo_smoke PRIVATE ${ARCH} -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo_smoke smoke_vocr_rt smoke_bearssl m
  SceDisplay_stub SceSysmem_stub SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub ScePower_stub_weak
  SceProcessmgr_stub SceAppMgr_stub
  taihen_stub_weak)  # the hooks engine's (game_ocr/hooks.c), weak as the worker's: Vita3K has no taiHEN
vita_create_self(eboot.bin vjo_smoke)
vita_create_vpk(vjo_smoke.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME})
