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


SHELL = ("vjoGetVersion", "vjoRegisterShell", "vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel", "vjoSetTextSource")
WORKER = tuple(n for n in abi.GAME_FUNCTIONS if n not in ("vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel")) + \
    ("vjoGetTextSource",)
KERNEL = tuple(dict.fromkeys(SHELL + abi.GAME_FUNCTIONS + ("vjoGetTextSource",)))


def audit(kernel=(), shell=(), worker=(), with_worker=True, drop=()):
    def keep(names):
        return tuple(n for n in names if n not in drop)
    return abi.audit(Fake("VitaJPOverlay_Kernel", keep(KERNEL) + kernel, True),
                     Fake("VitaJPOverlay_Shell", keep(SHELL) + shell, False),
                     False, Fake("VitaJPOverlay_OCR", keep(WORKER) + worker, False) if with_worker else None)


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


class TextSource(unittest.TestCase):
    """API 31: the kernel exports vjoSetTextSource and vjoGetTextSource, the Shell imports Set (it pushes
    config.ini's text_source), the worker imports Get (read as its game starts) and never Set."""

    def test_the_kernel_exports_both(self):
        for name in ("vjoSetTextSource", "vjoGetTextSource"):
            with self.subTest(name=name), self.assertRaises(abi.InvalidABI) as e:
                audit(drop=(name,), with_worker=False)
            self.assertIn(name, str(e.exception))

    def test_the_shell_imports_set(self):
        with self.assertRaises(abi.InvalidABI) as e:
            abi.audit(Fake("VitaJPOverlay_Kernel", KERNEL, True),
                      Fake("VitaJPOverlay_Shell", tuple(n for n in SHELL if n != "vjoSetTextSource"), False),
                      False, None)
        self.assertEqual(str(e.exception), "Shell does not import vjoSetTextSource")

    def test_the_worker_imports_get_and_never_set(self):
        with self.assertRaises(abi.InvalidABI) as e:
            abi.audit(Fake("VitaJPOverlay_Kernel", KERNEL, True), Fake("VitaJPOverlay_Shell", SHELL, False), False,
                      Fake("VitaJPOverlay_OCR", tuple(n for n in WORKER if n != "vjoGetTextSource"), False))
        self.assertEqual(str(e.exception), "Worker does not import vjoGetTextSource")
        with self.assertRaises(abi.InvalidABI) as e:
            audit(worker=("vjoSetTextSource",))
        self.assertEqual(str(e.exception), "Worker imports Shell-only syscall vjoSetTextSource")


class HookCache(unittest.TestCase):
    def test_worker_import_and_kernel_export_are_required(self):
        with self.assertRaises(abi.InvalidABI) as e:
            audit(drop=("vjoOcrHookCache",))
        self.assertIn("vjoOcrHookCache", str(e.exception))
        with self.assertRaises(abi.InvalidABI) as e:
            abi.audit(Fake("VitaJPOverlay_Kernel", KERNEL, True),
                      Fake("VitaJPOverlay_Shell", SHELL, False), False,
                      Fake("VitaJPOverlay_OCR", tuple(n for n in WORKER if n != "vjoOcrHookCache"), False))
        self.assertIn("vjoOcrHookCache", str(e.exception))

    def test_shell_does_not_import_the_cache_broker(self):
        with self.assertRaises(abi.InvalidABI) as e:
            audit(shell=("vjoOcrHookCache",))
        self.assertEqual(str(e.exception), "Shell imports the game worker's vjoOcrHookCache")



class HookLine(unittest.TestCase):
    """API 41: the hook watch's report is the worker's import and the kernel's export, never the Shell's."""

    def test_worker_import_and_kernel_export_are_required(self):
        with self.assertRaises(abi.InvalidABI) as e:
            audit(drop=("vjoOcrHookLine",))
        self.assertIn("vjoOcrHookLine", str(e.exception))
        with self.assertRaises(abi.InvalidABI) as e:
            abi.audit(Fake("VitaJPOverlay_Kernel", KERNEL, True),
                      Fake("VitaJPOverlay_Shell", SHELL, False), False,
                      Fake("VitaJPOverlay_OCR", tuple(n for n in WORKER if n != "vjoOcrHookLine"), False))
        self.assertIn("vjoOcrHookLine", str(e.exception))

    def test_shell_does_not_import_it(self):
        with self.assertRaises(abi.InvalidABI) as e:
            audit(shell=("vjoOcrHookLine",))
        self.assertEqual(str(e.exception), "Shell imports the game worker's vjoOcrHookLine")

if __name__ == "__main__":
    unittest.main()
