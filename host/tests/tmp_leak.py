#!/usr/bin/env python3
"""A host run must leave nothing in its temporary directories.

    python3 host/tests/tmp_leak.py setup DIR TEST...
    python3 host/tests/tmp_leak.py check DIR TEST...

ctest runs each test with TMPDIR=DIR/TEST (CMakeLists.txt). DIR is this build's own directory in the
system's temporary directory (vjo-ctest-<hash of the build directory>), outside the source tree as
/tmp is. `tmp_leak_setup` runs before the first test and `tmp_leak_check` after the last (a ctest
fixture). One ctest run per build directory at a time, as for the build's other outputs.

setup makes DIR, this user's alone (0700; a link or another user's directory is refused), and in it
one empty directory per test: 0 entries before the run. What an interrupted run left is named and
removed; an entry it cannot remove fails setup.

check counts each test's directory after the run and fails on every entry in it, named with its
test, its files and, for a suite's directory (vjo-NAME-XXXXXX, host/tests/test_tmpdir.h), the
suite's source; on anything else in DIR; and on a test's directory that is gone (a test that removed
its TMPDIR, or a run without setup). It also fails when a test source (host/tests, tools/tests) names
/tmp itself: such a test's files go around TMPDIR, where this check cannot count them. Then it
removes DIR, so a run leaves nothing either way. A directory a test left without permissions is made
this user's to list and remove first (u+rwx, on this user's directories only, never through a link);
whatever DIR still holds then is named, and fails the check, and the next setup refuses it.
"""
import os
import pathlib
import re
import shutil
import stat
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCANNED = [ROOT / "host" / "tests", ROOT / "tools" / "tests"]
# test_tmpdir.h falls back to /tmp itself when TMPDIR is unset; this file names it in its scan
SCAN_EXEMPT = {"test_tmpdir.h", "tmp_leak.py"}
TMP_LITERAL = re.compile(r"""["'](?:/var)?/tmp(?:/|["'])|\bP_tmpdir\b""")
TEST_NAME = re.compile(r"[A-Za-z0-9_.-]+")


def entries(d):
    try:
        return sorted(os.listdir(d))
    except FileNotFoundError:
        return []


def size_of(path):
    """(files and directories under path, path included; bytes of its files)"""
    st = path.lstat()
    if not stat.S_ISDIR(st.st_mode):
        return 1, st.st_size
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
        src = ROOT / "host" / "tests" / f"test_{m.group(1).replace('-', '_')}.c"
        if src.exists():
            return src.relative_to(ROOT)
    return None


def describe(path, label):
    count, size = size_of(path)
    src = source_of(path.name)
    return f"  {label}: {count} files and directories, {size} bytes" + (f" (made by {src})" if src else "")


def make_removable(path):
    """u+rwx on path and on every directory under it that is this user's and not a link, top down, so
    that a directory a test left without permissions can be listed and emptied"""
    uid = os.getuid()

    def fix(q):
        try:
            st = os.lstat(q)
            if stat.S_ISDIR(st.st_mode) and st.st_uid == uid and st.st_mode & 0o700 != 0o700:
                os.chmod(q, stat.S_IMODE(st.st_mode) | 0o700)
        except OSError:
            pass

    fix(path)
    for parent, dirs, _ in os.walk(path):  # (followlinks=False: a link to a directory is not entered)
        for name in dirs:
            fix(os.path.join(parent, name))


def remove(path):
    """path and everything under it, no link followed: whether it is gone"""
    try:
        if stat.S_ISDIR(path.lstat().st_mode):
            make_removable(path)
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink()
    except FileNotFoundError:
        pass
    except OSError:
        pass
    return not os.path.lexists(path)


def ours(d):
    """DIR is a directory of this user's, not a link (or not there)"""
    try:
        st = os.lstat(d)
    except FileNotFoundError:
        return True
    return stat.S_ISDIR(st.st_mode) and st.st_uid == os.getuid()


