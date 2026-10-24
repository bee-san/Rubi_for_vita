"""tools/reading_stats.py: its character count against the C one (core/reading_chars.h, which the
Shell's history writer runs: host/tests/history_writer.c, VJO_HISTORY_WRITER, set by ctest), on
every code point and on malformed bytes; 0.8.0's files, 0.9.1's, this writer's and files of them all;
records cut short; each line's text source (line_source, or an older Rubi's engine); the reading time
and the report. The dialogue is made up and the games are the promo's mock games
(docs/promo/capture/make_promo_games.py).

fixtures/history-0.8.0.csv is RECORDS as 0.8.0's writer wrote them: shell/history.c at main 44d4e8d
(the release's writer, #187), built on the host with host/tests/history_writer.c's record mode.
fixtures/history-0.9.1.csv is RECORDS from the sources FROM gives as 0.9.1's writer wrote them (42
columns, engine hook or vocr, no line_source): the v0.9.1 tree (e33f8f6) with this change's
host/tests/history_writer.c, whose fifth field is the source (test_091_fixture_is_the_writers says how)."""
import csv
import datetime
import io
import os
import random
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock
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
# Each record's text_from (VjoGameOcrResult): 0 OCR's line, else the native hook stream that supplied it (a
# string copy's 9, a table site's 1). Ember Garden's hook takes over after its first line; 夕凪ステップ has none.
FROM = [0, 9, 9, 0, 1, 1, 0, 0, 0, 0]
SOURCES = ["hook" if f else "ocr" for f in FROM]
FIXTURE_091 = ROOT / "tools/tests/fixtures/history-0.9.1.csv"


def records_input(records, sources=None):
    return b"".join(f"{tid}\x1f{name}\x1f{T0 + int(sec * 1e6)}\x1f{text}"
                    f"{'' if sources is None else chr(0x1F) + str(sources[i])}\0".encode()
                    for i, ((tid, name), sec, text, _) in enumerate(records))


def write_history(path, records, sources=None):
    """Appends records with the real writer; sources: each one's text_from (default: all OCR's)."""
    out = subprocess.run([WRITER, str(path)], input=records_input(records, sources), capture_output=True,
                         check=True)
    return [int(x) for x in out.stdout.split()]


def c_counts(items):
    """core/reading_chars.h's count of each byte string (none holding a NUL)."""
    out = subprocess.run([WRITER, "--count"], input=b"".join(i + b"\0" for i in items), capture_output=True,
                         check=True)
    return [int(x) for x in out.stdout.split()]


def shell_total(path):
    """The Shell's reading total of a history.csv (shell/history.c, docs/ocr-history.md#total-characters-read),
    counted as a Triangle press has the control thread count it, with nothing kept from before: (characters,
    the bytes they are of, the windows read)."""
    out = subprocess.run([WRITER, "--total", str(path)], capture_output=True, check=True)
    chars, covered, windows, _ns = (int(x) for x in out.stdout.split())
    return chars, covered, windows


def py_count(item: bytes) -> int:
    return rs.characters(item.decode("utf-8", errors="replace"))


