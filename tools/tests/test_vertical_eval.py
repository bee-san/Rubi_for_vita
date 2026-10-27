"""tools/vertical_eval.py, the vertical-text measurement through Rubi's job (docs/vita-vn-ocr.md "Vertical text"):
its units of a page (tight, card, fixed, full; a PSP page in the console view), its matching and scoring (column
recall and precision, dialogue CER in the job's order, a name tag's column neither), and its normalisation, which
must be vita-vn-ocr's eval/metrics.norm. Synthetic records only: no page or text of a game."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import vertical_eval as ve  # noqa: E402


def page(title="T0001", n=1, cols=((805, 35, 27, 239, "あいう"), (773, 35, 27, 263, "えお")), name=None,
         platform="vita", dims=(960, 544), split="train"):
    """A page record of the pool's schema (vocr6-hooked/1) with vertical dialogue columns and an optional name tag."""
    lines, boxes = [], []
    for i, (x, y, w, h, text) in enumerate(cols, 1):
        lines.append({"id": f"{title}:{n:06d}:line{i}", "kind": "line", "vertical": True, "box": [x, y, w, h],
                      "box_index": [i], "text": text, "drop": None})
        boxes.append({"index": i, "xyxy": [x, y, x + w, y + h], "kind": "line"})
    if name:
        boxes.append({"index": len(boxes) + 1, "xyxy": [name[0], name[1], name[0] + name[2], name[1] + name[3]],
                      "kind": "line"})
    return {"title_id": title, "page": f"{n:06d}", "image": f"{title}/pages/{n:06d}.png", "dims": list(dims),
            "platform": platform, "split": split, "complete": not name, "untrusted": False, "boxes": boxes,
            "lines": lines, "regions": []}


class Units(unittest.TestCase):
    def test_tight_card_full(self):
        us = {u["kind"]: u for u in ve.page_units(page(name=(852, 56, 26, 73)), ["tight", "card", "full"])}
        self.assertEqual(set(us), {"tight", "card", "full"})
        # tight: the columns' union plus 12 px; card: the name tag too; full: the screen
        self.assertEqual(us["tight"]["region"], [773 - 12, 35 - 12, 805 + 27 + 12 - (773 - 12), 35 + 263 + 12 - 23])
        self.assertEqual(us["card"]["region"][0], 773 - 12)
        self.assertEqual(us["card"]["region"][0] + us["card"]["region"][2], 852 + 26 + 12)
        self.assertEqual(us["full"]["region"], [0, 0, 960, 544])
        # the references right to left; the name tag is an ignore box
        self.assertEqual([r["text"] for r in us["tight"]["refs"]], ["あいう", "えお"])
        self.assertEqual(us["card"]["ignore"], [[852, 56, 26, 73]])

    def test_psp_console_view(self):
        u = ve.page_units(page(platform="psp", dims=(480, 272), cols=((400, 20, 14, 120, "あい"),)), ["tight"])[0]
        self.assertEqual((u["scale"], u["dims"]), (2, [960, 544]))
        self.assertEqual(u["refs"][0]["box"], [800, 40, 28, 240])

    def test_fixed_box_per_place(self):
        units = []
        for n in range(1, 61):
            units += ve.page_units(page(n=n, cols=((805, 35, 27, 200 + n, "あ"),)), ["tight"])
        units += ve.page_units(page(n=99, cols=((405, 35, 27, 100, "い"),)), ["tight"])  # alone at its place
        fixed = ve.fixed_units(units)
        self.assertEqual(len(fixed), 60)  # the lone page's place has fewer than 50 pages
        self.assertEqual({tuple(u["region"]) for u in fixed}, {(805 - 12, 35 - 12, 27 + 24, 260 + 24)})
        self.assertTrue(all(u["id"].endswith(":fixed") for u in fixed))

    def test_a_page_of_both_is_neither(self):
        p = page()
        p["lines"].append({"id": "T0001:000001:line9", "kind": "line", "vertical": False, "box": [100, 400, 300, 30],
                           "box_index": [], "text": "よこ", "drop": None})
        self.assertEqual(ve.page_units(p, ["tight", "full"]), [])

    def test_units_command(self):
        with tempfile.TemporaryDirectory() as d:
            pool = Path(d) / "pool"
            pool.mkdir()
            (pool / "T0001.jsonl").write_text(json.dumps(page(), ensure_ascii=False) + "\n", encoding="utf-8")
            out = Path(d) / "u.jsonl"
            ve.main(["units", "--pagepool", str(pool), "--regions", "tight,full", "--out", str(out)])
            self.assertEqual([json.loads(row)["kind"] for row in out.read_text().splitlines()], ["tight", "full"])


