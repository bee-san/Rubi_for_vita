#!/usr/bin/env python3
"""The owner scenario (#528): Rubi's memory failures on the owner's console, replayed on the host.

The owner's PCH-1001 (Rubi 0.10.1, API 57) logged them in ux0:data/VitaJPOverlay/log.txt: the overlay that did not
open ("overlay not opened: no Paf memory for its text"), the lookups whose result arena the Paf reserve guard
refused, the Paf heap's free size at each of them and at every game start and exit, and the Anki saves. This tool

  --extract LOG...  turns such logs into tools/owner_scenario.txt: one line per event, numbers only (no game text,
                    no title IDs), each boot of a Rubi build that logged an event one scenario;
  --run             runs the scenario binaries of a build (the Shell's: shell_worker_paf; the game's: alloc_sim,
                    with the release models) and prints the table of what failed and what Rubi held;
  --check           does --run and fails when a count or a peak is above tools/owner_scenario.json's limits
                    (a ratchet: a fix lowers them in the same pull request; ctest owner_scenario);
  --lower           does --run and lowers those limits to the run's values (never loosens one): a fix's step;
  --set-before L    does --run and records it as the table's 0.10.1 column (done once).

The replay runs Rubi's own code (shell/paf_guard.h, shell/overlay_mem.h, shell/worker.c's result arena, the game
worker and kernel) against a model of the Paf heap: what the log shows free, less what this build holds beyond what
0.10.1 held then. docs/owner-scenario.md has the model, its limits and how to read the table. Host evidence only:
no console number comes from it (AGENTS.md rule 5)."""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVENTS = ROOT / "tools" / "owner_scenario.txt"
LIMITS = ROOT / "tools" / "owner_scenario.json"

BOOT_RE = re.compile(r"^Rubi shell started \(kernel API (\d+)(.*)\)")
LEDGER_RE = re.compile(r"^ledger (boot|start|exit) paf: current=(\d+) peak=(\d+) limit=\d+ free=(\d+)")
GUARD_RE = re.compile(r"^(overlay text arena|result arenas) Paf reserve guard: free=(\d+) requested=(\d+) reserve=(\d+)")
# One line per saved card, with a screenshot or without (shell/anki.c's "save rc=0").
ANKI_RE = re.compile(r"^anki: save rc=0, \d+ ms$")
# The Paf plugin's own 8 KiB, which every ledger line of 0.10.1 shows held between jobs.
HELD_0101 = 8192


def parse_boots(lines):
    """Splits a log into boots: (api, normal build?, [(line number, line)])."""
    boots, cur = [], None
    for n, line in enumerate(lines, 1):
        m = BOOT_RE.match(line)
        if m:
            cur = (int(m.group(1)), "normal build" in m.group(2), [])
            boots.append(cur)
        elif cur is not None:
            cur[2].append((n, line))
    return boots


def boot_events(body):
    """One boot's events, in order. A refusal of the overlay's text arena is a press of the toggle; a refusal of a
    result arena within three lines of it, at the same free size, is that press's lookup; any other refusal of a
    result arena is a lookup of its own (a background read's, or a cursor move's). An Anki save is a press that
    saved a card, at the last free size the log showed."""
    events, guards = [], []
    level = None
    for n, line in body:
        m = LEDGER_RE.match(line)
        if m:
            level = int(m.group(4))
            events.append([m.group(1), level, int(m.group(2))])
            continue
        m = GUARD_RE.match(line)
        if m:
            level = int(m.group(2))
            guards.append((n, m.group(1), level, len(events)))
            events.append(None)  # placed below, once the pairs are known
            continue
        if ANKI_RE.match(line) and level is not None:
            events.append(["anki", level, HELD_0101])
    paired = set()
    for i, (n, what, free, slot) in enumerate(guards):
        if what != "overlay text arena":
            continue
        lookup = None
        for j, (n2, what2, free2, slot2) in enumerate(guards):
            if what2 == "result arenas" and j not in paired and free2 == free and abs(n2 - n) <= 3:
                lookup = j
                break
        if lookup is not None:
            paired.add(lookup)
            events[guards[lookup][3]] = "drop"
        events[slot] = ["press", free, HELD_0101, 1 if lookup is not None else 0]
    for j, (n, what, free, slot) in enumerate(guards):
        if what == "result arenas" and j not in paired:
            events[slot] = ["lookup", free, HELD_0101]
    return [e for e in events if e != "drop" and e is not None]


