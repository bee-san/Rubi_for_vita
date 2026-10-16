"""tools/e2e_package.py's own edges; CI's `package` job runs it on the two real release zips."""
import os
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

    def test_the_working_directory_keeps_the_callers_files(self):
        """audit-overlay-release-variants-d B1: `--work out`, where out/ holds the release itself, must keep it.
        The e2e removes only its own entries, and only beside its marker; a package that is not one stops after
        the work directory is ready, with the caller's files intact."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            out.mkdir()
            package = out / "VitaJPOverlay-0.8.0.zip"
            with zipfile.ZipFile(package, "w") as z:  # a manifest but no package around it
                z.writestr("package-manifest.json", "{}")
            (out / "SHA256SUMS").write_text("sums\n")
            (out / "notes.txt").write_text("the owner's notes\n")
            for _ in range(2):  # the second run replaces the first one's entries, beside its marker
                r = run(package, "--work", out)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("is not a release package", r.stderr)
                self.assertEqual(sorted(p.name for p in out.iterdir()),
                                 [".e2e_package", "SHA256SUMS", "VitaJPOverlay-0.8.0.zip", "notes.txt", "pkg"])
                self.assertEqual((out / "notes.txt").read_text(), "the owner's notes\n")
                self.assertTrue(zipfile.is_zipfile(package))

    def test_a_directory_it_did_not_make_is_refused_and_left_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            package = tmp / "p.zip"
            with zipfile.ZipFile(package, "w") as z:
                z.writestr("package-manifest.json", "{}")
            checkout = tmp / "checkout"  # `--work .` in a checkout that has a pkg/ of its own
            (checkout / "pkg").mkdir(parents=True)
            (checkout / "pkg" / "keep.c").write_text("x")
            a_file = tmp / "a-file"
            a_file.write_text("x")
            linked = tmp / "linked"  # a marker that is a link: never written through
            linked.mkdir()
            (linked / ".e2e_package").symlink_to(a_file)
            inside = tmp / "inside"
            (inside / "pkg").mkdir(parents=True)
            for work, pkg, why in ((checkout, package, "which tools/e2e_package.py did not make"),
                                   (a_file, package, "is not a directory"),
                                   (linked, package, "is not tools/e2e_package.py's marker file"),
                                   (inside, inside / "pkg" / "p.zip", "the package is inside")):
                if not pkg.exists():
                    pkg.write_bytes(package.read_bytes())
                with self.subTest(why=why):
                    r = run(pkg, "--work", work)
                    self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                    self.assertIn(why, r.stderr)
            self.assertEqual((checkout / "pkg" / "keep.c").read_text(), "x")
            self.assertFalse((checkout / ".e2e_package").exists())
            self.assertEqual(a_file.read_text(), "x")
            self.assertTrue((inside / "pkg" / "p.zip").is_file())

    def test_an_entry_that_is_a_link_is_removed_not_followed(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            work, target = tmp / "work", tmp / "elsewhere"
            (target / "sub").mkdir(parents=True)
            (target / "sub" / "f").write_text("x")
            work.mkdir()
            (work / e2e_package.MARKER).write_text("")
            (work / "A-test2").symlink_to(target)
            (work / "pkg").mkdir()
            self.assertIsNone(e2e_package.prepare_work(work, tmp / "p.zip"))
            self.assertFalse(os.path.lexists(work / "A-test2") or (work / "pkg").exists())
            self.assertEqual((target / "sub" / "f").read_text(), "x")

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
