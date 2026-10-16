"""The Vita build is Release (audit-test3-smoke-dep's note 2). Without optimisation libpng compares
its jump-buffer limit in a register tools/check_settings_png_abi.py does not recognise, so the
Overlay app does not build. Every documented configure of a VitaSDK project (vita/,
tools/vita3k_smoke/) passes -DCMAKE_BUILD_TYPE=Release, and vita/ uses Release when none is given.

A documented command is code in the project's Markdown (what git tracks or would: a clone inside
the checkout, ignored files, another project's vita-vn-ocr and dated records such as reports/ or a
...-YYYYMMDD.md left out): a fenced block's lines, an inline code span (its lines joined, so a
span may wrap) or <code>, and a cmake line that starts a block (an indented block, or a paragraph);
in the two CMake files, a comment. It configures one of these projects when its source, -S's value
or a positional path outside a build directory, ends in vita or tools/vita3k_smoke (a vita-vn-ocr
checkout's vita/ is that project's). Not read: <pre> blocks, a source given by cd, $CMAKE; a fenced
Markdown example's commands are read as commands."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = {"build", ".refs", ".git", "third_party", "node_modules", "vita-vn-ocr"}
DATED = re.compile(r"(?:^|/)reports/|-20\d{6}\.md$")  # reports/, docs/todo-20261011.md, ...-20261015.md
CMAKE_HEADERS = ("vita/CMakeLists.txt", "tools/vita3k_smoke/CMakeLists.txt")
# vita, ./vita, vita/, $ROOT/vita, "$root"/vita (quotes removed), $(pwd)/vita, an absolute path ...
VITA_PROJECT = re.compile(r"(?:^|/)(?:vita|tools/vita3k_smoke)/?$")
OTHER_PROJECT = re.compile(r"(?:^|/)vita-vn-ocr(?:/|$)")
NOT_CONFIGURE = {"--build", "--install", "--open", "--workflow", "-E", "-P", "--help", "--version"}
TAKES_VALUE = {"-S", "-B", "-G", "-T", "-A", "-C", "-D", "-U", "--preset", "--toolchain", "--install-prefix"}
FENCE = re.compile(r"`{3,}|~{3,}")
CMAKE = r"(?<![\w.])cmake3?\s"  # the command (cmake, cmake3), not a file's .cmake
# A line that runs cmake: after a $ prompt, VAR=value, env, time, nice or sudo, by a path or not.
CMAKE_LINE = re.compile(r"(?:\$\s*)?(?:(?:env|time|nice|sudo)\s+|\w+=\S*\s+)*(?:\S*/)?cmake3?\s")
NOT_WRITERS = {"if", "elseif", "else", "endif", "while", "endwhile", "message"}  # conditions, and text


def joined(lines):
    """The lines with each trailing backslash's continuation joined on."""
    out, held = [], ""
    for line in lines:
        if line.rstrip().endswith("\\"):
            held += line.rstrip()[:-1] + " "
        else:
            out.append(held + line)
            held = ""
    return out + ([held] if held else [])


def code(markdown):
    """The command lines of a Markdown text: fenced blocks (to the end when unterminated), inline
    code spans and <code>, and cmake lines that start a block (a paragraph's later lines are prose)."""
    out, para, block, bare, fence = [], [], [], [], None

    def end_paragraph():
        text = " ".join(para)
        out.extend(re.findall(r"`([^`]+)`", text) + re.findall(r"<code>(.*?)</code>", text))
        para.clear()

    for line in markdown.splitlines():
        s = line.strip()
        if fence:
            if re.fullmatch(re.escape(fence[0]) + "{%d,}" % len(fence), s):
                out.extend(joined(block))
                block, fence = [], None
            else:
                block.append(line)
        elif bare or (not para and CMAKE_LINE.match(s)):
            bare.append(line)
            if not s.endswith("\\"):
                out.extend(joined(bare))
                bare = []
        elif FENCE.match(s):
            end_paragraph()
            fence = FENCE.match(s)[0]
        elif re.match(r"#{1,6}(?:\s|$)", s):  # a heading is a block of its own
            end_paragraph()
        elif s:
            para.append(s)
        else:
            end_paragraph()
    end_paragraph()
    return out + joined(block) + joined(bare)


def commands(text, comments=False):
    """The arguments of each cmake command in text (Markdown, or a CMake file's comments). A command
    ends at its line's end, &&, ;, | or a shell comment; a cmake that another one runs (cmake -E env
    ... cmake) is a command of its own."""
    if comments:
        lines = joined([line.lstrip()[1:] for line in text.splitlines() if line.lstrip().startswith("#")])
    else:
        lines = code(text)
    lines = [re.sub(r"(?:^|\s)#.*$", "", line) for line in lines]
    return [" ".join(m[1].split()) for line in lines
            for m in re.finditer(r"(?=" + CMAKE + r"(.*?)(?:&&|;|\||$))", line)]


