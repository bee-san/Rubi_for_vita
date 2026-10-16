"""tools/analyse_selftest.py against reports shaped like shell/selftest.c writes them.

v1 (API 18): reports built here, and #74's own report from the PCH-1001 on 3.65
(tools/tests/fixtures/selftest-74-report.txt, verbatim from the issue).
v2 (API 19): tools/tests/fixtures/selftest-v2-*.txt, written by the real
shell/selftest.c on the host (host/tests/selftest_writer.c, synthetic figures);
with VJO_SELFTEST_WRITER set the writer runs again and must reproduce them.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/analyse_selftest.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures"
TIERS_JSON = ROOT / "tools/model_tiers.json"
sys.path.insert(0, str(ROOT / "tools"))
import analyse_selftest as a  # noqa: E402

HEADER = """selftest v1 api=18
boot seq=7 mode=0 prev=0/4/0/0 failures=0 write=0x00000000
config backend=2 model=H15_w8.vocr fallback=FL10_w8.vocr dictionary=3 anki=0
"""


def sample(step, shell_main=1_200_000, paf=2_557_952, kd_free=9_000_000, kd_total=37_748_736, capture=0, pages=540672, pafl=8192):
    return (f"mem {step} shell_main={shell_main} shell_cdram=4194304 shell_phycont=0 rc=0x00000000 paf_free={paf}\n"
            f"ledger {step} pages={pages}/{pages} paf={pafl}/{pafl}\n"
            f"kernel {step} kd_total={kd_total} kd_free={kd_free} kd_rc=0x00000000 shell_total=52428800 "
            f"shell_free={shell_main + 65536} shell_rc=0x00000000 ledger=131072/131072 limit=2220032 capture={capture}\n")


def full_run(rc=0, free_user=91_226_112, title="PCSG00415", enabled=1, fallback=0):
    return (HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok")
            + f"step game t=95000 title={title} enabled={enabled} mode=1 region=full\n" + sample("game", shell_main=98304)
            + f"step ocr t=99000 rc={rc} model={'FL10_w8.vocr' if fallback else 'H15_w8.vocr'} fallback={fallback} 960x544 lines=3 recognized=3 frames=330\n"
            + f"game free_user={free_user} reserve=4194304 need_workspace=712704 need_weights=1159168 release=0 workspace=709936 "
              f"resident=1159168 weights=1 io=0x00000000 stat=0x00000000 cleanup=0 capture_rc=0x00000000 find_ms=120 load_ms=900 recognize_ms=640\n"
            + sample("ocr", shell_main=98304, paf=2_490_000, kd_free=6_900_000, capture=2088960) + "step done t=99100\n")


def run(text, *args):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
    try:
        return subprocess.run([sys.executable, str(SCRIPT), f.name, *args], capture_output=True, text=True)
    finally:
        Path(f.name).unlink()


def failed_cells(stdout):
    """Table rows or memory-route cells whose result is FAIL."""
    return re.findall(r"^ {2}\S+ +FAIL .*$|^ {4}.{40} FAIL .*$", stdout, re.M)


def rows(stdout):
    """stage -> result from the printed table."""
    out = {}
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and line.startswith("  ") and not line.startswith("    ") and parts[1] in ("PASS", "FAIL", "WARN", "NOTEXT", "-"):
            out[parts[0]] = parts[1]
    return out


class AnalyseV1(unittest.TestCase):
    def test_a_complete_run_passes_and_reports_every_pool(self):
        r = run(full_run())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS", r.stdout)
        self.assertEqual(rows(r.stdout), {"boot": "PASS", "ok": "PASS", "game": "PASS", "ocr": "PASS", "done": "PASS"})
        self.assertIn("user API (sceKernelGetFreeMemorySize)    PASS  main 98,304 B", r.stdout)
        self.assertIn("kernel block route, KD                   PASS  6,900,000 of 37,748,736 B free", r.stdout)
        self.assertIn("OCR ran in the game at 960x544 with H15_w8.vocr: 3 line(s) recognized, 640 ms", r.stdout)
        self.assertIn("The game had 91,226,112 B of USER free; the job needed 6,066,176 B", r.stdout)
        self.assertIn("check passed", r.stdout)
        # the low SceShell headroom at the game start is called out, with #59
        self.assertIn("only 98,304 B free", r.stdout)
        self.assertIn("#59", r.stdout)
        as_json = json.loads(run(full_run(), "--json").stdout)
        self.assertEqual(as_json["reached"], "done")
        self.assertEqual([s["step"] for s in as_json["steps"]], ["boot", "ok", "game", "ocr", "done"])

    def test_incomplete_runs_name_the_missing_step(self):
        cases = {
            HEADER + "step boot t=12\n" + sample("boot"): ("boot", "never wrote the stable minute (ok)"),
            HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok"): ("ok", "never wrote a game start"),
            HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok")
            + "step game t=95000 title=PCSG00415 enabled=1 mode=1 region=full\n" + sample("game"): ("game", "never wrote an OCR result"),
        }
        for text, (reached, phrase) in cases.items():
            r = run(text)
            self.assertEqual(r.returncode, 1, reached)
            self.assertIn(f"INCOMPLETE: the run reached '{reached}'", r.stdout)
            self.assertIn(phrase, r.stdout)
        # an exit event after the game is not progress: never "never wrote ?" (#74)
        r = run(HEADER + "step boot t=12\n" + sample("boot") + "step game t=95000 title=X enabled=1 mode=1 region=full\n"
                + sample("game") + "step exit t=99000\n" + sample("exit"))
        self.assertIn("INCOMPLETE: the run reached 'game'", r.stdout)
        self.assertNotIn("never wrote ?", r.stdout)
        r = run(HEADER)
        self.assertEqual(r.returncode, 1)
        self.assertIn("never wrote a step", r.stdout)

    def test_a_safe_boot_is_explained(self):
        text = HEADER.replace("mode=0 prev=0/4/0/0 failures=0", "mode=1 prev=0/2/0/0 failures=1") + "step boot t=12\n" + sample("boot")
        r = run(text)
        self.assertEqual(r.returncode, 1)
        self.assertIn("SAFE boot", r.stdout)
        self.assertIn("1 normal boot(s) in a row had failed", r.stdout)

    def test_game_out_of_memory_is_blamed_on_the_game_not_the_shell(self):
        r = run(full_run(rc=-103, free_user=5_000_000))
        self.assertEqual(r.returncode, 1)
        self.assertIn("The OCR job failed: out of memory (rc -103)", r.stdout)
        self.assertIn("check refused", r.stdout)
        self.assertIn("Out of memory in the GAME, not in SceShell", r.stdout)
        self.assertIn("FAIL: ocr failed", r.stdout)

    def test_fallback_model_io_errors_and_cleanup_are_named(self):
        text = full_run(fallback=1).replace("io=0x00000000", "io=0x80010013").replace("cleanup=0", "cleanup=1")
        r = run(text)
        self.assertIn("(the fallback model)", r.stdout)
        self.assertIn("Model I/O error 0x80010013 (ENODEV", r.stdout)
        self.assertIn("could not free its memory", r.stdout)

    def test_psp_launches_are_listed_once_each(self):
        """shell/game.c's psp: line: the IDs the console reports for a PSP game, its key and its game."""
        log = ('psp: NPXS10028 ULJS00378 -> ULJS00378; disc ULJS00378; boots ux0:pspemu/ISO/S.iso; "Synthetic"\n'
               'game ULJS00378 started: dictionary Local dictionaries (key MISSING), trigger l+r, subtitles select+r, '
               'ocr_mode auto\n'
               'psp: NPXS10028 ULJS00378 -> ULJS00378; disc ULJS00378; boots ux0:pspemu/ISO/S.iso; "Synthetic"\n'
               'psp: NPXS10028 PSPEMU006 -> PSPEMU006; disc ; boots ?; ""\n'
               'psp: NPXS10028 PSPEMU007 -> PSPEMU007; disc ULJS00385; boots ux0:pspemu/ISO/A long path.iso; "A title cut\n')
        lg = a.summarise_log(log)
        self.assertEqual([p["key"] for p in lg["psp"]], ["ULJS00378", "PSPEMU006", "PSPEMU007"])
        self.assertEqual(lg["psp"][0], {"title_id": "NPXS10028", "app": "ULJS00378", "key": "ULJS00378",
                                        "disc": "ULJS00378", "title": "Synthetic", "target": "ux0:pspemu/ISO/S.iso"})
        self.assertEqual((lg["psp"][1]["disc"], lg["psp"][1]["target"], lg["psp"][1]["title"]), ("", "?", ""))
        self.assertEqual((lg["psp"][2]["target"], lg["psp"][2]["title"]), ("ux0:pspemu/ISO/A long path.iso", "A title cut"))
        self.assertEqual(lg["games"]["ULJS00378"]["starts"], 1)

    def test_kernel_capture_failure_and_disabled_game(self):
        text = (HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok")
                + "step game t=95000 title=PCSG00415 enabled=1 mode=1 region=full\n" + sample("game")
                + "fail capture rc=-1073741826 t=97000\n")
        r = run(text)
        self.assertEqual(r.returncode, 1)
        self.assertIn("capture failed: no memory", r.stdout)
        self.assertIn("ScePhyMemPartKD", r.stdout)
        text = HEADER + "step boot t=12\n" + sample("boot") + "step game t=95000 title=PCSG00415 enabled=0 mode=1 region=full\n" + sample("game")
        r = run(text)
        self.assertIn("disabled in games.ini", r.stdout)
        self.assertEqual(rows(r.stdout)["game"], "FAIL")

    def test_unconfirmed_kd_and_low_paf_and_unavailable_kernel_sample(self):
        text = (HEADER + "step boot t=12\n" + sample("boot", kd_total=16_777_216, paf=1_000_000)
                + "step ok t=60012\nmem ok shell_main=-1 shell_cdram=-1 shell_phycont=-1 rc=0x80020001 paf_free=0\n"
                  "ledger ok pages=540672/540672 paf=8192/8192\nkernel ok unavailable rc=0xBFFFFFF8\n")
        r = run(text)
        self.assertIn("not the 36 MiB of ScePhyMemPartKD", r.stdout)
        self.assertIn("under the 1,572,864 B reserve", r.stdout)
        self.assertIn("sceKernelGetFreeMemorySize failed (0x80020001", r.stdout)
        self.assertIn("kernel memory sample was unavailable (rc 0xBFFFFFF8)", r.stdout)

    def test_garbage_is_rejected(self):
        r = run("hello\nworld\n")
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a self-test report", r.stderr)
        r = subprocess.run([sys.executable, str(SCRIPT), "/nonexistent/report.txt"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)

    def test_parser_keeps_pairs_and_hex(self):
        recs = a.parse("kernel ocr kd_total=37748736 kd_rc=0x80024302 ledger=1/2\n")
        self.assertEqual(recs[0]["fields"], {"kd_total": 37748736, "kd_rc": 0x80024302, "ledger": (1, 2)})
        self.assertEqual(recs[0]["words"], ["ocr"])


class Issue74(unittest.TestCase):
    """The owner's PCH-1001 on 3.65 at 2d1c08c: boot passed, every capture failed."""

    def setUp(self):
        self.report = (FIXTURES / "selftest-74-report.txt").read_text()

    def test_the_74_report_fails_at_the_capture_not_as_a_crash(self):
        r = run(self.report)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout), {"boot": "PASS", "ok": "PASS", "game": "PASS", "ocr": "FAIL", "done": "-"})
        self.assertIn("never reached: the capture failed 5 time(s) (no memory)", r.stdout)
        self.assertIn("2,088,960 B capture buffer", r.stdout)
        self.assertIn("0x80024302", r.stdout)
        self.assertIn("stayed armed and kept forcing the full-screen region", r.stdout)
        self.assertIn("FAIL: ocr failed", r.stdout)
        self.assertNotIn("never wrote ?", r.stdout)
        self.assertNotIn("INCOMPLETE", r.stdout)

    def test_the_74_memory_queries_are_named_as_failures_not_figures(self):
        r = run(self.report)
        # both exact queries returned 0x80020001 at every step; their zeros are not measurements
        self.assertIn("The block kernel memory route failed at every step it was tried (0x80020001", r.stdout)
        self.assertIn("The block shell memory route failed at every step it was tried (0x80020001", r.stdout)
        self.assertNotIn("0 of 0 B free", r.stdout)
        # the user view after boot was under the system margin (rc 0), not an error
        self.assertIn("main -2097152 (under the margin: 0 B free above it)", r.stdout)
        self.assertNotIn("sceKernelGetFreeMemorySize failed", r.stdout)
        # the routes are diagnostics: WARN cells; the verdict is the OCR row's FAIL
        self.assertIn("kernel block route, KD                   WARN  0x80020001", r.stdout)
        self.assertIn("diagnostics: WARN (block kernel route, block shell route)", r.stdout)
        as_json = json.loads(run(self.report, "--json").stdout)
        self.assertEqual((as_json["ocr_result"], as_json["diagnostics_result"]), ("FAIL", "WARN"))
        self.assertEqual(as_json["reached"], "game")
        self.assertEqual(len(as_json["fails"]), 5)


