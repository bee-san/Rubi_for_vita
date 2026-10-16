"""The Vita build is Release (audit-test3-smoke-dep's note 2). Without optimisation libpng compares
its jump-buffer limit in a register tools/check_settings_png_abi.py does not recognise, so the
Overlay app does not build. Every documented configure of a VitaSDK project (vita/,
tools/vita3k_smoke/) passes -DCMAKE_BUILD_TYPE=Release, and vita/ uses Release when none is given."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = {"build", ".refs", ".git", "third_party", "node_modules"}
CMAKE_HEADERS = ("vita/CMakeLists.txt", "tools/vita3k_smoke/CMakeLists.txt")
# -S naming one of the VitaSDK projects, also as "$root/vita" or ${ROOT}/vita.
VITA_SOURCE = re.compile(r"(?:^|\s)-S\s+[\"']?(?:\$\{?\w+\}?/)?(?:vita|tools/vita3k_smoke)/?[\"']?(?=\s|$)")
RELEASE = re.compile(r"(?:^|\s)-DCMAKE_BUILD_TYPE=Release(?=\s|$)")


def commands(text, comments=False):
    """The arguments of each cmake command in text (Markdown, or a CMake file's comments), with the
    lines a trailing backslash continues joined. A command ends at the line's end, a backtick, && or ;."""
    lines = text.splitlines()
    if comments:
        lines = [line.lstrip()[1:] for line in lines if line.lstrip().startswith("#")]
    logical, held = [], ""
    for line in lines:
        if line.rstrip().endswith("\\"):
            held += line.rstrip()[:-1] + " "
        else:
            logical.append(held + line)
            held = ""
    logical.append(held)
    return [" ".join(m[1].split()) for line in logical
            for m in re.finditer(r"\bcmake\s+(.*?)(?=`|&&|;|$)", line)]


def vita_configures(root=ROOT):
    """(file, arguments) of every documented configure of a VitaSDK project under root."""
    found = []
    docs = sorted(p for p in root.rglob("*.md") if not SKIP & set(p.relative_to(root).parts))
    for path in docs:
        for args in commands(path.read_text(encoding="utf-8", errors="replace")):
            if VITA_SOURCE.search(args):
                found.append((path.relative_to(root).as_posix(), args))
    for name in CMAKE_HEADERS:
        for args in commands((root / name).read_text(encoding="utf-8"), comments=True):
            if VITA_SOURCE.search(args):
                found.append((name, args))
    return found


class VitaBuildType(unittest.TestCase):
    def test_every_documented_vita_configure_is_release(self):
        found = vita_configures()
        files = {name for name, _ in found}
        # Both CMake headers and at least one document, so a matcher that finds nothing cannot pass.
        self.assertLessEqual(set(CMAKE_HEADERS), files)
        self.assertTrue(any(name.endswith(".md") for name in files), found)
        missing = [f"{name}: cmake {args}" for name, args in found if not RELEASE.search(args)]
        self.assertEqual(missing, [], "add -DCMAKE_BUILD_TYPE=Release (the PNG ABI check needs it)")

    def test_the_matcher_finds_a_configure_without_it(self):
        text = ("```sh\ncmake -S vita -B build/vita -DVJO_VOCR_SOURCE_DIR=/x \\\n"
                "  -DVJO_PLAINTEXT_SOURCE=/y\ncmake --build build/vita --target release\n```\n"
                "and `cmake -B b -S tools/vita3k_smoke -DCMAKE_BUILD_TYPE=Debug`; host: `cmake -S . -B h`\n")
        args = [a for a in commands(text) if VITA_SOURCE.search(a)]
        self.assertEqual(args, ["-S vita -B build/vita -DVJO_VOCR_SOURCE_DIR=/x -DVJO_PLAINTEXT_SOURCE=/y",
                                "-B b -S tools/vita3k_smoke -DCMAKE_BUILD_TYPE=Debug"])
        self.assertFalse(any(RELEASE.search(a) for a in args))
        header = "#   cmake -S vita -B build/vita \\\n#     -DCMAKE_BUILD_TYPE=Release\nproject(x)\n"
        self.assertEqual(commands(header, comments=True), ["-S vita -B build/vita -DCMAKE_BUILD_TYPE=Release"])
        self.assertTrue(RELEASE.search(commands(header, comments=True)[0]))
        self.assertFalse(RELEASE.search("-S vita -DCMAKE_BUILD_TYPE=ReleaseX"))

    def test_vita_uses_release_when_no_build_type_is_given(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        default = "if(NOT CMAKE_BUILD_TYPE)\n  set(CMAKE_BUILD_TYPE Release CACHE STRING "
        self.assertEqual(cmake.count(default), 1)
        # After project(), which creates the empty cache entry (or one from the environment's
        # CMAKE_BUILD_TYPE), and FORCE, so an empty entry left by an older configure is replaced.
        self.assertLess(cmake.index("\nproject(VitaJPOverlay "), cmake.index(default))
        line = cmake[cmake.index(default):].splitlines()[1]
        self.assertTrue(line.rstrip().endswith(" FORCE)"), line)


if __name__ == "__main__":
    unittest.main()
