#!/usr/bin/env bash
# Host preflight for the next console run (#74): every check that can run off the console,
# then GO or NO-GO, with the assumptions the console still has to confirm.
#
#   VJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr VJO_VOCR_MODEL_DIR=/path/to/models tools/preflight.sh
#
# Required: VJO_VOCR_SOURCE_DIR (the pinned runtime). Each optional input adds checks:
#   VJO_VOCR_MODEL_DIR   the release weights: the simulator's and the parity test's real runs
#   VJO_PLAINTEXT_SOURCE plaintext-dicts checkout (or .refs/plaintext-dicts): the word lookup on real
#                        recognized lines with the bundled dictionary (host/tests/test_word_lookup.cpp)
#   VJO_EVAL_MANIFESTS   vita-vn-ocr eval/linefind/manifests, with VJO_EVAL_SHOTS (dirs, colon-separated):
#                        the capture parity test on the dev eval crops
#   VITASDK              the Vita build: module pages, memory ledger, syscall ABI, kernel FPU check
#   ARM_CC, QEMU         the ARMv7 lane under qemu-arm
#   VITA3K               the Vita3K smoke (tools/vita3k_smoke.sh); with VITA3K_FIRMWARE and
#                        VITA3K_FONTS (Sony's PUPs) also the Rubi app's (tools/vita3k_settings_smoke.sh)
#   BUILD                host build directory (default build/preflight)
#   VJO_DEBUG            1 (the default): the debug build, the test builds'; 0: the normal build
#                        (docs/release-variants.md). Every build here is made as that build, and a
#                        check of a debug-only feature (the self-test's) says N/A for the normal one.
# A check whose input is missing prints SKIP; GO needs every check that ran to pass and the
# required ones (host suites, simulator, parity, Vita build) to have run.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
BUILD=${BUILD:-build/preflight}
PY=${PYTHON:-python3}
log=$BUILD/preflight-logs
mkdir -p "$log"
fails=() ; skipped=() ; required_missing=()
pass() { printf '  [PASS] %-28s %s\n' "$1" "$2"; }
fail() { printf '  [FAIL] %-28s %s\n' "$1" "$2"; fails+=("$1"); }
skip() { printf '  [SKIP] %-28s %s\n' "$1" "$2"; skipped+=("$1"); [ "${3:-}" = required ] && required_missing+=("$1"); }
# not applicable: a debug-only feature's check, for the normal build (counts as neither run nor skipped)
na() { printf '  [N/A ] %-28s %s\n' "$1" "$2"; }
# The build variant (docs/release-variants.md): every configure below passes it.
if [ "${VJO_DEBUG:-1}" = 0 ]; then VARIANT=normal DBG=OFF; else VARIANT=debug DBG=ON; fi
export VJO_DEBUG=$([ "$VARIANT" = normal ] && echo 0 || echo 1)

echo "preflight $(git rev-parse --short HEAD 2>/dev/null)$(git diff --quiet 2>/dev/null || echo '+local changes') at $(date -u +%FT%TZ), the $VARIANT build (VJO_DEBUG=$DBG)"
[ -f "${VJO_VOCR_SOURCE_DIR:-}/runtime/vocr_lines_rows.c" ] || {
  echo "  VJO_VOCR_SOURCE_DIR must name the pinned vita-vn-ocr checkout (runtime/vocr_lines_rows.c)"; echo "PREFLIGHT: NO-GO"; exit 1; }

# 1. host build and every host suite (the real kernel, Shell and game worker with SDK stubs)
if cmake -S . -B "$BUILD" -G Ninja -DVJO_DEBUG="$DBG" -DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" \
     ${VJO_VOCR_MODEL_DIR:+-DVJO_VOCR_MODEL_DIR="$VJO_VOCR_MODEL_DIR"} -DPython3_EXECUTABLE="$(command -v "$PY")" \
     ${VJO_PLAINTEXT_SOURCE:+-DVJO_PLAINTEXT_SOURCE="$VJO_PLAINTEXT_SOURCE"} \
     > "$log/configure.log" 2>&1 && cmake --build "$BUILD" -j"${JOBS:-8}" > "$log/build.log" 2>&1; then
  if ctest --test-dir "$BUILD" -j"${JOBS:-8}" --output-on-failure > "$log/ctest.log" 2>&1; then
    pass "host suites" "$(grep -o '[0-9]*% tests passed, [0-9]* tests failed out of [0-9]*' "$log/ctest.log" | tail -1) (kernel, Shell, worker, core, tools)"
  else
    fail "host suites" "$(grep -E 'tests passed|\*\*\*' "$log/ctest.log" | tr '\n' ' ' | cut -c1-160) ($log/ctest.log)"
  fi
