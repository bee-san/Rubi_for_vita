#!/usr/bin/env python3
"""The text-presence gate (ocr_text_gate) as Rubi's game worker runs it, on hooked pages and text-free regions.

    python3 tools/gate_eval.py --binary build/bench/vjo-gate-eval --models DIR \\
        [--pagepool DIR] [--empties MANIFEST ...] [--shots DIR] [--exclude TITLE ...] [--jobs 5] -o OUT.json

--pagepool: vita-vn-ocr's hooked page pool (pagepool/v1: one JSONL per title, each page's hook text, its verified line
boxes and its dialogue `regions`), pages beside it in ../pages. Every page is read in two regions by host/gate_eval.c's
job (Rubi's finder, then the gate at its pinned point): its own base dialogue region, and the one region a player would
draw for the whole game (user_region: around the dialogue of almost every page of the title); a PSP page
is first scaled 2x, as Adrenaline shows it full-screen (bilinear: its filter is not known here), and refined, as the
worker refines a PSP game's boxes. A region whose finder found lines and whose gate stopped them all loses its lines:
the missed-line rate. Each stopped region is read again without the gate, with the recognizer, so the report says what
it held.

--empties: eval/linefind manifests (vita-vn-ocr): their regions without a labelled line are its text-free development
and holdout regions; --shots holds their screenshots (vita/, psp/).

Pages, crops and texts stay local: OUT.json has counts, rates and each stopped region's title, page and lengths only.
Host execution: no console time (docs/offline-testing.md).
"""
import argparse
import json
import unicodedata
import multiprocessing
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

GATE_NAME = "textgate_p8_c8_w8.vtd"


def crop(path, xyxy, psp, cache):
    if cache.get("path") != path:
        im = Image.open(path)
        im.load()
        if psp:  # Adrenaline's full-screen picture: 480 x 272 at 2x
            im = im.resize((im.width * 2, im.height * 2), Image.BILINEAR)
        cache.clear()
        cache.update(path=path, im=im)
    im = cache["im"]
    s = 2 if psp else 1
    x0, y0, x1, y1 = (int(round(v * s)) for v in xyxy)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(im.width, x1), min(im.height, y1)
    if x1 - x0 < 8 or y1 - y0 < 8:
        return None
    c = im.crop((x0, y0, x1, y1))
    return c.width, c.height, c.convert("RGB").tobytes()  # made grey by the tool, as the kernel does


