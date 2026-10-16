#!/usr/bin/env bash
# tools/build.sh [16x9|9x16|all] [--skip-render]
# Renders the promo frame by frame, mixes its soundtrack, and encodes the films, a GIF and stills
# into out/media/. Needs: npm ci, tools/assets.sh, the audio venv (.venv: tools/requirements-audio.txt),
# FFmpeg 8.1 (FFMPEG=/path/to/ffmpeg), and chrome-headless-shell 152.0.7977.30 in .cache/browsers
# (npx @puppeteer/browsers install chrome-headless-shell@152.0.7977.30 --path .cache/browsers).
# RUN="<prefix>" runs each render worker under a job slot wrapper (e.g. the fleet's heavy.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
FFMPEG=${FFMPEG:-ffmpeg}
WORKERS=${WORKERS:-4}
RUN=${RUN:-}
FPS=30
cuts=${1:-all}; [ "$cuts" = all ] && cuts="16x9 9x16"
skip=${2:-}
mkdir -p out/media out/stills

render_cut() {
  local cut=$1 n=$2 pids=()
  mkdir -p "out/$cut"
  for ((w = 0; w < WORKERS; w++)); do
    local from=$((w * n / WORKERS)) to=$(((w + 1) * n / WORKERS))
    $RUN timeout 7200 node tools/render.mjs --cut "$cut" --from "$from" --to "$to" --out "out/$cut" \
      > "out/render-$cut-$w.log" 2>&1 < /dev/null &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p"; done
  local have; have=$(ls "out/$cut" | grep -c '^f.*\.png$')
  [ "$have" -ge "$n" ] || { echo "render $cut: $have of $n frames" >&2; exit 1; }
}

# the timeline's metadata: SFX events and the caption list (cheap: one frame)
node tools/render.mjs --cut 16x9 --times 0 --meta --out out/meta > out/meta.log 2>&1 < /dev/null
N=$(grep -o '[0-9]* frames' out/meta.log | head -1 | cut -d' ' -f1)

if [ "$skip" != --skip-render ]; then
  for cut in $cuts; do render_cut "$cut" "$N"; done # one cut after the other; WORKERS renders split a cut
fi

# soundtrack: bed + SFX, then loudness to -16 LUFS (two-pass loudnorm), true peak <= -1.5 dBTP
.venv/bin/python tools/audio.py out/meta/sfx.json out/mix.wav
"$FFMPEG" -hide_banner -nostats -i out/mix.wav -af loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json -f null - 2> out/loud1.txt
m() { grep "\"$1\"" out/loud1.txt | sed 's/.*: "\(.*\)".*/\1/'; }
"$FFMPEG" -hide_banner -nostats -y -i out/mix.wav -af "loudnorm=I=-16:TP=-1.5:LRA=11:measured_I=$(m input_i):measured_TP=$(m input_tp):measured_LRA=$(m input_lra):measured_thresh=$(m input_thresh):offset=$(m target_offset):linear=true,alimiter=limit=0.70:attack=4:release=60:level=false,aresample=48000" -c:a pcm_s24le out/mix-16.wav 2> /dev/null

for cut in $cuts; do
  "$FFMPEG" -hide_banner -nostats -y -framerate $FPS -i "out/$cut/f%05d.png" -i out/mix-16.wav -map 0:v -map 1:a \
    -c:v libx264 -threads 5 -preset slow -crf 18 -pix_fmt yuv420p -profile:v high -r $FPS -g 60 \
    -c:a aac -b:a 192k -ar 48000 -movflags +faststart -shortest "out/media/promo-scratch-$cut.mp4" 2> "out/encode-$cut.log"
done

# GIF (16:9, the lookup and the mining: L + R to the second card), and stills of every line
if [ -d out/16x9 ]; then
  "$FFMPEG" -hide_banner -nostats -y -ss 4.9 -t 7.4 -framerate $FPS -i out/16x9/f%05d.png \
    -vf "fps=10,scale=560:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle" \
    out/media/promo-scratch.gif 2> out/gif.log
  for ts in 3.4 6.4 11.3 15.9 20.3 23.7 31.0 37.9 42.8; do
    f=$(printf 'f%05d.png' "$(awk -v t="$ts" 'BEGIN{printf "%d", t*30}')")
    "$FFMPEG" -hide_banner -loglevel error -y -i "out/16x9/$f" -q:v 3 "out/stills/16x9-$ts.jpg"
    [ -f "out/9x16/$f" ] && "$FFMPEG" -hide_banner -loglevel error -y -i "out/9x16/$f" -q:v 3 "out/stills/9x16-$ts.jpg"
  done
fi
ls -la out/media
for cut in $cuts; do "$FFMPEG" -hide_banner -nostats -i "out/media/promo-scratch-$cut.mp4" -af ebur128=peak=true -f null - 2>&1 | grep -E "^\s+(I|Peak):" | tr -s ' ' | sed "s/^/$cut /"; done
