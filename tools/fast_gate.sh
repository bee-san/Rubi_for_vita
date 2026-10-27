#!/usr/bin/env bash
# The local merge gate (AGENTS.md, "Merge gate"): CI's checks of a change, side by side on this machine, in a
# few minutes. The working tree is what is checked, uncommitted changes too.
#
#   bash tools/fast_gate.sh [--base REV] [--e2e auto|on|off] [--models auto|on|off] [--no-arm] [--jobs N]
#                           [--out DIR] [--clean]
#
# Lanes, all at once (logs and each lane's script in OUT/logs):
#   host-debug, host-normal  the host build and ctest -j (CI's `host` job): every suite, with the pinned
#                            OCR runtime, the frozen dictionary and the Anki import (VJO_REQUIRE_ANKI); the
#                            normal build leaves the suites labelled variant_free to the debug build's run
#   models                   what CI never has: the debug host build again, with the pinned release models
#                            (VJO_VOCR_MODEL_DIR, below), and every suite that reads the build, so their
#                            real-model checks run (ctest -LE variant_free: the others read no build, no model)
#   arm-debug, arm-normal    tools/arm_host_tests.sh, the suites on the Vita's CPU under qemu-arm
#   vita-debug, vita-normal  the Vita build (CI's `vita` job) and every check it runs: kernel instructions,
#                            syscall ABI, memory ledger, offline imports, module variant, the Rubi app's
#                            SELF, PNG ABI and artwork; with e2e, also the release target (the zip)
#   e2e                      when packaging changed (tools/ci_scope.py against --base) or --e2e on: the two
#                            zips through tools/package_variants.py with the pinned models, then
#                            tools/e2e_package.py on each (CI's `package` job)
#
#   --base REV     what the change is measured against (default: the merge base of HEAD and origin/main)
#   --e2e WHEN     auto (the default), on or off
#   --models WHEN  auto (the default): the models lane runs when its directory holds every pinned model the
#                  host tests read at its size and SHA-256 (tools/prepare_ocr.py --backend vocr-tests --check),
#                  else its line says SKIP and what is missing; on: a missing one fails the gate, and the cache's
#                  directory (never VJO_VOCR_MODEL_DIR's) is filled first (the public release downloads, each
#                  checked); off
#   --no-arm       leave out the ARM lanes (CI still runs them)
#   --jobs N       parallel jobs per lane (default: the processors here)
#   --out DIR      the gate's build directories (default build/gate; reused, so a second run is incremental)
#   --clean        remove OUT first
#
# Containers (CONTAINER_ENGINE=docker or podman; the default is whichever runs): tools/gate/host.Dockerfile
# (Ubuntu 24.04 as CI's runner, its packages, the ARM toolchain, qemu-arm and the pinned anki) and
# tools/gate/vita.Dockerfile (CI's vitasdk image by digest, tools/setup_toolchain.sh done). Each is built
# once, tagged with the hash of its inputs. CONTAINER_ENGINE=none runs the lanes here instead: then cmake,
# ninja, python3 with Pillow, VITASDK (tools/setup_toolchain.sh), ARM_CC/QEMU and VJO_ANKI_PYTHON are this
# machine's (a lane whose tools are missing fails).
#
# Inputs: VJO_VOCR_SOURCE_DIR (else .refs/vita-vn-ocr at cmake/VocrOCR.cmake's pin, else a clone in the
# cache) and VJO_PLAINTEXT_SOURCE (else .refs/plaintext-dicts at docs/plaintext-dictionaries.json's revision,
# else a sparse clone in the cache over SSH). RUBI_GATE_CACHE (default ~/.cache/rubi-fast-gate) keeps those
# clones and the pinned models. Without the private dictionary the gate runs what CI runs for a fork's pull
# request and says so; the e2e then cannot run. The models lane reads VJO_VOCR_MODEL_DIR, else the cache's
# models directory (the e2e lane's, which keeps the package's models there; `python3 tools/prepare_ocr.py
# --backend vocr-tests --output DIR` adds the rest once, as --models on does). Its container has that directory
# only, and checks it again: a symlink there to a file outside fails the lane, by name.
#
# Prints one line per lane and FAST GATE: PASS or FAIL (also in OUT/result.txt); exit 0 only on PASS,
# 2 when the gate itself cannot start.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
BASE='' E2E=auto MODELS=auto ARM=1 JOBS='' OUT=build/gate CLEAN=0
usage() { sed -n '2,52p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
while [ $# -gt 0 ]; do
  case $1 in
    --base) [ $# -ge 2 ] || usage; BASE=$2; shift 2 ;;
    --e2e) [ $# -ge 2 ] && case $2 in auto|on|off) true ;; *) false ;; esac || usage; E2E=$2; shift 2 ;;
    --models) [ $# -ge 2 ] && case $2 in auto|on|off) true ;; *) false ;; esac || usage; MODELS=$2; shift 2 ;;
    --no-arm) ARM=0; shift ;;
    --jobs) [ $# -ge 2 ] && [ "$2" -gt 0 ] 2> /dev/null || usage; JOBS=$2; shift 2 ;;
    --out) [ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    *) usage ;;
  esac
