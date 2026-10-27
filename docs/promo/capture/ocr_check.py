#!/usr/bin/env python3
"""ocr_check.py CHECKOUT MODEL_DIR VN_ART OUT.md: does the checkout's recognizer read each line the film shows?

The overlay's header is the game worker's text: what VOCR reads in the player's dialogue region, normalised
(core/textnorm.c). The film writes the line it typed there, so that has to be what the recognizer reads. For each
frame of the mock game the film shows (promo-assets' vn/<scene>/<scene>-m<n>-frame.png, the whole message on
screen), this crops the region a player would draw (x 54..906, y 402..506 of the 960 x 544 screen: the textbox's
text, not the name plate or the menu), runs the checkout's host OCR tool, vjo-ocr, with the checkout's defaults
(the Normal size's model and, as its confidence cascade, the next size up: tools/model_tiers.json; the default
ocr_lm of docs/config.example.ini), normalises both texts with the checkout's vjo-textnorm, and writes the table to
OUT.md. Exit 1 on a difference. A host run, not a console one.
(from promo-assets' src/ocr_check.py)"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

checkout, models, art, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
REGION = (54, 402, 906, 506)
LINES = [  # (frame, the line as the film shows it: src/script.mjs VN)
    ("s1/s1-m1-frame.png", "「ねえ、聞いて！明日の夜、丘の上で\n流れ星が見えるんだって！」"),
    ("s1/s1-m2-frame.png", "「また明日、一緒に見に行こう！」"),
    ("s2/s2-m1-frame.png", "「ドキドキ、ワクワク……！」"),
    ("s3/s3-m1-frame.png", "「キラキラ……。\n子供の頃みたい。」"),
]
tool = checkout / "build" / "host"
tiers = json.loads((checkout / "tools" / "model_tiers.json").read_text())
names = [t["id"] for t in tiers["tiers"]]
normal = names.index(tiers["default"])
MODEL = tiers["tiers"][normal]["model"]
CASCADE = tiers["tiers"][normal + 1]["model"] if normal + 1 < len(names) else None
lm = re.search(r"^ocr_lm = (\S+)", (checkout / "docs" / "config.example.ini").read_text(), re.M).group(1)
# the tier's pinned file, as the kernel's broker names it (include/vjo_vocr_sizes.h VJO_VOCR_LM<n>_NAME, tiers 1-4)
_sizes = (checkout / "include" / "vjo_vocr_sizes.h").read_text()
_names = dict(re.findall(r'#define VJO_VOCR_LM([1-4])_NAME "([^"]+)"', _sizes))
LM = _names[str(("500k", "1000k", "2000k", "4000k").index(lm) + 1)]


def norm(text):
    r = subprocess.run([str(tool / "vjo-textnorm")], input="\n".join(text.split("\n")) + "\n", capture_output=True,
                       text=True, check=True)
    return "\n".join(l.split("\t")[0] for l in r.stdout.rstrip("\n").split("\n"))


rows, bad = [], 0
commit = subprocess.run(["git", "-C", str(checkout), "rev-parse", "--short=7", "HEAD"], capture_output=True, text=True,
                        check=True).stdout.strip()
for frame, line in LINES:
    im = Image.open(art / frame).convert("RGBA").crop(REGION)
    with tempfile.NamedTemporaryFile(suffix=".rgba") as f:
        f.write(im.tobytes())
        f.flush()
        r = subprocess.run([str(tool / "vjo-ocr"), "--backend", "vocr", "--model", MODEL, "--lm", LM,
                            *(["--cascade", CASCADE] if CASCADE else []), models, str(im.width), str(im.height), f.name],
                           capture_output=True, text=True, timeout=600)
    got, want = norm(r.stdout.rstrip("\n")), norm(line)
    ok = got == want
    bad += not ok
    rows.append(f"| `vn/{frame}` | {want.replace(chr(10), ' / ')} | {got.replace(chr(10), ' / ')} | {'same' if ok else 'DIFFERENT'} |")
md = ["# What the recognizer reads from each line the film shows", "",
      f"Host vjo-ocr of Rubi {commit} (its OCR files and pinned vita-vn-ocr runtime), {MODEL} + {LM} + "
      f"cascade {CASCADE} (its defaults), on the region x 54..906, y 402..506; both texts normalised by its "
      "vjo-textnorm, as the game worker normalises. A host run, not a console one.", "",
      "| frame (promo-assets) | the film's line | recognized | |", "| --- | --- | --- | --- |", *rows, ""]
out.write_text("\n".join(md), encoding="utf-8")
print("\n".join(rows))
sys.exit(1 if bad else 0)
