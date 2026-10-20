"""tools/gen_luna_psp.py: Luna's PSP table as core/hook_luna_psp.inc holds it (the debug build's PSP probe).

The counts are reports/hooks-psp.md section 3's (415 entries, 354 title IDs; 13 entries name several IDs),
the committed .inc is the generator's output, and every (address, title ID) pair of ppsspp_1.cpp, read
here by a parser of this test's own, is in the .inc exactly once, under its title, in the table's order.
"""
import collections
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import gen_luna_psp  # noqa: E402

SRC = ROOT / "third_party" / "LunaTranslator" / "ppsspp_1.cpp"
INC = ROOT / "core" / "hook_luna_psp.inc"


def source_pairs():
    """[(address, [IDs])] of ppsspp_1.cpp's table, in source order: lines that start with `{0x`."""
    lines = SRC.read_text(encoding="utf-8").split("\n")
    start = next(i for i, line in enumerate(lines) if "emfunctionhooks_1[] = {" in line)
    out = []
    for line in lines[start + 1:]:
        s = line.strip()
        if s.startswith("};"):
            break
        m = re.match(r"\{\s*0[xX]([0-9a-fA-F]+)\s*,", s)
        if m:
            body = s.rsplit("}}", 1)[0]  # the entry, without its trailing comment
            out.append((int(m.group(1), 16), re.findall(r'"([A-Z]{4}\d{5})"', body)))
    return out


def inc_table():
    """{id: [addresses]} read back from core/hook_luna_psp.inc."""
    text = INC.read_text(encoding="utf-8")

    def block(name):
        m = re.search(name + r"\[[^\]]*\]\s*=\s*(.*?);", text, re.S)
        return m.group(1)

    ids = "".join(re.findall(r'"([A-Z0-9]*)"', block("vjo_luna_psp_ids")))
    ids = [ids[k:k + 9] for k in range(0, len(ids), 9)]
    counts = [int(x) for x in re.findall(r"\b(\d+)\b", block("vjo_luna_psp_count"))]
    addrs = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{8})u", block("vjo_luna_psp_addr"))]
    out, k = collections.OrderedDict(), 0
    for i, n in zip(ids, counts):
        out[i] = addrs[k:k + n]
        k += n
    assert k == len(addrs) and len(ids) == len(counts), "the .inc's arrays disagree"
    return out


class TestGenLunaPsp(unittest.TestCase):
    def test_counts(self):
        c = gen_luna_psp.counts()
        self.assertEqual(c["entries"], 415)
        self.assertEqual(c["addresses"], 415)
        self.assertEqual(c["titles"], 354)
        self.assertEqual(c["sites"], 431)
        self.assertEqual(c["multi_id_entries"], 13)
        self.assertEqual(c["max_sites_per_title"], 6)
        self.assertEqual(c["titles_with_2_or_more"], 69)
        self.assertEqual((c["min_addr"], c["max_addr"]), (0x08804094, 0x08B0449C))
        self.assertEqual(c["bytes"], 354 * 9 + 1 + 354 + 431 * 4)

    def test_source_parsers_agree(self):
        pairs = source_pairs()
        self.assertEqual(len(pairs), 415)
        self.assertEqual(len({i for _, ids in pairs for i in ids}), 354)
        self.assertEqual(sum(len(ids) for _, ids in pairs), 431)
        entries, _ = gen_luna_psp.table()
        self.assertEqual([(e["addr"], e["ids"]) for e in entries], pairs)

    def test_inc_is_fresh(self):
        self.assertEqual(INC.read_text(encoding="utf-8"), gen_luna_psp.render_inc())
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_luna_psp.py"), "--check"],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_round_trip(self):
        want = collections.OrderedDict()
        for addr, ids in source_pairs():
            for i in ids:
                want.setdefault(i, []).append(addr)
        got = inc_table()
        self.assertEqual(list(got), sorted(want))
        self.assertEqual(dict(got), dict(want))  # every pair once, each title's in the table's order

    def test_some_entries(self):
        got = inc_table()
        self.assertEqual(got["ULJM05889"], [0x08868AB8])  # ppsspp_1.cpp:1843
        self.assertEqual(got["NPJH50654"], [0x089DB26C])  # :1712, one entry for two IDs
        self.assertEqual(got["NPJH50655"], [0x089DB26C])
        self.assertEqual(got["NPJH50619"], [0x08952CFC, 0x0884AAD4, 0x0882E1B0, 0x088BB108, 0x089526A0,
                                            0x088BCEF8])  # :1814-1819
        self.assertNotIn("ULJM05334", got)  # MEMORIES OFF: a comment only (:1740)


if __name__ == "__main__":
    unittest.main()
