#!/usr/bin/env python3
"""Which build a set of Vita modules is: the debug build or the normal one (docs/release-variants.md).

    check_variant.py --variant normal|debug [--version V] --kernel K.skprx --shell S.suprx --worker O.suprx --app eboot.bin

Reads each SELF's loadable segments (decompressed) and checks them for the markers below: the debug
build's own format strings must all be in the debug build and none of them in the normal one, which
carries its own marker instead; the Overlay app names its version and, in the debug build, "debug".
With --sources (the tree's root; the Vita build passes it), each module is also checked against the
literals of 12 bytes or more its sources compile only into the debug build (`#if VJO_DEBUG` branches,
vjo_dlog() and klog_d() calls, shell/selftest.c, kernel/meminfo.c) and kept code does not use too: none
may be in the normal build as a string of its own, which catches a file or target built without
VJO_DEBUG=0, and a debug module must carry at least one of them, which catches one built with
VJO_DEBUG=0 (the worker has no fixed marker: this is its only check).
Exits non-zero with every failure listed. tools/package_release.py's base_variant() names a base zip's
build with classify(), tools/package_variants.py --sources runs check() on the bases, and
vita/CMakeLists.txt runs this after every build of the four modules.
"""
import argparse
import functools
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


# ---- the debug-only strings of the sources (--sources): every string literal the normal build compiles out ----
# A part's sources, the files only the debug build compiles, and the directories whose other literals are kept
# (a debug-only literal also used by kept code is not one). include/ holds vjo_build.h's "debug".
PART_SOURCES = {"shell": ("shell",), "kernel": ("kernel",), "worker": ("game_ocr",), "app": ("settings",)}
DEBUG_ONLY_FILES = {"shell": ("shell/selftest.c",), "kernel": ("kernel/meminfo.c",)}
KEPT_SOURCES = ("shell", "kernel", "game_ocr", "settings", "core", "include", "vita")
SUFFIXES = (".c", ".cpp", ".h", ".inc")  # .inc: tables a .c file includes (core/deinflect.inc, lapis_notetype.inc)
DERIVED_MIN = 12  # shorter literals ("%s", "none") are too likely to be other strings too
DLOG_CALL = re.compile(r"\b(?:vjo_dlog|klog_d)\s*\(\s*$")
CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b(.*)$")  # C23's #elifdef too
ESCAPES = {"n": b"\n", "t": b"\t", "r": b"\r", "0": b"\0", "\\": b"\\", '"': b'"', "'": b"'", "a": b"\a",
           "b": b"\b", "f": b"\f", "v": b"\v", "?": b"?"}


def _debug_lines(text):
    """Per line of C source (index 0 = line 1): whether it is in a branch only the debug build compiles: an
    `#if VJO_DEBUG` branch; the `#elif` and `#else` branches of an `#if !VJO_DEBUG` (reached only with VJO_DEBUG).
    The `#elif` and `#else` branches of an `#if VJO_DEBUG` may be the normal build's, so they are not. `#elifdef`
    and `#elifndef` (C23; the Vita's GCC takes them) are an `#elif`. Other conditionals keep their parent's
    state, an `#elif` that names VJO_DEBUG after one of them too (a weaker check there, never a false failure)."""
    out, on, stack = [], False, []
    for line in text.split("\n"):
        m = CONDITIONAL.match(line)
        if m:
            kind, arg = m.group(1), re.sub(r"/\*.*?\*/|//.*", "", m.group(2)).replace(" ", "")
            if kind in ("if", "ifdef", "ifndef"):
                take = {"VJO_DEBUG": True, "!VJO_DEBUG": False}.get(arg) if kind == "if" else None
                stack.append((on, take))
                on = on or take is True
            elif kind in ("elif", "elifdef", "elifndef") and stack:
                parent, take = stack[-1]
                if take is True:  # after #if VJO_DEBUG: this and any later branch may be built without it
                    on = parent
                    stack[-1] = (parent, None)
                elif take is False:  # after #if !VJO_DEBUG: reached only with VJO_DEBUG (a later #else as well)
                    on = True
            elif kind == "else" and stack:
                parent, take = stack[-1]
                on = parent or take is False if take is not None else on
            elif kind == "endif" and stack:
                on = stack.pop()[0]
        out.append(on)
    return out


