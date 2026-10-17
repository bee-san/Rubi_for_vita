#!/usr/bin/env bash
# capture_app.sh ONE_VPK OUT_DIR: the promo's Rubi app shots, as Vita3K draws the app's own code.
# ONE_VPK is the one-VPK release made around the capture app (capture/run.sh: app_capture_patch.py's scripts,
# capture_package.py, the checkout's package_release.py and package_vpk.py). One sandboxed Vita3K card, headless
# under Xvfb (as tools/vita3k_settings_smoke.sh runs it), with the official firmware and its fonts, two runs:
#   1. ONE_VPK installed as Vita3K installs a VPK; the app opens on its Install page (the plan), Enter, the run
#      (the capture script waits for it), its result;
#   2. the installed app started again (-r VJOSET001): it opens on Games; the Games and OCR & lookup beats.
# The card before run 1: original synthetic games (make_promo_games.py: titles and icons made for the film),
# HENkaku's stock taiHEN configurations in ux0:tai and ur0:tai (as h-encore writes them; the one-VPK e2e test's),
# and nothing of the overlay. ffmpeg records each run's screen losslessly (screen-1.mkv, screen-2.mkv); split them
# with split_holds.py. Vita3K is not evidence about the console (AGENTS.md rule 5): these are pictures of the app's
# drawing code and of what its Install page does to Vita3K's card, nothing more.
# env: VITA3K (dir with app/usr/bin/Vita3K), VITA3K_FIRMWARE, VITA3K_FONTS, FFMPEG, VN_ART (promo-assets' vn/),
#      FONT_DIR (with ZenMaruGothic-Bold.ttf), PY (Python with Pillow), TIMEOUT (default 900)
# (from promo-assets' src/app_capture/capture_app.sh)
set -uo pipefail
vpk=$(realpath "$1"); out=$(realpath -m "$2"); mkdir -p "$out"
here=$(cd "$(dirname "$0")" && pwd)
V=${VITA3K:?}; FF=${FFMPEG:?}
sbx=$(mktemp -d "${TMPDIR:-/tmp}/vjo-promo-app.XXXXXX")
xpid=; ffpid=
trap 'kill $ffpid $xpid 2>/dev/null; rm -rf "$sbx"' EXIT
mkdir -p "$sbx/config/Vita3K" "$sbx/data/Vita3K/Vita3K/ux0/data" "$sbx/cache"
cat > "$sbx/config/Vita3K/config.yml" <<EOF
---
initial-setup: true
backend-renderer: OpenGL
validation-layer: false
discord-rich-presence: false
show-live-area-screen: false
delay-start: 0
delay-background: 0
log-level: 2
sys-button: 1
...
EOF
v3=$sbx/data/Vita3K/Vita3K
ux0=$v3/ux0; ur0=$v3/ur0
libs="$V/app/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu"
loader=("$V/app/usr/bin/ld-linux-x86-64.so.2" --library-path "$V/compat:$libs")
disp=:$(( 140 + RANDOM % 50 ))
Xvfb "$disp" -screen 0 1280x720x24 -nolisten tcp > "$out/xvfb.log" 2>&1 & xpid=$!
sleep 2
vita3k() {
  env DISPLAY="$disp" APPDIR="$V/app" HOME="$sbx" XDG_CONFIG_HOME="$sbx/config" XDG_DATA_HOME="$sbx/data" \
    XDG_CACHE_HOME="$sbx/cache" timeout "${TIMEOUT:-900}" "${loader[@]}" "$V/app/usr/bin/Vita3K" "$@" < /dev/null
}
vita3k --firmware "$VITA3K_FIRMWARE" > "$out/firmware.stdout" 2>&1
vita3k --firmware "$VITA3K_FONTS" > "$out/fonts.stdout" 2>&1
ls "$v3"/sa0/data/font/pvf/*.pvf > /dev/null 2>&1 || { echo "fonts not installed"; exit 1; }
# the memory card: the games, HENkaku's stock taiHEN configurations, no overlay
"${PY:-python3}" "$here/make_promo_games.py" "$ux0" "${VN_ART:?}" "${FONT_DIR:?}" > "$out/games.txt" 2>&1 || { cat "$out/games.txt"; exit 1; }
mkdir -p "$ux0/tai" "$ur0/tai"
stock='# For users plugins, you must refresh taiHEN from HENkaku Settings for
# changes to take place.
# For kernel plugins, you must reboot for changes to take place.
*KERNEL
# henkaku.skprx is hard-coded to load and is not listed here
*main
# main is a special titleid for SceShell
ur0:tai/henkaku.suprx
*NPXS10015
# this is for modifying the version string
ur0:tai/henkaku.suprx
*NPXS10016
# this is for modifying the version string in settings widget
ur0:tai/henkaku.suprx'
printf '%s\n' "$stock" > "$ux0/tai/config.txt"
printf '# This file is used as an alternative if ux0:tai/config.txt is not found.\n%s\n' "$stock" > "$ur0/tai/config.txt"
record() { # record NAME: ffmpeg records the screen into OUT/NAME.mkv until stop
  "$FF" -nostdin -y -loglevel error -f x11grab -draw_mouse 0 -framerate 30 -video_size 1280x720 -i "$disp" -c:v ffv1 -level 3 \
    "$out/$1.mkv" > "$out/ffmpeg-$1.log" 2>&1 & ffpid=$!
}
stop() { sleep 1; kill -INT $ffpid; wait $ffpid 2>/dev/null; ffpid=; }
# run 1: the one-VPK installed; the Install page, Enter, the run, its result
record screen-1
t0=$(date +%s)
vita3k --frontend sdl "$vpk" > "$out/vita3k-1.stdout" 2>&1; rc=$?
stop
echo "run 1 (install): vita3k rc=$rc after $(( $(date +%s) - t0 ))s" | tee "$out/run.txt"
cp "$sbx/cache/Vita3K/vita3k.log" "$out/vita3k-1.log" 2>/dev/null || true
cp "$ux0/data/vjo_settings_smoke.txt" "$out/results-1.txt" 2>/dev/null || true
( cd "$v3" && find ux0/tai ur0/tai ux0/data ur0/data -type f -printf '%s %p\n' | sort -k2 ) > "$out/card-after-install.txt" 2>/dev/null
cp "$ux0/tai/config.txt" "$out/ux0-tai-config.txt" 2>/dev/null; cp "$ur0/tai/config.txt" "$out/ur0-tai-config.txt" 2>/dev/null
# run 2: the installed app started again: Games, OCR & lookup
record screen-2
t0=$(date +%s)
vita3k --frontend sdl -r VJOSET001 > "$out/vita3k-2.stdout" 2>&1; rc=$?
stop
echo "run 2 (installed): vita3k rc=$rc after $(( $(date +%s) - t0 ))s" | tee -a "$out/run.txt"
cp "$sbx/cache/Vita3K/vita3k.log" "$out/vita3k-2.log" 2>/dev/null || true
cp "$ux0/data/vjo_settings_smoke.txt" "$out/results-2.txt" 2>/dev/null || true
cp -r "$ux0/data/VitaJPOverlay" "$out/ux0-data-VitaJPOverlay" 2>/dev/null || true
rm -f "$out/ux0-data-VitaJPOverlay/dictionaries/plaintext.vjdict"  # (157 MB; its size is in card-after-install.txt)
