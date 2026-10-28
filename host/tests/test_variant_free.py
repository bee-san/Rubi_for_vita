"""host/tests/variant_free.py's rules on small inputs: CMake's if() blocks on the variant or the models, each unit's
preprocessed text with VJO_DEBUG flipped and the model directories changed, and a whole derivation over a made-up
build (its compile lines, targets and tests), with the labels it writes.

    python3 host/tests/test_variant_free.py
"""
import importlib.util
import json
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("variant_free", HERE / "variant_free.py")
vf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vf)
CC = shutil.which("cc") or shutil.which("gcc")


class CMakeConditions(unittest.TestCase):
    def configured(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "CMakeLists.txt"
            path.write_text(text)
            return vf.configured_by_conditions([path])

    def test_commands_are_parsed_whole(self):
        cmds = vf.cmake_commands('foo(a "b ) # c" $<IF:$<BOOL:x>,y,z>) # bar(no)\nbaz(\n  d\n  (e)\n)\n')
        self.assertEqual(cmds, [("foo", ["a", '"b ) # c"', "$<IF:$<BOOL:x>,y,z>"]), ("baz", ["d", "e"])])

    def test_what_the_variant_and_the_models_configure(self):
        names, everything = self.configured("""
if(VJO_DEBUG)
  add_compile_definitions(VJO_DEBUG=1)
else()
  add_compile_definitions(VJO_DEBUG=0)
endif()
add_executable(both both.c)
if(VJO_DEBUG)
  add_executable(debug-only x.c core/both.c) # (both named as a path: not configured)
  target_link_libraries(debug-only both)
  add_test(NAME debug_test COMMAND debug-only)
  if(FOO)
    target_compile_definitions(nested PRIVATE X=1)
  endif()
endif()
if(SOMETHING_ELSE)
  add_executable(other y.c)
elseif(VJO_VOCR_MODEL_DIR)
  add_test(model_test z)
  set_tests_properties(t1 t2 PROPERTIES LABELS x)
  set_property(TEST t3 APPEND PROPERTY LABELS y)
  set_target_properties(e1 e2 PROPERTIES OUTPUT_NAME q)
endif()
""")
        self.assertEqual(names, {"debug-only", "debug_test", "nested", "model_test", "t1", "t2", "t3", "e1", "e2"})
        self.assertFalse(everything)

    def test_a_directory_wide_setting_by_variant_is_everything(self):
        _, everything = self.configured("if(NOT VJO_DEBUG)\n  add_compile_options(-O0)\nendif()\n")
        self.assertTrue(everything)


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


@unittest.skipUnless(CC, "no C compiler")
class Derivation(unittest.TestCase):
    """A build of five programs: free (its own unit and a library's, neither reading the variant), debug (a library
    that reads it), model (a unit that reads the model directory), driven (run by a host/tests driver), and odd
    (configured inside an if() on VJO_DEBUG)."""

    def test_labels(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src, build = root / "src", root / "build"
            (src / "host" / "tests").mkdir(parents=True)
            (src / "cmake").mkdir()
            build.mkdir()
            (src / "CMakeLists.txt").write_text("if(VJO_DEBUG)\n  target_compile_definitions(odd PRIVATE X)\nendif()\n")
            (src / "host" / "tests" / "drive.py").write_text("")
            units, targets = [], {}
            sources = {"free": "int a;\n", "libfree": "int b;\n", "libdebug": "int c = VJO_DEBUG;\n",
                       "model": "const char *m = VJO_VOCR_MODEL_DIR;\n", "driven": "int d;\n", "odd": "int e;\n",
                       "uses_debug_lib": "int f;\n"}
            for name, text in sources.items():
                (src / f"{name}.c").write_text(text)
                defines = ['-DVJO_VOCR_MODEL_DIR=""'] if name == "model" else []
                units.append({"directory": str(build), "file": str(src / f"{name}.c"),
                              "output": f"CMakeFiles/{name}.dir/{name}.c.o",
                              "arguments": [CC, "-DVJO_DEBUG=1", *defines, "-c", str(src / f"{name}.c"), "-o",
                                            f"CMakeFiles/{name}.dir/{name}.c.o"]})
            programs = {"free": ["libfree"], "model": [], "driven": [], "odd": [], "uses_debug_lib": ["libdebug"],
                        "unbuilt": []}
            for name, deps in programs.items():
                targets[name] = {"type": "EXECUTABLE", "file": str(build / name), "deps": deps}
            for lib in ("libfree", "libdebug"):
                targets[lib] = {"type": "STATIC_LIBRARY", "file": "", "deps": []}
            (build / "compile_commands.json").write_text(json.dumps(units))
            (build / "vjo_targets.json").write_text(json.dumps(targets))
            py = sys.executable
            tests = [
                {"name": "t_free", "command": [str(build / "free")], "properties": [{"name": "LABELS", "value": ["x"]}]},
                {"name": "t_debug", "command": [str(build / "uses_debug_lib")]},
                {"name": "t_model", "command": [str(build / "model")]},
                {"name": "t_driven", "command": [py, str(src / "host" / "tests" / "drive.py"), str(build / "driven")]},
                {"name": "t_env", "command": [py, str(src / "host" / "tests" / "drive.py")],
                 "properties": [{"name": "ENVIRONMENT", "value": [f"PROG={build / 'free'}"]}]},
                {"name": "t_nothing", "command": [py, str(src / "host" / "tests" / "drive.py")]},
                {"name": "t_tool", "command": [py, str(src / "tools.py"), str(build / "free")]},
                {"name": "t_odd", "command": [str(build / "odd")]},
                {"name": "t_unbuilt", "command": [str(build / "unbuilt")]},
                {"name": "t_unresolved"},  # (ctest gives no command for a program not built: CTestTestfile.cmake's)
            ]
            (build / "CTestTestfile.cmake").write_text(f'add_test(t_unresolved "{build / "free"}")\n'
                                                     'set_tests_properties(t_unresolved PROPERTIES  LABELS "y")\n')
            ctest = root / "ctest"
            ctest.write_text(f"#!{py}\nimport sys\nassert sys.argv[1:] == ['--show-only=json-v1']\n"
                             f"print({json.dumps(json.dumps({'tests': tests}))})\n")
            ctest.chmod(ctest.stat().st_mode | stat.S_IXUSR)
            out = build / "labels.cmake"
            rc = vf.main([str(build), str(src), str(ctest), str(out), "--jobs", "2"])
            self.assertEqual(rc, 0)
            text = out.read_text()
            self.assertIn('set_tests_properties("t_free" PROPERTIES LABELS "x;variant_free")', text)
            self.assertIn('set_tests_properties("t_driven" PROPERTIES LABELS "variant_free")', text)
            self.assertIn('set_tests_properties("t_env" PROPERTIES LABELS "variant_free")', text)
            self.assertIn('set_tests_properties("t_unresolved" PROPERTIES LABELS "variant_free")', text)
            for name in ("t_debug", "t_model", "t_nothing", "t_tool", "t_odd", "t_unbuilt"):
                self.assertNotIn(f'"{name}"', text)
            report = Path(str(out) + ".txt").read_text()
            self.assertIn("t_debug: lane-dependent: libdebug:", report)
            self.assertIn("t_model: lane-dependent: model:", report)
            self.assertIn("t_odd: lane-dependent: odd is configured", report)
            self.assertIn("t_unbuilt: lane-dependent: unbuilt: no compile lines", report)
            self.assertIn("t_tool: not derived", report)

    def test_no_compile_lines_label_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "labels.cmake"
            self.assertEqual(vf.main([tmp, tmp, "ctest", str(out)]), 1)
            self.assertNotIn("set_tests_properties", out.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
