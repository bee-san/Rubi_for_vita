"""tools/audio.py SFX_JSON OUT.wav: the promo's soundtrack.

Mixes promo-assets' music bed (bed-44s, CC0, 120 BPM) with the SFX events the timeline lists (one per
button press, card, panel and typed pair of characters), each placed at its frame time. Loudness is set
afterwards by tools/encode.sh (FFmpeg loudnorm, two passes, -16 LUFS, true peak -1.5 dBTP).
"""
import json
import sys
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
A = ROOT / "assets" / "shared" / "audio"
SR = 48000


def read(path):
    with wave.open(str(path)) as w:
        assert w.getframerate() == SR and w.getsampwidth() == 3, path
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8).reshape(-1, 3)
        v = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / (1 << 23)
        return v.reshape(-1, w.getnchannels())


def write(path, x):
    x = np.clip(x, -1, 1 - 1 / (1 << 23))
    v = np.round(x * (1 << 23)).astype(np.int32).reshape(-1)
    b = np.stack([(v & 0xFF), (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=1).astype(np.uint8).tobytes()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2); w.setsampwidth(3); w.setframerate(SR); w.writeframes(b)


def main():
    events = json.loads(Path(sys.argv[1]).read_text())
    bed = read(A / "music" / "bed-44s.wav")
    # the bed's breakdown (23.5-31 s, the install) sits 5-6 LU under the rest: lift it so it
    # stays a breath, not a drop-out (5 dB); smooth ramps keep the lift on L + R at 31 s
    t = np.arange(len(bed)) / SR
    ramp = lambda a, b: np.clip((t - a) / (b - a), 0, 1)
    env = ramp(23.6, 24.3) * (1 - ramp(30.55, 30.95))
    env = env * env * (3 - 2 * env)
    gain = 1 + (10 ** (5.0 / 20) - 1) * env
    mix = bed * 0.82 * gain[:, None]
    cache = {}
    for e in events:
        name = e["name"]
        if name not in cache:
            cache[name] = read(A / "sfx" / f"{name}.wav")
        s = cache[name] * e["gain"] * 0.9
        i = int(round(e["t"] * SR))
        n = min(len(s), len(mix) - i)
        if n > 0:
            mix[i:i + n] += s[:n]
    write(sys.argv[2], mix)
    print(f"{sys.argv[2]}: {len(mix) / SR:.3f} s, {len(events)} events, peak {np.abs(mix).max():.3f}")


if __name__ == "__main__":
    main()
