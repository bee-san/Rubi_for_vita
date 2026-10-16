"""tools/check_vita_syscall_abi.py's removed syscalls (REMOVED): a kernel that exports one, or a Shell or worker
that imports one, fails the audit with the API that removed it (audit-overlay-shell-slim-5 N2)."""
import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_vita_syscall_abi as abi  # noqa: E402


def nid(name):
    return int.from_bytes(hashlib.sha256(name.encode()).digest()[:4], "big")


class Fake:
    """Just what audit() reads of a SelfFile: the name, one custom library and an executable segment."""

    def __init__(self, name, names, exports):
        self.name, self.exports = name, exports
        self.symbols = {nid(n): (0, 0x100 + 16 * i + (1 if exports else 0)) for i, n in enumerate(names)}
        self.segments = [(0, 0x10000, 5, b"\0" * 0x10000)]
        self.sha256 = "0" * 64

    def check_firmware_imports(self):
        pass

    def library(self, exports):
        return 1, (0x4001 if exports else 8), self.symbols

    def read(self, segment, offset, size):
        return b"\0" * size if self.exports else struct.pack("<4I", *abi.WEAK_STUB)[:size]


SHELL = ("vjoGetVersion", "vjoRegisterShell", "vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel")
WORKER = tuple(n for n in abi.GAME_FUNCTIONS if n not in ("vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel"))
KERNEL = tuple(dict.fromkeys(SHELL + abi.GAME_FUNCTIONS))


def audit(kernel=(), shell=(), worker=(), with_worker=True):
    return abi.audit(Fake("VitaJPOverlay_Kernel", KERNEL + kernel, True), Fake("VitaJPOverlay_Shell", SHELL + shell, False),
                     False, Fake("VitaJPOverlay_OCR", WORKER + worker, False) if with_worker else None)


class RemovedSyscalls(unittest.TestCase):
    def test_the_current_set_passes(self):
        self.assertEqual(audit()["result"], "pass")

    def test_a_removed_export_or_import_fails_with_its_api(self):
        for name, api in (("vjoRequestCapture", 24), ("vjoReadRaw", 24), ("vjoOcrReadRaw", 19)):
            # the kernel exports it; the worker imports it; the Shell imports it, with or without the kernel's export;
            # and the kernel's and the Shell's in the audit without the worker (the build's vjo_syscall_abi)
            for case in ({"kernel": (name,)}, {"worker": (name,)}, {"kernel": (name,), "shell": (name,)},
                         {"shell": (name,)}, {"kernel": (name,), "with_worker": False}, {"shell": (name,), "with_worker": False},
                         {"kernel": (name,), "shell": (name,), "with_worker": False}):
                with self.subTest(name=name, case=sorted(case)), self.assertRaises(abi.InvalidABI) as e:
                    audit(**case)
                self.assertEqual(str(e.exception), f"{name} was removed in API {api}")
        self.assertEqual(audit(with_worker=False)["result"], "pass")


if __name__ == "__main__":
    unittest.main()
