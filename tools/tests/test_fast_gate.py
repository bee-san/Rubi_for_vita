"""tools/fast_gate.sh builds the tree's release version in its Vita lanes. The gate reuses its build directories
(build/gate), and a CMake cache keeps the VJO_VERSION of its first configure (vita/CMakeLists.txt's set() is a CACHE
STRING): rubi-release-final's gate on the 0.10.0 release commit (2026-10-28) built, packaged and e2e-checked
Rubi-0.9.8.zip and passed; only --clean built 0.10.0. The gate reads the version from vita/CMakeLists.txt and passes
it to both Vita lanes' configure, which replaces a cached one; its e2e lane counts this run's packages' logs only.
(CI configures a new build directory on every run, and tools/ci_container_build.sh builds in a new container:
neither keeps a cache.)"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = (ROOT / "tools/fast_gate.sh").read_text(encoding="utf-8")
VITA = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
VERSION_LINE = re.compile(r'^set\(VJO_VERSION "([^"]*)" CACHE STRING "[^"]*"\)$', re.M)


def with_version(text, version):
    """TEXT with its set(VJO_VERSION ...) line's value VERSION."""
    return VERSION_LINE.sub(lambda m: m.group(0).replace(f'"{m.group(1)}"', f'"{version}"', 1), text)


TREE_VERSION = re.compile(r'^tree_version\(\) \{ sed .* vita/CMakeLists\.txt; \}$', re.M)


def gate_version(cmakelists):
    """fast_gate.sh's own three lines (tree_version(), VERSION= and the die after it), run by bash in a tree whose
    vita/CMakeLists.txt is CMAKELISTS: (exit status, the VERSION they read, what the gate says on stderr)."""
    lines = re.search(TREE_VERSION.pattern + r'\nVERSION=\$\(tree_version\)\n\[ -n "\$VERSION" \] \|\| die .*$', GATE,
                      re.M).group(0)
    script = f'die() {{ echo "fast_gate: $*" >&2; exit 2; }}\n{lines}\nprintf %s "$VERSION"'
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "vita").mkdir()
        (Path(tmp) / "vita/CMakeLists.txt").write_text(cmakelists, encoding="utf-8")
        run = subprocess.run(["bash", "-c", script], cwd=tmp, capture_output=True, text=True, timeout=60)
        return run.returncode, run.stdout, run.stderr


def lanes_check(cmakelists, caches, rcs=None, before=""):
    """fast_gate.sh's check of the Vita lanes' versions (its tree_version() and the loop after the lanes), run by
    bash in a tree whose vita/CMakeLists.txt is CMAKELISTS, on an OUT whose vita-<v>/CMakeCache.txt holds
    VJO_VERSION:STRING=caches[v] (None: no cache) and whose LOGS/vita-<v>.rc is rcs[v] (default "0 42"). BEFORE runs
    first (e.g. VERSION set to another). {v: (rc file, the error lines it appended to the lane's log)}."""
    check = GATE[GATE.index("# ---- the Vita lanes built the tree's version"):GATE.index("# ---- the result")]
    with tempfile.TemporaryDirectory() as tmp:
        tree, out = Path(tmp) / "tree", Path(tmp) / "out"
        (tree / "vita").mkdir(parents=True)
        (tree / "vita/CMakeLists.txt").write_text(cmakelists, encoding="utf-8")
        (out / "logs").mkdir(parents=True)
        for v, built in caches.items():
            (out / f"vita-{v}").mkdir()
            if built is not None:
                (out / f"vita-{v}/CMakeCache.txt").write_text(f"VJO_DEBUG:BOOL=ON\nVJO_VERSION:STRING={built}\n")
            (out / f"logs/vita-{v}.rc").write_text((rcs or {}).get(v, "0 42") + "\n")
            (out / f"logs/vita-{v}.log").write_text("memory ledger: pass\n")
        script = f'{TREE_VERSION.search(GATE).group(0)}\nOUT={out}\nLOGS={out}/logs\n{before}\n{check}'
        subprocess.run(["bash", "-c", script], cwd=tree, check=True, capture_output=True, text=True, timeout=60)
        logs = {v: (out / f"logs/vita-{v}.log").read_text().splitlines() for v in caches}
        return {v: ((out / f"logs/vita-{v}.rc").read_text().strip(), [l for l in logs[v] if l.startswith("error:")])
                for v in caches}


