#!/usr/bin/env python3
"""Rubi's verified hook entries against a hook farm's recorded pages (docs/native-hooks.md#farm-checks).

The farm (bee-san/psandvita's Vita3K screenshot farm; its private data bucket) keeps, per title,
<platform>/<TITLE_ID> <title>/hook/rubi-hook.json (the site: module NID, segment-0 offset, register,
encoding, the 16 bytes there) and pages.jsonl.zst: every page an AI reviewer approved against the frame the
game drew, with the raw bytes the hook read (raw_text_events[].raw_hex) and the text the farm derived from
them (text_clean: what the farm verified as drawn). The bucket is private (copyrighted game text): nothing of it is committed but short excerpts.

  hook_farm.py --bucket DIR inventory            the Vita titles, their farm hook and Rubi's entry (Markdown)
  hook_farm.py --bucket DIR check [TITLE ...]    Rubi's text for every recorded hit (host/hook_farm_check.c,
                                                 built here with cc) against the farm's page text
  hook_farm.py --bucket DIR fixtures [--out F]   the committed excerpts and the farm's hook records
                                                 (host/tests/fixtures/hook_farm.inc)

check compares each single-hit page's text two ways: exactly, and as shown: both texts normalised as the worker
normalises what it shows (core/textnorm.h: ！ as !, the dashes, the ellipses...), white space at the two ends of
the text dropped (Rubi's filters trim the first line's indentation and blank lines, the farm keeps them), and the
farm's U+F8F0 read as U+3000 (its cp932 codec's 0xA0, a drawn blank cell: the farm's line labels leave the blank out,
but their glyph counts are one higher per blank, 164 of the 165 labels that span one in PCSG00540's pages) and
its C0 controls dropped as vjo_hook_clean drops them (PCSG00380's button icons: Rubi draws no icon). A
page whose shown text differs only by a player name the filter writes (DEFAULT_NAMES: the farm's text dropped the
name code, its glyph count shows the name drawn) is counted apart. The worker keeps a site's latest hit, so a
page the farm built from several hits is compared with its hits' texts joined by line breaks.

fixtures writes, per title Rubi has an entry for at the farm's site, the farm's record of its hook (rubi-hook.json:
the module NID, offset, mode, register, padding, encoding, the reader and the 16 bytes there), which the engine test
installs as the game for Rubi's entry (#316's audit F3: not Rubi's own NID, bytes, register and padding), and for
each such title but the SC3 ones (their excerpts are tools/sc3_charset.py --fixtures') up to 5 short excerpts.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEG0 = 0x81000000
CHECK_SOURCES = ["host/hook_farm_check.c", "core/hook_place.c", "core/hook_luna.c", "core/hook_re.c",
                 "core/hook_text.c", "core/hook_sc3.c", "core/textnorm.c"]
CODECS = {"cp932": "cp932", "utf-8": "utf-8", "utf-16le": "utf-16-le"}
# The player names Rubi's filters write for a name code (core/hook_luna.c VJO_RF_*): where the farm's text
# dropped the code instead, the glyph count of the farm's own boxes shows the name drawn (PCSG00469 page 80:
# 14 glyphs, 私――九楼撫子は小学６年生。).
DEFAULT_NAMES = {"PCSG00380": ["北條", "紗希"], "PCSG00448": ["片岡", "ユエ"], "PCSG00469": ["撫子"],
                 "PCSG00509": ["鈴"], "PCSG00853": ["花宮", "かずは", "カズハ"]}
WS = " \t\r\n\u3000"


def shown_form(text, farm=False):
    """A shown text for comparison: the ends' white space dropped; the farm's U+F8F0 as U+3000."""
    if farm:  # and the controls the farm's text keeps (PCSG00380's button icons), which vjo_hook_clean drops
        text = "".join(c for c in text.replace("\uf8f0", "\u3000") if c >= " " or c in "\n\t\r")
    return text.strip(WS)


def titles(bucket, platform="vita"):
    """(title_id, folder) of every title folder of a platform, sorted by title ID."""
    base = os.path.join(bucket, platform)
    out = []
    for name in sorted(os.listdir(base)):
        path = os.path.join(base, name)
        if os.path.isdir(path) and re.match(r"^[A-Z]{4}\d{5} ", name):
            out.append((name[:9], path))
    return out


def load_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None


def pages(folder):
    """The folder's page records (pages.jsonl.zst, through the zstd tool; or pages.jsonl), in order."""
    path = os.path.join(folder, "pages.jsonl.zst")
    if os.path.exists(path):
        raw = subprocess.run(["zstd", "-dc", path], check=True, capture_output=True).stdout
    elif os.path.exists(path[:-4]):
        with open(path[:-4], "rb") as f:
            raw = f.read()
    else:
        return []
    return [json.loads(line) for line in raw.decode("utf-8").splitlines() if line.strip()]


