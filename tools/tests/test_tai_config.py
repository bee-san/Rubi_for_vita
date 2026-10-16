"""tools/patch_tai_config.py, the PC installer's copy of the Overlay app's config.txt editor
(core/taicfg.c): the same expected bytes on tools/tests/fixtures/taicfg/, its command line, and,
when ctest gives it the C build (VJO_TAICFG: host/taicfg_tool.c), the two editors and the two copies
of taiHEN's parser compared on every fixture and on a generated corpus."""
import os
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import patch_tai_config as p  # noqa: E402
from patch_tai_config import (APP_NOTE, DISABLED, KERNEL, NOTE, OCR, SHELL, TEXT, Refused, TitleError,  # noqa: E402
                              edit, patch, registered, taihen_parse, taihen_validate)

SCRIPT = Path(__file__).resolve().parents[1] / "patch_tai_config.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "taicfg"
TAICFG = os.environ.get("VJO_TAICFG")
STOCK = (FIXTURES / "henkaku-ux0.in").read_bytes()


def fixtures():
    for path in sorted(FIXTURES.glob("*.in")):
        name = path.name[:-3]
        titles_file = FIXTURES / (name + ".titles")
        titles = titles_file.read_text().split() if titles_file.is_file() else []
        yield name, path.read_bytes(), titles


def active(result):
    """(section, plugin) for every plugin taiHEN reads in a section (any title), in file order."""
    pairs, current = [], None
    for line in p.lines_of(result):
        if line.kind == p.L_SECTION:
            current = p._name(result, line).decode("latin-1")
        elif line.kind == p.L_PATH:
            pairs.append((current, p._content(result, line).decode("latin-1")))
    return pairs


def loads(text, title, kind):
    return sum(1 for path in taihen_parse(text, title.encode()) if p.ours(path) == kind)


