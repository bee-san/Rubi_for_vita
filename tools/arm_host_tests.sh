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
# Ubuntu: apt-get install gcc-arm-linux-gnueabihf libc6-dev-armhf-cross
# qemu-user-static (the cross gcc alone has no ARM libc headers or static
# libc). The defaults below are those package names; the fleet box sets both
# variables to its own toolchain, which bundles a sysroot.
set -euo pipefail
cd "$(dirname "$0")/.."
ARM_CC=${ARM_CC:-arm-linux-gnueabihf-gcc}
QEMU=${QEMU:-qemu-arm-static}
OUT=${1:-build/arm-tests}
mkdir -p "$OUT"
FLAGS=(-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -Wall -Wextra -Wno-unused-parameter
       -I third_party/acutest/include -I host/tests/vita_stubs -I core -I include -DVJO_HOST=1)
QEMU_ARGS=(-cpu cortex-a9)
# qemu-arm needs the dynamic loader's directory only for dynamic binaries;
# everything here is static.
"$ARM_CC" --version | head -1
"$QEMU" --version | head -1

build() { # build NAME SOURCES...
  local name=$1; shift
  "$ARM_CC" "${FLAGS[@]}" -o "$OUT/$name" "$@"
  file "$OUT/$name" 2>/dev/null | sed -E 's/, (BuildID|for GNU|with debug_info|not stripped).*//' || true
}

build boot_guard host/tests/test_boot_guard.c core/boot_guard.c kernel/boot.c
build ledger host/tests/test_ledger.c core/ledger.c
build kernel_capture host/tests/test_kernel_capture.c kernel/scene.c kernel/ledger.c core/ledger.c core/region_select.c
build kernel_input host/tests/test_kernel_input.c kernel/triggers.c
build kernel_game_ocr host/tests/test_kernel_game_ocr.c kernel/scene.c kernel/ledger.c core/ledger.c
build kernel_meminfo host/tests/test_kernel_meminfo.c core/ledger.c
build region_select host/tests/test_region_select.c core/region_select.c core/regions.c core/arena.c
# The overlay's arrow keys over the header's displayed rows (#81): pure C, float cell edges.
build text_rows host/tests/test_text_rows.c core/text_rows.c core/utf.c
# The Overlay app's text layouts, icon file check and title letters: 32-bit size_t in the PNG chunk walk.
build settings_layout host/tests/test_settings_layout.c settings/text_layout.c settings/icons.c settings/letter.c core/utf.c

TESTS=(boot_guard ledger kernel_capture kernel_input kernel_game_ocr kernel_meminfo region_select)
TESTS+=(text_rows settings_layout)
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
    "$RT/vocr_lines_rows.c" "$RT/vocr_lm.c" "$RT/vocr_lines_refine.c" third_party/bearssl/src/hash/sha2small.c third_party/bearssl/src/codec/dec32be.c \
    third_party/bearssl/src/codec/enc32be.c -lm
  TESTS+=(capture_parity)
fi

fail=0
if [ -n "${VJO_PLAINTEXT_DIR:-}" ]; then
  WL=(host/tests/word_lookup_parity.c core/local_dict.c core/utf.c core/arena.c)
  WL_FLAGS=(-O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 -I core -I include -DVJO_HOST=1)
  "$ARM_CC" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static "${WL_FLAGS[@]}" -o "$OUT/word_lookup_parity" "${WL[@]}"
  "${HOST_CC:-cc}" "${WL_FLAGS[@]}" -o "$OUT/word_lookup_parity_host" "${WL[@]}"
  fx=host/fixtures/word_lookup/vocr_lines.tsv
  if "$QEMU" "${QEMU_ARGS[@]}" "$OUT/word_lookup_parity" "$VJO_PLAINTEXT_DIR" "$fx" > "$OUT/word_lookup.arm" 2> "$OUT/word_lookup.log" &&
     "$OUT/word_lookup_parity_host" "$VJO_PLAINTEXT_DIR" "$fx" > "$OUT/word_lookup.host" 2>> "$OUT/word_lookup.log" &&
     cmp -s "$OUT/word_lookup.arm" "$OUT/word_lookup.host"; then
    echo "arm word_lookup: $(head -1 "$OUT/word_lookup.log"), identical to the host's"
  else
    echo "arm word_lookup: FAILED"; cat "$OUT/word_lookup.log"
    diff "$OUT/word_lookup.arm" "$OUT/word_lookup.host" | head -20 || true; fail=1
  fi
fi
for t in "${TESTS[@]}"; do
  if "$QEMU" "${QEMU_ARGS[@]}" "$OUT/$t" > "$OUT/$t.log" 2>&1; then
    echo "arm $t: $(grep -c '\[ OK \]' "$OUT/$t.log") cases pass"
  else
    echo "arm $t: FAILED"; tail -20 "$OUT/$t.log"; fail=1
  fi
done
# The record the kernel writes must be the same bytes on ARM and on the host.
"$QEMU" "${QEMU_ARGS[@]}" "$OUT/boot_guard" record_format_and_parse_round_trip > /dev/null
exit $fail
