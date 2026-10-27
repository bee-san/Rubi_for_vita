#!/usr/bin/env python3
"""An animated icon or background in the OCR region, on hooked pages: when the text is shown after L + R, and what.

    python3 tools/spinner_eval.py --binary build/host/vjo-spinner-bench --models DIR --pagepool DIR \\
        [--per-title 4] [--titles T ...] [--exclude T ...] [--scenarios A,B] [--press-ms 3000] [--jobs 5] \\
        -o OUT.json [--texts TEXTS.jsonl]
    python3 tools/spinner_eval.py --binary ... --models DIR --synthetic -o OUT.json   (the bench's own 3-line box)

--pagepool: vita-vn-ocr's hooked page pool (pagepool/v1: one JSONL per title, each page's hook text, its verified line
boxes and its dialogue `regions`), pages beside it in ../pages. Each sampled page is read in its base dialogue region,
widened to the right by three times its last line's height and downwards by 1.5 times (room for an icon beside the
text: the page's own pixels there), a PSP page first scaled 2x as Adrenaline shows it (bilinear, as tools/gate_eval.py).
host/spinner_bench.c's scenarios (an icon after the last line or in the region's corner, at constant or variable speed;
a moving background; the last line typing out) run on each, with the real kernel change detection, the game worker's
job and a model of the Shell's decisions. A region's text read with an icon is compared with its still read (the
motion's own effect, whatever the OCR's errors) and with the hook's text.

Pages, crops and texts stay local: OUT.json has counts, rates and times only; --texts (local, never committed) has
each read's text. Host execution, console time estimated (host/spinner_bench.c): no console has run this.
"""
import argparse
import json
import multiprocessing
import os
import statistics
import subprocess
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_eval import fold, similarity  # noqa: E402  (the same compared form as the gate's evaluation)


