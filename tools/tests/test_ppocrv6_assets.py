"""PP-OCRv6 model pins and pipeline plumbing with tiny fixtures; no network, models or device."""
import contextlib
import hashlib
import io
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ppocrv6 as v6  # noqa: E402


class Pins(unittest.TestCase):
    def test_weights_pin_matches_the_worker_and_broker(self):
        sizes = (ROOT / "include/vjo_vocr_sizes.h").read_text()
        self.assertIn(f'#define VJO_PPOCRV6_MODEL_NAME "{v6.MODEL_NAME}"', sizes)
        self.assertIn(f"#define VJO_PPOCRV6_MODEL_BYTES {v6.WEIGHTS[1]}u", sizes)
        self.assertRegex(v6.WEIGHTS[2], r"^[0-9a-f]{64}$")
        self.assertTrue(v6.WEIGHTS[0].endswith(f"/{v6.RELEASE_TAG}/{v6.MODEL_NAME}"))
        # Every upstream input is pinned by SHA-256.
        for pin in (v6.ONNX, v6.PNNX, v6.NCNN):
            self.assertRegex(pin[2], r"^[0-9a-f]{64}$")
        self.assertEqual(v6.NCNN_REV, re.search(r"VJO_NCNN_REVISION (\w+)",
                                                (ROOT / "cmake/Ncnn.cmake").read_text()).group(1))

    def test_committed_param_and_classes_agree(self):
        classes = v6.read_dict(v6.DATA_DIR / v6.DICT_NAME)
        param = (v6.DATA_DIR / v6.PARAM_NAME).read_text().splitlines()
        head = [line for line in param if line.startswith("Gemm")][-1]
        self.assertIn(" out0 ", head)
        self.assertEqual(int(re.search(r" 8=(\d+)", head).group(1)), len(classes) + 1)
        self.assertEqual(classes[-1], " ")  # Paddle's final space class
        keep = v6.jis_classes() | {" "}
        self.assertFalse([c for c in classes if c not in keep])
        self.assertEqual(len(set(classes)), len(classes))
        # No layer outside the worker's ncnn layer list.
        layers = set(re.findall(r"^(\w+)\s", "\n".join(param[2:]), re.M))
        allowed = re.search(r"if\(VJO_WITH_PPOCRV6\).*?list\(APPEND _ncnn_layers (.*?)\)",
                            (ROOT / "cmake/Ncnn.cmake").read_text(), re.S).group(1).split()
        self.assertFalse(layers - set(allowed))
        # Every convolution (int8_scale_term 1/2, or 101/102 with fused requantize)
        # and every Gemm/attention (18=2) is int8.
        convs = [l for l in param if l.startswith(("Convolution ", "ConvolutionDepthWise "))]
        self.assertEqual(len(convs), 57)
        self.assertTrue(all(re.search(r" 8=(1|2|101|102)( |$)", l) for l in convs))
        dense = [l for l in param if l.startswith(("Gemm ", "MultiHeadAttention "))]
        self.assertTrue(len(dense) == 7 and all(" 18=2" in l for l in dense))