class Counter(unittest.TestCase):
    def test_table_is_core_utf_c(self):
        src = (ROOT / "core/utf.c").read_text()
        declaration = re.search(r"\bjapanese_letters\[\]\s*(?:VJO_DATA_TABLE\([^)]*\)\s*)?=\s*\{", src)
        self.assertIsNotNone(declaration)
        body = src[declaration.end():]
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
        got = c_counts(items)
        self.assertEqual(len(got), len(items))
        wrong = [(i, g, py_count(i)) for i, g in zip(items, got) if g != py_count(i)]
        self.assertEqual(wrong[:5], [])  # the first ones only: a diff of two 25,012-item lists takes minutes


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

    def check_records(self, report, stored, sources=None):
        self.assertEqual([r.characters for r in report.records], COUNTS)
        self.assertEqual([r.stored for r in report.records], stored)
        self.assertEqual([r.game for r in report.records], ["vita-" + g[0] for g, *_ in RECORDS])
        self.assertEqual([r.source for r in report.records], sources or ["ocr"] * len(RECORDS))
        self.assertEqual(report.mismatched, 0)

    def test_080_file(self):
        """0.8.0 had no native hooks: every line is OCR's, as its engine (vocr) says."""
        rows = list(csv.reader(io.StringIO(FIXTURE.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080)
        self.assertEqual({len(r) for r in rows}, {41})
        self.assertEqual({r[7] for r in rows[1:]}, {"vocr"})
        report = rs.read_history([FIXTURE])
        self.check_records(report, [False] * len(RECORDS))
        self.assertEqual((report.incomplete, report.duplicates, report.stored), (0, 0, 0))
        self.assertEqual((report.source_cells, report.source_engines), (0, len(RECORDS)))

    def test_091_file(self):
        """0.9.1's file has the characters and no line_source: its engine says hook for a hook's line."""
        rows = list(csv.reader(io.StringIO(FIXTURE_091.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080 + ["characters"])
        self.assertEqual({len(r) for r in rows}, {42})
        self.assertEqual([r[7] for r in rows[1:]], ["hook" if f else "vocr" for f in FROM])
        report = rs.read_history([FIXTURE_091])
        self.check_records(report, [True] * len(RECORDS), SOURCES)
        self.assertEqual((report.incomplete, report.stored, report.source_cells, report.source_engines),
                         (0, len(RECORDS), 0, len(RECORDS)))

    @need_writer
    def test_new_file(self):
        path = self.dir / "history.csv"
        self.assertEqual(write_history(path, RECORDS), [1] * len(RECORDS))
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER)
        self.assertEqual(rs.HEADER, rs.HEADER_080 + ["characters", "line_source"])
        self.assertEqual(len(set(rows[0])), len(rows[0]))  # no name twice: `source` is the capture route's
        self.assertEqual({len(r) for r in rows}, {43})
        self.assertEqual([int(r[41]) for r in rows[1:]], COUNTS)
        self.assertEqual([r[42] for r in rows[1:]], ["ocr"] * len(RECORDS))
        report = rs.read_history([path])
        self.check_records(report, [True] * len(RECORDS))
        self.assertEqual((report.source_cells, report.source_engines), (len(RECORDS), 0))
        # the same records as the 0.8.0 file, cell for cell, but the new cells and the byte offsets
        # in given_identifier (the header and the rows are longer)
        old = list(csv.reader(io.StringIO(FIXTURE.read_text(encoding="utf-8"), newline="")))
        self.assertEqual([r[:1] + r[2:41] for r in rows[1:]], [r[:1] + r[2:] for r in old[1:]])

    @need_writer
    def test_091_fixture_is_the_writers(self):
        """This writer writes 0.9.1's records with the line_source cell appended: the same 42 cells but the byte
        offsets in given_identifier, its engine and line_source agreeing. The fixture was made with 0.9.1's
        writer (v0.9.1, e33f8f6) and this change's host/tests/history_writer.c, with its one call that changed
        since 0.9.1 put back in 0.9.1's form (API 36, #247, gave vjo_text_normalize_keep a flags argument):

            mkdir -p /tmp/t && git archive v0.9.1 | tar -x -C /tmp/t
            cp host/tests/history_writer.c /tmp/t/host/tests/
            cd /tmp/t && sed -i 's/sizeof(result.text), NULL, 0, NULL)/sizeof(result.text), NULL, NULL)/' \\
              host/tests/history_writer.c
            gcc -std=gnu11 -DVJO_HOST=1 -Ihost/tests/vita_stubs -Icore -Iinclude \\
              host/tests/history_writer.c core/utf.c core/text_rows.c core/textnorm.c core/history_lines.c \\
              core/arena.c core/textfilter.c -o writer-091
            ./writer-091 history-0.9.1.csv < records   # records_input(RECORDS, FROM)
        """
        path = self.dir / "history.csv"
        self.assertEqual(write_history(path, RECORDS, FROM), [1] * len(RECORDS))
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        old = list(csv.reader(io.StringIO(FIXTURE_091.read_text(encoding="utf-8"), newline="")))
        self.assertEqual([r[:1] + r[2:42] for r in rows[1:]], [r[:1] + r[2:] for r in old[1:]])
        self.assertEqual([r[42] for r in rows[1:]], SOURCES)
        self.assertEqual([r[7] for r in rows[1:]], ["hook" if s == "hook" else "vocr" for s in SOURCES])
        report = rs.read_history([path])
        self.check_records(report, [True] * len(RECORDS), SOURCES)
        self.assertEqual((report.source_cells, report.source_engines), (len(RECORDS), 0))

    @need_writer
    def test_file_080_began(self):
        """A newer Rubi appends to 0.8.0's file: its header stays, its rows get a 42nd and a 43rd cell."""
        path = self.dir / "history.csv"
        path.write_bytes(FIXTURE.read_bytes())
        more = [(A, 172800, "三日目です。", 5), (A, 172810, "ABCは3文字", 7)]
        self.assertEqual(write_history(path, more, [9, 0]), [1, 1])
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080)
        self.assertEqual([len(r) for r in rows[1:]], [41] * len(RECORDS) + [43, 43])
        report = rs.read_history([path])
        self.assertEqual([r.characters for r in report.records], COUNTS + [5, 7])
        self.assertEqual([r.stored for r in report.records], [False] * len(RECORDS) + [True, True])
        self.assertEqual([r.source for r in report.records], ["ocr"] * len(RECORDS) + ["hook", "ocr"])
        self.assertEqual((report.mismatched, report.source_cells, report.source_engines), (0, 2, len(RECORDS)))

    @need_writer
    def test_file_091_began(self):
        """A newer Rubi appends to 0.9.1's file: its header stays, its rows get a 43rd cell; the older rows'
        sources come from their engine, the newer rows' from their line_source."""
        path = self.dir / "history.csv"
        path.write_bytes(FIXTURE_091.read_bytes())
        more = [(B, 172800, "三日目です。", 5), (B, 172810, "ABCは3文字", 7)]
        self.assertEqual(write_history(path, more, [1, 0]), [1, 1])
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        self.assertEqual(rows[0], rs.HEADER_080 + ["characters"])
        self.assertEqual([len(r) for r in rows[1:]], [42] * len(RECORDS) + [43, 43])
        report = rs.read_history([path])
        self.assertEqual([r.characters for r in report.records], COUNTS + [5, 7])
        self.assertEqual([r.stored for r in report.records], [True] * (len(RECORDS) + 2))
        self.assertEqual([r.source for r in report.records], SOURCES + ["hook", "ocr"])
        self.assertEqual((report.mismatched, report.source_cells, report.source_engines), (0, 2, len(RECORDS)))

    @need_writer
    def test_gsm_import_reads_them_all(self):
        """GSM's importer reads the five ExStatic columns by name: the extra cells are ignored. A reader by
        name (csv.DictReader, as GSM's) finds line_source by its name, and `source` is still the capture route."""
        new, mixed, mixed_091 = self.dir / "new.csv", self.dir / "mixed.csv", self.dir / "mixed-091.csv"
        write_history(new, RECORDS, FROM)
        mixed.write_bytes(FIXTURE.read_bytes())
        write_history(mixed, [(B, 172800, "続き", 2)])
        mixed_091.write_bytes(FIXTURE_091.read_bytes())
        write_history(mixed_091, [(B, 172800, "続き", 2)], [9])
        expected = [(f"vita-{tid}", name, text) for (tid, name), _, text, _ in RECORDS]
        for path in (FIXTURE, FIXTURE_091, new, mixed, mixed_091):
            rows = gsm_rows(path)
            got = [(r["uuid"], r["name"], r["line"]) for r in rows[:len(RECORDS)]]
            self.assertEqual([g[:2] for g in got], [e[:2] for e in expected], path.name)
            self.assertTrue(all(r["given_identifier"].startswith("vjo-") and float(r["time"]) > 1.7e9 for r in rows))
            self.assertEqual(rs.characters(rows[2]["line"]), 11)  # normalised: 10時に駅で待ってるね。
        self.assertEqual(gsm_rows(mixed)[-1]["line"], "続き")
        self.assertEqual(gsm_rows(mixed_091)[-1]["line"], "続き")
        by_name = list(csv.DictReader(io.StringIO(new.read_text(encoding="utf-8"), newline="")))
        self.assertEqual([r["line_source"] for r in by_name], SOURCES)
        self.assertEqual([r["engine"] for r in by_name], ["hook" if s == "hook" else "vocr" for s in SOURCES])
        self.assertEqual({r["source"] for r in by_name}, {"0"})  # VJO_GAME_OCR_SRC_NONE in the harness's results

    @need_writer
    def test_cut_short(self):
        """Power lost while a record was written: whatever is left of it is skipped, never miscounted."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS, FROM)
        data = path.read_bytes()
        last = data.rindex(b"\n", 0, len(data) - 1) + 1
        record = data[last:]
        source = record.rindex(b',"ocr"')  # the line_source cell's comma
        cuts = {len(record) - 1, len(record) - 2,  # no final line break; the line_source's quote too
                source + 3, source,                # in the line_source cell; before it (the count whole)
                source - 1,                         # in the count
                record.index("今日".encode()) + 3,  # in the line
                record.index(b'"vocr"') + 2,       # among the OCR facts
                record.index(b"vjo-") + 6}          # in given_identifier
        for cut in sorted(cuts):
            (self.dir / "cut.csv").write_bytes(data[:last + cut])
            report = rs.read_history([self.dir / "cut.csv"])
            self.assertEqual((len(report.records), report.incomplete), (len(RECORDS) - 1, 1), cut)
            self.assertEqual([r.characters for r in report.records], COUNTS[:-1])
            self.assertEqual([r.source for r in report.records], SOURCES[:-1])
        two = "「一行目\n二行目」".encode()  # cut right after a line break inside the quoted line
        write_history(self.dir / "nl.csv", [(A, 0, "前の行", 3), (A, 10, "「一行目\n二行目」", 6)])
        data = (self.dir / "nl.csv").read_bytes()
        (self.dir / "nl.csv").write_bytes(data[:data.index(two) + two.index(b"\n") + 1])
        report = rs.read_history([self.dir / "nl.csv"])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), ([3], 1))

    @need_writer
    def test_every_cut_and_a_record_after_it(self):
        """A power loss can cut the last record at any byte; the next append starts right after it (the
        writer adds no line break first). Every cut position, of an OCR line's record and of a hook's: the cut
        record is skipped, every other counted with its source, and the record appended after it is read."""
        for cut_from in (0, 9):
            sources = FROM[:-2] + [cut_from]
            path = self.dir / "history.csv"
            path.unlink(missing_ok=True)
            write_history(path, RECORDS[:-1], sources)
            data = path.read_bytes()
            last = data.rindex(b"\n", 0, len(data) - 1) + 1
            self.assertTrue(data.endswith(b',"hook"\n' if cut_from else b',"ocr"\n'))
            (self.dir / "next.csv").unlink(missing_ok=True)
            write_history(self.dir / "next.csv", [RECORDS[-1]], [1])
            after = (self.dir / "next.csv").read_bytes()
            after = after[after.index(b"\n") + 1:]  # its record without the header
            for cut in range(last + 1, len(data)):
                (self.dir / "cut.csv").write_bytes(data[:cut] + after)
                report = rs.read_history([self.dir / "cut.csv"])
                got = ([r.characters for r in report.records], [r.source for r in report.records], report.incomplete)
                self.assertEqual(got, (COUNTS[:-2] + [COUNTS[-1]], SOURCES[:-2] + ["hook"], 1), (cut_from, cut))

    @need_writer
    def test_name_like_an_identifier(self):
        """A game's name that looks like a given_identifier does not split its record."""
        path = self.dir / "history.csv"
        records = [(("PROMO0002", "vjo-1-2"), 0, "名前が変", 4), (("PROMO0002", '"vita-X","vjo-1-2",'), 5, "続き", 2)]
        write_history(path, records)
        report = rs.read_history([path])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), ([4, 2], 0))

    @need_writer
    def test_names_and_lines_like_a_record_start(self):
        """A name or a line that is "vita-..." or "psp-..." (one half of what a record starts with) does
        not split its record."""
        path = self.dir / "history.csv"
        records = [(("PROMO0002", "vita-PROMO0002"), 0, "psp-PROMO0001", 12), (("PROMO0002", "psp-X"), 5, "vita-Y", 5),
                   (("PROMO0002", "vita-Z"), 9, "続き", 2)]
        write_history(path, records)
        report = rs.read_history([path])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), ([12, 5, 2], 0))
        self.assertEqual([r.name for r in report.records], ["vita-PROMO0002", "psp-X", "vita-Z"])

    @need_writer
    def test_blank_lines_and_joined_files(self):
        """Blank lines (an edited copy) and the header lines of files joined with cat are no records and
        no records cut short."""
        a, b = self.dir / "a.csv", self.dir / "b.csv"
        write_history(a, RECORDS[:5])
        write_history(b, RECORDS[5:])
        data = a.read_bytes()
        first = data.index(b"\n") + 1
        second = data.index(b"\n", data.index(b"vjo-", first) + 1) + 1  # after the first record
        header_only = b.read_bytes()[:b.read_bytes().index(b"\n") + 1]
        cases = {
            "blank lines": data[:second] + b"\n\r\n" + data[second:] + b"\n\n",
            "blank lines after the header": data[:first] + b"\n\n" + data[first:],
            "two files joined": data + b.read_bytes(),
            "two files joined, the second's BOM and CRLF header": data + b"\xef\xbb\xbf"
            + b.read_bytes().replace(b"line_source\n", b"line_source\r\n", 1),
            "a header-only file and a blank line joined between them": data + header_only + b"\n" + b.read_bytes(),
        }
        for name, content in cases.items():
            path = self.dir / "edited.csv"
            path.write_bytes(content)
            report = rs.read_history([path])
            want = COUNTS[:5] if name.startswith("blank") else COUNTS
            self.assertEqual(([r.characters for r in report.records], report.incomplete), (want, 0), name)
        # blank lines after the header, then a record cut short and the next append after it: the file is
        # still read as the writer's bytes (as plain CSV, the cut record's open quote would take the next)
        cut = data[:data.index(b"\n", data.index("待ってる".encode())) - 3]
        path.write_bytes(cut[:first] + b"\n\n" + cut[first:] + b.read_bytes()[len(header_only):])
        report = rs.read_history([path])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), (COUNTS[:2] + COUNTS[5:], 1))

    @need_writer
    def test_zeros_after_a_power_loss(self):
        """A file system can leave zeros where a write was lost: 4 KiB, and 200 KB (more than csv's field
        limit, 131,072), at the end and between two records. They are one record cut short each."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS[:3])
        more = self.dir / "more.csv"
        write_history(more, RECORDS[3:5])
        tail = more.read_bytes()
        tail = tail[tail.index(b"\n") + 1:]
        for zeros in (4096, 200000):
            for name, content in (("at the end", path.read_bytes() + b"\0" * zeros),
                                  ("in the middle", path.read_bytes() + b"\0" * zeros + tail),
                                  ("with a line break", path.read_bytes() + b"\0" * zeros + b"\n" + tail)):
                (self.dir / "zeros.csv").write_bytes(content)
                report = rs.read_history([self.dir / "zeros.csv"])
                want = COUNTS[:3] if name == "at the end" else COUNTS[:5]
                self.assertEqual(([r.characters for r in report.records], report.incomplete), (want, 1),
                                 f"{zeros} zeros {name}")

    def test_other_trackers_files(self):
        """An ExStatic lines CSV that Rubi did not write (exSTATic's own export: a BOM, CRLF, papaparse's
        quoting only where needed, a given_identifier per game) is read as plain CSV, and counted here."""
        path = self.dir / "exSTATic_lines.csv"
        path.write_bytes("\ufeffuuid,given_identifier,name,line,time\r\n"
                         "0f9d-11,Some VN,Some VN,今日は晴れ。,1792220400.5\r\n"
                         "0f9d-11,Some VN,Some VN,\"「えっ、\r\n本当？」\",1792220410\r\n"
                         "0f9d-11,Some VN,Some VN,今日は晴れ。,1792220400.5\r\n"   # the same line again
                         "0f9d-11,Some VN,Some VN,時間がない,\r\n"                  # no time: skipped
                         "0f9d-11,Some VN,Some VN,同じ文,1792220420\r\n"
                         "0f9d-11,Some VN,Some VN,同じ文,1792220430\r\n".encode())  # the same text, later
        report = rs.read_history([path])
        self.assertEqual([r.characters for r in report.records], [5, 4, 3, 3])
        self.assertEqual((report.duplicates, report.incomplete, report.stored), (1, 1, 0))
        self.assertEqual({r.game for r in report.records}, {"0f9d-11"})
        # no line_source or engine column: where its lines came from is unknown
        self.assertEqual({r.source for r in report.records}, {"unknown"})
        self.assertEqual((report.source_cells, report.source_engines), (0, 0))
        self.assertEqual(list(rs.by_source(report.records)), ["hook", "ocr", "unknown"])
        # another tracker's columns of those names, which it can fill as it likes: hook and ocr only
        path.write_bytes("uuid,given_identifier,name,line,time,engine,line_source\n"
                         "g1,VN,VN,一つ目,1792220400,hook,\n"
                         "g1,VN,VN,二つ目,1792220410,textractor,OCR\n"
                         "g1,VN,VN,三つ目,1792220420,vocr,clipboard\n"
                         "g1,VN,VN,四つ目,1792220430,clipboard,\n".encode())
        report = rs.read_history([path])
        self.assertEqual([r.source for r in report.records], ["hook", "ocr", "ocr", "unknown"])
        self.assertEqual((report.source_cells, report.source_engines), (1, 2))

    def test_other_trackers_odd_rows(self):
        """Plain CSV's own troubles: a row without the five columns, a cell csv refuses (over its field limit,
        131,072 characters) and the rows after them; two lines at the same time; a column the rows leave out."""
        path = self.dir / "lines.csv"
        path.write_bytes(("uuid,given_identifier,name,line,time\n"
                          "g1,Some VN,Some VN,一つ目,1792220400\n"
                          "g1,Some VN,Some VN\n"                                       # short: incomplete
                          "g1,Some VN,Some VN," + "長" * 200000 + ",1792220405\n"       # csv refuses it
                          "g1,Some VN,Some VN,二つ目,1792220410\n"
                          "g1,Some VN,Some VN,同時の文,1792220420\n"                    # same time, another line
                          "g1,Some VN,Some VN,同時の別の文,1792220420\n").encode())
        report = rs.read_history([path])
        self.assertEqual([r.characters for r in report.records], [3, 3, 4, 6])
        self.assertEqual((report.incomplete, report.duplicates), (2, 0))
        # a header naming a column after the five that the rows leave out: they are whole all the same
        path.write_bytes("uuid,given_identifier,name,line,time,note\ng2,VN,VN,最後,1792220400\n".encode())
        report = rs.read_history([path])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), ([2], 0))

    def test_times_that_are_not_unix_seconds(self):
        """Milliseconds (another tracker's), a time past the year 3000, zero and a negative one are numbers
        but not Unix seconds; a word, NaN and inf are no time (a damaged record, as a cut record's time with
        the next file's header glued to it): each record is skipped and counted, none stops the report. A
        time after 2038 is read, where this computer can place it on a day."""
        path = self.dir / "lines.csv"
        times = ["1792220400500", "32503680000", "0", "-5", "soon", "17922uuid", "nan", "inf"]
        path.write_bytes(("uuid,given_identifier,name,line,time\n" + "".join(
            f"g1,VN,VN,文{i},{t}\n" for i, t in enumerate(times)) + "g1,VN,VN,よし,1792220400\n"
            "g1,VN,VN,ずっと先,2200000000\n").encode())
        report = rs.read_history([path])
        later = rs.placeable(2200000000.0)  # 2039-09-18: not where time_t has 32 bits
        self.assertEqual(([r.characters for r in report.records], report.bad_times, report.incomplete),
                         ([2, 4] if later else [2], 4 if later else 5, 4))
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(rs.main(["--utc", str(path)]), 0)
        self.assertIn(f"1 file, {2 if later else 1} records (4 incomplete skipped; {4 if later else 5} with a time "
                      "that is not Unix seconds skipped)\n", out.getvalue())

    def test_a_32_bit_time_t(self):
        """Where time_t has 32 bits, datetime cannot place a time after 2038 (OverflowError): such a record is a
        bad time, and the report is made."""
        class Datetime32(datetime.datetime):
            @classmethod
            def fromtimestamp(cls, t, tz=None):
                if t >= 2 ** 31:
                    raise OverflowError("timestamp out of range for platform time_t")
                return datetime.datetime.fromtimestamp(t, tz)
        fake = type("datetime32", (), {"datetime": Datetime32, "timezone": datetime.timezone})
        path = self.dir / "lines.csv"
        path.write_bytes("uuid,given_identifier,name,line,time\ng1,VN,VN,今日,1792220400\n"
                         "g1,VN,VN,ずっと先,2200000000\n".encode())
        with mock.patch.object(rs, "datetime", fake):
            report = rs.read_history([path])
            rs.credit_time(report.records, 300.0)
            text = rs.render(report, 5.0, utc=False)
        self.assertEqual(([r.characters for r in report.records], report.bad_times), ([2], 1))
        self.assertIn("1 file, 1 records (1 with a time that is not Unix seconds skipped)", text)

        class DatetimeLocal(datetime.datetime):  # local time fails where UTC does not (the days are local)
            @classmethod
            def fromtimestamp(cls, t, tz=None):
                if t >= 2 ** 31 and tz is None:
                    raise OSError(22, "Invalid argument")
                return datetime.datetime.fromtimestamp(t, tz)
        fake = type("datetime_local", (), {"datetime": DatetimeLocal, "timezone": datetime.timezone})
        with mock.patch.object(rs, "datetime", fake):
            report = rs.read_history([path])
            rs.credit_time(report.records, 300.0)
            rs.render(report, 5.0, utc=False)
        self.assertEqual(([r.characters for r in report.records], report.bad_times), ([2], 1))

    @need_writer
    def test_stored_counts_are_the_files(self):
        """The file's count is the record's, also when this tool would count otherwise (it says how many
        differ); a cell that cannot be a line's count (5,000 digits) is not one, and the line is counted."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS[:3])
        lines = path.read_bytes().split(b"\n")

        def count_cell(line, cell):  # the characters cell, before the line_source cell (the last)
            head, _, source = line.rsplit(b",", 2)
            return head + b"," + cell + b"," + source
        lines[1] = count_cell(lines[1], b"99")
        lines[2] = count_cell(lines[2], b"9" * 5000)
        path.write_bytes(b"\n".join(lines))
        report = rs.read_history([path])
        self.assertEqual([r.characters for r in report.records], [99] + COUNTS[1:3])
        self.assertEqual((report.stored, report.mismatched), (2, 1))
        self.assertEqual([r.source for r in report.records], ["ocr"] * 3)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(rs.main([str(path)]), 0)
        self.assertIn("Characters: 2 records carry their count, 1 counted here; 1 stored counts differ", out.getvalue())
        # nine digits are a count (an absurd one), ten are not; nor are other digits (² ① １２): int() would refuse
        # some, and none is the writer's
        for cell, want in ((b"123456789", 123456789), (b"1234567890", COUNTS[0]), ("²".encode(), COUNTS[0]),
                           ("①".encode(), COUNTS[0]), ("１２".encode(), COUNTS[0])):
            lines[1] = count_cell(lines[1], cell)
            path.write_bytes(b"\n".join(lines))
            self.assertEqual(rs.read_history([path]).records[0].characters, want, cell)

    @need_writer
    def test_line_source_cells(self):
        """A record's line_source cell decides (hook or ocr, in any case, with spaces), also against its engine;
        any other cell is no source, and the engine decides (hook, or vocr: OCR); a record with neither is
        unknown. Each record's reading time goes to its line's source, and the sources' totals are the total."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS, FROM)
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        cells = [(" HOOK ", "vocr"), ("Ocr", "hook"), ("", "hook"), ("unknown", "vocr"), ("?", "ncnn"),
                 ("hook", ""), ("", ""), ("ocr", "vocr"), ("x", "VOCR"), ("hook", "hook")]
        want = ["hook", "ocr", "hook", "ocr", "unknown", "hook", "unknown", "ocr", "ocr", "hook"]
        for row, (source, engine) in zip(rows[1:], cells):
            row[42], row[7] = source, engine
        rows[3] = rows[3][:42]  # a row without the line_source cell (as 0.9.1 wrote), its engine hook
        out = io.StringIO(newline="")
        csv.writer(out, quoting=csv.QUOTE_ALL, lineterminator="\n").writerows(rows)  # the writer's quotes
        path.write_bytes(out.getvalue().encode("utf-8"))
        report = rs.read_history([path])
        self.assertEqual([r.source for r in report.records], want)
        self.assertEqual([r.characters for r in report.records], COUNTS)
        self.assertEqual((report.source_cells, report.source_engines, report.incomplete), (5, 3, 0))
        rs.credit_time(report.records, 300.0)
        total, *_ = rs.summarize(report.records)
        sources = rs.by_source(report.records)
        self.assertEqual(list(sources), ["hook", "ocr", "unknown"])
        for field in ("lines", "characters", "timed_characters", "seconds"):
            self.assertEqual(sum(getattr(t, field) for t in sources.values()), getattr(total, field), field)
        self.assertEqual((sources["unknown"].lines, sources["unknown"].characters), (2, COUNTS[4] + COUNTS[6]))
        text = io.StringIO()
        with redirect_stdout(text):
            self.assertEqual(rs.main([str(path)]), 0)
        self.assertIn("Text source: 5 records name it (line_source), 3 known by their engine (an older Rubi's), "
                      "2 unknown\n", text.getvalue())
        self.assertRegex(text.getvalue(), r"\nUnknown +2 +19 ")

    @need_writer
    def test_records_at_the_same_time(self):
        """Two of Rubi's records with one time (their given_identifiers differ) are two records."""
        path = self.dir / "history.csv"
        write_history(path, [(A, 0, "同じ時の文", 5), (A, 0, "もう一つ", 4)])
        report = rs.read_history([path])
        self.assertEqual(([r.characters for r in report.records], report.duplicates), ([5, 4], 0))

    @need_writer
    def test_a_cut_header(self):
        """A power loss can cut a new file's first append in its header; the writer writes the header only into
        an empty file, so the next record follows the cut on the same line. At every cut, that record and the
        ones after it are read, with Rubi's columns; and the same for a file 0.8.0 or 0.9.1 began, with its
        records (their sources by their engine)."""
        whole = self.dir / "whole.csv"
        write_history(whole, RECORDS[:1])
        header = whole.read_bytes()[:whole.read_bytes().index(b"\n") + 1]
        self.assertEqual(header, rs.HEADER_LINE)
        path = self.dir / "history.csv"
        for cut in range(1, len(header)):
            path.write_bytes(header[:cut])
            write_history(path, RECORDS[1:3], FROM[1:3])
            report = rs.read_history([path])
            got = ([r.characters for r in report.records], [r.source for r in report.records], report.stored,
                   report.source_cells, report.incomplete, report.cut_headers)
            # the record the cut append held: lost, counted
            self.assertEqual(got, (COUNTS[1:3], SOURCES[1:3], 2, 2, 1, 1), cut)
        for fixture, stored in ((FIXTURE, 0), (FIXTURE_091, len(RECORDS) - 1)):
            old = fixture.read_bytes()
            old_header = old[:old.index(b"\n") + 1]
            old_records = old[old.index(b"\n", old.index(b"vjo-")) + 1:]  # without the first: it went with the cut
            sources = SOURCES[1:] if fixture == FIXTURE_091 else ["ocr"] * (len(RECORDS) - 1)
            for cut in range(1, len(old_header)):
                path.write_bytes(old_header[:cut] + old_records)
                report = rs.read_history([path])
                got = ([r.characters for r in report.records], [r.source for r in report.records], report.stored,
                       report.source_engines, report.incomplete, report.cut_headers)
                self.assertEqual(got, (COUNTS[1:], sources, stored, len(RECORDS) - 1, 1, 1), (fixture.name, cut))
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(rs.main([str(path)]), 0)
        self.assertIn("\nA header cut short (a power loss in a new file's first append) in 1 file: read with Rubi's "
                      "columns", out.getvalue())

    @need_writer
    def test_a_cut_header_then_zeros_or_a_cut_record(self):
        """The cut header's two rarer shapes: the zeros a lost write leaves after it (and a first append lost to
        zeros whole), and the next append cut short too, in its record start, before the ones after it."""
        whole = self.dir / "whole.csv"
        write_history(whole, RECORDS[:1])
        header = whole.read_bytes()[:whole.read_bytes().index(b"\n") + 1]
        more = self.dir / "more.csv"
        write_history(more, RECORDS[1:4])
        records = more.read_bytes()[len(header):]
        second = records.index(b'"vita-', 1)  # where the second of them starts
        path = self.dir / "history.csv"
        for cut in list(range(0, len(header), 7)) + [len(header) - 1]:
            for zeros in (1, 300):
                path.write_bytes(header[:cut] + b"\0" * zeros + records)
                report = rs.read_history([path])
                got = ([r.characters for r in report.records], report.incomplete, report.cut_headers)
                self.assertEqual(got, (COUNTS[1:4], 1, 1), (cut, zeros))
            for part in (1, 6, 20, second - 1):  # the next record cut after `part` of its bytes
                path.write_bytes(header[:max(cut, 1)] + records[:part] + records[second:])
                report = rs.read_history([path])
                got = ([r.characters for r in report.records], report.incomplete, report.cut_headers)
                self.assertEqual(got, (COUNTS[2:4], 2, 1), (cut, part))
            for part in (1, 3, 6, 20):  # ... and nothing after it: no record start whole on the line
                path.write_bytes(header[:max(cut, 1)] + records[:part])
                report = rs.read_history([path])
                got = ([r.characters for r in report.records], report.incomplete, report.cut_headers)
                self.assertEqual(got, ([], 2, 1), (cut, part))
            for part in (6, 20):  # ... then a whole file joined (cat), its header glued to the cut record
                path.write_bytes(header[:max(cut, 1)] + records[:part] + more.read_bytes())
                report = rs.read_history([path])
                got = ([r.characters for r in report.records], report.incomplete, report.cut_headers)
                self.assertEqual(got, (COUNTS[1:4], 2, 1), (cut, part, "joined"))
            # two such files joined: the second's cut header is its lost first record
            path.write_bytes(header[:max(cut, 1)] + records[:second] + header[:max(cut, 1)] + records[second:])
            report = rs.read_history([path])
            got = ([r.characters for r in report.records], report.incomplete, report.cut_headers)
            self.assertEqual(got, (COUNTS[1:4], 2, 1), (cut, "two joined"))

    @need_writer
    def test_no_header_and_a_header_with_names_twice(self):
        """A file of records without a header is refused (it is not a cut one: nothing comes before its first
        quote). A header with one of the five names again after Rubi's (a cut header glued to a whole file's by
        cat, or a column added) reads by the first."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS[:3])
        data = path.read_bytes()
        (self.dir / "bare.csv").write_bytes(data[data.index(b"\n") + 1:])
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
            self.assertEqual(rs.main([str(self.dir / "bare.csv")]), 2)
        self.assertIn("no uuid, given_identifier, name, line, time column", err.getvalue())
        header = data[:data.index(b"\n") + 1]
        for cut in range(1, len(header) - 1):  # a first append cut in its header, held nothing else: cat
            for glue in (b"", b"\0" * 9, b"\xef\xbb\xbf"):  # the lost write's zeros; the joined file's BOM
                (self.dir / "glued.csv").write_bytes(header[:cut] + glue + data)
                report = rs.read_history([self.dir / "glued.csv"])
                got = ([r.characters for r in report.records], report.incomplete, report.cut_headers)
                self.assertEqual(got, (COUNTS[:3], 1, 1), (cut, glue))
        (self.dir / "extra.csv").write_bytes(data.replace(b"line_source\n", b"line_source,time\n", 1))
        self.assertIn(b"line_source,time\n", (self.dir / "extra.csv").read_bytes())
        report = rs.read_history([self.dir / "extra.csv"])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), (COUNTS[:3], 0))

    @need_writer
    def test_blank_lines_before_the_header(self):
        path = self.dir / "history.csv"
        write_history(path, RECORDS[:3])
        (self.dir / "blank.csv").write_bytes(b"\n\r\n\xef\xbb\xbf\n" + path.read_bytes())
        report = rs.read_history([self.dir / "blank.csv"])
        self.assertEqual(([r.characters for r in report.records], report.incomplete), (COUNTS[:3], 0))

    @need_writer
    def test_resaved_by_a_spreadsheet(self):
        """Rubi's file re-saved by a spreadsheet (its quotes only where needed, CRLF) is read as plain CSV:
        the same records, counts and sources."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS, FROM)
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"), newline="")))
        out = io.StringIO(newline="")
        csv.writer(out, lineterminator="\r\n").writerows(rows)
        (self.dir / "resaved.csv").write_bytes(out.getvalue().encode("utf-8"))  # as is: CRLF (3.9 has no newline=)
        self.assertNotIn(b'"vita-', (self.dir / "resaved.csv").read_bytes())  # no longer the writer's quotes
        report = rs.read_history([self.dir / "resaved.csv"])
        self.assertEqual(([r.characters for r in report.records], [r.source for r in report.records], report.stored,
                          report.source_cells, report.incomplete), (COUNTS, SOURCES, len(RECORDS), len(RECORDS), 0))

    @need_writer
    def test_copies_count_once(self):
        a, b = self.dir / "a.csv", self.dir / "b.csv"
        write_history(a, RECORDS)
        b.write_bytes(a.read_bytes())
        write_history(b, [(B, 90000, "新しい行", 4)])
        report = rs.read_history([a, b])
        self.assertEqual((len(report.records), report.duplicates), (len(RECORDS) + 1, len(RECORDS)))

    def test_the_release_ships_the_tool_and_its_doc(self):
        """The README tells users to run tools/reading_stats.py: the release zip has it, with the doc
        it points to (vita/CMakeLists.txt's release target)."""
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        for name, dest in (("tools/reading_stats.py", "${STAGE}/tools/"), ("docs/ocr-history.md", "${STAGE}/docs/")):
            copy = [line for line in cmake.splitlines() if f" ${{ROOT}}/{name} " in line]
            self.assertEqual(len(copy), 1, name)
            self.assertTrue(copy[0].lstrip().startswith("COMMAND ${CMAKE_COMMAND} -E copy "), copy[0][:80])
            self.assertTrue(copy[0].rstrip().endswith(f" {dest}"), copy[0][-80:])

    def test_not_a_history(self):
        bad = self.dir / "bad.csv"
        bad.write_text("a,b\n1,2\n")
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
            self.assertEqual(rs.main([str(bad)]), 2)
            bad.write_bytes(b"")
            self.assertEqual(rs.main([str(bad)]), 2)
            self.assertEqual(rs.main([str(self.dir / "missing.csv")]), 2)
            self.assertEqual(rs.main([str(self.dir)]), 2)  # a directory
        self.assertIn("not a Rubi history.csv (no uuid, given_identifier, name, line, time column)", err.getvalue())

    def test_binary_files(self):
        """A picture, a zip, random bytes (a NUL, a lone CR) and a file with no line break exit 2 with a message,
        never a traceback."""
        rnd = random.Random(233)
        files = {"a.jpg": (ROOT / "docs/promo/media/stills/02-7.8.jpg").read_bytes(),
                 "a.png": (ROOT / "docs/img/overlay-icon-candidates.png").read_bytes(),
                 "a.zip": b"PK\x03\x04" + bytes(rnd.randrange(256) for _ in range(4000)),
                 "nul.csv": b"uuid,given_identifier,name,line,time\0\n",
                 "cr.csv": b"uu\rid,x\n",
                 "long.csv": b"x" * 300000}
        for seed in range(20):
            files[f"random{seed}.bin"] = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 3000)))
        for name, content in files.items():
            (self.dir / name).write_bytes(content)
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
                self.assertEqual(rs.main([str(self.dir / name)]), 2, name)
            self.assertRegex(err.getvalue(), r"^reading_stats: .*: (not a Rubi history\.csv|empty file)", name)

    @need_writer
    def test_old_mac_line_endings(self):
        """A copy whose lines end in CR alone (an old Mac format) is refused with a message that says so; a file
        whose first CR-ended line is no ExStatic header is not called one."""
        path = self.dir / "history.csv"
        write_history(path, RECORDS[:3])
        (self.dir / "cr.csv").write_bytes(path.read_bytes().replace(b"\n", b"\r"))
        (self.dir / "cr2.csv").write_bytes(b"uuid,given_identifier,name\rline,time\r1,2\n")
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
            self.assertEqual(rs.main([str(self.dir / "cr.csv")]), 2)
            self.assertEqual(rs.main([str(self.dir / "cr2.csv")]), 2)
        first, second = err.getvalue().splitlines()
        self.assertIn("its lines end in a carriage return alone", first)
        self.assertIn("not a Rubi history.csv (its first line is not CSV", second)

    def test_the_first_lines_read_is_capped(self):
        """A file with no line break (3 MiB here) is not read whole for its header: at most HEADER_READ (1 MiB)
        of it is read, and it is refused."""
        read = []

        class CountingFile(io.BytesIO):
            def readline(self, *args):
                line = super().readline(*args)
                read.append(len(line))
                return line

        class CountingPath(type(rs.Path())):
            def open(self, mode="r", *args, **kwargs):
                return CountingFile(b"x" * (3 << 20))
        with mock.patch.object(rs, "Path", CountingPath), redirect_stdout(io.StringIO()), \
                redirect_stderr(io.StringIO()) as err:
            self.assertEqual(rs.main(["big.csv"]), 2)
        self.assertEqual(rs.HEADER_READ, 1 << 20)
        self.assertLessEqual(sum(read), rs.HEADER_READ)
        self.assertIn("not a Rubi history.csv", err.getvalue())

    def test_a_console_without_the_names_characters(self):
        """A console that cannot show a game's name (here ASCII) shows "?" for its characters, and the report is
        made (exit 0)."""
        env = dict(os.environ, PYTHONIOENCODING="ascii")
        r = subprocess.run([sys.executable, str(ROOT / "tools/reading_stats.py"), str(FIXTURE)], env=env,
                           capture_output=True, check=False)
        self.assertEqual((r.returncode, r.stderr), (0, b""))
        self.assertIn(b"\n?????? (vita-PROMO0001)", r.stdout)

    def test_read_errors(self):
        """A read that fails (a failing memory card or reader: EIO), at the header or later, exits 2 with the
        error, never a traceback."""
        real_path = rs.Path

        class FailingFile(io.BytesIO):
            def __init__(self, data, good):
                super().__init__(data)
                self.good = good

            def readline(self, *args):
                if self.good <= 0:
                    raise OSError(5, "Input/output error")
                self.good -= 1
                return super().readline(*args)

            def __iter__(self):
                return self

            def __next__(self):
                line = self.readline()
                if not line:
                    raise StopIteration
                return line

        for good in (0, 1, 3):
            class FailingPath(type(real_path())):
                def open(self, mode="r", *args, **kwargs):
                    return FailingFile(real_path(self).read_bytes(), good)
            with mock.patch.object(rs, "Path", FailingPath), redirect_stdout(io.StringIO()), \
                    redirect_stderr(io.StringIO()) as err:
                self.assertEqual(rs.main([str(FIXTURE)]), 2, good)
            self.assertIn("Input/output error", err.getvalue())


