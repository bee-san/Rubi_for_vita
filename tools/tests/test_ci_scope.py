"""tools/ci_scope.py, the scope of a change for CI's `changes` job and tools/fast_gate.sh: Markdown alone
builds nothing, the hooks' verified-entry table alone runs its suites and the Vita builds, and the
package's files (or CI's own) run the package e2e. The recent changes of each kind are the cases."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ci_scope  # noqa: E402


class Scope(unittest.TestCase):
    def check(self, paths, code, hooks_only, packaging):
        self.assertEqual(ci_scope.scope(paths), {"code": code, "hooks_only": hooks_only, "packaging": packaging},
                         paths)

    def test_markdown_alone_builds_nothing(self):
        self.check(["README.md", "docs/guide.md", "CHANGELOG.md"], False, False, False)

    def test_an_empty_or_unknown_change_builds_everything(self):
        self.check([], True, False, False)
        self.check(["", "  "], True, False, False)

    def test_the_verified_entry_table_alone_is_hooks_only(self):
        # #293-#295: core/hook_place.c and docs/native-hooks.md
        self.check(["core/hook_place.c", "docs/native-hooks.md"], True, True, False)
        self.check(["core/hook_place.c", "host/tests/test_hooks_engine.c", "host/tests/test_hook_place.c"],
                   True, True, False)

    def test_a_table_change_with_the_ledger_or_the_engine_is_a_full_change(self):
        # #296 moved the worker's budget too; #292 changed the engine and Luna's filters
        self.check(["core/hook_place.c", "docs/memory-ledger.md", "include/vjo_ledger_budget.h"], True, False, False)
        self.check(["core/hook_place.c", "core/hook_place.h"], True, False, False)
        self.check(["core/hook_place.c", "core/hook_luna.c", "game_ocr/hooks.c"], True, False, False)

    def test_the_package_and_its_installers_run_the_package_e2e(self):
        # a release (#298): the version in vita/CMakeLists.txt
        self.check(["CHANGELOG.md", "README.md", "docs/guide.md", "vita/CMakeLists.txt"], True, False, True)
        for path in ("tools/install_ftp.sh", "tools/install-vita.sh", "tools/package_release.py", "tools/e2e_package.py",
                     "tools/model_tiers.json", "docs/config.example.ini", "compat/adrenaline_vsh/main.c",
                     "include/vjo_api.h", "tools/tests/ftp_mock.py", "tools/hook_request.py",
                     "tools/lookup_tables.txt"):
            self.check([path], True, False, True)

    def test_ci_itself_runs_everything(self):
        for path in (".github/workflows/release.yml", "tools/ci_scope.py", "tools/fast_gate.sh",
                     "tools/gate/host.Dockerfile"):
            self.check([path], True, False, True)

    def test_source_changes_build_and_test_without_the_package(self):
        self.check(["shell/overlay.cpp", "core/config.c", "tools/tests/test_reading_stats.py"], True, False, False)
        # Markdown under a packaging directory is still only Markdown
        self.check(["compat/adrenaline_vsh/README.md"], False, False, False)

    def test_the_command_line_and_github_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            out.write_text("earlier=1\n")
            result = subprocess.run([sys.executable, str(ROOT / "tools/ci_scope.py"), "--github-output", str(out)],
                                    input="core/hook_place.c\ndocs/native-hooks.md\n", capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            want = "code=true\nhooks_only=true\npackaging=false\n"
            self.assertEqual(result.stdout, want)
            self.assertEqual(out.read_text(), "earlier=1\n" + want)
            result = subprocess.run([sys.executable, str(ROOT / "tools/ci_scope.py"), "README.md"],
                                    capture_output=True, text=True)
            self.assertEqual(result.stdout, "code=false\nhooks_only=false\npackaging=false\n")


if __name__ == "__main__":
    unittest.main()
