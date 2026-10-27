# vita-vn-ocr, the overlay's only OCR engine: bee-san/vita-vn-ocr's plain-C99
# recognizer and dialogue line finder, plus core/vocr_ocr.c. The runtime is not
# copied into this repository: point VJO_VOCR_SOURCE_DIR at a checkout of the
# pinned commit (or place its runtime/ files under third_party/vita-vn-ocr/).
# Every file is checked against the hash it was evaluated with.
set(VJO_VOCR_COMMIT f8a1856c1b21bf41af744c159b54ce8203408a89)
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
# vocr_post.c (vita-vn-ocr #109) is the counter-context re-decode over the
# same top-K (core/vocr_ocr.c finish_line, before the LM) and the counter rule
# that core/textnorm.c mirrors (ッ月 -> ヶ月). vocr.c skips the output head on the frames
# a blob's head gate calls blank (vita-vn-ocr #120, VjoVocrPolicy.head_gate) and computes
# its rare rows only where a blob's two-level head needs them (#122, VjoVocrPolicy.two_level);
# since #123 it also parses width strides up to 3 (stride-12 blobs; none is pinned) and
# refuses a kernel narrower than its stride. Its gate and two-level code is #122's (fb63532). Since #130
# (VOCR6's stage-3 candidate) it also runs a light global-attention op (op type 5) in a blob that has one; no
# pinned blob has one, and every other blob reads bit for bit as before.
# vocr_vertical.c (vita-vn-ocr #99, merged by #103 and unchanged since: the same
# file at this pin) finds the columns of vertical text (GRAD on the region's
# transpose, right to left) and turns a column 90 degrees anticlockwise for the
# vertical recognizer (config.ini ocr_vertical, docs/vita-vn-ocr.md "Vertical text").
# vocr_charbox.c (vita-vn-ocr #114, unchanged since 6f24967) places each character a line's run emits on
# the line, from the CTC frame it fires at (core/vocr_ocr.c line_chars, API 46's overlay on the game's
# text; docs/overlay-on-text.md).
set(_vocr_pins
  vocr.c e2d43c501e8cec076fd21539feb03b1178bb920cc85782cba0e6422c66d9cd60
  vocr.h c9670efd980a993f98307e6093ca17c7595f64a09c11a3d4c7bb6cea84993de5
  vocr_lines.c 90c30a1c1e1707b818c2bd919243ee589700ff44ceac8b8d7a842b9d55d8d9ca
  vocr_lines.h 4f768c9a7aaa1f9e3b8f16990be23c3b098b675cf6316df7eb20dc6198b9f37d
  vocr_lines_rows.c 3207edb85d1cd7c79899390bb2ee028839dd120ca5d2278a28edd5fa9ca2ee6e
  vocr_lines_rows.h 60967239238df1ecca767e95c9c752f4c0b9efba097a59034ae2326717ad3465
  vocr_lines_fix.c 242a0a9eacaf38dc901eb35c54639f69c94cc0419eeb36467dfc74ab79c70fb4
  vocr_lines_fix.h d28af14903e1604006edbb75dc5d939ac845651220fa661c0e7fc7a1cec9efe4
  vocr_lm.c 89e0494caf22090cab52131b8b9d489a26085300b61e59855cb9a0d4074dbd05
  vocr_lm.h 6d314c7fbc74c240b4e6d70c0dc0a9e1c98fcb196ecd6bc1aa2ccf5c8fb9fb86
  vocr_lines_refine.c f4fb08aa995379c5ee9cb746307ed2c2d357574d71e5556f2b40d8ce1584e797
  vocr_lines_refine.h 66fe3b716193d835e9ab1d614d71ad8e19de70db5ac8ec0f09b8e027b260dc6b
  vocr_textgate.c da772dd316b69ac79dc0d6ffbae14da428df429657d089fd99ba8cd74c43266e
  vocr_textgate.h aeb8c5bdb810e8ab82bfaa37aadc1d564ec0dec2a80b1e2f80323819853d7258
  vocr_post.c 4636baa19b2ef991ab4aa306d606db1c0b2bd71fbc09df9ce2619bba458b95f4
  vocr_post.h 9f1cf5735a2fbabda104536603b3f209d3a52b6d8d76954b07382e8cc314b43a
  vocr_vertical.c 7e11ef75cd5308daac4323e58961f62ae2df537f6170004c2cc387f341788b54
  vocr_vertical.h ccc2a658a34b42e16071898aaa560765b4033fcecc5e1d4740defdc641b6c96e
  vocr_charbox.c 0e338f516a59529507ce9451198b692acd26a26267105e9efba2d58a18750018
  vocr_charbox.h da5209e372dbdf4efff3e5da3c05a424becbb860b23ebe64bab984af534ff5c9)
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

