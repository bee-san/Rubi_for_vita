#!/usr/bin/env bash
# tools/assets.sh [--lock]
# Copies the promo's inputs into assets/ (git-ignored) and checks every file against assets.lock
# (path and SHA-256). --lock rewrites assets.lock from the files as copied.
#
# Inputs:
#   - the shared promo assets (promo-assets' MANIFEST.md has each file's licence, source and SHA-256):
#     PROMO_ASSETS, default the fleet's shared folder;
#   - this repository's own Overlay app art (settings/assets/).
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${PROMO_ASSETS:-/local/home/skerraut/herd-vita-ocr/shared/artifacts/promo-v2/assets}
REPO=$(git rev-parse --show-toplevel)
OUT=assets
mkdir -p "$OUT"

shared=(
  vn/s1/s1-m1-notext@2x.png vn/s1/s1-m1-text@2x.png vn/s1/s1-m1-text.json
  vn/s1/s1-m2-notext@2x.png vn/s1/s1-m2-text@2x.png vn/s1/s1-m2-text.json vn/s1/s1-m2-frame@2x.png
  vn/s2/s2-m1-notext@2x.png vn/s2/s2-m1-text@2x.png vn/s2/s2-m1-text.json
  vn/s3/s3-m1-notext@2x.png vn/s3/s3-m1-text@2x.png vn/s3/s3-m1-text.json
  vn/s3/s3-m2-notext@2x.png vn/s3/s3-m2-text@2x.png vn/s3/s3-m2-text.json
  overlay/s1/s1-m2-ov01-open@2x.png overlay/s1/s1-m2-ov02-right-inflight@2x.png
  overlay/s1/s1-m2-ov03-issho@2x.png overlay/s1/s1-m2-ov04-issho-saving@2x.png
  overlay/s1/s1-m2-ov05-issho-saved@2x.png overlay/s1/s1-m2-ov06-miniiku@2x.png
  overlay/s1/s1-m2-ov07-miniiku-saved@2x.png
  overlay/s3/s3-m1-ov03-open@2x.png overlay/s3/s3-m1-ov04-kodomonokoro@2x.png overlay/s3/s3-m1-ov05-mitai@2x.png
  anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-1-front-desktop-light-MOCKUP.png
  anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-1-back-desktop-light-MOCKUP.png
  anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-2-front-desktop-light-MOCKUP.png
  app/pr146-ocr-mode-21488a7-NOT-MERGED/png/PR146-10-read-text-at-top-automatically.png
  app/pr146-ocr-mode-21488a7-NOT-MERGED/png/PR146-11-read-text-at-top-only-when-I-press-LR.png
  app/install-MOCKUP-one-vpk/png/MOCKUP-01-install-plan.png app/install-MOCKUP-one-vpk/png/MOCKUP-02-installing-plugins-and-models.png
  app/install-MOCKUP-one-vpk/png/MOCKUP-03-installing-checking-the-dictionary-56MB.png
  app/install-MOCKUP-one-vpk/png/MOCKUP-04-installing-checking-the-dictionary-121MB.png
  app/install-MOCKUP-one-vpk/png/MOCKUP-05-installing-config-txt-lines.png app/install-MOCKUP-one-vpk/png/MOCKUP-06-installed-restart-the-vita-now.png
  app/install-MOCKUP-one-vpk/README-MOCKUP.md
  app/main-1ed95ca/png/01-games-top.png app/main-1ed95ca/png/04-games-neon-drift-selected.png
  app/main-1ed95ca/png/05-games-neon-drift-OFF-saved.png
  app/main-1ed95ca/png/02-games-scroll-1.png app/main-1ed95ca/png/03-games-scroll-2.png
  app/icons/PROMO0001-yuunagi-step.png app/icons/PROMO0002-neon-drift.png app/icons/PROMO0003-courier-run.png
  app/icons/PROMO0004-ember-garden.png app/icons/PROMO0005-hoshizora-diary.png app/icons/PROMO0006-paper-lantern.png
  app/icons/PROMO0008-sakura-no-kioku.png app/icons/VJOSET001-overlay.png
  audio/music/bed-44s.wav
  fonts/NotoSansJP[wght].ttf fonts/OFL-NotoSansJP.txt
)
# optional inputs: used when present (the mock-ups and audio that land later)
optional=(
  audio/sfx/click.wav audio/sfx/shoulder.wav audio/sfx/lr.wav audio/sfx/dpad.wav audio/sfx/whoosh-in.wav
  audio/sfx/whoosh-out.wav audio/sfx/swish.wav audio/sfx/pop.wav audio/sfx/chime.wav audio/sfx/confirm.wav
  audio/sfx/notify.wav audio/sfx/text-blip-1.wav audio/sfx/text-blip-2.wav audio/sfx/text-blip-3.wav
  audio/sfx/text-blip-4.wav audio/sfx/text-blip-5.wav
)

copy() { mkdir -p "$OUT/shared/$(dirname "$1")"; cp -p "$SRC/$1" "$OUT/shared/$1"; }
for f in "${shared[@]}"; do copy "$f"; done
for f in "${optional[@]}"; do [ -f "$SRC/$f" ] && copy "$f" || echo "optional input missing: $f" >&2; done

mkdir -p "$OUT/repo"
for f in icon0.png bg.png startup.png; do cp -p "$REPO/settings/assets/$f" "$OUT/repo/$f"; done

# VitaShell 2.02's Default theme (GPL-3.0, TheOfficialFloW/VitaShell tag v2.02): the file browser's icons
mkdir -p "$OUT/vitashell"
VS=${VITASHELL_SRC:-/local/home/skerraut/herd-synth3/clones/promo-video-scratch/vitashell-2.02}
for f in folder_icon.png archive_icon.png battery.png ftp.png; do
  if [ -f "$VS/$f" ]; then cp -p "$VS/$f" "$OUT/vitashell/$f"
  else curl -fsSL "https://raw.githubusercontent.com/TheOfficialFloW/VitaShell/v2.02/resources/default/$f" -o "$OUT/vitashell/$f"; fi
done

if [ "${1-}" = "--lock" ]; then
  (cd "$OUT" && find . -type f | LC_ALL=C sort | xargs sha256sum) > assets.lock
  echo "assets.lock: $(wc -l < assets.lock) files"
else
  (cd "$OUT" && sha256sum --quiet -c ../assets.lock) && echo "assets: all files match assets.lock"
fi