done
die() { echo "fast_gate: $*" >&2; exit 2; }
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)}
case $OUT in /*) ;; *) OUT=$ROOT/$OUT ;; esac
[ "$CLEAN" = 0 ] || rm -rf "$OUT"
mkdir -p "$OUT/logs" || die "cannot make $OUT"
LOGS=$OUT/logs
CACHE=${RUBI_GATE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/rubi-fast-gate}
mkdir -p "$CACHE" || die "cannot make the cache $CACHE"
START=$(date +%s)

# ---- what changed, and so what runs
git rev-parse --verify -q HEAD > /dev/null || die "not a git checkout: $ROOT"
if [ -z "$BASE" ]; then
  BASE=$(git merge-base HEAD origin/main 2> /dev/null) || BASE=HEAD
fi
git rev-parse --verify -q "$BASE^{commit}" > /dev/null || die "no such revision: $BASE"
CHANGED=$( { git diff --name-only "$BASE" --; git ls-files --others --exclude-standard; } | sort -u)
SCOPE=$(printf '%s\n' "$CHANGED" | python3 tools/ci_scope.py) || die "tools/ci_scope.py failed"
PACKAGING=$(printf '%s\n' "$SCOPE" | sed -n 's/^packaging=//p')
case $E2E in auto) [ "$PACKAGING" = true ] && E2E=on || E2E=off ;; esac
REV=$(git rev-parse HEAD)
DIRTY=$(git status --porcelain --untracked-files=normal | head -1)

# ---- the pinned private inputs
VOCR_PIN=$(sed -n 's/^set(VJO_VOCR_COMMIT \([0-9a-f]\{40\}\))$/\1/p' cmake/VocrOCR.cmake)
DICTS_PIN=$(sed -n 's/^ *"revision": "\([0-9a-f]\{40\}\)",*$/\1/p' docs/plaintext-dictionaries.json | head -1)
[ -n "$VOCR_PIN" ] && [ -n "$DICTS_PIN" ] || die "no pinned vita-vn-ocr commit or dictionary revision found"
at_pin() { [ "$(git -C "$1" rev-parse HEAD 2> /dev/null)" = "$2" ]; }
clone_pin() { # clone_pin URL PIN DIR [SPARSE]: DIR checked out at PIN (once, in the cache)
  [ -d "$3" ] && at_pin "$3" "$2" && return 0
  rm -rf "$3.tmp"
  timeout 600 git clone -q --filter=blob:none --no-checkout ${4:+--sparse} "$1" "$3.tmp" < /dev/null &&
    { [ -z "${4:-}" ] || git -C "$3.tmp" sparse-checkout set "$4"; } &&
    timeout 600 git -C "$3.tmp" checkout -q --detach "$2" < /dev/null &&
    rm -rf "$3" && mv "$3.tmp" "$3"
}
if [ -n "${VJO_VOCR_SOURCE_DIR:-}" ]; then
  VOCR=$VJO_VOCR_SOURCE_DIR
elif [ -f .refs/vita-vn-ocr/runtime/vocr.c ] && at_pin .refs/vita-vn-ocr "$VOCR_PIN"; then
  VOCR=$ROOT/.refs/vita-vn-ocr
else
  VOCR=$CACHE/vita-vn-ocr-${VOCR_PIN:0:12}
  clone_pin https://github.com/bee-san/vita-vn-ocr.git "$VOCR_PIN" "$VOCR" || die "cannot fetch vita-vn-ocr $VOCR_PIN"
fi
if [ -n "${VJO_PLAINTEXT_SOURCE:-}" ]; then
  DICTS=$VJO_PLAINTEXT_SOURCE
elif [ -f .refs/plaintext-dicts/processed/plaintext.vjdict.zip ] && at_pin .refs/plaintext-dicts "$DICTS_PIN"; then
  DICTS=$ROOT/.refs/plaintext-dicts
else
  DICTS=$CACHE/plaintext-dicts-${DICTS_PIN:0:12}
  if ! clone_pin git@github.com:bee-san/plaintext-dicts.git "$DICTS_PIN" "$DICTS" processed > "$LOGS/dicts-clone.log" 2>&1; then
    echo "fast_gate: no private dictionary (bee-san/plaintext-dicts $DICTS_PIN: $LOGS/dicts-clone.log):" \
         "its tests and the release zip are left out, as in CI for a fork's pull request" >&2
    DICTS=
  fi
fi
case $VOCR in /*) ;; *) VOCR=$ROOT/$VOCR ;; esac
case $DICTS in ''|/*) ;; *) DICTS=$ROOT/$DICTS ;; esac
if [ "$E2E" = on ] && [ -z "$DICTS" ]; then E2E=blocked; fi

# ---- the real models (the models lane): every pinned one the host tests read, checked before a lane starts
MODEL_DIR=${VJO_VOCR_MODEL_DIR:-$CACHE/models}
case $MODEL_DIR in /*) ;; *) MODEL_DIR=$ROOT/$MODEL_DIR ;; esac
MODELS_WHY=
if [ "$MODELS" != off ]; then
  if [ "$MODELS" = on ] && [ -z "${VJO_VOCR_MODEL_DIR:-}" ]; then
    # into the cache only, under the e2e lane's lock (it fills the same directory)
    mkdir -p "$MODEL_DIR" &&
      timeout 900 flock "$CACHE/models.lock" python3 tools/prepare_ocr.py --backend vocr-tests --output "$MODEL_DIR" \
        > "$LOGS/models-fetch.log" 2>&1 < /dev/null || MODELS_WHY="fetching them failed (${LOGS#"$ROOT"/}/models-fetch.log)"
  fi
  if python3 tools/prepare_ocr.py --backend vocr-tests --check --output "$MODEL_DIR" > "$LOGS/models-check.log" 2>&1; then
    MODELS=run
  else
    # a directory the caller names is never written: only the cache's is filled (--models on)
    hint="--models on fetches them"
    [ -z "${VJO_VOCR_MODEL_DIR:-}" ] || hint="tools/prepare_ocr.py --backend vocr-tests --output DIR fetches them"
    count=$(sed -n '$s/ in .*//p' "$LOGS/models-check.log")
    bad=$(sed '$d' "$LOGS/models-check.log" | head -3 | paste -sd ';' -)
    MODELS_WHY=${MODELS_WHY:-"${count:-the check failed} (${bad:+$bad; }${LOGS#"$ROOT"/}/models-check.log; $hint)"}
    if [ "$MODELS" = on ]; then MODELS=fail; else MODELS=skip; fi
  fi
