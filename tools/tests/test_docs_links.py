"""Every relative link in the project's documents resolves (#418, P9): every Markdown file git tracks (or would: a
new one), but those of SKIP. A link's file (or directory) is in the repository (tracked, or new and not ignored: a
file of build/ does not count), and its #anchor is a heading or an `id` of the Markdown file it names (or of the
same file), as GitHub makes them.
Checked against GitHub's own rendering of the 75 documents on main b55c5f5: the same 881 heading anchors, and the
same relative links (but the test-build issue's template fields, below).

What counts as a link: an inline link or image, `[text](target)` (its text may wrap, nest an image or hold an
escaped bracket), a reference definition, `[label]: target`, and an HTML `href`, `src` or `srcset` (quoted or not);
never what a fenced or indented code block, a code span or an HTML comment holds (whichever opens first wins, as in
GitHub's parser), nor a footnote's definition. A link with a scheme (https:, mailto:) is not checked, nor a
template's field (`{VPK_URL}`: the test-build issue's skeleton), nor the anchor of a file that is not Markdown (a
source file's #L12).

    python3 tools/tests/test_docs_links.py --list    the broken links, one per line (exit 1 if any)
"""
import html
import re
import subprocess
import sys
import tempfile
import unicodedata
import unittest
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
SKIP = {"node_modules", "build", ".refs", ".git"}  # an npm install's, the builds', the pinned checkouts'

