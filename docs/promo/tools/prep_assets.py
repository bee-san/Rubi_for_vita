#!/usr/bin/env python3
"""Copy the promo's shared art into docs/promo/assets, in the forms the composition loads.

    python3 tools/prep_assets.py [SHARED_ASSETS_DIR]

SHARED_ASSETS_DIR is promo-assets' folder (MANIFEST.md lists every file's licence, source and
sha256; default: the fleet's shared path). Needs Pillow. Writes:

- assets/vn/: the mock visual novel 夕凪ステップ (original, CC0): the three backgrounds as JPEG
  (from bg@2x.png, 1920 x 1088) and the heroine's three expressions (vn/sprite/*.png);
- assets/shots/: the Overlay app as main draws it, captured in Vita3K (main 1ed95ca), each
  checked against settings/main.c's accent colour;
- assets/SOURCES.md: each output with the sha256 of its input and of itself.
"""
import hashlib
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/local/home/skerraut/herd-vita-ocr/shared/artifacts/promo-v2/assets")

rows = []


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def note(src, out, how):
    rows.append((str(out.relative_to(ROOT)), str(src.relative_to(SRC)), sha(src), sha(out), how))


def jpeg(src, out, q=90):
    Image.open(src).convert("RGB").save(out, quality=q, optimize=True, progressive=False, subsampling=0)
    note(src, out, f"JPEG q{q} 4:4:4")


def copy(src, out):
    out.write_bytes(src.read_bytes())
    note(src, out, "copy")


ACCENT = (91, 220, 207)  # settings/main.c ACCENT: the 4 px bar along the top of every page


def app_shot(src, out):
    """A capture of the Overlay app, checked against settings/main.c's colours (an earlier cut of
    these captures had red and blue swapped)."""
    im = Image.open(src).convert("RGB")
    got = im.getpixel((480, 1))
    if any(abs(a - b) > 3 for a, b in zip(got, ACCENT)):
        sys.exit(f"{src}: top bar {got}, expected settings/main.c's ACCENT {ACCENT}")
    im.save(out, optimize=True)
    note(src, out, "copy (colours checked against settings/main.c)")


def main():
    vn = ROOT / "assets" / "vn"
    shots = ROOT / "assets" / "shots"
    vn.mkdir(parents=True, exist_ok=True)
    shots.mkdir(parents=True, exist_ok=True)
    for s in ("s1", "s2", "s3"):
        jpeg(SRC / "vn" / s / "bg@2x.png", vn / f"bg-{s}.jpg")
    # the card's screenshot: the game's whole frame when × is pressed, without the overlay, as
    # overlay-anki-screenshot's own encoder makes it (quality 70), and the card its exporter makes,
    # rendered by Anki: MOCK-UPS until that branch merges
    shot = SRC / "anki" / "with-screenshot-MOCKUP-feat-anki-screenshot"
    copy(shot / "MOCKUP-screenshot-s1-m2-q70.jpg", vn / "shot-s1-m2.jpg")
    anki = ROOT / "assets" / "anki"
    anki.mkdir(parents=True, exist_ok=True)
    copy(shot / "png" / "card-1-back-desktop-light-MOCKUP.png", anki / "card-1-back-mockup.png")
    # the music and the sound effects (CC0, promo-assets: audio/MANIFEST-fragment.md)
    audio = ROOT / "assets" / "audio"
    audio.mkdir(parents=True, exist_ok=True)
    copy(SRC / "audio" / "music" / "bed-44s.m4a", audio / "bed-44s.m4a")
    for n in ("click", "lr", "dpad", "whoosh-in", "whoosh-out", "swish", "pop", "chime", "confirm",
              "text-blip-1", "text-blip-2", "text-blip-3", "text-blip-4", "text-blip-5"):
        copy(SRC / "audio" / "sfx" / f"{n}.wav", audio / f"sfx-{n}.wav")
    for e in ("smile", "surprised", "happy"):
        copy(SRC / "vn" / "sprite" / f"hinata-{e}.png", vn / f"hinata-{e}.png")
    cap = SRC / "app" / "main-1ed95ca" / "png"
    for n in ("01-games-top", "02-games-scroll-1", "03-games-scroll-2", "04-games-neon-drift-selected",
              "05-games-neon-drift-OFF-saved"):
        app_shot(cap / f"{n}.png", shots / f"app-{n}.png")
    # PR #146 (feat/ocr-mode-setting 21488a7, not merged yet): its Read text row, in Vita3K
    pr = SRC / "app" / "pr146-ocr-mode-21488a7-NOT-MERGED" / "png"
    app_shot(pr / "PR146-03-read-text-selected-automatically.png", shots / "pr146-read-auto.png")
    app_shot(pr / "PR146-04-read-text-only-when-I-press-LR-saved.png", shots / "pr146-read-on-press.png")
    # the one-VPK install's Install page (overlay-one-vpk-install's design, not built yet), drawn by
    # the app's own drawing code in Vita3K: a MOCK-UP (promo-assets' README-MOCKUP.md)
    mk = SRC / "app" / "install-MOCKUP-one-vpk" / "png"
    for n in ("01-install-plan", "02-installing-plugins-and-models", "03-installing-checking-the-dictionary-56MB",
              "04-installing-checking-the-dictionary-121MB", "05-installing-config-txt-lines",
              "06-installed-restart-the-vita-now"):
        app_shot(mk / f"MOCKUP-{n}.png", shots / f"install-MOCKUP-{n[:2]}.png")
    # the home screen's icons: the icon0.png files the app capture installed
    icons = ROOT / "assets" / "icons"
    for p in sorted((SRC / "app" / "icons").glob("PROMO000[1-5]-*.png")):
        copy(p, icons / (p.name[:9] + ".png"))
    lines = [
        "# Where the composition's art comes from",
        "",
        "Written by `tools/prep_assets.py` from promo-assets' shared folder (its MANIFEST.md has each",
        "input's licence and source). The VN art is original (FLUX.1 [schnell], Apache-2.0, released CC0 by",
        "promo-assets); the app shots are Vita3K captures of this repository's Overlay app (GPL-3.0).",
        "",
        "| output | input | input sha256 | output sha256 | how |",
        "| --- | --- | --- | --- | --- |",
    ]
    lines += [f"| `{o}` | `{i}` | `{a[:16]}…` | `{b[:16]}…` | {h} |" for o, i, a, b, h in rows]
    (ROOT / "assets" / "SOURCES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(rows)} files")


if __name__ == "__main__":
    main()
