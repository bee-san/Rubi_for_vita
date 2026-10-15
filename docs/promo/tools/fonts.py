#!/usr/bin/env python3
"""Fetch the promo's fonts (SIL OFL 1.1) at a pinned google/fonts commit,
check their SHA-256, and subset them to the characters the compositions use.

    python3 tools/fonts.py            # writes assets/fonts/*.woff2 and the licences
    python3 tools/fonts.py --check    # fails if a subset is out of date

Needs fontTools and brotli (pinned in tools/requirements-fonts.txt). Run it
after `npm run build` whenever on-screen text changes: the subsets hold only
the characters of the composition's sources (plus printable ASCII).
"""
import argparse
import hashlib
import io
import sys
import urllib.request
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / ".cache" / "fonts"
OUT = ROOT / "assets" / "fonts"
COMMIT = "94301472b4d809299b4775bc6502592b83dd6934"  # google/fonts main, 2026-10-15
BASE = f"https://raw.githubusercontent.com/google/fonts/{COMMIT}/ofl/"

FONTS = [
    # (source path under ofl/, sha256, output, licence path, licence sha256)
    ("notosansjp/NotoSansJP%5Bwght%5D.ttf",
     "c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f",
     "noto-sans-jp.woff2", "notosansjp/OFL.txt",
     "1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9"),
    ("zenmarugothic/ZenMaruGothic-Medium.ttf",
     "3cfdb98a13571ede17fcc769f5093a97c38b80a7b9b2ab754a26b4d822092b3b",
     "zen-maru-gothic-medium.woff2", "zenmarugothic/OFL.txt",
     "2a20cf7ce1909d8ee1e949095d340f7d7656705f7c810a2d6faf56800ad0cb3d"),
    ("zenmarugothic/ZenMaruGothic-Bold.ttf",
     "fe24426b9c8b5523a0146a8235c8674eccf0493af354a53ec895c3596d9eb745",
     "zen-maru-gothic-bold.woff2", "zenmarugothic/OFL.txt",
     "2a20cf7ce1909d8ee1e949095d340f7d7656705f7c810a2d6faf56800ad0cb3d"),
    ("notoserifjp/NotoSerifJP%5Bwght%5D.ttf",
     "2fd527ba12b6a44ec30d796d633360da0aeba6c5d4af1304ce12bb4dc15a7dfc",
     "noto-serif-jp.woff2", "notoserifjp/OFL.txt",
     "5e0da210fb04058a8c0087985d2d456b931c2579811a49655721d3cf0c36b6d6"),
    ("jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf",
     "48715a42ec242c21e9f02692891e147d022299a52e48d5e413e1a942193ffeda",
     "jetbrains-mono.woff2", "jetbrainsmono/OFL.txt",
     "b2fe5e8987594e9ffd1d2ca52a2f5d73eb8335243893c5d6254b5ad69269591d"),
]
LICENCE_NAMES = {
    "notosansjp/OFL.txt": "OFL-NotoSansJP.txt",
    "zenmarugothic/OFL.txt": "OFL-ZenMaruGothic.txt",
    "notoserifjp/OFL.txt": "OFL-NotoSerifJP.txt",
    "jetbrainsmono/OFL.txt": "OFL-JetBrainsMono.txt",
}


def fetch(path, sha):
    CACHE.mkdir(parents=True, exist_ok=True)
    local = CACHE / path.replace("/", "__").replace("%5B", "[").replace("%5D", "]")
    if not local.exists():
        with urllib.request.urlopen(BASE + path, timeout=120) as r:
            data = r.read()
        local.write_bytes(data)
    data = local.read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        sys.exit(f"{path}: sha256 {got}, expected {sha}")
    return data


def used_text():
    text = set(chr(c) for c in range(0x20, 0x7F))
    for name in ("src/promo.template.html", "src/promo.js", "src/script.mjs", "tools/build.mjs"):
        text |= set((ROOT / name).read_text(encoding="utf-8"))
    return "".join(sorted(c for c in text if c.isprintable() or c == "\u3000"))


def make_subset(data, chars):
    # recalcTimestamp=False: keep the source's head.modified, so the same input
    # always gives the same bytes (--check depends on it).
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    opts = subset.Options()
    opts.flavor = "woff2"
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    sub = subset.Subsetter(options=opts)
    sub.populate(text=chars)
    sub.subset(font)
    buf = io.BytesIO()
    font.flavor = "woff2"
    font.save(buf)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    chars = used_text()
    OUT.mkdir(parents=True, exist_ok=True)
    stale = 0
    for src, sha, out, lic, lic_sha in FONTS:
        woff2 = make_subset(fetch(src, sha), chars)
        licence = fetch(lic, lic_sha)
        for name, data in ((out, woff2), (LICENCE_NAMES[lic], licence)):
            path = OUT / name
            if args.check:
                if not path.exists() or path.read_bytes() != data:
                    print(f"{path.relative_to(ROOT)} is out of date: run python3 tools/fonts.py")
                    stale += 1
            else:
                path.write_bytes(data)
        if not args.check:
            print(f"{out}: {len(woff2)} bytes")
    if not args.check:
        print(f"{len(chars)} characters")
    sys.exit(1 if stale else 0)


if __name__ == "__main__":
    main()
