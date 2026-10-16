"""tools/e2e_package.py's own edges; CI's `package` job runs it on the two real release zips."""
import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import e2e_package  # noqa: E402


def fake_package(path, variant="normal"):
    """The least package the e2e reads before its checks: a manifest, the config template, the app's record."""
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("package-manifest.json", json.dumps({"version": "0.8.0", "variant": variant, "files": {}}))
        z.writestr("config.ini", "ocr_lm = 1000k\nocr_cascade_ms = 2000\n")
        z.writestr("ux0/data/VitaJPOverlay/vjo_app.txt", "vjoapp1 api=22 build=0123456789abcdef\n")
    return path


def in_process(package, work, *extra):
    """main() in this process (so that a test can stand in for the scenarios): its exit code and output."""
    out = io.StringIO()
    with mock.patch.object(sys, "argv", ["e2e_package.py", str(package), "--work", str(work), *extra]), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        try:
            e2e_package.main()
        except SystemExit as exc:
            return exc.code, out.getvalue()
    return 0, out.getvalue()


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
                                   (inside, inside / "pkg" / "p.zip", "the package is, or is inside,")):
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

    def test_the_verdict_counts_every_failure_and_the_exit_code_says_so(self):
        """audit-overlay-release-variants-d note 3: a failed check, a scenario that stops half way and a file the
        e2e left beside its entries each fail the run (exit 1, named in the summary); the work stays to inspect."""
        def scenario_a(e):
            e.save_log("A-install.log", "a log")
            e.check(False, "a check that failed")

        def scenario_b(e):
            raise RuntimeError("the scenario stopped half way")

        def scenario_c(e):
            (e.work / "stray.txt").write_text("not one of the e2e's entries")
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(e2e_package.E2E, "package_checks", lambda e: e.check(True, "the package")), \
                mock.patch.object(e2e_package.E2E, "scenario_a", scenario_a), \
                mock.patch.object(e2e_package.E2E, "scenario_b", scenario_b), \
                mock.patch.object(e2e_package.E2E, "scenario_c", scenario_c):
            work = Path(tmp) / "work"
            code, out = in_process(fake_package(Path(tmp) / "p.zip"), work)
            self.assertEqual(code, 1, out)
            self.assertIn("FAIL a check that failed", out)
            self.assertIn("FAIL scenario_b stopped: RuntimeError: the scenario stopped half way", out)
            self.assertIn("FAIL the e2e made nothing in work/ but its own entries", out)
            summary = json.loads(next(line for line in out.splitlines() if line.startswith('{"package"')))
            self.assertEqual((summary["checks"], len(summary["failed"])), (4, 3), summary)
            self.assertTrue((work / "pkg").is_dir() and (work / "A-install.log").is_file())  # kept for inspection

    def test_a_pass_exits_0_and_keeps_only_the_logs(self):
        def scenario(e):
            e.save_log("C-install.log", "a log")
            e.check(True, "a check that passed")
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(e2e_package.E2E, "package_checks", scenario), \
                mock.patch.object(e2e_package.E2E, "scenario_a", scenario), \
                mock.patch.object(e2e_package.E2E, "scenario_b", scenario), \
                mock.patch.object(e2e_package.E2E, "scenario_c", scenario):
            work = Path(tmp) / "work"
            code, out = in_process(fake_package(Path(tmp) / "p.zip"), work)
            self.assertEqual(code, 0, out)
            self.assertEqual(sorted(p.name for p in work.iterdir()), [".e2e_package", "C-install.log"])
            code, out = in_process(fake_package(Path(tmp) / "p.zip"), work, "--keep")
            self.assertEqual(code, 0, out)
            self.assertTrue((work / "pkg" / "package-manifest.json").is_file())

    def test_the_package_checks_hold_the_manifest_to_its_modules_and_the_pins(self):
        """audit-overlay-release-variants-d notes 1 and 4: the manifest's variant and app version against the
        modules (package_release.base_variant), the models and the LMs' attribution against the pins."""
        blob, attribution = b"a pinned model", b"the LMs' attribution"
        pins = {"m.vocr": ("https://fixture.invalid/m", len(blob), hashlib.sha256(blob).hexdigest())}
        att = {"ATTRIBUTION.md": ("https://fixture.invalid/a", len(attribution), hashlib.sha256(attribution).hexdigest())}
        release = e2e_package.release
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(release, "shipped_models", return_value=[("m.vocr", pins, "v0", "a size")]), \
                mock.patch.object(release, "VOCR_LM_ATTRIBUTION_PIN", att), \
                mock.patch.object(release, "VOCR_LM_ATTRIBUTION", "ATTRIBUTION.md"):
            pkg = Path(tmp) / "pkg"
            e2e_package.unpack(fake_package(Path(tmp) / "p.zip"), pkg)
            ocr = pkg / "ux0/data/VitaJPOverlay/ocr"
            ocr.mkdir(parents=True)
            (ocr / "LM-ATTRIBUTION.md").write_bytes(attribution)
            for read_as, model, want in ((("normal", "0.8.0"), blob, []),
                                         (("debug", "0.8.0"), blob, ["the normal build its manifest names"]),
                                         (("normal", "0.7.12"), blob, ["the Overlay app is v0.7.12"]),
                                         (("normal", "0.8.0"), b"a pinned modeL", ["m.vocr is the pinned file"])):
                (ocr / "m.vocr").write_bytes(model)
                e = e2e_package.E2E(pkg, Path(tmp), Path(tmp) / "p.zip")
                e.manifest["app_version"] = "0.8.0"
                with self.subTest(read_as=read_as, model=model), \
                        mock.patch.object(release, "base_variant", return_value=read_as), \
                        contextlib.redirect_stdout(io.StringIO()):
                    e.package_checks()
                    failed = [what for ok, what in e.checks if not ok]
                    self.assertEqual(len(failed), len(want), failed)
                    for part, what in zip(want, failed):
                        self.assertIn(part, what)

    def test_an_older_runs_default_directory_gets_advice_that_fits(self):
        """audit-overlay-release-variants-d note 13: a default directory from before the marker is the e2e's
        own (an older run's): the refusal says to remove it, not to choose the default."""
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "work"
            (work / "pkg").mkdir(parents=True)
            problem = e2e_package.prepare_work(work, Path(tmp) / "p.zip", default=True)
            self.assertIn("an older run's? Remove it (rm -rf", problem)
            self.assertNotIn("or the default", problem)
            self.assertIn("or the default", e2e_package.prepare_work(work, Path(tmp) / "p.zip"))
            self.assertTrue((work / "pkg").is_dir())

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
