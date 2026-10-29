#!/usr/bin/env python3
"""Per-stage summary of vjo-dict-stage-bench's output, with the console model.

  python3 tools/dict_stage_report.py taps.jsonl [--before other.jsonl] [--json out.json]
      [--read-ms 2.98] [--fixed-ms 13.1] [--cpu-factor 10]

Prints, over every tap: reads per lookup by what they fetch, bytes, distinct 4 KiB pages, the
candidates generated/searched/found, the host microseconds by stage (CPU with the host's warm file
cache), and the console estimate: fixed + read-ms x reads + cpu-factor x host CPU, the fit of the
owner's console logs (docs/lookup-performance.md, "the test.4 fit": v0.8.0-test.4, 338 cursor
lookups, 13.1 ms + 2.98 ms a read, R^2 0.73; test.2: 11.2 + 3.76; CPU about 10x the host's;
docs/performance.md's older fit, test.1, is 15.3 + 3.82).
--before compares two runs tap by tap: every digest must match (identical results), and the
reads/time deltas are printed. Exit 1 on a digest difference.
"""
import argparse
import json
import statistics
import sys

READ_KINDS = ("header", "filter", "index", "group", "head", "payload")
NS_KINDS = ("deinflect", "search", "rank", "entry", "page")


def load(path):
    taps, summary = [], None
    with open(path, encoding="utf-8") as f:
        for line in f:
            row = json.loads(line)
            if "summary" in row:
                summary = row["summary"]
            else:
                taps.append(row)
    return taps, summary