class Time(unittest.TestCase):
    def report(self, minutes=5.0):
        report = rs.read_history([FIXTURE])
        rs.credit_time(report.records, minutes * 60)
        return report

    def test_break_boundary(self):
        """A gap of exactly --break-minutes is reading time; a microsecond more is a break."""
        records = [rs.Record(game="g", name="g", time=t, characters=1, stored=True) for t in (0.0, 300.0, 600.000001)]
        rs.credit_time(records, 300.0)
        self.assertEqual([r.seconds for r in records], [300.0, None, None])

    def test_local_days(self):
        """Days in this computer's time zone unless --utc: at UTC-10 (TZ=HST10, a POSIX zone that needs no
        zoneinfo files) the fixture's two days are 10-16 and 10-17, in UTC 10-17 and 10-18."""
        env = dict(os.environ, TZ="HST10")
        out = subprocess.run([sys.executable, str(ROOT / "tools/reading_stats.py"), str(FIXTURE)], env=env,
                             capture_output=True, text=True, check=True).stdout
        self.assertRegex(out, r"By day \(local time\)\n.*\n2026-10-16 +8 +64 .*\n2026-10-17 +2 +11 ")
        out = subprocess.run([sys.executable, str(ROOT / "tools/reading_stats.py"), "--utc", str(FIXTURE)], env=env,
                             capture_output=True, text=True, check=True).stdout
        self.assertRegex(out, r"By day \(UTC\)\n.*\n2026-10-17 +8 +64 .*\n2026-10-18 +2 +11 ")

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
        self.assertIn("Text source: 0 records name it (line_source), 10 known by their engine (an older Rubi's), "
                      "0 unknown\n", text)
        self.assertRegex(text, r"\nAll games +10 +75 +0 h 01 m +2,082\n")
        # 0.8.0 had no native hooks; no line's source is unknown, so there is no row for it
        self.assertRegex(text, r"\nBy text source\n.*\nNative text hook +0 +0 +0 h 00 m +-\nOCR +10 +75 +0 h 01 m "
                               r"+2,082\n\nBy game\n")
        self.assertRegex(text, r"\nEmber Garden \(vita-PROMO0004\) +6 +44 +0 h 01 m +1,883\n")
        self.assertRegex(text, r"\n夕凪ステップ \(vita-PROMO0001\) +4 +31 +0 h 00 m +2,800\n")
        self.assertRegex(text, r"By day \(UTC\)\n.*\n2026-10-17 +8 +64 .*\n2026-10-18 +2 +11 +0 h 00 m +1,800\n")
        for _, _, line, _ in RECORDS:  # no dialogue in the report
            self.assertNotIn(line, text)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            rs.main(["--break-minutes", "0", str(FIXTURE)])

    def test_report_by_source(self):
        """0.9.1's file: Ember Garden's lines from its hook after the first (FROM). Each line's reading time
        is its source's: hook 18 + 15 + 20 s for 2 + 11 + 9 characters (the fourth, before the change of game,
        gets none), OCR 12 + 10 + 8 s for 12 + 10 + 4; the sources' totals are the total."""
        report = rs.read_history([FIXTURE_091])
        rs.credit_time(report.records, 300.0)
        sources = rs.by_source(report.records)
        self.assertEqual(list(sources), ["hook", "ocr"])
        hook, ocr = sources["hook"], sources["ocr"]
        self.assertEqual((hook.lines, hook.characters, hook.timed_characters, hook.seconds), (4, 32, 22, 53))
        self.assertEqual((ocr.lines, ocr.characters, ocr.timed_characters, ocr.seconds), (6, 43, 26, 30))
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(rs.main(["--utc", str(FIXTURE_091)]), 0)
        text = out.getvalue()
        self.assertIn("Characters: 10 records carry their count, 0 counted here\n", text)
        self.assertIn("Text source: 0 records name it (line_source), 10 known by their engine (an older Rubi's), "
                      "0 unknown\n", text)
        self.assertRegex(text, r"\nAll games +10 +75 +0 h 01 m +2,082\n")
        self.assertRegex(text, r"\nBy text source\n.*\nNative text hook +4 +32 +0 h 01 m +1,494\nOCR +6 +43 "
                               r"+0 h 00 m +3,120\n")



