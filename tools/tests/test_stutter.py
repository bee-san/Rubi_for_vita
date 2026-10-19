"""docs/stutter.md: the analyser's late frames by strip and by what started the job
(tools/analyse_selftest.py stutter_summary, stutter_lines) and the replay model's input and arithmetic
(tools/stutter_model.py). Synthetic log lines in the debug build's format; no console log is used."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import analyse_selftest  # noqa: E402
import stutter_model  # noqa: E402


def job_lines(trigger, h, strips, passes, wait_us, frames, slow, before_frames=100, before_slow=0, refresh=False,
              rc=1, sum_us=None):
    """One job's lines as shell/worker.c logs them (the parts the analysis reads)."""
    sum_us = sum_us if sum_us is not None else frames * 16667
    return [f"job start ({trigger}){', refresh' if refresh else ''}",
            f"game vocr rc=0 VOCR5_N_w8.vocr: 852x{h} at 50,409 of 960x544 fmt 0x00000000, lines=1 USER workspace=1 "
            "weights=resident resident=1, find=1 ms load=0 ms recognize=100 ms frames=40 io=0x00000000 stat=0x00000000 "
            "cleanup=0",
            f"game vocr capture: hook into USER, 61440 B block, 64-row strips, {strips} strips in 300 ms, 0 rows changed, "
            "sum 0x00000001",
            f"gstep job settle kind=- arg={passes} rc=0x{rc:08X} bytes=0 sum=0x00000000 main=0 cdram=0 phycont=0 "
            f"us={wait_us}",
            f"gstep job pace kind=- arg={frames} rc=0x00008000 bytes={sum_us} sum=0x{slow:08X} main={before_frames * 16667} "
            f"cdram=17000 phycont={before_slow} us={before_frames}",
            "job 0 done rc=0 in 500 ms, arena peak 1"]


def log(*jobs):
    lines = ["Rubi shell started (kernel API 29)",
             "game PCSG00415 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto"]
    for j in jobs:
        lines += j
    return "\n".join(lines) + "\n"