class Tool:
    """One vjo-gate-eval process, a region at a time (it answers each with one line)."""

    def __init__(self, binary, models, gate):
        self.p = subprocess.Popen([binary, models, "1" if gate else "0"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.p.stdin.write(b"VJGE")

    def run(self, w, h, psp, pixels):
        self.p.stdin.write(struct.pack("<III", w, h, 1 if psp else 0) + pixels)
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=60)


def lines_in(page, xyxy):
    """The page's verified lines whose box centre lies in the region: their count, characters and text."""
    n = chars = 0
    texts = []
    for ln in page.get("lines") or []:
        x, y, w, h = ln["box"]
        cx, cy = x + w / 2, y + h / 2
        if xyxy[0] <= cx <= xyxy[2] and xyxy[1] <= cy <= xyxy[3]:
            n += 1
            chars += len(ln.get("text") or "")
            texts.append(ln.get("text") or "")
    return n, chars, "".join(texts)


def fold(s):
    """A compared form close to core/hook_select.c's: NFKC (full-width ASCII, half-width kana), small letters,
    letters, digits, kana and kanji only."""
    s = unicodedata.normalize("NFKC", s).lower().replace("一", "ー")
    return "".join(c for c in s if unicodedata.category(c)[0] in "LN" or c in "ー々〆〇")


def similarity(ocr, hook):
    """1000 x the share of the hook text OCR read: (|h| - d) / max(|h|, |o|), d the edit distance of the hook's
    folded text to OCR's (0 when either is empty), as core/hook_select.c scores a stream against OCR."""
    o, h = fold(ocr), fold(hook)
    if not o or not h:
        return 0
    prev = list(range(len(o) + 1))
    for i, hc in enumerate(h, 1):
        cur = [i] + [0] * len(o)
        for j, oc in enumerate(o, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (hc != oc))
        prev = cur
    d = prev[-1]
    return max(0, (len(h) - d)) * 1000 // max(len(h), len(o))


def work(args):
    items, binary, gate_dir, models = args
    tool, full, cache, out = Tool(binary, gate_dir, True), None, {}, []
    try:
        for it in items:
            if it.get("missing"):
                out.append(dict(it, skipped=1))
                continue
            c = crop(it["image"], it["xyxy"], it["psp"], cache)
            if c is None:
                out.append(dict(it, skipped=1))
                continue
            w, h, px = c
            r = tool.run(w, h, it["psp"], px)
            rec = {k: it[k] for k in ("title", "page", "kind", "set", "platform", "lines", "chars")}
            rec.update(w=w, h=h, found=r.get("found", 0), gate_ran=r.get("gate_ran", 0), score=r.get("gate_score"),
                       stopped=r.get("gate_stopped", 0), gate_rc=r.get("gate_rc", 0), gate_us=r.get("gate_us", 0),
                       find_us=r.get("find_us", 0), rc=r.get("rc"))
            if rec["stopped"]:  # what it held: read again, the recognizer on and the gate off
                if full is None:
                    full = Tool(binary, models, False)
                t = full.run(w, h, it["psp"], px).get("text", "")
                rec["lost_chars"] = len(t.replace("\n", ""))
                rec["lost_lines"] = t.count("\n") + 1 if t else 0
                # did OCR read the region's hook text? (>= 700: the line a user would have had)
                rec["lost_sim"] = similarity(t, it.get("hook_text", ""))
            out.append(rec)
    finally:
        tool.close()
        if full is not None:
            full.close()
    return out


def user_region(pages):
    """The region a player would draw once for the game (□, two corners): around the dialogue of almost every page,
    the 2nd and 98th percentiles of the pages' base dialogue regions' edges, 6 px wider on each side."""
    base = [next((g["region"] for g in p.get("regions") or [] if g["variant"] == "base"), None) for p in pages]
    base = [b for b in base if b]
    if not base:
        return None
    w, h = pages[0]["dims"]

    def pct(vals, q):
        vals = sorted(vals)
        return vals[min(len(vals) - 1, max(0, int(round(q * (len(vals) - 1)))))]
    x0 = max(0, pct([b[0] for b in base], 0.02) - 6)
    y0 = max(0, pct([b[1] for b in base], 0.02) - 6)
    x1 = min(w, pct([b[2] for b in base], 0.98) + 6)
    y1 = min(h, pct([b[3] for b in base], 0.98) + 6)
    return [x0, y0, x1, y1]


def page_items(pagepool, pages_dir, exclude, kinds):
    items = []
    for f in sorted(Path(pagepool).glob("*.jsonl")):
        title = f.stem
        if title in exclude:
            continue
        pages = [json.loads(line) for line in open(f, encoding="utf-8")]
        fixed = user_region(pages)
        for p in pages:
            psp = p["platform"] == "psp"
            for kind in kinds:
                if kind == "user":
                    xyxy = fixed
                elif kind == "full":  # the whole screen: Rubi's region until the player draws one
                    xyxy = [0, 0, p["dims"][0], p["dims"][1]]
                elif kind == "roi":
                    xyxy = p.get("roi")
                else:
                    xyxy = next((r["region"] for r in p.get("regions") or [] if r["variant"] == kind), None)
                if not xyxy:
                    continue
                n, chars, hook_text = lines_in(p, xyxy)
                items.append({"title": title, "page": p["page"], "kind": kind, "set": "hooked", "platform": p["platform"],
                              "image": str(Path(pages_dir) / p["image"]), "xyxy": xyxy, "psp": psp, "lines": n,
                              "chars": chars, "hook_text": hook_text})
    return items


def empty_items(manifests, shots):
    items = []
    for m in manifests:
        name = Path(m).stem
        for line in open(m, encoding="utf-8"):
            r = json.loads(line)
            if r.get("lines"):
                continue
            psp = r["platform"] == "psp"
            path = Path(shots) / ("psp" if psp else "vita") / r["shot"]
            if not path.is_file():  # a screenshot this machine does not hold: counted as missing in OUT.json
                items.append({"set": name, "missing": 1})
                continue
            items.append({"title": r["vn"], "page": r["id"], "kind": "region", "set": name, "platform": r["platform"],
                          "image": str(path), "xyxy": r["region"], "psp": False, "lines": 0, "chars": 0})
    return items


def summary(recs):
    s = {}
    for r in recs:
        if r.get("skipped"):
            continue
        key = f"{r['set']}:{r['platform']}:{r['kind']}"
        a = s.setdefault(key, dict(regions=0, with_lines=0, lines=0, chars=0, found=0, stopped=0, lines_lost=0,
                                   chars_lost=0, undecided=0, gate_ms=0.0, find_ms=0.0, no_line_after=0,
                                   readable_regions_lost=0, readable_lines_lost=0))
        a["regions"] += 1
        a["with_lines"] += r["lines"] > 0
        a["lines"] += r["lines"]
        a["chars"] += r["chars"]
        a["found"] += r["found"] > 0
        a["stopped"] += r["stopped"] > 0
        a["undecided"] += bool(r["gate_rc"])
        a["no_line_after"] += r["found"] == 0 or r["stopped"] > 0
        if r["stopped"] and r["lines"]:
            a["lines_lost"] += r["lines"]
            a["chars_lost"] += r["chars"]
            if r.get("lost_sim", 0) >= 700:  # lines OCR without the gate would have read
                a["readable_regions_lost"] += 1
                a["readable_lines_lost"] += r["lines"]
        a["gate_ms"] += r["gate_us"] / 1000
        a["find_ms"] += r["find_us"] / 1000
    for a in s.values():
        a["gate_ms_mean"] = round(a.pop("gate_ms") / max(1, a["regions"]), 3)
        a["find_ms_mean"] = round(a.pop("find_ms") / max(1, a["regions"]), 3)
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--models", required=True, help="the pinned models (VOCR5 S/N/L, the LMs, the gate's weights)")
    ap.add_argument("--pagepool")
    ap.add_argument("--pages", help="the pages beside the pool (default: ../pages)")
    ap.add_argument("--kinds", default="base,user", help="base: each page's dialogue region; user: the title's one region (user_region); "
                    "full: the whole screen (Rubi's default region); roi")
    ap.add_argument("--empties", nargs="*", default=[])
    ap.add_argument("--shots")
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--limit", type=int, default=0, help="at most this many pages a title (0: all)")
    ap.add_argument("--jobs", type=int, default=5)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--records", help="also write every region's record (JSONL, local only)")
    args = ap.parse_args()
    items = []
    if args.pagepool:
        pages = args.pages or str(Path(args.pagepool).parent.parent / "pages")
        items += page_items(args.pagepool, pages, set(args.exclude), args.kinds.split(","))
        if args.limit:
            seen, kept = {}, []
            for it in items:
                k = (it["title"], it["kind"])
                seen[k] = seen.get(k, 0) + 1
                if seen[k] <= args.limit:
                    kept.append(it)
            items = kept
    if args.empties:
        items += empty_items(args.empties, args.shots)
    with tempfile.TemporaryDirectory(prefix="vjo-gate-") as tmp:
        gate_dir = Path(tmp) / "gate-only"  # the gate's weights alone: each job stops after the gate
        gate_dir.mkdir()
        os.symlink(Path(args.models).resolve() / GATE_NAME, gate_dir / GATE_NAME)
        # chunks by page image, so a page's regions share one decode
        n = max(1, args.jobs)
        chunks = [items[i::n] for i in range(n)]
        for c in chunks:
            c.sort(key=lambda it: it.get("image", ""))
        with multiprocessing.Pool(n) as pool:
            recs = [r for part in pool.map(work, [(c, args.binary, str(gate_dir), args.models) for c in chunks])
                    for r in part]
    s = summary(recs)
    for a in s.values():
        a["missed_region_rate"] = round(a["stopped"] / a["with_lines"], 6) if a["with_lines"] else None
        a["missed_line_rate"] = round(a["lines_lost"] / a["lines"], 6) if a["lines"] else None
        a["missed_readable_line_rate"] = round(a["readable_lines_lost"] / a["lines"], 6) if a["lines"] else None
        a["text_free_left_with_lines"] = a["regions"] - a["no_line_after"]
    stopped = [{k: r[k] for k in ("set", "title", "page", "kind", "platform", "lines", "chars", "score", "found")} |
               {"lost_lines": r.get("lost_lines"), "lost_chars": r.get("lost_chars"), "lost_sim": r.get("lost_sim")}
               for r in recs if r.get("stopped") and r["set"] == "hooked"]
    missing = {}
    for r in recs:
        if r.get("missing"):
            missing[r["set"]] = missing.get(r["set"], 0) + 1
    out = {"tool": "tools/gate_eval.py", "binary": os.path.basename(args.binary), "kinds": args.kinds,
           "missing_screenshots": missing,
           "excluded_titles": sorted(args.exclude), "summary": s, "stopped_hooked_regions": stopped}
    Path(args.output).write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.records:
        with open(args.records, "w", encoding="utf-8") as f:
            for r in recs:
                f.write(json.dumps({k: v for k, v in r.items() if k != "image"}, ensure_ascii=False) + "\n")
    for k, a in sorted(s.items()):
        print(k, json.dumps(a))


if __name__ == "__main__":
    sys.exit(main())