class Scoring(unittest.TestCase):
    def setUp(self):
        self.unit = ve.page_units(page(name=(852, 56, 26, 73)), ["card"])[0]
        self.ox, self.oy = self.unit["region"][:2]

    def result(self, boxes_texts, columns=1, rc=0):
        lines = [{"box": [x - self.ox, y - self.oy, w, h], "text": t} for x, y, w, h, t in boxes_texts]
        return {"id": self.unit["id"], "rc": rc, "columns": columns, "switched": 0, "lines": lines,
                "text": "\n".join(t for *_, t in boxes_texts), "found": len(lines), "strips": 9, "wall_us": 1000.0,
                "frames": 50}

    def test_perfect_columns(self):
        o = ve.score_unit(self.unit, self.result([(805, 35, 27, 239, "あいう"), (773, 35, 27, 263, "えお")]))
        self.assertEqual((o["tp"], o["fp"], o["fn"], o["errs"], o["chars"], o["order_ok"]), (2, 0, 0, 0, 5, True))

    def test_a_name_tags_column_is_neither_found_nor_read(self):
        o = ve.score_unit(self.unit, self.result([(850, 55, 30, 75, "なまえ"), (805, 35, 27, 239, "あいう"),
                                                  (773, 35, 27, 263, "えお")]))
        self.assertEqual((o["tp"], o["fp"], o["errs"]), (2, 0, 0))
        self.assertEqual(o["shown_errs"], 3)  # the text as shown keeps the name's three characters

    def test_a_missed_column_and_a_junk_one(self):
        o = ve.score_unit(self.unit, self.result([(805, 35, 27, 239, "あいう"), (700, 300, 20, 40, "ん")]))
        self.assertEqual((o["tp"], o["fp"], o["fn"]), (1, 1, 1))
        self.assertEqual(o["errs"], ve.lev("あいうえお", "あいうん"))

    def test_rows_find_no_column_but_their_text_counts(self):
        o = ve.score_unit(self.unit, self.result([(761, 40, 80, 27, "えあ"), (761, 70, 80, 27, "おい")], columns=0))
        self.assertEqual((o["tp"], o["fn"]), (0, 2))
        self.assertEqual(o["errs"], ve.lev("あいうえお", "えあおい"))

    def test_a_failed_job_reads_nothing(self):
        o = ve.score_unit(self.unit, self.result([(805, 35, 27, 239, "あいう")], rc=-115))
        self.assertEqual((o["tp"], o["errs"]), (0, 5))

    def test_summary_and_bootstrap(self):
        rows = [ve.score_unit(self.unit, self.result([(805, 35, 27, 239, "あいう"), (773, 35, 27, 263, "えお")]))
                for _ in range(3)]
        s = ve.summarize(rows)["vertical:card"]
        self.assertEqual((s["recall"], s["precision"], s["cer"], s["cer_ci95"]), (1.0, 1.0, 0.0, [0.0, 0.0]))
        self.assertEqual(ve.boot([(1, 10), (3, 10)]), ve.boot([(1, 10), (3, 10)]))  # seeded


class Norm(unittest.TestCase):
    def test_as_vita_vn_ocr(self):
        # eval/metrics.norm at bee-san/vita-vn-ocr 817d203: NFKC, no spaces, ... and ‥ as one character, the dashes
        # and tildes folded, ・・・ as an ellipsis; ー and 一 stay apart
        self.assertEqual(ve.norm("！ １ あ　い..."), "!1あい…")
        self.assertEqual(ve.norm("――〜～"), "――~~")
        self.assertEqual(ve.norm("・・・ー一"), "…ー一")
        self.assertEqual(ve.lev("abc", "abd"), 1)


if __name__ == "__main__":
    unittest.main()
