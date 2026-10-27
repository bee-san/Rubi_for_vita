"""tools/spinner_eval.py, the animated-icon measurement through Rubi's kernel change detection, game-worker job and
Shell decisions (host/spinner_bench.c, docs/text-stability.md "Animated icons and moving pictures"): the pages it
samples, the region it widens for an icon, the job it writes for the bench, and its summary. Synthetic records and
pixels only: no page or text of a game."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import spinner_eval as se  # noqa: E402


def page(title="T0001", n=1, lines=((40, 400, 600, 30, "あいう"), (40, 440, 300, 30, "えお")), platform="vita",
         complete=True, vertical=False, region=(30, 390, 650, 480)):
    """A page record of the pool's schema (vocr6-hooked/1) with horizontal dialogue lines and a base region."""
    recs = [{"id": f"{title}:{n:06d}:line{i}", "kind": "line", "vertical": vertical, "box": [x, y, w, h], "text": t}
            for i, (x, y, w, h, t) in enumerate(lines, 1)]
    return {"title_id": title, "page": f"{n:06d}", "image": f"{title}/pages/{n:06d}.png", "platform": platform,
            "complete": complete, "untrusted": False, "text_free": False, "lines": recs,
            "regions": [{"variant": "base", "region": list(region), "lines": [r["id"] for r in recs]}]}


class Pages(unittest.TestCase):
    def test_sampling_skips_unusable_pages(self):
        with tempfile.TemporaryDirectory() as d:
            pool = Path(d)
            rows = [page(n=i) for i in range(1, 9)] + [page(n=20, complete=False), page(n=21, vertical=True)]
            (pool / "T0001.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
            (pool / "T0002.jsonl").write_text(json.dumps(page("T0002")) + "\n", encoding="utf-8")
            got = se.pages_of(pool, set(), {"T0002"}, 4)
            self.assertEqual([p["page"] for p, _ in got], ["000001", "000003", "000005", "000007"])
            self.assertEqual(len(se.pages_of(pool, {"T0002"}, set(), 4)), 1)

    def test_the_job_widens_the_region_for_an_icon(self):
        with tempfile.TemporaryDirectory() as d:
            pages = Path(d)
            (pages / "T0001" / "pages").mkdir(parents=True)
            Image.new("RGB", (960, 544), (20, 20, 20)).save(pages / "T0001" / "pages" / "000001.png")
            p = page()
            rid, job, hook = se.job_of(p, p["regions"][0], pages)
            head, rest = job.split(b"\n", 1)
            _, w, h, n = head.split()
            # 4 px each way, then 3 last-line heights to the right and 1.5 below (room for an icon beside the text)
            self.assertEqual((rid, int(w), int(h), int(n)), ("T0001:000001", 740 - 26, 525 - 386, 2))
            boxes, pixels = rest.split(b"\n", 2)[:2], rest.split(b"\n", 2)[2]
            self.assertEqual([b.decode() for b in boxes], ["14 14 600 30", "14 54 300 30"])
            self.assertEqual(len(pixels), int(w) * int(h) * 3)
            self.assertEqual(hook, "あいうえお")

    def test_a_psp_page_is_read_at_2x(self):
        with tempfile.TemporaryDirectory() as d:
            pages = Path(d)
            (pages / "T0001" / "pages").mkdir(parents=True)
            Image.new("RGB", (480, 272), (20, 20, 20)).save(pages / "T0001" / "pages" / "000001.png")
            p = page(platform="psp", lines=((20, 200, 300, 15, "あ"),), region=(15, 195, 325, 220))
            _, job, _ = se.job_of(p, p["regions"][0], pages)
            head, box = job.split(b"\n")[:2]
            self.assertEqual(box.decode(), "14 14 600 30")
            self.assertEqual(head.split()[1:3], [b"%d" % (650 + 90 - 26), b"%d" % (440 + 45 - 386)])


def result(rid, scenario, shown=1, show_ms=0, text="あいう", how="the shown text", **kw):
    r = {"id": rid, "scenario": scenario, "shown": shown, "show_ms": show_ms, "how": how, "text": text, "hook": "あいう",
         "jobs": 1, "screen_changed": 0, "settle_timeouts": 0, "text_not_shown": 0, "recognized_dropped": 0,
         "symptom": 0, "kernel": {"settled": 1, "quiet": 0, "scene_changes": 1},
         "memory": {"allocations": 0, "capture_block": 61388, "tracker": 15072}}
    r.update(kw)
    return r


class Summary(unittest.TestCase):
    def test_times_and_the_text_against_the_still_read(self):
        rs = [result("a", "still"), result("b", "still", text="かき"),
              result("a", "spin", show_ms=900), result("b", "spin", shown=0, show_ms=-1, text="", how="",
                                                       screen_changed=7, symptom=1, recognized_dropped=7)]
        t = se.summary(rs, 20000)["scenarios"]
        self.assertEqual((t["still"]["shown"], t["still"]["show_ms_median"]), (2, 0))
        s = t["spin"]
        self.assertEqual((s["shown"], s["never_shown"], s["show_ms_median"], s["same_text_as_still"]), (1, 1, 900, 1))
        self.assertEqual((s["symptom_text_recognized_never_shown"], s["jobs_recognized_then_dropped"]), (1, 7))
        self.assertEqual((s["screen_changed"], s["similarity_to_hook_mean"]), (7, 1000))

    def test_a_shorter_text_counts_the_characters_missed(self):
        rs = [result("a", "still", text="あいうえお"), result("a", "spin", text="あいう")]
        self.assertEqual(se.summary(rs, 20000)["scenarios"]["spin"]["chars_missed_vs_still"], 2)

    def test_a_line_read_before_it_was_whole_counts_as_shorter(self):
        # the still text's beginning, shorter: a line read half typed (audit-rubi-spinner F1's rule); another text, or
        # one that differs early, is not
        rs = [result(r, "still", text="あいうえおかきくけこ") for r in "abcd"]
        rs += [result("a", "spin", text="あいうえお"), result("b", "spin", text="かきくけ"),
               result("c", "spin", text="あいうえおかきくけこ"), result("d", "spin", text="あXうえお")]
        s = se.summary(rs, 20000)["scenarios"]["spin"]
        self.assertEqual((s["shorter_than_still"], s["same_text_as_still"]), (1, 1))


if __name__ == "__main__":
    unittest.main()