def pct(values, q):
    values = sorted(values)
    if not values:
        return 0.0
    k = (len(values) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def describe(taps, args):
    n = len(taps)
    out = {"taps": n}
    reads = [t["reads"] for t in taps]
    out["reads"] = {"mean": statistics.mean(reads), "p50": pct(reads, 0.5), "p95": pct(reads, 0.95), "max": max(reads)}
    out["reads_by"] = {k: statistics.mean(t["reads_by"][k] for t in taps) for k in READ_KINDS}
    out["bytes"] = {"mean": statistics.mean(t["bytes"] for t in taps), "p95": pct([t["bytes"] for t in taps], 0.95)}
    out["pages"] = statistics.mean(t["pages"] for t in taps)
    out["opens"] = sum(t["opens"] for t in taps)
    for k in ("spans", "candidates", "probes", "filtered", "miss_cached", "hash_present", "found", "ranked", "hinted",
              "absent_reads"):
        out[k] = statistics.mean(t[k] for t in taps)
    out["entries"] = sum(1 for t in taps if t["entries"]) / n
    out["more"] = statistics.mean(t["more"] for t in taps)
    out["entry_bytes"] = statistics.mean(t["entry_bytes"] for t in taps if t["entries"]) if any(t["entries"] for t in taps) else 0
    out["us"] = {"mean": statistics.mean(t["us"] for t in taps), "p50": pct([t["us"] for t in taps], 0.5),
                 "p95": pct([t["us"] for t in taps], 0.95)}
    out["us_by"] = {k: statistics.mean(t["ns_by"][k] for t in taps) / 1000.0 for k in NS_KINDS}
    out["us_styled"] = statistics.mean(t["us_styled"] for t in taps)
    out["prepare_reads"] = statistics.mean(t["prepare"]["reads"] for t in taps)
    out["arena_peak"] = max(t["arena_peak"] for t in taps)
    # console model per tap
    est = [args.fixed_ms + args.read_ms * t["reads"] + args.cpu_factor * t["us"] / 1000.0 for t in taps]
    out["console_ms"] = {"mean": statistics.mean(est), "p50": pct(est, 0.5), "p95": pct(est, 0.95), "max": max(est)}
    # the stages' share of the console estimate: reads by stage x read_ms + their cpu
    stage_reads = {
        "deinflect": 0.0,
        "search (filter+index+group+rank heads)": out["reads_by"]["header"] + out["reads_by"]["filter"] + out["reads_by"]["index"] +
        out["reads_by"]["group"] + out["reads_by"]["head"],
        "entry (shown payload)": out["reads_by"]["payload"] - out["more"],
        "stack (other dictionaries' entries)": out["more"],
    }
    stage_cpu = {
        "deinflect": out["us_by"]["deinflect"],
        "search (filter+index+group+rank heads)": out["us_by"]["search"],
        "entry (shown payload)": out["us_by"]["entry"],
        "stack (other dictionaries' entries)": out["us_by"]["page"],
    }
    out["stage_console_ms"] = {k: args.read_ms * stage_reads[k] + args.cpu_factor * stage_cpu[k] / 1000.0 for k in stage_reads}
    out["stage_console_ms"]["popup text (styled)"] = args.cpu_factor * out["us_styled"] / 1000.0
    out["stage_console_ms"]["fixed (thread hops, open/seek, log)"] = args.fixed_ms
    return out


def fmt_table(d, title):
    print(f"\n## {title}: {d['taps']:,} taps")
    print(f"reads/lookup mean {d['reads']['mean']:.2f} p50 {d['reads']['p50']:.0f} p95 {d['reads']['p95']:.0f} max {d['reads']['max']}"
          f"; distinct 4 KiB pages {d['pages']:.2f}; bytes mean {d['bytes']['mean']:.0f} p95 {d['bytes']['p95']:.0f}; opens total {d['opens']}")
    print("reads by kind: " + ", ".join(f"{k} {d['reads_by'][k]:.2f}" for k in READ_KINDS))
    print(f"spans {d['spans']:.2f} candidates {d['candidates']:.2f} probes {d['probes']:.2f} filtered {d['filtered']:.2f} "
          f"miss_cached {d['miss_cached']:.2f} hash_present {d['hash_present']:.2f} found {d['found']:.2f} "
          f"ranked(heads) {d['ranked']:.2f} hinted {d['hinted']:.2f} absent_reads {d['absent_reads']:.2f}")
    print(f"taps with an entry {100 * d['entries']:.1f}%; stacked entries/lookup {d['more']:.2f}; entry text {d['entry_bytes']:.0f} B; "
          f"prepare reads/lookup {d['prepare_reads']:.2f}; arena peak {d['arena_peak']} B")
    print(f"host us/lookup mean {d['us']['mean']:.1f} p50 {d['us']['p50']:.1f} p95 {d['us']['p95']:.1f}; by stage (us): " +
          ", ".join(f"{k} {d['us_by'][k]:.1f}" for k in NS_KINDS) + f"; popup styled {d['us_styled']:.1f}")
    print(f"console estimate ms/lookup mean {d['console_ms']['mean']:.1f} p50 {d['console_ms']['p50']:.1f} "
          f"p95 {d['console_ms']['p95']:.1f} max {d['console_ms']['max']:.1f}")
    print("console estimate by stage (ms, mean): " + "; ".join(f"{k} {v:.1f}" for k, v in d["stage_console_ms"].items()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("after")
    ap.add_argument("--before")
    ap.add_argument("--json")
    ap.add_argument("--read-ms", type=float, default=2.98)
    ap.add_argument("--fixed-ms", type=float, default=13.1)
    ap.add_argument("--cpu-factor", type=float, default=10.0)
    ap.add_argument("--allow-different", action="store_true", help="do not fail when results differ (report only)")
    args = ap.parse_args()
    after, summary = load(args.after)
    result = {"after": describe(after, args), "summary": summary, "model": {"fixed_ms": args.fixed_ms, "read_ms": args.read_ms,
                                                                            "cpu_factor": args.cpu_factor}}
    fmt_table(result["after"], f"after ({args.after})")
    rc = 0
    if args.before:
        before, bsummary = load(args.before)
        result["before"] = describe(before, args)
        result["before_summary"] = bsummary
        fmt_table(result["before"], f"before ({args.before})")
        if len(before) != len(after):
            print(f"\nDIFFERENT: {len(before)} taps before, {len(after)} after", file=sys.stderr)
            rc = 1
        else:
            diffs = [(b, a) for b, a in zip(before, after) if (b["id"], b["tap"], b["digest"]) != (a["id"], a["tap"], a["digest"])]
            result["different_taps"] = len(diffs)
            if diffs:
                print(f"\nDIFFERENT RESULTS at {len(diffs)} of {len(after)} taps; the first: " +
                      "; ".join(f"{b['id']} tap {b['tap']}" for b, a in diffs[:5]), file=sys.stderr)
                rc = 1
            else:
                print(f"\nIDENTICAL RESULTS at every one of the {len(after)} taps")
        b, a = result["before"], result["after"]
        print(f"reads/lookup {b['reads']['mean']:.2f} -> {a['reads']['mean']:.2f} ({a['reads']['mean'] - b['reads']['mean']:+.2f}); "
              f"console est. p50 {b['console_ms']['p50']:.1f} -> {a['console_ms']['p50']:.1f} ms, mean {b['console_ms']['mean']:.1f} -> "
              f"{a['console_ms']['mean']:.1f} ms; host us p50 {b['us']['p50']:.1f} -> {a['us']['p50']:.1f}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1, ensure_ascii=False)
    return 0 if args.allow_different else rc


if __name__ == "__main__":
    sys.exit(main())
