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
# type, or Debug) fails the Rubi app's PNG ABI check (tools/check_settings_png_abi.py).
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
include(${ROOT}/cmake/HookRevision.cmake)
vjo_hook_revision(${ROOT})
add_subdirectory(${ROOT}/compat/adrenaline_vsh ${CMAKE_CURRENT_BINARY_DIR}/adrenaline_compat)
set(VJO_BUILD_VITA ON)
option(VJO_WITH_NCNN "Build the CPU-only local mobile OCR recognizer" OFF)
option(VJO_WITH_VOCR "Build the vita-vn-ocr local OCR backend (needs its runtime sources)" ON)
# The release's version: the base zips' names and the Rubi app's version (beside its title). The
# test builds use this version with a -test.N suffix; the final release sets it to its own
# version first (docs/release-variants.md), and tools/package_variants.py checks the two agree.
set(VJO_VERSION "0.10.0" CACHE STRING "Release archive version")
# The build variant (docs/release-variants.md). ON, the default, is the debug build, the test
# builds' own: every log line, the self-test, the timing and trace lines, the game worker's step
# log. OFF is the normal build, the release's main download: errors, warnings and a short log;
# the rest is compiled out of all three plugins and the Rubi app, format strings included.
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
message(STATUS "Rubi ${VJO_VERSION}: the ${VJO_VARIANT} build (VJO_DEBUG=${VJO_DEBUG})")


# ---------------- kernel ----------------
add_executable(VitaJPOverlay_Kernel
  ${ROOT}/kernel/main.c
  ${ROOT}/kernel/io_worker.c
  ${ROOT}/kernel/boot.c
  ${ROOT}/kernel/ledger.c
  $<$<BOOL:${VJO_DEBUG}>:${ROOT}/kernel/meminfo.c> # the self-test's memory sample: debug build only
  ${ROOT}/core/boot_guard.c
  ${ROOT}/core/ledger.c
  ${ROOT}/kernel/log.c
  ${ROOT}/kernel/buffers.c
  ${ROOT}/kernel/capture.c
  ${ROOT}/kernel/game_ocr.c
  ${ROOT}/kernel/display.c
  ${ROOT}/kernel/input.c
  ${ROOT}/kernel/lifecycle.c
  ${ROOT}/kernel/foreground.c
  ${ROOT}/kernel/scene.c
  ${ROOT}/kernel/triggers.c
  ${ROOT}/kernel/loader.c
  ${ROOT}/kernel/text_source.c # API 31: the text source and the loader's ON processes (docs/native-hooks.md)
  ${ROOT}/core/games.c)
target_compile_options(VitaJPOverlay_Kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu99 -fno-builtin
  -mgeneral-regs-only)
# core/port.h: the kernel's snprintf is SceSysclibForDriver's (core/games.c).
target_compile_definitions(VitaJPOverlay_Kernel PRIVATE VJO_KERNEL=1)
# The OCR job and model-file broker is a cold path (a few syscalls per job, a
# rows call a frame at most; the row copy itself is kernel/capture.c's): built
# for size, since every page of the kernel module is the shared kernel pool's.
# So are the process event hook and the foreground state machine (a few calls
# per app switch) and the self-test's memory sample (kernel/meminfo.c, one
# vjoMemInfo call per report). They pay for #104's release of the game's input
# block in the hook (lifecycle.c, +464 B at -O2) within the module's pages. The
# worker loader (#138: one games.ini read per game start, kernel/loader.c) is
# one too. (core/games.c, the Shell's own policy code it calls, keeps each
# target's flags: source properties apply to every target of this directory.)
# The rest of the kernel's cold code is built for size too (API 23), which pays for the Anki
# screenshot's file broker and colour rows (kernel/game_ocr.c, kernel/capture.c) within the
# module's pages: the syscalls and the worker thread's loop (kernel/main.c: a few calls every
# 16 ms tick or 50 ms Shell poll, none per pixel), the boot-safety guard (kernel/boot.c,
# core/boot_guard.c: at boot and when a stage is marked), the kernel strip and the log
# (kernel/buffers.c, kernel/log.c). The per-pixel and per-frame code
# (kernel/capture.c, kernel/display.c, kernel/scene.c) and the pad and touch hooks
# (kernel/input.c, kernel/triggers.c) stay at -O2.
set_source_files_properties(${ROOT}/kernel/game_ocr.c ${ROOT}/kernel/lifecycle.c ${ROOT}/kernel/foreground.c
  ${ROOT}/kernel/meminfo.c ${ROOT}/kernel/loader.c ${ROOT}/kernel/main.c ${ROOT}/kernel/io_worker.c ${ROOT}/kernel/boot.c
  ${ROOT}/core/boot_guard.c ${ROOT}/kernel/buffers.c ${ROOT}/kernel/log.c ${ROOT}/kernel/text_source.c
  PROPERTIES COMPILE_OPTIONS "-Os")

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
  ${ROOT}/core/textfilter.c ${ROOT}/core/textjoin.c ${ROOT}/core/linebreak.c ${ROOT}/core/entries.c
  ${ROOT}/core/history_lines.c ${ROOT}/core/streak.c
  ${ROOT}/core/config.c ${ROOT}/core/regions.c
  ${ROOT}/core/client.c ${ROOT}/core/styled.c
  ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c ${ROOT}/core/local_dict.c
  ${ROOT}/core/ocr_lines.c ${ROOT}/core/games.c ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c
  ${ROOT}/core/ledger.c ${ROOT}/core/app_stamp.c ${ROOT}/core/pspbubble.c
  ${ROOT}/core/hook_pick.c) # API 31: text-source config helpers
