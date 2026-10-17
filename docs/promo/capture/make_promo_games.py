#!/usr/bin/env python3
"""make_promo_games.py UX0_DIR VN_ART_DIR FONT_DIR: the installed games the promo's Rubi app capture lists.
Original titles (no real games) and original icons: ux0/app/<TITLEID>/sce_sys/param.sfo (TITLE, as
tools/make_settings_smoke_games.py writes it) and icon0.png (128x128, 8-bit palette, no text chunks). The
promo's own VN, 夕凪ステップ (PROMO0001), gets an icon cut from its art (vn/src/bg-s1.png and the heroine).
Needs Pillow. (promo-assets' src/app_capture/make_promo_games.py; FONT_DIR has ZenMaruGothic-Bold.ttf, OFL)"""
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

GAMES = [  # (title ID, title, two gradient colours, glyph)
    ("PROMO0001", "夕凪ステップ", None, None),
    ("PROMO0002", "Neon Drift", ((255, 60, 170), (40, 20, 120)), "N"),
    ("PROMO0003", "Courier Run", ((255, 190, 60), (220, 70, 40)), "C"),
    ("PROMO0004", "Ember Garden", ((255, 120, 60), (90, 30, 60)), "E"),
    ("PROMO0005", "Hoshizora Diary", ((90, 120, 255), (20, 20, 70)), "★"),
    ("PROMO0006", "Paper Lantern", ((255, 220, 150), (200, 90, 50)), "P"),
    ("PROMO0007", "Tidal Archive", ((60, 220, 200), (20, 60, 120)), "T"),
    ("PROMO0008", "さくらの記憶", ((255, 170, 200), (150, 70, 140)), "さ"),
]


def sfo(text):
    value = text + b"\0"
    cap = (len(value) + 3) & ~3
    header = struct.pack("<4sIIII", b"\0PSF", 0x0101, 36, 44, 1)
    entry = struct.pack("<HHIII", 0, 0x0204, len(value), cap, 0)
    return header + entry + b"TITLE\0\0\0" + value.ljust(cap, b"\0")


def gradient_icon(c0, c1, glyph, font_path):
    im = Image.new("RGB", (128, 128))
    d = ImageDraw.Draw(im)
    for y in range(128):
        t = y / 127
        d.line([(0, y), (127, y)], fill=tuple(int(c0[k] * (1 - t) + c1[k] * t) for k in range(3)))
    d.ellipse((-30, -50, 110, 60), fill=None, outline=(255, 255, 255), width=2)
    font = ImageFont.truetype(font_path, 78)
    box = d.textbbox((0, 0), glyph, font=font)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((128 - w) / 2 - box[0] + 2, (128 - h) / 2 - box[1] + 3), glyph, font=font, fill=(0, 0, 0))
    d.text(((128 - w) / 2 - box[0], (128 - h) / 2 - box[1]), glyph, font=font, fill=(255, 255, 255))
    return im


def vn_icon(art):
    bg = Image.open(art / "src" / "bg-s1.png").convert("RGB").crop((300, 0, 660, 360)).resize((128, 128), Image.LANCZOS)
    face = Image.open(art / "sprite" / "hinata-smile.png").convert("RGBA").crop((150, 90, 430, 370)).resize((112, 112), Image.LANCZOS)
    bg.paste(face, (8, 16), face)
    return bg


def main():
    ux0, art = Path(sys.argv[1]), Path(sys.argv[2])
    font = str(Path(sys.argv[3]) / "ZenMaruGothic-Bold.ttf")
    for tid, title, cols, glyph in GAMES:
        d = ux0 / "app" / tid / "sce_sys"
        d.mkdir(parents=True, exist_ok=True)
        (d / "param.sfo").write_bytes(sfo(title.encode()))
        im = vn_icon(art) if cols is None else gradient_icon(*cols, glyph, font)
        im.quantize(256, method=Image.Quantize.MEDIANCUT).save(d / "icon0.png", optimize=True)
    print(len(GAMES), "games")


if __name__ == "__main__":
    main()
