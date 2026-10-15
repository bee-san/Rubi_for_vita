"""tools/model_tiers.json is the one table of the model sizes (Small, Normal, Large): its tiers, their
reserves, models and estimates are the same in every C table that holds them (include/vjo_vocr_sizes.h,
include/vjo_ledger_budget.h, core/config.c, core/vocr_models.c, kernel/game_ocr.c, game_ocr/main.c), and
each tier's model is within that tier's ceilings."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import prepare_ocr  # noqa: E402
from test_vocr_defaults import array_body, c_table, entries, macros, metadata_table  # noqa: E402

TIERS = json.loads((ROOT / "tools/model_tiers.json").read_text(encoding="utf-8"))


def c_tiers(source=None):
    """id -> (name, tagline, model, reserve) of core/vocr_models.c's tiers[]."""
    if source is None:
        source = (ROOT / "core/vocr_models.c").read_text()
    sizes = macros("include/vjo_ledger_budget.h")
    for m in re.finditer(r"#define (VJO_GAME_RESERVE_\w+) \((\d+)u \* 1024u \* 1024u\)",
                         (ROOT / "include/vjo_ledger_budget.h").read_text()):
        sizes[m.group(1)] = int(m.group(2)) * 1024 * 1024
    body = array_body(source, "static const VjoVocrTierInfo tiers[VJO_VOCR_TIERS]")
    rows = [(m.group(1), (m.group(2), m.group(3), sizes[m.group(4)], sizes[m.group(5)]))
            for m in re.finditer(r'\{"(\w+)", "(\w+)", "([^"]+)", (VJO_\w+), (VJO_\w+)\}', body)]
    if len(rows) != entries(body):
        raise AssertionError(f"core/vocr_models.c tiers[]: read {len(rows)} of {entries(body)} entries")
    return rows


class ModelTiers(unittest.TestCase):
    def test_the_table_has_the_agreed_schema(self):
        # overlay-test3-cleanup's schema (tools/analyse_selftest.py reads it): tiers ascending, these keys
        self.assertEqual(TIERS["default"], "normal")
        self.assertEqual(TIERS["box"], {"lines": 3, "line_px": 640, "cores": 1, "mhz": 444})
        self.assertIn("source", TIERS)
        self.assertEqual([t["id"] for t in TIERS["tiers"]], ["small", "normal", "large"])
        for tier in TIERS["tiers"]:
            for key in ("id", "name", "weights_max_bytes", "box_ms_under", "arena_max_bytes", "reserve_bytes",
                        "model", "tagline", "est_box_ms", "job_bytes", "need_bytes"):
                self.assertIn(key, tier, tier["id"])

    def test_the_c_tiers_are_the_table(self):
        sizes = macros()
        self.assertEqual(sizes["VJO_VOCR_TIERS"], len(TIERS["tiers"]))
        ids = [t["id"] for t in TIERS["tiers"]]
        self.assertEqual(sizes["VJO_VOCR_TIER_DEFAULT"], ids.index(TIERS["default"]) + 1)
        ledger = (ROOT / "include/vjo_ledger_budget.h").read_text()
        config = (ROOT / "core/config.c").read_text()
        self.assertIn('tier_ids[VJO_VOCR_TIERS] = {"' + '", "'.join(ids) + '"}', config)
        self.assertIn("\nvocr_tier = " + TIERS["default"] + "\n", (ROOT / "docs/config.example.ini").read_text())
        rows = c_tiers()
        self.assertEqual([r[0] for r in rows], ids)
        pins = c_table()
        meta = metadata_table(estimates=True)
        for n, (tier, (tid, (name, tagline, model, reserve))) in enumerate(zip(TIERS["tiers"], rows), 1):
            self.assertEqual((tid, name, tagline, model, reserve),
                             (tier["id"], tier["name"], tier["tagline"], tier["model"], tier["reserve_bytes"]))
            # the ABI's names and sizes of the tiers, which the kernel's broker and the worker open
            self.assertEqual(sizes[f"VJO_VOCR_TIER{n}_NAME"], tier["model"])
            self.assertEqual(sizes[f"VJO_VOCR_TIER{n}_BYTES"], pins[tier["model"]][0])
            reserve_macro = {"small": "SMALL", "normal": "NORMAL", "large": "LARGE"}[tier["id"]]
            got = int(re.search(rf"#define VJO_GAME_RESERVE_{reserve_macro} \((\d+)u \* 1024u \* 1024u\)",
                                ledger).group(1)) * 1024 * 1024
            self.assertEqual(got, tier["reserve_bytes"], tier["id"])
            # the Overlay app's estimates are the model's metadata
            _, _, _, box_ms, job_bytes = meta[tier["model"]]
            self.assertEqual((box_ms, job_bytes), (tier["est_box_ms"], tier["job_bytes"]), tier["id"])
            self.assertEqual(tier["need_bytes"], tier["job_bytes"] + tier["reserve_bytes"], tier["id"])
        # the kernel and the worker name the tiers by these macros, in this order
        for path in ("kernel/game_ocr.c", "game_ocr/main.c", "core/config.c"):
            self.assertIn("{VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME,", re.sub(r"\s+", " ",
                          (ROOT / path).read_text()).replace("{ VJO", "{VJO"), path)

    def test_each_tier_model_is_within_its_ceilings(self):
        """The owner's ceilings (2026-10-14 18:09Z weights, and the box times; the arena ceilings of
        vita-vn-ocr docs/model-tiers.md 1.3): each tier's model fits its own tier, and a larger tier's
        model is larger and slower. The times are estimates (est_box_basis)."""
        pins = c_table()
        previous = None
        for tier in TIERS["tiers"]:
            size, _, arena = pins[tier["model"]]
            self.assertLessEqual(size, tier["weights_max_bytes"], tier["id"])
            self.assertLessEqual(arena, tier["arena_max_bytes"], tier["id"])
            self.assertLess(tier["est_box_ms"], tier["box_ms_under"], tier["id"])
            if previous:
                self.assertGreater(size, pins[previous["model"]][0])
                self.assertGreater(tier["est_box_ms"], previous["est_box_ms"])
                self.assertGreater(tier["reserve_bytes"], previous["reserve_bytes"])
            previous = tier

    def test_each_tier_model_is_shipped_and_pinned_once(self):
        for tier in TIERS["tiers"]:
            assets, _ = prepare_ocr.release_assets(tier["model"])
            self.assertEqual(assets[tier["model"]][1], c_table()[tier["model"]][0])
        self.assertEqual(len({t["model"] for t in TIERS["tiers"]}), 3)

    def test_a_tier_table_that_disagrees_fails(self):
        source = (ROOT / "core/vocr_models.c").read_text()
        rows = dict(c_tiers(source))
        self.assertEqual(rows["large"][2], "VOCR4_W20_w8.vocr")
        broken = source.replace('{"large", "Large", "most accurate", VJO_VOCR_TIER3_NAME, VJO_GAME_RESERVE_LARGE}',
                                '{"large", "Large", "most accurate", VJO_VOCR_TIER2_NAME, VJO_GAME_RESERVE_LARGE}')
        self.assertNotEqual(broken, source)
        self.assertEqual(dict(c_tiers(broken))["large"][2], "VOCR3_H15_w8.vocr")  # read, and so it would differ
        with self.assertRaises(AssertionError):  # an entry in another form is not silently skipped
            c_tiers(source.replace('{"small", "Small",', '{.id = "small", "Small",'))


if __name__ == "__main__":
    unittest.main()