target_include_directories(vjo_core_vita PUBLIC ${ROOT}/core ${ROOT}/include)
target_link_libraries(vjo_core_vita PUBLIC vjo_bearssl)
target_compile_options(vjo_core_vita PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -fno-builtin -fshort-wchar
  -ffunction-sections -fdata-sections)
# Cold core paths (configuration, parsers, queue files): built for
# size, since every page of the Shell module is SceShell's. OCR text handling
# stays at -O2. The UTF-8 helpers (core/utf.c: a few
# microseconds a text or a key) are built for size since the self-test's own job
# and the first-scan timings (docs/first-ocr.md): 1,532 -> 1,136 B (the JPEG
# encoder, built for size then too, went with Google Lens). The property is the directory's, so the
# kernel's core/ledger.c and the game worker's core/ledger.c and core/games.c
# are -Os too (their pages unchanged; the ARM lane runs the ledger suites).
# The dictionary lookup (core/local_dict.c) is built for size since #138's left
# stick, whose 792 B with API 23's screenshot would otherwise take the normal
# Shell a text page past its exact budget: 27,407 -> 24,958 B of the object's
# text and rodata; on main 74f8de8 2,552 B off the normal module and 2,600 B off
# the debug one [measured: the module rows with and without this line, 90,684 ->
# 88,132 and 101,284 -> 98,684 B]. A lookup runs 0.44 and 0.60 % more instructions
# on the Cortex-A9 (lookup_first 134,948 -> 135,545, lookup_every 123,845 ->
# 124,590 a lookup, counted under qemu-arm: tools/bench_stages.py a9 on both
# normal builds), which vita-vn-ocr's A9 cost model puts at most 2.1 us more at
# 333 MHz (1.6 us at 444) [estimate], against a console lookup's 70-81 ms of
# mostly file reads (docs/performance.md); the Rubi app's copy is -Os too.
# Line history's reader (core/history_lines.c) and the non-Japanese line filter
# (core/textfilter.c, once per recognized text and per earlier line: 288 -> 240 B)
# are built for size for the room line history's code takes in the normal Shell's
# text pages (docs/line-history.md).
set_source_files_properties(${ROOT}/core/config.c ${ROOT}/core/ledger.c ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c
  ${ROOT}/core/json.c
  ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c ${ROOT}/core/regions.c
  ${ROOT}/core/games.c ${ROOT}/core/sfo.c ${ROOT}/core/client.c
  ${ROOT}/core/utf.c
  ${ROOT}/core/app_stamp.c
  ${ROOT}/core/pspbubble.c ${ROOT}/core/psptarget.c ${ROOT}/core/taicfg.c
  ${ROOT}/core/local_dict.c ${ROOT}/core/textjoin.c ${ROOT}/core/linebreak.c ${ROOT}/core/hook_pick.c
  ${ROOT}/core/history_lines.c ${ROOT}/core/textfilter.c ${ROOT}/core/streak.c
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
  ${ROOT}/shell/history.c
  ${ROOT}/vita/games_storage.c
  ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/log_vita.c
  ${ROOT}/shell/files.c
  ${ROOT}/vita/games_storage.c
  ${ROOT}/vita/buttons_vita.c
  ${ROOT}/shell/libc_shim.c
  ${ROOT}/shell/divide.c) # the Shell's integer division, in place of libgcc's (the Shell only)
# Cross-file optimization removes redundant wrappers and constant branches so reading statistics
# fit the existing Shell pages. Only these Shell targets use LTO; kernel, OCR and Settings do not.
set_property(TARGET vjo_core_vita VitaJPOverlay_Shell PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
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
  ${ROOT}/shell/anki_queue.c ${ROOT}/shell/history.c ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/strip.cpp ${ROOT}/core/text_rows.c ${ROOT}/core/arena.c
  PROPERTIES COMPILE_OPTIONS "-Os")
