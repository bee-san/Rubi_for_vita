"""The two builds (docs/release-variants.md): tools/check_variant.py tells them apart by their modules,
tools/package_variants.py packages the release's two zips, the ledger has each build's module pages,
and both builds read and write config.ini with the same code. No device, network or Vita SDK."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_memory_ledger as ledger  # noqa: E402
import check_variant as variant  # noqa: E402
import package_release as release  # noqa: E402
import package_variants  # noqa: E402
import prepare_ocr as prepare  # noqa: E402


def fake_self(*payloads):
    """A Vita SELF (fself) whose PT_LOAD segments hold payloads, uncompressed, as
    vita-make-fself lays it out: the header, the ELF header, the program headers, the
    segment info, the segments."""
    elf_off, n = 0xA0, len(payloads)
    ph_off = elf_off + 0x34
    info_off = ph_off + 32 * n
    data_off = info_off + 32 * n
    data = bytearray(b"SCE\0" + b"\0" * (data_off - 4))
    struct.pack_into("<QQ", data, 0x40, elf_off, ph_off)
    struct.pack_into("<Q", data, 0x58, info_off)
    data[elf_off:elf_off + 6] = b"\x7fELF\x01\x01"
    struct.pack_into("<HH", data, elf_off + 42, 32, n)
    at = data_off
    for i, blob in enumerate(payloads):
        struct.pack_into("<8I", data, ph_off + 32 * i, 1, 0, 0, 0, len(blob), len(blob), 5, 0x1000)
        struct.pack_into("<4Q", data, info_off + 32 * i, at, len(blob), 1, 2)
        at += len(blob)
    return bytes(data) + b"".join(payloads)


def module(part, build):
    """A module's segment bytes for one build: its markers among some code-like filler."""
    markers = variant.DEBUG_MARKERS.get(part, ()) if build == "debug" else variant.NORMAL_MARKERS.get(part, ())
    return b"\0code\0" + b"\0".join(markers) + b"\0rodata\0"


def app(build, version="0.8.0"):
    start = b"start: v" + version.encode() + (b" debug" if build == "debug" else b"") + b"; %s; heap %d B"
    frames = variant.DEBUG_MARKERS["app"] if build == "debug" else ()
    return b"\0" + start + b"\0" + b"\0".join(frames) + b"\0"


