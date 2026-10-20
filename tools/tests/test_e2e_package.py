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
sys.path.insert(0, str(ROOT / "tools/tests"))
import check_variant  # noqa: E402
import e2e_package  # noqa: E402
from test_release_variants import fake_self  # noqa: E402  (a Vita SELF as check_variant.segments reads one)

PLUGINS = {"kernel": "ur0/tai/VitaJPOverlay_Kernel.skprx", "shell": "ur0/tai/VitaJPOverlay_Shell.suprx",
           "worker": "ur0/tai/VitaJPOverlay_OCR.suprx"}
OCR = "ux0/data/VitaJPOverlay/ocr/"


def segments(part, build, version="0.8.0"):
    """A module's loaded bytes as this tree's Vita build checks them (check_variant.check with ROOT's sources): the
    debug build has the fixed markers and its sources' debug-only strings, the normal build its own marker and
    none of them; the Rubi app's start line names its version and build."""
    if build == "debug":
        strings = [*check_variant.DEBUG_MARKERS.get(part, ()), *check_variant.debug_only_strings(ROOT, part)]
    else:
        strings = list(check_variant.NORMAL_MARKERS.get(part, ()))
    if part == "app":
        strings.insert(0, b"start: v" + version.encode() + (b" debug" if build == "debug" else b"") + b"; %s; heap %d B")
    return b"\0code\0" + b"\0".join(strings) + b"\0rodata\0"


def fake_package(path, variant="normal", build=None, worker=None, app_version="0.8.0", ocr=None, app_extra=b""):
    """A package as the e2e reads it: its manifest (variant None: a manifest that names none; app_version None: one
    without app_version), the config template, the app's record, the four modules of `build` (default: the
    manifest's variant, else debug; `worker`: the worker's build, if another; app_extra: more strings in the app)
    and the files under ocr/ ({name: bytes})."""
    build = build or variant or "debug"
    manifest = {"version": "0.8.0", "files": {}}
    if variant is not None:
        manifest["variant"] = variant
    if app_version is not None:
        manifest["app_version"] = app_version
    vpk = io.BytesIO()
    with zipfile.ZipFile(vpk, "w") as z:
        z.writestr("eboot.bin", fake_self(segments("app", build) + app_extra))
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("package-manifest.json", json.dumps(manifest))
        z.writestr("config.ini", "ocr_lm = 1000k\nocr_cascade_ms = 2000\n")
        z.writestr("ux0/data/VitaJPOverlay/vjo_app.txt", "vjoapp1 api=22 build=0123456789abcdef\n")
        for part, name in PLUGINS.items():
            z.writestr(name, fake_self(segments(part, (worker or build) if part == "worker" else build)))
        z.writestr("Rubi.vpk", vpk.getvalue())
        for name, blob in (ocr or {}).items():
            z.writestr(OCR + name, blob)
    return path


def in_process(package, work, *extra):
    """main() in this process (so that a test can stand in for the scenarios): its exit code and output."""
    return main_in_process(str(package), "--work", str(work), *extra)