def extract(paths, min_api):
    scenarios = []
    for path in paths:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").split("\n")
        for api, normal, body in parse_boots(lines):
            if api < min_api:
                continue
            events = boot_events(body)
            if not any(e[0] in ("press", "lookup", "anki") for e in events):
                continue
            first = body[0][0] - 1 if body else 0
            scenarios.append({"name": f"boot{len(scenarios) + 1}-line{first}", "api": api,
                              "build": "normal" if normal else "debug", "line": first, "events": events})
    return scenarios


def write_events(scenarios, source, out):
    rows = [
        "# The owner scenario (#528): Rubi's Shell Paf events on the owner's PCH-1001, one scenario per boot.",
        f"# Generated by tools/owner_scenario.py --extract from {source}; numbers only (docs/owner-scenario.md).",
        "# scenario NAME API BUILD | boot|start|exit FREE HELD | press FREE HELD LOOKUP | lookup FREE HELD | anki FREE HELD",
        "# FREE: the Paf heap's free bytes the log showed; HELD: what Rubi held then (the ledger's current, or the",
        "# plugin's 8 KiB at a refusal); LOOKUP: 1 when 0.10.1 asked for the press's result arena too.",
    ]
    for s in scenarios:
        rows.append(f"scenario {s['name']} {s['api']} {s['build']}")
        for e in s["events"]:
            rows.append(" ".join(str(x) for x in e))
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


def read_events(path=EVENTS):
    scenarios, cur = [], None
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] == "scenario":
            cur = {"name": parts[1], "api": int(parts[2]), "build": parts[3], "events": []}
            scenarios.append(cur)
        else:
            cur["events"].append([parts[0]] + [int(x) for x in parts[1:]])
    return scenarios


def logged_counts(scenarios):
    """What 0.10.1 logged in the scenarios: the 'before' of the table, from the log alone."""
    out = {}
    for s in scenarios:
        presses = [e for e in s["events"] if e[0] == "press"]
        lookups = [e for e in s["events"] if e[0] == "lookup"]
        out[s["name"]] = {"presses_refused": len(presses),
                          "arena_refusals": sum(e[3] for e in presses) + len(lookups),
                          "anki_saves": sum(1 for e in s["events"] if e[0] == "anki")}
    return out


LINE_RE = re.compile(r"owner-scenario (shell|game) (\S+): (.*)$")  # acutest may print its test name before the first


def parse_lines(text):
    """The binaries' `owner-scenario <part> <name>: key=value ...` lines."""
    rows = {}
    for line in text.splitlines():
        m = LINE_RE.search(line.strip())
        if not m:
            continue
        values = {}
        for kv in m.group(3).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                values[k] = int(v) if re.fullmatch(r"-?\d+", v) else v
        rows[(m.group(1), m.group(2))] = values
    return rows


def run_binary(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1800)
    return p.returncode, p.stdout


def find(build, names):
    for name in names:
        for p in sorted(Path(build).rglob(name)):
            if p.is_file():
                return p
    return None


def run(build, shell_bin=None, game_bin=None):
    shell_bin = shell_bin or find(build, ["vjo-shell-worker-paf-tests"])
    game_bin = game_bin or find(build, ["vjo-alloc-sim-tests", "vjo-alloc_sim-tests", "alloc_sim"])
    rows, problems = {}, []
    if not shell_bin:
        problems.append(f"no Shell scenario binary (vjo-shell-worker-paf-tests) under {build}")
    else:
        rc, out = run_binary([str(shell_bin), "owner scenario"])
        rows.update(parse_lines(out))
        if rc:
            problems.append(f"{shell_bin.name} 'owner scenario' failed (rc {rc}):\n{out[-3000:]}")
    if game_bin:
        rc, out = run_binary([str(game_bin), "owner scenario"])
        rows.update(parse_lines(out))
        if rc:
            problems.append(f"{game_bin.name} 'owner scenario' failed (rc {rc}):\n{out[-3000:]}")
    return rows, problems