class Rewrites(unittest.TestCase):
    @staticmethod
    def graph():
        reduce = "Reduction                mean_{0}                   1 1 {1} {2} 0=3 1=0 -23303=2,1,2 4=1 5=1"
        lines = ["Input                    in0                      0 1 in0"]
        blob = "in0"
        for i in range(5):  # the five squeeze-excitation squeezes
            lines.append(reduce.format(i, blob, f"r{i}"))
            blob = f"r{i}"
        lines += ["Reshape                  reshape_a                1 1 r4 a 0=-1 1=120 12=0 13=0 2=1",
                  "Reshape                  reshape_b                1 1 a out0 0=120 1=-1 11=1 12=0 13=0 2=1"]
        return "\n".join(["7767517", f"{len(lines)} {len(lines)}"] + lines) + "\n"

    def test_fix_param_rewrites_reductions_and_batch_reshapes_only(self):
        out = v6.fix_param(self.graph()).splitlines()
        self.assertEqual(out[:2], ["7767517", "8 8"])
        rows = [line.split() for line in out[2:]]
        self.assertEqual([r[0] for r in rows], ["Input"] + ["Pooling"] * 5 + ["Reshape", "Reshape"])
        for r in rows[1:6]:
            self.assertEqual(r[6:], ["0=1", "7=1", "8=1", "18=1"])  # adaptive average to 1x1
        self.assertEqual(rows[6][6:], ["0=-1", "1=120"])        # batch axis dropped
        self.assertEqual(rows[7][6:], ["0=120", "1=-1", "2=1"])
        self.assertEqual([r[4:6] for r in rows[1:]], [r.split()[4:6] for r in self.graph().splitlines()[3:]])
        with self.assertRaises(AssertionError):  # any other graph is refused
            v6.fix_param(self.graph().replace("mean_4", "mean_4x").replace("0=3 1=0 -23303=2,1,2 4=1 5=1\nReshape", "0=3 1=0 -23303=2,1,2 4=0 5=1\nReshape"))

    def test_floor_table_raises_thresholds_to_a_fraction_of_the_maximum(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "t").write_text("conv_1_param_0 10.0 20.0\nconv_1 100.0\nconv_2 50.0\n")
            (tmp / "log").write_text(
                "conv_1                                   : max = 4.000000         threshold = 1.270000         scale = 100.000000\n"
                "conv_2                                   : max = 2.000000         threshold = 1.900000         scale = 66.842105\n")
            self.assertEqual(v6.floor_table(tmp / "t", tmp / "log", tmp / "o", 0.5), 2)
            rows = dict(line.split(" ", 1) for line in (tmp / "o").read_text().splitlines())
            self.assertEqual(rows["conv_1_param_0"], "10.0 20.0")    # weights untouched
            self.assertAlmostEqual(float(rows["conv_1"]), 127 / 2.0)  # 1.27 raised to 0.5 x 4
            self.assertAlmostEqual(float(rows["conv_2"]), 127 / 1.9)  # already above the floor
            (tmp / "log").write_text("")
            with self.assertRaises(RuntimeError):
                v6.floor_table(tmp / "t", tmp / "log", tmp / "o", 0.5)


class Fetch(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data = b"tiny PP-OCRv6 fixture, not OCR weights"
        self.enterContext(mock.patch.object(v6, "WEIGHTS", (
            "https://fixture.invalid/" + v6.MODEL_NAME, len(self.data), hashlib.sha256(self.data).hexdigest())))
        self.enterContext(mock.patch.object(v6.urllib.request, "urlopen",
                                            side_effect=OSError("No test network access")))

    def test_from_file_or_directory_is_checked(self):
        source = self.root / "src"
        source.mkdir()
        (source / v6.MODEL_NAME).write_bytes(self.data)
        for arg in (source, source / v6.MODEL_NAME):
            out = self.root / "out"
            path = v6.fetch(out, arg)
            self.assertEqual(path.read_bytes(), self.data)
            path.unlink()
        (source / v6.MODEL_NAME).write_bytes(self.data[:-1] + b"!")
        with self.assertRaises(SystemExit):
            v6.fetch(self.root / "out", source)
        self.assertFalse((self.root / "out" / v6.MODEL_NAME).exists())

    def test_draft_release_fallback_uses_the_exact_tag_and_name(self):
        def gh(argv, **kwargs):
            self.assertEqual(argv[:8], ["gh", "release", "download", v6.RELEASE_TAG,
                                        "-R", v6.RELEASE_REPO, "-p", v6.MODEL_NAME])
            (Path(argv[9]) / v6.MODEL_NAME).write_bytes(self.data)
        with mock.patch.object(v6.shutil, "which", return_value="/fixture/gh"), \
             mock.patch.object(v6.subprocess, "run", side_effect=gh), \
             contextlib.redirect_stdout(io.StringIO()):
            path = v6.fetch(self.root / "out")
        self.assertEqual(path.read_bytes(), self.data)

    def test_no_gh_and_no_network_fails_without_a_file(self):
        with mock.patch.object(v6.shutil, "which", return_value=None):
            with self.assertRaises(SystemExit):
                v6.fetch(self.root / "out")
        self.assertFalse((self.root / "out" / v6.MODEL_NAME).exists())


class Calibration(unittest.TestCase):
    def test_calibration_manifest_is_complete_and_pinned(self):
        import json
        rows = [json.loads(line) for line in v6.CALIBRATION.read_text().splitlines()]
        self.assertEqual(len(rows), 400)
        self.assertEqual({r["kind"] for r in rows}, {"vita", "psp"})
        for r in rows:
            self.assertRegex(r["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(r["url"].startswith("https://t.vndb.org/"))
        self.assertEqual(len({r["id"] for r in rows}), len(rows))


if __name__ == "__main__":
    unittest.main()
