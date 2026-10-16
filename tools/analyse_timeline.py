#!/usr/bin/env python3
"""Per-job, per-stage timeline of the overlay's OCR jobs from a console's logs (docs/first-ocr.md).

    python3 tools/analyse_timeline.py --collect build/device/collect/vjo-collect-STAMP [--jobs] [--json]
    python3 tools/analyse_timeline.py LOG.TXT [--kernel kernel.txt] [--jobs] [--json]
    python3 tools/analyse_timeline.py --compare OLD_COLLECT NEW_COLLECT [--json]

log.txt has no clock of its own: the Shell writes durations ("job 0 done ... in T ms") and the
game worker's step log (`gstep job ...`, game_ocr/main.c step_add) writes for each step either a
duration or the time since the game's job started. This tool puts them on one line per job:

- Instant steps carry the time since the job started (`us`, a timestamp: step_add with us 0):
  mem, check, alloc, free, sig, tear, fb, clock. An `alloc ... arg=weights us=1376964` is an
  allocation made 1.38 s into the job (after its stable-text wait), not one that took 1.38 s; the
  TEAR step is written after the job's tries, so its us is the job's end, and (this build on) its
  bytes say when the last try began.
- Duration steps: pass (the first pass's strip waits, a settle-first wait included), find (the
  last try's finder, its strips included), settle (every stable-text wait of the job), load (the
  weights: block, read, SHA-256, parse; arg 1: read by the warm-up of a job without a line),
  recognize (every line and its LM decoding, the cascade's re-reads not), lm (decoding; its sum
  the LM's read), cascade (re-reads; its sum its model's read), norm, refine, blank, probe, par
  (API 22). pace (API 22, the kernel's) counts the game's frames during the job.
- The game's job ends at its last instant step (the cleanup's frees).

The Shell's side of a job is what the game's log does not cover: its submission and hand-off,
its log lines (each an open/append/close of log.txt and, until it is full, status.txt: 12-46 ms
an append on the console, #81's self-test timestamps) and the lookup. This build's done line also
gives the time spent in exchanges with the game (`game N ms`) and in log appends (`log N ms in K
appends`). The self-test's records (`stage NAME begin/end/wait t=MS`) carry the Shell's clock
(ms since it started): with them a stage splits into the game's job, the rest of its exchange
and the record's own writes.

Sessions: a game process from its `game TITLE started` to the next start; kernel.txt's `game pid`
lines (--collect) count the processes.

The output holds numbers and step names only, never recognized text (a copyrighted game's: the
logs are private). Exit status: 0, or 2 without a log.
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

DATA = "ux0/data/VitaJPOverlay/"

GSTEP = re.compile(r"^gstep (\S+) (\w+) kind=(\S+) arg=(\S+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) sum=0x([0-9A-Fa-f]+) "
                   r"main=(-?\d+) cdram=(-?\d+) phycont=(-?\d+) us=(\d+)")
JOB_LINE = re.compile(r"^game vocr rc=(-?\d+) (\S+?)( \(fallback(, refused too)?\))?: (\d+)x(\d+) at \d+,\d+ of \d+x\d+ "
                      r"fmt 0x[0-9A-Fa-f]+, lines=(\d+) USER workspace=(\d+) weights=(\w+) resident=(\d+), find=(\d+) ms "
                      r"load=(\d+) ms recognize=(\d+) ms frames=(\d+)")
CAPTURE = re.compile(r"^game vocr capture: (.+?) into (\S+), (\d+) B block, (\d+)-row strips, (\d+) strips in (\d+) ms, "
                     r"(\d+) rows changed")
JOB_START = re.compile(r"^job start \((.*)\)(, refresh|, OCR only)?$")
JOB_DONE = re.compile(r"^job (\d+) done rc=(-?\d+) in (\d+) ms(?:, arena peak (\d+))?"
                      r"(?:.*?, game (\d+) ms, log (\d+) ms in (\d+) appends)?")
TEXT_READY = re.compile(r"^subtitle text ready in (\d+) ms")
LOOKUP = re.compile(r"^lookup (first word|cursor|on open|page) at \d+: .*?, (\d+) reads (\d+) B, rc=(-?\d+), (\d+) ms")
GAME_START = re.compile(r"^game (\S+) started: ")
STAGE = re.compile(r"^stage (\w+) (begin|end|wait) (?:rc=(-?\d+) )?t=(\d+)(?: ms=(\d+))?(?: rc=(-?\d+))?")
STEP = re.compile(r"^step (\w+) t=(\d+)")
KPID = re.compile(r"^game pid 0x([0-9A-Fa-f]+) active")

INSTANT = {"mem", "check", "alloc", "free", "sig", "tear", "fb", "clock", "fallback"}
GLM_READ = 0x80000000
VJO_E = {0: "ok", -103: "out of memory", -105: "too large", -108: "source", -115: "ocr model", -116: "ocr region",
         -117: "ocr unavailable", -118: "cancelled", -119: "inference", -120: "screen changed"}
PHASES = ("find_ms", "settle_ms", "load_ms", "lm_load_ms", "recognize_ms", "cascade_ms", "cascade_load_ms",
          "norm_ms", "refine_ms", "blank_ms")


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def rc_name(rc):
    return VJO_E.get(rc, str(rc)) if rc is not None else "-"


def pct(values, q):
    """The q-th percentile (nearest rank) of values; None for none."""
    if not values:
        return None
    v = sorted(values)
    return v[max(0, min(len(v) - 1, round(q / 100 * (len(v) - 1))))]


def span(values):
    if not values:
        return "-"
    return f"{min(values):,.0f}-{max(values):,.0f} (median {statistics.median(values):,.0f})"


def fmt_ms(v):
    return "-" if v is None else f"{v:,.0f}"


class GameJob:
    """One game-worker job: its `game vocr rc=` record, capture line and step log."""

    def __init__(self, m, stage=None):
        self.rc = int(m.group(1))
        self.model = m.group(2)
        self.fallback = bool(m.group(3))
        self.region = f"{m.group(5)}x{m.group(6)}"
        self.lines = int(m.group(7))
        self.weights = m.group(9)
        self.frames = int(m.group(14))
        self.stage = stage
        self.steps = []
        self.capture = None

    def add_step(self, m):
        self.steps.append({"step": m.group(2), "kind": m.group(3), "arg": m.group(4), "rc": int(m.group(5), 16),
                           "bytes": int(m.group(6)), "sum": int(m.group(7), 16), "main": int(m.group(8)),
                           "cdram": int(m.group(9)), "phycont": int(m.group(10)), "us": int(m.group(11))})

    def timeline(self):
        """The job's phases (ms), its instant steps' times (ms since it started) and its end."""
        t = {"end_ms": None, "first_pass_ms": None, "first_pass_strips": None, "find_ms": None, "settle_ms": 0.0,
             "settle": None, "settle_passes": 0, "load_ms": 0.0, "weights": self.weights, "warm": False,
             "lm_load_ms": 0.0, "lm_read": False, "lm_lines": 0, "recognize_ms": 0.0, "cascade_ms": 0.0,
             "cascade_load_ms": 0.0, "cascade_read": False, "norm_ms": 0.0, "refine_ms": 0.0, "blank_ms": 0.0,
             "tears": 0, "tries": 1, "retry_at_ms": None, "allocs": [], "pace": None, "clock": None}
        instants = []
        for s in self.steps:
            name, us = s["step"], s["us"]
            if name in INSTANT:
                instants.append(us)
            if name == "pass" and s["rc"] == 0 and t["first_pass_ms"] is None:
                t["first_pass_ms"] = us / 1000
                t["first_pass_strips"] = int(s["arg"]) if s["arg"].isdigit() else None
            elif name == "find":
                t["find_ms"] = us / 1000
            elif name == "settle":
                t["settle_ms"] = us / 1000 if us > 1 else 0.0
                t["settle"] = {1: "held", 2: "limit"}.get(s["rc"], "none")
                t["settle_passes"] = int(s["arg"])
            elif name == "load":
                t["load_ms"] = us / 1000
                t["warm"] = s["arg"] == "1"
            elif name == "recognize":
                t["recognize_ms"] = us / 1000
            elif name == "lm":
                t["lm_lines"] = s["bytes"] & 0xFFFF
                if s["bytes"] & GLM_READ:
                    t["lm_read"] = True
                    t["lm_load_ms"] = s["sum"] / 1000
            elif name == "cascade":
                t["cascade_ms"] = max(us - 1, 0) / 1000
                if s["bytes"] & GLM_READ:
                    t["cascade_read"] = True
                    t["cascade_load_ms"] = s["sum"] / 1000
            elif name in ("norm", "refine"):
                t[name + "_ms"] = max(us - 1, 0) / 1000
            elif name == "blank":
                t["blank_ms"] = us / 1000
            elif name == "tear":
                t["tears"], t["tries"] = int(s["arg"]), s["rc"]
                if s["bytes"]:
                    t["retry_at_ms"] = s["bytes"] / 1000
            elif name == "alloc":
                t["allocs"].append({"item": s["arg"], "kind": s["kind"], "at_ms": us / 1000, "bytes": s["bytes"],
                                    "ok": s32(s["rc"]) >= 0})
            elif name == "pace":
                t["pace"] = {"frames": int(s["arg"]), "max_gap_ms": s32(s["rc"]) / 1000, "slow": s["sum"]}
            elif name == "clock":
                t["clock"] = {"before": int(s["arg"]), "during": s["bytes"], "after": s["sum"], "rc": s32(s["rc"])}
        t["end_ms"] = max(instants) / 1000 if instants else None
        if t["end_ms"] is not None:
            t["other_ms"] = round(t["end_ms"] - sum(t[k] or 0 for k in PHASES), 1)
        if self.capture:
            t["capture_strips"], t["capture_ms"] = self.capture["strips"], self.capture["ms"]
        return t

    def as_dict(self):
        d = {k: getattr(self, k) for k in ("rc", "model", "fallback", "region", "lines", "weights", "frames", "stage")}
        d["rc_name"] = rc_name(self.rc)
        d["timeline"] = self.timeline()
        return d


class ShellJob:
    """One Shell job, from `job start` to `job N done`."""

    def __init__(self, why, kind, n, session):
        self.why, self.kind, self.n, self.session = why, kind, n, session
        self.done_ms = self.rc = self.text_ready_ms = None
        self.arena_peak = 0
        self.game_ms = self.log_ms = self.appends = None
        self.lookup_ms, self.games, self.stages = [], [], []
        self.lines = self.opened_during = self.text_lines = 0  # text_lines: the shown text's lines (counted only)
        self.cancelled = False

    def as_dict(self):
        game_end = round(sum(g.timeline()["end_ms"] or 0 for g in self.games), 1)
        d = {"n": self.n, "why": self.why, "kind": self.kind, "rc": self.rc, "rc_name": rc_name(self.rc),
             "done_ms": self.done_ms, "text_ready_ms": self.text_ready_ms, "lookup_ms": sum(self.lookup_ms),
             "game_end_ms": game_end, "games": [g.as_dict() for g in self.games], "stages": self.stages,
             "log_lines": self.lines, "overlay_opened_during": self.opened_during, "cancelled": self.cancelled,
             "text_lines": self.text_lines,
             "session": self.session, "selftest": bool(self.stages), "exchange_ms": self.game_ms,
             "log_ms": self.log_ms, "appends": self.appends}
        if self.done_ms is not None:
            # the Shell's own: the job less the game's (its exchanges, or the game's step log) and the lookup
            game = self.game_ms if self.game_ms is not None else game_end
            d["shell_ms"] = round(self.done_ms - game - d["lookup_ms"], 1)
        return d


def parse_log(text):
    sessions, jobs, stages, steps = [], [], [], []
    job, game, stage_now, session = None, None, None, None
    n = 0
    normal = 0  # boots of the normal build, whose short log has no job, step or timing line (docs/release-variants.md)
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if job is not None:
            job.lines += 1
        if line.startswith("Vita JP Overlay shell started"):
            session = None
            normal += "the normal build" in line
            continue
        m = GAME_START.match(line)
        if m:
            session = {"index": len(sessions), "title": m.group(1), "jobs": 0, "opened": 0, "weights_read": [],
                       "lm_read": [], "cascade_read": [], "warm_reads": 0, "first_text_job": None, "first_text_ms": None,
                       "first_open_wait_ms": None}
            sessions.append(session)
            continue
        m = STAGE.match(line)
        if m:
            rec = {"stage": m.group(1), "what": m.group(2), "t": int(m.group(4))}
            if rec["what"] == "end":
                rec["rc"] = int(m.group(3)) if m.group(3) is not None else None
                rec["ms"] = int(m.group(5)) if m.group(5) is not None else None
            elif rec["what"] == "wait":
                rec["rc"] = int(m.group(6)) if m.group(6) is not None else None
            stages.append(rec)
            if job is not None:
                job.stages.append(rec)
            stage_now = rec["stage"] if rec["what"] == "begin" else None
            continue
        m = STEP.match(line)
        if m:
            steps.append({"step": m.group(1), "t": int(m.group(2))})
            continue
        m = JOB_START.match(line)
        if m:
            n += 1
            kind = "refresh" if m.group(2) == ", refresh" else "ocr-only" if m.group(2) else \
                "selftest" if m.group(1) == "self-test" else "ocr"
            job = ShellJob(m.group(1), kind, n, session["index"] if session else None)
            game = None
            continue
        m = JOB_DONE.match(line)
        if m:
            if job is None:  # a lookup (cursor) job logs no start line
                job = ShellJob("lookup", "lookup", n, session["index"] if session else None)
            job.done_ms, job.rc = int(m.group(3)), int(m.group(2))
            job.arena_peak = int(m.group(4) or 0)
            if m.group(5) is not None:
                job.game_ms, job.log_ms, job.appends = int(m.group(5)), int(m.group(6)), int(m.group(7))
            jobs.append(job)
            if session is not None:
                session["jobs"] += 1
            job, game = None, None
            continue
        if line.startswith("input block on"):
            if session is not None:
                session["opened"] += 1
            if job is not None:
                job.opened_during += 1
            continue
        if job is None:
            continue
        if line.startswith(("job cancelled:", "self-test: stopped")):
            job.cancelled = True
        elif line.startswith("game vocr text ") and " shown: " not in line:
            job.text_lines += 1  # the job's text, logged once it is the job's: counted, never kept
        elif (m := TEXT_READY.match(line)):
            job.text_ready_ms = int(m.group(1))
        elif (m := LOOKUP.match(line)):
            job.lookup_ms.append(int(m.group(5)))
        elif (m := JOB_LINE.match(line)):
            game = GameJob(m, stage_now)
            job.games.append(game)
        elif game is not None and (m := CAPTURE.match(line)):
            game.capture = {"strips": int(m.group(5)), "ms": int(m.group(6)), "changed": int(m.group(7))}
        elif game is not None and (m := GSTEP.match(line)) and m.group(1) in ("job", "probe"):
            game.add_step(m)
    for j in jobs:
        if j.session is None:
            continue
        s = sessions[j.session]
        for g in j.games:
            t = g.timeline()
            if g.weights == "read":
                s["weights_read"].append(t["load_ms"])
            if t["lm_read"]:
                s["lm_read"].append(t["lm_load_ms"])
            if t["cascade_read"]:
                s["cascade_read"].append(t["cascade_load_ms"])
            s["warm_reads"] += t["warm"] or (not g.lines and (t["lm_read"] or t["cascade_read"]))
        # its text: logged (API 20 on), else looked up or put in the result arena (API 19 logs no text)
        if s["first_text_job"] is None and j.kind in ("ocr", "ocr-only") and j.rc == 0 and \
                (j.text_lines or j.lookup_ms or j.arena_peak):
            s["first_text_job"], s["first_text_ms"] = j.n, j.done_ms
        if s["first_open_wait_ms"] is None and j.kind == "ocr" and j.why in ("overlay opened",
                                                                              "the screen changed during the last one"):
            s["first_open_wait_ms"] = j.done_ms
    return {"sessions": sessions, "jobs": jobs, "stages": stages, "steps": steps, "normal_boots": normal}


def selftest_rows(stage_records, jobs):
    """Each stage run: the Shell's time, the game's job and the record's own writes."""
    games = [g for j in jobs for g in j.games if g.stage]
    out, begins, gi = [], {}, 0
    for rec in stage_records:
        if rec["what"] == "begin":
            begins[rec["stage"]] = rec
            continue
        b = begins.pop(rec["stage"], None)
        if b is None:
            continue
        while gi < len(games) and games[gi].stage != rec["stage"]:
            gi += 1
        g = games[gi] if gi < len(games) else None
        gi += 1
        row = {"stage": rec["stage"], "what": rec["what"], "rc": rec.get("rc"), "begin_t": b["t"], "end_t": rec["t"],
               "game_ms": g.timeline()["end_ms"] if g else None}
        if rec["what"] == "end" and rec.get("ms") is not None:
            row["shell_ms"], row["record_ms"] = rec["ms"], rec["t"] - b["t"] - rec["ms"]
        else:
            row["shell_ms"], row["record_ms"] = rec["t"] - b["t"], 0
        if row["game_ms"] is not None:
            row["exchange_ms"] = round(row["shell_ms"] - row["game_ms"], 1)
        out.append(row)
    return out