def table(rows, limits):
    out = []
    before = limits.get("before", {})
    out.append("Shell (Paf heap at the logged free sizes; 0.10.1's figures: its log, and this replay at main 1845471b)")
    out.append(f"{'scenario':<22} {'presses':>7} {'not opened':>10} {'lookups':>7} {'refused':>7} {'told':>5} "
               f"{'anki ok':>7} {'refused':>7} {'peak B':>8} {'held B':>7}   0.10.1: not opened / refused / peak")
    for (part, name), v in rows.items():
        if part != "shell":
            continue
        b = before.get("shell", {}).get(name, {})
        out.append(f"{name:<22} {v.get('presses', 0):>7} {v.get('not_opened', 0):>10} {v.get('lookups', 0):>7} "
                   f"{v.get('refused', 0):>7} {v.get('told', 0):>5} "
                   f"{v.get('anki_ok', 0):>7} {v.get('anki_refused', 0):>7} {v.get('peak', 0):>8} {v.get('held', 0):>7}   "
                   f"{b.get('not_opened', '-')} / {b.get('refused', '-')} / {b.get('peak', '-')}")
    game = [(name, v) for (part, name), v in rows.items() if part == "game"]
    if game:
        out.append("")
        out.append("Game USER (the owner's job: Large, LM 1000k, cascade, spotter, 3 threads; free USER as logged or "
                   "estimated)")
        out.append(f"{'level':<22} {'free USER':>10} {'rc':>5} {'tier':>5} {'lm':>3} {'casc':>4} {'spot':>4} "
                   f"{'helpers':>7} {'peak B':>9} {'told':>4}   0.10.1: rc / tier / peak")
        for name, v in game:
            b = before.get("game", {}).get(name, {})
            out.append(f"{name:<22} {v.get('free', 0):>10} {v.get('rc', 0):>5} {v.get('tier', 0):>5} "
                       f"{v.get('lm', 0):>3} {v.get('cascade', 0):>4} {v.get('spot', 0):>4} {v.get('helpers', 0):>7} "
                       f"{v.get('peak', 0):>9} {v.get('told', 0):>4}   {b.get('rc', '-')} / {b.get('tier', '-')} / "
                       f"{b.get('peak', '-')}")
    else:
        out.append("")
        out.append("Game USER: not run (no alloc_sim binary with the release models in this build: "
                   "VJO_VOCR_MODEL_DIR; the fast gate's models lane runs it)")
    return "\n".join(out)


def check(rows, limits):
    """The ratchet: no count above its limit, no floor undercut, every scenario present. Returns the problems."""
    problems = []
    game_ran = any(p == "game" for p, _ in rows)
    for part in ("shell", "game"):
        for name, lim in limits.get("limits", {}).get(part, {}).items():
            v = rows.get((part, name))
            if v is None:
                if part == "game" and not game_ran:
                    continue  # no models in this build: the game part did not run
                problems.append(f"{part} {name}: missing from the run (did it crash?)")
                continue
            for key, bound in lim.items():
                if key.startswith("min_"):
                    have = v.get(key[4:], 0)
                    if have < bound:
                        problems.append(f"{part} {name}: {key[4:]}={have}, below its floor {bound} "
                                        f"(tools/owner_scenario.json; a regression)")
                elif v.get(key, 0) > bound:
                    problems.append(f"{part} {name}: {key}={v.get(key, 0)}, above its limit {bound} "
                                    f"(tools/owner_scenario.json; a regression)")
    for (part, name) in rows:
        if name != "all" and name not in limits.get("limits", {}).get(part, {}):
            problems.append(f"{part} {name}: not in tools/owner_scenario.json's limits (--lower adds it)")
    return problems


# What the ratchet holds: counts and bytes that may only go down, and (min_) what a job chose that may only go up.
SHELL_LIMITS = ("not_opened", "refused", "anki_refused", "peak", "held")
GAME_LIMITS = ("failed", "peak")
GAME_FLOORS = ("told", "tier", "lm")


