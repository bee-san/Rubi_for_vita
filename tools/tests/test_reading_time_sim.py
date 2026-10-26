"""tools/reading_time_sim.py, the reading-time simulator (docs/reading-time.md): it is deterministic, its
committed timelines (host/fixtures/reading_time) are what it makes, its reference rules do what the trackers
they model do on hand-made gaps, and on a small grid the measured ordering holds: the adaptive rule's error is a
few percent where the 15 s rule's is tens of percent."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import reading_time_sim as sim  # noqa: E402


def recs(gaps, chars):
    """Records of one game whose consecutive gaps (s) and characters are given (one record more than gaps)."""
    out, t = [], 1_790_000_000.0
    for i, c in enumerate(chars):
        out.append(sim.Rec(time=t, mono=t, game="g", chars=c, text=("line", i)))
        if i < len(gaps):
            t += gaps[i]
    return out


class Simulator(unittest.TestCase):
    def test_deterministic(self):
        a = sim.simulate(sim.PROFILES[1], sim.SCENARIOS[6], 3)
        b = sim.simulate(sim.PROFILES[1], sim.SCENARIOS[6], 3)
        self.assertEqual([(r.time, r.chars, r.text) for r in a[0]], [(r.time, r.chars, r.text) for r in b[0]])
        self.assertEqual((a[1].seconds, a[1].chars), (b[1].seconds, b[1].chars))
        # the record before's text is written again only after a blank read (shell/history.c's check)
        again = [(x, y) for x, y in zip(a[0], a[0][1:]) if x.text == y.text]
        self.assertTrue(all(any(x.time < t < y.time for t in a[1].blanks) for x, y in again))

    def test_fixtures_are_current(self):
        """The committed timelines are the simulator's (times within a few microseconds: another libm may round
        a float's last bit differently)."""
        with tempfile.TemporaryDirectory() as tmp:
            names = sim.write_fixtures(Path(tmp))
            self.assertEqual(sorted(names), sorted(p.stem for p in (ROOT / "host/fixtures/reading_time").glob("*.tsv")))
            for name in names:
                head, made = sim.read_fixture(Path(tmp) / f"{name}.tsv")
                head0, kept = sim.read_fixture(ROOT / "host/fixtures/reading_time" / f"{name}.tsv")
                self.assertEqual([(c, t) for _, c, t in made], [(c, t) for _, c, t in kept], name)
                self.assertTrue(all(abs(a[0] - b[0]) <= 5 for a, b in zip(made, kept)), name)
                self.assertEqual(head["truth"]["characters"], head0["truth"]["characters"], name)

    def test_rules(self):
        """Each rule on gaps of 10 s (20 chars), 400 s (20), 10 s (2) and 40 minutes (20)."""
        r = recs([10, 400, 10, 2400], [20, 20, 2, 20, 20])
        self.assertEqual(sim.METHODS["Rubi 0.9.7 overlay: drop > 15 s"](r).seconds, 20)
        self.assertEqual(sim.METHODS["Rubi 0.9.7 tool: drop > 5 min"](r).seconds, 20)
        self.assertEqual(sim.METHODS["GSM 2025-09: cap 120 s"](r).seconds, 10 + 120 + 10 + 120)
        self.assertEqual(sim.METHODS["GSM 2025-12: cap 60 s"](r).seconds, 10 + 60 + 10 + 60)
        # 2026-03: max(15 s, 3 s a character), too few lines for the IQR stage
        self.assertEqual(sim.METHODS["GSM 2026-03: 3 s/char + IQR"](r).seconds, 10 + 60 + 10 + 60)
        # 2026-10: one sample (10 s for 20 chars: 2 a second; 400 s is over 300); the 40-minute gap ends the
        # session, uncredited
        self.assertEqual(sim.METHODS["GSM 2026-10: 2.5x median/session"](r).seconds, 10 + 25 + 2.5)
        # JL: chars / 10 a minute (6 s a character), at least 1.5 s
        self.assertEqual(sim.METHODS["JL: chars / 10 per minute"](r).seconds, 10 + 120 + 10 + 120)

    def test_measured_ordering(self):
        """2 runs of 1 h, two readers, three scenarios: the 15 s rule loses most of the time, the adaptive rule
        and GameSentenceMiner's current one a few percent."""
        scen = tuple(s for s in sim.SCENARIOS if s.name in ("clean", "afk breaks", "everyday"))
        profiles = (sim.PROFILES[1], sim.PROFILES[3])
        methods = {m: sim.METHODS[m] for m in ("Rubi 0.9.7 overlay: drop > 15 s", "Rubi adaptive (q75, 2.5x)",
                                               "GSM 2026-10: 2.5x median/session")}
        res = sim.evaluate(methods, seeds=2, profiles=profiles,
                           scenarios=tuple(sim.replace(s, hours=1.0) for s in scen))
        summ = sim.summary(res)
        self.assertGreater(summ["Rubi 0.9.7 overlay: drop > 15 s"][0], 30)
        self.assertLess(summ["Rubi adaptive (q75, 2.5x)"][0], 8)
        self.assertLess(summ["GSM 2026-10: 2.5x median/session"][0], 10)


if __name__ == "__main__":
    unittest.main()
