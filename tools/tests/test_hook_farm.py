"""tools/hook_farm.py: the farm checks' plumbing on a synthetic bucket (no game text; the farm's bucket is private).

One title with Rubi's real verified entry (PCSG00628, ALIA's CARNIVAL! サクラメント) and pages whose raw bytes are
made here from that engine's markup: check must build host/hook_farm_check.c, run the entry's real filter over
every hit and count the pages as the farm's text says (equal, a page whose text differs, a two-hit page, a C0
control vjo_hook_clean drops, white space at the farm text's ends); fixtures must write only the pages Rubi
reproduces exactly, as C the engine test includes, and the farm's record of each hook Rubi has an entry for (an SC3
title's too, without excerpts: tools/sc3_charset.py writes those).
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
# the farm's records of the two hooks (rubi-hook.json's fields: the site, its code bytes; no text)
RECORD_628 = {"title": "PCSG00628", "app_ver": "01.00", "register": 5, "encoding": "cp932", "padding": 0,
              "segment0_offset": "0x3373c", "module_nid": "0x1ee45f02", "thumb": 1,
              "mode_note": "Thumb-2 decodes cleanly", "signature": "f4620024c6f8b490a14695f9000018b1"}
RECORD_146 = {"title": "PCSG00146", "app_ver": "01.00", "register": 0, "encoding": "cp932", "padding": 0,
              "terminator": "sc3", "segment0_offset": "0x2b018", "module_nid": "0x2bac7a6d", "thumb": 1,
              "signature": "2de9f04f81b04af6a034c8f24b140025"}


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
        (folder / "hook" / "rubi-hook.json").write_text(json.dumps(RECORD_628))
        recs = [page("000001", ["rﾃｽﾄ[―――]ﾞﾟ"], "てすと―――！？"),          # Rubi's text: the farm's
                page("000002", ["名前r｢ｱ｣"], "名前\n「い」"),                  # the farm's text differs
                page("000003", ["名前r｢ｱ｣", "rﾃｽﾄ"], "名前\n「あ」\nてすと"),  # two hits, joined
                page("000004", ["rﾃ\x01ｽﾄk"], "てすと"),                     # a control vjo_hook_clean drops
                page("000005", ["rﾃｽﾄ"], "　てすと\n")]                      # the farm keeps the ends' blanks
        (folder / "pages.jsonl").write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in recs),
                                            encoding="utf-8")
        sc3 = Path(self.tmp, "vita", "PCSG00146 Test", "hook")  # an SC3 title: its record, no pages here
        sc3.mkdir(parents=True)
        (sc3 / "rubi-hook.json").write_text(json.dumps(RECORD_146))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(ROOT / "tools" / "hook_farm.py"), "--bucket", self.tmp] +
                              list(args), capture_output=True, text=True, check=True).stdout

    def test_check_counts(self):
        out = self.run_tool("check", "--show", "5")
        # 000004 exact (its control dropped), 000005 shown only (the farm's text has a blank at each end)
        self.assertIn("PCSG00628: 5 pages; single-hit 4: shown 3 (+0 default name), exact 2; multi-hit 1 "
                      "(hits joined: shown 1, exact 1)", out)
        self.assertIn("000002", out)  # the differing page is shown, with Rubi's text and the farm's
        self.assertIn("'名前\\n「あ」'", out)
        self.assertNotIn("000004", out)
        self.assertNotIn("000005", out)

    def test_check_cell_record_entry(self):
        """A page-build record's entry (PCSG00769, VJO_PLACE_READ_CELLS_B): the checker reads the farm's raw bytes as its
        cells, two bytes a character (audit-psandvita-1 N8: a checker that read them as a string passed every test). A
        single-byte character's cell has a 0 high byte, where a string read would stop; ㌫ is drawn as the game's default
        name (DEFAULT_NAMES), which the farm's text leaves out. Text made here, none of the game's."""
        folder = Path(self.tmp, "vita", "PCSG00769 Test")
        (folder / "hook").mkdir(parents=True)
        (folder / "hook" / "rubi-hook.json").write_text(json.dumps(dict(
            RECORD_628, title="PCSG00769", register=6, segment0_offset="0x31306", module_nid="0xd597d1ca",
            signature="b068c4f8e00eb069c4f8f00e307f84f8",
            reader={"schema": "vn-hook-reader/1", "encoding": "cp932", "data_offset": 46,
                    "length": {"offset": 32, "width": 4, "scale": 2, "unit": "bytes"}})))

        def cells(text):  # each character a little-endian cell: its first CP932 byte, then its second or 0
            return b"".join(c.encode("cp932").ljust(2, b"\0") for c in text).hex()
        recs = [{"id": pid, "review_state": "approved", "text_clean": farm,
                 "raw_text_events": [{"id": 1, "pc": 0x81031306, "encoding": "cp932", "raw_hex": cells(raw)}]}
                for pid, raw, farm in (("000001", "「Aあ」\n", "「Aあ」"), ("000002", "「㌫さん」\n", "「さん」"))]
        (folder / "pages.jsonl").write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in recs),
                                            encoding="utf-8")
        out = self.run_tool("check", "PCSG00769")
        self.assertIn("PCSG00769: 2 pages; single-hit 2: shown 1 (+1 default name), exact 1;", out)

    def test_the_farm_text_goes_with_the_last_hit(self):
        """The worker shows a site's latest hit: a page's text is the checker's farm text for its last hit only."""
        seen = []

        def fake(exe, rows, farm=None):
            seen.extend(zip([r[3] for r in rows], farm))
            return [("ok", "x", "x", f) for f in farm]
        real, hook_farm.run_checker = hook_farm.run_checker, fake
        try:
            hook_farm.check_title("unused", "PCSG00628", str(Path(self.tmp, "vita", "PCSG00628 Test")))
        finally:
            hook_farm.run_checker = real
        self.assertEqual(len(seen), 6)
        self.assertEqual(seen[2:4], [("名前r｢ｱ｣".encode("cp932"), ""), ("rﾃｽﾄ".encode("cp932"), "名前\n「あ」\nてすと")])

    def test_fixtures(self):
        inc = Path(self.tmp, "farm.inc")
        self.run_tool("fixtures", "--out", str(inc))
        text = inc.read_text(encoding="utf-8")
        self.assertIn("#define HOOK_FARM_FIXTURES 2", text)
        want = "てすと―――！？".encode("utf-8")
        self.assertIn('"%s"}, /* page 000001 */' % "".join("\\x%02X" % b for b in want), text)
        self.assertIn("/* page 000004 */", text)  # its control dropped: the farm's text exactly
        self.assertNotIn("page 000002", text)
        self.assertNotIn("page 000003", text)  # single-hit pages only
        self.assertNotIn("page 000005", text)  # equal as shown only
        # the farm's records of the two hooks, the SC3 one's too (its excerpts are tools/sc3_charset.py's)
        self.assertIn('{"PCSG00628", 0x1EE45F02u, 0x3373Cu, 1, 5, 0x00000000u, VJO_HOOK_ENC_CP932, '
                      'VJO_PLACE_READ_STRING, 0x00u, 0x00u, "\\xF4\\x62\\x00\\x24\\xC6\\xF8\\xB4\\x90\\xA1\\x46\\x95'
                      '\\xF9\\x00\\x00\\x18\\xB1", 16},', text)
        self.assertIn('{"PCSG00146", 0x2BAC7A6Du, 0x2B018u, 1, 0, 0x00000000u, VJO_HOOK_ENC_SC3, VJO_PLACE_READ_SC3,',
                      text)
        self.assertIn("#define HOOK_FARM_SITE_COUNT 2", text)
        self.assertNotIn('{"PCSG00146", 0x', text.split("#define HOOK_FARM_SITES")[0])

    def test_farm_site(self):
        rec = dict(RECORD_628, title="PCSG00380", register=4, padding=2174599152, segment0_offset="0x4de9e",
                   reader={"schema": "vn-hook-reader/1", "encoding": "cp932", "data_offset": 32,
                           "length": {"offset": 20, "width": 4, "scale": 2, "unit": "bytes"}})
        self.assertEqual(hook_farm.farm_site(rec)[2:9], (1, 4, 0x819DBFF0, "VJO_HOOK_ENC_CP932",
                                                         "VJO_PLACE_READ_CELLS", 20, 32))
        # PCSG00769's page-build record (psandvita #1): its count at +0x20, its cells from +0x2E
        rec = dict(RECORD_628, title="PCSG00769", register=6, padding=0, segment0_offset="0x31306",
                   reader={"schema": "vn-hook-reader/1", "encoding": "cp932", "data_offset": 46,
                           "length": {"offset": 32, "width": 4, "scale": 2, "unit": "bytes"}})
        self.assertEqual(hook_farm.farm_site(rec)[2:9], (1, 6, 0, "VJO_HOOK_ENC_CP932", "VJO_PLACE_READ_CELLS_B", 32, 46))
        with self.assertRaises(SystemExit):  # a layout neither read has
            hook_farm.farm_site(dict(rec, reader=dict(rec["reader"], data_offset=44)))
        rec = dict(RECORD_628, reader={"schema": "vn-hook-reader/1", "pointer_offsets": [0]})
        self.assertEqual(hook_farm.farm_site(rec)[2:7], (1, 5, 0, "VJO_HOOK_ENC_CP932", "VJO_PLACE_READ_DEREF"))
        # a record that says to check its mode by disassembly: the mode decided from exactly its bytes, else a stop
        # (audit-rubi-audit-fix-hooks A-F1)
        check = "check mode by disassembly"
        for title, sig, mode in (("PCSG00469", "2de9f04985b000210091041c049183f1", 1),
                                 ("PCSG00553", "0dc0a0e1f0592de920d04de20db0a0e1", 0)):
            rec = dict(RECORD_628, title=title, signature=sig.upper(), mode_note=check, thumb=1 - mode)
            self.assertEqual(hook_farm.farm_site(rec)[2], mode, title)
        for rec in (dict(RECORD_628, mode_note=check),  # a title not decided
                    dict(RECORD_628, title="PCSG00469", signature="2de9f04985b000210091041c049183f0", mode_note=check)):
            with self.assertRaises(SystemExit):
                hook_farm.farm_site(rec)
        for reader in ({"pointer_offsets": [0, 4]}, {"length": {"offset": 20, "width": 1, "scale": 4, "unit": "bytes"}},
                       {"stride": 2}):
            with self.assertRaises(SystemExit):
                hook_farm.farm_site(dict(RECORD_628, reader=reader))
        self.assertIsNone(hook_farm.farm_site({"title": "PCSG00667", "register": 0, "encoding": "utf-16le"}))

    def test_app_ver_bcd(self):
        self.assertEqual(hook_farm.app_ver_bcd("01.00"), 0x0100)
        self.assertEqual(hook_farm.app_ver_bcd("01.10"), 0x0110)
        self.assertEqual(hook_farm.app_ver_bcd(None), 0)
        self.assertEqual(hook_farm.app_ver_bcd("01.00 (VERSION 01.01)"), 0)


if __name__ == "__main__":
    unittest.main()