# vocr_textspot6.c (vita-vn-ocr #128 and #149; docs/textspot6.md, docs/textspot8.md) is the text spotter, the game
# worker's default finder (config.ini ocr_finder = spotter; core/vocr_ocr.c reads its boxes as the lines). It is pinned
# apart from the recognizer's files: its sha256 at vita-vn-ocr main's merge of textspot6 (#147, 4674e54), whose
# runtime holds the spotter's files that VJO_VOCR_COMMIT predates, until the recognizer's pin reaches a commit that
# holds them (then VJO_VOCR_SPOT_COMMIT is VJO_VOCR_COMMIT and the source below is the same checkout). Its headers
# are read from its own directory (vocr_textspot6.h includes vocr_textspot.h, which includes vocr_lines.h and
# vocr_lines_rows.h), so those are pinned there too, at the hashes the recognizer's checkout has. The source: an
# explicit VJO_VOCR_SPOT_SOURCE_DIR (a checkout of VJO_VOCR_SPOT_COMMIT), else VJO_VOCR_SOURCE_DIR when it holds the
# five files at these pins, else .refs/vita-vn-ocr-spot, else the five files at VJO_VOCR_SPOT_COMMIT downloaded from
# the public repository into the build directory, each checked against its hash (tools/fast_gate.sh and CI pass a
# checkout; other tools need nothing new). The five files are byte-identical at the earlier pin a54a5b2.
set(VJO_VOCR_SPOT_COMMIT 4674e54b79ac87d9dbcc3e7ba2bfc670c9e3b52c)
set(VJO_VOCR_SPOT_SOURCE_DIR "" CACHE PATH "Checkout of github.com/bee-san/vita-vn-ocr at ${VJO_VOCR_SPOT_COMMIT} (the text spotter)")
set(_vocr_spot_pins
  vocr_textspot6.c 678e88b60a8347b2019c054f74eb9c741b0bf871b97505f70c682a8e34bb018c
  vocr_textspot6.h 526d6d19796c3aa314d3ec14655129c5ef2dbab75125d2b31225c7e78620b512
  vocr_textspot.h a25ef000164af61610a6f5201fb9e345f8a67cf6525d2f16bae44afca712dd57
  vocr_lines.h 4f768c9a7aaa1f9e3b8f16990be23c3b098b675cf6316df7eb20dc6198b9f37d
  vocr_lines_rows.h 60967239238df1ecca767e95c9c752f4c0b9efba097a59034ae2326717ad3465)
# _vjo_spot_dir(DIR OUT): OUT is TRUE when DIR/runtime holds every file of _vocr_spot_pins at its pin.
function(_vjo_spot_dir dir out)
  set(_pins ${_vocr_spot_pins})
  set(_ok TRUE)
  if(NOT dir)
    set(_ok FALSE)
  endif()
  while(_ok AND _pins)
    list(POP_FRONT _pins _file _want)
    if(NOT EXISTS "${dir}/runtime/${_file}")
      set(_ok FALSE)
    else()
      file(SHA256 "${dir}/runtime/${_file}" _got)
      if(NOT _got STREQUAL _want)
        set(_ok FALSE)
      endif()
    endif()
  endwhile()
  set(${out} ${_ok} PARENT_SCOPE)
