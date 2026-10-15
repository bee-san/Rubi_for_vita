#!/usr/bin/env bash
# Vita3K smoke of the Overlay app: settings/main.c and the sources the settings VPK ships,
# unchanged, with a scripted controller (tools/vita3k_settings_smoke.c, vita/CMakeLists.txt
# VJO_SETTINGS_SMOKE), over synthetic installed games (tools/make_settings_smoke_games.py).
# The script scrolls the Games list down and back, pages with L/R (pressed and held), jumps
# letters with Left/Right, toggles games, cycles the filters, rescans with Start, visits the
# other pages and leaves with Cancel (○ and × as the console's setting has them, #106). It fails
# on a crash, a hang, an unsafe texture free or a
# texture left alive. A crash check of the app's own code under Vita3K's libgxm and the real
# libvita2d/libpng, headless under Xvfb; not evidence about the console
# (docs/offline-testing.md section 5, AGENTS.md rule 5).
#
#   tools/vita3k_settings_smoke.sh [OUT_DIR]
#
#   env VITASDK              VitaSDK (to build the app)
#   env VJO_VOCR_SOURCE_DIR  vita-vn-ocr checkout at the pinned commit (the Vita build's configure)
#   env VITA3K               an extracted Vita3K build, as for tools/vita3k_smoke.sh
#   env VITA3K_FIRMWARE      Sony's system software (PSVUPDAT.PUP) and
#   env VITA3K_FONTS         its font package (PSP2UPDAT.PUP with the system fonts): the app draws
#                            with the firmware's PVF library and fonts, which this Vita3K does not
#                            emulate (scePvfNewLib is unimplemented); --firmware installs both
#   env GAMES                synthetic games to install (default 120), TIMEOUT seconds (default 900)
#   env RELEASE_VPK          optional: the release JPOverlaySettings.vpk, then booted over the same
#                            games with no input; it must still be running after 30 s
# Prints one line, "vita3k settings smoke: PASS|FAIL|SKIPPED <why>"; exit 0 for PASS or SKIPPED.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/vita3k-settings-smoke/run}
skip() { echo "vita3k settings smoke: SKIPPED $*"; exit 0; }
fail() { echo "vita3k settings smoke: FAIL $*"; exit 1; }
[ -n "${VITASDK:-}" ] && [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || skip "(no VitaSDK: set VITASDK)"
[ -f "${VJO_VOCR_SOURCE_DIR:-}/runtime/vocr_lines_rows.c" ] || skip "(no vita-vn-ocr runtime: set VJO_VOCR_SOURCE_DIR)"
V=${VITA3K:-}
[ -n "$V" ] && [ -x "$V/app/usr/bin/Vita3K" ] || skip "(no Vita3K build: set VITA3K)"
[ -f "${VITA3K_FIRMWARE:-}" ] && [ -f "${VITA3K_FONTS:-}" ] || skip "(no firmware or font package: set VITA3K_FIRMWARE and VITA3K_FONTS)"
command -v xvfb-run > /dev/null || skip "(xvfb-run is not installed)"
PY=${PYTHON:-python3}

build=$root/build/vita3k-settings-smoke
mkdir -p "$root/build"
make_app() { # make_app PAD
  cmake -S "$root/vita" -B "$build" -DVJO_SETTINGS_SMOKE=ON -DVJO_SETTINGS_SMOKE_PAD="$1" \
    -DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" -DPython3_EXECUTABLE="$(command -v "$PY")" > "$build.config.log" 2>&1 \
    && cmake --build "$build" --target vjo_settings_smoke.vpk-vpk -j4 > "$build.build.log" 2>&1
}
# vita-elf-create refuses a code segment that ends just below the data segment: move it.
make_app 0 || { grep -q "segment 1 overlaps" "$build.build.log" && make_app 4096; } \
  || fail "(the app did not build: $build.config.log, $build.build.log)"
mkdir -p "$out"
sbx=$(mktemp -d "${TMPDIR:-/tmp}/vjo-vita3k-settings.XXXXXX")
trap 'rm -rf "$sbx"' EXIT
mkdir -p "$sbx/config/Vita3K" "$sbx/data/Vita3K/Vita3K/ux0/data" "$sbx/cache"
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
libs="$V/app/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu"
if [ -x "$V/app/usr/bin/ld-linux-x86-64.so.2" ]; then loader=("$V/app/usr/bin/ld-linux-x86-64.so.2" --library-path "$V/compat:$libs")
elif [ -d "$V/compat" ]; then loader=("$V/compat/ld-linux-x86-64.so.2" --library-path "$V/compat:$libs")
elif [ -x "$V/ld-linux-x86-64.so.2" ]; then loader=("$V/ld-linux-x86-64.so.2" --library-path "$V:$libs")
else loader=(); fi
vita3k() {
  env APPDIR="$V/app" HOME="$sbx" XDG_CONFIG_HOME="$sbx/config" XDG_DATA_HOME="$sbx/data" XDG_CACHE_HOME="$sbx/cache" \
    timeout "${TIMEOUT:-900}" xvfb-run -a -s "-screen 0 1280x720x24" "${loader[@]}" "$V/app/usr/bin/Vita3K" "$@" < /dev/null
}
vita3k --firmware "$VITA3K_FIRMWARE" > "$out/firmware.stdout" 2>&1
vita3k --firmware "$VITA3K_FONTS" > "$out/fonts.stdout" 2>&1
[ -f "$sbx/data/Vita3K/Vita3K/vs0/sys/external/libpvf.suprx" ] || fail "(the firmware did not install: $out/firmware.stdout)"
ls "$sbx"/data/Vita3K/Vita3K/sa0/data/font/pvf/*.pvf > /dev/null 2>&1 || fail "(the font package did not install: $out/fonts.stdout)"
"$PY" "$root/tools/make_settings_smoke_games.py" "$ux0" "${GAMES:-120}" > "$out/games.txt" 2>&1 || fail "(no games: $out/games.txt)"
t0=$(date +%s)
vita3k --frontend sdl "$build/vjo_settings_smoke.vpk" > "$out/vita3k.stdout" 2>&1
rc=$?
secs=$(( $(date +%s) - t0 ))
cp "$sbx/cache/Vita3K/vita3k.log" "$out/vita3k.log" 2>/dev/null || true
cp "$ux0/data/vjo_settings_smoke.txt" "$out/results.txt" 2>/dev/null \
  || fail "(Vita3K exit $rc after ${secs}s, the app wrote no results: $out/vita3k.stdout)"
r=$(cat "$out/results.txt")
case $r in
  done*) ;;
  *) fail "(Vita3K exit $rc after ${secs}s, the app stopped at: $r; $out/vita3k.stdout)" ;;
esac
field() { echo "$r" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
[ "$rc" -eq 0 ] && grep -q "The app exited with code 0" "$out/vita3k.stdout" \
  || fail "(the script finished, then Vita3K exit $rc after ${secs}s: $out/vita3k.stdout)"
[ "$(field unsafe_frees)" = 0 ] || fail "($(field unsafe_frees) textures freed while the GPU may read them: $r)"
[ "$(field live)" = 0 ] || fail "($(field live) textures left alive at exit: $r)"
[ "$(field loads)" -gt 0 ] && [ "$(field draws)" -gt 0 ] || fail "(no icon was loaded or drawn: $r)"
[ "$(field max_live)" -le 9 ] || fail "(more than 9 icons alive: $r)"
[ "$(field frames)" -ge "$(field script)" ] || fail "(the app stopped before the script's end: $r)"
release=
if [ -n "${RELEASE_VPK:-}" ]; then
  TIMEOUT=30 vita3k --frontend sdl "$RELEASE_VPK" > "$out/release.stdout" 2>&1
  rrc=$?
  [ "$rrc" -eq 124 ] && ! grep -q "The app exited" "$out/release.stdout" \
    || fail "(the release VPK did not keep running for 30 s: exit $rrc, $out/release.stdout)"
  release="; the release VPK ran 30 s on its Games page"
fi
echo "vita3k settings smoke: PASS ($(field frames) frames, $(field loads) icon loads, $(field failed) refused," \
  "$(field frees) frees after $(field waits) GPU waits, $(field draws) draws, at most $(field max_live) alive," \
  "no unsafe free; $(cat "$out/games.txt"); the app and Vita3K exited 0 after ${secs}s$release; $out/results.txt)"
