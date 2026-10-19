#!/usr/bin/env python3
"""The stutter replay model (docs/stutter.md): a collect's OCR jobs replayed on the host through the core's own
stable-text wait (`vjo-settle-bench --replay`, core/vocr_ocr.c), back to back as the console ran them and paced, and
the late game frames each would cost.

    python3 tools/stutter_model.py --collect DIR [--bench build/host/vjo-settle-bench] [--pace 150] [--json]

What is measured, what is replayed and what is estimated:
- measured (the collect's log, the debug build's lines): each job's strips (the frames whose display call copied rows of
  the region), its wait's passes, time and verdict, its frames and its late ones (the kernel's pace step);
- replayed (on the host, with the core's code): each job that waited, as a box whose rows take a new state every frame
  until the start of the last pass of the wait that saw a change, then hold still (a wait that held), or for the whole
  wait (one that ran out), at the job's own frame rate. The replay at pace 0 is checked against the measured passes
  (`replay at pace 0`); a job whose wait was only the check after the finder (settle_verify, which already sleeps
  until a pass is due) is kept as measured, and so is every job the pace does not apply to: a press's (the overlay
  waits for it: shell/worker.c paces Automatically's, the subtitle strip's and the open overlay's refreshes only) and
  the self-test's;
- estimated: the late frames at the replayed strips, at each job's own measured late frames a strip. What setting the
  frame before the copy and the balanced strips change (each strip's cost) is not modelled: the estimate counts only
  the strips the pace leaves out.
Counts only: no text leaves the log."""
import argparse
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import analyse_selftest  # noqa: E402  (the analyser's own log reader: one parser for both tools)

SETTLE_US = 150000  # ocr_settle_ms's default: the model replays the wait the console ran
WINDOW = 20  # jobs in a rolling window of the late share (the stutter switch's window, docs/stutter.md)


def collect_logs(folder):
    """The collect's log.old.txt then log.txt (each earlier log is a byte prefix of the next collect's: one per job)."""
    d = Path(folder) / "ux0" / "data" / "VitaJPOverlay"
    texts = [p.read_text(encoding="utf-8", errors="replace") for p in (d / "log.old.txt", d / "log.txt") if p.exists()]
    if not texts:
        raise SystemExit(f"stutter_model: no log.txt in {d}")
    return "\n".join(texts)


def jobs_of(text):
    su = analyse_selftest.summarise_log(text).get("stutter")
    return su["jobs_rows"] if su else []


PACED = ("auto", "refresh", "subtitles")  # shell/worker.c paces the jobs nobody waits for, not a press's or the self-test's