endfunction()
set(_spot_dir "")
if(VJO_VOCR_SPOT_SOURCE_DIR)
  _vjo_spot_dir("${VJO_VOCR_SPOT_SOURCE_DIR}" _spot_ok)
  if(NOT _spot_ok)
    if(VJO_VOCR_ALLOW_UNPINNED)
      message(WARNING "vita-vn-ocr: the text spotter's files in ${VJO_VOCR_SPOT_SOURCE_DIR} differ from ${VJO_VOCR_SPOT_COMMIT} "
        "(unpinned build)")
    else()
      message(FATAL_ERROR "vita-vn-ocr: ${VJO_VOCR_SPOT_SOURCE_DIR}/runtime does not hold the text spotter's files at "
        "their pins (commit ${VJO_VOCR_SPOT_COMMIT}). Check out that commit, or set VJO_VOCR_ALLOW_UNPINNED=ON.")
    endif()
  endif()
  set(_spot_dir "${VJO_VOCR_SPOT_SOURCE_DIR}")
else()
  foreach(_cand "${VJO_VOCR_SOURCE_DIR}" "${VJO_ROOT}/.refs/vita-vn-ocr-spot")
    if(NOT _spot_dir)
      _vjo_spot_dir("${_cand}" _spot_ok)
      if(_spot_ok)
        set(_spot_dir "${_cand}")
      endif()
    endif()
  endforeach()
endif()
if(NOT _spot_dir)
  set(_spot_dir "${CMAKE_BINARY_DIR}/vita-vn-ocr-spot")
  set(_pins ${_vocr_spot_pins})
  while(_pins)
    list(POP_FRONT _pins _file _want)
    set(_dst "${_spot_dir}/runtime/${_file}")
    if(EXISTS "${_dst}")
      file(SHA256 "${_dst}" _got)
      if(NOT _got STREQUAL _want)
        file(REMOVE "${_dst}")
      endif()
    endif()
    if(NOT EXISTS "${_dst}")
      set(_url "https://raw.githubusercontent.com/bee-san/vita-vn-ocr/${VJO_VOCR_SPOT_COMMIT}/runtime/${_file}")
      file(DOWNLOAD "${_url}" "${_dst}" STATUS _dl)
      list(GET _dl 0 _dl_rc)
      if(_dl_rc EQUAL 0)
        file(SHA256 "${_dst}" _got)
      endif()
      if(NOT _dl_rc EQUAL 0 OR NOT _got STREQUAL _want)
        file(REMOVE "${_dst}")
        message(FATAL_ERROR "vita-vn-ocr: the text spotter's runtime/${_file} (commit ${VJO_VOCR_SPOT_COMMIT}) is not in "
          "VJO_VOCR_SOURCE_DIR, and ${_url} could not be had (${_dl}). Clone it:\n"
          "  git clone https://github.com/bee-san/vita-vn-ocr /path/to/vita-vn-ocr-spot\n"
          "  git -C /path/to/vita-vn-ocr-spot checkout ${VJO_VOCR_SPOT_COMMIT}\n"
          "then configure with -DVJO_VOCR_SPOT_SOURCE_DIR=/path/to/vita-vn-ocr-spot (docs/vita-vn-ocr.md).")
      endif()
    endif()
  endwhile()
