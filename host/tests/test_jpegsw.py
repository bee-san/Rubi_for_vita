"""The Anki screenshot's JPEG encoder (core/jpegsw.c, the game worker's): every
output decodes with Pillow, at the size it was given, close to its source.

    python3 host/tests/test_jpegsw.py BUILD/vjo-jpeg

The pictures are procedural (gradients, text-like strokes, noise, odd sizes) and
docs/screenshot.jpg, a 960 x 544 picture of a game with the overlay (committed in
the repository; the private game frames docs/anki.md measures are not used here).
"""
import io
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

TOOL = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[2]


def psnr(a, b):
    """PSNR (dB) of two RGB images of one size."""
    pa, pb = a.tobytes(), b.tobytes()
    err = sum((x - y) * (x - y) for x, y in zip(pa, pb)) / len(pa)
    return 99.0 if not err else 10 * math.log10(255 * 255 / err)


def markers(data):
    """The marker segments up to the scan: [(marker, payload)]."""
    out, i = [], 2
    assert data[:2] == b"\xff\xd8", "no SOI"
    while i < len(data):
        assert data[i] == 0xFF, f"no marker at {i}"
        m = data[i + 1]
        n = struct.unpack(">H", data[i + 2:i + 4])[0]
        out.append((m, data[i + 4:i + 2 + n]))
        if m == 0xDA:
            break
        i += 2 + n
    return out


def stripes(w, h):
    """Dark strokes on a light gradient, as a textbox's text on a background."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            ink = (x // 3) % 7 < 2 and (y // 5) % 4 < 3
            px[x, y] = (20, 20, 30) if ink else (200 + x * 50 // max(w, 1), 180 + y * 60 // max(h, 1), 160)
    return im


def noise(w, h, seed=7):
    """Pseudo-random colour noise, the worst case for a JPEG's size."""
    v, data = seed, bytearray()
    for _ in range(w * h * 3):
        v = (v * 1103515245 + 12345) & 0x7FFFFFFF
        data.append((v >> 16) & 0xFF)
    return Image.frombytes("RGB", (w, h), bytes(data))


class Encoder(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vjo-jpeg-")
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def encode(self, im, quality, arena=None):
        raw = self.dir / "in.rgba"
        out = self.dir / "out.jpg"
        raw.write_bytes(im.convert("RGBA").tobytes())
        cmd = [TOOL, str(raw), str(im.width), str(im.height), str(quality), str(out)] + ([str(arena)] if arena else [])
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if done.returncode:
            return None, done
        size, peak = map(int, done.stdout.split())
        data = out.read_bytes()
        self.assertEqual(len(data), size)
        return data, peak

    def decoded(self, data, im):
        got = Image.open(io.BytesIO(data))
        got.load()  # every scan byte decoded
        self.assertEqual((got.format, got.mode, got.size), ("JPEG", "RGB", im.size))
        return got

    def test_a_game_screen_decodes_close_to_its_source_at_each_quality(self):
        screen = Image.open(ROOT / "docs/screenshot.jpg").convert("RGB")
        self.assertEqual(screen.size, (960, 544))
        sizes = {}
        for quality, floor in ((35, 27.0), (50, 28.5), (70, 30.0), (95, 33.0)):
            data, peak = self.encode(screen, quality)
            got = self.decoded(data, screen)
            p = psnr(screen, got)
            sizes[quality] = len(data)
            self.assertGreater(p, floor, f"q{quality}: {p:.1f} dB")
            # within 15 % of what libjpeg (Pillow, the same tables, no optimisation) makes of it
            ref = io.BytesIO()
            screen.save(ref, "JPEG", quality=quality, subsampling=2, optimize=False)
            self.assertLess(abs(len(data) - len(ref.getvalue())) / len(ref.getvalue()), 0.15,
                            f"q{quality}: {len(data)} B against Pillow's {len(ref.getvalue())} B")
            print(f"measure: docs/screenshot.jpg q{quality}: {len(data)} B, {p:.2f} dB, encoder peak {peak} B")
        self.assertLess(sizes[35], sizes[50])
        self.assertLess(sizes[50], sizes[70])
        self.assertLess(sizes[70], sizes[95])

    def test_baseline_420_with_the_standard_tables(self):
        data, _ = self.encode(stripes(64, 48), 70)
        segs = markers(data)
        kinds = [m for m, _ in segs]
        self.assertEqual(kinds, [0xE0, 0xDB, 0xDB, 0xC0, 0xC4, 0xC4, 0xC4, 0xC4, 0xDA])
        sof = dict(segs)[0xC0]
        # 8-bit, 48 x 64, three components: Y 2x2 (4:2:0) on table 0, Cb and Cr 1x1 on table 1
        self.assertEqual(sof, bytes([8, 0, 48, 0, 64, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1]))
        self.assertEqual(data[-2:], b"\xff\xd9")
        self.assertEqual(dict(segs)[0xE0][:5], b"JFIF\x00")

    def test_odd_sizes_repeat_their_edges(self):
        # widths and heights that are no multiple of 16 (the last band and MCU column repeat their edge)
        for w, h in ((1, 1), (17, 9), (31, 33), (960, 544), (720, 408), (480, 272), (15, 16)):
            im = stripes(w, h)
            data, _ = self.encode(im, 70)
            self.assertIsNotNone(data, f"{w}x{h}")
            got = self.decoded(data, im)
            if w * h >= 256:
                self.assertGreater(psnr(im, got), 24.0, f"{w}x{h}")

    def test_flat_colours_are_exact_enough(self):
        for colour in ((0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 128, 255)):
            im = Image.new("RGB", (48, 32), colour)
            got = self.decoded(self.encode(im, 70)[0], im)
            for c, want in zip(got.getpixel((20, 10)), colour):
                self.assertLessEqual(abs(c - want), 4, colour)

    def test_noise_is_the_worst_case_and_still_decodes(self):
        im = noise(256, 128)
        data, _ = self.encode(im, 70)
        self.decoded(data, im)
        print(f"measure: 256x128 noise q70: {len(data)} B ({len(data) / (256 * 128):.2f} B a pixel)")

    def test_a_full_arena_fails_instead_of_overflowing(self):
        # the game worker's block bounds the JPEG: past it the encoder stops (out of memory), and
        # with room for state + band + JPEG it succeeds
        im = noise(960, 64)
        self.assertIsNone(self.encode(im, 95, arena=4096 + 960 * 4 * 16 + 1024)[0])
        data, peak = self.encode(im, 95, arena=4 * 1024 * 1024)
        self.assertIsNotNone(data)
        # the state, the band and the JPEG's buffer, which doubles while it is the arena's last
        # allocation (in the worker's exact block it grows by what it needs once it cannot)
        self.assertLessEqual(peak, 4096 + 16 + 960 * 4 * 16 + 2 * len(data) + 512)
        exact, _ = self.encode(im, 95, arena=4096 + 16 + 960 * 4 * 16 + len(data) + 64)
        self.assertEqual(exact, data)

    def test_quality_is_clamped_to_1_95(self):
        im = stripes(32, 32)
        top, _ = self.encode(im, 100)
        q95, _ = self.encode(im, 95)
        self.assertEqual(top, q95)  # 100 would need DC codes past the standard tables
        low, _ = self.encode(im, 0)
        q1, _ = self.encode(im, 1)
        self.assertEqual(low, q1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
