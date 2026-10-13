"""The device default is H15_w8.vocr from bee-san/vita-vn-ocr v0.2.0, with FL10
as its fallback, and every pinned model fits the console budget ceiling."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr  # noqa: E402

# The sha256 published in the v0.2.0 release notes (and its asset digest).
H15_V020_SHA256 = "316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd"
# docs/vita-memory-budget.md in bee-san/vita-vn-ocr: BUDGET_READY weights<=1600000 arena<=262144.
CEILING_WEIGHTS, CEILING_ARENA = 1_600_000, 262_144


def c_table():
    """name -> (bytes, sha256 hex, arena) from core/vocr_ocr.c's release table."""
    source = (ROOT / "core/vocr_ocr.c").read_text()
    sizes = {m.group(1): int(m.group(2)) for m in
             re.finditer(r"#define (VJO_VOCR_\w+_BYTES) (\d+)u", (ROOT / "include/vjo_vocr_sizes.h").read_text())}
    sizes["VJO_VOCR2_BYTES"] = sizes["VJO_VOCR_FL10_BYTES"]
    table = source[source.index("#else\nstatic const VjoVocrModel models[]"):]
    out = {}
    for m in re.finditer(r'\{"?([\w.]+)"?, (VJO_\w+),\s*\{([^}]*)\},\s*(\d+), VOCR_LINES_\w+\}', table):
        name = "VOCR2_FL10_w8.vocr" if m.group(1) == "VJO_VOCR2_ARTIFACT_NAME" else m.group(1)
        digest = "".join("%02x" % int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", m.group(3)))
        out[name] = (sizes[m.group(2)], digest, int(m.group(4)))
    return out


class VocrDefaults(unittest.TestCase):
    def test_h15_is_the_published_v020_asset(self):
        url, size, sha = prepare_ocr.VOCR_ASSETS["H15_w8.vocr"]
        self.assertEqual(prepare_ocr.VOCR_TAG, "v0.2.0")
        self.assertTrue(url.endswith("/releases/download/v0.2.0/H15_w8.vocr"))
        self.assertEqual((size, sha), (1155968, H15_V020_SHA256))

    def test_c_and_python_pins_agree_and_fit_the_ceiling(self):
        table = c_table()
        self.assertEqual(set(table), set(prepare_ocr.VOCR_ASSETS) | set(prepare_ocr.VOCR2_ASSETS))
        for name, (size, sha, arena) in table.items():
            _, want_size, want_sha = {**prepare_ocr.VOCR_ASSETS, **prepare_ocr.VOCR2_ASSETS}[name]
            self.assertEqual((size, sha), (want_size, want_sha), name)
            self.assertLessEqual(size, CEILING_WEIGHTS, name)
            self.assertLessEqual(arena, CEILING_ARENA, name)

    def test_defaults_everywhere_are_h15_with_fl10_fallback(self):
        config = (ROOT / "core/config.c").read_text()
        self.assertIn('c->vocr_model, sizeof(c->vocr_model), "H15_w8.vocr"', config)
        self.assertIn('c->vocr_fallback_model, sizeof(c->vocr_fallback_model), "FL10_w8.vocr"', config)
        example = (ROOT / "docs/config.example.ini").read_text()
        self.assertIn("\nvocr_model = H15_w8.vocr\n", example)
        self.assertIn("\nvocr_fallback_model = FL10_w8.vocr\n", example)
        package = (ROOT / "tools/package_release.py").read_text()
        self.assertIn('"vocr_model": "H15_w8.vocr"', package)
        self.assertIn('"vocr_fallback_model": "FL10_w8.vocr"', package)
        self.assertIn('model_names = ["FL10_w8.vocr", "H15_w8.vocr"]', package)  # both shipped


if __name__ == "__main__":
    unittest.main()