endif()
set(_vocr_spot_rt "${_spot_dir}/runtime")
message(STATUS "vita-vn-ocr text spotter: ${_vocr_spot_rt} (pinned ${VJO_VOCR_SPOT_COMMIT})")
# core/vocr_ocr.c includes "vocr_textspot6.h" through the include path, the recognizer's directory first (its vocr.h
# is the one vocr.c is built from): a vocr_textspot6.h there must be the pinned one.
if(EXISTS "${_vocr_rt}/vocr_textspot6.h" AND NOT _vocr_rt STREQUAL _vocr_spot_rt)
  file(SHA256 "${_vocr_rt}/vocr_textspot6.h" _got)
  list(GET _vocr_spot_pins 3 _want)
  if(NOT _got STREQUAL _want AND NOT VJO_VOCR_ALLOW_UNPINNED)
    message(FATAL_ERROR "vita-vn-ocr: ${_vocr_rt}/vocr_textspot6.h is not the pinned text spotter's header "
      "(${VJO_VOCR_SPOT_COMMIT}); core/vocr_ocr.c would read it before ${_vocr_spot_rt}'s")
  endif()
endif()
# VOCR_TEXTSPOT6_NO_MS (vocr_textspot6.h) leaves out every multi-scale (VTMS) path, its map ops, their kernels and
# their arena plan: a build for a trunk blob needs none of it (Vita -O2: 13,020 B of the spotter's text against
# 24,964 B, vita-vn-ocr eval/textspot6_a9_20261026.json code_size), and refuses a multi-scale blob at load (the job
# then reads with GRAD). Its default follows the pinned blob's form (include/vjo_vocr_sizes.h VJO_VOCR_SPOT_FORM).
file(STRINGS "${VJO_ROOT}/include/vjo_vocr_sizes.h" _spot_form REGEX "^#define VJO_VOCR_SPOT_FORM \"(trunk|ms)\"$")
string(REGEX REPLACE "^#define VJO_VOCR_SPOT_FORM \"(trunk|ms)\"$" "\\1" _spot_form "${_spot_form}")
if(NOT _spot_form MATCHES "^(trunk|ms)$")
  message(FATAL_ERROR "include/vjo_vocr_sizes.h: no VJO_VOCR_SPOT_FORM \"trunk\" or \"ms\" line")
endif()
if(_spot_form STREQUAL "trunk")
  set(_spot_no_ms_default ON)
else()
  set(_spot_no_ms_default OFF)
endif()
option(VJO_VOCR_SPOT_NO_MS "Build the text spotter without its multi-scale networks (-DVOCR_TEXTSPOT6_NO_MS): a trunk blob only"
  ${_spot_no_ms_default})
if(VJO_VOCR_SPOT_NO_MS AND _spot_form STREQUAL "ms")
  message(FATAL_ERROR "VJO_VOCR_SPOT_NO_MS=ON, but the pinned spotter is a multi-scale network (VJO_VOCR_SPOT_FORM \"ms\"): "
    "every job would refuse it and read with GRAD")
endif()

# runtime/README.md, "Porting notes": C99, -O2, no FP contraction, so every
# platform computes the same bits; the Vita build adds the Cortex-A9 NEON flags.
# Its few libc calls (memcpy, memset, the fabsf/signbit builtins) need no
# newlib, which SceShell plugins link without.
# vocr_textgate.c (vita-vn-ocr #100) is the text-presence gate; its weights are a pinned file beside the models
# (include/vjo_vocr_sizes.h VJO_VOCR_GATE_NAME, tools/prepare_ocr.py), read at run time.
# vocr_lines_fix.c (vita-vn-ocr #129, API 50) is vocr_lines_rows.c with the line finder's fixes behind flags
# (core/vocr_ocr.h VjoVocrPolicy.line_fix); flags 0 give vocr_lines_rows.c's lines bit for bit.
# vocr_textspot6.c (vita-vn-ocr #128, #149) is the text spotter, the default finder (API 54); its weights are a
# pinned file beside the models too (VJO_VOCR_SPOT_NAME), read at run time. Only the game worker links this library
# on the Vita: the Shell links no OCR runtime (vita/CMakeLists.txt), so none of it is ever in SceShell.
add_library(vjo_vocr_rt STATIC "${_vocr_rt}/vocr.c" "${_vocr_rt}/vocr_lines.c" "${_vocr_rt}/vocr_lines_rows.c"
  "${_vocr_rt}/vocr_lines_fix.c" "${_vocr_rt}/vocr_lm.c" "${_vocr_rt}/vocr_lines_refine.c" "${_vocr_rt}/vocr_textgate.c"
  "${_vocr_rt}/vocr_post.c" "${_vocr_rt}/vocr_vertical.c" "${_vocr_rt}/vocr_charbox.c" "${_vocr_spot_rt}/vocr_textspot6.c")