def analyse(text, kernel_text=None):
    p = parse_log(text)
    jobs = p["jobs"]
    res = {"sessions": p["sessions"], "jobs": [j.as_dict() for j in jobs], "normal_boots": p["normal_boots"],
           "kernel_pids": list(dict.fromkeys(m.group(1) for m in (KPID.match(l.strip())
                                                                  for l in (kernel_text or "").splitlines()) if m))}
    rows = selftest_rows(p["stages"], jobs)
    res["selftest"] = rows
    if rows:
        first = next((r["t"] for r in p["stages"] if r["what"] == "begin"), None)
        done = next((s["t"] for s in p["steps"] if s["step"] == "done"), None)
        res["selftest_total"] = {"first_begin_t": first, "done_t": done,
                                 "span_ms": done - first if done is not None and first is not None else None,
                                 "stage_runs": len(rows), "cancelled": sum(r["rc"] == -118 for r in rows),
                                 "shell_ms": sum(r["shell_ms"] for r in rows),
                                 "game_ms": round(sum(r["game_ms"] or 0 for r in rows), 1),
                                 "record_ms": sum(r["record_ms"] for r in rows),
                                 "in_user_jobs": sum(1 for j in jobs if j.stages and j.kind != "selftest")}
    tl = [g.timeline() for j in jobs if j.kind in ("ocr", "ocr-only", "refresh", "selftest") for g in j.games]

    def vals(key, keep=lambda t: True):
        return [t[key] for t in tl if t.get(key) is not None and keep(t)]
    user = [j.as_dict() for j in jobs if j.kind in ("ocr", "ocr-only", "refresh") and j.done_ms is not None and not j.stages]
    res["distributions"] = {
        "game_end_ms": vals("end_ms"), "capture_ms": vals("capture_ms"), "capture_strips": vals("capture_strips"),
        "find_ms": vals("find_ms"), "settle_held_ms": vals("settle_ms", lambda t: t["settle"] == "held"),
        "settle_limit_ms": vals("settle_ms", lambda t: t["settle"] == "limit"),
        "load_read_ms": [t["load_ms"] for t in tl if t["weights"] == "read"],
        "lm_read_ms": [t["lm_load_ms"] for t in tl if t["lm_read"]],
        "cascade_read_ms": [t["cascade_load_ms"] for t in tl if t["cascade_read"]],
        "recognize_ms": vals("recognize_ms", lambda t: t["recognize_ms"] > 0),
        "cascade_ms": vals("cascade_ms", lambda t: t["cascade_ms"] > 0),
        "tears": [t["tears"] for t in tl if t["tears"]],
        "user_job_ms": [d["done_ms"] for d in user], "user_shell_ms": [d["shell_ms"] for d in user if "shell_ms" in d],
        "user_log_ms": [d["log_ms"] for d in user if d["log_ms"] is not None],
        "user_log_lines": [d["log_lines"] for d in user],
        "pace_max_gap_ms": [t["pace"]["max_gap_ms"] for t in tl if t["pace"]],
    }
    return res


