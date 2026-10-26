"""tools/hook_farm.py: the farm checks' plumbing on a synthetic bucket (no game text; the farm's bucket is private).

One title with Rubi's real verified entry (PCSG00628, ALIA's CARNIVAL! サクラメント) and three pages whose raw bytes are
made here from that engine's markup: check must build host/hook_farm_check.c, run the entry's real filter over
every hit and count the pages as the farm's text says (equal, a page whose text differs, a two-hit page);
fixtures must write only the pages Rubi reproduces exactly, as C the engine test includes.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import hook_farm  # noqa: E402

SITE = 0x81000000 + 0x3373C  # Rubi's PCSG00628 entry


def page(pid, raws, text):
    return {"id": pid, "review_state": "approved", "text_clean": text,
            "raw_text_events": [{"id": k + 1, "pc": SITE, "encoding": "cp932", "raw_hex": r.encode("cp932").hex()}
                                for k, r in enumerate(raws)]}


@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "no C compiler")
class HookFarm(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        folder = Path(self.tmp, "vita", "PCSG00628 Test")
        (folder / "hook").mkdir(parents=True)
        (folder / "hook" / "rubi-hook.json").write_text(json.dumps({"title": "PCSG00628", "app_ver": "01.00"}))
        recs = [page("000001", ["rﾃｽﾄ[―――]ﾞﾟ"], "てすと―――！？"),          # Rubi's text: the farm's
                page("000002", ["名前r｢ｱ｣"], "名前\n「い」"),                  # the farm's text differs
                page("000003", ["名前r｢ｱ｣", "rﾃｽﾄ"], "名前\n「あ」\nてすと")]  # two hits, joined
        (folder / "pages.jsonl").write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in recs),
                                            encoding="utf-8")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(ROOT / "tools" / "hook_farm.py"), "--bucket", self.tmp] +
                              list(args), capture_output=True, text=True, check=True).stdout

    def test_check_counts(self):
        out = self.run_tool("check", "--show", "5")
        self.assertIn("PCSG00628: 3 pages; single-hit 2: shown 1 (+0 default name), exact 1; multi-hit 1 "
                      "(hits joined: shown 1, exact 1)", out)
        self.assertIn("000002", out)  # the differing page is shown, with Rubi's text and the farm's
        self.assertIn("'名前\\n「あ」'", out)

    def test_fixtures(self):
        inc = Path(self.tmp, "farm.inc")
        self.run_tool("fixtures", "--out", str(inc))
        text = inc.read_text(encoding="utf-8")
        self.assertIn("#define HOOK_FARM_FIXTURES 1", text)
        want = "てすと―――！？".encode("utf-8")
        self.assertIn('"%s"}, /* page 000001 */' % "".join("\\x%02X" % b for b in want), text)
        self.assertNotIn("page 000002", text)
        self.assertNotIn("page 000003", text)  # single-hit pages only

    def test_app_ver_bcd(self):
        self.assertEqual(hook_farm.app_ver_bcd("01.00"), 0x0100)
        self.assertEqual(hook_farm.app_ver_bcd("01.10"), 0x0110)
        self.assertEqual(hook_farm.app_ver_bcd(None), 0)
        self.assertEqual(hook_farm.app_ver_bcd("01.00 (VERSION 01.01)"), 0)


if __name__ == "__main__":
    unittest.main()