else
  fail "host build" "$log/build.log"
fi
# kernel/meminfo.c itself, built on the host with the firmware stubbed (#75's audit, B1: its first guard
# read only user-half pointers, so on a console every route failed before reading anything)
km=$BUILD/vjo-kernel-meminfo-tests
if [ "$VARIANT" = normal ]; then
  na "kernel memory routes (B1)" "the self-test's memory sample (kernel/meminfo.c) is not in the normal build"
elif [ -x "$km" ] && "$km" > "$log/kernel-meminfo.log" 2>&1; then
  pass "kernel memory routes (B1)" "$(grep -c '\[ OK \]' "$log/kernel-meminfo.log") cases: objects below 0x40000000 read by every route; user-half ones (0x5..., 0x8..., unreadable) refused unread, step 3; the guard at its edges"
else
  fail "kernel memory routes (B1)" "${km} ($log/kernel-meminfo.log)"
fi

# word lookup end to end: real VOCR lines -> the bundled dictionary -> highlight span -> entry
wl=$BUILD/vjo-word-lookup-tests
if [ -f "$BUILD/plaintext/plaintext.vjdict" ]; then
  if VJO_PLAINTEXT_DIR=$BUILD/plaintext VJO_REQUIRE_PLAINTEXT=1 "$wl" > "$log/word-lookup.log" 2>&1; then
    pass "word lookup (bundled dict)" "$(grep -o '[0-9]* real VOCR lines, [0-9]* lookups' "$log/word-lookup.log"), $(grep -o '[0-9.]*%)' "$log/word-lookup.log" | tr -d ')') of letters in a word, $(grep -o 'host lookup [0-9.]* us mean' "$log/word-lookup.log")"
  else
    fail "word lookup (bundled dict)" "$log/word-lookup.log"
  fi
else
  skip "word lookup (bundled dict)" "VJO_PLAINTEXT_SOURCE unset (no plaintext.vjdict)"
fi

# 2. the self-test analyser and its tests, with the real writer, so the fixtures are checked to be what
# shell/selftest.c writes (the debug build's: the normal build has no self-test, and its analyser tests run on
# the fixtures alone)
writer=; [ "$VARIANT" = debug ] && writer=$BUILD/vjo-selftest-writer
if VJO_SELFTEST_WRITER=$writer "$PY" -m unittest tools.tests.test_analyse_selftest \
     > "$log/analyser.log" 2>&1; then
  pass "self-test analyser" "$(grep -o 'Ran [0-9]* tests' "$log/analyser.log")$(grep -o ' (skipped=[0-9]*)' "$log/analyser.log"): v2 PASS/FAIL/INCOMPLETE, diagnostics PASS/WARN${writer:+, the real writer}"
else
  fail "self-test analyser" "$log/analyser.log"
fi
# a working OCR whose diagnostics warn (routes refused at step 3 as B1's guard refused them, the kernel
# strip, a CDRAM capture block): the verdict is the OCR's PASS, exit 0; a diagnostic can never say FAIL
rd=$("$PY" tools/analyse_selftest.py tools/tests/fixtures/selftest-v2-diagnostics.txt 2>&1); rc=$?
if [ $rc -eq 0 ] && grep -q '^  kernel-memory  *WARN .*at step 3 (pointer outside the kernel range)' <<<"$rd" \
   && ! grep -qE '^  [^ ]+ +FAIL ' <<<"$rd" && tail -1 <<<"$rd" | grep -q '^PASS: every stage completed'; then
  pass "diagnostics never decide" "$(grep -o '^diagnostics: WARN ([^)]*)' <<<"$rd"), verdict PASS, exit 0 (B1's routes replayed)"
else
  fail "diagnostics never decide" "a diagnostic changed the verdict (exit $rc)"
fi

