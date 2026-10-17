#!/usr/bin/env python3
"""Rebuild the Rubi app's LiveArea art (settings/assets/{icon0,startup,bg,pic0}.png).

Needs tools/requirements-assets.txt. The mascot is settings/assets/icon-source.png,
a FLUX.1 [schnell] image whose model, licence, checksums, prompt and seed are
recorded in settings/assets/ICON-SOURCE.md. The model drew an unreadable glyph in
her speech bubble, so this script paints the bubble clean and draws a hiragana
あ from hand-made strokes. The "Rubi" wordmark is drawn the same way, so no
font is needed and the output depends only on the source image and Pillow.
The Vita promoter rejects RGBA artwork (0x8010113D); every image is written as an
opaque, non-interlaced, 8-bit palette PNG with at most 256 colours.
"""
import hashlib
import math
from pathlib import Path

from PIL import Image, ImageDraw

DEST = Path(__file__).resolve().parents[1] / "settings" / "assets"
SOURCE = DEST / "icon-source.png"
SOURCE_SHA256 = "3bc8d4250b2ec35766e0b8ca4bf2431d8f199b699426a2d47ad49b7a4ea79991"
SS = 4                      # supersampling for the hand-drawn strokes
INK = (95, 25, 25)          # the mascot's outline colour
PAPER = (255, 255, 255)
LAVENDER = (241, 223, 238)  # the mascot's background
BUBBLE = (408.5, 154.0, 47.0)  # speech-bubble interior in the source: centre x, y, radius
# The Vita's file limit for icon0.png (Sony's content information specification).
ICON0_MAX_BYTES = 32 * 1024

# Hiragana あ: three strokes of cubic Bezier segments in a 100 x 100 box (y down);
# the ink spans y 10..88.
KANA_A = [
    [((19, 31), (37, 30), (56, 28), (75, 25))],
    [((42, 10), (41, 38), (43, 62), (52, 82))],
    [((65, 40), (59, 56), (47, 72), (33, 80)),
     ((33, 80), (22, 86), (12, 72), (28, 59)),
     ((28, 59), (40, 50), (56, 46), (70, 47)),
     ((70, 47), (88, 50), (92, 72), (64, 88))],
]


