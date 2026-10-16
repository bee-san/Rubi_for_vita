#!/usr/bin/env python3
"""Which build a set of Vita modules is: the debug build or the normal one (docs/release-variants.md).

    check_variant.py --variant normal|debug [--version V] --kernel K.skprx --shell S.suprx --worker O.suprx --app eboot.bin

Reads each SELF's loadable segments (decompressed) and checks them for the markers below: the debug
build's own format strings must all be in the debug build and none of them in the normal one, which
carries its own marker instead; the Overlay app names its version and, in the debug build, "debug".
Exits non-zero with every failure listed. tools/package_release.py uses variant_of() to name a base
zip's build, and vita/CMakeLists.txt runs this after every build of the four modules.
"""
import argparse
import json
import re
import struct
import sys
import zlib
from pathlib import Path

# Strings of code the normal build compiles out (each from one debug-only call site or file).
DEBUG_MARKERS = {
    "shell": (
        b"selftest v2 api=",                          # shell/selftest.c: the self-test's report
        b"gstep %s %s kind=",                         # shell/selftest.c: the game worker's step log
        b"game vocr text %u: %s",                     # worker.c: the recognized text
        b"game vocr capture: %s into %s",             # worker.c: a job's capture
        b"job %d done rc=%d in %d ms",                # worker.c: a job's timing
        b"lookup %s at %u: %d chars",                 # worker.c: a lookup's line
        b"overlay closed: %d frames",                 # overlay.cpp: the open overlay's counts and timings
        b"touch region %s: 0x%X t=%u ms",             # overlay.cpp: the touch region's trace
        b"input block on rc=%d",                      # worker.c: the input block's trace
        b"anki: arena peak %u of %u",                 # anki.c
        b"Paf allocation: free=%u",                   # memory.cpp: every allocation
    ),
    "kernel": (
        b"region settled: scene %u",                  # kernel/main.c: every settled screen
        b"input blocked hook=%d",                     # kernel/input.c: the hooks' traces
        b"ocr job %u: first pass in",                 # kernel/game_ocr.c: every job
    ),
    "app": (
        b"frames: %d in %u us",                       # settings/main.c: the frame interval
    ),
}
NORMAL_MARKERS = {
    "shell": (b"the normal build: a short log",),
    "kernel": (b"the normal build: a short log",),
}
# The Overlay app's start line names its version and its build (settings/main.c VJO_APP_VERSION).
APP_START = re.compile(rb"start: v([0-9A-Za-z.+~-]+)( debug)?; ")


def segments(path):
    """The loadable segments of a Vita SELF (fself), decompressed, as one byte string."""
    data = Path(path).read_bytes()
    if data[:4] != b"SCE\0":
        raise ValueError(f"{path}: not a Vita SELF")
    elf_offset, ph_offset = struct.unpack_from("<QQ", data, 0x40)
    seginfo_offset, = struct.unpack_from("<Q", data, 0x58)
    if data[elf_offset:elf_offset + 4] != b"\x7fELF":
        raise ValueError(f"{path}: no ELF payload")
    ph_size, ph_count = struct.unpack_from("<HH", data, elf_offset + 42)
    out = []
    for i in range(ph_count):
        kind, = struct.unpack_from("<I", data, ph_offset + i * ph_size)
        offset, size, compression, _ = struct.unpack_from("<4Q", data, seginfo_offset + i * 32)
        if kind != 1:  # PT_LOAD only (not the relocations)
            continue
        blob = data[offset:offset + size]
        out.append(zlib.decompress(blob) if compression == 2 else blob)
    return b"".join(out)


def app_version(blob):
    """(version, variant) the Overlay app's start line names, or (None, None)."""
    m = APP_START.search(blob)
    if not m:
        return None, None
    return m.group(1).decode("ascii"), "debug" if m.group(2) else "normal"


def classify(blobs):
    """The variant the modules' markers say ("debug", "normal"), or None when they disagree, and why."""
    seen = {}
    why = []
    for part, blob in blobs.items():
        if part == "app":
            _, v = app_version(blob)
            if v:
                seen[part] = v
            continue
        debug = [m for m in DEBUG_MARKERS.get(part, ()) if m in blob]
        normal = [m for m in NORMAL_MARKERS.get(part, ()) if m in blob]
        if debug and not normal:
            seen[part] = "debug"
        elif normal and not debug:
            seen[part] = "normal"
        elif part in NORMAL_MARKERS:
            why.append(f"{part}: {len(debug)} debug and {len(normal)} normal markers")
    variants = set(seen.values())
    return (variants.pop() if len(variants) == 1 and not why else None), seen, why


def check(variant, blobs, version=None):
    """Every failure of blobs ({part: bytes}) as the given variant."""
    problems = []
    for part, blob in blobs.items():
        if part == "worker":
            continue  # no strings of its own; its probe is checked by the host tests (VJO_DEBUG)
        markers = DEBUG_MARKERS.get(part, ())
        found = [m.decode() for m in markers if m in blob]
        missing = [m.decode() for m in markers if m not in blob]
        if variant == "normal" and found:
            problems.append(f"{part}: the normal build carries debug-only strings: {found}")
        if variant == "debug" and missing:
            problems.append(f"{part}: the debug build lacks its markers (renamed? update tools/check_variant.py): {missing}")
        for m in NORMAL_MARKERS.get(part, ()):
            if (m in blob) != (variant == "normal"):
                problems.append(f"{part}: {m.decode()!r} {'missing from' if variant == 'normal' else 'in'} the {variant} build")
        if part == "app":
            v, kind = app_version(blob)
            if kind != variant:
                problems.append(f"app: its start line says {kind or 'nothing'} (want {variant})")
            if version is not None and v != version:
                problems.append(f"app: version v{v} (want v{version})")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variant", required=True, choices=("debug", "normal"))
    parser.add_argument("--version", help="The Overlay app's version (VJO_VERSION)")
    for part in ("kernel", "shell", "worker", "app"):
        parser.add_argument("--" + part, type=Path)
    args = parser.parse_args()
    blobs = {part: segments(getattr(args, part)) for part in ("kernel", "shell", "worker", "app") if getattr(args, part)}
    problems = check(args.variant, blobs, args.version)
    print(json.dumps({"variant": args.variant, "modules": sorted(blobs), "result": "fail" if problems else "pass",
                      "problems": problems}, indent=2))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