def render_job(j, out):
    tags = ("  [self-test]" if j["selftest"] else "") + ("  [overlay opened]" if j["overlay_opened_during"] else "")
    out.append(f"  job {j['n']:>3} {j['kind']:<8} {fmt_ms(j['done_ms']):>7} ms rc {j['rc_name']:<15} ({j['why']}){tags}")
    for g in j["games"]:
        t = g["timeline"]
        parts = [f"game {fmt_ms(t['end_ms'])} ms" + (f" ({g['stage']})" if g["stage"] else ""),
                 g["model"] + (" fallback" if g["fallback"] else ""), f"lines {g['lines']}"]
        if t.get("capture_strips") is not None:
            parts.append(f"{t['capture_strips']} strips in {t['capture_ms']} ms")
        if t["first_pass_ms"] is not None:
            parts.append(f"first pass {t['first_pass_ms']:.0f}")
        if t["find_ms"] is not None:
            parts.append(f"find {t['find_ms']:.0f}")
        if t["settle"]:
            parts.append(f"settle {t['settle_ms']:.0f} ({t['settle']}, {t['settle_passes']} passes)")
        if t["tears"]:
            parts.append(f"{t['tears']} rows torn, {t['tries']} tries"
                         + (f", the last from {t['retry_at_ms']:.0f}" if t["retry_at_ms"] else ""))
        if t["load_ms"]:
            parts.append(f"load {t['load_ms']:.0f} ({t['weights']}{', warm-up' if t['warm'] else ''})")
        if t["lm_read"]:
            parts.append(f"LM read {t['lm_load_ms']:.0f}")
        if t["recognize_ms"]:
            parts.append(f"recognize {t['recognize_ms']:.0f}")
        if t["cascade_read"]:
            parts.append(f"cascade model read {t['cascade_load_ms']:.0f}")
        if t["cascade_ms"]:
            parts.append(f"cascade {t['cascade_ms']:.0f}")
        if t.get("other_ms") is not None:
            parts.append(f"other {t['other_ms']:.0f}")
        big = [f"{a['item']}@{a['at_ms']:.0f}" for a in t["allocs"] if a["item"] in ("weights", "lm", "cascade")]
        if big:
            parts.append("allocated at " + ", ".join(big) + " ms")
        if t["pace"]:
            parts.append(f"{t['pace']['frames']} frames, longest gap {t['pace']['max_gap_ms']:.0f} ms")
        out.append("      " + "; ".join(parts))
    if "shell_ms" in j and not j["selftest"]:
        extra = (f"; game exchanges {j['exchange_ms']:,} ms, log {j['log_ms']:,} ms in {j['appends']} appends"
                 if j["exchange_ms"] is not None else "")
        out.append(f"      Shell: {fmt_ms(j['shell_ms'])} ms beside the game's job and the lookup "
                   f"({fmt_ms(j['lookup_ms'])} ms); {j['log_lines']} log lines{extra}")


