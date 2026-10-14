"""Frozen archive integrity, safe staging and packaging, plus real disk lookup."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import convert_dictionary as convert
import prepare_plaintext as prepare
import package_release as release


class FrozenPlaintext(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        archive = self.root / "source.zip"
        with zipfile.ZipFile(archive, "w") as zipped:
            zipped.writestr("index.json", json.dumps({"format": 3, "title": "Frozen fixture"}))
            zipped.writestr("term_bank_1.json", json.dumps([
                ["食べる", "たべる", "", "v1", 0, ["synthetic frozen verb"], 1, ""]]))
        dictionary = self.root / prepare.FILENAME
        convert.convert([archive], dictionary)
        self.archive = self.root / "frozen.zip"
        with zipfile.ZipFile(self.archive, "w") as zipped:
            zipped.write(dictionary, prepare.FILENAME)
        self.pin = {"bytes": dictionary.stat().st_size, "sha256": prepare.digest(dictionary),
                    "archive": "processed/plaintext.vjdict.zip", "archive_bytes": self.archive.stat().st_size,
                    "archive_sha256": prepare.digest(self.archive), "repository": "fixture/private",
                    "revision": "a" * 40}
        self.enterContext(mock.patch.object(prepare, "load_manifest", return_value=self.pin))
        self.enterContext(mock.patch.object(release, "load_manifest", return_value=self.pin))

    def test_stage_is_installable_and_keeps_source_attribution(self):
        stage = self.root / "stage"
        dictionary = prepare.stage(stage, self.archive)
        cli = os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli")
        lookup = subprocess.run([str(cli), "--dict", "local", "--local-dir", str(dictionary.parent),
                                 "--local-dicts", prepare.FILENAME, "--text", "食べました", "--nav"],
                                capture_output=True, text=True, check=True)
        self.assertIn("synthetic frozen verb", lookup.stdout)
        self.assertEqual(json.loads((stage / "docs/plaintext-dictionaries.json").read_text()), self.pin)
        self.assertIn("Daijirin4", (stage / "docs/plaintext-attribution.txt").read_text())
        self.assertTrue((stage / "docs/JMdict-LICENSE.html").is_file())

    def test_bad_archive_preserves_existing_output(self):
        output = self.root / "output.vjdict"
        output.write_bytes(b"existing file")
        self.archive.write_bytes(b"corrupt archive")
        with self.assertRaisesRegex(ValueError, "archive checksum"):
            prepare.prepare(output, self.archive)
        self.assertEqual(output.read_bytes(), b"existing file")

    def test_private_fetch_uses_only_exact_commit_and_cache_needs_no_network(self):
        output = self.root / "output.vjdict"
        def metadata(argv, **kwargs):
            self.assertEqual(argv, ["gh", "api",
                "repos/fixture/private/contents/processed/plaintext.vjdict.zip?ref=" + "a" * 40,
                "-H", "Accept: application/vnd.github.object+json"])
            return json.dumps({"download_url": "https://fixture.invalid/pinned-blob"})
        with mock.patch.object(prepare, "ROOT", self.root), \
             mock.patch.object(prepare.subprocess, "check_output", side_effect=metadata) as fetch, \
             mock.patch.object(prepare.urllib.request, "urlopen", return_value=io.BytesIO(self.archive.read_bytes())) as download:
            prepare.prepare(output)
            prepare.prepare(output)
        fetch.assert_called_once()
        download.assert_called_once_with("https://fixture.invalid/pinned-blob", timeout=60)
        prepare.validate(output, self.pin)

    def test_package_uses_staged_dictionary_without_external_data_and_rejects_wrong_one(self):
        stage = self.root / "stage"
        prepare.stage(stage, self.archive)
        for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                     "ur0/tai/VitaJPOverlay_OCR.suprx", "JPOverlaySettings.vpk",
                     "ur0/data/VitaJPOverlay/vitajpoverlay.rco"):
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"synthetic plugin")
        base = self.root / "base.zip"
        with zipfile.ZipFile(base, "w") as zipped:
            for path in stage.rglob("*"):
                if path.is_file():
                    zipped.write(path, str(path.relative_to(stage)))
        models = self.root / "models"
        models.mkdir()
        payload = b"synthetic model"
        pins = {name: ("fixture", len(payload), hashlib.sha256(payload).hexdigest())
                for name in ("FL10_w8.vocr", "H15_w8.vocr")}
        vocr3 = {name: ("fixture", len(payload), hashlib.sha256(payload).hexdigest())
                 for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr")}
        lms = {name: ("fixture", len(payload), hashlib.sha256(payload).hexdigest())
               for name in ("vocr_lm_o3_500k.vlm", "vocr_lm_o3_4000k.vlm")}
        vocr4 = {name: ("fixture", len(payload), hashlib.sha256(payload).hexdigest())
                 for name in ("VOCR4_W20_w8.vocr",)}
        for name in (*pins, *vocr3, *lms, *vocr4):
            (models / name).write_bytes(payload)
        (models / "ATTRIBUTION.md").write_text("CC BY-SA 4.0 fixture")
        output = self.root / "package.zip"
        with mock.patch.object(release, "VOCR_ASSETS", pins), mock.patch.object(release, "VOCR3_ASSETS", vocr3), \
             mock.patch.object(release, "VOCR_LM_ASSETS", lms), mock.patch.object(release, "VOCR4_ASSETS", vocr4), \
             mock.patch.object(prepare.subprocess, "run", side_effect=AssertionError("No packaging fetch")), \
             contextlib.redirect_stdout(io.StringIO()):
            release.package(base, output, models)
        with zipfile.ZipFile(output) as zipped:
            manifest = json.loads(zipped.read("package-manifest.json"))
            self.assertEqual(manifest["defaults"]["local_dictionaries"], prepare.FILENAME)
            name = release.DATA + "dictionaries/" + prepare.FILENAME
            self.assertEqual(hashlib.sha256(zipped.read(name)).hexdigest(), self.pin["sha256"])
            self.assertEqual(manifest["dictionary"], self.pin)
            zipped.extractall(self.root / "unpacked")
        with contextlib.redirect_stdout(io.StringIO()):
            release.verify(self.root / "unpacked")
        wrong = self.root / "wrong.vjdict"
        wrong.write_bytes(b"VJDICT2\0wrong snapshot")
        with self.assertRaisesRegex(ValueError, "dictionary checksum"):
            release.package(base, self.root / "rejected.zip", models, wrong)
        self.assertFalse((self.root / "rejected.zip").exists())


class CommittedSnapshot(unittest.TestCase):
    def test_real_frozen_archive_and_all_three_sources(self):
        source = Path(os.environ.get("VJO_PLAINTEXT_SOURCE", ROOT / ".refs/plaintext-dicts"))
        if not source.is_dir():
            self.skipTest("Private frozen checkout not available; CI checks it out")
        pin = prepare.load_manifest()
        with tempfile.TemporaryDirectory() as tmp:
            dictionary = prepare.prepare(Path(tmp) / prepare.FILENAME, source)
            cli = os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli")
            for text, title in (("猫", "Daijirin4 Plain"), ("食べました", "Daijirin4 Plain"),
                                ("愛し合う", "Sankoku8 Plain"), ("あうんの呼吸", "JMdict Plain")):
                result = subprocess.run([str(cli), "--dict", "local", "--local-dir", tmp,
                                         "--local-dicts", prepare.FILENAME, "--text", text, "--nav"],
                                        capture_output=True, text=True, check=True)
                self.assertIn("[" + title + "]", result.stdout)
            self.assertEqual(pin["priority"], ["Daijirin4", "Sankoku8", "JMdict"])
            self.assertTrue(all(n > 0 for n in pin["conversion"]["source_entries"]))
