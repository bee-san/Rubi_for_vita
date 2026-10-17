#!/usr/bin/env python3
"""The film's music bed, 42.0 s: promo-assets' bed with one bar fewer in its breakdown.

    python3 tools/make_bed.py PROMO_ASSETS/src/make_audio.py assets/audio/bed-42s.m4a

promo-assets' src/make_audio.py (CC0) synthesises bed-44s from code: 22 bars at 120 BPM in D major, a lift
(riser, bell shimmer, drums and bass in) on 4.5 s and on 31.0 s, a breakdown 23.5-31.0 s, the last chord on
41.5 s. This runs that same code with its bar 13 (Em, the breakdown's first bar) left out, so everything after
it comes one bar (2.0 s) sooner: the breakdown is 23.5-29.0 s (A, the groove's last bar, then A, D, Bm), the
second lift on 29.0 s, melody B 29.0-39.5 s, the last chord on 39.5 s, the fade to silence over 41.2-42.0 s.
Nothing is spliced: the bed is synthesised whole, its reverb included. Its loudness rule is bed-44s's
(-18 LUFS integrated, the true peak at most -1.5 dBTP). Writes AAC 192 kb/s 48 kHz through ffmpeg (FFMPEG).
Needs numpy. Deterministic: the same input makes the same file.
"""
import importlib.util
import os
import subprocess
import sys

import numpy as np

DROP = 13  # the bar (1-based) left out: Em, the breakdown's first bar (beats 48-51)


def main():
    src, out = sys.argv[1], sys.argv[2]
    spec = importlib.util.spec_from_file_location("make_audio", src)
    ma = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ma)
    # bed-44s's plan, checked, so that another make_audio.py stops here instead of making another bed
    if not (ma.BPM == 120 and ma.BED_SECONDS == 44.0 and len(ma.PROG) == 22 and ma.PROG[DROP - 1] == "Em"
            and ma.LIFTS == (9, 62) and ma.GROOVE == ((9, 47), (62, 83)) and ma.FINAL == 83):
        sys.exit(f"{src}: not the make_audio.py of bed-44s")
    a, b = (DROP - 1) * 4, DROP * 4  # the dropped bar's beats
    if any(a <= sb < b for sb, _, _ in ma.MELODY):
        sys.exit("a melody note starts in the dropped bar")
    sooner = lambda beat: beat - 4 if beat >= b else beat  # noqa: E731 (beats after the bar come a bar sooner)
    ma.PROG = ma.PROG[:DROP - 1] + ma.PROG[DROP:]
    ma.LIFTS = tuple(sooner(x) for x in ma.LIFTS)
    ma.GROOVE = tuple((sooner(x), sooner(y)) for x, y in ma.GROOVE)
    ma.FINAL = sooner(ma.FINAL)
    ma.MELODY = [(sooner(sb), m, nb) for sb, m, nb in ma.MELODY]
    ma.BED_SECONDS = 2.0 * len(ma.PROG)
    assert ma.LIFTS == (9, 58) and ma.GROOVE == ((9, 47), (58, 79)) and ma.FINAL == 79 and ma.BED_SECONDS == 42.0
    # the bed as make_audio.py's main() makes bed-44s: the parts, their reverb, the last 0.8 s eased to silence
    sr = ma.SR
    n = int(ma.BED_SECONDS * sr)
    bed = sum(ma.mix_parts(ma.music(list(range(1, len(ma.PROG) + 1)), n)).values())
    tail = int(0.8 * sr)
    bed[:, -tail:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(tail) / tail)
    bed[:, -int(0.02 * sr):] = 0
    li, _ = ma.loudness(bed)
    g = 10 ** ((ma.BED_LUFS - li) / 20)
    tp = ma.true_peak_db(bed * g)
    if tp > ma.TP_MAX:
        g *= 10 ** ((ma.TP_MAX - tp) / 20)
    bed = bed * g
    li, _ = ma.loudness(bed)
    ff = os.environ.get("FFMPEG", "ffmpeg")
    subprocess.run([ff, "-v", "error", "-y", "-f", "f32le", "-ar", str(sr), "-ac", "2", "-i", "-",
                    "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", "-map_metadata", "-1",
                    "-fflags", "+bitexact", "-flags:a", "+bitexact", out],
                   input=bed.T.astype(np.float32).tobytes(), check=True)
    print(f"{out}: {n / sr:.3f} s, {li:.2f} LUFS, true peak {ma.true_peak_db(bed):.2f} dBTP; "
          f"lifts {[x * 0.5 for x in ma.LIFTS]} s, last chord {ma.FINAL * 0.5} s")


if __name__ == "__main__":
    main()