FENCE = re.compile(r"^[ \t]*(`{3,}|~{3,})")  # indented too: a fence in a list item
ATX = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`)((?:(?!\n[ \t]*\n).)+?)(?<!`)\1(?!`)", re.S)
COMMENT = re.compile(r"<!--.*?-->", re.S)
# [text]( with brackets nested two deep inside the text (an image in a link: [![alt](a.png)](b.md)), escapes (\]);
# the text may wrap, but not over a blank line. Matched at every [ (a lookahead), so the image inside is found too.
_TEXT = r"(?:\\.|[^\[\]\n\\]|\n(?![ \t]*\n))"
INLINE = re.compile(r"(?=(\[(?:" + _TEXT + r"|\[(?:" + _TEXT + r"|\[" + _TEXT + r"*\])*\])*\]\())")
DEFINITION = re.compile(r"^ {0,3}\[(?!\^)([^\]\n]+)\]:[ \t]*(<[^>\n]*>|\S+)", re.M)
# a tag: its attributes' values may hold >; href, src or srcset, quoted or not
TAG = re.compile(r"""<[a-zA-Z][a-zA-Z0-9-]*(?:\s+[^\s"'>/=]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s"'=<>`]+))?)*\s*/?>""")
ATTR = re.compile(r"""\s([^\s"'>/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?""")
HTML_HEADING = re.compile(r"<h([1-6])\b[^>]*>(.*?)</h\1\s*>", re.S | re.I)
# the elements whose id GitHub keeps (its sanitizer drops input, meta, form controls and their names)
ID_TAGS = {"a", "span", "div", "section", "p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "table", "td", "th", "tr",
           "details", "summary", "sup", "sub", "dl", "dt", "dd", "figure", "img", "picture", "ul", "ol", "blockquote"}
HTML_BLOCK = re.compile(r"^ {0,3}<(?:/?(?:div|table|details|section|picture|p|ul|ol|dl|pre|blockquote|figure|h[1-6])"
                        r"\b|!--)", re.I)
SCHEME = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
FIELD = re.compile(r"^\{[A-Z0-9_]+\}$")  # a template's field, filled per use


def repository(root=ROOT):
    """The repository's files and directories (root-relative): git's tracked and new ones, but the ignored; None
    where there is no git checkout (a copy of the tree), then the file system decides."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             capture_output=True, timeout=60, check=True).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    files = {f for f in out.decode("utf-8", "surrogateescape").split("\0") if f}
    if not files:
        return None
    dirs = {"."}
    for f in files:
        parts = f.split("/")[:-1]
        dirs |= {"/".join(parts[:i]) for i in range(1, len(parts) + 1)}
    return files | dirs


def documents(root=ROOT):
    """The documents read, in path order: every Markdown file of the repository, but those under SKIP's names."""
    known = repository(root)
    if known is not None:
        paths = [root / f for f in known if f.endswith(".md") and (root / f).is_file()]
    else:
        paths = [p for p in root.rglob("*.md") if p.is_file()]
    return sorted(p for p in paths if not SKIP & set(p.relative_to(root).parts))


def blank(match):
    """The match's text with every character but a newline replaced: positions and line numbers stay."""
    return re.sub(r"[^\n]", " ", match.group(0))


def indented_code(lines):
    """Which lines are an indented code block's: 4 spaces (or a tab) after a blank line or another such line, outside
    a list item (whose continuation lines are indented too)."""
    out, prev_blank, in_code, in_list = [], True, False, False
    for line in lines:
        blank_line = not line.strip()
        if re.match(r"^ {0,3}(?:[-*+]|\d+[.)])[ \t]", line):
            in_list = True
        elif not blank_line and not line.startswith((" ", "\t")):
            in_list = False
        code = not blank_line and not in_list and line.startswith(("    ", "\t")) and (prev_blank or in_code)
        in_code = code or (in_code and blank_line)
        out.append(code)
        prev_blank = blank_line
    return out


def prose(text, spans=True):
    """The text with fenced and indented code blocks, HTML comments and (spans) code spans blanked out (a span's text
    becomes x's, so link text holding one stays link text). Comments and spans are taken in document order: whichever
    opens first wins (a span holding <!-- is a span)."""
    lines, fence = [], None
    raw = text.split("\n")
    for line, code in zip(raw, indented_code(raw)):
        m = FENCE.match(line)
        if fence:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and not line.strip(" `~"):
                fence = None
            lines.append("")
        elif m and not (m.group(1)[0] == "`" and "`" in line[m.end():]):
            fence = m.group(1)
            lines.append("")
        elif code:
            lines.append("")
        else:
            lines.append(line)
    text, out, i = "\n".join(lines), [], 0
    while i < len(text):
        c, s = COMMENT.match(text, i) if text.startswith("<!--", i) else None, None
        if text[i] == "`" and (i == 0 or text[i - 1] != "`"):
            s = CODE_SPAN.match(text, i)
        if c:
            out.append(blank(c))
            i = c.end()
        elif s:
            out.append(re.sub(r"[^\n]", "x", s.group(0)) if spans else s.group(0))
            i = s.end()
        elif text[i] == "`":
            run = len(text[i:]) - len(text[i:].lstrip("`"))
            out.append(text[i:i + run])
            i += run
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def heading_text(raw):
    """A heading's text as GitHub renders it: code spans' text verbatim; no images (not even their alt text), links'
    text only, autolinks' text, no tags, no emphasis marks, no escapes; entities decoded."""
    parts, i = [], 0
    for m in CODE_SPAN.finditer(raw):  # a code span's text is literal: <h1> in it is text, not a tag
        parts.append((raw[i:m.start()], False))
        parts.append((m.group(2).strip(" ") if m.group(2).strip(" ") else m.group(2), True))
        i = m.end()
    parts.append((raw[i:], False))
    out = []
    for text, code in parts:
        if not code:
            text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)
            text = re.sub(r"\[([^\]]*)\](?:\([^)]*\)|\[[^\]]*\])?", r"\1", text)
            text = re.sub(r"<((?:https?|mailto|ftp):[^>\s]*)>", r"\1", text)
            text = re.sub(r"<[^>]+>", "", text)
            text = re.sub(r"(?<![\w\\])(\*{1,3}|_{1,3})(?=\S)(.+?)(?<=\S)\1(?!\w)", r"\2", text)
            text = re.sub(r"\\([!-/:-@\[-`{-~])", r"\1", text)
            text = html.unescape(text)
        out.append(text)
    return "".join(out)


def slug(text):
    """GitHub's anchor of a heading (its TableOfContentsFilter): lower case, every character that is not a
    letter, a mark, a decimal digit, a connector (_), a hyphen or a space dropped, each space a hyphen. The text is
    the heading's rendered text, of which only the ends are trimmed (a dropped image leaves its space: '-imaged')."""
    kept = [c for c in text.lower()
            if c in "- " or unicodedata.category(c)[0] in "LM" or unicodedata.category(c) in ("Nd", "Pc")]
    return "".join(kept).replace(" ", "-")


_anchors = {}


