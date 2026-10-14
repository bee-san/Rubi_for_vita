#!/usr/bin/env python3
"""The capture parity test on eval crops (#74): API 18's capture against API 19's.

host/tests/test_capture_parity.c runs both capture pipelines to vocr_run and
compares what the recognizer is given, byte for byte: API 18 (the kernel copies
the region as A8B8G8R8, the game worker builds its grey plane; 8228c3c,
verbatim) and API 19 (the display hook writes grey rows into the worker's
strip). This script hands it the regions of vita-vn-ocr's linefind dev
manifests, each screenshot as a game frame in every SceDisplay format the
kernel accepts. Only the dev manifests are read: the protected test sets
(vita-test, psp-test, fresh native, CLANNAD) are never used. Frames are
written to a temporary directory and deleted afterwards.

  python3 tools/capture_parity.py --binary build/host/vjo-capture-parity-tests \\
      --manifests /path/to/vita-vn-ocr/eval/linefind/manifests --shots DIR [--shots DIR ...]

With the real weights in the build (VJO_VOCR_MODEL_DIR) the preprocessed
tensors are compared too. Exit status: the test binary's (0: no difference).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

DEV_MANIFESTS = ("vita_dev.jsonl", "psp_dev.jsonl")
# Never for scoring, tuning or this test (vita-vn-ocr eval/README): by set name and by game.
PROTECTED_SETS = {"vita-test", "psp-test", "vita_test", "psp_test", "native", "fresh-native", "fresh_native", "clannad"}
PROTECTED_VNS = {"v4"}  # CLANNAD
TEST = "eval_crops_give_identical_recognizer_input"


def protected(row):
    text = " ".join(str(row.get(k, "")) for k in ("set", "id")).lower()
    return (str(row.get("set", "")).lower() in PROTECTED_SETS or row.get("vn") in PROTECTED_VNS
            or "clannad" in text or "native" in text)


def find_shot(name, dirs):
    for d in dirs:
        p = Path(d) / name
        if p.exists():
            return p
    return None


def load_frame(path, mask):
    """The screenshot as RGBA bytes, with manifest masks filled as build_gt.load_shot does."""
    from PIL import Image
    img = Image.open(path).convert("RGB")
    if mask:
        import numpy as np
        a = np.asarray(img).copy()
        for x0, y0, x1, y1 in mask:
            ref = a[max(0, y0 - 6):y0, x0:x1].reshape(-1, 3)
            a[y0:y1, x0:x1] = np.median(ref, axis=0).astype(np.uint8)
        img = Image.fromarray(a)
    return img.size, img.convert("RGBA").tobytes()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True, help="vjo-capture-parity-tests")
    ap.add_argument("--manifests", required=True, help="vita-vn-ocr eval/linefind/manifests")
    ap.add_argument("--shots", action="append", required=True, help="directory with the screenshots (repeatable)")
    ap.add_argument("--only", choices=DEV_MANIFESTS, action="append", help="one manifest only (default both)")
    args = ap.parse_args(argv)

    names = args.only or list(DEV_MANIFESTS)
    regions, excluded, frames = [], 0, {}
    with tempfile.TemporaryDirectory(prefix="vjo-parity-") as tmp:
        for name in names:
            path = Path(args.manifests) / name
            if not path.exists():
                print(f"capture parity: {path} missing", file=sys.stderr)
                return 2
            for line in path.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                row = json.loads(line)
                if protected(row):
                    excluded += 1
                    continue
                shot = find_shot(row["shot"], args.shots)
                if shot is None:
                    print(f"capture parity: {row['shot']} not found in {args.shots}", file=sys.stderr)
                    return 2
                if row["shot"] not in frames:
                    if row.get("sha256") and hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                        print(f"capture parity: {shot} does not match the manifest's sha256", file=sys.stderr)
                        return 2
                    (w, h), rgba = load_frame(shot, row.get("mask"))
                    if w > 960 or h > 544:
                        print(f"capture parity: {shot} is {w}x{h}, larger than a Vita frame", file=sys.stderr)
                        return 2
                    raw = Path(tmp) / f"{len(frames):04d}.rgba"
                    raw.write_bytes(rgba)
                    frames[row["shot"]] = (raw, w, h)
                raw, w, h = frames[row["shot"]]
                x0, y0, x1, y1 = (int(v) for v in row["region"])
                x0, y0, x1, y1 = max(0, x0), max(0, y0), min(w, x1), min(h, y1)
                if x1 <= x0 or y1 <= y0:
                    continue
                regions.append(f"{raw} {w} {h} {x0} {y0} {x1} {y1}\n")
        if not regions:
            print("capture parity: no region to compare", file=sys.stderr)
            return 2
        listing = Path(tmp) / "regions.txt"
        listing.write_text("".join(regions))
        print(f"capture parity: {len(regions)} regions of {len(frames)} screenshots from {', '.join(names)}"
              f" ({excluded} protected rows excluded), 3 formats each")
        env = dict(os.environ, VJO_PARITY_LIST=str(listing))
        run = subprocess.run([args.binary, TEST], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True)
        sys.stdout.write(run.stdout)
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
