"""tools/sentence_readings/compare.py: its units, renderings and verdicts on synthetic lines (no analyser needed).
#311's measurement counts wrong and missing readings with them (reports/sentence-readings.md)."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "sentence_readings"))
import compare  # noqa: E402


class Units(unittest.TestCase):
    def test_a_token_with_kanji_is_a_unit_and_kana_tokens_are_not(self):
        text = "学校へ行った"
        tokens = [(0, 2, "ガッコウ"), (2, 3, "ヘ"), (3, 5, "イッ"), (5, 6, "タ")]
        self.assertEqual(compare.units(text, tokens, []), [(0, 2, 1, "がっこう"), (3, 5, 1, "いっ")])

    def test_a_reading_across_tokens_joins_them(self):
        # 一緒に read whole by a system: the unit takes 一緒 and に, counted once (one kanji token)
        text = "一緒に"
        tokens = [(0, 2, "イッショ"), (2, 3, "ニ")]
        self.assertEqual(compare.units(text, tokens, [(0, 3, "いっしょに")]), [(0, 3, 1, "いっしょに")])

    def test_an_unknown_word_has_no_reference(self):
        self.assertEqual(compare.units("朋也", [(0, 2, "")], []), [(0, 2, 1, None)])


class Verdicts(unittest.TestCase):
    def verdict(self, text, readings, reference):
        return compare.verdict(compare.render(text, 0, len(text), readings), reference)

    def test_correct_missing_and_wrong(self):
        self.assertEqual(self.verdict("学校", [(0, 2, "がっこう")], "がっこう"), "correct")
        self.assertEqual(self.verdict("学校", [], "がっこう"), "missing")
        self.assertEqual(self.verdict("学校", [(0, 2, "がくこう")], "がっこう"), "wrong")
        # katakana readings are compared as hiragana
        self.assertEqual(self.verdict("学校", [(0, 2, "ガッコウ")], "がっこう"), "correct")

    def test_part_read_rest_missing(self):
        # 行っ: 行 read い, the kana as itself: correct; only kana: missing; a wrong part: wrong
        self.assertEqual(self.verdict("行っ", [(0, 1, "い")], "いっ"), "correct")
        self.assertEqual(self.verdict("今日は", [(2, 3, "は")], "きょうは"), "missing")
        self.assertEqual(self.verdict("友達", [(0, 1, "とも")], "ともだち"), "missing")  # 達 has none, 友 fits
        self.assertEqual(self.verdict("友達", [(0, 1, "ゆう")], "ともだち"), "wrong")

    def test_display(self):
        self.assertEqual(compare.display(compare.render("友達", 0, 2, [(0, 1, "とも")])), "とも＊")


class EndToEnd(unittest.TestCase):
    def test_counts_and_adjudication(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            lines = [{"id": "a", "text": "私は学校へ行った"}, {"id": "b", "text": "友達と"}]
            (d / "lines.jsonl").write_text("".join(json.dumps(x, ensure_ascii=False) + "\n" for x in lines))
            sud = [{"id": "a", "tokens": [[0, 1, "ワタクシ"], [1, 2, "ハ"], [2, 4, "ガッコウ"], [4, 5, "ヘ"],
                                          [5, 7, "イッ"], [7, 8, "タ"]]},
                   {"id": "b", "tokens": [[0, 2, "トモダチ"], [2, 3, "ト"]]}]
            (d / "sudachi.jsonl").write_text("".join(json.dumps(x, ensure_ascii=False) + "\n" for x in sud))
            (d / "sys.tsv").write_text("a\t0:1:わたし 2:4:がっこう 5:6:い\nb\t\n")
            (d / "adj.tsv").write_text("# comment\n私\tわたし\tわたくし\tboth\n")
            subprocess.run([sys.executable, str(ROOT / "tools/sentence_readings/compare.py"), "--corpus",
                            str(d / "lines.jsonl"), "--sudachi", str(d / "sudachi.jsonl"), "--system",
                            f"rubi={d / 'sys.tsv'}", "--adjudication", str(d / "adj.tsv"), "-o", str(d / "r.json"),
                            "--patterns", str(d / "p.tsv")], check=True, capture_output=True)
            r = json.loads((d / "r.json").read_text())["systems"]["rubi"]
            c = r["counts"]
            self.assertEqual((c["kanji tokens"], c["correct"], c["missing"], c["right by adjudication"]), (4, 2, 1, 1))
            self.assertEqual(c.get("wrong", 0), 0)
            self.assertEqual(r["per_1000_kanji_tokens"]["missing"], 250.0)
            self.assertEqual((d / "p.tsv").read_text(), "")


if __name__ == "__main__":
    unittest.main()
