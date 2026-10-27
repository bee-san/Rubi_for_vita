#!/usr/bin/env python3
"""When the open overlay's wait hint (#358, config.ini ocr_wait_hint) would show, replayed on console logs and on the
host spinner bench's job timelines, for each N of torn jobs in a row (shell/worker.c WAIT_HINT_AFTER is 2):

    python3 tools/wait_hint_replay.py [--logs LOG ...] [--bench RESULTS.jsonl] [--n 1,2,3] [-o OUT.json]

The rule, as shell/worker.c start_job_kind applies it: as the open overlay starts a try at its text, it says why it
still is recognizing once the last N jobs in a row (the press's tries, or Automatically's before it) ended
VJO_E_SCREEN_CHANGED (rc -120: the text changed under every read).

--logs: a debug build's log.txt files in order (log.old.txt first), the jobs of kernel API 20 and later (the
stable-text wait's builds): the openings that waited for a recognition and whether any of their jobs was torn; and the
torn jobs' runs in a row, as a wait through each would meet them (a run of N or more shows the hint, which the next
job's result then replaces: the time it would be shown).
--bench: host/spinner_bench.c's results (vjo-spinner-bench, #353; one JSON object a region and scenario, with its
job_log): for the runs that never show text, when the hint shows after L + R; for those that show text after a wait,
how often the hint came first, and how long before the text. The bench's times are estimates.

Counts and times only: the logs' and the bench's texts stay where they are (docs/text-stability.md,
"Why the overlay waits")."""
import argparse
import json
import re
import statistics
import sys

TORN = -120  # VJO_E_SCREEN_CHANGED
CANCELLED = -118  # VJO_E_CANCELLED: the Shell's count of torn jobs neither grows nor ends (on_job_done)
API = re.compile(r"shell started \(kernel API (\d+)\)")
START = re.compile(r"^job start \((.*?)\)")
DONE = re.compile(r"^job \d+ done rc=(-?\d+) in (\d+) ms")
WAITING = ("no background result", "screen changed since the background result", "background job still running")


def spread(xs):
    """median, 90th percentile and largest of xs (None for none)."""
    xs = sorted(xs)
    return [statistics.median(xs), xs[int(0.9 * (len(xs) - 1))], xs[-1]] if xs else None


def read_logs(paths):
    """The jobs (why, rc, ms) and the openings that waited for a recognition (their jobs' rcs) of API 20 and later."""
    jobs, opens, api, cur, job = [], [], 0, None, None
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                m = API.search(line)
                if m:
                    api, cur, job = int(m.group(1)), None, None
                    jobs.append(None)  # a restart: no run of torn jobs goes across it
                    continue
                if api < 20:
                    continue
                if line.startswith("open: "):
                    why = line[6:].split(" (")[0].strip()
                    cur = {"rcs": []} if why in WAITING else None
                    if cur is not None:
                        opens.append(cur)
                    continue
                if line.startswith("overlay closed:"):
                    cur = None
                    continue
                m = START.match(line)
                if m:
                    job = {"why": m.group(1)}
                    continue
                m = DONE.match(line)
                if m and job is not None:
                    job.update(rc=int(m.group(1)), ms=int(m.group(2)))
                    jobs.append(job)
                    job = None
                    if cur is not None:
                        cur["rcs"].append(int(m.group(1)))
                        if int(m.group(1)) != TORN:
                            cur = None  # a result: the wait ended
    return jobs, opens


def log_replay(jobs, opens, n):
    """A wait through each run of torn jobs: does it reach n, and for how long the next job's result keeps the hint."""
    runs, run = [], []
    for j in jobs + [None]:
        if j is not None and j["rc"] == TORN:
            run.append(j)
            continue
        if j is not None and j["rc"] == CANCELLED:
            continue
        if run:
            runs.append((run, j))
        run = []
    shown = [nxt["ms"] for r, nxt in runs if len(r) >= n and nxt is not None]
    return {"torn_jobs": sum(len(r) for r, _ in runs), "runs": len(runs),
            "runs_by_length": {str(k): sum(len(r) == k for r, _ in runs) for k in sorted({len(r) for r, _ in runs})},
            "hint_shown": sum(len(r) >= n for r, _ in runs),
            "until_the_next_result_ms": spread(shown),
            "waits": len(opens), "waits_with_a_torn_job": sum(TORN in o["rcs"] for o in opens),
            "waits_hint_shown": sum(_streak(o["rcs"]) >= n for o in opens)}


def _streak(rcs):
    best = run = 0
    for rc in rcs:
        if rc != CANCELLED:
            run = run + 1 if rc == TORN else 0
        best = max(best, run)
    return best


def bench_hint(r, n):
    """ms after L + R when the hint shows in this run (None: never, or the text came first)."""
    press, streak = r["press_ms"], 0
    for j in sorted(r["job_log"], key=lambda j: j["at_ms"]):
        if j["at_ms"] >= press and j["kind"] == "press":
            wait = j["at_ms"] - press
            if r["shown"] and 0 <= r["show_ms"] <= wait:
                return None
            if streak >= n:
                return wait
        if j["rc"] != CANCELLED and not j.get("cancelled"):
            streak = streak + 1 if j["rc"] == TORN else 0
    return None


def bench_replay(rows, n):
    never = [r for r in rows if not r["shown"]]
    hints = [h for h in (bench_hint(r, n) for r in never) if h is not None]
    waited = [r for r in rows if r["shown"] and r["show_ms"] > 0]
    before = [r["show_ms"] - h for r in waited for h in [bench_hint(r, n)] if h is not None]
    return {"never_shown": len(never), "never_shown_torn": sum(any(j["rc"] == TORN for j in r["job_log"]) for r in never),
            "hint_shown": len(hints), "after_press_ms": spread(hints),
            "shown_after_a_wait": len(waited), "hint_first": len(before), "hint_first_under_1s": sum(b < 1000 for b in before),
            "hint_before_text_ms": spread(before)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--logs", nargs="*", default=[], help="debug log.txt files, in order (log.old.txt first)")
    ap.add_argument("--bench", help="host/spinner_bench.c's results (JSONL)")
    ap.add_argument("--n", default="1,2,3", help="torn jobs in a row to replay (comma-separated)")
    ap.add_argument("-o", "--out", help="write the JSON here (default: stdout)")
    a = ap.parse_args(argv)
    ns = [int(x) for x in a.n.split(",")]
    out = {"tool": "tools/wait_hint_replay.py", "n": ns}
    if a.logs:
        jobs, opens = read_logs(a.logs)
        out["logs"] = {str(n): log_replay(jobs, opens, n) for n in ns}
    if a.bench:
        with open(a.bench, encoding="utf-8") as f:
            rows = [json.loads(l) for l in f if l.strip()]
        out["bench"] = {"runs": len(rows), **{str(n): bench_replay(rows, n) for n in ns}}
    text = json.dumps(out, indent=1, sort_keys=True)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
