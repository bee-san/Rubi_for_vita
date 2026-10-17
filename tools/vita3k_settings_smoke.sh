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
#   env SYSAPP               0 leaves out the system-application starts: the same app built to start
#                            as a system application (VJO_SETTINGS_SMOKE_SYSAPP), which Vita3K never
#                            does by itself, with stand-ins for SceShell's framebuffer hand-over and
#                            the GPU library's shared parameter buffer: a hand-over, the same with the
#                            shared buffer refused, and none (the app must close itself after 10 s);
#                            SYSAPP_TIMEOUT seconds each (default 300)
# Prints one line, "vita3k settings smoke: PASS|FAIL|SKIPPED <why>"; exit 0 for PASS or SKIPPED.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/vita3k-settings-smoke$([ "${VJO_DEBUG:-1}" = 0 ] && echo -normal)/run}
skip() { echo "vita3k settings smoke: SKIPPED $*"; exit 0; }
fail() { echo "vita3k settings smoke: FAIL $*"; exit 1; }
[ -n "${VITASDK:-}" ] && [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || skip "(no VitaSDK: set VITASDK)"
[ -f "${VJO_VOCR_SOURCE_DIR:-}/runtime/vocr_lines_rows.c" ] || skip "(no vita-vn-ocr runtime: set VJO_VOCR_SOURCE_DIR)"
V=${VITA3K:-}
[ -n "$V" ] && [ -x "$V/app/usr/bin/Vita3K" ] || skip "(no Vita3K build: set VITA3K)"
[ -f "${VITA3K_FIRMWARE:-}" ] && [ -f "${VITA3K_FONTS:-}" ] || skip "(no firmware or font package: set VITA3K_FIRMWARE and VITA3K_FONTS)"
command -v xvfb-run > /dev/null || skip "(xvfb-run is not installed)"
PY=${PYTHON:-python3}

# one directory per build (VJO_DEBUG=0: the normal one), so both can be checked at once in one tree
build=$root/build/vita3k-settings-smoke$([ "${VJO_DEBUG:-1}" = 0 ] && echo -normal)
mkdir -p "$root/build"
make_app() { # make_app PAD
  cmake -S "$root/vita" -B "$build" -DVJO_SETTINGS_SMOKE=ON -DVJO_SETTINGS_SMOKE_PAD="$1" \
    -DVJO_DEBUG="$([ "${VJO_DEBUG:-1}" = 0 ] && echo OFF || echo ON)" \
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
# Vita3K does not fault on an access to unmapped guest memory: it logs it with the PC and goes on,
# a read giving 0 and a write dropped (b75d5e6 vita3k/cpu/src/dynarmic_cpu.cpp:161-173, 198-213,
# 234-248); a console ends the app there. It also logs an instruction it cannot run and goes on
# (:272-317): an undefined one, which a console faults on, or one it does not implement, its own
# gap; either way the run says nothing past it. Any of these lines fails the run, the first quoted;
# no passing run's log has one. A write to the app's own code or read-only data is
# not seen: Vita3K maps the module's segments writable (load_self.cpp:651, mem.cpp:183).
GUEST_ACCESS='Invalid (read|write|exclusive write) of uint'
GUEST_INSN='(Unimplemented|Undefined|Unpredictable) instruction at|Decode error at|Unknown exception [0-9]+ Raised'
# guest_fault PATTERN LOG: the first such line and the number of such lines, or nothing. An invalid
# access is quoted with the PC, SP and LR Vita3K logs on the next line (the PC is where its JIT block
# began: the function, not the line); an instruction with the address in its own line (Vita3K logs
# two lines for each, so the count is twice the instructions).
guest_fault() {
  awk -v pat="$1" '
    want { want = 0; if ($1 == "PC:") { pc = $0; gsub(/ +/, " ", pc); first = first " (" pc ")" } }
    $0 ~ pat { if (!n++) { first = $0; sub(/^\[[0-9:.]+\] \|[A-Z]\| /, "", first); want = 1 } }
    END { if (n) printf "%s, %d in all", first, n }' "$2"
}
guest_faults() { # guest_faults WHAT LOG: fails the run on either kind
  local g
  g=$(guest_fault "$GUEST_ACCESS" "$2")
  [ -z "$g" ] || fail "($1 made an invalid memory access, which ends it on a console; Vita3K logs it and goes on: $g; $2)"
  g=$(guest_fault "$GUEST_INSN" "$2")
  [ -z "$g" ] || fail "(Vita3K could not run an instruction of $1 and went on, so the run says nothing past it: $g; $2)"
}
cp "$ux0/data/vjo_settings_smoke.txt" "$out/results.txt" 2>/dev/null
results=$? # kept before any check fails the run
guest_faults "the app" "$out/vita3k.stdout"
[ "$results" -eq 0 ] || fail "(Vita3K exit $rc after ${secs}s, the app wrote no results: $out/vita3k.stdout)"
r=$(cat "$out/results.txt")
case $r in
  done*) ;;
  *) fail "(Vita3K exit $rc after ${secs}s, the app stopped at: $r; $out/vita3k.stdout)" ;;
