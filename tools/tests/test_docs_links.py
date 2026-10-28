"""Every relative link in the project's documents resolves (#418, P9): README.md, CHANGELOG.md, AGENTS.md,
third_party/README.md and every Markdown file under docs/ and reports/. A link's file (or directory) exists, and its
#anchor is a heading or an `id`/`name` of the Markdown file it names (or of the same file), as GitHub makes them.
Checked against GitHub's own rendering of the 75 documents on main b55c5f5: the same 881 heading anchors, and the
same relative links (but the test-build issue's template fields, below).

What counts as a link: an inline link or image, `[text](target)` (its text may wrap), a reference definition,
`[label]: target`, and an HTML `href`/`src` attribute; never what a fenced code block, a code span or an HTML
comment holds. A link with a scheme (https:, mailto:) is not checked, nor a template's field (`{VPK_URL}`: the
test-build issue's skeleton), nor the anchor of a file that is not Markdown (a source file's #L12).

    python3 tools/tests/test_docs_links.py --list    the broken links, one per line (exit 1 if any)
"""
import re
import sys
import tempfile
import unicodedata
import unittest
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
TOP = ("README.md", "CHANGELOG.md", "AGENTS.md", "third_party/README.md")
TREES = ("docs", "reports")
SKIP = {"node_modules"}  # docs/promo's npm install, when one was made there: not the project's

