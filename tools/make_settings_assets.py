#!/usr/bin/env python3
"""Rebuild Vita-compatible indexed art with tools/requirements-assets.txt."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

DEST = Path(__file__).resolve().parents[1] / "settings" / "assets"
DEST.mkdir(parents=True, exist_ok=True)
BG, CARD, ACCENT, WHITE = "#0d121f", "#182134", "#5bdccf", "#eef4ff"


def create(name, width, height, size, title):
    im = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((4, 4, width - 5, height - 5), radius=width // 12, fill=CARD)
    d.rectangle((width // 8, height // 6, width * 7 // 8, height // 6 + 3), fill=ACCENT)
    font = ImageFont.load_default(size=size)
    d.text((width / 2, height * .43), title, font=font, anchor="mm", fill=WHITE)
    x, y, w, h = width * .32, height * .70, width * .36, height * .14
    d.rounded_rectangle((x, y, x + w, y + h), radius=h / 2, fill=ACCENT)
    d.ellipse((x + w - h + 3, y + 3, x + w - 3, y + h - 3), fill=BG)
    # The Vita promoter rejects RGBA artwork (0x8010113D). These images
    # have <=256 colors, so indexing keeps every displayed pixel unchanged.
    colors = sorted(set(im.get_flattened_data()))
    if len(colors) > 256:
        raise ValueError(f"{name}: artwork needs more than 256 palette colors")
    indexed = Image.new("P", im.size)
    palette = [channel for color in colors for channel in color]
    indexed.putpalette(palette + [0] * (768 - len(palette)))
    indices = {color: index for index, color in enumerate(colors)}
    indexed.putdata([indices[color] for color in im.get_flattened_data()])
    if indexed.convert("RGB").tobytes() != im.tobytes():
        raise ValueError(f"{name}: palette encoding changed the artwork")
    indexed.save(DEST / name, optimize=True, bits=8)


create("icon0.png", 128, 128, 37, "JP")
create("startup.png", 280, 158, 25, "JP Overlay")
create("bg.png", 840, 500, 57, "JP Overlay Settings")
