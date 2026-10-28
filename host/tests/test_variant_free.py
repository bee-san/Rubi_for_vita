"""host/tests/variant_free.py's rules on small inputs: each unit's preprocessed text with VJO_DEBUG flipped and the
model directories changed, a derivation over three made-up lane configurations (a test, link or compile line that one
of them has otherwise), and one over a real CMake project configured three times by the script itself.

    python3 host/tests/test_variant_free.py
"""
import importlib.util
import json
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("variant_free", HERE / "variant_free.py")
vf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vf)
CC = shutil.which("cc") or shutil.which("gcc")
CMAKE, CTEST = shutil.which("cmake"), shutil.which("ctest")


class Commands(unittest.TestCase):
    def test_commands_are_parsed_whole(self):
        cmds = vf.cmake_commands('foo(a "b ) # c" $<IF:$<BOOL:x>,y,z>) # bar(no)\nbaz(\n  d\n  (e)\n)\n')
        self.assertEqual(cmds, [("foo", ["a", '"b ) # c"', "$<IF:$<BOOL:x>,y,z>"]), ("baz", ["d", "e"])])


@unittest.skipUnless(CC, "no C compiler")
class Units(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "build.h").write_text("#if VJO_DEBUG\n#define NAME \"debug\"\n#else\n#define NAME \"normal\"\n#endif\n")

    def tearDown(self):
        self.tmp.cleanup()

    def unit(self, name, text, *defines, debug="-DVJO_DEBUG=1"):
        (self.dir / name).write_text(text)
        args = [CC, *([debug] if debug else []), *defines, "-c", name, "-o", name + ".o", "-MD", "-MF", name + ".d"]
        return {"directory": str(self.dir), "arguments": args, "file": str(self.dir / name), "output": name + ".o"}

    def test_a_unit_that_uses_the_variant(self):
        v = vf.unit_verdict(self.unit("a.c", '#include "build.h"\nconst char *n = NAME;\n'))
        self.assertIn("depends on VJO_DEBUG", v)

    def test_a_unit_whose_headers_only_define_it(self):
        # the definitions differ, the text compiled does not: the same program in both builds
        self.assertIsNone(vf.unit_verdict(self.unit("b.c", '#include "build.h"\nint x;\n')))

    def test_either_value_flips(self):
        self.assertIn("VJO_DEBUG", vf.unit_verdict(self.unit("c.c", "int x = VJO_DEBUG;\n", debug="-DVJO_DEBUG=0")))

    def test_the_model_directories(self):
        used = self.unit("d.c", "const char *d = VJO_VOCR_MODEL_DIR;\n", '-DVJO_VOCR_MODEL_DIR=""')
        self.assertIn("depends on VJO_VOCR_MODEL_DIR", vf.unit_verdict(used))
        unused = self.unit("e.c", "int y;\n", '-DVJO_VOCR_GATE_TEST_DIR="/x"')
        self.assertIsNone(vf.unit_verdict(unused))

    def test_a_compile_line_without_the_variant(self):
        self.assertIn("not one -D", vf.unit_verdict(self.unit("f.c", "int z;\n", debug=None)))


def fake_ctest(root):
    """a ctest whose --show-only=json-v1 prints the tests.json of the directory it runs in"""
    ctest = root / "ctest"
    ctest.write_text(f"#!{sys.executable}\nimport sys\nassert sys.argv[1:] == ['--show-only=json-v1']\n"
                     "print(open('tests.json').read())\n")
    ctest.chmod(ctest.stat().st_mode | stat.S_IXUSR)
    return ctest


