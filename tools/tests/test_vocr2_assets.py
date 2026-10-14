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
        self.vocr3 = b"tiny VOCR3 asset fixture, not OCR weights!!"
        vocr3_spec = ("https://fixture.invalid/VOCR3", len(self.vocr3), hashlib.sha256(self.vocr3).hexdigest())
        vocr3 = {name: vocr3_spec for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr")}
        for module in (prepare, release):
            self.enterContext(mock.patch.object(module, "VOCR_ASSETS", old))
            self.enterContext(mock.patch.object(module, "VOCR2_ASSETS", new))
            self.enterContext(mock.patch.object(module, "VOCR3_ASSETS", vocr3))
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

    def package_fixture(self, enabled, settings=None):
        for name in ("FL10_w8.vocr", "H15_w8.vocr"):
            (self.models / name).write_bytes(self.legacy)
        for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"):
            (self.models / name).write_bytes(self.vocr3)
        dictionary = self.root / "jmdict.vjdict"
        dictionary.write_bytes(b"VJDICT2\0synthetic dictionary fixture")
        snapshot = {"bytes": dictionary.stat().st_size, "sha256": release.digest(dictionary)}
        license_file = self.root / "fixture-license.txt"
        license_file.write_text("Unit-test fixture")
        base = self.root / "base.zip"
        with zipfile.ZipFile(base, "w") as archive:
            for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                         "ur0/tai/VitaJPOverlay_OCR.suprx", "JPOverlaySettings.vpk",
                         "ur0/data/VitaJPOverlay/vitajpoverlay.rco"):
                archive.writestr(name, b"synthetic package fixture")
            archive.writestr("config.ini", b"; the build's own template copy\n")
        output = self.root / "package.zip"
        with mock.patch.object(release, "VOCR2_ENABLED", enabled), \
             mock.patch.object(release, "load_manifest", return_value=snapshot), \
             contextlib.redirect_stdout(io.StringIO()):
            release.package(base, output, self.models, dictionary, license_file, settings=settings)
        return output

    def test_bundle_ships_vocr3_as_default_and_v020_as_fallback(self):
        (self.models / prepare.VOCR2_MODEL).write_bytes(self.data)
        package = self.package_fixture(True)
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "H15_w8.vocr", "FL10_w8.vocr", prepare.VOCR2_MODEL):
                self.assertIn(release.DATA + "ocr/" + name, manifest["files"])
                self.assertIn(name, manifest["models"])
            self.assertEqual(manifest["models"]["VOCR3_H15_w8.vocr"]["release"], "bee-san/vita-vn-ocr v0.2.5-vocr3.1")
            self.assertEqual(manifest["models"]["VOCR3_H15_w8.vocr"]["role"], "default")
            self.assertEqual(manifest["models"]["H15_w8.vocr"]["release"], "bee-san/vita-vn-ocr v0.2.0")
            self.assertEqual(manifest["defaults"]["vocr_model"], "VOCR3_H15_w8.vocr")
            self.assertEqual(manifest["defaults"]["vocr_fallback_model"], "VOCR3_FL10_w8.vocr")
            self.assertTrue(manifest["defaults"]["log_file"])
            self.assertFalse(manifest["defaults"]["anki"])
            self.assertNotIn("install_settings", manifest)
            # both template copies are the source template: logging on, Anki off
            for name in ("config.ini", release.DATA + "config.ini"):
                text = archive.read(name).decode()
                self.assertIn("\nlog_file = on\n", text)
                self.assertIn("\nanki_mining = off\n", text)
                self.assertIn("\nvocr_model = VOCR3_H15_w8.vocr\n", text)

    def test_package_settings_go_into_both_templates_and_the_manifest(self):
        (self.models / prepare.VOCR2_MODEL).write_bytes(self.data)
        package = self.package_fixture(True, {"anki_mining": "on", "log_file": "on"})
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertEqual(manifest["install_settings"], {"anki_mining": "on", "log_file": "on"})
            self.assertTrue(manifest["defaults"]["anki"])
            for name in ("config.ini", release.DATA + "config.ini"):
                text = archive.read(name).decode()
                self.assertIn("\nanki_mining = on\n", text)
                self.assertNotIn("anki_mining = off", text)
                self.assertEqual(manifest["files"][name]["sha256"], hashlib.sha256(text.encode()).hexdigest())
        # a key the template does not have, or a value with a line break, is refused before output
        with self.assertRaisesRegex(ValueError, "Not a setting of the config template: anki_minnig"):
            self.package_fixture(True, {"anki_minnig": "on"})
        for bad in ("anki_mining", "anki_mining=on\nlog_file=off", "=on", "Bad Key=1"):
            with self.assertRaises(Exception):
                release.parse_setting(bad)
        self.assertEqual(release.parse_setting(" Anki_Mining = on "), ("anki_mining", "on"))
        self.assertEqual(release.parse_setting("anki_field_picture="), ("anki_field_picture", ""))

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
            self.assertEqual(manifest["defaults"]["vocr_model"], "VOCR3_H15_w8.vocr")
            self.assertEqual(manifest["defaults"]["vocr_fallback_model"], "VOCR3_FL10_w8.vocr")

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