# 2b. the offline Anki queue as shipped (anki_mining = on in the test build): a card's screenshot is the
# game worker's (API 23), so × never asks the kernel for its 2,088,960 B capture (#74's refusal); the
# job's arena and stack are the ledger's, and the Shell budget model replays them against the console's
# measured SceShell
ab=$BUILD/vjo-shell-anki-tests
ae=$BUILD/vjo-apkg-tests
# VJO_BUDGET_SHELL_ANKI_STACK: the Anki job's thread stack and its TLS, from the budget header
anki_stack=$(( $(grep -E '^#define VJO_STACK_SHELL_ANKI ' include/vjo_ledger_budget.h | grep -oE '[0-9]+u' | tr -d u)
               + $(grep -E '^#define VJO_LEDGER_TLS ' include/vjo_ledger_budget.h | grep -oE '[0-9]+u' | tr -d u) ))
aq=$BUILD/vjo-app-queue-tests
if [ -x "$ab" ] && [ -x "$ae" ] && [ -x "$aq" ] && "$ab" > "$log/anki-build.log" 2>&1 \
   && "$PY" host/tests/test_anki_restart.py "$ab" "$ae" "$aq" > "$log/anki-restart.log" 2>&1; then
  am=$("$PY" tools/shell_budget_model.py --anki --json 2>/dev/null | "$PY" -c '
import json, sys
r = json.load(sys.stdin)
v = next(s for s in r["scenarios"] if s["id"] == "notes26-v074")
anki = [s for s in v["steps"] if s["step"].startswith("Anki")]
print(min(s["headroom"] for s in anki), r["peak_pages"], r["peak_paf"])' 2>/dev/null)
  read -r am_head am_pages am_paf <<<"$am"
  arena=$(grep -E '^#define VJO_BUDGET_PAF_ANKI ' include/vjo_ledger_budget.h | grep -oE '[0-9]+u' | tr -d u)
  # the Shell's own arena (shell/anki.c ANKI_TEXT_MEM, KiB), which the ledger's item must equal (#528: 88 KiB)
  arena_kib=$(sed -nE 's/^#define ANKI_TEXT_MEM \(([0-9]+) \* 1024\).*/\1/p' shell/anki.c)
  if [ -n "$am_head" ] && [ "$am_head" -gt 0 ] && [ -n "$arena_kib" ] && [ "$arena" = "$((arena_kib * 1024))" ] \
     && "$PY" tools/check_memory_ledger.py > "$log/ledger-src.log" 2>&1 && ! grep -q WARNING "$log/ledger-src.log"; then
    pass "Anki offline queue (Shell)" "screenshots in the game worker, no network, 0 kernel captures; arena ${arena} B (VJO_BUDGET_PAF_ANKI) + ${anki_stack} B stack and TLS (VJO_BUDGET_SHELL_ANKI_STACK); SceShell headroom at the Anki step ${am_head} B on the measured CLANNAD sample (model, not a measurement); saves (with screenshots too) and exports stopped at every storage boundary $(grep -o 'Ran [0-9]* tests' "$log/anki-restart.log")"
  else
    fail "Anki offline queue (Shell)" "budget/model check (arena ${arena:-?}, headroom ${am_head:-?}; $log/ledger-src.log)"
  fi
else
  fail "Anki offline queue (Shell)" "$log/anki-build.log $log/anki-restart.log"
fi
# 2c. the Rubi app's .apkg export (settings/anki_export.c, core/apkg.c): Lapis notes in a package
# written, flushed, renamed, read back and verified before the queue is cleared; the packages opened
# with Python's zipfile and sqlite3, and imported by Anki itself where the anki package is
# installed (VJO_ANKI_PYTHON, tools/requirements-anki.txt)
if [ -x "$ae" ] && "$ae" > "$log/apkg.log" 2>&1 && "$PY" host/tests/test_apkg.py "$ae" > "$log/apkg-py.log" 2>&1; then
  if grep -q "skipped 'the anki package" "$log/apkg-py.log"; then imported="not imported (no anki package)"; else
    imported="imported by $(grep -o 'anki [0-9][0-9.]*' "$log/apkg-py.log" | head -1)"; fi
  pass "Anki export (Rubi app)" "$(grep -c '\[ OK \]' "$log/apkg.log") cases (no clear on any failed write); packages: $(grep -o 'Ran [0-9]* tests' "$log/apkg-py.log"), $imported"
else
  fail "Anki export (Rubi app)" "$ae ($log/apkg.log $log/apkg-py.log)"
fi

# 2d. the one-VPK install (docs/one-vpk-install.md): the config.txt editor against taiHEN's own parser on
# the fixtures and a corpus, the PC installer's copy of it compared with the C build, the Rubi app's
# install engine and page on a temporary memory card (faults at every write and rename, the interrupted
# swap, h-encore's rewrite), and the VPK's packaging rules (VitaShell's install scan, the app's own facts)
ti=$BUILD/vjo-settings-install-tests
tc=$BUILD/vjo-taicfg-tests
if [ -x "$ti" ] && [ -x "$tc" ] && "$tc" > "$log/taicfg.log" 2>&1 && "$ti" > "$log/install.log" 2>&1 \
   && VJO_TAICFG=$BUILD/vjo-taicfg "$PY" -m unittest tools.tests.test_tai_config tools.tests.test_one_vpk > "$log/one-vpk-py.log" 2>&1 \
   && ! grep -q "skipped" "$log/one-vpk-py.log"; then
  pass "one-VPK install" "editor $(grep -c '\[ OK \]' "$log/taicfg.log") groups ($(ls tools/tests/fixtures/taicfg/*.in | wc -l) fixtures, 12,000 corpus edits, taiHEN's parser as the check), app engine $(grep -c '\[ OK \]' "$log/install.log") cases on a temporary card, PC copy + VPK packaging $(grep -o 'Ran [0-9]* tests' "$log/one-vpk-py.log")"
else
  fail "one-VPK install" "$log/taicfg.log $log/install.log $log/one-vpk-py.log"
fi

# 2c. the one-command collect (install_ftp.sh --collect: the real script, curl and ftplib against a
# VitaShell-like FTP server) and logging on by default in every template the installer uses
if "$PY" -W ignore -m unittest tools.tests.test_collect > "$log/collect.log" 2>&1 \
   && grep -qx 'log_file = on' docs/config.example.ini && grep -q -- '--set log_file=on' tools/install-vita.sh; then
  pass "collect + logging on" "install_ftp.sh --collect vs the FTP mock: $(grep -o 'Ran [0-9]* tests' "$log/collect.log"): read-only, API keys redacted, one folder + zip, analysed; log_file = on by default"
else
  fail "collect + logging on" "$log/collect.log"
fi

# 3. the allocation simulator: the real worker and kernel under #74's measured pools
sim=$BUILD/vjo-alloc-sim-tests
if [ -x "$sim" ]; then
  "$sim" > "$log/alloc-sim.log" 2>&1; rc=$?
  if [ $rc -eq 0 ] && grep -q "API 19 full screen 960x544, Normal .*rc=0 .*KD       0 B" "$log/alloc-sim.log" \
     && grep -q "API 19 full screen 960x544, Large .*rc=0 .*KD       0 B" "$log/alloc-sim.log"; then
    pass "allocation simulator" "API 18 full screen refused (0x80024302) as on the console; API 19 0 B of KD at every region and model, every ladder rung works"
    grep -o "alloc-sim: .*" "$log/alloc-sim.log" | sed 's/^alloc-sim: /           /'
  elif [ $rc -eq 0 ]; then
    skip "allocation simulator" "release weights absent (VJO_VOCR_MODEL_DIR): only the API 18 refusal ran" required
  else
    fail "allocation simulator" "$log/alloc-sim.log"
  fi
else
  fail "allocation simulator" "not built"
fi

# 4. capture parity: API 18's copy + grey plane against API 19's strips, byte for byte
par=$BUILD/vjo-capture-parity-tests
if [ -x "$par" ]; then
  if "$par" procedural_frames_give_identical_recognizer_input > "$log/parity.log" 2>&1; then
    pass "capture parity (procedural)" "$(grep -o 'parity procedural: .*' "$log/parity.log")"
  else
    fail "capture parity (procedural)" "$log/parity.log"
  fi
  if [ -n "${VJO_EVAL_MANIFESTS:-}" ] && [ -n "${VJO_EVAL_SHOTS:-}" ]; then
    shots=(); IFS=: read -ra dirs <<<"$VJO_EVAL_SHOTS"; for d in "${dirs[@]}"; do shots+=(--shots "$d"); done
    if "$PY" tools/capture_parity.py --binary "$par" --manifests "$VJO_EVAL_MANIFESTS" "${shots[@]}" > "$log/parity-eval.log" 2>&1; then
      pass "capture parity (eval crops)" "$(head -1 "$log/parity-eval.log" | sed 's/^capture parity: //')"
      grep -o 'parity eval .*' "$log/parity-eval.log" | sed 's/^/           /'
    else
      fail "capture parity (eval crops)" "$log/parity-eval.log"
    fi
  else
    skip "capture parity (eval crops)" "VJO_EVAL_MANIFESTS/VJO_EVAL_SHOTS unset" required
  fi
else
  fail "capture parity" "not built"
fi

# 5. the Vita build: module pages, the memory ledger, the syscall ABI, the kernel's FPU check
if [ -n "${VITASDK:-}" ] && [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
  vb=$BUILD-vita
  if cmake -S vita -B "$vb" -DCMAKE_BUILD_TYPE=Release -DVJO_DEBUG="$DBG" -DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" \
       -DPython3_EXECUTABLE="$(command -v "$PY")" > "$log/vita-configure.log" 2>&1 \
     && cmake --build "$vb" -j"${JOBS:-8}" > "$log/vita-build.log" 2>&1 \
     && "$PY" tools/check_kernel_instructions.py --objdump "$VITASDK/bin/arm-vita-eabi-objdump" \
          "$vb/VitaJPOverlay_Kernel" >> "$log/vita-build.log" 2>&1; then
    # (the kernel's FPU/NEON check again: the build runs it only when it relinks the kernel, so a
    # second preflight in the same build directory would not see its line)
    pages=$(grep -E "VitaJPOverlay_(Kernel|Shell|OCR) +[0-9,]+ B of" "$log/vita-build.log" | sort -u | sed -E 's/ +/ /g; s/^ //' | tr '\n' ';')
    if grep -q "memory ledger: pass" "$log/vita-build.log" && grep -q '"result": "pass"' "$log/vita-build.log" \
       && grep -q "no FPU/NEON instructions" "$log/vita-build.log" \
       && grep -q "\"variant\": \"$VARIANT\"" "$log/vita-build.log" \
       && grep -q "The one-VPK install's modules: vjoworker=" "$log/vita-build.log"; then
      pass "Vita build" "ledger pass, syscall ABI pass, kernel without FPU/NEON, the $VARIANT build's modules (tools/check_variant.py), the one-VPK module check (tools/package_vpk.py)"
      echo "$pages" | tr ';' '\n' | sed '/^$/d; s/^/           /'
    else
      fail "Vita build" "ledger/ABI/FPU check did not pass ($log/vita-build.log)"
    fi
  else
    fail "Vita build" "$log/vita-build.log"
  fi
else
  skip "Vita build" "VITASDK unset" required
fi

# 6. the ARMv7 lane (32-bit, LDREX/STREX) and the Vita3K smoke
if [ -n "${ARM_CC:-}" ] && [ -n "${QEMU:-}" ]; then
  wl_dir=; [ -f "$BUILD/plaintext/plaintext.vjdict" ] && wl_dir=$root/$BUILD/plaintext
  if ARM_CC=$ARM_CC QEMU=$QEMU VJO_VOCR_SOURCE_DIR=$VJO_VOCR_SOURCE_DIR VJO_VOCR_MODEL_DIR=${VJO_VOCR_MODEL_DIR:-} \
       VJO_PLAINTEXT_DIR=$wl_dir bash tools/arm_host_tests.sh "$BUILD-arm" > "$log/arm.log" 2>&1; then
    pass "ARM lane (qemu-arm)" "$(grep '^arm ' "$log/arm.log" | sed 's/^arm //; s/ cases pass//' | tr '\n' ' ')"
    # the capture parity on the eval crops, on the Vita's CPU
    if [ -n "${VJO_EVAL_MANIFESTS:-}" ] && [ -n "${VJO_EVAL_SHOTS:-}" ] && [ -x "$BUILD-arm/capture_parity" ]; then
      printf '#!/bin/sh\nexec "%s" -cpu cortex-a9 "%s" "$@"\n' "$QEMU" "$root/$BUILD-arm/capture_parity" > "$log/arm-parity.sh"
      chmod +x "$log/arm-parity.sh"
      shots=(); IFS=: read -ra dirs <<<"$VJO_EVAL_SHOTS"; for d in "${dirs[@]}"; do shots+=(--shots "$d"); done
      if "$PY" tools/capture_parity.py --binary "$log/arm-parity.sh" --manifests "$VJO_EVAL_MANIFESTS" "${shots[@]}" \
           > "$log/arm-parity-eval.log" 2>&1; then
        pass "ARM capture parity (eval)" "$(head -1 "$log/arm-parity-eval.log" | sed 's/^capture parity: //')"
        grep -o 'parity eval .*' "$log/arm-parity-eval.log" | sed 's/^/           /'
      else
        fail "ARM capture parity (eval)" "$log/arm-parity-eval.log"
      fi
    fi
  else
    fail "ARM lane (qemu-arm)" "$log/arm.log"
  fi
else
  skip "ARM lane (qemu-arm)" "ARM_CC/QEMU unset"
fi
if [ -n "${VITA3K:-}" ] || [ -n "${VITASDK:-}" ]; then
  # with VITASDK alone the smoke app is built, and the run is skipped (tools/vita3k_smoke.sh); the text spotter's
  # runtime: VJO_VOCR_SPOT_SOURCE_DIR, else the one the Vita build above found or fetched (cmake/VocrOCR.cmake)
  spot=${VJO_VOCR_SPOT_SOURCE_DIR:-}
  [ -n "$spot" ] || [ ! -f "$BUILD-vita/vita-vn-ocr-spot/runtime/vocr_textspot6.c" ] || spot=$root/$BUILD-vita/vita-vn-ocr-spot
  v3=$(VJO_VOCR_SOURCE_DIR=$VJO_VOCR_SOURCE_DIR VJO_VOCR_SPOT_SOURCE_DIR=$spot bash tools/vita3k_smoke.sh "$BUILD-vita3k" \
       2>&1 | tail -1)
  case $v3 in
    *": PASS"*) pass "Vita3K smoke (not evidence)" "${v3#vita3k smoke: PASS }" ;;
    *": SKIPPED"*) skip "Vita3K smoke (not evidence)" "${v3#vita3k smoke: SKIPPED }" ;;
    *) fail "Vita3K smoke (not evidence)" "${v3#vita3k smoke: }" ;;
  esac
