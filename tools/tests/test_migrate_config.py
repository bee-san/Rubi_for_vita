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
        self.assertIn("vocr_tier = normal\n", text)  # H15: Normal
        self.assertNotIn("vocr_model", text)
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
            self.assertIn("vocr_tier = normal\n", text)
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

    def test_older_model_choices_become_their_model_size(self):
        # API 21: vocr_model and vocr_fallback_model became one model size, vocr_tier. Each older
        # model by its family: FL10 (v0.7.12's installer wrote FL10_w8.vocr, the VOCR2 trial was
        # FL10-sized) Small, H15 Normal, F20 and W20 Large; both lines go.
        for model, tier in (("FL10_w8.vocr", "small"), ("VOCR3_FL10_w8.vocr", "small"),
                            ("VOCR2_FL10_w8.vocr", "small"), ("H15_w8.vocr", "normal"),
                            ("VOCR3_H15_w8.vocr", "normal"), ("F20_w8.vocr", "large"),
                            ("VOCR4_W20_w8.vocr", "large")):
            text, report = self.migrate("ocr_backend = vocr\nvocr_model = %s\nvocr_fallback_model = none\n" % model)
            self.assertIn("\nvocr_tier = %s\n" % tier, text, model)
            self.assertEqual(text.count("vocr_tier ="), 1)
            self.assertNotIn("vocr_model", text)
            self.assertNotIn("vocr_fallback_model", text)
            self.assertIn("config.ini: vocr_model removed", report)
            self.assertIn("config.ini: vocr_fallback_model removed", report)
            self.assertIn("config.ini: vocr_tier = " + tier, report)
        # an unknown name keeps the template's size, Normal
        text, _ = self.migrate("vocr_model = VOCR9_X_w8.vocr\n")
        self.assertIn("\nvocr_tier = normal\n", text)
        self.assertNotIn("vocr_model", text)
        # a file that names its size keeps it, whatever an older line says
        text, _ = self.migrate("vocr_tier = large\nvocr_model = FL10_w8.vocr\n")
        self.assertIn("\nvocr_tier = large\n", text)
        text, report = self.migrate("vocr_tier = small\n")
        self.assertIn("\nvocr_tier = small\n", text)
        self.assertNotIn("vocr_tier", report)  # unchanged
        # a fresh install: the template's Normal, no model line
        text, _ = self.migrate("")
        self.assertIn("\nvocr_tier = normal\n", text)
        self.assertNotIn("vocr_model", text)

    def test_an_older_installers_model_settings_set_the_size(self):
        # tools/install_ftp.sh --set vocr_model=... of an older checkout: its size, explicitly
        text, _ = self.migrate("vocr_tier = normal\n", "vocr_model=FL10_w8.vocr", "vocr_fallback_model=none")
        self.assertIn("\nvocr_tier = small\n", text)
        self.assertNotIn("vocr_model", text)
        self.assertNotIn("vocr_fallback_model", text)
        # a vocr_tier setting wins over a vocr_model one
        text, _ = self.migrate("", "vocr_model=FL10_w8.vocr", "vocr_tier=large")
        self.assertIn("\nvocr_tier = large\n", text)
        self.assertEqual(text.count("vocr_tier ="), 1)

    def test_release_installer_keeps_the_chosen_model_size(self):
        # The Overlay app's Model size is the user's: the installer sets no size and no model, so an
        # update keeps the choice; a first install gets the template's Normal.
        script = (ROOT / "tools/install-vita.sh").read_text()
        self.assertNotIn("vocr_model", script)
        self.assertNotIn("vocr_fallback_model", script)
        self.assertNotIn("--set vocr_tier", script)
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

    def test_a_credential_is_reported_only_as_set_or_empty(self):
        # an API key, and the audio source's URL, which can carry one (audit-overlay-test3-cleanup B1)
        url = "https://audio.invalid/a?term={term}&key=url-secret-99"
        text, report = self.migrate("jpdb_api_key = old-key-1\n", f"anki_audio_url={url}", "jiten_api_key=new-key-2")
        self.assertIn(f"anki_audio_url = {url}\n", text)
        self.assertIn("jiten_api_key = new-key-2\n", text)
        self.assertIn("config.ini: anki_audio_url = set", report)
        self.assertIn("config.ini: jiten_api_key = set", report)
        for secret in ("url-secret-99", "audio.invalid", "new-key-2", "old-key-1"):
            self.assertNotIn(secret, report)
        _, report = self.migrate(f"anki_audio_url = {url}\n", "anki_audio_url=")
        self.assertIn("config.ini: anki_audio_url = empty", report)
        self.assertNotIn("url-secret-99", report)

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
