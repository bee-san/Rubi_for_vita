#!/usr/bin/env bash
# The pure-C kernel and core suites, built for the Vita's CPU (ARMv7-A
# Cortex-A9, 32-bit pointers and size_t, LDREX/STREX atomics) with a Linux
# ARM toolchain and run under qemu-arm user mode. The x86_64 host suites
# cannot see 32-bit width or ARM atomics bugs; this lane can. It is not a
# Vita: no firmware, no SceShell, no taiHEN, no memory budgets
# (docs/offline-testing.md). Suites that need the vocr runtime, BearSSL or
# the full core library are left to the host job and to vita-vn-ocr's own
# ARM parity lane.
#
#   ARM_CC=arm-linux-gnueabihf-gcc QEMU=qemu-arm-static tools/arm_host_tests.sh [build-dir]
#
# With VJO_VOCR_SOURCE_DIR (a vita-vn-ocr checkout at the pinned commit) the
# capture parity test (#74, host/tests/test_capture_parity.c) runs here too:
# API 18's and API 19's capture give the recognizer the same bytes on the
# Vita's CPU (procedural frames; VJO_VOCR_MODEL_DIR adds the preprocessed
# tensors; tools/preflight.sh runs the eval crops through it as well).
#
# With VJO_PLAINTEXT_DIR (the directory holding the bundled plaintext.vjdict,
# e.g. build/host/plaintext after the host ctest) the local dictionary lookup
# runs at every Japanese letter of the real VOCR lines in
# host/fixtures/word_lookup on ARM and natively (HOST_CC, default cc), and the
# two outputs must be identical (host/tests/word_lookup_parity.c).
#
# The Rubi app's .apkg export (host/tests/test_apkg.c: the SQLite and ZIP
# writers, the Lapis notes and every storage step, with BearSSL's SHA-1 and
# SHA-256 only) runs here too, and the packages it writes (--write) must be
# the same bytes on ARM as natively (HOST_CC). So does the hand-over of the
# same cards to Anki for Vita (host/tests/test_afv.c, settings/anki_afv.c).
#
# Every suite runs with a TMPDIR of its own (host/tests/test_tmpdir.h), and the lane's other steps
# (the builds, the comparisons) with theirs, all in one directory of the lane's: each must be empty
# again at the end. The lane fails on whatever is left, named with its suite, and removes the
# directory either way.
#
# Ubuntu: apt-get install gcc-arm-linux-gnueabihf libc6-dev-armhf-cross
# qemu-user-static (the cross gcc alone has no ARM libc headers or static
# libc). The defaults below are those package names; the fleet box sets both
# variables to its own toolchain, which bundles a sysroot. With
# VJO_VOCR_SOURCE_DIR the cross gcc must be GCC 14 or later: the pinned
# runtime's NEON kernels (vita-vn-ocr #85) use vld1q_*_x2, which the 32-bit
# arm_neon.h has from GCC 14 on. On Ubuntu 24.04, whose default is GCC 13,
# install gcc-14-arm-linux-gnueabihf and set ARM_CC=arm-linux-gnueabihf-gcc-14
# (as CI does).
set -euo pipefail
cd "$(dirname "$0")/.."
ARM_CC=${ARM_CC:-arm-linux-gnueabihf-gcc}
QEMU=${QEMU:-qemu-arm-static}
OUT=${1:-build/arm-tests}
mkdir -p "$OUT"
# VJO_DEBUG=0: the normal build's suites (docs/release-variants.md); the default, 1, the debug build's.
VJO_DEBUG=${VJO_DEBUG:-1}
# -D_FILE_OFFSET_BITS=64: a 32-bit readdir() or stat() fails with EOVERFLOW on a file system whose
# directory cookies or inode numbers pass 2^32 (XFS: the taicfg fixtures' readdir under qemu-arm)
FLAGS=(-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wno-unused-parameter
       -I third_party/acutest/include -I host/tests/vita_stubs -I core -I include -DVJO_HOST=1 -DVJO_DEBUG="$VJO_DEBUG")
