#!/usr/bin/env python3
"""split_holds.py SCREEN.mkv OUT_DIR [--box X,Y,W,H] [--min-hold N]: the Vita3K window cut out of the Xvfb
recording (default box 160,88,960,544: Vita3K's 960x544 window in the 1280x720 screen), then every run of at
least N identical recorded frames (default 20, 2/3 s at 30 fps) is a hold: its middle frame becomes
OUT_DIR/hold-<k>.png (lossless, exactly Vita3K's pixels) and OUT_DIR/holds.json lists them (start, end, seconds).
Also writes OUT_DIR/contact.png (every hold, small, numbered) and OUT_DIR/window.mp4 (the whole run, cropped,
H.264 CRF 12, yuv444p, and a yuv420p copy for players: window-420.mp4). Red and blue are swapped back (see below;
--keep-rb keeps them as recorded). Needs ffmpeg (FFMPEG), Pillow, numpy."""
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def main():
    src, out = sys.argv[1], Path(sys.argv[2])
    box = [160, 88, 960, 544]
    min_hold = 20
    args = sys.argv[3:]
    for i, a in enumerate(args):
        if a == "--box":
            box = [int(v) for v in args[i + 1].split(",")]
        if a == "--min-hold":
            min_hold = int(args[i + 1])
    ff = os.environ.get("FFMPEG", "ffmpeg")
    out.mkdir(parents=True, exist_ok=True)
    x, y, w, h = box
    # This Xvfb + llvmpipe capture delivers red and blue swapped: the app's BG RGBA8(9, 15, 23) records as
    # (23, 15, 9) and ACCENT (91, 220, 207) as (207, 220, 91); docs/settings-dictionaries-vita3k.png (another
    # Vita3K capture path) and settings/main.c have them the right way. Swapped back here (--keep-rb: not).
    crop = f"crop={w}:{h}:{x}:{y}" + ("" if "--keep-rb" in args else ",colorchannelmixer=rr=0:rb=1:br=1:bb=0")
    for name, extra in (("window.mp4", ["-pix_fmt", "yuv444p", "-crf", "12"]),
                        ("window-420.mp4", ["-pix_fmt", "yuv420p", "-crf", "14"])):
        subprocess.run([ff, "-nostdin", "-v", "error", "-y", "-i", src, "-vf", crop, "-c:v", "libx264", "-preset", "slow",
                        *extra, "-movflags", "+faststart", str(out / name)], check=True)
    p = subprocess.Popen([ff, "-nostdin", "-v", "error", "-i", src, "-vf", crop, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         stdout=subprocess.PIPE)
    n = w * h * 3
    digests, frames_kept = [], {}
    k = 0
    while True:
        buf = p.stdout.read(n)
        if len(buf) < n:
            break
        digests.append(hashlib.sha1(buf).hexdigest())
        frames_kept.setdefault(digests[-1], buf)
        k += 1
    p.wait()
    holds, i = [], 0
    while i < len(digests):
        j = i
        while j + 1 < len(digests) and digests[j + 1] == digests[i]:
            j += 1
        if j - i + 1 >= min_hold:
            holds.append((i, j))
        i = j + 1
    meta = []
    thumbs = []
    for hi, (a, b) in enumerate(holds, 1):
        buf = frames_kept[digests[a]]
        im = Image.frombytes("RGB", (w, h), buf)
        name = f"hold-{hi:02d}.png"
        im.save(out / name, optimize=True)
        meta.append({"hold": hi, "file": name, "first_frame": a, "last_frame": b, "start_s": round(a / 30, 2),
                     "seconds": round((b - a + 1) / 30, 2)})
        thumbs.append(im.resize((320, 181)))
    (out / "holds.json").write_text(json.dumps(meta, indent=1))
    cols = 4
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 320, max(1, rows) * 200), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for t, im in enumerate(thumbs):
        sheet.paste(im, ((t % cols) * 320, (t // cols) * 200))
        d.text(((t % cols) * 320 + 4, (t // cols) * 200 + 184), f"hold {t + 1}", fill=(255, 255, 0))
    sheet.save(out / "contact.png")
    print(f"{k} frames, {len(holds)} holds")


if __name__ == "__main__":
    main()