class Fixtures(unittest.TestCase):
    """The expected results host/tests/test_taicfg.c checks the C editor against."""

    def test_the_inputs_are_the_generators(self):
        r = subprocess.run([sys.executable, str(Path(__file__).with_name("make_taicfg_fixtures.py")), "--check"],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_every_fixture(self):
        count = 0
        for name, raw, titles in fixtures():
            count += 1
            refused = FIXTURES / (name + ".refused")
            with self.subTest(name=name):
                if refused.is_file():
                    rc, line = (int(field.split("=")[1]) for field in refused.read_text().split())
                    for op in (p.INSTALL, p.UNINSTALL):
                        for mode in (p.WORKER_KERNEL, p.WORKER_LINES):
                            if rc == p.E_CHECK and op == p.UNINSTALL:
                                continue
                            with self.assertRaises(Refused) as caught:
                                edit(raw, op, mode)
                            self.assertEqual((caught.exception.rc, caught.exception.line), (rc, line))
                            self.assertTrue(caught.exception.why)
                    continue
                for op, mode, ts, suffix in ((p.INSTALL, p.WORKER_KERNEL, [], "kernel"),
                                             (p.INSTALL, p.WORKER_LINES, titles, "lines"),
                                             (p.UNINSTALL, p.WORKER_KERNEL, [], "uninstall")):
                    out, report = edit(raw, op, mode, ts)
                    self.assertEqual(out, (FIXTURES / f"{name}.{suffix}.out").read_bytes(), suffix)
                    self.assertEqual(report["changed"], out != raw)
                    again, report = edit(out, op, mode, ts)
                    self.assertEqual(again, out, suffix + ": idempotent")
                    self.assertFalse(report["changed"])
                    if out != raw:
                        self.assertNotEqual(len(out) % 4096, 0, suffix)
        self.assertGreaterEqual(count, 38)


class Rules(unittest.TestCase):
    def test_stock_install_and_uninstall(self):
        out, report = edit(STOCK)
        self.assertIn(b"*KERNEL\n" + KERNEL.encode() + b"\n# henkaku.skprx", out)
        self.assertIn(b"*main\n" + SHELL.encode() + b"\n# main is", out)
        self.assertEqual((report["added"], report["removed"], report["commented"]), (2, 0, 0))
        self.assertEqual((loads(out, "KERNEL", p.O_KERNEL), loads(out, "main", p.O_SHELL)), (1, 1))
        self.assertEqual(loads(out, "PCSG00415", p.O_WORKER), 0)
        self.assertEqual(edit(out, p.UNINSTALL)[0], STOCK)

    def test_the_kernel_loads_the_worker_so_old_lines_go(self):
        """v0.7.12's *ALL line and test.3's --title sections (install-vita.sh's blank line, header, worker)."""
        raw = ("*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n*ALL\nur0:tai/Every.suprx\n" + OCR +
               "\n\n*PCSG00415\n" + OCR + "\n\n*PSPEMUCFW\n" + OCR + "\n").encode()
        out, report = edit(raw)
        self.assertEqual(out, ("*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n*ALL\nur0:tai/Every.suprx\n").encode())
        self.assertEqual(report["removed_sections"], ["*ALL", "*PCSG00415", "*PSPEMUCFW"])
        self.assertEqual((report["removed"], report["headers_removed"]), (3, 2))
        # the same file in lines mode: the games are kept, the *ALL line commented out (the rules before)
        lines, _ = edit(raw, p.INSTALL, p.WORKER_LINES)
        self.assertIn(("*ALL\nur0:tai/Every.suprx\n" + DISABLED + OCR + "\n").encode(), lines)
        self.assertEqual(registered(lines), ["PCSG00415", "PSPEMUCFW"])

    def test_a_header_with_anything_else_stays(self):
        raw = STOCK + ("*PCSG00415\n# mine\n" + OCR + "\n*!PCSE00120\n" + OCR + "\n*ULJS00378\n" + OCR +
                       "\nux0:tai/other.suprx\n").encode()
        out, _ = edit(raw)
        self.assertIn(b"*PCSG00415\n# mine\n", out)    # a comment of the user's
        self.assertIn(b"*!PCSE00120\n", out)          # a halting header decides where taiHEN stops
        self.assertIn(b"*ULJS00378\nux0:tai/other.suprx\n", out)
        self.assertNotIn(OCR.encode(), out)

    def test_bytes_python_str_splits_are_one_line_to_taihen(self):
        """The audit's case: "# note\\x0c*ALL" is one comment line to taiHEN; str.splitlines() made it
        a live *ALL header in the patcher before."""
        raw = b"# note\x0c*ALL\n*KERNEL\nur0:tai/a.skprx\n*main\nur0:tai/henkaku.suprx\n"
        out, _ = edit(raw)
        self.assertIn(b"# note\x0c*ALL\n", out)
        self.assertEqual(taihen_parse(out, b"PCSG00415"), [])
        for sep in (b"\x0b", b"\x1c", b"\xc2\x85", "\u2028".encode()):
            text = b"# x" + sep + b"*ALL\n" + STOCK
            self.assertEqual(len(list(p.lines_of(text))), len(list(p.lines_of(STOCK))) + 1)

    def test_line_ends_are_kept_per_line(self):
        raw = b"*KERNEL\r\nur0:tai/a.skprx\n*main\rur0:tai/henkaku.suprx"
        out, _ = edit(raw)
        self.assertEqual(out, b"*KERNEL\r\n" + KERNEL.encode() + b"\r\nur0:tai/a.skprx\n*main\r" + SHELL.encode() +
                         b"\r\nur0:tai/henkaku.suprx")
        self.assertEqual(edit(b"*KERNEL\r*main\r")[0], b"*KERNEL\r" + KERNEL.encode() + b"\r*main\r" + SHELL.encode() + b"\r")

    def test_refusals(self):
        cases = {b"": p.E_EMPTY, b"# only\n\n": p.E_NO_SECTION, b"*KERNEL\n\0": p.E_NUL,
                 b"*KERNEL\n" + b"#" * 256 + b"\n": p.E_LONG_LINE, b"#" * (64 * 1024 + 1): p.E_TOO_LARGE,
                 b"*KERNEL\nur0:tai/a.skprx\n*!ALL\nx\n*NPXS10015\ny\n": p.E_CHECK}
        for raw, rc in cases.items():
            with self.subTest(raw=raw[:30]), self.assertRaises(Refused) as caught:
                edit(raw)
            self.assertEqual(caught.exception.rc, rc)
        for title in ("ALL", "*ALL", "main", "KERNEL", "NPXS10015", "VITASHELL", "VJOSET001", "PCSG0041", "PCSG004150",
                      "PCSG 0415", ""):
            with self.subTest(title=title), self.assertRaises(TitleError):
                edit(STOCK, p.INSTALL, p.WORKER_LINES, [title])
        self.assertIn(b"*PSPEMUCFW\n" + OCR.encode(), edit(STOCK, p.INSTALL, p.WORKER_LINES, ["pspemucfw"])[0])

    def test_patch_keeps_the_pc_installers_interface(self):
        self.assertNotIn(OCR.encode(), patch(STOCK, titles=["PCSG00415"]))  # kernel mode ignores --title
        with self.assertRaises(TitleError):
            patch(STOCK, mode=p.WORKER_LINES)  # lines mode needs a game
        ur0 = ("*KERNEL\n" + KERNEL + "\n*PCSG00415\n" + OCR + "\n").encode()
        out = patch(STOCK, mode=p.WORKER_LINES, keep_from=[ur0])
        self.assertEqual(registered(out), ["PCSG00415"])
        extra = patch(STOCK, extra_kernel=["ur0:tai/NoPowerLimits.skprx"])
        self.assertIn(b"*KERNEL\nur0:tai/NoPowerLimits.skprx\n" + KERNEL.encode() + b"\n", extra)
        self.assertEqual(patch(extra, extra_kernel=["ur0:tai/NoPowerLimits.skprx"]), extra)
        self.assertNotIn(KERNEL.encode(), patch(extra, uninstall=True))
        self.assertIn(b"NoPowerLimits", patch(extra, uninstall=True))  # another plugin stays

    def test_notes(self):
        added = edit(b"*NPXS10015\nur0:tai/henkaku.suprx\n")[0]
        self.assertTrue(added.endswith(("\n" + NOTE + "\n*KERNEL\n" + KERNEL + "\n" + NOTE + "\n*main\n" + SHELL +
                                        "\n").encode()))
        self.assertEqual(edit(added, p.UNINSTALL)[0], b"*NPXS10015\nur0:tai/henkaku.suprx\n")
        bubble = STOCK + (APP_NOTE + "\n*ULJS00378\n" + OCR + "\n").encode()
        self.assertEqual(edit(bubble, p.UNINSTALL)[0], STOCK)


class CommandLine(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True,
                              timeout=60)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / "ur0.txt").write_bytes(("*ALL\r\n" + OCR + "\r\n").encode())
            (folder / "ux0.txt").write_bytes(("*PCSG00415\r\n" + OCR + "\r\n").encode())
            missing = self.run_cli(folder / "ur0.txt", folder / "out")
            self.assertEqual(missing.returncode, 2)  # --worker is required
            done = self.run_cli(folder / "ur0.txt", folder / "out", "--worker", "kernel", "--title", "PCSG00415")
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn("removed the OCR worker's lines from *ALL", done.stdout)
            self.assertNotIn(OCR.encode(), (folder / "out").read_bytes())
            refused = self.run_cli(folder / "ur0.txt", folder / "out2", "--worker", "lines")
            self.assertEqual(refused.returncode, 3, refused.stderr)
            self.assertIn("--title PCSG00415", refused.stderr)
            self.assertFalse((folder / "out2").exists())
            lines = self.run_cli(folder / "ur0.txt", folder / "out3", "--worker", "lines", "--title", "NPXS10028",
                                 "--keep-titles-from", folder / "ux0.txt")
            self.assertEqual(lines.returncode, 0, lines.stderr)
            self.assertIn("OCR worker registered for NPXS10028, PCSG00415", lines.stdout)
            (folder / "broken.txt").write_bytes(b"# only comments\n")
            broken = self.run_cli(folder / "broken.txt", folder / "out4", "--worker", "kernel")
            self.assertEqual(broken.returncode, 4)
            self.assertIn("no section", broken.stderr)
            self.assertFalse((folder / "out4").exists())
            same = self.run_cli(folder / "out", folder / "out5", "--worker", "kernel")
            self.assertEqual(same.returncode, 0)
            self.assertIn("unchanged", same.stdout)


