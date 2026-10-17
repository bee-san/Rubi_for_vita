"""tools/dict_lookup_bench.py and host/dict_bench.c: counts, the golden check, the pool sample."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
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
# Long definitions after them: their payloads come first in the data region and the key
# groups after all payloads, so ranking a posting reads its head from another page.
FILLER = [term(f"詰め物{i}", gloss=["x" * 3500]) for i in range(6)]


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

    def test_exact_counts_of_known_lookups(self):
        """Every counter of the first-word lookups, by hand where it is countable by hand:
        今日は猫を見ました。 tries spans of 9..2 characters (8 spans) until 今日 matches;
        猫舌なので... tries 14..2 (13 spans) until 猫舌: its line is full, so the break after it
        joins the next line's 猫 (core/textjoin.h) and the longest span crosses it. One file: one header read; one
        key found, its bucket page, key group, the head ranking its one posting (another
        page: the filler sits between the payloads and the groups) and its payload."""
        self.build(ROWS + FILLER)
        sets = self.run_bench("--modes", "first", "--rounds", "1")
        rows = bench.run(BENCH, self.dir, "main.vjdict", self.lines, "first", 1)
        self.assertEqual(len(rows), 2)
        one, two = rows
        for q, spans in ((one, 8), (two, 13)):
            self.assertEqual(q["spans"], spans)
            self.assertEqual((q["opens"], q["entries"], q["found"], q["ranked"]), (1, 1, 1, 1))
            self.assertEqual(q["reads_by"], {"header": 1, "index": 1, "group": 1, "head": 1, "payload": 1,
                                             "filter": 0})
            self.assertEqual(q["reads"], 5)
            # Every probe but the one found is absent; the first absent one reads the
            # bucket page (one bucket), the others find it cached.
            self.assertEqual(q["absent_reads"], 1)
            self.assertEqual(q["filtered"], 0)
        self.assertEqual((one["candidates"], one["probes"], one["hash_present"], one["miss_cached"]), (13, 13, 1, 1))
        self.assertEqual((two["candidates"], two["probes"], two["hash_present"], two["miss_cached"]), (34, 34, 2, 5))
        summary = sets["sets"][str(self.lines)]["first"]["after"]
        self.assertEqual(summary["found"]["mean"], 1)
        self.assertEqual(summary["reads_by"]["head"], 1)

    def test_a_hash_collision_is_present_but_not_found(self):
        """Two keys of one length forced to one hash: looking up 猫 meets 犬's slot first
        (equal hashes sort by key), reads 犬's key, moves on and finds 猫. One probe: its
        hash present once, its key found once, and no absent reads (reading 犬's key is part
        of the found probe's search)."""
        with mock.patch.object(convert, "key_hash", return_value=convert.key_hash("猫".encode())):
            self.build([term("犬", gloss=["dog"]), term("猫", gloss=["cat"])])
        bench.write_tsv([("cat", "猫")], self.lines, "fixture")
        (q,) = bench.run(BENCH, self.dir, "main.vjdict", self.lines, "first", 1)
        self.assertEqual((q["probes"], q["hash_present"], q["found"], q["entries"]), (1, 1, 1, 1))
        # Both keys' groups share one page: 犬's key read brings 猫's into the page cache.
        self.assertEqual((q["absent_reads"], q["reads_by"]["group"]), (0, 1))
        bench.write_tsv([("bird", "鳥")], self.lines, "fixture")
        (q,) = bench.run(BENCH, self.dir, "main.vjdict", self.lines, "first", 1)
        # 鳥's hash is real, not the forced one: absent, one bucket page.
        self.assertEqual((q["probes"], q["hash_present"], q["found"], q["entries"]), (1, 0, 0, 0))
        self.assertEqual((q["reads"], q["absent_reads"]), (2, 1))

    def test_timed_rounds_fail_on_a_changed_result(self):
        self.build(ROWS)
        argv = [BENCH, str(self.dir), "main.vjdict", str(self.lines), "first", "3"]
        ok = subprocess.run(argv, capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        bad = subprocess.run(argv, capture_output=True, text=True, timeout=60, check=False,
                             env={**os.environ, "VJO_DICT_BENCH_CHANGE_ROUND": "2"})
        self.assertEqual(bad.returncode, 1)
        self.assertIn("result changed between rounds", bad.stderr)

    def test_preparation_counts_background_work_and_preserves_every_result(self):
        self.build(ROWS + FILLER)
        argv = [BENCH, str(self.dir), "main.vjdict", str(self.lines), "words", "3"]
        variants = []
        for flags in ({}, {"VJO_DICT_BENCH_SESSION": "1"}, {"VJO_DICT_BENCH_PREPARE": "1"}):
            run = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                                 env={**os.environ, **flags})
            self.assertEqual(run.returncode, 0, run.stderr)
            variants.append([q for line in run.stdout.splitlines() if "summary" not in (q := json.loads(line))])
        identity = lambda rows: [(q["cursor"], q["rc"], q["entries"], q["end"], q["digest"]) for q in rows]
        self.assertEqual(identity(variants[0]), identity(variants[1]))
        self.assertEqual(identity(variants[0]), identity(variants[2]))
        for q in variants[2]:
            self.assertEqual(q["prepare"]["rc"], 0)
            self.assertLessEqual(q["arena_peak"], 65536)
        self.assertTrue(any(q["prepare"]["reads"] for q in variants[2]))
        self.assertLess(sum(q["reads"] for q in variants[2]), sum(q["reads"] for q in variants[1]))
        # Preparation is outside foreground timing, but the round-equivalence guard still runs.
        bad = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                             env={**os.environ, "VJO_DICT_BENCH_PREPARE": "1", "VJO_DICT_BENCH_CHANGE_ROUND": "1"})
        self.assertEqual(bad.returncode, 1)
        self.assertIn("result changed between rounds", bad.stderr)

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
                {"id": "v1_10_3", "shot": "v1_10", "vn": "v1", "status": "pending", "label": "未確定", "name": False},
                {"id": "v3_14_0", "shot": "v3_14", "vn": "v3", "status": "pending", "label": "未確定", "name": False},
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
