"""tools/e2e_package.py's own edges; CI's `package` job runs it on the two real release zips."""
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import e2e_package  # noqa: E402


def run(*args):
    return subprocess.run([sys.executable, str(ROOT / "tools/e2e_package.py"), *map(str, args)],
                          capture_output=True, text=True, timeout=120)


class E2EPackageEdges(unittest.TestCase):
    def test_a_base_zip_or_a_file_that_is_not_a_zip_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "VitaJPOverlay-0.8.0-debug.zip"  # a Vita build's own zip: no manifest, no models
            with zipfile.ZipFile(base, "w") as z:
                z.writestr("ur0/tai/VitaJPOverlay_Kernel.skprx", b"SCE\0")
            junk = Path(tmp) / "junk.zip"
            junk.write_bytes(b"not a zip")
            for path, why in ((base, "has no package-manifest.json: a base zip"), (junk, "not a zip"),
                              (Path(tmp) / "missing.zip", "not a zip")):
                with self.subTest(path=path.name):
                    r = run(path, "--work", Path(tmp) / "work")
                    self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                    self.assertIn(why, r.stderr)
                    self.assertFalse((Path(tmp) / "work").exists())

    def test_unpack_keeps_each_files_mode(self):
        """The package's scripts run as they were packed (zipfile's extractall drops the mode bits)."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.zip"
            with zipfile.ZipFile(path, "w") as z:
                for name, mode in (("install-vita.sh", 0o755), ("README.md", 0o644)):
                    info = zipfile.ZipInfo(name)
                    info.external_attr = (0o100000 | mode) << 16
                    z.writestr(info, b"x")
            e2e_package.unpack(path, Path(tmp) / "out")
            self.assertEqual((Path(tmp) / "out/install-vita.sh").stat().st_mode & 0o777, 0o755)
            self.assertEqual((Path(tmp) / "out/README.md").stat().st_mode & 0o777, 0o644)


if __name__ == "__main__":
    unittest.main()
