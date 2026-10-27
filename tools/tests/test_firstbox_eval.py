"""tools/firstbox_eval.py, the measurement of the text box suggested on first use (#358, docs/first-box.md): its pages
of the pool (complete, trusted, horizontal dialogue; a PSP page in the console view), its scoring of a box on a
title's other pages (the dialogue it covers, the junk it reads, its area) and the report's halves. Synthetic records
only: no page or text of a game; the end-to-end run uses a stand-in for vjo-page-ocr and blank pages."""
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import firstbox_eval as fe  # noqa: E402

try:
    from PIL import Image  # noqa: F401  (the end-to-end run writes and reads pages)
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def page(title="T0001", n=1, lines=((118, 391, 722, 32, "line"),), platform="vita", dims=(960, 544),
         complete=True, untrusted=False, vertical=False):
    """A page record of the pool's schema (vocr6-hooked/1)."""
    recs = [{"id": f"{title}:{n:06d}:{kind}{i}", "kind": kind, "vertical": vertical, "box": [x, y, w, h],
             "text": "あ"} for i, (x, y, w, h, kind) in enumerate(lines, 1)]
    xs = [x for x, _, _, _, k in lines if k == "line"] or [0]
    return {"title_id": title, "page": f"{n:06d}", "image": f"{title}/pages/{n:06d}.png", "dims": list(dims),
            "platform": platform, "split": "train", "complete": complete, "untrusted": untrusted, "lines": recs,
            "regions": [{"variant": "base", "region": [min(xs) - 12, 379, 852, 435], "lines": []}]}


def job(lines=(), suggest=None):
    """A read_title() row: the job's recognized lines (box, Japanese or not) and its suggested box (x, y, w, h)."""
    return {"rc": 0, "suggest": suggest, "lines": [{"box": list(b), "ja": ja} for b, ja in lines]}


