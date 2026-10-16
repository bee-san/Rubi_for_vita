#!/usr/bin/env bash
# Vita3K smoke test (#74 preflight): tools/vita3k_smoke/ as a homebrew app in Vita3K,
# headless under Xvfb. It runs the real game worker's OCR jobs (probe, forced
# fallback, textbox, full screen, textbox again, each model size's own recognizer at
# the full screen, the 16-bit and 10-bit formats) and an
# Anki card's screenshot in each format (API 23: the JPEG encoder, which Pillow then
# decodes) in a Vita user process with the release weights, and fails on a crash, a hang
# or a failed stage. It is not evidence about the console: Vita3K cannot load the
# kernel plugin, and has no ScePhyMemPartKD, SceShell or taiHEN kernel
# (docs/offline-testing.md section 5, AGENTS.md rule 5).
#
#   tools/vita3k_smoke.sh [OUT_DIR]
#
#   env VITASDK              VitaSDK (to build the app)
#   env VJO_VOCR_SOURCE_DIR  vita-vn-ocr checkout at the pinned commit
#   env VJO_VOCR_MODEL_DIR   directory with H15_w8.vocr, FL10_w8.vocr and the three model sizes'
#                            recognizers (tools/model_tiers.json "model")
#   env VITA3K               an extracted Vita3K build (vita-vn-ocr vita/scripts/fetch_vita3k.sh),
#                            with app/usr/bin/Vita3K and, if the host glibc is older, compat/
#   env TIMEOUT              seconds (default 900)
# Prints one line, "vita3k smoke: PASS|FAIL|SKIPPED <why>"; exit 0 for PASS or SKIPPED, 1 for FAIL.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/vita3k-smoke$([ "${VJO_DEBUG:-1}" = 0 ] && echo -normal)/run}
skip() { echo "vita3k smoke: SKIPPED $*"; exit 0; }
fail() { echo "vita3k smoke: FAIL $*"; exit 1; }
[ -n "${VITASDK:-}" ] && [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || skip "(no VitaSDK: set VITASDK)"
[ -n "${VJO_VOCR_SOURCE_DIR:-}" ] && [ -f "$VJO_VOCR_SOURCE_DIR/runtime/vocr_lines_rows.c" ] || skip "(no vita-vn-ocr runtime: set VJO_VOCR_SOURCE_DIR)"
[ -f "${VJO_VOCR_MODEL_DIR:-}/H15_w8.vocr" ] && [ -f "$VJO_VOCR_MODEL_DIR/FL10_w8.vocr" ] || skip "(no release weights: set VJO_VOCR_MODEL_DIR)"
sizes=$(sed -n 's/.*"model": "\([^"]*\)".*/\1/p' "$root/tools/model_tiers.json")
[ "$(echo "$sizes" | wc -w)" -eq 3 ] || fail "(tools/model_tiers.json: expected three model sizes, read: $sizes)"
for m in $sizes; do [ -f "$VJO_VOCR_MODEL_DIR/$m" ] || skip "(no $m, a model size's recognizer, in VJO_VOCR_MODEL_DIR)"; done
V=${VITA3K:-}
[ -n "$V" ] && [ -x "$V/app/usr/bin/Vita3K" ] || skip "(no Vita3K build: set VITA3K)"
command -v xvfb-run > /dev/null || skip "(xvfb-run is not installed)"

# one directory per build (VJO_DEBUG=0: the normal one), so both can be checked at once in one tree
build=$root/build/vita3k-smoke$([ "${VJO_DEBUG:-1}" = 0 ] && echo -normal)
# VJO_DEBUG=0: the normal build's game worker (docs/release-variants.md)
cmake -S "$root/tools/vita3k_smoke" -B "$build" -DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" \
  -DVJO_DEBUG="$([ "${VJO_DEBUG:-1}" = 0 ] && echo OFF || echo ON)" > "$build.config.log" 2>&1 \
  && cmake --build "$build" -j4 > "$build.build.log" 2>&1 || fail "(the app did not build: $build.build.log)"
mkdir -p "$out"
sbx=$(mktemp -d "${TMPDIR:-/tmp}/vjo-vita3k.XXXXXX")
trap 'rm -rf "$sbx"' EXIT
mkdir -p "$sbx/config/Vita3K" "$sbx/data/Vita3K/Vita3K/ux0/data/vjo_smoke" "$sbx/cache"
cat > "$sbx/config/Vita3K/config.yml" <<'EOF'
---
initial-setup: true
backend-renderer: OpenGL
validation-layer: false
discord-rich-presence: false
show-live-area-screen: false
delay-start: 0
delay-background: 0
log-level: 2
...
EOF
ux0=$sbx/data/Vita3K/Vita3K/ux0
cp "$VJO_VOCR_MODEL_DIR/H15_w8.vocr" "$VJO_VOCR_MODEL_DIR/FL10_w8.vocr" "$ux0/data/vjo_smoke/"
for m in $sizes; do cp -L "$VJO_VOCR_MODEL_DIR/$m" "$ux0/data/vjo_smoke/"; done
libs="$V/app/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu"
if [ -x "$V/app/usr/bin/ld-linux-x86-64.so.2" ]; then loader=("$V/app/usr/bin/ld-linux-x86-64.so.2" --library-path "$V/compat:$libs")
elif [ -d "$V/compat" ]; then loader=("$V/compat/ld-linux-x86-64.so.2" --library-path "$V/compat:$libs")
elif [ -x "$V/ld-linux-x86-64.so.2" ]; then loader=("$V/ld-linux-x86-64.so.2" --library-path "$V:$libs")
else loader=(); fi
t0=$(date +%s)
env APPDIR="$V/app" HOME="$sbx" XDG_CONFIG_HOME="$sbx/config" XDG_DATA_HOME="$sbx/data" XDG_CACHE_HOME="$sbx/cache" \
  timeout "${TIMEOUT:-900}" xvfb-run -a -s "-screen 0 1280x720x24" \
  "${loader[@]}" "$V/app/usr/bin/Vita3K" --frontend sdl "$build/vjo_smoke.vpk" < /dev/null > "$out/vita3k.stdout" 2>&1
rc=$?
secs=$(( $(date +%s) - t0 ))
cp "$sbx/cache/Vita3K/vita3k.log" "$out/vita3k.log" 2>/dev/null || true
[ -f "$ux0/data/vjo_smoke/results.txt" ] || fail "(Vita3K exit $rc after ${secs}s, the app wrote no results: $out/vita3k.stdout)"
cp "$ux0/data/vjo_smoke/results.txt" "$out/results.txt"
grep -q "^done failures=0$" "$out/results.txt" \
  || fail "(Vita3K exit $rc after ${secs}s: $(grep -m1 '^done\|rc=-' "$out/results.txt" || tail -1 "$out/results.txt"); $out/results.txt)"
[ "$rc" -eq 0 ] && grep -q "The app exited with code 0" "$out/vita3k.stdout" \
  || fail "(the jobs finished, then Vita3K exit $rc after ${secs}s: a crash or hang at the end; $out/vita3k.stdout)"
stages=$(grep -c "^stage " "$out/results.txt")
lines=$(awk '/^stage /{for(i=1;i<=NF;i++) if ($i ~ /^recognized=/) {split($i,a,"="); n+=a[2]}} END{print n+0}' "$out/results.txt")
# API 23: the three screenshots' JPEGs, each the size its job reported, decoded at 960x544
cp "$ux0"/data/vjo_smoke/*.jpg "$out/" 2>/dev/null
shots=$("${PYTHON:-python3}" - "$out" <<'PY' 2>&1
import re, sys
from pathlib import Path
from PIL import Image
out = Path(sys.argv[1])
rows = [l for l in (out / "results.txt").read_text().splitlines() if l.startswith("shot ")]
assert len(rows) == 3, rows
for l in rows:
    card, size = l.split()[1], int(re.search(r" bytes=(\d+)", l).group(1))
    jpg = out / f"{card}.jpg"
    assert jpg.stat().st_size == size, (jpg, size)
    with Image.open(jpg) as im:
        im.load()
        assert (im.format, im.size) == ("JPEG", (960, 544)), (jpg, im.format, im.size)
print(" ".join(str((out / f"{l.split()[1]}.jpg").stat().st_size) for l in rows))
PY
) || fail "(a screenshot's JPEG is missing or does not decode: $shots)"
echo "vita3k smoke: PASS ($stages jobs, $lines lines recognized, 3 screenshots of $shots B decoded, the app and Vita3K exited 0 after ${secs}s; $out/results.txt)"
