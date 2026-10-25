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


class PerGameNames(unittest.TestCase):
    def test_the_pack_puts_the_running_games_names_first(self):
        pin = names.load_manifest()["per_game"]
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            names.stage(stage)
            folder = stage / "ux0/data/VitaJPOverlay/dictionaries"
            pack = folder / names.GAMES
            names.validate_games(pack, pin)
            data = pack.read_bytes()
            magic, version, header, count, size, records, total = names.GAMES_HEADER.unpack_from(data)
            self.assertEqual((magic, version, header, size, records, total),
                             (names.GAMES_MAGIC, 1, 64, names.GAMES_RECORD.size, 64, len(data)))
            self.assertEqual(count, pin["title_ids"])
            ids = [names.GAMES_RECORD.unpack_from(data, 64 + 24 * i) for i in range(count)]
            self.assertEqual([r[0] for r in ids], sorted({r[0] for r in ids}))  # sorted, each once
            for _, offset, length in ids:
                self.assertEqual(data[offset:offset + 8], b"VJDICT1\0")
                self.assertLessEqual(offset + length, len(data))
            # an exact rebuild, and no common word in any game's names
            second = names.prepare_games(stage / "second" / names.GAMES)
            self.assertEqual(names.digest(second), names.digest(pack))
            guarded = names.guard_words()
            self.assertIn("一", guarded)
            for _, offset, length in ids:
                self.assertNotIn("\0一\0".encode(), data[offset:offset + length])
            words = stage / "words.zip"
            with zipfile.ZipFile(words, "w") as z:
                z.writestr("index.json", json.dumps({"title": "Word fixture", "format": 3}))
                z.writestr("term_bank_1.json", json.dumps([
                    ["渚", "なぎさ", "", "", 0, ["shore (ordinary word)"], 1, ""],
                    ["一", "いち", "", "", 0, ["one (ordinary word)"], 2, ""]]))
            convert([words], folder / "plaintext.vjdict")
            cli = os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli")
            def lookup(text, *game):
                run = subprocess.run([str(cli), "--dict", "local", "--local-dir", str(folder), "--local-dicts",
                                      "plaintext.vjdict," + names.FILENAME, "--text", text, "--nav", *game],
                                     capture_output=True, text=True, check=True)
                return run.stdout, run.stderr
            for game in ("PCSG00415", "ULJM05338"):  # CLANNAD on the Vita, and its PSP disc
                out, err = lookup("渚", "--game", game)
                self.assertIn(f"names for {game}: this game's", err)
                self.assertLess(out.index("Furukawa Nagisa"), out.index("shore (ordinary word)"))
                self.assertNotIn("Chinen Nagisa", out.split("shore (ordinary word)")[0])  # this game's only
            out, err = lookup("渚", "--game", "PCSG99999")
            self.assertIn("names for PCSG99999: none", err)
            self.assertLess(out.index("shore (ordinary word)"), out.index("Furukawa Nagisa"))
            # Hakuouki's Saitou Hajime (斎藤 一): his full name is this game's, 一 alone stays the word
            out, _ = lookup("一", "--game", "PCSG00217")
            self.assertLess(out.index("[Word fixture]"), out.index("Saitou Hajime"))
            out, _ = lookup("斎藤一", "--game", "PCSG00217")
            self.assertEqual(out.split("\n")[3:5], ["[Hakuouki ~Shinsengumi Kitan~]", "Saitou Hajime (斎藤 一)"])
            pack.write_bytes(b"VJGAMES1" + bytes(56))
            out, err = lookup("渚", "--game", "PCSG00415")
            self.assertIn("names for PCSG00415: pack unreadable", err)
            self.assertLess(out.index("shore (ordinary word)"), out.index("Furukawa Nagisa"))

    def test_a_bad_archive_or_guard_preserves_the_existing_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / names.GAMES
            output.write_bytes(b"existing pack")
            bad = Path(tmp) / "bad.zip"
            bad.write_bytes(b"bad source")
            with self.assertRaisesRegex(ValueError, "archive checksum"):
                names.prepare_games(output, bad)
            manifest = names.load_manifest()
            pin = dict(manifest, per_game=dict(manifest["per_game"], guard_sha256="0" * 64))
            with mock.patch.object(names, "load_manifest", return_value=pin), \
                    self.assertRaisesRegex(ValueError, "guard list checksum"):
                names.prepare_games(output)
            self.assertEqual(output.read_bytes(), b"existing pack")


if __name__ == "__main__":
    unittest.main()
