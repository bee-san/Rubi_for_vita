#!/usr/bin/env bash
# Makes the films' media from the renders (run after `npm run render` and `npm run render:vertical`):
#   media/promo-1920x1080.mp4, media/promo-1080x1920.mp4 (45 s): the renders with their sound brought to
#     -16 LUFS integrated, true peak -1.5 dBTP (ffmpeg loudnorm, two passes; the picture is copied);
#   media/promo.gif: the L + R lookup and the mining, 540 px wide, 10 fps (at most 5 MB);
#   media/poster.jpg and media/stills/: one still per scene, from the landscape film.
# Needs ffmpeg and ffprobe (FFMPEG / FFPROBE override them) and python3.
set -euo pipefail
cd "$(dirname "$0")/.."
FFMPEG=${FFMPEG:-ffmpeg}
mkdir -p media/stills
norm() { # in out
  local stats
  stats=$("$FFMPEG" -hide_banner -nostats -i "$1" -af loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json -f null - 2>&1 |
    python3 -c 'import sys,json,re; t=sys.stdin.read(); print(json.dumps(json.loads(re.findall(r"\{[^{}]*\}", t)[-1])))')
  local f
  f=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print("measured_I=%s:measured_TP=%s:measured_LRA=%s:measured_thresh=%s:offset=%s" % (d["input_i"], d["input_tp"], d["input_lra"], d["input_thresh"], d["target_offset"]))' "$stats")
  # the sound ends with the picture (the encoder's last frame would run 0.1 s past it)
  local dur
  dur=$("${FFPROBE:-ffprobe}" -v error -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$1")
  "$FFMPEG" -hide_banner -loglevel error -y -i "$1" -c:v copy \
    -af "loudnorm=I=-16:TP=-1.5:LRA=11:$f:linear=true,aresample=48000,atrim=end=$dur" -c:a aac -b:a 192k \
    -movflags +faststart "$2"
}
norm renders/promo-1920x1080.mp4 media/promo-1920x1080.mp4
norm renders/promo-1080x1920.mp4 media/promo-1080x1920.mp4
# the poster: the overlay on 一緒に (scene 2)
"$FFMPEG" -hide_banner -loglevel error -y -ss 7.8 -i media/promo-1920x1080.mp4 -frames:v 1 -q:v 3 media/poster.jpg
# the README loop: L + R, the lookup, the mining (4.6-12.1 s), one 128-colour palette
"$FFMPEG" -hide_banner -loglevel error -y -ss 4.6 -t 7.5 -i media/promo-1920x1080.mp4 \
  -vf "fps=10,scale=540:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle" \
  -loop 0 media/promo.gif
size=$(stat -c %s media/promo.gif)
if [ "$size" -gt 5000000 ]; then echo "media/promo.gif is $size B, over 5 MB" >&2; exit 1; fi
# one still per beat: 1 2 3 4, 5 (and its setting), 6 (GitHub, VitaShell, its LiveArea, the Install page, the result,
# the restart), 7 (the tap on 子) 8, the end card
i=0
for t in 2.6 7.8 11.6 16.6 20.8 22.6 24.0 25.95 27.95 29.5 30.7 31.0 36.6 42.0 44.0; do
  i=$((i + 1))
  "$FFMPEG" -hide_banner -loglevel error -y -ss "$t" -i media/promo-1920x1080.mp4 -frames:v 1 -vf scale=1280:-1 -q:v 4 \
    "media/stills/$(printf '%02d' "$i")-$t.jpg"
done
ls -la media media/stills