fi

# ---- the containers
ENGINE=${CONTAINER_ENGINE:-}
if [ -z "$ENGINE" ]; then
  for e in docker podman; do
    if command -v "$e" > /dev/null 2>&1 && timeout 60 "$e" info > /dev/null 2>&1; then ENGINE=$e; break; fi
  done
  [ -n "$ENGINE" ] || die "no working docker or podman (CONTAINER_ENGINE=none runs the lanes on this machine's own tools)"
fi
image() { # image NAME DOCKERFILE FILE...: the tag of NAME's image, built if it is not there
  local name=$1 dockerfile=$2; shift 2
  local tag ctx
  tag=rubi-gate-$name:$(cat "$dockerfile" "$@" | sha256sum | cut -c1-12)
  if ! "$ENGINE" image inspect "$tag" > /dev/null 2>&1; then
    ctx=$(mktemp -d)
    cp "$@" "$ctx/"
    echo "fast_gate: building $tag (once)" >&2
    timeout 1800 "$ENGINE" build -t "$tag" -f "$dockerfile" "$ctx" > "$LOGS/image-$name.log" 2>&1 < /dev/null ||
      { rm -rf "$ctx"; echo "fast_gate: building $tag failed: $LOGS/image-$name.log" >&2; return 1; }
    rm -rf "$ctx"
  fi
  echo "$tag"
}
if [ "$ENGINE" != none ]; then
  HOST_IMAGE=$(image host tools/gate/host.Dockerfile tools/requirements-anki.txt) || exit 2
  VITA_IMAGE=$(image vita tools/gate/vita.Dockerfile tools/setup_toolchain.sh tools/setup_settings_deps.py) || exit 2
  # Every path a lane reads, at its own path inside the container; files are made as this user.
  MOUNTS=(-v "$ROOT:$ROOT" -v "$CACHE:$CACHE")
  for d in "$VOCR" ${DICTS:+"$DICTS"} "$(cd "$(git rev-parse --git-common-dir)" && pwd)"; do
    case $d/ in "$ROOT"/*|"$CACHE"/*) ;; *) MOUNTS+=(-v "$d:$d") ;; esac
  done
  if [ "$MODELS" = run ]; then # the models lane only reads them
    case $MODEL_DIR/ in "$ROOT"/*|"$CACHE"/*) ;; *) MOUNTS+=(-v "$MODEL_DIR:$MODEL_DIR:ro") ;; esac
  fi
  USER_ARGS=(--user "$(id -u):$(id -g)")
  [ "$ENGINE" != podman ] || USER_ARGS+=(--userns=keep-id)
  # The lanes' TMPDIR (/tmp: every suite's own directory, the compilers' temporary files) in memory, not in the
  # container's overlay on the machine's disk. The suites write real files and sync them (the Shell's history
  # writer syncs every append): on the overlay each sync waits for whatever else the machine writes, and with
  # the fleet's training and gates on the same disk shell_history took 98-120+ s of its 120 s TIMEOUT, in every
  # host lane, on main too (2026-10-27); on tmpfs 2.2 s against 40.2 s on the overlay in the same minute. Only
  # where the files are changes: CI's runners and CONTAINER_ENGINE=none keep their own TMPDIR.
  USER_ARGS+=(--tmpfs /tmp:rw,exec,nosuid,nodev,mode=1777)
fi
run_lane() { # run_lane NAME host|vita SCRIPT: SCRIPT in that image (or here), its log in LOGS/NAME.log
  local name=$1 kind=$2 script=$3 t0 rc
  printf 'set -euo pipefail\ncd %q\n%s\n' "$ROOT" "$script" > "$LOGS/$name.sh"
  t0=$(date +%s)
  if [ "$ENGINE" = none ]; then
    bash "$LOGS/$name.sh" > "$LOGS/$name.log" 2>&1 < /dev/null
  else
    local image=$HOST_IMAGE
    [ "$kind" = host ] || image=$VITA_IMAGE
    "$ENGINE" run --rm --init --name "rubi-gate-$$-$name" "${USER_ARGS[@]}" -e HOME=/tmp -e TMPDIR=/tmp \
      "${MOUNTS[@]}" -w "$ROOT" "$image" bash "$LOGS/$name.sh" > "$LOGS/$name.log" 2>&1 < /dev/null
  fi
  rc=$?
  echo "$rc $(( $(date +%s) - t0 ))" > "$LOGS/$name.rc"
  return $rc
}
cleanup() {
  local pids
  pids=$(jobs -p)
  [ -z "$pids" ] || kill $pids 2> /dev/null
  if [ "$ENGINE" != none ]; then
    for n in host-debug host-normal models arm-debug arm-normal vita-debug vita-normal plaintext e2e; do
      "$ENGINE" rm -f "rubi-gate-$$-$n" > /dev/null 2>&1 || true
    done
  fi
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM HUP

q() { printf '%q' "$1"; }
rm -f "$LOGS"/*.rc
echo "fast gate: $(git rev-parse --short HEAD)${DIRTY:+ + local changes} against $(git rev-parse --short "$BASE")," \
     "$(printf '%s\n' "$SCOPE" | tr '\n' ' ')e2e=$E2E, models=$MODELS, $JOBS jobs, ${ENGINE} (logs: ${LOGS#"$ROOT"/})"

# ---- host: build and every suite, both builds
anki='export VJO_ANKI_PYTHON=/opt/anki/bin/python VJO_REQUIRE_ANKI=1'
[ "$ENGINE" != none ] || anki='[ -z "${VJO_ANKI_PYTHON:-}" ] || export VJO_REQUIRE_ANKI=1'
for v in debug normal; do
  dbg=$([ $v = debug ] && echo ON || echo OFF)
  select=$([ $v = normal ] && echo "-LE variant_free" || true)
  run_lane host-$v host "
cmake -S . -B $(q "$OUT/host-$v") -G Ninja -DVJO_DEBUG=$dbg -DVJO_VOCR_SOURCE_DIR=$(q "$VOCR") \\
  -DVJO_PLAINTEXT_SOURCE=$(q "$DICTS")
cmake --build $(q "$OUT/host-$v") -j$JOBS
$anki
ctest --test-dir $(q "$OUT/host-$v") -j$JOBS --output-on-failure $select" &
done
# ---- the models lane: the debug build with the release models, whose host checks CI's job never runs (#335's
# failed only there); the variant_free suites read no build, so no model: host-debug's run is theirs. The models
# are checked again where the lane runs: the container has only the directory, so a symlink in it to a file outside
# (or a file the directory lost meanwhile) fails here, by name, not as the tests' failures. The gate's weights go to
# cmake/VocrGate.cmake from there (VJO_VOCR_GATE_BLOB), checked, instead of its download.
if [ "$MODELS" = run ]; then
  run_lane models host "
if ! seen=\$(python3 tools/prepare_ocr.py --backend vocr-tests --check --output $(q "$MODEL_DIR") 2>&1); then
  echo 'error: the lane does not see the models the gate checked (a symlink to a file outside the directory?)'
  printf '%s\n' \"\$seen\" | sed 's/^/error: in the lane: /' | head -4 || true
  exit 1
fi
cmake -S . -B $(q "$OUT/models-debug") -G Ninja -DVJO_DEBUG=ON -DVJO_VOCR_SOURCE_DIR=$(q "$VOCR") \\
  -DVJO_PLAINTEXT_SOURCE=$(q "$DICTS") -DVJO_VOCR_MODEL_DIR=$(q "$MODEL_DIR") \\
  -DVJO_VOCR_GATE_BLOB=$(q "$MODEL_DIR/textgate_p8_c8_w8.vtd")
cmake --build $(q "$OUT/models-debug") -j$JOBS
$anki
ctest --test-dir $(q "$OUT/models-debug") -j$JOBS --output-on-failure -LE variant_free" &
fi

# ---- the ARM lanes: the dictionary they compare lookups on, unpacked once, then both builds
if [ "$ARM" = 1 ]; then
  (
    if [ -n "$DICTS" ]; then
      run_lane plaintext host "python3 tools/prepare_plaintext.py --source $(q "$DICTS") -o $(q "$OUT/plaintext/plaintext.vjdict")" ||
        exit 1
      plain="VJO_PLAINTEXT_DIR=$(q "$OUT/plaintext")"
    else
      plain=
    fi
    tools='ARM_CC=arm-linux-gnueabihf-gcc-14 QEMU=qemu-arm-static'
    [ "$ENGINE" != none ] || tools=
    for n in 1 0; do
      v=$([ $n = 1 ] && echo debug || echo normal)
      run_lane arm-$v host "$plain VJO_DEBUG=$n $tools VJO_VOCR_SOURCE_DIR=$(q "$VOCR") ARM_JOBS=$JOBS \\
  bash tools/arm_host_tests.sh $(q "$OUT/arm-$v")" &
    done
    wait
  ) &
fi

# ---- the Vita builds, then the package e2e
VITA_PIDS=()
for v in debug normal; do
  dbg=$([ $v = debug ] && echo ON || echo OFF)
  release=
  [ "$E2E" != on ] || release="rm -f $(q "$OUT/vita-$v")/Rubi-*.zip; cmake --build $(q "$OUT/vita-$v") --target release"
  path='export PATH=/opt/toolchain/psp2cxml-tool/mbuild:$PATH'
  [ "$ENGINE" != none ] || path="export PATH=$(q "$ROOT/.refs/toolchain/psp2cxml-tool/mbuild"):\$PATH"
  B=$OUT/vita-$v
  run_lane vita-$v vita "$path
cmake -S vita -B $(q "$B") -DCMAKE_BUILD_TYPE=Release -DVJO_DEBUG=$dbg -DVJO_VOCR_SOURCE_DIR=$(q "$VOCR") \\
  -DVJO_PLAINTEXT_SOURCE=$(q "$DICTS")
cmake --build $(q "$B") -j$JOBS
$release
python3 tools/check_memory_ledger.py --variant $v --kernel $(q "$B/VitaJPOverlay_Kernel.skprx") \\
  --shell $(q "$B/VitaJPOverlay_Shell.suprx") --worker $(q "$B/VitaJPOverlay_OCR.suprx") | grep -E 'VitaJPOverlay_|memory ledger'" &
  VITA_PIDS+=($!)
done
if [ "$E2E" = on ]; then
  # (the Vita lanes are this shell's children: only it can wait for them)
  vita_ok=1
  for p in "${VITA_PIDS[@]}"; do wait "$p" || vita_ok=0; done
  if [ $vita_ok = 1 ]; then
    run_lane e2e host "
t0=\$(date +%s); step() { echo \"e2e step: \$1 (\$(( \$(date +%s) - t0 ))s)\"; }
exec 9> $(q "$CACHE/models.lock") && flock 9
python3 tools/prepare_ocr.py --backend vocr-tiers --output $(q "$CACHE/models")
python3 tools/prepare_ocr.py --backend vocr-lm --output $(q "$CACHE/models")
flock -u 9
step 'the pinned models'
normal=\$(ls $(q "$OUT/vita-normal")/Rubi-*.zip | grep -v -- -debug.zip)
debug=\$(ls $(q "$OUT/vita-debug")/Rubi-*-debug.zip)
version=\$(basename \"\$normal\" .zip); version=\${version#Rubi-}
rm -rf $(q "$OUT/release")
python3 tools/package_variants.py --normal \"\$normal\" --debug \"\$debug\" --models $(q "$CACHE/models") \\
  --version \"\$version\" --source-revision $REV --sources . -o $(q "$OUT/release")
step 'tools/package_variants.py'
# the two packages side by side, each in its own work directory; their logs in order
pids=()
for zip in $(q "$OUT/release")/Rubi-*.zip; do
  name=\$(basename \"\$zip\" .zip)
  python3 tools/e2e_package.py \"\$zip\" --work $(q "$OUT/e2e")/\$name > $(q "$OUT/e2e")-\$name.log 2>&1 &
  pids+=(\$!)
done
rc=0
for p in \"\${pids[@]}\"; do wait \$p || rc=1; done
cat $(q "$OUT/e2e")-*.log
step 'tools/e2e_package.py on each zip'
exit \$rc"
  else
    echo "1 0" > "$LOGS/e2e.rc"
    echo "not run: a Vita build failed" > "$LOGS/e2e.log"
  fi
fi
wait

# ---- the result
fail=0
line() { printf '  %-5s %-12s %5s  %s\n' "$@"; }
summary() { # summary NAME: the lane's PASS/FAIL line and what its log says
  local name=$1 rc=1 secs=- what=
  [ -f "$LOGS/$name.rc" ] && read -r rc secs < "$LOGS/$name.rc"
  local log=$LOGS/$name.log
  case $name in
    host-*|models) what=$(grep -E 'tests passed, [0-9]+ tests failed' "$log" | tail -1) ;;
    arm-*) what="$(grep -c -E '^arm .*(cases pass|identical to the host)' "$log" 2> /dev/null) suites pass$(grep -q '^arm TMPDIR: nothing left' "$log" && echo ', TMPDIR clean')" ;;
    vita-*) what=$(grep -E '^memory ledger' "$log" | tail -1) ;;
    e2e) what=$(grep -c -E '^PASS' "$log" 2> /dev/null) && what="$what checks pass" ;;
    plaintext) what=$(tail -1 "$log") ;;
  esac
  if [ "$rc" = 0 ]; then
    line PASS "$name" "${secs}s" "${what:0:110}"
  else
    fail=1
    line FAIL "$name" "${secs}s" "${what:0:60} (exit $rc: ${log#"$ROOT"/})"
    grep -E 'FAILED|\*\*\*Failed|Error|error:|^FAIL' "$log" 2> /dev/null | head -8 | sed 's/^/          /'
  fi
}
summary host-debug
summary host-normal
case $MODELS in
  run) summary models ;;
  off) line SKIP models "" "--models off" ;;
  skip) line SKIP models "" "$MODELS_WHY" ;;
  fail) fail=1; line FAIL models "" "$MODELS_WHY" ;;
esac
if [ "$ARM" = 1 ]; then
  [ -z "$DICTS" ] || [ ! -f "$LOGS/plaintext.rc" ] || [ "$(cut -d' ' -f1 "$LOGS/plaintext.rc")" = 0 ] || summary plaintext
  summary arm-debug
  summary arm-normal
else
  line SKIP arm "" "--no-arm"
fi
summary vita-debug
summary vita-normal
case $E2E in
  on) summary e2e ;;
  off) line SKIP e2e "" "no packaging change (tools/ci_scope.py; --e2e on runs it)" ;;
  blocked) fail=1; line FAIL e2e "" "packaging changed, but there is no private dictionary for the release zips" ;;
esac
total=$(( $(date +%s) - START ))
verdict=$([ $fail = 0 ] && echo PASS || echo FAIL)
note=$([ -n "$DICTS" ] || echo ", without the private dictionary")
result="FAST GATE: $verdict in $((total / 60))m$((total % 60))s ($(git rev-parse --short HEAD)${DIRTY:+ + local changes}, e2e=$E2E, models=$MODELS$note)"
echo "$result"
echo "$result" > "$OUT/result.txt"
exit $fail