def corpus(seed, count):
    pieces = [b"*KERNEL", b"*main", b"*ALL", b"*!ALL", b"*PCSG00415", b"*!PCSG00415", b"*PSPEMUCFW", b"*NPXS10015",
              b"*kernel", b"* main", b"*  !  KERNEL", b"*", b"*!", b"# a comment", b"#", b"", b"  ", b"\t",
              KERNEL.encode(), SHELL.encode(), OCR.encode(), TEXT.encode(), b"ux0:tai/VitaJPOverlay_Kernel.skprx",
              b"UR0:TAI/VITAJPOVERLAY_SHELL.SUPRX", b"ur0:/tai/VitaJPOverlay_OCR.suprx", b"ur0:tai/henkaku.suprx",
              b"ur0:tai/other.skprx", NOTE.encode(), APP_NOTE.encode(), (DISABLED + OCR).encode(),
              b"\xef\xbb\xbf*KERNEL", b"\xef\xbb\xbf# bom", "\u3000".encode() + OCR.encode(), OCR.encode() + b"\xc2\xa0",
              b"# x\x0c*ALL", b"# y\xc2\x85*main", "# z\u2028*ALL".encode(), b"# s\x1c*ALL", b"ur0:tai/a.skprx\x0b",
              "日本".encode(), b"caf\xe9", b"*ULJS00378", b"*!main"]
    ends = [b"\n", b"\r\n", b"\r", b"\n", b"\x85\n"]
    rng = random.Random(seed)
    for _ in range(count):
        out = []
        for i in range(rng.randint(1, 24)):
            lead = b"  " if rng.random() < 0.12 else b""
            trail = b" \t" if rng.random() < 0.12 else b""
            end = rng.choice(ends) if (i < 23 or rng.random() < 0.7) else b""
            out.append(lead + rng.choice(pieces) + trail + end)
        yield b"".join(out)


