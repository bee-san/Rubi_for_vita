"""tools/wait_hint_replay.py (#358): the wait hint's rule replayed on a debug log and on spinner-bench rows, synthetic."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import wait_hint_replay as w  # noqa: E402

LOG = """Vita JP Overlay shell started (kernel API 19)
job start (auto: screen quiet)
job 0 done rc=-120 in 2000 ms
job start (auto: screen quiet)
job 0 done rc=-120 in 2000 ms
Vita JP Overlay shell started (kernel API 24)
job start (auto: screen quiet)
job 0 done rc=-120 in 2500 ms
job start (auto: screen quiet)
job 0 done rc=0 in 1200 ms
open: no background result (stable 0, quiet 0)
job start (overlay opened)
job 0 done rc=0 in 1400 ms
lookup cursor at 3: 2 chars, rc=0, 39 ms
job 0 done rc=0 in 55 ms
overlay closed: 100 frames, 0 word moves, stick 0, slowest frame 1 ms
job start (auto: screen settled)
job 0 done rc=-120 in 2400 ms
job start (auto: screen settled)
job 0 done rc=-118 in 300 ms
job start (the screen changed during the last one)
job 0 done rc=-120 in 3100 ms
open: background job still running (stable 0, quiet 0)
job start (auto: screen quiet)
job 0 done rc=-120 in 2000 ms
job start (the screen changed during the last one)
job 0 done rc=0 in 900 ms
overlay closed: 50 frames, 0 word moves, stick 0, slowest frame 1 ms
"""


def row(shown, show_ms, jobs, press=3000):
    return {"press_ms": press, "shown": shown, "show_ms": show_ms, "text": "never read",
            "job_log": [{"kind": k, "at_ms": t, "ms": 1000, "rc": rc, "cancelled": int(rc == -118)} for k, t, rc in jobs]}


class WaitHintReplay(unittest.TestCase):
    def logs(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "log.txt")
            with open(p, "w", encoding="utf-8") as f:
                f.write(LOG)
            return w.read_logs([p])

    def test_logs(self):
        jobs, opens = self.logs()
        # API 19's two jobs are left out; the lookup's "done" has no "job start" and is no job
        self.assertEqual([j["rc"] for j in jobs if j], [-120, 0, 0, -120, -118, -120, -120, 0])
        self.assertEqual(len(opens), 2)  # the press that started a job, and the one that waited for a running job
        one, two, three = (w.log_replay(jobs, opens, n) for n in (1, 2, 3))
        # runs: one torn job, then (a cancelled job between them) three in a row
        self.assertEqual((one["torn_jobs"], one["runs"], one["runs_by_length"]), (4, 2, {"1": 1, "3": 1}))
        self.assertEqual((one["hint_shown"], two["hint_shown"], three["hint_shown"]), (2, 1, 1))
        self.assertEqual(two["until_the_next_result_ms"], [900, 900, 900])
        self.assertEqual(one["until_the_next_result_ms"], [1050.0, 900, 1200])
        # the second opening waited through a torn job (the third of its run: the hint, from N = 3 down)
        self.assertEqual((one["waits_with_a_torn_job"], one["waits_hint_shown"], three["waits_hint_shown"]), (1, 1, 0))

    def test_bench(self):
        rows = [
            # never shown: Automatically's torn job before the press, then the press's torn tries
            row(0, -1, [("background", 2000, -120), ("press", 4500, -120), ("press", 6000, -120), ("press", 8000, -120)]),
            # shown after a wait: two torn tries, then a read at 5.5 s
            row(1, 5500, [("press", 3000, -120), ("press", 4400, -120), ("press", 6000, 0)]),
            # shown at once (the background result): no wait
            row(1, 0, [("background", 1000, 0)]),
            # a cancelled try between torn ones neither counts nor ends the run
            row(0, -1, [("press", 3000, -120), ("press", 4000, -118), ("press", 4500, -120), ("press", 7000, -120)]),
        ]
        two = w.bench_replay(rows, 2)
        self.assertEqual((two["never_shown"], two["never_shown_torn"], two["hint_shown"]), (2, 2, 2))
        self.assertEqual(two["after_press_ms"], [3500.0, 3000, 4000])  # 6000 - 3000 and 7000 - 3000
        self.assertEqual((two["shown_after_a_wait"], two["hint_first"], two["hint_first_under_1s"]), (1, 1, 0))
        self.assertEqual(two["hint_before_text_ms"], [2500, 2500, 2500])  # the third try at 3 s (after the press), text 5.5 s
        one = w.bench_replay(rows, 1)
        self.assertEqual(one["after_press_ms"], [1250.0, 1000, 1500])
        four = w.bench_replay(rows, 4)
        self.assertEqual((four["hint_shown"], four["hint_first"]), (0, 0))

    def test_cli_writes_counts_only(self):
        with tempfile.TemporaryDirectory() as d:
            log, bench, out = (os.path.join(d, n) for n in ("log.txt", "bench.jsonl", "out.json"))
            with open(log, "w", encoding="utf-8") as f:
                f.write(LOG)
            with open(bench, "w", encoding="utf-8") as f:
                f.write(json.dumps(row(0, -1, [("press", 3000, -120), ("press", 4500, -120), ("press", 6000, -120)])) + "\n")
            self.assertEqual(w.main(["--logs", log, "--bench", bench, "--n", "2", "-o", out]), 0)
            with open(out, encoding="utf-8") as f:
                text = f.read()
            got = json.loads(text)
            self.assertEqual(sorted(got), ["bench", "logs", "n", "tool"])
            self.assertEqual(got["bench"]["2"]["after_press_ms"], [3000, 3000, 3000])
            self.assertNotIn("never read", text)  # the bench's texts stay out


if __name__ == "__main__":
    unittest.main()
