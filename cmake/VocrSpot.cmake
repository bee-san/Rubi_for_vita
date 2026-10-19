# Scan screen's text spotter weights (vita-vn-ocr #104) for the host tests that run it: the pinned release asset
# (include/vjo_vocr_sizes.h VJO_VOCR_SPOT_NAME; tools/prepare_ocr.py and core/vocr_ocr.c pin the same SHA-256),
# from -DVJO_VOCR_SPOT_BLOB=FILE or downloaded once into the build directory (a public release: no token), and
# checked, as cmake/VocrGate.cmake does for the gate's. Sets VJO_VOCR_SPOT_TEST_DIR to the directory holding it,
# or "" (with a warning) when it cannot be had: those tests then say so and skip. The plugins read the file at run
# time; nothing here is built in.
include_guard(GLOBAL)
set(VJO_VOCR_SPOT_NAME textspot_h2_s4_c24n4_w8.vtsp)
set(VJO_VOCR_SPOT_SHA256 852506919ba94e54a4efc55e3695f365626d9d4ce6347f420cbf2afe3e8360cf)
# Provisional until vita-vn-ocr #104 selects its model and publishes it (then the release's asset URL).
set(VJO_VOCR_SPOT_URL "https://github.com/bee-san/vita-vn-ocr/releases/download/textspot-provisional/${VJO_VOCR_SPOT_NAME}")
set(VJO_VOCR_SPOT_BLOB "" CACHE FILEPATH "A local copy of vita-vn-ocr's ${VJO_VOCR_SPOT_NAME} for scan screen's host tests")
set(VJO_VOCR_SPOT_TEST_DIR "")
set(_spot_dir "${CMAKE_BINARY_DIR}/vita-vn-ocr")
set(_spot_file "${_spot_dir}/${VJO_VOCR_SPOT_NAME}")
if(VJO_VOCR_SPOT_BLOB)
  file(SHA256 "${VJO_VOCR_SPOT_BLOB}" _spot_got)
  if(NOT _spot_got STREQUAL VJO_VOCR_SPOT_SHA256)
    message(FATAL_ERROR "vita-vn-ocr: ${VJO_VOCR_SPOT_BLOB} has sha256 ${_spot_got}, expected ${VJO_VOCR_SPOT_SHA256}")
  endif()
  configure_file("${VJO_VOCR_SPOT_BLOB}" "${_spot_file}" COPYONLY)
elseif(NOT EXISTS "${_spot_file}")
  # No EXPECTED_HASH: with it, a failed download is a CMake error. The file is hashed below, and refused.
  file(DOWNLOAD "${VJO_VOCR_SPOT_URL}" "${_spot_file}" STATUS _spot_dl)
  list(GET _spot_dl 0 _spot_dl_rc)
  if(NOT _spot_dl_rc EQUAL 0)
    file(REMOVE "${_spot_file}")
    message(WARNING "vita-vn-ocr: cannot download ${VJO_VOCR_SPOT_URL} (${_spot_dl}): scan screen's host tests "
      "skip. Pass -DVJO_VOCR_SPOT_BLOB=/path/to/${VJO_VOCR_SPOT_NAME} to run them.")
  endif()
endif()
if(EXISTS "${_spot_file}")
  file(SHA256 "${_spot_file}" _spot_got)
  if(NOT _spot_got STREQUAL VJO_VOCR_SPOT_SHA256)
    file(REMOVE "${_spot_file}") # not kept: the next configure downloads it again
    message(FATAL_ERROR "vita-vn-ocr: ${_spot_file} has sha256 ${_spot_got}, expected ${VJO_VOCR_SPOT_SHA256}")
  endif()
  set(VJO_VOCR_SPOT_TEST_DIR "${_spot_dir}")
  message(STATUS "vita-vn-ocr text spotter (host tests): ${_spot_file}")
endif()