@unittest.skipUnless(CC, "no C compiler")
class Derivation(unittest.TestCase):
    """Three made-up configurations of one source: the lane's (debug) and the other two lanes'. Programs: free (its
    unit and a library's, alike everywhere), uses_debug_lib (a library that reads VJO_DEBUG), model (a unit that
    reads the model directory), driven (run by a host/tests driver), and one each that the normal configuration
    defines, links or compiles otherwise."""

    SOURCES = {"free": "int a;\n", "libfree": "int b;\n", "libdebug": "int c = VJO_DEBUG;\n",
               "model": "const char *m = VJO_VOCR_MODEL_DIR;\n", "driven": "int d;\n", "envdiff": "int e;\n",
               "linkdiff": "int f;\n", "linediff": "int g;\n", "uses_debug_lib": "int h;\n", "gone": "int i;\n"}
    DEPS = {"free": ["libfree"], "uses_debug_lib": ["libdebug"]}

    def config(self, root, src, name, debug, py):
        build = root / name
        build.mkdir()
        units, targets, ninja = [], {}, []
        for prog, text in self.SOURCES.items():
            defines = ['-DVJO_VOCR_MODEL_DIR=""'] if prog == "model" else []
            if prog == "linediff" and not debug:
                defines.append("-DEXTRA=1")  # the normal configuration compiles it with more
            units.append({"directory": str(build), "file": str(src / f"{prog}.c"),
                          "output": f"CMakeFiles/{prog}.dir/{prog}.c.o",
                          "arguments": [CC, f"-DVJO_DEBUG={1 if debug else 0}", *defines, "-c", str(src / f"{prog}.c"),
                                        "-o", f"CMakeFiles/{prog}.dir/{prog}.c.o"]})
            lib = prog.startswith("lib")
            targets[prog] = {"type": "STATIC_LIBRARY" if lib else "EXECUTABLE", "file": "" if lib else str(build / prog),
                             "deps": self.DEPS.get(prog, [])}
            extra = " -lm" if prog == "linkdiff" and not debug else ""
            rule = "C_STATIC_LIBRARY_LINKER" if lib else "C_EXECUTABLE_LINKER"
            ninja += [f"build {'lib' + prog + '.a' if lib else prog}: {rule}__{prog}_Release CMakeFiles/{prog}.dir/{prog}.c.o",
                      f"  LINK_LIBRARIES = {extra}"]
        tests = [{"name": "t_free", "command": [str(build / "free")], "properties": [{"name": "LABELS", "value": ["x"]}]},
                 {"name": "t_debug", "command": [str(build / "uses_debug_lib")]},
                 {"name": "t_model", "command": [str(build / "model")]},
                 {"name": "t_driven", "command": [py, str(src / "host" / "tests" / "drive.py"), str(build / "driven")],
                  "properties": [{"name": "WORKING_DIRECTORY", "value": str(build)}]},
                 {"name": "t_env", "command": [str(build / "envdiff")],
                  "properties": [{"name": "ENVIRONMENT", "value": [f"X={1 if debug else 0}"]}]},
                 {"name": "t_link", "command": [str(build / "linkdiff")]},
                 {"name": "t_line", "command": [str(build / "linediff")]},
                 {"name": "t_tool", "command": [py, str(src / "tools.py"), str(build / "free")]},
                 {"name": "t_unresolved"}]
        if debug:
            tests.append({"name": "t_gone", "command": [str(build / "gone")]})  # (the normal configuration has none)
        (build / "compile_commands.json").write_text(json.dumps(units))
        (build / "vjo_targets.json").write_text(json.dumps(targets))
        (build / "build.ninja").write_text("\n".join(ninja) + "\n")
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        (build / "CTestTestfile.cmake").write_text(f'add_test(t_unresolved "{build / "free"}")\n')
        (build / "tests.json").write_text(json.dumps({"tests": tests}))
        return build

    def test_labels(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            (src / "host" / "tests").mkdir(parents=True)
            (src / "host" / "tests" / "drive.py").write_text("")
            for prog, text in self.SOURCES.items():
                (src / f"{prog}.c").write_text(text)
            py = sys.executable
            lane = self.config(root, src, "debug", True, py)
            normal = self.config(root, src, "host-normal", False, py)
            models = self.config(root, src, "models", True, py)
            out = lane / "labels.cmake"
            rc = vf.main([str(lane), str(src), str(fake_ctest(root)), str(out), "--jobs", "2",
                          "--probe", str(normal), str(models)])
            self.assertEqual(rc, 0)
            text = out.read_text()
            report = Path(str(out) + ".txt").read_text()
            self.assertIn('set_tests_properties("t_free" PROPERTIES LABELS "x;variant_free")', text, report)
            self.assertIn('set_tests_properties("t_driven" PROPERTIES LABELS "variant_free")', text, report)
            self.assertIn('set_tests_properties("t_unresolved" PROPERTIES LABELS "variant_free")', text, report)
            for name in ("t_debug", "t_model", "t_env", "t_link", "t_line", "t_tool", "t_gone"):
                self.assertNotIn(f'"{name}"', text)
            self.assertIn("t_debug: lane-dependent: libdebug:", report)
            self.assertIn("t_model: lane-dependent: model:", report)
            self.assertIn("t_env: lane-dependent: the host-normal configuration defines the test otherwise", report)
            self.assertIn("t_link: lane-dependent: linkdiff: the host-normal configuration links it otherwise", report)
            self.assertIn("t_line: lane-dependent: linediff: the host-normal configuration compiles", report)
            self.assertIn("t_gone: lane-dependent: the host-normal configuration defines the test otherwise", report)
            self.assertIn("t_tool: not derived", report)

    def test_no_compile_lines_label_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "labels.cmake"
            self.assertEqual(vf.main([tmp, tmp, "ctest", str(out), "--probe"]), 1)
            self.assertNotIn("set_tests_properties", out.read_text())


TOY = r"""cmake_minimum_required(VERSION 3.16)
project(toy C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(VJO_DEBUG "" ON)
set(VJO_VOCR_MODEL_DIR "" CACHE PATH "")
if(VJO_DEBUG)
  add_compile_definitions(VJO_DEBUG=1)
  set(_toy_env "LEVEL=debug")
else()
  add_compile_definitions(VJO_DEBUG=0)
  set(_toy_env "LEVEL=normal")
endif()
enable_testing()
add_executable(free free.c)
add_test(NAME t_free COMMAND free)
add_executable(cfg cfg.c)
if(VJO_DEBUG)
  target_compile_definitions(cfg PRIVATE EXTRA=1)
endif()
add_test(NAME t_cfg COMMAND cfg)
add_executable(envy envy.c)
add_test(NAME t_env COMMAND envy)
set_tests_properties(t_env PROPERTIES ENVIRONMENT "${_toy_env}")
add_executable(model model.c)
add_test(NAME t_model COMMAND model "${VJO_VOCR_MODEL_DIR}")
set(_toy_json)
foreach(_t free cfg envy model)
  list(APPEND _toy_json "\"${_t}\": {\"type\": \"EXECUTABLE\", \"file\": \"$<TARGET_FILE:${_t}>\", \"deps\": []}")
endforeach()
list(JOIN _toy_json ", " _toy_json)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/vjo_targets.json CONTENT "{${_toy_json}}")
"""


@unittest.skipUnless(CC and CMAKE and CTEST, "no C compiler or CMake")
class RealConfigurations(unittest.TestCase):
    """A CMake project the script configures as the other two lanes itself: a test the variant configures in an if(),
    one whose environment comes from a variable set in an if() on VJO_DEBUG and used outside it, and one given the
    model directory as an argument are each lane-dependent; the plain one is labelled."""

    def test_labels(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, build = Path(tmp) / "src", Path(tmp) / "build"
            src.mkdir()
            (src / "CMakeLists.txt").write_text(TOY)
            for name in ("free", "cfg", "envy", "model"):
                (src / f"{name}.c").write_text("int main(void) { return 0; }\n")
            subprocess.run([CMAKE, "-S", str(src), "-B", str(build)], check=True, capture_output=True)
            out = build / "labels.cmake"
            self.assertEqual(vf.main([str(build), str(src), CTEST, str(out), "--cmake", CMAKE, "--jobs", "2"]), 0)
            text, report = out.read_text(), Path(str(out) + ".txt").read_text()
            self.assertIn('set_tests_properties("t_free" PROPERTIES LABELS "variant_free")', text, report)
            for name in ("t_cfg", "t_env", "t_model"):
                self.assertIn(f"{name}: lane-dependent", report)
            self.assertTrue((build / "variant_free" / "host-normal" / "CMakeCache.txt").exists())
            self.assertTrue((build / "variant_free" / "models" / "CMakeCache.txt").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
