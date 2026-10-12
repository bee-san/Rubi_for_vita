# PP-OCRv6 small engine (ocr_backend = ppocrv6), linked statically into the
# game worker: core/ppocrv6_ocr.cpp on the pinned ncnn (cmake/Ncnn.cmake) and
# vita-vn-ocr's line finder (cmake/VocrOCR.cmake). The param and classes are
# committed (third_party/ppocrv6); the int8 weights are a pinned release asset
# that the kernel model broker reads (tools/prepare_ppocrv6.py).
include(${CMAKE_CURRENT_LIST_DIR}/Ncnn.cmake)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(VJO_PPOCRV6_GENERATED "${CMAKE_BINARY_DIR}/ppocrv6")
set(_ppocrv6_data "${VJO_ROOT}/third_party/ppocrv6")
add_custom_command(OUTPUT "${VJO_PPOCRV6_GENERATED}/vjo_ppocrv6_data.h"
  COMMAND ${Python3_EXECUTABLE} "${VJO_ROOT}/tools/prepare_ppocrv6.py" header
    --ncnn-source "${ncnn_SOURCE_DIR}" --output "${VJO_PPOCRV6_GENERATED}/vjo_ppocrv6_data.h"
  DEPENDS "${VJO_ROOT}/tools/prepare_ppocrv6.py" "${VJO_ROOT}/tools/prepare_ocr.py"
    "${_ppocrv6_data}/PP-OCRv6_small_jis_int8.ncnn.param" "${_ppocrv6_data}/ppocrv6_jis_dict.txt"
  COMMENT "Generating the PP-OCRv6 small param and class header")

# Global operator new/delete for the executables that run PP-OCRv6.
add_library(vjo_ncnn_new STATIC "${VJO_ROOT}/core/ncnn_new.cpp")
target_link_libraries(vjo_ncnn_new PUBLIC vjo_ocr_heap)
target_compile_options(vjo_ncnn_new PRIVATE -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions>")

add_library(vjo_ppocrv6 STATIC "${VJO_ROOT}/core/ppocrv6_ocr.cpp"
  "${VJO_PPOCRV6_GENERATED}/vjo_ppocrv6_data.h")
target_include_directories(vjo_ppocrv6 PRIVATE "${VJO_PPOCRV6_GENERATED}" "${VJO_ROOT}/third_party/bearssl/inc")
target_include_directories(vjo_ppocrv6 PUBLIC "${VJO_ROOT}/core" "${VJO_ROOT}/include")
# core/net.h and ncnn/net.h share a basename: angle includes resolve to ncnn first.
target_include_directories(vjo_ppocrv6 BEFORE PRIVATE "${ncnn_SOURCE_DIR}/src" "${ncnn_BINARY_DIR}/src")
target_link_libraries(vjo_ppocrv6 PUBLIC ncnn vjo_vocr_rt vjo_ocr_heap vjo_ncnn_new)
target_compile_features(vjo_ppocrv6 PRIVATE cxx_std_11)
target_compile_options(vjo_ppocrv6 PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-fcheck-new;-include;${VJO_ROOT}/core/ncnn_alloc.h>")
if(VJO_BUILD_VITA)
  target_compile_options(vjo_ppocrv6 PRIVATE -O2 -fno-builtin -fshort-wchar -mfpu=neon)
  target_compile_options(vjo_ncnn_new PRIVATE -O2 -fno-builtin -fshort-wchar)
else()
  target_compile_definitions(vjo_ncnn_new PRIVATE VJO_HOST=1)
endif()