class AnalyseV2(unittest.TestCase):
    def analyse(self, name, *args):
        return run((FIXTURES / f"selftest-v2-{name}.txt").read_text(), *args)

    def test_the_smallest_size_configured_passes_its_fallback_stage_with_itself(self):
        """API 21: the fallback stage runs the smallest model size. With Small configured there is none
        smaller (the config record says fallback=-), so the stage runs Small itself: a pass, not "the
        fallback model was not used"; with a larger size it still has to run a smaller one."""
        text = (FIXTURES / "selftest-v2-pass.txt").read_text()
        config = "config backend=2 model=VOCR3_H15_w8.vocr fallback=VOCR3_FL10_w8.vocr tier=normal dictionary=3 anki=0"
        self.assertIn(config, text)
        self.assertIn(" model=FL10_w8.vocr fallback=1 ", text)
        small = text.replace(config, "config backend=2 model=VOCR3_FL10_w8.vocr fallback=- tier=small dictionary=3 anki=0")
        small = small.replace(" model=FL10_w8.vocr fallback=1 ", " model=FL10_w8.vocr fallback=0 ")
        r = run(small)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(rows(r.stdout)["fallback"], "PASS")
        self.assertIn("Configured the small model size (VOCR3_FL10_w8.vocr); the fallback stage runs it too: it is "
                      "the smallest.", r.stdout)
        normal = text.replace(" model=FL10_w8.vocr fallback=1 ", " model=FL10_w8.vocr fallback=0 ")
        r = run(normal)
        self.assertEqual(rows(r.stdout)["fallback"], "FAIL")
        self.assertIn("the fallback model was not used", r.stdout)
        r = run(text)
        self.assertIn("Configured the normal model size (VOCR3_H15_w8.vocr); the fallback stage runs the smallest, "
                      "VOCR3_FL10_w8.vocr.", r.stdout)

    def test_a_complete_run_passes_every_row(self):
        r = self.analyse("pass")
        self.assertEqual(r.returncode, 0, r.stdout)
        got = rows(r.stdout)
        for name in a.ROWS_V2 + ("capture<64KiB", "kernel-pool", "recognitions", "tearing", "kernel-memory",
                                 "game-memory"):
            self.assertIn(name, got)
        self.assertEqual({k: v for k, v in got.items() if v != "PASS"}, {})
        self.assertIn("worked: hook->user, kstrip->user, user->user", r.stdout)
        self.assertIn("weights read in 1050 ms", r.stdout)       # H15 read from the card (textbox)
        self.assertIn("FL10_w8.vocr, 3 line(s)", r.stdout)        # the forced fallback
        self.assertIn("960x544+0+0 H15_w8.vocr", r.stdout)        # the full screen through the strips
        self.assertIn("largest capture block 61,440 B (limit 65,536 B)", r.stdout)
        self.assertIn("14 line(s) recognized in all", r.stdout)
        self.assertIn("game worker memory", r.stdout)
        self.assertTrue(r.stdout.rstrip().endswith("PASS: every stage completed; the figures above are this console's."))
        self.assertIn("diagnostics: PASS\n", r.stdout)
        as_json = json.loads(self.analyse("pass", "--json").stdout)
        self.assertEqual((as_json["ocr_result"], as_json["diagnostics_result"], as_json["warnings"]),
                         ("PASS", "PASS", []))

    def test_each_stage_has_its_headroom_and_the_free_memory_picks_a_tier(self):
        r = self.analyse("pass")
        # textbox: 91,226,112 + 0 - 4,194,304 - (249,856 + 1,159,168) = 85,622,784
        self.assertIn("textbox    H15_w8.vocr          free   91,226,112 reserve  4,194,304 need  1,409,024 "
                      "headroom   85,622,784 B", r.stdout)
        as_json = json.loads(self.analyse("pass", "--json").stdout)
        heads = {h["stage"]: h["headroom"] for h in as_json["headroom"]}
        self.assertEqual(heads, {"fallback": 86_122_496, "textbox": 85_622_784, "fullscreen": 86_781_952,
                                 "textbox2": 86_781_952})
        self.assertNotIn("probe", heads)  # the probe makes no memory check
        t = as_json["tiers"]
        self.assertEqual((t["free_user"], t["recommendation"]), (91_226_112, "Large"))
        # the formula of vita-vn-ocr docs/model-tiers.md 1.5 (with today's 65,536 B capture block), at each
        # tier's ceilings of tools/model_tiers.json
        self.assertEqual([row["need"] for row in t["rows"]], [4_399_104, 2_449_408, 1_720_320])
        self.assertEqual([row["weights"] for row in t["rows"]], [2_500_000, 1_600_000, 999_999])
        self.assertEqual([row["reserve"] for row in t["rows"]], [16 << 20, 8 << 20, 4 << 20])
        self.assertEqual([row["box_ms"] for row in t["rows"]], [3000, 2000, 1000])
        self.assertEqual([row["id"] for row in t["rows"]], ["large", "normal", "small"])
        self.assertIn("recommendation: Large", r.stdout)
        self.assertIn("  Large     2,500,000     < 3 s  1,310,720        4,399,104  16,777,216    70,049,792 yes\n",
                      r.stdout)
        self.assertIn("box time: the tier's limit for a typical full textbox (3 lines of about 640 px, 1 core at "
                      "444 MHz), estimated; the recommendation is memory alone", r.stdout)

    def test_the_tier_falls_with_the_free_memory(self):
        # each tier's first job (above) plus its reserve, and one byte less
        for free, want in ((21_176_320, "Large"), (21_176_319, "Normal"), (10_838_016, "Normal"), (10_838_015, "Small"),
                           (5_914_624, "Small"), (5_914_623, None)):
            self.assertEqual(a.tier_table(free)["recommendation"], want, free)
        # a refused stage has a negative headroom and says so: the textbox stage in a game with 5,000,000 B free
        text = (FIXTURES / "selftest-v2-pass.txt").read_text()
        assert "memory textbox free_user=91226112 " in text
        out = a.analyse(text.replace("memory textbox free_user=91226112 ", "memory textbox free_user=5000000 "))
        refused = [h for h in out["headroom"] if h["headroom"] is not None and h["headroom"] < 0]
        self.assertEqual([(h["stage"], h["headroom"]) for h in refused], [("textbox", 5_000_000 - 4_194_304 - 249_856 - 1_159_168)])
        self.assertEqual(refused[0]["headroom"], -603_328)
        self.assertEqual(a.render(out).count("(refused)"), 1)

    def test_collect_needs_a_collect_folder(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([sys.executable, str(SCRIPT), "--collect", d], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a collect folder", r.stderr)

    def test_a_failed_stage_keeps_its_record_and_the_run_goes_on(self):
        r = self.analyse("fail")
        self.assertEqual(r.returncode, 1)
        got = rows(r.stdout)
        self.assertEqual(got["textbox"], "FAIL")
        self.assertEqual((got["fullscreen"], got["textbox2"], got["done"]), ("PASS", "PASS", "PASS"))
        self.assertIn("out of memory (rc -103); failed steps: alloc user/workspace 0x80024302", r.stdout)
        self.assertIn("alloc cdram/workspace 0x80024302", r.stdout)
        self.assertIn("alloc phycont/workspace 0x80024302", r.stdout)
        self.assertIn("FAIL: textbox failed", r.stdout)

    def test_a_stage_without_its_record_is_where_the_vita_stopped(self):
        r = self.analyse("incomplete")
        self.assertEqual(r.returncode, 1)
        got = rows(r.stdout)
        self.assertEqual((got["textbox"], got["fullscreen"], got["textbox2"], got["done"]), ("PASS", "FAIL", "-", "-"))
        self.assertIn("INCOMPLETE: stage 'fullscreen' began and never wrote its record", r.stdout)

    def test_a_failed_memory_route_is_named_and_another_route_still_counts(self):
        r = self.analyse("routes")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("kernel block route, KD                   WARN  0x80020001", r.stdout)
        self.assertIn("at step 2 (block/address-space info)", r.stdout)
        # the kernel's address space refused, the caller's read; a slot outside the kernel half not read
        self.assertIn("kernel process route (faps-coredump)     PASS  2 partitions of shell's address space, "
                      "KD 9,437,184 B free; 1 not read (pointer outside the kernel range)", r.stdout)
        self.assertIn("routes that read nothing: block kernel 0x80020001", r.stdout)
        self.assertEqual(rows(r.stdout)["kernel-memory"], "PASS")
        self.assertEqual(failed_cells(r.stdout), [])

    def test_stages_that_waited_for_text_in_the_region_pass_and_say_so(self):
        r = self.analyse("waited")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual({k: v for k, v in rows(r.stdout).items() if v != "PASS"}, {})
        self.assertIn("FL10_w8.vocr, 3 line(s)", r.stdout)
        self.assertIn("; after 3 settled screen(s) without text in your region", r.stdout)
        self.assertIn("The stages waited for text in your region past 3 settled screen(s) (2 with no line in your "
                      "region, 1 that changed under every read (rc -120)) before they ran.", r.stdout)
        self.assertNotIn("The wait reached its bound", r.stdout)
        self.assertIn("14 line(s) recognized in all, 9 in your region", r.stdout)
        self.assertNotIn("INCOMPLETE", r.stdout)  # each wait closed the begin before it
        self.assertTrue(r.stdout.rstrip().endswith("PASS: every stage completed; the figures above are this console's."))

    def test_region_stages_that_found_no_line_are_not_a_recognition_pass(self):
        """#81: the stages ran on the game's opening notice; the region stages found no line while the full
        screen found five, and the old analyser said PASS. Such rows are NOTEXT, and so is the run."""
        r = self.analyse("notext")
        self.assertEqual(r.returncode, 1, r.stdout)
        got = rows(r.stdout)
        self.assertEqual({k: got[k] for k in ("fallback", "textbox", "fullscreen", "textbox2", "recognitions", "done")},
                         {"fallback": "NOTEXT", "textbox": "NOTEXT", "fullscreen": "PASS", "textbox2": "NOTEXT",
                          "recognitions": "NOTEXT", "done": "PASS"})
        self.assertIn("no text in your region (no line found there): 917x125+22+400 FL10_w8.vocr, 0 line(s)", r.stdout)
        self.assertIn("5 line(s) recognized in all, none in your region", r.stdout)
        self.assertIn("The wait reached its bound (30 screens)", r.stdout)
        verdict = r.stdout.rstrip().splitlines()[-1]
        self.assertTrue(verdict.startswith("NO TEXT: every stage completed, but no line was found in your region in "
                                           "fallback, textbox, textbox2 (the full screen found 5"), verdict)
        as_json = json.loads(self.analyse("notext", "--json").stdout)
        self.assertEqual(as_json["ocr_result"], "NOTEXT")
        # a failure still wins over a missing text
        text = re.sub(r"^(stage textbox end) rc=0 ", r"\1 rc=-103 ", (FIXTURES / "selftest-v2-notext.txt").read_text(),
                      flags=re.M)
        r = run(text)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(rows(r.stdout)["textbox"], "FAIL")
        self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("FAIL: textbox failed"))

    def test_no_text_in_some_region_stages_says_what_the_others_read(self):
        """#92's audit, N3: fallback and textbox read lines but textbox2 found none (the game moved on between
        the stages). Still NO TEXT (textbox2's figures are not a recognition), but never 'elsewhere on the
        screen'; the screen changed between the stages only if the region's checksum did."""
        base = (FIXTURES / "selftest-v2-pass.txt").read_text()
        text = re.sub(r"^line textbox2 .*\n", "", base, flags=re.M)
        text, n = re.subn(r"^(stage textbox2 end .*) lines=3 recognized=3 frames=330( text=0x[0-9A-F]+)?$",
                          r"\1 lines=0 recognized=0 frames=0\2", text, flags=re.M)
        self.assertEqual(n, 1)
        changed, n = re.subn(r"^(memory textbox2 .*) sum=0x5EED0003$", r"\1 sum=0x5EED0000", text, flags=re.M)
        self.assertEqual(n, 1)
        r = run(changed)
        self.assertEqual(r.returncode, 1, r.stdout)
        got = rows(r.stdout)
        self.assertEqual((got["fallback"], got["textbox"], got["textbox2"], got["recognitions"]),
                         ("PASS", "PASS", "NOTEXT", "PASS"))
        verdict = r.stdout.rstrip().splitlines()[-1]
        self.assertTrue(verdict.startswith("NO TEXT: every stage completed, but textbox2 found no line in your region "
                                           "while fallback, textbox found 6 there: the screen changed between the "
                                           "stages."), verdict)
        self.assertNotIn("elsewhere", verdict)
        same = run(text).stdout.rstrip().splitlines()[-1]  # the same pixels: no claim that the screen changed
        self.assertTrue(same.startswith("NO TEXT: every stage completed, but textbox2 found no line in your region "
                                        "while fallback, textbox found 6 there. "), same)

    def test_a_run_still_waiting_for_text_is_incomplete_and_says_why(self):
        r = self.analyse("waiting")
        self.assertEqual(r.returncode, 1, r.stdout)
        got = rows(r.stdout)
        self.assertEqual((got["fallback"], got["textbox"], got["done"]), ("-", "-", "-"))
        self.assertIn("waiting for text in your region: 4 settled screen(s) had none, the last at", r.stdout)
        self.assertIn("INCOMPLETE: the stages were still waiting for text in your region: 4 settled screen(s) had none",
                      r.stdout)
        self.assertNotIn("began and never wrote its record", r.stdout)
        self.assertNotIn("The wait reached its bound", r.stdout)
        self.assertEqual(json.loads(self.analyse("waiting", "--json").stdout)["ocr_result"], "INCOMPLETE")
        # a stop in the first stage after the waits is still where the Vita stopped
        text = (FIXTURES / "selftest-v2-waiting.txt").read_text() + \
            "stage fallback begin t=170000 flags=0x2 model=H15_w8.vocr fallback=FL10_w8.vocr\n"
        r = run(text)
        self.assertIn("INCOMPLETE: stage 'fallback' began and never wrote its record", r.stdout)

    @unittest.skipUnless(os.environ.get("VJO_SELFTEST_WRITER"), "vjo-selftest-writer not built")
    def test_the_step_formatter_prints_the_lm_tier_and_the_cascade_bit(self):
        # shell/selftest.c vjo_gstep_format: the kind of the check (the LM tier | 8 with the cascade),
        # lm (the tier) and cascade (lines it read as empty) steps; other steps keep kind=-
        with tempfile.TemporaryDirectory() as tmp:
            done = subprocess.run([os.environ["VJO_SELFTEST_WRITER"], tmp, "gstep"], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = done.stdout.splitlines()
        for start in ("gstep job check kind=10 ", "gstep job lm kind=2 arg=1 ", "gstep job cascade kind=1 arg=1 ",
                      "gstep job recognize kind=- arg=1 ", "gstep job refine kind=2 arg=16 "):
            self.assertTrue(any(line.startswith(start) for line in lines), (start, lines))
        # the refine step as the formatter writes it, read back by --collect: its mode, boxes, splits,
        # the boxes not split for the 16-line cap and the rows read
        rf = a.summarise_log("\n".join(lines) + "\n")["refine"]
        self.assertEqual({k: rf[k] for k in ("jobs", "modes", "boxes", "lines", "split", "capped", "rows", "us")},
                         {"jobs": 1, "modes": {"psp": 1}, "boxes": 9, "lines": 16, "split": 7, "capped": 2,
                          "rows": 410, "us": [1400]})

    def test_the_vjonet_stack_line_keeps_the_largest_and_warns_near_the_top(self):
        # shell/worker.c net_stack_check: logged at a game exit when it grew; the summary keeps the largest
        lg = a.summarise_log("VjoNet stack: at most 5920 of 16384 B used\nVjoNet stack: at most 13000 of 16384 B used\n"
                             "VjoNet stack: at most 9000 of 16384 B used\n")
        self.assertEqual(lg["net_stack"], (13000, 16384))
        self.assertIsNone(a.summarise_log("job 0 done rc=0 in 1 ms, arena peak 1\n")["net_stack"])
        self.assertEqual(a.net_stack_line(6144, 16384), "VjoNet stack: at most 6,144 of 16,384 B used (37%)")
        self.assertEqual(a.net_stack_line(12288, 16384), "VjoNet stack: at most 12,288 of 16,384 B used (75%)")
        self.assertEqual(a.net_stack_line(12289, 16384),
                         "VjoNet stack: at most 12,289 of 16,384 B used (75%) WARN: under a quarter left")
        # a boot that did not paint (shell/worker.c net_stack_paint) has no figure: said, as a WARN, per reason
        lg = a.summarise_log("VjoNet stack: not painted (no thread info)\n"
                             "VjoNet stack: not painted (the kernel reports 65536 B, not 16384)\n"
                             "VjoNet stack: not painted (no thread info)\nVjoNet stack: at most 6144 of 16384 B used\n")
        self.assertEqual(lg["net_stack_off"], ["no thread info", "the kernel reports 65536 B, not 16384", "no thread info"])
        self.assertEqual(a.net_stack_lines(lg), [
            "  VjoNet stack: at most 6,144 of 16,384 B used (37%)",
            "  VjoNet stack: not painted at 3 boot(s) WARN: no figure from them (no thread info x2; the kernel reports "
            "65536 B, not 16384 x1)"])
        self.assertEqual(a.net_stack_lines(a.summarise_log("")), [])

    def test_the_clock_step_counts_raised_and_left_alone(self):
        # game_ocr/main.c job_busy (ocr_boost_clock = on, the default): 333 raised to 444 and put back; a clock
        # already at 444 or more (PSVshell's 500) left as it is, rc 1, never lowered
        log = "".join("gstep job clock kind=- arg=%d rc=0x%08X bytes=%d sum=0x%08X main=0 cdram=0 phycont=0 us=1\n"
                      % row for row in ((333, 0, 444, 333), (500, 1, 500, 500), (444, 1, 444, 444)))
        ck = a.summarise_log(log)["clock"]
        self.assertEqual((ck["jobs"], ck["raised"], ck["kept"]), (3, 1, 2))
        self.assertEqual((ck["before"], ck["during"], ck["after"]), ([333, 500, 444], [444, 500, 444], [333, 500, 444]))

    @unittest.skipUnless(os.environ.get("VJO_SELFTEST_WRITER"), "vjo-selftest-writer not built")
    def test_the_fixtures_are_what_the_writer_writes(self):
        for name in ("pass", "fail", "incomplete", "routes", "diagnostics", "waited", "notext", "waiting"):
            with tempfile.TemporaryDirectory() as tmp:
                done = subprocess.run([os.environ["VJO_SELFTEST_WRITER"], tmp, name], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)
                got = (Path(tmp) / "selftest-report.txt").read_text()
                self.assertEqual(got, (FIXTURES / f"selftest-v2-{name}.txt").read_text(),
                                 f"selftest-v2-{name}.txt differs from shell/selftest.c's output: regenerate it with "
                                 f"vjo-selftest-writer DIR {name}")
                # the marker is removed when (and only when) the run finished
                self.assertEqual((Path(tmp) / "selftest.txt").exists(), name in ("incomplete", "waiting"))
                if name == "pass":
                    # ocr-first: a record is one append of the report (two for a stage's end of more than
                    # 2 KiB, and a memory sample's record two: before and after the kernel's routes), not
                    # one a line (this run: 21 for its 132 lines, line by line 132): boot, ok, game, 5
                    # begins, 5 ends, done (docs/first-ocr.md)
                    appends = int(done.stdout.split("report appends ")[1].split(",")[0])
                    self.assertLessEqual(appends, 2 * 3 + 5 + 2 * 5 + 2, done.stdout)
                    self.assertGreaterEqual(got.count("\n"), 5 * appends)
                    # every report line is also logged (status.txt, log.txt), as line by line
                    self.assertEqual(int(done.stdout.split("log bytes ")[1].split(",")[0]), len(got.encode()),
                                     done.stdout)
                    # every sample's lines are written before the kernel's memory routes run (a crash there
                    # keeps them, as line by line did: audit-overlay-first-ocr)
                    self.assertEqual(int(done.stdout.split("unflushed samples ")[1]), 0, done.stdout)

    @unittest.skipUnless(os.environ.get("VJO_SELFTEST_WRITER"), "vjo-selftest-writer not built")
    def test_the_report_stops_at_its_cap_as_line_by_line(self):
        # shell/selftest.c REPORT_CAP (48 KiB) with records written a group at a time: a line is written
        # while the bytes before it (the group's included) are at most the cap, as line by line, so the
        # report ends with the first line that crosses it, whole, and the logs got the same lines
        # (capN puts the cap in another place of a record: in a group, between two, ...)
        cap = 48 * 1024
        for shift in range(8):
            with tempfile.TemporaryDirectory() as tmp:
                done = subprocess.run([os.environ["VJO_SELFTEST_WRITER"], tmp, f"cap{shift}"], capture_output=True,
                                      text=True, check=False)
                self.assertEqual(done.returncode, 0, done.stderr)
                data = (Path(tmp) / "selftest-report.txt").read_bytes()
            lines = data.split(b"\n")
            self.assertEqual(lines[-1], b"", shift)  # whole lines only
            last = lines[-2] + b"\n"
            self.assertGreater(len(data), cap, shift)
            self.assertLessEqual(len(data) - len(last), cap, shift)
            self.assertEqual(int(done.stdout.split("log bytes ")[1].split(",")[0]), len(data), done.stdout)
            self.assertEqual(int(done.stdout.split("unflushed samples ")[1]), 0, done.stdout)



# A tier table in a report and in --collect's, the line for the game's tier, and the finding a report
# without the table has instead.
TIER_BLOCK = re.compile(r"\n\nmodel tiers \(.*?\n  box time: [^\n]*", re.S)
GAME_TIER_LINE = re.compile(r"\n  recommendation for this game: [^\n]*")
NOT_PRICED = re.compile(r"\n  - [Tt]he model tiers were not priced[^\n]*")


def compiled_ints(path):
    """Every integer constant of a Python file, as compiled (spelling and constant arithmetic folded)."""
    out = set()

    def walk(consts):
        for c in consts:
            if isinstance(c, types.CodeType):
                walk(c.co_consts)
            elif isinstance(c, (tuple, frozenset)):
                walk(c)
            elif isinstance(c, int) and not isinstance(c, bool):
                out.add(c)

    walk(compile(Path(path).read_text(encoding="utf-8"), str(path), "exec").co_consts)
    return out


def without_tiers(out):
    """A --json object with the fields the tier table *fills* removed (a report's or a --collect's
    self-test "tiers", --collect's "tiers") and the 'not priced' finding dropped, so what is left must
    be the same with the table and without it. "tiers_all" is kept: without the table it must be [],
    which the caller checks, so dropping it there is caught."""
    out = json.loads(json.dumps(out))  # a copy
    not_priced = "The model tiers were not priced: "

    def strip(node):
        if isinstance(node, dict):
            node.pop("tiers", None)
            if isinstance(node.get("findings"), list):
                node["findings"] = [f for f in node["findings"] if not f.startswith(not_priced)]
            if isinstance(node.get("undecided"), list):
                node["undecided"] = [u for u in node["undecided"] if "model tiers were not priced" not in u]
            for v in node.values():
                strip(v)
        elif isinstance(node, list):
            for v in node:
                strip(v)

    strip(out)
    return out


def collect_folder(root):
    """A minimal --collect folder: the PASS self-test report and one job's memory line in log.txt."""
    folder = root / "vjo-collect-20261015-000000Z"
    data = folder / "ux0/data/VitaJPOverlay"
    data.mkdir(parents=True)
    shutil.copyfile(FIXTURES / "selftest-v2-pass.txt", data / "selftest-report.txt")
    (data / "log.txt").write_text("game vocr memory: free USER=50000000, needs workspace 249856 + weights 1159168 + "
                                  "reserve 4194304: check passed, headroom 44396672\n", encoding="utf-8")
    (folder / "COLLECT.json").write_text(json.dumps({"collected_at": "2026-10-15T00:00:00Z", "host": "192.0.2.1",
                                                     "files": {}, "missing": [], "errors": [], "redacted_keys": []}))
    return folder


class SharedTierTable(unittest.TestCase):
    """tools/model_tiers.json is the only copy of the tiers (audit-test-build-2 N2): the analyser prices
    its tiers and nothing of its own, it holds today's ceilings and box time limits, the Overlay app's
    extra keys change nothing here, and the release ships it beside the analyser."""

    def table(self):
        return json.loads(TIERS_JSON.read_text(encoding="utf-8"))

    def test_the_result_codes_are_read_from_core_errors_h(self):
        # core/errors.h holds the VJO_E_* codes since core/conn.h went with the network; -118 and -119
        # are in no table of the analyser's own, so only that read names them (audit-overlay-shell-slim-3 N5)
        a.load_rc_names()
        self.assertEqual(a.rc_text(-118), "cancelled (rc -118)")
        self.assertEqual(a.rc_text(-119), "ocr inference (rc -119)")
        self.assertEqual(a.rc_text(-100), "connect (rc -100)")  # an older build's network code keeps its name

    def test_a_log_host_line_is_one_to_delete_and_its_address_is_not_shown(self):
        self.assertIsNone(a.log_host_line(None))
        self.assertEqual(a.log_host_line(""), "    log_host (empty): the UDP log was removed (the overlay is "
                                                "offline); today's app or installer deletes the line")
        line = a.log_host_line("192.168.1.5")
        self.assertTrue(line.startswith("    log_host (set): ") and "192.168" not in line)

    def test_collect_never_shows_a_log_host_address(self):
        # audit-overlay-shell-slim-3b B1: the config line and analysis.json say set or empty, not the address
        with tempfile.TemporaryDirectory() as d:
            data = Path(d) / "ux0/data/VitaJPOverlay"
            data.mkdir(parents=True)
            (Path(d) / "COLLECT.json").write_text("{}")
            for value, shown in (("192.168.1.5", "set"), ("", "empty")):
                with self.subTest(value=value):
                    (data / "config.ini").write_text(f"log_file = on\nlog_host = {value}\n")
                    r = subprocess.run([sys.executable, str(SCRIPT), "--collect", d], capture_output=True, text=True,
                                       timeout=60)
                    text = r.stdout + r.stderr
                    for out in Path(d).glob("analysis.*"):
                        text += out.read_text(errors="replace")
                    self.assertNotIn("192.168", text)
                    self.assertIn(f"log_host ({shown}): the UDP log was removed", r.stdout)

    def test_any_ocr_backend_line_is_one_to_delete(self):
        # VOCR is the only engine since Google Lens's removal: the plugins warn for a removed engine's
        # value (core/config.c removed_engine) and ignore vocr; the app's next save and the installer
        # delete the line either way
        for value in ("lens", "Lens", "vocr2", "meiki", "NCNN"):
            line = a.backend_line(value)
            self.assertIn(f"ocr_backend={value}: a removed engine", line)
            self.assertIn("deletes the line", line)
        self.assertEqual(a.backend_line("vocr"), "    ocr_backend=vocr: no longer a setting (VOCR is the only "
                                                 "engine); today's app or installer deletes the line")
        self.assertIsNone(a.backend_line(None))

    def test_the_analyser_prices_the_shared_tables_tiers(self):
        table = self.table()
        self.assertIsNone(a.TIERS_ERROR)
        self.assertEqual(a.TIERS_FILE, TIERS_JSON)
        self.assertEqual(list(a.TIERS), [{k: t[k] for k in ("id", "name", *a.TIER_KEYS)}
                                         for t in reversed(table["tiers"])])
        self.assertEqual((a.TIER_DEFAULT, a.TIER_BOX), (table["default"], table["box"]))
        # no ceiling of its own, in any spelling: 9000000, 9_000_000 and 9 * 10**6 compile to the same
        # constant. None of the table's ceilings and reserves, nor the superseded 9 MB / 4 MB / 1 MB ones,
        # is a constant of the tool; 262,144 is (KERNEL_TXT_CAP, kernel.txt's 256 KiB), not as a tier's.
        forbidden = {t[k] for t in table["tiers"] for k in ("weights_max_bytes", "arena_max_bytes", "reserve_bytes")}
        forbidden |= {9_000_000, 4_000_000, 1_000_000}
        self.assertEqual(a.KERNEL_TXT_CAP, 262_144)
        self.assertEqual(forbidden & (compiled_ints(SCRIPT) - {a.KERNEL_TXT_CAP}), set())

    def test_the_formulas_blocks_are_todays(self):
        """The capture block and the line-block ceiling of the first-job formula are the game worker's
        own (the vita-vn-ocr formula's 61,440 B capture block is API 19's)."""
        ledger = (ROOT / "include/vjo_ledger_budget.h").read_text(encoding="utf-8")
        vocr = (ROOT / "core/vocr_ocr.h").read_text(encoding="utf-8")
        self.assertEqual(a.TIER_CAPTURE, int(re.search(r"#define VJO_BUDGET_GAME_CAPTURE (\d+)u", ledger).group(1)))
        self.assertEqual(a.TIER_LINE, int(re.search(r"#define VJO_VOCR_LINE_MAX_BYTES (\d+)u", vocr).group(1)))
        self.assertEqual(a.tier_need(999_999, 131_072), 1_003_520 + 135_168 + 65_536 + 516_096)

    def test_the_tiers_are_todays_ceilings_and_box_time_limits(self):
        # Weights: the owner's ceilings of 2026-10-14 18:09Z, kept by the conductor's correction of
        # 2026-10-15 05:56Z; box time: the owner's limits of 07:33Z, a typical full textbox (3 lines of
        # about 640 px) on one A9 core at 444 MHz; default Normal (owner, 2026-10-15 04:52Z).
        table = self.table()
        self.assertEqual([(t["id"], t["name"], t["weights_max_bytes"], t["box_ms_under"]) for t in table["tiers"]],
                         [("small", "Small", 999_999, 1000), ("normal", "Normal", 1_600_000, 2000),
                          ("large", "Large", 2_500_000, 3000)])
        self.assertEqual(table["box"], {"lines": 3, "line_px": 640, "cores": 1, "mhz": 444})
        self.assertEqual(table["default"], "normal")
        # vita-vn-ocr docs/model-tiers.md 1.3 and 1.5: the W8I arena ceilings and the per-tier reserve
        self.assertEqual([(t["arena_max_bytes"], t["reserve_bytes"]) for t in table["tiers"]],
                         [(131_072, 4 << 20), (262_144, 8 << 20), (1_310_720, 16 << 20)])

    def test_other_keys_are_ignored_and_a_table_it_cannot_use_is_refused(self):
        table = self.table()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "model_tiers.json"

            def load(t):
                path.write_text(json.dumps(t), encoding="utf-8")
                return a.load_tiers(path)

            # the keys the Overlay app's tier choice adds (overlay-ux-tiers) change nothing here
            extra = json.loads(json.dumps(table))
            extra["est_box_basis"] = "the shipped model at the console's measured ms per CTC frame"
            for t, model in zip(extra["tiers"], ("VOCR3_FL10_w8.vocr", "VOCR3_H15_w8.vocr", "VOCR4_W20_w8.vocr")):
                t.update(model=model, tagline="fastest", est_box_ms=700, need_bytes=6_000_000)
            self.assertEqual(load(extra), (a.TIERS, a.TIER_DEFAULT, a.TIER_BOX))
            for broken in (lambda t: t["tiers"][1].pop("reserve_bytes"),
                           lambda t: t["tiers"][0].update(weights_max_bytes="999999"),
                           lambda t: t["tiers"][0].update(box_ms_under=True),
                           lambda t: t["tiers"][2].update(arena_max_bytes=0),
                           lambda t: t["tiers"].reverse(),               # largest first
                           lambda t: t["tiers"][2].update(box_ms_under=1500),  # a larger tier's limit below Normal's
                           lambda t: t["tiers"][2].update(id="normal"),
                           lambda t: t.update(default="medium"),
                           lambda t: t.update(tiers=[]),
                           lambda t: t["box"].pop("mhz"),
                           lambda t: t.pop("tiers"),
                           lambda t: t["tiers"][2].update(id=["large"]),  # unhashable: crashed at import before
                           lambda t: t["tiers"][0].update(name=None),     # crashed the table's rendering before
                           lambda t: t["tiers"][1].update(name=["Normal"]),
                           lambda t: t["tiers"][1].update(name="Nor\nmal"),
                           lambda t: t["tiers"][0].update(id=""),
                           lambda t: t.update(default=["normal"]),
                           lambda t: t.update(tiers={"small": t["tiers"][0]}),
                           lambda t: t["tiers"].append(7),
                           lambda t: t["box"].update(lines="3"),
                           lambda t: t.update(box=[3, 640, 1, 444])):
                copy = json.loads(json.dumps(table))
                broken(copy)
                with self.assertRaises(ValueError):
                    load(copy)
            for text in ("{not json", "", "null", "[]", '"tiers"', "\udcff"):
                path.write_text(text, encoding="utf-8", errors="surrogateescape")
                with self.assertRaises(ValueError, msg=text):
                    a.load_tiers(path)
            with self.assertRaises(OSError):
                a.load_tiers(Path(d) / "missing.json")

    def test_without_the_table_the_verdict_stands_and_says_why(self):
        text = (FIXTURES / "selftest-v2-pass.txt").read_text()
        with mock.patch.object(a, "TIERS", ()), mock.patch.object(a, "TIERS_ERROR", "model_tiers.json: missing"):
            out = a.analyse(text)
            shown = a.render(out)
        self.assertEqual((out["ocr_result"], out["diagnostics_result"]), ("PASS", "PASS"))
        self.assertNotIn("tiers", out)
        self.assertIn("The model tiers were not priced: model_tiers.json: missing.", out["findings"])
        self.assertNotIn("model tiers (", shown)
        self.assertTrue(shown.endswith("PASS: every stage completed; the figures above are this console's."))

    def test_without_the_table_or_with_a_broken_one_the_report_is_the_rest_unchanged(self):
        """The real tool, run from a copy of tools/ as a release has it, on the PASS fixture and on a
        --collect folder: with tools/model_tiers.json missing or broken, the exit status and every line
        of the report are what they are with it, less the tier tables, plus the finding that says why."""
        good = TIERS_JSON.read_text(encoding="utf-8")

        def edited(edit):
            table = json.loads(good)
            edit(table)
            return json.dumps(table)

        variants = {"missing": None, "not JSON": "{not json", "empty": "",
                    "a null name": edited(lambda t: t["tiers"][0].update(name=None)),
                    "a list id": edited(lambda t: t["tiers"][2].update(id=["large"])),
                    "a list name": edited(lambda t: t["tiers"][1].update(name=["Normal"])),
                    "tiers an object": edited(lambda t: t.update(tiers={"small": t["tiers"][0]})),
                    "a string reserve": edited(lambda t: t["tiers"][1].update(reserve_bytes="8388608"))}
        with tempfile.TemporaryDirectory() as d:
            tools, folder = Path(d) / "tools", collect_folder(Path(d))
            tools.mkdir()
            shutil.copyfile(SCRIPT, tools / SCRIPT.name)

            def runs():
                out = []
                for args in ([FIXTURES / "selftest-v2-pass.txt"], [FIXTURES / "selftest-v2-pass.txt", "--json"],
                             ["--collect", folder], ["--collect", folder, "--json"]):
                    r = subprocess.run([sys.executable, str(tools / SCRIPT.name), *map(str, args)],
                                       capture_output=True, text=True, timeout=60)
                    out.append(r)
                return out

            shutil.copyfile(TIERS_JSON, tools / TIERS_JSON.name)
            plain, as_json, collected, collect_json = runs()
            for r in (plain, as_json, collected, collect_json):
                self.assertEqual((r.returncode, r.stderr), (0, ""))
            # the reference, less its tier tables: one in the report; in --collect two samples and the self-test's
            want_plain, n = TIER_BLOCK.subn("", plain.stdout)
            self.assertEqual(n, 1)
            want_collect, n = TIER_BLOCK.subn("", collected.stdout)
            self.assertEqual(n, 3)
            want_collect, n = GAME_TIER_LINE.subn("", want_collect)
            self.assertEqual(n, 1)
            # the reference --json, less what the tier table adds (its "tiers", and in --collect the
            # self-test's "tiers"/"tiers_all" and the finding), so a dropped field (stages, tiers_all)
            # fails per variant below, not only the verdict
            self.assertIn("tiers", json.loads(as_json.stdout))
            want_plain_json = without_tiers(json.loads(as_json.stdout))
            # --collect --json: "tiers_all" is [] without a table; set the reference's to [] and check each
            # variant's is [] too, so dropping it (or making it null) fails
            want_collect_json = without_tiers(json.loads(collect_json.stdout))
            self.assertTrue(want_collect_json.get("tiers_all"))  # the reference has samples
            want_collect_json["tiers_all"] = []
            for name, text in variants.items():
                with self.subTest(name):
                    if text is None:
                        (tools / TIERS_JSON.name).unlink(missing_ok=True)
                    else:
                        (tools / TIERS_JSON.name).write_text(text, encoding="utf-8")
                    plain, as_json, collected, collect_json = runs()
                    for r in (plain, as_json, collected, collect_json):
                        self.assertEqual((r.returncode, r.stderr), (0, ""))
                        self.assertNotIn("model tiers (", r.stdout)
                        self.assertNotIn("recommendation", r.stdout)
                        self.assertNotIn(d, r.stdout)  # the reason names the file, not where it is
                    self.assertTrue(plain.stdout.rstrip().endswith("PASS: every stage completed; the figures above "
                                                                   "are this console's."))
                    got, n = NOT_PRICED.subn("", plain.stdout)
                    self.assertEqual((got, n), (want_plain, 1))
                    got, n = NOT_PRICED.subn("", collected.stdout)
                    self.assertEqual((got, n), (want_collect, 2))  # the self-test's finding, and "not decided here"
                    out = json.loads(as_json.stdout)
                    self.assertEqual((out["ocr_result"], out["diagnostics_result"]), ("PASS", "PASS"))
                    self.assertNotIn("tiers", out)
                    self.assertEqual(sum(f.startswith("The model tiers were not priced: ") for f in out["findings"]), 1)
                    # the whole --json, not just the verdict and finding: dropping stages or tiers_all fails here
                    self.assertEqual(without_tiers(out), want_plain_json)
                    collect_out = json.loads(collect_json.stdout)
                    self.assertEqual(collect_out["tiers_all"], [])  # [] without a table, not null
                    self.assertEqual(without_tiers(collect_out), want_collect_json)
            # a fault of any other kind while the table loads (here json.loads raising) is caught the same way
            shutil.copyfile(TIERS_JSON, tools / TIERS_JSON.name)
            runner = Path(d) / "with_a_fault.py"
            runner.write_text("import json, runpy, sys\n"
                              "def fault(*args, **kwargs):\n"
                              "    raise RuntimeError('a fault of another kind')\n"
                              "json.loads = fault\n"
                              "sys.argv = sys.argv[1:]\n"
                              "runpy.run_path(sys.argv[0], run_name='__main__')\n", encoding="utf-8")
            r = subprocess.run([sys.executable, str(runner), str(tools / SCRIPT.name), str(FIXTURES / "selftest-v2-pass.txt")],
                               capture_output=True, text=True, timeout=60)
            self.assertEqual((r.returncode, r.stderr), (0, ""))
            self.assertIn("\n  - The model tiers were not priced: model_tiers.json: RuntimeError: a fault of another kind.\n",
                          r.stdout)
            self.assertEqual(NOT_PRICED.subn("", r.stdout), (want_plain, 1))

    def test_the_release_ships_the_table_beside_the_analyser(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        copy = [line for line in cmake.splitlines() if "${ROOT}/tools/analyse_selftest.py" in line]
        self.assertEqual(len(copy), 1, copy)
        self.assertIn(" ${ROOT}/tools/model_tiers.json ", copy[0])
        self.assertTrue(copy[0].rstrip().endswith(" ${STAGE}/tools/"), copy[0])

    def test_the_self_test_doc_shows_the_analysers_table(self):
        doc = (ROOT / "docs/self-test.md").read_text(encoding="utf-8")
        for r in a.tier_table(0)["rows"]:
            row = (f"| {r['tier']} | {r['weights']:,} B | < {r['box_ms'] / 1000:g} s | {r['arena']:,} B | "
                   f"{r['need']:,} B | {r['reserve'] >> 20} MiB | {r['need'] + r['reserve']:,} B |")
            if row not in doc:
                self.fail(f"docs/self-test.md's tier table has no row {row!r}")


# The kernel memory routes exactly as #75's first guard (7bba2aa: only pointers at or above 0x40000000
# were read) answered on a console, where every kernel object is below 0x40000000: the audit's B1.
B1_KMEM = (
    (re.compile(r"^(kmem \w+ route=block part=\w+) rc=0x00000000 step=0 total=\d+ free=\d+$", re.M),
     r"\1 rc=0xBFFFFFFA step=3 total=0 free=0", 8),  # kernel and shell, at boot, ok, game, done
    (re.compile(r"^(kmem \w+ route=cas part=\w+) rc=0x00000000 step=0 .*$", re.M),
     r"\1 rc=0xBFFFFFFA step=3 main=0/0 cdram=0/0 phycont=0/0", 4),
    (re.compile(r"^(kmem \w+ route=as part=\w+) rc=0x00000000 .*$", re.M),
     r"\1 rc=0xBFFFFFFB step=3 parts=0 skipped=2", 4),
)


def with_b1_routes(text):
    for pattern, repl, count in B1_KMEM:
        text, n = pattern.subn(repl, text)
        assert n == count, (pattern.pattern, n)
    return text


class DiagnosticsNeverDecide(unittest.TestCase):
    """A diagnostic that read nothing or did not work is a WARN: it can never turn a working OCR into FAIL
    (#75's first build would have: its kernel memory guard was inverted)."""

    def fixture(self, name):
        return (FIXTURES / f"selftest-v2-{name}.txt").read_text()

    def test_the_parallel_row_compares_textbox_with_textbox2(self):
        """API 22: textbox with the configured helpers, textbox2 the same region serially, neither cached."""
        text = self.fixture("pass")
        self.assertRegex(text, r"(?m)^stage textbox begin .* speed=0xA$")   # two helpers, no line cache
        self.assertRegex(text, r"(?m)^stage textbox2 begin .* speed=0x8$")  # serial, no line cache
        r = run(text)
        self.assertIn("textbox with 2 helper(s), 2 line(s) on them; their threads' share of a core: lowest 95 %, "
                      "mean 98 %: 820 ms recognizing; textbox2, the same region serially: 1500 ms; the same text",
                      r.stdout)
        # ocr_threads = 1: both serial, nothing to compare but the texts
        r = run(re.sub(r"(?m)^(stage textbox begin .* speed=)0xA$", r"\g<1>0x8", text))
        self.assertEqual(rows(r.stdout)["parallel"], "PASS")
        self.assertIn("ocr_threads = 1: both stages ran serially", r.stdout)
        # an older build's stages carry no speed: no row
        r = run(re.sub(r" speed=0x[0-9A-F]+$", "", text, flags=re.M))
        self.assertNotIn("parallel", rows(r.stdout))

    def test_the_row_kinds_are_fixed(self):
        rows_json = json.loads(run(self.fixture("pass"), "--json").stdout)["rows"]
        kinds = {r["row"]: r["kind"] for r in rows_json}
        self.assertEqual({n for n, k in kinds.items() if k == "diagnostic"}, set(a.DIAGNOSTIC_ROWS))
        self.assertEqual({n for n, k in kinds.items() if k == "ocr"},
                         (set(a.ROWS_V2) - {"probe"}) | {"recognitions"})

    def test_every_diagnostic_warns_and_ocr_still_passes(self):
        r = run(self.fixture("diagnostics"))
        self.assertEqual(r.returncode, 0, r.stdout)
        got = rows(r.stdout)
        self.assertEqual({k for k, v in got.items() if v == "WARN"}, {"probe", "capture<64KiB", "kernel-pool",
                                                                     "kernel-memory"})
        self.assertEqual({k for k, v in got.items() if v not in ("PASS", "WARN")}, set())
        self.assertEqual(failed_cells(r.stdout), [])
        self.assertIn("block kernel 0xBFFFFFFA argument at step 3 (pointer outside the kernel range)", r.stdout)
        self.assertIn("fullscreen 262,144 B of cdram (USER_RW refused it there, and CDRAM allocates in 256 KiB units)",
                      r.stdout)
        self.assertIn("the kernel strip (<= 64 KiB of the kernel pool) was used in: fallback, textbox, fullscreen, "
                      "textbox2", r.stdout)
        self.assertIn("the display hook could not write the game's USER memory: worked: kstrip->user, user->user, "
                      "kstrip->cdram; failed: hook->user 0xBFFFFFF9 copy", r.stdout)
        self.assertIn("diagnostics: WARN (probe, capture<64KiB, kernel-pool, kernel-memory)", r.stdout)
        self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("PASS: every stage completed"))
        as_json = json.loads(run(self.fixture("diagnostics"), "--json").stdout)
        self.assertEqual((as_json["ocr_result"], as_json["diagnostics_result"]), ("PASS", "WARN"))
        self.assertEqual(as_json["warnings"], ["probe", "capture<64KiB", "kernel-pool", "kernel-memory"])

    def test_warnings_never_soften_an_ocr_failure(self):
        text = re.sub(r"^(stage textbox2 end) rc=0 ", r"\1 rc=-103 ", self.fixture("diagnostics"), flags=re.M)
        r = run(text)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout)["textbox2"], "FAIL")
        self.assertIn("FAIL: textbox2 failed", r.stdout)

    def test_the_b1_console_outcome_is_a_warning_not_a_fail(self):
        """The first audit put 7bba2aa's route results into the pass fixture: exit 1, kernel-memory FAIL."""
        r = run(with_b1_routes(self.fixture("pass")))
        self.assertEqual(r.returncode, 0, r.stdout)
        got = rows(r.stdout)
        self.assertEqual(got["kernel-memory"], "WARN")
        self.assertEqual({k for k, v in got.items() if v != "PASS"}, {"kernel-memory"})
        for route in ("block kernel 0xBFFFFFFA", "block shell 0xBFFFFFFA", "cas 0xBFFFFFFA", "as 0xBFFFFFFB"):
            self.assertIn(route, r.stdout)
        self.assertIn("no KD figure or no user figure at: boot, ok, game, done", r.stdout)
        self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("PASS"))

    def test_no_diagnostic_row_can_fail(self):
        base = self.fixture("pass")
        cases = {
            "kernel-memory": [  # vjoMemInfo refused outright at every step (route=none)
                (r"^kmem (\w+) route=block part=kernel .*$", r"kmem \1 route=none rc=0xBFFFFFF8"),
                (r"^kmem \w+ route=(block part=shell|cas|as) .*\n", "")],
            "game-memory": [(r"^(gstep \w+ mem kind=- arg=0) rc=0x00000000 (.*) main=\d+ ",
                             r"\1 rc=0x80020001 \2 main=-1 ")],
            "probe": [(r"^(gstep probe probe kind=\w+ arg=\w+) rc=0x0000003E ", r"\1 rc=0xBFFFFFF9 ")],
            "capture<64KiB": [(r"^(result textbox .* capture=)61440 ", r"\g<1>69632 ")],
            "kernel-pool": [(r"^(result textbox2 source=)hook ", r"\1kstrip ")],
            "parallel": [(r"^(stage textbox2 end .* text=)0x[0-9A-Fa-f]+$", r"\g<1>0x00000001")],  # another reading
        }
        for name, subs in cases.items():
            text = base
            for pattern, repl in subs:
                text, n = re.subn(pattern, repl, text, flags=re.M)
                self.assertGreater(n, 0, (name, pattern))
            r = run(text)
            with self.subTest(name):
                self.assertEqual(r.returncode, 0, r.stdout)
                self.assertEqual(rows(r.stdout)[name], "WARN", r.stdout)
                self.assertEqual(failed_cells(r.stdout), [])
                self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("PASS"))
        # the user API itself failing in SceShell: a WARN cell and a kernel-memory WARN
        text = re.sub(r"^(mem ok) rc=0x00000000 ", r"\1 rc=0x80020001 ", base, flags=re.M)
        r = run(text)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("user API (sceKernelGetFreeMemorySize)    WARN  0x80020001", r.stdout)
        self.assertEqual(rows(r.stdout)["kernel-memory"], "WARN")

    def test_a_missing_ocr_stage_with_done_present_is_a_failure(self):
        """report() drops a line it cannot write (shell/selftest.c): a 'done' run missing an OCR
        stage's records is a failure, not 'not reached'."""
        base = self.fixture("pass")
        # drop every line of the textbox2 stage (begin..its records), keep 'done'
        lines = base.splitlines(keepends=True)
        kept = [ln for ln in lines if " textbox2 " not in ln and not ln.startswith("stage textbox2 ")]
        r = run("".join(kept))
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout)["textbox2"], "FAIL")
        self.assertIn("no record though the run finished", r.stdout)
        self.assertEqual(json.loads(run("".join(kept), "--json").stdout)["ocr_result"], "FAIL")

    def test_a_stop_in_the_probe_is_never_a_warning(self):
        text = self.fixture("pass")
        text = text[:text.index("\n", text.index("stage probe begin")) + 1]  # the Vita stopped in the probe
        r = run(text)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout)["probe"], "FAIL")
        self.assertIn("INCOMPLETE: stage 'probe' began and never wrote its record", r.stdout)
        self.assertIn("diagnostics: not all reached", r.stdout)  # a partial run has no full diagnostic table
        j = json.loads(run(text, "--json").stdout)
        self.assertEqual((j["ocr_result"], j["diagnostics_result"]), ("INCOMPLETE", "INCOMPLETE"))


