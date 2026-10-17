#!/usr/bin/env python3
"""Copy the promo's art and captures into docs/promo/assets (and the overlay's states into src/).

    python3 tools/prep_assets.py CAPTURE_DIR CHECKOUT PROMO_ASSETS

CAPTURE_DIR is capture/run.sh's output for CHECKOUT, the vita-jp-overlay checkout the film shows (its commit is
CAPTURE_DIR/commit.txt); PROMO_ASSETS (or $PROMO_ASSETS) is promo-assets' folder: the original mock game's art and
the sound, made for the promo by the fleet's promo-assets agent (its MANIFEST.md has each file's licence, source and
sha256; its src/ the tools that made them). Needs Pillow, numpy and ffmpeg (FFMPEG).
Writes:
- src/overlay-states.json: the overlay at each beat (capture/overlay_states.py);
- assets/shots/: the Overlay app in Vita3K (capture/capture_app.sh): its Install page, the run and its result,
  Games and OCR & lookup, each checked against settings/main.c's accent colour;
- assets/anki/: the mined card's front and back as Anki renders it; assets/vn/shot-s1-m2.jpg: its screenshot,
  the game worker's JPEG (capture/run.sh);
- assets/app/: the app's icon and LiveArea art, the checkout's settings/assets/;
- assets/vn/, assets/audio/, assets/icons/: the mock game, the sound and the home screen's icons (promo-assets);
  assets/audio/bed-45s.m4a: its 44 s music bed with one second more in the breakdown (tools/make_bed.py);
- src/release.json: the release's file names and sizes (capture/run.sh release/sizes.json);
- assets/SOURCES.md: each output with the sha256 of its input and of itself.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CAP = Path(sys.argv[1]).resolve()
CHECKOUT = Path(sys.argv[2]).resolve()
if len(sys.argv) < 3 or (len(sys.argv) < 4 and not os.environ.get("PROMO_ASSETS")):
    sys.exit(__doc__)
SRC = Path(sys.argv[3] if len(sys.argv) > 3 else os.environ["PROMO_ASSETS"])

# The app's shots, by capture run and hold (capture/split_holds.py) or the Install page's run frame
# (CAPTURE_DIR/app/run/progress/f-NNN.png), as the run's contact sheets show them (holds-*/contact.png):
# the capture script's beats (capture/app_capture_patch.py) are one hold each. Checked by eye for each capture.
APP_SHOTS = {
    "install-1-plan": ("holds-1", 4),          # Install the overlay (v0.8.0): the plan, [Install]
    "install-2-checking": ("progress", 10),    # Installing...: Checking this app's plaintext.vjdict, 33.8 of 157.3 MB
    "install-3-checking": ("progress", 27),    # ... 115.1 of 157.3 MB
    "install-4-copying": ("progress", 37),     # Copying VOCR4_W20_w8.vocr
    "install-5-copying": ("progress", 40),     # Copying vocr_lm_o3_4000k.vlm
    "install-6-done": ("holds-1", 5),          # The overlay is installed: Installed ... Restart the Vita now
    "games-1-top": ("holds-2", 3),             # Games, as the app opens once it is installed
    "games-2-down": ("holds-2", 4),            # Down: Courier Run
    "games-3-down": ("holds-2", 5),            # Down: Ember Garden
    "games-4-off": ("holds-2", 6),             # Enter: Ember Garden OFF, "Saved. Resume or restart your game to apply."
    "read-1-auto": ("holds-2", 17),            # OCR & lookup scrolled to Read text: Automatically (when it changes)
    "read-2-on-press": ("holds-2", 18),        # Right: Only when I press L + R, "Saved. Resume your game, then reopen ..."
}

rows = []


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def rel(p):
    p = Path(p).resolve()
    for base, name in ((CAP, "capture"), (CHECKOUT, "checkout"), (SRC.resolve(), "promo-assets")):
        if p.is_relative_to(base):
            return f"{name}: {p.relative_to(base)}"
    return str(p)


def note(src, out, how):
    rows.append((str(out.relative_to(ROOT)), rel(src), sha(src), sha(out), how))


def jpeg(src, out, q=90):
    Image.open(src).convert("RGB").save(out, quality=q, optimize=True, progressive=False, subsampling=0)
    note(src, out, f"JPEG q{q} 4:4:4")


def copy(src, out, how="copy"):
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, out)
    note(src, out, how)


ACCENT = (91, 220, 207)  # settings/main.c ACCENT: the 4 px bar along the top of every page


def app_shot(src, out):
    """A Vita3K capture of the Overlay app, checked against settings/main.c's colours (the Xvfb path swaps red and
    blue; capture/split_holds.py swaps them back)."""
    im = Image.open(src).convert("RGB")
    if im.size != (960, 544):
        sys.exit(f"{src}: {im.size}, not the Vita's 960 x 544")
    got = im.getpixel((480, 1))
    if any(abs(a - b) > 3 for a, b in zip(got, ACCENT)):
        sys.exit(f"{src}: top bar {got}, expected settings/main.c's ACCENT {ACCENT}")
    im.save(out, optimize=True)
    note(src, out, "PNG (colours checked against settings/main.c)")


def main():
    commit = (CAP / "commit.txt").read_text().strip()
    head = subprocess.run(["git", "-C", str(CHECKOUT), "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    if head != commit:
        sys.exit(f"the capture is of {commit[:7]}, the checkout is at {head[:7]}")
    for d in ("vn", "shots", "anki", "app", "audio", "icons"):
        (ROOT / "assets" / d).mkdir(parents=True, exist_ok=True)
    for old in (ROOT / "assets" / "shots").glob("*.png"):
        old.unlink()
    for old in (ROOT / "assets" / "anki").glob("*.png"):
        old.unlink()
    # the overlay's states (the checkout's own render order, strings and lookups)
    copy(CAP / "overlay-states.json", ROOT / "src" / "overlay-states.json")
    # the mock game (original, CC0: promo-assets) and its heroine
    vn = ROOT / "assets" / "vn"
    for s in ("s1", "s2", "s3"):
        jpeg(SRC / "vn" / s / "bg@2x.png", vn / f"bg-{s}.jpg")
    for e in ("smile", "surprised", "happy"):
        copy(SRC / "vn" / "sprite" / f"hinata-{e}.png", vn / f"hinata-{e}.png")
    # the card's screenshot: the game worker's JPEG of the game's frame when × is pressed (the overlay is not in it)
    copy(CAP / "anki" / "shot-s1-m2-q70.jpg", vn / "shot-s1-m2.jpg", "copy (core/jpegsw.c at quality 70, host vjo-jpeg)")
    # the card as Anki renders it (Lapis), front and back
    anki = ROOT / "assets" / "anki"
    copy(CAP / "anki" / "png" / "card-1-front-desktop-light.png", anki / "card-1-front.png", "copy (Anki 26.09.3, Lapis 1.7.0)")
    copy(CAP / "anki" / "png" / "card-1-back-desktop-light.png", anki / "card-1-back.png", "copy (Anki 26.09.3, Lapis 1.7.0)")
    # the app in Vita3K
    run = CAP / "app" / "run"
    for name, (where, k) in APP_SHOTS.items():
        src = run / where / (f"f-{k:03d}.png" if where == "progress" else f"hold-{k:02d}.png")
        app_shot(src, ROOT / "assets" / "shots" / f"{name}.png")
    # the app's icon and LiveArea art (the bubble, the LiveArea page, the end card)
    for n in ("icon0.png", "bg.png", "startup.png"):
        copy(CHECKOUT / "settings" / "assets" / n, ROOT / "assets" / "app" / n)
    # the music and the sound effects (CC0, promo-assets: audio/README.md)
    audio = ROOT / "assets" / "audio"
    (audio / "bed-44s.m4a").unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "tools" / "make_bed.py"), str(SRC / "audio" / "music" / "bed-44s.wav"),
                    str(audio / "bed-45s.m4a")], check=True)
    note(SRC / "audio" / "music" / "bed-44s.wav", audio / "bed-45s.m4a", "tools/make_bed.py: 1.0 s more breakdown at 29.5 s, AAC")
    for n in ("click", "lr", "dpad", "whoosh-in", "whoosh-out", "swish", "pop", "chime", "confirm",
              "text-blip-1", "text-blip-2", "text-blip-3", "text-blip-4", "text-blip-5"):
        copy(SRC / "audio" / "sfx" / f"{n}.wav", audio / f"sfx-{n}.wav")
    # the home screen's icons: the icon0.png files the app capture installed (capture/make_promo_games.py)
    for p in sorted((SRC / "app" / "icons").glob("PROMO000[1-5]-*.png")):
        copy(p, ROOT / "assets" / "icons" / (p.name[:9] + ".png"))
    # the release's names and sizes (the Releases page, VitaShell's list)
    sizes = json.loads((CAP / "release" / "sizes.json").read_text())
    (ROOT / "src" / "release.json").write_text(json.dumps({"commit": commit, "files": sizes}, indent=1) + "\n")
    note(CAP / "release" / "sizes.json", ROOT / "src" / "release.json", "the sizes of capture/run.sh's package_variants.py release")
    # drawn for the film and committed as they are (not made by this script)
    for path, what in (("assets/device/vita-white.svg", "the console: an original drawing from its published "
                        "dimensions, no photo traced, no logos (#134's; its power button added for the final cut); CC0-1.0"),
                       ("assets/icons/vitashell.svg", "VitaShell's bubble on the home screen: a generic file-manager icon, "
                        "original, drawn for promo v2; GPL-3.0-only")):
        out = ROOT / path
        rows.append((path, "committed", sha(out), sha(out), what))
    lines = [
        "# Where the composition's art comes from",
        "",
        f"Written by `tools/prep_assets.py` from `capture/run.sh`'s output for vita-jp-overlay `{commit[:7]}` (the real",
        "captures: the overlay's states, the card, the app in Vita3K, the release's sizes), that checkout's",
        "`settings/assets/`, and promo-assets' shared folder (the original mock game, the sound; its MANIFEST.md has each",
        "file's licence and source).",
        "",
        "| output | input | input sha256 | output sha256 | how |",
        "| --- | --- | --- | --- | --- |",
    ]
    lines += [f"| `{o}` | `{i}` | `{a[:16]}…` | `{b[:16]}…` | {h} |" for o, i, a, b, h in rows]
    (ROOT / "assets" / "SOURCES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(rows)} files from {commit[:7]}")


if __name__ == "__main__":
    main()
