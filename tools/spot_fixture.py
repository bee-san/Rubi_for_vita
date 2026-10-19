#!/usr/bin/env python3
"""Writes scan screen's fixture: vita-vn-ocr's bit-exact Python twin of the text spotter (synth/textspot_twin.py at
the commit cmake/VocrOCR.cmake pins) on tools/spot_frames.py's frames, with the pinned weights, as
host/fixtures/textspot/spot-<the weights' sha256, 16 digits>.json: each frame's sha256 and the spotter's boxes,
blocks, pick and truncated flag (boxes only: no pixels). Run it again whenever the weights' pin changes
(host/tests/test_game_spot.py, which compares the game worker's scan with it, names the file it looks for):

    python3 tools/spot_fixture.py --vocr-source /path/to/vita-vn-ocr --blob ocr/textspot_..._w8.vtsp

Needs numpy and Pillow (vita-vn-ocr's .venv-ci has both). The twin's dark flags are written as 0: a scan reads
no box (VOCR_TEXTSPOT_NO_DARK), and the runtime then leaves every dark flag 0.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import spot_frames  # noqa: E402


def pinned(name):
    text = (ROOT / "cmake" / "VocrSpot.cmake").read_text()
    m = re.search(rf"set\({name} (\S+)\)", text)
    if not m:
        raise SystemExit(f"cmake/VocrSpot.cmake: no {name}")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--vocr-source", required=True, type=Path, help="a vita-vn-ocr checkout at the pinned commit")
    ap.add_argument("--blob", required=True, type=Path, help="the pinned weights file")
    ap.add_argument("--out", type=Path, help="default: host/fixtures/textspot/spot-<sha256[:16]>.json")
    args = ap.parse_args()
    blob = args.blob.read_bytes()
    sha = hashlib.sha256(blob).hexdigest()
    if sha != pinned("VJO_VOCR_SPOT_SHA256"):
        raise SystemExit(f"{args.blob}: sha256 {sha}, not the pinned {pinned('VJO_VOCR_SPOT_SHA256')}")
    commit = re.search(r"set\(VJO_VOCR_COMMIT ([0-9a-f]{40})\)", (ROOT / "cmake" / "VocrOCR.cmake").read_text()).group(1)
    twin_path = args.vocr_source / "synth" / "textspot_twin.py"
    sys.path.insert(0, str(args.vocr_source / "synth"))
    import numpy as np
    import textspot_twin as T
    model = T.Model(blob)
    out = {"kind": "rubi-spot-fixture", "weights": pinned("VJO_VOCR_SPOT_NAME"), "weights_sha256": sha,
           "vita_vn_ocr": commit, "twin_sha256": hashlib.sha256(twin_path.read_bytes()).hexdigest(),
           "note": "vita-vn-ocr synth/textspot_twin.py spot() on tools/spot_frames.py's frames; dark written as 0 "
                   "(a scan reads no box: VOCR_TEXTSPOT_NO_DARK)", "frames": []}
    for name, w, h, data in spot_frames.frames():
        rgb = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 4)[:, :, :3]
        r = T.spot(model, rgb)
        out["frames"].append({
            "name": name, "size": [w, h], "rgba_sha256": spot_frames.sha256(data),
            "boxes": [[b.x, b.y, b.w, b.h, b.vertical, b.conf, 0, b.block] for b in r.boxes],
            "blocks": [[b.x, b.y, b.w, b.h, b.vertical, b.first, b.count] for b in r.blocks],
            "pick": r.pick, "truncated": r.truncated})
        print(f"{name}: {len(r.boxes)} boxes, {len(r.blocks)} blocks, pick {r.pick}, truncated {r.truncated}")
    path = args.out or ROOT / "host" / "fixtures" / "textspot" / f"spot-{sha[:16]}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1) + "\n")
    print(path)


if __name__ == "__main__":
    main()
