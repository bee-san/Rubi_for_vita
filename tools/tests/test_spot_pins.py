"""API 45: the text spotter's pins (vita-vn-ocr #128; config.ini ocr_finder = spotter). include/vjo_vocr_sizes.h's
VJO_VOCR_SPOT_* lines are the one place of its tag, name, bytes, SHA-256, attribution, form and whether it is published:
C (core/vocr_ocr.c, the kernel, include/vjo_payload.h), CMake (cmake/VocrOCR.cmake's VOCR_TEXTSPOT6_NO_MS default) and
the tools (prepare_ocr.py, package_release.py, package_vpk.py, e2e_package.py) read them, so pinning the published pick
changes one line per constant. Until then the pin is a placeholder that fetches, packages and installs nothing."""
import hashlib
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import package_release  # noqa: E402
import prepare_ocr  # noqa: E402

SIZES = ROOT / "include/vjo_vocr_sizes.h"
FIXTURE = ROOT / "host/fixtures/spotter/textspot6_tiny_s1.vtsp"


def with_pin(text, **values):
    """include/vjo_vocr_sizes.h's text with VJO_VOCR_SPOT_<KEY> lines set to the given values (as written)."""
    for key, value in values.items():
        text, n = re.subn(rf"^#define VJO_VOCR_SPOT_{key} \S+$", f"#define VJO_VOCR_SPOT_{key} {value}", text,
                          flags=re.M)
        assert n == 1, key
    return text