def app_ver_bcd(text):
    """"01.00" -> 0x0100 (as game_ocr/hooks.c app_version codes it); anything else -> 0."""
    m = re.match(r"^(\d{1,2})\.(\d{2})$", text or "")
    return int(m.group(1), 16) << 8 | int(m.group(2), 16) if m else 0


def build_checker(out_dir):
    exe = os.path.join(out_dir, "hook_farm_check")
    cc = os.environ.get("CC", "cc")
    subprocess.run([cc, "-O1", "-std=gnu11", "-I", os.path.join(ROOT, "core"), "-I", os.path.join(ROOT, "include"),
                    '-DVJO_SOURCE_DIR="%s"' % ROOT, "-o", exe] + [os.path.join(ROOT, s) for s in CHECK_SOURCES],
                   check=True)
    return exe


def run_checker(exe, rows, farm=None):
    """rows: (title, offset, app_ver, raw bytes) -> [(status, text)] in order; with farm (the farm's texts, one
    per row), [(status, text, shown, farm_shown)]: both normalised as the worker shows a text."""
    farm = farm or [""] * len(rows)
    data = "".join("%s\t%x\t%x\t%s\t%s\n" % (t, off, av, raw.hex(), f.encode("utf-8").hex())
                   for (t, off, av, raw), f in zip(rows, farm))
    out = subprocess.run([exe], input=data.encode(), capture_output=True, check=True).stdout.decode()
    res = []
    for line in out.splitlines():
        cols = line.split("\t")
        dec = [bytes.fromhex(c).decode("utf-8", "replace") for c in cols[1:4]]
        res.append((cols[0], dec[0], dec[1], dec[2]))
    if len(res) != len(rows):
        sys.exit("hook_farm_check: %d results for %d rows" % (len(res), len(rows)))
    return res


def page_hits(rec):
    """The page's hits: (segment-0 offset, raw bytes) in the order the farm recorded them."""
    return [(ev["pc"] - SEG0, bytes.fromhex(ev["raw_hex"])) for ev in rec.get("raw_text_events", [])]


def check_title(exe, tid, folder, show=5):
    """Rubi's text for every hit of the title's approved pages against the farm's: counts, and the first show
    single-hit pages whose shown text differs (raw event, Rubi's text, the farm's)."""
    hook = load_json(os.path.join(folder, "hook", "rubi-hook.json")) or {}
    av = app_ver_bcd(hook.get("app_ver"))
    recs = [r for r in pages(folder) if r.get("review_state") == "approved"]
    rows, where, farm = [], [], []
    for r in recs:
        hits = page_hits(r)
        for k, (off, raw) in enumerate(hits):
            rows.append((tid, off, av, raw))
            where.append((r["id"], k))
            farm.append(farm_text(r) if k == len(hits) - 1 else "")
    res = run_checker(exe, rows, farm) if rows else []
    by_page = {}
    for (pid, k), got in zip(where, res):
        by_page.setdefault(pid, []).append(got)
    st = dict.fromkeys(("pages", "single", "single_exact", "single_shown", "single_name", "multi", "multi_exact",
                        "multi_shown", "noentry", "empty", "cut"), 0)
    names = DEFAULT_NAMES.get(tid, [])

    def unnamed(t):
        for nm in names:
            t = t.replace(nm, "")
        return t
    st["pages"] = len(recs)
    bad = []
    for r in recs:
        got = by_page.get(r["id"], [])
        want = farm_text(r)
        if not got or any(g[0] == "noentry" for g in got):
            st["noentry"] += 1
            continue
        st["empty"] += sum(1 for g in got if g[0] == "empty")
        st["cut"] += sum(1 for g in got if g[0] == "cut")
        want_shown = shown_form(got[-1][3], farm=True)
        if len(got) == 1:
            st["single"] += 1
            st["single_exact"] += got[0][1] == want
            mine = shown_form(got[0][2])
            if mine == want_shown:
                st["single_shown"] += 1
            elif names and unnamed(mine) == want_shown:
                st["single_name"] += 1
            else:
                bad.append((r["id"], r["raw_text_events"][0], got[0][1], want))
        else:
            st["multi"] += 1
            st["multi_exact"] += "\n".join(g[1] for g in got if g[1]) == want
            joined = shown_form("\n".join(g[2] for g in got if g[2]))
            st["multi_shown"] += joined == want_shown or (bool(names) and unnamed(joined) == want_shown)
    return st, bad[:show], len(bad)