def anchors(path):
    """Every anchor of a Markdown file: its headings' (Markdown and HTML ones, in order: a repeated one gets
    -1, -2, ...) and its HTML ids."""
    path = path.resolve()
    if path not in _anchors:
        text = path.read_text(encoding="utf-8")
        unspanned = prose(text, spans=False)
        headings, at, in_html = [], 0, False
        lines = unspanned.split("\n")
        for k, line in enumerate(lines):
            if in_html:
                in_html = bool(line.strip())  # an HTML block ends at a blank line
            elif HTML_BLOCK.match(line):
                in_html = bool(line.strip())
            else:
                m = ATX.match(line) or (re.match(r"^ {0,3}(?:>[ \t]?|(?:[-*+]|\d+[.)])[ \t]+)(#{1,6})(?:[ \t]+(.*?))?"
                                                 r"(?:[ \t]+#+)?[ \t]*$", line))
                if m:
                    headings.append((at, m.group(2) or ""))
                elif (k + 1 < len(lines) and re.match(r"^ {0,3}(?:=+|-+)[ \t]*$", lines[k + 1]) and line.strip()
                      and not re.match(r"^ {0,3}(?:[-*+>]|\d+[.)]|\||#)", line)
                      and (k == 0 or not lines[k - 1].strip())):
                    headings.append((at, line.strip()))  # a setext heading (a one-line paragraph)
            at += len(line) + 1
        headings += [(m.start(), m.group(2)) for m in HTML_HEADING.finditer(unspanned)]
        found, used = set(), set()
        for _, raw in sorted(headings):
            base = slug(heading_text(raw.strip()))
            name, n = base, 0
            while name in used:  # GitHub's rule: the first free name-N
                n += 1
                name = f"{base}-{n}"
            used.add(name)
            found.add(name)
        for tag in TAG.finditer(prose(text)):
            if tag.group(0)[1:].split(None, 1)[0].rstrip("/>").lower() not in ID_TAGS:
                continue
            for name, *values in ATTR.findall(tag.group(0)):
                if name.lower() in ("id", "name"):
                    found.add(next((v for v in values if v), ""))
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
        target = destination(text, m.start() + len(m.group(1)))
        if target is not None:
            found.append((m.start(), target))
    for m in DEFINITION.finditer(text):
        found.append((m.start(), m.group(2).strip("<>")))
    for tag in TAG.finditer(text):
        for name, *values in ATTR.findall(tag.group(0)):
            value = next((v for v in values if v), None)
            if value is None:
                continue
            if name.lower() in ("href", "src"):
                found.append((tag.start(), value))
            elif name.lower() == "srcset":
                found += [(tag.start(), url.split()[0]) for url in value.split(",") if url.split()]
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


_known = {}


