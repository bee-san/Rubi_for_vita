"""tools/analyse_timeline.py on a synthetic log (made-up numbers in the console's formats; no game text)."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import analyse_timeline as at


def gstep(name, us, kind="-", arg="0", rc=0, nbytes=0, sum_=0, who="job"):
    return (f"gstep {who} {name} kind={kind} arg={arg} rc=0x{rc & 0xFFFFFFFF:08X} bytes={nbytes} sum=0x{sum_:08X} "
            f"main=88080384 cdram=12582912 phycont=27262976 us={us}")


def job_line(rc=0, model="VOCR3_H15_w8.vocr", fallback=False, lines=1, weights="read", frames=77):
    return (f"game vocr rc={rc} {model}{' (fallback)' if fallback else ''}: 883x112 at 46,416 of 960x544 "
            f"fmt 0x00000000, lines={lines} USER workspace=538624 weights={weights} resident=1159168, find=134 ms "
            f"load=425 ms recognize=429 ms frames={frames} io=0x00000000 stat=0x00000000 cleanup=0")


# test.2's shape: a self-test stage in the user's job (a tear, a late weights allocation), then a
# later job with this build's done line (exchanges and log appends) and a warm-up job.
LOG = "\n".join([
    "Vita JP Overlay shell started (kernel API 20)",
    "game PCSG00415 started: dictionary Local dictionaries (key MISSING), trigger l+r, subtitles select+r, ocr_mode auto",
    "job start (auto: screen settled)",
    "stage textbox begin t=47328 flags=0x0 model=VOCR3_H15_w8.vocr fallback=VOCR3_FL10_w8.vocr",
    job_line(),
    "game vocr capture: hook into USER, 61440 B block, 64-row strips, 57 strips in 1248 ms, 13 rows changed, sum 0x1",
    gstep("mem", 17), gstep("check", 81, kind="10"),
    gstep("alloc", 382, kind="user", arg="capture", rc=0x400104C9, nbytes=61440),
    gstep("alloc", 992, kind="user", arg="workspace", rc=0x400104CB, nbytes=544768),
    gstep("pass", 44254, kind="hook", arg="2", nbytes=98896),
    gstep("sig", 52175, kind="hook"),
    gstep("alloc", 1376964, kind="user", arg="weights", rc=0x400104CD, nbytes=1159168),
    gstep("tear", 2298159, arg="13", rc=2),
    gstep("find", 134479, arg="1"),
    gstep("settle", 1129125, arg="24", rc=1, nbytes=260),
    gstep("load", 425710, nbytes=1159168),
    gstep("recognize", 429939, arg="1", nbytes=77),
    gstep("lm", 1079, kind="2", nbytes=0x40000001),
    gstep("free", 2298318, arg="workspace", nbytes=544768),
    gstep("free", 2298385, arg="capture", nbytes=61440),
    "game vocr text 0: SENTINEL-TEXT-1",
    ("stage textbox end rc=0 t=52483 ms=4174 region=883x112+46+416 fb=960x544/1024/0x00000000 "
     "model=VOCR3_H15_w8.vocr fallback=0 lines=1 recognized=1 frames=77"),
    "job 0 done rc=0 in 12271 ms, arena peak 53552, dictionary page 1/1",
    "game PCSG00415 started: dictionary Local dictionaries (key MISSING), trigger l+r, subtitles select+r, ocr_mode auto",
    "job start (auto: screen settled)",
    job_line(lines=0, weights="read", frames=0),
    gstep("mem", 18), gstep("check", 80, kind="10"),
    gstep("alloc", 200, kind="user", arg="capture", rc=0x41, nbytes=61440),
    gstep("alloc", 700, kind="user", arg="workspace", rc=0x42, nbytes=544768),
    gstep("pass", 52000, kind="hook", arg="3", nbytes=115020),
    gstep("alloc", 120000, kind="user", arg="weights", rc=0x43, nbytes=1159168),
    gstep("find", 106000, arg="0"),
    gstep("load", 223000, arg="1", kind="1", nbytes=1159168),
    gstep("lm", 1, kind="2", nbytes=0x80000000, sum_=201000),
    gstep("cascade", 1, kind="0", arg="0", nbytes=0x80000000, sum_=299000),
    gstep("pace", 41, arg="44", rc=34000, nbytes=736000, sum_=1),
    gstep("free", 830000, arg="workspace", nbytes=544768),
    gstep("free", 830100, arg="capture", nbytes=61440),
    "job 1 done rc=0 in 1100 ms, arena peak 0, dictionary page 1/0, game 900 ms, log 60 ms in 4 appends",
    "job start (overlay opened)",
    "input block on rc=0 (00000000)",
    job_line(weights="resident"),
    gstep("mem", 18),
    gstep("find", 130000, arg="1"),
    gstep("settle", 300000, arg="5", rc=1),
    gstep("load", 1500, kind="2", nbytes=1159168),
    gstep("recognize", 200000, arg="1", nbytes=37),
    gstep("free", 650000, arg="workspace"),
    "game vocr text 0: SENTINEL-TEXT-2",
    "game vocr text 0 shown: SENTINEL-TEXT-3",
    "subtitle text ready in 900 ms",
    "lookup first word at 0: 2 chars, dictionary 1/1, page 1/1, 13 reads 45429 B, rc=0, 65 ms; \"x\" -> x",
    "job 0 done rc=0 in 1000 ms, arena peak 53552, dictionary page 1/1, game 700 ms, log 45 ms in 3 appends",
    "job start (self-test)",
    "stage fallback begin t=90000 flags=0x2 model=VOCR3_H15_w8.vocr fallback=VOCR3_FL10_w8.vocr",
    "self-test: stopped for the overlay",
    "stage fallback end rc=-118 t=90300 ms=280 region=0x0+0+0 fb=0x0/0/0x00000000 model=VOCR3_FL10_w8.vocr fallback=1",
    "job 1 done rc=-118 in 320 ms, arena peak 0, dictionary page 1/0, game 250 ms, log 20 ms in 2 appends",
]) + "\n"


class TimelineTest(unittest.TestCase):
    def test_instant_steps_are_times_and_durations_are_phases(self):
        res = at.analyse(LOG)
        first = res["jobs"][0]
        t = first["games"][0]["timeline"]
        self.assertTrue(first["selftest"])
        self.assertEqual(first["games"][0]["stage"], "textbox")
        # the weights' allocation is a moment (1.38 s into the job), not a duration; the TEAR step's us is the end
        self.assertEqual([a["at_ms"] for a in t["allocs"] if a["item"] == "weights"], [1376.964])
        self.assertAlmostEqual(t["end_ms"], 2298.385)
        self.assertEqual((t["tears"], t["tries"], t["retry_at_ms"]), (13, 2, None))
        self.assertAlmostEqual(t["settle_ms"], 1129.125)
        self.assertEqual(t["settle"], "held")
        self.assertAlmostEqual(t["load_ms"], 425.71)
        self.assertFalse(t["warm"])
        # what the phases leave: the first try's pass and finder
        self.assertAlmostEqual(t["other_ms"], 2298.4 - 134.5 - 1129.1 - 425.7 - 429.9, delta=0.3)
        self.assertEqual((t["capture_strips"], t["capture_ms"]), (57, 1248))

    def test_warm_up_reads_and_the_done_lines_timing(self):
        res = at.analyse(LOG)
        warm = res["jobs"][1]
        t = warm["games"][0]["timeline"]
        self.assertTrue(t["warm"] and t["lm_read"] and t["cascade_read"])
        self.assertEqual((t["lm_load_ms"], t["cascade_load_ms"], t["lm_lines"]), (201.0, 299.0, 0))
        self.assertEqual(t["pace"], {"frames": 44, "max_gap_ms": 34.0, "slow": 1})
        self.assertEqual((warm["exchange_ms"], warm["log_ms"], warm["appends"]), (900, 60, 4))
        self.assertEqual(warm["shell_ms"], 200)  # 1100 - 900 exchanged - 0 looked up
        s = res["sessions"][1]
        self.assertEqual((s["weights_read"], s["lm_read"], s["cascade_read"], s["warm_reads"]), ([223.0], [201.0], [299.0], 1))
        opened = res["jobs"][2]
        self.assertEqual((opened["why"], opened["overlay_opened_during"], opened["lookup_ms"]), ("overlay opened", 1, 65))
        self.assertEqual(opened["shell_ms"], 1000 - 700 - 65)
        self.assertEqual(s["first_text_job"], 3)  # the warm-up read no line
        self.assertEqual((opened["text_lines"], warm["text_lines"]), (1, 0))  # its shown form is not another line
        self.assertEqual(s["first_open_wait_ms"], 1000)

    def test_selftest_rows_split_the_shell_time(self):
        res = at.analyse(LOG)
        rows = res["selftest"]
        self.assertEqual([r["stage"] for r in rows], ["textbox", "fallback"])
        r = rows[0]
        self.assertEqual((r["shell_ms"], r["record_ms"]), (4174, 52483 - 47328 - 4174))
        self.assertAlmostEqual(r["exchange_ms"], 4174 - 2298.385, delta=0.1)
        tot = res["selftest_total"]
        self.assertEqual((tot["stage_runs"], tot["cancelled"], tot["in_user_jobs"]), (2, 1, 1))
        self.assertEqual(res["jobs"][3]["kind"], "selftest")
        self.assertTrue(res["jobs"][3]["cancelled"])

    def test_this_builds_tear_step_says_when_the_retry_began(self):
        log = LOG.replace(gstep("tear", 2298159, arg="13", rc=2), gstep("tear", 2298159, arg="13", rc=2, nbytes=180000))
        t = at.analyse(log)["jobs"][0]["games"][0]["timeline"]
        self.assertEqual(t["retry_at_ms"], 180.0)

    def test_render_and_the_command_line(self):
        res = at.analyse(LOG)
        text = at.render(res, show_jobs=True)
        self.assertIn("allocated at weights@1377 ms", text)
        self.assertIn("warm-up", text)
        self.assertIn("in jobs the user waited for: 1", text)
        self.assertNotIn("SENTINEL", text)  # no text from the log, in the report or the JSON
        self.assertNotIn("SENTINEL", json.dumps(res))
        self.assertNotIn('"x"', text)
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d) / "collect"
            (folder / at.DATA).mkdir(parents=True)
            (folder / at.DATA / "log.txt").write_text("Vita JP Overlay shell started (kernel API 19)\njob start (x)\n" + LOG)
            (folder / at.DATA / "kernel.txt").write_text("game pid 0x52AA7 active\ngame pid 0xA20E1 active\n")
            out = subprocess.run([sys.executable, str(ROOT / "tools/analyse_timeline.py"), "--collect", str(folder),
                                  "--json"], capture_output=True, text=True, check=True).stdout
            data = json.loads(out)
            self.assertEqual(data["kernel_pids"], ["52AA7", "A20E1"])
            self.assertEqual(len(data["sessions"]), 2)  # the last boot only
            out = subprocess.run([sys.executable, str(ROOT / "tools/analyse_timeline.py"), "--compare", str(folder),
                                  str(folder)], capture_output=True, text=True, check=True).stdout
            self.assertIn("first job with text per session 12,271, 1,000 ms", out)
            empty = Path(d) / "empty"
            empty.mkdir()
            rc = subprocess.run([sys.executable, str(ROOT / "tools/analyse_timeline.py"), "--collect", str(empty)],
                                capture_output=True, text=True, check=False).returncode
            self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
