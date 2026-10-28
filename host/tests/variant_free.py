#!/usr/bin/env python3
"""Which host tests give the same result in every lane of the gate, derived from the build: their label variant_free.

    python3 host/tests/variant_free.py BUILD_DIR SOURCE_DIR CTEST OUT [--cmake CMAKE] [--jobs N] [--probe DIR ...]

The gate and CI run the host suites three times: the debug build (every test), the normal build and the debug build
with the release models (`ctest -LE variant_free` both). A test labelled variant_free runs in the first only, so the
label must mean: its result cannot depend on VJO_DEBUG, nor on the model directories (VJO_VOCR_MODEL_DIR,
VJO_VOCR_GATE_TEST_DIR). Here it is derived, never listed by hand. A test qualifies when

- it runs programs of this build only: one of them, or a driver in host/tests/ (python3 host/tests/X.py) given them
  as arguments or in its environment; every other test (the tools' suites, the generators' checks, ...) keeps the
  labels CMake gave it;
- the other two lanes' configurations, which this script makes (BUILD_DIR/variant_free/: the same source and cache
  entries, configured with VJO_DEBUG and VJO_VOCR_MODEL_DIR as those lanes have them, not built), define the test with
  the same command, environment and properties, and its programs, and every library linked into them
  (LINK_LIBRARIES, transitively, and $<TARGET_OBJECTS:...>), with the same link lines and the same units with the
  same compile lines, but for the value of -DVJO_DEBUG and of the model directories' definitions; so CMake configures
  the test alike in every lane, however its CMakeLists.txt arrives at it;
- and every one of those units preprocesses to the same text with VJO_DEBUG=1 and VJO_DEBUG=0, and with the model
  directories' definitions changed (each on its own compile line, compile_commands.json): so the program is the same
  in every lane.

The programs' other inputs (the fixtures, the bundled dictionary each lane unpacks from the same pinned archive) are
the same in every lane by construction.

OUT is a ctest include file (host/tests/variant_labels adds it to TEST_INCLUDE_FILES) that adds variant_free to
each such test's labels; OUT.txt says, for every test, why it has the label or not. When the derivation cannot run
(no compile_commands.json, a configuration or preprocessor failure) it exits 1, which fails the build, and OUT
labels nothing. --probe takes those configurations ready made (the tests of this script).
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
# the gate's host lanes: (VJO_DEBUG, with the models)
LANES = {(True, False): "host-debug", (False, False): "host-normal", (True, True): "models"}


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


def cache_of(build):
    out = {}
    path = build / "CMakeCache.txt"
    for line in path.read_text().split("\n") if path.exists() else []:
        m = re.match(r"^([A-Za-z_][\w.-]*):(\w+)=(.*)$", line)
        if m:
            out[m.group(1)] = (m.group(2), m.group(3))
    return out


class Config:
    """What one configured build defines: its units' compile lines, its programs and libraries, its tests."""

    def __init__(self, build, ctest):
        self.build = build.resolve()
        self.units = {u["output"]: u for u in json.loads((self.build / "compile_commands.json").read_text())
                      if "output" in u}
        self.targets = json.loads((self.build / "vjo_targets.json").read_text())
        env = dict(os.environ, VJO_VARIANT_LABELS_OFF="1")  # the tests' labels as CMake gave them, not ours
        shown = subprocess.run([ctest, "--show-only=json-v1"], cwd=self.build, env=env, capture_output=True,
                               text=True, timeout=300, check=True).stdout
        written = {}  # the commands as CMake wrote them, for a test whose program ctest cannot resolve (not built)
        testfile = self.build / "CTestTestfile.cmake"
        if testfile.exists():
            for name, args in cmake_commands(testfile.read_text(encoding="utf-8")):
                if name == "add_test" and args:
                    args = [x[1:-1] if len(x) > 1 and x[0] == x[-1] == '"' else x for x in args]
                    written[args[0]] = args[1:]
        self.tests = {}
        for t in json.loads(shown)["tests"]:
            props = {p["name"]: p["value"] for p in t.get("properties", []) if p["name"] != "_BACKTRACE_TRIPLES"}
            self.tests[t["name"]] = (t.get("command") or written.get(t["name"]) or [], props)
        self.links = {}
        ninja = self.build / "build.ninja"
        if ninja.exists():  # (each link edge's rule is <LANG>_<KIND>_LINKER__<target>_<build type>)
            config = "_" + cache_of(self.build).get("CMAKE_BUILD_TYPE", ("", ""))[1]
            target = None
            for line in ninja.read_text().replace("$\n", " ").split("\n"):
                m = re.match(r"build (\S+): (?:C|CXX)_(?:EXECUTABLE|STATIC_LIBRARY)_LINKER__(\S+) (.*)$", line)
                if m:
                    target = m.group(2)[:-len(config)] if m.group(2).endswith(config) else m.group(2)
                    self.links[target] = [m.group(3).split(" || ")[0]]
                elif target and re.match(r"  (FLAGS|LINK_FLAGS|LINK_LIBRARIES|LINK_PATH) = ", line):
                    self.links[target].append(line.strip())
                elif not line.startswith("  "):
                    target = None
        for link in (self.build / "CMakeFiles").glob("*.dir/link.txt"):  # (the Makefile generators)
            self.links.setdefault(link.parent.name[:-4], [link.read_text().strip()])

    def norm(self, text):
        text = text.replace(str(self.build), "BUILD")
        return re.sub(r"/vjo-ctest-[0-9a-f]{12}", "/vjo-ctest-X", text)

    def unit_line(self, output):
        """The unit's compile line, VJO_DEBUG's and the model directories' values aside."""
        u = self.units.get(output)
        if not u:
            return None
        args = [re.sub(r"^-D(VJO_DEBUG|" + "|".join(MODEL_DEFINES) + r")=.*$", r"-D\1=X", a) for a in compile_args(u)]
        return self.norm(" ".join(args))

    def test_line(self, name):
        if name not in self.tests:
            return None
        cmd, props = self.tests[name]
        return self.norm(json.dumps([cmd, props], sort_keys=True))

    def link_line(self, target):
        return self.norm(" ".join(self.links[target])) if target in self.links else None

    def own(self, target):
        return sorted(o for o in self.units if o.startswith(f"CMakeFiles/{target}.dir/"))

    def closure(self, name, seen=None):
        seen = set() if seen is None else seen
        if name in seen or name not in self.targets:
            return seen
        seen.add(name)
        for dep in self.targets[name]["deps"]:
            self.closure(dep, seen)
        return seen