@unittest.skipUnless(TAICFG, "VJO_TAICFG (the C build's vjo-taicfg, set by ctest) is not set")
class AgainstTheCBuild(unittest.TestCase):
    """The PC installer's editor and parser against the app's (core/taicfg.c, third_party/taihen-parser)."""

    def c_edit(self, path, op, mode, titles=()):
        r = subprocess.run([TAICFG, "edit", str(path), op, mode, *titles], capture_output=True, timeout=60)
        return r.returncode, r.stdout

    def py_edit(self, raw, op, mode, titles=()):
        try:
            return 0, edit(raw, op, mode, titles)[0]
        except Refused as error:
            return 1, ("REFUSED rc=%d line=%d: %s\n" % (error.rc, error.line, error.why)).encode()
        except TitleError:
            return 1, b"TITLE"

    def compare(self, raw, path, label):
        for op in (p.INSTALL, p.UNINSTALL):
            for mode in (p.WORKER_KERNEL, p.WORKER_LINES):
                titles = ["PCSG00415", "ULJS00378"] if mode == p.WORKER_LINES else []
                self.assertEqual(self.c_edit(path, op, mode, titles), self.py_edit(raw, op, mode, titles),
                                 f"{label}: {op} {mode}")

    def test_fixtures(self):
        for name, raw, _ in fixtures():
            with self.subTest(name=name):
                self.compare(raw, FIXTURES / (name + ".in"), name)

    def test_corpus(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config.txt"
            for i, raw in enumerate(corpus(20261016, 400)):
                path.write_bytes(raw)
                self.compare(raw, path, f"case {i}: {raw!r}")
                for title in (b"KERNEL", b"main", b"ALL", b"PCSG00415", b"NPXS10015", b"*", b""):
                    if not title:
                        continue
                    r = subprocess.run([TAICFG, "parse", str(path), title.decode()], capture_output=True, timeout=60)
                    want = b"".join(module + b"\n" for module in taihen_parse(raw, title))
                    self.assertEqual(r.stdout, want, f"case {i} {title}: {raw!r}")
                r = subprocess.run([TAICFG, "validate", str(path)], capture_output=True, timeout=60)
                self.assertEqual(int(r.stdout), taihen_validate(raw), f"case {i}: {raw!r}")


if __name__ == "__main__":
    unittest.main()
