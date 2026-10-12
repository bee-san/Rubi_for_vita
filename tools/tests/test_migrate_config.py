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

    def test_current_backends_are_unchanged(self):
        for backend in ("vocr", "vocr2", "lens"):
            text, _ = self.migrate("ocr_backend = %s\n" % backend)
            self.assertIn("ocr_backend = %s\n" % backend, text)

    def test_explicit_setting_still_wins(self):
        text, _ = self.migrate("ocr_backend = meiki\n", "ocr_backend=lens")
        self.assertIn("ocr_backend = lens\n", text)


if __name__ == "__main__":
    unittest.main()
