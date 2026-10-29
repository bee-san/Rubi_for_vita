"""tools/shell_budget_model.py: the replay's arithmetic on a synthetic budget and
samples, and the real tree's result."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/shell_budget_model.py"
sys.path.insert(0, str(ROOT / "tools"))
import shell_budget_model as m  # noqa: E402

BUDGET = {
    "VJO_BUDGET_SHELL_MODULE_PAGES": 300_000, "VJO_BUDGET_WORKER_IN_SHELL_PAGES": 40_000,
    "VJO_BUDGET_SHELL_NET_STACK": 70_000, "VJO_BUDGET_SHELL_CTL_STACK": 20_000, "VJO_BUDGET_SHELL_ANKI_STACK": 70_000,
    "VJO_BUDGET_PAF_PLUGIN": 8_000, "VJO_BUDGET_PAF_RESULTS_LOCAL": 64_000,
    "VJO_BUDGET_PAF_ANKI": 390_000, "VJO_BUDGET_PAF_UI": 20_000,
}
SAMPLES = {
    "paf_reserve": 1_500_000,
    "samples": [
        {"id": "pages", "date": "d", "source": "s", "shell_free": 100_000, "ownership": "SceShell",
         "overlay_build": "old", "overlay_pages": {"shell_module": 350_000, "worker_module": 40_000, "stacks_tls": 90_000}},
        {"id": "pages-unknown", "date": "d", "source": "s", "shell_free": 30_000, "ownership": "not verified",
         "overlay_build": "older", "overlay_pages": None, "other_occupants": "x"},
        {"id": "paf", "date": "d", "source": "s", "shell_free": None, "overlay_build": "old", "paf_free": 2_000_000,
         "paf_held_by_overlay": "the Paf plugin and RCO"},
    ],
}


class ShellBudgetModel(unittest.TestCase):
    def test_sequence_and_headroom_arithmetic(self):
        r = m.replay(BUDGET, SAMPLES, anki=True)
        names = [s["step"] for s in r["steps"]]
        self.assertEqual(names[1], "Shell module loaded into SceShell")
        self.assertEqual(r["steps"][5]["pages"], 300_000 + 40_000 + 70_000 + 20_000)  # boot complete
        self.assertEqual(r["peak_pages"], 430_000 + 70_000)                           # the Anki thread
        self.assertEqual(r["peak_paf"], 8_000 + 20_000 + 64_000 + 390_000)
        # #528: the overlay's text block is taken as the boot completes and kept; the arenas go at the exit
        self.assertEqual(r["steps"][6], {"step": "overlay text block (from the start, kept)", "pages": 430_000, "paf": 28_000})
        self.assertEqual(r["steps"][-1]["paf"], 8_000 + 20_000)
        pages = next(s for s in r["scenarios"] if s["id"] == "pages")
        self.assertEqual(pages["without_us"], 100_000 + 480_000)
        boot = next(s for s in pages["steps"] if s["step"].startswith("VjoControl"))
        self.assertEqual(boot["headroom"], 580_000 - 430_000)
        self.assertEqual(pages["worst_headroom"], 580_000 - 500_000)
        self.assertIn("of headroom", pages["verdict"])
        unknown = next(s for s in r["scenarios"] if s["id"] == "pages-unknown")
        self.assertEqual(unknown["without_us"], 30_000)
        self.assertTrue(unknown["verdict"].startswith("would run out"))
        self.assertTrue(any("estimate" in n for n in unknown["notes"]))
        self.assertTrue(any("not verified" in n for n in unknown["notes"]))
        paf = next(s for s in r["scenarios"] if s["id"] == "paf")
        self.assertEqual(paf["without_us"], 2_008_000)
        anki = next(s for s in paf["steps"] if s["step"].startswith("Anki card saved: job arena"))
        self.assertEqual(anki["headroom_over_reserve"], 2_008_000 - 482_000 - 1_500_000)
        self.assertEqual(paf["worst_headroom"], 26_000)

    def test_overrides_model_another_build(self):
        r = m.replay(BUDGET, SAMPLES, shell_pages=200_000, worker_pages=0)
        self.assertEqual(r["steps"][5]["pages"], 200_000 + 70_000 + 20_000)
        self.assertEqual(r["peak_paf"], 8_000 + 20_000 + 64_000)
        pages = next(s for s in r["scenarios"] if s["id"] == "pages")
        self.assertEqual(pages["worst_headroom"], 580_000 - 290_000)

    def test_warning_band(self):
        samples = json.loads(json.dumps(SAMPLES))
        samples["samples"][0]["shell_free"] = 10_000  # without us 490,000; boot complete 430,000: 60,000 left
        r = m.replay(BUDGET, samples)
        pages = next(s for s in r["scenarios"] if s["id"] == "pages")
        self.assertIn("within one 64 KiB change", pages["verdict"])
        samples["samples"][0]["shell_free"] = 0        # 480,000: Anki would not fit, boot would
        r = m.replay(BUDGET, samples, anki=True)
        pages = next(s for s in r["scenarios"] if s["id"] == "pages")
        self.assertTrue(pages["verdict"].startswith("would run out"))
        anki = next(s for s in pages["steps"] if s["step"].startswith("Anki card saved: job thread"))
        self.assertEqual(anki["headroom"], -20_000)

    def test_the_real_tree_runs_and_states_its_limits(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--anki"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("scenario notes26-v074", result.stdout)
        self.assertIn("taken with v0.7.4 (3f7ae81, API 13) holding 462,848 B", result.stdout)
        self.assertIn("The model ranks builds; only the console decides", result.stdout)
        as_json = json.loads(subprocess.run([sys.executable, str(SCRIPT), "--json"], capture_output=True, text=True).stdout)
        v074 = next(s for s in as_json["scenarios"] if s["id"] == "notes26-v074")
        # the current build against the last session known to run beside a game: the
        # boot must complete in the model, and the number is what the fix PR must improve
        boot = next(s for s in v074["steps"] if s["step"].startswith("VjoControl"))
        self.assertGreaterEqual(boot["headroom"], 0, "the boot would run out of SceShell pages in the v0.7.4 session")
        self.assertEqual(boot["held"], as_json["peak_pages"])
        # the pre-fix footprint (the *ALL worker resident in SceShell, the overlay arena in .bss),
        # modelled with overrides, ranks worse than the tree's
        before = subprocess.run([sys.executable, str(SCRIPT), "--json", "--worker-pages", "45056", "--shell-pages", "397312"],
                                capture_output=True, text=True)
        before_v074 = next(s for s in json.loads(before.stdout)["scenarios"] if s["id"] == "notes26-v074")
        self.assertLess(before_v074["worst_headroom"], v074["worst_headroom"])
        self.assertGreaterEqual(v074["worst_headroom"], 65536, "the tree is within one #42-sized change of the v0.7.4 session")

    def test_bad_inputs(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            f.write("{}")
        result = subprocess.run([sys.executable, str(SCRIPT), "--samples", f.name], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        Path(f.name).unlink()


if __name__ == "__main__":
    unittest.main()
