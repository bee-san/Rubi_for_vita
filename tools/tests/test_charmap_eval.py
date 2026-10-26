"""tools/charmap_eval.py and tools/overlay_mock.py without the corpus or a model: GameSentenceMiner's glyph widths
and its cut of a box, the edit-distance alignment, the hit test (core/charmap.c vjo_cmap_hit's slack), a page's
scoring on a made-up record (OCR's places, the box cut, a hook's text on the finder's box and on its true extent),
the summary's rates and title interval, and the mock's word rectangles and popup place (core/charmap.c
vjo_cmap_span, vjo_cmap_popup_top: host/tests/test_charmap.c checks the C on the same cases)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import charmap_eval as ce  # noqa: E402
import overlay_mock as om  # noqa: E402


def utf8_offsets(s):
    out, at = [], 0
    for ch in s:
        out.append(at)
        at += len(ch.encode())
    return out


class Widths(unittest.TestCase):
    def test_full_and_half_width_advances(self):
        self.assertEqual(ce.advance("漢"), 1.0)
        self.assertEqual(ce.advance("「"), 1.0)
        self.assertEqual(ce.advance("A"), 0.5)
        self.assertEqual(ce.advance("ｱ"), 0.5)  # half-width katakana
        self.assertEqual(ce.advance("\n"), 0.0)

    def test_split_follows_the_advances(self):
        cells = ce.split_cells("あAい", 0, 50)
        self.assertEqual([round(a) for a, _ in cells], [0, 20, 30])
        self.assertEqual(round(cells[-1][1]), 50)


class Alignment(unittest.TestCase):
    def test_equal_substituted_inserted_and_deleted(self):
        self.assertEqual(ce.align("abc", "abc"), {0: 0, 1: 1, 2: 2})
        self.assertEqual(ce.align("abc", "axc"), {0: 0, 1: 1, 2: 2})  # a substitution pairs
        self.assertEqual(ce.align("abc", "ac"), {0: 0, 2: 1})         # b has no counterpart
        self.assertEqual(ce.align("ac", "abc"), {0: 0, 1: 2})


class Hit(unittest.TestCase):
    def test_inside_slack_and_zero_width(self):
        places = [(0, 10, 0, 20), (10, 10, 0, 20), (10, 20, 0, 20)]  # the middle one has no width
        self.assertEqual(ce.hit(places, 5, 5), 0)
        self.assertEqual(ce.hit(places, 10, 5), 2)                  # a zero-width cell holds no tap
        self.assertEqual(ce.hit(places, 25, 25), 2)                 # 6 + 6 px off: within the slack
        self.assertEqual(ce.hit(places, 30, 5), 2)                  # 11 px: within it
        self.assertEqual(ce.hit(places, 33, 5), -1)                 # 14 px: outside it


class Scoring(unittest.TestCase):
    def record(self):
        """A Vita page: one region, two hooked lines with glyph cells; a name line that is not scored."""
        lines = [
            {"id": "T:1:l1", "kind": "line", "vertical": False, "text": "あいう", "source": "glyph_cells",
             "cell_boxes": [[100, 400, 130, 430], [130, 400, 160, 430], [160, 400, 190, 430]]},
            {"id": "T:1:l2", "kind": "line", "vertical": False, "text": "かきA", "source": "glyph_cells",
             "cell_boxes": [[100, 440, 130, 470], [130, 440, 160, 470], [160, 440, 175, 470]]},
            {"id": "T:1:n", "kind": "name", "vertical": False, "text": "名", "source": "glyph_cells",
             "cell_boxes": [[100, 360, 130, 390]]},
        ]
        return {"title_id": "T", "page": "000001", "platform": "vita", "split": "hval", "lines": lines,
                "regions": [{"variant": "base", "region": [90, 390, 300, 480], "lines": ["T:1:l1", "T:1:l2"]}]}

    def doc(self, sentence, places, ocr):
        """vjo-ocr --chars's output, region px: places [(x0, x1, y0, y1) or None] per character of the sentence, one
        line per row; a None (a line break) is not in the map, as in the block."""
        offs, rows = utf8_offsets(sentence), {}
        chars = []
        for k, p in enumerate(places):
            if p is None:
                continue
            x0, x1, y0, y1 = p
            rows.setdefault((y0, y1), []).append(len(chars))
            chars.append([offs[k], x0, x1])
        lines = [[y0, y1, ks[0], len(ks)] for (y0, y1), ks in rows.items()]
        return {"rc": 0, "region": [90, 390, 300, 480], "scale": 1,
                "chars": {"sentence": sentence, "lines": lines, "chars": chars, "ocr": ocr}}

    def test_exact_places_hit_every_character(self):
        exact = [(10, 40, 10, 40), (40, 70, 10, 40), (70, 100, 10, 40),
                 (10, 40, 50, 80), (40, 70, 50, 80), (70, 85, 50, 80)]
        ocr = [[8, 8, 100, 34, [[0x3042, 10, 40], [0x3044, 40, 70], [0x3046, 70, 100]]],
               [8, 48, 100, 34, [[0x304B, 10, 40], [0x304D, 40, 70], [0x41, 70, 85]]]]
        row = ce.score_region(self.record(), self.doc("あいう\nかきA", exact[:3] + [None] + exact[3:], ocr))
        self.assertIsNotNone(row)
        self.assertEqual(len(row["res"]["ocr_cells"]), 6)  # the name line is not scored
        self.assertTrue(all(h for h, _, _, _ in row["res"]["ocr_cells"]))
        self.assertTrue(all(on for _, on, _, _ in row["res"]["ocr_cells"]))
        # the true extent cut by widths: あいう's cells are even, かきA's half-width A too
        self.assertTrue(all(h for h, _, _, _ in row["res"]["hook_box"]))

    def test_a_box_cut_misses_a_shifted_pitch(self):
        """A box with 30 px of padding on its right: its cut drifts off the glyphs, OCR's cells do not."""
        exact = [(10, 40, 10, 40), (40, 70, 10, 40), (70, 100, 10, 40)]
        ocr = [[8, 8, 140, 34, [[0x3042, 10, 40], [0x3044, 40, 70], [0x3046, 70, 100]]]]
        rec = self.record()
        rec["lines"][1]["text"], rec["regions"][0]["lines"] = "", ["T:1:l1"]
        row = ce.score_region(rec, self.doc("あいう", exact, ocr))
        self.assertEqual(sum(h for h, _, _, _ in row["res"]["ocr_cells"]), 3)
        self.assertLess(sum(h for h, _, _, _ in row["res"]["ocr_split"]), 3)
        self.assertLess(sum(h for h, _, _, _ in row["res"]["hook_finder"]), 3)

    def test_no_places_score_nothing(self):
        row = ce.score_region(self.record(), {"rc": 1, "region": [90, 390, 300, 480], "scale": 1})
        self.assertTrue(not any(h for h, _, _, _ in row["res"]["ocr_cells"]))
        self.assertTrue(not any(p for _, _, _, p in row["res"]["hook_finder"]))

    def test_summary_rates_and_interval(self):
        rows = [{"title": "A", "platform": "vita", "sources": ["glyph_cells"] * 2,
                 "res": {"k": [(True, True, 1.0, True), (False, False, 0.0, False)]}},
                {"title": "B", "platform": "psp", "sources": ["glyph_cells", "image_rows"],
                 "res": {"k": [(True, True, 0.5, True), (True, True, 0.5, True)]}}]
        s = ce.summarize(rows, "k")
        self.assertEqual(s["chars"], 3)  # image_rows' character is not scored
        self.assertAlmostEqual(s["hit"], 2 / 3, places=4)
        self.assertEqual(s["titles"], 2)
        self.assertEqual(s["title_min_hit"], 0.5)
        self.assertTrue(s["hit_ci95"][0] <= s["hit"] <= s["hit_ci95"][1])


