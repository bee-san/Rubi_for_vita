"""Frozen archive integrity, safe staging and packaging, plus real disk lookup."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
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
import prepare_vndb


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
        frequency = self.root / "frequency.zip"
        with zipfile.ZipFile(frequency, "w") as zipped:
            zipped.writestr("index.json", json.dumps({"format": 3, "title": "Fixture frequency",
                                                      "frequencyMode": "rank-based"}))
            zipped.writestr("term_meta_bank_1.json", json.dumps([
                ["食べる", "freq", {"reading": "たべる", "frequency": {"value": 412, "displayValue": "412"}}]],
                ensure_ascii=False))
        dictionary = self.root / prepare.FILENAME
        convert.convert([archive], dictionary, frequency=frequency)
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
        self.assertIn("食べる (たべる) #412", lookup.stdout)  # the frozen rank, shown as the badge
        self.assertEqual(json.loads((stage / "docs/plaintext-dictionaries.json").read_text()), self.pin)
        attribution = (stage / "docs/plaintext-attribution.txt").read_text()
        self.assertEqual(attribution, (ROOT / "docs/plaintext-attribution.txt").read_text())
        # every source of Bee's Ultimate Dictionary is credited with its licence
        for part in ("Bee's Ultimate Dictionary", "Daijirin4", "Sankoku8", "JMdict"):
            self.assertIn(part, attribution)
        for paragraph, licence in (("Frequency ranks: the Jiten", "CC BY-SA 4.0"),
                                   ("Kanji lines: English kanji meanings", "CC BY-SA 4.0"),
                                   ("Yokubi, The Common Grammar Guide", "CC BY 4.0"),
                                   ("NINJAL 日本語文型データベース", "CC BY 4.0")):
            self.assertIn(licence, attribution.split(paragraph, 1)[1].split("\n\n", 1)[0].split("\n- ", 1)[0], paragraph)
        self.assertTrue((stage / "docs/JMdict-LICENSE.html").is_file())

    def test_a_dictionary_without_frequency_ranks_is_refused(self):
        older = self.root / "v2.vjdict"
        with zipfile.ZipFile(self.root / "source.zip") as zipped:
            self.assertIn("term_bank_1.json", zipped.namelist())
        convert.convert([self.root / "source.zip"], older)
        pin = dict(self.pin, bytes=older.stat().st_size, sha256=prepare.digest(older))
        with self.assertRaisesRegex(ValueError, "version 3"):
            prepare.validate(older, pin)

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
        prepare_vndb.stage(stage)
        for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                     "ur0/tai/VitaJPOverlay_OCR.suprx", "Rubi.vpk",
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
        vocr4 = {name: ("fixture", len(payload), hashlib.sha256(payload).hexdigest()) for name in release.VOCR4_ASSETS}
        vocr5 = {name: ("fixture", len(payload), hashlib.sha256(payload).hexdigest()) for name in release.VOCR5_ASSETS}
        for name in (*pins, *vocr3, *lms, *vocr4, *vocr5):
            (models / name).write_bytes(payload)
        (models / "ATTRIBUTION.md").write_text("CC BY-SA 4.0 fixture")
        att = {"ATTRIBUTION.md": ("fixture", 20, hashlib.sha256(b"CC BY-SA 4.0 fixture").hexdigest())}
        (models / "MODEL-ATTRIBUTION.md").write_text("CC BY 4.0 fixture")
        model_att = {"MODEL-ATTRIBUTION.md": ("fixture", 17, hashlib.sha256(b"CC BY 4.0 fixture").hexdigest())}
        (models / release.VOCR_GATE_MODEL).write_bytes(payload)  # API 26: the text-presence gate's
        gate = {release.VOCR_GATE_MODEL: ("fixture", len(payload), hashlib.sha256(payload).hexdigest())}
        (models / "GATE-ATTRIBUTION.md").write_text("gate fixture")
        gate_att = {"GATE-ATTRIBUTION.md": ("fixture", 12, hashlib.sha256(b"gate fixture").hexdigest())}
        output = self.root / "package.zip"
        with mock.patch.object(release, "VOCR_ASSETS", pins), mock.patch.object(release, "VOCR3_ASSETS", vocr3), \
             mock.patch.object(release, "VOCR_LM_ATTRIBUTION_PIN", att), \
             mock.patch.object(release, "VOCR_MODEL_ATTRIBUTION_PIN", model_att), \
             mock.patch.object(release, "VOCR_GATE_ASSETS", gate), mock.patch.object(release, "VOCR_GATE_ATTRIBUTION_PIN", gate_att), \
             mock.patch.object(release, "VOCR_LM_ASSETS", lms), mock.patch.object(release, "VOCR4_ASSETS", vocr4), \
             mock.patch.object(release, "VOCR5_ASSETS", vocr5), \
             mock.patch.object(convert, "convert", side_effect=AssertionError("No packaging conversion")), \
             mock.patch.object(prepare.subprocess, "run", side_effect=AssertionError("No packaging fetch")), \
             contextlib.redirect_stdout(io.StringIO()):
            release.package(base, output, models)
        with zipfile.ZipFile(output) as zipped:
            manifest = json.loads(zipped.read("package-manifest.json"))
            self.assertEqual(manifest["defaults"]["local_dictionaries"], prepare.FILENAME + ",vndb-names.vjdict")
            name = release.DATA + "dictionaries/" + prepare.FILENAME
            self.assertEqual(hashlib.sha256(zipped.read(name)).hexdigest(), self.pin["sha256"])
            self.assertEqual(manifest["dictionary"], self.pin)
            names_pin = prepare_vndb.load_manifest()
            self.assertEqual(hashlib.sha256(zipped.read(release.DATA + "dictionaries/" + prepare_vndb.FILENAME)).hexdigest(),
                             names_pin["sha256"])
            for name in prepare_vndb.DOCS:
                self.assertEqual(zipped.read("docs/" + name), (ROOT / "docs" / name).read_bytes())
            for name in prepare_vndb.LICENSES:
                self.assertIn("third_party/licenses/" + name, manifest["files"])
            zipped.extractall(self.root / "unpacked")
        with contextlib.redirect_stdout(io.StringIO()):
            release.verify(self.root / "unpacked")
        wrong = self.root / "wrong.vjdict"
        wrong.write_bytes(b"VJDICT3\0wrong snapshot")
        with self.assertRaisesRegex(ValueError, "dictionary checksum"):
            release.package(base, self.root / "rejected.zip", models, wrong)
        self.assertFalse((self.root / "rejected.zip").exists())


def strip_hints(path):
    """A format 3 file as #127's converter wrote it: no posting-hints descriptor, every posting's u16 0."""
    data = bytearray(path.read_bytes())
    data[convert.HINTS_DESCRIPTOR_OFFSET:convert.HINTS_DESCRIPTOR_OFFSET + 8] = bytes(8)
    for page in range(convert.HEADER.unpack_from(data)[9]):
        base = convert.PAGE_SIZE * (1 + page)
        for slot in range(struct.unpack_from("<I", data, base + 4)[0]):
            _, group, postings, key_len, _ = convert.HASH_RECORD.unpack_from(data, base + 16 + 24 * slot)
            for j in range(postings):
                at = group + key_len + 16 * j + 14
                data[at:at + 2] = bytes(2)
    path.write_bytes(data)


