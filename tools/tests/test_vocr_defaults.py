"""The device default is VOCR3_H15_w8.vocr from bee-san/vita-vn-ocr v0.2.5-vocr3.1, with
VOCR3_FL10_w8.vocr as its fallback; v0.2.0's H15/FL10 stay pinned and shipped; every pinned
model fits the console budget ceiling, and the C, kernel and Python pins agree."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr  # noqa: E402

# The sha256s in the v0.2.0 release notes and in v0.2.5-vocr3.1's SHA256SUMS (the VOCR3_H15 and
# VOCR3_FL10 arms of vita-vn-ocr eval/vocr3_report_spec_20261013.json).
H15_V020_SHA256 = "316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd"
VOCR3_H15_SHA256 = "cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c"
VOCR3_FL10_SHA256 = "87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154"
# docs/vita-memory-budget.md in bee-san/vita-vn-ocr: BUDGET_READY weights<=1600000 arena<=262144.
CEILING_WEIGHTS, CEILING_ARENA = 1_600_000, 262_144


def macros():
    """#define NAME value (number or string) of include/vjo_vocr_sizes.h."""
    text = (ROOT / "include/vjo_vocr_sizes.h").read_text()
    out = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define (VJO_\w+) (\d+)u", text)}
    out.update({m.group(1): m.group(2) for m in re.finditer(r'#define (VJO_\w+) "([^"]+)"', text)})
    out["VJO_VOCR2_BYTES"] = out["VJO_VOCR_FL10_BYTES"]
    return out


def c_table():
    """name -> (bytes, sha256 hex, arena) from core/vocr_ocr.c's release table."""
    source = (ROOT / "core/vocr_ocr.c").read_text()
    sizes = macros()
    table = source[source.index("#else\nstatic const VjoVocrModel models[]"):]
    out = {}
    for m in re.finditer(r'\{"?([\w.]+)"?, (VJO_\w+),\s*\{([^}]*)\},\s*(\d+), VOCR_LINES_\w+\}', table):
        name = sizes.get(m.group(1), m.group(1))  # a name macro (VJO_VOCR3_H15_NAME) or a literal
        digest = "".join("%02x" % int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", m.group(3)))
        out[name] = (sizes[m.group(2)], digest, int(m.group(4)))
    return out


def kernel_names():
    """name -> bytes the kernel's model broker accepts (kernel/game_ocr.c model_size)."""
    source = (ROOT / "kernel/game_ocr.c").read_text()
    body = source[source.index("static uint32_t model_size(const char *name)"):]
    body = body[:body.index("\n}\n")]
    sizes = macros()
    return {sizes.get(n, n.strip('"')): sizes[b]
            for n, b in re.findall(r'!strcmp\(name, ("[\w.]+"|VJO_\w+)\)\) return (VJO_\w+);', body)}


class VocrDefaults(unittest.TestCase):
    def test_vocr3_assets_are_the_published_release(self):
        self.assertEqual(prepare_ocr.VOCR3_TAG, "v0.2.5-vocr3.1")
        for name, size, sha in (("VOCR3_H15_w8.vocr", 1155968, VOCR3_H15_SHA256),
                                ("VOCR3_FL10_w8.vocr", 657664, VOCR3_FL10_SHA256)):
            url, got_size, got_sha = prepare_ocr.VOCR3_ASSETS[name]
            self.assertTrue(url.endswith("/releases/download/v0.2.5-vocr3.1/" + name), url)
            self.assertEqual((got_size, got_sha), (size, sha))
        url, size, sha = prepare_ocr.VOCR_ASSETS["H15_w8.vocr"]
        self.assertEqual(prepare_ocr.VOCR_TAG, "v0.2.0")
        self.assertTrue(url.endswith("/releases/download/v0.2.0/H15_w8.vocr"))
        self.assertEqual((size, sha), (1155968, H15_V020_SHA256))

    def test_c_kernel_and_python_pins_agree_and_fit_the_ceiling(self):
        table = c_table()
        python = {**prepare_ocr.VOCR3_ASSETS, **prepare_ocr.VOCR_ASSETS, **prepare_ocr.VOCR2_ASSETS}
        self.assertEqual(set(table), set(python))
        for name, (size, sha, arena) in table.items():
            _, want_size, want_sha = python[name]
            self.assertEqual((size, sha), (want_size, want_sha), name)
            self.assertLessEqual(size, CEILING_WEIGHTS, name)
            self.assertLessEqual(arena, CEILING_ARENA, name)
        # the kernel opens exactly the pinned names, at their pinned sizes
        self.assertEqual(kernel_names(), {name: size for name, (size, _, _) in table.items()})
        # VOCR3 has the shapes of v0.2.0's H15 and FL10: the same bytes and arena, other weights
        for new, old in (("VOCR3_H15_w8.vocr", "H15_w8.vocr"), ("VOCR3_FL10_w8.vocr", "FL10_w8.vocr")):
            self.assertEqual(table[new][0], table[old][0])
            self.assertEqual(table[new][2], table[old][2])
            self.assertNotEqual(table[new][1], table[old][1])

    def test_defaults_everywhere_are_vocr3_h15_with_vocr3_fl10_fallback(self):
        self.assertEqual((prepare_ocr.VOCR_DEFAULT_MODEL, prepare_ocr.VOCR_FALLBACK_MODEL),
                         ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"))
        config = (ROOT / "core/config.c").read_text()
        self.assertIn('c->vocr_model, sizeof(c->vocr_model), "VOCR3_H15_w8.vocr"', config)
        self.assertIn('c->vocr_fallback_model, sizeof(c->vocr_fallback_model), "VOCR3_FL10_w8.vocr"', config)
        self.assertIn('#define VJO_VOCR_DEFAULT_MODEL "VOCR3_H15_w8.vocr"', (ROOT / "core/vocr_ocr.h").read_text())
        example = (ROOT / "docs/config.example.ini").read_text()
        self.assertIn("\nvocr_model = VOCR3_H15_w8.vocr\n", example)
        self.assertIn("\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n", example)
        self.assertIn("\nlog_file = on\n", example)
        installer = (ROOT / "tools/install-vita.sh").read_text()
        self.assertIn("--set vocr_model=VOCR3_H15_w8.vocr", installer)
        self.assertIn("--set vocr_fallback_model=VOCR3_FL10_w8.vocr", installer)
        package = (ROOT / "tools/package_release.py").read_text()
        self.assertIn('"vocr_model": VOCR_DEFAULT_MODEL', package)
        self.assertIn('"vocr_fallback_model": VOCR_FALLBACK_MODEL', package)
        # VOCR3 and the v0.2.0 fallback are both shipped, each with its own release's pins
        import package_release  # noqa: E402
        shipped = {name: (pins[name], tag, role) for name, pins, tag, role in package_release.shipped_models()}
        self.assertEqual(list(shipped)[:4], ["VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "H15_w8.vocr", "FL10_w8.vocr"])
        self.assertEqual(shipped["VOCR3_H15_w8.vocr"][1:], ("v0.2.5-vocr3.1", "default"))
        self.assertEqual(shipped["VOCR3_FL10_w8.vocr"][1:], ("v0.2.5-vocr3.1", "fallback"))
        self.assertEqual(shipped["H15_w8.vocr"][0][2], H15_V020_SHA256)
        self.assertEqual(shipped["H15_w8.vocr"][1], "v0.2.0")


if __name__ == "__main__":
    unittest.main()