class CheckVariant(unittest.TestCase):
    def blobs(self, build, version="0.8.0"):
        return {"kernel": module("kernel", build), "shell": module("shell", build), "worker": b"\0worker\0",
                "app": app(build, version)}

    def test_each_build_is_what_it_says_and_never_the_other(self):
        for build, other in (("debug", "normal"), ("normal", "debug")):
            with self.subTest(build=build):
                self.assertEqual(variant.check(build, self.blobs(build), "0.8.0"), [])
                problems = variant.check(other, self.blobs(build), "0.8.0")
                self.assertTrue(any(p.startswith("shell:") for p in problems), problems)
                self.assertTrue(any(p.startswith("kernel:") for p in problems), problems)
                self.assertTrue(any(p.startswith("app:") for p in problems), problems)
                self.assertEqual(variant.classify(self.blobs(build))[0], build)

    def test_a_normal_build_with_one_debug_string_left_fails(self):
        blobs = self.blobs("normal")
        blobs["shell"] += b"\0gstep %s %s kind=\0"
        problems = variant.check("normal", blobs)
        self.assertEqual(len(problems), 1)
        self.assertIn("gstep %s %s kind=", problems[0])
        self.assertIsNone(variant.classify(blobs)[0])  # neither build: the packager refuses it

    def test_the_apps_version_is_its_own(self):
        self.assertEqual(variant.app_version(app("debug", "0.8.0")), ("0.8.0", "debug"))
        self.assertEqual(variant.app_version(app("normal", "1.2.3-rc.1")), ("1.2.3-rc.1", "normal"))
        self.assertEqual(variant.app_version(b"no start line"), (None, None))
        self.assertIn("app: version v0.7.12 (want v0.8.0)", variant.check("normal", self.blobs("normal", "0.7.12"), "0.8.0"))

    def test_segments_reads_a_self_and_refuses_anything_else(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.suprx"
            path.write_bytes(fake_self(b"first segment", b"second"))
            self.assertEqual(variant.segments(path), b"first segmentsecond")
            path.write_bytes(b"not a SELF")
            with self.assertRaises(ValueError):
                variant.segments(path)

    def test_every_marker_is_a_format_string_of_this_tree(self):
        """A marker renamed in the sources but not here would fail every Vita build; catch it on the host."""
        sources = {"shell": ("shell", "*.c*"), "kernel": ("kernel", "*.c"), "app": ("settings", "*.c")}
        for part, markers in list(variant.DEBUG_MARKERS.items()) + list(variant.NORMAL_MARKERS.items()):
            d, pattern = sources[part]
            text = "".join(p.read_text(encoding="utf-8") for p in sorted((ROOT / d).glob(pattern)))
            for m in markers:
                with self.subTest(part=part, marker=m):
                    self.assertIn(m.decode(), text)


class Ledger(unittest.TestCase):
    def test_each_build_has_its_module_pages_and_the_normal_one_is_no_larger(self):
        budget = ROOT / "include/vjo_ledger_budget.h"
        debug, normal = ledger.parse_budget(budget, "debug"), ledger.parse_budget(budget, "normal")
        self.assertEqual(ledger.parse_budget(budget), debug)  # the default is the debug build's
        for macro in ("VJO_BUDGET_SHELL_MODULE_PAGES", "VJO_BUDGET_KERNEL_MODULE_PAGES", "VJO_BUDGET_WORKER_MODULE_PAGES"):
            with self.subTest(macro=macro):
                self.assertLessEqual(normal[macro], debug[macro])
                self.assertEqual(normal[macro] % 4096, 0)
        # everything else is the same budget: only the module pages and the sums made of them differ
        differ = {k for k in debug if debug[k] != normal[k]}
        derived = {"VJO_WORST_SHELL_PAGES_PERMANENT", "VJO_WORST_SHELL_PAGES", "VJO_WORST_KERNEL_PERMANENT",
                   "VJO_WORST_KERNEL", "VJO_LIMIT_KERNEL", "VJO_BUDGET_GAME_MODULE_PAGES", "VJO_WORST_GAME_PERMANENT"}
        self.assertLessEqual(differ, {"VJO_BUDGET_SHELL_MODULE_PAGES", "VJO_BUDGET_KERNEL_MODULE_PAGES",
                                      "VJO_BUDGET_WORKER_MODULE_PAGES"} | derived)
        self.assertIn("VJO_BUDGET_SHELL_MODULE_PAGES", differ)  # the normal Shell is smaller
        self.assertEqual(set(debug), set(normal))

    def test_other_spellings_and_twice_defined_macros_are_refused(self):
        """audit N5: a conditional that names VJO_DEBUG in another form would be read both ways here and one way
        by the compiler; a macro defined twice in the lines read means the same."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "b.h"
            for text in ("#if VJO_DEBUG == 0\n#define A 1u\n#else\n#define A 2u\n#endif\n",
                         "#if (VJO_DEBUG)\n#define A 1u\n#endif\n", "#ifdef VJO_DEBUG\n#define A 1u\n#endif\n",
                         "#ifndef VJO_DEBUG\n#define A 1u\n#endif\n", "#if 1\n#define A 1u\n#elif 0\n#endif\n",
                         "#define A 1u\n#define A 2u\n",
                         "#ifdef OTHER\n#define A 1u\n#else\n#define A 2u\n#endif\n"):
                path.write_text(text)
                for v in ("debug", "normal"):
                    with self.subTest(text=text, variant=v), self.assertRaises(ledger.CheckError):
                        ledger.parse_budget(path, v)

    def test_the_normal_build_budget_is_its_pages_exactly(self):
        """audit N4: with --exact (the normal build's check) a budget above the module's pages fails too."""
        budget = ROOT / "include/vjo_ledger_budget.h"
        values = ledger.parse_budget(budget, "normal")
        with tempfile.TemporaryDirectory() as tmp:
            shell = Path(tmp) / "VitaJPOverlay_Shell.suprx"
            pages = values["VJO_BUDGET_SHELL_MODULE_PAGES"]
            # a SELF of one PT_LOAD segment of that many bytes is that many pages
            shell.write_bytes(fake_self(b"\0" * pages))
            rows, problems = ledger.check_modules(values, {"shell": shell}, exact=True)
            self.assertEqual(problems, [])
            shell.write_bytes(fake_self(b"\0" * (pages - 4096)))
            rows, problems = ledger.check_modules(values, {"shell": shell}, exact=False)
            self.assertEqual(problems, [])  # the debug build's check: a budget above the pages passes
            rows, problems = ledger.check_modules(values, {"shell": shell}, exact=True)
            self.assertEqual(len(problems), 1)
            self.assertIn(f"lower it to {pages - 4096}u", problems[0])
            self.assertIn("a budget both builds share is split into the two branches first", problems[0])

    def test_conditionals_pick_one_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "b.h"
            path.write_text("#ifndef B_H\n#define B_H\n#define A 1u\n#if VJO_DEBUG\n#define B 2u\n#else\n#define B 3u\n"
                            "#endif\n#if !VJO_DEBUG\n#define C 5u\n#endif\n#define D (A + B)\n#endif\n")
            self.assertEqual(ledger.parse_budget(path, "debug"), {"A": 1, "B": 2, "D": 3})
            self.assertEqual(ledger.parse_budget(path, "normal"), {"A": 1, "B": 3, "C": 5, "D": 4})
            with self.assertRaises(ledger.CheckError):
                ledger.parse_budget(path, "release")


class DerivedStrings(unittest.TestCase):
    """audit N3: the normal build is also checked against every literal its sources compile only into the debug
    build (check_variant.debug_only_strings), as a whole string of its own."""

    def test_the_scan_finds_debug_only_literals_and_nothing_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "shell").mkdir()
            (root / "core").mkdir()
            (root / "shell" / "a.c").write_text(
                '#include "a fairly long include name.h"\n'
                '/* "a commented out literal here" */\n'
                'void f(int x) {\n'
                '#if VJO_DEBUG\n'
                '    vjo_log("a debug-only line %d", x);\n'
                '#else\n'
                '    vjo_log("the normal line, kept");\n'
                '#endif\n'
                '#if !VJO_DEBUG\n'
                '    vjo_log("another normal-only line");\n'
                '#else\n'
                '    vjo_log("the debug branch of an if-not");\n'
                '#endif\n'
                '    vjo_dlog("a vjo_dlog line, " "two literals", x);\n'
                '    if (x) vjo_dlog ( /* comment */ "after a comment, dlog");\n'
                '    vjo_log("kept in both builds %c", \'"\');\n'
                '#if VJO_DEBUG\n'
                '    vjo_log("also used by kept code");\n'
                '#endif\n'
                '}\n')
            (root / "core" / "b.c").write_text('const char *k = "also used by kept code";\n')
            got = {s.decode() for s in variant.debug_only_strings(root, "shell")}
            self.assertEqual(got, {"a debug-only line %d", "the debug branch of an if-not",
                                   "a vjo_dlog line, two literals", "after a comment, dlog"})
            self.assertEqual(variant.debug_only_strings(root, "kernel"), [])

    def test_elif_branches_and_inc_files(self):
        """audit-b note C: the #elif and #else of an #if VJO_DEBUG may be the normal build's (not debug-only); those of
        an #if !VJO_DEBUG are reached only with VJO_DEBUG (debug-only); a literal of an .inc file is kept code's."""
        text = ("#if VJO_DEBUG\na\n#elif X\nb\n#else\nc\n#endif\n#if !VJO_DEBUG\nd\n#elif Y\ne\n#else\nf\n#endif\n"
                "#if Z\ng\n#elif W\nh\n#endif\n")
        got = {line: flag for line, flag in zip(text.split("\n"), variant._debug_lines(text)) if line and line[0] != "#"}
        self.assertEqual(got, {"a": True, "b": False, "c": False, "d": False, "e": True, "f": True, "g": False, "h": False})
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "shell").mkdir()
            (root / "core").mkdir()
            (root / "shell" / "a.c").write_text('#if VJO_DEBUG\nvjo_log("a table row as text");\n'
                                                 'vjo_log("only the debug build has this");\n#elif OTHER\n'
                                                 'vjo_log("an elif branch, maybe normal");\n#endif\n')
            (root / "core" / "t.inc").write_text('{"a table row as text", 1},\n')
            got = {x.decode() for x in variant.debug_only_strings(root, "shell")}
            self.assertEqual(got, {"only the debug build has this"})

    def test_a_leak_is_a_whole_string_not_a_merged_tail(self):
        strings = [b"a debug-only line %d"]
        self.assertEqual(variant.leaked(b"\0kept\0a debug-only line %d\0more\0", strings), strings)
        self.assertEqual(variant.leaked(b"a debug-only line %d\0", strings), strings)  # at the very start
        self.assertEqual(variant.leaked(b"\0kept: a debug-only line %d\0", strings), [])  # the tail of a kept string
        self.assertEqual(variant.leaked(b"\0a debug-only line %dX\0", strings), [])

    def test_the_tree_has_debug_only_strings_and_the_markers_are_among_them(self):
        for part, minimum in (("shell", 40), ("kernel", 8), ("worker", 1), ("app", 1)):
            with self.subTest(part=part):
                found = variant.debug_only_strings(ROOT, part)
                self.assertGreaterEqual(len(found), minimum)
                for m in variant.DEBUG_MARKERS.get(part, ()):
                    self.assertTrue(any(m in s for s in found), m)

    def test_check_with_sources_fails_a_normal_build_that_kept_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "shell").mkdir()
            (root / "shell" / "a.c").write_text('#if VJO_DEBUG\nvjo_log("a debug-only line %d", x);\n#endif\n')
            blobs = {"shell": module("shell", "normal") + b"\0a debug-only line %d\0"}
            problems = variant.check("normal", blobs, sources=root)
            self.assertTrue(any("debug-only strings" in p and "a debug-only line %d" in p for p in problems), problems)
            self.assertEqual(variant.check("normal", {"shell": module("shell", "normal")}, sources=root), [])