def leftovers(d, tests):
    """[(path, label)] of everything in DIR but the tests' own (empty) directories"""
    found = []
    for name in entries(d):
        p = d / name
        if name in tests and stat.S_ISDIR(p.lstat().st_mode):
            try:
                found += [(p / e, f"{name}: {e}") for e in sorted(os.listdir(p))]
            except OSError as e:
                found.append((p, f"{name}: its TMPDIR cannot be listed ({e.strerror})"))
        else:
            found.append((p, f"{name} (in no test's directory)"))
    return found


def setup(d, tests):
    if not ours(d):
        print(f"{d} is not a directory of this user's: refused")
        return 1
    if os.path.lexists(d):
        make_removable(d)
    left = leftovers(d, tests)
    if left:
        print(f"{d}: an interrupted run left {len(left)} entries, removed now:")
        for p, label in left:
            print(describe(p, label))
    stuck = [label for p, label in left if not remove(p)]
    try:
        os.mkdir(d, 0o700)
    except FileExistsError:
        pass
    except OSError as e:
        print(f"cannot make {d}: {e}")
        return 1
    if not ours(d) or not stat.S_ISDIR(os.lstat(d).st_mode):  # made by someone else meanwhile
        print(f"{d} is not a directory of this user's: refused")
        return 1
    os.chmod(d, 0o700)
    for t in tests:
        try:
            os.mkdir(d / t, 0o700)
        except FileExistsError:
            if not stat.S_ISDIR((d / t).lstat().st_mode):
                stuck.append(t)
    if stuck:
        print(f"could not remove: {', '.join(stuck)}")
        return 1
    before = sum(len(entries(d / t)) for t in tests)
    print(f"TMPDIR {d}/TEST: {before} entries before the run in the {len(tests)} tests' directories")
    return 0 if before == 0 else 1


def tmp_in_sources():
    found = []
    for top in SCANNED:
        for path in sorted(top.rglob("*")):
            if (path.suffix not in (".c", ".cpp", ".h", ".py") or path.name in SCAN_EXEMPT
                    or "fixtures" in path.relative_to(ROOT).parts):  # (the checkout's own path may hold one)
                continue
            for i, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if TMP_LITERAL.search(line):
                    found.append(f"  {path.relative_to(ROOT)}:{i}: {line.strip()}")
    return found


def check(d, tests):
    if not ours(d):
        print(f"{d} is not a directory of this user's: refused")
        return 1
    missing = [t for t in tests if not (d / t).is_dir() or (d / t).is_symlink()]
    make_removable(d)  # what a test left without permissions is listed and counted whole
    left = leftovers(d, tests)
    print(f"TMPDIR {d}/TEST: 0 entries before the run, {len(left)} after, in the {len(tests)} tests' directories")
    for p, label in left:
        print(describe(p, label))
    if missing:
        print(f"{len(missing)} tests' TMPDIR is gone (removed by a test, or no tmp_leak_setup): {' '.join(missing)}")
    found = tmp_in_sources()
    if found:
        print(f"{len(found)} lines of the tests name /tmp instead of TMPDIR (host/tests/test_tmpdir.h):")
        print("\n".join(found))
    if left:
        print("tests left these in their TMPDIR: a suite must remove what it makes (host/tests/test_tmpdir.h)")
    stuck = not remove(d)
    if stuck:
        print(f"could not remove {d}; it still holds:")
        for parent, dirs, files in os.walk(d):
            for name in sorted(dirs + files):
                print(f"  {os.path.relpath(os.path.join(parent, name), d)}")
    return 1 if left or missing or found or stuck else 0


def main(argv):
    if len(argv) < 4 or argv[1] not in ("setup", "check"):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    d, tests = pathlib.Path(argv[2]), argv[3:]
    bad = [t for t in tests if not TEST_NAME.fullmatch(t) or t in (".", "..")]
    if not d.is_absolute() or bad:
        print(f"DIR must be absolute and each TEST a plain name: {d} {' '.join(bad)}", file=sys.stderr)
        return 2
    return setup(d, set(tests)) if argv[1] == "setup" else check(d, set(tests))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