def farm_text(rec):
    """The text the farm verified as drawn: text_clean (codes removed, ruby base kept, line-break codes as line
    breaks, the name on its own line), else text."""
    t = rec.get("text_clean")
    return t if t is not None else (rec.get("text") or "")


def decoded(ev):
    raw = bytes.fromhex(ev["raw_hex"])
    return raw.decode(CODECS.get(ev.get("encoding"), "latin-1"), "replace")


def cmd_check(args):
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_checker(tmp)
        total = {}
        for tid, folder in titles(args.bucket):
            if args.titles and tid not in args.titles:
                continue
            st, bad, nbad = check_title(exe, tid, folder, args.show)
            if st["noentry"] == st["pages"]:
                if args.titles:
                    print("%s: no verified entry at the farm's site" % tid)
                continue
            for k, v in st.items():
                total[k] = total.get(k, 0) + v
            print("%s: %d pages; single-hit %d: shown %d (+%d default name), exact %d; multi-hit %d (hits joined: "
                  "shown %d, exact %d); no entry %d, empty %d, cut %d" % (
                      tid, st["pages"], st["single"], st["single_shown"], st["single_name"], st["single_exact"],
                      st["multi"], st["multi_shown"], st["multi_exact"], st["noentry"], st["empty"], st["cut"]))
            for pid, ev, got, want in bad:
                print("   %s raw %r" % (pid, decoded(ev)[:160]))
                print("      got  %r" % got[:160])
                print("      want %r" % want[:160])
            if nbad > len(bad):
                print("   ... %d more" % (nbad - len(bad)))
        if total:
            print("all: single-hit %d: shown %d (+%d default name), exact %d; multi-hit %d (joined: shown %d, exact %d)"
                  % (total["single"], total["single_shown"], total["single_name"], total["single_exact"],
                     total["multi"], total["multi_shown"], total["multi_exact"]))


def cmd_inventory(args):
    sys.path.insert(0, ROOT)
    print("| Title | ID | APP_VER | Farm hook | Pages |")
    print("| --- | --- | --- | --- | --- |")
    for tid, folder in titles(args.bucket):
        hook = load_json(os.path.join(folder, "hook", "rubi-hook.json")) or {}
        recs = pages(folder)
        site = "r%s %s +%s" % (hook.get("register"), hook.get("encoding"), hook.get("segment0_offset"))
        print("| %s | %s | %s | %s | %d |" % ((hook.get("game_title") or "").replace("\n", " "), tid,
                                               hook.get("app_ver"), site, len(recs)))


def c_bytes(raw):
    return "".join("\\x%02X" % b for b in raw)


ENC_NAMES = {"cp932": "VJO_HOOK_ENC_CP932", "utf-8": "VJO_HOOK_ENC_UTF8", "utf-16le": "VJO_HOOK_ENC_UTF16LE"}
# The mode of the farm's records whose note says to check it by disassembly (their recorded thumb flag is not the
# code's), decided from the 16 bytes the record holds with LLVM 18.1.8's llvm-mc (`--disassemble`, `-triple
# thumbv7-none-eabi` and `armv7-none-eabi`): title -> (those bytes, 1 Thumb or 0 ARM). audit-rubi-audit-fix-hooks A-F1:
# without it the engine test left these entries' mode unchecked. A record with other bytes stops `fixtures` until its
# mode is decided here.
MODES_BY_DISASSEMBLY = {
    # push.w {r4-r8, r11, lr}; sub sp, #20; movs r1, #0; ... (as ARM its last two words do not decode)
    "PCSG00469": ("2de9f04985b000210091041c049183f1", 1),
    # adds r2, r5, #0; adds.w r0, r4, #0x9c0; ... (at +0x12C76, 2 mod 4, where no ARM instruction can start)
    "PCSG00509": ("2a1c14f51c6000255ff482710023fef7", 1),
    # mov r12, sp; push {r4-r8, r11, r12, lr}; sub sp, sp, #32; mov r11, sp (as Thumb its fourth halfword does not
    # decode); the same bytes for PCSG00807 and PCSG00706 (Rejet's Unity builds; PCSG00706's HistoryManager.Add, #394)
    "PCSG00553": ("0dc0a0e1f0592de920d04de20db0a0e1", 0),
    "PCSG00807": ("0dc0a0e1f0592de920d04de20db0a0e1", 0),
    "PCSG00706": ("0dc0a0e1f0592de920d04de20db0a0e1", 0),
}


