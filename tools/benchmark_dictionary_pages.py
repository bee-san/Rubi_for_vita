#!/usr/bin/env python3
"""Selected-word lookup and dictionary-page cost on the host, before vs after.

For each case, runs vjo-local-bench in cursor mode with the baseline build
(one result) and this build with local_dictionary_results 1 and N, interleaved
in rounds so host load drift affects all three alike. The shown first entry
must be identical in all three, and one result must read exactly as before.
Application caches start empty on every request; the host OS file cache is
warm. Host measurements only: they do not establish Vita or SD-card latency.
"""
import argparse
import json
from pathlib import Path
import platform
import statistics
import subprocess

# Selected positions from docs/benchmarks/local-character-memory-20261011.json.
CASES = [
    ("日本語を勉強しています。", 0),
    ("今日は学校に行きました。", 9),
    ("猫は食べました。行った。", 6),
    ("彼女は僕のことを何も知らなかった。", 9),
    ("私たちは明日、東京で友達に会う予定です。", 21),
    ("ありがとう。", 0),
    ("これはとても長い説明ですが、同じ言葉を何度も繰り返しても辞書全部がメモリに読み込まれるわけではありません。", 36),
    ("猫。", 3),
    ("猫\n猫は食べました。", 4),
]


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int(fraction * len(values)))]


def run_rows(binary, directory, names, text, cursor, runs, results=None):
    args = [str(binary.resolve()), str(directory), names, text, str(runs + 1), str(cursor)]
    if results:
        args.append(str(results))
    proc = subprocess.run(args, capture_output=True, text=True, check=True, timeout=600)
    return [json.loads(line) for line in proc.stdout.splitlines()][1:]  # first: OS cache warmup


def summarize(rows, results=None):
    if len({r["digest"] for r in rows}) != 1:
        raise RuntimeError("nondeterministic lookup result")
    last = rows[-1]
    summary = {"median_us": round(statistics.median(r["lookup_us"] for r in rows), 3),
               "p95_us": percentile([r["lookup_us"] for r in rows], 0.95),
               "read_calls": last["read_calls"], "read_bytes": last["read_bytes"],
               "arena_peak": max(r["arena_peak"] for r in rows), "entries": last["entries"],
               "digest": last["digest"]}
    if results:
        summary.update(resident=last["resident"], page_count=last["page_count"], pages=[])
        for index in range(last["page_count"] - 1):
            samples = [r["pages"][index] for r in rows]
            if len({s["digest"] for s in samples}) != 1:
                raise RuntimeError("nondeterministic page result")
            summary["pages"].append({
                "page": index + 1,
                "median_us": round(statistics.median(s["lookup_us"] for s in samples), 3),
                "read_calls": samples[-1]["read_calls"], "read_bytes": samples[-1]["read_bytes"],
                "files_opened": samples[-1]["files_opened"],
                "arena_peak": max(s["arena_peak"] for s in samples),
                "resident": max(s["resident"] for s in samples)})
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True, help="baseline vjo-local-bench")
    parser.add_argument("--after", type=Path, required=True, help="this build's vjo-local-bench")
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--dicts", required=True, help="comma-separated .vjdict files in priority order")
    parser.add_argument("--runs", type=int, default=20, help="timed requests per variant and round")
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()
    if args.runs < 1 or args.rounds < 1:
        parser.error("runs and rounds must be positive")
    count = len(args.dicts.split(","))
    rows = []
    for text, cursor in CASES:
        samples = {"before": [], "single": [], "paged": []}
        for _ in range(args.rounds):
            samples["before"] += run_rows(args.before, args.directory, args.dicts, text, cursor, args.runs)
            samples["single"] += run_rows(args.after, args.directory, args.dicts, text, cursor, args.runs, 1)
            samples["paged"] += run_rows(args.after, args.directory, args.dicts, text, cursor, args.runs, count)
        before, single = summarize(samples["before"]), summarize(samples["single"], 1)
        paged = summarize(samples["paged"], count)
        if not (before["digest"] == single["digest"] == paged["digest"]):
            raise RuntimeError(f"shown entry changed at {text!r} byte {cursor}")
        if before["read_calls"] != single["read_calls"] or before["arena_peak"] != single["arena_peak"]:
            raise RuntimeError(f"one-result lookup changed at {text!r} byte {cursor}: {before} vs {single}")
        rows.append({"text": text, "cursor_byte": cursor, "before": before,
                     "after_results_1": single, f"after_results_{count}": paged})
    print(json.dumps({
        "method": "host vjo-local-bench, cursor mode; empty application caches per request; "
                  "warm host OS file cache; first request discarded; not Vita hardware",
        "environment": f"{platform.system()} {platform.machine()} {platform.python_implementation()}",
        "timed_requests_per_variant": args.runs * args.rounds, "interleaved_rounds": args.rounds,
        "dictionaries": args.dicts.split(","),
        "cases": rows}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