esac
field() { echo "$r" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
# Vita3K crashing after the app's exit 0 fails too (a crash in its own shutdown is still a crash:
# docs/offline-testing.md section 5 has the one #112 met), and the failure says what ended it: the
# signal, and its signal handlers' last lines. A fault outside the JIT is printed by dynarmic
# straight to stderr ("Unhandled SIGSEGV at rip 0x..."), then handed to Vita3K's handler, which
# logs "[signal_handler]: Unhandled access to 0x..." and raises SIGTRAP: its logger can lose that
# line. Dynarmic's line is quoted only for a run a signal ended (a fault Vita3K handled prints it
# too). The only |C| line of a passing run is "[translate_path]: Unknown device with path
# gro0:app used" (settings/catalog.c lists gro0:app; when it does not open, the app's look at
# gro0: itself is an |E| line, "[stat_file]: Cannot find device for path: gro0:"), so no |C|
# line is quoted as such.
crash=
if [ "$rc" -eq 124 ]; then
  crash="a hang: timeout ended it after ${TIMEOUT:-900}s"
elif [ "$rc" -gt 128 ] && [ "$rc" -lt 192 ]; then
  crash="killed by SIG$(kill -l $((rc - 128)) 2>/dev/null || echo $((rc - 128)))"
  fault=$(grep -h -o -E 'Unhandled SIG[A-Z]+ at rip 0x[0-9a-fA-F]+' "$out/vita3k.stdout" | tail -1)
  crash="$crash${fault:+; $fault}"
fi
access=$(grep -h -o -E '\[signal_handler\]: Unhandled access to 0x[0-9a-fA-F]+' "$out/vita3k.stdout" \
  "$out/vita3k.log" 2>/dev/null | tail -1)
crash="$crash${crash:+${access:+; }}$access"
[ "$rc" -eq 0 ] && grep -q "The app exited with code 0" "$out/vita3k.stdout" \
  || fail "(the script finished, then Vita3K exit $rc after ${secs}s${crash:+, $crash}: $out/vita3k.stdout)"
[ "$(field unsafe_frees)" = 0 ] || fail "($(field unsafe_frees) textures freed while the GPU may read them: $r)"
[ "$(field live)" = 0 ] || fail "($(field live) textures left alive at exit: $r)"
[ "$(field loads)" -gt 0 ] && [ "$(field draws)" -gt 0 ] || fail "(no icon was loaded or drawn: $r)"
[ "$(field max_live)" -le 9 ] || fail "(more than 9 icons alive: $r)"
[ "$(field frames)" -ge "$(field script)" ] || fail "(the app stopped before the script's end: $r)"
release=
if [ -n "${RELEASE_VPK:-}" ]; then
  TIMEOUT=30 vita3k --frontend sdl "$RELEASE_VPK" > "$out/release.stdout" 2>&1
  rrc=$?
  guest_faults "the release VPK" "$out/release.stdout"
  [ "$rrc" -eq 124 ] && ! grep -q "The app exited" "$out/release.stdout" \
    || fail "(the release VPK did not keep running for 30 s: exit $rrc, $out/release.stdout)"
  release="; the release VPK ran 30 s on its Games page"
fi
# The system-application start, which Vita3K never takes by itself (its sceAppMgrGetBudgetInfo is a
# stub that refuses): the same app built with VJO_SETTINGS_SMOKE_SYSAPP, where the smoke answers it
# as a system application and stands in for SceShell's hand-over of the shared framebuffer and the
# GPU library's shared parameter buffer (tools/vita3k_settings_smoke.c), with the app's own
# settings/sysapp_gfx.c linked unchanged. Three starts: a hand-over at the 31st read, the same with
# the shared parameter buffer refused (the app's own), and none (the app closes after 10 s, says why
# in log.txt). SYSAPP=0 leaves them out.
sysapp=
if [ "${SYSAPP:-1}" != 0 ]; then
  sbuild=$build-sysapp
  make_sysapp() { # make_sysapp PAD
    cmake -S "$root/vita" -B "$sbuild" -DVJO_SETTINGS_SMOKE=ON -DVJO_SETTINGS_SMOKE_SYSAPP=ON -DVJO_SETTINGS_SMOKE_PAD="$1" \
      -DVJO_DEBUG="$([ "${VJO_DEBUG:-1}" = 0 ] && echo OFF || echo ON)" \
      -DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" -DPython3_EXECUTABLE="$(command -v "$PY")" > "$sbuild.config.log" 2>&1 \
      && cmake --build "$sbuild" --target vjo_settings_smoke.vpk-vpk -j4 > "$sbuild.build.log" 2>&1
  }
  make_sysapp 0 || { grep -q "segment 1 overlaps" "$sbuild.build.log" && make_sysapp 4096; } \
    || fail "(the system-application smoke app did not build: $sbuild.config.log, $sbuild.build.log)"
  mkdir -p "$ux0/data/VitaJPOverlay"
  applog=$ux0/data/VitaJPOverlay/log.txt
  for mode in "handover 30" "nopb 30" "never"; do
    name=${mode%% *}
    printf '%s' "$mode" > "$ux0/data/vjo_settings_smoke_sysapp.txt"
    rm -f "$ux0/data/vjo_settings_smoke.txt" "$applog"
    t0=$(date +%s)
    TIMEOUT=${SYSAPP_TIMEOUT:-300} vita3k --frontend sdl "$sbuild/vjo_settings_smoke.vpk" > "$out/sysapp-$name.stdout" 2>&1
    src=$?
    ssecs=$(( $(date +%s) - t0 ))
    cp "$applog" "$out/sysapp-$name.log.txt" 2>/dev/null || : > "$out/sysapp-$name.log.txt"
    guest_faults "the system-application start ($mode)" "$out/sysapp-$name.stdout"
    [ "$src" -eq 124 ] && fail "(the system-application start ($mode) hung: timeout ended it after ${SYSAPP_TIMEOUT:-300}s; $out/sysapp-$name.stdout)"
    [ "$src" -le 128 ] || fail "(the system-application start ($mode): Vita3K exit $src; $out/sysapp-$name.stdout)"
    sr=$(cat "$ux0/data/vjo_settings_smoke.txt" 2>/dev/null)
    gfx=$(grep -h "overlay app: graphics: " "$out/sysapp-$name.log.txt" | tr '\n' '|')
    case $mode in
      never)
        case $sr in exit-error*frames=0\ *) ;; *) fail "(no hand-over: the app did not close before its first frame: '$sr'; $out/sysapp-$name.stdout)" ;; esac
        grep -q "The app exited with code 1" "$out/sysapp-$name.stdout" || fail "(no hand-over: not the app's exit code 1: $out/sysapp-$name.stdout)"
        case $gfx in *"graphics: waiting for the display: nothing handed over in "*"graphics: no display from the system in 10 s ("*"buffer index 1); closing"*) ;;
          *) fail "(no hand-over: log.txt does not say why the app closed: '$gfx')" ;; esac
        [ "$ssecs" -ge 10 ] || fail "(no hand-over: the app closed after ${ssecs}s, before its 10 s)"
        sysapp="$sysapp; none: closed itself after ${ssecs}s with its two log.txt lines" ;;
      *)
        case $sr in done*) ;; *) fail "(the system-application start ($mode) stopped at: '$sr'; $out/sysapp-$name.stdout)" ;; esac
        grep -q "The app exited with code 0" "$out/sysapp-$name.stdout" || fail "(the system-application start ($mode): not the app's exit 0: $out/sysapp-$name.stdout)"
        sfield() { echo "$sr" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
        [ "$(sfield unsafe_frees)" = 0 ] && [ "$(sfield live)" = 0 ] && [ "$(sfield frames)" -ge "$(sfield script)" ] \
          || fail "(the system-application start ($mode): $sr)"
        want="graphics: system application; GPU library 0x0A: 0x00000000; display: buffer 2 at read 31 after "
        [ "$name" = nopb ] && want="graphics: system application; GPU library 0x0A: 0x805B0017, then its own parameter buffer 0x03: 0x00000000; display: buffer 2 at read 31 after "
        case $gfx in *"$want"*) ;; *) fail "(the system-application start ($mode): log.txt has '$gfx', not '$want...')" ;; esac
        ms=$(echo "$gfx" | sed -n 's/.*display: buffer 2 at read 31 after \([0-9]*\) ms.*/\1/p')
        sysapp="$sysapp; $mode: $(sfield frames) frames, buffer 2 after ${ms:-?} ms" ;;
    esac
  done
  sysapp="; the system-application start under Vita3K's stand-ins${sysapp}"
fi
echo "vita3k settings smoke: PASS ($(field frames) frames, $(field loads) icon loads, $(field failed) refused," \
  "$(field frees) frees after $(field waits) GPU waits, $(field draws) draws, at most $(field max_live) alive," \
  "no unsafe free; $(cat "$out/games.txt"); the app and Vita3K exited 0 after ${secs}s$release$sysapp; $out/results.txt)"
