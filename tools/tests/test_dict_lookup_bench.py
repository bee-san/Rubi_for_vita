"""tools/dict_lookup_bench.py and host/dict_bench.c: counts, the golden check, the pool sample."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import convert_dictionary as convert
import dict_lookup_bench as bench

BENCH = os.environ.get("VJO_DICT_BENCH")


def term(word, reading="", gloss=None, rules=""):
    return [word, reading, "", rules, 1, gloss or ["definition of " + word], 1, ""]


ROWS = [term("食べる", "たべる", ["to eat"], "v1"), term("猫", "ねこ", ["cat"]), term("猫舌", "ねこじた", ["cat tongue"]),
        term("する", "", ["do"], "vs"), term("見る", "みる", ["to see"], "v1"), term("今日", "きょう", ["today"])]


@unittest.skipUnless(BENCH, "VJO_DICT_BENCH (vjo-dict-bench) is set by CTest")
class DictLookupBench(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.lines = self.dir / "lines.tsv"
        bench.write_tsv([("one", "今日は猫を見ました。"), ("two", "猫舌なので食べませんでした\n猫")], self.lines, "fixture")

    def build(self, rows, name="main.vjdict"):
        archive = self.dir / (name + ".zip")
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("index.json", json.dumps({"format": 3, "title": "Fixture"}))
            z.writestr("term_bank_1.json", json.dumps(rows, ensure_ascii=False))
        convert.convert([archive], self.dir / name)

    def run_bench(self, *extra, dicts="main.vjdict"):
        out = self.dir / "report.json"
        argv = ["--bench", BENCH, "--directory", str(self.dir), "--dicts", dicts, "--lines", str(self.lines),
                "--rounds", "3", "-o", str(out), *extra]
        self.assertEqual(bench.main(argv), 0)
        return json.loads(out.read_text())

    def test_counts_add_up_and_words_follow_the_matches(self):
        self.build(ROWS)
        report = self.run_bench()
        sets = report["sets"][str(self.lines)]
        first, words, letters = sets["first"]["after"], sets["words"]["after"], sets["letters"]["after"]
        self.assertEqual(first["lookups"], 2)
        # Whole word: 今日|は|猫|を|見ました / 猫舌|な|の|で|食べませんでした / 猫 (Japanese letters only).
        self.assertEqual(words["lookups"], 11)
        self.assertGreater(letters["lookups"], words["lookups"])
        for summary in (first, words, letters):
            self.assertAlmostEqual(sum(summary["reads_by"].values()), summary["reads"]["mean"], places=2)
            self.assertGreaterEqual(summary["candidates"]["mean"], summary["probes"]["mean"] - 1e-9)
            self.assertGreaterEqual(summary["probes"]["mean"], summary["found"]["mean"])
            self.assertEqual(summary["reads_by"]["header"], 1.0)  # one open of one file per lookup
            self.assertLessEqual(summary["arena_peak_max"], 64 * 1024)
            self.assertGreater(summary["console_ms"]["mean"], bench.CONSOLE["fixed_ms"])
        self.assertEqual(words["per_screen"]["screens"], 2)
        self.assertGreater(words["host_lookups_per_s"], 0)

    def test_golden_round_trip_and_a_changed_entry_fails(self):
        self.build(ROWS)
        golden = self.dir / "golden.tsv"
        self.run_bench("--write-golden", str(golden))
        self.assertIn("sha256 " + bench.dictionary_digest(self.dir, "main.vjdict"), golden.read_text())
        self.assertNotIn("猫", golden.read_text())  # digests only, no text
        self.run_bench("--golden", str(golden))
        self.build([*ROWS[:1], term("猫", "ねこ", ["a different cat"]), *ROWS[2:]])
        with self.assertRaisesRegex(SystemExit, "golden mismatch"):
            self.run_bench("--golden", str(golden))

    def test_before_and_after_must_show_the_same_entries(self):
        self.build(ROWS)
        self.build([*ROWS[:1], term("猫", "ねこ", ["a different cat"]), *ROWS[2:]], "other.vjdict")
        report = self.run_bench("--before-dicts", "main.vjdict", "--modes", "words")
        self.assertIn("identical", report["sets"][str(self.lines)]["words"]["golden"])
        with self.assertRaisesRegex(RuntimeError, "shown result changed"):
            self.run_bench("--before-dicts", "other.vjdict", "--modes", "words")


class PoolSample(unittest.TestCase):
    def test_kept_lines_by_screen_without_names_or_test_vns(self):
        with tempfile.TemporaryDirectory() as tmp:
            labels, reserved = Path(tmp) / "labels.jsonl", Path(tmp) / "reserved.json"
            rows = [
                {"id": "v1_10_1", "shot": "v1_10", "vn": "v1", "status": "kept", "label": "二行目", "name": False},
                {"id": "v1_10_0", "shot": "v1_10", "vn": "v1", "status": "kept", "label": "一行目", "name": False},
                {"id": "v1_10_2", "shot": "v1_10", "vn": "v1", "status": "kept", "label": "名前", "name": True},
                {"id": "v1_11_0", "shot": "v1_11", "vn": "v1", "status": "dropped", "label": None, "name": False},
                {"id": "v4_12_0", "shot": "v4_12", "vn": "v4", "status": "kept", "label": "テスト", "name": False},
                {"id": "v2_13_0", "shot": "v2_13", "vn": "v2", "status": "kept", "label": "別の画面", "name": False},
            ]
            labels.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in rows), encoding="utf-8")
            reserved.write_text(json.dumps({"test": {"v4": "clannad"}}))
            sample = bench.pool_lines(labels, reserved, 10)
            self.assertEqual(sorted(sample), [("v1_10", "一行目\n二行目"), ("v2_13", "別の画面")])
            self.assertEqual(sample, bench.pool_lines(labels, reserved, 10))
            self.assertEqual(len(bench.pool_lines(labels, reserved, 1)), 1)


if __name__ == "__main__":
    unittest.main()
