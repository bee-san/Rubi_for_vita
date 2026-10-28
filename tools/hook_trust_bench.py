#!/usr/bin/env python3
"""The trusted-hook bench (docs/native-hooks.md, "Trusted hooks"): OCR calls, memory and time per hooked line,
Rubi 0.9.7's selection against API 43's trusted hooks, through host/hook_trust_bench.c (the actual game worker, core
adapter and runtime with the pinned models).

    python3 tools/hook_trust_bench.py --bench build/bench/vjo-hook-trust-bench --models DIR --pagepool DIR \\
        [--pages N] [--titles ...] [--exclude ...] [--free-mib 64] [--jobs 5] -o OUT.json

Each Vita title of the hooked page pool (vita-vn-ocr's pagepool/v1, the pages beside it in ../pages) is one game
session: its first N pages in order, each a new line its hook copies, one background job each (the Shell's for a
new hooked line, API 41), in the one region a player would draw for the game (tools/gate_eval.py user_region). The session runs for each source kind (a Luna site, a
Rubi verified site, a string copy's caller, and a caller one page behind the screen, which must never be trusted),
with hook_trust off (0.9.7) and on, and for the caller with N = 4, 3 and 2 agreements (--variants: the binaries
built with VJO_HOOK_TRUST_INTERVAL 32, 16 and 8). A second free-memory figure (--tight-mib) runs the sites again in
a game short of memory.

OUT.json has per-configuration totals only: jobs, OCR jobs (the OCR path, its memory check passed), recognized
lines, CTC frames, capture strips, model reads, the worker's blocks at each job's peak and after it (mean and most),
host time per job, and how many shown texts match the page's hook text (similarity >= 700). Pages and texts stay
local. Host execution: no console timing (docs/offline-testing.md).
"""
import argparse
import json
import multiprocessing
import os
import statistics
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_eval import user_region  # noqa: E402


def session_bytes(pagepool, pages_dir, title, limit):
    """pages.bin of the title's first `limit` pages (VJTB) in the one region a player draws for the game
    (tools/gate_eval.py user_region), and their count."""
    rows = [json.loads(l) for l in open(Path(pagepool) / f"{title}.jsonl", encoding="utf-8")]
    region = user_region(rows)
    rows = [r for r in rows if r.get("text")]
    rows.sort(key=lambda r: r["page"])
    rows = rows[:limit] if limit else rows
    out = [b"VJTB"]
    if not region:
        return out[0], 0
    for r in rows:
        im = Image.open(Path(pages_dir) / r["image"])
        x0, y0, x1, y1 = (int(v) for v in region)
        c = im.convert("RGB").crop((max(0, x0), max(0, y0), min(im.width, x1), min(im.height, y1)))
        text = r["text"].encode("utf-8")
        out.append(struct.pack("<III", c.width, c.height, len(text)) + text + c.tobytes())
    return b"".join(out), len(rows)


def run(args):
    bench, models, kind, trust, free_mib, data, press = args
    p = subprocess.run([bench, models, kind, str(trust), str(free_mib)] + (["press"] if press else []), input=data,
                       capture_output=True, timeout=7200)
    if p.returncode:
        raise RuntimeError(f"{bench} {kind} {trust}: rc {p.returncode}: {p.stderr[-500:]!r}")
    return [json.loads(l) for l in p.stdout.decode("utf-8").splitlines() if l.strip()]


