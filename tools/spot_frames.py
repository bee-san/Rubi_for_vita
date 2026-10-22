#!/usr/bin/env python3
"""Scan screen's test frames (docs/vita-vn-ocr.md "Scan screen"): built from the repo's own synthetic sample,
host/samples/vn_textbox.jpg (tools/make_test_screens.py), by pixel copies and an integer 2x2 mean only, so every
Pillow that decodes the JPEG alike gives the same bytes (each frame's sha256 is in the fixture). No game's pixels.

    sample  the sample itself, 960x544: a HUD line, a name plate and two lines of dialogue
    grid    96 copies of an 80x36 piece of its dialogue on black, 40 px apart, 960x544: more than 64 candidates (the
            spotter keeps the most confident 64: truncated) and more than 16 blocks (the overlay draws the first 16,
            the pick among them). The pieces were 100 px wide and 30 px apart until vita-vn-ocr #108's spotter, which
            reads such a row as one line (17 boxes, 1 block)
    psp     the sample at the PSP's 480x272 (exact 2x2 means, as a PSP frame): read without halving

frames() -> [(name, width, height, rgba bytes)]; tools/spot_fixture.py runs vita-vn-ocr's twin on them and
host/tests/test_game_spot.py the game worker's scan.
"""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLE = ROOT / "host" / "samples" / "vn_textbox.jpg"


def _rgb(path=SAMPLE):
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGB")
        if im.size != (960, 544):
            raise SystemExit(f"{path}: {im.size}, not 960x544")
        return im.copy()


def _grid(src):
    from PIL import Image
    piece = src.crop((70, 385, 150, 421))  # 80x36 of the first dialogue line
    frame = Image.new("RGB", (960, 544), (0, 0, 0))
    for y in range(4, 544 - 36, 36 + 8):
        for x in range(4, 960 - 80, 80 + 40):
            frame.paste(piece, (x, y))
    return frame


def _half(src):
    """(a + b + c + d + 2) >> 2 per channel over each 2x2 block: integers only."""
    from PIL import Image
    w, h = src.size
    px = src.tobytes()
    out = bytearray((w // 2) * (h // 2) * 3)
    o = 0
    for y in range(0, h, 2):
        r0, r1 = y * w * 3, (y + 1) * w * 3
        for x in range(0, w * 3, 6):
            for c in range(3):
                out[o] = (px[r0 + x + c] + px[r0 + x + 3 + c] + px[r1 + x + c] + px[r1 + x + 3 + c] + 2) >> 2
                o += 1
    return Image.frombytes("RGB", (w // 2, h // 2), bytes(out))


def frames():
    src = _rgb()
    out = []
    for name, im in (("sample", src), ("grid", _grid(src)), ("psp", _half(src))):
        out.append((name, im.size[0], im.size[1], im.convert("RGBA").tobytes()))
    return out


def sha256(data):
    return hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    for name, w, h, data in frames():
        print(f"{name} {w}x{h} {sha256(data)}")
