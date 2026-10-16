# Vita build: kernel plugin (.skprx), SceShell plugin (.suprx), RCO and the
# release zip. Configure with:
#   cmake -S vita -B build/vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
#     -DCMAKE_BUILD_TYPE=Release -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
#     -DVJO_PLAINTEXT_SOURCE=/path/to/plaintext-dicts
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaJPOverlay C CXX ASM)
# Release, as CI builds it, when no build type is given: libpng without optimisation (no build
# type, or Debug) fails the Overlay app's PNG ABI check (tools/check_settings_png_abi.py).
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Release (CI's); Debug fails the PNG ABI check" FORCE)
endif()
# taihen's TAI_CONTINUE relies on K&R-style `()` casts, which C23 (GCC 15 default) rejects.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VJO_ELF_FLAGS "${VITA_ELF_CREATE_FLAGS}")
set(VJO_SELF_FLAGS "${VITA_MAKE_FSELF_FLAGS}")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(VJO_ROOT ${ROOT})
set(VJO_BUILD_VITA ON)
option(VJO_WITH_NCNN "Build the CPU-only local mobile OCR recognizer" OFF)
option(VJO_WITH_VOCR "Build the vita-vn-ocr local OCR backend (needs its runtime sources)" ON)
# The release's version: the base zips' names and the Overlay app's version (beside its title). The
# test builds are v0.8.0-test.N packages of these binaries; the final release sets it to its own
# version first (docs/release-variants.md), and tools/package_variants.py checks the two agree.
set(VJO_VERSION "0.8.0" CACHE STRING "Release archive version")
# The build variant (docs/release-variants.md). ON, the default, is the debug build, the test
# builds' own: every log line, the self-test, the timing and trace lines, the game worker's step
# log. OFF is the normal build, the release's main download: errors, warnings and a short log;
# the rest is compiled out of all three plugins and the Overlay app, format strings included.
# Both read the same config.ini; the release ships both (target release_variants).
option(VJO_DEBUG "Build the debug variant (the test builds'); OFF: the normal release build" ON)
if(VJO_DEBUG)
  add_compile_definitions(VJO_DEBUG=1)
  set(VJO_VARIANT debug)
  set(VJO_VARIANT_SUFFIX "-debug")
else()
  add_compile_definitions(VJO_DEBUG=0)
  set(VJO_VARIANT normal)
  set(VJO_VARIANT_SUFFIX "")
endif()
message(STATUS "Vita JP Overlay ${VJO_VERSION}: the ${VJO_VARIANT} build (VJO_DEBUG=${VJO_DEBUG})")


# ---------------- kernel ----------------
add_executable(VitaJPOverlay_Kernel
  ${ROOT}/kernel/main.c
  ${ROOT}/kernel/boot.c
  ${ROOT}/kernel/ledger.c
  $<$<BOOL:${VJO_DEBUG}>:${ROOT}/kernel/meminfo.c> # the self-test's memory sample: debug build only
  ${ROOT}/core/boot_guard.c
  ${ROOT}/core/ledger.c
  ${ROOT}/kernel/log.c
  ${ROOT}/kernel/buffers.c
  ${ROOT}/kernel/capture.c
  ${ROOT}/kernel/capture_request.c
  ${ROOT}/kernel/game_ocr.c
  ${ROOT}/kernel/display.c
  ${ROOT}/kernel/input.c
  ${ROOT}/kernel/lifecycle.c
  ${ROOT}/kernel/foreground.c
  ${ROOT}/kernel/scene.c
  ${ROOT}/kernel/triggers.c)
target_compile_options(VitaJPOverlay_Kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu99 -fno-builtin
  -mgeneral-regs-only)
# The OCR job and model-file broker is a cold path (a few syscalls per job, a
# rows call a frame at most; the row copy itself is kernel/capture.c's): built
# for size, since every page of the kernel module is the shared kernel pool's.
# So are the process event hook and the foreground state machine (a few calls
# per app switch) and the self-test's memory sample (kernel/meminfo.c, one
# vjoMemInfo call per report). They pay for #104's release of the game's input
# block in the hook (lifecycle.c, +464 B at -O2) within the module's pages.
set_source_files_properties(${ROOT}/kernel/game_ocr.c ${ROOT}/kernel/lifecycle.c ${ROOT}/kernel/foreground.c
  ${ROOT}/kernel/meminfo.c PROPERTIES COMPILE_OPTIONS "-Os")

target_link_options(VitaJPOverlay_Kernel PRIVATE -nostdlib -Wl,-q)
target_link_libraries(VitaJPOverlay_Kernel
  gcc
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceThreadmgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceCpuForDriver_stub
  SceDisplayForDriver_stub
  SceCtrlForDriver_stub
  SceTouchForDriver_stub
  SceIofilemgrForDriver_stub)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET VitaJPOverlay_Kernel POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_kernel_instructions.py
    --objdump ${CMAKE_OBJDUMP} $<TARGET_FILE:VitaJPOverlay_Kernel>
  COMMENT "Checking kernel code contains no FPU or NEON instructions")
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(VitaJPOverlay_Kernel.skprx VitaJPOverlay_Kernel
  UNSAFE
  CONFIG ${ROOT}/kernel/exports.yml)