class GateVersion(unittest.TestCase):
    def test_the_gate_reads_the_trees_version(self):
        """vita/CMakeLists.txt has one set(VJO_VERSION ...) line, and the gate reads its value, a test build's too;
        a tree without the line stops the gate (exit 2) before any lane runs."""
        found = VERSION_LINE.findall(VITA)
        self.assertEqual(len(found), 1, 'one set(VJO_VERSION "..." CACHE STRING "...") line in vita/CMakeLists.txt')
        self.assertEqual(gate_version(VITA), (0, found[0], ""))
        self.assertEqual(gate_version(with_version(VITA, "0.10.1-test.1")), (0, "0.10.1-test.1", ""))
        rc, version, said = gate_version("project(rubi NONE)\n")
        self.assertEqual((rc, version), (2, ""))
        self.assertIn("fast_gate: no set(VJO_VERSION", said)

    def test_both_vita_lanes_configure_with_it(self):
        """The Vita lanes' one configure command, run for the debug and the normal build, passes the version."""
        lanes = GATE[GATE.index("# ---- the Vita builds"):GATE.index('if [ "$E2E" = on ]; then')]
        self.assertIn("for v in debug normal; do", lanes)
        self.assertIn("run_lane vita-$v vita", lanes)
        configure = re.findall(r"^cmake -S vita -B (?:[^\n]*\\\\\n)*[^\n]*$", GATE, re.M)
        self.assertEqual(len(configure), 1)
        self.assertIn(configure[0], lanes)
        self.assertIn('-DVJO_VERSION=$(q "$VERSION")', configure[0])

    def test_the_e2e_lane_counts_this_runs_packages_only(self):
        """The e2e lane's line counts the PASS lines of every OUT/e2e-<package>.log: after a version bump an earlier
        run's logs (Rubi-0.9.8*) would be counted with this run's (Rubi-0.10.0*: 744 checks instead of 372, in
        #499's end-to-end check). The lane removes them before it writes its own."""
        lane = GATE[GATE.index("run_lane e2e host"):GATE.index('echo "not run: a Vita build failed"')]
        drop = lane.index('rm -f $(q "$OUT/e2e")-*.log')
        self.assertLess(drop, lane.index('python3 tools/e2e_package.py'))
        self.assertLess(drop, lane.index('cat $(q "$OUT/e2e")-*.log'))

    def test_a_vita_lane_that_built_another_version_fails(self):
        """After the lanes, the gate reads each passing Vita lane's CMakeCache.txt and the tree's version again: a
        lane that built another fails, with the two versions in its log; the check reads the tree, not $VERSION, so
        a VERSION changed on the way is caught too (audit-rubi-cleanup-8-b F6: a flag one lane lost, or VERSION set
        again before the lanes, passed the text checks above)."""
        tree = with_version(VITA, "0.10.0")
        self.assertEqual(lanes_check(tree, {"debug": "0.10.0", "normal": "0.10.0"}),
                         {"debug": ("0 42", []), "normal": ("0 42", [])})
        got = lanes_check(tree, {"debug": "0.10.0", "normal": "0.9.8"})  # the normal lane kept a cache's 0.9.8
        self.assertEqual(got["debug"], ("0 42", []))
        self.assertEqual(got["normal"], ("1 42", ["error: this build is Rubi 0.9.8 (its CMakeCache.txt), "
                                                  "vita/CMakeLists.txt says 0.10.0"]))
        both = lanes_check(tree, {"debug": "0.9.8", "normal": "0.9.8"}, before="VERSION=0.9.8")
        self.assertEqual([rc for rc, _ in both.values()], ["1 42", "1 42"])
        got = lanes_check(tree, {"debug": None, "normal": "0.10.0"})  # no cache: no version
        self.assertEqual(got["debug"][0], "1 42")
        self.assertIn("this build is Rubi of no version", got["debug"][1][0])
        # a lane that failed on its own keeps its own status and log
        self.assertEqual(lanes_check(tree, {"debug": "0.9.8", "normal": "0.10.0"}, rcs={"debug": "2 7"})["debug"],
                         ("2 7", []))
        # the line the summary shows for a failed lane (summary(): grep ... 'error:' in the lane's log)
        self.assertIn("|error:|", GATE[GATE.index("summary() {"):GATE.index("summary host-debug")])

    def test_the_e2e_lane_refuses_zips_of_another_version(self):
        """The e2e lane packages and checks the zips the Vita lanes left, named by their version: it fails when that
        is not the tree's, read again when the lane is made (not $VERSION), before it packages anything."""
        lane = GATE[GATE.index("run_lane e2e host"):GATE.index('echo "not run: a Vita build failed"')]
        made = GATE[GATE.index('if [ $vita_ok = 1 ]; then'):GATE.index("run_lane e2e host")]
        self.assertIn('e2e_version=$(q "$(tree_version)")', made)
        check = lane.index('[ \\"\\$version\\" = $e2e_version ] ||\n  { echo \\"FAIL the zips are Rubi')
        self.assertLess(lane.index('version=\\$(basename'), check)
        self.assertLess(check, lane.index("python3 tools/package_variants.py"))

    def test_a_cache_with_another_version_gives_way(self):
        """A build directory configured on the old version line keeps it on a plain configure after the bump (what
        the gate built before); the gate's -DVJO_VERSION gives the tree's."""
        cmake = shutil.which("cmake")
        if cmake is None:
            self.skipTest("no cmake on PATH")
        old = VERSION_LINE.search(with_version(VITA, "0.9.8")).group(0)
        project = 'cmake_minimum_required(VERSION 3.16)\nproject(rubi_version NONE)\n{}\nmessage(STATUS "Rubi ${{VJO_VERSION}}")\n'
        with tempfile.TemporaryDirectory() as tmp:
            source, build = Path(tmp) / "vita", Path(tmp) / "build"
            source.mkdir()

            def configure(*args):
                run = subprocess.run([cmake, "-S", str(source), "-B", str(build), *args], capture_output=True,
                                     text=True, timeout=120)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                return re.search(r"^VJO_VERSION:STRING=(.*)$", (build / "CMakeCache.txt").read_text(), re.M).group(1)

            (source / "CMakeLists.txt").write_text(project.format(old), encoding="utf-8")
            self.assertEqual(configure(), "0.9.8")  # an earlier gate run
            bumped = project.format(with_version(old, "0.10.0"))  # the release commit's line
            (source / "CMakeLists.txt").write_text(bumped, encoding="utf-8")
            self.assertEqual(configure(), "0.9.8")  # without the flag: the cache's, the old version
            self.assertEqual(configure(f"-DVJO_VERSION={gate_version(bumped)[1]}"), "0.10.0")


if __name__ == "__main__":
    unittest.main()
