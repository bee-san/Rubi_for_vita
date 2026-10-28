"""The optional Shell bridge must not regain its old resident libc state."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_adrenaline_vsh as bridge  # noqa: E402
from self_fixture import Segment, fself  # noqa: E402


class BridgeBudget(unittest.TestCase):
    def self_file(self, data_size):
        """The bridge's code segment (1,604 B) and a data segment of data_size bytes."""
        return fself(Segment(memsz=1604), Segment(memsz=data_size, flags=6))

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
