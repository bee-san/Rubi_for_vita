#!/usr/bin/env bash
# The pure-C kernel and core suites, built for the Vita's CPU (ARMv7-A
# Cortex-A9, 32-bit pointers and size_t, LDREX/STREX atomics) with a Linux
# ARM toolchain and run under qemu-arm user mode. The x86_64 host suites
# cannot see 32-bit width or ARM atomics bugs; this lane can. It is not a
# Vita: no firmware, no SceShell, no taiHEN, no memory budgets
# (docs/offline-testing.md). The suites are the host build's own, chosen by
# name (TESTS below): host/tests/arm_build.py builds each from the host
# CMakeLists.txt's configuration, its units' compile lines and its link line,
# with the ARM compiler, so the two lanes cannot list different sources.
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
# the same bytes on ARM as natively (HOST_CC).
#
# Every suite runs with a TMPDIR of its own (host/tests/test_tmpdir.h), and the lane's other steps
# (the builds, the comparisons) with theirs, all in one directory of the lane's: each must be empty
# again at the end. The lane fails on whatever is left, named with its suite, and removes the
# directory either way.
#
# Ubuntu: apt-get install gcc-arm-linux-gnueabihf libc6-dev-armhf-cross
# qemu-user-static (the cross gcc alone has no ARM libc headers or static
# libc), and CMake and a host C/C++ compiler for the configuration. The defaults below are those package names; the fleet box sets both
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

# ARM_JOBS (default: the host's processors) steps at a time: first the builds, then the suites under
# qemu-arm. Each step writes its own log, and the logs and results are printed in the order of the
# steps below, as one at a time would print them; a build that fails stops the lane after them all.
ARM_JOBS=${ARM_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}
STEPS=()
bg() { # bg NAME COMMAND...: COMMAND in the background, its output in $OUT/NAME.step.log, its status in .step.rc
  local name=$1; shift
  while [ "$(jobs -rp | wc -l)" -ge "$ARM_JOBS" ]; do wait -n 2> /dev/null || sleep 0.1; done
  rm -f "$OUT/$name.step.rc"
  # (no errexit here: COMMAND's own status is the step's; the functions below return theirs)
  ( set +e; "$@" > "$OUT/$name.step.log" 2>&1; echo $? > "$OUT/$name.step.rc" ) &
  STEPS+=("$name")
}
steps_done() { # every step bg started, its log printed in order; 1 if any failed (with \"build\": each one named)
  wait
  local name rc failed=0
  for name in "${STEPS[@]}"; do
    cat "$OUT/$name.step.log"
    rc=$(cat "$OUT/$name.step.rc" 2> /dev/null || echo 1)
    if [ "$rc" != 0 ]; then
      failed=1
      [ "${1:-}" != build ] || echo "arm ${name#build-}: build FAILED (exit $rc)"
    fi
  done
  STEPS=()
  return $failed
}
# The suites (ctest's names). Their programs are the host build's: the host CMakeLists.txt configured here (not built),
# each unit compiled with its own compile line for the A9 and each program linked as the host links it
# (host/tests/arm_build.py), so a source added to a suite there is in its ARM program too.
TESTS=(boot_guard ledger fbgrey kernel_capture kernel_input kernel_lifecycle kernel_game_ocr kernel_loader region_select
       region_suggest)
[ "$VJO_DEBUG" = 0 ] || TESTS+=(kernel_meminfo) # the self-test's memory routes: the debug build's only
TESTS+=(touch_hold text_rows text_join linebreak history_lines history_read streak hook_select hook_cache stick_cursor
        cursor_nav local_cursor settings_layout shell_log shell_history shell_divide apkg psp taicfg shot_trim)
TESTS+=(hook_text hook_luna hook_sc3 hook_stream_seams psp_probe hooks_engine hooks_engine_external)
TESTS+=(kernel_text_source hook_titles) # API 31's plumbing (hooks/api)
# #528 (audit-mem-anki-followup-1): the core suite (the JSON helpers' walk over a 32-bit token array, entries, styled
# text, the scene and the settings parser, with an unsigned char)
TESTS+=(core)
# #528: the OOM paths on the A9's 32-bit layout (every block size differs there), with the lane's dictionary
[ -z "${VJO_PLAINTEXT_DIR:-}" ] || TESTS+=(oom_paths)
CMAKE_VOCR=(-DVJO_WITH_VOCR=OFF)
if [ -n "${VJO_VOCR_SOURCE_DIR:-}" ]; then
  # API 54: the text spotter, pinned at its own commit (cmake/VocrOCR.cmake VJO_VOCR_SPOT_COMMIT): VJO_VOCR_SPOT_SOURCE_DIR,
  # else the recognizer's checkout when it holds it
  CMAKE_VOCR=(-DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" -DVJO_VOCR_SPOT_SOURCE_DIR="${VJO_VOCR_SPOT_SOURCE_DIR:-$VJO_VOCR_SOURCE_DIR}"
              -DVJO_VOCR_GATE_DOWNLOAD=OFF) # (no gate test runs here: nothing to download)
  [ -z "${VJO_VOCR_MODEL_DIR:-}" ] || CMAKE_VOCR+=(-DVJO_VOCR_MODEL_DIR="$VJO_VOCR_MODEL_DIR")
  TESTS+=(capture_parity)
  # #528: the soaks on the A9's 32-bit layout (audit-mem-leak-1 F2): a Shell session (its dictionary tests skip
  # without VJO_PLAINTEXT_DIR) and the game process without the models (its jobs end at their model; with them it
  # takes minutes under qemu, and the fast gate's models lane runs it natively: audit-mem-leak-3 F6)
  TESTS+=(shell_worker)
  [ -n "${VJO_VOCR_MODEL_DIR:-}" ] || TESTS+=(game_soak)
