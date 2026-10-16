#!/usr/bin/env python3
"""Replay the Shell plugin's boot and game-launch allocation sequence against
the console's measured SceShell memory (docs/offline-testing.md).

    python3 tools/shell_budget_model.py [--shell-pages N] [--worker-pages N] [--anki] [--json]

The model takes each item's size from include/vjo_ledger_budget.h (the
ledger's static table, which CI ties to the packaged modules) and each
console sample from docs/benchmarks/shell-budget-samples.json. For a sample
taken with overlay build B loaded, SceShell had `free + pages(B)` without
us; the headroom the current build leaves at each step is that minus the
pages it holds there. A negative headroom at a step means the console, in
that session, would have run out of pages at that step (#59's failure). A
positive one is not a proof that it fits: SceShell's own use differs
between sessions and the firmware thread's request size is unknown. The
model ranks builds and changes; the console decides.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_memory_ledger import parse_budget  # noqa: E402

SAMPLES = ROOT / "docs/benchmarks/shell-budget-samples.json"
BUDGET = ROOT / "include/vjo_ledger_budget.h"
WARN = 65536  # one #42-sized change (a 64 KiB buffer broke the boot): within this, a build is one change from failing


def steps(v, shell_pages=None, worker_pages=None, anki=False):
    """(name, pages delta, paf delta) in boot and game-launch order."""
    shell_pages = v["VJO_BUDGET_SHELL_MODULE_PAGES"] if shell_pages is None else shell_pages
    worker_pages = v["VJO_BUDGET_WORKER_IN_SHELL_PAGES"] if worker_pages is None else worker_pages
    arena = v["VJO_BUDGET_PAF_RESULTS_LOCAL"]  # the only result memory: the overlay is offline
    out = [
        ("kernel module_start (nothing in SceShell)", 0, 0),
        ("Shell module loaded into SceShell", shell_pages, 0),
        ("OCR worker pages in SceShell", worker_pages, 0),
        ("Paf plugin and RCO", 0, v["VJO_BUDGET_PAF_PLUGIN"]),
        ("VjoNet thread", v["VJO_BUDGET_SHELL_NET_STACK"], 0),
        ("VjoControl thread (boot complete)", v["VJO_BUDGET_SHELL_CTL_STACK"], 0),
        ("game start: result memory (local: one arena)", 0, arena),
    ]
    if anki:
        out.append(("Anki card saved: job thread", v["VJO_BUDGET_SHELL_ANKI_STACK"], 0))
        out.append(("Anki card saved: job arena (text-only card)", 0, v["VJO_BUDGET_PAF_ANKI"]))
        out.append(("Anki job done", -v["VJO_BUDGET_SHELL_ANKI_STACK"], -v["VJO_BUDGET_PAF_ANKI"]))
    out.append(("game exit: result memory freed", 0, -arena))
    return out


def replay(v, samples, **kw):
    seq = steps(v, **kw)
    pages = paf = 0
    rows = []
    for name, dp, dq in seq:
        pages += dp
        paf += dq
        rows.append({"step": name, "pages": pages, "paf": paf})
    result = {"steps": rows, "scenarios": [], "peak_pages": max(r["pages"] for r in rows), "peak_paf": max(r["paf"] for r in rows)}
    reserve = samples.get("paf_reserve", 1536 * 1024)
    for s in samples["samples"]:
        sc = {"id": s["id"], "date": s.get("date"), "source": s.get("source"), "kind": None, "steps": [], "notes": []}
        if s.get("shell_free") is not None:
            sc["kind"] = "shell pages"
            op = s.get("overlay_pages")
            if op:
                held = op["shell_module"] + op["worker_module"] + op["stacks_tls"]
                sc["notes"].append(f"taken with {s['overlay_build']} holding {held:,} B of SceShell pages")
            else:
                held = None
                sc["notes"].append(f"taken with {s['overlay_build']}; its footprint is unknown, so the free figure is used as is "
                                   "(the real headroom was larger by that footprint) [estimate]")
            base = s["shell_free"] + (held or 0)
            sc["without_us"] = base
            sc["sampled_footprint"] = held
            worst = None
            for r in rows:
                h = base - r["pages"]
                sc["steps"].append({"step": r["step"], "held": r["pages"], "headroom": h})
                worst = h if worst is None else min(worst, h)
            sc["worst_headroom"] = worst
            if s.get("ownership", "").startswith("not verified"):
                sc["notes"].append("the sample's ownership (SceShell) was not verified")
            if s.get("other_occupants"):
                sc["notes"].append(f"other occupants: {s['other_occupants']}")
            sc["verdict"] = ("would run out of pages (negative headroom)" if worst < 0 else
                             f"within one 64 KiB change of running out ({worst:,} B)" if worst < WARN else
                             f"{worst:,} B of headroom at the worst step")
        elif s.get("paf_free") is not None:
            sc["kind"] = "shell paf"
            held = v["VJO_BUDGET_PAF_PLUGIN"] if s.get("paf_held_by_overlay", "").startswith("the Paf plugin") else 0
            base = s["paf_free"] + held
            sc["without_us"] = base
            worst = None
            for r in rows:
                h = base - r["paf"] - reserve
                sc["steps"].append({"step": r["step"], "held": r["paf"], "headroom_over_reserve": h})
                worst = h if worst is None else min(worst, h)
            sc["worst_headroom"] = worst
            sc["notes"].append(f"PAF allocations are refused by the runtime guard when free would drop under the {reserve:,} B reserve; "
                               "a refusal is survived (host tests), not a crash")
            if s.get("paf_held_by_overlay"):
                sc["notes"].append(f"held then: {s['paf_held_by_overlay']}")
            sc["verdict"] = ("some allocation would be refused (headroom over the reserve negative)" if worst < 0 else
                             f"{worst:,} B over the reserve at the worst step")
        result["scenarios"].append(sc)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--budget", type=Path, default=BUDGET)
    ap.add_argument("--samples", type=Path, default=SAMPLES)
    ap.add_argument("--shell-pages", type=int, help="model a build whose Shell module has this many bytes of pages")
    ap.add_argument("--worker-pages", type=int, help="model a build whose OCR worker holds this many bytes in SceShell "
                    "(an *ALL registration; 0: not loaded there, or NO_RESIDENT)")
    ap.add_argument("--anki", action="store_true", help="include an Anki card save (text-only: no screenshot)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    v = parse_budget(args.budget)
    samples = json.loads(args.samples.read_text())
    r = replay(v, samples, shell_pages=args.shell_pages, worker_pages=args.worker_pages, anki=args.anki)
    if args.json:
        print(json.dumps(r, indent=2))
        return 0
    print("Shell budget replay (model, not a measurement): pages held in SceShell's partition and bytes held in its PAF heap")
    print(f"  {'step':52} {'pages held':>12} {'paf held':>10}")
    for s in r["steps"]:
        print(f"  {s['step']:52} {s['pages']:>12,} {s['paf']:>10,}")
    print(f"  peak pages {r['peak_pages']:,} B, peak paf {r['peak_paf']:,} B")
    for sc in r["scenarios"]:
        print(f"\nscenario {sc['id']} ({sc['date']}, {sc['kind']}): {sc['source']}")
        for n in sc["notes"]:
            print(f"  note: {n}")
        if sc["kind"] == "shell pages":
            print(f"  SceShell free without the overlay: {sc['without_us']:,} B"
                  + (f" (sample {sc['without_us'] - sc['sampled_footprint']:,} + sampled build {sc['sampled_footprint']:,})" if sc["sampled_footprint"] else ""))
            for st in sc["steps"]:
                flag = "  <-- out of pages" if st["headroom"] < 0 else "  <-- within one change" if st["headroom"] < WARN else ""
                print(f"    {st['step']:52} held {st['held']:>9,}  headroom {st['headroom']:>9,}{flag}")
        else:
            print(f"  PAF free without the overlay: {sc['without_us']:,} B")
            for st in sc["steps"]:
                flag = "  <-- refused" if st["headroom_over_reserve"] < 0 else ""
                print(f"    {st['step']:52} held {st['held']:>9,}  over reserve {st['headroom_over_reserve']:>9,}{flag}")
        print(f"  verdict: {sc['verdict']}")
    print("\nThe model ranks builds; only the console decides (docs/offline-testing.md).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
