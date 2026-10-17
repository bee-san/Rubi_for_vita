#!/usr/bin/env python3
"""A host run must leave nothing in its TMPDIR.

    python3 host/tests/tmp_leak.py setup DIR
    python3 host/tests/tmp_leak.py check DIR

ctest runs every test with TMPDIR=DIR, a directory of this build's own in the system's temporary
directory (CMakeLists.txt: vjo-ctest-<hash of the build directory>, outside the source tree as /tmp
is): `tmp_leak_setup` runs before the first test and `tmp_leak_check` after the last (a ctest
fixture).

setup makes DIR, empty and this user's alone (0700; a DIR that is a link or another user's is
refused). What an interrupted run left in it is named and removed. It records the entries it could
not remove (normally none) in DIR.before: the count before the run.

check counts DIR's entries after the run and fails on every one that was not there before, naming
each with its files and, for a suite's directory (vjo-NAME-XXXXXX, host/tests/test_tmpdir.h), the
suite's source. It also fails when a host test's source names /tmp itself: such a test's files go
around DIR, where this check cannot count them. Then it removes DIR and DIR.before, so that a run
leaves nothing in the system's temporary directory either way.
"""
import os
import pathlib
import re
import shutil
import stat
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TESTS = ROOT / "host" / "tests"
# test_tmpdir.h falls back to /tmp itself when TMPDIR is unset; this file names it in its scan
SCAN_EXEMPT = {"test_tmpdir.h", "tmp_leak.py"}
TMP_LITERAL = re.compile(r"""["']/tmp(?:/|["'])""")


def entries(d):
    try:
        return sorted(os.listdir(d))
    except FileNotFoundError:
        return []


def size_of(path):
    """(files and directories under path, path included; bytes of its files)"""
    if not path.is_dir() or path.is_symlink():
        return 1, path.lstat().st_size
    count, size = 1, 0
    for parent, dirs, files in os.walk(path):
        count += len(dirs) + len(files)
        for f in files:
            try:
                size += (pathlib.Path(parent) / f).lstat().st_size
            except OSError:
                pass
    return count, size


def source_of(name):
    m = re.fullmatch(r"vjo-([a-z0-9-]+)-[A-Za-z0-9]{6}", name)
    if m:
        src = TESTS / f"test_{m.group(1).replace('-', '_')}.c"
        if src.exists():
            return src.relative_to(ROOT)
    return None


def describe(d, name):
    count, size = size_of(d / name)
    src = source_of(name)
    return f"  {name}: {count} files and directories, {size} bytes" + (f" (made by {src})" if src else "")


def remove(path):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path, ignore_errors=True)
    else:
        try:
            path.unlink()
        except OSError:
            pass


def ours(d):
    """DIR is a directory of this user's, not a link (or not there yet)"""
    try:
        st = os.lstat(d)
    except FileNotFoundError:
        return True
    return stat.S_ISDIR(st.st_mode) and st.st_uid == os.getuid()


def setup(d):
    if not ours(d):
        print(f"{d} is not a directory of this user's: refused")
        return 1
    left = entries(d)
    if left:
        print(f"{d}: an interrupted run left {len(left)} entries, removed now:")
        for name in left:
            print(describe(d, name))
            remove(d / name)
    d.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(d, 0o700)
    before = entries(d)
    pathlib.Path(f"{d}.before").write_text("".join(f"{n}\n" for n in before))
    print(f"TMPDIR {d}: {len(before)} entries before the run")
    for name in before:
        print(f"  {name} (could not be removed)")
    return 0


def tmp_in_sources():
    found = []
    for path in sorted(TESTS.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h", ".py") or path.name in SCAN_EXEMPT or "fixtures" in path.parts:
            continue
        for i, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if TMP_LITERAL.search(line):
                found.append(f"  {path.relative_to(ROOT)}:{i}: {line.strip()}")
    return found


def check(d):
    if not ours(d):
        print(f"{d} is not a directory of this user's: refused")
        return 1
    before_file = pathlib.Path(f"{d}.before")
    try:
        before = set(before_file.read_text().split("\n")) - {""}
    except FileNotFoundError:
        before = set()
    after = entries(d)
    new = [n for n in after if n not in before]
    print(f"TMPDIR {d}: {len(before)} entries before the run, {len(after)} after: {len(new)} new")
    for name in new:
        print(describe(d, name))
    found = tmp_in_sources()
    if found:
        print(f"{len(found)} lines of the host tests name /tmp instead of TMPDIR (host/tests/test_tmpdir.h):")
        print("\n".join(found))
    if new:
        print("a test left these in TMPDIR: its suite must remove what it makes (host/tests/test_tmpdir.h)")
    remove(d)
    remove(before_file)
    return 1 if new or found else 0


def main(argv):
    if len(argv) != 3 or argv[1] not in ("setup", "check"):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    d = pathlib.Path(argv[2])
    return setup(d) if argv[1] == "setup" else check(d)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
