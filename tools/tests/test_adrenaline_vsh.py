"""The optional Shell bridge must not regain its old resident libc state."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_adrenaline_vsh as bridge  # noqa: E402


class BridgeBudget(unittest.TestCase):
    def self_file(self, data_size):
        blob = bytearray(512)
        blob[:4] = b"SCE\0"
        struct.pack_into("<QQ", blob, 0x40, 0x80, 0xC0)
        blob[0x80:0x86] = b"\x7fELF\x01\x01"
        struct.pack_into("<HH", blob, 0x80 + 42, 32, 2)
        for i, size in enumerate((1604, data_size)):
            struct.pack_into("<8I", blob, 0xC0 + i * 32, 1, 0, 0, 0, size, size, 0, 4096)
        return blob

    def test_page_budget_includes_bss(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "bridge.suprx"
            path.write_bytes(self.self_file(44))
            self.assertEqual(bridge.check(path)["pages"], 8192)
            path.write_bytes(self.self_file(280300))
            with self.assertRaisesRegex(ValueError, "limit 8192"):
                bridge.check(path)

    def test_not_a_self(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "bridge.suprx"
            path.write_bytes(b"not a plugin")
            with self.assertRaisesRegex(ValueError, "not a Vita SELF"):
                bridge.check(path)