class StutterSummary(unittest.TestCase):
    def test_late_frames_by_strip_and_trigger(self):
        text = log(job_lines("auto: screen settled", 135, 30, 9, 500000, 40, 6),
                   job_lines("overlay opened", 135, 20, 6, 400000, 25, 5, before_slow=1),
                   job_lines("auto: screen settled", 135, 12, 3, 160000, 15, 1, refresh=True))
        su = analyse_selftest.summarise_log(text)["stutter"]
        self.assertEqual((su["jobs"], su["frames"], su["slow"], su["strips"]), (3, 80, 12, 62))
        self.assertEqual((su["before_frames"], su["before_slow"]), (300, 1))
        # the wait's strips: passes x ceil(135 / 64) = 3, each job's at most its own strips
        self.assertEqual(su["wait_strips"], 27 + 18 + 9)
        self.assertEqual(su["wait_passes"], 18)
        self.assertEqual(su["wait_us"], 1060000)
        self.assertEqual({k: (v["jobs"], v["frames"], v["slow"], v["strips"]) for k, v in su["kinds"].items()},
                         {"auto": (1, 40, 6, 30), "overlay": (1, 25, 5, 20), "refresh": (1, 15, 1, 12)})
        self.assertEqual([r["kind"] for r in su["jobs_rows"]], ["auto", "overlay", "refresh"])
        self.assertEqual(su["jobs_rows"][0]["ms"], 40 * 16667 // 1000)
        lines = analyse_selftest.stutter_lines({"stutter": su})
        self.assertEqual(lines[0],
                         "  late frames during OCR jobs (docs/stutter.md): 12 of 80 (15.0%) in 3 job(s), against 1 of 300 "
                         "(0.3%) between them; 62 strips, the frames whose display call copied rows of the region, 0.19 "
                         "late frames a strip; the stable-text wait read 54 of the strips (87.1%), 17.0 passes a second of "
                         "waiting")
        self.assertEqual(lines[1], "    by what started the job: Automatically's 6 of 40 (15.0%) in 1, 30 strips; the "
                                   "overlay's (presses) 5 of 25 (20.0%) in 1, 20 strips; refreshes 1 of 15 (6.7%) in 1, 12 "
                                   "strips")

    def test_no_pace_step_no_summary(self):
        """A normal build's log (no pace step) or an API 21 one: nothing to attribute, no line."""
        text = log([l for l in job_lines("auto: screen settled", 135, 30, 9, 500000, 40, 6) if "pace" not in l])
        lg = analyse_selftest.summarise_log(text)
        self.assertIsNone(lg["stutter"])
        self.assertEqual(analyse_selftest.stutter_lines(lg), [])

    def test_a_job_without_a_capture_line_is_counted_and_said(self):
        lines = [l for l in job_lines("auto: screen settled", 135, 30, 9, 500000, 40, 6) if "capture" not in l]
        su = analyse_selftest.summarise_log(log(lines))["stutter"]
        self.assertEqual((su["jobs"], su["strips"], su["no_strips"], su["wait_strips"]), (1, 0, 1, 0))
        self.assertIn("no strip counted", analyse_selftest.stutter_lines({"stutter": su})[0])
        self.assertIn("1 job(s) without a capture line", analyse_selftest.stutter_lines({"stutter": su})[0])


class StutterModel(unittest.TestCase):
    def row(self, **kw):
        r = {"kind": "auto", "w": 852, "h": 135, "strip_rows": 64, "strips": 60, "passes": 18, "wait_us": 1050000,
             "wait_rc": 1, "frames": 70, "slow": 10, "ms": 1170}
        r.update(kw)
        return r

    def test_replay_input(self):
        # a held wait: its rows changed until the start of its last changed pass, then held for settle_us
        w, h, fps, anim = stutter_model.replay_input(self.row())
        self.assertEqual((w, h, fps), (852, 135, 60))
        per_pass_ms = 3 * 1000 / 60
        self.assertEqual(anim, int((1050000 - (1 + 3) * per_pass_ms * 1000) / 1000))
        # the limit: the rows changed for the whole wait
        self.assertEqual(stutter_model.replay_input(self.row(wait_rc=2, wait_us=2000000))[3], 2000)
        # a press's job, the self-test's: the pace does not apply (shell/worker.c)
        self.assertIsNone(stutter_model.replay_input(self.row(kind="overlay")))
        self.assertIsNone(stutter_model.replay_input(self.row(kind="self-test")))
        self.assertIsNotNone(stutter_model.replay_input(self.row(kind="refresh")))
        # only the check after the finder: kept as measured
        self.assertIsNone(stutter_model.replay_input(self.row(passes=2, wait_us=200000)))
        # nothing to replay without a wait or a capture line
        self.assertIsNone(stutter_model.replay_input(self.row(passes=0)))
        self.assertIsNone(stutter_model.replay_input(self.row(strips=None)))
        # the frame rate is the job's own, at least 20 and at most 60
        self.assertEqual(stutter_model.replay_input(self.row(frames=35, ms=1170))[2], 30)
        self.assertEqual(stutter_model.replay_input(self.row(frames=1, ms=1170))[2], 20)

    def test_the_model_counts_only_replayed_wait_strips(self):
        """With a stand-in bench: the wait's strips are replaced by the replay's, the others kept, at the job's own
        late frames a strip; jobs it does not replay are kept as measured."""
        with tempfile.TemporaryDirectory() as d:
            bench = Path(d) / "bench.py"
            bench.write_text("import sys, json\n"
                             "for line in sys.stdin:\n"
                             "    w, h, fps, first, anim, pace = map(int, line.split())\n"
                             "    passes = 18 if pace == 0 else 7\n"
                             "    print(json.dumps({'settle_passes': passes, 'ms': 1000 + (20 if pace else 0)}))\n")
            wrapper = Path(d) / "bench"
            wrapper.write_text(f"#!/bin/sh\nexec {sys.executable} {bench} \"$@\"\n")
            wrapper.chmod(0o755)
            rows = [self.row(), self.row(kind="overlay")]
            per, checks = stutter_model.model(rows, wrapper, 150)
        self.assertEqual(checks, [(18, 18)])
        self.assertEqual(per[0]["strips_paced"], (60 - 18 * 3) + 7 * 3)
        self.assertAlmostEqual(per[0]["slow_paced"], 10 / 60 * 27)
        self.assertEqual(per[0]["frames_paced"], 70 + round(20 * 60 / 1000))
        self.assertFalse(per[1]["replayed"])
        self.assertEqual((per[1]["strips_paced"], per[1]["slow_paced"]), (60, 10))
        s = stutter_model.summary(per, checks, 150)
        self.assertEqual((s["strips"], s["strips_paced"], s["replayed"]), (120, 87, 1))
        self.assertEqual(s["replay_check"]["median_error"], 0)


@unittest.skipUnless(Path(os.environ.get("VJO_SETTLE_BENCH", ROOT / "build/host/vjo-settle-bench")).exists(),
                     "no host build of vjo-settle-bench")
class SettleBenchReplay(unittest.TestCase):
    """The bench's replay mode on the core's own wait: paced, a job reads fewer strips and reads the line whole."""

    def test_replay_paced_reads_fewer_strips(self):
        bench = os.environ.get("VJO_SETTLE_BENCH", str(ROOT / "build/host/vjo-settle-bench"))
        out = subprocess.run([bench, "--replay", "150"], input="852 135 60 1 900 0\n852 135 60 1 900 150\n",
                             capture_output=True, text=True, timeout=120, check=True).stdout.splitlines()
        a, b = (json.loads(x) for x in out)
        self.assertEqual((a["settled"], b["settled"], a["rc"], b["rc"]), (1, 1, a["rc"], a["rc"]))
        self.assertLess(b["strips"], a["strips"])
        self.assertGreater(b["settle_naps"], 0)
        self.assertEqual(a["settle_naps"], 0)
        self.assertGreaterEqual(b["ms"], a["ms"])  # never sooner (core/vocr_ocr.c settle_wait)
        self.assertEqual(b["chars_at_published_capture"], b["of"])


if __name__ == "__main__":
    unittest.main()
