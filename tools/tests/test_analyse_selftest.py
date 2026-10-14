"""tools/analyse_selftest.py against reports shaped like shell/selftest.c writes them.

v1 (API 18): reports built here, and #74's own report from the PCH-1001 on 3.65
(tools/tests/fixtures/selftest-74-report.txt, verbatim from the issue).
v2 (API 19): tools/tests/fixtures/selftest-v2-*.txt, written by the real
shell/selftest.c on the host (host/tests/selftest_writer.c, synthetic figures);
with VJO_SELFTEST_WRITER set the writer runs again and must reproduce them.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/analyse_selftest.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures"
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


def rows(stdout):
    """stage -> result from the printed table."""
    out = {}
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and line.startswith("  ") and not line.startswith("    ") and parts[1] in ("PASS", "FAIL", "WARN", "-"):
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
        as_json = json.loads(run(self.report, "--json").stdout)
        self.assertEqual(as_json["reached"], "game")
        self.assertEqual(len(as_json["fails"]), 5)


class AnalyseV2(unittest.TestCase):
    def analyse(self, name):
        return run((FIXTURES / f"selftest-v2-{name}.txt").read_text())

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
        self.assertIn("kernel block route, KD                   FAIL  0x80020001", r.stdout)
        self.assertIn("at step 2 (block/address-space info)", r.stdout)
        # the kernel's address space refused, the caller's read; a slot outside the kernel half not read
        self.assertIn("kernel process route (faps-coredump)     PASS  2 partitions of shell's address space, "
                      "KD 9,437,184 B free; 1 not read (pointer outside the kernel range)", r.stdout)
        self.assertIn("failed routes: block kernel 0x80020001", r.stdout)
        self.assertEqual(rows(r.stdout)["kernel-memory"], "PASS")

    @unittest.skipUnless(os.environ.get("VJO_SELFTEST_WRITER"), "vjo-selftest-writer not built")
    def test_the_fixtures_are_what_the_writer_writes(self):
        for name in ("pass", "fail", "incomplete", "routes"):
            with tempfile.TemporaryDirectory() as tmp:
                done = subprocess.run([os.environ["VJO_SELFTEST_WRITER"], tmp, name], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)
                got = (Path(tmp) / "selftest-report.txt").read_text()
                self.assertEqual(got, (FIXTURES / f"selftest-v2-{name}.txt").read_text(),
                                 f"selftest-v2-{name}.txt differs from shell/selftest.c's output: regenerate it with "
                                 f"vjo-selftest-writer DIR {name}")
                # the marker is removed when (and only when) the run finished
                self.assertEqual((Path(tmp) / "selftest.txt").exists(), name == "incomplete")


if __name__ == "__main__":
    unittest.main()