def lowered(rows, limits, parts=("shell", "game")):
    """limits with every limit of `parts` lowered to the run's value and every floor raised to it: never the other
    way."""
    out = json.loads(json.dumps(limits))
    lim = out.setdefault("limits", {})
    for (part, name), v in sorted(rows.items()):
        if name == "all" or part not in parts:
            continue
        cur = lim.setdefault(part, {}).setdefault(name, {})
        for key in SHELL_LIMITS if part == "shell" else GAME_LIMITS:
            cur[key] = min(cur.get(key, v.get(key, 0)), v.get(key, 0))
        for key in () if part == "shell" else GAME_FLOORS:
            cur["min_" + key] = max(cur.get("min_" + key, v.get(key, 0)), v.get(key, 0))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", nargs="+", metavar="LOG", help="log.txt files to turn into the event file")
    ap.add_argument("--min-api", type=int, default=31, help="oldest Rubi API whose boots are kept (default 31)")
    ap.add_argument("--source", default="the owner's log.txt", help="where the logs came from (the file's header)")
    ap.add_argument("--out", type=Path, default=EVENTS, help="event file to write (default tools/owner_scenario.txt)")
    ap.add_argument("--build", type=Path, help="a host build directory (ctest's)")
    ap.add_argument("--shell-bin", type=Path, help="the Shell scenario binary (default: found under --build)")
    ap.add_argument("--game-bin", type=Path, help="the game scenario binary (default: found under --build)")
    ap.add_argument("--run", action="store_true", help="run the scenario and print the table")
    ap.add_argument("--check", action="store_true", help="run, print, and fail on a regression against the limits")
    ap.add_argument("--lower", action="store_true",
                    help="run, then lower tools/owner_scenario.json's limits (and raise its floors) to the run's "
                         "values: the ratchet's step in a fix's pull request; it never loosens one")
    ap.add_argument("--set-before", metavar="LABEL",
                    help="run, then record the run as the table's 'before' column, named LABEL, and make it the "
                         "limits: done for 0.10.1's code, and again only when the replay's model changes (with 0.10.1's "
                         "code for that part, and --part)")
    ap.add_argument("--part", choices=("shell", "game"),
                    help="--set-before and --lower act on this part only (the other keeps its baseline and limits)")
    ap.add_argument("--json", action="store_true", help="print the run's rows as JSON instead of the table")
    args = ap.parse_args(argv)
    if args.extract:
        scenarios = extract(args.extract, args.min_api)
        write_events(scenarios, args.source, args.out)
        counts = logged_counts(scenarios)
        print(f"{len(scenarios)} scenarios, {sum(len(s['events']) for s in scenarios)} events -> {args.out}")
        for name, c in counts.items():
            print(f"  {name}: {c}")
        return 0
    if not (args.run or args.check or args.lower or args.set_before):
        ap.error("nothing to do: --extract, --run, --check, --lower or --set-before")
    if not args.build and not args.shell_bin:
        ap.error("--build (or --shell-bin) is needed to run the scenario")
    limits = json.loads(LIMITS.read_text(encoding="utf-8")) if LIMITS.exists() else {}
    rows, problems = run(args.build or Path("."), args.shell_bin, args.game_bin)
    if args.json:
        print(json.dumps({f"{p} {n}": v for (p, n), v in sorted(rows.items())}, indent=1))
    else:
        print(table(rows, limits))
    parts = (args.part,) if args.part else ("shell", "game")
    if args.set_before and not problems:
        before = limits.setdefault("before", {})
        for part in parts:
            before[part] = {name: v for (p, name), v in rows.items() if p == part}
            before.setdefault("labels", {})[part] = args.set_before
            limits.setdefault("limits", {})[part] = {}  # the new baseline is the limit: lowered() fills it below
        before.pop("label", None)
    if (args.lower or args.set_before) and not problems:
        limits = lowered(rows, limits, parts)
        LIMITS.write_text(json.dumps(limits, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {LIMITS.relative_to(ROOT)}")
    if args.check:
        problems += check(rows, limits)
    for p in problems:
        print(f"owner scenario: {p}", file=sys.stderr)
    if problems:
        print("OWNER SCENARIO: FAIL", file=sys.stderr)
        return 1
    print("OWNER SCENARIO: PASS" if args.check else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