def configures_vita(args):
    """Whether a cmake command with these arguments configures one of the VitaSDK projects."""
    # quotes removed, and a subshell's closing ) after the last argument
    tokens = [t.rstrip(")") for t in args.replace('"', "").replace("'", "").split()]
    if NOT_CONFIGURE & set(tokens):
        return False
    previous = None
    for token in tokens:
        if previous == "-S":
            source = token
        elif token.startswith("-S") and len(token) > 2:  # -Svita
            source = token[2:].lstrip("=")
        elif previous not in TAKES_VALUE and not token.startswith("-"):
            # a positional path: a source, or a build directory to configure again (left out)
            source = None if re.search(r"(?:^|/)build(?:/|$)", token) else token
        else:
            source = None
        if source and VITA_PROJECT.search(source) and not OTHER_PROJECT.search(source):
            return True
        previous = token
    return False


def release(args):
    """Whether the command's last CMAKE_BUILD_TYPE (-D, -D VAR, -DVAR:STRING, quoted) is Release."""
    values = re.findall(r"-D\s*CMAKE_BUILD_TYPE(?::\w+)?=(\S*)", args.replace('"', "").replace("'", ""))
    return bool(values) and values[-1].rstrip(")") == "Release"


def documents(root=ROOT):
    """The project's Markdown: in a checkout, what git tracks or would (untracked files that are not
    ignored; git leaves out a clone inside it, such as a vita-vn-ocr at ./vita-vn-ocr), else every
    file. Dated records (a report, a dated benchmark or to-do list) quote old commands as run."""
    paths = None
    if (root / ".git").exists():
        try:
            listed = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
                                     "--exclude-standard", "--", "*.md"], capture_output=True, timeout=60,
                                    check=True).stdout.decode("utf-8").split("\0")
            paths = [root / name for name in listed if name]
        except (OSError, subprocess.SubprocessError):
            pass
    if paths is None:
        paths = list(root.rglob("*.md"))
    return sorted(p for p in paths if p.is_file() and not SKIP & set(p.relative_to(root).parts)
                  and not DATED.search(p.relative_to(root).as_posix()))


def vita_configures(root=ROOT):
    """(file, arguments) of every documented configure of a VitaSDK project under root."""
    found = []
    for path in documents(root):
        for args in commands(path.read_text(encoding="utf-8", errors="replace")):
            if configures_vita(args):
                found.append((path.relative_to(root).as_posix(), args))
    for name in CMAKE_HEADERS:
        for args in commands((root / name).read_text(encoding="utf-8"), comments=True):
            if configures_vita(args):
                found.append((name, args))
    return found


def cmake_calls(text):
    """(name, arguments) of each command call in CMake code, line comments dropped (a # outside a
    quoted string). CMake's grammar puts each call at the start of a line."""
    kept, quoted, i = [], False, 0
    while i < len(text):
        c = text[i]
        if c == '"' and text[i - 1:i] != "\\":
            quoted = not quoted
        elif c == "#" and not quoted:
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        kept.append(c)
        i += 1
    source = "".join(kept)
    calls, end = [], 0
    for m in re.finditer(r"(?m)^[ \t]*(\w+)[ \t]*\(", source):
        if m.start() < end:
            continue  # inside the previous call's arguments
        depth, quoted, j = 0, False, m.end() - 1
        while j < len(source):
            c = source[j]
            if c == '"' and source[j - 1] != "\\":
                quoted = not quoted
            elif not quoted and c in "()":
                depth += 1 if c == "(" else -1
                if depth == 0:
                    break
            j += 1
        calls.append((m[1], source[m.end():j]))
        end = j
    return calls


def build_type_writers(text):
    """The calls in CMake code that take the variable CMAKE_BUILD_TYPE itself as an argument, outside a
    condition or a message: whatever sets, unsets or changes it, in any spelling (set (, SET(, "CMAKE_BUILD_TYPE",
    set_property(CACHE ... VALUE), string(... CMAKE_BUILD_TYPE)). Its value, ${CMAKE_BUILD_TYPE}, is
    not the variable, nor are a cache entry's STRINGS (the choices a GUI offers)."""
    found = []
    for name, args in cmake_calls(text):
        tokens = [t.strip('"') for t in re.findall(r'"(?:[^"\\]|\\.)*"|[^\s"()]+', args)]
        if name.lower() in NOT_WRITERS or "CMAKE_BUILD_TYPE" not in tokens:
            continue
        if name.lower() == "set_property" and "STRINGS" in tokens:
            continue
        found.append(f"{name}({' '.join(args.split())})")
    return found



