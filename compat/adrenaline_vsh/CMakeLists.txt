cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
endif()
project(rubi_adrenaline_vsh C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_STANDARD 99)
add_executable(adrenaline_vsh main.c)
target_compile_options(adrenaline_vsh PRIVATE -Os -Wall -Wextra -fno-builtin -ffunction-sections -fdata-sections)
target_link_options(adrenaline_vsh PRIVATE -nostdlib -Wl,-q -Wl,--gc-sections -Wl,--undefined=module_stop)
target_link_libraries(adrenaline_vsh gcc taihen_stub SceAppMgr_stub SceLsdb_stub_weak SceIofilemgr_stub SceLibKernel_stub)
vita_create_self(adrenaline_vsh.suprx adrenaline_vsh CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml UNSAFE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(adrenaline_vsh_budget ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/check_adrenaline_vsh.py
    ${CMAKE_CURRENT_BINARY_DIR}/adrenaline_vsh.suprx
  DEPENDS adrenaline_vsh.suprx-self
  COMMENT "Checking Adrenaline's Shell bridge fits 8 KiB of resident pages")
