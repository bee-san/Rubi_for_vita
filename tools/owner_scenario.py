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
# A refusal of an item of the Shell's Paf ledger: the reserve guard's line (shell/paf_guard.h), or the normal build's
# line for an allocation the heap refused after the guard passed (shell/memory.cpp). Its item is the ledger's name
# (shell/ledger.c): the overlay page (#537: its own room, after #534 kept the text block) and its text arena (0.10.1)
# are a press; the result arenas a lookup; the Anki arena a card's save.
REFUSAL_RE = re.compile(r"^(.+?) Paf (?:reserve guard: free=(\d+) requested=(\d+) reserve=\d+|"
                        r"allocation refused: free=(\d+) requested=(\d+))$")
PRESS = ("overlay page", "overlay text arena")
RESULTS = "result arenas"
ANKI = "Anki arena"
# One line per card saved (shell/anki.c's "save ... rc=0": the normal build's has no word, the debug build's has it).
ANKI_RE = re.compile(r"^anki: save (?:.+ )?rc=0, \d+ ms$")
# The lines a press's own job is not past: the next press's, a ledger line, the debug build's next job.
JOB_START_RE = re.compile(r"^job start ")
# The Paf plugin's own 8 KiB, which every ledger line of 0.10.1 shows held between jobs: what Rubi held at an event
# before the boot's first ledger line. After it, an event holds what the boot's last ledger line said Rubi held (0.10.1:
# 8,192 B at every refusal; after #534, the kept text block too: 28,672 B).
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
    """One boot's events, in order. A refusal of the overlay's page or text arena is a press of the toggle. Its own job's
    result arena is the refusal at the same free size up to three lines before it, else the first at the same free size
    after it before the next press, ledger line or job start (the debug build logs the job's lines between: the heap
    has not moved meanwhile); any other refusal of a result
    arena is a lookup of its own (a background read's, or a cursor move's). An Anki save, or its arena's refusal, is a
    press that saved a card, at the last free size the log showed. Other items' refusals (line history) are not
    replayed: unreplayed() counts them."""
    events, refusals = [], []
    level, held = None, HELD_0101
    for n, line in body:
        m = LEDGER_RE.match(line)
        if m:
            level, held = int(m.group(4)), int(m.group(2))
            events.append([m.group(1), level, held])
            refusals.append((n, "ledger", level, None, held))
            continue
        if JOB_START_RE.match(line):
            refusals.append((n, "job", level, None, held))
            continue
        m = REFUSAL_RE.match(line)
        if m:
            what = m.group(1)
            level = int(m.group(2) or m.group(4))
            if what in PRESS or what == RESULTS:
                refusals.append((n, what, level, len(events), held))
                events.append(None)  # placed below, once the pairs are known
            elif what == ANKI:
                events.append(["anki", level, held])
            continue
        if ANKI_RE.match(line) and level is not None:
            events.append(["anki", level, held])
    paired = set()
    for i, (n, what, free, slot, held) in enumerate(refusals):
        if what not in PRESS:
            continue
        lookup = None
        for j in range(i - 1, -1, -1):  # just before it, at the same free size
            n2, what2, free2, slot2, _ = refusals[j]
            if n - n2 > 3:
                break
            if what2 == RESULTS and j not in paired and free2 == free:
                lookup = j
                break
        if lookup is None:  # else the first after it, before the next press, ledger line or job start
            for j in range(i + 1, len(refusals)):
                n2, what2, free2, slot2, _ = refusals[j]
                if what2 in PRESS or what2 in ("ledger", "job"):
                    break
                if what2 == RESULTS and j not in paired and free2 == free:
                    lookup = j
                    break
        if lookup is not None:
            paired.add(lookup)
            events[refusals[lookup][3]] = "drop"
        events[slot] = ["press", free, held, 1 if lookup is not None else 0]
    for j, (n, what, free, slot, held) in enumerate(refusals):
        if what == RESULTS and j not in paired:
            events[slot] = ["lookup", free, held]
    return [e for e in events if e != "drop" and e is not None]


def unreplayed(body):
    """The boot's refusals of items the replay does not run (line history): counted, so a log that has them says so."""
    return sum(1 for _, line in body if (m := REFUSAL_RE.match(line)) and m.group(1) not in PRESS + (RESULTS, ANKI))


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
            # named by its API and its first line: another boot added to the logs renames no other scenario
            scenarios.append({"name": f"api{api}-line{first}", "api": api, "build": "normal" if normal else "debug",
                              "line": first, "events": events, "unreplayed": unreplayed(body)})
    return scenarios


