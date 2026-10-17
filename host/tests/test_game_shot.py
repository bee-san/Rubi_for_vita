"""API 23, the Anki screenshot through the actual game worker and kernel transport
(host/tests/test_game_shot.c): its unit cases, then docs/screenshot.jpg (a 960 x 544
picture of a game with the overlay) shown by the "game" in each pixel format a game
submits (A8B8G8R8, A2B10G10R10, A1B5G5R5 as Adrenaline's), shot, and the JPEG the
kernel wrote beside the card decoded by Pillow and compared with the picture.

    python3 host/tests/test_game_shot.py BUILD/vjo-game-shot-tests
"""
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[2]


def psnr(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    err = sum((x - y) * (x - y) for x, y in zip(pa, pb)) / len(pa)
    return 99.0 if not err else 10 * math.log10(255 * 255 / err)


class GameShot(unittest.TestCase):
    def test_unit_cases(self):
        done = subprocess.run([BINARY], capture_output=True, text=True, timeout=600)
        print(done.stdout[-2000:])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_a_game_screen_in_every_format_decodes_close_to_it(self):
        screen = Image.open(ROOT / "docs/screenshot.jpg").convert("RGB")
        self.assertEqual(screen.size, (960, 544))
        for fmt, floor in (("abgr", 35.0), ("a2b10", 35.0), ("a1b5", 30.0)):
            with self.subTest(format=fmt), tempfile.TemporaryDirectory(prefix="vjo-shot-") as tmp:
                raw = Path(tmp) / "frame.rgba"
                raw.write_bytes(screen.convert("RGBA").tobytes())
                done = subprocess.run([BINARY, "--shot", str(raw), "960", "544", fmt, tmp], capture_output=True,
                                      text=True, timeout=300)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                r = json.loads(done.stdout.strip().splitlines()[-1])
                # #194: the whole frame in one rows call, copied in one display call of the game
                self.assertEqual((r["rc"], r["quality"], r["tries"], r["strips"]), (0, 70, 1, 1))
                jpg = Path(r["path"])
                data = jpg.read_bytes()
                self.assertEqual(len(data), r["bytes"])
                self.assertLessEqual(len(data), 192 * 1024)
                got = Image.open(jpg)
                got.load()
                self.assertEqual((got.format, got.mode, got.size), ("JPEG", "RGB", (960, 544)))
                # A1B5G5R5 holds 5 bits a channel: compare with the picture as that format shows it
                ref = screen if fmt != "a1b5" else screen.point(lambda c: (c >> 3) << 3)
                p = psnr(ref, got)
                self.assertGreater(p, floor, f"{fmt}: {p:.2f} dB")
                print(f"measure: game shot of docs/screenshot.jpg ({fmt}): {len(data)} B, {p:.2f} dB, "
                      f"block {r['block']} B, {r['strips']} strips")
                self.assertEqual(sorted(p.name for p in jpg.parent.iterdir()), [jpg.name])  # no .tmp left


if __name__ == "__main__":
    unittest.main(verbosity=2)