class GameStartLines(unittest.TestCase):
    """log.txt's "game ... started" lines (shell/worker.c on_game_start): before the online dictionaries'
    removal they named the API key's state, "(key set|MISSING)"; since, they do not (audit N6, M24)."""

    OLD = ("game PCSG00415 started: dictionary Local dictionaries (key MISSING), trigger l+r, subtitles select+r, "
           "ocr_mode auto\n")
    NEW = "game PCSG00415 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto\n"
    RELAY = "game NPXS10028 started: dictionary Hachidori relay, trigger select, subtitles select+l, ocr_mode on_press\n"

    def test_both_forms_are_read(self):
        for text in (self.OLD, self.NEW):
            lg = a.summarise_log(text)
            self.assertEqual(lg["games"], {"PCSG00415": {"starts": 1, "ocr_mode": "auto",
                                                         "dictionary": "Local dictionaries"}}, text)
        lg = a.summarise_log(self.OLD + self.NEW + self.RELAY)
        self.assertEqual(lg["games"], {"PCSG00415": {"starts": 2, "ocr_mode": "auto", "dictionary": "Local dictionaries"},
                                       "NPXS10028": {"starts": 1, "ocr_mode": "on_press",
                                                     "dictionary": "Hachidori relay"}})

    def test_the_shell_writes_the_new_form(self):
        # the format string the analyser reads, as shell/worker.c has it: the dictionary's name is
        # core/config.h's VJO_DICT_NAME, the only dictionary since the Hachidori relay's removal
        worker = (ROOT / "shell/worker.c").read_text(encoding="utf-8")
        self.assertIn('vjo_log("game %s started: dictionary " VJO_DICT_NAME ", trigger %s, subtitles %s, ocr_mode %s"',
                      worker)
        self.assertIn('#define VJO_DICT_NAME "Local dictionaries"\n', (ROOT / "core/config.h").read_text(encoding="utf-8"))
        self.assertNotIn("(key %s)", worker)
        line = "game PCSG00415 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto\n"
        self.assertEqual(a.summarise_log(line)["games"],
                         {"PCSG00415": {"starts": 1, "ocr_mode": "auto", "dictionary": "Local dictionaries"}})

