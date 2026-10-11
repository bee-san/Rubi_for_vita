#!/usr/bin/env python3
"""Round-trips host/samples/*.jpg through the software encoder (vjo-jpegsw)
and reports PSNR against the source; the output must decode with Pillow."""
import math, os, subprocess, sys, tempfile
from PIL import Image

root = os.path.join(os.path.dirname(__file__), "..")
tool = os.environ.get("VJO_JPEGSW", os.path.join(root, "build", "host", "vjo-jpegsw"))
worst = 99.0
for name in sorted(os.listdir(os.path.join(root, "host", "samples"))):
    if not name.endswith(".jpg"):
        continue
    src = Image.open(os.path.join(root, "host", "samples", name)).convert("RGBA")
    w, h = src.size
    with tempfile.TemporaryDirectory() as d:
        raw, out = os.path.join(d, "in.rgba"), os.path.join(d, "out.jpg")
        open(raw, "wb").write(src.tobytes())
        subprocess.run([tool, raw, str(w), str(h), out, "90"], check=True)
        dec = Image.open(out).convert("RGB")
        dec.load()
    a, b = src.convert("RGB").tobytes(), dec.tobytes()
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    worst = min(worst, psnr)
    print(f"{name}: {w}x{h} PSNR {psnr:.1f} dB")

# Failed input/encoding/output paths must return an error without leaking the
# owned pixels or crashing on a failed fopen. Sanitizer builds also check cleanup.
with tempfile.TemporaryDirectory() as d:
    missing, short, pixels, large = (os.path.join(d, name) for name in
                                    ("missing.rgba", "short.rgba", "pixels.rgba", "large.rgba"))
    with open(short, "wb") as f:
        f.write(b"\0" * 4)
    with open(pixels, "wb") as f:
        f.write(bytes((0, 0, 0, 255)) * (16 * 16))
    with open(large, "wb") as f:
        f.write(b"\0" * (5000 * 4))
    output = os.path.join(d, "output.jpg")
    cases = [(missing, 16, 16, output), (short, 2, 2, output),
             (large, 5000, 1, output), (pixels, 16, 16, d)]
    if os.path.exists("/dev/full"):
        cases.append((pixels, 16, 16, "/dev/full"))
    for raw, w, h, output in cases:
        result = subprocess.run([tool, raw, str(w), str(h), output, "90"],
                                capture_output=True, text=True, timeout=15)
        assert result.returncode == 1, result
        assert "Sanitizer" not in result.stderr, result.stderr
    print(f"JPEG error-path checks passed ({len(cases)} cases).")
sys.exit(0 if worst > 30 else 1)