# The Shell's integer division (shell/divide.c): for size, and each helper in its own section, so the
# build links only those its code calls (the normal build no divmod).
# GCC emits calls to these helpers after LTO's reachability pass; keep this tiny ABI object native.
set_source_files_properties(${ROOT}/shell/divide.c PROPERTIES COMPILE_OPTIONS "-Os;-ffunction-sections;-fno-lto")
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
  -Wl,--undefined=module_start -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning)
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
  vjo_bearssl  # again after the core: its 64-bit division comes from libgcc (32-bit: shell/divide.c)
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
    ${ROOT}/game_ocr/main.c ${ROOT}/core/games.c ${ROOT}/core/ledger.c ${ROOT}/shell/libc_shim.c
    # API 23: the Anki screenshot's JPEG encoder, in the game's process only (never the Shell's),
    # and the arena it encodes into
    ${ROOT}/core/jpegsw.c ${ROOT}/core/shottrim.c ${ROOT}/core/arena.c
    # API 31: the native text hooks (docs/native-hooks.md), in the game's process only: the hook
    # engine, the site check, the stream slots, Luna's table and the hook text decoder; MAGES' SC3 text
    ${ROOT}/game_ocr/hooks.c ${ROOT}/game_ocr/hooks_observer.S ${ROOT}/core/hook_place.c
    ${ROOT}/core/hook_stream.c ${ROOT}/core/hook_luna.c ${ROOT}/core/hook_re.c ${ROOT}/core/hook_text.c
    ${ROOT}/core/hook_sc3.c ${ROOT}/core/sfo.c)
  add_dependencies(VitaJPOverlay_OCR vjo_kernel_stubs)
  # API 31: which text a job shows, OCR's or a native hook stream's (core/hook_select.h), at job time.
  # For size, as the worker's control code is: 1,992 B of text and rodata at -Os (2,640 B at -O2; measured,
  # vitasdk 2026.08, with the lock's checks of audit-hooks-text F3).
  target_sources(VitaJPOverlay_OCR PRIVATE ${ROOT}/core/hook_select.c ${ROOT}/core/hook_cache.c)
  set_source_files_properties(${ROOT}/core/hook_select.c ${ROOT}/core/hook_cache.c PROPERTIES COMPILE_OPTIONS "-Os")
  # The job path decodes a stream's text (vjo_hooks_text): for a Luna site through its filter, whose
  # regular expressions (core/hook_re.c, in the engine's list) are control code too: for size. So are
  # core/hook_luna.c's filter ops and Luna sites' reads (run_ops, vjo_luna_capture: a few per hit or job): -Os
  # makes room for the hook farm's 20 entries of #394 in both builds' pages (its text 5,612 -> 4,168 B and rodata
  # 13,595 -> 13,347 B, the normal build's object; docs/memory-ledger.md, "The hook farm's 20 verified entries").
  # (A source property: the Rubi app's copy, for its Games page's hook tag, is -Os too.)
  set_source_files_properties(${ROOT}/core/hook_re.c ${ROOT}/core/hook_luna.c PROPERTIES COMPILE_OPTIONS "-Os")
  # MAGES' SC3 text (core/hook_sc3.c): a hit's span and the job's decoder, for size too.
  set_source_files_properties(${ROOT}/core/hook_sc3.c PROPERTIES COMPILE_OPTIONS "-Os")
  # The read-only PSP probe (docs/native-hooks.md "PSP probe (debug build)"): the debug build's worker only,
  # control code built for size, with Luna's PSP table (core/hook_luna_psp.inc) as its data.
  if(VJO_DEBUG)
    target_sources(VitaJPOverlay_OCR PRIVATE ${ROOT}/game_ocr/psp_probe.c ${ROOT}/core/psp_probe.c)
    set_source_files_properties(${ROOT}/game_ocr/psp_probe.c ${ROOT}/core/psp_probe.c PROPERTIES COMPILE_OPTIONS "-Os")
  endif()
  target_compile_options(VitaJPOverlay_OCR PRIVATE -O2 -Wall -Wextra -fno-builtin -fshort-wchar
    -ffunction-sections -fdata-sections)
  # The worker's control code (a job's request, memory checks and ladder, one call
  # per strip, the probe) is built for size: the recognizer, the finder and the
  # decoder are vita-vn-ocr's runtime and core/vocr_ocr.c, built apart. -Os pays for
  # the model tiers' ladder (API 21: +788 B of text at -O2) within the module's
  # pages (game_ocr/main.c's text 12,276 B at -O2 before it, 9,164 B at -Os with it).
  set_source_files_properties(${ROOT}/game_ocr/main.c PROPERTIES COMPILE_OPTIONS "-Os")
  # API 31: the native hooks' engine is control code too (install once, a few checks per hit), built
  # for size: every page of it is in every game that is ON.
  set_source_files_properties(${ROOT}/game_ocr/hooks.c ${ROOT}/core/hook_place.c ${ROOT}/core/hook_stream.c
    ${ROOT}/core/sfo.c PROPERTIES COMPILE_OPTIONS "-Os")
  # API 31: the worker reads the text source (vjoGetTextSource) once the kernel exports it; until then
  # its native hooks stay off (game_ocr/main.c text_source: OCR).
  file(STRINGS ${ROOT}/kernel/exports.yml VJO_KERNEL_TEXT_SOURCE REGEX "vjoGetTextSource")
  if(VJO_KERNEL_TEXT_SOURCE)
    target_compile_definitions(VitaJPOverlay_OCR PRIVATE VJO_HAVE_TEXT_SOURCE_IMPORT=1)
  endif()
  # API 32: the screenshot's black bars, for size too (612 B of text against 816 B at -O2): the scan
  # reads only the bars' pixels, a small part of the frame, once a shot; the encoder stays at -O2.
  set_source_files_properties(${ROOT}/core/shottrim.c PROPERTIES COMPILE_OPTIONS "-Os")
  # __sce_headroom, as the Rubi app's link below has it: room after the code segment for the SCE data
  # vita-elf-create appends there (the module info and import tables: 1,204 B in the normal build). Without
  # it the room is what the code leaves below the next 64 KiB boundary, and vita-elf-create refuses, then
  # crashes, when that is less ("Cannot allocate 1204 bytes for SCE data at end of segment 0; segment 1
  # overlaps": the normal build's worker with vita-vn-ocr #109's counter re-decode, its code ending 1,112 B
  # below 128 KiB). The gap costs no memory: only the segments are loaded (the ledger counts their pages).
  target_link_options(VitaJPOverlay_OCR PRIVATE -nostdlib -nostartfiles -Wl,-q -Wl,--gc-sections
    -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning -Wl,--defsym,__sce_headroom=0x1000)
  target_link_directories(VitaJPOverlay_OCR PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
  target_link_libraries(VitaJPOverlay_OCR vjo_vocr vjo_vocr_rt vjo_bearssl VitaJPOverlay_Kernel_stub_weak
    SceLibKernel_stub SceKernelThreadMgr_stub SceSysmem_stub_weak SceProcessmgr_stub_weak
    SceIofilemgr_stub SceAppMgr_stub_weak SceDisplay_stub_weak ScePower_stub_weak taihen_stub_weak
    SceKernelModulemgr_stub_weak gcc) # sceKernelGetModuleInfo: one copy per process (#138)
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
# made from the queue's records (core/anki_queue.c) with core/anki.c's fields, and the readings over their
# sentences from the local dictionaries (#311: core/sentence_reading.c, its files through settings/app_platform.c,
# the Look up page's, below). No network.
set(VJO_SETTINGS_ANKI_SRC
  ${ROOT}/settings/anki_page.c ${ROOT}/settings/anki_export.c
  ${ROOT}/core/anki.c ${ROOT}/core/textjoin.c ${ROOT}/core/anki_queue.c ${ROOT}/core/apkg.c ${ROOT}/core/sqlw.c ${ROOT}/core/zipw.c
  ${ROOT}/core/lapis.c ${ROOT}/core/json.c ${ROOT}/core/entries.c ${ROOT}/core/furigana.c ${ROOT}/core/pitch.c
  ${ROOT}/core/word_key.c
  ${ROOT}/core/sentence_reading.c) # #311: readings over a card's sentence
# The overlay's reading streak, seeded once from history.csv (settings/streak_seed.c: core/streak.c's rule, the seed's
# windows in core/streak_scan.c), with the record reader (core/history_read.c, the Look up page's too) and the Shell's
# character count (core/history_lines.c's vjo_line_characters; of it the link keeps what the app calls).
set(VJO_SETTINGS_HISTORY_SRC
  ${ROOT}/settings/streak_seed.c ${ROOT}/core/streak.c ${ROOT}/core/streak_scan.c ${ROOT}/core/history_read.c
  ${ROOT}/core/history_lines.c ${ROOT}/core/textfilter.c ${ROOT}/core/text_rows.c)
# The Look up page (#313, settings/lookup_page.c): typed search and earlier lines (history.csv: core/history_read.c,
# above), looked up with the overlay's own code (core/client.c, local_dict.c, text_rows.c) through the app's sceIo
# platform (settings/app_platform.c), and its cards saved to the queue as the overlay saves them (settings/app_queue.c).
set(VJO_SETTINGS_LOOKUP_SRC
  ${ROOT}/settings/lookup_page.c ${ROOT}/settings/lookup.c ${ROOT}/settings/lookup_history.c ${ROOT}/settings/app_queue.c
  ${ROOT}/settings/app_platform.c ${ROOT}/core/client.c)
set(VJO_SETTINGS_SRC
  ${ROOT}/settings/no_net.c ${ROOT}/settings/main.c ${ROOT}/settings/catalog.c ${ROOT}/settings/psp.c ${ROOT}/settings/preferences.c ${ROOT}/settings/engine.c ${ROOT}/settings/app_prefs.c
  ${ROOT}/settings/icons.c ${ROOT}/settings/search.c ${ROOT}/settings/text_layout.c ${ROOT}/core/linebreak.c ${ROOT}/settings/letter.c ${ROOT}/settings/app_check.c
  ${ROOT}/settings/applog.c ${ROOT}/settings/vita2d_textures.c
  ${ROOT}/core/games.c ${ROOT}/core/games_app.c ${ROOT}/core/sfo.c ${ROOT}/core/arena.c ${ROOT}/core/app_stamp.c ${ROOT}/core/pspbubble.c ${ROOT}/core/psptarget.c
  ${ROOT}/core/taicfg.c ${ROOT}/core/payload.c ${ROOT}/settings/install.c ${ROOT}/settings/install_page.c
  ${ROOT}/third_party/taihen-parser/src/lexer.c ${ROOT}/third_party/taihen-parser/src/parser.c
  ${ROOT}/core/config.c ${ROOT}/core/utf.c ${ROOT}/core/local_dict.c ${ROOT}/core/vocr_models.c
  ${ROOT}/core/regions.c # #358: Suggest a text box, region.ini's line for every game (settings/preferences.c)
  ${ROOT}/vita/games_storage.c ${ROOT}/vita/buttons_vita.c ${ROOT}/settings/sysapp.c ${ROOT}/settings/sysapp_gfx.c
  ${VJO_SETTINGS_ANKI_SRC} ${VJO_SETTINGS_HISTORY_SRC} ${VJO_SETTINGS_LOOKUP_SRC})
# API 31: the Games page's hook tag (core/hook_titles.c): Rubi's own entries, the worker's verified table
# itself (core/hook_place.c: of it only vjo_place_title_verified, vjo_place_verified and the table stay
# linked), and the games of Luna's
# ported Vita table (core/hook_luna.c, from hooks/luna-port: of it only vjo_luna_title_ported, the title
# list and the site table stay linked, --gc-sections drops the rest; audit-hooks-text N2: a title with a
# ported site). Without that file only Rubi's own entries.
list(APPEND VJO_SETTINGS_SRC ${ROOT}/core/hook_titles.c ${ROOT}/core/hook_place.c)
if(EXISTS ${ROOT}/core/hook_luna.c)
  foreach(_vjo_luna hook_luna.c hook_re.c hook_text.c)
    if(EXISTS ${ROOT}/core/${_vjo_luna})
      list(APPEND VJO_SETTINGS_SRC ${ROOT}/core/${_vjo_luna})
    endif()
  endforeach()
  set_property(SOURCE ${ROOT}/core/hook_titles.c APPEND PROPERTY COMPILE_DEFINITIONS VJO_HOOK_LUNA=1)
endif()
# The one-VPK install's config.txt check is taiHEN's own parser (DaveeFTW/taihen-parser 6d817fd, MIT),
# built as taiHEN 309b380 builds it: its inbuilt ctype and string functions (docs/one-vpk-install.md).
set_source_files_properties(${ROOT}/third_party/taihen-parser/src/lexer.c ${ROOT}/third_party/taihen-parser/src/parser.c
  PROPERTIES COMPILE_DEFINITIONS "NO_CTYPE;NO_STRING" COMPILE_OPTIONS "-w")
# vjo_bearssl (SHA-256 for the queue's IDs and the screenshots' names, SHA-1 for Anki's field
# checksum) is the Shell's build: it uses no wchar_t, so its 2-byte setting is harmless here.
# SceShellSvc: the install's PS button and power-off menu lock (sceShellUtilLock, as VitaShell's installs
# hold it); SceProcessmgr: its auto-suspend hold (sceKernelPowerTick). Both only while an install runs.
set(VJO_SETTINGS_LIBS
  vita2d png_static jpeg z m SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub
  ScePvf_stub ScePgf_stub SceIme_stub SceCommonDialog_stub SceSysmodule_stub SceIofilemgr_stub
  SceAppMgr_stub SceAppUtil_stub SceRtc_stub SceRegistryMgr_stub SceShellSvc_stub SceProcessmgr_stub vjo_bearssl)
add_executable(VitaJPOverlay_Settings ${VJO_SETTINGS_SRC})
# Its version, beside its title (settings/main.c): this VJO_VERSION, and "debug" in the debug build.
target_compile_definitions(VitaJPOverlay_Settings PRIVATE VJO_STANDALONE=1 VJO_VERSION_STRING="${VJO_VERSION}")
target_include_directories(VitaJPOverlay_Settings PRIVATE ${ROOT}/third_party/bearssl/inc ${ROOT}/third_party/taihen-parser/include)
target_compile_options(VitaJPOverlay_Settings PRIVATE -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
# __sce_headroom: VitaSDK's linker script (armelf.x, 2026.08 and later: ". += DEFINED(__sce_headroom) ?
# __sce_headroom : 0" before the data segment's 64 KiB alignment) keeps that many bytes free after the code
# segment for the SCE data vita-elf-create appends there (the module info and the import tables: 2,612 B
# for this app). Without it the gap is whatever the code's size leaves below the next 64 KiB boundary, and
# vita-elf-create refuses, then crashes, when that is less ("Cannot allocate 2612 bytes for SCE data at end
# of segment 0; segment 1 overlaps": CI's normal build of the one-VPK install). The gap costs no memory:
# only the segments are loaded, and tools/check_settings_self.py counts them.
# settings/sysapp_gfx.c wraps libvita2d's two calls into the system (its graphics start with no
# game running: the GPU library's shared parameter buffer and SceShell's framebuffer hand-over);
# VJO_SETTINGS_GFX_WRAP is the smoke app's too. tools/check_settings_self.py checks that libvita2d
# calls the wrappers.
# settings/vita2d_textures.c wraps libvita2d's plain textures (vita2d_create_empty_texture_format): their two stale
# fields cleared, so that a font's glyph atlas, which main.c frees while the app runs, frees nothing else.
set(VJO_SETTINGS_GFX_WRAP -Wl,--wrap=sceGxmVshInitialize,--wrap=sceSharedFbGetInfo,--wrap=vita2d_create_empty_texture_format)
target_link_options(VitaJPOverlay_Settings PRIVATE -Wl,-q,--gc-sections -Wl,--no-wchar-size-warning
  -Wl,--undefined=sceUserMainThreadCpuAffinityMask,--undefined=sceUserMainThreadPriority
  -Wl,--wrap=sceKernelCreateThread ${VJO_SETTINGS_GFX_WRAP} -Wl,--defsym,__sce_headroom=0x4000)
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
  COMMENT "Checking the Rubi app's eboot.bin is the system application it is built as")
# The app's version stamp (core/app_stamp.h): VJO_API_VERSION and the first 16 hex digits of the
# eboot's SHA-256. In the VPK it is app0:vjo_app.txt (installed as ux0:app/VJOSET001/vjo_app.txt); the
# release carries the same bytes as ux0:data/VitaJPOverlay/vjo_app.txt, the record of the app these
# plugins come with, which the Shell plugin, the app and the installer compare it with.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/app_stamp.py make --eboot ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin
    --api-header ${ROOT}/include/vjo_api.h -o ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin ${ROOT}/tools/app_stamp.py ${ROOT}/include/vjo_api.h
  COMMENT "Stamping the Rubi app (vjo_app.txt)")