class ShellTotal(unittest.TestCase):
    """The overlay's "Total characters read" (shell/history.c, as the Shell counts a file a 24 KiB window at a
    time) is this tool's total of the characters for every file the writer writes, began by any Rubi; for a
    record a power loss cut short, each of the two counts it or the record after it at most once."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="vjo-reading-total-")
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def tools_total(self, path):
        return sum(r.characters for r in rs.read_history([path]).records)

    @need_writer
    def test_every_rubis_files(self):
        new, began_080, began_091 = self.dir / "new.csv", self.dir / "080.csv", self.dir / "091.csv"
        write_history(new, RECORDS, FROM)
        began_080.write_bytes(FIXTURE.read_bytes())
        began_091.write_bytes(FIXTURE_091.read_bytes())
        more = [(A, 172800, "三日目です。", 5), (B, 172810, "ABCは3文字", 7)]
        write_history(began_080, more, [9, 0])
        write_history(began_091, more, [0, 1])
        for path in (FIXTURE, FIXTURE_091, new, began_080, began_091):
            chars, covered, windows = shell_total(path)
            self.assertEqual(chars, self.tools_total(path), path.name)
            self.assertEqual((covered, windows), (path.stat().st_size, 1), path.name)
        self.assertEqual(shell_total(FIXTURE)[0], sum(COUNTS))

    @need_writer
    def test_a_long_file_in_windows(self):
        """3,000 records of 1 to 300 characters (some 1.6 MB, 70 windows): the same total as this tool's."""
        rnd = random.Random(20261023)
        words = ["また明日", "一緒に", "見に行こう", "、", "。", "「", "」", "AB12", "𠮟", "😀", "\"引用\"", ",", "\n", "ｶﾀｶﾅ"]
        records = []
        for i in range(3000):
            text = "".join(rnd.choice(words) for _ in range(rnd.randrange(1, 80))) + str(i)
            records.append(((A if i % 3 else B), i * 7, text, None))
        path = self.dir / "history.csv"
        self.assertEqual(write_history(path, records), [1] * len(records))
        chars, covered, windows = shell_total(path)
        self.assertEqual(chars, self.tools_total(path))
        self.assertEqual(covered, path.stat().st_size)
        self.assertGreater(windows, 50)

    @need_writer
    def test_overlay_time_at_15_seconds(self):
        """Independent Python reference, including many scan-window boundaries and older CSV versions."""
        for path in (FIXTURE, FIXTURE_091):
            report = rs.read_history([path])
            rs.credit_time(report.records, 15)
            want = rs.summarize(report.records)[0]
            out = subprocess.check_output([WRITER, "--stats", str(path)], text=True)
            chars, us, timed = map(int, out.split())
            self.assertEqual(chars, want.characters)
            self.assertAlmostEqual(us / 1e6, want.seconds, places=5)
            self.assertEqual(timed, want.timed_characters)
        rnd = random.Random(272)
        records, sec = [], 0
        for i in range(2000):
            sec += rnd.choice([0.125, 1, 5, 15, 15.125, 80])
            records.append((A if i % 7 else B, sec, f'{i}猫\n"台詞", ABC', None))
        path = self.dir / "timed.csv"
        write_history(path, records)
        report = rs.read_history([path])
        rs.credit_time(report.records, 15)
        want = rs.summarize(report.records)[0]
        chars, us, timed = map(int, subprocess.check_output([WRITER, "--stats", str(path)], text=True).split())
        self.assertEqual((chars, us, timed), (want.characters, round(want.seconds * 1e6), want.timed_characters))

    @need_writer
    def test_a_cut_record_and_the_one_after_it(self):
        """Every cut of a record, with the next append right after it: the records before the cut always count,
        and the cut one and the one after it each at most once, with their own characters."""
        path, nxt = self.dir / "history.csv", self.dir / "next.csv"
        write_history(path, RECORDS[:-1], FROM[:-1])
        data = path.read_bytes()
        last = data.rindex(b"\n", 0, len(data) - 1) + 1
        write_history(nxt, [RECORDS[-1]], [1])
        after = nxt.read_bytes()
        after = after[after.index(b"\n") + 1:]
        before = sum(COUNTS[:-2])
        allowed = {before + a + b for a in (0, COUNTS[-2]) for b in (0, COUNTS[-1])}
        for cut in range(last + 1, len(data)):
            (self.dir / "cut.csv").write_bytes(data[:cut] + after)
            chars, covered, _ = shell_total(self.dir / "cut.csv")
            self.assertIn(chars, allowed, cut)
            self.assertIn(self.tools_total(self.dir / "cut.csv"), allowed, cut)
            self.assertEqual(covered, cut + len(after))

if __name__ == "__main__":
    unittest.main()
