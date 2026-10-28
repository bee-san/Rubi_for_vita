"""The release zip's documents are the user's (#418, P9): what vita/CMakeLists.txt's `release` target copies from docs/,
with what tools/prepare_plaintext.py and tools/prepare_vndb.py stage there (the dictionaries' pins and licences).

- Every file README.md or docs/guide.md links (in docs/, outside the history) is in the zip, so the guide's
  feature and recovery links work in the downloaded zip, and so is every Markdown document that a document of the zip
  links (its measurement records, docs/benchmarks/*.json and images, may stay on GitHub).
- No history record is: docs/history/ and docs/memory-ledger-history.md stay on GitHub (a shipped document's link to
  one is to the repository's copy).
- Every file the zip takes from docs/ exists, and is linked or named by README.md or by another document of the zip,
  or is a file a tool of the zip reads (DATA).
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import prepare_vndb  # noqa: E402
import test_docs_links as links  # noqa: E402

ENTRY = ("README.md", "docs/guide.md")
# Read by a tool the zip ships: the settings template (tools/package_release.py, migrate_config.py, install_ftp.sh)
# and JMdict's pin (tools/prepare_jmdict.py reads it beside itself, ../docs/).
DATA = {"docs/config.example.ini", "docs/jmdict-source.json"}
STAGED = {"docs/plaintext-dictionaries.json", "docs/plaintext-attribution.txt", "docs/JMdict-LICENSE.html",
          *("docs/" + name for name in prepare_vndb.DOCS)}  # tools/prepare_plaintext.py and prepare_vndb.py --stage


def history(path):
    return path.startswith("docs/history/") or path == "docs/memory-ledger-history.md"


def copied(cmake=None):
    """The docs/ files the release target copies into the zip's docs/ (vita/CMakeLists.txt, `add_custom_target(release`
    to its COMMENT: each `-E copy SOURCE... DEST` whose DEST is ${STAGE}/docs/...), in order."""
    text = cmake if cmake is not None else (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    body = text.split("add_custom_target(release", 1)[1].split("COMMENT", 1)[0]
    body = re.sub(r"#[^\n]*", "", body)
    out = []
    for command in body.split("COMMAND")[1:]:
        words = command.split()
        if words[1:3] != ["-E", "copy"] or len(words) < 5:
            continue
        if words[-1].startswith("${STAGE}/docs"):
            out += [w[len("${ROOT}/"):] for w in words[3:-1] if w.startswith("${ROOT}/docs/")]
    return out


def targets(doc, root=ROOT):
    """The repository files a document links (root-relative paths), and the docs/ paths it names in its text."""
    path = root / doc
    out = set()
    for _, target in links.links(path):
        if links.SCHEME.match(target) or target.startswith("#") or links.FIELD.match(target):
            continue
        file = (path.parent / target.split("#", 1)[0]).resolve()
        try:
            out.add(file.relative_to(root.resolve()).as_posix())
        except ValueError:
            pass
    named = set(re.findall(r"\bdocs/[A-Za-z0-9_./-]+[A-Za-z0-9]", path.read_text(encoding="utf-8")))
    return out, named


def problems(cmake=None, root=ROOT):
    """What is wrong with the zip's documents, one line each."""
    files = copied(cmake)
    zipped = set(files) | STAGED
    out = [f"{f}: copied twice" for f in sorted({f for f in files if files.count(f) > 1})]
    out += [f"{f}: copied, but no such file" for f in sorted(set(files)) if not (root / f).is_file()]
    out += [f"{f}: a history record in the zip" for f in sorted(set(files)) if history(f)]
    for doc in ENTRY:
        linked, _ = targets(doc, root)
        out += [f"{f}: {doc} links it, but the zip lacks it" for f in sorted(linked)
                if f.startswith("docs/") and not history(f) and (root / f).is_file() and f not in zipped]
    for doc in sorted(f for f in zipped if f.endswith(".md") and f not in ENTRY and (root / f).is_file()):
        linked, _ = targets(doc, root)
        out += [f"{f}: {doc} links it, but the zip lacks it" for f in sorted(linked)
                if f.startswith("docs/") and f.endswith(".md") and not history(f) and (root / f).is_file()
                and f not in zipped]
    reached = set()
    for doc in ["README.md"] + sorted(f for f in zipped if f.endswith(".md") and (root / f).is_file()):
        linked, named = targets(doc, root)
        reached |= linked | named
    out += [f"{f}: in the zip, but no document of it links or names it" for f in sorted(set(files))
            if f not in reached and f not in DATA and (root / f).is_file()]
    return out


class ZipDocs(unittest.TestCase):
    def test_the_zip_has_the_users_documents(self):
        bad = problems()
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_the_list_is_read(self):
        files = copied()
        for name in ("docs/guide.md", "docs/anki.md", "docs/config.example.ini"):
            self.assertIn(name, files)
        self.assertGreater(len(files), 30)

    def test_what_fails(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        line = "${ROOT}/docs/anki.md "
        self.assertEqual(cmake.count(line), 1)
        self.assertIn("docs/anki.md: README.md links it, but the zip lacks it", problems(cmake.replace(line, "")))
        for extra, why in (("docs/history/plugin-audit.md", "a history record in the zip"),
                           ("docs/no-such-file.md", "copied, but no such file"),
                           ("docs/benchmarks/kernel-startup-20261010.json",
                            "in the zip, but no document of it links or names it"),
                           ("docs/anki.md", "copied twice")):
            with self.subTest(extra):
                self.assertIn(f"{extra}: {why}", problems(cmake.replace(line, line + "${ROOT}/" + extra + " ")))


if __name__ == "__main__":
    unittest.main()