def check(doc, target, root=ROOT):
    """Why a link of doc does not resolve, or None when it does (or is not ours to check)."""
    if not target or SCHEME.match(target) or target.startswith("//") or FIELD.match(target):
        return None
    path, _, fragment = target.partition("#")
    path = unquote(path.split("?", 1)[0])
    if path.startswith("/"):
        return "a path from the root: GitHub resolves it, but a copy of the document (the release zip's) cannot"
    file = (doc.parent / path).resolve() if path else doc.resolve()
    try:
        rel = file.relative_to(root.resolve()).as_posix()
    except ValueError:
        return "outside the repository"
    if root not in _known:
        _known[root] = repository(root)
    known = _known[root]
    if not file.exists() or (known is not None and rel not in known):
        return "no such file" if not file.exists() else "not in the repository (ignored)"
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
                     "reports/yomitan-check.md", "third_party/README.md", "third_party/sc3-charsets/README.md",
                     "settings/assets/ICON-SOURCE.md", "compat/adrenaline_vsh/README.md"):
            self.assertIn(name, docs)
        self.assertFalse([d for d in docs if d.startswith(("build/", ".refs/"))])
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
            # #418's audit (F2), each as GitHub's rendering of the heading makes it
            "The _foo_ bar": "the-foo-bar",
            "snake_case_name": "snake_case_name",
            "Q&amp;A and A&nbsp;B": "qa-and-ab",
            "![alt text](pic.png) Imaged": "-imaged",
            "The `<h1>` tag": "the-h1-tag",
            "See <https://example.com/x>": "see-httpsexamplecomx",
            "Party :tada: time": "party-tada-time",
        }
        for text, want in cases.items():
            with self.subTest(text):
                self.assertEqual(slug(heading_text(text)), want)

    def test_what_is_and_is_not_a_link(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "docs").mkdir()
            (root / "docs/b.md").write_text(
                "# B\n\n## Two\n\n## Two\n\n## Two 1\n\n<a id=\"here\"></a>\n"
                "<h2 align=\"center\">HTML <b>heading</b></h2>\n\n"
                "- item\n\n    ```\n    [in a list's fence](c.md)\n    ```\n\nSetext\n======\n\n> ## Quoted\n\n"
                "<div>\n## Not a heading\n</div>\n\n<input name=\"dropped\"> <span id=\"kept\">x</span>\n",
                encoding="utf-8")
            (root / "docs/a.md").write_text(
                "# A title\n\n"
                "[ok](b.md) [ok](b.md#two) [ok](b.md#two-1) [ok](b.md#here) [ok](b.md#html-heading) [ok](#a-title) "
                "[ok](../docs/) [ok](b.md#two-1-1) [ok](b.md#setext) [ok](b.md#quoted) [ok](b.md#kept)\n"
                "[gone](c.md) [no anchor](b.md#three) [no anchor](#nowhere) [up](../../x.md)\n"
                "[a link that\nwraps](c.md#wrapped) ![image](img/p.png \"title\") <img src=\"pic.png\">\n"
                "`[code](c.md)` and ``[code `span`](c.md)`` are not links, <!-- [nor](c.md) --> this\n\n"
                "```sh\n[fenced](c.md)\n```\n\n"
                "[ref]: c.md#def\n[ext](https://example.com/x.md) [mail](mailto:a@b) [src](../src.c#L12)\n\n"
                "[![badge](badge.png)](b.md) `<!--` then [after a span's opener](span.md) `-->`\n"
                "<picture><source srcset=\"set.png 2x, b.md\"></picture> <a href=bare.md>x</a> "
                "<a title=\"a>b\" href=\"gt.md\">x</a> [a \\] b](esc.md)\n\n"
                "[gone anchors](b.md#not-a-heading) [x](b.md#dropped) [y](b.md#two-2)\n\n"
                "    [indented code](indented.md)\n\n"
                "[^1]: A footnote, not a link to A.\n[ignored](../build/out.txt)\n",
                encoding="utf-8")
            (root / "README.md").write_text("[guide](docs/a.md#a-title) [root](/docs/a.md)\n", encoding="utf-8")
            (root / "src.c").write_text("int x;\n", encoding="utf-8")
            (root / "build").mkdir()
            (root / "build/out.txt").write_text("x\n", encoding="utf-8")
            (root / ".gitignore").write_text("build/\n", encoding="utf-8")
            try:  # a checkout: what git ignores is not the repository's, though it exists here
                subprocess.run(["git", "-C", d, "init", "-q"], capture_output=True, timeout=60, check=True)
            except (OSError, subprocess.SubprocessError):
                self.skipTest("no git")
            self.assertEqual(broken(root), [
                "README.md:1: /docs/a.md: a path from the root: GitHub resolves it, but a copy of the document (the "
                "release zip's) cannot",
                "docs/a.md:4: c.md: no such file",
                "docs/a.md:4: b.md#three: no such anchor",
                "docs/a.md:4: #nowhere: no such anchor",
                "docs/a.md:4: ../../x.md: outside the repository",
                "docs/a.md:5: c.md#wrapped: no such file",
                "docs/a.md:6: img/p.png: no such file",
                "docs/a.md:6: pic.png: no such file",
                "docs/a.md:13: c.md#def: no such file",
                "docs/a.md:16: badge.png: no such file",
                "docs/a.md:16: span.md: no such file",
                "docs/a.md:17: set.png: no such file",
                "docs/a.md:17: bare.md: no such file",
                "docs/a.md:17: gt.md: no such file",
                "docs/a.md:17: esc.md: no such file",
                "docs/a.md:19: b.md#not-a-heading: no such anchor",
                "docs/a.md:19: b.md#dropped: no such anchor",
                "docs/a.md:19: b.md#two-2: no such anchor",
                "docs/a.md:24: ../build/out.txt: not in the repository (ignored)",
            ])


if __name__ == "__main__":
    if sys.argv[1:] == ["--list"]:
        found = broken()
        print("\n".join(found) if found else "every link resolves")
        sys.exit(1 if found else 0)
    unittest.main()
