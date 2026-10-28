#!/usr/bin/env python3
"""Vertical text through Rubi's own job (docs/vita-vn-ocr.md "Vertical text", API 47): column recall and dialogue CER
on hooked pages with labelled vertical columns, and what auto (config.ini ocr_vertical = auto) decides on pages of
horizontal games.

  vertical_eval.py units --pagepool DIR [--titles T,...] [--splits S,...] [--regions tight,card,full] [--max N]
                         [--skip-titles T,...] --out UNITS.jsonl
      the region units of the page pool's pages (vita-vn-ocr's vocr6-ingest pagepool/<version>/<TITLE>.jsonl):
      a page with vertical dialogue columns gives "tight" (the union of its labelled dialogue columns plus 12 px,
      vita-vn-ocr #103's region), "card" (the same with every other text box of the page inside it, a speaker's
      name: what a player draws around the text box), "fixed" (one box for every page whose text box is in the same
      place, as a player draws it once: the union of those pages' dialogue columns plus 12 px, for each place of at
      least 50 pages, the places told apart by their first column's right edge in 20-px steps) and "full" (the
      whole screen: no region drawn); a page of horizontal dialogue gives its pool regions ("base": the dialogue
      lines plus 12 px) and "full". A PSP page is cut from the frame as Rubi sees it (the console view: shown 2x by
      Adrenaline, here a bilinear upscale), with its boxes in those pixels.
  vertical_eval.py ext-units [--vdev REGIONS.jsonl] [--shots SHOTS.json --reserved RESERVED.json [--test-games J]]
                             --out UNITS.jsonl
      units of pages no rule here was chosen on: vita-vn-ocr #103's vertical development regions (eval/vertical.py's
      regions: "tight", and "full", the whole page), and VNDB's PS Vita and PSP screenshots as whole screens
      ("full"; vita-vn-ocr's census, eval/vertical_count.py shots_v2.json: no page with a vertical line, no test
      VN or test session of eval/manifests/reserved.json and test_games.json). Each page as the console shows it:
      a PC page scaled to fit 960 x 544, a PSP page 2x (Adrenaline).
  vertical_eval.py run --units UNITS.jsonl --pages DIR --tool vjo-page-ocr --models DIR [--workers N]
                       [--tool-args "--vertical on"] --out RESULTS.jsonl
      every unit's region (the page's PNG, cut) through build/<host>/vjo-page-ocr: the game worker's job on it
  vertical_eval.py abtime --units UNITS.jsonl --pages DIR --models DIR --arm NAME=TOOL[:ARGS] ... [--reps 3]
                          [--max N] --out TIMES.json
      the same regions through two or more builds of the job, one region at a time and each arm in turn (the order
      rotated), so that the machine's load falls on all of them alike: each arm's job time (min of the reps) and
      its ratio to the first arm's, and whether each arm's text, lines, strips and workspace equal the first's
  vertical_eval.py score --units UNITS.jsonl --results RESULTS.jsonl [--out SCORE.json]
      column recall and precision (IoU >= 0.5, one to one; a found column mostly inside another text box of the
      page, a name, is neither; a job that read rows finds no column), dialogue CER (the job's lines over another
      text box left out, each line's text normalised with vita-vn-ocr's eval/metrics.norm, joined in the job's
      order: columns right to left, rows top to bottom; against the dialogue columns right to left), the text as
      shown (names included), and, for horizontal units, how often auto read columns;
      95% intervals from 10,000 page bootstrap resamples (seed 0)

Everything read here is private (copyrighted frames and their text): units, results and scores stay outside git;
SCORE.json holds counts and rates only.
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys
import threading
import unicodedata
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evallib import lev  # noqa: E402

LINE_COLUMN = 0x0800  # core/vocr_ocr.h VJO_VOCR_LINE_COLUMN
PAD = 12              # vita-vn-ocr eval/vertical.py: a region is its columns' union plus 12 px (Vita)
IOU = 0.5

# ---- vita-vn-ocr eval/metrics.py norm (bee-san/vita-vn-ocr 817d203), so the CER is that repo's
_DASHES = "―—─━–‐‑‒−-"
_TILDES = "〜～~∼"
_TRANS = str.maketrans({**{c: "―" for c in _DASHES}, **{c: "~" for c in _TILDES}})
_SPACE = re.compile(r"\s+")
_DOTS = re.compile(r"・{2,}")


def norm(s):
    s = unicodedata.normalize("NFKC", s or "")
    s = _SPACE.sub("", s)
    s = s.replace("...", "…").replace("..", "‥")
    s = _DOTS.sub(lambda m: "…" * max(1, len(m.group()) // 3), s)
    return s.translate(_TRANS)


def iou(a, b):
    ix = max(0, min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1]))
    i = ix * iy
    u = a[2] * a[3] + b[2] * b[3] - i
    return i / u if u else 0.0


def mostly_inside(d, b):
    ix = max(0, min(d[0] + d[2], b[0] + b[2]) - max(d[0], b[0]))
    iy = max(0, min(d[1] + d[3], b[1] + b[3]) - max(d[1], b[1]))
    return ix * iy >= 0.5 * d[2] * d[3]


def match(dets, refs, ignore, thr=IOU):
    """One to one at IoU >= thr, greedy by IoU (vita-vn-ocr eval/vertical.py match): (pairs, free dets, free refs);
    a free detection mostly inside an ignore box is not counted."""
    cand = sorted(((iou(d, r), i, j) for i, d in enumerate(dets) for j, r in enumerate(refs)), reverse=True)
    used_d, used_r, pairs = set(), set(), []
    for v, i, j in cand:
        if v < thr or i in used_d or j in used_r:
            continue
        used_d.add(i)
        used_r.add(j)
        pairs.append((i, j))
    free_d = [i for i in range(len(dets)) if i not in used_d and not any(mostly_inside(dets[i], b) for b in ignore)]
    free_r = [j for j in range(len(refs)) if j not in used_r]
    return pairs, free_d, free_r


def clip_box(x0, y0, x1, y1, w, h):
    x0, y0 = max(0, int(x0)), max(0, int(y0))
    x1, y1 = min(w, int(round(x1))), min(h, int(round(y1)))
    return [x0, y0, max(1, x1 - x0), max(1, y1 - y0)]


def union(boxes, pad, w, h):
    return clip_box(min(b[0] for b in boxes) - pad, min(b[1] for b in boxes) - pad,
                    max(b[0] + b[2] for b in boxes) + pad, max(b[1] + b[3] for b in boxes) + pad, w, h)


def scaled(rec, k):
    """A page record in the console view's pixels (k = 2 for a PSP frame shown 2x)."""
    if k == 1:
        return rec
    s = lambda b: [v * k for v in b]  # noqa: E731
    rec = dict(rec, dims=s(rec["dims"]))
    rec["lines"] = [dict(ln, box=s(ln["box"])) for ln in rec["lines"]]
    rec["boxes"] = [dict(b, xyxy=s(b["xyxy"])) for b in rec["boxes"]]
    rec["regions"] = [dict(r, region=s(r["region"])) for r in rec.get("regions") or []]
    return rec


