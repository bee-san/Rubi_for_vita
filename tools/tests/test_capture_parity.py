"""tools/capture_parity.py and the frozen API 18 core it compares against (#74)."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import capture_parity  # noqa: E402

# git show 8228c3c:core/vocr_ocr.c / .h (the last API 18 core), kept verbatim.
API18 = {
    "host/tests/api18/vocr_ocr.c": "bb349273178a528d248b3fd005125d835393a2e36dd196be290d049b232135de",
    "host/tests/api18/vocr_ocr.h": "ac131c911a5f55c5369f8f4c9992db2e048c33884993741d58152a83878ada3c",
}


class FrozenApi18(unittest.TestCase):
    def test_the_api18_core_is_8228c3c_verbatim(self):
        for rel, want in API18.items():
            got = hashlib.sha256((ROOT / rel).read_bytes()).hexdigest()
            self.assertEqual(got, want, f"{rel} changed: the parity test must compare against 8228c3c")


class ProtectedSets(unittest.TestCase):
    def test_protected_rows_are_never_used(self):
        for row in ({"set": "vita-test", "vn": "v1"}, {"set": "psp_test", "vn": "v1"}, {"set": "psp", "vn": "v4"},
                    {"set": "native", "vn": "v9"}, {"set": "psv", "vn": "v9", "id": "clannad_1"}):
            self.assertTrue(capture_parity.protected(row), row)
        self.assertFalse(capture_parity.protected({"set": "psp", "vn": "v10104", "id": "psp_tight_v10104_38874"}))

    def test_only_the_dev_manifests_can_be_named(self):
        self.assertEqual(capture_parity.DEV_MANIFESTS, ("vita_dev.jsonl", "psp_dev.jsonl"))
        with self.assertRaises(SystemExit):
            capture_parity.main(["--binary", "x", "--manifests", "m", "--shots", "s", "--only", "vita_test.jsonl"])


def screenshot(path):
    """A 960x544 VN-like frame: a dark textbox with two lines of light strokes."""
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (960, 544), (90, 120, 160))
    d = ImageDraw.Draw(img)
    d.rectangle([20, 380, 940, 530], fill=(16, 18, 30))
    for row, y in enumerate((396, 436)):
        for x in range(40, 860 - 120 * row, 30):
            d.rectangle([x, y, x + 3, y + 24], fill=(240, 240, 240))
            d.rectangle([x, y + 10, x + 22, y + 13], fill=(240, 240, 240))
    img.save(path, "PNG")


@unittest.skipUnless(os.environ.get("VJO_CAPTURE_PARITY"), "vjo-capture-parity-tests not built (VJO_WITH_VOCR off)")
class EvalCrops(unittest.TestCase):
    def test_a_dev_manifest_runs_through_both_pipelines_without_difference(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "m").mkdir()
            (tmp / "s").mkdir()
            shot = tmp / "s" / "v1_1.png"
            screenshot(shot)
            sha = hashlib.sha256(shot.read_bytes()).hexdigest()
            rows = [{"vn": "v1", "shot": "v1_1.png", "sha256": sha, "set": "psv", "id": "a", "mask": [],
                     "region": [20, 380, 940, 530]},
                    {"vn": "v1", "shot": "v1_1.png", "sha256": sha, "set": "psv", "id": "b", "mask": [],
                     "region": [30, 390, 800, 470]},
                    {"vn": "v4", "shot": "v1_1.png", "sha256": sha, "set": "psv", "id": "c", "mask": [],
                     "region": [0, 0, 960, 544]}]
            (tmp / "m" / "vita_dev.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
            run = subprocess.run([sys.executable, str(ROOT / "tools/capture_parity.py"), "--binary",
                                  os.environ["VJO_CAPTURE_PARITY"], "--manifests", str(tmp / "m"), "--shots",
                                  str(tmp / "s"), "--only", "vita_dev.jsonl"], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("2 regions of 1 screenshots", run.stdout)
            self.assertIn("1 protected rows excluded", run.stdout)
            self.assertEqual(run.stdout.count(", 0 differences"), 3, run.stdout)
            # a screenshot that is not the manifest's is refused
            rows[0]["sha256"] = "0" * 64
            (tmp / "m" / "vita_dev.jsonl").write_text(json.dumps(rows[0]) + "\n")
            run = subprocess.run([sys.executable, str(ROOT / "tools/capture_parity.py"), "--binary",
                                  os.environ["VJO_CAPTURE_PARITY"], "--manifests", str(tmp / "m"), "--shots",
                                  str(tmp / "s"), "--only", "vita_dev.jsonl"], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            self.assertEqual(run.returncode, 2, run.stdout)


if __name__ == "__main__":
    unittest.main()
