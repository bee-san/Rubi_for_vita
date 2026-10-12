# CPU-only, pinned ncnn shared by the host-only PP-OCRv5 experiment
# (VJO_WITH_NCNN, cmake/LocalOCR.cmake) and the game worker's PP-OCRv6 engine
# (VJO_WITH_PPOCRV6, cmake/PPOCRv6.cmake). No OpenCV, ONNX, pthread/OpenMP or
# GPU dependencies. Only the operators those graphs use are compiled.
include_guard(GLOBAL)
include(FetchContent)
set(VJO_NCNN_REVISION 3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82)
FetchContent_Declare(ncnn
  URL https://github.com/Tencent/ncnn/archive/${VJO_NCNN_REVISION}.tar.gz
  URL_HASH SHA256=276ed0523e7a80048957c2bff89f37242cadd8858ed026a5538222af338e9fde)
FetchContent_GetProperties(ncnn)
if(NOT ncnn_POPULATED)
  # Populate before add_subdirectory so only the required operators are built.
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(ncnn)
endif()
set(_ncnn_layers)
if(VJO_WITH_NCNN)
  list(APPEND _ncnn_layers BinaryOp HardSwish Convolution ConvolutionDepthWise Split Swish LayerNorm Gemm
    Reshape Pooling Permute HardSigmoid MultiHeadAttention Input Concat Squeeze Softmax
    Padding Packing Cast Flatten InnerProduct ReLU Clip Sigmoid Mish TanH)
endif()
if(VJO_WITH_PPOCRV6)
  # The PP-OCRv6 small param's layer types (third_party/ppocrv6), the ReLU its
  # convolutions fuse, the Softmax inside MultiHeadAttention, the layout and
  # shape helpers ncnn's Mat functions create (Packing, Cast, Flatten, Crop)
  # and the three layers ncnn's int8 kernels create internally.
  list(APPEND _ncnn_layers Input Padding Pooling Concat Convolution ConvolutionDepthWise Split BinaryOp
    GELU LayerNorm HardSigmoid Gemm Swish Sigmoid Permute Reshape MultiHeadAttention Squeeze ReLU
    Softmax Packing Cast Flatten Crop Quantize Dequantize Requantize)
endif()
list(REMOVE_DUPLICATES _ncnn_layers)
file(STRINGS "${ncnn_SOURCE_DIR}/src/CMakeLists.txt" _ncnn_layer_lines REGEX "^ncnn_add_layer\\(")
foreach(_line IN LISTS _ncnn_layer_lines)
  string(REGEX REPLACE "^ncnn_add_layer\\(([A-Za-z0-9]+).*" "\\1" _layer "${_line}")
  string(TOLOWER "${_layer}" _lower)
  if(_layer IN_LIST _ncnn_layers)
    set(WITH_LAYER_${_lower} ON CACHE BOOL "" FORCE)
  else()
    set(WITH_LAYER_${_lower} OFF CACHE BOOL "" FORCE)
  endif()
endforeach()
# int8 kernels only when the int8 PP-OCRv6 graph needs them. SSE2-only on x86
# hosts: no runtime CPU dispatch, so every host computes the same results.
set(_ncnn_off VULKAN OPENMP THREADS STDIO STRING BENCHMARK C_API PLATFORM_API BATCH
    PIXEL_ROTATE PIXEL_AFFINE PIXEL_DRAWING BUILD_TESTS BUILD_TOOLS BUILD_EXAMPLES
    BUILD_BENCHMARK PYTHON INSTALL_SDK RUNTIME_CPU WEIGHT_QUANT BF16 VFPV4
    AVX AVX2 AVX512 AVXVNNI AVXVNNIINT8 AVXVNNIINT16 AVXNECONVERT
    AVX512VNNI AVX512BF16 AVX512FP16 FMA FMA4 F16C XOP)
if(VJO_WITH_PPOCRV6)
  set(NCNN_INT8 ON CACHE BOOL "" FORCE)
else()
  list(APPEND _ncnn_off INT8)
endif()
foreach(_off IN LISTS _ncnn_off)
  set(NCNN_${_off} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(_on SIMPLESTL SIMPLEMATH PIXEL DISABLE_RTTI DISABLE_EXCEPTION VALIDATION)
  set(NCNN_${_on} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${ncnn_SOURCE_DIR}" "${ncnn_BINARY_DIR}" EXCLUDE_FROM_ALL)
get_target_property(_ncnn_interface_options ncnn INTERFACE_COMPILE_OPTIONS)
list(TRANSFORM _ncnn_interface_options REPLACE "^(-fno-rtti|-fno-exceptions)$" "$<$<COMPILE_LANGUAGE:CXX>:\\1>")
set_property(TARGET ncnn PROPERTY INTERFACE_COMPILE_OPTIONS "${_ncnn_interface_options}")
# Reuse the platform's small-object C++ allocation operators. In SceShell
# those belong to Paf; defining ncnn's global operators would replace them.
# The game worker supplies its own (core/ncnn_new.cpp).
get_target_property(_ncnn_sources ncnn SOURCES)
list(FILTER _ncnn_sources EXCLUDE REGEX "(^|/)simplestl\\.cpp$")
list(FILTER _ncnn_sources EXCLUDE REGEX "(^|/)expression\\.cpp$")
set_property(TARGET ncnn PROPERTY SOURCES "${_ncnn_sources}")
target_sources(ncnn PRIVATE "${VJO_ROOT}/core/ncnn_placement.cpp" "${VJO_ROOT}/core/ncnn_expression.cpp")
# -fcheck-new: an exhausted private heap makes `new` return NULL, which ncnn
# checks (create_layer), instead of constructing at address zero.
target_compile_options(ncnn PRIVATE -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:CXX>:-fcheck-new;-fno-threadsafe-statics>"
  -include "${VJO_ROOT}/core/ncnn_alloc.h")
set_target_properties(ncnn PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
if(VJO_BUILD_VITA OR CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7")
  # ncnn's armv7 inline assembly needs every register; -fPIC reserves one.
  set_target_properties(ncnn PROPERTIES POSITION_INDEPENDENT_CODE OFF INTERFACE_POSITION_INDEPENDENT_CODE OFF)
endif()
if(VJO_BUILD_VITA)
  target_compile_options(ncnn PRIVATE -fno-builtin -fshort-wchar -mfpu=neon)
endif()

# The private heap ncnn's remapped malloc/free use (core/ncnn_alloc.h).
add_library(vjo_ocr_heap STATIC "${VJO_ROOT}/core/ocr_heap.c")
target_include_directories(vjo_ocr_heap PUBLIC "${VJO_ROOT}/core")
target_compile_options(vjo_ocr_heap PRIVATE -ffunction-sections -fdata-sections)
if(VJO_BUILD_VITA)
  target_compile_options(vjo_ocr_heap PRIVATE -O2 -fno-builtin -fshort-wchar)
else()
  target_compile_definitions(vjo_ocr_heap PRIVATE VJO_HOST=1)
endif()
target_link_libraries(ncnn PUBLIC vjo_ocr_heap)