QEMU_ARGS=(-cpu cortex-a9)
# qemu-arm needs the dynamic loader's directory only for dynamic binaries;
# everything here is static.
"$ARM_CC" --version | head -1
"$QEMU" --version | head -1
TMP_BASE=${TMPDIR:-/tmp}
case $TMP_BASE in /*) ;; *) TMP_BASE=/tmp ;; esac
TMP_ROOT=$(mktemp -d "$TMP_BASE/vjo-arm-tests-XXXXXX")
# (chmod -R gives back the permissions a suite took from a directory of its own; it follows no link)
trap 'chmod -R u+rwx "$TMP_ROOT" 2> /dev/null || :; rm -rf "$TMP_ROOT"' EXIT
tmp_of() { mkdir -p "$TMP_ROOT/$1" && printf '%s' "$TMP_ROOT/$1"; } # tmp_of NAME: that step's own TMPDIR
TMPDIR=$(tmp_of lane)
export TMPDIR

build() { # build NAME SOURCES...
  local name=$1; shift
  "$ARM_CC" "${FLAGS[@]}" -o "$OUT/$name" "$@"
  file "$OUT/$name" 2>/dev/null | sed -E 's/, (BuildID|for GNU|with debug_info|not stripped).*//' || true
}

build boot_guard host/tests/test_boot_guard.c core/boot_guard.c kernel/boot.c
build ledger host/tests/test_ledger.c core/ledger.c
build fbgrey host/tests/test_fbgrey.c
build kernel_capture host/tests/test_kernel_capture.c kernel/scene.c core/region_select.c
build kernel_input host/tests/test_kernel_input.c kernel/triggers.c
# #104: the process event hook's input release (LDREX/STREX exchange) and the page's touch hold.
build kernel_lifecycle host/tests/test_kernel_lifecycle.c kernel/foreground.c
build touch_hold host/tests/test_touch_hold.c
build kernel_game_ocr host/tests/test_kernel_game_ocr.c kernel/scene.c kernel/ledger.c core/ledger.c
# #138: the worker loader's question to the kernel worker (a 32-bit seqlock and
# tagged answer) and the Shell's games.ini rules, on the Vita's word size.
build kernel_loader host/tests/test_kernel_loader.c host/tests/games_io_posix.c vita/games_storage.c core/games.c \
  core/arena.c kernel/ledger.c core/ledger.c
# the self-test's memory routes: the debug build's only
[ "$VJO_DEBUG" = 0 ] || build kernel_meminfo host/tests/test_kernel_meminfo.c core/ledger.c
build region_select host/tests/test_region_select.c core/region_select.c core/regions.c core/arena.c
# The overlay's arrow keys over the header's displayed rows (#81): pure C, float cell edges.
build text_rows host/tests/test_text_rows.c core/text_rows.c core/utf.c
# The recognized text's lines as one running text (core/textjoin.h): byte offsets, 32-bit size_t.
build text_join host/tests/test_textjoin.c core/textjoin.c core/utf.c
# Where the overlay's own wrapping may break a line (core/linebreak.h).
build linebreak host/tests/test_linebreak.c core/linebreak.c core/utf.c
# The overlay's left stick (#138): its clock arithmetic (64-bit microseconds, 32-bit credit) and the step.
build stick_cursor host/tests/test_stick_cursor.c core/text_rows.c core/utf.c
# The cursor over several lines, d-pad and stick (owner 2026-10-16): 32-bit offsets, float cells.
build cursor_nav host/tests/test_cursor_nav.c core/text_rows.c core/utf.c
# The Rubi app's text layouts, icon file check and title letters: 32-bit size_t in the PNG chunk walk.
build settings_layout host/tests/test_settings_layout.c settings/text_layout.c core/linebreak.c settings/icons.c settings/letter.c core/utf.c
# taiHEN's own parser (third_party/taihen-parser, built as taiHEN builds it), the config.txt editor's check
TP=(-I third_party/taihen-parser/include third_party/taihen-parser/src/lexer.c third_party/taihen-parser/src/parser.c)
build psp -DNO_CTYPE -DNO_STRING host/tests/test_psp.c core/pspbubble.c core/psptarget.c core/taicfg.c core/games.c core/sfo.c "${TP[@]}"
# The one-VPK install's config.txt editor (core/taicfg.c) on its fixtures and corpus, on the Vita's word size.
build taicfg -DNO_CTYPE -DNO_STRING -DVJO_SOURCE_DIR="\"$PWD\"" host/tests/test_taicfg.c core/taicfg.c "${TP[@]}"
# The Shell's logger (shell/log_vita.c) on an in-memory file system: batched appends, 32-bit sizes.
# Its stubs come before vita_stubs (both have psp2/io/fcntl.h).
"$ARM_CC" -I host/tests/log_stubs "${FLAGS[@]}" -o "$OUT/shell_log" host/tests/test_shell_log.c

