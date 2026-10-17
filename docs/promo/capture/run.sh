#!/usr/bin/env bash
# capture/run.sh CHECKOUT OUT: every real picture and string the promo shows, made from one vita-jp-overlay checkout.
#
# CHECKOUT is built first, as the README's "Build and validation" says: the host tree in build/host and the Vita
# release in build/vita-normal and build/vita-debug (cmake --build ... --target release, VJO_DEBUG=OFF and ON).
# OUT gets (tools/prep_assets.py copies them into assets/ and src/):
#   overlay-states.json      the overlay at each beat: the checkout's lookups and render() (overlay_lookups.c, overlay_states.py)
#   ocr-check.md             the checkout's recognizer reads each line the film shows, as it shows it (ocr_check.py)
#   anki/                    the mined cards: the game worker's JPEG of the game's frame (vjo-jpeg), the Shell's records
#                            and the app's export (anki_card.c), imported and rendered by Anki 26.09.3 (anki_cards.py,
#                            render_cards.js)
#   release/                 the release as package_variants.py makes it (both zips and VPKs, SHA256SUMS): the names
#                            and sizes on the Releases page and in VitaShell (sizes.json)
#   app/                     the Overlay app in Vita3K: the one-VPK install of a capture build of the app (its own code,
#                            app_capture_patch.py's pad scripts), the Install page, its run, Games, OCR & lookup
#                            (capture_app.sh, split_holds.py; holds-1/, holds-2/, progress/)
# env: A (promo-assets' shared folder: vn/ art, fonts/), MODELS, DICT_DIR (the released plaintext.vjdict), VITASDK,
#      VJO_VOCR_SOURCE_DIR, PYB (the Vita build's Python), VITA3K, VITA3K_FIRMWARE, VITA3K_FONTS, FFMPEG, ANKI_ENV
#      (a directory with venv/: anki 26.9.3, tools/requirements-anki.txt, made in ubuntu:24.04), NODE_PATH (this
#      folder's node_modules: puppeteer-core), VERSION (default 0.8.0). Needs docker (Anki needs glibc 2.35+), Xvfb.
set -euo pipefail
M=$(realpath "$1"); O=$(realpath -m "$2"); here=$(cd "$(dirname "$0")" && pwd)
V=${VERSION:-0.8.0}
mkdir -p "$O/bin"
git -C "$M" rev-parse HEAD > "$O/commit.txt"
git -C "$M" status --porcelain --untracked-files=no > "$O/dirty.txt"
[ -s "$O/dirty.txt" ] && { echo "run.sh: $M has local changes"; exit 1; }
dict_sha=$(sha256sum "$DICT_DIR/plaintext.vjdict" | cut -d' ' -f1)
want=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["key_filter"]["sha256"])' "$M/docs/plaintext-dictionaries.json")
[ "$dict_sha" = "$want" ] || { echo "run.sh: $DICT_DIR/plaintext.vjdict is not the checkout's released dictionary"; exit 1; }
HOST="$M/build/host"
INC=(-I "$M/core" -I "$M/host" -I "$M/include")
LIBS=("$M/core/textnorm.c" "$HOST/libvjo_host.a" "$HOST/libvjo_core.a" "$HOST/libbearssl.a" -lm)

echo "== the overlay"
stick=$(grep -q 'int japanese_only, int stick, size_t \*out' "$M/core/text_rows.h" && echo -DVJO_ROW_KEY_STICK=1 || true)
gcc -O1 -DVJO_HOST=1 $stick "${INC[@]}" -I "$M/host/tests" "$here/overlay_lookups.c" "${LIBS[@]}" -o "$O/bin/overlay_lookups"
printf '%s\n' '「ねえ、聞いて！明日の夜、丘の上で\n流れ星が見えるんだって！」' '「また明日、一緒に見に行こう！」' \
  '「ドキドキ、ワクワク……！」' '「キラキラ……。\n子供の頃みたい。」' > "$O/lines.txt"