def replay_input(j):
    """The bench's line for a job that waited ("W H FPS FIRST ANIM_MS"), or None to keep it as measured."""
    if j["kind"] not in PACED:
        return None
    if not (j["strips"] and j["strip_rows"] and j["passes"] and j["wait_us"] and j["ms"] and j["frames"]):
        return None
    per_pass = -(-j["h"] // j["strip_rows"])
    fps = max(20, min(60, round(j["frames"] * 1000 / j["ms"])))
    pass_us = per_pass * 1e6 / fps
    if j["wait_us"] < SETTLE_US + 2 * pass_us and j["passes"] <= 2:
        return None  # only the check after the finder: it sleeps until a pass is due already
    if j["wait_rc"] == 2:  # the limit: the rows never held still
        anim_ms = j["wait_us"] // 1000
    else:  # held: the last pass that saw a change started this long into the wait (back to back)
        anim_ms = max(0, int((j["wait_us"] - (1 + math.ceil(SETTLE_US / pass_us)) * pass_us) / 1000))
    return j["w"], j["h"], fps, anim_ms


def run_bench(bench, lines, pace_ms):
    text = "".join(f"{w} {h} {fps} 1 {anim} {pace_ms}\n" for w, h, fps, anim in lines)
    out = subprocess.run([str(bench), "--replay", str(SETTLE_US // 1000)], input=text, capture_output=True, text=True,
                         timeout=600, check=True).stdout
    return [json.loads(x) for x in out.splitlines() if x.strip()]


def model(rows, bench, pace_ms):
    todo = [(i, replay_input(j)) for i, j in enumerate(rows)]
    todo = [(i, x) for i, x in todo if x]
    zero = run_bench(bench, [x for _, x in todo], 0)
    paced = run_bench(bench, [x for _, x in todo], pace_ms)
    per = []
    for j in rows:
        per.append({"kind": j["kind"], "frames": j["frames"], "slow": j["slow"], "strips": j["strips"] or 0,
                    "strips_paced": j["strips"] or 0, "slow_paced": j["slow"], "frames_paced": j["frames"],
                    "wait_passes": j["passes"] or 0, "replayed": False})
    checks = []
    for (i, (_w, h, fps, _a)), z, p in zip(todo, zero, paced):
        j, r = rows[i], per[i]
        per_pass = -(-h // j["strip_rows"])
        wait_strips = j["passes"] * per_pass
        other = max(0, j["strips"] - wait_strips)
        strips_paced = other + p["settle_passes"] * per_pass
        rate = j["slow"] / j["strips"] if j["strips"] else 0.0
        dms = p["ms"] - z["ms"]
        r.update(replayed=True, strips_paced=strips_paced, slow_paced=rate * strips_paced,
                 frames_paced=max(1, j["frames"] + round(dms * fps / 1000)), dms=dms,
                 passes_zero=z["settle_passes"], passes_paced=p["settle_passes"])
        checks.append((j["passes"], z["settle_passes"]))
    return per, checks


def rolling_max(per, key_slow, key_frames, n=WINDOW):
    best = None
    for k in range(0, max(0, len(per) - n + 1)):
        f = sum(p[key_frames] for p in per[k:k + n])
        s = sum(p[key_slow] for p in per[k:k + n])
        if f:
            best = max(best or 0.0, 100 * s / f)
    return best


def summary(per, checks, pace_ms):
    def tot(key):
        return sum(p[key] for p in per)
    rep = [p for p in per if p["replayed"]]
    meas, zero = [c[0] for c in checks], [c[1] for c in checks]
    err = [abs(a - b) / a for a, b in checks if a]
    dms = sorted(p["dms"] for p in rep)
    kinds = {}
    for p in per:
        k = kinds.setdefault(p["kind"] or "unknown", {"jobs": 0, "frames": 0, "slow": 0, "strips": 0, "strips_paced": 0,
                                                       "slow_paced": 0.0, "frames_paced": 0})
        for key in ("frames", "slow", "strips", "strips_paced", "slow_paced", "frames_paced"):
            k[key] += p[key]
        k["jobs"] += 1
    return {
        "pace_ms": pace_ms, "jobs": len(per), "replayed": len(rep),
        "replay_check": {"measured_passes": sum(meas), "replayed_passes_at_0": sum(zero),
                         "median_error": statistics.median(err) if err else None},
        "strips": tot("strips"), "strips_paced": tot("strips_paced"),
        "frames": tot("frames"), "frames_paced": tot("frames_paced"),
        "slow": tot("slow"), "slow_paced": round(tot("slow_paced")),
        "wait_ms_change": {"median": statistics.median(dms) if dms else None, "min": dms[0] if dms else None,
                           "max": dms[-1] if dms else None},
        "window_max_share": rolling_max(per, "slow", "frames"),
        "window_max_share_paced": rolling_max(per, "slow_paced", "frames_paced"),
        "kinds": kinds,
    }


def render(s):
    pct = lambda a, b: f"{100 * a / b:.1f}%" if b else "-"  # noqa: E731
    rc = s["replay_check"]
    out = [f"jobs with frame pacing: {s['jobs']}; replayed (they waited): {s['replayed']}",
           f"replay at pace 0: {rc['replayed_passes_at_0']} passes against {rc['measured_passes']} measured "
           f"(median error {100 * rc['median_error']:.0f}% a job)" if rc["median_error"] is not None else
           "replay at pace 0: no job to check",
           f"strips (frames with a copy): {s['strips']} measured, {s['strips_paced']} at ocr_settle_pace_ms = {s['pace_ms']} "
           f"({pct(s['strips_paced'] - s['strips'], s['strips'])})",
           f"late frames during the jobs: {s['slow']} of {s['frames']} ({pct(s['slow'], s['frames'])}) measured; "
           f"est. {s['slow_paced']} of {s['frames_paced']} ({pct(s['slow_paced'], s['frames_paced'])}) paced",
           f"the wait's time a replayed job: median {s['wait_ms_change']['median']:+} ms "
           f"(from {s['wait_ms_change']['min']:+} to {s['wait_ms_change']['max']:+})"
           if s["wait_ms_change"]["median"] is not None else "the wait's time: no replayed job",
           f"the worst {WINDOW} jobs in a row: {s['window_max_share']:.1f}% late measured, est. "
           f"{s['window_max_share_paced']:.1f}% paced" if s["window_max_share"] is not None else
           f"fewer than {WINDOW} jobs: no rolling window"]
    for k, v in s["kinds"].items():
        out.append(f"  {k}: {v['jobs']} jobs, {v['slow']} of {v['frames']} late ({pct(v['slow'], v['frames'])}) measured, "
                   f"est. {round(v['slow_paced'])} of {v['frames_paced']} ({pct(v['slow_paced'], v['frames_paced'])}); "
                   f"strips {v['strips']} -> {v['strips_paced']}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--collect", action="append", required=True, help="a collect's folder (repeatable)")
    ap.add_argument("--bench", default=str(ROOT / "build/host/vjo-settle-bench"))
    ap.add_argument("--pace", type=int, default=150, help="ocr_settle_pace_ms to model (default 150)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    if not Path(args.bench).exists():
        raise SystemExit(f"stutter_model: no {args.bench}: build the host tree first (docs/guide.md)")
    rows = []
    for c in args.collect:
        rows += jobs_of(collect_logs(c))
    if not rows:
        raise SystemExit("stutter_model: no job with frame pacing (a debug build of API 22 or later) in the logs")
    per, checks = model(rows, args.bench, args.pace)
    s = summary(per, checks, args.pace)
    print(json.dumps(s, indent=1) if args.json else render(s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
