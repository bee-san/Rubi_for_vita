#!/usr/bin/env python3
"""Build the hook archive with the host compiler, also during a Vita cross build."""
import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def revision():
    cmake = (ROOT / "cmake/HookRevision.cmake").read_text()
    block = cmake.split("set(inputs", 1)[1].split(")", 1)[0]
    paths = block.split()
    data = "".join(f"{p}:{hashlib.sha256((ROOT / p).read_bytes()).hexdigest()}\n" for p in paths)
    return hashlib.sha256(data.encode()).hexdigest()

def build(output, cc="cc"):
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rubi-hook-profiles-") as d:
        exe = Path(d) / "export"
        sources = ["tools/hook_profiles.c", "core/hook_profile.c", "core/hook_luna.c", "core/hook_place.c",
                   "core/hook_re.c", "core/hook_text.c"]
        subprocess.run(shlex.split(cc) + ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                       f'-DVJO_HOOK_DEFINITION_SHA256="{revision()}"', "-I", str(ROOT / "core"),
                       *[str(ROOT / s) for s in sources], "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(output)], check=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc", help="host C compiler (never the Vita compiler)")
    args = parser.parse_args()
    build(args.output, args.cc)
