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
  hook_farm.py --bucket DIR fixtures [--out F]   the committed excerpts (host/tests/fixtures/hook_farm.inc)

check compares each single-hit page's text two ways: exactly, and as shown: both texts normalised as the worker
normalises what it shows (core/textnorm.h: ！ as !, the dashes, the ellipses...), white space at the two ends of
the text dropped (Rubi's filters trim the first line's indentation and blank lines, the farm keeps them), and the
farm's U+F8F0 read as U+3000 (its cp932 codec's 0xA0, a blank cell: its glyph-cell line labels have U+3000) and
its C0 controls dropped as vjo_hook_clean drops them (PCSG00380's button icons: Rubi draws no icon). A
page whose shown text differs only by a player name the filter writes (DEFAULT_NAMES: the farm's text dropped the
name code, its glyph count shows the name drawn) is counted apart. The worker keeps a site's latest hit, so a
page the farm built from several hits is compared with its hits' texts joined by line breaks.
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
                 "core/hook_text.c", "core/textnorm.c"]
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
                    "-o", exe] + [os.path.join(ROOT, s) for s in CHECK_SOURCES], check=True)
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
    chosen to cover the codes its hits hold (features), the most new ones per byte first."""
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_checker(tmp)
        out = ["/* Generated by tools/hook_farm.py fixtures from the psandvita farm's approved pages: per verified",
               " * entry, a few short hits (the raw bytes its hook read, as the farm recorded them) and the text the",
               " * farm verified against the frame the game drew, which Rubi gives exactly (the farm's U+F8F0, its",
               " * cp932 codec's 0xA0, a blank cell, as U+3000). Short excerpts only, never a script. Do not edit:",
               " * regenerate. */"]
        n_all = 0
        for tid, folder in titles(args.bucket):
            hook = load_json(os.path.join(folder, "hook", "rubi-hook.json")) or {}
            av = app_ver_bcd(hook.get("app_ver"))
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
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print("%s: %d excerpts" % (args.out, n_all))


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
