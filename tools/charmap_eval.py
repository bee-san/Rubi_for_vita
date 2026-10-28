#!/usr/bin/env python3
"""How well the overlay on the game's text places characters (API 46, docs/overlay-on-text.md, #309).

Runs the game worker's own job (host/ocr_tool.c `vjo-ocr --chars`: the finder, the recognizer, the LM and the
cascade as config.ini's defaults set them, the CTC frames' cells and the char map the worker writes) on the
dialogue regions of a hooked corpus's pages, and scores the places against the corpus's renderer glyph cells
(the boxes a capture build of the emulator took from the game's own draw calls: what "hook positions" give).

  charmap_eval.py run   --pagepool DIR --pages DIR --ocr vjo-ocr --models DIR --work DIR [--split hval] [--jobs N]
                        [--hook lines|flat]
  charmap_eval.py score --pagepool DIR --work DIR --out results.json [--split hval]

The corpus is private: its pages, crops and texts stay under --work (never in the repository); --out holds
counts and rates only. Every source of places is scored on the same characters:

  ocr_cells    the char map as the worker writes it: the CTC frame each character fires at (vita-vn-ocr
               runtime/vocr_charbox.c), the shown text aligned to them (core/charmap_build.c)
  ocr_split    the OCR line's box cut by the OCR text's advance (full-width 1, else 1/2: GameSentenceMiner's
               distributeGlyphWidths), the shown characters aligned the same way
  hook_finder  a hook's text without recognition: the finder's line box cut by the hook line's advance
  hook_box     the same cut of the line's true extent (the union of its glyph cells): the cut's own error

With --hook, the job's text is the region's hooked text (its dialogue lines joined by line breaks, or flat: with
none, as an engine that wraps its message on the screen hands it over), placed on OCR's characters as the game
worker places a native hook's text (vjo-ocr --hook-text); ocr_cells then scores that.

A character counts as hit when a tap at the middle of its glyph cell selects the character the overlay holds for
it (vjo_cmap_hit, with its slack; the shown text aligned to the hooked text by edit distance); as on its glyph
when the middle of its place lies inside its glyph cell. Screen px: a Vita page as it is, a PSP page 2x (the
Vita shows a PSP frame 2x, and Rubi reads Adrenaline's frame)."""
import argparse
import json
import os
import random
import subprocess
import sys
import unicodedata
from concurrent.futures import ProcessPoolExecutor

SLACK = 12  # core/charmap.h VJO_CMAP_SLACK
MODEL = "VOCR5_N_w8g2.vocr"   # vocr_tier = normal
CASCADE = "VOCR5_L_w8g2.vocr"  # ocr_cascade_ms = 2000: the next tier up
LM = "vocr_lm_o3_1000k.vlm"    # ocr_lm = 1000k


def records(pagepool, split):
    for name in sorted(os.listdir(pagepool)):
        if not name.endswith(".jsonl"):
            continue
        with open(os.path.join(pagepool, name), encoding="utf-8") as f:
            for line in f:
                r = json.loads(line)
                if r["split"] == split and r.get("regions"):
                    yield r


def base_region(r):
    for g in r["regions"]:
        if g["variant"] == "base":
            return g
    return None


def job_key(r):
    return f"{r['title_id']}/{r['page']}"


def hook_text(r, mode):
    """The region's hooked text: its dialogue lines (no names), joined by line breaks or (flat) by nothing."""
    by_id = {l["id"]: l for l in r["lines"]}
    texts = [by_id[i]["text"] for i in base_region(r)["lines"]
             if i in by_id and by_id[i]["kind"] == "line" and not by_id[i]["vertical"]]
    return ("\n" if mode == "lines" else "").join(texts)


