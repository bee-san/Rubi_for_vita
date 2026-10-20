"""tools/reading_stats.py: its character count against the C one (core/reading_chars.h, which the
Shell's history writer runs: host/tests/history_writer.c, VJO_HISTORY_WRITER, set by ctest), on
every code point and on malformed bytes; 0.8.0's files, this writer's and files of both; records
cut short; the reading time and the report. The dialogue is made up and the games are the promo's
mock games (docs/promo/capture/make_promo_games.py).

fixtures/history-0.8.0.csv is RECORDS as 0.8.0's writer wrote them: shell/history.c at main 44d4e8d
(the release's writer, #187), built on the host with host/tests/history_writer.c's record mode."""
import csv
import io
import os
import random
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import reading_stats as rs  # noqa: E402

WRITER = os.environ.get("VJO_HISTORY_WRITER")
FIXTURE = ROOT / "tools/tests/fixtures/history-0.8.0.csv"
need_writer = unittest.skipUnless(WRITER, "VJO_HISTORY_WRITER (the host build's vjo-history-writer, set by ctest) "
                                          "is not set")

T0 = 1792220400250123  # 2026-10-17T07:00:00.250123Z, in microseconds
A, B = ("PROMO0004", "Ember Garden"), ("PROMO0001", "夕凪ステップ")
# (game, seconds after T0, the text as recognized, its characters once normalised as the game
# worker returns it)
RECORDS = [
    (A, 0, "「また明日、一緒に見に行こう！」", 12),
    (A, 12, "うん。", 2),
    (A, 30, "１０時に駅で待ってるね。", 11),       # full-width digits: ASCII once normalised
    (A, 45, "……", 0),                          # a line, no character
    (A, 1245, "「えっ、本当に？」\n「本当だよ」", 9),  # after a 20-minute break; two lines
    (A, 1265, "コーヒーを一杯どうぞ。", 10),
    (B, 1300, "Stage 2: スタート!", 10),         # another game
    (B, 1310, "\"Quote\", comma", 10),
    (B, 86400, "おはよう", 4),                    # the next day
    (B, 86408, "今日も頑張ろう♪", 7),
]
COUNTS = [c for *_, c in RECORDS]


def records_input(records):
    return b"".join(f"{tid}\x1f{name}\x1f{T0 + int(sec * 1e6)}\x1f{text}\0".encode()
                    for (tid, name), sec, text, _ in records)


def write_history(path, records):
    out = subprocess.run([WRITER, str(path)], input=records_input(records), capture_output=True, check=True)
    return [int(x) for x in out.stdout.split()]


def c_counts(items):
    """core/reading_chars.h's count of each byte string (none holding a NUL)."""
    out = subprocess.run([WRITER, "--count"], input=b"".join(i + b"\0" for i in items), capture_output=True,
                         check=True)
    return [int(x) for x in out.stdout.split()]


def py_count(item: bytes) -> int:
    return rs.characters(item.decode("utf-8", errors="replace"))


class Counter(unittest.TestCase):
    def test_table_is_core_utf_c(self):
        src = (ROOT / "core/utf.c").read_text()
        body = src[src.index("japanese_letters[] = {"):]
        body = body[:body.index("};")]
        table = tuple((int(a, 16), int(b, 16)) for a, b in re.findall(r"\{0x([0-9A-F]+)u, 0x([0-9A-F]+)u\}", body))
        self.assertEqual(len(table), 33)
        self.assertEqual(table, rs.JAPANESE_LETTERS)

    def test_cases(self):
        cases = {
            "ひらがなカタカナ": 8, "漢字と仮名": 5, "OK, 2人で100円!": 9, "「えっ……？」『うん』": 4,
            "コーヒー・タイム": 7, "人々\u3000ヶ月〆": 5, "ｶﾀｶﾅ": 4, "𠮟る": 2, "😀笑": 1,
            "一行目\n二行目\r\n三": 7, "♪〜〜♪": 0, "": 0, " \t\n": 0,
            "ＯＫ１２３": 0,  # full-width forms count as ASCII only once normalised, as the line is
        }
        for text, n in cases.items():
            self.assertEqual(rs.characters(text), n, text)

    @need_writer
    def test_c_every_code_point(self):
        """Every code point but NUL as its UTF-8, and the surrogates' 3-byte forms (not UTF-8)."""
        items = [chr(cp).encode() for cp in range(1, 0x110000) if not 0xD800 <= cp <= 0xDFFF]
        items += [bytes([0xED, 0xA0 | (cp >> 6 & 0x1F), 0x80 | (cp & 0x3F)]) for cp in range(0xD800, 0xE000)]
        got = c_counts(items)
        self.assertEqual(len(got), len(items))
        wrong = [(i, g) for i, g in zip(items, got) if g != py_count(i)]
        self.assertEqual(wrong[:5], [])
        self.assertEqual(sum(got), sum(rs.counts(chr(cp)) for cp in range(1, 0x110000)
                                       if not 0xD800 <= cp <= 0xDFFF))
        self.assertGreater(sum(got), 100000)  # the kanji are there

    @need_writer
    def test_c_malformed_and_random_bytes(self):
        items = [b"\xc1\x81", b"\xe0\x81\x81", b"\xf0\x83\x81\x82", b"\xf0\x80\x80\xb0", b"\xf4\x90\x80\x80",
                 b"\xf8\x88\x80\x80\x80", b"\xff\xfe", "あ".encode() + b"\x80" + "い".encode() + b"\xe3\x81",
                 b"\xe3\x81\xe3\x81\x82", b"\xc2\xe3\x81\x82", b"\xf0\xe3\x81\x82\x80", b"A\xe3B"]
        rnd = random.Random(20261020)
        alphabet = list(range(0x01, 0x80)) + list(range(0x80, 0xC0)) * 2 + list(range(0xC0, 0x100)) * 2
        items += [bytes(rnd.choice(alphabet) for _ in range(rnd.randrange(0, 40))) for _ in range(20000)]
        items += [rnd.choice(["あ", "漢", "ー", "・", "A", "1", "𠮟", "😀"]).encode() * rnd.randrange(1, 4)
                  + bytes(rnd.choice(alphabet) for _ in range(rnd.randrange(0, 6))) for _ in range(5000)]
        self.assertEqual(c_counts(items), [py_count(i) for i in items])


