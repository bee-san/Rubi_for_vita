#!/usr/bin/env bash
# Makes the README media from the rendered films (run after `npm run render` and
# `npm run render:vertical`): the MP4s in media/, the poster, the README loop and
# one still per scene. Needs ffmpeg and npx hyperframes (for the stills).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p media/stills
cp renders/promo-1920x1080.mp4 renders/promo-1080x1920.mp4 media/
# the poster: the overlay showing the first lookup (scene 2)
ffmpeg -hide_banner -loglevel error -y -ss 6.2 -i media/promo-1920x1080.mp4 -frames:v 1 -q:v 3 media/poster.jpg
# the README loop: scene 2 (the lookup), 720 px wide, 12 fps, one 128-colour palette
ffmpeg -hide_banner -loglevel error -y -ss 4.2 -t 4.6 -i media/promo-1920x1080.mp4 \
  -vf "fps=12,scale=720:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle" \
  -loop 0 media/promo.gif
# one still per scene, from the landscape film
i=0
# scenes 1, 2, 3, 4, 4b, 5, 6, 7, 8 (the app, then back in the game) and the end card
for t in 2.8 7.4 11.8 15.4 19.4 22.4 28.3 31.9 37.9 39.9 43.2; do
  i=$((i + 1))
  ffmpeg -hide_banner -loglevel error -y -ss "$t" -i media/promo-1920x1080.mp4 -frames:v 1 -vf scale=1280:-1 -q:v 4 \
    "media/stills/$(printf '%02d' "$i")-$t.jpg"
done
ls -la media media/stills