# The Rubi app's .apkg export: 32-bit size_t in the B-tree, overflow pages and ZIP offsets. Its
# picture cards carry the game worker's JPEG (core/jpegsw.c), which must come out the same on the A9.
BS=third_party/bearssl/src
APKG=(host/tests/test_apkg.c core/apkg.c core/sqlw.c core/zipw.c core/lapis.c core/anki.c core/textjoin.c core/anki_queue.c core/json.c
      core/entries.c core/furigana.c core/pitch.c core/utf.c core/arena.c core/jpegsw.c "$BS/hash/sha1.c" "$BS/hash/sha2small.c"
      "$BS/codec/dec32be.c" "$BS/codec/enc32be.c")
build apkg -I third_party/bearssl/inc -I "$BS" "${APKG[@]}"
# The Rubi app's hand-over to Anki for Vita (settings/anki_afv.c, #220): its notes, the 64-bit times
# and 32-bit sizes on the Vita's CPU, and the export's own steps it reuses (settings/anki_export.c).
AFV=(host/tests/test_afv.c settings/anki_afv.c settings/anki_export.c settings/anki_index_request.c
     "${APKG[@]:1}")
build afv -I third_party/bearssl/inc -I "$BS" -I host/tests "${AFV[@]}"
# Anki for Vita's duplicate index as the overlay reads it (core/anki_index_find.c): 32-bit offsets and the
# little-endian words on the Vita's CPU.
build anki_index -I third_party/bearssl/inc -I "$BS" -I host/tests host/tests/test_anki_index.c core/anki_index_find.c \
  "$BS/hash/sha2small.c" "$BS/codec/dec32be.c" "$BS/codec/enc32be.c"

TESTS=(boot_guard ledger fbgrey kernel_capture kernel_input kernel_lifecycle kernel_game_ocr kernel_loader region_select)
[ "$VJO_DEBUG" = 0 ] || TESTS+=(kernel_meminfo)
TESTS+=(touch_hold text_rows text_join linebreak stick_cursor cursor_nav settings_layout shell_log apkg afv anki_index psp
        taicfg)
if [ -n "${VJO_VOCR_SOURCE_DIR:-}" ]; then
  RT=$VJO_VOCR_SOURCE_DIR/runtime
  "$ARM_CC" "${FLAGS[@]}" -c -o "$OUT/parity_kernel.o" host/tests/parity_kernel.c
  "$ARM_CC" "${FLAGS[@]}" -c -o "$OUT/parity_capture.o" kernel/capture.c
  "$ARM_CC" "${FLAGS[@]}" -c -o "$OUT/parity_scene.o" kernel/scene.c
  MODEL_DEF=()
  [ -n "${VJO_VOCR_MODEL_DIR:-}" ] && MODEL_DEF=(-DVJO_VOCR_MODEL_DIR="\"$VJO_VOCR_MODEL_DIR\"")
  "$ARM_CC" -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    "${MODEL_DEF[@]}" -I third_party/acutest/include -I core -I include -I "$RT" -I third_party/bearssl/inc -I third_party/bearssl/src \
    -o "$OUT/capture_parity" host/tests/test_capture_parity.c host/tests/parity_api18.c host/tests/parity_api19.c \
    "$OUT/parity_kernel.o" "$OUT/parity_capture.o" "$OUT/parity_scene.o" "$RT/vocr.c" "$RT/vocr_lines.c" \
    "$RT/vocr_lines_rows.c" "$RT/vocr_lm.c" "$RT/vocr_lines_refine.c" "$RT/vocr_textgate.c" \
    third_party/bearssl/src/hash/sha2small.c third_party/bearssl/src/codec/dec32be.c \
    third_party/bearssl/src/codec/enc32be.c -lm
  TESTS+=(capture_parity)
fi

fail=0
"${HOST_CC:-cc}" -O2 -Wall -Wextra -Wno-unused-parameter -I third_party/acutest/include -I host/tests/vita_stubs -I core \
  -I include -I third_party/bearssl/inc -I "$BS" -DVJO_HOST=1 -o "$OUT/apkg_host" "${APKG[@]}"
