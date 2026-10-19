#!/usr/bin/env python3
"""Scan screen (API 30): the game worker's scan through the kernel's display hook and strips
(host/tests/test_game_spot.c --spot) gives vita-vn-ocr's twin's boxes, blocks, pick and truncated flag on the
same frames (tools/spot_frames.py), as host/fixtures/textspot/spot-<the pinned weights' sha256>.json records
them (tools/spot_fixture.py writes it). Then the overlay's map (core/spot_pick.h) of each: at most 16 blocks, each
inside the screen, its neighbours' indices among them. Skips (exit 77) without the pinned weights
(cmake/VocrSpot.cmake). Pillow decodes the sample; a frame whose bytes differ from the fixture's fails as such."""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import spot_frames  # noqa: E402


def main():
    tool = sys.argv[1]
    sha = re.search(r"set\(VJO_VOCR_SPOT_SHA256 ([0-9a-f]{64})\)", (ROOT / "cmake" / "VocrSpot.cmake").read_text()).group(1)
    fixture = ROOT / "host" / "fixtures" / "textspot" / f"spot-{sha[:16]}.json"
    if not fixture.exists():
        sys.exit(f"{fixture}: no fixture for the pinned weights {sha}: run tools/spot_fixture.py")
    want = {f["name"]: f for f in json.loads(fixture.read_text())["frames"]}
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, w, h, data in spot_frames.frames():
            f = want[name]
            if spot_frames.sha256(data) != f["rgba_sha256"] or [w, h] != f["size"]:
                print(f"{name}: the frame is not the fixture's (this Pillow decodes the sample otherwise)")
                failed += 1
                continue
            path = Path(tmp) / f"{name}.rgba"
            path.write_bytes(data)
            run = subprocess.run([tool, "--spot", str(path), str(w), str(h)], capture_output=True, text=True,
                                 timeout=120)
            line = [ln for ln in run.stdout.splitlines() if ln.startswith("{")]
            if not line:
                print(f"{name}: no result (exit {run.returncode}): {run.stdout[-400:]} {run.stderr[-400:]}")
                failed += 1
                continue
            got = json.loads(line[-1])
            if not got["weights"]:
                print("skipped: no spotter weights (cmake/VocrSpot.cmake)")
                sys.exit(77)
            checks = [("rc", got["rc"], 0), ("boxes", got["boxes"], f["boxes"]), ("blocks", got["blocks"], f["blocks"]),
                      ("pick", got["pick"], f["pick"]), ("truncated", got["truncated"], f["truncated"]),
                      ("strips", got["strips"], (h + 63) // 64)]
            m = got["map"]
            shown = min(16, len(f["blocks"]))
            checks.append(("map count", m["count"], shown))
            inside = all(0 <= x and 0 <= y and w2 > 0 and h2 > 0 and x + w2 <= 960 and y + h2 <= 544
                         for x, y, w2, h2 in m["rect"])
            checks.append(("map inside the screen", inside, True))
            checks.append(("map neighbours", all(0 <= n < shown for row in m["next"] for n in row), True))
            for what, a, b in checks:
                if a != b:
                    print(f"{name}: {what}: {a} != {b}")
                    failed += 1
            print(f"{name} {w}x{h}: {len(got['boxes'])} boxes, {len(got['blocks'])} blocks, pick {got['pick']}, "
                  f"truncated {got['truncated']}, {got['strips']} strips, arena {got['arena']} B, "
                  f"map {m['count']} blocks from {m['start']}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