class Pins(unittest.TestCase):
    def test_each_pin_is_one_line_of_its_form(self):
        pin = prepare_ocr.spot_pin()
        self.assertEqual(set(pin), {"tag", "name", "bytes", "sha256", "attribution_bytes", "attribution_sha256",
                                    "form", "published"})
        text = SIZES.read_text()
        for field in prepare_ocr.VOCR_SPOT_FIELDS:
            self.assertEqual(len(re.findall(rf"^#define VJO_VOCR_SPOT_{field} ", text, re.M)), 1, field)
        # the pins fit the ceilings the C code and the memory ledger hold (include/vjo_vocr_sizes.h)
        maximum = int(re.search(r"^#define VJO_VOCR_SPOT_MAX_BYTES (\d+)u$", text, re.M).group(1))
        self.assertTrue(32 < pin["bytes"] <= maximum == 512000)
        self.assertEqual(int(re.search(r"^#define VJO_VOCR_SPOT_ARENA_MAX (\d+)u$", text, re.M).group(1)), 262144)
        # a name the kernel's request field holds (kernel/game_ocr.c checks it at compile time)
        self.assertLess(len(pin["name"]) + 1, 32)

    def test_the_placeholder_is_unpublished_and_fetches_nothing(self):
        """The pre-release v0.3.8-textspot6.1 does not exist yet: the placeholder's hashes are zeros, which no file
        has (core/vocr_ocr.c reads with GRAD), and nothing is fetched or packaged for it."""
        pin = prepare_ocr.spot_pin()
        if pin["published"]:
            self.skipTest("the spotter's pin is published")
        self.assertEqual((pin["tag"], pin["sha256"], pin["attribution_sha256"]),
                         ("v0.3.8-textspot6.1", "0" * 64, "0" * 64))
        self.assertFalse(prepare_ocr.VOCR_SPOT_PUBLISHED)
        with mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
            self.assertEqual(prepare_ocr.fetch_spot(Path("/nonexistent")), [])
        self.assertNotIn(prepare_ocr.VOCR_SPOT_MODEL, {name for name, *_ in package_release.shipped_models()})
        self.assertIn("#define VJO_VOCR_SPOT_PUBLISHED 0", SIZES.read_text())

    def test_a_published_pin_is_checked(self):
        """VJO_VOCR_SPOT_PUBLISHED 1 needs real hashes and the attribution's size; malformed or doubled lines are
        refused, so a swap that misses one fails here and not on a console."""
        text = SIZES.read_text()
        good = with_pin(text, PUBLISHED="1", SHA256='"' + "ab" * 32 + '"', ATTRIBUTION_SHA256='"' + "cd" * 32 + '"',
                        ATTRIBUTION_BYTES="2952u")
        bad = {
            "a placeholder hash": with_pin(text, PUBLISHED="1", ATTRIBUTION_SHA256='"' + "cd" * 32 + '"',
                                           ATTRIBUTION_BYTES="2952u"),
            "no attribution size": with_pin(good, ATTRIBUTION_BYTES="0u"),
            "upper-case hex": with_pin(good, SHA256='"' + "AB" * 32 + '"'),
            "a short hash": with_pin(good, SHA256='"abcd"'),
            "another extension": with_pin(good, NAME='"textspot6_x.vtd"'),
            "another form": with_pin(good, FORM='"deep"'),
            "a line twice": good + "\n#define VJO_VOCR_SPOT_BYTES 1u\n",
            "a line missing": re.sub(r"^#define VJO_VOCR_SPOT_FORM .*$", "", good, flags=re.M),
        }
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "vjo_vocr_sizes.h"
            header.write_text(good)
            pin = prepare_ocr.spot_pin(header)
            self.assertTrue(pin["published"] and pin["sha256"] == "ab" * 32 and pin["attribution_bytes"] == 2952)
            for what, body in bad.items():
                header.write_text(body)
                with self.assertRaises(ValueError, msg=what):
                    prepare_ocr.spot_pin(header)
        # a copy of the script without the header (an older release zip's tools/): no pin, nothing fetched
        with mock.patch.object(prepare_ocr, "VOCR_SPOT_HEADERS", (Path("/nonexistent/vjo_vocr_sizes.h"),)):
            self.assertIsNone(prepare_ocr.spot_pin())

    def test_the_host_tests_pin_is_the_committed_random_blob(self):
        """host/tests/test_spot_pins.h (the C tests' VJO_VOCR_SPOT_TEST) names the fixture, at its size and hash."""
        data = FIXTURE.read_bytes()
        pins = (ROOT / "host/tests/test_spot_pins.h").read_text()
        self.assertIn(f'#define VJO_VOCR_SPOT_NAME "{FIXTURE.name}"', pins)
        self.assertIn(f"#define VJO_VOCR_SPOT_BYTES {len(data)}u", pins)
        self.assertIn(f'#define VJO_VOCR_SPOT_SHA256 "{hashlib.sha256(data).hexdigest()}"', pins)
        self.assertEqual(data[:4], b"VTSP")
        self.assertEqual(int.from_bytes(data[4:8], "little"), 4)  # VTSP version 4, the tagged spotter
        self.assertLess(len(data), 8192)  # a few KB: random weights, not a trained model
        # the README names the hash too
        self.assertIn(hashlib.sha256(data).hexdigest(), (ROOT / "host/fixtures/spotter/README.md").read_text())

    def test_the_build_takes_the_form_and_the_runtime_pins_from_cmake(self):
        """cmake/VocrOCR.cmake's VOCR_TEXTSPOT6_NO_MS default reads VJO_VOCR_SPOT_FORM, and the spotter's runtime
        files are pinned at their own commit (until vita-vn-ocr merges them), which tools/fast_gate.sh and CI read."""
        cmake = (ROOT / "cmake/VocrOCR.cmake").read_text()
        self.assertIn('REGEX "^#define VJO_VOCR_SPOT_FORM \\"(trunk|ms)\\"$"', cmake)
        commit = re.search(r"^set\(VJO_VOCR_SPOT_COMMIT ([0-9a-f]{40})\)$", cmake, re.M).group(1)
        for name in ("vocr_textspot6.c", "vocr_textspot6.h", "vocr_textspot.h"):
            self.assertRegex(cmake, rf"\n  {re.escape(name)} [0-9a-f]{{64}}\n")
        self.assertIn('"${_vocr_spot_rt}/vocr_textspot6.c"', cmake)
        gate = (ROOT / "tools/fast_gate.sh").read_text()
        self.assertIn("s/^set(VJO_VOCR_SPOT_COMMIT", gate)
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        self.assertEqual(workflow.count(f"ref: {commit}"), 3)  # the host, ARM and Vita jobs' checkouts
        self.assertEqual(workflow.count("VJO_VOCR_SPOT_SOURCE_DIR=$PWD/.refs/vita-vn-ocr-spot"), 4)