vita_create_stubs(vjo_kernel_stubs VitaJPOverlay_Kernel ${ROOT}/kernel/exports.yml KERNEL)

# ---------------- shell ----------------
# BearSSL's SHA-1 and SHA-224/256 (all that is kept of it: the overlay is offline) and the
# portable core, built for SceShell (no newlib: see shell/libc_shim.c).
add_library(vjo_bearssl STATIC
  ${ROOT}/third_party/bearssl/src/hash/sha1.c ${ROOT}/third_party/bearssl/src/hash/sha2small.c
  ${ROOT}/third_party/bearssl/src/codec/dec32be.c ${ROOT}/third_party/bearssl/src/codec/enc32be.c)
target_include_directories(vjo_bearssl PUBLIC ${ROOT}/third_party/bearssl/inc PRIVATE ${ROOT}/third_party/bearssl/src)
target_compile_options(vjo_bearssl PRIVATE -O2 -w -fno-builtin -fshort-wchar -ffunction-sections -fdata-sections)

add_library(vjo_core_vita STATIC
  ${ROOT}/core/arena.c ${ROOT}/core/utf.c ${ROOT}/core/text_hit.c ${ROOT}/core/text_rows.c
  ${ROOT}/core/json.c
  ${ROOT}/core/textfilter.c ${ROOT}/core/entries.c
  ${ROOT}/core/config.c ${ROOT}/core/regions.c
  ${ROOT}/core/client.c ${ROOT}/core/styled.c
  ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c ${ROOT}/core/local_dict.c
  ${ROOT}/core/ocr_lines.c ${ROOT}/core/games.c ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c
  ${ROOT}/core/ledger.c ${ROOT}/core/app_stamp.c ${ROOT}/core/pspbubble.c)
target_include_directories(vjo_core_vita PUBLIC ${ROOT}/core ${ROOT}/include)
target_link_libraries(vjo_core_vita PUBLIC vjo_bearssl)
target_compile_options(vjo_core_vita PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -fno-builtin -fshort-wchar
  -ffunction-sections -fdata-sections)
# Cold core paths (configuration, parsers, queue files): built for
# size, since every page of the Shell module is SceShell's. Lookup and OCR text
# handling stay at -O2. The UTF-8 helpers (core/utf.c: a few
# microseconds a text or a key) are built for size since the self-test's own job
# and the first-scan timings (docs/first-ocr.md): 1,532 -> 1,136 B (the JPEG
# encoder, built for size then too, went with Google Lens). The property is the directory's, so the
# kernel's core/ledger.c and the game worker's core/ledger.c and core/games.c
# are -Os too (their pages unchanged; the ARM lane runs the ledger suites).
set_source_files_properties(${ROOT}/core/config.c ${ROOT}/core/ledger.c ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c
  ${ROOT}/core/json.c
  ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c ${ROOT}/core/regions.c
  ${ROOT}/core/games.c ${ROOT}/core/sfo.c ${ROOT}/core/client.c
  ${ROOT}/core/utf.c
  ${ROOT}/core/app_stamp.c
  ${ROOT}/core/pspbubble.c ${ROOT}/core/psptarget.c ${ROOT}/core/taicfg.c
  PROPERTIES COMPILE_OPTIONS "-Os")
if(VJO_WITH_NCNN)
  include(${ROOT}/cmake/LocalOCR.cmake)
  target_compile_definitions(vjo_core_vita PUBLIC VJO_WITH_NCNN=1)
  target_link_libraries(vjo_ocr PUBLIC vjo_core_vita)
endif()
if(VJO_WITH_VOCR)
  include(${ROOT}/cmake/VocrOCR.cmake)
  target_compile_definitions(vjo_core_vita PUBLIC VJO_WITH_VOCR=1)
  target_include_directories(vjo_vocr PUBLIC ${ROOT}/core ${ROOT}/include ${ROOT}/third_party/bearssl/inc)
endif()

add_executable(VitaJPOverlay_Shell
  ${ROOT}/shell/boot.c
  ${ROOT}/shell/guard.c
  ${ROOT}/shell/ledger.c
  $<$<BOOL:${VJO_DEBUG}>:${ROOT}/shell/selftest.c> # the on-device self-test: debug build only
  ${ROOT}/shell/main.cpp
  ${ROOT}/shell/memory.cpp
  ${ROOT}/shell/overlay.cpp
  ${ROOT}/core/region_select.c
  ${ROOT}/shell/strip.cpp
  ${ROOT}/shell/strip_view.c
  ${ROOT}/shell/worker.c
  ${ROOT}/shell/game_ocr_client.c
  ${ROOT}/shell/game.c
  ${ROOT}/shell/anki.c
  ${ROOT}/shell/anki_queue.c
  ${ROOT}/vita/games_storage.c
  ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/log_vita.c
  ${ROOT}/shell/files.c
  ${ROOT}/vita/games_storage.c
  ${ROOT}/vita/buttons_vita.c
  ${ROOT}/shell/libc_shim.c)