else
  skip "Vita3K smoke (not evidence)" "VITA3K and VITASDK unset"
fi
if [ -n "${VITA3K:-}" ] && [ -n "${VITA3K_FIRMWARE:-}" ] && [ -n "${VITA3K_FONTS:-}" ]; then
  vpk=; [ -f "$BUILD-vita/Rubi.vpk" ] && vpk=$root/$BUILD-vita/Rubi.vpk
  v3=$(VJO_VOCR_SOURCE_DIR=$VJO_VOCR_SOURCE_DIR RELEASE_VPK=$vpk bash tools/vita3k_settings_smoke.sh \
       "$BUILD-vita3k-settings" 2>&1 | tail -1)
  case $v3 in
    *": PASS"*) pass "Vita3K app (not evidence)" "${v3#vita3k settings smoke: PASS }" ;;
    *": SKIPPED"*) skip "Vita3K app (not evidence)" "${v3#vita3k settings smoke: SKIPPED }" ;;
    *) fail "Vita3K app (not evidence)" "${v3#vita3k settings smoke: }" ;;
  esac
else
  skip "Vita3K app (not evidence)" "VITA3K, VITA3K_FIRMWARE or VITA3K_FONTS unset"
fi

cat <<'EOF'

assumptions the console still has to confirm (none of these is measured yet):
  - ScePhyMemPartKD's free size on 3.65 is unknown: both exact queries returned 0x80020001 in #74.
    API 19's OCR asks it for 0 B; only the ladder's last rung (the hook cannot write the game's
    memory) takes a kernel strip of at most 64 KiB, for one call.
  - The display hook writes the region's grey rows into the game's own memory with
    ksceKernelMemcpyKernelToUser in the game's thread (as PSVshell/reVita copy in this hook).
    That this works for every game, and how long a strip takes there, is unmeasured: the
    self-test's probe and capture_ms lines answer it.
  - Pixel formats: A8B8G8R8 (0x0), A2B10G10R10 (0x60800000) and A1B5G5R5 (0x50000000) are read;
    any other is refused (format error) and logged with its value. SceDisplay documents no
    RGB565 value, so none is handled.
  - The game's free USER memory: 91,226,112 B was measured in CLANNAD in an earlier session
    (docs/vita-memory.md); a job is refused below its need plus the fixed 1 MiB reserve, and the
    memory ladder (USER -> CDRAM -> PHYCONT) logs each refusal.
  - The three memory-query routes (block, PSVshell's address-space block, faps-coredump's
    process route) are untested on 3.65; each reports its own result and step. They are
    diagnostics: one that reads nothing is a WARN and never changes the OCR verdict. Whether
    ksceKernelFindMemBlockByAddr finds a block for a kernel object above 0x1000000 is
    unverified (step 4 says when it does not).
  - The rows call checks its destination with ksceKernelFindProcMemBlockByAddr(pid, addr, 0),
    size 0 meaning "the block that contains addr" (VitaSDK psp2kern/kernel/sysmem.h: "usually
    set to 0"; henkaku payload.c:775 passes 0 to the kernel form). No console run has used that
    form with size 0 yet: if it refuses the game's blocks, every rows call returns 0xBFFFFFFA
    (argument), each OCR stage FAILs with that step, and the probe shows it per memory type.
  - taiHEN binding of the new export vjoOcrCaptureRows: the ABI check covers packaged metadata
    and relocations only.
EOF
echo "  - Anki (anki_mining = on in the test build): every Anki job holds a ${anki_stack} B thread stack in"
echo "    SceShell's partition while it runs (a count of the queue on every overlay open, a save),"
echo "    and a save a $(grep -E '^#define VJO_BUDGET_PAF_ANKI ' include/vjo_ledger_budget.h | grep -oE '[0-9]+u' | tr -d u) B PAF arena (VJO_BUDGET_PAF_ANKI); a refused thread is logged with its rc. SceShell's free pages"
cat <<'EOF'
    beside a game were measured once at 98,304 B with an older, larger build; the model's
    headroom at that step is an estimate, and a refused thread or arena is survived (host
    tests), the card is then not saved and the overlay says so.
  - An Anki card's screenshot (API 23, anki_screenshot = on by default) runs in the game: a
    450,560 B block beside the game's 1 MiB reserve, 9 frames of colour strips through the
    display hook (or the kernel strip; with anki_screenshot_trim_bars, API 32, up to 19 when a
    frame is read again: docs/anki.md "Black bars"), the JPEG encoded at the lowest priority (60.0 M A9
    instructions, 69-149 ms at 444 MHz, an estimate), then the kernel worker's file writes and
    two memory card syncs. None of it has run on a console: the hook's colour copy into the
    game's block, the encode's time beside a running game, the syncs' time and whether
    ux0:data/VitaJPOverlay/anki_queue accepts the kernel's writes are unmeasured; a failure saves
    the card without one and log.txt's "anki: screenshot rc=" line and its gstep lines say why.
  - taiHEN binding of the new exports vjoOcrShotWrite and vjoOcrShotSave: packaged metadata only.
  - The Rubi app's .apkg export (its 2 MiB arena, sceIoPwrite/sceIoPread on ux0:, the
    memory card's speed for a large queue) has run on the host only; its packages were imported
    by Anki 26.09.3 on a computer (tools/requirements-anki.txt), not yet by AnkiDroid or
    AnkiMobile, and no console has written one.
  - CPU3 load and the hook's cost per frame: unmeasured (AGENTS.md rule 8).
  - Vita3K is not evidence: it cannot load the kernel plugin and has no KD, SceShell or taiHEN.
EOF
echo
if [ ${#fails[@]} -eq 0 ] && [ ${#required_missing[@]} -eq 0 ]; then
  echo "PREFLIGHT: GO (${#skipped[@]} optional check(s) skipped${skipped:+: ${skipped[*]}})"
  exit 0
fi
echo "PREFLIGHT: NO-GO (failed: ${fails[*]:-none}; required but not run: ${required_missing[*]:-none})"
exit 1