def page_units(rec, regions):
    """The units of one page record (pagepool schema vocr6-hooked/1), in the console view."""
    k = 2 if rec["platform"] == "psp" else 1
    rec = scaled(rec, k)
    w, h = rec["dims"]
    lines = [ln for ln in rec["lines"] if not ln.get("drop")]
    vert = [ln for ln in lines if ln.get("vertical")]
    base = {"title": rec["title_id"], "page": rec["page"], "split": rec["split"], "image": rec["image"],
            "platform": rec["platform"], "scale": k, "dims": [w, h], "complete": rec["complete"]}
    covered = {i for ln in lines for i in ln.get("box_index") or []}
    others = [clip_box(b["xyxy"][0], b["xyxy"][1], b["xyxy"][2], b["xyxy"][3], w, h)
              for b in rec["boxes"] if b["index"] not in covered]
    out = []
    if vert:
        if len(vert) != len(lines):
            return []  # a page of both: not a vertical unit, not a horizontal one
        refs = sorted(({"box": ln["box"], "text": ln["text"]} for ln in vert),
                      key=lambda r: (-r["box"][0], r["box"][1]))
        for kind in regions:
            if kind == "fixed":  # cmd_units sets it, from every page of the title in the same place
                continue
            if kind == "tight":
                region = union([r["box"] for r in refs], PAD, w, h)
            elif kind == "card":
                region = union([r["box"] for r in refs] + others, PAD, w, h)
            elif kind == "full":
                region = [0, 0, w, h]
            else:
                continue
            out.append({**base, "id": f"{rec['title_id']}:{rec['page']}:{kind}", "kind": kind, "vertical": True,
                        "region": region, "refs": refs, "ignore": others})
    else:
        if not lines:
            return []
        refs = [{"box": ln["box"], "text": ln["text"]} for ln in lines]
        for kind in regions:
            if kind == "full":
                out.append({**base, "id": f"{rec['title_id']}:{rec['page']}:full", "kind": "full", "vertical": False,
                            "region": [0, 0, w, h], "refs": refs, "ignore": others})
            elif kind == "base":
                for k, reg in enumerate(r for r in rec.get("regions") or [] if r["variant"] == "base"):
                    x0, y0, x1, y1 = reg["region"]
                    ids = set(reg["lines"])
                    out.append({**base, "id": f"{rec['title_id']}:{rec['page']}:base{k}", "kind": "base",
                                "vertical": False, "region": clip_box(x0, y0, x1, y1, w, h),
                                "refs": [{"box": ln["box"], "text": ln["text"]} for ln in lines if ln["id"] in ids],
                                "ignore": others})
    return out


