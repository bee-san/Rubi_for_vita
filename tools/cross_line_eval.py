#!/usr/bin/env python3
"""How often a dictionary word crosses a line break in real visual-novel text boxes.

For every pair of consecutive lines (A, B) of one text box, the real overlay lookup
(vjo-word-forms: vjo_overlay_lookup_at, as a tap on a word's first character) segments A
from its start, longest match first, over the two lines joined without the break: a token
that starts in A and ends in B is a word the break splits (the owner's "tabe / ru"). With
--tool (vjo-cross-lines) it also asks the overlay's join rule (core/textjoin.c) for each
break, and segments the text as the Shell keeps it, with its breaks, so the crossing tokens
are the ones the shipped lookup finds.

  cross_line_eval.py --forms build/host/vjo-word-forms --dict DIR [--tool build/host/vjo-cross-lines]
                     [--vita-test F] [--psp-test F] [--clannad F] [--realplus F] --out RESULT.json
                     [--examples LOCAL.tsv]

Text boxes:
  vita-test / psp-test  vita-vn-ocr eval/manifests/*.jsonl: lines grouped by screenshot and region,
                        top to bottom (box y); an uncertain line ends the chain.
  clannad               the CLANNAD long run's frozen references (refs.jsonl): a page's lines in
                        order; no-text frames are skipped, any other excluded line ends the chain.
  realplus              realplus-v3 lines_full: a shot's kept lines in the finder's order; the next
                        line is in the same box when its ink box is just below (gap < 0.8 line
                        heights) and overlaps it horizontally.

The output holds counts only. --examples writes the crossing pairs' text to a local file for
review: never commit it (the CLANNAD and realplus text is private; vita-test/psp-test text is
vita-vn-ocr's public manifests).
"""
import argparse
import collections
import json
import subprocess
import sys
from pathlib import Path


def chars_for_units(s, units):
    """The number of code points of s that make `units` UTF-16 units."""
    n = u = 0
    while n < len(s) and u < units:
        u += 2 if ord(s[n]) > 0xFFFF else 1
        n += 1
    return n


def manifest_boxes(path, kind):
    """vita-test / psp-test: [(box_id, [(text, is_name, ok)])] by screenshot and region, top to bottom."""
    groups = collections.defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            groups[(r["sha256"], tuple(r["region"]))].append(r)
    out = []
    for (sha, region), rows in groups.items():
        rows.sort(key=lambda r: (r["box"][1], r["box"][0]))
        out.append((f"{kind}:{sha[:12]}:{','.join(map(str, region))}",
                    [(r["text"] or "", "name" in r.get("styles", []), not r.get("uncertain")) for r in rows]))
    return out


def clannad_boxes(path):
    pages = collections.defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            pages[r["page"]].append(r)
    out = []
    for page, rows in pages.items():
        rows.sort(key=lambda r: int(r["id"].rsplit("-", 1)[1]))
        lines = []
        for r in rows:
            if r.get("notext"):
                continue  # scene art, a box edge, a cursor: not a text line
            lines.append((r["text"] or "", bool(r.get("name")), r["status"] == "ref"))
        out.append((f"clannad:{page}", lines))
    return out


def realplus_boxes(path):
    shots = collections.defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            box = r.get("crops", {}).get("vita_ink")
            if not box or not r.get("label"):
                continue
            shots[r["shot"]].append((int(r["id"].rsplit("_", 1)[1]), box, r["label"], bool(r.get("name"))))
    out = []
    for shot, rows in shots.items():
        rows.sort()
        cur, prev = [], None
        for idx, box, text, name in rows:
            if prev is not None:
                pidx, pbox = prev
                h = max(pbox[3] - pbox[1], box[3] - box[1], 1)
                gap = box[1] - pbox[3]
                overlap = min(box[2], pbox[2]) - max(box[0], pbox[0])
                if idx != pidx + 1 or gap > 0.8 * h or gap < -0.5 * h or overlap <= 0:
                    out.append((f"realplus:{shot}:{len(out)}", cur))
                    cur = []
            cur.append((text, name, True))
            prev = (idx, box)
        if cur:
            out.append((f"realplus:{shot}:{len(out)}", cur))
    return out


def run_forms(forms, directory, files, texts):
    """vjo-word-forms on each text (one lookup at its first character): matched UTF-16 units or 0."""
    if not texts:
        return []
    p = subprocess.run([forms, directory, files], input="".join(t + "\n" for t in texts),
                       capture_output=True, text=True, encoding="utf-8", check=True)
    rows = p.stdout.split("\n")[:len(texts)]
    if len(rows) != len(texts):
        raise SystemExit(f"vjo-word-forms printed {len(rows)} lines for {len(texts)} texts")
    return [0 if r == "-" else int(r.rsplit("\t", 1)[1]) for r in rows]


def crossing_tokens(forms, directory, files, pairs):
    """For each (A, B): the token of A's greedy segmentation over A+B that ends in B, else None.
    Returned as (start, end) in code points of A+B."""
    pos = [0] * len(pairs)
    result = [None] * len(pairs)
    active = [i for i, (a, b) in enumerate(pairs) if a and b]
    while active:
        texts = [(pairs[i][0] + pairs[i][1])[pos[i]:] for i in active]
        units = run_forms(forms, directory, files, texts)
        nxt = []
        for i, u, t in zip(active, units, texts):
            a = pairs[i][0]
            n = chars_for_units(t, u) if u else 1
            if pos[i] < len(a) < pos[i] + n:
                result[i] = (pos[i], pos[i] + n)
                continue
            pos[i] += n
            if pos[i] < len(a):
                nxt.append(i)
        active = nxt
    return result