def farm_mode(hook):
    """The record's mode: its thumb flag, or for a record that says to check its mode by disassembly the decided one
    (MODES_BY_DISASSEMBLY, for exactly the bytes it was decided on)."""
    if "check mode" not in (hook.get("mode_note") or ""):
        return int(hook["thumb"])
    sig, mode = MODES_BY_DISASSEMBLY.get(hook.get("title"), (None, None))
    if sig != (hook.get("signature") or "").lower():
        raise SystemExit("%s: the farm's record says to check its mode by disassembly: decide it from its bytes (%s) "
                         "and add it to MODES_BY_DISASSEMBLY" % (hook.get("title"), hook.get("signature")))
    return mode


def farm_site(hook):
    """The farm's record of its hook (rubi-hook.json) in the terms of Rubi's VjoPlaceVerified, or None for a record
    without a site: (offset, module NID, Thumb (farm_mode: the flag, or the mode the bytes decode to where the record
    says to check it by disassembly), register, padding (the farm's; an address of the game's when it is 0x81000000 or
    more: its register is 0 at the site, VNCAPTURE_HOOK_OFFSET), encoding, read, the reader's count and data offsets (a
    cell record's), the 16 bytes)."""
    if not hook.get("segment0_offset") or not hook.get("signature") or not hook.get("module_nid"):
        return None
    reader = {k: v for k, v in (hook.get("reader") or {}).items() if k not in ("schema", "encoding", "provenance")}
    count_off = data_off = 0
    if hook.get("terminator") == "sc3":  # MAGES' script text, through its end token (tools/sc3_charset.py)
        enc, read = "VJO_HOOK_ENC_SC3", "VJO_PLACE_READ_SC3"
    else:
        enc = ENC_NAMES[hook["encoding"]]
        length = reader.pop("length", None)
        pointers = reader.pop("pointer_offsets", None)
        data = reader.pop("data_offset", None)
        if reader:
            raise SystemExit("%s: a reader Rubi has no read for: %s" % (hook.get("title"), sorted(reader)))
        if length is not None:  # a count of 16-bit cells, then the cells (VJO_PLACE_READ_CELLS)
            if (length.get("width"), length.get("scale"), length.get("unit")) != (4, 2, "bytes") or pointers:
                raise SystemExit("%s: a length reader Rubi has no read for: %r" % (hook.get("title"), length))
            read, count_off, data_off = "VJO_PLACE_READ_CELLS", int(length["offset"]), int(data or 0)
        elif pointers == [0]:  # the string's pointer there (VJO_PLACE_READ_DEREF)
            read = "VJO_PLACE_READ_DEREF"
        elif pointers or data:
            raise SystemExit("%s: a pointer reader Rubi has no read for: %r" % (hook.get("title"), pointers))
        else:
            read = "VJO_PLACE_READ_STRING"
    sig = bytes.fromhex(hook["signature"])
    thumb = farm_mode(hook)
    return (int(hook["segment0_offset"], 16), int(hook["module_nid"], 16), thumb, int(hook["register"]),
            int(hook.get("padding") or 0) & 0xFFFFFFFF, enc, read, count_off, data_off, sig)


def features(raw_text, text):
    """What a hit exercises: each ASCII character of its raw text (the codes are ASCII), the marks the filters
    map (half-width kana, ･ ﾞ ﾟ, ＊, 《》, U+3000, the 0xA0 blank), and a line break in its text."""
    f = {c for c in raw_text if "!" <= c <= "~"}
    for c in "･ﾞﾟ＊《》\u3000\uf8f0":
        if c in raw_text:
            f.add(c)
    if any("\uff66" <= c <= "\uff9d" for c in raw_text):
        f.add("kana")
    if "\n" in text:
        f.add("\\n")
    return f


