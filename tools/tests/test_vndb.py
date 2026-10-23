"""The shipped handheld names: deterministic conversion, real lookup and provenance."""
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
import prepare_vndb as names
from convert_dictionary import convert


class BundledNames(unittest.TestCase):
    def test_real_dictionary_lookup_priority_and_offline_stage(self):
        pin = names.load_manifest()
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            dictionary = names.stage(stage)
            names.validate(dictionary, pin)
            with dictionary.open("rb") as stream:
                page0 = stream.read(256)
            self.assertEqual(page0[:8], b"VJDICT2\0")
            self.assertEqual(page0[192:256].rstrip(b"\0").decode(), pin["title"])
            # An exact rebuild, then reuse of a valid output without opening its source.
            second = names.prepare(stage / "second.vjdict")
            self.assertEqual(names.digest(second), names.digest(dictionary))
            names.prepare(second, stage / "missing-source.zip")
            for name in names.DOCS:
                self.assertEqual((stage / "docs" / name).read_bytes(), (ROOT / "docs" / name).read_bytes())
            for name in names.LICENSES:
                self.assertEqual((stage / "third_party/licenses" / name).read_bytes(),
                                 (ROOT / "third_party/licenses" / name).read_bytes())
            # A common word that is also a character name: words win same-length ties,
            # while a longer full name still finds the names dictionary.
            words = stage / "words.zip"
            with zipfile.ZipFile(words, "w") as z:
                z.writestr("index.json", json.dumps({"title": "Word fixture", "format": 3}))
                z.writestr("term_bank_1.json", json.dumps([
                    ["渚", "なぎさ", "", "", 0, ["shore (ordinary word)"], 1, ""]]))
            convert([words], dictionary.parent / "plaintext.vjdict")
            cli = os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli")
            def lookup(text):
                return subprocess.run([str(cli), "--dict", "local", "--local-dir", str(dictionary.parent),
                                       "--local-dicts", "plaintext.vjdict," + names.FILENAME,
                                       "--text", text, "--nav"], capture_output=True, text=True, check=True).stdout
            full = lookup("古河渚は")
            for part in ("Furukawa Nagisa", "CLANNAD [PSP, PS Vita]", "https://vndb.org/c478"):
                self.assertIn(part, full)
            shared = lookup("渚")
            self.assertIn("shore (ordinary word)", shared)
            self.assertLess(shared.index("shore (ordinary word)"), shared.index("Furukawa Nagisa"))
            dictionary.write_bytes(b"corrupted cache")
            names.prepare(dictionary)
            names.validate(dictionary, pin)

    def test_bad_source_or_output_pin_preserves_existing_dictionary(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / names.FILENAME
            output.write_bytes(b"existing dictionary")
            bad = Path(tmp) / "bad.zip"
            bad.write_bytes(b"bad source")
            with self.assertRaisesRegex(ValueError, "archive checksum"):
                names.prepare(output, bad)
            self.assertEqual(output.read_bytes(), b"existing dictionary")
            pin = dict(names.load_manifest(), sha256="0" * 64)
            with mock.patch.object(names, "load_manifest", return_value=pin), \
                    self.assertRaisesRegex(ValueError, "dictionary checksum"):
                names.prepare(output)
            self.assertEqual(output.read_bytes(), b"existing dictionary")


if __name__ == "__main__":
    unittest.main()
