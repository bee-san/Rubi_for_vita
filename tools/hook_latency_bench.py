#!/usr/bin/env python3
"""The trusted-hook latency bench's driver (docs/native-hooks.md, "No wait for a trusted line"): each title of the
hooked page pool (vita-vn-ocr's pagepool/v1) is one session, its first N pages' hook texts in page order, replayed
through host/hook_latency_bench.c for each scenario and each build given; prints the latencies from a line's hook
write (ms) pooled over the sessions, and the worker loop's rounds a second and host time a round.

    python3 tools/hook_latency_bench.py --bench before=PATH --bench after=PATH [--bench NAME=PATH ...] \\
        --pagepool DIR [--pages 300] [--titles T ...] [--scenarios reading quick ...] [--jobs 5] -o OUT.json

A session's first line is left out of the pooled figures (the worker's first rounds and the empty box's job before
it) and reported apart, as the bench's first_line_ready_ms; so is a page whose text the stream already holds (no hook
event: repeats). Texts stay local: OUT.json has figures only. Host
execution: no console timing (docs/offline-testing.md); the first word's lookup is the bench's constant (70 ms, the
console's fit).
"""
import argparse
import json
import multiprocessing
import statistics
import subprocess
import sys
from pathlib import Path

SCENARIOS = ["reading", "quick", "subtitles", "open", "site", "grow"]
# Left out unless named with --titles: titles that share a release with CLANNAD or vita-test, and the pool's test title
# (docs/native-hooks.md, the trust bench's sessions; #338's audit N8: the default took all of the pool's titles)
LEFT_OUT = ("PCSG00007", "PCSG00807", "PCSG00866")


def session_text(pagepool, title, limit):
    rows = [json.loads(line) for line in open(Path(pagepool) / f"{title}.jsonl", encoding="utf-8")]
    rows = [r for r in rows if r.get("text")]
    rows.sort(key=lambda r: r["page"])
    rows = rows[:limit] if limit else rows
    lines = ["".join(r["text"].split("\n")).strip() for r in rows]
    return "".join(f"{t}\n" for t in lines if t)


def run(args):
    name, bench, scenario, title, text, extra = args
    p = subprocess.run([bench, scenario, *extra], input=text.encode("utf-8"), capture_output=True, timeout=3600)
    if p.returncode:
        return name, scenario, title, None, p.stderr.decode("utf-8", "replace")[-400:]
    out = [json.loads(line) for line in p.stdout.decode("utf-8").splitlines() if line.strip()]
    return name, scenario, title, out, ""


def pooled(values):
    if not values:
        return None
    v = sorted(values)
    return {"n": len(v), "mean": round(statistics.fmean(v), 1), "p50": round(v[len(v) // 2], 1),
            "p95": round(v[min(len(v) - 1, (len(v) * 95) // 100)], 1), "max": round(v[-1], 1)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", action="append", required=True, help="NAME=PATH of a vjo-hook-latency-bench build")
    ap.add_argument("--pagepool", required=True)
    ap.add_argument("--pages", type=int, default=300)
    ap.add_argument("--titles", nargs="*")
    ap.add_argument("--scenarios", nargs="*", default=SCENARIOS)
    ap.add_argument("--lookup-ms", type=int, default=70)
    ap.add_argument("--jobs", type=int, default=5)
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    titles = a.titles or sorted(p.stem for p in Path(a.pagepool).glob("PCSG*.jsonl") if p.stem not in LEFT_OUT)
    texts = {t: session_text(a.pagepool, t, a.pages) for t in titles}
    titles = [t for t in titles if texts[t]]
    benches = [b.split("=", 1) for b in a.bench]
    extra = ["--lookup-ms", str(a.lookup_ms)]
    work = [(n, p, s, t, texts[t], extra) for n, p in benches for s in a.scenarios for t in titles]
    with multiprocessing.Pool(a.jobs) as pool:
        results = pool.map(run, work, chunksize=1)
    report = {"titles": titles, "pages": a.pages, "lookup_ms": a.lookup_ms, "configs": {}}
    for name, scenario, title, out, err in results:
        key = f"{name}:{scenario}"
        c = report["configs"].setdefault(key, {"sessions": 0, "failed": [], "ready": [], "text": [], "first_text": [],
                                               "report": [], "ready_last": [], "first_line_ready": [], "not_ready": 0, "lines": 0,
                                               "rounds_per_s": [], "host_ns_per_round": [], "shell_jobs": 0, "repeats": 0})
        if out is None:
            c["failed"].append({"title": title, "stderr": err})
            continue
        c["sessions"] += 1
        summary = out[-1]
        for row in out[:-1]:
            if row["line"] == 0:
                continue
            if row.get("repeat"):
                c["repeats"] += 1
                continue
            c["lines"] += 1
            if row["ready_ms"] >= 0:
                c["ready"].append(row["ready_ms"])
            else:
                c["not_ready"] += 1
            if row["text_ms"] >= 0:
                c["text"].append(row["text_ms"])
            if row["first_ms"] >= 0:
                c["first_text"].append(row["first_ms"])
            if row["report_ms"] >= 0:
                c["report"].append(row["report_ms"])
            if row.get("ready_last_ms", -1) >= 0:
                c["ready_last"].append(row["ready_last_ms"])
        if summary.get("first_line_ready_ms", -1) >= 0:
            c["first_line_ready"].append(summary["first_line_ready_ms"])
        c["rounds_per_s"].append(summary["rounds_per_s"])
        c["host_ns_per_round"].append(summary["worker_host_ns_per_round"])
        c["shell_jobs"] += summary["shell_jobs"]
        c["api"] = summary["api"]
        c["watch_us"] = summary["watch_us"]
    for key, c in report["configs"].items():
        for k in ("ready", "text", "first_text", "report", "ready_last", "first_line_ready"):
            c[k] = pooled(c[k])
        c["rounds_per_s"] = round(statistics.fmean(c["rounds_per_s"]), 2) if c["rounds_per_s"] else None
        c["host_ns_per_round"] = round(statistics.fmean(c["host_ns_per_round"])) if c["host_ns_per_round"] else None
    Path(a.output).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    for key, c in report["configs"].items():
        r = c["ready"] or {}
        print(f"{key:24} sessions {c['sessions']:2} lines {c['lines']:5} not ready {c['not_ready']:5} "
              f"ready mean {r.get('mean')} p50 {r.get('p50')} p95 {r.get('p95')} max {r.get('max')} "
              f"rounds/s {c['rounds_per_s']} ns/round {c['host_ns_per_round']}", file=sys.stderr)


if __name__ == "__main__":
    main()
