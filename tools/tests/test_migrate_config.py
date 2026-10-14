"""Run the installer's real config migration on configs saved by older releases."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MigrateConfig(unittest.TestCase):
    def migrate(self, old_text, *sets):
        with tempfile.TemporaryDirectory() as folder:
            old = Path(folder) / "old.ini"
            out = Path(folder) / "new.ini"
            old.write_text(old_text, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/migrate_config.py"), str(old),
                 str(ROOT / "docs/config.example.ini"), str(out), *sets],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return out.read_text(encoding="utf-8"), result.stdout

    def test_removed_meiki_backend_becomes_vocr_and_keeps_other_values(self):
        text, report = self.migrate("ocr_backend = Meiki\nocr_mode = on_press\n"
                                    "vocr_model = H15_w8.vocr\njpdb_api_key = secret-key\n")
        self.assertIn("ocr_backend = vocr\n", text)
        self.assertNotIn("meiki", text.lower())
        self.assertIn("ocr_mode = on_press\n", text)
        self.assertIn("vocr_model = H15_w8.vocr\n", text)
        self.assertIn("jpdb_api_key = secret-key\n", text)
        self.assertIn("config.ini: ocr_backend = vocr", report)
        self.assertNotIn("secret-key", report)

    def test_removed_engines_become_vocr_and_keep_other_values(self):
        # The VOCR2 trial (v0.7.12 to v0.8.0-test.1) and ncnn (host tools) are no engines any more.
        for old in ("vocr2", "VOCR2", "ncnn", "meiki"):
            text, report = self.migrate("ocr_backend = %s\nvocr_model = H15_w8.vocr\nocr_mode = on_press\n" % old)
            self.assertIn("\nocr_backend = vocr\n", text, old)
            self.assertNotIn("ocr_backend = " + old, text)
            self.assertEqual(text.count("ocr_backend ="), 1)
            self.assertIn("vocr_model = H15_w8.vocr\n", text)
            self.assertIn("ocr_mode = on_press\n", text)
            self.assertIn("config.ini: ocr_backend = vocr", report)

    def test_the_two_engines_are_unchanged(self):
        for backend in ("vocr", "lens"):
            text, report = self.migrate("ocr_backend = %s\n" % backend)
            self.assertIn("\nocr_backend = %s\n" % backend, text)
            self.assertNotIn("config.ini: ocr_backend", report)

    def test_explicit_setting_still_wins(self):
        text, _ = self.migrate("ocr_backend = meiki\n", "ocr_backend=lens")
        self.assertIn("ocr_backend = lens\n", text)

    def test_previous_release_config_gains_the_fl10_fallback(self):
        # v0.7.12's installer wrote vocr_model = FL10_w8.vocr; a source install
        # keeps that choice and adds the fallback key with its default.
        text, _ = self.migrate("ocr_backend = vocr\nvocr_model = FL10_w8.vocr\n")
        self.assertIn("vocr_model = FL10_w8.vocr\n", text)
        self.assertIn("vocr_fallback_model = VOCR3_FL10_w8.vocr\n", text)
        # The release installer selects the new default and its fallback.
        text, _ = self.migrate("vocr_model = H15_w8.vocr\nvocr_fallback_model = FL10_w8.vocr\n",
                               "vocr_model=VOCR3_H15_w8.vocr", "vocr_fallback_model=VOCR3_FL10_w8.vocr")
        self.assertIn("vocr_model = VOCR3_H15_w8.vocr\n", text)
        self.assertIn("vocr_fallback_model = VOCR3_FL10_w8.vocr\n", text)
        text, _ = self.migrate("vocr_fallback_model = none\n")
        self.assertIn("vocr_fallback_model = none\n", text)
        self.assertEqual(text.count("vocr_fallback_model ="), 1)

    def test_release_installer_sets_vocr3_h15_with_vocr3_fl10_fallback(self):
        script = (ROOT / "tools/install-vita.sh").read_text()
        self.assertIn("--set vocr_model=VOCR3_H15_w8.vocr", script)
        self.assertIn("--set vocr_fallback_model=VOCR3_FL10_w8.vocr", script)
        self.assertNotIn("vocr_model=FL10_w8.vocr", script)
        self.assertIn("--set log_file=on", script)

    def test_anki_settings_are_kept_and_mining_stays_off_until_set(self):
        # A config from the LAN-only release: anki_host alone used to send.
        text, _ = self.migrate("anki_host = 192.168.1.23\nanki_audio_url = http://h/?t={term};k\n")
        self.assertIn("anki_mining = off\n", text)
        self.assertIn("anki_host = 192.168.1.23\n", text)
        self.assertIn("anki_audio_url = http://h/?t={term};k\n", text)
        text, _ = self.migrate("anki_mining = on\nanki_host = auto\n")
        self.assertIn("anki_mining = on\n", text)
        self.assertEqual(text.count("anki_mining"), 1)

    def test_the_field_mapping_of_earlier_builds_is_removed(self):
        # Kiku is the one note type now: no deck, note type, tags or fields to set.
        old = ("anki_host = auto\nanki_deck = Mining #1 ; main\nanki_note_type = Lapis\nanki_tags = a b\n"
               + "".join("anki_field_%s = X\n" % f for f in (
                   "word", "reading", "furigana", "definition", "sentence", "picture", "frequency", "audio",
                   "sentence_furigana", "pitch_position", "pitch_categories", "frequency_list", "misc_info"))
               + "anki_field_wrod = Word\n")
        text, out = self.migrate(old)
        self.assertNotIn("anki_deck", text)
        self.assertNotIn("anki_note_type", text)
        self.assertNotIn("anki_tags", text)
        self.assertNotIn("anki_field_", text)
        self.assertIn("anki_host = auto\n", text)
        self.assertIn("config.ini: anki_deck removed", out)
        self.assertIn("config.ini: anki_field_misc_info removed", out)
        self.assertIn("config.ini: anki_field_wrod removed", out)

if __name__ == "__main__":
    unittest.main()