def bezier(p0, p1, p2, p3, steps=48):
    return [((1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t * t * p2[0] + t ** 3 * p3[0],
             (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t * t * p2[1] + t ** 3 * p3[1])
            for t in (i / steps for i in range(steps + 1))]


def arc(cx, cy, rx, ry, start, end, steps=64):
    """Points on an ellipse in y-up letter units, angles in degrees, counter-clockwise."""
    return [(cx + rx * math.cos(math.radians(a)), cy + ry * math.sin(math.radians(a)))
            for a in (start + (end - start) * i / steps for i in range(steps + 1))]


def kana_paths(cx, cy, height):
    """The あ strokes as polylines, ink `height` px tall, centred on (cx, cy)."""
    scale = height / 78
    paths = [[p for segment in stroke for p in bezier(*segment)] for stroke in KANA_A]
    xs = [x for path in paths for x, _ in path]
    ox, oy = cx - (min(xs) + max(xs)) / 2 * scale, cy - 49 * scale
    return [[(ox + x * scale, oy + y * scale) for x, y in path] for path in paths]


def wordmark_paths(x, baseline, xh):
    """'Rubi' as round monoline strokes: x-height `xh` px, from `x` on `baseline`.

    Returns (paths, width). Letters are designed in x-height units with y up; the cap height (1.40)
    and the ascender (1.45) are those of the 'Overlay' wordmark the app had before the rename.
    """
    gap = 0.44
    letters = [
        (0.86, [[(0, 0), (0, 1.40)],                                                      # R: stem,
                [(0, 1.40), (0.40, 1.40)] + arc(0.40, 1.04, 0.38, 0.36, 90, -90) + [(0.40, 0.68), (0, 0.68)],
                [(0.40, 0.68), (0.86, 0)]]),                                              #    bowl, leg
        (0.84, [[(0, 1), (0, 0.42)] + arc(0.42, 0.42, 0.42, 0.42, 180, 360) + [(0.84, 1)],  # u
                [(0.84, 1), (0.84, 0)]]),
        (0.96, [[(0, 0), (0, 1.45)], arc(0.48, 0.5, 0.48, 0.5, 0, 360)]),               # b
        (0.00, [[(0, 0), (0, 1)], [(0, 1.40), (0, 1.40)]]),                               # i and its dot
    ]
    paths, origin = [], 0.0
    for advance, strokes in letters:
        for stroke in strokes:
            paths.append([(x + (origin + u) * xh, baseline - v * xh) for u, v in stroke])
        origin += advance + gap
    return paths, (origin - gap) * xh


def stroke_mask(size, paths, width):
    """Antialiased L mask of round-capped polylines (pixel coordinates of `size`)."""
    mask = Image.new("L", (size[0] * SS, size[1] * SS), 0)
    draw = ImageDraw.Draw(mask)
    r = width * SS / 2
    for path in paths:
        points = [(x * SS, y * SS) for x, y in path]
        draw.line(points, fill=255, width=round(width * SS), joint="curve")
        for px, py in (points[0], points[-1]):
            draw.ellipse((px - r, py - r, px + r, py + r), fill=255)
    return mask.resize(size, Image.LANCZOS)


def disc_mask(size, cx, cy, r):
    mask = Image.new("L", (size[0] * SS, size[1] * SS), 0)
    ImageDraw.Draw(mask).ellipse(((cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS), fill=255)
    return mask.resize(size, Image.LANCZOS)


def paint(image, mask, colour):
    image.paste(Image.new("RGB", image.size, colour), (0, 0), mask)


def sticker_text(image, paths, width, outline):
    paint(image, stroke_mask(image.size, paths, width + 2 * outline), PAPER)
    paint(image, stroke_mask(image.size, paths, width), INK)


def mascot():
    """The source image with a clean speech bubble holding a hand-drawn あ (512 x 512 RGB)."""
    data = SOURCE.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError(f"{SOURCE.name} changed; update ICON-SOURCE.md and SOURCE_SHA256 together")
    image = Image.open(SOURCE).convert("RGB")
    cx, cy, r = BUBBLE
    paint(image, disc_mask(image.size, cx, cy, r), PAPER)
    paint(image, stroke_mask(image.size, kana_paths(cx, cy, 62), 10), INK)
    return image


def cutout(image):
    """The mascot without her flat background: (RGB crop, L mask) of her bounding box."""
    key = image.copy()
    marker = (0, 255, 0)
    w, h = key.size
    for seed in [(x, y) for x in (0, w - 1) for y in range(0, h, 16)] + \
                [(x, y) for y in (0, h - 1) for x in range(0, w, 16)]:
        pixel = key.getpixel(seed)
        if pixel != marker and sum(abs(a - b) for a, b in zip(pixel, LAVENDER)) <= 24:
            ImageDraw.floodfill(key, seed, marker, thresh=24)
    mask = Image.new("L", key.size, 0)
    mask.putdata([0 if pixel == marker else 255 for pixel in key.get_flattened_data()])
    box = mask.getbbox()
    return image.crop(box), mask.crop(box)


def place(canvas, sprite, mask, height, x, y):
    width = round(sprite.width * height / sprite.height)
    canvas.paste(sprite.resize((width, height), Image.LANCZOS), (x, y),
                 mask.resize((width, height), Image.LANCZOS))


def gradient(size, top, bottom):
    w, h = size
    column = Image.new("RGB", (1, h))
    column.putdata([tuple(round(a + (b - a) * y / (h - 1)) for a, b in zip(top, bottom)) for y in range(h)])
    return column.resize(size, Image.NEAREST)


def sparkle(canvas, cx, cy, r, colour):
    """A four-point star."""
    k = r * 0.28
    points = [(cx, cy - r), (cx + k, cy - k), (cx + r, cy), (cx + k, cy + k),
              (cx, cy + r), (cx - k, cy + k), (cx - r, cy), (cx - k, cy - k)]
    mask = Image.new("L", (canvas.width * SS, canvas.height * SS), 0)
    ImageDraw.Draw(mask).polygon([(x * SS, y * SS) for x, y in points], fill=255)
    paint(canvas, mask.resize(canvas.size, Image.LANCZOS), colour)


def bubble(canvas, cx, cy, r, colour, ink):
    """A small round speech bubble with a tail to the lower left, holding あ."""
    mask = Image.new("L", (canvas.width * SS, canvas.height * SS), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse(((cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS), fill=255)
    draw.polygon([((cx - r * 0.75) * SS, (cy + r * 0.25) * SS), ((cx - r * 1.15) * SS, (cy + r * 1.05) * SS),
                  ((cx - r * 0.15) * SS, (cy + r * 0.80) * SS)], fill=255)
    paint(canvas, mask.resize(canvas.size, Image.LANCZOS), colour)
    paint(canvas, stroke_mask(canvas.size, kana_paths(cx, cy, r * 1.25), max(1.5, r * 0.2)), ink)


def save_indexed(image, name, max_bytes=None):
    """Quantize to <= 256 colours and write an opaque 8-bit palette PNG with no extra chunks."""
    quantized = image.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    indexed = Image.new("P", quantized.size)
    indexed.putpalette(quantized.getpalette())
    indexed.putdata(list(quantized.get_flattened_data()))
    path = DEST / name
    indexed.save(path, optimize=True, bits=8)
    with Image.open(path) as written:
        if written.mode != "P" or written.size != image.size or "transparency" in written.info:
            raise ValueError(f"{name}: not an opaque palette image")
        if written.convert("RGB").tobytes() != indexed.convert("RGB").tobytes():
            raise ValueError(f"{name}: palette encoding changed the artwork")
    if max_bytes and path.stat().st_size > max_bytes:
        raise ValueError(f"{name}: {path.stat().st_size} bytes, over the {max_bytes}-byte limit")
    return path


def splash(bg):
    """The launch splash (pic0.png, 960 x 544): bg scaled to the full height, its plain
    edge columns extended into the margins, so the wordmark is not cropped."""
    width = round(bg.width * 544 / bg.height)
    scaled = bg.resize((width, 544), Image.LANCZOS)
    left = (960 - width) // 2
    canvas = Image.new("RGB", (960, 544))
    canvas.paste(scaled.crop((0, 0, 1, 544)).resize((left, 544)), (0, 0))
    canvas.paste(scaled.crop((width - 1, 0, width, 544)).resize((960 - left - width, 544)), (left + width, 0))
    canvas.paste(scaled, (left, 0))
    return canvas


def main():
    art = mascot()
    save_indexed(art.resize((128, 128), Image.LANCZOS), "icon0.png", ICON0_MAX_BYTES)
    sprite, mask = cutout(art)

    # Gate image: the system draws its Start button over the bottom of it.
    startup = gradient((280, 158), (248, 237, 248), (239, 222, 240))
    for cx, cy, r in ((258, 22, 6), (150, 30, 4), (266, 118, 4.5)):
        sparkle(startup, cx, cy, r, PAPER)
    place(startup, sprite, mask, 150, 4, 6)
    _, width = wordmark_paths(0, 0, 22)
    paths, _ = wordmark_paths(214 - width / 2, 88, 22)   # centred in the space right of the mascot
    sticker_text(startup, paths, 6, 2)
    save_indexed(startup, "startup.png")

    # LiveArea background; the gate sits in the middle of its upper part.
    bg = gradient((840, 500), (247, 236, 248), (250, 226, 238))
    for cx, cy, r in ((330, 52, 22), (782, 70, 26), (612, 300, 18), (340, 440, 15), (720, 196, 14)):
        bubble(bg, cx, cy, r, (255, 248, 252), (226, 196, 222))
    for cx, cy, r in ((240, 40, 9), (560, 34, 7), (812, 150, 8), (660, 120, 6), (480, 300, 8),
                      (380, 452, 7), (34, 60, 8), (120, 120, 6), (806, 470, 6)):
        sparkle(bg, cx, cy, r, PAPER)
    place(bg, sprite, mask, 300, 24, 192)
    _, width = wordmark_paths(0, 0, 46)
    paths, _ = wordmark_paths(816 - width, 428, 46)   # right-aligned at x 816
    sticker_text(bg, paths, 11, 5)
    save_indexed(bg, "bg.png")
    save_indexed(splash(bg), "pic0.png")


if __name__ == "__main__":
    main()