def fixed_units(units, step=20, least=50):
    """"fixed" units: a page's text box at a place shared by at least `least` pages of its title gets that place's
    box, the union of their dialogue columns plus PAD (what a player draws once around the columns)."""
    places = defaultdict(list)
    for u in units:
        if u["vertical"] and u["kind"] == "tight":
            places[max(r["box"][0] + r["box"][2] for r in u["refs"]) // step].append(u)
    out = []
    for g in places.values():
        if len(g) < least:
            continue
        w, h = g[0]["dims"]
        box = union([r["box"] for u in g for r in u["refs"]], PAD, w, h)
        for u in g:
            out.append(dict(u, id=u["id"].rsplit(":", 1)[0] + ":fixed", kind="fixed", region=box))
    return out


CONSOLE = (960, 544)


def fit_scale(w, h, screen):
    s = min(screen[0] / w, screen[1] / h)
    return (max(1, int(round(w * s))), max(1, int(round(h * s))))


def cmd_ext_units(a):
    units = []
    if a.vdev:
        for r in read_jsonl(a.vdev):
            k = 2 if r["kind"] == "psp" else 1  # the regions are in the page's own pixels (eval/vertical_data.py)
            w, h = r["dims"]
            s = lambda b: [v * k for v in b]  # noqa: E731
            base = {"title": r["vn"], "page": r["page"], "split": "vdev:" + r["cohort"] + ":" + r["plat"],
                    "image": r["path"], "platform": "psp" if r["kind"] == "psp" else "vita", "fit": [w, h],
                    "scale": k, "dims": [w * k, h * k], "complete": True, "vertical": True,
                    "refs": [{"box": s(c["box"]), "text": c["text"]} for c in r["columns"]],
                    "ignore": [s(b) for b in r["ignore"]]}
            units.append({**base, "id": r["id"] + ":tight", "kind": "tight", "region": s(r["region"])})
            units.append({**base, "id": r["id"] + ":full", "kind": "full", "region": [0, 0, w * k, h * k]})
    if a.shots:
        reserved = json.loads(Path(a.reserved).read_text())
        test = set(reserved.get("test") or [])
        sessions = set(reserved.get("sessions") or [])
        if a.test_games:
            tg = json.loads(Path(a.test_games).read_text())
            test |= set(tg.get("all") or [])
            sessions |= set(tg.get("sessions") or [])
        kept = skipped = 0
        for r in json.loads(Path(a.shots).read_text()):
            if r.get("origin") != "vndb" or r.get("plat") not in ("psv", "psp"):
                continue
            if r["vn"] in test or r.get("release") in sessions or r.get("vpage") or r.get("vl") \
                    or not os.path.exists(r["path"]):
                skipped += 1
                continue
            psp = r["plat"] == "psp"
            fit = list(fit_scale(r["dims"][0], r["dims"][1], (480, 272) if psp else CONSOLE))
            k = 2 if psp else 1
            units.append({"title": r["vn"], "page": r["shot"], "split": "vndb:" + r["plat"], "image": r["path"],
                          "platform": "psp" if psp else "vita", "fit": fit, "scale": k,
                          "dims": [fit[0] * k, fit[1] * k],
                          "complete": False, "vertical": False, "id": r["shot"] + ":full", "kind": "full",
                          "region": [0, 0, fit[0] * k, fit[1] * k], "refs": [], "ignore": []})
            kept += 1
        print(f"VNDB shots: {kept} kept, {skipped} left out (test VN or session, a vertical line, no file)")
    with open(a.out, "w", encoding="utf-8") as fh:
        for u in units:
            fh.write(json.dumps(u, ensure_ascii=False) + "\n")
    print(f"{len(units)} units -> {a.out}")


def cmd_units(a):
    titles = set(a.titles.split(",")) if a.titles else None
    skip = set(a.skip_titles.split(",")) if a.skip_titles else set()
    splits = set(a.splits.split(",")) if a.splits else None
    regions = a.regions.split(",")
    units = []
    for f in sorted(Path(a.pagepool).glob("*.jsonl")):
        if (titles and f.stem not in titles) or f.stem in skip:
            continue
        per = []
        with open(f, encoding="utf-8") as fh:
            for line in fh:
                rec = json.loads(line)
                if rec.get("untrusted") or (splits and rec["split"] not in splits):
                    continue
                per.extend(page_units(rec, regions))
        if "fixed" in regions:
            per.extend(fixed_units(per))
        if a.max and len(per) > a.max:
            per = random.Random(f"{f.stem}:{a.seed}").sample(per, a.max)
            per.sort(key=lambda u: u["id"])
        units.extend(per)
    with open(a.out, "w", encoding="utf-8") as fh:
        for u in units:
            fh.write(json.dumps(u, ensure_ascii=False) + "\n")
    kinds = defaultdict(int)
    for u in units:
        kinds[(u["vertical"], u["kind"])] += 1
    print(f"{len(units)} units: " + ", ".join(f"{'vertical' if v else 'horizontal'} {k} {n}"
                                               for (v, k), n in sorted(kinds.items())))


def read_jsonl(path):
    with open(path, encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]


def run_shard(units, a, out_rows, lock):
    from PIL import Image  # the host image's Pillow (tools/gate/host.Dockerfile)
    args = [a.tool, a.models] + (a.tool_args.split() if a.tool_args else [])
    p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    failed = []

    def feed():
        cache = (None, None)
        try:
            for u in units:
                if cache[0] != u["image"]:
                    im = Image.open(Path(a.pages) / u["image"]).convert("RGBA")
                    if u.get("fit") and tuple(u["fit"]) != im.size:  # a page scaled to fit the screen
                        down = u["fit"][0] < im.width
                        im = im.resize(tuple(u["fit"]), Image.BOX if down else Image.BILINEAR)
                    if u.get("scale", 1) != 1:  # the console view: a PSP frame shown 2x
                        im = im.resize((im.width * u["scale"], im.height * u["scale"]), Image.BILINEAR)
                    cache = (u["image"], im)
                x, y, w, h = u["region"]
                crop = cache[1].crop((x, y, x + w, y + h))
                psp = int(u.get("platform") == "psp")
                p.stdin.write(f"{u['id']} {w} {h} {psp}\n".encode() + crop.tobytes())
        except BrokenPipeError:
            pass
        except Exception as e:  # a page that cannot be read: the tool gets no more jobs, and the run fails below
            failed.append(repr(e))
        finally:
            try:
                p.stdin.close()
            except BrokenPipeError:
                pass

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    rows = [json.loads(line) for line in p.stdout]
    t.join()
    err = p.stderr.read().decode(errors="replace")
    if failed:
        p.wait()
        raise SystemExit(f"cannot feed the regions: {failed[0]}")
    if p.wait() or len(rows) != len(units):
        raise SystemExit(f"vjo-page-ocr failed ({len(rows)} of {len(units)} results): {err[-2000:]}")
    with lock:
        out_rows.extend(rows)


def cmd_run(a):
    units = read_jsonl(a.units)
    shards = [units[i::a.workers] for i in range(a.workers)]
    # pages in order within a shard: each image is decoded once for its units
    shards = [sorted(s, key=lambda u: u["id"]) for s in shards]
    rows, lock = [], threading.Lock()
    threads = [threading.Thread(target=run_shard, args=(s, a, rows, lock)) for s in shards if s]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    order = {u["id"]: i for i, u in enumerate(units)}
    if len(rows) != len(units):
        raise SystemExit(f"{len(rows)} results for {len(units)} units")
    rows.sort(key=lambda r: order[r["id"]])
    with open(a.out, "w", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"{len(rows)} results -> {a.out}")


def load_rgba(u, pages):
    from PIL import Image
    im = Image.open(Path(pages) / u["image"]).convert("RGBA")
    if u.get("fit") and tuple(u["fit"]) != im.size:
        im = im.resize(tuple(u["fit"]), Image.BOX if u["fit"][0] < im.width else Image.BILINEAR)
    if u.get("scale", 1) != 1:
        im = im.resize((im.width * u["scale"], im.height * u["scale"]), Image.BILINEAR)
    x, y, w, h = u["region"]
    return im.crop((x, y, x + w, y + h)).tobytes(), w, h


def cmd_abtime(a):
    import math
    import statistics
    units = read_jsonl(a.units)
    if a.max and len(units) > a.max:
        units = sorted(random.Random(a.seed).sample(units, a.max), key=lambda u: u["id"])
    arms = []
    for spec in a.arm:
        name, rest = spec.split("=", 1)
        tool, _, args = rest.partition(":")
        p = subprocess.Popen([tool, a.models] + args.split(), stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        arms.append((name, p))
    keys = ("rc", "text", "lines", "found", "frames", "strips", "workspace")
    times = {name: [] for name, _ in arms}
    same = {name: 0 for name, _ in arms}
    for i, u in enumerate(units):
        rgba, w, h = load_rgba(u, a.pages)
        head = f"{u['id']} {w} {h} {int(u.get('platform') == 'psp')}\n".encode()
        best = {}
        outs = {}
        for rep in range(a.reps):
            order = arms[(i + rep) % len(arms):] + arms[:(i + rep) % len(arms)]
            for name, p in order:
                p.stdin.write(head + rgba)
                p.stdin.flush()
                r = json.loads(p.stdout.readline())
                best[name] = min(best.get(name, 1e18), r["wall_us"])
                outs[name] = {k: r[k] for k in keys}
        first = arms[0][0]
        for name, _ in arms:
            times[name].append(best[name])
            same[name] += outs[name] == outs[first]
    for _, p in arms:
        p.stdin.close()
        p.wait()
    first = arms[0][0]
    res = {"units": len(units), "reps": a.reps, "arms": {}}
    for name, _ in arms:
        t = times[name]
        lr = [math.log(b / max(1.0, x)) for x, b in zip(times[first], t) if x > 0 and b > 0]
        m = statistics.mean(lr) if lr else 0.0
        se = statistics.stdev(lr) / math.sqrt(len(lr)) if len(lr) > 1 else 0.0
        res["arms"][name] = {"median_ms": round(statistics.median(t) / 1000, 3),
                             "mean_ms": round(statistics.mean(t) / 1000, 3),
                             "ratio_geomean": round(math.exp(m), 4),
                             "ratio_ci95": [round(math.exp(m - 1.96 * se), 4), round(math.exp(m + 1.96 * se), 4)],
                             "same_as_first": same[name]}
    Path(a.out).write_text(json.dumps(res, indent=1) + "\n")
    print(json.dumps(res, indent=1))


def boot(values, n=10000, seed=0):
    """95% interval of sum(errs) / sum(chars) over pages resampled with replacement."""
    if not values:
        return None
    rng = random.Random(seed)
    k = len(values)
    stats = []
    for _ in range(n):
        e = c = 0
        for _ in range(k):
            ve, vc = values[rng.randrange(k)]
            e += ve
            c += vc
        stats.append(e / max(1, c))
    stats.sort()
    return [round(100 * stats[int(0.025 * n)], 2), round(100 * stats[int(0.975 * n) - 1], 2)]


def score_unit(u, r):
    """Per unit: columns matched, found, refs; dialogue and shown edit distances; whether it was read as columns."""
    ox, oy = u["region"][0], u["region"][1]
    lines = r.get("lines") or []
    dets = [[ln["box"][0] + ox, ln["box"][1] + oy, ln["box"][2], ln["box"][3]] for ln in lines]
    refs = [ref["box"] for ref in u["refs"]]
    ignore = u["ignore"]
    cols = bool(r.get("columns"))
    o = {"id": u["id"], "kind": u["kind"], "vertical": u["vertical"], "split": u["split"], "rc": r["rc"],
         "columns": cols, "switched": bool(r.get("switched")), "found": r.get("found", 0),
         "strips": r.get("strips", 0), "wall_us": r.get("wall_us", 0), "frames": r.get("frames", 0)}
    if u["vertical"]:
        ref_text = "".join(norm(ref["text"]) for ref in u["refs"])  # right to left (page_units)
        # the job's lines, but those over another text box of the page (a speaker's name), in the job's order:
        # columns right to left, or (read as rows) rows top to bottom; recall and precision count columns only
        keep = [] if r["rc"] else [i for i in range(len(dets)) if not any(mostly_inside(dets[i], b) for b in ignore)]
        if cols and not r["rc"]:
            pairs, free_d, free_r = match(dets, refs, ignore)
        else:
            pairs, free_d, free_r = [], [], list(range(len(refs)))
        hyp = "".join(norm(lines[i]["text"]) for i in keep)
        o.update(tp=len(pairs), fp=len(free_d), fn=len(free_r), refs=len(refs),
                 errs=lev(ref_text, hyp), chars=len(ref_text),
                 shown_errs=lev(ref_text, norm(r.get("text", ""))))
        order = [j for _, j in sorted(pairs)]
        o["order_ok"] = order == sorted(order)
    return o


def summarize(rows):
    out = {}
    groups = defaultdict(list)
    for o in rows:
        groups[("vertical" if o["vertical"] else "horizontal") + ":" + o["kind"]].append(o)
    for key, g in sorted(groups.items()):
        s = {"units": len(g), "rc_nonzero": sum(1 for o in g if o["rc"]),
             "read_as_columns": sum(o["columns"] for o in g),
             "auto_switched": sum(o["switched"] for o in g)}
        if g[0]["vertical"]:
            tp, fp, fn = (sum(o[k] for o in g) for k in ("tp", "fp", "fn"))
            errs, chars, shown = (sum(o[k] for o in g) for k in ("errs", "chars", "shown_errs"))
            s.update(columns=tp + fn, recall=round(tp / max(1, tp + fn), 4), precision=round(tp / max(1, tp + fp), 4),
                     regions_exact=sum(1 for o in g if not o["fp"] and not o["fn"] and o["order_ok"]),
                     cer=round(100 * errs / max(1, chars), 2), cer_ci95=boot([(o["errs"], o["chars"]) for o in g]),
                     chars=chars, errs=errs, shown_cer=round(100 * shown / max(1, chars), 2),
                     shown_cer_ci95=boot([(o["shown_errs"], o["chars"]) for o in g]))
        walls = sorted(o["wall_us"] for o in g)
        strips = sorted(o["strips"] for o in g)
        if walls:
            s.update(wall_ms_median=round(walls[len(walls) // 2] / 1000, 2),
                     wall_ms_mean=round(sum(walls) / len(walls) / 1000, 2),
                     strips_median=strips[len(strips) // 2], strips_mean=round(sum(strips) / len(strips), 2))
        out[key] = s
    return out


def cmd_score(a):
    units = {u["id"]: u for u in read_jsonl(a.units)}
    rows = [score_unit(units[r["id"]], r) for r in read_jsonl(a.results)]
    res = {"units": a.units, "results": a.results, "summary": summarize(rows)}
    by_split = defaultdict(list)
    for o in rows:
        by_split[o["split"]].append(o)
    res["by_split"] = {k: summarize(v) for k, v in sorted(by_split.items())}
    text = json.dumps(res, ensure_ascii=False, indent=1)
    if a.out:
        Path(a.out).write_text(text + "\n", encoding="utf-8")
    print(json.dumps(res["summary"], ensure_ascii=False, indent=1))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    u = sub.add_parser("units")
    u.add_argument("--pagepool", required=True)
    u.add_argument("--titles")
    u.add_argument("--skip-titles")
    u.add_argument("--splits")
    u.add_argument("--regions", default="tight,card,fixed,full,base")
    u.add_argument("--max", type=int, default=0, help="at most this many units per title (a seeded sample)")
    u.add_argument("--seed", default="20261026")
    u.add_argument("--out", required=True)
    x = sub.add_parser("ext-units")
    x.add_argument("--vdev")
    x.add_argument("--shots")
    x.add_argument("--reserved")
    x.add_argument("--test-games")
    x.add_argument("--out", required=True)
    r = sub.add_parser("run")
    r.add_argument("--units", required=True)
    r.add_argument("--pages", required=True)
    r.add_argument("--tool", required=True)
    r.add_argument("--models", required=True)
    r.add_argument("--tool-args", default="")
    r.add_argument("--workers", type=int, default=4)
    r.add_argument("--out", required=True)
    t = sub.add_parser("abtime")
    t.add_argument("--units", required=True)
    t.add_argument("--pages", required=True)
    t.add_argument("--models", required=True)
    t.add_argument("--arm", action="append", required=True)
    t.add_argument("--reps", type=int, default=3)
    t.add_argument("--max", type=int, default=0)
    t.add_argument("--seed", default="20261026")
    t.add_argument("--out", required=True)
    s = sub.add_parser("score")
    s.add_argument("--units", required=True)
    s.add_argument("--results", required=True)
    s.add_argument("--out")
    a = p.parse_args(argv)
    {"units": cmd_units, "ext-units": cmd_ext_units, "run": cmd_run, "abtime": cmd_abtime, "score": cmd_score}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
