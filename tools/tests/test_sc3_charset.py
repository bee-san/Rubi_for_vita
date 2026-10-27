"""tools/sc3_charset.py: the SC3 hooks' charset files (third_party/sc3-charsets) and their pins
(core/hook_sc3_pins.inc), the format core/hook_sc3.h reads, the characters it refuses, and its token and decode
rules (core/hook_sc3.c's), on the committed files and the committed farm excerpts
(host/tests/fixtures/hook_sc3_farm.inc).
"""
import hashlib
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import sc3_charset  # noqa: E402

FIXTURES = ROOT / "host" / "tests" / "fixtures" / "hook_sc3_farm.inc"


def c_string(text):
    return bytes(int(h, 16) for h in re.findall(r"\\x([0-9A-F]{2})", text))


class Files(unittest.TestCase):
    def test_the_committed_files_and_pins_agree(self):
        files = sc3_charset.committed()
        self.assertEqual(sorted(files), ["PCSG00146", "PCSG00352", "PCSG00555"])
        self.assertEqual(sc3_charset.PINS.read_text(encoding="utf-8"), sc3_charset.pins_text(files))
        for title, data in files.items():
            name, _, count = sc3_charset.parse(data, title)
            self.assertEqual((name, count), (title, (len(data) - 32) // 2))
            self.assertIn(hashlib.sha256(data).hexdigest(),
                          (ROOT / "third_party/sc3-charsets/README.md").read_text(encoding="utf-8"))
        self.assertEqual({t: len(d) for t, d in files.items()}, {"PCSG00146": 5364, "PCSG00352": 6432,
                                                                 "PCSG00555": 16032})

    def test_build_and_parse(self):
        data = sc3_charset.build("PCSG00146", ["　", "A", None, "漢"], 0x2BAC7A6D)
        self.assertEqual(data[:8], b"vjosc3\x00\x01")
        self.assertEqual(data[8:20], b"PCSG00146\x00\x00\x00")
        self.assertEqual(struct.unpack_from("<III", data, 20), (0x2BAC7A6D, 4, 0))
        self.assertEqual(struct.unpack_from("<4H", data, 32), (0x3000, 0x41, 0, 0x6F22))
        self.assertEqual(sc3_charset.parse(data, "x"), ("PCSG00146", 0x2BAC7A6D, 4))

    def test_what_a_file_never_holds(self):
        for charset in (["AB"], [""], ["\U0001F600"], ["\x00"], [3]):
            with self.assertRaises(ValueError):
                sc3_charset.build("PCSG00146", charset, 1)
        with self.assertRaises(ValueError):
            sc3_charset.build("pcsg00146", ["A"], 1)
        with self.assertRaises(ValueError):  # over the worker's 16 KiB
            sc3_charset.build("PCSG00146", ["A"] * 8177, 1)
        self.assertEqual(len(sc3_charset.build("PCSG00146", ["A"] * 8176, 1)), 16384)
        good = sc3_charset.build("PCSG00146", ["A", "B"], 1)
        for bad in (good[:-1], b"x" + good[1:], good[:20] + b"\x05" + good[21:28] + b"\x01" + good[29:],
                    good[:32] + b"\x00\xd8" + good[34:]):
            with self.assertRaises(ValueError):
                sc3_charset.parse(bad, "bad")

    def test_check_names_a_stale_pin(self):
        with tempfile.TemporaryDirectory() as tmp:
            pins = Path(tmp) / "pins.inc"
            pins.write_text(sc3_charset.pins_text(sc3_charset.committed()).replace("5364u", "5366u"), encoding="utf-8")
            with mock.patch.object(sc3_charset, "PINS", pins), \
                    mock.patch.object(sys, "argv", ["sc3_charset.py", "--check"]):
                self.assertEqual(sc3_charset.main(), 1)
            with mock.patch.object(sys, "argv", ["sc3_charset.py", "--check"]):
                self.assertEqual(sc3_charset.main(), 0)


class Rules(unittest.TestCase):
    TABLE = [0x41, 0x3042] + [0] * 0xFE + [0x6F22] + [0] * 0xFE + [0x5B57]

    def test_tokens(self):
        raw = bytes.fromhex("8100" "00" "11ff00" "0428ff80ff00" "81ff" "ff" "8001ff")
        kinds = [(k, v) for k, v, _, _ in sc3_charset.tokens(raw)]
        self.assertEqual(kinds, [("glyph", 0x100), ("control", 0), ("control", 0x11), ("control", 4),
                                 ("glyph", 0x1FF), ("end", 0xFF)])
        for cut in (b"\x80", b"\x11\xff", b"\x04\x28\xff", b"\x01\x80\x00"):
            with self.assertRaises(ValueError):
                list(sc3_charset.tokens(cut))

    def test_decode(self):
        raw = bytes.fromhex("01800102" "8000" "00" "098100" "0a80018001" "0b" "81ff" "8002" "ff")
        self.assertEqual(sc3_charset.decode(raw, self.TABLE), "あ\nA\n漢字")
        self.assertEqual(sc3_charset.decode(raw[:-3] + b"\xff", self.TABLE, farm=True), "あ\nA\n<漢,ああ>字")
        with self.assertRaises(ValueError):  # the farm's strict codec: an unknown glyph is an error
            sc3_charset.decode(raw, self.TABLE, farm=True)

    def test_the_committed_excerpts_decode_as_written(self):
        files = sc3_charset.committed()
        rows = re.findall(r'\{"(\w+)", "([^"]*)", (\d+), "([^"]*)"\}', FIXTURES.read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(rows), 12)
        for title, raw, n, text in rows:
            raw = c_string(raw)
            table = [v for (v,) in struct.iter_unpack("<H", files[title][32:])]
            self.assertEqual(len(raw), int(n))
            self.assertEqual(sc3_charset.decode(raw, table), c_string(text).decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
