"""The Vita build is Release (audit-test3-smoke-dep's note 2). Without optimisation libpng compares
its jump-buffer limit in a register tools/check_settings_png_abi.py does not recognise, so the
Overlay app does not build. Every documented configure of a VitaSDK project (vita/,
tools/vita3k_smoke/) passes -DCMAKE_BUILD_TYPE=Release, and vita/ uses Release when none is given.

A documented command is code: a fenced block's lines, an inline code span (its lines joined, so a
span may wrap) or a line that starts with cmake (an indented block); in the two CMake files, a
comment. It configures one of these projects when its source, -S's value or a positional path
outside a build directory, ends in vita or tools/vita3k_smoke."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = {"build", ".refs", ".git", "third_party", "node_modules"}
CMAKE_HEADERS = ("vita/CMakeLists.txt", "tools/vita3k_smoke/CMakeLists.txt")
# vita, ./vita, vita/, $ROOT/vita, "$root"/vita (quotes removed), $(pwd)/vita, an absolute path ...
VITA_PROJECT = re.compile(r"(?:^|/)(?:vita|tools/vita3k_smoke)/?$")
NOT_CONFIGURE = {"--build", "--install", "--open", "--workflow", "-E", "-P", "--help", "--version"}
TAKES_VALUE = {"-S", "-B", "-G", "-T", "-A", "-C", "-D", "-U", "--preset", "--toolchain", "--install-prefix"}
FENCE = re.compile(r"`{3,}|~{3,}")


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
    """The command lines of a Markdown text: fenced blocks, inline code spans and cmake lines."""
    out, para, block, bare, fence = [], [], [], [], None

    def end_paragraph():
        out.extend(re.findall(r"`([^`]+)`", " ".join(para)))
        para.clear()

    for line in markdown.splitlines():
        s = line.strip()
        if fence:
            if re.fullmatch(re.escape(fence[0]) + "{%d,}" % len(fence), s):
                out.extend(joined(block))
                block, fence = [], None
            else:
                block.append(line)
        elif bare or (re.match(r"(?:\$\s*)?cmake\s", s) and " ".join(para).count("`") % 2 == 0):
            if not bare:
                end_paragraph()
            bare.append(line)
            if not s.endswith("\\"):
                out.extend(joined(bare))
                bare = []
        elif FENCE.match(s):
            end_paragraph()
            fence = FENCE.match(s)[0]
        elif s:
            para.append(s)
        else:
            end_paragraph()
    end_paragraph()
    return out + joined(block) + joined(bare)


def commands(text, comments=False):
    """The arguments of each cmake command in text (Markdown, or a CMake file's comments). A command
    ends at its line's end, &&, ;, | or a shell comment."""
    if comments:
        lines = joined([line.lstrip()[1:] for line in text.splitlines() if line.lstrip().startswith("#")])
    else:
        lines = code(text)
    lines = [re.sub(r"(?:^|\s)#.*$", "", line) for line in lines]
    return [" ".join(m[1].split()) for line in lines for m in re.finditer(r"\bcmake\s+(.*?)(?=&&|;|\||$)", line)]


def configures_vita(args):
    """Whether a cmake command with these arguments configures one of the VitaSDK projects."""
    tokens = args.replace('"', "").replace("'", "").split()
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
        if source and VITA_PROJECT.search(source):
            return True
        previous = token
    return False


def release(args):
    """Whether the command's last CMAKE_BUILD_TYPE (-D, -D VAR, -DVAR:STRING, quoted) is Release."""
    values = re.findall(r"-D\s*CMAKE_BUILD_TYPE(?::\w+)?=(\S*)", args.replace('"', "").replace("'", ""))
    return bool(values) and values[-1].rstrip(".,:)") == "Release"


def vita_configures(root=ROOT):
    """(file, arguments) of every documented configure of a VitaSDK project under root."""
    found = []
    docs = sorted(p for p in root.rglob("*.md") if not SKIP & set(p.relative_to(root).parts))
    for path in docs:
        for args in commands(path.read_text(encoding="utf-8", errors="replace")):
            if configures_vita(args):
                found.append((path.relative_to(root).as_posix(), args))
    for name in CMAKE_HEADERS:
        for args in commands((root / name).read_text(encoding="utf-8"), comments=True):
            if configures_vita(args):
                found.append((name, args))
    return found


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
            "inline, -B first, Debug": "Run `cmake -B b -S tools/vita3k_smoke -DCMAKE_BUILD_TYPE=Debug` first.\n",
            "-S ./vita": "```sh\ncmake -S ./vita -B build/vita\n```\n",
            "an absolute path": "```sh\ncmake -S /home/me/vita-jp-overlay/vita -B build/vita\n```\n",
            '"$root"/vita': '```sh\ncmake -S "$root"/vita -B "$build"\n```\n',
            "${ROOT}/vita": "~~~\ncmake -S ${ROOT}/vita -B build/vita\n~~~\n",
            "$(pwd)/vita": "```sh\ncmake -S $(pwd)/vita -B build/vita\n```\n",
            "-Svita": "```sh\ncmake -Svita -Bbuild/vita\n```\n",
            "positional ../../vita": "```sh\nmkdir -p build/vita && cd build/vita && cmake ../../vita\n```\n",
            "-S ./tools/vita3k_smoke/": "Or `cmake -S ./tools/vita3k_smoke/ -B build/smoke`.\n",
            "a span wrapped after cmake": "Configure with `cmake\n-S vita -B build/vita` first.\n",
            "a span wrapped before -S": "Configure with `cmake -B build/vita\n-S vita -DVJO_VOCR_SOURCE_DIR=/x` first.\n",
            "a span whose next line starts with cmake": "Run `cmake -S vita -B b &&\ncmake --build b` first.\n",
            "a list item's fenced block": "1. Build:\n\n   ```sh\n   cmake -S vita -B build/vita \\\n"
                                          "     -DVJO_VOCR_SOURCE_DIR=/x\n   ```\n",
            "an indented block": "Then:\n\n    cmake -S vita -B build/vita \\\n      -DVJO_VOCR_SOURCE_DIR=/x\n",
            "Release, then Debug": "`cmake -S vita -DCMAKE_BUILD_TYPE=Release -DCMAKE_BUILD_TYPE=Debug`\n",
            "Release only in a comment": "```sh\ncmake -S vita -B build/vita  # not -DCMAKE_BUILD_TYPE=Release\n```\n",
            "ReleaseX": "`cmake -S vita -DCMAKE_BUILD_TYPE=ReleaseX`\n",
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
            "configuring a build directory again": "`cmake build/vita`\n",
            "another project": "`cmake -S vita-vn-ocr/runtime -B b`\n",
            "prose": "The cmake cache of vita/ keeps the build type, and -S vita names it.\n",
        }
        for name, text in others.items():
            with self.subTest(name):
                self.assertEqual(found(text), [])
        header = "#   cmake -S vita -B build/vita \\\n#     -DCMAKE_BUILD_TYPE=Release\nproject(x)\n"
        self.assertEqual(commands(header, comments=True), ["-S vita -B build/vita -DCMAKE_BUILD_TYPE=Release"])

    def test_vita_uses_release_when_no_build_type_is_given(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        default = "if(NOT CMAKE_BUILD_TYPE)\n  set(CMAKE_BUILD_TYPE Release CACHE STRING "
        self.assertEqual(cmake.count(default), 1)
        # Nothing else sets it, so nothing overrides the default or a build type given on the command line.
        self.assertEqual(len(re.findall(r"set\(\s*CMAKE_BUILD_TYPE\b", cmake)), 1)
        # After project(), which creates the empty cache entry (or one from the environment's
        # CMAKE_BUILD_TYPE), and FORCE, so an empty entry left by an older configure is replaced.
        self.assertLess(cmake.index("\nproject(VitaJPOverlay "), cmake.index(default))
        # And before anything that may read it while configuring: the SDK's vita.cmake, libpng's
        # FetchContent (cmake/SettingsPNG.cmake) and the targets.
        first = min(cmake.index(m) for m in ("\ninclude(", "\nadd_executable(", "\nadd_library(") if m in cmake)
        self.assertLess(cmake.index(default), first)
        block = cmake[cmake.index(default):].splitlines()
        self.assertTrue(block[1].rstrip().endswith(" FORCE)"), block[1])
        self.assertEqual(block[2].strip(), "endif()")


if __name__ == "__main__":
    unittest.main()
