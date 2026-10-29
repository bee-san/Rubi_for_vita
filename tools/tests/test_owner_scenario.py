"""tools/owner_scenario.py (#528, docs/owner-scenario.md): the events it reads from a log, the binaries' lines it
parses, and its ratchet. The replay itself is host/tests/owner_scenario.h's (ctest shell_worker_paf, owner_scenario)."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import owner_scenario as osc  # noqa: E402

# A boot of a normal build, as Rubi 0.10.1 logs it: one game start, a background read's refusal, two presses (the
# second with its lookup's refusal beside it), a saved card, the exit; then a boot with nothing to replay.
LOG = """\
Rubi shell started (kernel API 57, the normal build: a short log)
ledger boot paf: current=8192 peak=8192 limit=985088 free=3726136
game PCSG00415 started: trigger l+r, subtitles select+r, ocr_mode auto, ocr_finder spotter, ocr_line_fix on
ledger start paf: current=8192 peak=8192 limit=985088 free=2491704
anki: save rc=0, 751 ms
result arenas Paf reserve guard: free=413704 requested=65536 reserve=1572864
result workspace Paf allocation failed (64 KiB)
overlay text arena Paf reserve guard: free=426872 requested=20480 reserve=1572864
overlay not opened: no Paf memory for its text (20 KiB)
overlay text arena Paf reserve guard: free=413160 requested=20480 reserve=1572864
result arenas Paf reserve guard: free=413160 requested=65536 reserve=1572864
overlay not opened: no Paf memory for its text (20 KiB)
result workspace Paf allocation failed (64 KiB)
ledger exit paf: current=8192 peak=225280 limit=985088 free=410312
Rubi shell started (kernel API 57, the normal build: a short log)
ledger boot paf: current=8192 peak=8192 limit=985088 free=3726472
ledger exit paf: current=8192 peak=8192 limit=985088 free=2701224
Rubi shell started (kernel API 25, the normal build: a short log)
result arenas Paf reserve guard: free=404424 requested=65536 reserve=1572864
"""


class Extract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.log = Path(self.tmp.name) / "log.txt"
        self.log.write_text(LOG, encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_events_in_order_with_presses_paired_to_their_lookup(self):
        scenarios = osc.extract([self.log], 31)
        self.assertEqual(len(scenarios), 1)  # the boot with nothing to replay, and the API 25 one, are left out
        s = scenarios[0]
        self.assertEqual((s["api"], s["build"]), (57, "normal"))
        self.assertEqual(s["events"], [
            ["boot", 3726136, 8192], ["start", 2491704, 8192], ["anki", 2491704, 8192], ["lookup", 413704, 8192],
            ["press", 426872, 8192, 0], ["press", 413160, 8192, 1], ["exit", 410312, 8192]])
        self.assertEqual(osc.logged_counts(scenarios)[s["name"]],
                         {"presses_refused": 2, "arena_refusals": 2, "anki_saves": 1})

    def test_the_event_file_round_trips(self):
        scenarios = osc.extract([self.log], 31)
        out = Path(self.tmp.name) / "events.txt"
        osc.write_events(scenarios, "a test log", out)
        text = out.read_text(encoding="utf-8")
        self.assertNotIn("PCSG", text)  # numbers only: no title IDs, no text
        back = osc.read_events(out)
        self.assertEqual([b["events"] for b in back], [s["events"] for s in scenarios])

    def test_the_committed_events_hold_the_issues_counts(self):
        """tools/owner_scenario.txt, from the owner's log (#528): 0.10.1's (API 57) boots logged 24 overlays that
        did not open and 28 result arenas refused, API 31's 10 and 23."""
        scenarios = osc.read_events(osc.EVENTS)
        counts = osc.logged_counts(scenarios)
        for api, presses, arenas in ((57, 24, 28), (31, 10, 23)):
            mine = [counts[s["name"]] for s in scenarios if s["api"] == api]
            self.assertEqual(sum(c["presses_refused"] for c in mine), presses)
            self.assertEqual(sum(c["arena_refusals"] for c in mine), arenas)


class Lines(unittest.TestCase):
    def test_lines_parse_after_acutests_test_name(self):
        rows = osc.parse_lines("Test owner scenario...      owner-scenario shell boot1-line9: presses=3 not_opened=2 "
                               "peak=8192\nowner-scenario game est-2MiB-box: free=2097152 rc=-103 failed=1\n"
                               "[ OK ]\n")
        self.assertEqual(rows[("shell", "boot1-line9")], {"presses": 3, "not_opened": 2, "peak": 8192})
        self.assertEqual(rows[("game", "est-2MiB-box")], {"free": 2097152, "rc": -103, "failed": 1})


class Ratchet(unittest.TestCase):
    LIMITS = {"limits": {"shell": {"a": {"not_opened": 2, "peak": 100}},
                         "game": {"g": {"failed": 0, "peak": 50, "min_tier": 3, "min_told": 1}}}}

    def test_within_the_limits_passes(self):
        rows = {("shell", "a"): {"not_opened": 1, "peak": 100}, ("game", "g"): {"failed": 0, "peak": 40,
                                                                                 "tier": 3, "told": 1}}
        self.assertEqual(osc.check(rows, self.LIMITS), [])

    def test_a_regression_fails(self):
        rows = {("shell", "a"): {"not_opened": 3, "peak": 100}, ("game", "g"): {"failed": 1, "peak": 40,
                                                                                 "tier": 2, "told": 0}}
        problems = "\n".join(osc.check(rows, self.LIMITS))
        for what in ("not_opened=3", "failed=1", "tier=2", "told=0"):
            self.assertIn(what, problems)

    def test_a_missing_scenario_fails_and_an_unknown_one_too(self):
        rows = {("shell", "b"): {}, ("game", "g"): {"failed": 0, "peak": 1, "tier": 3, "told": 1}}
        problems = "\n".join(osc.check(rows, self.LIMITS))
        self.assertIn("shell a: missing", problems)
        self.assertIn("shell b: not in", problems)

    def test_without_models_the_game_part_is_not_asked_for(self):
        self.assertEqual(osc.check({("shell", "a"): {"not_opened": 0, "peak": 1}}, self.LIMITS), [])

    def test_lower_never_loosens(self):
        rows = {("shell", "a"): {"not_opened": 5, "peak": 10, "refused": 0, "anki_refused": 0, "held": 8},
                ("game", "g"): {"failed": 0, "peak": 70, "tier": 2, "told": 1, "lm": 1}}
        out = osc.lowered(rows, self.LIMITS)["limits"]
        self.assertEqual(out["shell"]["a"]["not_opened"], 2)  # kept: the run is worse
        self.assertEqual(out["shell"]["a"]["peak"], 10)       # lowered: the run is better
        self.assertEqual(out["game"]["g"]["peak"], 50)
        self.assertEqual(out["game"]["g"]["min_tier"], 3)


if __name__ == "__main__":
    unittest.main()