add_dependencies(VitaJPOverlay_Shell vjo_kernel_stubs)
target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_PAF_ALLOC=1)
# The overlay page (once a frame while it is open; paf's own calls dominate)
# and the region selector it draws: built for size, which gave the region's
# stick cursor its room in the module's pages (#81; overlay.cpp's text 11,608 B at -O2,
# 8,822 B at -Os with the cursor).
set_source_files_properties(${ROOT}/shell/overlay.cpp ${ROOT}/core/region_select.c
  PROPERTIES COMPILE_OPTIONS "-Os")
# Startup runs before Paf is ready; it only needs integer operations. Avoid
# compiler-generated VFP/NEON copies in the early bootstrap and status logger.
set_source_files_properties(${ROOT}/shell/boot.c ${ROOT}/shell/guard.c ${ROOT}/shell/log_vita.c
  PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only;-Os")
# Cold paths (bootstrap, guard, ledger, logging, file access, queue files, the
# worker's job control): every page of this module is SceShell's, so they are
# built for size. worker.c at -Os pays for the word lookup's ranking and log
# within the module's pages (docs/memory-ledger.md). The header's row
# navigation (#81: core/text_rows.c, once per arrow key) is paid for by the
# subtitle strip's layout (strip.cpp) and core/arena.c, whose allocation fast
# path is the same instructions at -Os (only VjoBuf's growth path and the
# inlined copies of it shrink).
set_source_files_properties(${ROOT}/shell/ledger.c ${ROOT}/shell/selftest.c ${ROOT}/shell/anki.c
  ${ROOT}/shell/files.c ${ROOT}/shell/game.c ${ROOT}/shell/worker.c
  ${ROOT}/shell/strip_view.c ${ROOT}/shell/game_ocr_client.c
  ${ROOT}/shell/anki_queue.c ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/strip.cpp ${ROOT}/core/text_rows.c ${ROOT}/core/arena.c
  PROPERTIES COMPILE_OPTIONS "-Os")
if(VJO_WITH_NCNN)
  target_link_libraries(VitaJPOverlay_Shell vjo_ocr ncnn)
endif()
# The Shell imports only the bounded job transport. No vocr inference is linked here.
# More cold code built for size, which pays for the open overlay's quiet refresh
# (#81 request 4) within the module's pages: the module and Paf allocator entry
# points (once per boot or allocation), the entry lists of a sentence lookup,
# the per-tap hit test, the overlay's text styling (once per render) and
# games.ini storage (the Settings app's copy too). Measured -O2 -> -Os: 54 + 176 + 279 + 108 + 454 + 133 B.
set_source_files_properties(${ROOT}/shell/main.cpp ${ROOT}/shell/memory.cpp ${ROOT}/core/entries.c
  ${ROOT}/core/text_hit.c ${ROOT}/core/styled.c ${ROOT}/vita/games_storage.c
  PROPERTIES COMPILE_OPTIONS "-Os")

target_compile_options(VitaJPOverlay_Shell PRIVATE -O2 -Wall -Wno-unused-parameter -fno-builtin -fshort-wchar
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-Wno-sign-compare;-Wno-unused-function>"
  "-D__declspec(x)=")
target_link_directories(VitaJPOverlay_Shell PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
target_link_options(VitaJPOverlay_Shell PRIVATE -nostdlib -nostartfiles -Wl,-q -Wl,--gc-sections
  -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning)
target_link_libraries(VitaJPOverlay_Shell
  vjo_core_vita
  VitaJPOverlay_Kernel_stub_weak  # weak: the shell still starts (and reports) without the kernel plugin
  taihen_stub
  ScePaf_stub_weak
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  SceProcessmgr_stub
  SceIofilemgr_stub
  SceVshBridge_stub
  # Weak: SceShell loads these after *main plugins start; the imports are
  # bound once the libraries load (as with ScePaf). No SceNet or SceNetCtl:
  # the overlay is offline.
  SceRtc_stub_weak
  SceAppMgr_stub_weak
  # Weak: the enter button's registry read (vita/buttons_vita.c); unbound, it
  # fails and × enters, as before.
  SceRegistryMgr_stub_weak
  vjo_bearssl  # again after the core: its integer division comes from libgcc
  gcc)
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(VitaJPOverlay_Shell.suprx VitaJPOverlay_Shell
  UNSAFE
  CONFIG ${ROOT}/shell/exports.yml)
add_custom_target(vjo_syscall_abi ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_vita_syscall_abi.py
    --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
    --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self
  COMMENT "Checking packaged syscall ABI and firmware-independent kernel imports")

# ---------------- game-process vocr worker ----------------
if(VJO_WITH_VOCR)
  add_executable(VitaJPOverlay_OCR
    ${ROOT}/game_ocr/main.c ${ROOT}/core/games.c ${ROOT}/core/ledger.c ${ROOT}/shell/libc_shim.c)
  add_dependencies(VitaJPOverlay_OCR vjo_kernel_stubs)
  target_compile_options(VitaJPOverlay_OCR PRIVATE -O2 -Wall -Wextra -fno-builtin -fshort-wchar
    -ffunction-sections -fdata-sections)
  # The worker's control code (a job's request, memory checks and ladder, one call
  # per strip, the probe) is built for size: the recognizer, the finder and the
  # decoder are vita-vn-ocr's runtime and core/vocr_ocr.c, built apart. -Os pays for
  # the model tiers' ladder (API 21: +788 B of text at -O2) within the module's
  # pages (game_ocr/main.c's text 12,276 B at -O2 before it, 9,164 B at -Os with it).
  set_source_files_properties(${ROOT}/game_ocr/main.c PROPERTIES COMPILE_OPTIONS "-Os")
  target_link_options(VitaJPOverlay_OCR PRIVATE -nostdlib -nostartfiles -Wl,-q -Wl,--gc-sections
    -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning)
  target_link_directories(VitaJPOverlay_OCR PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
  target_link_libraries(VitaJPOverlay_OCR vjo_vocr vjo_vocr_rt vjo_bearssl VitaJPOverlay_Kernel_stub_weak
    SceLibKernel_stub SceKernelThreadMgr_stub SceSysmem_stub_weak SceProcessmgr_stub_weak
    SceIofilemgr_stub SceAppMgr_stub_weak SceDisplay_stub_weak ScePower_stub_weak taihen_stub_weak gcc)
  set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
  set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
  vita_create_self(VitaJPOverlay_OCR.suprx VitaJPOverlay_OCR UNSAFE CONFIG ${ROOT}/game_ocr/exports.yml)
  add_custom_target(vjo_game_syscall_abi ALL
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_vita_syscall_abi.py
      --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
      --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
      --worker ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx
    DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self VitaJPOverlay_OCR.suprx-self)
  # The normal build's budgets are its pages exactly (docs/release-variants.md): a budget above them fails too.
  set(VJO_LEDGER_EXACT)
  if(NOT VJO_DEBUG)
    set(VJO_LEDGER_EXACT --exact)
  endif()
  # The memory ledger (docs/memory-ledger.md): allocation sites, worst cases
  # against the cited limits, and the packaged modules' pages against the
  # declared budget. Its table is this build's record of what we hold.
  add_custom_target(vjo_memory_ledger ALL
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_memory_ledger.py --variant ${VJO_VARIANT} ${VJO_LEDGER_EXACT}
      --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
      --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
      --worker ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx
    DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self VitaJPOverlay_OCR.suprx-self
    COMMENT "Checking the memory ledger against the packaged modules"
    USES_TERMINAL)
endif()

# ---------------- RCO ----------------
find_program(PSP2CXML psp2cxml-tool HINTS ${ROOT}/.refs/toolchain/psp2cxml-tool/mbuild ${ROOT}/.refs/psp2cxml-tool/mbuild ENV PATH REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/shell/rco/vitajpoverlay.xml ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.xml
  COMMAND ${PSP2CXML} ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.xml
  DEPENDS ${ROOT}/shell/rco/vitajpoverlay.xml
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_custom_target(vjo_rco ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco)

# ---------------- standalone game settings VPK ----------------
include(${ROOT}/cmake/SettingsPNG.cmake)
# The Anki page (settings/anki_page.c): the export of the queue (settings/anki_export.c) as Lapis
# notes in an .apkg (core/apkg.c: SQLite, ZIP and the Lapis note type, core/sqlw.c, zipw.c, lapis.c),
# made from the queue's records (core/anki_queue.c) with core/anki.c's fields. No network.
set(VJO_SETTINGS_ANKI_SRC
  ${ROOT}/settings/anki_page.c ${ROOT}/settings/anki_export.c
  ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c ${ROOT}/core/apkg.c ${ROOT}/core/sqlw.c ${ROOT}/core/zipw.c
  ${ROOT}/core/lapis.c ${ROOT}/core/json.c ${ROOT}/core/entries.c ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c)
set(VJO_SETTINGS_SRC
  ${ROOT}/settings/main.c ${ROOT}/settings/catalog.c ${ROOT}/settings/psp.c ${ROOT}/settings/preferences.c ${ROOT}/settings/engine.c
  ${ROOT}/settings/icons.c ${ROOT}/settings/text_layout.c ${ROOT}/settings/letter.c ${ROOT}/settings/app_check.c
  ${ROOT}/core/games.c ${ROOT}/core/sfo.c ${ROOT}/core/arena.c ${ROOT}/core/app_stamp.c ${ROOT}/core/pspbubble.c ${ROOT}/core/psptarget.c
  ${ROOT}/core/taicfg.c
  ${ROOT}/core/config.c ${ROOT}/core/utf.c ${ROOT}/core/local_dict.c ${ROOT}/core/vocr_models.c
  ${ROOT}/vita/games_storage.c ${ROOT}/vita/buttons_vita.c ${ROOT}/settings/sysapp.c ${VJO_SETTINGS_ANKI_SRC})
# vjo_bearssl (SHA-256 for the queue's IDs and the screenshots' names, SHA-1 for Anki's field
# checksum) is the Shell's build: it uses no wchar_t, so its 2-byte setting is harmless here.
set(VJO_SETTINGS_LIBS
  vita2d png_static jpeg z m SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub
  ScePvf_stub ScePgf_stub SceCommonDialog_stub SceSysmodule_stub SceIofilemgr_stub
  SceAppMgr_stub SceAppUtil_stub SceRtc_stub SceRegistryMgr_stub vjo_bearssl)
add_executable(VitaJPOverlay_Settings ${VJO_SETTINGS_SRC})
# Its version, beside its title (settings/main.c): this VJO_VERSION, and "debug" in the debug build.
target_compile_definitions(VitaJPOverlay_Settings PRIVATE VJO_STANDALONE=1 VJO_VERSION_STRING="${VJO_VERSION}")
target_include_directories(VitaJPOverlay_Settings PRIVATE ${ROOT}/third_party/bearssl/inc)
target_compile_options(VitaJPOverlay_Settings PRIVATE -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
target_link_options(VitaJPOverlay_Settings PRIVATE -Wl,-q,--gc-sections -Wl,--no-wchar-size-warning
  -Wl,--undefined=sceUserMainThreadCpuAffinityMask,--undefined=sceUserMainThreadPriority
  -Wl,--wrap=sceKernelCreateThread)
target_link_libraries(VitaJPOverlay_Settings ${VJO_SETTINGS_LIBS})
add_dependencies(VitaJPOverlay_Settings vjo_settings_png_geometry)
add_custom_command(TARGET VitaJPOverlay_Settings POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_settings_png_abi.py
    --objdump ${CMAKE_OBJDUMP}
    --geometry-object $<TARGET_OBJECTS:vjo_settings_png_geometry>
    $<TARGET_FILE:VitaJPOverlay_Settings>
  COMMENT "Checking linked Settings setjmp/longjmp fit libpng's jump buffer")
# Plugin CONFIG exports must not leak into the standalone application's ELF.
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
# A system application (attribute 0x0E) with a 50 MiB budget (0xC800 KiB), as
# vita-content-manager-pro: it starts beside a suspended game instead of closing
# it, in the system-application memory and without CDRAM (settings/sysapp.c).
vita_create_self(eboot.bin VitaJPOverlay_Settings UNSAFE ATTRIBUTE 0x0E MEMSIZE 0xC800)
# The eboot is that system application, whatever the lines above become: each of
# audit-pr109's mutants M1-M7 (no --undefined, no ATTRIBUTE or MEMSIZE, no
# settings/sysapp.c, another heap, priority or thread CPU) built and packaged.
# tools/check_settings_self.py reads the SELF's control info and process
# parameters, and the ELF's heap and thread calls.
add_custom_target(vjo_settings_self ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_settings_self.py
    --elf $<TARGET_FILE:VitaJPOverlay_Settings> ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin
  DEPENDS eboot.bin-self ${ROOT}/tools/check_settings_self.py
  COMMENT "Checking the Overlay app's eboot.bin is the system application it is built as")
# The app's version stamp (core/app_stamp.h): VJO_API_VERSION and the first 16 hex digits of the
# eboot's SHA-256. In the VPK it is app0:vjo_app.txt (installed as ux0:app/VJOSET001/vjo_app.txt); the
# release carries the same bytes as ux0:data/VitaJPOverlay/vjo_app.txt, the record of the app these
# plugins come with, which the Shell plugin, the app and the installer compare it with.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/app_stamp.py make --eboot ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin
    --api-header ${ROOT}/include/vjo_api.h -o ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin ${ROOT}/tools/app_stamp.py ${ROOT}/include/vjo_api.h
  COMMENT "Stamping the Overlay app (vjo_app.txt)")
# LiveArea title (param.sfo TITLE/STITLE) is "Overlay". The title ID, the VPK file name and every
# plugin, path and config name keep their old names, so installing over JP Overlay Settings is an update.
vita_create_vpk(JPOverlaySettings.vpk VJOSET001 eboot.bin
  VERSION "01.00" NAME "Overlay"
  FILE ${ROOT}/settings/assets/icon0.png sce_sys/icon0.png
       ${ROOT}/settings/assets/pic0.png sce_sys/pic0.png
       ${ROOT}/settings/assets/bg.png sce_sys/livearea/contents/bg.png
       ${ROOT}/settings/assets/startup.png sce_sys/livearea/contents/startup.png
       ${ROOT}/settings/assets/template.xml sce_sys/livearea/contents/template.xml
       ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt vjo_app.txt)
add_custom_target(vjo_settings_assets ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_settings_assets.py
    ${CMAKE_CURRENT_BINARY_DIR}/JPOverlaySettings.vpk
  DEPENDS JPOverlaySettings.vpk-vpk)
# The Overlay app's Vita3K crash check (tools/vita3k_settings_smoke.sh): the same sources
# with a scripted controller and counted textures (tools/vita3k_settings_smoke.c, linked in
# by -Wl,--wrap). Off by default, never in the release, and not evidence about the console.
option(VJO_SETTINGS_SMOKE "Build the Overlay app's Vita3K smoke VPK (vjo_settings_smoke.vpk)" OFF)
if(VJO_SETTINGS_SMOKE)
  # vita-elf-create needs a few KiB after the code segment for its SCE data; a code segment
  # that ends just below the data segment fails ("segment 1 overlaps"). The smoke script then
  # rebuilds with this many bytes of padding in .rodata.
  set(VJO_SETTINGS_SMOKE_PAD 0 CACHE STRING "Padding bytes in the smoke app's code segment")
  add_executable(vjo_settings_smoke ${VJO_SETTINGS_SRC} ${ROOT}/tools/vita3k_settings_smoke.c)
  target_compile_definitions(vjo_settings_smoke PRIVATE VJO_STANDALONE=1 VJO_SMOKE_PAD=${VJO_SETTINGS_SMOKE_PAD})
  target_include_directories(vjo_settings_smoke PRIVATE ${ROOT}/third_party/bearssl/inc)
  target_compile_options(vjo_settings_smoke PRIVATE -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
  target_link_options(vjo_settings_smoke PRIVATE -Wl,-q,--gc-sections -Wl,--no-wchar-size-warning
    -Wl,--wrap=sceCtrlPeekBufferPositive,--wrap=sceKernelExitProcess,--wrap=vita2d_load_PNG_buffer
    -Wl,--wrap=vita2d_load_PNG_file,--wrap=vita2d_free_texture,--wrap=vita2d_wait_rendering_done
    -Wl,--wrap=vita2d_draw_texture_scale,--wrap=sceKernelCreateThread)
  target_link_libraries(vjo_settings_smoke ${VJO_SETTINGS_LIBS})
  # An ordinary app, from the plugins' flags: vita_create_self (a macro) leaves the eboot's
  # in these, since #109 a list (-c;-at;0x0E;-m;0xC800) that the shell splits at each ';'.
  set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
  set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
  vita_create_self(vjo_settings_smoke.self vjo_settings_smoke UNSAFE)
  # vita_create_vpk (a macro) appends to these: start again, not from the Settings VPK's.
  set(VITA_MKSFOEX_FLAGS "")
  set(VITA_PACK_VPK_FLAGS "")
  # Its list of the VPK's files (resources, its VPK command's DEPENDS) too: since #122 the Settings
  # VPK's holds the generated vjo_app.txt, made from the app's eboot.bin, so this VPK would build the
  # Overlay app itself, and fail with it in a build without optimisation (#135: libpng at -O0, which
  # check_settings_png_abi.py refuses).
  set(resources "")
  vita_create_vpk(vjo_settings_smoke.vpk VJOSMK002 vjo_settings_smoke.self
    VERSION "01.00" NAME "VJO settings smoke"
    FILE ${ROOT}/settings/assets/icon0.png sce_sys/icon0.png)
endif()

# ---------------- the build's variant ----------------
# The modules are the build they say (tools/check_variant.py, docs/release-variants.md): the normal
# build's carry none of the debug build's own format strings (the self-test, the step log, the
# recognized text, the timings and traces) and its own short-log marker; the debug build's carry
# them all; the Overlay app names VJO_VERSION and, in the debug build, "debug".
set(VJO_VARIANT_WORKER_ARGS)
set(VJO_VARIANT_WORKER_DEPS)
if(VJO_WITH_VOCR)
  set(VJO_VARIANT_WORKER_ARGS --worker ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx)
  set(VJO_VARIANT_WORKER_DEPS VitaJPOverlay_OCR.suprx-self)
endif()
add_custom_target(vjo_variant ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_variant.py --variant ${VJO_VARIANT} --version ${VJO_VERSION}
    --sources ${ROOT}
    --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
    --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx ${VJO_VARIANT_WORKER_ARGS}
    --app ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self ${VJO_VARIANT_WORKER_DEPS} eboot.bin-self
    ${ROOT}/tools/check_variant.py
  COMMENT "Checking the modules are the ${VJO_VARIANT} build")

# ---------------- release zip ----------------
set(VJO_WORKER_RELEASE_COPY)
set(VJO_WORKER_RELEASE_DEPS)
if(VJO_WITH_VOCR)
  set(VJO_WORKER_RELEASE_COPY COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx ${CMAKE_CURRENT_BINARY_DIR}/release/ur0/tai/)
  # The memory ledger too: a zip is made only from modules that fit their build's budgets (the
  # release_variants target builds the other build's zip with `--target release` alone).
  set(VJO_WORKER_RELEASE_DEPS VitaJPOverlay_OCR.suprx-self vjo_game_syscall_abi vjo_memory_ledger)
endif()
# config.ini sits at the zip's top level (the user edits it and copies it to
# ux0:data/VitaJPOverlay/), so extracting an update over ur0 never overwrites
# the user's settings.
set(STAGE ${CMAKE_CURRENT_BINARY_DIR}/release)
set(VJO_PLAINTEXT_SOURCE "${ROOT}/.refs/plaintext-dicts" CACHE PATH "Frozen private dictionary checkout")
add_custom_target(release
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ur0/tai ${STAGE}/ur0/data/VitaJPOverlay
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx ${STAGE}/ur0/tai/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx ${STAGE}/ur0/tai/
  ${VJO_WORKER_RELEASE_COPY}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco ${STAGE}/ur0/data/VitaJPOverlay/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/README.md ${ROOT}/LICENSE ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/config.example.ini ${STAGE}/config.ini
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/tools ${STAGE}/docs/benchmarks ${STAGE}/third_party
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/tools/convert_dictionary.py ${ROOT}/tools/install_dictionary.py ${ROOT}/tools/prepare_ocr.py ${ROOT}/tools/prepare_jmdict.py ${ROOT}/tools/prepare_plaintext.py ${ROOT}/tools/package_release.py ${ROOT}/tools/install_ftp.sh ${ROOT}/tools/migrate_config.py ${ROOT}/tools/patch_tai_config.py ${ROOT}/tools/analyse_selftest.py ${ROOT}/tools/analyse_timeline.py ${ROOT}/tools/model_tiers.json ${ROOT}/tools/collect_vita.py ${ROOT}/tools/app_stamp.py ${ROOT}/tools/check_variant.py ${ROOT}/tools/package_variants.py ${STAGE}/tools/
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/prepare_plaintext.py --source ${VJO_PLAINTEXT_SOURCE} --stage ${STAGE}
  # The record of the app these plugins come with (core/app_stamp.h): the VPK's own vjo_app.txt.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ux0/data/VitaJPOverlay
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt ${STAGE}/ux0/data/VitaJPOverlay/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/project-lessons.md ${ROOT}/docs/todo-20261011.md ${ROOT}/docs/settings-readability.md ${ROOT}/docs/settings-png-runtime.md ${ROOT}/docs/touch-lookup.md ${ROOT}/docs/anki.md ${ROOT}/docs/local-dictionaries.md ${ROOT}/docs/local-ocr.md ${ROOT}/docs/vita-vn-ocr.md ${ROOT}/docs/lookup-performance.md ${ROOT}/docs/jmdict-source.json ${ROOT}/docs/release-variants.md ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/benchmarks/local-lookup-jitendex.json ${ROOT}/docs/benchmarks/kernel-startup-20261010.json ${ROOT}/docs/benchmarks/daijirin-v3-selected-20261011.json ${ROOT}/docs/benchmarks/daijirin-import-20261010.json ${ROOT}/docs/benchmarks/vocr-vs-ncnn-20261010.json ${ROOT}/docs/benchmarks/vocr-release-20261011.json ${ROOT}/docs/benchmarks/vocr2-candidate-20261012.json ${ROOT}/docs/benchmarks/local-result-memory-20261011.json ${ROOT}/docs/benchmarks/local-character-memory-20261011.json ${STAGE}/docs/benchmarks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/README.md ${STAGE}/third_party/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/third_party/licenses ${STAGE}/third_party/licenses
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/bearssl/LICENSE.txt ${STAGE}/third_party/licenses/BearSSL-MIT.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/jsmn/LICENSE ${STAGE}/third_party/licenses/jsmn-MIT.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/JPOverlaySettings.vpk ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E chdir ${STAGE} ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay-${VJO_VERSION}${VJO_VARIANT_SUFFIX}.zip --format=zip ur0 ux0 config.ini README.md LICENSE tools docs third_party JPOverlaySettings.vpk
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self ${VJO_WORKER_RELEASE_DEPS} vjo_rco vjo_syscall_abi vjo_settings_assets vjo_settings_self vjo_variant
  COMMENT "Packaging VitaJPOverlay-${VJO_VERSION}${VJO_VARIANT_SUFFIX}.zip (the ${VJO_VARIANT} build)")

# ---------------- the release's two zips: the normal build and the debug build ----------------
# The final release ships both builds of one tree (docs/release-variants.md): the normal build as
# VitaJPOverlay-<version>.zip, the main download, and the debug build, the test builds' own, as
# VitaJPOverlay-<version>-debug.zip, with the same models, dictionary and config template. This
# target makes this build's base zip (release) and the other build's, in variant-<other>/ configured
# from this cache (toolchain, runtime, dictionary, version; the app's PNG sources not fetched
# again); with VJO_RELEASE_MODELS (the pinned models and LMs, as tools/package_release.py --models
# wants them) tools/package_variants.py then packages both into release-variants/ with their
# SHA256SUMS. VJO_RELEASE_SETTINGS (KEY=VALUE;...) are both packages' --set settings and
# VJO_RELEASE_SOURCE_REVISION their manifests' source revision.
set(VJO_RELEASE_MODELS "" CACHE PATH "Pinned models and LMs for release_variants (package_release.py --models)")
set(VJO_RELEASE_SETTINGS "" CACHE STRING "Both packages' settings for release_variants (KEY=VALUE;...)")
set(VJO_RELEASE_SOURCE_REVISION "" CACHE STRING "Exact source revision for release_variants' manifests")
if(VJO_DEBUG)
  set(VJO_OTHER_DEBUG OFF)
  set(VJO_OTHER_VARIANT normal)
else()
  set(VJO_OTHER_DEBUG ON)
  set(VJO_OTHER_VARIANT debug)
endif()
set(VJO_OTHER_DIR ${CMAKE_CURRENT_BINARY_DIR}/variant-${VJO_OTHER_VARIANT})
set(VJO_THIS_ZIP ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay-${VJO_VERSION}${VJO_VARIANT_SUFFIX}.zip)
if(VJO_DEBUG)
  set(VJO_NORMAL_ZIP ${VJO_OTHER_DIR}/VitaJPOverlay-${VJO_VERSION}.zip)
  set(VJO_DEBUG_ZIP ${VJO_THIS_ZIP})
else()
  set(VJO_NORMAL_ZIP ${VJO_THIS_ZIP})
  set(VJO_DEBUG_ZIP ${VJO_OTHER_DIR}/VitaJPOverlay-${VJO_VERSION}-debug.zip)
endif()
set(VJO_PACKAGE_VARIANTS)
if(VJO_RELEASE_MODELS)
  set(VJO_RELEASE_SET_ARGS)
  foreach(setting ${VJO_RELEASE_SETTINGS})
    list(APPEND VJO_RELEASE_SET_ARGS --set ${setting})
  endforeach()
  set(VJO_RELEASE_REVISION_ARGS)
  if(VJO_RELEASE_SOURCE_REVISION)
    set(VJO_RELEASE_REVISION_ARGS --source-revision ${VJO_RELEASE_SOURCE_REVISION})
  endif()
  set(VJO_PACKAGE_VARIANTS COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/package_variants.py
    --normal ${VJO_NORMAL_ZIP} --debug ${VJO_DEBUG_ZIP} --models ${VJO_RELEASE_MODELS} --version ${VJO_VERSION}
    -o ${CMAKE_CURRENT_BINARY_DIR}/release-variants ${VJO_RELEASE_REVISION_ARGS} ${VJO_RELEASE_SET_ARGS})
endif()
add_custom_target(release_variants
  COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR} -B ${VJO_OTHER_DIR} -G ${CMAKE_GENERATOR}
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DVJO_DEBUG=${VJO_OTHER_DEBUG} -DVJO_VERSION=${VJO_VERSION} -DVJO_WITH_VOCR=${VJO_WITH_VOCR}
    -DVJO_WITH_NCNN=${VJO_WITH_NCNN} -DVJO_VOCR_SOURCE_DIR=${VJO_VOCR_SOURCE_DIR}
    -DVJO_PLAINTEXT_SOURCE=${VJO_PLAINTEXT_SOURCE} -DPython3_EXECUTABLE=${Python3_EXECUTABLE}
    -DFETCHCONTENT_SOURCE_DIR_VJO_SETTINGS_PNG=${vjo_settings_png_SOURCE_DIR}
  COMMAND ${CMAKE_COMMAND} --build ${VJO_OTHER_DIR} --target release
  ${VJO_PACKAGE_VARIANTS}
  COMMENT "Both builds' zips: ${VJO_NORMAL_ZIP} (normal) and ${VJO_DEBUG_ZIP} (debug)"
  USES_TERMINAL VERBATIM)
add_dependencies(release_variants release)