# LiveArea title (param.sfo TITLE/STITLE) is "Rubi" (docs/rename-rubi.md). The title ID and every plugin,
# path and config name keep their old names, so installing it over the app of an earlier build (Overlay in
# the 0.8.0 test builds, JP Overlay Settings in 0.7.12 and older) is an update.
vita_create_vpk(Rubi.vpk VJOSET001 eboot.bin
  VERSION "01.00" NAME "Rubi"
  FILE ${ROOT}/settings/assets/icon0.png sce_sys/icon0.png
       ${ROOT}/settings/assets/pic0.png sce_sys/pic0.png
       ${ROOT}/settings/assets/bg.png sce_sys/livearea/contents/bg.png
       ${ROOT}/settings/assets/startup.png sce_sys/livearea/contents/startup.png
       ${ROOT}/settings/assets/template.xml sce_sys/livearea/contents/template.xml
       ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt vjo_app.txt)
# The one-VPK install (docs/one-vpk-install.md): the app names the worker mode the tree compiled (its
# vjoworker= mark, which tools/package_vpk.py writes into the payload manifest), and no module has what
# VitaShell's install scan warns of (a partition mount call; a plugin with ScePromoterUtil or SceShellSvc).
file(STRINGS ${ROOT}/include/vjo_api.h VJO_KERNEL_LOADS_WORKER_LINE REGEX "^#define VJO_KERNEL_LOADS_WORKER [01]")
if(VJO_KERNEL_LOADS_WORKER_LINE MATCHES " 1")
  set(VJO_WORKER_MODE kernel)