DEFAULT = "if(NOT CMAKE_BUILD_TYPE)\n  set(CMAKE_BUILD_TYPE Release CACHE STRING "


def default_problems(cmake):
    """What is wrong with vita/CMakeLists.txt's default build type. It is the one call that sets,
    unsets or changes CMAKE_BUILD_TYPE, so nothing overrides it or a build type given on the command
    line; FORCE, so an empty entry left by an older configure is replaced; after project(), which
    creates the empty cache entry (or one from the environment's CMAKE_BUILD_TYPE), and before the
    first include() and target after it (none reads the build type while configuring today, but one
    that did would see Release)."""
    if cmake.count(DEFAULT) != 1:
        return ["no if(NOT CMAKE_BUILD_TYPE) with set(CMAKE_BUILD_TYPE Release CACHE STRING ... FORCE)"]
    problems = []
    writers = build_type_writers(cmake)
    if len(writers) != 1 or not writers[0].startswith(DEFAULT.split("\n")[1].strip()):
        problems.append(f"CMAKE_BUILD_TYPE is set elsewhere too: {writers}")
    elif not writers[0].endswith(" FORCE)"):
        problems.append("the default is not FORCE")
    at, project = cmake.index(DEFAULT), cmake.find("\nproject(VitaJPOverlay ")
    if not 0 <= project < at:
        problems.append("the default is not after project()")
    later = [i for i in (cmake.find(m, max(project, 0)) for m in ("\ninclude(", "\nadd_executable(", "\nadd_library("))
             if i >= 0]
    if later and at > min(later):
        problems.append("the default comes after an include() or a target")
    if not re.match(r"(?i)\s*endif\s*\(", cmake[at:].splitlines()[2]):
        problems.append("the default has no endif()")
    return problems