def pages_of(pagepool, titles, exclude, per_title):
    """Up to per_title pages of each title, spread over its pages: complete, trusted, horizontal, with lines and a base
    region."""
    out = []
    for f in sorted(Path(pagepool).glob("*.jsonl")):
        tid = f.stem
        if (titles and tid not in titles) or tid in exclude:
            continue
        good = []
        for line in f.open(encoding="utf-8"):
            p = json.loads(line)
            if not p.get("complete") or p.get("untrusted") or p.get("text_free") or not p.get("lines"):
                continue
            if any(ln.get("vertical") for ln in p["lines"]):
                continue
            base = [r for r in p.get("regions") or [] if r.get("variant") == "base"]
            if not base:
                continue
            good.append((p, base[0]))
        if not good:
            continue
        step = max(1, len(good) // per_title)
        out.extend(good[::step][:per_title])
    return out


def job_of(page, base, pages_dir):
    psp = page.get("platform") == "psp"
    s = 2 if psp else 1
    im = Image.open(Path(pages_dir) / page["image"])
    im.load()
    if psp:
        im = im.resize((im.width * 2, im.height * 2), Image.BILINEAR)
    x0, y0, x1, y1 = base["region"]
    ids = set(base.get("lines") or [])
    lines = [ln for ln in page["lines"] if ln["id"] in ids] or page["lines"]
    last_h = max(4, lines[-1]["box"][3])
    x0, y0 = max(0, int(round(x0 * s)) - 4), max(0, int(round(y0 * s)) - 4)
    x1 = min(im.width, int(round(x1 * s + 3 * last_h * s)))
    y1 = min(im.height, int(round(y1 * s + 1.5 * last_h * s)))
    if x1 - x0 < 16 or y1 - y0 < 8:
        return None
    crop = im.crop((x0, y0, x1, y1)).convert("RGB")
    boxes = []
    for ln in lines:
        bx, by, bw, bh = ln["box"]
        bx, by = int(round(bx * s)) - x0, int(round(by * s)) - y0
        bw, bh = int(round(bw * s)), int(round(bh * s))
        bx, by = max(0, bx), max(0, by)
        if bx >= crop.width or by >= crop.height:
            continue
        boxes.append((bx, by, min(bw, crop.width - bx), min(bh, crop.height - by)))
    if not boxes:
        return None
    boxes = boxes[:16]
    rid = f"{page['title_id']}:{page['page']}"
    head = f"{rid} {crop.width} {crop.height} {len(boxes)}\n" + "".join(f"{b[0]} {b[1]} {b[2]} {b[3]}\n" for b in boxes)
    return rid, head.encode() + crop.tobytes(), "".join(ln.get("text") or "" for ln in lines)


def work(args):
    items, binary, models, scenarios, press_ms, max_ms, pages_dir = args
    cmd = [binary, models, "--press-ms", str(press_ms), "--max-ms", str(max_ms)]
    if scenarios:
        cmd += ["--scenarios", scenarios]
    jobs, hooks = [], {}
    for page, base in items:
        j = job_of(page, base, pages_dir)
        if j:
            jobs.append(j[1])
            hooks[j[0]] = j[2]
    p = subprocess.run(cmd, input=b"".join(jobs), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if p.returncode:
        raise SystemExit(f"{binary} failed ({p.returncode}): {p.stderr.decode(errors='replace')[-400:]}")
    out = []
    for line in p.stdout.decode("utf-8").splitlines():
        r = json.loads(line)
        r["hook"] = hooks.get(r["id"], "")
        out.append(r)
    return out


def pct(n, d):
    return round(100.0 * n / d, 1) if d else None


def summary(results, max_ms):
    still = {r["id"]: r for r in results if r["scenario"] == "still"}
    by = {}
    for r in results:
        by.setdefault(r["scenario"], []).append(r)
    table = {}
    for name, rs in by.items():
        shown = [r for r in rs if r["shown"] and r["text"]]
        times = sorted(r["show_ms"] for r in shown)
        sim_still, sim_hook, missed_chars, same = [], [], 0, 0
        for r in rs:
            base = still.get(r["id"])
            if r["shown"] and base and base["shown"]:
                sim_still.append(similarity(r["text"], base["text"]) if fold(base["text"]) else 1000)
                same += fold(r["text"]) == fold(base["text"])
                missed_chars += max(0, len(fold(base["text"])) - len(fold(r["text"])))
            if r["shown"] and r["hook"]:
                sim_hook.append(similarity(r["text"], r["hook"]))
        table[name] = {
            "regions": len(rs),
            "shown": len(shown),
            "shown_pct": pct(len(shown), len(rs)),
            "never_shown": len(rs) - len(shown),
            "symptom_text_recognized_never_shown": sum(1 for r in rs if r["symptom"]),
            "jobs_recognized_then_dropped": sum(r.get("recognized_dropped", 0) for r in rs),
            "jobs_text_not_shown": sum(r["text_not_shown"] for r in rs),
            "show_ms_median": statistics.median(times) if times else None,
            "show_ms_p90": times[int(0.9 * (len(times) - 1))] if times else None,
            "show_ms_max": times[-1] if times else None,
            "shown_by_background_result": sum(1 for r in shown
                                              if r["how"] in ("the background result", "the shown text")),
            "same_text_as_still": same,
            "similarity_to_still_mean": round(statistics.mean(sim_still), 1) if sim_still else None,
            "chars_missed_vs_still": missed_chars,
            "similarity_to_hook_mean": round(statistics.mean(sim_hook), 1) if sim_hook else None,
            "jobs": sum(r["jobs"] for r in rs),
            "screen_changed": sum(r["screen_changed"] for r in rs),
            "settle_timeouts": sum(r["settle_timeouts"] for r in rs),
            "kernel_settled": sum(r["kernel"]["settled"] for r in rs),
            "kernel_quiet": sum(r["kernel"]["quiet"] for r in rs),
            "kernel_scene_changes_median": statistics.median(r["kernel"]["scene_changes"] for r in rs),
            "allocations_after_first": sum(r["memory"]["allocations"] for r in rs[1:]) if len(rs) > 1 else 0,
            "capture_block_max": max(r["memory"]["capture_block"] for r in rs),
            "tracker_bytes": rs[0]["memory"]["tracker"],
        }
    return {"max_ms": max_ms, "scenarios": table}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--models", required=True)
    ap.add_argument("--pagepool", help="vita-vn-ocr's hooked page pool (pagepool/v1)")
    ap.add_argument("--synthetic", action="store_true", help="the bench's synthetic box instead of hooked pages")
    ap.add_argument("--pages", help="the page images' root (default: the pool's ../../pages)")
    ap.add_argument("--per-title", type=int, default=4)
    ap.add_argument("--titles", nargs="*", default=[])
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--scenarios", default="")
    ap.add_argument("--press-ms", type=int, default=3000)
    ap.add_argument("--max-ms", type=int, default=20000)
    ap.add_argument("--jobs", type=int, default=5)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--texts", help="each read's text, local only (never commit it)")
    a = ap.parse_args()
    if a.synthetic:
        cmd = [a.binary, a.models, "--synthetic", "--press-ms", str(a.press_ms), "--max-ms", str(a.max_ms)]
        if a.scenarios:
            cmd += ["--scenarios", a.scenarios]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)
        results = [dict(json.loads(line), hook="") for line in p.stdout.decode("utf-8").splitlines()]
    else:
        if not a.pagepool:
            ap.error("--pagepool or --synthetic")
        pages_dir = a.pages or str(Path(a.pagepool).resolve().parent.parent / "pages")
        items = pages_of(a.pagepool, set(a.titles), set(a.exclude), a.per_title)
        chunks = [items[i::a.jobs] for i in range(a.jobs)]
        args = [(c, a.binary, a.models, a.scenarios, a.press_ms, a.max_ms, pages_dir) for c in chunks if c]
        with multiprocessing.Pool(len(args)) as pool:
            results = [r for part in pool.map(work, args) for r in part]
    results.sort(key=lambda r: (r["id"], r["scenario"]))
    out = {"pages": len({r["id"] for r in results}), "titles": len({r["id"].split(":")[0] for r in results}),
           "press_ms": a.press_ms, **summary(results, a.max_ms)}
    Path(a.out).write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    if a.texts:
        with open(a.texts, "w", encoding="utf-8") as f:
            for r in results:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(json.dumps(out["scenarios"], indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
