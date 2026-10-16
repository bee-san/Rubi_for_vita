"""tools/check_offline_imports.py on minimal ELFs: a network library's stubs fail the check."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_offline_imports as c  # noqa: E402


def elf(names):
    """An ELF32 (ARM, little-endian) with empty PROGBITS sections of these names and a .shstrtab."""
    strtab = b"\0"
    offsets = []
    for n in list(names) + [".shstrtab"]:
        offsets.append(len(strtab))
        strtab += n.encode() + b"\0"
    shoff = 52 + len(strtab)
    shnum = len(names) + 2
    head = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    head += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0, 0, shoff, 0, 52, 0, 0, 40, shnum, shnum - 1)
    sections = bytes(40)
    for off in offsets[:-1]:
        sections += struct.pack("<IIIIIIIIII", off, 1, 6, 0, 0, 0, 0, 0, 4, 0)
    sections += struct.pack("<IIIIIIIIII", offsets[-1], 3, 0, 0, 52, len(strtab), 0, 0, 1, 0)
    return head + strtab + sections


class OfflineImports(unittest.TestCase):
    def run_check(self, *names):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "module"
            path.write_bytes(elf(names))
            return subprocess.run([sys.executable, str(ROOT / "tools/check_offline_imports.py"), str(path)],
                                  capture_output=True, text=True, timeout=30)

    def test_the_offline_modules_pass(self):
        r = self.run_check(".text", ".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs.SceIofilemgr",
                           ".vitalink.vstubs.ScePafStdc", ".rodata")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("3 import libraries, no network library", r.stdout)

    def test_a_network_library_fails(self):
        for lib in ("SceNet", "SceNetCtl", "SceNetPs", "SceHttp", "SceSsl"):
            with self.subTest(lib=lib):
                r = self.run_check(".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs." + lib)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("imports " + lib + ": the overlay is offline", r.stdout)
        r = self.run_check(".vitalink.vstubs.SceNet")  # a variable import counts too
        self.assertEqual(r.returncode, 1)

    def test_names_are_read_from_the_section_table(self):
        self.assertEqual(c.imported_libraries(elf([".vitalink.fstubs.SceSysmem", ".text"])), ["SceSysmem"])
        self.assertEqual(c.section_names(elf([".a"])), ["", ".a", ".shstrtab"])

    def run_files(self, *contents):
        with tempfile.TemporaryDirectory() as folder:
            paths = []
            for i, data in enumerate(contents):
                path = Path(folder) / f"module{i}"
                path.write_bytes(data)
                paths.append(str(path))
            return subprocess.run([sys.executable, str(ROOT / "tools/check_offline_imports.py"), *paths],
                                  capture_output=True, text=True, timeout=30)

    def test_a_clean_module_after_a_network_one_still_fails(self):
        # the Vita build checks four modules in one call: a later clean one must not reset the verdict
        bad, good = elf([".vitalink.fstubs.SceNet"]), elf([".vitalink.fstubs.SceLibKernel"])
        self.assertEqual(self.run_files(bad, good).returncode, 1)
        self.assertEqual(self.run_files(good, bad).returncode, 1)
        self.assertEqual(self.run_files(good, good).returncode, 0)

    def test_no_module_is_a_usage_error(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools/check_offline_imports.py")], capture_output=True,
                           text=True, timeout=30)
        self.assertEqual(r.returncode, 2)

    def test_another_elf_class_or_a_cut_header_is_an_error(self):
        good = elf([".vitalink.fstubs.SceLibKernel"])
        elf64 = good[:4] + bytes([2]) + good[5:]  # ELFCLASS64
        big = good[:5] + bytes([2]) + good[6:]    # big-endian
        for data in (elf64, big, good[:4], good[:5], good[:6], good[:40], good[:60]):
            with self.subTest(size=len(data)):
                r = self.run_files(data)
                self.assertEqual(r.returncode, 2, r.stderr)  # 1 is "imports a network library"
                self.assertNotIn("Traceback", r.stderr)

    def test_a_file_that_is_no_elf_is_an_error(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "x"
            path.write_bytes(b"SCE\0" + bytes(60))
            r = subprocess.run([sys.executable, str(ROOT / "tools/check_offline_imports.py"), str(path)],
                               capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
