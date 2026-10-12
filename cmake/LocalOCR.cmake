# Host-only PP-OCRv5 mobile experiment (VJO_WITH_NCNN) on the pinned ncnn.
include(${CMAKE_CURRENT_LIST_DIR}/Ncnn.cmake)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(VJO_OCR_ASSETS "${CMAKE_BINARY_DIR}/ocr-model")
add_custom_command(OUTPUT "${VJO_OCR_ASSETS}/vjo_ppocr_data.h"
  COMMAND ${Python3_EXECUTABLE} "${VJO_ROOT}/tools/prepare_ocr.py"
    --output "${VJO_OCR_ASSETS}" --ncnn-source "${ncnn_SOURCE_DIR}"
  DEPENDS "${VJO_ROOT}/tools/prepare_ocr.py"
  COMMENT "Preparing checksum-verified PP-OCRv5 mobile assets")
add_library(vjo_ocr STATIC "${VJO_ROOT}/core/local_ocr.cpp" "${VJO_OCR_ASSETS}/vjo_ppocr_data.h")
target_include_directories(vjo_ocr PRIVATE "${VJO_OCR_ASSETS}" "${VJO_ROOT}/third_party/bearssl/inc")
# core/net.h and ncnn/net.h have the same basename. Quoted local includes
# resolve beside the source; angle includes must resolve to ncnn first.
target_include_directories(vjo_ocr BEFORE PRIVATE "${ncnn_SOURCE_DIR}/src" "${ncnn_BINARY_DIR}/src")
target_link_libraries(vjo_ocr PUBLIC ncnn vjo_ocr_heap)
target_compile_features(vjo_ocr PRIVATE cxx_std_11)
target_compile_options(vjo_ocr PRIVATE -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-include;${VJO_ROOT}/core/ncnn_alloc.h>")
if(VJO_BUILD_VITA)
  target_compile_options(vjo_ocr PRIVATE -fno-builtin -fshort-wchar -mfpu=neon)
endif()