def render(res, show_jobs=False):
    out = []
    if res.get("normal_boots"):
        out.append(f"the normal build's short log ({res['normal_boots']} boot(s)): no job, step or timing line to time; "
                   "the -debug zip of the same release logs them (docs/release-variants.md)")
    pids = res["kernel_pids"]
    out.append(f"sessions (game starts): {len(res['sessions'])}"
               + (f"; game processes in kernel.txt: {len(pids)}" if pids else ""))
    for s in res["sessions"]:
        def reads(v):
            return f"{len(v)}x ({', '.join(f'{x:.0f}' for x in v)} ms)" if v else "0x"
        out.append(f"  {s['title']} #{s['index']}: {s['jobs']} jobs, overlay opened {s['opened']}x; reads: weights "
                   f"{reads(s['weights_read'])}, LM {reads(s['lm_read'])}, cascade model {reads(s['cascade_read'])}"
                   f" ({s['warm_reads']} by warm-ups); first job with text: "
                   + (f"#{s['first_text_job']}, {fmt_ms(s['first_text_ms'])} ms" if s["first_text_job"] else "-")
                   + (f"; first opened-overlay job {fmt_ms(s['first_open_wait_ms'])} ms" if s["first_open_wait_ms"] else ""))
    tot = res.get("selftest_total")
    if tot:
        out.append(f"self-test: {tot['stage_runs']} stage runs ({tot['cancelled']} cancelled), t={fmt_ms(tot['first_begin_t'])}"
                   f" to done t={fmt_ms(tot['done_t'])} ({fmt_ms(tot['span_ms'])} ms); in the Shell "
                   f"{fmt_ms(tot['shell_ms'])} ms, the game's jobs {fmt_ms(tot['game_ms'])} ms, the records' own writes "
                   f"{fmt_ms(tot['record_ms'])} ms; in jobs the user waited for: {tot['in_user_jobs']}")
        for r in res["selftest"]:
            out.append(f"  {r['stage']:<10} t={r['begin_t']:>6}..{r['end_t']:<6} {r['what']:<4} rc {rc_name(r['rc']):<15} "
                       f"Shell {fmt_ms(r['shell_ms']):>6} ms = game {fmt_ms(r['game_ms']):>6} + exchange/log "
                       f"{fmt_ms(r.get('exchange_ms')):>6}; record writes {fmt_ms(r['record_ms']):>5} ms")
    d = res["distributions"]
    out.append("OCR jobs, the game's side (ms):")
    for key, what in (("game_end_ms", "game job"), ("capture_strips", "strips read"), ("capture_ms", "strip waits"),
                      ("find_ms", "finder (its strips included)"), ("settle_held_ms", "stable-text wait, held"),
                      ("settle_limit_ms", "stable-text wait, limit"), ("load_read_ms", "weights read"),
                      ("lm_read_ms", "LM read"), ("cascade_read_ms", "cascade model read"),
                      ("recognize_ms", "recognition"), ("cascade_ms", "cascade re-reads"), ("tears", "rows torn"),
                      ("pace_max_gap_ms", "longest frame gap")):
        out.append(f"  {what:<30} {len(d[key]):>3}: {span(d[key])}")
    out.append("OCR jobs, the Shell's side (ms; the self-test's left out):")
    for key, what in (("user_job_ms", "job"), ("user_shell_ms", "beside the game and lookup"),
                      ("user_log_ms", "log appends"), ("user_log_lines", "log lines meanwhile")):
        out.append(f"  {what:<30} {len(d[key]):>3}: {span(d[key])}")
    if show_jobs:
        out.append("jobs:")
        for j in res["jobs"]:
            if j["kind"] != "lookup":
                render_job(j, out)
    return "\n".join(out)


