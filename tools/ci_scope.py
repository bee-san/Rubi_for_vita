#!/usr/bin/env python3
"""What a change needs checked: the scope that CI's `changes` job and tools/fast_gate.sh share.

    python3 tools/ci_scope.py [--github-output FILE] [PATH ...]   (or the paths one per line on stdin)

PATH is a changed file, relative to the repository's root. It prints three lines, KEY=true|false
(and appends them to FILE, the job's $GITHUB_OUTPUT):

  code        anything but Markdown changed: the builds and tests run. An empty list is code=true
              (an unknown change builds everything).
  hooks_only  every file that is not Markdown is the native hooks' verified-entry table
              (core/hook_place.c) or one of the C suites that compile it: the host jobs leave out the
              tools' Python suites (ctest -LE tools); every C suite, the ARM lane and the Vita builds
              still run (the worker's pages and the memory ledger). The merge gate, tools/fast_gate.sh,
              runs every suite whatever the scope.
  packaging   a file that the release package or tools/e2e_package.py uses changed (PACKAGING
              below), or CI itself: the release zips are made and tools/e2e_package.py runs on them.

A push to main, a tag and a manual run do not use this: CI runs everything there.
"""
import argparse
import re
import sys

DOCS = re.compile(r"\.md$")
# core/hook_place.c and the host suites that compile it beside the core library: the place, stream, stream seam
# (#366) and engine suites (CMakeLists.txt). A change to its header, core/hook_luna.c or the ledger's budgets reaches
# the core library and is a full change.
HOOK_TABLE = re.compile(r"^(core/hook_place\.c|host/tests/test_hook_place\.c|host/tests/test_hook_stream\.c"
                        r"|host/tests/test_hook_stream_seams\.c"
                        r"|host/tests/test_hooks_engine\.c|host/tests/fixtures/hook_place_forms\.inc)$")
# What the release target stages (vita/CMakeLists.txt `release`), tools/package_release.py,
# package_variants.py and package_vpk.py read, and tools/e2e_package.py runs: the package's own
# installers against a simulated Vita. CI's own files and this classifier run everything.
PACKAGING = re.compile(
    r"^(vita/CMakeLists\.txt"
    r"|tools/(package_release|package_variants|package_vpk|e2e_package|prepare_ocr|prepare_plaintext|prepare_vndb"
    r"|prepare_jmdict|install_dictionary|convert_dictionary|migrate_config|patch_tai_config|collect_vita|app_stamp"
    r"|check_variant|check_adrenaline_vsh|analyse_selftest|analyse_timeline|reading_stats|hook_request|ci_scope)\.py"
    r"|tools/lookup_tables\.txt|docs/plaintext-dictionaries\.json"  # the bundled dictionary's pin and table hints
    r"|tools/(install-vita|install_ftp|fast_gate)\.sh|tools/model_tiers\.json|tools/gate/.*"
    r"|tools/tests/(ftp_mock|test_e2e_package|test_ci_scope)\.py|tools/tests/fixtures/selftest-v2-diagnostics\.txt"
    r"|docs/config\.example\.ini|compat/adrenaline_vsh/.*|core/app_stamp\.h|core/vocr_models\.c"
    r"|include/vjo_(api|payload|vocr_sizes)\.h|\.github/.*)$")


def scope(paths):
    """{'code', 'hooks_only', 'packaging'} -> bool for these changed paths."""
    paths = [p.strip() for p in paths if p.strip()]
    code = not paths or not all(DOCS.search(p) for p in paths)
    rest = [p for p in paths if not DOCS.search(p)]
    hooks_only = code and bool(rest) and all(HOOK_TABLE.match(p) for p in rest)
    packaging = code and any(PACKAGING.match(p) for p in paths)
    return {"code": code, "hooks_only": hooks_only, "packaging": packaging}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", help="changed files (default: one per line on stdin)")
    parser.add_argument("--github-output", help="append the KEY=VALUE lines to this file as well")
    args = parser.parse_args(argv)
    paths = args.paths if args.paths else sys.stdin.read().splitlines()
    lines = [f"{k}={'true' if v else 'false'}" for k, v in scope(paths).items()]
    print("\n".join(lines))
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as out:
            out.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
