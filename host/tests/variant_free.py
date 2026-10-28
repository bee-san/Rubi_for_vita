#!/usr/bin/env python3
"""Which host tests give the same result in every lane of the gate, derived from the build: their label variant_free.

    python3 host/tests/variant_free.py BUILD_DIR SOURCE_DIR CTEST OUT [--jobs N]

The gate and CI run the host suites three times: the debug build (every test), the normal build and the debug build
with the release models (`ctest -LE variant_free` both). A test labelled variant_free runs in the first only, so the
label must mean: its result cannot depend on VJO_DEBUG, nor on the model directories (VJO_VOCR_MODEL_DIR,
VJO_VOCR_GATE_TEST_DIR). Here it is derived, never listed by hand. A test qualifies when

- it runs programs of this build only: one of them, or a driver in host/tests/ (python3 host/tests/X.py) given them
  as arguments or in its environment; every other test (the tools' suites, the generators' checks, ...) keeps the
  labels CMake gave it;
- every translation unit of each program, and of each library linked into it (LINK_LIBRARIES, transitively, and
  $<TARGET_OBJECTS:...>), preprocesses to the same text with VJO_DEBUG=1 and VJO_DEBUG=0 on its own compile line
  (compile_commands.json), and to the same text with the model directories' definitions changed: so the program is
  the same in every lane;
- neither the test nor any of its programs is named inside an `if(...VJO_DEBUG...)` or an
  `if(...VJO_VOCR_MODEL_DIR...)` block of the host CMake files: one that the variant configures differently.

The programs' runtime inputs (fixtures, the dictionary, TMPDIR, the environment) are the same in every lane.

OUT is a ctest include file (host/tests/variant_labels adds it to TEST_INCLUDE_FILES) that adds variant_free to
each such test's labels; OUT.txt says, for every test, why it has the label or not. When the derivation cannot run
(no compile_commands.json, a preprocessor failure) it exits 1, which fails the build, and OUT labels nothing.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

LABEL = "variant_free"
MODEL_DEFINES = ("VJO_VOCR_MODEL_DIR", "VJO_VOCR_GATE_TEST_DIR")
PROBE = '"/vjo-variant-free-probe"'
# the CMake conditions that configure a target differently by variant or by models
CONDITION_WORDS = re.compile(r"\b(?:VJO_DEBUG|VJO_VOCR_MODEL_DIR|VJO_VOCR_GATE\w*)\b")


def cmake_commands(text):
    """(name, args) of each command in CMake text, in order: comments dropped, quoted arguments kept whole."""
    text = re.sub(r'#[^\n]*', '', re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: m.group(0).replace('#', '\0'), text))
    out, i = [], 0
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', text):
        if m.start() < i:
            continue
        depth, j, quoted = 1, m.end(), False
        while j < len(text) and depth:
            c = text[j]
            if c == '"' and text[j - 1] != '\\':
                quoted = not quoted
            elif not quoted:
                depth += c == '('
                depth -= c == ')'
            j += 1
        body = text[m.end():j - 1].replace('\0', '#')
        out.append((m.group(1).lower(), re.findall(r'"(?:[^"\\]|\\.)*"|[^\s()]+', body)))
        i = j
    return out


# The commands that configure the targets or tests named first in their arguments.
TARGET_COMMANDS = {"add_executable", "add_library", "add_dependencies"}
DIRECTORY_COMMANDS = {"add_compile_definitions", "add_definitions", "add_compile_options", "include_directories",
                      "link_libraries", "link_directories", "set_source_files_properties", "set_directory_properties"}


def configured_by_conditions(paths):
    """The targets and tests that an if() on VJO_DEBUG or on the models configures (CMakeLists.txt, cmake/*.cmake),
    and whether such an if() configures what every target compiles (then every test is lane-dependent). The if() that
    defines VJO_DEBUG itself for every unit (add_compile_definitions(VJO_DEBUG=...)) is what the units' comparison
    checks."""
    names, everything = set(), False
    for path in paths:
        inside = []  # the if()s around this command: whether each is on the variant or the models
        for name, args in cmake_commands(path.read_text(encoding="utf-8")):
            if name == "if":
                inside.append(bool(CONDITION_WORDS.search(" ".join(args))))
                continue
            if name == "endif" and inside:
                inside.pop()
                continue
            if name == "elseif" and inside and CONDITION_WORDS.search(" ".join(args)):
                inside[-1] = True
            if not any(inside):
                continue
            if name == "set_target_properties":
                names.update(args[:args.index("PROPERTIES")] if "PROPERTIES" in args else args)
            elif name in TARGET_COMMANDS or name.startswith("target_"):
                names.update(args[:1])
            elif name == "add_test":
                names.add(args[args.index("NAME") + 1] if "NAME" in args else args[0])
            elif name == "set_tests_properties":
                names.update(args[:args.index("PROPERTIES")] if "PROPERTIES" in args else args)
            elif name == "set_property" and args and args[0] in ("TARGET", "TEST"):
                for a in args[1:]:
                    if a in ("APPEND", "APPEND_STRING", "PROPERTY"):
                        break
                    names.add(a)
            elif name in DIRECTORY_COMMANDS and not all(re.fullmatch(r"(-D)?VJO_DEBUG=[01]", a) for a in args):
                everything = True
    return names, everything


def compile_args(entry):
    args = entry.get("arguments") or shlex.split(entry["command"])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD", "-MMD") or a.startswith(("-MF", "-MT", "-MQ")):
            continue
        out.append(a)
    return out


def preprocess(args, directory):
    done = subprocess.run(args + ["-E", "-P", "-o", "-"], cwd=directory, capture_output=True, timeout=600)
    if done.returncode:
        raise RuntimeError(f"{' '.join(args[:3])}...: {done.stderr.decode(errors='replace')[-500:]}")
    return done.stdout


def unit_verdict(entry):
    """None when the unit is the same in every lane, else why not."""
    args = compile_args(entry)
    debug = [i for i, a in enumerate(args) if a.startswith("-DVJO_DEBUG=")]
    if len(debug) != 1:
        return f"{entry['file']}: VJO_DEBUG is not one -D on its compile line"
    flipped = list(args)
    flipped[debug[0]] = "-DVJO_DEBUG=0" if args[debug[0]] == "-DVJO_DEBUG=1" else "-DVJO_DEBUG=1"
    here = preprocess(args, entry["directory"])
    if preprocess(flipped, entry["directory"]) != here:
        return f"{entry['file']}: its preprocessed text depends on VJO_DEBUG"
    models = [i for i, a in enumerate(args) if a.split("=", 1)[0][2:] in MODEL_DEFINES and a.startswith("-D")]
    if models:
        probed = list(args)
        for i in models:
            probed[i] = args[i].split("=", 1)[0] + "=" + PROBE
        if preprocess(probed, entry["directory"]) != here:
            return f"{entry['file']}: its preprocessed text depends on {args[models[0]].split('=', 1)[0][2:]}"
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path)
    ap.add_argument("source", type=Path)
    ap.add_argument("ctest")
    ap.add_argument("out", type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    a = ap.parse_args(argv)
    build, source = a.build.resolve(), a.source.resolve()
    report = []

    def finish(labels, rc):
        lines = ["# variant_free by derivation (host/tests/variant_free.py); see " + a.out.name + ".txt"]
        for name, have in sorted(labels.items()):
            lines.append(f'set_tests_properties("{name}" PROPERTIES LABELS "{";".join(have)}")')
        a.out.write_text("\n".join(lines) + "\n")
        Path(str(a.out) + ".txt").write_text("\n".join(report) + "\n")
        return rc

    try:
        units = json.loads((build / "compile_commands.json").read_text())
        targets = json.loads((build / "vjo_targets.json").read_text())
        env = dict(os.environ, VJO_VARIANT_LABELS_OFF="1")  # the tests' labels as CMake gave them, not ours
        shown = subprocess.run([a.ctest, "--show-only=json-v1"], cwd=build, env=env, capture_output=True, text=True,
                               timeout=300, check=True).stdout
        tests = json.loads(shown)["tests"]
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        report.append(f"cannot derive: {e}")
        return finish({}, 1)

    # each target's own units (CMakeFiles/<target>.dir/...), its file, the targets it links (transitively)
    own = {}
    for u in units:
        out = Path(u["directory"]) / u["output"] if "output" in u else None
        m = re.search(r"CMakeFiles/([^/]+)\.dir/", str(out or ""))
        if m:
            own.setdefault(m.group(1), []).append(u)
    by_file = {t["file"]: name for name, t in targets.items() if t.get("file")}

    def closure(name, seen=None):
        seen = set() if seen is None else seen
        if name in seen or name not in targets:
            return seen
        seen.add(name)
        for dep in targets[name]["deps"]:
            closure(dep, seen)
        return seen

    configured, everything = configured_by_conditions([source / "CMakeLists.txt"] +
                                                      sorted((source / "cmake").glob("*.cmake")))

    def named_in_blocks(word):
        return everything or word in configured

    # which tests: their programs
    candidates = {}
    for t in tests:
        cmd = t.get("command") or []
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        envs = [e.split("=", 1)[1] for e in props.get("ENVIRONMENT", []) if "=" in e]
        programs = [by_file[x] for x in cmd + envs if x in by_file]
        driver = len(cmd) >= 2 and Path(cmd[1]).suffix == ".py" and Path(cmd[1]).parent == source / "host" / "tests"
        if not cmd or not (cmd[0] in by_file or (driver and programs)):
            report.append(f"{t['name']}: not derived (runs {Path(cmd[0]).name if cmd else 'nothing'} on what is not "
                          "only this build's programs): its labels are CMake's")
            continue
        labels = [x for x in props.get("LABELS", []) if x != LABEL]
        candidates[t["name"]] = (sorted(set(programs)), labels)

    verdicts, jobs = {}, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        for name, (programs, _) in candidates.items():
            for prog in programs:
                for target in closure(prog):
                    for u in own.get(target, []):
                        key = u.get("output") or u["file"]
                        if key not in jobs:
                            jobs[key] = pool.submit(unit_verdict, u)
        try:
            for key, job in jobs.items():
                verdicts[key] = job.result()
        except (RuntimeError, OSError, subprocess.SubprocessError) as e:
            report.append(f"cannot derive: {e}")
            return finish({}, 1)

    labels_out = {}
    for name, (programs, labels) in sorted(candidates.items()):
        why = None
        if named_in_blocks(name):
            why = "the test is configured inside an if() on VJO_DEBUG or the models"
        for prog in programs:
            for target in sorted(closure(prog)):
                if why:
                    break
                if named_in_blocks(target):
                    why = f"{target} is configured inside an if() on VJO_DEBUG or the models"
                elif not own.get(target):  # (no compile lines: nothing is known, so nothing is the same)
                    why = f"{target}: no compile lines for it in compile_commands.json"
                for u in [] if why else own.get(target, []):
                    v = verdicts[u.get("output") or u["file"]]
                    if v:
                        why = f"{target}: {v}"
                        break
        if why:
            report.append(f"{name}: lane-dependent: {why}")
        else:
            report.append(f"{name}: {LABEL} ({', '.join(programs)}: every unit the same in every lane)")
            labels_out[name] = labels + [LABEL]
    return finish(labels_out, 0)


if __name__ == "__main__":
    sys.exit(main())