def last_boot(text):
    """The log from the last Shell start on (a collect's log.txt keeps earlier builds' runs)."""
    i = text.rfind("Vita JP Overlay shell started")
    return text[i:] if i >= 0 else text


def read_collect(folder):
    folder = Path(folder)
    text = "".join(p.read_text(encoding="utf-8", errors="replace")
                   for p in (folder / (DATA + "log.old.txt"), folder / (DATA + "log.txt")) if p.is_file())
    k = folder / (DATA + "kernel.txt")
    return text, (k.read_text(encoding="utf-8", errors="replace") if k.is_file() else None)


def compare(a, b):
    out = []
    for name, res in (("old", a), ("new", b)):
        d, tot = res["distributions"], res.get("selftest_total")
        first = [s["first_text_ms"] for s in res["sessions"] if s["first_text_job"]]
        out.append(f"{name}: first job with text per session {', '.join(fmt_ms(v) for v in first) or '-'} ms; jobs "
                   f"median {fmt_ms(statistics.median(d['user_job_ms'])) if d['user_job_ms'] else '-'} ms, p90 "
                   f"{fmt_ms(pct(d['user_job_ms'], 90))} ms; the Shell beside the game median "
                   f"{fmt_ms(statistics.median(d['user_shell_ms'])) if d['user_shell_ms'] else '-'} ms; reads: weights "
                   f"{sum(len(s['weights_read']) for s in res['sessions'])}, LM "
                   f"{sum(len(s['lm_read']) for s in res['sessions'])}, cascade "
                   f"{sum(len(s['cascade_read']) for s in res['sessions'])} in {len(res['sessions'])} session(s); "
                   + (f"self-test {fmt_ms(tot['span_ms'])} ms over {tot['stage_runs']} stage runs, "
                      f"{tot['in_user_jobs']} in jobs the user waited for" if tot else "no self-test"))
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path, nargs="?")
    ap.add_argument("--collect", type=Path, metavar="DIR")
    ap.add_argument("--kernel", type=Path, help="kernel.txt, with a LOG argument")
    ap.add_argument("--compare", type=Path, nargs=2, metavar=("OLD", "NEW"), help="two collect folders")
    ap.add_argument("--all-boots", action="store_true", help="every boot in the log, not only the last")
    ap.add_argument("--jobs", action="store_true", help="a line per job and per game job")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    def load(folder=None, log=None, kernel=None):
        if folder:
            text, ktext = read_collect(folder)
        else:
            text = log.read_text(encoding="utf-8", errors="replace") if log and log.is_file() else ""
            ktext = kernel.read_text(encoding="utf-8", errors="replace") if kernel and kernel.is_file() else None
        if not text.strip():
            return None
        return analyse(text if args.all_boots else last_boot(text), ktext)

    if args.compare:
        a, b = load(args.compare[0]), load(args.compare[1])
        if a is None or b is None:
            print("no log.txt in one of the folders", file=sys.stderr)
            return 2
        print(json.dumps({"old": a, "new": b}, indent=1) if args.json else compare(a, b))
        return 0
    res = load(args.collect, args.log, args.kernel)
    if res is None:
        print("no log", file=sys.stderr)
        return 2
    print(json.dumps(res, indent=1) if args.json else render(res, args.jobs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