class ConfigIsTheSame(unittest.TestCase):
    def test_both_builds_read_and_write_config_ini_with_the_same_code(self):
        """A config.ini from either build works in the other: the parser, the Overlay app's writer, the
        installer's migration and the template have no build-specific code."""
        for rel in ("core/config.c", "core/config.h", "settings/preferences.c", "tools/migrate_config.py",
                    "docs/config.example.ini", "tools/install-vita.sh"):
            with self.subTest(file=rel):
                self.assertNotIn("VJO_DEBUG", (ROOT / rel).read_text(encoding="utf-8"))

    def test_the_debug_build_is_the_default(self):
        """Test builds are CI's default build, the debug one: both CMake projects default VJO_DEBUG to ON, and a
        file compiled without the definition is the debug build too (include/vjo_build.h)."""
        for rel in ("vita/CMakeLists.txt", "CMakeLists.txt", "tools/vita3k_smoke/CMakeLists.txt"):
            with self.subTest(file=rel):
                self.assertRegex((ROOT / rel).read_text(), r'option\(VJO_DEBUG "[^"]*" ON\)')
        self.assertRegex((ROOT / "include/vjo_build.h").read_text(), r"#ifndef VJO_DEBUG\n#define VJO_DEBUG 1\n")


class PackageVariants(unittest.TestCase):
    """The release's two zips from two synthetic base zips and fixture models (as test_package_models.py)."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.models = self.root / "models"
        self.models.mkdir()
        blob = b"tiny model fixture, not OCR weights"
        spec = ("https://fixture.invalid/m", len(blob), hashlib.sha256(blob).hexdigest())
        attribution = b"CC BY-SA 4.0 fixture"
        att = {prepare.VOCR_LM_ATTRIBUTION: ("https://fixture.invalid/A", len(attribution),
                                             hashlib.sha256(attribution).hexdigest())}
        self.enterContext(mock.patch.object(release, "VOCR_LM_ATTRIBUTION_PIN", att))
        for mod in (prepare, release):
            for table in ("VOCR_ASSETS", "VOCR3_ASSETS", "VOCR4_ASSETS", "VOCR_LM_ASSETS"):
                pinned = {name: spec for name in getattr(mod, table)}
                self.enterContext(mock.patch.object(mod, table, pinned))
        for table in ("VOCR_ASSETS", "VOCR3_ASSETS", "VOCR4_ASSETS", "VOCR_LM_ASSETS"):
            for name in getattr(prepare, table):
                (self.models / name).write_bytes(blob)
        (self.models / prepare.VOCR_LM_ATTRIBUTION).write_bytes(attribution)
        self.dictionary = b"VJDICT2\0synthetic dictionary fixture"
        snapshot = {"bytes": len(self.dictionary), "sha256": hashlib.sha256(self.dictionary).hexdigest()}
        self.enterContext(mock.patch.object(release, "load_manifest", return_value=snapshot))
        self.enterContext(mock.patch.object(release, "validate", return_value=None))

    def base(self, build, version="0.8.0", readme=b"README\n"):
        """A base zip of one build: its plugins (SELFs with that build's markers), the RCO, the VPK
        (its eboot a SELF naming the version), the frozen dictionary, the docs."""
        path = self.root / f"base-{build}-{version}-{hashlib.sha256(readme).hexdigest()[:6]}.zip"
        eboot = fake_self(app(build, version))
        stamp = f"vjoapp1 api=22 build={hashlib.sha256(eboot).hexdigest()[:16]}\n"
        vpk = io.BytesIO()
        with zipfile.ZipFile(vpk, "w") as z:
            z.writestr("eboot.bin", eboot)
            z.writestr("vjo_app.txt", stamp)
            z.writestr("sce_sys/param.sfo", b"\0PSF fixture")
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("ur0/tai/VitaJPOverlay_Kernel.skprx", fake_self(module("kernel", build)))
            z.writestr("ur0/tai/VitaJPOverlay_Shell.suprx", fake_self(module("shell", build)))
            z.writestr("ur0/tai/VitaJPOverlay_OCR.suprx", fake_self(b"\0worker " + build.encode() + b"\0"))
            z.writestr("ur0/data/VitaJPOverlay/vitajpoverlay.rco", b"RCOF fixture")
            z.writestr("JPOverlaySettings.vpk", vpk.getvalue())
            z.writestr(release.APP_RECORD, stamp)
            z.writestr("ux0/data/VitaJPOverlay/dictionaries/" + release.FILENAME, self.dictionary)
            z.writestr("README.md", readme)
            z.writestr("tools/install_ftp.sh", b"#!/bin/sh\n")
        return path

    def run_main(self, *argv):
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["package_variants.py", *map(str, argv)]), contextlib.redirect_stdout(out):
            package_variants.main()
        return out.getvalue()

    def test_two_zips_one_release(self):
        out = self.root / "out"
        printed = self.run_main("--normal", self.base("normal"), "--debug", self.base("debug"), "--models", self.models,
                                "--version", "0.8.0", "-o", out, "--source-revision", "a" * 40,
                                "--set", "anki_mining=on")
        normal, debug = out / "VitaJPOverlay-0.8.0.zip", out / "VitaJPOverlay-0.8.0-debug.zip"
        self.assertTrue(normal.is_file() and debug.is_file())
        sums = (out / "SHA256SUMS").read_text()
        self.assertEqual(sums, f"{hashlib.sha256(normal.read_bytes()).hexdigest()}  {normal.name}\n"
                               f"{hashlib.sha256(debug.read_bytes()).hexdigest()}  {debug.name}\n")
        self.assertIn(normal.name, printed)
        mn = json.loads(zipfile.ZipFile(normal).read("package-manifest.json"))
        md = json.loads(zipfile.ZipFile(debug).read("package-manifest.json"))
        self.assertEqual((mn["variant"], md["variant"]), ("normal", "debug"))
        self.assertEqual((mn["app_version"], md["app_version"]), ("0.8.0", "0.8.0"))
        for key in ("version", "defaults", "models", "dictionary", "install_settings", "source_revision"):
            self.assertEqual(mn[key], md[key], key)
        self.assertEqual(mn["install_settings"], {"anki_mining": "on"})
        # the same models, dictionary and config template; the plugins and the app differ
        a, b = package_variants.contents(normal), package_variants.contents(debug)
        self.assertEqual(set(a), set(b))
        differ = {n for n in a if a[n] != b[n]}
        self.assertEqual(differ, package_variants.BUILD_FILES | {"package-manifest.json"})
        self.assertEqual(a["config.ini"], b["config.ini"])
        self.assertIn(b"anki_mining = on", a["config.ini"])

    def test_bases_that_are_not_the_builds_they_are_given_as_are_refused(self):
        out = self.root / "out"
        for normal, debug, why in ((self.base("debug"), self.base("normal"), "the debug build, given as the normal one"),
                                   (self.base("normal"), self.base("normal"), "the normal build, given as the debug one"),
                                   (self.base("normal", "0.7.12"), self.base("debug", "0.7.12"), "the Overlay app is v0.7.12"),
                                   (self.base("normal", readme=b"other\n"), self.base("debug"), "README.md differs")):
            with self.subTest(why=why), self.assertRaises(SystemExit) as caught:
                self.run_main("--normal", normal, "--debug", debug, "--models", self.models, "--version", "0.8.0",
                              "-o", out)
            self.assertIn(why, str(caught.exception))
            self.assertFalse((out / "SHA256SUMS").exists())

    def test_package_release_records_and_checks_the_build(self):
        output = self.root / "one.zip"
        with contextlib.redirect_stdout(io.StringIO()):
            release.package(self.base("normal"), output, self.models, version="0.8.0")
        manifest = json.loads(zipfile.ZipFile(output).read("package-manifest.json"))
        self.assertEqual((manifest["variant"], manifest["app_version"]), ("normal", "0.8.0"))
        with self.assertRaises(ValueError) as caught, contextlib.redirect_stdout(io.StringIO()):
            release.package(self.base("debug"), output, self.models, version="0.8.0", variant="normal")
        self.assertIn("the debug build, not the normal one", str(caught.exception))
        # a base whose modules disagree (a normal Shell beside a debug kernel) is refused
        mixed = self.root / "mixed.zip"
        with zipfile.ZipFile(self.base("normal")) as src, zipfile.ZipFile(mixed, "w") as dst:
            for info in src.infolist():
                data = src.read(info)
                if info.filename.endswith("Kernel.skprx"):
                    data = fake_self(module("kernel", "debug"))
                dst.writestr(info, data)
        with self.assertRaises(ValueError) as caught, contextlib.redirect_stdout(io.StringIO()):
            release.package(mixed, output, self.models, version="0.8.0")
        self.assertIn("not one build", str(caught.exception))
        # a refused base leaves no output behind (audit N12)
        output.unlink(missing_ok=True)
        with self.assertRaises(ValueError), contextlib.redirect_stdout(io.StringIO()):
            release.package(self.base("debug"), output, self.models, version="0.8.0", variant="normal")
        self.assertFalse(output.exists())
        # a base that is not a zip: a refusal too (audit-b note F), writing nothing
        junk = self.root / "junk.zip"
        junk.write_bytes(b"not a zip at all")
        with self.assertRaises(SystemExit) as caught:
            self.run_main("--normal", junk, "--debug", self.base("debug"), "--models", self.models, "--version", "0.8.0",
                          "-o", self.root / "out3")
        self.assertIn("the bases are not this release's two builds", str(caught.exception))
        self.assertIn("junk.zip", str(caught.exception))
        self.assertFalse((self.root / "out3").exists())
        # and package_variants says so as a refusal, not a traceback
        with self.assertRaises(SystemExit) as caught:
            self.run_main("--normal", mixed, "--debug", self.base("debug"), "--models", self.models, "--version", "0.8.0",
                          "-o", self.root / "out2")
        self.assertIn("the bases are not this release's two builds", str(caught.exception))
        self.assertIn("not one build", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