"$O/bin/overlay_lookups" "$DICT_DIR" plaintext.vjdict < "$O/lines.txt" > "$O/lookups.jsonl"
python3 "$here/overlay_states.py" "$M" "$O/lookups.jsonl" "$dict_sha" > "$O/overlay-states.json"
# the header is the recognizer's text: it must read each line as the film shows it
python3 "$here/ocr_check.py" "$M" "$MODELS" "$A/vn" "$O/ocr-check.md"

echo "== the card"
mkdir -p "$O/anki" "$O/bin/anki"
sed 's/^int main(int argc, char \*\*argv)$/int test_apkg_main(int argc, char **argv)/' "$M/host/tests/test_apkg.c" > "$O/bin/anki/test_apkg_lib.c"
gcc -O2 -std=gnu99 -DVJO_DEBUG=1 -DVJO_HOST=1 -DVJO_WITH_VOCR=1 -Wno-unused-parameter -I "$O/bin/anki" -I "$M/host/tests" \
  -I "$M/third_party/acutest/include" -I "$M/host/tests/vita_stubs" "${INC[@]}" -I "$M/third_party/bearssl/inc" \
  -I "$M/third_party/taihen-parser/include" "$here/anki_card.c" "${LIBS[@]}" -o "$O/bin/anki_card"
python3 -c 'import sys; from PIL import Image; open(sys.argv[2], "wb").write(Image.open(sys.argv[1]).convert("RGBA").tobytes())' \
  "$A/vn/s1/s1-m2-frame.png" "$O/anki/frame-s1-m2.rgba"
"$HOST/vjo-jpeg" "$O/anki/frame-s1-m2.rgba" 960 544 70 "$O/anki/shot-s1-m2-q70.jpg" 450560  # game_ocr/main.c SHOT_QUALITY, SHOT_BLOCK_BYTES(960)
rm -rf "$O/anki/work" "$O/anki/cards" "$O/anki/png"; mkdir -p "$O/anki/work"
"$O/bin/anki_card" "$DICT_DIR" plaintext.vjdict "$O/anki/work" "$O/anki/shot-s1-m2-q70.jpg" \
  "「また明日、一緒に見に行こう！」" 6 "「また明日、一緒に見に行こう！」" 9 > "$O/anki/export-result.json"
pkg=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$O/anki/export-result.json")
cp "$O/anki/work/ux0:data/VitaJPOverlay/anki/$pkg" "$O/anki/"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ANKI_ENV:/env:ro" -v "$O/anki:/work" -v "$here:/cap:ro" ubuntu:24.04 \
  /env/venv/bin/python /cap/anki_cards.py "/work/$pkg" /work/cards
FONTS=$A/fonts node "$here/render_cards.js" "$O/anki/cards" "$O/anki/png"

