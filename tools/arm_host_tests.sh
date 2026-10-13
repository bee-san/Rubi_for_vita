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
# Vita's CPU (procedural frames; tools/capture_parity.py adds eval crops).
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
build kernel_capture host/tests/test_kernel_capture.c kernel/scene.c kernel/ledger.c core/ledger.c
build kernel_input host/tests/test_kernel_input.c kernel/triggers.c
build kernel_game_ocr host/tests/test_kernel_game_ocr.c kernel/scene.c kernel/ledger.c core/ledger.c

TESTS=(boot_guard ledger kernel_capture kernel_input kernel_game_ocr)
if [ -n "${VJO_VOCR_SOURCE_DIR:-}" ]; then
  RT=$VJO_VOCR_SOURCE_DIR/runtime
  "$ARM_CC" "${FLAGS[@]}" -c -o "$OUT/parity_kernel.o" host/tests/parity_kernel.c
  "$ARM_CC" "${FLAGS[@]}" -c -o "$OUT/parity_capture.o" kernel/capture.c
  "$ARM_CC" "${FLAGS[@]}" -c -o "$OUT/parity_scene.o" kernel/scene.c
  "$ARM_CC" -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I third_party/acutest/include -I core -I include -I "$RT" -I third_party/bearssl/inc -I third_party/bearssl/src \
    -o "$OUT/capture_parity" host/tests/test_capture_parity.c host/tests/parity_api18.c host/tests/parity_api19.c \
    "$OUT/parity_kernel.o" "$OUT/parity_capture.o" "$OUT/parity_scene.o" "$RT/vocr.c" "$RT/vocr_lines.c" \
    "$RT/vocr_lines_rows.c" third_party/bearssl/src/hash/sha2small.c third_party/bearssl/src/codec/dec32be.c \
    third_party/bearssl/src/codec/enc32be.c -lm
  TESTS+=(capture_parity)
fi

fail=0
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