rm -rf "$OUT/apkg.arm" "$OUT/apkg.host" && mkdir -p "$OUT/apkg.arm" "$OUT/apkg.host"
if TMPDIR=$(tmp_of apkg_write) "$QEMU" "${QEMU_ARGS[@]}" "$OUT/apkg" --write "$OUT/apkg.arm" > "$OUT/apkg_write.log" 2>&1 &&
   TMPDIR=$(tmp_of apkg_write) "$OUT/apkg_host" --write "$OUT/apkg.host" >> "$OUT/apkg_write.log" 2>&1 &&
   diff -r "$OUT/apkg.arm" "$OUT/apkg.host" > /dev/null; then
  echo "arm apkg packages: $(ls "$OUT/apkg.arm" | grep -c '\.apkg$') .apkg files, identical to the host's"
else
  echo "arm apkg packages: FAILED"; cat "$OUT/apkg_write.log"; fail=1
fi
if [ -n "${VJO_PLAINTEXT_DIR:-}" ]; then
  WL=(host/tests/word_lookup_parity.c core/local_dict.c core/textjoin.c core/utf.c core/arena.c)
  WL_FLAGS=(-O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 -I core -I include -DVJO_HOST=1)
  "$ARM_CC" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static "${WL_FLAGS[@]}" -o "$OUT/word_lookup_parity" "${WL[@]}"
  "${HOST_CC:-cc}" "${WL_FLAGS[@]}" -o "$OUT/word_lookup_parity_host" "${WL[@]}"
  fx=host/fixtures/word_lookup/vocr_lines.tsv
  if TMPDIR=$(tmp_of word_lookup) "$QEMU" "${QEMU_ARGS[@]}" "$OUT/word_lookup_parity" "$VJO_PLAINTEXT_DIR" "$fx" \
       > "$OUT/word_lookup.arm" 2> "$OUT/word_lookup.log" &&
     TMPDIR=$(tmp_of word_lookup) "$OUT/word_lookup_parity_host" "$VJO_PLAINTEXT_DIR" "$fx" > "$OUT/word_lookup.host" \
       2>> "$OUT/word_lookup.log" &&
     cmp -s "$OUT/word_lookup.arm" "$OUT/word_lookup.host"; then
    echo "arm word_lookup: $(head -1 "$OUT/word_lookup.log"), identical to the host's"
  else
    echo "arm word_lookup: FAILED"; cat "$OUT/word_lookup.log"
    diff "$OUT/word_lookup.arm" "$OUT/word_lookup.host" | head -20 || true; fail=1
  fi
fi
for t in "${TESTS[@]}"; do
  if TMPDIR=$(tmp_of "$t") "$QEMU" "${QEMU_ARGS[@]}" "$OUT/$t" > "$OUT/$t.log" 2>&1; then
    echo "arm $t: $(grep -c '\[ OK \]' "$OUT/$t.log") cases pass"
  else
    echo "arm $t: FAILED"; tail -20 "$OUT/$t.log"; fail=1
  fi
done
# The record the kernel writes must be the same bytes on ARM and on the host.
TMPDIR=$(tmp_of boot_guard) "$QEMU" "${QEMU_ARGS[@]}" "$OUT/boot_guard" record_format_and_parse_round_trip > /dev/null
# What each suite or step left in its TMPDIR (SUITE/ENTRY), and anything outside them, listed whole.
chmod -R u+rwx "$TMP_ROOT" 2> /dev/null || true
left=$(cd "$TMP_ROOT" && { find . -mindepth 2 -maxdepth 2; find . -mindepth 1 -maxdepth 1 ! -type d; } | sed 's|^\./||' | sort)
if [ -z "$left" ]; then
  echo "arm TMPDIR: nothing left in it"
else
  echo "arm TMPDIR: FAILED, $(printf '%s\n' "$left" | wc -l) entries left:"
  while IFS= read -r e; do
    case $e in
      */*) echo "  ${e%%/*}: ${e#*/}: $(find "$TMP_ROOT/$e" | wc -l) files and directories" ;;
      *) echo "  $e (in no suite's directory)" ;;
    esac
  done <<< "$left"
  fail=1
fi
exit $fail