def main_in_process(*argv):
    out = io.StringIO()
    with mock.patch.object(sys, "argv", ["e2e_package.py", *argv]), \
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
            base = Path(tmp) / "Rubi-0.8.0-debug.zip"  # a Vita build's own zip: no manifest, no models
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
            package = out / "Rubi-0.8.0.zip"
            with zipfile.ZipFile(package, "w") as z:  # a manifest but no package around it
                z.writestr("package-manifest.json", "{}")
            (out / "SHA256SUMS").write_text("sums\n")
            (out / "notes.txt").write_text("the owner's notes\n")
            for _ in range(2):  # the second run replaces the first one's entries, beside its marker
                r = run(package, "--work", out)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("is not a release package", r.stderr)
                self.assertEqual(sorted(p.name for p in out.iterdir()),
                                 [".e2e_package", "Rubi-0.8.0.zip", "SHA256SUMS", "notes.txt", "pkg"])
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
        """audit-overlay-release-variants-d notes 1 and 4, -e notes 1-4: the manifest's variant against all four modules
        as the Vita build checks them (the worker too: a normal package with the debug worker fails), its app version
        (required) against the app, the models and the LMs' attribution against the pins, and nothing else in ocr/."""
        blob, attribution = b"a pinned model", b"the LMs' attribution"
        pins = {"m.vocr": ("https://fixture.invalid/m", len(blob), hashlib.sha256(blob).hexdigest())}
        att = {"ATTRIBUTION.md": ("https://fixture.invalid/a", len(attribution), hashlib.sha256(attribution).hexdigest())}
        model_attribution = b"the model sizes' attribution"
        model_att = {"MODEL-ATTRIBUTION.md": ("https://fixture.invalid/m", len(model_attribution),
                                              hashlib.sha256(model_attribution).hexdigest())}
        gate_attribution = b"the gate's attribution"
        gate_att = {"GATE-ATTRIBUTION.md": ("https://fixture.invalid/g", len(gate_attribution),
                                            hashlib.sha256(gate_attribution).hexdigest())}
        release = e2e_package.release
        worker_dbg = len(check_variant.debug_only_strings(ROOT, "worker"))  # the worker's sources' debug-only strings
        cases = (  # (what, fake_package's arguments, the ocr/ files, the failed checks in order: a part of each)
            ("the normal build", {}, {}, []),
            ("the debug build", {"variant": "debug"}, {}, []),
            ("a normal package whose manifest says debug (P3a)", {"variant": "debug", "build": "normal"}, {},
             ["the debug build its manifest names (they read as normal)", "pass the debug build's own check"]),
            ("a normal package with the debug worker (P8)", {"worker": "debug"}, {},
             ["worker: the normal build carries %d of its sources' %d debug-only strings" % (worker_dbg, worker_dbg)]),
            ("a debug package with the normal worker", {"variant": "debug", "worker": "normal"}, {},
             ["worker: the debug build carries none of its sources' %d debug-only strings" % worker_dbg]),
            ("a normal app that still logs its frames (its start line says normal)",
             {"app_extra": check_variant.debug_only_strings(ROOT, "app")[0] + b"\0"}, {},
             ["app: the normal build carries 1 of its sources' 1 debug-only strings"]),
            ("another app version (P3d)", {"app_version": "0.7.12"}, {}, ["the Rubi app is v0.8.0, as the manifest"]),
            ("no app_version (P3e)", {"app_version": None}, {}, ["the manifest has no app_version"]),
            ("no variant, the debug build: a package from before the two builds", {"variant": None, "build": "debug"},
             {}, []),
            ("no variant, the normal build (P3c)", {"variant": None, "build": "normal"}, {},
             ["the debug build its manifest names (they read as normal)", "pass the debug build's own check"]),
            ("a model altered (P4c)", {}, {"m.vocr": b"a pinned modeL"}, ["m.vocr is the pinned file"]),
            ("the attribution altered (P4d)", {}, {"LM-ATTRIBUTION.md": b"the LMs' attributioN"},
             ["LM-ATTRIBUTION.md is the LMs' pinned attribution"]),
            ("the attribution missing", {}, {"LM-ATTRIBUTION.md": None}, ["LM-ATTRIBUTION.md is the LMs' pinned"]),
            ("the model sizes' attribution altered", {}, {"MODEL-ATTRIBUTION.md": b"the model sizes' attributioN"},
             ["MODEL-ATTRIBUTION.md is the model sizes' pinned attribution"]),
            ("the model sizes' attribution missing", {}, {"MODEL-ATTRIBUTION.md": None},
             ["MODEL-ATTRIBUTION.md is the model sizes' pinned attribution"]),
            ("the gate's attribution missing", {}, {"GATE-ATTRIBUTION.md": None},
             ["GATE-ATTRIBUTION.md is the text-presence gate's pinned attribution"]),
            ("an extra model, a name the broker accepts (P9)", {}, {"FL10_w8.vocr": blob},
             ["ocr/ has nothing but the shipped models and their attributions (other files: FL10_w8.vocr)"]),
            ("an extra LM and a file in a subdirectory", {}, {"x.vlm": b"lm", "old/m.vocr": blob},
             ["(other files: old/m.vocr, x.vlm)"]),
        )
        with mock.patch.object(release, "shipped_models", return_value=[("m.vocr", pins, "v0", "a size")]), \
                mock.patch.object(release, "VOCR_LM_ATTRIBUTION_PIN", att), \
                mock.patch.object(release, "VOCR_LM_ATTRIBUTION", "ATTRIBUTION.md"), \
                mock.patch.object(release, "VOCR_MODEL_ATTRIBUTION_PIN", model_att), \
                mock.patch.object(release, "VOCR_MODEL_ATTRIBUTION", "MODEL-ATTRIBUTION.md"), \
                mock.patch.object(release, "VOCR_GATE_ATTRIBUTION_PIN", gate_att), \
                mock.patch.object(release, "VOCR_GATE_ATTRIBUTION", "GATE-ATTRIBUTION.md"):
            for what, kwargs, files, want in cases:
                ocr = {name: data for name, data in {"m.vocr": blob, "LM-ATTRIBUTION.md": attribution,
                                                     "MODEL-ATTRIBUTION.md": model_attribution,
                                                     "GATE-ATTRIBUTION.md": gate_attribution, **files}.items()
                       if data is not None}
                with self.subTest(what), tempfile.TemporaryDirectory() as tmp, \
                        contextlib.redirect_stdout(io.StringIO()):
                    package = fake_package(Path(tmp) / "p.zip", ocr=ocr, **kwargs)
                    e2e_package.unpack(package, Path(tmp) / "pkg")
                    e = e2e_package.E2E(Path(tmp) / "pkg", Path(tmp), package)
                    e.package_checks()
                    failed = [label for ok, label in e.checks if not ok]
                    self.assertEqual(len(failed), len(want), failed)
                    for part, label in zip(want, failed):
                        self.assertIn(part, label)

    def test_self_test_says_the_normal_build_has_none_only_in_the_normal_build(self):
        """audit-overlay-release-variants-e note 4 (M17): install_ftp.sh --self-test arms the marker in either build,
        and its note that the normal build has no self-test is right only in the normal build's package (a manifest
        that names no variant is the debug build's)."""
        note = "Note: this package is the normal build, which has no self-test."
        cases = [(variant, says, 0) for variant in ("normal", "debug", None) for says in (True, False)]
        cases += [("normal", True, 1), ("debug", False, 1)]  # --self-test failed: the marker check fails too
        for variant, says, rc in cases:
            with self.subTest(variant=variant, says=says, rc=rc), tempfile.TemporaryDirectory() as tmp, \
                    contextlib.redirect_stdout(io.StringIO()):
                tmp = Path(tmp)
                package = fake_package(tmp / "p.zip", variant=variant)
                e2e_package.unpack(package, tmp / "pkg")
                e = e2e_package.E2E(tmp / "pkg", tmp, package)
                dev, run_pkg = tmp / "device", tmp / "run"
                (dev / "ux0/data/VitaJPOverlay").mkdir(parents=True)
                (run_pkg / "tools").mkdir(parents=True)
                marker = dev / "ux0/data/VitaJPOverlay/selftest.txt"  # what the real script uploads
                (run_pkg / "tools/install_ftp.sh").write_text(
                    f': > "{marker}"\n' + (f'echo "{note}"\n' if says else "") + f"exit {rc}\n")
                e.self_test(dev, run_pkg, "127.0.0.1:1")
                checks = {label: ok for ok, label in e.checks}
                self.assertEqual(checks.pop("A --self-test armed the marker"), rc == 0, e.checks)
                (label, ok), = checks.items()
                self.assertIn("that the normal build has no self-test", label)
                self.assertEqual(ok, says == (variant == "normal"), label)

    def test_the_collect_zip_lists_the_apkg_packages_and_the_analysis_counts_them(self):
        """audit-overlay-release-variants-e note 4 (Q8): --collect lists the Rubi app's .apkg packages without
        copying them, and its analysis counts them; a zip with one, or an analysis without the count, fails."""
        count = "2 exported card(s), 1 .apkg package(s) in anki/"
        for members, analysis, want in (([], count, True), ([], "no count", False),
                                        (["ux0/data/VitaJPOverlay/anki/2026-10-16_120000-2cards.apkg"], count, False)):
            with self.subTest(members=members, analysis=analysis), tempfile.TemporaryDirectory() as tmp, \
                    contextlib.redirect_stdout(io.StringIO()):
                tmp = Path(tmp)
                package = fake_package(tmp / "p.zip")
                e2e_package.unpack(package, tmp / "pkg")
                e = e2e_package.E2E(tmp / "pkg", tmp, package)
                collect = tmp / "run/build/device/collect"
                collect.mkdir(parents=True)
                zip_path = collect / "vjo-collect-20261016-120000.zip"
                with zipfile.ZipFile(zip_path, "w") as z:
                    for name in members:
                        z.writestr(zip_path.stem + "/" + name, b"a package")
                e.collected_a(tmp / "run", f"{analysis}\nSend this one file: {zip_path}\n")
                got = [ok for ok, label in e.checks if label.startswith("A the zip lists the .apkg packages")]
                self.assertEqual(got, [want], e.checks)

    def test_main_gives_an_older_runs_default_directory_its_own_advice(self):
        """audit-overlay-release-variants-e notes 4 (Q10) and 9: the default directory (build/e2e-package/<the zip's
        name>) without the marker is an older run's: "remove it", whether --work names it or not; any other directory
        the e2e did not make gets "choose another"."""
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(e2e_package, "ROOT", Path(tmp).resolve()):
            tmp = Path(tmp).resolve()
            package = fake_package(tmp / "p.zip")
            default, other = tmp / "build/e2e-package/p", tmp / "other"
            for d in (default, other):
                (d / "pkg").mkdir(parents=True)
            for argv, advice in (([], f"an older run's? Remove it (rm -rf {default})"),
                                 (["--work", str(default)], f"an older run's? Remove it (rm -rf {default})"),
                                 (["--work", str(tmp / "build/e2e-package/../e2e-package/p")], "an older run's?"),
                                 (["--work", str(other)], "choose another directory (a new one, or the default)")):
                with self.subTest(argv=argv):
                    code, out = main_in_process(str(package), *argv)
                    self.assertIn(advice, str(code), out)
            self.assertTrue((default / "pkg").is_dir() and (other / "pkg").is_dir())

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