FENCE = re.compile(r"^[ \t]*(`{3,}|~{3,})")  # indented too: a fence in a list item
ATX = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`)((?:(?!\n[ \t]*\n).)+?)(?<!`)\1(?!`)", re.S)
COMMENT = re.compile(r"<!--.*?-->", re.S)
# [text]( with one level of brackets inside the text; the text may wrap, but not over a blank line
INLINE = re.compile(r"\[(?:[^\[\]\n]|\n(?![ \t]*\n)|\[[^\[\]\n]*\])*\]\(")
DEFINITION = re.compile(r"^ {0,3}\[([^\]\n]+)\]:[ \t]*(<[^>\n]*>|\S+)", re.M)
ATTRIBUTE = re.compile(r"""<[a-zA-Z][^>]*?\s(?:href|src)\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.S)
HTML_ANCHOR = re.compile(r"""<[a-zA-Z][^>]*?\s(?:id|name)\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.S)
HTML_HEADING = re.compile(r"<h([1-6])\b[^>]*>(.*?)</h\1\s*>", re.S | re.I)
SCHEME = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
FIELD = re.compile(r"^\{[A-Z0-9_]+\}$")  # a template's field, filled per use


def documents(root=ROOT):
    """The documents read, in path order."""
    paths = [root / name for name in TOP if (root / name).is_file()]
    for tree in TREES:
        paths += sorted(p for p in (root / tree).rglob("*.md")
                        if p.is_file() and not SKIP & set(p.relative_to(root).parts))
    return paths


def blank(match):
    """The match's text with every character but a newline replaced: positions and line numbers stay."""
    return re.sub(r"[^\n]", " ", match.group(0))


def prose(text, spans=True):
    """The text with fenced code blocks, HTML comments and (spans) code spans blanked out (a span's text
    becomes x's, so link text holding one stays link text)."""
    lines, fence = [], None
    for line in text.split("\n"):
        m = FENCE.match(line)
        if fence:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and not line.strip(" `~"):
                fence = None
            lines.append("")
        elif m and not (m.group(1)[0] == "`" and "`" in line[m.end():]):
            fence = m.group(1)
            lines.append("")
        else:
            lines.append(line)
    text = COMMENT.sub(blank, "\n".join(lines))
    return CODE_SPAN.sub(lambda m: re.sub(r"[^\n]", "x", m.group(0)), text) if spans else text


def heading_text(raw):
    """A heading's text as GitHub renders it: no images, links' text only, no code marks, tags or escapes."""
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", raw)
    text = re.sub(r"\[([^\]]*)\](?:\([^)]*\)|\[[^\]]*\])?", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"`+", "", text)
    return re.sub(r"\\([!-/:-@\[-`{-~])", r"\1", text)


def slug(text):
    """GitHub's anchor of a heading (its TableOfContentsFilter): lower case, every character that is not a
    letter, a mark, a decimal digit, a connector (_), a hyphen or a space dropped, each space a hyphen."""
    kept = [c for c in text.strip().lower()
            if c in "- " or unicodedata.category(c)[0] in "LM" or unicodedata.category(c) in ("Nd", "Pc")]
    return "".join(kept).replace(" ", "-")


_anchors = {}


def anchors(path):
    """Every anchor of a Markdown file: its headings' (Markdown and HTML ones, in order: a repeated one gets
    -1, -2, ...) and its HTML ids."""
    path = path.resolve()
    if path not in _anchors:
        text = path.read_text(encoding="utf-8")
        headings, at = [], 0
        for line in prose(text, spans=False).split("\n"):
            m = ATX.match(line)
            if m:
                headings.append((at, m.group(2) or ""))
            at += len(line) + 1
        blanked = prose(text)
        headings += [(m.start(), m.group(2)) for m in HTML_HEADING.finditer(blanked)]
        found, seen = set(), {}
        for _, raw in sorted(headings):
            base = slug(heading_text(raw))
            n = seen.get(base, 0)
            seen[base] = n + 1
            found.add(base if n == 0 else f"{base}-{n}")
        found |= {a or b for a, b in HTML_ANCHOR.findall(blanked)}
        _anchors[path] = found
    return _anchors[path]


def destination(text, start):
    """The destination of an inline link whose '(' ends at start, or None when what follows is not one."""
    i = start
    while i < len(text) and text[i] in " \t":
        i += 1
    if i < len(text) and text[i] == "<":
        end = text.find(">", i)
        if end < 0 or "\n" in text[i:end]:
            return None
        return text[i + 1:end]
    depth, j = 0, i
    while j < len(text):
        c = text[j]
        if c in " \t\n":
            break
        if c == "(":
            depth += 1
        elif c == ")":
            if depth == 0:
                break
            depth -= 1
        j += 1
    rest = text[j:j + 300].lstrip(" \t\n")
    if not rest or rest[0] not in ")\"'(":  # the link ends, or a title follows
        return None
    return text[i:j]


def links(path):
    """(line, target) for every link of a document, in order."""
    text = prose(path.read_text(encoding="utf-8"))
    found = []  # (position, target)
    for m in INLINE.finditer(text):
        target = destination(text, m.end())
        if target is not None:
            found.append((m.start(), target))
    for m in DEFINITION.finditer(text):
        found.append((m.start(), m.group(2).strip("<>")))
    for m in ATTRIBUTE.finditer(text):
        found.append((m.start(), m.group(1) if m.group(1) is not None else m.group(2)))
    return [(text.count("\n", 0, at) + 1, target) for at, target in sorted(found)]


def broken(root=ROOT):
    """'path:line: target: why' for every link of the documents that does not resolve."""
    root = root.resolve()
    out = []
    for doc in documents(root):
        for line, target in links(doc):
            why = check(doc, target, root)
            if why:
                out.append(f"{doc.relative_to(root).as_posix()}:{line}: {target}: {why}")
    return out


def check(doc, target, root=ROOT):
    """Why a link of doc does not resolve, or None when it does (or is not ours to check)."""
    if not target or SCHEME.match(target) or target.startswith("//") or FIELD.match(target):
        return None
    path, _, fragment = target.partition("#")
    path = unquote(path.split("?", 1)[0])
    if path.startswith("/"):
        return "a path from the root, which GitHub does not resolve in the repository"
    file = (doc.parent / path).resolve() if path else doc.resolve()
    try:
        file.relative_to(root)
    except ValueError:
        return "outside the repository"
    if not file.exists():
        return "no such file"
    if fragment and file.is_file() and file.suffix == ".md" and unquote(fragment) not in anchors(file):
        return "no such anchor"
    return None


class DocsLinks(unittest.TestCase):
    def test_every_relative_link_resolves(self):
        bad = broken()
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_the_documents_read(self):
        docs = [p.relative_to(ROOT).as_posix() for p in documents()]
        for name in ("README.md", "CHANGELOG.md", "docs/guide.md", "docs/memory-ledger.md",
                     "reports/yomitan-check.md", "third_party/README.md"):
            self.assertIn(name, docs)
        # the links are found at all: 1,216 relative ones with an anchor on main b55c5f5
        anchored = [t for d in docs for _, t in links(ROOT / d) if "#" in t and not SCHEME.match(t)]
        self.assertGreater(len(anchored), 1000)

    def test_slugs_are_githubs(self):
        cases = {
            "#338's audit: the trusted hooks' fixes (no API change)":
                "338s-audit-the-trusted-hooks-fixes-no-api-change",
            "Head gate and two-level head (API 42, vita-vn-ocr #120 and #122)":
                "head-gate-and-two-level-head-api-42-vita-vn-ocr-120-and-122",
            "Duplicate-aware mining (#314): the Rubi app only": "duplicate-aware-mining-314-the-rubi-app-only",
            "Yomitan's deinflection rules, a packed table": "yomitans-deinflection-rules-a-packed-table",
            "Vertical text (VITA-VN-OCR #99)": "vertical-text-vita-vn-ocr-99",
            "The `ocr_finder` key, [its doc](a.md)": "the-ocr_finder-key-its-doc",
            "**Bold** and *em*": "bold-and-em",
            "Code:Realize ～創世の姫君～": "coderealize-創世の姫君",
            "ALIA's CARNIVAL! サクラメント": "alias-carnival-サクラメント",
            "1.  two  spaces": "1--two--spaces",
            "Install (0.9.8 → 0.10.0)": "install-098--0100",
        }
        for text, want in cases.items():
            with self.subTest(text):
                self.assertEqual(slug(heading_text(text)), want)

    def test_what_is_and_is_not_a_link(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "docs").mkdir()
            (root / "docs/b.md").write_text("# B\n\n## Two\n\n## Two\n\n<a id=\"here\"></a>\n<h2 align=\"center\">HTML "
                                            "<b>heading</b></h2>\n\n- item\n\n    ```\n    [in a list's fence](c.md)\n"
                                            "    ```\n", encoding="utf-8")
            (root / "docs/a.md").write_text(
                "# A title\n\n"
                "[ok](b.md) [ok](b.md#two) [ok](b.md#two-1) [ok](b.md#here) [ok](b.md#html-heading) [ok](#a-title) "
                "[ok](../docs/)\n"
                "[gone](c.md) [no anchor](b.md#three) [no anchor](#nowhere) [up](../../x.md)\n"
                "[a link that\nwraps](c.md#wrapped) ![image](img/p.png \"title\") <img src=\"pic.png\">\n"
                "`[code](c.md)` and ``[code `span`](c.md)`` are not links, <!-- [nor](c.md) --> this\n\n"
                "```sh\n[fenced](c.md)\n```\n\n"
                "[ref]: c.md#def\n[ext](https://example.com/x.md) [mail](mailto:a@b) [src](../src.c#L12)\n",
                encoding="utf-8")
            (root / "README.md").write_text("[guide](docs/a.md#a-title) [root](/docs/a.md)\n", encoding="utf-8")
            (root / "src.c").write_text("int x;\n", encoding="utf-8")
            self.assertEqual(broken(root), [
                "README.md:1: /docs/a.md: a path from the root, which GitHub does not resolve in the repository",
                "docs/a.md:4: c.md: no such file",
                "docs/a.md:4: b.md#three: no such anchor",
                "docs/a.md:4: #nowhere: no such anchor",
                "docs/a.md:4: ../../x.md: outside the repository",
                "docs/a.md:5: c.md#wrapped: no such file",
                "docs/a.md:6: img/p.png: no such file",
                "docs/a.md:6: pic.png: no such file",
                "docs/a.md:13: c.md#def: no such file",
            ])


if __name__ == "__main__":
    if sys.argv[1:] == ["--list"]:
        found = broken()
        print("\n".join(found) if found else "every link resolves")
        sys.exit(1 if found else 0)
    unittest.main()