target_include_directories(vjo_vocr_rt PUBLIC "${_vocr_rt}" "${_vocr_spot_rt}")
if(VJO_VOCR_SPOT_NO_MS)
  # PUBLIC: vocr_textspot6.h's types do not change with it, but core/vocr_ocr.c checks the form it was built for
  target_compile_definitions(vjo_vocr_rt PUBLIC VOCR_TEXTSPOT6_NO_MS=1)
endif()
target_compile_options(vjo_vocr_rt PRIVATE -std=c99 -O2 -ffp-contract=off -Wall -Wextra
  -ffunction-sections -fdata-sections)
if(VJO_BUILD_VITA)
  target_compile_options(vjo_vocr_rt PRIVATE -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  # The counter re-decode is a line's control code, a few thousand integer steps, not per-pixel work:
  # built for size like the job's (the worker's pages; vita-vn-ocr docs/counters.md: 2,598 B at -Os).
  # The column finder and rotation (vertical text, config.ini ocr_vertical; only its jobs run them): built
  # for size too. Integer arithmetic only, so -Os computes the same columns: 3,220 B of code against
  # 4,264 B at -O2 (arm-vita-eabi-gcc, the VitaSDK of CI's container).
  set_source_files_properties("${_vocr_rt}/vocr_post.c" "${_vocr_rt}/vocr_vertical.c" PROPERTIES
    COMPILE_OPTIONS "-Os")
  # API 46: the characters' places, a line's control code too (vita-vn-ocr runtime/README.md: 3,186 B at -Os).
  set_source_files_properties("${_vocr_rt}/vocr_charbox.c" PROPERTIES COMPILE_OPTIONS "-Os")
else()
  target_link_libraries(vjo_vocr_rt PUBLIC m)
endif()


# core/textnorm.c: the text the game worker returns, normalised (core/textnorm.h). core/region_suggest.c (API 52,
# #358): the text box a job on the whole frame suggests, and each line's kana count it reads. core/charmap_build.c
# (API 46): the char map the worker writes after the text (core/charmap.h; its reader core/charmap.c and
# core/utf.c are the worker's own sources on the Vita, vjo_core's on the host).
add_library(vjo_vocr STATIC "${VJO_ROOT}/core/vocr_ocr.c" "${VJO_ROOT}/core/textnorm.c"
  "${VJO_ROOT}/core/region_suggest.c" "${VJO_ROOT}/core/charmap_build.c")
target_link_libraries(vjo_vocr PUBLIC vjo_vocr_rt)
target_compile_options(vjo_vocr PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
if(VJO_BUILD_VITA)
  target_compile_options(vjo_vocr PRIVATE -O2 -fno-builtin -fshort-wchar)
  # The job's control code (the capture's row checks, the wait, residency, the LM
  # and cascade set-up, a few hundred bytes of text normalisation once per job):
  # built for size, the worker's pages. The per-pixel work, the recognizer and
  # the decoder, is the runtime's (vjo_vocr_rt, -O2).
  set_source_files_properties("${VJO_ROOT}/core/vocr_ocr.c" "${VJO_ROOT}/core/textnorm.c"
    "${VJO_ROOT}/core/region_suggest.c" "${VJO_ROOT}/core/charmap_build.c" PROPERTIES
    COMPILE_OPTIONS "-Os")
endif()
