#!/usr/bin/env python3
"""Analyse an on-device self-test report (docs/self-test.md).

    python3 tools/analyse_selftest.py build/device/status/selftest-report.txt [--json]

Prints a PASS/FAIL table with one row per stage of the run, the free memory
every memory route read at each step, the game worker's own memory around
each allocation, and what failed, in plain words.

v2 reports (API 19, `selftest v2`): boot, ok, game, then the stages one OCR job
runs on the first settled screen (probe, fallback, textbox, fullscreen,
textbox2), then done. v1 reports (API 18, `selftest v1`, as #74 sent) get the
same table for their steps (boot, ok, game, ocr, done).

Exit status: 0 when every row passed, 1 when one failed or the run stopped
early, 2 for a file that is not a report.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

KD_TOTAL = 36 * 1024 * 1024          # ScePhyMemPartKD on retail firmware (wiki Memory_budget)
SHELL_TOTAL = 50 * 1024 * 1024       # ScePhyMemPartShell
SHELL_LOW = 512 * 1024               # SceShell free main below this: a firmware thread's next page can fail (#59)
PAF_RESERVE = 1536 * 1024            # shell/memory.cpp RESERVE: below it every PAF request is refused
FULL_FRAME = 960 * 544 * 4           # API 18's kernel capture block at the largest region
CAPTURE_LIMIT = 64 * 1024            # API 19: a region's capture block (one strip and its row checksums)
STEPS_V1 = ("boot", "ok", "game", "ocr", "done")
STAGES = ("probe", "fallback", "textbox", "fullscreen", "textbox2")
ROWS_V2 = ("boot", "ok", "game") + STAGES + ("done",)
ROUTES = (("user", "user API (sceKernelGetFreeMemorySize)"), ("block kernel", "kernel block route, KD"),
          ("block shell", "kernel block route, SceShell"), ("cas", "address-space route (PSVshell)"),
          ("as", "kernel process route (faps-coredump)"))

# VJO_E_* of core/conn.h, as the game result reports them (read from the
# source when it is at hand, so the two cannot drift).
RC_NAMES = {0: "ok", -100: "connect", -101: "tls", -102: "http", -103: "out of memory", -104: "parse",
            -105: "too large", -106: "status", -107: "no key", -108: "source", -109: "anki", -110: "not found",
            -111: "anki duplicate", -112: "no host", -113: "local io", -114: "local format", -115: "ocr model",
            -116: "ocr region", -117: "ocr unavailable", -120: "screen changed"}
NATIVE = {0xBFFFFFFF: "no game", 0xBFFFFFFE: "no memory", 0xBFFFFFFD: "busy", 0xBFFFFFFC: "timeout",
          0xBFFFFFFB: "format", 0xBFFFFFFA: "argument", 0xBFFFFFF9: "copy", 0xBFFFFFF7: "cancelled",
          0xBFFFFFF8: "permission", 0x80024302: "NO_FREE_PHYSICAL_PAGE (the partition is out of pages)",
          0x80020001: "SCE_KERNEL_ERROR_ERROR (the call failed)", 0x80010002: "ENOENT", 0x80010013: "ENODEV",
          0x8001000D: "EACCES"}


def load_rc_names():
    """The VJO_E_* values from core/conn.h, if the source is at hand."""
    path = ROOT / "core/conn.h"
    if not path.is_file():
        return
    for m in re.finditer(r"VJO_E_([A-Z_]+)\s*=\s*(-\d+)", path.read_text(errors="replace")):
        RC_NAMES.setdefault(int(m.group(2)), m.group(1).lower().replace("_", " "))


def parse_value(v):
    if re.fullmatch(r"-?\d+", v):
        return int(v)
    if re.fullmatch(r"0x[0-9A-Fa-f]+", v):
        return int(v, 16)
    if re.fullmatch(r"\d+/\d+", v):
        a, b = v.split("/")
        return (int(a), int(b))
    return v


def parse(text):
    """Lines of `kind [words] key=value ...` into records."""
    records = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        tokens = line.split()
        kind = tokens[0]
        fields, words = {}, []
        for t in tokens[1:]:
            if "=" in t:
                k, v = t.split("=", 1)
                fields[k] = parse_value(v)
            else:
                words.append(t)
        records.append({"line": n, "kind": kind, "words": words, "fields": fields, "raw": line})
    return records


def u32(v):
    return v & 0xFFFFFFFF if isinstance(v, int) else 0


def s32(v):
    v = u32(v)
    return v - (1 << 32) if v & 0x80000000 else v


def native(v):
    v = u32(v)
    return f"0x{v:08X}" + (f" {NATIVE[v]}" if v in NATIVE else "")


def rc_text(rc):
    rc = s32(rc)
    if rc in RC_NAMES:
        return f"{RC_NAMES[rc]} (rc {rc})"
    return native(rc)


def mib(v):
    return f"{v:,} B"


# ---------------------------------------------------------------- v2 (API 19)

def analyse_v2(records):
    out = {"version": 2, "api": records[0]["fields"].get("api"), "steps": [], "samples": {}, "stages": {},
           "rows": [], "routes": [], "game_memory": [], "findings": [], "verdict": None}
    stages = out["stages"]
    for r in records:
        k, w, f = r["kind"], r["words"], r["fields"]
        if k in ("boot", "config"):
            out[k] = f
        elif k == "step" and w:
            out["steps"].append({"step": w[0], **f})
        elif k in ("mem", "ledger", "kernel") and w:
            out["samples"].setdefault(w[0], {})[k] = f
        elif k == "kmem" and w:
            route = f.get("route", "?") + (f" {f['part']}" if f.get("route") == "block" else "")
            out["samples"].setdefault(w[0], {}).setdefault("kmem", {})[route] = f
        elif k == "stage" and len(w) >= 2:
            s = stages.setdefault(w[0], {"gsteps": [], "lines": []})
            s[w[1]] = f
        elif k == "gstep" and len(w) >= 2:
            stages.setdefault(w[0], {"gsteps": [], "lines": []})["gsteps"].append({"step": w[1], **f})
        elif k in ("result", "memory") and w:
            stages.setdefault(w[0], {"gsteps": [], "lines": []})[k] = f
        elif k == "line" and w:
            stages.setdefault(w[0], {"gsteps": [], "lines": []})["lines"].append(f)
    steps = {s["step"]: s for s in out["steps"]}
    findings = out["findings"]
    rows = out["rows"]

    def row(name, ok, detail):
        rows.append({"row": name, "result": ok, "detail": detail})

    boot = out.get("boot")
    if "boot" not in steps:
        row("boot", None, "no boot step: the Shell plugin never wrote one (see status.txt and kernel.txt)")
    elif boot and boot.get("mode") == 1:
        row("boot", "FAIL", "SAFE boot: the previous boot did not complete, the plugins stayed inert; reboot once")
    else:
        row("boot", "PASS", f"normal boot (seq {boot.get('seq', '?') if boot else '?'}, "
                            f"{boot.get('failures', 0) if boot else 0} failed boots before)")
    row("ok", "PASS" if "ok" in steps else None,
        f"stable minute at {steps['ok'].get('t', '?')} ms" if "ok" in steps else "not reached")
    g = steps.get("game")
    if not g:
        row("game", None, "not reached: no enabled game was started")
    elif g.get("enabled") == 0:
        row("game", "FAIL", f"{g.get('title')} is disabled in games.ini: no OCR ran")
    else:
        row("game", "PASS", f"{g.get('title')} enabled, region {g.get('region')}"
                            + (f" {g.get('rect')}" if g.get("region") == "user" else " (full screen)"))

    def failing_steps(s):
        bad = []
        for gs in s["gsteps"]:
            rc = s32(gs.get("rc", 0))
            if rc < 0:
                bad.append(f"{gs['step']} {gs.get('kind')}/{gs.get('arg')} {native(rc)}")
        return bad

    recognized = 0
    for name in STAGES:
        s = stages.get(name)
        if not s or "begin" not in s:
            row(name, None, "not reached")
            continue
        end, res, mem = s.get("end"), s.get("result", {}), s.get("memory", {})
        if end is None:
            row(name, "FAIL", f"began at {s['begin'].get('t', '?')} ms and has no record: the Vita stopped "
                              f"(crash, hang or power) during this stage")
            continue
        rc = s32(end.get("rc", 0))
        bad = failing_steps(s)
        if name == "probe":
            paths = [gs for gs in s["gsteps"] if gs["step"] == "probe"]
            ok_paths = [f"{gs.get('kind')}->{gs.get('arg')}" for gs in paths if s32(gs.get("rc", 0)) > 0]
            failed = [f"{gs.get('kind')}->{gs.get('arg')} {native(gs.get('rc'))}" for gs in paths
                      if s32(gs.get("rc", 0)) <= 0]
            refused = [f"{gs.get('kind')} {native(gs.get('rc'))}" for gs in s["gsteps"]
                       if gs["step"] == "alloc" and s32(gs.get("rc", 0)) < 0]
            primary = "hook->user" in ok_paths
            detail = (f"worked: {', '.join(ok_paths) or 'none'}"
                      + (f"; failed: {', '.join(failed)}" if failed else "")
                      + (f"; memory refused: {', '.join(refused)}" if refused else ""))
            row(name, "PASS" if primary else "FAIL", detail if primary else
                "the display hook could not write the game's USER memory: " + detail)
            continue
        if rc != 0:
            row(name, "FAIL", f"{rc_text(rc)}" + (f"; failed steps: {'; '.join(bad)}" if bad else "")
                + (f"; free USER {mem.get('free_user'):,} B" if isinstance(mem.get("free_user"), int) else ""))
            continue
        recognized += end.get("recognized", 0)
        region = end.get("region", "?")
        detail = (f"{region} {end.get('model')}, {end.get('lines', 0)} line(s), "
                  f"capture {res.get('source', '?')} into {res.get('mem', '?')} {res.get('capture', 0):,} B "
                  f"({res.get('strips', 0)} strips of {res.get('strip_rows', 0)} rows, {res.get('tears', 0)} rows "
                  f"changed), weights {res.get('weights', '?')}"
                  + (f" in {res.get('load_ms')} ms" if res.get("weights") == "read" else "")
                  + f", recognize {res.get('recognize_ms', '?')} ms")
        verdict = "PASS"
        if name == "fallback" and not end.get("fallback"):
            verdict, detail = "FAIL", "the fallback model was not used: " + detail
        if name == "fullscreen":
            m = re.match(r"(\d+)x(\d+)", str(region))
            fb = re.match(r"(\d+)x(\d+)", str(end.get("fb", "")))
            if m and fb and m.groups() != fb.groups():
                verdict, detail = "FAIL", "the region was not the whole frame: " + detail
        row(name, verdict, detail)

    # the run's own requirements, one row each
    regions = [(n, stages[n]) for n in STAGES if n != "probe" and n in stages and stages[n].get("end")
               and s32(stages[n]["end"].get("rc", 0)) == 0]
    if regions:
        caps = [s.get("result", {}).get("capture", 0) for _, s in regions]
        tall = [(n, gs.get("bytes", 0)) for n, s in regions for gs in s["gsteps"]
                if gs["step"] == "alloc" and gs.get("arg") == "line" and s32(gs.get("rc", 0)) >= 0]
        row("capture<64KiB", "PASS" if max(caps) <= CAPTURE_LIMIT else "FAIL",
            f"largest capture block {max(caps):,} B (limit {CAPTURE_LIMIT:,} B)"
            + (f"; plus a line taller than a strip, kept whole: {', '.join(f'{b:,} B in {n}' for n, b in tall)}"
               if tall else ""))
        kstrip = [n for n, s in regions if s.get("result", {}).get("source") == "kstrip"]
        row("kernel-pool", "PASS" if not kstrip else "WARN",
            "every capture was written by the display hook into the game's memory" if not kstrip else
            f"the kernel strip (<= 64 KiB of the kernel pool) was used in: {', '.join(kstrip)}")
        row("recognitions", "PASS" if recognized >= 3 else "FAIL", f"{recognized} line(s) recognized in all")
        tears = sum(s.get("result", {}).get("tears", 0) for _, s in regions)
        row("tearing", "PASS", f"{tears} row(s) changed between two reads (the job reads again)")

    # memory routes at each step
    for step, sample in out["samples"].items():
        cells = {}
        mem = sample.get("mem")
        if mem is not None:
            rc = u32(mem.get("rc", 0))
            if rc:
                cells["user"] = ("FAIL", native(rc))
            elif mem.get("valid") == 0 or (isinstance(mem.get("main"), int) and mem["main"] < 0):
                cells["user"] = ("PASS", f"main {mem.get('main')} (under the margin: 0 B free above it)")
            else:
                cells["user"] = ("PASS", f"main {mem.get('main', 0):,} B")
        for route, f in sample.get("kmem", {}).items():
            rc = u32(f.get("rc", 0))
            if route == "none":
                cells["kernel"] = ("FAIL", f"vjoMemInfo {native(rc)}")
            elif rc:
                cells[route] = ("FAIL", native(rc) + (f" at step {f.get('step')}" if f.get("step") else ""))
            elif route.startswith("block"):
                total = f.get("total", 0)
                cells[route] = ("PASS" if total else "FAIL", f"{f.get('free', 0):,} of {total:,} B free")
            elif route == "cas":
                main = f.get("main", (0, 0))
                cells[route] = ("PASS" if isinstance(main, tuple) and main[1] else "FAIL",
                                f"main {main[0]:,} of {main[1]:,} B" if isinstance(main, tuple) else str(main))
            elif route == "as":
                parts = [f[k] for k in sorted(f) if re.fullmatch(r"p\d+", k)]
                kd = [p for p in parts if isinstance(p, tuple) and p[1] == KD_TOTAL]
                cells[route] = ("PASS" if f.get("parts", 0) else "FAIL",
                                f"{f.get('parts', 0)} partitions" + (f", KD {kd[0][0]:,} B free" if kd else ""))
        out["routes"].append({"step": step, "cells": cells})

    # memory queries: the user API and at least one kernel route reading ScePhyMemPartKD, at every step
    if out["routes"]:
        missing, notes = [], set()
        for r in out["routes"]:
            c = r["cells"]
            kd = c.get("block kernel", ("FAIL",))[0] == "PASS" or (c.get("as", ("FAIL", ""))[0] == "PASS"
                                                                   and "KD" in c["as"][1])
            if c.get("user", ("FAIL",))[0] != "PASS" or not kd:
                missing.append(r["step"])
            notes.update(f"{n} {d}" for n, (ok, d) in c.items() if ok == "FAIL")
        row("kernel-memory", "PASS" if not missing else "FAIL",
            ("the user API and a KD figure at every step" if not missing else
             f"no KD figure or no user figure at: {', '.join(missing)}")
            + (f"; failed routes: {'; '.join(sorted(notes))}" if notes else ""))
    gmem = [gs for n in STAGES for gs in stages.get(n, {}).get("gsteps", []) if gs["step"] == "mem"]
    if gmem:
        good = [gs for gs in gmem if s32(gs.get("rc", 0)) >= 0 and isinstance(gs.get("main"), int) and gs["main"] >= 0]
        row("game-memory", "PASS" if good else "FAIL",
            f"free USER {good[0]['main']:,} B, CDRAM {good[0].get('cdram', 0):,} B in the game before its first block"
            if good else f"the game's free memory could not be read ({native(gmem[0].get('rc', 0))})")

    row("done", "PASS" if "done" in steps else None,
        f"at {steps['done'].get('t', '?')} ms, the marker removed" if "done" in steps else "not reached")

    # the game worker's memory around each allocation
    for name in STAGES:
        s = stages.get(name)
        for gs in (s or {}).get("gsteps", []):
            if gs["step"] in ("mem", "alloc", "free"):
                out["game_memory"].append({"stage": name, **gs})

    # findings
    if out.get("config"):
        c = out["config"]
        findings.append(f"Configured {c.get('model')} with fallback {c.get('fallback')}.")
    for step, sample in out["samples"].items():
        paf = sample.get("mem", {}).get("paf_free")
        if isinstance(paf, int) and 0 < paf < PAF_RESERVE:
            findings.append(f"At {step}: the PAF heap had {paf:,} B free, under the {PAF_RESERVE:,} B reserve: every "
                            f"Shell result allocation is refused (dictionary results, Anki).")
        kmem = sample.get("kmem", {})
        if all(u32(f.get("rc", 0)) for f in kmem.values()) and kmem:
            findings.append(f"At {step}: no kernel memory route read a figure.")
    for name in STAGES:
        mem = stages.get(name, {}).get("memory", {})
        if mem.get("cleanup"):
            findings.append(f"{name}: the game worker could not free its memory (cleanup set): it blocks further jobs.")
        io = u32(mem.get("io", 0))
        if io:
            findings.append(f"{name}: model I/O error {native(io)}.")

    begun = [n for n in STAGES if n in stages and "begin" in stages[n] and stages[n].get("end") is None]
    failed = [r["row"] for r in rows if r["result"] == "FAIL"]
    if begun:
        out["verdict"] = (f"INCOMPLETE: stage '{begun[0]}' began and never wrote its record: the Vita stopped during "
                          f"it. The rows before it are what was measured.")
    elif "done" not in steps:
        reached = [r["row"] for r in rows if r["result"] is not None]
        out["verdict"] = (f"INCOMPLETE: the run reached '{reached[-1] if reached else 'nothing'}' and never wrote "
                          f"'done'. If the Vita crashed or hung, look at the step after it.")
    elif failed:
        out["verdict"] = f"FAIL: {', '.join(failed)} failed; see the rows above."
    else:
        out["verdict"] = "PASS: every stage completed; the figures above are this console's."
    return out


# ---------------------------------------------------------------- v1 (API 18)

def analyse_v1(records):
    out = {"version": 1, "api": records[0]["fields"].get("api"), "steps": [], "samples": {}, "game": None,
           "ocr": None, "fails": [], "findings": [], "rows": [], "routes": [], "verdict": None}
    for r in records:
        k, w, f = r["kind"], r["words"], r["fields"]
        if k == "boot":
            out["boot"] = f
        elif k == "config":
            out["config"] = f
        elif k == "step":
            step = w[0] if w else "?"
            for word in w[1:]:  # the ocr step's bare "WxH"
                m = re.fullmatch(r"(\d+)x(\d+)", word)
                if m:
                    f["width"], f["height"] = int(m.group(1)), int(m.group(2))
            out["steps"].append({"step": step, **f})
            if step == "game":
                out["game"] = f
            elif step == "ocr":
                out["ocr"] = dict(f)
        elif k in ("mem", "ledger", "kernel") and w:
            out["samples"].setdefault(w[0], {})[k] = {**f, "words": w[1:]}
        elif k == "game":
            out["ocr"] = {**(out["ocr"] or {}), **f}
        elif k == "fail":
            out["fails"].append({"what": w[0] if w else "?", **f})
    steps = [s["step"] for s in out["steps"]]
    progress = [s for s in steps if s in STEPS_V1]
    out["reached"] = progress[-1] if progress else None
    findings, rows = out["findings"], out["rows"]

    def row(name, ok, detail):
        rows.append({"row": name, "result": ok, "detail": detail})

    boot = out.get("boot")
    if boot:
        if boot.get("mode") == 1:
            findings.append("This was a SAFE boot: the previous boot did not complete, the plugins stayed inert. "
                            "The report has only the boot sample; reboot once more to test normally.")
        if boot.get("failures", 0):
            findings.append(f"{boot['failures']} normal boot(s) in a row had failed before this one.")
        if boot.get("write", 0) & 0x80000000:
            findings.append(f"boot.txt was not writable (0x{boot['write']:08X}): the boot guard cannot remember this boot.")
    if "boot" in steps:
        row("boot", "FAIL" if boot and boot.get("mode") == 1 else "PASS",
            "SAFE boot" if boot and boot.get("mode") == 1 else f"normal boot (seq {boot.get('seq') if boot else '?'})")
    else:
        row("boot", None, "no boot step")
    row("ok", "PASS" if "ok" in steps else None, "stable minute" if "ok" in steps else "not reached")
    g = out["game"]
    if g is None:
        row("game", None, "not reached")
    elif g.get("enabled") == 0:
        row("game", "FAIL", f"{g.get('title')} is disabled in games.ini: no OCR ran")
        findings.append(f"The game {g.get('title')} is disabled in games.ini: no OCR ran. Enable it in the Overlay app "
                        f"(JP Overlay Settings in older builds) and run again.")
    else:
        row("game", "PASS", f"{g.get('title')} enabled, region {g.get('region')}")

    captures = [fl for fl in out["fails"] if fl["what"] == "capture"]
    counts = {}
    for fl in out["fails"]:
        key = (fl["what"], u32(fl.get("rc", 0)))
        counts[key] = counts.get(key, 0) + 1
    for (what, rc), n in counts.items():
        findings.append(f"{what} failed{f' {n} times' if n > 1 else ''}: {NATIVE.get(rc, hex(rc))}"
                        + (" — the kernel could not allocate the capture block in ScePhyMemPartKD." if what == "capture" and rc == 0xBFFFFFFE else ""))
    if captures and all(u32(c.get("rc", 0)) == 0xBFFFFFFE for c in captures):
        findings.append(f"The capture failed {len(captures)} time(s) with no memory: API 18 asks the kernel for a "
                        f"{FULL_FRAME:,} B capture buffer at the full screen, and its pool refused it (#74 measured "
                        f"0x80024302, NO_FREE_PHYSICAL_PAGE, each time). API 19 takes no kernel buffer for OCR: the game "
                        f"reads its own screen in strips.")
        if "done" not in steps:
            findings.append("API 18's self-test never finished after a failed capture: it stayed armed and kept forcing "
                            "the full-screen region, so a smaller region could not be tried in this run (fixed in API 19).")
    o = out["ocr"]
    if o is not None:
        rc = o.get("rc", 0)
        name = RC_NAMES.get(rc, str(rc))
        if rc == 0:
            findings.append(f"OCR ran in the game at {o.get('width', '?')}x{o.get('height', '?')} with {o.get('model')}"
                            + (" (the fallback model)" if o.get("fallback") else "")
                            + f": {o.get('recognized', 0)} line(s) recognized, {o.get('recognize_ms', '?')} ms recognizing.")
        else:
            findings.append(f"The OCR job failed: {name} (rc {rc}).")
        fu = o.get("free_user")
        if isinstance(fu, int) and o.get("reserve") is not None:
            need = o.get("need_workspace", 0) + o.get("need_weights", 0) + o.get("reserve", 0) - o.get("release", 0)
            verdict = "passed" if fu >= need else "refused"
            findings.append(f"The game had {fu:,} B of USER free; the job needed {need:,} B "
                            f"(workspace {o.get('need_workspace', 0):,} + weights {o.get('need_weights', 0):,} + reserve "
                            f"{o.get('reserve', 0):,}): check {verdict}.")
            if fu < need and rc == -103:
                findings.append("Out of memory in the GAME, not in SceShell: a smaller model or a tighter region fits; "
                                "this title cannot run the configured one.")
        if o.get("io", 0):
            io = o["io"] & 0xFFFFFFFF
            findings.append(f"Model I/O error 0x{io:08X}" + (" (ENODEV: the game cannot see ux0; the kernel broker should have been used)" if io == 0x80010013 else "")
                            + (" (ENOENT: the model file is missing)" if io == 0x80010002 else ""))
        if o.get("cleanup", 0):
            findings.append("The game worker could not free its memory after the job (cleanup_status set): it blocks further jobs.")
        if o.get("capture_rc", 0):
            findings.append(f"The capture reported 0x{o['capture_rc'] & 0xFFFFFFFF:08X}: {NATIVE.get(o['capture_rc'] & 0xFFFFFFFF, '?')}.")
        row("ocr", "PASS" if rc == 0 else "FAIL", f"{RC_NAMES.get(rc, rc)} at {o.get('width', '?')}x{o.get('height', '?')}")
    elif captures:
        row("ocr", "FAIL", f"never reached: the capture failed {len(captures)} time(s) "
                           f"({NATIVE.get(u32(captures[0].get('rc', 0)), hex(u32(captures[0].get('rc', 0))))})")
    else:
        row("ocr", None, "not reached")
    row("done", "PASS" if "done" in steps else None, "reached" if "done" in steps else "not reached")

    for step in dict.fromkeys(s for s in steps):
        s = out["samples"].get(step)
        if not s:
            continue
        mem, kern = s.get("mem", {}), s.get("kernel", {})
        cells = {}
        main = mem.get("shell_main")
        if mem:
            if u32(mem.get("rc", 0)):
                cells["user"] = ("FAIL", native(mem.get("rc")))
                findings.append(f"At {step}: sceKernelGetFreeMemorySize failed ({native(mem.get('rc'))}).")
            elif isinstance(main, int) and main < 0:
                cells["user"] = ("PASS", f"main {main} (under the margin: 0 B free above it)")
            else:
                cells["user"] = ("PASS", f"main {main:,} B")
                if isinstance(main, int) and main < SHELL_LOW:
                    findings.append(f"At {step}: SceShell's partition had only {main:,} B free (user view). This is the "
                                    f"headroom a firmware thread needs for its next page; #59's crash happened in this state.")
        paf = mem.get("paf_free")
        if isinstance(paf, int) and 0 < paf < PAF_RESERVE:
            findings.append(f"At {step}: the PAF heap had {paf:,} B free, under the {PAF_RESERVE:,} B reserve: every "
                            f"Shell result allocation is refused (dictionary results, Anki).")
        if kern and kern.get("words") and kern["words"][0] == "unavailable":
            cells["kernel"] = ("FAIL", f"sample unavailable {native(kern.get('rc', 0))}")
            findings.append(f"At {step}: the kernel memory sample was unavailable (rc 0x{u32(kern.get('rc', 0)):08X}).")
        elif kern:
            if u32(kern.get("kd_rc", 0)) == 0:
                cells["block kernel"] = ("PASS", f"{kern.get('kd_free', 0):,} of {kern.get('kd_total', 0):,} B free")
                if kern.get("kd_total") != KD_TOTAL:
                    findings.append(f"At {step}: the kernel module's partition reads {kern.get('kd_total', 0):,} B total, "
                                    f"not the 36 MiB of ScePhyMemPartKD: treat kd_free as unconfirmed.")
            else:
                cells["block kernel"] = ("FAIL", native(kern.get("kd_rc")))
            if u32(kern.get("shell_rc", 0)) == 0:
                cells["block shell"] = ("PASS", f"{kern.get('shell_free', 0):,} of {kern.get('shell_total', 0):,} B free")
                if kern.get("shell_total") not in (None, SHELL_TOTAL):
                    findings.append(f"At {step}: the Shell's partition reads {kern['shell_total']:,} B total, not 50 MiB.")
            else:
                cells["block shell"] = ("FAIL", native(kern.get("shell_rc")))
        out["routes"].append({"step": step, "cells": cells})
    bad_routes = {name for r in out["routes"] for name, (ok, _) in r["cells"].items() if ok == "FAIL"}
    for name in sorted(bad_routes):
        rcs = sorted({c[1] for r in out["routes"] for n, c in r["cells"].items() if n == name and c[0] == "FAIL"})
        findings.append(f"The {name} memory route failed at every step it was tried ({', '.join(rcs)}): its zero "
                        f"figures are not measurements.")

    failed = [r["row"] for r in rows if r["result"] == "FAIL"]
    if "done" in steps and not failed:
        out["verdict"] = "PASS: boot, game start and an OCR job completed; see the figures above."
    elif failed:
        out["verdict"] = f"FAIL: {', '.join(failed)} failed; see the rows above."
    elif out["reached"] is None:
        out["verdict"] = "FAIL: the Shell plugin never wrote a step: it did not start (see status.txt and kernel.txt)."
    else:
        nxt = {"boot": "the stable minute (ok)", "ok": "a game start", "game": "an OCR result (the first settled text "
               "screen)", "ocr": "done"}.get(out["reached"], "done")
        out["verdict"] = (f"INCOMPLETE: the run reached '{out['reached']}' and never wrote {nxt}. "
                          + ("If the Vita crashed or hung there, the step after it is where to look." if out["reached"] != "boot"
                             else "Either the console was off within the first minute, or it hung or crashed before it."))
    return out


def analyse(text):
    load_rc_names()
    records = parse(text)
    if not records or records[0]["kind"] != "selftest":
        raise ValueError("not a self-test report (first line must start with 'selftest')")
    if records[0]["words"][:1] == ["v1"]:
        return analyse_v1(records)
    return analyse_v2(records)


def render(result):
    lines = [f"self-test report v{result['version']}, API {result['api']}", "",
             f"  {'stage':16} {'result':7} detail"]
    for r in result["rows"]:
        lines.append(f"  {r['row']:16} {r['result'] or '-':7} {r['detail']}")
    route_names = [n for n, _ in ROUTES] + ["kernel"]
    used = [n for n in route_names if any(n in r["cells"] for r in result["routes"])]
    if used:
        lines += ["", "memory routes (PASS: the route read a figure; FAIL: its native result)"]
        for r in result["routes"]:
            lines.append(f"  {r['step']}:")
            for n in used:
                if n in r["cells"]:
                    ok, detail = r["cells"][n]
                    label = dict(ROUTES).get(n, n)
                    lines.append(f"    {label:40} {ok:5} {detail}")
    if result.get("game_memory"):
        lines += ["", "game worker memory (free USER/CDRAM in the game after each step)"]
        for g in result["game_memory"]:
            rc = s32(g.get("rc", 0))
            lines.append(f"  {g['stage']:10} {g['step']:5} {str(g.get('arg')):9} {str(g.get('kind')):7} "
                         f"{g.get('bytes', 0):>9,} B rc={'ok' if rc >= 0 else native(rc):12} "
                         f"main={g.get('main', 0):,} cdram={g.get('cdram', 0):,}")
    lines += ["", "findings:"] + [f"  - {f}" for f in result["findings"]] + ["", result["verdict"]]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = analyse(args.report.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError) as e:
        print(f"analyse_selftest: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2, default=list))
    else:
        print(render(result))
    return 0 if result["verdict"].startswith("PASS") else 1


if __name__ == "__main__":
    sys.exit(main())