fi
CONFIG=$OUT/host-config
# CMAKE_LINK_DEPENDS_USE_LINKER: the link lines as CI's newer CMake writes them (3.29 on: -Wl,--dependency-file, which
# arm_build.py leaves out), here too (3.28 writes them so only when asked)
cmake -S . -B "$CONFIG" -G "Unix Makefiles" -DVJO_DEBUG="$([ "$VJO_DEBUG" = 0 ] && echo OFF || echo ON)" "${CMAKE_VOCR[@]}" \
  -DCMAKE_LINK_DEPENDS_USE_LINKER=ON \
  > "$OUT/host-config.log" 2>&1 || { cat "$OUT/host-config.log"; echo "arm: the host build's configuration FAILED"; exit 1; }
python3 tools/hook_profiles.py --output "$CONFIG/profiles.vjhp"
bg build-suites python3 host/tests/arm_build.py "$CONFIG" "$OUT" --cc "$ARM_CC" --jobs "$ARM_JOBS" "${TESTS[@]}"
steps_done build || exit 1
# then this machine's copies of what the A9's must equal: the Rubi app's .apkg export from the same lines (HOST_CC),
# and the dictionary lookup
bg build-apkg_host python3 host/tests/arm_build.py "$CONFIG" "$OUT/host" --cc "${HOST_CC:-cc}" --native --jobs "$ARM_JOBS" apkg
if [ -n "${VJO_PLAINTEXT_DIR:-}" ]; then
  WL=(host/tests/word_lookup_parity.c core/local_dict.c core/textjoin.c core/utf.c core/arena.c)
  WL_FLAGS=(-O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 -I core -I include -DVJO_HOST=1)
  bg build-word_lookup_parity "$ARM_CC" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static "${WL_FLAGS[@]}" \
    -o "$OUT/word_lookup_parity" "${WL[@]}"
  bg build-word_lookup_parity_host "${HOST_CC:-cc}" "${WL_FLAGS[@]}" -o "$OUT/word_lookup_parity_host" "${WL[@]}"
fi
# Every build, its output in order; any that failed stops the lane here.
steps_done build || exit 1

# The runs, ARM_JOBS at a time, each printing its line (and a failure's log) as before.
apkg_write() {
  rm -rf "$OUT/apkg.arm" "$OUT/apkg.host" && mkdir -p "$OUT/apkg.arm" "$OUT/apkg.host"
  if TMPDIR=$(tmp_of apkg_write) "$QEMU" "${QEMU_ARGS[@]}" "$OUT/apkg" --write "$OUT/apkg.arm" > "$OUT/apkg_write.log" 2>&1 &&
     TMPDIR=$(tmp_of apkg_write) "$OUT/host/apkg" --write "$OUT/apkg.host" >> "$OUT/apkg_write.log" 2>&1 &&
     diff -r "$OUT/apkg.arm" "$OUT/apkg.host" > /dev/null; then
    echo "arm apkg packages: $(ls "$OUT/apkg.arm" | grep -c '\.apkg$') .apkg files, identical to the host's"
  else
    echo "arm apkg packages: FAILED"; cat "$OUT/apkg_write.log"; return 1
  fi
}
word_lookup() {
  local fx=host/fixtures/word_lookup/vocr_lines.tsv
  if TMPDIR=$(tmp_of word_lookup) "$QEMU" "${QEMU_ARGS[@]}" "$OUT/word_lookup_parity" "$VJO_PLAINTEXT_DIR" "$fx" \
       > "$OUT/word_lookup.arm" 2> "$OUT/word_lookup.log" &&
     TMPDIR=$(tmp_of word_lookup) "$OUT/word_lookup_parity_host" "$VJO_PLAINTEXT_DIR" "$fx" > "$OUT/word_lookup.host" \
       2>> "$OUT/word_lookup.log" &&
     cmp -s "$OUT/word_lookup.arm" "$OUT/word_lookup.host"; then
    echo "arm word_lookup: $(head -1 "$OUT/word_lookup.log"), identical to the host's"
  else
    echo "arm word_lookup: FAILED"; cat "$OUT/word_lookup.log"
    diff "$OUT/word_lookup.arm" "$OUT/word_lookup.host" | head -20 || true; return 1
  fi
}
suite() { # suite NAME: one suite under qemu-arm
  if TMPDIR=$(tmp_of "$1") "$QEMU" "${QEMU_ARGS[@]}" "$OUT/$1" > "$OUT/$1.log" 2>&1; then
    echo "arm $1: $(grep -c '\[ OK \]' "$OUT/$1.log") cases pass"
  else
    echo "arm $1: FAILED"; tail -20 "$OUT/$1.log"; return 1
  fi
}
fail=0
bg run-apkg_write apkg_write
[ -z "${VJO_PLAINTEXT_DIR:-}" ] || bg run-word_lookup word_lookup
for t in "${TESTS[@]}"; do
  bg "run-$t" suite "$t"
done
steps_done || fail=1
rm -f "$OUT"/*.step.log "$OUT"/*.step.rc
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
