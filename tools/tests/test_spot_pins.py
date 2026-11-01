"""API 54: the text spotter's pins (vita-vn-ocr #149; config.ini ocr_finder = spotter, the default). include/vjo_vocr_sizes.h's
VJO_VOCR_SPOT_* lines are the one place of its tag, name, bytes, SHA-256, attribution, form and whether it is published:
C (core/vocr_ocr.c, the kernel, include/vjo_payload.h), CMake (cmake/VocrOCR.cmake's VOCR_TEXTSPOT6_NO_MS default) and
the tools (prepare_ocr.py, package_release.py, package_vpk.py, e2e_package.py) read them, so pinning another published
pick changes one line per constant. The pin is vita-vn-ocr #149's pick (v0.3.10-textspot8.1)."""
import hashlib
import re
import shutil
import subprocess
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

    def test_the_pin_is_vita_vn_ocrs_published_pick(self):
        """vita-vn-ocr #149's pick, pre-release v0.3.10-textspot8.1 (its SHA256SUMS: the weights and ATTRIBUTION.md),
        the trunk form; a package ships the weights and the attribution kept as SPOTTER-ATTRIBUTION.md."""
        pin = prepare_ocr.spot_pin()
        self.assertTrue(pin["published"] and prepare_ocr.VOCR_SPOT_PUBLISHED)
        self.assertEqual((pin["tag"], pin["name"], pin["bytes"], pin["form"]),
                         ("v0.3.10-textspot8.1", "textspot8_t64n8c16_w8.vtsp", 52176, "trunk"))
        self.assertEqual(pin["sha256"], "e129d41adfa02c658a7d13d6f98064e6ab6ce9a2bc8c7a56da700a21357b5e1c")
        self.assertEqual((pin["attribution_bytes"], pin["attribution_sha256"]),
                         (4183, "fd44f500114ca2c8373e02bda1134753619e76de869ddf8ec86c14e008be5b84"))
        self.assertIn(prepare_ocr.VOCR_SPOT_MODEL, {name for name, *_ in package_release.shipped_models()})

    def test_a_published_pin_is_checked(self):
        """VJO_VOCR_SPOT_PUBLISHED 1 needs real hashes and the attribution's size; malformed or doubled lines are
        refused, so a swap that misses one fails here and not on a console."""
        text = SIZES.read_text()
        good = with_pin(text, PUBLISHED="1", SHA256='"' + "ab" * 32 + '"', ATTRIBUTION_SHA256='"' + "cd" * 32 + '"',
                        ATTRIBUTION_BYTES="2952u")
        bad = {
            "a placeholder hash": with_pin(text, PUBLISHED="1", SHA256='"' + "0" * 64 + '"',
                                           ATTRIBUTION_SHA256='"' + "cd" * 32 + '"', ATTRIBUTION_BYTES="2952u"),
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
        # the host, ARM and Vita jobs' checkouts: the action they share (its spotter's checkout, and the recognizer's
        # when one commit pins both)
        pin = re.search(r"^set\(VJO_VOCR_COMMIT ([0-9a-f]{40})\)$", cmake, re.M).group(1)
        action = (ROOT / ".github/actions/pinned-sources/action.yml").read_text()
        self.assertEqual(action.count(f"ref: {commit}"), 2 if commit == pin else 1)
        self.assertEqual(action.count("path: .refs/vita-vn-ocr-spot\n"), 1)
        self.assertEqual(workflow.count("\n      - uses: ./.github/actions/pinned-sources\n"), 3)
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


class ReusedBuildDirectory(unittest.TestCase):
    """cmake/VocrOCR.cmake's VJO_VOCR_SPOT_NO_MS in a reused build directory (the fast gate's, a developer's): its
    default comes from the pinned form, and a cache keeps an option's first value, so after the form went from "ms"
    back to "trunk" the directory built the multi-scale code CI's new directories leave out (audit-rubi-cleanup-8-b
    F1). The file's own block, run by CMake on a copy of include/vjo_vocr_sizes.h whose form each step sets."""

    def setUp(self):
        self.cmake = shutil.which("cmake")
        if self.cmake is None:
            self.skipTest("no cmake on PATH")
        text = (ROOT / "cmake/VocrOCR.cmake").read_text(encoding="utf-8")
        block = re.search(r'^file\(STRINGS "\$\{VJO_ROOT\}/include/vjo_vocr_sizes\.h" _spot_form .*?'
                          r'^if\(VJO_VOCR_SPOT_NO_MS AND _spot_form STREQUAL "ms"\)\n.*?^endif\(\)$', text, re.M | re.S)
        self.assertIsNotNone(block, "cmake/VocrOCR.cmake's VJO_VOCR_SPOT_FORM block")
        self.tmp = Path(tempfile.mkdtemp(prefix="vjo-spotform-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        (self.tmp / "root/include").mkdir(parents=True)
        (self.tmp / "src").mkdir()
        (self.tmp / "src/CMakeLists.txt").write_text(
            f'cmake_minimum_required(VERSION 3.16)\nproject(spot_form NONE)\nset(VJO_ROOT "{self.tmp / "root"}")\n'
            f'{block.group(0)}\n', encoding="utf-8")
        self.n = 0

    def form(self, form):
        text, n = re.subn(r'^#define VJO_VOCR_SPOT_FORM "[a-z]+"$', f'#define VJO_VOCR_SPOT_FORM "{form}"',
                          SIZES.read_text(encoding="utf-8"), flags=re.M)
        self.assertEqual(n, 1)
        (self.tmp / "root/include/vjo_vocr_sizes.h").write_text(text, encoding="utf-8")

    def directory(self):
        self.n += 1
        return self.tmp / f"build{self.n}"

    def configure(self, build, *args, ok=True):
        """The cache's VJO_VOCR_SPOT_NO_MS after a configure of BUILD (ARGS: -D...), and its output."""
        run = subprocess.run([self.cmake, "-S", str(self.tmp / "src"), "-B", str(build), *args], capture_output=True,
                             text=True, timeout=120)
        self.assertEqual(run.returncode == 0, ok, run.stdout + run.stderr)
        cache = (build / "CMakeCache.txt").read_text()
        return re.search(r"^VJO_VOCR_SPOT_NO_MS:BOOL=(\w+)$", cache, re.M).group(1), run.stdout + run.stderr

    def test_the_option_follows_the_pinned_form(self):
        self.form("trunk")
        self.assertEqual(self.configure(self.directory())[0], "ON")  # a new directory, as CI's
        self.form("ms")
        self.assertEqual(self.configure(self.directory())[0], "OFF")
        reused = self.directory()
        self.assertEqual(self.configure(reused)[0], "OFF")  # ms first
        self.form("trunk")
        value, said = self.configure(reused)  # the form back to trunk: as a new directory, ON (it stayed OFF)
        self.assertEqual(value, "ON")
        self.assertIn('VJO_VOCR_SPOT_NO_MS: ON, as the pinned spotter\'s form is now "trunk"', said)
        self.assertEqual(self.configure(reused)[0], "ON")  # and stays
        self.form("ms")
        self.assertEqual(self.configure(reused)[0], "OFF")  # to ms again: OFF (it failed the configure)

    def test_a_value_someone_gave_stays(self):
        # given on the first configure, other than the form's default
        self.form("trunk")
        given = self.directory()
        self.assertEqual(self.configure(given, "-DVJO_VOCR_SPOT_NO_MS=OFF")[0], "OFF")
        self.assertEqual(self.configure(given)[0], "OFF")
        self.form("ms")
        self.assertEqual(self.configure(given)[0], "OFF")
        self.form("trunk")
        self.assertEqual(self.configure(given)[0], "OFF")
        # given later, on a directory that had the default
        changed = self.directory()
        self.assertEqual(self.configure(changed)[0], "ON")
        self.assertEqual(self.configure(changed, "-DVJO_VOCR_SPOT_NO_MS=OFF")[0], "OFF")
        self.form("ms")
        self.configure(changed)
        self.form("trunk")
        self.assertEqual(self.configure(changed)[0], "OFF")
        # ON given for a multi-scale pin still fails the configure, loudly
        self.form("ms")
        value, said = self.configure(self.directory(), "-DVJO_VOCR_SPOT_NO_MS=ON", ok=False)
        self.assertIn("VJO_VOCR_SPOT_NO_MS=ON, but the pinned spotter is a multi-scale network", said)

    def test_a_directory_configured_before_this_rule(self):
        """A cache from before the rule (no _VJO_VOCR_SPOT_NO_MS_AUTO): the default it holds follows the form from
        now on, a value other than the default is taken as someone's and stays."""
        self.form("trunk")
        # (given, after the form went to ms, after it came back to trunk)
        for value, at_ms, back in (("ON", "OFF", "ON"), ("OFF", "OFF", "OFF")):
            build = self.directory()
            self.configure(build, f"-DVJO_VOCR_SPOT_NO_MS={value}")
            cache = build / "CMakeCache.txt"
            # the entry and its help lines (//...) before it
            text, n = re.subn(r"^(?://[^\n]*\n)*_VJO_VOCR_SPOT_NO_MS_AUTO:INTERNAL=\w+\n", "", cache.read_text(),
                              flags=re.M)
            self.assertEqual(n, 1)
            cache.write_text(text)
            self.form("trunk")
            self.assertEqual(self.configure(build)[0], value)
            self.form("ms")
            self.assertEqual(self.configure(build)[0], at_ms)
            self.form("trunk")
            self.assertEqual(self.configure(build)[0], back)


if __name__ == "__main__":
    unittest.main()