else()
  set(VJO_WORKER_MODE lines)
endif()
add_custom_target(vjo_one_vpk ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/package_vpk.py --worker ${VJO_WORKER_MODE} --check-build
    ${CMAKE_CURRENT_BINARY_DIR}/eboot.bin ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
    ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx
  DEPENDS eboot.bin-self VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self VitaJPOverlay_OCR.suprx-self
    ${ROOT}/tools/package_vpk.py
  COMMENT "Checking the one-VPK install's modules (tools/package_vpk.py --check-build)")
add_custom_target(vjo_settings_assets ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_settings_assets.py
    ${CMAKE_CURRENT_BINARY_DIR}/Rubi.vpk
  DEPENDS Rubi.vpk-vpk)
# The Rubi app's Vita3K crash check (tools/vita3k_settings_smoke.sh): the same sources
# with a scripted controller and counted textures (tools/vita3k_settings_smoke.c, linked in
# by -Wl,--wrap). Off by default, never in the release, and not evidence about the console.
option(VJO_SETTINGS_SMOKE "Build the Rubi app's Vita3K smoke VPK (vjo_settings_smoke.vpk)" OFF)
if(VJO_SETTINGS_SMOKE)
  # vita-elf-create needs a few KiB after the code segment for its SCE data; a code segment
  # that ends just below the data segment fails ("segment 1 overlaps"). The smoke script then
  # rebuilds with this many bytes of padding in .rodata.
  set(VJO_SETTINGS_SMOKE_PAD 0 CACHE STRING "Padding bytes in the smoke app's code segment")
  add_executable(vjo_settings_smoke ${VJO_SETTINGS_SRC} ${ROOT}/tools/vita3k_settings_smoke.c)
  target_compile_definitions(vjo_settings_smoke PRIVATE VJO_STANDALONE=1 VJO_SMOKE_PAD=${VJO_SETTINGS_SMOKE_PAD})
  target_include_directories(vjo_settings_smoke PRIVATE ${ROOT}/third_party/bearssl/inc ${ROOT}/third_party/taihen-parser/include)
  target_compile_options(vjo_settings_smoke PRIVATE -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
  target_link_options(vjo_settings_smoke PRIVATE -Wl,-q,--gc-sections -Wl,--no-wchar-size-warning
    -Wl,--wrap=sceCtrlPeekBufferPositive,--wrap=sceKernelExitProcess,--wrap=vita2d_load_PNG_buffer
    -Wl,--wrap=vita2d_load_PNG_file,--wrap=vita2d_free_texture,--wrap=vita2d_wait_rendering_done
    -Wl,--wrap=vita2d_draw_texture_scale,--wrap=sceKernelCreateThread ${VJO_SETTINGS_GFX_WRAP})
  # VJO_SETTINGS_SMOKE_SYSAPP: the system-application start, which Vita3K never takes (its
  # sceAppMgrGetBudgetInfo is a stub that refuses): the smoke answers it as a system application,
  # and stands in for SceShell's framebuffer hand-over and the GPU library's shared parameter
  # buffer as ux0:data/vjo_settings_smoke_sysapp.txt says (tools/vita3k_settings_smoke.c).
  option(VJO_SETTINGS_SMOKE_SYSAPP "Build the smoke app to start as a system application under Vita3K" OFF)
  set(VJO_SETTINGS_SMOKE_TITLE VJOSMK002)
  if(VJO_SETTINGS_SMOKE_SYSAPP)
    target_compile_definitions(vjo_settings_smoke PRIVATE VJO_SMOKE_SYSAPP=1)
    target_link_options(vjo_settings_smoke PRIVATE -Wl,--wrap=sceAppMgrGetBudgetInfo)
    set(VJO_SETTINGS_SMOKE_TITLE VJOSMK003) # installed beside the ordinary smoke app
  endif()
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
  # Rubi app itself, and fail with it in a build without optimisation (#135: libpng at -O0, which
  # check_settings_png_abi.py refuses).
  set(resources "")
  vita_create_vpk(vjo_settings_smoke.vpk ${VJO_SETTINGS_SMOKE_TITLE} vjo_settings_smoke.self
    VERSION "01.00" NAME "VJO settings smoke"
    FILE ${ROOT}/settings/assets/icon0.png sce_sys/icon0.png)
endif()

# ---------------- the build's variant ----------------
# The modules are the build they say (tools/check_variant.py, docs/release-variants.md): the normal
# build's carry none of the debug build's own format strings (the self-test, the step log, the
# recognized text, the timings and traces) and its own short-log marker; the debug build's carry
# them all; the Rubi app names VJO_VERSION and, in the debug build, "debug".
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

# ---------------- no network imports ----------------
# The overlay is offline (owner, 2026-10-15): no module imports SceNet*, SceHttp* or SceSsl*
# (tools/check_offline_imports.py reads each ELF's import stub sections).
set(VJO_OFFLINE_MODULES $<TARGET_FILE:VitaJPOverlay_Kernel> $<TARGET_FILE:VitaJPOverlay_Shell>
  $<TARGET_FILE:VitaJPOverlay_Settings>)
set(VJO_OFFLINE_DEPS VitaJPOverlay_Kernel VitaJPOverlay_Shell VitaJPOverlay_Settings)
if(VJO_WITH_VOCR)
  list(APPEND VJO_OFFLINE_MODULES $<TARGET_FILE:VitaJPOverlay_OCR>)
  list(APPEND VJO_OFFLINE_DEPS VitaJPOverlay_OCR)
endif()
add_custom_target(vjo_offline_imports ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_offline_imports.py ${VJO_OFFLINE_MODULES}
  DEPENDS ${VJO_OFFLINE_DEPS} ${ROOT}/tools/check_offline_imports.py
  COMMENT "Checking that no module imports a network library")

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
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/compat
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/compat/adrenaline_vsh ${STAGE}/compat/adrenaline_vsh
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/adrenaline_compat/adrenaline_vsh.suprx ${STAGE}/compat/
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/tools
  # the bridge's budget check (compat/adrenaline_vsh/CMakeLists.txt runs ../../tools/check_adrenaline_vsh.py) and the
  # SELF reader it, check_variant.py and package_vpk.py share
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/tools/check_adrenaline_vsh.py ${ROOT}/tools/vita_self.py ${STAGE}/tools/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/config.example.ini ${STAGE}/config.ini
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/tools ${STAGE}/docs/benchmarks ${STAGE}/docs/img
    ${STAGE}/third_party/LunaTranslator
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/tools/convert_dictionary.py ${ROOT}/tools/install_dictionary.py ${ROOT}/tools/prepare_ocr.py ${ROOT}/tools/prepare_plaintext.py ${ROOT}/tools/prepare_vndb.py ${ROOT}/tools/package_release.py ${ROOT}/tools/install_ftp.sh ${ROOT}/tools/migrate_config.py ${ROOT}/tools/patch_tai_config.py ${ROOT}/tools/analyse_selftest.py ${ROOT}/tools/analyse_timeline.py ${ROOT}/tools/reading_stats.py ${ROOT}/tools/known_words.py ${ROOT}/tools/model_tiers.json ${ROOT}/tools/collect_vita.py ${ROOT}/tools/hook_request.py ${ROOT}/tools/app_stamp.py ${ROOT}/tools/check_variant.py ${ROOT}/tools/package_vpk.py ${STAGE}/tools/
  # API 54: the text spotter's pins (tools/prepare_ocr.py reads them from this header, beside it in the zip)
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/include/vjo_vocr_sizes.h ${STAGE}/tools/
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/prepare_plaintext.py --source ${VJO_PLAINTEXT_SOURCE} --stage ${STAGE}
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/prepare_vndb.py --stage ${STAGE}
  # The record of the app these plugins come with (core/app_stamp.h): the VPK's own vjo_app.txt.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ux0/data/VitaJPOverlay
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt ${STAGE}/ux0/data/VitaJPOverlay/
  # The SC3 hooks' charsets (docs/native-hooks.md "SC3 text"), which the game worker reads through the kernel's broker
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ux0/data/VitaJPOverlay/hooks
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/sc3-charsets/PCSG00146.sc3 ${ROOT}/third_party/sc3-charsets/PCSG00352.sc3
    ${ROOT}/third_party/sc3-charsets/PCSG00555.sc3 ${STAGE}/ux0/data/VitaJPOverlay/hooks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/guide.md ${ROOT}/docs/project-lessons.md ${ROOT}/docs/todo-20261011.md ${ROOT}/docs/settings-readability.md ${ROOT}/docs/settings-png-runtime.md ${ROOT}/docs/touch-lookup.md ${ROOT}/docs/anki.md ${ROOT}/docs/local-dictionaries.md ${ROOT}/docs/local-ocr.md ${ROOT}/docs/vita-vn-ocr.md ${ROOT}/docs/lookup-performance.md ${ROOT}/docs/jmdict-source.json ${ROOT}/docs/release-variants.md ${ROOT}/docs/ocr-history.md ${STAGE}/docs/
  # Keep the guide's feature and recovery links usable in the downloaded ZIP.
  COMMAND ${CMAKE_COMMAND} -E copy
    ${ROOT}/docs/line-history.md ${ROOT}/docs/native-hooks.md ${ROOT}/docs/hook-titles.md
    ${ROOT}/docs/hook-requests.md
    ${ROOT}/docs/text-detector.md ${ROOT}/docs/memory-ledger.md ${ROOT}/docs/boot-safety.md
    ${ROOT}/docs/worker-loading.md ${ROOT}/docs/ingame-capture.md ${ROOT}/docs/performance.md
    ${ROOT}/docs/auto-vs-press.md ${ROOT}/docs/first-ocr.md ${ROOT}/docs/ocr-speed.md
    ${ROOT}/docs/offline-testing.md ${ROOT}/docs/one-vpk-install.md ${ROOT}/docs/rename-rubi.md
    ${ROOT}/docs/self-test.md ${ROOT}/docs/text-stability.md ${ROOT}/docs/vita-memory.md
    ${ROOT}/docs/config.example.ini ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/img/line-history-host.png ${STAGE}/docs/img/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/benchmarks/local-lookup-jitendex.json ${ROOT}/docs/benchmarks/kernel-startup-20261010.json ${ROOT}/docs/benchmarks/daijirin-v3-selected-20261011.json ${ROOT}/docs/benchmarks/daijirin-import-20261010.json ${ROOT}/docs/benchmarks/vocr-vs-ncnn-20261010.json ${ROOT}/docs/benchmarks/vocr-release-20261011.json ${ROOT}/docs/benchmarks/vocr2-candidate-20261012.json ${ROOT}/docs/benchmarks/local-result-memory-20261011.json ${ROOT}/docs/benchmarks/local-character-memory-20261011.json ${STAGE}/docs/benchmarks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/benchmarks/refine-20261015.json ${STAGE}/docs/benchmarks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/README.md ${STAGE}/third_party/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/LunaTranslator/README.md
    ${ROOT}/third_party/LunaTranslator/LICENSE ${STAGE}/third_party/LunaTranslator/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/third_party/licenses ${STAGE}/third_party/licenses
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/bearssl/LICENSE.txt ${STAGE}/third_party/licenses/BearSSL-MIT.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/jsmn/LICENSE ${STAGE}/third_party/licenses/jsmn-MIT.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/taihen-parser/LICENSE ${STAGE}/third_party/licenses/taihen-parser-MIT.txt
  # package_vpk.py carries third_party/licenses into the standalone VPK.
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/LunaTranslator/LICENSE
    ${STAGE}/third_party/licenses/LunaTranslator-GPL-3.0.txt
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/Rubi.vpk ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E chdir ${STAGE} ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/Rubi-${VJO_VERSION}${VJO_VARIANT_SUFFIX}.zip --format=zip ur0 ux0 config.ini README.md LICENSE tools docs third_party compat Rubi.vpk
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self ${VJO_WORKER_RELEASE_DEPS} vjo_rco vjo_syscall_abi vjo_settings_assets vjo_settings_self vjo_variant vjo_one_vpk
    vjo_offline_imports adrenaline_vsh_budget # a zip is never packaged from modules that import a network library
  COMMENT "Packaging Rubi-${VJO_VERSION}${VJO_VARIANT_SUFFIX}.zip (the ${VJO_VARIANT} build)")

# ---------------- the release's two zips: the normal build and the debug build ----------------
# The final release ships both builds of one tree (docs/release-variants.md): the normal build as
# Rubi-<version>.zip, the main download, and the debug build, the test builds' own, as
# Rubi-<version>-debug.zip, with the same models, dictionary and config template. This
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
set(VJO_THIS_ZIP ${CMAKE_CURRENT_BINARY_DIR}/Rubi-${VJO_VERSION}${VJO_VARIANT_SUFFIX}.zip)
if(VJO_DEBUG)
  set(VJO_NORMAL_ZIP ${VJO_OTHER_DIR}/Rubi-${VJO_VERSION}.zip)
  set(VJO_DEBUG_ZIP ${VJO_THIS_ZIP})
else()
  set(VJO_NORMAL_ZIP ${VJO_THIS_ZIP})
  set(VJO_DEBUG_ZIP ${VJO_OTHER_DIR}/Rubi-${VJO_VERSION}-debug.zip)
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

