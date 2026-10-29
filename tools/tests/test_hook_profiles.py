"""Profile exporter/parser and shipped-mode parity with the built-in catalogue."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import hook_profiles

class HookProfiles(unittest.TestCase):
    def test_all_games_match_built_in_catalogue(self):
        with tempfile.TemporaryDirectory(prefix="rubi-profile-test-") as d:
            d = Path(d)
            archive = d / "profiles.vjhp"
            hook_profiles.build(archive)
            outputs = []
            for external in (False, True):
                exe = d / ("external" if external else "builtin")
                sources = ["host/tests/profile_vectors.c", "core/hook_profile.c", "core/hook_profile_file.c",
                           "core/hook_luna.c", "core/hook_place.c", "core/hook_re.c", "core/hook_text.c"]
                command = ["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                           f'-DVJO_HOOK_DEFINITION_SHA256="{hook_profiles.revision()}"']
                if external:
                    command.append("-DVJO_EXTERNAL_HOOK_PROFILES=1")
                subprocess.run(command + [*[str(ROOT / p) for p in sources], "-o", str(exe)], check=True)
                outputs.append(subprocess.check_output([str(exe), str(archive)]))
            self.assertEqual(outputs[0], outputs[1])
            self.assertIn(b"AFEJ00001 0 1\n", outputs[0])
            self.assertIn(b"PCSG00415", outputs[0])
            self.assertIn(b"PCSG00628", outputs[0])
            self.assertIn(b"PCSG01282", outputs[0])
            # Per-title tables and programs must be absent from the shipped mode.
            symbols = subprocess.check_output(["nm", "-a", str(d / "external")], text=True)
            for name in ("vjo_luna_table", "vjo_luna_ids", "vjo_luna_filters", "vjo_rubi_filters", "f_PCSG", "r_PCSG"):
                self.assertNotIn(name, symbols)

if __name__ == "__main__":
    unittest.main()
