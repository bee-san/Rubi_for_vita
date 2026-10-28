"""README.md's hook counts are the tables' (#418, P9): the native text hooks section's numbers of LunaTranslator's
titles and ported sites, of Rubi's own verified entries (core/hook_place.c, beside CLANNAD's own row), of those that are
games of Luna's table and of those at one of Luna's sites, and the total of title IDs with a game-specific entry, as
tools/gen_luna_vita.py reads the tables for docs/hook-titles.md (whose generated part ctest hook_luna_table checks)."""
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import gen_luna_vita as gen  # noqa: E402

CLANNAD = "PCSG00415"  # its own row in the README's table
PATTERNS = {
    "luna_titles": r"\*\*(\d+) Vita title IDs from LunaTranslator's table\*\*",
    "luna_sites": r"using (\d+) ported hook sites",
    "rubi_more": r"\*\*(\d+) more Vita title IDs with Rubi's own verified entries\*\*",
    "rubi_in_luna": r"(\d+) are games of Luna's table: for \d+ of them",
    "rubi_at_luna_site": r"\d+ are games of Luna's table: for (\d+) of them",
    "total": r"\*\*(\d+) Vita title IDs with game-specific hook entries\*\*",
}


def counts():
    """The numbers the README states, from the tables."""
    with mock.patch("sys.stdout"):
        _, sites, _, _, _ = gen.vita_sites()
    rubi = gen.rubi_entries(sites)
    ported = [s for s in sites if s["ported"]]
    luna_titles = {s["id"] for s in ported}
    more = [r for r in rubi if r["title"] != CLANNAD]
    return {"luna_titles": len(luna_titles), "luna_sites": len(ported), "rubi_more": len(more),
            "rubi_in_luna": sum(r["luna"] != "no" for r in more),
            "rubi_at_luna_site": sum(r["luna"] == "its site" for r in more),
            "total": len(luna_titles | {r["title"] for r in rubi})}


def stated(text):
    """The numbers in a README text (None for a sentence it lacks)."""
    flat = " ".join(text.split())
    found = {}
    for key, pattern in PATTERNS.items():
        m = re.findall(pattern, flat)
        found[key] = int(m[0]) if len(m) == 1 else None
    return found


class DocsHookCounts(unittest.TestCase):
    def test_the_readme_states_the_tables_counts(self):
        want = counts()
        self.assertEqual(stated((ROOT / "README.md").read_text(encoding="utf-8")), want,
                         "README.md's native text hooks section: update its numbers to the tables'")

    def test_the_counts_are_read(self):
        c = counts()
        self.assertGreater(c["luna_titles"], 100)
        self.assertGreater(c["rubi_more"], 30)
        self.assertLessEqual(c["rubi_at_luna_site"], c["rubi_in_luna"])
        # the total counts a title once: Luna's ported titles, Rubi's own, CLANNAD; at most their sum
        self.assertLessEqual(c["total"], c["luna_titles"] + c["rubi_more"] + 1)
        self.assertGreaterEqual(c["total"], c["luna_titles"] + c["rubi_more"] + 1 - c["rubi_in_luna"])

    def test_the_console_column_is_the_sources(self):
        """tools/gen_luna_vita.py's RUBI_CONSOLE (the doc's Console column) and core/hook_place.c's comments agree: a
        title whose comment says it ran on a console is in it, and a title in it has a comment that speaks of one."""
        said = gen.rubi_comments((ROOT / "core/hook_place.c").read_text(encoding="utf-8"))
        ran = re.compile(r"(?i)proven on a console|console-tested|on a console \(|on the owner's (?:vita|console)")
        self.assertEqual({t for t, text in said.items() if ran.search(text)} - set(gen.RUBI_CONSOLE), set(),
                         "core/hook_place.c says these ran on a console: add them to RUBI_CONSOLE")
        for title in gen.RUBI_CONSOLE:
            self.assertRegex(said.get(title, ""), r"(?i)console", title)
        self.assertIn("PCSG01198", {t for t, text in said.items() if ran.search(text)})  # the rule finds one

    def test_a_stale_number_is_found(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        n = stated(text)["rubi_more"]
        self.assertIsNotNone(n)
        stale = text.replace(f"**{n} more Vita title IDs", f"**{n - 1} more Vita title IDs", 1)
        self.assertEqual(stated(stale)["rubi_more"], n - 1)
        self.assertIsNone(stated("no such sentence")["total"])


if __name__ == "__main__":
    unittest.main()
