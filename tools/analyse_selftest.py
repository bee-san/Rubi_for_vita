#!/usr/bin/env python3
"""Analyse an on-device self-test report (docs/self-test.md).

    python3 tools/analyse_selftest.py build/device/status/selftest-report.txt [--kernel kernel.txt] [--json]

Says how far the run got (boot, ok, game, ocr, done), what every pool had
free at each step, and what failed, in plain words. Exit status 0 when the
run reached "done" with a successful OCR job, 1 when it did not, 2 for a
file that is not a report.
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
FULL_FRAME = 960 * 544 * 4           # the kernel capture block at the largest region
STEPS = ("boot", "ok", "game", "ocr", "done")

# VJO_E_* of core/conn.h, as the game result reports them (read from the
# source when it is at hand, so the two cannot drift).
RC_NAMES = {0: "ok", -100: "connect", -101: "tls", -102: "http", -103: "out of memory", -104: "parse",
            -105: "too large", -106: "status", -107: "no key", -108: "source", -109: "anki", -110: "not found",
            -111: "anki duplicate", -112: "no host", -113: "local io", -114: "local format", -115: "ocr model",
            -116: "ocr region", -117: "ocr unavailable"}
KERNEL_ERRORS = {0xBFFFFFFF: "no game", 0xBFFFFFFE: "no memory", 0xBFFFFFFD: "busy", 0xBFFFFFFC: "timeout",
                 0xBFFFFFFB: "format", 0xBFFFFFFA: "argument", 0xBFFFFFF9: "copy", 0xBFFFFFF7: "cancelled",
                 0xBFFFFFF8: "permission", 0x80024302: "NO_FREE_PHYSICAL_PAGE (the partition is out of pages)"}


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
    """Lines of `kind [step] key=value ...` into records."""
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


def analyse(text):
    load_rc_names()
    records = parse(text)
    if not records or records[0]["kind"] != "selftest":
        raise ValueError("not a self-test report (first line must start with 'selftest')")
    out = {"api": records[0]["fields"].get("api"), "steps": [], "samples": {}, "game": None, "ocr": None,
           "fails": [], "findings": [], "verdict": None, "reached": None}
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
    reached = [s["step"] for s in out["steps"]]
    out["reached"] = reached[-1] if reached else None
    findings = out["findings"]

    boot = out.get("boot")
    if boot:
        if boot.get("mode") == 1:
            findings.append("This was a SAFE boot: the previous boot did not complete, the plugins stayed inert. "
                            "The report has only the boot sample; reboot once more to test normally.")
        if boot.get("failures", 0):
            findings.append(f"{boot['failures']} normal boot(s) in a row had failed before this one.")
        if boot.get("write", 0) & 0x80000000:
            findings.append(f"boot.txt was not writable (0x{boot['write']:08X}): the boot guard cannot remember this boot.")

    for step in STEPS:
        s = out["samples"].get(step)
        if not s:
            continue
        mem, kern = s.get("mem", {}), s.get("kernel", {})
        main = mem.get("shell_main")
        if isinstance(main, int) and main >= 0:
            if main < SHELL_LOW:
                findings.append(f"At {step}: SceShell's partition had only {main:,} B free (user view). This is the "
                                f"headroom a firmware thread needs for its next page; #59's crash happened in this state.")
        elif mem:
            findings.append(f"At {step}: sceKernelGetFreeMemorySize failed (0x{mem.get('rc', 0):08X}).")
        paf = mem.get("paf_free")
        if isinstance(paf, int) and 0 < paf < PAF_RESERVE:
            findings.append(f"At {step}: the PAF heap had {paf:,} B free, under the {PAF_RESERVE:,} B reserve: every "
                            f"Shell result allocation is refused (dictionary results, Anki).")
        if kern and "words" in kern and kern["words"] and kern["words"][0] == "unavailable":
            findings.append(f"At {step}: the kernel memory sample was unavailable (rc 0x{kern.get('rc', 0):08X}).")
        elif kern:
            if kern.get("kd_rc", 0) == 0:
                if kern.get("kd_total") != KD_TOTAL:
                    findings.append(f"At {step}: the kernel module's partition reads {kern.get('kd_total', 0):,} B total, "
                                    f"not the 36 MiB of ScePhyMemPartKD: treat kd_free as unconfirmed.")
            else:
                findings.append(f"At {step}: the kernel partition figure was unavailable "
                                f"({KERNEL_ERRORS.get(kern.get('kd_rc', 0) & 0xFFFFFFFF, hex(kern.get('kd_rc', 0) & 0xFFFFFFFF))}).")
            if kern.get("shell_rc", 0) == 0 and kern.get("shell_total") not in (None, SHELL_TOTAL):
                findings.append(f"At {step}: the Shell's partition reads {kern['shell_total']:,} B total, not 50 MiB.")

    g = out["game"]
    if g is not None and g.get("enabled") == 0:
        findings.append(f"The game {g.get('title')} is disabled in games.ini: no OCR ran. Enable it in the Overlay app (JP Overlay Settings in older builds) and run again.")
    for fl in out["fails"]:
        rc = fl.get("rc", 0) & 0xFFFFFFFF
        findings.append(f"{fl['what']} failed: {KERNEL_ERRORS.get(rc, hex(rc))}"
                        + (" — the kernel could not allocate the capture block in ScePhyMemPartKD." if fl["what"] == "capture" and rc == 0xBFFFFFFE else ""))
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
            findings.append(f"The capture reported 0x{o['capture_rc'] & 0xFFFFFFFF:08X}: {KERNEL_ERRORS.get(o['capture_rc'] & 0xFFFFFFFF, '?')}.")

    # the memory the run added, boot -> game -> ocr
    def delta(step_a, step_b, key, group="mem"):
        a = out["samples"].get(step_a, {}).get(group, {}).get(key)
        b = out["samples"].get(step_b, {}).get(group, {}).get(key)
        return b - a if isinstance(a, int) and isinstance(b, int) and a >= 0 and b >= 0 else None
    d = delta("boot", "game", "shell_main")
    if d is not None:
        findings.append(f"SceShell's free partition changed by {d:+,} B between boot and the game start (firmware and us together).")
    d = delta("game", "ocr", "paf_free")
    if d is not None:
        findings.append(f"The PAF heap changed by {d:+,} B between the game start and the OCR result (our result arenas are in it).")
    d = delta("game", "ocr", "kd_free", "kernel")
    if d is not None:
        findings.append(f"The kernel partition changed by {d:+,} B between the game start and the OCR result "
                        f"(the capture block is {out['samples'].get('ocr', {}).get('kernel', {}).get('capture', 0):,} B when held).")

    done = "done" in reached and o is not None and o.get("rc") == 0
    if done:
        out["verdict"] = "PASS: boot, game start and an OCR job at the full screen completed; see the figures above."
    elif "done" in reached:
        out["verdict"] = "FAIL: the run completed but the OCR job did not succeed."
    elif out["reached"] is None:
        out["verdict"] = "FAIL: the Shell plugin never wrote a step: it did not start (see status.txt and kernel.txt)."
    else:
        nxt = {"boot": "the stable minute (ok)", "ok": "a game start", "game": "an OCR result (the first settled text screen)",
               "ocr": "done"}.get(out["reached"], "?")
        out["verdict"] = (f"INCOMPLETE: the run reached '{out['reached']}' and never wrote {nxt}. "
                          + ("If the Vita crashed or hung there, the step after it is where to look." if out["reached"] != "boot"
                             else "Either the console was off within the first minute, or it hung or crashed before it."))
    return out


def fmt_sample(step, s):
    mem, led, kern = s.get("mem", {}), s.get("ledger", {}), s.get("kernel", {})
    lines = []
    if mem:
        main = mem.get("shell_main")
        lines.append(f"    SceShell free (user view): main {main:,} B, CDRAM {mem.get('shell_cdram', 0):,} B, "
                     f"phycont {mem.get('shell_phycont', 0):,} B; PAF heap free {mem.get('paf_free', 0):,} B"
                     if isinstance(main, int) and main >= 0 else f"    SceShell free: unavailable (rc 0x{mem.get('rc', 0):08X})")
    if led:
        p, q = led.get("pages", (0, 0)), led.get("paf", (0, 0))
        if isinstance(p, tuple) and isinstance(q, tuple):
            lines.append(f"    Shell ledger: pages {p[0]:,} B (peak {p[1]:,}), PAF {q[0]:,} B (peak {q[1]:,})")
    if kern:
        if kern.get("words") and kern["words"][0] == "unavailable":
            lines.append(f"    kernel: sample unavailable (rc 0x{kern.get('rc', 0):08X})")
        else:
            kd = (f"KD {kern.get('kd_free', 0):,} of {kern.get('kd_total', 0):,} B free" if kern.get("kd_rc", 0) == 0
                  else f"KD unavailable (0x{kern.get('kd_rc', 0) & 0xFFFFFFFF:08X})")
            sh = (f"SceShell {kern.get('shell_free', 0):,} of {kern.get('shell_total', 0):,} B free (exact)" if kern.get("shell_rc", 0) == 0
                  else f"SceShell exact figure unavailable (0x{kern.get('shell_rc', 0) & 0xFFFFFFFF:08X})")
            led_c = kern.get("ledger", (0, 0))
            lines.append(f"    kernel: {kd}; {sh}; kernel ledger {led_c[0]:,} B (peak {led_c[1]:,}, limit {kern.get('limit', 0):,}), "
                         f"capture block {kern.get('capture', 0):,} B" if isinstance(led_c, tuple) else f"    kernel: {kd}; {sh}")
    return lines


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
        print(f"self-test report, API {result['api']}")
        for s in result["steps"]:
            extra = " ".join(f"{k}={v}" for k, v in s.items() if k not in ("step", "t"))
            print(f"  step {s['step']:5} at {s.get('t', '?')} ms {extra}")
            for line in fmt_sample(s["step"], result["samples"].get(s["step"], {})):
                print(line)
        for f in result["fails"]:
            print(f"  fail {f}")
        print("findings:")
        for f in result["findings"]:
            print(f"  - {f}")
        print(result["verdict"])
    return 0 if result["verdict"].startswith("PASS") else 1


if __name__ == "__main__":
    sys.exit(main())
