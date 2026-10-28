#!/usr/bin/env python3
"""The ARM lane's programs, built from the host build's own configuration (tools/arm_host_tests.sh).

    python3 host/tests/arm_build.py CONFIG OUT --cc ARM_CC [--native] [--jobs N] TEST...

CONFIG is the host build configured (cmake -G "Unix Makefiles"), not built. Each TEST's program is the one its ctest
runs (CTestTestfile.cmake); each of its units is compiled with its own compile line (compile_commands.json): the host
compiler replaced by ARM_CC, the lane's flags for the Vita's CPU added (-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard,
64-bit file offsets) and the host's -g and -DNDEBUG left out (assert() stays on, as the lane always had it). The program
is linked as its link line links it: its objects, then each static library of the build archived for ARM from that
library's own units, so the linker takes from them what the host's takes. The ARM sysroot has no zlib: a program
the host links with it is built without (its units without VJO_TEST_ZLIB, as test_psp.c allows). The program is
OUT/TEST, statically linked. With --native, the same with the host's compiler and nothing added (the lane's host-side
copy of a program it compares with).

So the lane builds what the host build builds, from the same lists: a source added to a suite in CMakeLists.txt is
in its ARM program too. Exit 1 when a program cannot be built (its compiler's output is printed).
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

# -D_FILE_OFFSET_BITS=64: a 32-bit readdir() or stat() fails with EOVERFLOW on a file system whose directory cookies or
# inode numbers pass 2^32 (XFS: the taicfg fixtures' readdir under qemu-arm)
ARM_FLAGS = ["-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=hard", "-D_FILE_OFFSET_BITS=64"]
DROPPED = {"-g", "-DNDEBUG"}
# host libraries the ARM sysroot does not have, and what their users define to use them
HOST_ONLY = {"libz.so": "-DVJO_TEST_ZLIB=1", "libz.a": "-DVJO_TEST_ZLIB=1"}


def cmake_args(text):
    """The arguments of each add_test(NAME ...) of a ctest file: {name: [program, args...]}."""
    out = {}
    for m in re.finditer(r'^add_test\((\S+) (.*)\)$', text, re.M):
        args = [a[1:-1] if a.startswith('"') else a for a in re.findall(r'"(?:[^"\\]|\\.)*"|\S+', m.group(2))]
        out[m.group(1)] = args
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--native", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("tests", nargs="+")
    a = ap.parse_args(argv)
    cfg, out = a.config.resolve(), a.out.resolve()
    tag = "host" if a.native else "arm"
    units = {u["output"]: u for u in json.loads((cfg / "compile_commands.json").read_text())}
    tests = cmake_args((cfg / "CTestTestfile.cmake").read_text())
    ar = subprocess.run([a.cc, "-print-prog-name=ar"], capture_output=True, text=True).stdout.strip() or "ar"
    extra = [] if a.native else ARM_FLAGS

    def flags_of(args, drop_defines):
        return [x for x in args if x not in DROPPED and x not in drop_defines]

    plans = {}
    for name in a.tests:
        prog = tests.get(name, [None])[0]
        if not prog or Path(prog).parent != cfg:
            sys.exit(f"{tag} {name}: not a test that runs one of this build's programs")
        target = Path(prog).name
        link = shlex.split((cfg / "CMakeFiles" / f"{target}.dir" / "link.txt").read_text())
        i = link.index("-o")
        objs = [x for x in link[1:i] if x.endswith(".o")]
        libs = link[i + 2:]
        drop = {HOST_ONLY[Path(x).name] for x in libs if Path(x).name in HOST_ONLY}
        plans[name] = (target, [x for x in link[1:i] if not x.endswith(".o")], objs,
                       [x for x in libs if Path(x).name not in HOST_ONLY], drop)

    def args_of(o):
        u = units[o]
        return u.get("arguments") or shlex.split(u["command"])

    def key_of(o, drop):
        """a unit compiled once for every program, unless one of its own definitions is dropped for this one"""
        return (o, frozenset(d for d in drop if o in units and d in args_of(o)))

    def members(lib):
        return [o for o in units if o.startswith(f"CMakeFiles/{lib}.dir/")]

    def is_archive(lib):
        m = re.fullmatch(r"lib(.+)\.a", lib)
        return m.group(1) if m and (cfg / lib).parent == cfg else None

    # every unit to compile: the programs' objects, and each archive's (once each)
    jobs = {}
    for name, (target, _, objs, libs, drop) in plans.items():
        for o in objs:
            jobs.setdefault(key_of(o, drop), None)
        for lib in libs:
            if is_archive(lib):
                for o in members(is_archive(lib)):
                    jobs.setdefault(key_of(o, drop), None)

    def obj_path(key):
        o, drop = key
        return out / "obj" / tag / ("-".join(sorted(d.lstrip("-D").split("=")[0] for d in drop)) or "all") / o

    def compile_unit(key):
        o, drop = key
        if o not in units:
            return False, f"{o}: no compile line for it"
        u = units[o]
        args = args_of(o)
        k = args.index("-o")
        dst = obj_path(key)
        dst.parent.mkdir(parents=True, exist_ok=True)
        cmd = [a.cc] + flags_of(args[1:k], drop) + extra + ["-o", str(dst)] + flags_of(args[k + 2:], drop)
        done = subprocess.run(cmd, cwd=u["directory"], capture_output=True, text=True)
        if done.returncode:
            return False, f"{' '.join(cmd)}\n{done.stdout}{done.stderr}"
        return True, done.stdout + done.stderr  # (its warnings, in the lane's log as before)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        results = list(pool.map(compile_unit, list(jobs)))
    print("".join(text for ok, text in results if ok and text), end="")
    failed = [text for ok, text in results if not ok]
    if failed:
        print("\n".join(failed))
        return 1

    archives, rc = {}, 0
    for name, (target, link_flags, objs, libs, drop) in plans.items():
        line = [a.cc] + flags_of(link_flags, set()) + extra + ([] if a.native else ["-static"])
        line += [str(obj_path(key_of(o, drop))) for o in objs] + ["-o", str(out / name)]
        for lib in libs:
            name_ = is_archive(lib)
            if not name_:
                line.append(lib)
                continue
            keys = tuple(key_of(o, drop) for o in members(name_))
            if keys not in archives:
                path = out / "lib" / tag / f"{len(archives)}" / lib
                path.parent.mkdir(parents=True, exist_ok=True)
                path.unlink(missing_ok=True)
                subprocess.run([ar, "rcs", str(path)] + [str(obj_path(k)) for k in keys], check=True)
                archives[keys] = str(path)
            line.append(archives[keys])
        done = subprocess.run(line, capture_output=True, text=True)
        if done.returncode:
            print(f"{tag} {name}: build FAILED\n{' '.join(line)}\n{done.stdout}{done.stderr}")
            rc = 1
            continue
        kind = subprocess.run(["file", str(out / name)], capture_output=True, text=True).stdout.strip()
        print(re.sub(r", (BuildID|for GNU|with debug_info|not stripped).*", "", kind))
    return rc


if __name__ == "__main__":
    sys.exit(main())
