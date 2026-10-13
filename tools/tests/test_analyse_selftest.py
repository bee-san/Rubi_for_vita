"""tools/analyse_selftest.py against reports shaped like shell/selftest.c writes them."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/analyse_selftest.py"
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


class AnalyseSelfTest(unittest.TestCase):
    def test_a_complete_run_passes_and_reports_every_pool(self):
        r = run(full_run())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS", r.stdout)
        self.assertIn("step ocr", r.stdout)
        self.assertIn("SceShell free (user view): main 98,304 B", r.stdout)
        self.assertIn("KD 6,900,000 of 37,748,736 B free", r.stdout)
        self.assertIn("capture block 2,088,960 B", r.stdout)
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
        self.assertIn("FAIL: the run completed but the OCR job did not succeed", r.stdout)

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

    def test_unconfirmed_kd_and_low_paf_and_unavailable_kernel_sample(self):
        text = (HEADER + "step boot t=12\n" + sample("boot", kd_total=16_777_216, paf=1_000_000)
                + "step ok t=60012\nmem ok shell_main=-1 shell_cdram=-1 shell_phycont=-1 rc=0x80020001 paf_free=0\n"
                  "ledger ok pages=540672/540672 paf=8192/8192\nkernel ok unavailable rc=0xBFFFFFF8\n")
        r = run(text)
        self.assertIn("not the 36 MiB of ScePhyMemPartKD", r.stdout)
        self.assertIn("under the 1,572,864 B reserve", r.stdout)
        self.assertIn("sceKernelGetFreeMemorySize failed (0x80020001)", r.stdout)
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


if __name__ == "__main__":
    unittest.main()
