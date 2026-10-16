#!/usr/bin/env bash
# tools/draft-render.sh: a quick draft for review: both cuts at half size and half rate (960x540 and
# 540x960, 15 fps: an eighth of the work), one render at a time. RUN="<prefix>" wraps each render
# (e.g. a job-slot wrapper). Output: out-draft/media and out-draft/stills.
set -euo pipefail
cd "$(dirname "$0")/.."
FFMPEG=${FFMPEG:-ffmpeg}; RUN=${RUN:-}; FPS=15
O=out-draft; mkdir -p "$O/media" "$O/stills"
node tools/render.mjs --cut 16x9 --times 0 --meta --scale 0.5 --out "$O/meta" > "$O/meta.log" 2>&1 < /dev/null
for cut in 16x9 9x16; do
  [ -n "${SKIP_RENDER-}" ] && continue
  $RUN timeout 7200 node tools/render.mjs --cut "$cut" --fps $FPS --scale 0.5 --out "$O/$cut" > "$O/render-$cut.log" 2>&1 < /dev/null
done
.venv/bin/python tools/audio.py "$O/meta/sfx.json" "$O/mix.wav"
"$FFMPEG" -hide_banner -nostats -i "$O/mix.wav" -af loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json -f null - 2> "$O/loud1.txt"
m() { grep "\"$1\"" "$O/loud1.txt" | sed 's/.*: "\(.*\)".*/\1/'; }
"$FFMPEG" -hide_banner -nostats -y -i "$O/mix.wav" -af "loudnorm=I=-16:TP=-1.5:LRA=11:measured_I=$(m input_i):measured_TP=$(m input_tp):measured_LRA=$(m input_lra):measured_thresh=$(m input_thresh):offset=$(m target_offset):linear=true,alimiter=limit=0.70:attack=4:release=60:level=false,aresample=48000" -c:a pcm_s24le "$O/mix-16.wav" 2> /dev/null
for cut in 16x9 9x16; do
  "$FFMPEG" -hide_banner -nostats -y -framerate $FPS -i "$O/$cut/f%05d.png" -i "$O/mix-16.wav" -map 0:v -map 1:a \
    -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -r $FPS -g 30 -c:a aac -b:a 192k -ar 48000 -movflags +faststart -shortest \
    "$O/media/promo-scratch-$cut.mp4" 2> "$O/encode-$cut.log"
done
"$FFMPEG" -hide_banner -nostats -y -ss 4.9 -t 7.4 -framerate $FPS -i "$O/16x9/f%05d.png" \
  -vf "fps=10,scale=600:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle" \
  "$O/media/promo-scratch.gif" 2> "$O/gif.log"
for ts in 1.9 6.4 11.6 15.8 19.6 25.3 31.6 38.3 42.8; do
  f=$(printf 'f%05d.png' "$(awk -v t="$ts" -v r=$FPS 'BEGIN{printf "%d", t*r}')")
  for cut in 16x9 9x16; do "$FFMPEG" -hide_banner -loglevel error -y -i "$O/$cut/$f" -q:v 3 "$O/stills/$cut-$ts.jpg"; done
done
ls -la "$O/media"
for cut in 16x9 9x16; do "$FFMPEG" -hide_banner -nostats -i "$O/media/promo-scratch-$cut.mp4" -af ebur128=peak=true -f null - 2>&1 | grep -E "^\s+(I|Peak):" | tr -s ' ' | sed "s/^/$cut /"; done