def gsm_rows(path):
    """GameSentenceMiner's ExStatic import (GameSentenceMiner/web/import_api.py at 7a46a00): the
    first line's names must include the five, then csv.DictReader reads each row by name."""
    content = Path(path).read_bytes().decode("utf-8-sig")
    headers = {h.strip() for h in next(csv.reader([content.split("\n")[0].strip()]))}
    assert {"uuid", "given_identifier", "name", "line", "time"} <= headers
    reader = csv.DictReader(io.StringIO(content), quoting=csv.QUOTE_MINIMAL, skipinitialspace=True)
    return [{k: row.get(k, "").strip() for k in ("uuid", "given_identifier", "name", "line", "time")}
            for row in reader]


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="vjo-reading-stats-")
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def check_records(self, report, stored):
        self.assertEqual([r.characters for r in report.records], COUNTS)
        self.assertEqual([r.stored for r in report.records], stored)
        self.assertEqual([r.game for r in report.records], ["vita-" + g[0] for g, *_ in RECORDS])
        self.assertEqual(report.mismatched, 0)

    def test_080_file(self):
        rows = list(csv.reader(io.StringIO(FIXTURE.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080)
        self.assertEqual({len(r) for r in rows}, {41})
        report = rs.read_history([FIXTURE])
        self.check_records(report, [False] * len(RECORDS))
        self.assertEqual((report.incomplete, report.duplicates, report.stored), (0, 0, 0))

    @need_writer
    def test_new_file(self):
        path = self.dir / "history.csv"
        self.assertEqual(write_history(path, RECORDS), [1] * len(RECORDS))
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080 + ["characters"])
        self.assertEqual({len(r) for r in rows}, {42})
        self.assertEqual([int(r[41]) for r in rows[1:]], COUNTS)
        report = rs.read_history([path])
        self.check_records(report, [True] * len(RECORDS))
        # the same records as the 0.8.0 file, cell for cell, but the new cell and the byte offsets
        # in given_identifier (the header and the rows are longer)
        old = list(csv.reader(io.StringIO(FIXTURE.read_text(encoding="utf-8"), newline="")))
        self.assertEqual([r[:1] + r[2:41] for r in rows[1:]], [r[:1] + r[2:] for r in old[1:]])

    @need_writer
    def test_file_080_began(self):
        """A newer Rubi appends to 0.8.0's file: its header stays, its rows get a 42nd cell."""
        path = self.dir / "history.csv"
        path.write_bytes(FIXTURE.read_bytes())
        more = [(A, 172800, "三日目です。", 5), (A, 172810, "ABCは3文字", 7)]
        self.assertEqual(write_history(path, more), [1, 1])
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080)
        self.assertEqual([len(r) for r in rows[1:]], [41] * len(RECORDS) + [42, 42])
        report = rs.read_history([path])
        self.assertEqual([r.characters for r in report.records], COUNTS + [5, 7])
        self.assertEqual([r.stored for r in report.records], [False] * len(RECORDS) + [True, True])
        self.assertEqual(report.mismatched, 0)

    @need_writer
    def test_gsm_import_reads_all_three(self):
        """GSM's importer reads the five ExStatic columns by name: the extra cell is ignored."""
        new, mixed = self.dir / "new.csv", self.dir / "mixed.csv"
        write_history(new, RECORDS)
        mixed.write_bytes(FIXTURE.read_bytes())
        write_history(mixed, [(B, 172800, "続き", 2)])
        expected = [(f"vita-{tid}", name, text) for (tid, name), _, text, _ in RECORDS]
        for path in (FIXTURE, new, mixed):
            rows = gsm_rows(path)
            got = [(r["uuid"], r["name"], r["line"]) for r in rows[:len(RECORDS)]]
            self.assertEqual([g[:2] for g in got], [e[:2] for e in expected], path.name)
            self.assertTrue(all(r["given_identifier"].startswith("vjo-") and float(r["time"]) > 1.7e9 for r in rows))
            self.assertEqual(rs.characters(rows[2]["line"]), 11)  # normalised: 10時に駅で待ってるね。
        self.assertEqual(gsm_rows(mixed)[-1]["line"], "続き")

    @need_writer
    def test_cut_short(self):
        """Power lost while a record was written: whatever is left of it is skipped, never miscounted."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS)
        data = path.read_bytes()
        last = data.rindex(b"\n", 0, len(data) - 1) + 1
        record = data[last:]
        cuts = {len(record) - 1, len(record) - 2,  # no final line break; the count's digit too
                record.index("今日".encode()) + 3,  # in the line
                record.index(b'"vocr"') + 2,       # among the OCR facts
                record.index(b"vjo-") + 6}          # in given_identifier
        for cut in sorted(cuts):
            (self.dir / "cut.csv").write_bytes(data[:last + cut])
            report = rs.read_history([self.dir / "cut.csv"])
            self.assertEqual((len(report.records), report.incomplete), (len(RECORDS) - 1, 1), cut)
            self.assertEqual([r.characters for r in report.records], COUNTS[:-1])
        two = "「一行目\n二行目」".encode()  # cut right after a line break inside the quoted line
        write_history(self.dir / "nl.csv", [(A, 0, "前の行", 3), (A, 10, "「一行目\n二行目」", 6)])
        data = (self.dir / "nl.csv").read_bytes()
        (self.dir / "nl.csv").write_bytes(data[:data.index(two) + two.index(b"\n") + 1])
        report = rs.read_history([self.dir / "nl.csv"])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), ([3], 1))

    @need_writer
    def test_copies_count_once(self):
        a, b = self.dir / "a.csv", self.dir / "b.csv"
        write_history(a, RECORDS)
        b.write_bytes(a.read_bytes())
        write_history(b, [(B, 90000, "新しい行", 4)])
        report = rs.read_history([a, b])
        self.assertEqual((len(report.records), report.duplicates), (len(RECORDS) + 1, len(RECORDS)))

    def test_not_a_history(self):
        bad = self.dir / "bad.csv"
        bad.write_text("a,b\n1,2\n")
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
            self.assertEqual(rs.main([str(bad)]), 2)
            bad.write_bytes(b"")
            self.assertEqual(rs.main([str(bad)]), 2)
            self.assertEqual(rs.main([str(self.dir / "missing.csv")]), 2)
        self.assertIn("not a Rubi history.csv (no uuid, given_identifier, name, line, time column)", err.getvalue())


class Time(unittest.TestCase):
    def report(self, minutes=5.0):
        report = rs.read_history([FIXTURE])
        rs.credit_time(report.records, minutes * 60)
        return report

    def test_breaks(self):
        seconds = [r.seconds for r in self.report().records]
        # 20 minutes after "……" is a break, so is the change of game and the night; the last has none
        self.assertEqual(seconds, [12, 18, 15, None, 20, None, 10, None, 8, None])
        seconds = [r.seconds for r in self.report(30).records]
        self.assertEqual(seconds[3], 1200)

    def test_totals(self):
        total, games, names, days = rs.summarize(self.report().records, utc=True)
        self.assertEqual((total.lines, total.characters, total.timed_characters, total.seconds), (10, 75, 48, 83))
        self.assertAlmostEqual(total.per_hour(), 48 * 3600 / 83)
        a, b = games["vita-PROMO0004"], games["vita-PROMO0001"]
        self.assertEqual((a.lines, a.characters, a.timed_characters, a.seconds), (6, 44, 34, 65))
        self.assertEqual((b.lines, b.characters, b.timed_characters, b.seconds), (4, 31, 14, 18))
        self.assertEqual(names["vita-PROMO0001"], "夕凪ステップ")
        self.assertEqual(sorted(days), ["2026-10-17", "2026-10-18"])
        self.assertEqual((days["2026-10-17"].characters, days["2026-10-17"].seconds), (64, 75))
        self.assertEqual((days["2026-10-18"].characters, days["2026-10-18"].per_hour()), (11, 1800))

    def test_report_text(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(rs.main(["--utc", str(FIXTURE)]), 0)
        text = out.getvalue()
        self.assertIn("1 file, 10 records\n", text)
        self.assertIn("Characters: 0 records carry their count, 10 counted here\n", text)
        self.assertRegex(text, r"\nAll games +10 +75 +0 h 01 m +2,082\n")
        self.assertRegex(text, r"\nEmber Garden \(vita-PROMO0004\) +6 +44 +0 h 01 m +1,883\n")
        self.assertRegex(text, r"\n夕凪ステップ \(vita-PROMO0001\) +4 +31 +0 h 00 m +2,800\n")
        self.assertRegex(text, r"By day \(UTC\)\n.*\n2026-10-17 +8 +64 .*\n2026-10-18 +2 +11 +0 h 00 m +1,800\n")
        for _, _, line, _ in RECORDS:  # no dialogue in the report
            self.assertNotIn(line, text)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            rs.main(["--break-minutes", "0", str(FIXTURE)])


if __name__ == "__main__":
    unittest.main()