class OverlayCloseLines(unittest.TestCase):
    """log.txt's "overlay closed" lines (shell/overlay.cpp close_page), before and after row navigation (#81)."""

    OLD = "overlay closed: 1158 frames, 19 word moves, stick 71, slowest frame 2 ms\n"
    NEW = ("overlay closed: 993 frames, 6 word moves, stick 2, slowest frame 36 ms, 4 row moves, 2 wraps, 1 without layout, "
           "slowest key 412 us\n")
    NEW_SLOWER = ("overlay closed: 10 frames, 1 word moves, stick 0, slowest frame 3 ms, 1 row moves, 0 wraps, 0 without layout, "
                  "slowest key 1250 us\n")

    def test_an_older_build_reads_as_before(self):
        lg = a.summarise_log(self.OLD)
        self.assertEqual(lg["overlay"], [2])
        self.assertNotIn("arrows", lg)
        self.assertEqual(a.overlay_summary(lg), "  overlay opened 1x, slowest frame 2 ms")

    def test_the_arrow_keys_are_counted(self):
        lg = a.summarise_log(self.OLD + self.NEW + self.NEW_SLOWER + self.NEW)
        self.assertEqual(lg["overlay"], [2, 36, 3, 36])
        self.assertEqual(lg["arrows"], {"closes": 3, "row_moves": 9, "wraps": 4, "without_layout": 2,
                                        "slowest_key_us": 1250})
        self.assertEqual(a.overlay_summary(lg), "  overlay opened 4x, slowest frame 36 ms; arrows (3 of them): "
                                                "9 row changes, 4 row wraps, 2 before the header's layout, "
                                                "slowest key 1,250 us")

    # #104: shell/worker.c close_overlay_for's reason and shell/overlay.cpp's hand-back of the touch region
    PS = ("overlay close (game left) t=1791614286 ms: input block off rc=0\n"
          "touch region left to SceShell t=1791614299 ms\n" + OLD)
    CIRCLE = ("overlay close (in game) t=1791620000 ms: input block off rc=0\n"
              "touch region off: 0x0 t=1791620016 ms\n" + OLD)
    SWITCH = "overlay close (foreground changed) t=1791630000 ms: input block off rc=-1073741816\n" + OLD

    def test_closes_are_counted_by_reason(self):
        lg = a.summarise_log(self.PS + self.CIRCLE + self.PS + self.SWITCH)
        self.assertEqual(lg["closes"], {"in game": 1, "game left": 2, "foreground changed": 1, "touch_left": 2,
                                        "block_fail": 1})
        self.assertEqual(a.overlay_summary(lg), "  overlay opened 4x, slowest frame 2 ms; closed 1x in game, "
                                                "2x as the game left (PS or exit; touch region left to SceShell 2x), "
                                                "1x for another process in front, 1 input block release(s) refused")

    def test_a_build_before_104_has_no_close_reasons(self):
        lg = a.summarise_log("touch region off: 0x0\n" + self.OLD)
        self.assertNotIn("closes", lg)
        self.assertEqual(a.overlay_summary(lg), "  overlay opened 1x, slowest frame 2 ms")


def export_line(what="queued", rc=0, cards=2, moved=2, kept=0, unreadable=0, too_large=0, left=0, stopped=0):
    """settings/anki_page.c worker_main's line, as the Overlay app writes it into log.txt."""
    return (f"overlay app: anki: export ({what}) rc={rc}: {cards} cards, {moved} moved, {kept} kept, {unreadable} "
            f"unreadable, {too_large} too large, {left} left, {stopped} stopped, {cards * 20480} bytes, arena peak 98304\n")


class AnkiExportLines(unittest.TestCase):
    """The Overlay app's .apkg export in log.txt (settings/anki_page.c logs each export's result and counts,
    settings/anki_export.c its steps). A successful export is reported as one and is not a problem line
    (audit-test-build-3 N1: TROUBLE's "unreadable" and "too large" matched its zero counts); a failed export, or one
    that left cards queued for a problem, is one, as the app itself shows it (its result_error)."""

    VERIFIED = ("overlay app: anki export: ux0:data/VitaJPOverlay/anki/2026-10-16_120501-2cards.apkg: 2 cards, "
                "0 screenshots, 40960 bytes, verified\n")
    NOT_VERIFIED = ("overlay app: anki export: ux0:data/VitaJPOverlay/anki/2026-10-16_120601.apkg.tmp did not verify "
                    "(-114): removed, nothing cleared\n")

    def test_a_successful_export_is_a_success_and_not_a_problem(self):
        lg = a.summarise_log(self.VERIFIED + export_line())
        self.assertEqual(lg["trouble"], {})
        an = lg["anki"]
        self.assertEqual((an["exports"], an["export_ok"], an["export_fail"], an["exported"]), (1, 1, 0, 2))
        self.assertEqual(a.export_summary(an), ["  Anki exports (the Overlay app): 1 export(s): 1 succeeded (a package "
                                                "written and checked), 0 failed; 2 card(s) in the packages"])

    def test_a_failed_export_and_its_reason_are_problems(self):
        lg = a.summarise_log(self.NOT_VERIFIED + export_line("all", rc=-114, cards=0, moved=0))
        self.assertEqual(sorted(lg["trouble"]), [key[:120] for key in (  # a problem line is kept to 120 characters
            "overlay app: anki export: uxN:data/VitaJPOverlay/anki/N-N-N_N.apkg.tmp did not verify (-N): removed, "
            "nothing cleared",
            "overlay app: anki: export (all) rc=-N: N cards, N moved, N kept, N unreadable, N too large, N left, N "
            "stopped, N bytes, arena peak N")])
        self.assertEqual(a.export_summary(lg["anki"]), [
            "  Anki exports (the Overlay app): 1 export(s): 0 succeeded (a package written and checked), 1 FAILED "
            "(no package); 0 card(s) in the packages",
            "    all: local format (rc -114): no package, the queue unchanged"])

    def test_cards_a_problem_left_queued_are_a_problem(self):
        lg = a.summarise_log(export_line(cards=5, moved=3, kept=1, unreadable=2))
        self.assertEqual(len(lg["trouble"]), 1)
        self.assertEqual(a.export_summary(lg["anki"]), [
            "  Anki exports (the Overlay app): 1 export(s): 0 succeeded (a package written and checked), 1 wrote its "
            "package and left cards queued by a problem, 0 failed; 5 card(s) in the packages",
            "    queued: 5 card(s) in its package; still queued by a problem: 1 kept (in the package; a move failed), "
            "2 unreadable, 0 too large"])
        lg = a.summarise_log(export_line(cards=1, moved=1, too_large=1))
        self.assertEqual((lg["anki"]["export_partial"], len(lg["trouble"])), (1, 1))
        lg = a.summarise_log(export_line(cards=2, moved=1, kept=1))  # a move failed: the card stays queued
        self.assertEqual((lg["anki"]["export_partial"], len(lg["trouble"])), (1, 1))

    def test_cards_a_stop_or_the_cap_left_queued_are_not_a_problem(self):
        # stopped: the user stopped the queue's clearing; left: over a package's cap. Both are in the next export.
        lg = a.summarise_log(export_line(cards=4, moved=1, left=3, stopped=3) + export_line(cards=0, moved=0))
        self.assertEqual(lg["trouble"], {})
        self.assertEqual(a.export_summary(lg["anki"]), [
            "  Anki exports (the Overlay app): 2 export(s): 1 succeeded (a package written and checked), 1 had no card "
            "to export, 0 failed; 4 card(s) in the packages; still queued for the next export, not a problem: 3 a "
            "stop left, 3 over a package's cap"])

    def test_no_export_and_an_older_line_without_its_stop_count(self):
        self.assertEqual(a.export_summary(a.summarise_log("anki: queue 1 card(s)\n")["anki"]),
                         ["  Anki exports (the Overlay app): none in the log"])
        old = ("overlay app: anki: export (queued) rc=0: 3 cards, 3 moved, 0 kept, 0 unreadable, 0 too large, 0 left, "
               "40960 bytes, arena peak 1000\n")
        lg = a.summarise_log(old)
        self.assertEqual((lg["anki"]["export_ok"], lg["trouble"]), (1, {}))

    def test_the_app_writes_the_lines_the_analyser_reads(self):
        page = (ROOT / "settings/anki_page.c").read_text(encoding="utf-8")
        fmt = re.search(r'vjo_log\("(anki: export \(%s\) rc=%d: [^"]*)"\s*"([^"]*)"', page)
        self.assertIsNotNone(fmt, "settings/anki_page.c's export line moved: update LOG['anki_export']")
        line = "overlay app: " + (fmt.group(1) + fmt.group(2)) % ("queued", -113, 5, 3, 1, 2, 4, 6, 7, 1000, 2000)
        m = a.LOG["anki_export"].match(line)
        self.assertEqual([m.group(g) for g in range(1, 10)], ["queued", "-113", "5", "3", "1", "2", "4", "6", "7"])
        # the app's own idea of an error, which export_result follows
        self.assertIn("result_error = r.rc != 0 || r.kept || r.unreadable || r.too_large;", page)
        export = (ROOT / "settings/anki_export.c").read_text(encoding="utf-8")
        for words in ('"anki export: %s did not verify (%d): removed, nothing cleared"',
                      '"anki export: %s: 99 packages are named for this second already"',
                      '"anki export: %s: its screenshot %s.jpg is missing or not a JPEG"',
                      '"anki export: %s: %d cards, %d screenshots, %u bytes, verified"'):
            self.assertIn(words, export)
        self.assertTrue(a.EXPORT_TROUBLE.search(
            "overlay app: anki export: ux0:data/VitaJPOverlay/anki: 99 packages are named for this second already"))
        self.assertIsNone(a.EXPORT_TROUBLE.search("overlay app: " + self.VERIFIED.strip()))