echo "== the release"
rm -rf "$O/release"
base_n=$(ls "$M"/build/vita-normal/*-"$V".zip); base_d=$(ls "$M"/build/vita-debug/*-"$V"-debug.zip 2>/dev/null || ls "$M"/build/vita-debug/*-"$V".zip)
"$PYB" "$M/tools/package_variants.py" --normal "$base_n" --debug "$base_d" --models "$MODELS" --version "$V" --sources "$M" -o "$O/release" > "$O/release.log"
python3 - "$O/release" > "$O/release/sizes.json" <<'EOF'
import json, sys
from pathlib import Path
def github(n):  # the Releases page: 1024-based, MB with one decimal, KB whole, else bytes
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{round(n / 1024)} KB" if n >= 1024 else f"{n} Bytes"
def vitashell(n):  # VitaShell's getSizeString (utils.c): 1024-based, two decimals past bytes
    units, d, i = ["B", "KB", "MB", "GB"], float(n), 0
    while d >= 1024.0:
        d /= 1024.0; i += 1
    return f"{d:.{0 if i == 0 else 2}f} {units[i]}"
files = sorted(p for p in Path(sys.argv[1]).iterdir() if p.is_file() and p.suffix in (".vpk", ".zip") or p.name == "SHA256SUMS")
print(json.dumps({p.name: {"bytes": p.stat().st_size, "github": github(p.stat().st_size), "vitashell": vitashell(p.stat().st_size)}
                  for p in files}, indent=1))
EOF

echo "== the app in Vita3K"
W="$O/bin/cap-src"
rm -rf "$W"; git -C "$M" worktree remove --force "$W" 2>/dev/null || true
git -C "$M" worktree add -q --detach "$W" HEAD
python3 "$here/app_capture_patch.py" "$W"
( export PATH="$(dirname "$PYB"):$PATH"
  cmake -S "$W/vita" -B "$W/build/cap" -DCMAKE_BUILD_TYPE=Release -DVJO_DEBUG=OFF -DVJO_SETTINGS_SMOKE=ON \
    -DVJO_SETTINGS_SMOKE_PAD=0 -DVJO_VOCR_SOURCE_DIR="$VJO_VOCR_SOURCE_DIR" -DPython3_EXECUTABLE="$PYB" > "$O/cap-configure.log" 2>&1
  cmake --build "$W/build/cap" --target vjo_settings_smoke.vpk-vpk -j5 > "$O/cap-build.log" 2>&1 || {
    grep -q "segment 1 overlaps" "$O/cap-build.log" && cmake -S "$W/vita" -B "$W/build/cap" -DVJO_SETTINGS_SMOKE_PAD=4096 > /dev/null &&
      cmake --build "$W/build/cap" --target vjo_settings_smoke.vpk-vpk -j5 > "$O/cap-build.log" 2>&1; } )
mkdir -p "$O/app/pkg"
python3 "$here/capture_package.py" "$M" "$base_n" "$W/build/cap/vjo_settings_smoke.self" "$O/app/pkg/base-capture.zip"
"$PYB" "$M/tools/package_release.py" "$O/app/pkg/base-capture.zip" -o "$O/app/pkg/capture-$V.zip" --models "$MODELS" \
  --version "$V" --variant normal > "$O/app/pkg/package_release.log"
vpk_name=$(python3 -c 'import json,sys; print(next(k for k in json.load(open(sys.argv[1])) if k.endswith(".vpk") and "-debug" not in k))' "$O/release/sizes.json")
"$PYB" "$M/tools/package_vpk.py" "$O/app/pkg/capture-$V.zip" -o "$O/app/pkg/$vpk_name" > "$O/app/pkg/package_vpk.log"
rm -rf "$O/app/run"
VN_ART=$A/vn FONT_DIR=$A/fonts PY=$(command -v python3) bash "$here/capture_app.sh" "$O/app/pkg/$vpk_name" "$O/app/run"
( cd "$O/app/run" && python3 "$here/split_holds.py" screen-1.mkv holds-1 && python3 "$here/split_holds.py" screen-2.mkv holds-2 )
# the Install page's run, frame by frame (it changes every frame, so it has no holds): from the plan's last frame
# to the result's first
python3 - "$O/app/run" "${FFMPEG:-ffmpeg}" <<'EOF'
import json, subprocess, sys
from pathlib import Path
run, ff = Path(sys.argv[1]), sys.argv[2]
holds = json.loads((run / "holds-1" / "holds.json").read_text())
a, b = holds[-3]["last_frame"], holds[-2]["first_frame"]  # the plan's hold, then the result's (the last is the exit)
out = run / "progress"; out.mkdir(exist_ok=True)
subprocess.run([ff, "-nostdin", "-v", "error", "-y", "-i", str(run / "screen-1.mkv"), "-vf",
                f"crop=960:544:160:88,colorchannelmixer=rr=0:rb=1:br=1:bb=0,select='between(n\\,{a}\\,{b})'",
                "-vsync", "0", str(out / "f-%03d.png")], check=True)
print(f"progress: frames {a}..{b}")
EOF
git -C "$M" worktree remove --force "$W"
echo "run.sh: done ($(cat "$O/commit.txt"))"
