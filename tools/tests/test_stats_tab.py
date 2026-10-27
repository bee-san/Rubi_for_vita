"""The Rubi app's Stats tab (settings/stats_page.c, core/stats.c; #312) gives tools/reading_stats.py's numbers on the
same history.csv: each game's characters, reading time and timed characters, every day's (UTC here: the tab's days
are the Vita's local ones, VJO_STATS_TOOL's --tz), and all games', for both reading_time rules. The tab counts with
the Shell's own rule (core/history_lines.c vjo_reading_time_add), so these are the Triangle view's numbers too
(host/tests/test_stats.c checks that against the Shell's count itself).

Files: 0.8.0's (no characters column) and 0.9.1's (no line_source) fixtures, 1,200 random records of three games (twice)
written by the Shell's real writer (VJO_HISTORY_WRITER), with gaps from 0.1 s to a day, and the simulated timelines
of tools/reading_time_sim.py (host/fixtures/reading_time). VJO_STATS_TOOL is the host build's vjo-stats (set by
ctest)."""
import datetime
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import reading_stats as rs  # noqa: E402
import reading_time_sim as sim  # noqa: E402

TOOL = os.environ.get("VJO_STATS_TOOL")
WRITER = os.environ.get("VJO_HISTORY_WRITER")
need_tools = unittest.skipUnless(TOOL and WRITER, "VJO_STATS_TOOL and VJO_HISTORY_WRITER (the host build's vjo-stats "
                                                  "and vjo-history-writer, set by ctest) are not set")
FIXTURES = [ROOT / "tools/tests/fixtures/history-0.8.0.csv", ROOT / "tools/tests/fixtures/history-0.9.1.csv"]
TIMELINES = sorted((ROOT / "host/fixtures/reading_time").glob("*.tsv"))
T0 = 1792220400250123  # 2026-10-17T07:00:00.250123Z
GAMES = [("PCSG00415", "CLANNAD"), ("PCSG00062", "TIME TRAVELERS"), ("PCSG00206", "グリザイアの果実")]


def write(path, records):
    """records (title ID, name, microseconds, text) appended by the Shell's writer."""
    data = b"".join(f"{tid}\x1f{name}\x1f{us}\x1f{text}\0".encode() for tid, name, us, text in records)
    subprocess.run([WRITER, str(path)], input=data, capture_output=True, check=True)


def tab(path, rule):
    """The tab's counts of path (vjo-stats: days in UTC)."""
    out = subprocess.run([TOOL, "--tz", "0", "--rule", rule, str(path)], capture_output=True, check=True, text=True)
    return json.loads(out.stdout)


def reference(path, rule):
    """tools/reading_stats.py's records of path, with their credited time (seconds), as its report counts them."""
    report = rs.read_history([path])
    rs.credit_time(report.records, None if rule == "adaptive" else 15.0)
    return report.records


def us_of(records):
    return sum(round(r.seconds * 1e6) for r in records if r.seconds is not None)


def timed_of(records):
    return sum(r.characters for r in records if r.seconds is not None)


@need_tools
class SameAsReadingStats(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def random_file(self, seed, n=2000):
        rnd = random.Random(seed)
        records, us, game = [], T0, 0
        for i in range(n):
            if rnd.random() < 0.2:
                game = rnd.randrange(len(GAMES))
            us += int(rnd.choice([0, 0.1, 0.9, 1, 3, 5, 15, 15.000001, 42, 80, 150, 299, 301, 1800, 7200, 86400]) * 1e6)
            us += rnd.choice([0, 0, rnd.randrange(1000)])  # (two records at one time too)
            text = f'{i}猫\n"台詞", ABC{"あ" * rnd.randrange(40)}' if rnd.random() < 0.5 else "「" + "漢" * (1 + i % 60) + "」"
            records.append((*GAMES[game], us, text))
        path = self.dir / f"random-{seed}.csv"
        write(path, records)
        return path

    def timeline_file(self, tsv):
        head, recs = sim.read_fixture(tsv)
        path = self.dir / (tsv.stem + ".csv")
        # the text a timeline's line ID names, one text per ID (as host/tests/test_shell_history.c makes them)
        write(path, [("PCSG00415", "CLANNAD", us, "「" + "あ" * chars + "」" + str(n)) for n, (us, chars, _) in
                     enumerate(recs)])
        return path

    def check(self, path, rule):
        got = tab(path, rule)
        records = reference(path, rule)
        games = {g["uuid"]: g for g in got["games"]}
        want_games = {}
        for r in records:
            want_games.setdefault(r.game, []).append(r)
        self.assertEqual(set(games), set(want_games), path.name)
        for uuid, recs in want_games.items():
            g = games[uuid]
            self.assertEqual(g["chars"], sum(r.characters for r in recs), (path.name, rule, uuid))
            self.assertEqual(g["lines"], len(recs), (path.name, rule, uuid))
            self.assertEqual(g["timed_chars"], timed_of(recs), (path.name, rule, uuid))
            if rule == "adaptive":
                self.assertEqual(g["active_us"], us_of(recs), (path.name, rule, uuid))
            else:  # (the tool's 15 s compares seconds as floats: at most a microsecond a record)
                self.assertLessEqual(abs(g["active_us"] - us_of(recs)), len(recs), (path.name, rule, uuid))
        days = {}
        for r in records:
            days.setdefault(datetime.datetime.fromtimestamp(r.time, datetime.timezone.utc).date().isoformat(),
                            []).append(r)
        got_days = {d["day"]: d for d in got["days"]}
        self.assertEqual(set(got_days), set(days), path.name)
        for day, recs in days.items():
            d = got_days[day]
            self.assertEqual((d["chars"], d["timed_chars"]), (sum(r.characters for r in recs), timed_of(recs)),
                             (path.name, rule, day))
            if rule == "adaptive":
                self.assertEqual(d["active_us"], us_of(recs), (path.name, rule, day))
        self.assertEqual(got["all"]["chars"], sum(r.characters for r in records))
        self.assertEqual(got["all"]["timed_chars"], timed_of(records))
        self.assertEqual(got["all"]["lines"], len(records))
        return got

    def test_the_fixtures_of_older_builds(self):
        """0.8.0's file has no characters column, 0.9.1's no line_source: the tab counts their lines itself."""
        for path in FIXTURES:
            for rule in ("adaptive", "15s"):
                got = self.check(path, rule)
                self.assertEqual(got["damaged"], 0)

    def test_random_files_of_three_games(self):
        for seed in (1, 2):
            path = self.random_file(seed, 1200)
            for rule in ("adaptive", "15s"):
                self.check(path, rule)

    def test_the_simulated_timelines(self):
        self.assertTrue(TIMELINES)
        for tsv in TIMELINES:
            path = self.timeline_file(tsv)
            self.check(path, "adaptive")

    def test_any_window_counts_the_same(self):
        path = self.random_file(9, 600)
        whole = subprocess.run([TOOL, "--tz", "540", str(path)], capture_output=True, check=True, text=True).stdout
        for window in ("2048", "4097", "65536"):
            out = subprocess.run([TOOL, "--tz", "540", "--window", window, str(path)], capture_output=True,
                                 check=True, text=True).stdout
            self.assertEqual(out, whole, window)


if __name__ == "__main__":
    unittest.main()