def game_job(lines, clock=None, model="VOCR3_H15_w8.vocr", late=True, why=None, helpers=None, share=0):
    """One game job's batch as shell/worker.c logs it (the job line, its lines' crops, frames and times, then the
    game worker's steps). lines: [(frames, ms)], a line from the line cache as (frames, 0): core/vocr_ocr.c logs
    its frames and 0.0 ms. clock: (before, rc, during, after). late: the steps the worker adds after the clock's
    place (game_ocr/main.c: FIND, then RECOGNIZE, once the run that logs the clock has returned); "find": a step log
    that filled between the two; False: one that filled before both. why: the Shell job's reason, its "job start"
    line first (shell/worker.c start_job_kind). helpers: the job's PAR step (none for 0: a serial job with no line
    from the cache logs none), then its workspace's free and the kernel's PACE, as API 22 logs them after
    RECOGNIZE; share: PAR's mean share of a core (0: not known)."""
    frames = sum(f for f, t in lines if t)
    out = [f"job start ({why})"] if why else []
    out += [f"game vocr rc=0 {model}: 883x112 at 46,416 of 960x544 fmt 0x00000000, lines={len(lines)} USER "
           f"workspace=1040384 weights=resident resident=1159168, find=92 ms load=1 ms recognize=180 ms "
           f"frames={frames} io=0x00000000 stat=0x00000000 cleanup=0",
           "game vocr lines:" + "".join(f" 742x39 {f}f {t:.1f}ms c900" for f, t in lines),
           "gstep job mem kind=- arg=0 rc=0x00000000 bytes=0 sum=0x00000000 main=86921216 cdram=0 phycont=0 us=17"]
    if clock:
        before, rc, during, after = clock
        out.append(f"gstep job clock kind=- arg={before} rc=0x{rc & 0xFFFFFFFF:08X} bytes={during} sum=0x{after:08X} "
                   "main=0 cdram=0 phycont=0 us=300000")
    if late:
        out.append(f"gstep job find kind=- arg={len(lines)} rc=0x00000000 bytes=0 sum=0x00000000 main=0 cdram=0 "
                   "phycont=0 us=92000")
    if late is True:
        out.append(f"gstep job recognize kind=- arg={len(lines)} rc=0x00000000 bytes={frames} sum=0x00000000 main=0 "
                   "cdram=0 phycont=0 us=180000")
    if late is True and helpers is not None:
        cached = sum(1 for _, t in lines if not t)
        if helpers or cached:
            out.append(f"gstep job par kind=- arg={helpers} rc=0x{min(helpers, len(lines) - 1):08X} bytes=533504 "
                       f"sum=0x00{cached:02X}{share:02X}{share:02X} main=0 cdram=0 phycont=0 us=180000")
        out += ["gstep job free kind=- arg=workspace rc=0x00000000 bytes=1040384 sum=0x00000000 main=86859776 "
                "cdram=12582912 phycont=27262976 us=487575",
                "gstep job pace kind=- arg=12 rc=0x00004E20 bytes=200400 sum=0x00000000 main=1970600 cdram=17000 "
                "phycont=0 us=118"]
    return "\n".join(out) + "\n"


def auto_job(lines, clock=None, helpers=2, **kw):
    """An ordinary job of test.3's defaults: Auto's background job of a settled screen (the overlay closed), two
    helpers (ocr_threads = 3)."""
    return game_job(lines, clock, why="auto: screen settled", helpers=helpers, **kw)


def stage_job(name, lines, clock=None, helpers=2, model="VOCR3_H15_w8.vocr"):
    """A self-test stage as the Shell logs it in log.txt (shell/selftest.c, shell/worker.c run_selftest_stages): its
    begin record, its job's batch, its end record. textbox2 runs serially (vjo_selftest_stage_speed)."""
    speed = (helpers or 0) | (0x4 if clock else 0) | 0x8
    frames = sum(f for f, _ in lines)
    return (f"stage {name} begin t=90000 flags=0x0 model=VOCR3_H15_w8.vocr fallback=VOCR3_FL10_w8.vocr "
            f"speed=0x{speed:X}\n" + game_job(lines, clock, model=model, helpers=helpers)
            + f"line {name} i=0 w=742 h=39 frames={lines[0][0]} us={int(lines[0][1] * 1000)}\n"
            f"stage {name} end rc=0 t=90700 ms=700 region=883x112+46+416 fb=960x544/960/0x00000000 model={model} "
            f"fallback=0 lines={len(lines)} recognized={len(lines)} frames={frames} text=0x2B7E1516\n")


BOOT22 = "Vita JP Overlay shell started (kernel API 22)\n"
START = "game PCSG00415 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto\n"
RAISED = (333, 0, 444, 333)