def write_events(scenarios, source, out):
    rows = [
        "# The owner scenario (#528): Rubi's Shell Paf events on the owner's PCH-1001, one scenario per boot.",
        f"# Generated by tools/owner_scenario.py --extract from {source}; numbers only (docs/owner-scenario.md).",
        "# scenario NAME API BUILD | boot|start|exit FREE HELD | press FREE HELD LOOKUP | lookup FREE HELD | anki FREE HELD",
        "# FREE: the Paf heap's free bytes the log showed; HELD: what Rubi held then (the ledger's current; at a",
        "# refusal, the boot's last ledger figure); LOOKUP: 1 when the log has the press's own job's result arena.",
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
        if m.group(1) == "shell" and "silent" not in values and "refused" in values:
            values["silent"] = values["refused"] - values.get("told", 0)  # a binary from before `silent`
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
    """Runs the binaries given, or found under `build` (none is looked for without it)."""
    shell_bin = shell_bin or (find(build, ["vjo-shell-worker-paf-tests"]) if build else None)
    game_bin = game_bin or (find(build, ["vjo-alloc-sim-tests"]) if build else None)
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
    labels = before.get("labels", {})
    out.append("Shell (the Paf heap at the logged free sizes). Before: " + labels.get("shell", "-"))
    out.append(f"{'scenario':<22} {'presses':>7} {'not opened':>10} {'page':>4} {'lookups':>7} {'refused':>7} "
               f"{'told':>5} {'silent':>6} {'anki ok':>7} {'refused':>7} {'peak B':>8} {'held B':>7}"
               f"   before: not opened / silent / peak")
    for (part, name), v in rows.items():
        if part != "shell":
            continue
        b = before.get("shell", {}).get(name, {})
        bsilent = b.get("silent", b.get("refused", 0) - b.get("told", 0)) if b else "-"
        out.append(f"{name:<22} {v.get('presses', 0):>7} {v.get('not_opened', 0):>10} {v.get('page_failed', 0):>4} "
                   f"{v.get('lookups', 0):>7} {v.get('refused', 0):>7} {v.get('told', 0):>5} {v.get('silent', 0):>6} "
                   f"{v.get('anki_ok', 0):>7} {v.get('anki_refused', 0):>7} {v.get('peak', 0):>8} {v.get('held', 0):>7}"
                   f"   {b.get('not_opened', '-')} / {bsilent} / {b.get('peak', '-')}")
    game = [(name, v) for (part, name), v in rows.items() if part == "game"]
    if game:
        out.append("")
        out.append("Game USER (the owner's job: Large, LM 1000k, cascade, spotter, 3 threads; free USER as logged or "
                   "estimated). Before: " + labels.get("game", "-"))
        out.append(f"{'level':<22} {'free USER':>10} {'rc':>5} {'tier':>5} {'lm':>3} {'casc':>4} {'spot':>4} "
                   f"{'helpers':>7} {'lines':>5} {'peak B':>9} {'told':>4}   before: rc / tier / peak")
        for name, v in game:
            b = before.get("game", {}).get(name, {})
            out.append(f"{name:<22} {v.get('free', 0):>10} {v.get('rc', 0):>5} {v.get('tier', 0):>5} "
                       f"{v.get('lm', 0):>3} {v.get('cascade', 0):>4} {v.get('spot', 0):>4} {v.get('helpers', 0):>7} "
                       f"{v.get('lines', 0):>5} {v.get('peak', 0):>9} {v.get('told', 0):>4}   {b.get('rc', '-')} / "
                       f"{b.get('tier', '-')} / {b.get('peak', '-')}")
    else:
        out.append("")
        out.append("Game USER: not run (no alloc_sim binary with the release models in this build: "
                   "VJO_VOCR_MODEL_DIR; the fast gate's models lane runs it)")
    return "\n".join(out)


def check(rows, limits, require_game=False):
    """The ratchet: no count above its limit, no floor undercut, every scenario present with every counter it is held
    to. Returns the problems."""
    problems = []
    game_ran = any(p == "game" for p, _ in rows)
    if require_game and not game_ran:
        problems.append("game: no rows, although this build has the release models (the game part did not run)")
    for part in ("shell", "game"):
        for name, lim in limits.get("limits", {}).get(part, {}).items():
            v = rows.get((part, name))
            if v is None:
                if part == "game" and not game_ran:
                    continue  # no models in this build: the game part did not run
                problems.append(f"{part} {name}: missing from the run (did it crash?)")
                continue
            for key, bound in lim.items():
                have = v.get(key[4:] if key.startswith("min_") else key)
                if have is None:
                    problems.append(f"{part} {name}: no {key.removeprefix('min_')} in the run's line, which "
                                    f"tools/owner_scenario.json holds")
                elif key.startswith("min_"):
                    if have < bound:
                        problems.append(f"{part} {name}: {key[4:]}={have}, below its floor {bound} "
                                        f"(tools/owner_scenario.json; a regression)")
                elif have > bound:
                    problems.append(f"{part} {name}: {key}={have}, above its limit {bound} "
                                    f"(tools/owner_scenario.json; a regression)")
    for (part, name) in rows:
        if name != "all" and name not in limits.get("limits", {}).get(part, {}):
            problems.append(f"{part} {name}: not in tools/owner_scenario.json's limits (--lower adds it)")
    return problems


# What the ratchet holds: counts and bytes that may only go down, and (min_) what a job chose or did that may only go
# up. `silent`: refusals no open overlay showed (refused - told); `refused` and `told` alone are not held, as a press
# that now opens asks for its lookup, and is told when it is refused.
SHELL_LIMITS = ("not_opened", "silent", "anki_refused", "peak", "held")
GAME_LIMITS = ("failed", "peak")
GAME_FLOORS = ("told", "tier", "lm", "spot", "helpers", "lines")


def lowered(rows, limits, parts=("shell", "game")):
    """limits with every limit of `parts` lowered to the run's value and every floor raised to it: never the other
    way. A scenario new to the limits takes the run's values."""
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


def rises(old, new):
    """Every limit that `new` holds looser than `old` (a higher limit, a lower floor, a scenario no longer held)."""
    out = []
    for part, scen in old.get("limits", {}).items():
        for name, lim in scen.items():
            now = new.get("limits", {}).get(part, {}).get(name)
            if now is None:
                out.append(f"{part} {name}: no longer held")
                continue
            for key, bound in lim.items():
                if key not in now:
                    out.append(f"{part} {name}: {key} no longer held")
                elif key.startswith("min_") and now[key] < bound or not key.startswith("min_") and now[key] > bound:
                    out.append(f"{part} {name}: {key} {bound} -> {now[key]}")
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
    ap.add_argument("--reset-limits", action="store_true",
                    help="with --set-before: the run's values become the part's limits even where they are looser (a "
                         "change of the replay's model; every limit that rises is printed); without it --set-before "
                         "keeps the tighter of the old limit and the run")
    ap.add_argument("--require-game", action="store_true",
                    help="with --check: fail when the game part printed no rows (a build with the release models)")
    ap.add_argument("--json", action="store_true", help="print the run's rows as JSON instead of the table")
    args = ap.parse_args(argv)
    if args.extract:
        scenarios = extract(args.extract, args.min_api)
        write_events(scenarios, args.source, args.out)
        counts = logged_counts(scenarios)
        print(f"{len(scenarios)} scenarios, {sum(len(s['events']) for s in scenarios)} events -> {args.out}")
        for s in scenarios:
            print(f"  {s['name']}: {counts[s['name']]}" + (f", {s['unreplayed']} refusal(s) of items not replayed"
                                                          if s["unreplayed"] else ""))
        return 0
    if not (args.run or args.check or args.lower or args.set_before):
        ap.error("nothing to do: --extract, --run, --check, --lower or --set-before")
    if not args.build and not args.shell_bin:
        ap.error("--build (or --shell-bin) is needed to run the scenario")
    limits = json.loads(LIMITS.read_text(encoding="utf-8")) if LIMITS.exists() else {}
    rows, problems = run(args.build, args.shell_bin, args.game_bin)
    if args.json:
        print(json.dumps({f"{p} {n}": v for (p, n), v in sorted(rows.items())}, indent=1))
    else:
        print(table(rows, limits))
    parts = (args.part,) if args.part else ("shell", "game")
    old_limits = json.loads(json.dumps(limits))
    if args.set_before and not problems:
        for part in parts:
            if not any(p == part for p, _ in rows):
                problems.append(f"--set-before: no {part} rows in this run (the game part needs a build with "
                                f"VJO_VOCR_MODEL_DIR): nothing recorded")
    if args.set_before and not problems:
        before = limits.setdefault("before", {})
        for part in parts:
            before[part] = {name: v for (p, name), v in rows.items() if p == part}
            before.setdefault("labels", {})[part] = args.set_before
            if args.reset_limits:
                limits.setdefault("limits", {})[part] = {}  # the new baseline is the limit: lowered() fills it below
        before.pop("label", None)
    if (args.lower or args.set_before) and not problems:
        limits = lowered(rows, limits, parts)
        for r in rises(old_limits, limits):
            print(f"limit loosened (--reset-limits): {r}")
        LIMITS.write_text(json.dumps(limits, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {LIMITS.relative_to(ROOT)}")
    if args.check:
        problems += check(rows, limits, args.require_game)
    for p in problems:
        print(f"owner scenario: {p}", file=sys.stderr)
    if problems:
        print("OWNER SCENARIO: FAIL", file=sys.stderr)
        return 1
    print("OWNER SCENARIO: PASS" if args.check else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