class Packaging(unittest.TestCase):
    def test_a_published_spotter_is_fetched_and_shipped(self):
        """With the pin published, fetch_tiers (--backend vocr-tiers) fetches the weights and the release's
        ATTRIBUTION.md kept as SPOTTER-ATTRIBUTION.md, each checked, and the package ships both."""
        weights, attribution = FIXTURE.read_bytes(), b"the spotter's attribution fixture"
        assets = {prepare_ocr.VOCR_SPOT_MODEL: ("https://fixture.invalid/s", len(weights),
                                                hashlib.sha256(weights).hexdigest())}
        att_pin = {prepare_ocr.VOCR_SPOT_ATTRIBUTION: ("https://fixture.invalid/sa", len(attribution),
                                                       hashlib.sha256(attribution).hexdigest())}
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "src", Path(tmp) / "out"
            src.mkdir()
            (src / prepare_ocr.VOCR_SPOT_MODEL).write_bytes(weights)
            (src / "ATTRIBUTION.md").write_bytes(attribution)  # the release's name for it
            with mock.patch.object(prepare_ocr, "VOCR_SPOT_PUBLISHED", True), \
                 mock.patch.object(prepare_ocr, "VOCR_SPOT_ASSETS", assets), \
                 mock.patch.object(prepare_ocr, "VOCR_SPOT_ATTRIBUTION_PIN", att_pin), \
                 mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
                paths = prepare_ocr.fetch_spot(out, src)
                self.assertEqual([p.name for p in paths], [prepare_ocr.VOCR_SPOT_MODEL, "SPOTTER-ATTRIBUTION.md"])
                self.assertEqual((out / "SPOTTER-ATTRIBUTION.md").read_bytes(), attribution)
                # another file under its name is refused
                (out / prepare_ocr.VOCR_SPOT_MODEL).unlink()
                (src / prepare_ocr.VOCR_SPOT_MODEL).write_bytes(weights[:-1] + b"\0")
                with self.assertRaises(RuntimeError):
                    prepare_ocr.fetch_spot(out, src)
            with mock.patch.object(package_release, "VOCR_SPOT_PUBLISHED", True), \
                 mock.patch.object(package_release, "VOCR_SPOT_ASSETS", assets):
                shipped = {name: role for name, _, _, role in package_release.shipped_models()}
                self.assertEqual(shipped[prepare_ocr.VOCR_SPOT_MODEL], "text spotter (ocr_finder = spotter)")

    def test_the_tools_name_the_spotters_files(self):
        """install_ftp.sh uploads them, collect_vita.py hashes them, once a package carries them."""
        self.assertIn("ocr/*.vtsp", (ROOT / "tools/install_ftp.sh").read_text())
        self.assertIn("SPOTTER-ATTRIBUTION.md", (ROOT / "tools/install_ftp.sh").read_text())
        import collect_vita  # noqa: PLC0415
        ocr = next(pattern for path, _, pattern in collect_vita.DIRS if path.endswith("/ocr"))
        self.assertTrue(ocr.search("textspot6_x_w8.vtsp") and ocr.search("SPOTTER-ATTRIBUTION.md"))
        # the release zip's copy of prepare_ocr.py reads the header beside it (vita/CMakeLists.txt)
        self.assertIn("${ROOT}/include/vjo_vocr_sizes.h ${STAGE}/tools/", (ROOT / "vita/CMakeLists.txt").read_text())
        self.assertEqual(prepare_ocr.VOCR_SPOT_HEADERS[1].name, "vjo_vocr_sizes.h")


if __name__ == "__main__":
    unittest.main()
