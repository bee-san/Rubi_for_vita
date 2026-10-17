#!/usr/bin/env python3
"""The film's music bed, 45.0 s: promo-assets' bed-44s with one more second in its breakdown.

    python3 tools/make_bed.py BED_44S.wav assets/audio/bed-45s.m4a

bed-44s (CC0, promo-assets' src/make_audio.py: 22 bars at 120 BPM in D major, a breakdown 23.5-31.0 s
under the install, the second lift on 31.0 s, the last chord on 41.5 s) is cut at 29.5 s and its
28.5-29.5 s, two beats of the breakdown's D bar (the keys' quarter notes and the pad's sustain, no
drums, no melody), go in again there: that bar holds six beats instead of four. Everything from the
old 29.5 s on is unchanged and comes 1.0 s later: the riser into the second lift, which starts on
the old 29.5 s, the lift on 32.0 s, the last chord on 42.5 s, the fade to silence over 44.2-45.0 s.
The seam is an equal-power crossfade of 40 ms. Reads the 24-bit WAV and writes AAC 192 kb/s 48 kHz
through ffmpeg (FFMPEG). Deterministic: the same input makes the same file."""
import os
import subprocess
import sys

import numpy as np

SR = 48000
CUT, COPY, XF = 29.5, 1.0, 0.040  # the seam, the repeated stretch (before it), the crossfade


def main():
    src, out = sys.argv[1], sys.argv[2]
    ff = os.environ.get("FFMPEG", "ffmpeg")
    raw = subprocess.run([ff, "-v", "error", "-i", src, "-f", "f32le", "-ac", "2", "-ar", str(SR), "-"],
                         capture_output=True, check=True).stdout
    x = np.frombuffer(raw, np.float32).reshape(-1, 2).astype(np.float64)
    if abs(len(x) / SR - 44.0) > 1e-3:
        sys.exit(f"{src}: {len(x) / SR:.3f} s, not bed-44s")
    c, n, f = int(CUT * SR), int(COPY * SR), int(XF * SR)
    head, again, tail = x[:c], x[c - n - f:c].copy(), x[c:]
    # the seam: the head's last 40 ms (29.46-29.5 s) fade out as the 40 ms before the repeat (28.46-28.5 s)
    # fade in, so the repeat's first beat (28.5 s) lands on 29.5 s
    t = np.linspace(0, np.pi / 2, f)[:, None]
    seam = head[-f:] * np.cos(t) + again[:f] * np.sin(t)
    y = np.concatenate([head[:-f], seam, again[f:], tail])
    if abs(len(y) / SR - 45.0) > 1e-3:
        sys.exit(f"the bed is {len(y) / SR:.3f} s")
    subprocess.run([ff, "-v", "error", "-y", "-f", "f32le", "-ar", str(SR), "-ac", "2", "-i", "-",
                    "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", "-map_metadata", "-1",
                    "-fflags", "+bitexact", "-flags:a", "+bitexact", out],
                   input=y.astype(np.float32).tobytes(), check=True)
    print(f"{out}: {len(y) / SR:.3f} s")


if __name__ == "__main__":
    main()
