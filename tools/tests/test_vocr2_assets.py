"""Asset plumbing with tiny fixture bytes; no models, network or device access."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr as prepare
import package_release as release


class Vocr2AssetPins(unittest.TestCase):
    def test_distinct_trained_asset_has_its_own_release_and_hash(self):
        spec = prepare.VOCR2_ASSETS[prepare.VOCR2_MODEL]
        self.assertEqual(spec, (
            "https://github.com/bee-san/vita-vn-ocr/releases/download/v0.2.3-vocr2.1/VOCR2_FL10_w8.vocr",
            657664, "d3adc52e5cf62b548af04c5ab07a2f57cf0419386bbdf49ba19c9703be6e1766"))
        self.assertNotEqual(spec[2], prepare.VOCR_ASSETS["FL10_w8.vocr"][2])


class Vocr2AssetPlumbing(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.models = self.root / "models"
        self.models.mkdir()
        self.data = b"tiny trained asset fixture, not OCR weights"
        self.legacy = b"tiny legacy asset fixture, not OCR weights!"
        self.assertEqual(len(self.data), len(self.legacy))
        self.spec = ("https://fixture.invalid/VOCR2_FL10_w8.vocr", len(self.data),
                     hashlib.sha256(self.data).hexdigest())
        old_spec = ("https://fixture.invalid/FL10_w8.vocr", len(self.legacy),
                    hashlib.sha256(self.legacy).hexdigest())
        old = {name: old_spec for name in ("FL10_w8.vocr", "H15_w8.vocr")}
        new = {prepare.VOCR2_MODEL: self.spec}
        for module in (prepare, release):
            self.enterContext(mock.patch.object(module, "VOCR_ASSETS", old))
            self.enterContext(mock.patch.object(module, "VOCR2_ASSETS", new))
        self.enterContext(mock.patch.object(prepare.urllib.request, "urlopen",
                                          side_effect=AssertionError("No test network access")))

    def test_disabled_preparation_fails_before_copy_or_download(self):
        with mock.patch.object(prepare, "VOCR2_ENABLED", False):
            with self.assertRaisesRegex(RuntimeError, "not enabled"):
                prepare.fetch_vocr2(self.models)
        self.assertEqual(list(self.models.iterdir()), [])

    def test_valid_existing_vocr2_needs_no_download_or_legacy_file(self):
        pinned = self.models / prepare.VOCR2_MODEL
        pinned.write_bytes(self.data)
        with mock.patch.object(prepare, "VOCR2_ENABLED", True), \
             mock.patch.object(prepare, "fetch_vocr", side_effect=AssertionError("No legacy fetch")):
            self.assertEqual(prepare.fetch_vocr2(self.models), pinned)

    def test_legacy_cache_is_not_used_for_vocr2(self):
        old = self.models / "FL10_w8.vocr"
        old.write_bytes(self.legacy)
        with mock.patch.object(prepare, "VOCR2_ENABLED", True), \
             mock.patch.object(prepare, "fetch", return_value=self.models / prepare.VOCR2_MODEL) as fetch:
            prepare.fetch_vocr2(self.models)
        fetch.assert_called_once_with(self.models, prepare.VOCR2_MODEL, prepare.VOCR2_ASSETS)
        self.assertEqual(old.read_bytes(), self.legacy)

    def test_from_file_or_directory_checks_separate_hash(self):
        source = self.root / "source"
        source.mkdir()
        path = source / prepare.VOCR2_MODEL
        path.write_bytes(self.data)
        with mock.patch.object(prepare, "VOCR2_ENABLED", True):
            for source_arg in (source, path):
                copied = prepare.fetch_vocr2(self.models, source_arg)
                self.assertEqual(copied.read_bytes(), self.data)
                copied.unlink()
            path.write_bytes(self.legacy)
            with self.assertRaisesRegex(RuntimeError, "size or SHA-256 differs"):
                prepare.fetch_vocr2(self.models, path)
        self.assertFalse((self.models / prepare.VOCR2_MODEL).exists())

    def test_source_directory_never_substitutes_legacy_filename(self):
        source = self.root / "source"
        source.mkdir()
        (source / "FL10_w8.vocr").write_bytes(self.legacy)
        with mock.patch.object(prepare, "VOCR2_ENABLED", True):
            with self.assertRaises(FileNotFoundError):
                prepare.fetch_vocr2(self.models, source)
        self.assertFalse((self.models / prepare.VOCR2_MODEL).exists())

    def test_private_fallback_uses_exact_vocr2_tag_and_name(self):
        def gh_download(argv, **kwargs):
            self.assertEqual(argv[:8], ["gh", "release", "download", prepare.VOCR2_TAG,
                                      "-R", "bee-san/vita-vn-ocr", "-p", prepare.VOCR2_MODEL])
            self.assertEqual(argv[8], "-D")
            (Path(argv[9]) / prepare.VOCR2_MODEL).write_bytes(self.data)
        with mock.patch.object(prepare, "VOCR2_ENABLED", True), \
             mock.patch.object(prepare, "fetch", side_effect=OSError("private")), \
             mock.patch.object(prepare.shutil, "which", return_value="/fixture/gh"), \
             mock.patch.object(prepare.subprocess, "run", side_effect=gh_download):
            pinned = prepare.fetch_vocr2(self.models)
        self.assertEqual(pinned.read_bytes(), self.data)

    def package_fixture(self, enabled):
        for name in ("FL10_w8.vocr", "H15_w8.vocr"):
            (self.models / name).write_bytes(self.legacy)
        dictionary = self.root / "jmdict.vjdict"
        dictionary.write_bytes(b"VJDICT2\0synthetic dictionary fixture")
        source_pin = json.loads((ROOT / "docs/jmdict-source.json").read_text())["sha256"]
        dictionary.with_suffix(".json").write_text(json.dumps({
            "source_sha256": source_pin, "sha256": release.digest(dictionary)}))
        license_file = self.root / "fixture-license.txt"
        license_file.write_text("Unit-test fixture")
        base = self.root / "base.zip"
        with zipfile.ZipFile(base, "w") as archive:
            for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                         "ur0/tai/VitaJPOverlay_OCR.suprx", "JPOverlaySettings.vpk",
                         "ur0/data/VitaJPOverlay/vitajpoverlay.rco"):
                archive.writestr(name, b"synthetic package fixture")
        output = self.root / "package.zip"
        with mock.patch.object(release, "VOCR2_ENABLED", enabled), \
             contextlib.redirect_stdout(io.StringIO()):
            release.package(base, output, self.models, dictionary, license_file)
        return output

    def test_enabled_bundle_contains_distinct_model_with_old_defaults(self):
        (self.models / prepare.VOCR2_MODEL).write_bytes(self.data)
        package = self.package_fixture(True)
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            name = release.DATA + "ocr/" + prepare.VOCR2_MODEL
            self.assertEqual(archive.read(name), self.data)
            self.assertEqual(archive.read(release.DATA + "ocr/FL10_w8.vocr"), self.legacy)
            self.assertEqual(manifest["files"][name]["sha256"], self.spec[2])
            self.assertEqual(manifest["ocr_backends"]["vocr2"], {
                "model": prepare.VOCR2_MODEL, "finder_mode": "textbox",
                "model_release": prepare.VOCR2_TAG, "model_sha256": self.spec[2],
                "model_bytes": self.spec[1], "recognizer_arena_bytes": 124832,
                "experimental": True, "qualification": "rejected_for_promotion",
                "default": False,
                "protected_result_sha256": "3c2c873239e1bcbdf3858650d7b628853b0f4b1f4c324ecc9519a7348c8d09f7",
                "runtime_commit": "49ff6f4d1c55804c600d098810574a49d382a009"})
            self.assertEqual(manifest["defaults"]["ocr_backend"], "vocr")
            self.assertEqual(manifest["defaults"]["vocr_model"], "FL10_w8.vocr")

    def test_enabled_bundle_refuses_missing_or_legacy_model_before_output(self):
        with self.assertRaisesRegex(ValueError, "OCR model is missing: " + prepare.VOCR2_MODEL):
            self.package_fixture(True)
        self.assertFalse((self.root / "package.zip").exists())
        (self.models / prepare.VOCR2_MODEL).write_bytes(self.legacy)
        with self.assertRaisesRegex(ValueError, "OCR model checksum failed: " + prepare.VOCR2_MODEL):
            self.package_fixture(True)
        self.assertFalse((self.root / "package.zip").exists())

    def test_disabled_bundle_has_no_vocr2_artifact_or_backend_claim(self):
        package = self.package_fixture(False)
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertNotIn(release.DATA + "ocr/" + prepare.VOCR2_MODEL, archive.namelist())
            self.assertNotIn("ocr_backends", manifest)


if __name__ == "__main__":
    unittest.main()
