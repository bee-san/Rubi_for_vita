#!/usr/bin/env python3
"""The text box suggested on first use (#358, core/region_suggest.h, docs/first-box.md), on hooked pages.

    python3 tools/firstbox_eval.py --binary build/host/vjo-page-ocr --models DIR --pagepool DIR [--pages DIR] \\
        [--per-title 60] [--exclude T ...] [--jobs 5] -o OUT.json

--pagepool: vita-vn-ocr's hooked page pool (vocr6-ingest pagepool/v1: one JSONL per title, each page's hook text and its
verified line boxes, kind "line" the dialogue and "name" a speaker's tag), pages beside it in ../../pages (--pages).
Up to --per-title pages of each title, spread over its pages (complete, trusted, horizontal dialogue), are read whole,
as a game with no region of its own is: each through host/page_ocr.c, the game worker's job on the defaults of
config.ini, which prints the box the worker suggests (vjo_vocr_suggest). A PSP page is read as Rubi sees it, shown 2x
by Adrenaline (a bilinear upscale, as tools/gate_eval.py), its boxes in those pixels.

A suggestion is saved once for every later screen of its game, so each page's box is scored on the title's other
sampled pages:
  covers   the share of their dialogue lines inside it (95 % of a line's box), the mean over pages of each title,
           then over titles; covers_own: its own page's dialogue all inside
  junk     their recognized lines (the job on the whole page) at least half inside it that are no dialogue line (a
           name, a button, a picture read as text), per page; visible: those with Japanese in their text, which the
           Shell's line filter keeps (non_japanese_filter = lines)
  area     its share of the screen
against the whole screen (no box) and the page's own dialogue lines plus 12 px (the pool's "base" region: a box drawn
tight around one screen's text). Titles are split into two halves by a hash of their ID (`half`): the rule was chosen
on half 0, and half 1 is its held-out report.

Pages, crops and texts stay local: OUT.json holds counts and rates only. Host execution: no console has run this.
"""
import argparse
import hashlib
import json
import re
import statistics
import subprocess
import sys
import threading
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

W, H = 960, 544
# The pool's titles that share a release with a protected set (vocr6-ingest NOTES: PCSG00007 and ULJM05383 with
# CLANNAD, PCSG00866 with vita-test) and its one test title (PCSG00807): never read here.
EXCLUDE = ("PCSG00007", "ULJM05383", "PCSG00866", "PCSG00807")
COVER = 0.95  # a line is inside a box with this share of its own box
JUNK = 0.5    # a recognized line counts for a box with at least half of it inside
# Japanese as the Shell's line filter sees it (core/utf.h vjo_is_japanese_char)
JA = re.compile(r"[\u3040-\u30ff\u4e00-\u9faf]")


def half(title):
    """The title's half: 0 the rule was chosen on, 1 held out."""
    return int(hashlib.sha256(("firstbox:" + title).encode()).hexdigest(), 16) % 2


def inter(a, b):
    w = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])
    h = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
    return max(0, w) * max(0, h)


def area(b):
    return b[2] * b[3]


def box_pixels(rows):
    """VjoGameOcrResult.suggest_rows (the band's rows at the bottom of the 960 x 544 screen, its whole width) as x, y,
    w, h; None for none (0)."""
    if not rows:
        return None
    return [0, H - rows, W, rows]


