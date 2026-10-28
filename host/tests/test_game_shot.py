"""API 23, the Anki screenshot through the actual game worker and kernel transport
(host/tests/test_game_shot.c): its unit cases, then docs/screenshot.jpg (a 960 x 544
picture of a game with the overlay) shown by the "game" in each pixel format a game
submits (A8B8G8R8, A2B10G10R10, A1B5G5R5 as Adrenaline's), shot, and the JPEG the
kernel wrote beside the card decoded by Pillow and compared with the picture.

API 32 (anki_screenshot_trim_bars): the same picture shot with its black bars trimmed is
the same JPEG, byte for byte (it has none); shown as Adrenaline shows a PSP game at 1x
(480 x 272 in the middle of the black 960 x 544 frame) its JPEG is the picture and 2 px of
each bar, and at 2x (the whole screen) the whole frame, as without the option.

    python3 host/tests/test_game_shot.py BUILD/vjo-game-shot-tests
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from testlib import psnr

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[2]
FORMATS = (("abgr", 35.0), ("a2b10", 35.0), ("a1b5", 30.0))


def shot(frame, fmt, tmp, trim=False):
    """One shot of frame (a 960 x 544 RGB image) in pixel format fmt: (the result's JSON, the JPEG's bytes)."""
    raw = Path(tmp) / "frame.rgba"
    raw.write_bytes(frame.convert("RGBA").tobytes())
    done = subprocess.run([BINARY, "--shot", str(raw), "960", "544", fmt, tmp] + (["trim"] if trim else []),
                          capture_output=True, text=True, timeout=300)
    if done.returncode:
        raise AssertionError(done.stdout + done.stderr)
    r = json.loads(done.stdout.strip().splitlines()[-1])
    jpg = Path(r["path"])
    data = jpg.read_bytes()
    assert len(data) == r["bytes"], (len(data), r["bytes"])
    assert sorted(p.name for p in jpg.parent.iterdir()) == [jpg.name], "a .tmp was left"
    return r, data


def as_format(im, fmt):
    """The picture as a game in that format shows it (A1B5G5R5 holds 5 bits a channel)."""
    return im if fmt != "a1b5" else im.point(lambda c: (c >> 3) << 3)


class GameShot(unittest.TestCase):
    def test_unit_cases(self):
        done = subprocess.run([BINARY], capture_output=True, text=True, timeout=900)
        print(done.stdout[-3000:])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_a_game_screen_in_every_format_decodes_close_to_it(self):
        screen = Image.open(ROOT / "docs/screenshot.jpg").convert("RGB")
        self.assertEqual(screen.size, (960, 544))
        for fmt, floor in FORMATS:
            with self.subTest(format=fmt), tempfile.TemporaryDirectory(prefix="vjo-shot-") as tmp:
                r, data = shot(screen, fmt, tmp)
                self.assertEqual((r["rc"], r["quality"], r["tries"], r["strips"], r["trim"]), (0, 70, 1, 9, -1))
                self.assertLessEqual(len(data), 192 * 1024)
                got = Image.open(Path(r["path"]))
                got.load()
                self.assertEqual((got.format, got.mode, got.size), ("JPEG", "RGB", (960, 544)))
                p = psnr(as_format(screen, fmt), got)
                self.assertGreater(p, floor, f"{fmt}: {p:.2f} dB")
                print(f"measure: game shot of docs/screenshot.jpg ({fmt}): {len(data)} B, {p:.2f} dB, "
                      f"block {r['block']} B, {r['strips']} strips")

    def test_api32_a_frame_without_bars_is_the_same_jpeg_with_the_option(self):
        # docs/screenshot.jpg's first rows reach its edges: decided by them, read and encoded as without the option
        screen = Image.open(ROOT / "docs/screenshot.jpg").convert("RGB")
        for fmt, _ in FORMATS:
            with self.subTest(format=fmt), tempfile.TemporaryDirectory(prefix="vjo-shot-") as off, \
                    tempfile.TemporaryDirectory(prefix="vjo-shot-") as on:
                r_off, plain = shot(screen, fmt, off)
                r_on, trimmed = shot(screen, fmt, on, trim=True)
                self.assertEqual(plain, trimmed, f"{fmt}: the option changed an untrimmed frame's JPEG")
                self.assertEqual((r_on["trim"], r_on["how"], r_on["w"], r_on["h"], r_on["strips"]), (0, 0, 960, 544, 9))
                self.assertEqual(r_off["strips"], 9)

    def test_api32_adrenaline_at_1x_is_trimmed_and_at_2x_kept(self):
        # a PSP game's 480 x 272 picture as Adrenaline shows it: at 1x in the middle of a black frame, at 2x the
        # whole screen (bilinear, the official emulator's smoothing). docs/screenshot.jpg's own top and bottom rows
        # are dark: at 1x its first rows guess another picture than every row comes to, and the frame is read
        # again (core/shottrim.h); the JPEG is the picture the rule gives (bars(), written apart from the C), with
        # every pixel of the PSP picture in it
        psp = Image.open(ROOT / "docs/screenshot.jpg").convert("RGB").resize((480, 272), Image.LANCZOS)
        one = Image.new("RGB", (960, 544))
        one.paste(psp, (240, 136))
        two = psp.resize((960, 544), Image.BILINEAR)
        for fmt, floor in FORMATS:
            with self.subTest(format=fmt, size="1x"), tempfile.TemporaryDirectory(prefix="vjo-shot-") as tmp:
                frame = as_format(one, fmt)
                want = bars(frame)
                r, data = shot(one, fmt, tmp, trim=True)
                self.assertEqual((r["rc"], r["trim"], (r["x"], r["y"], r["w"], r["h"])), (0, 1, want))
                x, y, w, h = want
                self.assertTrue(x <= 240 and y <= 136 and x + w >= 720 and y + h >= 408, want)  # the picture whole
                self.assertIn(r["how"], (1, 2))
                self.assertEqual(r["strips"] > 9, r["how"] == 2)  # read again only for a wrong guess
                got = Image.open(Path(r["path"]))
                got.load()
                self.assertEqual((got.format, got.mode, got.size), ("JPEG", "RGB", (w, h)))
                p = psnr(frame.crop((x, y, x + w, y + h)), got)
                self.assertGreater(p, 30.0, f"{fmt}: {p:.2f} dB")  # (a 480 x 272 picture: more detail a pixel)
                with tempfile.TemporaryDirectory(prefix="vjo-shot-") as off:
                    _, whole = shot(one, fmt, off)
                print(f"measure: Adrenaline 1x ({fmt}): {len(data)} B trimmed ({w}x{h} at {x},{y}, how {r['how']}, "
                      f"{r['strips']} strips, {p:.2f} dB), {len(whole)} B whole")
            with self.subTest(format=fmt, size="2x"), tempfile.TemporaryDirectory(prefix="vjo-shot-") as off, \
                    tempfile.TemporaryDirectory(prefix="vjo-shot-") as on:
                self.assertEqual(bars(as_format(two, fmt)), (0, 0, 960, 544))
                _, plain = shot(two, fmt, off)
                r, trimmed = shot(two, fmt, on, trim=True)
                self.assertEqual((r["w"], r["h"]), (960, 544))
                self.assertIn(r["trim"], (0, 2))
                self.assertEqual(plain, trimmed, f"{fmt}: 2x changed by the option")

    def test_api32_the_oracle(self):
        # bars() itself, on the cases core/shottrim.h names
        black = Image.new("RGB", (960, 544))
        self.assertEqual(bars(black), (0, 0, 960, 544))  # all black: kept
        im = black.copy()
        im.paste(Image.new("RGB", (480, 272), (90, 90, 90)), (240, 136))
        self.assertEqual(bars(im), (238, 134, 484, 276))
        im = black.copy()
        im.paste(Image.new("RGB", (960, 424), (90, 90, 90)), (0, 60))
        self.assertEqual(bars(im), (0, 58, 960, 428))
        im = black.copy()
        im.paste(Image.new("RGB", (960, 444), (90, 90, 90)), (0, 60))
        self.assertEqual(bars(im), (0, 0, 960, 544))  # bars of 60 and 40: under 3/4
        im = black.copy()
        im.paste(Image.new("RGB", (960, 484), (90, 90, 90)), (0, 60))
        self.assertEqual(bars(im), (0, 0, 960, 544))  # one bar alone
        self.assertEqual(bars(Image.new("RGB", (960, 544), (15, 15, 15))), (0, 0, 960, 544))  # near-black: all bar


def bars(im):
    """The picture between im's black bars by core/shottrim.h's rule, written apart from the C: black is every
    channel under 16; an axis with both bars at least 8 px, neither over 3/10 of the side and the narrower at
    least 3/4 of the wider is trimmed by the narrower less 2 px, rounded down to even; both bars under 8 px: none;
    anything else, or no content at all: the whole frame."""
    w, h = im.size
    px = im.load()
    content = [[max(px[x, y]) >= 16 for x in range(w)] for y in range(h)]
    rows = [y for y in range(h) if any(content[y])]
    if not rows:
        return (0, 0, w, h)
    top, bottom = rows[0], h - 1 - rows[-1]
    left = min(next(x for x in range(w) if content[y][x]) for y in rows)
    right = min(next(x for x in range(w) if content[y][w - 1 - x]) for y in rows)

    def axis(a, b, side):
        lo, hi = min(a, b), max(a, b)
        if hi < 8:
            return 0
        if lo < 8 or hi * 10 > side * 3 or lo * 4 < hi * 3:
            return None
        return (lo - 2) & ~1

    v, x = axis(top, bottom, h), axis(left, right, w)
    if v is None or x is None:
        return (0, 0, w, h)
    return (x, v, w - 2 * x, h - 2 * v)


if __name__ == "__main__":
    unittest.main(verbosity=2)
