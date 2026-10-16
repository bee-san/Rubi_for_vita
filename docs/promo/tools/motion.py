#!/usr/bin/env python3
"""Measure how much a rendered film moves, frame to frame (a check for still holds).

    python3 tools/motion.py FILM.mp4 [--threshold 0.6] [--min-hold 0.75]

Decodes the film at 30 fps as 320 x 180 greyscale through ffmpeg, takes the mean absolute
difference between consecutive frames (0-255), and reports the frames below the threshold
("frozen") and every run of them at least --min-hold seconds long. Needs ffmpeg on PATH or in
$FFMPEG, and numpy.
"""
import argparse
import os
import subprocess

import numpy as np

W, H, FPS = 320, 180, 30


def frames(path):
    ff = os.environ.get("FFMPEG", "ffmpeg")
    cmd = [ff, "-v", "error", "-i", path, "-vf", f"fps={FPS},scale={W}:{H}:flags=area,format=gray",
           "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, H, W).astype(np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("film")
    ap.add_argument("--threshold", type=float, default=0.6)
    ap.add_argument("--min-hold", type=float, default=0.75)
    a = ap.parse_args()
    f = frames(a.film)
    d = np.abs(np.diff(f, axis=0)).mean(axis=(1, 2))
    frozen = d < a.threshold
    print(f"{len(f)} frames, {len(f) / FPS:.2f} s; frozen (diff < {a.threshold}): {frozen.mean() * 100:.1f} %")
    holds, start = [], None
    for i, z in enumerate(np.append(frozen, False)):
        if z and start is None:
            start = i
        elif not z and start is not None:
            if (i - start) / FPS >= a.min_hold:
                holds.append((start / FPS, (i - start) / FPS))
            start = None
    for s, n in holds:
        print(f"  hold {s:6.2f} s for {n:.2f} s")
    print(f"{len(holds)} holds of at least {a.min_hold} s, {sum(n for _, n in holds):.2f} s in all")
    # the slowest second of each scene-ish window, to see where motion is thin
    for t0 in range(0, int(len(d) / FPS)):
        seg = d[t0 * FPS:(t0 + 1) * FPS]
        if len(seg) and seg.mean() < 1.0:
            print(f"  low motion {t0:2d}-{t0 + 1:2d} s: mean diff {seg.mean():.2f}")


if __name__ == "__main__":
    main()
