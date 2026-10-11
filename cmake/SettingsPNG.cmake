# The pinned SDK package was built with a 92-byte ARM jmp_buf, while the
# current linked setjmp saves 104 bytes. Build the opaque libpng structures
# with this project's actual toolchain; never overwrite the global SDK.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(vjo_settings_png
  URL https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.58
  URL_HASH SHA256=a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_ARM_NEON on CACHE STRING "" FORCE)
FetchContent_MakeAvailable(vjo_settings_png)

# Geometry is compiled against the same generated configuration and private
# structures as png_static, then inspected without running code on the Vita.
# It is a build-only object and adds no bytes to the Settings executable.
add_library(vjo_settings_png_geometry OBJECT ${ROOT}/vita/png_jump_geometry.c)
target_include_directories(vjo_settings_png_geometry PRIVATE
  ${vjo_settings_png_SOURCE_DIR} ${vjo_settings_png_BINARY_DIR})
add_dependencies(vjo_settings_png_geometry png_static)