class Mock(unittest.TestCase):
    def chars(self):
        """Two rows, あいうえ and かきくけ, 25 px each from x 100: rows 380-410 and 420-450."""
        sent = "あいうえ\nかきくけ"
        offs = utf8_offsets(sent)
        chars = [[offs[k], 100 + 25 * k, 125 + 25 * k] for k in range(4)] + \
                [[offs[5 + k], 100 + 25 * k, 125 + 25 * k] for k in range(4)]
        return {"sentence": sent, "lines": [[380, 410, 0, 4], [420, 450, 4, 4]], "chars": chars}

    def test_a_word_over_the_line_break_has_two_rectangles(self):
        self.assertEqual(om.span(self.chars(), 9, 16), [(175, 380, 25, 30), (100, 420, 25, 30)])

    def test_the_popup_clears_the_text(self):
        c = self.chars()
        self.assertEqual(om.popup_top(c, (100, 380, 50, 30)), 380 - 6 - 196)  # above all the text
        c["lines"] = [[40, 70, 0, 4], [80, 110, 4, 4]]
        self.assertEqual(om.popup_top(c, (100, 40, 25, 30)), 116)              # below it all
        c["lines"] = [[40, 70, 0, 4], [480, 510, 4, 4]]
        self.assertEqual(om.popup_top(c, (100, 40, 25, 30)), 76)               # beside the word


if __name__ == "__main__":
    unittest.main()