class Pages(unittest.TestCase):
    def test_pages_of_filters_and_spreads(self):
        with tempfile.TemporaryDirectory() as tmp:
            pool = Path(tmp)
            rows = [page(n=i) for i in range(1, 11)]
            rows.append(page(n=11, complete=False))
            rows.append(page(n=12, untrusted=True))
            rows.append(page(n=13, vertical=True))
            rows.append(page(n=14, lines=((450, 500, 60, 20, "name"),)))  # no dialogue line
            (pool / "T0001.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
            (pool / "PCSG00007.jsonl").write_text(json.dumps(page("PCSG00007")) + "\n", encoding="utf-8")
            got = fe.pages_of(pool, set(), set(fe.EXCLUDE), 4)
            self.assertEqual([p["page"] for p in got], ["000001", "000003", "000005", "000007"])
            self.assertEqual({p["title_id"] for p in got}, {"T0001"})  # the protected sets' titles are never read
            self.assertEqual(len(fe.pages_of(pool, {"T0002"}, set(), 4)), 0)

    def test_truth_in_the_console_view(self):
        t = fe.truth_of(page(platform="psp", dims=(480, 272),
                             lines=((59, 195, 361, 16, "line"), (228, 254, 24, 8, "name"))))
        self.assertEqual(t["lines"], [[118, 390, 722, 32]])
        self.assertEqual(t["others"], [[456, 508, 48, 16]])
        self.assertEqual(t["base"], [94, 758, 1610, 112])  # the pool's base region, 2x

    def test_box_pixels(self):
        self.assertEqual(fe.box_pixels([0, 374, 960, 544]), [0, 374, 960, 170])
        self.assertIsNone(fe.box_pixels([0, 0, 0, 0]))
        self.assertIsNone(fe.box_pixels(None))


class Score(unittest.TestCase):
    def test_a_box_is_scored_on_the_titles_other_pages(self):
        band = [0, 374, 960, 170]
        a = {"truth": fe.truth_of(page(n=1)), "job": job([((118, 391, 722, 32), True), ((700, 512, 230, 22), False)]),
             "box": {"suggested": band, "whole_screen": [0, 0, 960, 544]}}
        # page 2: one dialogue line in the band, one above it; a button inside it, a picture's junk above it
        b = {"truth": fe.truth_of(page(n=2, lines=((118, 391, 722, 32, "line"), (118, 300, 500, 32, "line")))),
             "job": job([((118, 391, 722, 32), True), ((118, 300, 500, 32), True), ((700, 512, 230, 22), True),
                         ((300, 40, 120, 60), False)]),
             "box": {"suggested": None, "whole_screen": [0, 0, 960, 544]}}
        s = fe.score_title([a, b])
        sug, whole = s["suggested"], s["whole_screen"]
        self.assertEqual((sug["pages"], sug["offered"], sug["own"], sug["pairs"]), (2, 1, 1, 1))
        self.assertAlmostEqual(sug["covers"], 0.5)                # one of page 2's two lines
        self.assertEqual((sug["junk"], sug["visible"]), (1, 1))   # the button, with Japanese
        self.assertAlmostEqual(sug["area"], 170 / 544)
        self.assertEqual((whole["offered"], whole["pairs"], whole["covers"]), (2, 2, 2.0))
        self.assertEqual(whole["junk"], 2 + 1)                    # page 2's button and picture, page 1's button
        r = fe.summary({"T0001": s}, ["T0001"])
        self.assertEqual(r["suggested"]["covers"], 0.5)
        self.assertEqual(r["suggested"]["offered"], 0.5)
        self.assertEqual(r["whole_screen"]["covers"], 1.0)

    def test_halves(self):
        ids = [f"PCSG{n:05d}" for n in range(200)]
        halves = [fe.half(t) for t in ids]
        self.assertEqual(set(halves), {0, 1})
        self.assertEqual(halves, [fe.half(t) for t in ids])  # the same every run
        self.assertTrue(60 < sum(halves) < 140)


@unittest.skipUnless(HAVE_PIL, "Pillow writes the pages")
class EndToEnd(unittest.TestCase):
    def test_a_run_reports_counts_and_rates_only(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            pool, pages = tmp / "pagepool" / "v1", tmp / "pages"
            pool.mkdir(parents=True)
            rows = []
            for title in ("T0001", "T0002"):
                (pages / title / "pages").mkdir(parents=True)
                for n in (1, 2, 3):
                    rows.append(page(title, n))
                    Image.new("RGB", (960, 544)).save(pages / title / "pages" / f"{n:06d}.png")
                (pool / f"{title}.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows[-3:]),
                                                     encoding="utf-8")
            # a stand-in for vjo-page-ocr: each page's job reads the dialogue line and suggests the band
            tool = tmp / "page-ocr"
            tool.write_text("#!" + sys.executable + "\n"
                            "import sys, json\n"
                            "src = sys.stdin.buffer\n"
                            "while True:\n"
                            "    head = src.readline()\n"
                            "    if not head: break\n"
                            "    pid, w, h, psp = head.split()\n"
                            "    src.read(int(w) * int(h) * 4)\n"
                            "    print(json.dumps({'id': pid.decode(), 'rc': 0, 'w': int(w), 'h': int(h),\n"
                            "        'suggest': [0, 375, 960, 544], 'lines': [{'box': [118, 391, 722, 32],\n"
                            "        'blank': 0, 'text': 'あいうえお'}]}), flush=True)\n", encoding="utf-8")
            tool.chmod(tool.stat().st_mode | stat.S_IEXEC)
            out = tmp / "out.json"
            self.assertEqual(fe.main(["--binary", str(tool), "--models", str(tmp), "--pagepool", str(pool),
                                      "--per-title", "3", "--jobs", "2", "-o", str(out)]), 0)
            report = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(report["all"]["suggested"]["titles"], 2)
            self.assertEqual(report["all"]["suggested"]["covers"], 1.0)
            self.assertEqual(report["all"]["suggested"]["junk"], 0.0)
            self.assertEqual(report["pages_failed"], 0)
            self.assertNotIn("あ", out.read_text(encoding="utf-8"))  # no text of a page in the report


if __name__ == "__main__":
    unittest.main()