class ClockWindows(unittest.TestCase):
    """ocr_boost_clock in log.txt, per game window (audit-test-build-3-b D6; test.3's step 6). A job logs the clock
    (`gstep job clock`) only when it recognizes a line: game_ocr/main.c job_busy is the busy hook core/vocr_ocr.c
    calls at the first line it runs, after the line cache's lookup, so a job whose every line came from the cache
    logs none, boost on or off. A window, from a `game TITLE started` line (a start or a resume; shell/worker.c
    activate_game reads config.ini again just before it) to the next, is read from its jobs that recognized a line,
    and the time per CTC frame is each recognized line's own."""

    def test_a_job_from_the_cache_does_not_make_a_window_boost_off(self):
        log = (BOOT22 + START + game_job([(80, 176.0)], RAISED) + game_job([(80, 0.0)])  # 1: on, then the cache
               + START + game_job([(80, 0.0), (76, 0.0)])                                # 2: a resume, the cache only
               + START + game_job([(80, 232.8)]) + game_job([(76, 220.4)]) + game_job([(64, 0.0)])  # 3: restarted, off
               + START + game_job([(64, 140.8)], RAISED))                                # 4: on again
        lg = a.summarise_log(log)
        rows = lg["clock_windows"]
        self.assertEqual([(r["window"], r["state"], r["jobs"], r["recognizing"], r["clock_lines"]) for r in rows],
                         [(1, "on", 2, 1, 1), (2, "none", 1, 0, 0), (3, "off", 3, 2, 0), (4, "on", 1, 1, 1)])
        self.assertEqual([r["line"] for r in rows], [2, 14, 20, 36])
        self.assertEqual((rows[0]["before"], rows[0]["during"], rows[0]["after"], rows[0]["raised"]),
                         ([333], [444], [333], 1))
        # per CTC frame, each recognized line's own (frame-weighted); a cached line's 0.0 ms is left out
        self.assertEqual(rows[2]["per_frame"], {"VOCR3_H15_w8.vocr": {"jobs": 2, "lines": 2, "frames": 156, "us": 453200}})
        self.assertEqual(lg["clock_windows_older"], 0)
        cmp_ = lg["clock_compare"]["VOCR3_H15_w8.vocr"]
        self.assertEqual(cmp_["off"], {"jobs": 2, "selftest": 0, "lines": 2, "frames": 156, "us": 453200, "windows": [3]})
        self.assertEqual(cmp_["raised"], {"jobs": 2, "selftest": 0, "lines": 2, "frames": 144, "us": 316800,
                                          "windows": [1, 4], "before": 333, "during": 444})
        text = flat("\n".join(a.render_clock_windows(lg)))
        for want in ("ARM clock by game window (a window runs from a 'game TITLE started' line, a start or a resume, "
                     "where config.ini is read again, to the next; a job logs the clock only when it recognizes a "
                     "line, so one whose every line came from the cache logs none, boost on or off):",
                     "window from title jobs recognizing clock MHz before / during / after; us per CTC frame",
                     "1 line 2 PCSG00415 2 1 1 333 / 444 / 333 (raised in 1); VOCR3_H15_w8.vocr 2,200 us (1 job, 1 line)",
                     "2 line 14 PCSG00415 1 0 0 no job recognized a line: no clock either way",
                     "3 line 20 PCSG00415 3 2 0 BOOST OFF, clock assumed (not logged): no clock line in its 2 "
                     "recognizing jobs; VOCR3_H15_w8.vocr 2,905 us (2 jobs, 2 lines)",
                     "4 line 36 PCSG00415 1 1 1 333 / 444 / 333 (raised in 1); VOCR3_H15_w8.vocr 2,200 us (1 job, 1 line)",
                     "boost off against on, us per CTC frame (each recognized line's time / its frames, "
                     "frame-weighted; lines from the cache left out; a core's share: the recognizing threads' run "
                     "time / wall time, the jobs' means, under 100 when game threads had the core):",
                     "VOCR3_H15_w8.vocr, all jobs: off (assumed 333 MHz, not logged) 2,905 us (2 jobs, 2 lines; "
                     "window 3), raised 333 -> 444 MHz 2,200 us (2 jobs, 2 lines; windows 1, 4): off / raised = 1.32 "
                     "(a time that scales with the clock: 444 / 333 = 1.33; no gain: 1.00)"):
            self.assertIn(want, text)

    def test_a_window_with_and_without_the_clock_is_mixed_and_left_out_of_the_comparison(self):
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 176.0)], RAISED) + game_job([(80, 232.8)]))
        r = lg["clock_windows"][0]
        self.assertEqual((r["state"], r["clock_lines"], r["without_clock"]), ("mixed", 1, 1))
        self.assertIn("MIXED: 333 / 444 / 333 in 1 job; 1 recognizing job logged none",
                      flat("\n".join(a.render_clock_windows(lg))))
        self.assertEqual(lg["clock_compare"]["VOCR3_H15_w8.vocr"]["raised"]["lines"], 1)
        self.assertNotIn("off", lg["clock_compare"]["VOCR3_H15_w8.vocr"])

    def test_a_step_log_that_filled_before_the_clocks_place_is_no_evidence(self):
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 232.8)], late=False))
        self.assertEqual(lg["clock_windows"][0]["state"], "unknown")
        self.assertIn("not known: the step log of its 1 recognizing job filled before the clock's place",
                      flat("\n".join(a.render_clock_windows(lg))))
        self.assertEqual(lg["clock_compare"], {})
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 176.0)], RAISED) + game_job([(80, 232.8)], late=False))
        self.assertEqual((lg["clock_windows"][0]["state"], lg["clock_windows"][0]["unsure"]), ("on", 1))
        # FIND comes after the clock's place too: a log that filled before RECOGNIZE still had room for the clock
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 232.8)], late="find"))
        self.assertEqual(lg["clock_windows"][0]["state"], "off")

    def test_a_reboot_ends_a_window(self):
        # config.ini is read again at boot: jobs logged after a boot and before its first game start (a log that
        # rotated there, say) are not the previous boot's window
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 176.0)], RAISED) + BOOT22 + game_job([(80, 232.8)]))
        self.assertEqual([(r["window"], r["title"], r["state"]) for r in lg["clock_windows"]],
                         [(1, "PCSG00415", "on"), (2, None, "off")])
        self.assertIn("2 line 10 ? 1 1 0 BOOST OFF, clock assumed (not logged): no clock line in its 1 "
                      "recognizing job", flat("\n".join(a.render_clock_windows(lg))))

    def test_a_clock_already_at_444_or_more_is_left_and_compared_apart(self):
        # PSVshell's 500 (or the game's own 444): rc 1, never lowered; a set call refused (rc < 0) leaves it too
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 160.0)], (500, 1, 500, 500))
                             + game_job([(80, 176.0)], RAISED) + game_job([(80, 232.0)], (333, -2147352567, 333, 333)))
        r = lg["clock_windows"][0]
        self.assertEqual((r["state"], r["raised"], r["left"], r["not_raised"]), ("on", 1, 1, 1))
        text = flat("\n".join(a.render_clock_windows(lg)))
        self.assertIn("333-500 / 333-500 / 333-500 (raised in 1, left at 444 or more in 1, not raised in 1)", text)
        cmp_ = lg["clock_compare"]["VOCR3_H15_w8.vocr"]
        self.assertEqual((cmp_["left"]["before"], cmp_["left"]["lines"], cmp_["raised"]["lines"]), (500, 1, 1))
        self.assertIn("boost effective: unclear (ratio -, n=0): no window with the boost off in this log (step 6's "
                      "optional boxes); yes at 1.25 to 1.42, no at 1.08 or less", text)

    def test_off_against_a_clock_left_at_500_or_read_by_another_model(self):
        # PSVshell's 500 MHz for the game: the boost never raises it, so the off window compares with the left jobs
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 160.0)], (500, 1, 500, 500))
                             + START + game_job([(80, 232.8)]))
        self.assertEqual([r["state"] for r in lg["clock_windows"]], ["on", "off"])
        text = flat("\n".join(a.render_clock_windows(lg)))
        self.assertIn("VOCR3_H15_w8.vocr, all jobs: off (assumed 500 MHz, not logged) 2,910 us (1 job, 1 line; window 2); "
                      "left at 500 MHz (444 or more already) 2,000 us (1 job, 1 line; window 1)", text)
        self.assertIn("boost effective: unclear (ratio -, n=0): VOCR3_H15_w8.vocr; no job raised the clock: it was at "
                      "444 MHz or more already in 1 job (PSVshell's 500, or the game's own)", text)
        # the boost off read with Small's model, on with Normal's: per frame they are not comparable
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 176.0)], RAISED)
                             + START + game_job([(80, 120.0)], model="VOCR3_FL10_w8.vocr"))
        self.assertIn("boost effective: unclear (ratio -, n=0): no model was read both with the boost off and on",
                      flat("\n".join(a.render_clock_windows(lg))))

    PAR = "gstep job par kind=- arg=2 rc=0x00000001 bytes=533504 sum=0x0000{lo:02X}{mean:02X} main=0 cdram=0 phycont=0 us=1\n"

    def test_a_rotated_logs_window_is_read_by_its_api_22_steps(self):
        # log.old.txt's first window has no "shell started" line before it: its build is read from its steps
        start = START + game_job([(80, 232.8)]) + self.PAR.format(lo=90, mean=95)
        lg = a.summarise_log(start + START + game_job([(80, 176.0)], RAISED) + self.PAR.format(lo=97, mean=99))
        self.assertEqual([(r["window"], r["state"], r["api"]) for r in lg["clock_windows"]], [(1, "off", None), (2, "on", None)])
        g = lg["clock_compare"]["VOCR3_H15_w8.vocr"]
        self.assertEqual((g["off"]["share"], g["raised"]["share"]), ((95, 95), (99, 99)))
        self.assertIn("off (assumed 333 MHz, not logged) 2,910 us (1 job, 1 line; window 1; a core's share 95%), raised "
                      "333 -> 444 MHz 2,200 us (1 job, 1 line; window 2; a core's share 99%): off / raised = 1.32",
                      flat("\n".join(a.render_clock_windows(lg))))
        # without such a step (an older build's jobs), a window the log does not name is not listed
        lg = a.summarise_log(START + game_job([(80, 400.0)]))
        self.assertEqual((lg["clock_windows"], lg["clock_windows_older"]), ([], 1))

    def test_windows_of_builds_before_the_boost_are_not_listed(self):
        old = "Vita JP Overlay shell started (kernel API 21)\n" + START + game_job([(80, 400.0)])
        lg = a.summarise_log(old)
        self.assertEqual((lg["clock_windows"], lg["clock_windows_older"]), ([], 1))
        self.assertEqual(a.render_clock_windows(lg), [
            "  ARM clock by game window: no window of a build with the boost (API 22 on) has an OCR job; 1 window "
            "of older builds does (they log no clock)"])
        # a later boot of an API 22 build is listed; the older windows are counted
        lg = a.summarise_log(old + BOOT22 + START + game_job([(80, 176.0)], RAISED))
        self.assertEqual(([r["window"] for r in lg["clock_windows"]], lg["clock_windows_older"]), ([2], 1))
        self.assertIn("(1 window of builds before API 22, which have no boost and log no clock, is not listed)",
                      flat("\n".join(a.render_clock_windows(lg))))
        self.assertEqual(a.render_clock_windows(a.summarise_log("anki: queue 1 card(s)\n")), [])

    def test_no_clock_line_anywhere_is_said_to_be_off_or_unavailable(self):
        lg = a.summarise_log(BOOT22 + START + game_job([(80, 232.8)]))
        text = flat("\n".join(a.render_clock_windows(lg)))
        self.assertIn("no clock line in its 1 recognizing job: the boost off, or the game could not call ScePower "
                      "(no job in this log logged the clock)", text)
        self.assertIn("boost effective: unclear (ratio -, n=0): no job raised the clock", text)

    def test_the_busy_hook_runs_only_for_a_line_to_recognize(self):
        # the premise: the clock's step is written by job_busy, which core/vocr_ocr.c calls after the cache lookup,
        # for the first line it has to run, and which returns at once without ocr_boost_clock
        core = (ROOT / "core/vocr_ocr.c").read_text(encoding="utf-8")
        hit, slot = core.index("cur.kind = LN_HIT;"), core.index("cur.kind = LN_SLOT;")
        busy = core.index("pol->busy(pol->busy_ud, 1);")
        self.assertLess(hit, slot)
        self.assertLess(slot, busy)
        self.assertEqual(core.count("pol->busy(pol->busy_ud, 1);"), 1)
        worker = (ROOT / "game_ocr/main.c").read_text(encoding="utf-8")
        body = worker[worker.index("static void job_busy(void *ud, int on)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("if (!boost_job ||", body)
        self.assertIn("step_add(VJO_GSTEP_CLOCK,", body)
        # a window's config: activate_game reads config.ini again, then logs "game TITLE started"
        shell = (ROOT / "shell/worker.c").read_text(encoding="utf-8")
        body = shell[re.search(r"\nstatic void activate_game\(", shell).start():]
        body = body[:body.index("\n}\n")]
        self.assertLess(body.index("apply_config();"), body.index('vjo_log("game %s started: '))

    def test_the_test3_style_fixture_reads_its_boost_off_window_plainly(self):
        # tools/tests/fixtures/log-test3-style.txt: synthetic (no console, no game), test.3's checklist in order:
        # boxes with the boost on and one from the cache, two cards and the Overlay app's export, the game resumed,
        # step 6's restart with ocr_boost_clock = off and 3 boxes, a restart with it on
        lg = a.summarise_log((FIXTURES / "log-test3-style.txt").read_text(encoding="utf-8"))
        self.assertEqual([(r["window"], r["state"], r["jobs"], r["recognizing"], r["clock_lines"])
                          for r in lg["clock_windows"]],
                         [(1, "on", 3, 2, 2), (2, "on", 2, 1, 1), (3, "off", 4, 3, 0), (4, "on", 1, 1, 1)])
        cmp_ = lg["clock_compare"]["VOCR3_H15_w8.vocr"]
        self.assertEqual((cmp_["off"]["lines"], cmp_["off"]["windows"], cmp_["raised"]["lines"],
                          cmp_["raised"]["windows"]), (9, [3], 11, [1, 2, 4]))
        # the lines' logged times: 1,922,100 us over 660 frames off, 1,797,500 us over 816 raised
        self.assertEqual((cmp_["off"]["us"], cmp_["off"]["frames"]), (1922100, 660))
        self.assertEqual((cmp_["raised"]["us"], cmp_["raised"]["frames"]), (1797500, 816))
        self.assertIn("off / raised = 1.32 (a time that scales with the clock: 444 / 333 = 1.33; no gain: 1.00)",
                      flat("\n".join(a.render_clock_windows(lg))))
        self.assertEqual((lg["trouble"], lg["anki"]["export_ok"]), ({}, 1))
        # the per-line span leaves the cache's lines (0.0 ms) out: 20 recognized of the 30 logged
        self.assertEqual((len(lg["line_ms"]), min(lg["line_ms"]), max(lg["line_ms"])), (20, 140, 233))


def per_frame(us, frames=(80, 76, 64)):
    """A job's recognized lines at `us` per CTC frame each: [(frames, ms)] as the Shell logs them (0.1 ms)."""
    return [(f, round(f * us / 1000, 1)) for f in frames]


def with_selftest_job(base, job):
    """The test.3-style log with the self-test's own job (API 22, #116) in window 1, after job 3's batch: where its
    stages run once a scan has read text and the overlay has stayed closed (shell/worker.c selftest_job_due)."""
    lines = base.splitlines()
    i = next(n for n, line in enumerate(lines) if line.startswith("job 3 done"))
    return "\n".join(lines[:i + 1] + job.splitlines() + lines[i + 1:]) + "\n"


def log_collect(root, log, tai=None):
    """A minimal collect folder (tools/collect_vita.py's layout): COLLECT.json, log.txt and, if given, ur0's taiHEN
    configuration."""
    data = root / "ux0/data/VitaJPOverlay"
    data.mkdir(parents=True)
    (root / "COLLECT.json").write_text(json.dumps({"collected_at": "synthetic", "host": "-", "missing": [],
                                                   "errors": []}), encoding="utf-8")
    (data / "log.txt").write_text(log, encoding="utf-8")
    if tai is not None:
        (root / "ur0/tai").mkdir(parents=True)
        (root / "ur0/tai/config.txt").write_text(tai, encoding="utf-8")
    return root


class BoostLikeForLike(unittest.TestCase):
    """test.3's step 6 decides VOCR5's Normal size (1.26 or 1.6 MB) by the boost-off / raised ratio of the time per
    CTC frame (audit-overlay-analyser-fixes N1-N3, N5). Pooled over every job, the raised side takes the self-test's
    stage jobs, textbox2 among them, which runs serially: if a line read beside two other recognizer threads is
    slower per frame than a serial one (the shared L2; unmeasured), they make the raised side faster and the ratio
    larger, toward 1.6 MB. So the ratio is also computed like for like: not the self-test's stages, between jobs of
    one kind on both sides (as many recognized lines, the same helpers, and the same trigger: Auto's job with the
    overlay closed, or the overlay's own). It is given per model, per window and against the off window's
    neighbours, with job counts; the off window's clock is said to be assumed, with what the logs hold about it; and
    a one-line verdict states its thresholds."""

    H15 = "VOCR3_H15_w8.vocr"
    FL10 = "VOCR3_FL10_w8.vocr"
    SELFTEST = (FIXTURES / "log-test3-selftest-job.txt").read_text(encoding="utf-8")
    BASE = (FIXTURES / "log-test3-style.txt").read_text(encoding="utf-8")

    def text(self, lg, tai=None):
        return flat("\n".join(a.render_clock_windows(lg, tai)))

    def test_the_auditors_variant_134_pooled_is_132_like_for_like(self):
        # audit-overlay-analyser-fixes v8: the self-test's stages in window 1, textbox2 (serial) invented 10 % faster
        # per frame; pooled, the raised side's 1,616 frames take 3,509,100 us (2,171 us a frame): 1.34 against the
        # off window's 2,912; its 3 auto jobs of 3 lines (2,201 us) against the off window's 3 (all of 3 lines): 1.32
        lg = a.summarise_log(with_selftest_job(self.BASE, self.SELFTEST))
        g = lg["clock_compare"][self.H15]
        self.assertEqual((g["raised"]["jobs"], g["raised"]["selftest"], g["raised"]["frames"], g["raised"]["us"]),
                         (7, 3, 1616, 3509100))
        self.assertEqual((g["off"]["jobs"], g["off"]["frames"], g["off"]["us"]), (3, 660, 1922100))
        self.assertEqual(round(_per(g["off"]) / _per(g["raised"]), 2), 1.34)
        p = lg["clock_like"][self.H15]["pooled"]
        self.assertEqual([(k["lines"], k["helpers"], k["trigger"], k["off"]["jobs"], k["raised"]["jobs"])
                          for k in p["strata"]], [(3, 2, "auto", 3, 3)])
        self.assertEqual((round(p["ratio"], 2), p["off"]["jobs"], p["raised"]["jobs"], p["raised_windows"]),
                         (1.32, 3, 3, [1, 4]))
        self.assertEqual(lg["clock_like"][self.H15]["left_out"],
                         {"selftest": 3, "reason_unknown": 0, "helpers_unknown": 0, "unmatched": 1})
        # the off window's neighbours with a job of its kind: windows 1 and 4 (window 2 read only 2 lines)
        (w3,) = [w for w in lg["clock_like"][self.H15]["windows"] if w["side"] == "off"]
        self.assertEqual((w3["window"], w3["neighbours"], round(w3["near"]["ratio"], 2)), (3, [1, 4], 1.32))
        (v,) = lg["boost_verdicts"]
        self.assertEqual((v["model"], v["verdict"], round(v["ratio"], 2), v["n"], round(v["all_jobs"], 2)),
                         (self.H15, "yes", 1.32, 6, 1.34))
        # window 1's row keeps the self-test's stages apart: FL10's fallback, then textbox, fullscreen and textbox2
        # (the probe recognizes nothing)
        r = lg["clock_windows"][0]
        self.assertEqual((r["per_frame"][self.H15]["jobs"], r["selftest_per_frame"][self.H15]["jobs"],
                          r["selftest_per_frame"][self.H15]["lines"], r["selftest_per_frame"][self.FL10]["jobs"]),
                         (2, 3, 13, 1))
        text = self.text(lg)
        for want in ("1 line 10 PCSG00415 8 6 6 333 / 444 / 333 (raised in 6); VOCR3_H15_w8.vocr 2,202 us (2 jobs, 6 "
                     "lines); the self-test's stages: VOCR3_FL10_w8.vocr 1,300 us (1 job, 3 lines), VOCR3_H15_w8.vocr "
                     "2,140 us (3 jobs, 13 lines)",
                     "VOCR3_H15_w8.vocr, all jobs: off (assumed 333 MHz, not logged) 2,912 us (3 jobs, 9 lines; window "
                     "3; a core's share 99%), raised 333 -> 444 MHz 2,171 us (7 jobs, 3 of them the self-test's stages, "
                     "24 lines; windows 1, 2, 4; a core's share 99%): off / raised = 1.34",
                     "VOCR3_H15_w8.vocr, like for like (not the self-test's stages; jobs of one kind on both sides: as "
                     "many recognized lines, the same helpers, and both auto (Auto's job, the overlay closed) or both "
                     "overlay (a press, a retry or a refresh, the overlay open); each kind weighted by the off side's "
                     "frames): off / raised = 1.32 (3 off, 3 raised jobs): 3 lines, 2 helpers, auto: off 2,912 us (3 "
                     "jobs; a core's share 99%), raised 2,201 us (3 jobs; a core's share 99%): 1.32; left out: 3 jobs "
                     "of the self-test's stages, 1 job of a kind not on the other side",
                     "boost effective: yes (ratio 1.32, n=6): VOCR3_H15_w8.vocr like for like, 3 off and 3 raised jobs "
                     "(all jobs: 1.34); yes at 1.25 to 1.42, no at 1.08 or less, unclear between them, above 1.42 or "
                     "with under 2 jobs a side (444 / 333 = 1.33 if the time scales with the clock, 1.00 without a "
                     "gain)"):
            self.assertIn(want, text)
        # v8b: textbox2 as fast as the parallel stages: 1.32 both ways
        lg = a.summarise_log(with_selftest_job(self.BASE, self.SELFTEST.replace(
            "742x39 80f 158.4ms c900 742x39 76f 150.5ms c900 742x39 64f 126.7ms c900",
            "742x39 80f 176.0ms c900 742x39 76f 167.2ms c900 742x39 64f 140.8ms c900")))
        (v,) = lg["boost_verdicts"]
        self.assertEqual((v["verdict"], round(v["ratio"], 2), round(v["all_jobs"], 2)), ("yes", 1.32, 1.32))

    def test_a_serial_stage_cannot_make_the_call(self):
        # invented: Auto's lines 1.22x slower with the boost off (unclear), the self-test's serial textbox2 25 % faster
        # per frame: pooled, 1.27 would read as "yes" (1.6 MB); like for like it stays 1.22, unclear (1.26 MB)
        st = ("job start (self-test)\n" + stage_job("textbox", per_frame(2400), RAISED)
              + stage_job("fullscreen", per_frame(2400, (80, 76, 64, 40, 40, 30, 30)), RAISED)
              + stage_job("textbox2", per_frame(1800), RAISED, helpers=0))
        log = (BOOT22 + START + auto_job(per_frame(2400), RAISED) + auto_job(per_frame(2400), RAISED) + st
               + START + "".join(auto_job(per_frame(2928)) for _ in range(3))
               + START + auto_job(per_frame(2400), RAISED))
        lg = a.summarise_log(log)
        self.assertEqual([r["state"] for r in lg["clock_windows"]], ["on", "off", "on"])
        (v,) = lg["boost_verdicts"]
        self.assertEqual((v["verdict"], round(v["ratio"], 2), v["n"], round(v["all_jobs"], 2)), ("unclear", 1.22, 6, 1.27))
        text = self.text(lg)
        self.assertIn("off / raised = 1.27", text)
        self.assertIn("boost effective: unclear (ratio 1.22, n=6): VOCR3_H15_w8.vocr like for like, 3 off and 3 raised "
                      "jobs (all jobs: 1.27); between the two thresholds; yes at 1.25 to 1.42", text)
        # the serial stage is its own kind (3 lines, no helper): it is never matched with Auto's jobs
        self.assertEqual(lg["clock_like"][self.H15]["left_out"], {"selftest": 3, "reason_unknown": 0,
                                                                  "helpers_unknown": 0, "unmatched": 0})
        # builds before #116 ran the stages inside the user's own job (an auto one): their begin records still
        # mark them; and the job after a stage is the user's again
        log = log.replace("job start (self-test)\n", "job start (auto: screen settled)\n")
        lg = a.summarise_log(log + START + auto_job(per_frame(2400), RAISED))
        self.assertEqual(lg["clock_like"][self.H15]["left_out"]["selftest"], 3)
        self.assertEqual((round(lg["clock_like"][self.H15]["pooled"]["ratio"], 2),
                          lg["clock_like"][self.H15]["pooled"]["raised"]["jobs"]), (1.22, 4))
        # one job a stage: a job after the stage's, with no begin record of its own, is the user's
        st_old = st.replace("job start (self-test)\n", "job start (auto: screen settled)\n")
        lg = a.summarise_log(BOOT22 + START + st_old + game_job(per_frame(2400), RAISED, helpers=2)
                             + START + "".join(auto_job(per_frame(2928)) for _ in range(2)))
        self.assertEqual((lg["clock_like"][self.H15]["left_out"]["selftest"],
                          lg["clock_like"][self.H15]["pooled"]["raised"]["jobs"]), (3, 1))

    def test_the_ratio_by_window_and_model_with_job_counts(self):
        # invented drift: window 3's scene costs more (2,500 us a frame raised, against window 1's 2,200); pooled
        # 1.24, window 1 alone 1.33, window 3 alone 1.17. FL10 read in the same windows: 1.33, a verdict of its own
        log = (BOOT22 + START + auto_job(per_frame(2200), RAISED) + auto_job(per_frame(2200), RAISED)
               + auto_job(per_frame(1300), RAISED, model=self.FL10) + auto_job(per_frame(1300), RAISED, model=self.FL10)
               + START + "".join(auto_job(per_frame(2925)) for _ in range(3))
               + auto_job(per_frame(1725), model=self.FL10) + auto_job(per_frame(1725), model=self.FL10)
               + START + auto_job(per_frame(2500), RAISED) + auto_job(per_frame(2500), RAISED))
        lg = a.summarise_log(log)
        windows = lg["clock_like"][self.H15]["windows"]
        self.assertEqual([(w["window"], w["side"], w["jobs"], round(w["like"]["ratio"], 2)) for w in windows],
                         [(1, "raised", 2, 1.33), (2, "off", 3, 1.24), (3, "raised", 2, 1.17)])
        self.assertEqual((windows[1]["neighbours"], round(windows[1]["near"]["ratio"], 2)), ([1, 3], 1.24))
        self.assertEqual([(v["model"], v["verdict"], round(v["ratio"], 2), v["off_jobs"], v["raised_jobs"])
                          for v in lg["boost_verdicts"]],
                         [(self.H15, "unclear", 1.24, 3, 4), (self.FL10, "yes", 1.33, 2, 2)])
        text = self.text(lg)
        for want in ("by window, like for like (a raised window against the off side, an off window against the raised "
                     "side): window 1 (raised, 2 jobs): 1.33; window 2 (off, 3 jobs): 1.24, against its neighbours "
                     "(windows 1, 3): 1.24 (4 raised jobs); window 3 (raised, 2 jobs): 1.17",
                     "boost effective: unclear (ratio 1.24, n=7): VOCR3_H15_w8.vocr",
                     "boost effective: yes (ratio 1.33, n=4): VOCR3_FL10_w8.vocr like for like, 2 off and 2 raised jobs",
                     "1 line 2 PCSG00415 4 4 4 333 / 444 / 333 (raised in 4); VOCR3_H15_w8.vocr 2,200 us (2 jobs, 6 "
                     "lines); VOCR3_FL10_w8.vocr 1,300 us (2 jobs, 6 lines)"):
            self.assertIn(want, text)

    def test_like_for_like_matches_lines_and_helpers_weighted_by_the_off_side(self):
        refresh = "job start (auto: screen settled), refresh\n" + game_job(per_frame(2000), RAISED, helpers=2)
        log = (BOOT22 + START + auto_job(per_frame(2200), RAISED) + auto_job(per_frame(2200), RAISED)
               + auto_job(per_frame(2200, (80, 76)), RAISED) + auto_job(per_frame(2200, (80, 76)), RAISED)
               + auto_job(per_frame(1980), RAISED, helpers=0)                # ocr_threads = 1: no PAR step
               + game_job(per_frame(2000), RAISED, why="overlay opened", helpers=2) + refresh
               + START + auto_job(per_frame(2925)) + auto_job(per_frame(2925)) + auto_job(per_frame(2925, (80,))))
        lg = a.summarise_log(log)
        lk = lg["clock_like"][self.H15]
        p = lk["pooled"]
        self.assertEqual([(k["lines"], k["helpers"], k["trigger"], round(k["ratio"], 2)) for k in p["strata"]],
                         [(3, 2, "auto", 1.33)])
        self.assertEqual(p["unmatched"], {"off": [{"lines": 1, "helpers": 2, "trigger": "auto", "jobs": 1}],
                                          "raised": [{"lines": 2, "helpers": 2, "trigger": "auto", "jobs": 2},
                                                     {"lines": 3, "helpers": 0, "trigger": "auto", "jobs": 1},
                                                     {"lines": 3, "helpers": 2, "trigger": "overlay", "jobs": 2}]})
        self.assertEqual(lk["left_out"], {"selftest": 0, "reason_unknown": 0, "helpers_unknown": 0, "unmatched": 6})
        self.assertIn("3 lines, 2 helpers, auto: off 2,925 us (2 jobs), raised 2,200 us (2 jobs): 1.33; left out: 6 jobs "
                      "of a kind not on the other side", self.text(lg))
        # the overlay's own jobs (a press, its refresh) match the overlay's own: a second boost-off window of them
        lg = a.summarise_log(log + START + game_job(per_frame(2700), why="overlay opened", helpers=2) * 2)
        p = lg["clock_like"][self.H15]["pooled"]
        self.assertEqual([(k["trigger"], k["off"]["jobs"], k["raised"]["jobs"], round(k["ratio"], 2)) for k in p["strata"]],
                         [("auto", 2, 2, 1.33), ("overlay", 2, 2, 1.35)])
        self.assertEqual(round(p["ratio"], 4), round(2475000 / 1848000, 4))  # each kind against its own
        self.assertIn("3 lines, 2 helpers, auto: off 2,925 us (2 jobs), raised 2,200 us (2 jobs): 1.33; 3 lines, 2 "
                      "helpers, overlay: off 2,700 us (2 jobs), raised 2,000 us (2 jobs): 1.35", self.text(lg))
        # a 1-line kind on both sides: each kind against its own, weighted by the off side's frames:
        # (2 x 643,500 + 234,000) us / (440 x 2,200 + 80 x 2,250) us = 1.32 (the kinds' own 1.33 and 1.30)
        lg = a.summarise_log(log + START + auto_job(per_frame(2250, (80,)), RAISED))
        p = lg["clock_like"][self.H15]["pooled"]
        self.assertEqual([(k["lines"], round(k["ratio"], 2)) for k in p["strata"]], [(1, 1.3), (3, 1.33)])
        self.assertEqual((round(p["ratio"], 4), p["off"]["jobs"], p["raised"]["jobs"]),
                         (round(1521000 / 1148000, 4), 3, 3))
        self.assertIn("1 line, 2 helpers, auto: off 2,925 us (1 job), raised 2,250 us (1 job): 1.30; 3 lines, 2 helpers, "
                      "auto", self.text(lg))
        # a serial job matches serial jobs only
        lg = a.summarise_log(BOOT22 + START + auto_job(per_frame(1980), RAISED, helpers=0)
                             + auto_job(per_frame(1980), RAISED, helpers=0)
                             + START + auto_job(per_frame(2640), helpers=0) + auto_job(per_frame(2640), helpers=0))
        p = lg["clock_like"][self.H15]["pooled"]
        self.assertEqual([(k["lines"], k["helpers"]) for k in p["strata"]], [(3, 0)])
        self.assertIn("3 lines, serial, auto: off 2,640 us (2 jobs), raised 1,980 us (2 jobs): 1.33", self.text(lg))

    def test_a_job_whose_helpers_are_not_logged_is_left_out(self):
        # a step log that filled before PAR's place (no PAR, no free of the workspace, no PACE): its helpers are not
        # known, so it is not matched; the pooled ratio still has it
        log = (BOOT22 + START + auto_job(per_frame(2200), RAISED) + auto_job(per_frame(2200), RAISED)
               + game_job(per_frame(2200), RAISED, why="auto: screen settled")
               + START + auto_job(per_frame(2925)) + auto_job(per_frame(2925)))
        lg = a.summarise_log(log)
        self.assertEqual(lg["clock_like"][self.H15]["left_out"]["helpers_unknown"], 1)
        self.assertEqual(lg["clock_compare"][self.H15]["raised"]["jobs"], 3)
        self.assertIn("left out: 1 job whose helpers are not logged", self.text(lg))
        # a job with no start line (the start of a rotated log): what started it is not known
        lg = a.summarise_log(BOOT22 + START + game_job(per_frame(2200), RAISED, helpers=2) + log[len(BOOT22):])
        self.assertEqual(lg["clock_like"][self.H15]["left_out"],
                         {"selftest": 0, "reason_unknown": 1, "helpers_unknown": 1, "unmatched": 0})
        self.assertIn("left out: 1 job with no start line, 1 job whose helpers are not logged", self.text(lg))

    def test_what_started_a_job(self):
        # shell/worker.c's job start reasons: Auto's with the overlay closed, the overlay's own, the subtitles', the
        # self-test's
        cases = {("auto: screen settled", ""): "auto", ("auto: screen quiet", ""): "auto",
                 ("auto: screen settled", ", OCR only"): "auto", ("auto: screen settled", ", refresh"): "overlay",
                 ("overlay opened", ""): "overlay", ("the screen changed during the last one", ""): "overlay",
                 ("result was for an earlier screen", ""): "overlay", ("the running job was OCR only", ""): "overlay",
                 ("subtitles on", ""): "subtitles", ("subtitles: screen keeps changing", ", OCR only"): "subtitles",
                 ("region changed", ""): "subtitles", ("self-test", ""): "self-test"}
        self.assertEqual({k: a.trigger(k) for k in cases}, cases)
        self.assertIsNone(a.trigger(None))
        shell = (ROOT / "shell/worker.c").read_text(encoding="utf-8")
        for why, _ in cases:  # every reason above is one the Shell logs
            self.assertTrue(f'"{why}"' in shell or f'"job start ({why})"' in shell
                            or why in ("auto: screen settled", "auto: screen quiet"), why)
        self.assertIn('start_job(st.stable ? "auto: screen settled" : "auto: screen quiet", want_lookup());', shell)

    def test_the_verdicts_thresholds(self):
        def verdict(off_us, off_jobs=2, raised_us=2000):
            log = (BOOT22 + START + "".join(auto_job([(100, raised_us / 10)], RAISED) for _ in range(2))
                   + START + "".join(auto_job([(100, off_us / 10)]) for _ in range(off_jobs)))
            (v,) = a.summarise_log(log)["boost_verdicts"]
            return v["verdict"], round(v["ratio"], 2), v["n"]
        self.assertEqual(verdict(2700), ("yes", 1.35, 4))
        self.assertEqual(verdict(2840), ("yes", 1.42, 4))       # 1.42: a quarter over the gain
        self.assertEqual(verdict(2860), ("unclear", 1.43, 4))   # more than the clock explains
        self.assertEqual(verdict(2500), ("yes", 1.25, 4))       # 1.25: three quarters of 444 / 333's gain
        self.assertEqual(verdict(2480), ("unclear", 1.24, 4))
        self.assertEqual(verdict(2180), ("unclear", 1.09, 4))
        self.assertEqual(verdict(2160), ("no", 1.08, 4))        # 1.08: under a quarter of the gain
        self.assertEqual(verdict(2000), ("no", 1.0, 4))
        self.assertEqual(verdict(2700, off_jobs=1), ("unclear", 1.35, 3))  # under 2 jobs a side
        lg = a.summarise_log(BOOT22 + START + auto_job([(100, 200.0)], RAISED) + auto_job([(100, 200.0)], RAISED)
                             + START + auto_job([(100, 270.0)]))
        self.assertIn("boost effective: unclear (ratio 1.35, n=3): VOCR3_H15_w8.vocr like for like, 1 off and 2 raised "
                      "jobs (all jobs: 1.35); too few jobs to decide: at least 2 a side; yes at 1.25 to 1.42, no at 1.08 "
                      "or less", self.text(lg))
        lg = a.summarise_log(BOOT22 + START + "".join(auto_job([(100, 200.0)], RAISED) for _ in range(2))
                             + START + "".join(auto_job([(100, 300.0)]) for _ in range(2)))
        self.assertIn("boost effective: unclear (ratio 1.50, n=4): VOCR3_H15_w8.vocr like for like, 2 off and 2 raised "
                      "jobs (all jobs: 1.50); more than the clock explains: the off side lost time to something else",
                      self.text(lg))
        # another clock pair: the thresholds follow its gain (222 -> 444: 2.00; yes at 1.75, no at 1.24)
        v = a.boost_verdicts([], {self.H15: {"off": {"jobs": 2, "us": 1, "frames": 1}, "raised": {
            "jobs": 2, "us": 1, "frames": 1, "before": 222, "during": 444}}},
            {self.H15: {"pooled": None}})[0]
        self.assertEqual((v["yes_at"], v["no_at"], v["over_at"], v["gain"]), (1.75, 1.24, 2.25, (222, 444)))

    def test_a_clock_the_boost_could_not_raise_gives_no_ratio(self):
        # a clock plugin holding 333 MHz (the set call returns 0, the clock stays) or a refused set call (rc < 0)
        log = (BOOT22 + START + auto_job(per_frame(2200), (333, 0, 333, 333))
               + auto_job(per_frame(2200), (333, -2147352567, 333, 333)) + START + auto_job(per_frame(2925)) * 2)
        lg = a.summarise_log(log)
        self.assertEqual(sorted(lg["clock_compare"][self.H15]), ["not_raised", "off"])
        self.assertIn("boost effective: unclear (ratio -, n=0): no job raised the clock: 2 jobs logged it not raised "
                      "(the set call refused, or the clock not moved: a clock plugin holding it, say)", self.text(lg))

    def test_the_off_windows_clock_is_assumed_and_its_evidence_shown(self):
        # invented: the game at 333 MHz, lowered to 222 during window 1's job, still at 222 in window 3
        log = (BOOT22 + START + auto_job(per_frame(2200), (333, 0, 444, 222)) + START + auto_job(per_frame(2925)) * 2
               + START + auto_job(per_frame(2200), (222, 0, 444, 222)))
        lg = a.summarise_log(log)
        text = self.text(lg)
        self.assertIn("2 line 13 PCSG00415 2 2 0 BOOST OFF, clock assumed (not logged): no clock line in its 2 "
                      "recognizing jobs", text)
        self.assertIn("the boost-off windows' clock is assumed (not logged): with ocr_boost_clock off no job reads it, "
                      "and no plugin's start line logs it: window 2: assumed 222 MHz, as the jobs nearest it read it: "
                      "222 after window 1's last job (before it; its jobs 333 MHz before, 222 after), 222 before window "
                      "3's first job (after it; its jobs 222 MHz before, 222 after)", text)
        # so the clock the off window ran at is not one figure: no verdict from it
        self.assertIn("boost effective: unclear (ratio 1.33, n=4): VOCR3_H15_w8.vocr like for like, 2 off and 2 raised "
                      "jobs (all jobs: 1.33); the game's own clock was not one figure around the raised jobs (222-333 "
                      "MHz before or after them), so the boost-off side's, assumed from it, is not known", text)
        # a clock plugin in the collect's taiHEN configurations (a commented line is not one)
        tai = {"ur0:tai/config.txt": a.summarise_tai("*KERNEL\nur0:tai/VitaJPOverlay_Kernel.skprx\n"
                                                     "ur0:tai/PSVshell.skprx\n# ur0:tai/lolicon.skprx\n")}
        self.assertEqual(tai["ur0:tai/config.txt"]["clock_plugins"], [("KERNEL", "PSVshell.skprx")])
        self.assertIn("clock plugins in the taiHEN configurations: PSVshell.skprx under *KERNEL in ur0:tai/config.txt: a "
                      "clock such a plugin holds for the game (a PSVshell profile) applies in every window, the "
                      "boost-off one too, and nothing logs it", self.text(lg, tai))
        self.assertIn("clock plugins in the taiHEN configurations: none (no PSVshell, LOLIcon or oclockvita)",
                      self.text(lg, {"ur0:tai/config.txt": a.summarise_tai("*KERNEL\nur0:tai/VitaJPOverlay_Kernel.skprx\n")}))
        self.assertIn("clock plugins in the taiHEN configurations: not known (this collect has no taiHEN configuration)",
                      self.text(lg, {}))
        # the collect's analysis reads the configurations it fetched
        with tempfile.TemporaryDirectory() as tmp:
            folder = log_collect(Path(tmp) / "c", log, tai="*KERNEL\nur0:tai/PSVshell.skprx\n")
            rendered = flat(a.render_collect(a.summarise_collect(folder)))
        self.assertIn("clock plugins in the taiHEN configurations: PSVshell.skprx under *KERNEL in ur0:tai/config.txt",
                      rendered)
        self.assertIn("boost effective: unclear (ratio 1.33, n=4)", rendered)

    def test_a_rotated_off_window_is_named_by_its_boots_api_22_lines(self):
        # N5: log.old.txt starts inside a boot with ocr_threads = 1 and full step logs (no PAR, no PACE): nothing in
        # the boost-off window names API 22, but the Shell's done lines count their appends since #116, after API 22
        done = "job 0 done rc=0 in 900 ms, arena peak 4096, dictionary page 1/1, game 700 ms, log 31 ms in 2 appends\n"
        off = START + game_job(per_frame(2925), why="auto: screen settled") + done
        lg = a.summarise_log(off)
        self.assertEqual(([(r["window"], r["state"]) for r in lg["clock_windows"]], lg["clock_windows_older"]),
                         ([(1, "off")], 0))
        # or by a later window of the same boot (its clock lines): the boot is one build
        lg = a.summarise_log(START + game_job(per_frame(2925), why="auto: screen settled")
                             + START + auto_job(per_frame(2200), RAISED))
        self.assertEqual([(r["window"], r["state"]) for r in lg["clock_windows"]], [(1, "off"), (2, "on")])
        # not by a later boot's: its build may be another (an install in between)
        lg = a.summarise_log(START + game_job(per_frame(2925), why="auto: screen settled")
                             + BOOT22 + START + auto_job(per_frame(2200), RAISED))
        self.assertEqual(([r["window"] for r in lg["clock_windows"]], lg["clock_windows_older"]), ([2], 1))
        # an older build's done line has no appends
        lg = a.summarise_log(START + game_job(per_frame(2925)) + "job 0 done rc=0 in 900 ms, arena peak 4096, "
                             "dictionary page 1/1\n")
        self.assertEqual((lg["clock_windows"], lg["clock_windows_older"]), ([], 1))

    def test_a_mixed_window_is_named_when_no_window_has_the_boost_off(self):
        # N5: config.ini rewritten with the game in front (a background writer), then an overlay open: the window is
        # MIXED and left out; the verdict names it rather than reading as if step 6 had been skipped
        lg = a.summarise_log(BOOT22 + START + auto_job(per_frame(2200), RAISED) + auto_job(per_frame(2925)))
        (v,) = lg["boost_verdicts"]
        self.assertEqual((v["model"], v["verdict"], v["ratio"]), (None, "unclear", None))
        self.assertIn("boost effective: unclear (ratio -, n=0): no window with the boost off in this log (step 6's "
                      "optional boxes); window 1 MIXED (some recognizing jobs logged the clock, some did not: "
                      "config.ini changed with the game in front?), left out", self.text(lg))

    def test_the_verdict_is_in_the_json(self):
        lg = a.summarise_log(self.BASE)
        out = json.loads(json.dumps(lg, default=list))
        (v,) = out["boost_verdicts"]
        self.assertEqual((v["model"], v["verdict"], round(v["ratio"], 2), v["n"], v["yes_at"], v["no_at"], v["over_at"],
                          v["gain"]), (self.H15, "yes", 1.32, 6, 1.25, 1.08, 1.42, [333, 444]))
        self.assertEqual(out["clock_like"][self.H15]["pooled"]["strata"][0]["off"]["jobs"], 3)

    def test_the_premises_in_the_source(self):
        shell = (ROOT / "shell/worker.c").read_text(encoding="utf-8")
        # each Shell job's reason, and ", refresh" for the open overlay's quiet refresh (JOB_START)
        self.assertIn('vjo_log("job start (%s)%s", why, refresh ? ", refresh" : lookup ? "" : ", OCR only");', shell)
        self.assertTrue(a.JOB_START.match("job start (auto: screen settled), OCR only"))
        # Auto's job: only with the overlay closed; the open overlay's refresh is a refresh
        body = shell[shell.index("static void auto_prefetch(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("if (ov != OV_CLOSED || job_running", body)
        self.assertIn('start_job(st.stable ? "auto: screen settled" : "auto: screen quiet", want_lookup());', body)
        body = shell[shell.index("static void background_open(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn('start_job_kind("auto: screen settled", 0, 1);', body)
        # the self-test's stages: each begin record before its job, in a job of its own
        self.assertIn('vjo_log("job start (self-test)");', shell)
        body = shell[shell.index("static int run_selftest_stages("):]
        body = body[:body.index("\n}\n")]
        self.assertLess(body.index("vjo_selftest_stage_begin("), body.index("vocr_run_one("))
        selftest = (ROOT / "shell/selftest.c").read_text(encoding="utf-8")
        self.assertIn('"stage %s begin t=%lld flags=0x%X model=%s fallback=%s speed=0x%X"', selftest)
        self.assertIn("i == STAGE_SERIAL ? configured & ~VJO_GAME_OCR_S_HELPERS_MASK : configured", selftest)
        # no PAR step for a serial job with no line from the cache (a job's helpers: PAR's arg)
        worker = (ROOT / "game_ocr/main.c").read_text(encoding="utf-8")
        par = worker.index("step_add(VJO_GSTEP_PAR, 0, stats.helpers,")
        cond = worker.index("if (stats.lines && (want_helpers || stats.cached)) {")
        self.assertLess(cond, par)
        self.assertLess(par - cond, 1500)
        # the done line's appends (DONE_APPENDS), which builds before API 22 do not log
        self.assertIn('"job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d, game %u ms, log %u ms in %u "\n'
                      '            "appends"', shell)
        self.assertTrue(a.DONE_APPENDS.match("job 1 done rc=0 in 3300 ms, arena peak 0, dictionary page 1/1, game 3100 "
                                             "ms, log 62 ms in 6 appends"))
        # the off window's clock is assumed: only job_busy reads the clock, and it returns first with the boost off;
        # no start line logs it (a plugin that read it elsewhere would have to be shown beside the assumption)
        body = worker[worker.index("static void job_busy(void *ud, int on)"):]
        body = body[:body.index("\n}\n")]
        reads = {f: (ROOT / f).read_text(encoding="utf-8").count("scePowerGetArmClockFrequency()")
                 for d in ("game_ocr", "shell", "kernel", "core") for f in
                 (str(q.relative_to(ROOT)) for q in sorted((ROOT / d).glob("*.c*")))}
        self.assertEqual({f: n for f, n in reads.items() if n}, {"game_ocr/main.c": body.count("scePowerGetArmClockFrequency()")})

    def test_the_selftest_fixture_is_formatted_as_the_shell_writes(self):
        # tools/tests/fixtures/log-test3-selftest-job.txt: synthetic (invented figures), the self-test's own job
        lines = self.SELFTEST.splitlines()
        self.assertEqual(lines[0], "job start (self-test)")
        begins = [line for line in lines if line.startswith("stage ") and " begin " in line]
        self.assertEqual([b.split()[1] for b in begins], ["fallback", "textbox", "fullscreen", "probe", "textbox2"])
        for b in begins:
            self.assertRegex(b, r"^stage \w+ begin t=\d+ flags=0x[0-9A-F]+ model=\S+ fallback=\S+ speed=0x[0-9A-F]+$")
        # the configured helpers with the boost, without the line cache; textbox2 serially
        self.assertEqual([b.rsplit("speed=", 1)[1] for b in begins], ["0xE", "0xE", "0xE", "0xE", "0xC"])
        jobs = [n for n, line in enumerate(lines) if line.startswith("game vocr rc=")]
        self.assertEqual(len(jobs), 5)
        for n in jobs:
            self.assertTrue(a.LOG["job"].match(lines[n]), lines[n])
            self.assertTrue(lines[n - 1].startswith("stage ") and " begin " in lines[n - 1])
        # textbox2's job logs no PAR (serial, no cache)
        t2 = lines[jobs[4]:]
        self.assertFalse(any(line.startswith("gstep job par ") for line in t2))
        for line in lines:
            if line.startswith("gstep "):
                self.assertRegex(line, r"^gstep \w+ \w+ kind=\S+ arg=\S+ rc=0x[0-9A-F]{8} bytes=\d+ sum=0x[0-9A-F]{8} "
                                       r"main=-?\d+ cdram=-?\d+ phycont=-?\d+ us=\d+$")
        self.assertTrue(a.DONE_APPENDS.match(lines[-1]))


def _per(g):
    return g["us"] / g["frames"]


def flat(text):
    """Text with each run of white space one space (the tables' column widths are not what is tested)."""
    return " ".join(text.split())



class WorkerLoaderLines(unittest.TestCase):
    """#138: kernel/loader.c logs one `worker for` line per game start it decided on; the collect
    analysis says where the worker was loaded, where not and why, and counts refused second copies.
    The lines are the loader's own formats (host/tests/test_kernel_loader.c checks them)."""
    KERNEL = (
        "Vita JP Overlay kernel 22 started (normal mode)\n"
        "hooks: display 0, input 0, procevent 0, worker loader 0 (0 = ok)\n"
        "worker for PCSG00940 (pid 0x72ED3): ON (no games.ini): ur0:tai/VitaJPOverlay_OCR.suprx 0x40010129, start 0\n"
        "worker for PSPEMUCFW (PSP) (pid 0x11B2F15): ON in games.ini: ur0:tai/VitaJPOverlay_OCR.suprx 0x40010131, start 0\n"
        "worker for PCSG00940 (pid 0x73001): ON in games.ini.bak: ux0:tai/VitaJPOverlay_OCR.suprx 0x40010133, start 0\n"
        "worker for PCSF00001 (pid 0x73101): OFF in games.ini: not loaded\n"
        "worker for PCSF00001 (pid 0x73201): OFF in games.ini: not loaded\n"
        "worker for PCSG00415 (pid 0x73301): L held at its start: not loaded\n"
        "worker for VITADBDLD (pid 0x73401): ON in games.ini: ur0:tai/VitaJPOverlay_OCR.suprx 0x80024302, start 0\n"
        "worker for PCSG00415 (pid 0x73501): ON in games.ini: ur0:tai/VitaJPOverlay_OCR.suprx 0x40010141, start 1\n"
        "ocr worker registered: game pid 0x72ED3 module 0x40010129\n"
        "ocr worker: second worker thread 0x4001A001 in game pid 0x73501 refused (registered: 0x40019001)\n")

    def test_the_loaders_decisions_are_summarised(self):
        k = a.summarise_kernel(self.KERNEL, len(self.KERNEL))
        self.assertEqual(k["loaded"], ["PCSG00940", "PSPEMUCFW", "PCSG00940"])
        self.assertEqual(k["second"], 1)
        self.assertEqual(k["registered"], 1)
        self.assertEqual(a.loader_text(k),
                         "worker loader: loaded into PCSG00940 x2, PSPEMUCFW; "
                         "not loaded (OFF in games.ini): PCSF00001 x2; "
                         "not loaded (L held at its start): PCSG00415; "
                         "not loaded (its load failed: 0x80024302, start 0): VITADBDLD; "
                         "not loaded (its load failed: 0x40010141, start 1): PCSG00415; "
                         "1 second worker thread(s) refused")
        # the refusal is the loader's, not a memory problem line
        self.assertFalse(any("second worker thread" in x for x in k["alloc_fail"]))

    def test_a_kernel_without_the_loader_says_nothing_of_it(self):
        old = "Vita JP Overlay kernel 22 started (normal mode)\nhooks: display 0, input 0, procevent 0 (0 = ok)\n"
        self.assertIsNone(a.loader_text(a.summarise_kernel(old, len(old))))
        none = "worker for PCSF00001 (pid 0x73101): OFF in games.ini: not loaded\n"
        self.assertEqual(a.loader_text(a.summarise_kernel(none, len(none))),
                         "worker loader: loaded into no game; not loaded (OFF in games.ini): PCSF00001")


class OverlayAppSummary(unittest.TestCase):
    """summarise_app and render_app (#138): the collect's own reading of the installed Overlay app decides when it
    read a file; when it read neither, what it found of the folder decides "not installed", and otherwise the Shell's
    check at a game start of the log's last boot answers for it. test.3's collect said "not installed" for an app the
    Shell had read at the same boot's game starts."""
    STAMP = "vjoapp1 api=22 build=c3e781ad58a5ebe3"
    OLDER = "vjoapp1 api=21 build=0123456789abcdef"
    READ = {"state": "read", "method": "cwd", "rc": [0]}
    BY_PATH = {"state": "read", "method": "nocwd", "rc": [9, 0]}
    ABSENT = {"state": "absent", "method": None, "rc": [9, 78]}
    TIMEOUT = {"state": "error", "method": "cwd", "rc": [28], "error": "curl: (28) Operation timed out"}
    OK = f"overlay app ok: installed '{STAMP}' (0x00000026, app 1), plugins' '{STAMP}' (0x00000026)\n"
    PRE = f"overlay app MISMATCH: installed '' (0x80010002, app 1), plugins' '{STAMP}' (0x00000026)\n"
    NONE = f"overlay app unknown: installed '' (0x80010002, app 0), plugins' '{STAMP}' (0x00000026)\n"
    UNREADABLE = f"overlay app unknown: installed '' (0x80010013, app 1), plugins' '{STAMP}' (0x00000026)\n"
    # a check before the plugins' record was replaced (an install without a reboot since): another record
    STALE = f"overlay app ok: installed '{OLDER}' (0x00000026, app 1), plugins' '{OLDER}' (0x00000026)\n"

    def app(self, stamp=ABSENT, eboot=ABSENT, **found):
        return {"reads": {"vjo_app.txt": stamp, "eboot.bin": eboot}, "eboot": None, **found}

    def summary(self, app, log="", installed=None, old_lines=0, **record):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "ux0/data/VitaJPOverlay").mkdir(parents=True)
            (folder / "ux0/data/VitaJPOverlay/vjo_app.txt").write_text(self.STAMP + "\n")
            if installed is not None:
                (folder / "ux0/app/VJOSET001").mkdir(parents=True)
                (folder / "ux0/app/VJOSET001/vjo_app.txt").write_text(installed + "\n")
            s = a.summarise_app(folder, {"app": app, **record}, a.summarise_log(log) if log else None, old_lines)
        return s, a.render_app(s)

    def test_what_the_collect_read_decides(self):
        s, line = self.summary(self.app(stamp=self.READ), installed=self.STAMP)
        self.assertEqual((s["state"], s["source"]), ("ok", "collect"))
        self.assertEqual(line, f"  Overlay app: the plugins' ({self.STAMP})")
        # read by its full path: said, with the first way's exit
        s, line = self.summary(self.app(stamp=self.BY_PATH), BOOT22 + START + self.PRE, installed=self.STAMP)
        self.assertEqual((s["state"], s["source"], s["by_path"]), ("ok", "collect", {"vjo_app.txt": [9, 0]}))
        self.assertTrue(line.endswith("; read by its full path, no CWD, after the way through its folder was answered "
                                      "'not there': vjo_app.txt (curl exit 9)"), line)
        # an older stamp read now outweighs any check of the Shell's, and says how it was read
        s, line = self.summary(self.app(stamp=self.BY_PATH), BOOT22 + START + self.OK, installed=self.OLDER)
        self.assertEqual((s["state"], s["source"]), ("MISMATCH", "collect"))
        self.assertTrue(line.startswith(f"  Overlay app: NOT the plugins' (installed: {self.OLDER}; the plugins came "
                                        f"with {self.STAMP}): "), line)
        self.assertTrue(line.endswith("in VitaShell; read by its full path, no CWD, after the way through its folder was "
                                      "answered 'not there': vjo_app.txt (curl exit 9)"), line)
        # an eboot.bin without a stamp (#101): an app from before the stamps
        pre = self.app(eboot=self.READ)
        pre["eboot"] = {"bytes": 4, "sha256": "ab" * 32}
        s, line = self.summary(pre)
        self.assertEqual(s["state"], "MISMATCH")
        self.assertIn("installed: an app from before the version stamps, eboot.bin abababababababab;", line)

    def test_not_installed_is_said_only_when_a_folder_or_the_shell_shows_it(self):
        for app, why in ((self.app(folder="ok", eboot_listed=False), "ux0:app/VJOSET001 has no eboot.bin"),
                         (self.app(folder="missing", apps="ok", in_apps=False), "ux0:app has no VJOSET001"),
                         ({"listed": True, "eboot": None}, "no ux0:app/VJOSET001/eboot.bin")):  # an older collect
            with self.subTest(why=why):
                s, line = self.summary(app)
                self.assertEqual(s["state"], "not installed")
                self.assertEqual(line, f"  Overlay app: not installed ({why})")
        # a check of this boot that found it: both facts are said
        s, line = self.summary(self.app(folder="missing", apps="ok", in_apps=False), BOOT22 + START + self.OK)
        self.assertEqual(s["state"], "not installed")
        self.assertIn(f"; the Shell's check at a game start of this boot found one ({self.STAMP}, log.txt:3): deleted "
                      "since, or the listing left it out", line)
        # the Shell found neither file, against the same record, and nothing in the collect shows the folder
        s, line = self.summary({"listed": False, "eboot": None}, BOOT22 + START + self.NONE)
        self.assertEqual((s["state"], s["source"]), ("not installed", "shell"))
        self.assertEqual(line, "  Overlay app: not installed: the Shell's check at a game start of this boot found "
                               "neither its vjo_app.txt nor its eboot.bin (log.txt:3); this collect could not read it "
                               "over FTP: its folder could not be entered, and this older collect_vita.py read the app "
                               "only through it (#138)")
        # an older collect's listing that showed eboot.bin it could not hash: not "not installed"
        s, line = self.summary({"listed": True, "eboot": None},
                               listings={"ux0:/app/VJOSET001": [{"name": "eboot.bin", "bytes": 9, "dir": False}]})
        self.assertEqual(s["state"], "not read")
        self.assertIn("(its folder lists eboot.bin, but it could not be hashed)", line)

    def test_an_app_the_collect_could_not_read_is_the_shells_answer(self):
        present = self.app(folder="missing", apps="ok", in_apps=True)
        for app, why, found in (
                (present, "ux0:app lists VJOSET001, but its folder could not be entered and neither its "
                          "vjo_app.txt nor its eboot.bin could be read by its full path", "present"),
                (self.app(folder="ok", eboot_listed=True),
                 "its folder lists eboot.bin, but neither it nor vjo_app.txt could be read", "present"),
                (self.app(folder="missing", apps="error", apps_error="EOFError: "),
                 "its folder could not be entered, neither file could be read by its full path, and ux0:app "
                 "could not be listed", None),
                # test.3's collect (#138): an older collect_vita.py that could not enter the folder
                ({"listed": False, "eboot": None},
                 "its folder could not be entered, and this older collect_vita.py read the app only through it", None)):
            with self.subTest(why=why):
                s, line = self.summary(app, "API 20 boot\n" + BOOT22 + START + self.OK)
                self.assertEqual((s["state"], s["source"], s["shell"]["where"]), ("ok", "shell", "log.txt:4"))
                self.assertEqual(line, f"  Overlay app: the plugins' ({self.STAMP}), by the Shell's check at a game start "
                                       f"of this boot (log.txt:4); this collect could not read it over FTP: {why} (#138)")
                # no answer: never "not installed", and the line says what the Shell's last check was
                start = f"  Overlay app: not checked: this collect could not read it over FTP ({why}), and "
                for log, shell in (
                        ("", "the Shell logged no check of it in this boot"),
                        (BOOT22 + START + self.OK + BOOT22, "the Shell logged no check of it in this boot"),
                        (BOOT22 + START + self.STALE, f"the Shell's last check in this boot (log.txt:3) compared it with "
                                                      f"another record ({self.OLDER})"),
                        (BOOT22 + START + self.UNREADABLE, "the Shell's last check in this boot (log.txt:3) does not "
                                                           "decide it either (its vjo_app.txt: unreadable, 0x80010013; "
                                                           "its eboot.bin: there)")):
                    s, line = self.summary(app, log)
                    self.assertEqual((s["state"], s["source"]), ("not read", "collect"), log)
                    self.assertTrue(line.startswith(start + shell + "; if the Overlay app is open beside VitaShell, "
                                                    "close it from its LiveArea page"), line)
                # the Shell found neither file: "not installed", unless the collect saw the folder
                s, line = self.summary(app, BOOT22 + START + self.NONE)
                if found == "present":
                    self.assertEqual(s["state"], "not read")
                    self.assertIn("does not decide it either (its vjo_app.txt: not there, 0x80010002; its eboot.bin: "
                                  "not there)", line)
                else:
                    self.assertEqual((s["state"], s["source"]), ("not installed", "shell"))
        # the Shell found an app without a stamp (#101): its mismatch, in its words
        s, line = self.summary(present, BOOT22 + START + self.PRE)
        self.assertEqual((s["state"], s["source"]), ("MISMATCH", "shell"))
        self.assertTrue(line.startswith("  Overlay app: NOT the plugins' (installed: an app from before the version "
                                        f"stamps; the plugins came with {self.STAMP}), by the Shell's check"), line)
        # the boot's last line is its answer (the Shell logs only a change): ok after a mismatch in one boot is ok
        s, _ = self.summary(present, BOOT22 + START + self.PRE + START + self.OK)
        self.assertEqual((s["state"], s["shell"]["line"]), ("ok", 5))
        # a check in log.old.txt is named there (its lines come first)
        s, line = self.summary(present, BOOT22 + START + self.OK + "log.txt's first line\n", old_lines=3)
        self.assertEqual(s["shell"]["where"], "log.old.txt:3")
        self.assertIn("by the Shell's check at a game start of this boot (log.old.txt:3)", line)

    def test_a_failed_read_is_no_absence(self):
        app = self.app(eboot=self.TIMEOUT)
        s, line = self.summary(app)
        self.assertEqual(s["state"], "not read")
        self.assertIn("(reading it failed: eboot.bin, curl exit 28 (curl: (28) Operation timed out))", line)
        s, _ = self.summary(app, BOOT22 + START + self.OK)
        self.assertEqual((s["state"], s["source"]), ("ok", "shell"))
        # a stamp that failed to read beside a hashed eboot.bin is no app from before the stamps
        app = self.app(stamp=self.TIMEOUT, eboot=self.READ)
        app["eboot"] = {"bytes": 4, "sha256": "ab" * 32}
        s, line = self.summary(app)
        self.assertEqual(s["state"], "not read")
        self.assertIn("(reading it failed: vjo_app.txt, curl exit 28 (curl: (28) Operation timed out))", line)
        self.assertNotIn("before the version stamps", line)
        s, _ = self.summary(app, BOOT22 + START + self.OK)
        self.assertEqual((s["state"], s["source"]), ("ok", "shell"))

    def test_the_line_the_shell_writes_is_read_whole(self):
        worker = (ROOT / "shell/worker.c").read_text(encoding="utf-8")
        fmt = "overlay app %s: installed '%s' (0x%08X, app %d), plugins' '%s' (0x%08X)"
        self.assertIn(f'vjo_log("{fmt}",', worker, "shell/worker.c check_app's line changed: update APP_CHECK")
        m = a.APP_CHECK.match(fmt % ("ok", self.STAMP, 0x26, 1, self.STAMP, 0x26))
        self.assertEqual(m.groups(), ("ok", self.STAMP, "0x00000026", "1", self.STAMP, "0x00000026"))
        m = a.APP_CHECK.match(fmt % ("MISMATCH", "", 0x80010002, 1, self.STAMP, 0x26))
        self.assertEqual(m.group(2, 3), ("", "0x80010002"))
        # it checks at each enabled game's start, after the game is active, and logs only a change
        body = worker[worker.index("static void check_app(void)"):]
        self.assertIn("if (s == app_state)\n        return;", body[:body.index("vjo_log(")])
        self.assertIn("check_app(); /* after the game is active", worker)


if __name__ == "__main__":
    unittest.main()