def pages_of(pagepool, titles, exclude, per_title):
    """Up to per_title pages of each title, spread over its pages: complete, trusted, with horizontal dialogue lines
    and none vertical."""
    out = []
    for f in sorted(Path(pagepool).glob("*.jsonl")):
        tid = f.stem
        if (titles and tid not in titles) or tid in exclude:
            continue
        good = []
        for line in f.open(encoding="utf-8"):
            p = json.loads(line)
            if not p.get("complete") or p.get("untrusted") or not p.get("lines"):
                continue
            if any(ln.get("vertical") for ln in p["lines"]) or not any(ln.get("kind") == "line" for ln in p["lines"]):
                continue
            good.append(p)
        if good:
            step = max(1, len(good) // per_title)
            out.extend(good[::step][:per_title])
    return out


def truth_of(page):
    """The page's dialogue lines and other text boxes (names) in the console view's pixels."""
    s = 2 if page.get("platform") == "psp" else 1
    lines = [[v * s for v in ln["box"]] for ln in page["lines"] if ln.get("kind") == "line"]
    others = [[v * s for v in ln["box"]] for ln in page["lines"] if ln.get("kind") != "line"]
    base = [r["region"] for r in page.get("regions") or [] if r.get("variant") == "base"]
    if base:
        x0, y0, x1, y1 = (v * s for v in base[0])
        base = [x0, y0, x1 - x0, y1 - y0]
    return {"lines": lines, "others": others, "base": base or None}


def job_of(page, pages_dir):
    from PIL import Image  # only to read the pages
    im = Image.open(Path(pages_dir) / page["image"]).convert("RGBA")
    psp = page.get("platform") == "psp"
    if psp:
        im = im.resize((im.width * 2, im.height * 2), Image.BILINEAR)
    pid = f"{page['title_id']}:{page['page']}"
    return pid, f"{pid} {im.width} {im.height} {1 if psp else 0}\n".encode(), im.tobytes()


def read_title(binary, models, pages, pages_dir, tool_args):
    """One vjo-page-ocr run over a title's pages: each page's suggested box (pixels) and recognized lines."""
    jobs = [job_of(p, pages_dir) for p in pages]
    proc = subprocess.Popen([binary, models] + tool_args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def feed():  # a writer thread: the tool's results come while the pages still go in
        for _, head, body in jobs:
            proc.stdin.write(head)
            proc.stdin.write(body)
        proc.stdin.close()

    t = threading.Thread(target=feed)
    t.start()
    results = [json.loads(line) for line in proc.stdout]
    t.join()
    if proc.wait() or len(results) != len(jobs):
        raise RuntimeError(f"{binary}: {len(results)} results for {len(jobs)} pages")
    out = []
    for (pid, _, _), r in zip(jobs, results):
        if r["id"] != pid:
            raise RuntimeError(f"{binary}: result {r['id']} for page {pid}")
        out.append({"rc": r["rc"], "suggest": box_pixels(r.get("suggest_rows")) if not r["rc"] else None,
                    "lines": [{"box": ln["box"], "ja": bool(JA.search(ln["text"]))}
                              for ln in r.get("lines", []) if not ln.get("blank")]})
    return out


def is_text_of(box, boxes, share=0.3):
    """A recognized line over one of these boxes (30 % of the smaller one)."""
    return any(inter(box, b) >= share * min(area(box), area(b)) for b in boxes if area(b))


def score_title(pages):
    """pages: [{"truth", "job", "box": {name: box or None}}] of one title; per box kind its sums over the pages it
    offers a box on, each scored on the title's other pages."""
    kinds = sorted(pages[0]["box"])
    out = {}
    for k in kinds:
        s = {"pages": len(pages), "offered": 0, "own": 0, "pairs": 0, "covers": 0.0, "junk": 0, "visible": 0,
             "area": 0.0}
        for i, p in enumerate(pages):
            b = p["box"][k]
            if not b:
                continue
            s["offered"] += 1
            s["area"] += area(b) / (W * H)
            s["own"] += all(inter(g, b) >= COVER * area(g) for g in p["truth"]["lines"])
            for j, q in enumerate(pages):
                if j == i:
                    continue
                d = q["truth"]["lines"]
                s["pairs"] += 1
                s["covers"] += sum(inter(g, b) >= COVER * area(g) for g in d) / len(d)
                for ln in q["job"]["lines"]:
                    if inter(ln["box"], b) >= JUNK * area(ln["box"]) and not is_text_of(ln["box"], d):
                        s["junk"] += 1
                        s["visible"] += ln["ja"]
        out[k] = s
    return out


def summary(per_title, titles):
    """The mean over titles (each title's means over its pages) of each kind's rates."""
    out = {}
    if not any(t in per_title for t in titles):
        return {"titles": 0}
    for k in sorted(next(iter(per_title.values())).keys()):
        rows = [per_title[t][k] for t in titles if t in per_title]
        cov = [r["covers"] / r["pairs"] for r in rows if r["pairs"]]
        out[k] = {
            "titles": len(rows),
            "pages": sum(r["pages"] for r in rows),
            "offered": round(statistics.mean(r["offered"] / r["pages"] for r in rows), 4),
            "covers": round(statistics.mean(cov), 4) if cov else None,
            "covers_worst_title": round(min(cov), 4) if cov else None,
            "covers_own": round(sum(r["own"] for r in rows) / max(1, sum(r["offered"] for r in rows)), 4),
            "junk": round(statistics.mean(r["junk"] / r["pairs"] for r in rows if r["pairs"]), 3),
            "visible_junk": round(statistics.mean(r["visible"] / r["pairs"] for r in rows if r["pairs"]), 3),
            "area": round(statistics.mean(r["area"] / r["offered"] for r in rows if r["offered"]), 4),
        }
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True, help="build/<host>/vjo-page-ocr")
    ap.add_argument("--models", required=True, help="the pinned release models (tools/prepare_ocr.py)")
    ap.add_argument("--pagepool", required=True, help="vocr6-ingest pagepool/v1")
    ap.add_argument("--pages", help="the pages' directory (default: PAGEPOOL/../../pages)")
    ap.add_argument("--titles", nargs="*", default=[])
    ap.add_argument("--exclude", nargs="*", default=list(EXCLUDE))
    ap.add_argument("--per-title", type=int, default=60)
    ap.add_argument("--jobs", type=int, default=5)
    ap.add_argument("--tool-args", default="--tier 2 --lm 2 --cascade on --line-fix on --vertical auto",
                    help="vjo-page-ocr's options: config.ini's defaults")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args(argv)
    pages_dir = a.pages or str(Path(a.pagepool).resolve().parent.parent / "pages")
    pages = pages_of(a.pagepool, set(a.titles), set(a.exclude), a.per_title)
    by_title = defaultdict(list)
    for p in pages:
        by_title[p["title_id"]].append(p)

    def run(tid):
        jobs = read_title(a.binary, a.models, by_title[tid], pages_dir, a.tool_args.split())
        rows = []
        for p, j in zip(by_title[tid], jobs):
            if j["rc"]:
                continue  # a failed job reads nothing: no page to score
            t = truth_of(p)
            rows.append({"truth": t, "job": j, "box": {"suggested": j["suggest"], "whole_screen": [0, 0, W, H],
                                                        "tight_base": t["base"]}})
        return tid, rows

    per_title, failed = {}, 0
    with ThreadPoolExecutor(a.jobs) as ex:
        for tid, rows in ex.map(run, sorted(by_title)):
            failed += len(by_title[tid]) - len(rows)
            if len(rows) >= 2:
                per_title[tid] = score_title(rows)
    titles = sorted(per_title)
    report = {
        "what": "first-use box (#358): core/region_suggest.c through host/page_ocr.c on hooked pages",
        "tool_args": a.tool_args, "per_title": a.per_title, "excluded": sorted(a.exclude),
        "pages_failed": failed,
        "all": summary(per_title, titles),
        "half0_chosen_on": summary(per_title, [t for t in titles if half(t) == 0]),
        "half1_held_out": summary(per_title, [t for t in titles if half(t) == 1]),
        "titles": {t: {"half": half(t), "covers": round(per_title[t]["suggested"]["covers"] /
                                                         max(1, per_title[t]["suggested"]["pairs"]), 4),
                       "offered": per_title[t]["suggested"]["offered"], "pages": per_title[t]["suggested"]["pages"]}
                   for t in titles},
    }
    Path(a.out).write_text(json.dumps(report, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    json.dump({k: report[k] for k in ("all", "half1_held_out")}, sys.stdout, indent=1, sort_keys=True)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