def totals(jobs):
    ok = [j for j in jobs if j["rc"] == 0]
    peaks = [j["peak"] for j in jobs]
    after = [j["after"] for j in jobs]
    us = [j["us"] for j in jobs]
    return {
        "jobs": len(jobs), "failed": len(jobs) - len(ok),
        "ocr_jobs": sum(j["ocr"] for j in jobs), "recognized_lines": sum(j["recognized"] for j in jobs),
        "ctc_frames": sum(j["frames"] for j in jobs), "strips": sum(j["strips"] for j in jobs),
        "weights_read": sum(1 for j in jobs if j["weights"] == 1),
        "hook_answers": sum(1 for j in ok if j["from"]), "trusted_answers": sum(1 for j in ok if j["flags"] & 0x100),
        "comparable_lines": sum(1 for j in jobs if j["cmp"]),
        "shown_matches_hook": sum(1 for j in ok if j["cmp"] and j["sim"] >= 700),
        "trusted_wrong": sum(1 for j in ok if j["flags"] & 0x100 and j["cmp"] and j["sim"] < 700),
        "peak_bytes_mean": round(statistics.mean(peaks)) if peaks else 0, "peak_bytes_max": max(peaks, default=0),
        "held_after_mean": round(statistics.mean(after)) if after else 0, "held_after_max": max(after, default=0),
        "reserve_min": min((j["reserve"] for j in jobs if j["reserve"]), default=0),
        "host_us_total": sum(us), "host_us_median": round(statistics.median(us)) if us else 0,
        "host_us_mean": round(statistics.mean(us)) if us else 0,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", required=True, help="vjo-hook-trust-bench (N = 4); its -16 and -8 builds beside it")
    ap.add_argument("--models", required=True)
    ap.add_argument("--pagepool", required=True)
    ap.add_argument("--pages-dir")
    ap.add_argument("--pages", type=int, default=300, help="pages a session (0: all)")
    ap.add_argument("--titles", nargs="*")
    # by default the titles the docs leave out (#338's audit N8): no dialogue region (PCSG00653), a release shared with
    # CLANNAD or vita-test, the pool's test title (docs/native-hooks.md "Why four agreements")
    ap.add_argument("--exclude", nargs="*", default=["PCSG00007", "PCSG00653", "PCSG00807", "PCSG00866"])
    ap.add_argument("--free-mib", type=int, default=64)
    ap.add_argument("--tight-mib", type=int, default=3)
    ap.add_argument("--jobs", type=int, default=5)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--records", help="every job's line (JSONL, local only: it holds shown texts)")
    ap.add_argument("--configs", nargs="*", help="only these configurations (e.g. call:trust1:n4)")
    ap.add_argument("--press", action="store_true",
                    help="every job a press's, without the stable-text wait (#413; #413's audit F2)")
    args = ap.parse_args()
    pages_dir = args.pages_dir or str(Path(args.pagepool).parent.parent / "pages")
    titles = args.titles or sorted(f.stem for f in Path(args.pagepool).glob("PCS*.jsonl"))
    titles = [t for t in titles if t not in set(args.exclude)]
    base = args.bench
    variants = {32: base, 16: base + "-16", 8: base + "-8"}
    configs = []  # (name, binary, kind, trust, free)
    for kind in ("luna", "verified", "call"):
        for trust in (0, 1):
            configs.append((f"{kind}:trust{trust}:n4", variants[32], kind, trust, args.free_mib))
    configs.append(("lag:trust1:n4", variants[32], "lag", 1, args.free_mib))  # 0.9.7 never trusts it either
    for n, interval in ((3, 16), (2, 8)):
        for kind in ("call", "lag"):
            configs.append((f"{kind}:trust1:n{n}", variants[interval], kind, 1, args.free_mib))
    for kind in ("luna", "verified", "call"):
        for trust in (0, 1):
            configs.append((f"{kind}:trust{trust}:n4:tight", variants[32], kind, trust, args.tight_mib))
    if args.configs:
        configs = [c for c in configs if c[0] in set(args.configs)]
    sessions = {}
    for t in titles:
        sessions[t] = session_bytes(args.pagepool, pages_dir, t, args.pages)
    tasks, keys = [], []
    for t in titles:
        data, _ = sessions[t]
        for name, binary, kind, trust, free in configs:
            tasks.append((binary, args.models, kind, trust, free, data, args.press))
            keys.append((name, t))
    with multiprocessing.Pool(max(1, args.jobs)) as pool:
        results = pool.map(run, tasks, chunksize=1)
    per = {}
    rec_out = open(args.records, "w", encoding="utf-8") if args.records else None
    for (name, t), jobs in zip(keys, results):
        per.setdefault(name, {})[t] = jobs
        if rec_out:
            for j in jobs:
                rec_out.write(json.dumps(dict(j, config=name, title=t), ensure_ascii=False) + "\n")
    if rec_out:
        rec_out.close()
    out = {"tool": "tools/hook_trust_bench.py", "pages_per_session": args.pages, "press": args.press, "titles": titles,
           "excluded_titles": sorted(args.exclude), "free_mib": args.free_mib, "tight_mib": args.tight_mib,
           "sessions": {t: sessions[t][1] for t in titles}, "configs": {}}
    for name, by_title in per.items():
        all_jobs = [j for jobs in by_title.values() for j in jobs]
        c = totals(all_jobs)
        lines = c["jobs"]
        c["ocr_per_line"] = round(c["ocr_jobs"] / lines, 4) if lines else None
        c["recognized_per_line"] = round(c["recognized_lines"] / lines, 4) if lines else None
        # titles whose source was ever trusted, and the first page it was trusted at
        firsts = [next((j["page"] for j in jobs if j["flags"] & 0x100), None) for jobs in by_title.values()]
        c["sessions_trusted"] = sum(f is not None for f in firsts)
        c["first_trusted_page_median"] = statistics.median([f for f in firsts if f is not None]) if any(
            f is not None for f in firsts) else None
        out["configs"][name] = c
    Path(args.output).write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8")
    for name, c in out["configs"].items():
        print(name, json.dumps({k: c[k] for k in ("jobs", "ocr_jobs", "ocr_per_line", "recognized_per_line",
                                                   "weights_read", "trusted_answers", "trusted_wrong", "comparable_lines", "shown_matches_hook",
                                                   "held_after_mean", "peak_bytes_mean", "host_us_median",
                                                   "sessions_trusted", "failed")}))


if __name__ == "__main__":
    sys.exit(main())
