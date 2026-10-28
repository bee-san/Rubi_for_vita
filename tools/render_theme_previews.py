#!/usr/bin/env python3
"""Render mock screenshots from the real app's host draw records (not Vita captures).

Build vjo-settings-app-tests, then run with --font pointing at a Japanese OpenType
font, e.g. NotoSansCJKjp-Regular.otf (SIL OFL). Pillow is the only dependency.
The test harness supplies positions, colours, clipping and substitute font widths;
this renderer fits the supplied font to those widths. Firmware PVF may differ.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont


def rgba(packed):
    return tuple((packed >> shift) & 255 for shift in (0, 8, 16, 24))


def render(record, font_path, target):
    scale = 2  # supersample for readable small labels
    screen = Image.new("RGBA", (960 * scale, 544 * scale), rgba(record["clear"]))
    draw = ImageDraw.Draw(screen)
    for x, y, w, h, color, clipped, top, bottom, left, right in record["rects"]:
        if clipped:
            edge = min(x + w, right)
            x = max(x, left)
            w = edge - x
            end = min(y + h, bottom)
            y = max(y, top)
            h = end - y
        if w > 0 and h > 0:
            if color >> 24 == 255:
                draw.rectangle((x * scale, y * scale, (x + w) * scale - 1, (y + h) * scale - 1), fill=rgba(color))
            else:
                # Match libvita2d's source-alpha blending for the procedural vignette.
                shade = Image.new("RGBA", (w * scale, h * scale), rgba(color))
                screen.alpha_composite(shade, (x * scale, y * scale))
    for x, y, color, size, clipped, top, bottom, clip_left, clip_right, text in record["texts"]:
        if not text:
            continue
        font = ImageFont.truetype(str(font_path), max(1, round(18 * size * scale)))
        # Use a baseline anchor, then fit to the exact width used by the harness.
        left, ascent, right, descent = font.getbbox(text, anchor="ls")
        ink = Image.new("RGBA", (max(1, right - left), max(1, descent - ascent)))
        ImageDraw.Draw(ink).text((-left, -ascent), text, font=font, anchor="ls", fill=rgba(color))
        units = sum(10 if ord(c) < 128 else 18 for c in text)
        width = max(1, int(units * size + 0.5) * scale)
        ink = ink.resize((width, ink.height), Image.Resampling.LANCZOS)
        py = y * scale + ascent
        px = x * scale
        if clipped:
            a = max(0, top * scale - py)
            b = min(ink.height, bottom * scale - py)
            l = max(0, clip_left * scale - px)
            r = min(ink.width, clip_right * scale - px)
            if b <= a or r <= l:
                continue
            ink = ink.crop((l, a, r, b))
            py += a
            px += l
        screen.alpha_composite(ink, (px, py))
    screen = screen.resize((960, 544), Image.Resampling.LANCZOS)
    labelled = Image.new("RGB", (960, 588), "#e8e8e8")
    labelled.paste(screen.convert("RGB"), (0, 0))
    label_font = ImageFont.truetype(str(font_path), 17)
    ImageDraw.Draw(labelled).text((16, 553), "Host-rendered preview · sample data · substitute fonts · not a Vita capture",
                                  font=label_font, fill="#303030")
    labelled.save(target, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-bin", type=Path, default=Path("build/host/vjo-settings-app-tests"))
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("docs/colour-schemes"))
    parser.add_argument("--walkthrough", action="store_true", help="Capture every settings category and scroll position")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rubi-theme-previews-") as records:
        env = dict(os.environ, VJO_THEME_PREVIEW_DIR=records)
        tests = ["app_colour_schemes", "settings_navigation"]
        if args.walkthrough:
            tests.append("settings_walkthrough")
        subprocess.run([str(args.test_bin.resolve()), *tests], env=env, check=True)
        if args.walkthrough:
            for unit in ("games", "anki", "install"):
                binary = args.test_bin.resolve().parent / f"vjo-settings-{unit}-tests"
                subprocess.run([str(binary), "settings_walkthrough"], env=env, check=True)
        captures = {p.stem: json.loads(p.read_text()) for p in Path(records).glob("*.json")}
        for name, record in sorted(captures.items()):
            if "page" in record:
                # Bodies are drawn by the real Anki/Install page. Compose main.c's real shared header,
                # including its gradient and Back control; only the category title changes.
                header = captures["default-picker"]
                rects = [copy.deepcopy(r) for r in header["rects"]
                         if r[1] < 100 or (r[0] == 0 and r[2] == 960) or (r[0] < 112 and r[1] >= 504)]
                texts = [copy.deepcopy(t) for t in header["texts"]
                         if t[1] < 100 or (t[0] < 112 and t[1] >= 508)]
                for t in texts:
                    if t[-1] == "Appearance" and t[1] == 35:
                        t[-1] = record["page"]
                        width = int(sum(10 if ord(c) < 128 else 18 for c in t[-1]) * t[3] + 0.5)
                        t[0] = 24 + (912 - width) // 2
                record = dict(record, rects=rects + record["rects"], texts=texts + record["texts"])
            folder = args.output / "walkthrough" if name.startswith("walk-") else args.output
            folder.mkdir(parents=True, exist_ok=True)
            target = folder / (name + ".png")
            render(record, args.font, target)
            print(target)


if __name__ == "__main__":
    main()