def probe(cmake, source, build, debug, models):
    """The source configured as a lane with VJO_DEBUG debug and, with models, a model directory: build's cache
    entries otherwise (its generator, compilers and VJO_* settings), no download (VJO_VOCR_GATE_DOWNLOAD off)."""
    cache = cache_of(build)
    out = build / "variant_free" / LANES[(debug, models)]
    out.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, (kind, value) in sorted(cache.items()):
        if kind != "INTERNAL" and (name.startswith("VJO_") or name in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
                                                                        "CMAKE_BUILD_TYPE")):
            lines.append(f'set({name} "{value}" CACHE {kind} "" FORCE)')
    model_dir = cache.get("VJO_VOCR_MODEL_DIR", ("", ""))[1] or PROBE.strip('"')
    lines += [f'set(VJO_DEBUG {"ON" if debug else "OFF"} CACHE BOOL "" FORCE)',
              f'set(VJO_VOCR_MODEL_DIR "{model_dir if models else ""}" CACHE PATH "" FORCE)',
              'set(VJO_VOCR_GATE_DOWNLOAD OFF CACHE BOOL "" FORCE)']
    (out.parent / f"{out.name}.cmake").write_text("\n".join(lines) + "\n")
    generator = cache.get("CMAKE_GENERATOR", ("", "Ninja"))[1]
    done = subprocess.run([cmake, "-S", str(source), "-B", str(out), "-G", generator, "-C",
                           str(out.parent / f"{out.name}.cmake")], capture_output=True, text=True, timeout=600)
    if done.returncode:
        raise RuntimeError(f"configuring {out}: {done.stdout[-500:]}{done.stderr[-1000:]}")
    return out


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
    ap.add_argument("--cmake", default="cmake")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--probe", type=Path, nargs="*", help="the other lanes' configurations, ready made")
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
        lane = Config(build, a.ctest)
        if a.probe is None:
            cache = cache_of(build)
            mine = (cache.get("VJO_DEBUG", ("", "ON"))[1].upper() in ("ON", "1", "TRUE", "YES"),
                    bool(cache.get("VJO_VOCR_MODEL_DIR", ("", ""))[1]))
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:  # (the two at once)
                a.probe = list(pool.map(lambda lane: probe(a.cmake, source, build, *lane),
                                        [lane for lane in LANES if lane != mine]))
        probes = [(p.name, Config(p, a.ctest)) for p in a.probe]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as e:
        report.append(f"cannot derive: {e}")
        return finish({}, 1)

    by_file = {t["file"]: name for name, t in lane.targets.items() if t.get("file")}
    candidates = {}
    for name, (cmd, props) in sorted(lane.tests.items()):
        envs = [e.split("=", 1)[1] for e in props.get("ENVIRONMENT", []) if "=" in e]
        programs = sorted({by_file[x] for x in cmd + envs if x in by_file})
        driver = len(cmd) >= 2 and Path(cmd[1]).suffix == ".py" and Path(cmd[1]).parent == source / "host" / "tests"
        if not cmd or not (cmd[0] in by_file or (driver and programs)):
            report.append(f"{name}: not derived (runs {Path(cmd[0]).name if cmd else 'nothing'} on what is not "
                          "only this build's programs): its labels are CMake's")
            continue
        candidates[name] = (programs, [x for x in props.get("LABELS", []) if x != LABEL])

    verdicts, jobs = {}, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        for name, (programs, _) in candidates.items():
            for prog in programs:
                for target in lane.closure(prog):
                    for o in lane.own(target):
                        if o not in jobs:
                            jobs[o] = pool.submit(unit_verdict, lane.units[o])
        try:
            for o, job in jobs.items():
                verdicts[o] = job.result()
        except (RuntimeError, OSError, subprocess.SubprocessError) as e:
            report.append(f"cannot derive: {e}")
            return finish({}, 1)

    def why_not(name, programs):
        for pname, p in probes:
            if p.test_line(name) != lane.test_line(name):
                return f"the {pname} configuration defines the test otherwise (or not at all)"
        for prog in programs:
            for target in sorted(lane.closure(prog)):
                if not lane.own(target):  # (no compile lines: nothing is known, so nothing is the same)
                    return f"{target}: no compile lines for it in compile_commands.json"
                for pname, p in probes:
                    if p.link_line(target) != lane.link_line(target) or p.own(target) != lane.own(target):
                        return f"{target}: the {pname} configuration links it otherwise"
                    for o in lane.own(target):
                        if p.unit_line(o) != lane.unit_line(o):
                            return f"{target}: the {pname} configuration compiles {lane.units[o]['file']} otherwise"
                for o in lane.own(target):
                    if verdicts[o]:
                        return f"{target}: {verdicts[o]}"
        return None

    labels_out = {}
    for name, (programs, labels) in sorted(candidates.items()):
        why = why_not(name, programs)
        if why:
            report.append(f"{name}: lane-dependent: {why}")
        else:
            report.append(f"{name}: {LABEL} ({', '.join(programs)}: configured and compiled alike in every lane)")
            labels_out[name] = labels + [LABEL]
    return finish(labels_out, 0)


if __name__ == "__main__":
    sys.exit(main())
