# Host build: portable core, BearSSL, the vjo-cli tool and unit tests.
# The Vita build lives in vita/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
project(vita_jp_overlay_host C CXX)
option(VJO_WITH_VOCR "Build the vita-vn-ocr local OCR backend (needs its runtime sources)" ON)
set(VJO_VOCR_MODEL_DIR "" CACHE PATH "Directory with vita-vn-ocr weights for the model tests (optional)")
set(VJO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(cmake/HookRevision.cmake)
vjo_hook_revision(${VJO_ROOT})
# The build variant (docs/release-variants.md), as vita/CMakeLists.txt's: ON, the default, the
# debug build (the test builds'); OFF the normal build. The host suites run for both: the
# debug-only ones (the self-test writer, the kernel's memory routes) only in the debug build, and
# the tests of the normal build's short log only in the normal one.
option(VJO_DEBUG "Build the debug variant (the test builds'); OFF: the normal release build" ON)
if(VJO_DEBUG)
  add_compile_definitions(VJO_DEBUG=1)
else()
  add_compile_definitions(VJO_DEBUG=0)
endif()

set(CMAKE_C_STANDARD 99)
# Every unit's compile line (compile_commands.json): host/tests/variant_free.py derives the variant_free label from
# them (the end of this file). Set before the first target: each target's EXPORT_COMPILE_COMMANDS starts from it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# BearSSL: only its SHA-1 and SHA-224/256 are kept (the Anki queue's IDs, the models' hashes, the
# .apkg's checksums); its TLS, X.509 and ciphers went with the network (the overlay is offline).
set(BEARSSL_SRC
  ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/hash/sha1.c ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/hash/sha2small.c
  ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/codec/dec32be.c ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/codec/enc32be.c)
add_library(bearssl STATIC ${BEARSSL_SRC})
target_include_directories(bearssl PUBLIC third_party/bearssl/inc PRIVATE third_party/bearssl/src)
target_compile_options(bearssl PRIVATE -w)

# taiHEN's own configuration parser (DaveeFTW/taihen-parser 6d817fd, MIT), built as taiHEN 309b380
# builds it (its inbuilt ctype and string functions): the check of the config.txt editor
# (core/taicfg.c) is what taiHEN itself would load (docs/one-vpk-install.md).
add_library(vjo_taihen_parser STATIC third_party/taihen-parser/src/lexer.c third_party/taihen-parser/src/parser.c)
target_include_directories(vjo_taihen_parser PUBLIC third_party/taihen-parser/include)
target_compile_definitions(vjo_taihen_parser PRIVATE NO_CTYPE NO_STRING)
target_compile_options(vjo_taihen_parser PRIVATE -w)

set(CORE_SRC
  core/arena.c core/utf.c core/text_hit.c core/text_rows.c core/json.c
  core/textfilter.c core/textjoin.c core/linebreak.c core/history_lines.c core/history_read.c core/streak.c core/streak_scan.c
  core/entries.c core/config.c core/colour_scheme.c core/regions.c
  core/client.c core/styled.c
  core/anki.c core/anki_queue.c core/local_dict.c
  core/games.c core/games_app.c core/sfo.c core/furigana.c core/pitch.c core/ledger.c core/vocr_models.c core/app_stamp.c
  shell/game_ocr_client.c core/sqlw.c core/zipw.c core/lapis.c core/apkg.c core/heap.c core/word_key.c core/sentence_reading.c
  core/pspbubble.c core/psptarget.c core/taicfg.c core/jpegsw.c core/shottrim.c core/payload.c core/hook_select.c core/hook_cache.c)
add_library(vjo_core STATIC ${CORE_SRC} core/hook_profile_file.c)
target_include_directories(vjo_core PUBLIC core include)
target_link_libraries(vjo_core PUBLIC bearssl vjo_taihen_parser)
target_compile_definitions(vjo_core PUBLIC VJO_HOST=1)
target_compile_options(vjo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(VJO_WITH_VOCR)
  include(cmake/VocrOCR.cmake)
  target_compile_definitions(vjo_core PUBLIC VJO_WITH_VOCR=1)
  target_link_libraries(vjo_vocr PUBLIC vjo_core)
endif()

add_library(vjo_host STATIC host/platform_posix.c host/render.c host/replay.c)
target_include_directories(vjo_host PUBLIC host)
target_link_libraries(vjo_host PUBLIC vjo_core)
target_compile_options(vjo_host PRIVATE -Wall -Wextra)

add_executable(vjo-cli host/cli.c)
target_link_libraries(vjo-cli vjo_host)
# The readings the Rubi app writes into a card's SentenceFurigana (core/sentence_reading.h) for lines of text,
# with the file reads they take: tools/sentence_readings/'s measurement (docs/anki.md, "Sentence readings").
add_executable(vjo-sentence-readings host/sentence_readings.c)
target_link_libraries(vjo-sentence-readings vjo_host)
target_compile_options(vjo-sentence-readings PRIVATE -Wall -Wextra)

# Runs the game worker's OCR (core/vocr_ocr.c) on a raw RGBA region (vjo-ocr --help).
if(VJO_WITH_VOCR)
  add_executable(vjo-ocr host/ocr_tool.c)
  target_link_libraries(vjo-ocr vjo_host vjo_vocr)
  # The stable-text wait's frames and time on a simulated screen (docs/vita-vn-ocr.md).
  add_executable(vjo-settle-bench host/settle_bench.c)
  target_link_libraries(vjo-settle-bench vjo_vocr vjo_host)
  # API 55, the pause for the frame (docs/game-pause.md): display calls, rows and the game's wait per read.
  add_executable(vjo-pause-bench host/pause_bench.c)
  target_link_libraries(vjo-pause-bench vjo_vocr vjo_host)
  # Recognized text normalised as the game worker returns it, line by line (tools/textnorm_eval.py).
  add_executable(vjo-textnorm host/textnorm_tool.c)
  target_link_libraries(vjo-textnorm vjo_vocr)
  # The game worker's job on many regions, its weights kept, rows, columns or auto (tools/vertical_eval.py).
  find_package(Threads REQUIRED)
  add_executable(vjo-page-ocr host/page_ocr.c)
  target_link_libraries(vjo-page-ocr vjo_vocr vjo_host Threads::Threads)
  # #379: L + R on a region with an animated icon, frame by frame, with the Shell's retries (docs/vita-vn-ocr.md).
  add_executable(vjo-vertical-on-bench host/vertical_on_bench.c)
  target_link_libraries(vjo-vertical-on-bench vjo_vocr vjo_host m)
  target_compile_options(vjo-vertical-on-bench PRIVATE -Wall -Wextra)
endif()


enable_testing()
# A host suite: its program from SOURCES (vjo-<NAME, with - for _>-tests, unless TARGET names it), compiled with
# acutest's include directory and INCLUDES, DEFINES, -Wall -Wextra (not with NO_WARNINGS; UNUSED adds
# -Wno-unused-parameter) and OPTIONS, FEATURES, linked with LIBS and LINK_OPTIONS; and its test NAME, which runs the
# program alone. A suite that needs more (a property, a driver, another variant) adds it after the call.
function(vjo_suite name)
  cmake_parse_arguments(PARSE_ARGV 1 S "NO_WARNINGS;UNUSED" "TARGET"
    "SOURCES;INCLUDES;DEFINES;OPTIONS;FEATURES;LIBS;LINK_OPTIONS")
  if(S_UNPARSED_ARGUMENTS OR NOT S_SOURCES)
    message(FATAL_ERROR "vjo_suite(${name}): no SOURCES, or arguments it does not know: ${S_UNPARSED_ARGUMENTS}")
  endif()
  set(target ${S_TARGET})
  if(NOT target)
    string(REPLACE "_" "-" target "vjo-${name}-tests")
  endif()
  add_executable(${target} ${S_SOURCES})
  target_include_directories(${target} PRIVATE third_party/acutest/include ${S_INCLUDES})
  if(S_DEFINES)
    target_compile_definitions(${target} PRIVATE ${S_DEFINES})
  endif()
  set(options ${S_OPTIONS})
  if(S_UNUSED)
    list(PREPEND options -Wno-unused-parameter)
  endif()
  if(NOT S_NO_WARNINGS)
    list(PREPEND options -Wall -Wextra)
  endif()
  if(options)
    target_compile_options(${target} PRIVATE ${options})
  endif()
  if(S_FEATURES)
    target_compile_features(${target} PRIVATE ${S_FEATURES})
  endif()
  if(S_LIBS)
    target_link_libraries(${target} ${S_LIBS})
  endif()
  if(S_LINK_OPTIONS)
    target_link_options(${target} PRIVATE ${S_LINK_OPTIONS})
  endif()
  add_test(NAME ${name} COMMAND ${target})
endfunction()
vjo_suite(adrenaline_vsh SOURCES host/tests/test_adrenaline_vsh.c INCLUDES host/tests/adrenaline_stubs)
find_package(Threads REQUIRED)
vjo_suite(kernel_io SOURCES host/tests/test_kernel_io.c kernel/ledger.c core/ledger.c INCLUDES host/tests/vita_stubs
  DEFINES VJO_HOST=1 NO_WARNINGS LIBS Threads::Threads)
vjo_suite(kernel_capture SOURCES host/tests/test_kernel_capture.c kernel/scene.c core/region_select.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_HOST=1 UNUSED)
# core/fbgrey.h's word-at-a-time grey against the per-channel code it replaced, exhaustively.
vjo_suite(fbgrey SOURCES host/tests/test_fbgrey.c INCLUDES core)
# kernel/meminfo.c's three routes with the firmware's objects at kernel-range addresses (#74 audit B1).
# The self-test's memory sample: the debug build's only (the normal kernel has no kernel/meminfo.c).
if(VJO_DEBUG)
vjo_suite(kernel_meminfo SOURCES host/tests/test_kernel_meminfo.c core/ledger.c INCLUDES host/tests/vita_stubs
  DEFINES VJO_HOST=1 UNUSED)
endif()
vjo_suite(kernel_input SOURCES host/tests/test_kernel_input.c kernel/triggers.c INCLUDES host/tests/vita_stubs
  DEFINES VJO_HOST=1 UNUSED)
# API 31: the text source's syscalls (kernel/text_source.c): the registered Shell sets it, a process the
# loader started the worker in as ON reads it (any other OCR), OCR until set and in a safe boot.
vjo_suite(kernel_text_source SOURCES host/tests/test_kernel_text_source.c INCLUDES host/tests/vita_stubs
  DEFINES VJO_HOST=1)
# API 31: the Rubi app's Games tag, which reads Rubi's verified entries from the worker's own table
# (core/hook_place.c) and knows Luna's games (core/hook_luna.c), as the Vita app's build does (vita/CMakeLists.txt).
target_sources(vjo_core PRIVATE core/hook_titles.c core/hook_place.c core/hook_luna.c core/hook_re.c core/hook_text.c)
set_property(SOURCE core/hook_titles.c APPEND PROPERTY COMPILE_DEFINITIONS VJO_HOOK_LUNA=1)
# API 31: the Rubi app's Games tag (core/hook_titles.c), through the host core.
vjo_suite(hook_titles SOURCES host/tests/test_hook_titles.c DEFINES VJO_HOOK_LUNA=1 LIBS vjo_core)
# #104: the process event hook releases the game's input block in the same event
# (kernel/lifecycle.c with the real foreground state machine).
vjo_suite(kernel_lifecycle SOURCES host/tests/test_kernel_lifecycle.c kernel/foreground.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_HOST=1 UNUSED)
# #138: the kernel loads the game worker into every game ON in games.ini at its
# start (kernel/loader.c), with the Shell's own policy code and, for the parity
# check, the Shell's own games.ini reader on the same files.
vjo_suite(kernel_loader SOURCES host/tests/test_kernel_loader.c host/tests/games_io_posix.c vita/games_storage.c
  core/games.c core/arena.c kernel/ledger.c core/ledger.c INCLUDES host/tests/vita_stubs DEFINES VJO_HOST=1 UNUSED)
# #104: the overlay page's hold on SceShell's front touch region (shell/touch_hold.h).
vjo_suite(touch_hold SOURCES host/tests/test_touch_hold.c)
# Memory ledger: the counters, the budget table against the real sizing code,
# and the CI check (allocation sites, worst cases against limits).
add_executable(vjo-ledger-tests host/tests/test_ledger.c)
target_include_directories(vjo-ledger-tests PRIVATE third_party/acutest/include)
target_compile_options(vjo-ledger-tests PRIVATE -Wall -Wextra)
target_link_libraries(vjo-ledger-tests vjo_host)
if(VJO_WITH_VOCR)
  target_link_libraries(vjo-ledger-tests vjo_vocr)
endif()
add_test(NAME ledger COMMAND vjo-ledger-tests)
# Boot-safety guard: the record (core) and the real kernel-side code with the
# ur0 file and the clock replaced, boot after simulated boot.
vjo_suite(boot_guard SOURCES host/tests/test_boot_guard.c core/boot_guard.c kernel/boot.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_HOST=1 UNUSED)
# Native hooks (docs/native-hooks.md): the site check on the VitaSDK assembler's byte fixtures
# (tools/gen_hook_place_fixtures.sh), Rubi's verified entry and the CALL prefilter; the stream slots.
vjo_suite(hook_place SOURCES host/tests/test_hook_place.c core/hook_place.c core/hook_luna.c core/hook_re.c
  core/hook_text.c)
find_package(Threads REQUIRED)
vjo_suite(hook_stream SOURCES host/tests/test_hook_stream.c core/hook_stream.c core/hook_place.c LIBS Threads::Threads)
# #366: the CALL slots' replacement at each point where another thread can act, one interleaving at a time: the suite
# includes core/hook_stream.c with its seam (VJO_STREAM_SEAM, nothing in the worker) defined.
vjo_suite(hook_stream_seams SOURCES host/tests/test_hook_stream_seams.c core/hook_place.c)
# The hook engine itself (game_ocr/hooks.c, audit-hooks F4) against stubbed taiHEN, modulemgr, sysmem and io
# (host/tests/hooks_stubs before game_stubs), in this build's variant (VJO_DEBUG: its hooks.log too).
vjo_suite(hooks_engine SOURCES host/tests/test_hooks_engine.c core/hook_place.c core/hook_stream.c core/hook_luna.c
  core/hook_re.c core/hook_text.c core/hook_sc3.c core/sfo.c
  INCLUDES host/tests host/tests/hooks_stubs host/tests/game_stubs
  DEFINES VJO_HOST=1 VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" UNUSED LIBS bearssl)
# Exercise the engine as shipped: current-title disk profiles, through actual farm excerpts.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VJO_PROFILE_ARCHIVE ${CMAKE_BINARY_DIR}/profiles.vjhp)
add_custom_target(vjo_hook_profile_archive
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/hook_profiles.py --output ${VJO_PROFILE_ARCHIVE}
  BYPRODUCTS ${VJO_PROFILE_ARCHIVE})
vjo_suite(hooks_engine_external SOURCES host/tests/test_hooks_engine.c core/hook_place.c core/hook_stream.c
  core/hook_luna.c core/hook_re.c core/hook_text.c core/hook_sc3.c core/sfo.c core/hook_profile.c core/hook_profile_file.c
  INCLUDES host/tests host/tests/hooks_stubs host/tests/game_stubs
  DEFINES VJO_HOST=1 VJO_EXTERNAL_HOOK_PROFILES=1 VJO_PROFILE_ARCHIVE_PATH="${VJO_PROFILE_ARCHIVE}"
    VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" UNUSED LIBS bearssl)
add_dependencies(vjo-hooks-engine-external-tests vjo_hook_profile_archive)
# MAGES/5pb. SC3 script text (core/hook_sc3.c, docs/native-hooks.md "SC3 text"): the span and the decoder on the
# farm's pages, the committed charsets (third_party/sc3-charsets) against their pins and Rubi's entries (the pins
# against the files: hook_sc3_pins, below).
vjo_suite(hook_sc3 SOURCES host/tests/test_hook_sc3.c core/hook_sc3.c core/hook_place.c core/hook_luna.c core/hook_re.c
  core/hook_text.c INCLUDES core host/tests DEFINES VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" LIBS bearssl)
# kernel/foreground.c, scene.c, triggers.c: kernel logic without psp2kern.
vjo_suite(core TARGET vjo-tests SOURCES host/tests/test_core.c kernel/foreground.c kernel/scene.c kernel/triggers.c
  INCLUDES kernel DEFINES VJO_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures" NO_WARNINGS LIBS vjo_host)
vjo_suite(local_cursor SOURCES host/tests/test_local_cursor.c LIBS vjo_host)

vjo_suite(header_highlight SOURCES host/tests/test_header_highlight.cpp FEATURES cxx_std_11 LIBS vjo_host)
vjo_suite(word_navigation SOURCES host/tests/test_word_navigation.cpp INCLUDES host/tests FEATURES cxx_std_11
  LIBS vjo_host)
# The region selector's cursor, corners and touch drags (core/region_select.c,
# drawn by shell/overlay.cpp); vjo_host for region.ini's parser.
vjo_suite(region_select SOURCES host/tests/test_region_select.c core/region_select.c LIBS vjo_host)
# The text box suggested on first use (#358, core/region_suggest.c): the game worker's rule on the lines of a
# job on the whole frame, its box's normalization and each line's kana count.
vjo_suite(region_suggest SOURCES host/tests/test_region_suggest.c core/region_suggest.c LIBS vjo_host)
# The Rubi app's left-stick list scrolling (core/stick_scroll.h), against VitaShell's readPad.
vjo_suite(stick_scroll SOURCES host/tests/test_stick_scroll.c INCLUDES core)
# The overlay's left stick over the recognized text (#138, core/stick_cursor.h): its speed and cap by
# the clock, the lookup deferred until the cursor rests, the d-pad's at once, the step along the text.
vjo_suite(stick_cursor SOURCES host/tests/test_stick_cursor.c LIBS vjo_host)
# The Rubi app's system-application values and its thread wrapper (settings/sysapp.c);
# tools/check_settings_self.py checks the Vita build's eboot.bin and ELF for them.
vjo_suite(settings_sysapp SOURCES host/tests/test_settings_sysapp.c INCLUDES host/tests/sysapp_stubs)
# Its graphics start with no game running (settings/sysapp_gfx.c): libvita2d's start
# (host/tests/sysapp_gfx_model.c, its own object as libvita2d.a's is) through the link's
# --wrap of its two calls into the system, as vita/CMakeLists.txt links the app, against
# a model of SceShell and the GPU library.
vjo_suite(settings_sysapp_gfx SOURCES host/tests/test_settings_sysapp_gfx.c host/tests/sysapp_gfx_model.c
  settings/sysapp_gfx.c INCLUDES host/tests/sysapp_gfx_stubs DEFINES VJO_HOST=1
  LINK_OPTIONS "LINKER:--wrap=sceGxmVshInitialize" "LINKER:--wrap=sceSharedFbGetInfo")
# A start that never ends fails in the model (STUCK_US); this bounds anything else.
set_tests_properties(settings_sysapp_gfx PROPERTIES TIMEOUT 60)

# End-to-end word lookup on real VOCR output with the bundled dictionary
# (#73's frozen plaintext.vjdict). VJO_PLAINTEXT_SOURCE: a plaintext-dicts
# checkout or the frozen archive (default: .refs/plaintext-dicts, as CI checks
# it out); without one the test reports itself skipped.
set(VJO_PLAINTEXT_SOURCE "" CACHE PATH "plaintext-dicts checkout or frozen archive (word-lookup test)")
set(_vjo_plaintext "${VJO_PLAINTEXT_SOURCE}")
if(NOT _vjo_plaintext AND EXISTS ${CMAKE_SOURCE_DIR}/.refs/plaintext-dicts/processed/plaintext.vjdict.zip)
  set(_vjo_plaintext ${CMAKE_SOURCE_DIR}/.refs/plaintext-dicts)
endif()
vjo_suite(word_lookup SOURCES host/tests/test_word_lookup.cpp INCLUDES host/tests
  DEFINES VJO_WORD_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures/word_lookup" FEATURES cxx_std_11 LIBS vjo_host)
set_tests_properties(word_lookup PROPERTIES SKIP_RETURN_CODE 77)
if(_vjo_plaintext)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME plaintext_dictionary COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/prepare_plaintext.py
    --source ${_vjo_plaintext} -o ${CMAKE_BINARY_DIR}/plaintext/plaintext.vjdict)
  set_tests_properties(plaintext_dictionary PROPERTIES FIXTURES_SETUP plaintext)
  set_tests_properties(word_lookup PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${CMAKE_BINARY_DIR}/plaintext;VJO_REQUIRE_PLAINTEXT=1")
  set(_vjo_plaintext_dir ${CMAKE_BINARY_DIR}/plaintext)
endif()

# The Rubi app's history.csv reader (core/history_read.c: the Look up page's earlier lines, #313, the streak's seed)
# against the Shell's own (core/history_lines.c) on random files, whole and in windows.
vjo_suite(history_read SOURCES host/tests/test_history_read.c LIBS vjo_host)
# The Rubi app's sceIo VjoPlatform (settings/app_platform.c) over the host's files: opens, sizes, short reads.
vjo_suite(app_platform SOURCES host/tests/test_app_platform.c settings/app_platform.c INCLUDES host/tests/vita_stubs
  UNUSED LIBS vjo_host)
if(_vjo_plaintext_dir) # the bundled dictionary's lookups through it, against the host's platform
  set_tests_properties(app_platform PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir}")
endif()
# #313: the Look up page's lookups (settings/lookup.c) are the overlay's, on the golden lines of the word lookup test.
vjo_suite(lookup_parity SOURCES host/tests/test_lookup_parity.c settings/lookup.c settings/app_platform.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_WORD_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures/word_lookup" UNUSED
  LIBS vjo_host)
set_tests_properties(lookup_parity PROPERTIES SKIP_RETURN_CODE 77)
if(_vjo_plaintext_dir)
  set_tests_properties(lookup_parity PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir};VJO_REQUIRE_PLAINTEXT=1")
endif()
vjo_suite(text_hit SOURCES host/tests/test_text_hit.c LIBS vjo_host)
# #528: each arena of the Shell's lookup, its overlay texts, an Anki card, the export's sentence readings and the line
# history cut at every block it gives (core/arena.c's host-only cap): every run returns an error or the full result.
vjo_suite(oom_paths SOURCES host/tests/test_oom_paths.c INCLUDES host
  DEFINES VJO_WORD_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures/word_lookup" LIBS vjo_host)
if(_vjo_plaintext_dir)
  set_tests_properties(oom_paths PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir};VJO_REQUIRE_PLAINTEXT=1")
endif()
# The arrow keys over the header's displayed rows (#81), on layout fixtures.
# The overlay's paf texts re-wrapped where paf broke inside a word (shell/wrap_fix.h), on a model layout.
vjo_suite(wrap_fix SOURCES host/tests/test_wrap_fix.cpp FEATURES cxx_std_11 LIBS vjo_host)
# Where the overlay's own wrapping may break a line (core/linebreak.h, owner 2026-10-16 item 4):
# between phrases, never inside a run of kanji, katakana or kana; Japanese line-breaking rules.
vjo_suite(linebreak SOURCES host/tests/test_linebreak.c LIBS vjo_host)
# Sentence readings (#311, core/sentence_reading.h): a word's reading on its text, the field's HTML, the names'
# romaji; with the bundled dictionary, golden sentences, a card's field, the longest sentence in its scratch.
vjo_suite(sentence_reading SOURCES host/tests/test_sentence_reading.c LIBS vjo_host)
if(_vjo_plaintext_dir)
  set_tests_properties(sentence_reading PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir};VJO_REQUIRE_PLAINTEXT=1")
endif()
# The recognized text's lines as one running text (core/textjoin.h, owner 2026-10-16 item 4): which
# breaks join for the lookup and the Anki sentence, and the in-place join.
vjo_suite(text_join SOURCES host/tests/test_textjoin.c LIBS vjo_host)
# Native hooks (docs/native-hooks.md): which text a job shows, OCR's or a hook stream's (core/hook_select.h):
# the similarity of a stream's text within OCR's, against a brute-force reference, and the lock job by job.
vjo_suite(hook_select SOURCES host/tests/test_hook_select.c LIBS vjo_host)
vjo_suite(hook_cache SOURCES host/tests/test_hook_cache.c LIBS vjo_host)
vjo_suite(text_rows SOURCES host/tests/test_text_rows.c LIBS vjo_host)
# The cursor over several lines with the d-pad and the left stick (owner 2026-10-16, items 1 and 3):
# Left/Right across the rows, stopping at the text's ends; the stick's four directions as the d-pad's.
vjo_suite(cursor_nav SOURCES host/tests/test_cursor_nav.c LIBS vjo_host)
vjo_suite(anki SOURCES host/tests/test_anki.c NO_WARNINGS LIBS vjo_host)
# The real Shell Anki miner and its offline file queue (real files); Vita SDK substituted.
vjo_suite(shell_anki SOURCES host/tests/test_shell_anki.c INCLUDES host/tests/vita_stubs UNUSED LIBS vjo_host)
# The hidden auto-OCR CSV appender: real files, bounded writes and injected failures; its
# characters column (core/reading_chars.h: core/utf.c's decoder, core/text_rows.c's letters).
# line history's reader: core/history_lines.c
vjo_suite(shell_history SOURCES host/tests/test_shell_history.c core/utf.c core/text_rows.c core/textnorm.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_HOST=1 VJO_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures" UNUSED
  LIBS vjo_core)
set_tests_properties(shell_history PROPERTIES TIMEOUT 120) # a count's read loop that never ends fails, not hangs
# The Shell's integer division (shell/divide.c, in place of libgcc's in the Vita Shell): every quotient and
# remainder checked against what C's division means, without dividing; the ARM lane runs it too.
vjo_suite(shell_divide SOURCES host/tests/test_shell_divide.c)
# The same writer and count for tools/reading_stats.py's tests (tools/tests/test_reading_stats.py:
# VJO_HISTORY_WRITER), with the game worker's normalisation of the text it returns, and the Shell's reading
# total of a file (--total).
add_executable(vjo-history-writer host/tests/history_writer.c core/utf.c core/text_rows.c core/textnorm.c)
target_include_directories(vjo-history-writer PRIVATE host/tests/vita_stubs)
target_compile_definitions(vjo-history-writer PRIVATE VJO_HOST=1)
target_compile_options(vjo-history-writer PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-history-writer PRIVATE vjo_core) # shell/history.c also contains the line-history reader
# The reading streak (core/streak.c, the overlay's Triangle line; core/streak_scan.c, the Rubi app's seed from
# history.csv): its rule on day rollovers, gaps, a time zone moved, a clock set back, first use, the seed's windows
# against the Shell's own writer's records, and the merge with the Shell's record.
vjo_suite(streak SOURCES host/tests/test_streak.c UNUSED LIBS vjo_core)
# Line history (docs/line-history.md): history.csv's records read back from a window of the file,
# this game's lines newest first, filtered and deduplicated as the overlay shows them.
vjo_suite(history_lines SOURCES host/tests/test_history_lines.c LIBS vjo_core)
# The Rubi app's .apkg export (settings/anki_export.c, core/apkg.c and its SQLite, ZIP and Lapis
# writers) on real files; host/tests/test_apkg.py opens its packages with Python's zipfile and sqlite3
# and imports them into Anki itself (the `anki` package, tools/requirements-anki.txt) when it is there:
# VJO_ANKI_PYTHON names a Python that has it, VJO_REQUIRE_ANKI=1 makes its absence a failure (CI).
# The Look up page's earlier lines (settings/lookup_history.c): history.csv read back from its end, a window at a time.
vjo_suite(lookup_history SOURCES host/tests/test_lookup_history.c settings/lookup_history.c
  INCLUDES host/tests/vita_stubs UNUSED LIBS vjo_host)
# The Rubi app's queue writer (settings/app_queue.c: the Look up page's cards, #313): saved once, as the overlay saves
# them, every storage failure; its restart steps for host/tests/test_anki_restart.py.
vjo_suite(app_queue SOURCES host/tests/test_app_queue.c settings/app_queue.c INCLUDES host/tests/vita_stubs UNUSED
  LIBS vjo_host)
# (#311: its sentence readings' files)
vjo_suite(apkg SOURCES host/tests/test_apkg.c settings/app_queue.c settings/app_platform.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_KNOWN_WORDS_ENABLED=1 UNUSED LIBS vjo_host)
# Real shell control policy, with host-only substitutes for Vita SDK I/O.
# shell/log_vita.c on an in-memory file system: a batch of lines (vjo_log_batch_begin/end)
# writes the same bytes in the same order as line by line, with one append per file.
vjo_suite(shell_log SOURCES host/tests/test_shell_log.c INCLUDES host/tests/log_stubs DEFINES VJO_HOST=1 UNUSED)
# The shared app/overlay palette and the exact Shell surface recolouring loop over host widgets.
vjo_suite(colour_scheme SOURCES host/tests/test_colour_scheme.cpp FEATURES cxx_std_11 LIBS vjo_host)
# The Shell's VjoPlatform (shell/platform_vita.c): its files over stubbed sceIo, and its clock.
vjo_suite(platform_vita SOURCES host/tests/test_platform_vita.c INCLUDES host/tests/vita_stubs UNUSED LIBS vjo_core)
set_tests_properties(platform_vita PROPERTIES TIMEOUT 60) # a read loop that never ends fails, not hangs

add_executable(vjo-shell-worker-tests host/tests/test_shell_worker.c core/region_select.c) # #358: its box word
target_include_directories(vjo-shell-worker-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-shell-worker-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED) # VjoNet's paint runs on a real thread with a known stack
target_link_libraries(vjo-shell-worker-tests vjo_host Threads::Threads)
# -z now: no lazy binding on the paint test's thread (the resolver saves the vector state on its stack)
target_link_options(vjo-shell-worker-tests PRIVATE -Wl,-z,now)
if(VJO_WITH_VOCR)
  target_link_libraries(vjo-shell-worker-tests vjo_vocr)
endif()
add_test(NAME shell_worker COMMAND vjo-shell-worker-tests)
add_executable(vjo-shell-worker-paf-tests host/tests/test_shell_worker.c core/region_select.c)
target_include_directories(vjo-shell-worker-paf-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
# VJO_SOURCE_DIR: the owner scenario's events (tools/owner_scenario.txt, host/tests/owner_scenario.h)
target_compile_definitions(vjo-shell-worker-paf-tests PRIVATE VJO_PAF_ALLOC=1 VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(vjo-shell-worker-paf-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-shell-worker-paf-tests vjo_host Threads::Threads)
target_link_options(vjo-shell-worker-paf-tests PRIVATE -Wl,-z,now)
if(VJO_WITH_VOCR)
  target_link_libraries(vjo-shell-worker-paf-tests vjo_vocr)
endif()
add_test(NAME shell_worker_paf COMMAND vjo-shell-worker-paf-tests)
if(_vjo_plaintext_dir) # the Shell's word lookup with the bundled dictionary too
  set_tests_properties(shell_worker shell_worker_paf PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir}")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
# The memory ledger's CI check: allocation sites and worst cases (docs/memory-ledger.md), against this build's
# variant's budgets (include/vjo_ledger_budget.h's #if VJO_DEBUG lines).
if(VJO_DEBUG)
  set(_vjo_ledger_variant debug)
else()
  set(_vjo_ledger_variant normal)
endif()
add_test(NAME memory_ledger COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_memory_ledger.py
  --variant ${_vjo_ledger_variant})
# Each step is a fresh process: the overlay's saves and the Rubi app's exports, stopped at each
# storage boundary, never lose a card.
add_test(NAME anki_restart COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/test_anki_restart.py
  $<TARGET_FILE:vjo-shell-anki-tests> $<TARGET_FILE:vjo-apkg-tests> $<TARGET_FILE:vjo-app-queue-tests>)
add_test(NAME apkg_packages COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/test_apkg.py
  $<TARGET_FILE:vjo-apkg-tests>)
if(_vjo_plaintext_dir) # #311: the export's sentence readings with the bundled dictionary too
  set_tests_properties(apkg apkg_packages PROPERTIES FIXTURES_REQUIRED plaintext
    ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir}")
endif()
# The Lapis note type in core/lapis_notetype.inc is third_party/lapis's, generated by tools/gen_lapis.py.
add_test(NAME lapis_notetype COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_lapis.py --check)
# Native hooks (docs/hook-titles.md): the hook text decoder (core/hook_text.c: every CP932 cell against
# tools/gen_cp932.py's mapping), LunaTranslator's Vita table and its ports (core/hook_luna.c) on a fake
# memory image, and its filters' regex subset (core/hook_re.c) against Python's re
# (host/tests/hook_re_vectors.inc); the generated files must match their generators.
vjo_suite(hook_text SOURCES host/tests/test_hook_text.c core/hook_text.c INCLUDES core LIBS vjo_core)
vjo_suite(hook_luna SOURCES host/tests/test_hook_luna.c core/hook_luna.c core/hook_re.c core/hook_text.c
  core/textnorm.c INCLUDES core host/tests)
add_test(NAME hook_luna_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_luna_vita.py --check)
add_test(NAME hook_cp932_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_cp932.py --check)
# The SC3 charsets' pins (core/hook_sc3_pins.inc) are the committed files' (third_party/sc3-charsets).
add_test(NAME hook_sc3_pins COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sc3_charset.py --check)
add_test(NAME hook_re_vectors COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_hook_re_vectors.py --check)
# The debug build's read-only PSP probe (docs/native-hooks.md "PSP probe (debug build)"): Luna's PSP table
# as the probe looks it up (core/hook_luna_psp.inc, generated by tools/gen_luna_psp.py), its reads' bounds
# and its log lines.
vjo_suite(psp_probe SOURCES host/tests/test_psp_probe.c core/psp_probe.c)
add_test(NAME psp_probe_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_luna_psp.py --check)
# API 23: the Anki screenshot's JPEG encoder (core/jpegsw.c, the game worker's): every output
# decoded by Pillow, its size and quality against its source and against libjpeg's.
add_executable(vjo-jpeg host/jpeg_tool.c)
target_link_libraries(vjo-jpeg vjo_core)
target_compile_options(vjo-jpeg PRIVATE -Wall -Wextra)
add_test(NAME jpeg_encoder COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/test_jpegsw.py
  $<TARGET_FILE:vjo-jpeg>)
# API 32: the Anki screenshot's black bars (core/shottrim.c, the game worker's) on synthetic frames.
vjo_suite(shot_trim SOURCES host/tests/test_shot_trim.c LIBS vjo_core)
if(VJO_WITH_VOCR)
  vjo_suite(vocr_unit TARGET vjo-vocr-tests SOURCES host/tests/test_vocr.c
    DEFINES VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" LIBS vjo_vocr vjo_host)
  # The job policy (core/vocr_ocr.h VjoVocrPolicy): the stable-text wait and the
  # blank-line check, frame by frame on a simulated screen, without weights.
  # The text-presence gate's weights for the tests that run it (cmake/VocrGate.cmake: VJO_VOCR_GATE_TEST_DIR).
  include(cmake/VocrGate.cmake)
  # The tests time, empty or fail the runtime's calls through these (test_vocr_policy.c, wrap).
  vjo_suite(vocr_policy SOURCES host/tests/test_vocr_policy.c
    DEFINES VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VJO_VOCR_GATE_TEST_DIR="${VJO_VOCR_GATE_TEST_DIR}" LIBS vjo_vocr vjo_host
    LINK_OPTIONS "LINKER:--wrap=vocr_run" "LINKER:--wrap=vocr_lm_decode" "LINKER:--wrap=vocr_lm_init")
  # API 54: the text spotter as a job's finder (core/vocr_ocr.c VjoVocrPolicy.spot): the committed random VTSP blob
  # (host/fixtures/spotter) as its pin, and a synthetic recognizer pin whose weights no test reads.
  vjo_suite(vocr_spot SOURCES host/tests/test_vocr_spot.c core/vocr_ocr.c core/region_suggest.c
    INCLUDES host/tests core include
    DEFINES VJO_VOCR_TEST_MODELS="test_spot_models.h" VJO_VOCR_SPOT_TEST="test_spot_pins.h"
    VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" LIBS vjo_vocr_rt vjo_host)
  # API 54: the pinned spotter's own weights from VJO_VOCR_MODEL_DIR (the models lane; skipped without it): the
  # worker's load takes them, their budgets hold, and they find both lines of host/fixtures/short_line's text box.
  vjo_suite(spot_release SOURCES host/tests/test_spot_release.c core/vocr_ocr.c core/region_suggest.c
    INCLUDES host/tests core include
    DEFINES VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" LIBS vjo_vocr_rt vjo_host)
  vjo_suite(textnorm SOURCES host/tests/test_textnorm.c NO_WARNINGS LIBS vjo_vocr)
  # ocr-speed: parallel lines (VjoVocrPool) and the line cache with real vocr_run
  # calls on a tiny weights blob the test builds and pins itself (no release files).
  add_executable(vjo-vocr-parallel-tests host/tests/test_vocr_parallel.c core/vocr_ocr.c core/region_suggest.c)
  target_include_directories(vjo-vocr-parallel-tests PRIVATE third_party/acutest/include host/tests core include
    third_party/bearssl/inc)
  target_compile_options(vjo-vocr-parallel-tests PRIVATE -Wall -Wextra)
  target_compile_definitions(vjo-vocr-parallel-tests PRIVATE VJO_VOCR_TEST_MODELS="test_parallel_models.h")
  find_package(Threads REQUIRED)
  target_link_libraries(vjo-vocr-parallel-tests vjo_vocr_rt vjo_host bearssl Threads::Threads)
  add_test(NAME vocr_parallel COMMAND vjo-vocr-parallel-tests)
  # vita-vn-ocr #99: vertical text (a model of columns) through the actual core adapter, with real
  # vocr_run calls on tiny weights blobs the test builds and pins itself (no release files), and the
  # job of rows today's exactly (main 796168e's core with the runtime API 50 pins, vita-vn-ocr f8a1856).
  vjo_suite(vocr_vertical SOURCES host/tests/test_vocr_vertical.c core/vocr_ocr.c core/region_suggest.c
    INCLUDES host/tests core include third_party/bearssl/inc
    DEFINES VJO_VOCR_TEST_MODELS="test_vertical_models.h" VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VJO_VOCR_GATE_TEST_DIR="${VJO_VOCR_GATE_TEST_DIR}" LIBS vjo_vocr_rt vjo_host bearssl Threads::Threads)
  # ocr-speed: serial against helpers (and the cache) on real regions with real weights (host only).
  add_executable(vjo-vocr-parallel-bench host/vocr_parallel_bench.c)
  target_link_libraries(vjo-vocr-parallel-bench vjo_vocr vjo_host Threads::Threads)
  # #74: API 18's capture (the kernel's A8B8G8R8 copy of the region, the worker's
  # grey plane; 8228c3c, verbatim in host/tests/api18) against API 19's (the
  # display hook's grey rows in the worker's strip): the recognizer gets
  # byte-identical input. tools/capture_parity.py adds the eval crops.
  add_library(vjo_parity_kernel OBJECT host/tests/parity_kernel.c kernel/capture.c kernel/scene.c)
  target_include_directories(vjo_parity_kernel PRIVATE host/tests/vita_stubs)
  target_compile_definitions(vjo_parity_kernel PRIVATE VJO_HOST=1)
  vjo_suite(capture_parity SOURCES host/tests/test_capture_parity.c host/tests/parity_api18.c host/tests/parity_api19.c
    core/region_suggest.c $<TARGET_OBJECTS:vjo_parity_kernel> DEFINES VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}"
    LIBS vjo_vocr_rt vjo_core)
  # Exercise the real adapter with only the finder call replaced by a spy.
  # This verifies dispatch and capture ownership without weights or inference.
  add_executable(vjo-vocr-mode-tests host/tests/test_vocr_mode.c core/vocr_ocr.c core/region_suggest.c)
  target_include_directories(vjo-vocr-mode-tests PRIVATE third_party/acutest/include)
  target_compile_definitions(vjo-vocr-mode-tests PRIVATE vocr_find_lines_rows=vjo_test_find_lines_rows
    vocr_find_columns_rows=vjo_test_find_columns_rows
    vocr_find_lines_fix_rows=vjo_test_find_lines_fix_rows)
  target_compile_definitions(vjo-vocr-mode-tests PRIVATE VJO_VOCR_GATE_TEST_DIR="${VJO_VOCR_GATE_TEST_DIR}")
  target_link_libraries(vjo-vocr-mode-tests vjo_vocr_rt vjo_host)
  target_compile_options(vjo-vocr-mode-tests PRIVATE -Wall -Wextra)
  add_test(NAME vocr_finder_dispatch COMMAND vjo-vocr-mode-tests)
  # The weights are a release asset, not part of this repository: run the
  # model checks when VJO_VOCR_MODEL_DIR holds them (tools/prepare_ocr.py --backend vocr).
  if(VJO_VOCR_MODEL_DIR)
    add_test(NAME vocr_model COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_local_ocr.py
      --cli $<TARGET_FILE:vjo-ocr> --model ${VJO_VOCR_MODEL_DIR})
  endif()
endif()
# The tools' Python suites (tools/tests/test_*.py), one test per module, tools_<module>: one
# `unittest discover` of them all took 312 of the 384 s of a CI host job's ctest (2026-10-26), and
# `ctest -j` runs these side by side. Each runs as that discover did (this build's directory, the
# environment below), on its own module only.
file(GLOB _vjo_tool_suites CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/tests/test_*.py)
set(_vjo_tool_tests)
foreach(_vjo_suite IN LISTS _vjo_tool_suites)
  get_filename_component(_vjo_suite_module ${_vjo_suite} NAME_WE)
  string(REGEX REPLACE "^test_" "tools_" _vjo_test ${_vjo_suite_module})
  add_test(NAME ${_vjo_test} COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/tools/tests -p ${_vjo_suite_module}.py -v)
  list(APPEND _vjo_tool_tests ${_vjo_test})
  # Label tools: a change to the hooks' verified-entry table alone leaves these out in CI (tools/ci_scope.py
  # hooks_only), but for the three whose programs read that table, core/hook_place.c: tools/hook_request.py parses its
  # entries, tools/hook_farm.py compiles it (#366's audit-b, N3), and test_docs_hook_counts holds README.md's counts
  # of its entries (#418).
  if(NOT _vjo_suite_module MATCHES "^test_(hook_(request|farm)|docs_hook_counts)$")
    set_property(TEST ${_vjo_test} APPEND PROPERTY LABELS tools)
  endif()
  # A suite that never reads this build (its programs, VJO_CLI and the rest below, or build/host) gives
  # the same result in both builds: label variant_free, which the normal build's CI job and
  # tools/fast_gate.sh leave to the debug build's run (ctest -LE variant_free).
  file(READ ${_vjo_suite} _vjo_suite_text)
  if(NOT _vjo_suite_text MATCHES "VJO_(CLI|BENCH|SELFTEST_WRITER|DICT_BENCH|TAICFG|HISTORY_WRITER|CAPTURE_PARITY)|build/host")
    set_property(TEST ${_vjo_test} APPEND PROPERTY LABELS variant_free)
  endif()
endforeach()
# The slowest suites first under ctest -j: a new build directory (CI's) has no timings of its own, and
# a long suite started last would end the run alone. Seconds, measured 2026-10-26 (this box, 64 cores,
# loaded); only the order matters.
foreach(_vjo_cost IN ITEMS tools_release_install:92 tools_collect:64 tools_package_models:46 tools_crash_dumps:43
                           tools_release_variants:37 tools_plaintext:33 plaintext_dictionary:27 tools_reading_stats:24
                           vocr_parallel:19)
  string(REPLACE ":" ";" _vjo_cost ${_vjo_cost})
  list(GET _vjo_cost 0 _vjo_test)
  list(GET _vjo_cost 1 _vjo_seconds)
  if(TEST ${_vjo_test})
    set_tests_properties(${_vjo_test} PROPERTIES COST ${_vjo_seconds})
  endif()
endforeach()
# The real self-test writer (shell/selftest.c) on the host: v2 reports for the analyser's tests.
# The debug build's only: the normal build has no self-test (its analyser tests run on the fixtures).
set(_vjo_tool_env "VJO_CLI=$<TARGET_FILE:vjo-cli>;VJO_BENCH=$<TARGET_FILE:vjo-local-bench>")
if(VJO_DEBUG)
  add_executable(vjo-selftest-writer host/tests/selftest_writer.c)
  target_include_directories(vjo-selftest-writer PRIVATE host/tests/vita_stubs)
  target_compile_definitions(vjo-selftest-writer PRIVATE VJO_HOST=1)
  target_link_libraries(vjo-selftest-writer vjo_core)
  string(APPEND _vjo_tool_env ";VJO_SELFTEST_WRITER=$<TARGET_FILE:vjo-selftest-writer>")
endif()
string(APPEND _vjo_tool_env ";VJO_DICT_BENCH=$<TARGET_FILE:vjo-dict-bench>")
string(APPEND _vjo_tool_env ";VJO_TAICFG=$<TARGET_FILE:vjo-taicfg>")
string(APPEND _vjo_tool_env ";VJO_HISTORY_WRITER=$<TARGET_FILE:vjo-history-writer>")
if(VJO_WITH_VOCR)
  string(APPEND _vjo_tool_env ";VJO_CAPTURE_PARITY=$<TARGET_FILE:vjo-capture-parity-tests>")
endif()
set_tests_properties(${_vjo_tool_tests} PROPERTIES ENVIRONMENT "${_vjo_tool_env}")


# Counts production file reads and times lookup only (no OCR or process startup).
add_executable(vjo-local-bench host/local_bench.c)
target_link_libraries(vjo-local-bench vjo_host)
# The overlay's lookup at many words' first characters (tools/frequency_eval.py).
add_executable(vjo-word-forms host/word_forms.c)
target_link_libraries(vjo-word-forms vjo_host)
# The Shell's tap lookup at chosen characters of many lines, set up as the worker sets it up
# (tools/yomitan_check: Rubi's lookups beside Yomitan's on the same taps).
add_executable(vjo-dict-taps host/dict_taps.c)
target_link_libraries(vjo-dict-taps vjo_host)
# Which line breaks of recognized texts join and the words the lookup finds across them
# (core/textjoin.h; tools/cross_line_eval.py).
add_executable(vjo-cross-lines host/cross_lines.c)
target_link_libraries(vjo-cross-lines vjo_host)

# The Shell's word lookups over real recognized lines, every read attributed
# (tools/dict_lookup_bench.py): its own local_dict.c with the counters on.
add_executable(vjo-dict-bench host/dict_bench.c core/local_dict.c core/textnorm.c)
target_compile_definitions(vjo-dict-bench PRIVATE VJO_LOCAL_STATS=1)
target_link_libraries(vjo-dict-bench vjo_host)
target_compile_options(vjo-dict-bench PRIVATE -Wall -Wextra -Wno-unused-parameter)
# The Shell's cursor lookup at every tap of a corpus, each stage timed and every read
# attributed (tools/dict_stage_report.py): its own local_dict.c with the counters and stage clocks on.
add_executable(vjo-dict-stage-bench host/dict_stage_bench.c core/local_dict.c)
target_compile_definitions(vjo-dict-stage-bench PRIVATE VJO_LOCAL_STATS=1)
target_link_libraries(vjo-dict-stage-bench vjo_host)
target_compile_options(vjo-dict-stage-bench PRIVATE -Wall -Wextra -Wno-unused-parameter)
# What every lookup of the committed lines shows with the bundled dictionary
# (tools/dict_lookup_golden.tsv): a lookup change that alters one fails.
if(_vjo_plaintext_dir)
  add_test(NAME dict_lookup_golden COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/dict_lookup_bench.py
    --bench $<TARGET_FILE:vjo-dict-bench> --directory ${_vjo_plaintext_dir} --rounds 1
    --golden ${CMAKE_SOURCE_DIR}/tools/dict_lookup_golden.tsv -o ${CMAKE_BINARY_DIR}/dict_lookup_golden.json)
  set_tests_properties(dict_lookup_golden PROPERTIES FIXTURES_REQUIRED plaintext)
endif()

vjo_suite(games SOURCES host/tests/test_games.c vita/games_storage.c INCLUDES host/tests/games_stubs LIBS vjo_host)
# Adrenaline's PSP games (core/pspbubble.c, core/psptarget.c, core/taicfg.c):
# synthetic bubble files, PSP images and taiHEN configurations. With zlib the
# CSO images' deflate blocks are compressed and read back too.
find_package(ZLIB QUIET)
add_executable(vjo-psp-tests host/tests/test_psp.c)
target_include_directories(vjo-psp-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-psp-tests vjo_core)
target_compile_options(vjo-psp-tests PRIVATE -Wall -Wextra)
if(ZLIB_FOUND)
  target_compile_definitions(vjo-psp-tests PRIVATE VJO_TEST_ZLIB=1)
  target_link_libraries(vjo-psp-tests ZLIB::ZLIB)
endif()
add_test(NAME psp COMMAND vjo-psp-tests)
# The config.txt editor (core/taicfg.c) on tools/tests/fixtures/taicfg and a generated corpus, with
# taiHEN's own parser (third_party/taihen-parser) as the check; vjo-taicfg runs it for the PC
# installer's tests (tools/tests/test_tai_config.py compares the two editors) and writes the
# fixtures' expected results (vjo-taicfg expect DIR).
add_executable(vjo-taicfg host/taicfg_tool.c)
target_link_libraries(vjo-taicfg vjo_core)
target_compile_options(vjo-taicfg PRIVATE -Wall -Wextra)
vjo_suite(taicfg SOURCES host/tests/test_taicfg.c DEFINES VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" LIBS vjo_core)
# The Shell's foreground classification (shell/game.c) with AppMgr, the
# process title and the memory card stubbed: bubbles, Adrenaline, the
# emulator's NPXS10028, disc IDs and Vita games.
vjo_suite(shell_game SOURCES host/tests/test_shell_game.c shell/game.c vita/games_storage.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_TEST_APPMGR_STUBS=1 UNUSED LIBS vjo_host)
vjo_suite(preferences SOURCES host/tests/test_preferences.c settings/preferences.c shell/files.c
  vita/games_storage.c # shell/files.c's reads are games_storage.c's (#418)
  INCLUDES host/tests/vita_stubs DEFINES VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" UNUSED LIBS vjo_host)
# The Rubi app's own settings file, app.ini (settings/app_prefs.c): its lines as config.ini's, its saves through
# config.ini's file layer (settings/preferences.c) without config.ini's removed-key rules, every row of its table.
vjo_suite(app_prefs SOURCES host/tests/test_app_prefs.c settings/app_prefs.c settings/preferences.c
  INCLUDES host/tests/vita_stubs UNUSED LIBS vjo_host)
# The Rubi app's actual input/draw loop (settings/main.c) with a scripted SDK
# boundary: the OCR card's Model size, its status line naming the size's model;
# then the same app with a later package's model table (generation VOCR6 for
# two sizes, VOCR5 kept for Large), which it names unchanged.
# The Rubi app's sources the four harnesses that drive settings/main.c compile alike (the two suites here, the Games
# page's suite and its bench below): object libraries, each unit compiled once. What the harnesses define besides
# reaches only their own units: VJO_VOCR_MODEL_INFO_TEST core/vocr_models.c and test_settings_app.c, VJO_SETTINGS_BENCH
# test_settings_games.c, VJO_TEST_ZLIB settings/psp.c and the Games page's suite (vjo_settings_catalog, below).
add_library(vjo_settings_app OBJECT settings/app_prefs.c settings/model_size.c settings/preferences.c settings/icons.c settings/text_layout.c
  settings/letter.c settings/app_check.c settings/applog.c settings/search.c host/tests/settings_stubs/search_dialog_stub.c
  vita/games_storage.c vita/buttons_vita.c host/tests/settings_stubs/streak_seed_stub.c
  host/tests/settings_stubs/anki_page_stub.c host/tests/settings_stubs/install_page_stub.c
  host/tests/settings_stubs/lookup_page_stub.c host/tests/settings_stubs/app_worker_stub.c)
add_library(vjo_settings_main OBJECT settings/main.c)
set_source_files_properties(settings/main.c PROPERTIES COMPILE_DEFINITIONS main=vjo_settings_main)
foreach(_vjo_target IN ITEMS vjo_settings_app vjo_settings_main vjo-settings-app-tests vjo-settings-vocr6-tests)
  if(NOT TARGET ${_vjo_target})
    add_executable(${_vjo_target} host/tests/test_settings_app.c)
    target_link_libraries(${_vjo_target} vjo_settings_main vjo_settings_app)
    string(REGEX REPLACE "^vjo-settings-(.*)-tests$" "settings_\\1" _vjo_test ${_vjo_target})
    add_test(NAME ${_vjo_test} COMMAND ${_vjo_target})
  endif()
  target_include_directories(${_vjo_target} PRIVATE third_party/acutest/include host/tests/settings_stubs
    host/tests/vita_stubs host/tests)
  target_compile_options(${_vjo_target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(${_vjo_target} vjo_host)
  # Their own sceKernelGetThreadId (settings/applog.c's batch owner), not host_stubs.h's inline 0.
  target_compile_definitions(${_vjo_target} PRIVATE VJO_TEST_THREAD_INFO_STUBS=1)
endforeach()
# The later package's metadata replaces this build's (core/vocr_models.c, compiled in).
target_sources(vjo-settings-vocr6-tests PRIVATE core/vocr_models.c)
target_compile_definitions(vjo-settings-vocr6-tests PRIVATE VJO_VOCR_MODEL_INFO_TEST="test_vocr6_models.h")
# The Rubi app's text layouts (settings/text_layout.c, against main.c's old
# wrapped_text), its icon file check and cache (settings/icons.c) and the
# titles' letters and order (settings/letter.c).
vjo_suite(settings_layout SOURCES host/tests/test_settings_layout.c settings/text_layout.c core/linebreak.c
  settings/icons.c settings/letter.c INCLUDES host/tests/vita_stubs UNUSED LIBS vjo_host)
# The Games page through the actual app (settings/main.c) and catalog
# (settings/catalog.c) over synthetic installed games: the icon cache, the
# loads per frame, the title layouts, the page and letter jumps. With zlib
# the icons are compressed and inflated as libpng would; without it they are
# stored blocks. vjo-settings-bench times the same scenarios per frame;
# VJO_SETTINGS_BENCH_MAIN builds it with another settings/main.c (the
# change's "before" measurement used main 7ed16d8's).
find_package(ZLIB QUIET)
set(VJO_SETTINGS_BENCH_MAIN ${CMAKE_SOURCE_DIR}/settings/main.c CACHE FILEPATH "settings/main.c for vjo-settings-bench")
add_library(vjo_settings_catalog OBJECT settings/catalog.c settings/psp.c)
foreach(_vjo_target IN ITEMS vjo_settings_catalog vjo-settings-games-tests vjo-settings-bench)
  if(NOT TARGET ${_vjo_target})
    add_executable(${_vjo_target} host/tests/test_settings_games.c)
    target_link_libraries(${_vjo_target} vjo_settings_catalog vjo_settings_app)
  endif()
  target_include_directories(${_vjo_target} PRIVATE third_party/acutest/include host/tests/settings_stubs
    host/tests/vita_stubs host/tests)
  target_compile_options(${_vjo_target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(${_vjo_target} vjo_host)
  if(ZLIB_FOUND)
    target_compile_definitions(${_vjo_target} PRIVATE VJO_TEST_ZLIB=1)
    target_link_libraries(${_vjo_target} ZLIB::ZLIB)
  endif()
  # As the app suites above: the harness's own sceKernelGetThreadId.
  target_compile_definitions(${_vjo_target} PRIVATE VJO_TEST_THREAD_INFO_STUBS=1)
endforeach()
foreach(_vjo_games_runner IN ITEMS vjo-settings-games-tests vjo-settings-bench)
  target_link_options(${_vjo_games_runner} PRIVATE -Wl,--wrap=vjo_icon_cache_get)
endforeach()
target_link_libraries(vjo-settings-games-tests vjo_settings_main)
if(VJO_SETTINGS_BENCH_MAIN STREQUAL ${CMAKE_SOURCE_DIR}/settings/main.c)
  target_link_libraries(vjo-settings-bench vjo_settings_main)
else()
  set_source_files_properties(${VJO_SETTINGS_BENCH_MAIN} PROPERTIES COMPILE_DEFINITIONS main=vjo_settings_main)
  target_sources(vjo-settings-bench PRIVATE ${VJO_SETTINGS_BENCH_MAIN})
endif()
target_compile_definitions(vjo-settings-bench PRIVATE VJO_SETTINGS_BENCH=1)
# A main.c from elsewhere finds its headers in settings/.
target_include_directories(vjo-settings-bench PRIVATE settings)
add_test(NAME settings_games COMMAND vjo-settings-games-tests)
vjo_suite(settings_search SOURCES host/tests/test_settings_search.c settings/search.c
  host/tests/settings_stubs/search_dialog_stub.c INCLUDES host/tests/settings_stubs LIBS vjo_host)
# The one-VPK install (settings/install.c, install_page.c, core/payload.c, docs/one-vpk-install.md) on a
# temporary memory card with a payload: fresh installs, updates, config.ini, faults at every write and
# rename, the interrupted swap, uninstall, the manifest's rules, the page. The kernel loads the worker
# (VJO_KERNEL_LOADS_WORKER, 1 in include/vjo_api.h; defined here for the test, which also
# checks the lines mode's refusal of another build's payload).
vjo_suite(settings_install SOURCES host/tests/test_settings_install.c settings/install.c settings/install_page.c
  settings/app_worker.c core/boot_guard.c
  settings/preferences.c settings/text_layout.c host/tests/settings_stubs/anki_page_stub.c
  INCLUDES host/tests/install_stubs host/tests/settings_stubs host/tests/vita_stubs third_party/bearssl/inc
  DEFINES VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" VJO_VERSION_STRING="0.8.0" VJO_KERNEL_LOADS_WORKER=1
  VJO_TEST_CREATE_STUBS VJO_TEST_MUTEX_STUBS VJO_TEST_THREAD_STUBS
  VJO_PREFERENCES_PATH_TEST="ux0:data/VitaJPOverlay/config.ini" UNUSED LIBS vjo_host)
# The Rubi app's Anki page itself (settings/anki_page.c, #163 c3): its rows' layout, Up/Down, the
# stick, Left/Right, Enter and touch on each row, each row's save into a real config.ini (read back by the
# Shell's own loader, shell/files.c), and its export (the real settings/anki_export.c) on a temporary
# memory card, on its worker thread, run when the test says. The harnesses that drive main.c keep
# settings_stubs/anki_page_stub.c.
vjo_suite(settings_anki SOURCES host/tests/test_settings_anki.c settings/anki_page.c settings/anki_export.c settings/text_layout.c
  settings/app_worker.c
  settings/app_prefs.c settings/app_platform.c settings/preferences.c shell/files.c vita/games_storage.c
  INCLUDES host/tests/settings_stubs host/tests/install_stubs host/tests/vita_stubs
  DEFINES VJO_TEST_CREATE_STUBS VJO_TEST_MUTEX_STUBS VJO_TEST_THREAD_STUBS UNUSED LIBS vjo_host)
# The Rubi app's seed of the overlay's reading streak (settings/streak_seed.c) on a temporary memory card: once (a
# seeded record is never read again), history.csv's days, the Shell's record kept, a stop that writes nothing, a
# history.csv that cannot be read, stats_cache.bin removed; its thread run when the test says.
vjo_suite(settings_streak SOURCES host/tests/test_settings_streak.c settings/streak_seed.c
  INCLUDES host/tests/settings_stubs host/tests/install_stubs host/tests/vita_stubs DEFINES VJO_TEST_CREATE_STUBS VJO_TEST_THREAD_STUBS UNUSED
  LIBS vjo_host)
# The same seed's real window loop (settings/streak_seed.c count_history) on history.csv files of many windows, LF and
# CRLF, a newest record not whole, a record longer than a window and one of a window less a byte; and the merge's
# runs that end on one day or overlap (#501's audit, F1 and F2).
vjo_suite(streak_seed_files SOURCES host/tests/test_streak_seed_files.c settings/streak_seed.c
  INCLUDES host/tests/settings_stubs host/tests/install_stubs host/tests/vita_stubs DEFINES VJO_TEST_CREATE_STUBS
  VJO_TEST_THREAD_STUBS UNUSED LIBS vjo_host)
# libvita2d's plain textures through the app's wrapper (settings/vita2d_textures.c, #313): no stale render target or
# depth buffer, so that a font freed while the app runs frees nothing else.
vjo_suite(settings_textures SOURCES host/tests/test_settings_textures.c settings/vita2d_textures.c
  INCLUDES host/tests/settings_stubs)
# libvita2d's font initialization in a separate object: the real link wrappers
# must change firmware raster size, packer AND texture bounds, and minification.
vjo_suite(settings_font_raster SOURCES host/tests/test_settings_font_raster.c host/tests/font_raster_model.c
  settings/font_raster.c INCLUDES host/tests/font_stubs host/tests/settings_stubs
  LINK_OPTIONS -Wl,--wrap=scePvfSetCharSize,--wrap=texture_atlas_create,--wrap=vita2d_texture_set_filters)
# The Rubi app's Look up page itself (settings/lookup_page.c, #313): its frames recorded through the helpers main.c
# gives it, on a temporary memory card with config.ini, two small dictionaries, history.csv and the Anki queue; the
# keyboard is settings_stubs/search_dialog_stub.c's. The harnesses that drive main.c keep settings_stubs/lookup_page_stub.c.
vjo_suite(settings_lookup SOURCES host/tests/test_settings_lookup.c settings/lookup_page.c settings/lookup.c
  settings/lookup_history.c settings/app_queue.c settings/app_platform.c settings/search.c settings/preferences.c
  host/tests/settings_stubs/search_dialog_stub.c
  INCLUDES host/tests/settings_stubs host/tests/install_stubs host/tests/vita_stubs host/tests UNUSED LIBS vjo_host)
vjo_suite(game_input SOURCES host/tests/test_game_input.c kernel/triggers.c INCLUDES host/tests/vita_stubs UNUSED
  LIBS vjo_host)

# Real role-restricted job transport; no simulated inference/accuracy claims.
vjo_suite(kernel_game_ocr SOURCES host/tests/test_kernel_game_ocr.c core/hook_profile_file.c kernel/scene.c kernel/ledger.c core/ledger.c
  INCLUDES host/tests/vita_stubs DEFINES VJO_HOST=1 UNUSED)
vjo_suite(game_ocr_client SOURCES host/tests/test_game_ocr_client.c NO_WARNINGS LIBS vjo_host)

if(VJO_WITH_VOCR)
  vjo_suite(game_vocr_worker TARGET vjo-game-vocr-tests SOURCES host/tests/test_game_vocr.c host/tests/hooks_none.c
    INCLUDES host/tests/game_stubs
    DEFINES VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" VJO_VOCR_GATE_TEST_DIR="${VJO_VOCR_GATE_TEST_DIR}" UNUSED
    LIBS vjo_vocr)
  # The same game worker and the Shell's job with another package's tier map, Normal
  # and Large one model (host/tests/test_shared_tiers.h): each file's own tests for it.
  vjo_suite(game_vocr_shared_tiers TARGET vjo-game-vocr-shared-tests
    SOURCES host/tests/test_game_vocr.c host/tests/hooks_none.c INCLUDES host/tests/game_stubs host/tests
    DEFINES VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" VJO_VOCR_TIERS_TEST="test_shared_tiers.h" UNUSED LIBS vjo_vocr)
  # the Shell's tier names come from core/config.c, compiled in with the same map
  vjo_suite(shell_worker_shared_tiers TARGET vjo-shell-worker-shared-tests
    SOURCES host/tests/test_shell_worker.c core/config.c core/vocr_models.c core/region_select.c
    INCLUDES host/tests/vita_stubs host/tests DEFINES VJO_VOCR_TIERS_TEST="test_shared_tiers.h" UNUSED
    LIBS vjo_host vjo_vocr)
  # The actual game worker and core adapter with synthetic pinned weights and a
  # spy runtime: the resident-weights lifecycle runs without release files (CI).
  vjo_suite(game_resident_weights TARGET vjo-game-resident-tests
    SOURCES host/tests/test_game_resident.c core/vocr_ocr.c core/textnorm.c core/region_suggest.c
    host/tests/hooks_none.c INCLUDES host/tests/game_stubs host/tests
    DEFINES VJO_VOCR_TEST_MODELS="test_resident_models.h" vocr_find_lines_rows=test_vocr_find_lines_rows
    vocr_find_lines_fix_rows=test_vocr_find_lines_fix_rows vocr_load=test_vocr_load
    vocr_arena_bytes=test_vocr_arena_bytes vocr_init=test_vocr_init vocr_run=test_vocr_run UNUSED
    LIBS vjo_vocr_rt vjo_host)
  # API 54: the text spotter in the actual worker (game_ocr/main.c): its weights through the broker into a block of
  # their own, kept and given back, and GRAD whenever it cannot run; the real spotter on the committed random blob,
  # spies for the recognizer and GRAD (host/tests/test_game_spot.c).
  vjo_suite(game_spot SOURCES host/tests/test_game_spot.c core/vocr_ocr.c core/textnorm.c core/region_suggest.c
    host/tests/hooks_none.c INCLUDES host/tests/game_stubs host/tests
    DEFINES VJO_VOCR_TEST_MODELS="test_spot_models.h" VJO_VOCR_SPOT_TEST="test_spot_pins.h"
    VJO_SOURCE_DIR="${CMAKE_SOURCE_DIR}" vocr_find_lines_rows=test_vocr_find_lines_rows
    vocr_find_lines_fix_rows=test_vocr_find_lines_fix_rows vocr_load=test_vocr_load
    vocr_arena_bytes=test_vocr_arena_bytes vocr_init=test_vocr_init vocr_run=test_vocr_run UNUSED
    LIBS vjo_vocr_rt vjo_host)
  # Native hooks in the job path (docs/native-hooks.md): the actual worker with the resident test's spies and
  # a fake of game_ocr/hooks.h: a stream answers with the first capture pass only, or replaces OCR's agreeing
  # text; nothing installed is OCR as before.
  vjo_suite(game_hooks SOURCES host/tests/test_game_hooks.c core/vocr_ocr.c core/textnorm.c core/region_suggest.c
    INCLUDES host/tests/game_stubs host/tests
    DEFINES VJO_VOCR_TEST_MODELS="test_resident_models.h" vocr_find_lines_rows=test_vocr_find_lines_rows
    vocr_find_lines_fix_rows=test_vocr_find_lines_fix_rows vocr_load=test_vocr_load
    vocr_arena_bytes=test_vocr_arena_bytes vocr_init=test_vocr_init vocr_run=test_vocr_run UNUSED
    LIBS vjo_vocr_rt vjo_host)
  # The trusted-hook bench (docs/native-hooks.md "Trusted hooks", tools/hook_trust_bench.py): the actual worker,
  # core adapter and runtime with the pinned models replaying hooked pages, one job a page. Built for N = 4 (the
  # default VJO_HOOK_TRUST_INTERVAL, 32) and, for the study of N, 3 (16) and 2 (8); not a test.
  foreach(_vjo_trust IN ITEMS 32 16 8)
    set(_vjo_target vjo-hook-trust-bench)
    if(NOT _vjo_trust EQUAL 32)
      set(_vjo_target vjo-hook-trust-bench-${_vjo_trust})
    endif()
    add_executable(${_vjo_target} host/hook_trust_bench.c core/hook_select.c)
    target_include_directories(${_vjo_target} PRIVATE host/tests/game_stubs host/tests core include)
    target_compile_options(${_vjo_target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(${_vjo_target} PRIVATE VJO_HOOK_TRUST_INTERVAL=${_vjo_trust}u)
    target_link_libraries(${_vjo_target} vjo_vocr vjo_host)
  endforeach()
  # The trusted-hook latency bench (docs/native-hooks.md "No wait for a trusted line", tools/hook_latency_bench.py):
  # the actual worker, kernel transport and client on one virtual clock, from a hook's write to the line's lookup;
  # not a test. Built with the worker's default watch pace and, for the poll's cost, 4, 8 and 33 ms (HOOK_WATCH_US).
  foreach(_vjo_watch IN ITEMS 0 4000 8000 33000)
    set(_vjo_target vjo-hook-latency-bench)
    if(NOT _vjo_watch EQUAL 0)
      set(_vjo_target vjo-hook-latency-bench-${_vjo_watch})
    endif()
    add_executable(${_vjo_target} host/hook_latency_bench.c host/hook_latency_kernel.c kernel/buffers.c
      kernel/capture.c kernel/scene.c kernel/ledger.c core/ledger.c)
    target_include_directories(${_vjo_target} PRIVATE host/tests/game_stubs host/tests/vita_stubs)
    target_compile_options(${_vjo_target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(${_vjo_target} PRIVATE VJO_HOST=1 VJO_TEST_MODEL_BROKER=1)
    if(NOT _vjo_watch EQUAL 0)
      target_compile_definitions(${_vjo_target} PRIVATE HOOK_WATCH_US=${_vjo_watch}u)
    endif()
    target_link_libraries(${_vjo_target} vjo_vocr)
  endforeach()
  # The text-presence gate on many regions as the worker's job runs it (tools/gate_eval.py); not a test.
  add_executable(vjo-gate-eval host/gate_eval.c)
  target_compile_options(vjo-gate-eval PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(vjo-gate-eval vjo_vocr vjo_host)
  add_executable(vjo-game-model-broker-tests host/tests/test_game_model_broker.c host/tests/hooks_none.c
    host/tests/kernel_model_broker_boundary.c core/hook_profile_file.c kernel/buffers.c kernel/capture.c
    kernel/scene.c kernel/ledger.c core/ledger.c)
  target_include_directories(vjo-game-model-broker-tests PRIVATE third_party/acutest/include
    host/tests/game_stubs host/tests/vita_stubs)
  target_compile_options(vjo-game-model-broker-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(vjo-game-model-broker-tests PRIVATE VJO_HOST=1 VJO_TEST_MODEL_BROKER=1
    VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}")
  target_link_libraries(vjo-game-model-broker-tests vjo_vocr)
  # #74 preflight: the real worker and kernel under the console's measured pool limits.
  vjo_suite(alloc_sim SOURCES host/tests/test_alloc_sim.c host/tests/hooks_none.c
    host/tests/kernel_model_broker_boundary.c core/hook_profile_file.c kernel/buffers.c kernel/capture.c kernel/scene.c kernel/ledger.c
    core/ledger.c INCLUDES host/tests/game_stubs host/tests/vita_stubs
    DEFINES VJO_HOST=1 VJO_TEST_MODEL_BROKER=1 VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" UNUSED LIBS vjo_vocr)
  # #528 (mem-leak): the same worker and kernel soaked, job after job in one game process, every pool counted
  # (host/tests/soak_track.h): no item may hold more after a scenario's cycles than after its first.
  # VJO_SOAK_CYCLES=N runs N cycles a scenario (a few by default).
  vjo_suite(game_soak SOURCES host/tests/test_game_soak.c host/tests/hooks_none.c
    host/tests/kernel_model_broker_boundary.c core/hook_profile_file.c kernel/buffers.c kernel/capture.c kernel/scene.c
    kernel/ledger.c core/ledger.c INCLUDES host/tests/game_stubs host/tests/vita_stubs
    DEFINES VJO_HOST=1 VJO_TEST_MODEL_BROKER=1 VJO_VOCR_MODEL_DIR="${VJO_VOCR_MODEL_DIR}" UNUSED LIBS vjo_vocr)
  add_test(NAME game_model_broker COMMAND vjo-game-model-broker-tests)
  # API 23: the Anki screenshot through the actual worker and kernel transport, real files, its
  # JPEGs decoded by Pillow (host/tests/test_game_shot.py).
  add_executable(vjo-game-shot-tests host/tests/test_game_shot.c host/tests/hooks_none.c host/tests/kernel_model_broker_boundary.c core/hook_profile_file.c
    kernel/buffers.c kernel/capture.c kernel/scene.c kernel/ledger.c core/ledger.c)
  target_include_directories(vjo-game-shot-tests PRIVATE third_party/acutest/include host/tests/game_stubs
    host/tests/vita_stubs)
  target_compile_options(vjo-game-shot-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_definitions(vjo-game-shot-tests PRIVATE VJO_HOST=1 VJO_TEST_MODEL_BROKER=1)
  target_link_libraries(vjo-game-shot-tests vjo_vocr)
  add_test(NAME game_shot COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/test_game_shot.py
    $<TARGET_FILE:vjo-game-shot-tests>)
  # A mined card carries the game's screenshot, end to end: the game worker's JPEG of a frame, the Shell's Enter that
  # saves the card with it, the Rubi app's export, the note's Picture (docs/anki.md "The screenshot").
  add_test(NAME card_screenshot COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/test_card_screenshot.py
    $<TARGET_FILE:vjo-game-shot-tests> $<TARGET_FILE:vjo-shell-anki-tests> $<TARGET_FILE:vjo-apkg-tests>)
  # The stage benchmark (docs/performance.md): every stage of an OCR job and of the Shell's
  # lookups on the real sources. tools/bench_stages.py times it here and counts the Vita
  # compiler's instructions under qemu-arm from a Vita build; the smoke test checks that every
  # stage runs and gives the recorded output (no timing).
  add_library(vjo_bench_kernel OBJECT host/bench/kernel_glue.c kernel/capture.c kernel/scene.c)
  target_include_directories(vjo_bench_kernel PRIVATE host/tests/vita_stubs core include)
  target_compile_definitions(vjo_bench_kernel PRIVATE VJO_HOST=1)
  add_executable(vjo-stage-bench host/bench/bench_main.c host/bench/stage_bench.c
    $<TARGET_OBJECTS:vjo_bench_kernel>)
  target_link_libraries(vjo-stage-bench vjo_vocr vjo_core)
  target_compile_options(vjo-stage-bench PRIVATE -Wall -Wextra)
  add_test(NAME stage_bench_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bench_stages.py smoke
    --binary $<TARGET_FILE:vjo-stage-bench> --models "${VJO_VOCR_MODEL_DIR}")
  if(_vjo_plaintext_dir)
    set_tests_properties(stage_bench_smoke PROPERTIES FIXTURES_REQUIRED plaintext
      ENVIRONMENT "VJO_PLAINTEXT_DIR=${_vjo_plaintext_dir}")
  endif()
endif()

# The owner scenario (#528, docs/owner-scenario.md): Rubi's memory failures on the owner's console replayed through
# Rubi's own code, the Shell's Paf requests (shell_worker_paf's "owner scenario") and, with the release models, the
# game worker's job at the logged free USER sizes (alloc_sim's), held against tools/owner_scenario.json's limits.
set(_vjo_owner_scenario --shell-bin $<TARGET_FILE:vjo-shell-worker-paf-tests>)
if(TARGET vjo-alloc-sim-tests)
  list(APPEND _vjo_owner_scenario --game-bin $<TARGET_FILE:vjo-alloc-sim-tests>)
endif()
add_test(NAME owner_scenario COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/owner_scenario.py --check
  ${_vjo_owner_scenario})

# The variant_free label's derivation (host/tests/variant_free.py, the end of this file) on small builds of its own.
add_test(NAME variant_free_rules COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/test_variant_free.py)

# Every test runs with a TMPDIR of its own, which must be empty again when the run ends
# (host/tests/test_tmpdir.h, host/tests/tmp_leak.py): <this build's directory>/TEST, the build's
# directory in the system's temporary directory (outside the source tree, as /tmp is).
# tmp_leak_setup makes each test's directory, empty, before the first test; tmp_leak_check fails on
# whatever a test left in its own, naming the test, or on a test source that names /tmp instead, and
# removes them. Keep this last: it covers the tests added above it.
set(_vjo_tmp_base "$ENV{TMPDIR}")
if(NOT IS_ABSOLUTE "${_vjo_tmp_base}" OR _vjo_tmp_base MATCHES ";")
  set(_vjo_tmp_base /tmp)
endif()
string(SHA1 _vjo_tmp_id "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_vjo_tmp_id}" 0 12 _vjo_tmp_id)
set(VJO_TEST_TMPDIR "${_vjo_tmp_base}/vjo-ctest-${_vjo_tmp_id}")
get_property(_vjo_tests DIRECTORY PROPERTY TESTS)
foreach(_vjo_test IN LISTS _vjo_tests)
  set_property(TEST ${_vjo_test} APPEND PROPERTY ENVIRONMENT "TMPDIR=${VJO_TEST_TMPDIR}/${_vjo_test}")
  set_property(TEST ${_vjo_test} APPEND PROPERTY FIXTURES_REQUIRED tmp_leak)
endforeach()
add_test(NAME tmp_leak_setup COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/tmp_leak.py
  setup ${VJO_TEST_TMPDIR} ${_vjo_tests})
add_test(NAME tmp_leak_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/tmp_leak.py
  check ${VJO_TEST_TMPDIR} ${_vjo_tests})
set_tests_properties(tmp_leak_setup PROPERTIES FIXTURES_SETUP tmp_leak)
set_tests_properties(tmp_leak_check PROPERTIES FIXTURES_CLEANUP tmp_leak)

# variant_free, derived (host/tests/variant_free.py): a test that the other two lanes' configurations define alike, with
# its programs (and every library linked into them) linked and compiled alike, whose units preprocess to the same text
# with VJO_DEBUG 1 and 0 and with the model directories changed, gives the same result in every lane; the normal
# build's ctest and the models lane (ctest -LE variant_free) leave it to the debug build's run. The tools' suites keep
# the label given above.
# After every program is built, the script writes the labels into a file that ctest includes (TEST_INCLUDE_FILES, from
# host/tests/variant_labels): without a build, or with VJO_VARIANT_LABELS_OFF in the environment, ctest sees CMake's
# labels only.
get_property(_vjo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_vjo_graph)
set(_vjo_programs)
foreach(_vjo_t IN LISTS _vjo_targets)
  get_target_property(_vjo_type ${_vjo_t} TYPE)
  if(NOT _vjo_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$")
    continue()
  endif()
  set(_vjo_deps)
  get_target_property(_vjo_links ${_vjo_t} LINK_LIBRARIES)
  get_target_property(_vjo_srcs ${_vjo_t} SOURCES)
  string(REGEX MATCHALL "\\$<TARGET_OBJECTS:[^>]+>" _vjo_objs "${_vjo_srcs}")
  string(REGEX REPLACE "\\$<TARGET_OBJECTS:([^>]+)>" "\\1" _vjo_objs "${_vjo_objs}")
  foreach(_vjo_d IN LISTS _vjo_links _vjo_objs)
    if(_vjo_d AND TARGET ${_vjo_d})
      list(APPEND _vjo_deps "\"${_vjo_d}\"")
    endif()
  endforeach()
  list(JOIN _vjo_deps ", " _vjo_deps)
  set(_vjo_file "")
  if(_vjo_type STREQUAL EXECUTABLE)
    set(_vjo_file "$<TARGET_FILE:${_vjo_t}>")
    list(APPEND _vjo_programs ${_vjo_t})
  endif()
  list(APPEND _vjo_graph "  \"${_vjo_t}\": {\"type\": \"${_vjo_type}\", \"file\": \"${_vjo_file}\", \"deps\": [${_vjo_deps}]}")
endforeach()
list(JOIN _vjo_graph ",\n" _vjo_graph)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/vjo_targets.json CONTENT "{\n${_vjo_graph}\n}\n")
set(_vjo_labels ${CMAKE_BINARY_DIR}/variant_labels.cmake)
# (again when a program relinks, or when what CMake configures may have changed: its files, the tests it wrote)
file(GLOB _vjo_cmake_files CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/*.cmake)
add_custom_command(OUTPUT ${_vjo_labels}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/host/tests/variant_free.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}
    ${CMAKE_CTEST_COMMAND} ${_vjo_labels} --cmake ${CMAKE_COMMAND}
  DEPENDS ${CMAKE_SOURCE_DIR}/host/tests/variant_free.py ${CMAKE_BINARY_DIR}/vjo_targets.json ${_vjo_programs}
    ${CMAKE_SOURCE_DIR}/CMakeLists.txt ${CMAKE_SOURCE_DIR}/host/tests/variant_labels/CMakeLists.txt ${_vjo_cmake_files}
    ${CMAKE_BINARY_DIR}/CTestTestfile.cmake ${CMAKE_BINARY_DIR}/compile_commands.json
  COMMENT "variant_free: the host tests whose result is the same in every lane (host/tests/variant_free.py)"
  VERBATIM)
add_custom_target(vjo_variant_labels ALL DEPENDS ${_vjo_labels})
set(VJO_VARIANT_LABELS_INCLUDE ${CMAKE_BINARY_DIR}/variant_labels_include.cmake)
file(WRITE ${VJO_VARIANT_LABELS_INCLUDE}
  "if(EXISTS \"${_vjo_labels}\" AND NOT DEFINED ENV{VJO_VARIANT_LABELS_OFF})\n  include(\"${_vjo_labels}\")\nendif()\n")
add_subdirectory(host/tests/variant_labels)
