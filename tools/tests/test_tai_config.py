import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from patch_tai_config import DISABLED, KERNEL, OCR, SHELL, TEXT, TitleError, patch, registered

SCRIPT = Path(__file__).resolve().parents[1] / "patch_tai_config.py"


def active(result):
    """(section, plugin) for every active plugin line, in file order."""
    pairs, current = [], None
    for line in result.decode().splitlines():
        item = line.strip()
        if not item or item.startswith("#"):
            continue
        if item.startswith("*"):
            current = item[1:].lstrip().lstrip("!").lstrip()
        else:
            pairs.append((current, item))
    return pairs


def worker_sections(result):
    return [section for section, plugin in active(result) if plugin == OCR]


class TaiConfigTests(unittest.TestCase):
    def test_patch_output(self):
        # The exact bytes for an *ALL install of the previous installer.
        raw = ("*KERNEL\r\nur0:tai/Other.skprx\r\n*main\r\n*ALL\r\nur0:tai/Every.suprx\r\n"
               + OCR + "\r\n").encode()
        expected = ("*KERNEL\r\n" + KERNEL + "\r\nur0:tai/Other.skprx\r\n*main\r\n" + SHELL + "\r\n"
                    "*ALL\r\nur0:tai/Every.suprx\r\n" + DISABLED + OCR + "\r\n"
                    "\r\n*PCSG00415\r\n" + OCR + "\r\n").encode()
        self.assertEqual(patch(raw, titles=["PCSG00415"]), expected)

    def test_ocr_only_and_idempotence(self):
        raw = ("# existing\r\n*KERNEL\r\nur0:tai/Other.skprx\r\n*PCSG00415\r\n"
               + TEXT + "\r\n" + OCR + "\r\nur0:tai/Game.suprx\r\n").encode()
        result = patch(raw, ["ur0:tai/Extra.skprx"], titles=["NPXS10028"])
        self.assertEqual(result, patch(result, ["ur0:tai/Extra.skprx"], titles=["NPXS10028"]))
        self.assertEqual(result, patch(result, ["ur0:tai/Extra.skprx"]))
        pairs = active(result)
        self.assertNotIn(TEXT, [plugin for _, plugin in pairs])
        # The registered game is kept, the named one added; one worker line each.
        self.assertEqual(worker_sections(result), ["PCSG00415", "NPXS10028"])
        self.assertIn(("*PCSG00415\r\n" + DISABLED + TEXT + "\r\n" + OCR).encode(), result)
        self.assertIn(("PCSG00415", "ur0:tai/Game.suprx"), pairs)
        self.assertIn(("KERNEL", "ur0:tai/Other.skprx"), pairs)
        self.assertNotIn(b"\n", result.replace(b"\r\n", b""))
        self.assertEqual(pairs.count(("KERNEL", KERNEL)), 1)
        self.assertEqual(pairs.count(("KERNEL", "ur0:tai/Extra.skprx")), 1)
        self.assertEqual(pairs.count(("main", SHELL)), 1)

    def test_all_worker_is_migrated_to_the_titles(self):
        raw = ("*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n*ALL\nur0:tai/Every.suprx\n"
               + OCR + "\n").encode()
        result = patch(raw, titles=["PCSG00415", "pcsg00415", "*NPXS10028"])
        self.assertIn(("*ALL\nur0:tai/Every.suprx\n" + DISABLED + OCR + "\n").encode(), result)
        self.assertEqual(worker_sections(result), ["PCSG00415", "NPXS10028"])
        self.assertIn(("ALL", "ur0:tai/Every.suprx"), active(result))
        self.assertEqual(result.count(OCR.encode()), 3)
        # Updating again, with or without the titles, changes nothing.
        self.assertEqual(patch(result, titles=["PCSG00415"]), result)
        self.assertEqual(patch(result), result)
        self.assertEqual(registered(result.decode().splitlines()), ["PCSG00415", "NPXS10028"])

    def test_no_title_and_none_registered_is_refused(self):
        for raw in (b"", b"*KERNEL\nur0:tai/Other.skprx\n", ("*ALL\n" + OCR + "\n").encode(),
                    ("*PCSG00415\n" + DISABLED + OCR + "\n").encode(),
                    ("*main\n" + OCR + "\n*NPXS10015\n" + OCR + "\n*pcsg00415\n" + OCR + "\n").encode()):
            with self.assertRaises(TitleError) as refusal:
                patch(raw)
            self.assertIn("--title", str(refusal.exception))
            self.assertIn("*ALL", str(refusal.exception))

    def test_titles_the_worker_cannot_serve_are_refused(self):
        for title in ("ALL", "*ALL", "main", "KERNEL", "NPXS10015", "VITASHELL", "VJOSET001",
                      "PCSG0041", "PCSG004150", "PCSG 0415", "PCSG0041!", ""):
            with self.subTest(title=title), self.assertRaises(TitleError):
                patch(b"*KERNEL\n", titles=[title])
        with self.assertRaises(TitleError) as refusal:
            patch(b"", titles=["NPXS10015"])
        self.assertIn("system application", str(refusal.exception))
        self.assertIn("Adrenaline", str(refusal.exception))
        # Adrenaline's process title is a candidate like any other title (core/games.c).
        self.assertEqual(worker_sections(patch(b"", titles=["PSPEMUCFW"])), ["PSPEMUCFW"])

    def test_kept_titles_come_from_every_configuration(self):
        ur0 = ("*KERNEL\nur0:tai/Other.skprx\n*ALL\n" + OCR + "\n").encode()
        ux0 = ("*PCSG00415\n" + OCR + "\n*NPXS10028\nur0:tai/Psp.suprx\n" + OCR + "\n").encode()
        for raw in (ur0, ux0):
            result = patch(raw, keep_from=[ur0, ux0])
            self.assertEqual(worker_sections(result), ["PCSG00415", "NPXS10028"])
            self.assertEqual(patch(result, keep_from=[ur0, ux0]), result)

    def test_worker_goes_first_into_an_existing_game_section(self):
        raw = b"*NPXS10028\nur0:tai/Psp.suprx\n*!PCSG00415\nur0:tai/Game.suprx\n"
        result = patch(raw, titles=["NPXS10028", "PCSG00415"])
        self.assertIn(("*NPXS10028\n" + OCR + "\nur0:tai/Psp.suprx\n").encode(), result)
        self.assertIn(("*!PCSG00415\n" + OCR + "\nur0:tai/Game.suprx\n").encode(), result)
        self.assertEqual(result.count(b"*NPXS10028"), 1)
        self.assertEqual(result.count(b"PCSG00415"), 1)

    def test_worker_lines_outside_game_sections_are_disabled(self):
        raw = ("*main\n" + OCR + "\n*NPXS10015\n" + OCR + "\n*pcsg00415\n" + OCR + "\n"
               "*PCSG00415\n" + OCR + "\n*PCSG00415\n" + OCR + "\n*!PCSE00120\n" + OCR + "\n").encode()
        result = patch(raw)
        self.assertEqual(worker_sections(result), ["PCSG00415", "PCSE00120"])
        self.assertEqual(result.count((DISABLED + OCR).encode()), 4)
        self.assertEqual(patch(result), result)

    def test_existing_global_registration_is_disabled(self):
        result = patch(("*ALL\n" + TEXT + "\n").encode(), titles=["PCSG00415"])
        self.assertIn((DISABLED + TEXT).encode(), result)
        self.assertEqual(result.count(TEXT.encode()), 1)

    def test_repeated_sections_preserve_existing_registrations(self):
        raw = ("*KERNEL\nur0:tai/Other.skprx\n*main\nur0:tai/Other.suprx\n"
               "*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n"
               "*PCSG00415\n" + OCR + "\n").encode()
        self.assertEqual(patch(raw), raw)
        self.assertEqual(patch(raw, titles=["PCSG00415"]), raw)

    def test_uninstall_keeps_other_plugins(self):
        raw = ("*KERNEL\n" + KERNEL + "\nur0:tai/Other.skprx\n*ALL\n" + TEXT
               + "\n" + OCR + "\n*main\n" + SHELL + "\n").encode()
        result = patch(raw, uninstall=True)
        self.assertIn(b"ur0:tai/Other.skprx", result)
        for plugin in (KERNEL, SHELL, TEXT, OCR):
            self.assertNotIn(plugin.encode(), result)

    def test_uninstall_after_a_per_title_install(self):
        raw = ("*KERNEL\nur0:tai/Other.skprx\n*ALL\n" + OCR + "\n*PCSG00415\nur0:tai/Game.suprx\n").encode()
        installed = patch(raw, titles=["PCSG00415", "NPXS10028"])
        result = patch(installed, uninstall=True)
        self.assertEqual(worker_sections(result), [])
        self.assertEqual(active(result), [("KERNEL", "ur0:tai/Other.skprx"), ("PCSG00415", "ur0:tai/Game.suprx")])
        self.assertEqual(patch(result, uninstall=True), result)
        # Nothing of ours stays registered, so the next install has to name its games again.
        with self.assertRaises(TitleError):
            patch(result)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / "ur0.txt").write_bytes(("*ALL\r\n" + OCR + "\r\n").encode())
            (folder / "ux0.txt").write_bytes(("*PCSG00415\r\n" + OCR + "\r\n").encode())
            refused = subprocess.run([sys.executable, str(SCRIPT), str(folder / "ur0.txt"), str(folder / "out")],
                                     capture_output=True, text=True)
            self.assertEqual(refused.returncode, 3, refused.stderr)
            self.assertIn("--title PCSG00415", refused.stderr)
            self.assertNotIn("usage:", refused.stderr)
            self.assertFalse((folder / "out").exists())
            invalid = subprocess.run([sys.executable, str(SCRIPT), str(folder / "ur0.txt"), str(folder / "out"),
                                      "--title", "ALL"], capture_output=True, text=True)
            self.assertEqual(invalid.returncode, 3, invalid.stderr)
            self.assertFalse((folder / "out").exists())
            done = subprocess.run([sys.executable, str(SCRIPT), str(folder / "ur0.txt"), str(folder / "out"),
                                   "--title", "NPXS10028", "--keep-titles-from", str(folder / "ux0.txt")],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn("OCR worker registered for NPXS10028, PCSG00415", done.stdout)
            self.assertEqual(worker_sections((folder / "out").read_bytes()), ["NPXS10028", "PCSG00415"])


class OverlayAppSections(unittest.TestCase):
    """The Overlay app appends a commented *TITLEID section for an Adrenaline
    bubble (core/taicfg.c); the installer keeps it as a registered game."""

    def test_an_app_section_is_registered_and_kept(self):
        base = "*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n*PCSG00415\n" + OCR + "\n"
        raw = (base + "# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)\n"
               "*ULJS00378\n" + OCR + "\n").encode()
        self.assertEqual(registered(raw.decode().splitlines()), ["PCSG00415", "ULJS00378"])
        result = patch(raw)
        self.assertEqual(worker_sections(result), ["PCSG00415", "ULJS00378"])
        self.assertEqual(patch(result), result)
        self.assertEqual(worker_sections(patch(raw, titles=["pspemu005"])), ["PCSG00415", "ULJS00378", "PSPEMU005"])

    def test_uninstall_removes_the_app_sections(self):
        base = "*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n"
        note = "# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)\n"
        raw = (base + note + "*ULJS00378\n" + OCR + "\n" + note + "*PSPEMU005\n" + OCR + "\nux0:tai/other.suprx\n"
               + note + "*PSPEMU006\n" + OCR + "\n\n").encode()
        result = patch(raw, uninstall=True).decode()
        self.assertNotIn("VitaJPOverlay: the OCR worker", result)
        self.assertNotIn("*ULJS00378", result)
        self.assertNotIn("*PSPEMU006", result)
        self.assertIn("*PSPEMU005\nux0:tai/other.suprx\n", result)  # another plugin keeps its section
        self.assertEqual(worker_sections(result.encode()), [])


if __name__ == "__main__":
    unittest.main()