def run_one(a):
    """One page's dialogue region through vjo-ocr --chars (resumable: an existing output is kept)."""
    from PIL import Image
    r, pages, work, ocr, models, hook = a
    out = os.path.join(work, job_key(r) + ".json")
    if os.path.exists(out):
        return out, 0
    os.makedirs(os.path.dirname(out), exist_ok=True)
    g = base_region(r)
    s = 2 if r["platform"] == "psp" else 1
    im = Image.open(os.path.join(pages, r["image"])).convert("RGBA")
    if s != 1:
        im = im.resize((im.width * s, im.height * s), Image.BILINEAR)
    x0, y0, x1, y1 = (int(round(v * s)) for v in g["region"])
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(im.width, x1), min(im.height, y1)
    crop = im.crop((x0, y0, x1, y1))
    raw = out[:-5] + ".rgba"
    with open(raw, "wb") as f:
        f.write(crop.tobytes())
    tmp = out + ".part"
    args = [ocr, "--backend", "vocr", "--model", MODEL, "--lm", LM, "--cascade", CASCADE, "--cascade-ms", "2000",
            "--counters", "on", "--head-gate", "on", "--two-level", "on", "--refine", "on" if s == 2 else "off",
            "--chars", tmp]
    if hook:
        with open(out[:-5] + ".hook", "w", encoding="utf-8") as f:
            f.write(hook_text(r, hook))
        args += ["--hook-text", out[:-5] + ".hook"]
    p = subprocess.run(args + [models, str(crop.width), str(crop.height), raw], stdin=subprocess.DEVNULL,
                       capture_output=True, timeout=600)
    os.remove(raw)
    if hook:
        os.remove(out[:-5] + ".hook")
    doc = {"rc": p.returncode, "region": [x0, y0, x1, y1], "scale": s, "stderr": p.stderr.decode()[-400:]}
    if os.path.exists(tmp):
        with open(tmp, encoding="utf-8") as f:
            doc["chars"] = json.load(f)
        os.remove(tmp)
    with open(out + ".tmp", "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
    os.replace(out + ".tmp", out)
    return out, p.returncode


def cmd_run(a):
    jobs = [(r, a.pages, a.work, a.ocr, a.models, a.hook) for r in records(a.pagepool, a.split) if base_region(r)]
    if a.limit:
        jobs = jobs[: a.limit]
    done = bad = 0
    with ProcessPoolExecutor(a.jobs) as ex:
        for _, rc in ex.map(run_one, jobs, chunksize=4):
            done += 1
            bad += rc != 0
            if done % 200 == 0:
                print(f"{done}/{len(jobs)} regions, {bad} failed", flush=True)
    print(f"{done} regions, {bad} failed")


# ---- scoring ----

def advance(ch):
    """GameSentenceMiner's glyph advance weight (GSM_Overlay/index.html glyphAdvanceWeight)."""
    if ord(ch) < 0x20 or 0x7F <= ord(ch) <= 0x9F:
        return 0.0
    return 1.0 if unicodedata.east_asian_width(ch) in ("F", "W") else 0.5


def split_cells(text, x0, x1):
    w = [advance(c) for c in text]
    tot = sum(w) or 1.0
    out, at = [], float(x0)
    for v in w:
        nx = at + (x1 - x0) * v / tot
        out.append((at, nx))
        at = nx
    return out


def align(a, b):
    """Edit-distance alignment of sequences a and b: {i: j} for the pairs it matches or substitutes."""
    n, m = len(a), len(b)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        d[i][0] = i
    for j in range(m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        ai, row, prev = a[i - 1], d[i], d[i - 1]
        for j in range(1, m + 1):
            row[j] = min(prev[j] + 1, row[j - 1] + 1, prev[j - 1] + (ai != b[j - 1]))
    pairs, i, j = {}, n, m
    while i and j:
        if d[i][j] == d[i - 1][j - 1] + (a[i - 1] != b[j - 1]):
            pairs[i - 1] = j - 1
            i, j = i - 1, j - 1
        elif d[i][j] == d[i - 1][j] + 1:
            i -= 1
        else:
            j -= 1
    return pairs


def outside(v, lo, hi):
    return lo - v if v < lo else v - hi + 1 if v >= hi else 0


def hit(places, x, y):
    """core/charmap.c vjo_cmap_hit over [(x0, x1, y0, y1)]: the index, or -1."""
    best, best_d = -1, SLACK + 1
    for k, (a, b, y0, y1) in enumerate(places):
        if b <= a:
            continue
        d = outside(y, y0, y1) + outside(x, a, b)
        if d < best_d:
            best, best_d = k, d
    return best


def iou(p, c):
    ix = max(0.0, min(p[1], c[2]) - max(p[0], c[0]))
    iy = max(0.0, min(p[3], c[3]) - max(p[2], c[1]))
    inter = ix * iy
    ua = (p[1] - p[0]) * (p[3] - p[2]) + (c[2] - c[0]) * (c[3] - c[1]) - inter
    return inter / ua if ua > 0 else 0.0


def score_places(ref_chars, ref_cells, shown, places):
    """ref_chars/ref_cells: the hooked characters and glyph cells (screen px); shown: the overlay's characters;
    places: their (x0, x1, y0, y1). Per hooked character: (hit, on_glyph, iou, placed)."""
    pairs = align(ref_chars, shown)
    out = []
    for k, c in enumerate(ref_cells):
        j = pairs.get(k)
        cx, cy = (c[0] + c[2]) / 2.0, (c[1] + c[3]) / 2.0
        h = hit(places, int(cx), int(cy))
        placed = j is not None and places[j][1] > places[j][0]
        on = placed and c[0] <= (places[j][0] + places[j][1]) / 2.0 < c[2] and \
            c[1] <= (places[j][2] + places[j][3]) / 2.0 < c[3]
        out.append((j is not None and h == j, on, iou(places[j], c) if placed else 0.0, placed))
    return out


def utf8_index(s):
    """byte offset -> code point index"""
    m, at = {}, 0
    for i, ch in enumerate(s):
        m[at] = i
        at += len(ch.encode("utf-8"))
    return m


def region_refs(r, s, origin):
    """The region's hooked lines with glyph cells, in the region's screen px: [(text, cells, source)]."""
    g = base_region(r)
    by_id = {l["id"]: l for l in r["lines"]}
    out = []
    for lid in g["lines"]:
        l = by_id.get(lid)
        if not l or l["kind"] != "line" or l["vertical"] or not l.get("cell_boxes") or l.get("drop"):
            continue
        if len(l["cell_boxes"]) != len(l["text"]):
            continue
        cells = [[v * s - o for v, o in zip(c, (origin[0], origin[1], origin[0], origin[1]))] for c in l["cell_boxes"]]
        out.append((l["text"], cells, l["source"]))
    return out


def score_region(r, doc):
    s = doc["scale"]
    refs = region_refs(r, s, doc["region"])
    if not refs:
        return None
    ch = doc.get("chars") or {"sentence": "", "lines": [], "chars": [], "ocr": []}
    sent = ch["sentence"]
    idx = utf8_index(sent)
    places = [None] * len(sent)
    for (y0, y1, first, count) in ch["lines"]:
        for k in range(first, first + count):
            off, x0, x1 = ch["chars"][k]
            if off in idx:
                places[idx[off]] = (x0, x1, y0, y1)
    shown = list(sent)
    places = [p if p else (0, 0, 0, 0) for p in places]
    # the OCR line split: the greedy text of each recognized line cut along its box
    sp_chars, sp_places = [], []
    for (x, y, w, h, gl) in ch["ocr"]:
        t = "".join(chr(c[0]) for c in gl)
        for (a, b), c in zip(split_cells(t, x, x + w), t):
            sp_chars.append(c)
            sp_places.append((int(a), int(b), y, y + h))
    ref_text = [c for t, _, _ in refs for c in t]
    ref_cells = [c for _, cells, _ in refs for c in cells]
    src = [sname for t, _, sname in refs for _ in t]
    res = {"ocr_cells": score_places(ref_text, ref_cells, shown, places),
           "ocr_split": score_places(ref_text, ref_cells, sp_chars, sp_places)}
    # a hook's text without recognition: each hooked line cut along the finder's box that overlaps it most (or its
    # true extent)
    hf, hb = [], []
    for t, cells, _ in refs:
        y0, y1 = min(c[1] for c in cells), max(c[3] for c in cells)
        best, bo = None, 0.0
        for (x, y, w, h, gl) in ch["ocr"]:
            ov = min(y1, y + h) - max(y0, y)
            if ov > bo:
                best, bo = (x, y, w, h), ov
        truth = split_cells(t, min(c[0] for c in cells), max(c[2] for c in cells))
        hb += score_places(list(t), cells, list(t), [(int(a), int(b), int(y0), int(y1)) for a, b in truth])
        if best and bo >= 0.5 * (y1 - y0):
            cut = split_cells(t, best[0], best[0] + best[2])
            hf += score_places(list(t), cells, list(t),
                               [(int(a), int(b), best[1], best[1] + best[3]) for a, b in cut])
        else:
            hf += [(False, False, 0.0, False)] * len(t)
    res["hook_finder"], res["hook_box"] = hf, hb
    return {"title": r["title_id"], "platform": r["platform"], "sources": src, "res": res,
            "ok": doc["rc"] == 0, "cer_chars": len(ref_text), "shown": len(shown)}


def summarize(rows, key, src_filter="glyph_cells"):
    agg, per = {"chars": 0, "hit": 0, "on": 0, "placed": 0, "iou": 0.0}, {}
    for row in rows:
        t = per.setdefault(row["title"], {"chars": 0, "hit": 0, "on": 0, "placed": 0, "iou": 0.0})
        for (h, on, u, placed), sname in zip(row["res"][key], row["sources"]):
            if src_filter and sname != src_filter:
                continue
            for d in (agg, t):
                d["chars"] += 1
                d["hit"] += h
                d["on"] += on
                d["placed"] += placed
                d["iou"] += u
    def rates(d):
        n = d["chars"] or 1
        return {"chars": d["chars"], "hit": round(d["hit"] / n, 4), "on_glyph": round(d["on"] / n, 4),
                "placed": round(d["placed"] / n, 4), "mean_iou": round(d["iou"] / n, 4)}
    titles = {k: rates(v) for k, v in per.items() if v["chars"]}
    out = rates(agg)
    hits = sorted(v["hit"] for v in titles.values())
    out["titles"] = len(titles)
    out["title_macro_hit"] = round(sum(hits) / len(hits), 4) if hits else 0
    out["title_min_hit"] = hits[0] if hits else 0
    out["title_median_hit"] = hits[len(hits) // 2] if hits else 0
    # 95% interval of the pooled hit rate, resampling titles (characters cluster by title)
    rng, names, boots = random.Random(20261026), list(per), []
    for _ in range(1000):
        pick = [rng.choice(names) for _ in names]
        c = sum(per[t]["chars"] for t in pick) or 1
        boots.append(sum(per[t]["hit"] for t in pick) / c)
    boots.sort()
    out["hit_ci95"] = [round(boots[25], 4), round(boots[974], 4)]
    out["per_title"] = titles
    return out


def cmd_score(a):
    rows, regions, failed, missing = [], 0, 0, 0
    lines_by_source = {}
    for r in records(a.pagepool, a.split):
        g = base_region(r)
        if not g:
            continue
        for l in r["lines"]:
            if l["id"] in g["lines"] and l["kind"] == "line" and not l["vertical"]:
                lines_by_source[l["source"]] = lines_by_source.get(l["source"], 0) + 1
        p = os.path.join(a.work, job_key(r) + ".json")
        if not os.path.exists(p):
            missing += 1
            continue
        with open(p, encoding="utf-8") as f:
            doc = json.load(f)
        regions += 1
        failed += doc["rc"] != 0
        row = score_region(r, doc)
        if row:
            rows.append(row)
    res = {"kind": "charmap-eval", "split": a.split, "model": MODEL, "lm": LM, "cascade": CASCADE,
           "regions": regions, "regions_failed": failed, "regions_missing": missing,
           "dialogue_lines_by_source": lines_by_source, "slack_px": SLACK}
    for key in ("ocr_cells", "ocr_split", "hook_finder", "hook_box"):
        res[key] = {"all": summarize(rows, key)}
        for plat in ("vita", "psp"):
            res[key][plat] = summarize([r for r in rows if r["platform"] == plat], key)
        if a.exclude:
            ex = set(a.exclude.split(","))
            res[key]["without_" + "_".join(sorted(ex))] = summarize([r for r in rows if r["title"] not in ex], key)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1, sort_keys=True)
    for key in ("ocr_cells", "ocr_split", "hook_finder", "hook_box"):
        v = res[key]["all"]
        print(f"{key:12s} chars {v['chars']:6d} hit {v['hit']:.4f} {v['hit_ci95']} on_glyph {v['on_glyph']:.4f} "
              f"placed {v['placed']:.4f} iou {v['mean_iou']:.3f} (titles {v['titles']}, macro {v['title_macro_hit']:.4f},"
              f" min {v['title_min_hit']:.4f})")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    for k in ("pagepool", "pages", "ocr", "models", "work"):
        r.add_argument("--" + k, required=True)
    r.add_argument("--split", default="hval")
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--limit", type=int, default=0)
    r.add_argument("--hook", choices=("lines", "flat"), help="the job's text is the region's hooked text")
    s = sub.add_parser("score")
    for k in ("pagepool", "work", "out"):
        s.add_argument("--" + k, required=True)
    s.add_argument("--split", default="hval")
    s.add_argument("--exclude", default="", help="titles to leave out of an extra row, comma-separated")
    a = p.parse_args(argv)
    return cmd_run(a) if a.cmd == "run" else cmd_score(a)


if __name__ == "__main__":
    sys.exit(main())