class VitaBuildType(unittest.TestCase):
    def test_every_documented_vita_configure_is_release(self):
        found = vita_configures()
        files = {name for name, _ in found}
        # Both CMake headers and at least one document, so a matcher that finds nothing cannot pass.
        self.assertLessEqual(set(CMAKE_HEADERS), files)
        self.assertTrue(any(name.endswith(".md") for name in files), found)
        missing = [f"{name}: cmake {args}" for name, args in found if not release(args)]
        self.assertEqual(missing, [], "add -DCMAKE_BUILD_TYPE=Release (the PNG ABI check needs it)")

    def test_the_matcher(self):
        def found(text):
            return [args for args in commands(text) if configures_vita(args)]

        without = {  # each configures a VitaSDK project without Release: found, and not Release
            "a fenced block, continued": "```sh\ncmake -S vita -B build/vita -DVJO_VOCR_SOURCE_DIR=/x \\\n"
                                         "  -DVJO_PLAINTEXT_SOURCE=/y\ncmake --build build/vita --target release\n```\n",
            "an unterminated fence": "```sh\ncmake -S vita -B build/vita\n",
            "a tilde fence": "~~~sh\ncd /tmp && cmake -S ${ROOT}/vita -B build/vita\n~~~\n",
            "inline, -B first, Debug": "Run `cmake -B b -S tools/vita3k_smoke -DCMAKE_BUILD_TYPE=Debug` first.\n",
            "<code>": "Configure with <code>cmake -S vita -B build/vita</code>.\n",
            "-S ./vita": "```sh\ncmake -S ./vita -B build/vita\n```\n",
            "an absolute path": "```sh\ncmake -S /home/me/vita-jp-overlay/vita -B build/vita\n```\n",
            '"$root"/vita': '```sh\ncmake -S "$root"/vita -B "$build"\n```\n',
            "$(pwd)/vita": "```sh\ncmake -S $(pwd)/vita -B build/vita\n```\n",
            "-Svita": "```sh\ncmake -Svita -Bbuild/vita\n```\n",
            "cmake3": "`cmake3 -S vita -B build/vita`\n",
            "positional ../../vita": "```sh\nmkdir -p build/vita && cd build/vita && cmake ../../vita\n```\n",
            "cmake -E env ... cmake": "```sh\ncmake -E env CMAKE_BUILD_PARALLEL_LEVEL=4 cmake -S vita -B build/vita\n```\n",
            "-S ./tools/vita3k_smoke/": "Or `cmake -S ./tools/vita3k_smoke/ -B build/smoke`.\n",
            "a span wrapped after cmake": "Configure with `cmake\n-S vita -B build/vita` first.\n",
            "a span wrapped before -S": "Configure with `cmake -B build/vita\n-S vita -DVJO_VOCR_SOURCE_DIR=/x` first.\n",
            "a span whose next line starts with cmake": "Run `cmake -S vita -B b &&\ncmake --build b` first.\n",
            "a list item's fenced block": "1. Build:\n\n   ```sh\n   cmake -S vita -B build/vita \\\n"
                                          "     -DVJO_VOCR_SOURCE_DIR=/x\n   ```\n",
            "an indented block": "Then:\n\n    cmake -S vita -B build/vita \\\n      -DVJO_VOCR_SOURCE_DIR=/x\n",
            "an indented block, $ prompt": "Then:\n\n    $ cmake -S vita -B build/vita\n",
            "an indented block, env and time": "Then:\n\n    env CMAKE_BUILD_PARALLEL_LEVEL=4 time cmake -S vita -B b\n",
            "a command after a heading": "## Build\ncmake -S vita -B build/vita\n",
            "Release, then Debug": "`cmake -S vita -DCMAKE_BUILD_TYPE=Release -DCMAKE_BUILD_TYPE=Debug`\n",
            "Release only in a comment": "```sh\ncmake -S vita -B build/vita  # not -DCMAKE_BUILD_TYPE=Release\n```\n",
            "ReleaseX": "`cmake -S vita -DCMAKE_BUILD_TYPE=ReleaseX`\n",
            "Release.": "`cmake -S vita -DCMAKE_BUILD_TYPE=Release.`\n",
            "a subshell": "```sh\n(cd /tmp && cmake -S $OLDPWD/vita)\n```\n",
        }
        for name, text in without.items():
            with self.subTest(name):
                args = found(text)
                self.assertEqual(len(args), 1, args)
                self.assertFalse(release(args[0]), args)
        accepted = {  # each configures one with Release
            "-DCMAKE_BUILD_TYPE=Release": "`cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release`.\n",
            "-D CMAKE_BUILD_TYPE=Release": "```sh\ncmake -S vita -D CMAKE_BUILD_TYPE=Release\n```\n",
            "-DCMAKE_BUILD_TYPE:STRING=Release": "```sh\ncmake -S vita -DCMAKE_BUILD_TYPE:STRING=Release\n```\n",
            "quoted": '```sh\ncmake -S "$root/vita" "-DCMAKE_BUILD_TYPE=Release"\n```\n',
            "Debug, then Release": "`cmake -S vita -DCMAKE_BUILD_TYPE=Debug -DCMAKE_BUILD_TYPE=Release`\n",
            "a subshell": "```sh\n(cd /tmp && cmake -S $OLDPWD/vita -DCMAKE_BUILD_TYPE=Release)\n```\n",
        }
        for name, text in accepted.items():
            with self.subTest(name):
                args = found(text)
                self.assertEqual(len(args), 1, args)
                self.assertTrue(release(args[0]), args)
        others = {  # no configure of a VitaSDK project of this repository
            "the host project": "`cmake -S . -B build/vita`\n",
            "a build": "```sh\ncmake --build build/vita --target release\n```\n",
            "a build of a directory named vita": "`cmake --build /tmp/vjo/vita -j4`\n",
            "cmake -E on a directory named vita": "`cmake -E make_directory /tmp/vita`\n",
            "configuring a build directory again": "`cmake build/vita`\n",
            "another project": "`cmake -S vita-vn-ocr/runtime -B b`\n",
            "vita-vn-ocr's own vita/": "```sh\ncmake -S /path/to/vita-vn-ocr/vita -B build/vvo-vita\n```\n",
            "a file's .cmake": "```sh\npython3 tools/a.py x.cmake vita\n```\n",
            "prose": "The cmake cache of vita/ keeps the build type, and -S vita names it.\n",
            "prose continuing a paragraph": "The build type stays in the cache when you run\n"
                                            "cmake on vita/ again, so give it once.\n",
            "prose after a fence closed by a longer one": "```sh\nmake\n````\nThe tree is set up by cmake -S vita here.\n",
        }
        for name, text in others.items():
            with self.subTest(name):
                self.assertEqual(found(text), [])
        header = "#   cmake -S vita -B build/vita \\\n#     -DCMAKE_BUILD_TYPE=Release\nproject(x)\n"
        self.assertEqual(commands(header, comments=True), ["-S vita -B build/vita -DCMAKE_BUILD_TYPE=Release"])

    def test_the_documents_read(self):
        names = ("README.md", "docs/a.md", "docs/new.md", "reports/r.md", "docs/todo-20261011.md", "scratch/s.md",
                 "vvo/notes/n.md", "vita-vn-ocr/notes/v.md")
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for name in names:
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text("x\n", encoding="utf-8")
            (root / ".gitignore").write_text("scratch/\n", encoding="utf-8")
            read = lambda: [p.relative_to(root).as_posix() for p in documents(root)]  # noqa: E731
            # no checkout: every file but dated records and vita-vn-ocr's
            self.assertEqual(read(), ["README.md", "docs/a.md", "docs/new.md", "scratch/s.md", "vvo/notes/n.md"])
            try:
                for repo, args in ((d, ["init", "-q"]), (str(root / "vvo"), ["init", "-q"]),
                                   (d, ["add", "README.md", "docs/a.md", "reports/r.md"])):
                    subprocess.run(["git", "-C", repo] + args, capture_output=True, timeout=60, check=True)
            except (OSError, subprocess.SubprocessError):
                self.skipTest("no git")
            # a checkout: tracked and untracked files, not the ignored ones nor a clone inside it (vvo/)
            self.assertEqual(read(), ["README.md", "docs/a.md", "docs/new.md"])

    def test_what_sets_the_build_type(self):
        sets = ['set (CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)', "SET(CMAKE_BUILD_TYPE Debug)",
                'set("CMAKE_BUILD_TYPE" Debug CACHE STRING "" FORCE)', "set(CMAKE_BUILD_TYPE\n  Debug)",
                "set_property(CACHE CMAKE_BUILD_TYPE PROPERTY VALUE Debug)", "unset(CMAKE_BUILD_TYPE CACHE)",
                'string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE)']
        for text in sets:
            with self.subTest(text):
                self.assertEqual(len(build_type_writers("project(x)\n" + text + "\n")), 1)
        reads = ["if(NOT CMAKE_BUILD_TYPE)\nendif(NOT CMAKE_BUILD_TYPE)", "# set(CMAKE_BUILD_TYPE Debug)",
                 'message(STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")', 'set(X "${CMAKE_BUILD_TYPE}")',
                 "set(MY_CMAKE_BUILD_TYPE Debug)", 'set(X "a # (set(CMAKE_BUILD_TYPE Debug)") # set(CMAKE_BUILD_TYPE x)',
                 "set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release Debug)", "message(STATUS CMAKE_BUILD_TYPE)",
                 "IF(NOT CMAKE_BUILD_TYPE)\nENDIF()", "set(X a\n  # set(CMAKE_BUILD_TYPE Debug)\n  b)",
                 "message(STATUS\n  x(CMAKE_BUILD_TYPE)\n)"]  # a line of a call's arguments is no call
        for text in reads:
            with self.subTest(text):
                self.assertEqual(build_type_writers("project(x)\n" + text + "\n"), [])
        # A ( in a string does not run the call on into the next one.
        self.assertEqual(build_type_writers('set(X "(")\nset(CMAKE_BUILD_TYPE Debug)\n'), ["set(CMAKE_BUILD_TYPE Debug)"])

    def test_vita_uses_release_when_no_build_type_is_given(self):
        self.assertEqual(default_problems((ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")), [])
        for path in sorted((ROOT / "cmake").glob("*.cmake")):  # the files vita/ includes
            self.assertEqual(build_type_writers(path.read_text(encoding="utf-8")), [], path.name)

    def test_the_default_check(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        lines = cmake[cmake.index(DEFAULT):].splitlines(keepends=True)[:3]  # if, set, endif
        block = "".join(lines)
        sdk = 'include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)\n'
        project = "project(VitaJPOverlay C CXX ASM)\n"
        broken = {
            "no default": cmake.replace(block, ""),
            "Debug": cmake.replace("set(CMAKE_BUILD_TYPE Release CACHE", "set(CMAKE_BUILD_TYPE Debug CACHE"),
            "not FORCE": cmake.replace('check" FORCE)', 'check")'),
            "no endif()": cmake.replace(block, "".join(lines[:2])),
            "before project()": cmake.replace(block, "").replace(project, block + project),
            "after include(vita.cmake)": cmake.replace(block, "").replace(sdk, sdk + block),
            "at the end": cmake.replace(block, "") + block,
            "another SET(CMAKE_BUILD_TYPE)": cmake + "SET(CMAKE_BUILD_TYPE Debug)\n",
        }
        for name, text in broken.items():
            with self.subTest(name):
                self.assertNotEqual(text, cmake)
                self.assertNotEqual(default_problems(text), [])


if __name__ == "__main__":
    unittest.main()