def tool_rows(tool, directory, files, texts):
    """vjo-cross-lines: per text, one row per break: join(0/1) and the crossing token (start end) or -."""
    p = subprocess.run([tool, directory, files], input="".join(t.replace("\\", "\\\\").replace("\n", "\\n") + "\n"
                                                               for t in texts),
                       capture_output=True, text=True, encoding="utf-8", check=True)
    out = []
    for line in p.stdout.split("\n")[:len(texts)]:
        breaks = []
        for item in line.split(" ") if line else []:
            j, rest = item.split(":", 1)
            span = None if rest == "-" else rest.split(":", 1)[0]
            breaks.append((j == "1", None if span is None else tuple(int(x) for x in span.split("-"))))
        out.append(breaks)
    if len(out) != len(texts):
        raise SystemExit(f"vjo-cross-lines printed {len(out)} rows for {len(texts)} texts")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--forms", required=True)
    ap.add_argument("--tool")
    ap.add_argument("--dict", required=True, help="the directory holding plaintext.vjdict")
    ap.add_argument("--files", default="plaintext.vjdict")
    ap.add_argument("--vita-test")
    ap.add_argument("--psp-test")
    ap.add_argument("--clannad")
    ap.add_argument("--realplus")
    ap.add_argument("--out", required=True)
    ap.add_argument("--examples")
    args = ap.parse_args()

    sets = {}
    if args.vita_test:
        sets["vita-test"] = manifest_boxes(args.vita_test, "vita")
    if args.psp_test:
        sets["psp-test"] = manifest_boxes(args.psp_test, "psp")
    if args.clannad:
        sets["clannad"] = clannad_boxes(args.clannad)
    if args.realplus:
        sets["realplus-v3"] = realplus_boxes(args.realplus)

    report = {"what": "consecutive line pairs of one text box whose break a dictionary word crosses",
              "dictionary": args.files, "sets": {}}
    examples = []
    for name, boxes in sets.items():
        pairs, meta = [], []
        lines_total = 0
        for box_id, lines in boxes:
            lines_total += sum(1 for t, _, ok in lines if ok and t)
            for k in range(len(lines) - 1):
                (a, an, aok), (b, bn, bok) = lines[k], lines[k + 1]
                if aok and bok and a and b:
                    pairs.append((a, b))
                    meta.append((box_id, k, an, bn, lines))
        cross = crossing_tokens(args.forms, args.dict, args.files, pairs)
        s = collections.Counter(lines=lines_total, pairs=len(pairs), boxes=len(boxes))
        s["pairs_with_a_name_line"] = sum(1 for m in meta if m[2] or m[3])
        s["crossing"] = sum(1 for c in cross if c)
        s["crossing_name_pairs"] = sum(1 for c, m in zip(cross, meta) if c and (m[2] or m[3]))
        if args.tool:
            # the Shell's text: the box's lines as recognized, '\n' between them; one row per break
            texts, where = [], []
            for box_id, lines in boxes:
                chain = [t for t, _, ok in lines]
                if all(ok for _, _, ok in lines) and all(chain):
                    texts.append("\n".join(chain))
                    where.append(box_id)
            rows = dict(zip(where, tool_rows(args.tool, args.dict, args.files, texts)))
            tab = collections.Counter()
            for c, m in zip(cross, meta):
                box_id, k = m[0], m[1]
                if box_id not in rows:
                    continue
                joined, span = rows[box_id][k]
                tab["pairs"] += 1
                tab["joined"] += joined
                tab["crossing_raw"] += bool(c)
                tab["crossing_raw_joined"] += bool(c) and joined
                tab["joined_no_crossing"] += joined and not c
                tab["crossing_shipped"] += bool(span)
                tab["name_pairs"] += m[2] or m[3]
                tab["name_pairs_joined"] += (m[2] or m[3]) and joined
                tab["name_pairs_crossing_shipped"] += (m[2] or m[3]) and bool(span)
            s["tool"] = dict(tab)
            if tab["crossing_raw"]:
                s["tool"]["recall_of_raw_crossings"] = round(tab["crossing_raw_joined"] / tab["crossing_raw"], 4)
            if tab["joined"]:
                s["tool"]["joined_share"] = round(tab["joined"] / tab["pairs"], 4)
        for c, (a, b), m in zip(cross, pairs, meta):
            if c:
                ab = a + b
                examples.append((name, m[0], a, b, ab[c[0]:len(a)] + "|" + ab[len(a):c[1]]))
        s["share_of_pairs"] = round(s["crossing"] / s["pairs"], 4) if s["pairs"] else None
        s["share_of_lines"] = round(s["crossing"] / s["lines"], 4) if s["lines"] else None
        report["sets"][name] = dict(s)
        print(name, json.dumps(report["sets"][name], ensure_ascii=False), file=sys.stderr)
    tot = collections.Counter()
    for v in report["sets"].values():
        for k in ("lines", "pairs", "crossing", "pairs_with_a_name_line", "crossing_name_pairs"):
            tot[k] += v[k]
    tot = dict(tot)
    tot["share_of_pairs"] = round(tot["crossing"] / tot["pairs"], 4) if tot.get("pairs") else None
    tot["share_of_lines"] = round(tot["crossing"] / tot["lines"], 4) if tot.get("lines") else None
    report["all"] = tot
    Path(args.out).write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    if args.examples:
        with open(args.examples, "w", encoding="utf-8") as f:
            f.writelines("\t".join(row) + "\n" for row in examples)
    print(json.dumps(report["all"]), file=sys.stderr)


if __name__ == "__main__":
    main()
