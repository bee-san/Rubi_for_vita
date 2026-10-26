#!/usr/bin/env python3
"""tools/yomitan_check/compare.py's classes on synthetic taps, and tools/gen_deinflect.py's packed
table read back as core/local_dict.c reads it (no dictionary, no Yomitan, no Node)."""
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/yomitan_check"))
import compare  # noqa: E402
import gen_deinflect  # noqa: E402


def rubi(spelling, reading, length, reasons=(), rank=0, dictionary="Bee's Ultimate Dictionary", more=()):
    return {"len": length, "spelling": spelling, "reading": reading, "rank": rank, "reasons": list(reasons),
            "dictionary": dictionary, "more": [dict(zip(("spelling", "reading", "dictionary"), m)) for m in more]}


def yomitan(term, reading, length, rules=(), processors=(), dictionaries=("Daijirin4 Plain",), freqs=(),
            pitches=(), names=(), word=None):
    entry = {"headwords": [{"term": term, "reading": reading, "sources": [[term, term, term, "exact", "term", True]]}],
             "dictionaries": list(dictionaries), "inflections": [{"source": "algorithm", "rules": list(rules)}],
             "processors": [list(processors)],
             "frequencies": [{"dictionary": "Jiten", "headwordIndex": 0, "hasReading": True, "frequency": f,
                              "displayValue": str(f)} for f in freqs],
             "pitches": [{"dictionary": "Kanjium", "headwordIndex": 0, "positions": [p]} for p in pitches]}
    return {"len": length, "entries": [entry], "names": [{"term": n, "reading": n} for n in names], "word": word}


class Classes(unittest.TestCase):
    RUBI_KEYS = {"わかる", "わかつ", "する", "かわる", "失礼", "しつれい", "しつらい", "かれ", "あれ", "そう"}
    YOMITAN_KEYS = {"わかる", "わかつ", "する", "かわる", "失礼", "しつれい", "かれ", "あれ", "そう"}

    def classify(self, r, y):
        return compare.classify("", 0, r, y, self.RUBI_KEYS, self.YOMITAN_KEYS)

    def test_same_entry_is_no_mismatch(self):
        cause, other = self.classify(rubi("分かる", "わかる", 4, ["past"], 278), yomitan("分かる", "わかる", 4, ["-た"], freqs=[278]))
        self.assertIsNone(cause)
        self.assertEqual(other, [])

    def test_deinflections(self):
        # Yomitan's longer inflected match, of a word Rubi's dictionary has
        self.assertEqual(self.classify(rubi("変", "へん", 1), yomitan("変わる", "かわる", 4, ["-ん"]))[0],
                         "deinflection_missing")
        # Rubi's longer inflected match that Yomitan does not take
        self.assertEqual(self.classify(rubi("失礼", "しつらい", 3, ["imperative negative"]),
                                       yomitan("失礼", "しつれい", 2))[0], "deinflection_extra")
        # same span, two dictionary forms
        self.assertEqual(self.classify(rubi("分かつ", "わかつ", 4, ["past"]), yomitan("分かる", "わかる", 4, ["-た"]))[0],
                         "deinflection_choice")

    def test_reading_spelling_and_names(self):
        self.assertEqual(self.classify(rubi("彼", "かれ", 1), yomitan("彼", "あれ", 1))[0], "reading")
        self.assertEqual(self.classify(rubi("然う", "そう", 2), yomitan("そう", "そう", 2))[0], "kana_kanji")
        self.assertEqual(self.classify(rubi("恐怖", "くふ", 2), yomitan("恐怖", "きょうふ", 2))[0], "reading")
        cause, other = self.classify(rubi("わたし", "わたし", 3, dictionary="VNDB Names — PSP & PS Vita",
                                          more=[("私", "わたし", "Bee's Ultimate Dictionary")]),
                                     yomitan("わたし", "わたし", 3, dictionaries=["VNDB Names — PSP & PS Vita"],
                                             names=["わたし"], word={"term": "私", "reading": "わたし", "len": 3}))
        self.assertIsNone(cause)
        self.assertIn("names_first_rubi", other)
        self.assertIn("names_first_yomitan", other)

    def test_frequency_and_pitch_are_counted_either_way(self):
        cause, other = self.classify(rubi("だと", "だと", 2, rank=73969), yomitan("だと", "だと", 2, pitches=[1]))
        self.assertIsNone(cause)
        self.assertEqual(sorted(other), ["frequency", "pitch"])


class PackedTable(unittest.TestCase):
    """The generated table decodes to exactly the rules it was made from."""

    def test_every_rule_decodes(self):
        text = (ROOT / "core/deinflect.inc").read_text(encoding="utf-8")
        blob = "".join(json.loads(s) for s in re.findall(r'^    ("(?:[^"\\]|\\.)*")$', text.split("static const uint8_t")[0],
                                                           re.M))
        rules_in = [int(x) for x in re.search(r"deinflect_rules_in\[\d+\] = \{([^}]*)\}", text).group(1).split(",")]
        rows = [tuple(map(int, m)) for m in re.findall(r"^    \{(\d+), (\d+), (\d+)\},$", text, re.M)]
        bits = {"v1": 1, "v5": 2, "vs": 4, "vk": 8, "vz": 16, "adj-i": 32, "iru": 64}
        labels = ["", *gen_deinflect.rule_sets()]
        want = sorted((r["kanaIn"], r["kanaOut"], label, sum(bits[x] for x in r["rulesIn"]),
                       sum(bits[x] for x in r["rulesOut"]))
                      for label, rules in gen_deinflect.rule_sets().items() for r in rules)
        got = sorted((blob[i & 0x7FF:(i & 0x7FF) + (i >> 11)], blob[o & 0x7FF:(o & 0x7FF) + (o >> 11)],
                      labels[b & 63], rules_in[b >> 13], b >> 6 & 127) for i, o, b in rows)
        self.assertEqual(got, want)
        self.assertEqual(len(rows), len(want))

    def test_supplement_changes_what_it_says(self):
        old = json.loads((ROOT / "third_party/yomichan/deinflect.json").read_text())
        supplement = json.loads((ROOT / "third_party/yomitan/deinflect-supplement.json").read_text())
        new = gen_deinflect.rule_sets()
        self.assertNotIn("imperative negative", new)  # the bare な is gone
        added = sum(len(rules) for part in ("add", "rubi") for rules in supplement[part].values())
        self.assertEqual(sum(map(len, old.values())) - len(supplement["remove"]) + added, sum(map(len, new.values())))
        self.assertEqual(sum(map(len, new.values())), 724)
        # left out, measured harmful (third_party/yomitan/README.md)
        ins = {r["kanaIn"] for rules in new.values() for r in rules}
        self.assertTrue({"んな", "さん", "たん", "はん", "なん"}.isdisjoint(ins))
        self.assertIn("かん", ins)


if __name__ == "__main__":
    unittest.main()