def _literals(text):
    """(line, bytes, after_dlog) of each string literal of C source, adjacent ones joined as the compiler joins
    them; comments, character literals and #include/#pragma/#error lines skipped; after_dlog: it is the first
    argument of a vjo_dlog()/klog_d() call."""
    out, i, n, line = [], 0, len(text), 1
    group, glines, after, last_code = None, 0, False, ""
    directive = re.compile(r"[ \t]*#[ \t]*(include|pragma|error|warning|line)\b")

    def end_group():
        nonlocal group
        if group is not None:
            out.append((glines, bytes(group), after))
            group = None

    if directive.match(text):
        i = text.find("\n") if "\n" in text else n
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c in " \t\r\n":
            if c == "\n":
                line += 1
                if directive.match(text, i + 1):  # the next line is an #include or the like: skip it
                    j = text.find("\n", i + 1)
                    i = n if j < 0 else j
                    continue
            if last_code and last_code[-1] != " ":
                last_code += " "  # a token boundary
            i += 1
            continue
        if c == "'":
            end_group()
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1 if j < n and text[j] == "'" else j
            last_code = (last_code + "'")[-64:]
            continue
        if c == '"' and not (i and (text[i - 1].isalnum() or text[i - 1] == "_")):  # not L"..", u8"..", R"(..)"
            j, buf = i + 1, bytearray()
            while j < n and text[j] != '"' and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    e = text[j + 1]
                    if e == "x":
                        k = j + 2
                        while k < n and text[k] in "0123456789abcdefABCDEF":
                            k += 1
                        buf.append(int(text[j + 2:k] or "0", 16) & 0xFF)
                        j = k
                    elif e in "01234567":
                        k = j + 1
                        while k < n and k < j + 4 and text[k] in "01234567":
                            k += 1
                        buf.append(int(text[j + 1:k], 8) & 0xFF)
                        j = k
                    else:
                        buf += ESCAPES.get(e, e.encode("utf-8"))
                        j += 2
                else:
                    buf += text[j].encode("utf-8")
                    j += 1
            if group is None:
                group, glines, after = bytearray(), line, bool(DLOG_CALL.search(last_code))
            group += buf
            i = j + 1 if j < n and text[j] == '"' else j  # an unterminated literal: its newline is counted above
            continue
        end_group()  # any other token ends a group of adjacent literals
        last_code = (last_code + c)[-64:]
        i += 1
    end_group()
    return out


@functools.lru_cache(maxsize=4)
def _scan(root):
    """(rel, literal, debug_only) of every literal in the scanned sources: debug_only for one in an
    `#if VJO_DEBUG` branch (or the #elif and #else of an #if !VJO_DEBUG: _debug_lines), a vjo_dlog()/klog_d()'s
    first argument, or a file only the debug build builds."""
    root = Path(root)
    only = {f for files in DEBUG_ONLY_FILES.values() for f in files}
    out = []
    for d in KEPT_SOURCES:
        for path in sorted((root / d).rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            text = path.read_text(encoding="utf-8", errors="replace")
            flags = _debug_lines(text)
            for line, lit, after_dlog in _literals(text):
                out.append((rel, lit, rel in only or after_dlog or (flags[line - 1] if line - 1 < len(flags) else False)))
    return tuple(out)


def debug_only_strings(root, part):
    """The literals of a part's sources that only the debug build compiles (_scan), less any also used outside
    those (kept by the normal build), and those shorter than DERIVED_MIN bytes."""
    debug, kept = set(), set()
    for rel, lit, is_debug in _scan(str(Path(root).resolve())):
        if not is_debug:
            kept.add(lit)
        elif rel.split("/")[0] in PART_SOURCES.get(part, ()):
            debug.add(lit)
    return sorted(s for s in debug - kept if len(s) >= DERIVED_MIN)


def leaked(blob, strings):
    """The strings found in blob as whole NUL-terminated strings (after a byte below 0x20: the start of one), not as
    the tail of a longer kept one (the linker merges a string with a longer one that ends with it)."""
    found = []
    for s in strings:
        k = blob.find(s + b"\0")
        while k >= 0:
            if k == 0 or blob[k - 1] < 0x20:
                found.append(s)
                break
            k = blob.find(s + b"\0", k + 1)
    return found


def check(variant, blobs, version=None, sources=None):
    """Every failure of blobs ({part: bytes}) as the given variant; with sources (the tree's root), the normal
    build is also checked against every debug-only literal of its sources (debug_only_strings), and a debug
    module must carry at least one of them (the worker has no fixed marker: this is its check)."""
    problems = []
    for part, blob in blobs.items():
        if variant == "normal" and sources is not None:
            gone = debug_only_strings(sources, part)
            left = leaked(blob, gone)
            if left:
                problems.append(f"{part}: the normal build carries {len(left)} of its sources' {len(gone)} debug-only "
                                f"strings (built without VJO_DEBUG=0?): " + "; ".join(repr(s.decode('utf-8', 'replace'))
                                                                                    for s in left[:6]))
        if variant == "debug" and sources is not None:
            want = debug_only_strings(sources, part)
            # at least one, not all: a debug-only literal the compiler drops (unused code) is no sign of the build
            if want and not any(s in blob for s in want):
                problems.append(f"{part}: the debug build carries none of its sources' {len(want)} debug-only strings "
                                "(built with VJO_DEBUG=0?)")
        if part == "worker":
            continue  # no fixed markers of its own (its probe's strings are derived above)
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
    parser.add_argument("--sources", type=Path, help="The tree's root: each module is also checked against its sources' "
                                                     "debug-only literals (debug_only_strings): none in the normal "
                                                     "build, at least one in the debug build")
    for part in ("kernel", "shell", "worker", "app"):
        parser.add_argument("--" + part, type=Path)
    args = parser.parse_args()
    blobs = {part: segments(getattr(args, part)) for part in ("kernel", "shell", "worker", "app") if getattr(args, part)}
    problems = check(args.variant, blobs, args.version, args.sources)
    derived = {part: len(debug_only_strings(args.sources, part)) for part in blobs} if args.sources else {}
    print(json.dumps({"variant": args.variant, "modules": sorted(blobs), "result": "fail" if problems else "pass",
                      "debug_only_strings": derived, "problems": problems}, indent=2))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