def cmd_fixtures(args):
    """Short excerpts of the farm's single-hit pages for the entries Rubi has: per title, at most
    args.per_title pages of at most args.max_bytes raw bytes that Rubi turns into exactly the farm's text (its
    U+F8F0 as U+3000),
    chosen to cover the codes its hits hold (features), the most new ones per byte first; and per title Rubi has an
    entry for at the farm's site (SC3 ones too, whose excerpts are tools/sc3_charset.py --fixtures'), the farm's
    record of the hook (farm_site)."""
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_checker(tmp)
        out = ["/* Generated by tools/hook_farm.py fixtures from the psandvita farm's approved pages: per verified",
               " * entry, a few short hits (the raw bytes its hook read, as the farm recorded them) and the text the",
               " * farm verified against the frame the game drew, which Rubi gives exactly (the farm's U+F8F0, its",
               " * cp932 codec's 0xA0, a blank cell, as U+3000). Short excerpts only, never a script. Do not edit:",
               " * regenerate. */"]
        sites = []
        n_all = 0
        for tid, folder in titles(args.bucket):
            hook = load_json(os.path.join(folder, "hook", "rubi-hook.json")) or {}
            av = app_ver_bcd(hook.get("app_ver"))
            # the farm's record, of a title Rubi has an entry for at its site (the checker finds one there)
            at = hook.get("segment0_offset")
            if at and run_checker(exe, [(tid, int(at, 16), av, b"")])[0][0] != "noentry":
                site = farm_site(hook)
                off, nid, thumb, reg, pad, enc, read, count_off, data_off, sig = site
                sites.append("    {\"%s\", 0x%08Xu, 0x%Xu, %d, %d, 0x%08Xu, %s, %s, 0x%02Xu, 0x%02Xu, \"%s\", %d}," % (
                    tid, nid, off, thumb, reg, pad, enc, read, count_off, data_off, c_bytes(sig), len(sig)))
                if read == "VJO_PLACE_READ_SC3":
                    continue
            recs = [r for r in pages(folder) if r.get("review_state") == "approved" and len(r["raw_text_events"]) == 1]
            cands = []
            for r in recs:
                (off, raw), = page_hits(r)
                if len(raw) > args.max_bytes or not farm_text(r):
                    continue
                cands.append((r, off, raw))
            if not cands:
                continue
            res = run_checker(exe, [(tid, off, av, raw) for _, off, raw in cands])
            if all(g[0] == "noentry" for g in res):
                continue
            # the farm's text, its codec's U+F8F0 (CP932 0xA0, drawn as a blank cell) read as U+3000
            good = [(c, g[1]) for c, g in zip(cands, res)
                    if g[0] == "ok" and g[1] == farm_text(c[0]).replace("\uf8f0", "\u3000")]
            feats = [features(decoded(c[0]["raw_text_events"][0]), t) for c, t in good]
            pick, have = [], set()
            while len(pick) < args.per_title:
                best, score = None, 0.0
                for k, (c, t) in enumerate(good):
                    if k in pick:
                        continue
                    new = len(feats[k] - have)
                    sc = (new + 0.01) / (len(c[2]) + 8)
                    if new and sc > score or (best is None and not new and not pick):
                        best, score = k, sc
                if best is None:
                    break
                pick.append(best)
                have |= feats[best]
            for k in pick:
                (r, off, raw), text = good[k]
                out.append("    {\"%s\", 0x%Xu, 0x%04Xu, \"%s\", %d, \"%s\"}, /* page %s */" % (
                    tid, off, av, c_bytes(raw), len(raw), c_bytes(text.encode("utf-8")), r["id"]))
                n_all += 1
        out.append("#define HOOK_FARM_FIXTURES %d" % n_all)
        text = "\n".join(out[:5]) + "\n#define HOOK_FARM_ROWS \\\n" + " \\\n".join(out[5:-1]) + "\n" + out[-1] + "\n"
        # the farm's hook records (farm_site): {title, NID, offset, Thumb (-1: unchecked), register, padding,
        # encoding, read, a cell record's count and data offsets, the bytes at the site, their number}
        text += ("/* The farm's record of each hook Rubi has an entry for (rubi-hook.json, its reader): the game the\n"
                 " * engine test runs Rubi's entry in. */\n#define HOOK_FARM_SITES \\\n" + " \\\n".join(sites) +
                 "\n#define HOOK_FARM_SITE_COUNT %d\n" % len(sites))
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print("%s: %d excerpts, %d hook records" % (args.out, n_all, len(sites)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bucket", required=True, help="the farm bucket's root (vita/, psp/, index.json)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("inventory")
    p.set_defaults(func=cmd_inventory)
    p = sub.add_parser("check")
    p.add_argument("titles", nargs="*")
    p.add_argument("--show", type=int, default=5, help="mismatches shown per title")
    p.set_defaults(func=cmd_check)
    p = sub.add_parser("fixtures")
    p.add_argument("--out", default=os.path.join(ROOT, "host", "tests", "fixtures", "hook_farm.inc"))
    p.add_argument("--per-title", type=int, default=5)
    p.add_argument("--max-bytes", type=int, default=128)
    p.set_defaults(func=cmd_fixtures)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