class KeyFilteredRelease(unittest.TestCase):
    """The released file is the frozen one plus the posting hints and the key filter, pinned by its own
    checksum."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        source = self.root / "source.zip"
        rows = [["食べる", "たべる", "", "v1", 0, ["synthetic frozen verb"], 1, ""]]
        rows += [[f"語彙{i:05d}", "", "", "", 0, [f"filler {i}"], 1, ""] for i in range(1200)]
        with zipfile.ZipFile(source, "w") as zipped:
            zipped.writestr("index.json", json.dumps({"format": 3, "title": "Frozen fixture"}))
            zipped.writestr("term_bank_1.json", json.dumps(rows, ensure_ascii=False))
        frequency = self.root / "frequency.zip"
        with zipfile.ZipFile(frequency, "w") as zipped:
            zipped.writestr("index.json", json.dumps({"format": 3, "title": "Fixture frequency",
                                                      "frequencyMode": "rank-based"}))
            zipped.writestr("term_meta_bank_1.json", json.dumps([
                ["食べる", "freq", {"reading": "たべる", "frequency": {"value": 412, "displayValue": "412"}}]],
                ensure_ascii=False))
        frozen = self.root / "frozen" / prepare.FILENAME
        frozen.parent.mkdir()
        self.hinted = self.root / "hinted.vjdict"  # this converter's --frequency: the hints included
        convert.convert([source], self.hinted, frequency=frequency, key_filter=False)
        # as committed: format 3 from #127's converter, without the hints and the filter
        shutil.copyfile(self.hinted, frozen)
        strip_hints(frozen)
        added = self.root / "added.vjdict"
        self.hinted_postings = convert.add_index_hints(frozen, added)["hinted_postings"]
        self.assertEqual(added.read_bytes(), self.hinted.read_bytes())  # the hints step = the converter's
        self.filtered = self.root / "filtered.vjdict"
        convert.add_key_filter(self.hinted, self.filtered)
        self.archive, self.pin = self.pinned(frozen, "frozen.zip")
        self.pin["key_filter"] = {"bytes": self.filtered.stat().st_size, "sha256": prepare.digest(self.filtered),
                                  "hinted_postings": self.hinted_postings}

    def pinned(self, frozen, name):
        archive = self.root / name
        with zipfile.ZipFile(archive, "w") as zipped:
            zipped.write(frozen, prepare.FILENAME)
        return archive, {"bytes": frozen.stat().st_size, "sha256": prepare.digest(frozen),
                         "archive": "processed/plaintext.vjdict.zip", "archive_bytes": archive.stat().st_size,
                         "archive_sha256": prepare.digest(archive), "repository": "fixture/private",
                         "revision": "a" * 40}

    def test_prepare_adds_the_pinned_filter_and_rejects_another(self):
        output = self.root / "out" / prepare.FILENAME
        with mock.patch.object(prepare, "load_manifest", return_value=self.pin):
            prepare.prepare(output, self.archive)
            prepare.validate(output, self.pin)
            data = output.read_bytes()
            self.assertEqual(data[convert.FILTER_DESCRIPTOR_OFFSET:convert.FILTER_DESCRIPTOR_OFFSET + 4], b"VJKF")
            self.assertEqual(data[convert.HINTS_DESCRIPTOR_OFFSET:convert.HINTS_DESCRIPTOR_OFFSET + 8],
                             convert.HINTS_DESCRIPTOR.pack(convert.HINTS_MAGIC, 1))
            self.assertEqual(data, self.filtered.read_bytes())
            self.assertEqual(len(data), self.pin["key_filter"]["bytes"])
            with self.assertRaisesRegex(ValueError, "checksum"):
                prepare.validate(output, self.pin, frozen=True)  # not the committed bytes any more
            prepare.prepare(output, self.archive)  # a valid cached output is kept
            self.assertEqual(output.read_bytes(), data)
        wrong = dict(self.pin, key_filter={"bytes": self.pin["key_filter"]["bytes"], "sha256": "0" * 64})
        other = self.root / "other" / prepare.FILENAME
        with mock.patch.object(prepare, "load_manifest", return_value=wrong), \
                self.assertRaisesRegex(ValueError, "dictionary checksum"):
            prepare.prepare(other, self.archive)
        self.assertFalse(other.exists())
        cli = os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli")
        lookup = subprocess.run([str(cli), "--dict", "local", "--local-dir", str(output.parent),
                                 "--local-dicts", prepare.FILENAME, "--text", "食べました", "--nav"],
                                capture_output=True, text=True, check=True)
        self.assertIn("synthetic frozen verb", lookup.stdout)

    def test_hinted_postings_switches_the_hint_step(self):
        """hinted_postings means "add the hints": the frozen file predates them. Without it the pinned
        (hinted) file cannot result; a frozen file that already has its hints is pinned without it."""
        def fails(pin, archive, pattern):
            out = self.root / "never" / prepare.FILENAME
            with mock.patch.object(prepare, "load_manifest", return_value=pin), \
                    self.assertRaisesRegex(ValueError, pattern):
                prepare.prepare(out, archive)
            self.assertFalse(out.exists())
        no_switch = dict(self.pin, key_filter={k: v for k, v in self.pin["key_filter"].items()
                                               if k != "hinted_postings"})
        fails(no_switch, self.archive, "dictionary checksum")
        archive, pin = self.pinned(self.hinted, "hinted.zip")
        fails(dict(pin, key_filter=self.pin["key_filter"]), archive, "not empty")
        pin["key_filter"] = no_switch["key_filter"]
        output = self.root / "repinned" / prepare.FILENAME
        with mock.patch.object(prepare, "load_manifest", return_value=pin):
            prepare.prepare(output, archive)
        self.assertEqual(output.read_bytes(), self.filtered.read_bytes())


class CommittedSnapshot(unittest.TestCase):
    def test_real_frozen_archive_is_bees_ultimate_dictionary(self):
        source = Path(os.environ.get("VJO_PLAINTEXT_SOURCE", ROOT / ".refs/plaintext-dicts"))
        if not source.is_dir():
            self.skipTest("Private frozen checkout not available; CI checks it out")
        pin = prepare.load_manifest()
        with tempfile.TemporaryDirectory() as tmp:
            dictionary = prepare.prepare(Path(tmp) / prepare.FILENAME, source)
            with dictionary.open("rb") as stream:
                page0 = stream.read(4096)
            # page 0: format 3, the frequency list's name and the dictionary's title
            self.assertEqual(page0[:8], b"VJDICT3\0")
            self.assertEqual(page0[64:128].rstrip(b"\0"), b"Jiten")
            self.assertEqual(page0[192:256].rstrip(b"\0").decode(), "Bee's Ultimate Dictionary")
            cli = os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli")
            # a word, a word with three kanji, an inflection, and a particle whose
            # grammar summary replaced its definition (no kanji line)
            for text, entry, lines in (("猫", "猫 (ねこ) #", ["猫="]), ("日本語", "日本語 (にほんご) #", ["日=", "本=", "語="]),
                                       ("食べました", "食べる (たべる) #", ["食="]), ("は", "は #2\nTopic particle", [])):
                result = subprocess.run([str(cli), "--dict", "local", "--local-dir", tmp,
                                         "--local-dicts", prepare.FILENAME, "--text", text, "--nav"],
                                        capture_output=True, text=True, check=True)
                self.assertIn(entry, result.stdout)
                shown = result.stdout.split(entry, 1)[1].split("\n\n", 1)[0].split("\n")
                self.assertEqual([line[:2] for line in shown if line[1:2] == "="], lines, text)
                # no source label (the converter's "[<title>]" line) anywhere
                for label in ("[Daijirin4", "[Sankoku8", "[JMdict", "Plain]"):
                    self.assertNotIn(label, result.stdout)
        self.assertEqual(pin["title"], "Bee's Ultimate Dictionary")
        self.assertEqual(pin["priority"], ["Daijirin4", "Sankoku8", "JMdict"])
        self.assertTrue(all(n > 0 for n in pin["conversion"]["source_entries"]))
        self.assertIs(pin["conversion"]["source_labels"], False)
        self.assertEqual(pin["conversion"]["format_version"], 3)
        for part in ("kanji", "grammar"):
            self.assertIn(part, pin)
