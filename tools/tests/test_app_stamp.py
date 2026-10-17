"""The Rubi app's version stamp (core/app_stamp.h, tools/app_stamp.py): its format, the installer's verdict
and box, and that the C, shell and Python sides name the same files (#101: test.1's app ran beside test.2's
plugins, showed VOCR2, had no Anki page and wrote ocr_backend = vocr2)."""
import hashlib
import io
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import app_stamp

EBOOT = b"SCE\0 synthetic eboot.bin, not a SELF"
API = int(re.search(r"^#define\s+VJO_API_VERSION\s+(\d+)", (ROOT / "include/vjo_api.h").read_text(),
                    re.MULTILINE).group(1))


def vpk(eboot=EBOOT, stamp=None):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("eboot.bin", eboot)
        z.writestr("sce_sys/param.sfo", b"sfo")
        if stamp is not None:
            z.writestr("vjo_app.txt", stamp)
    return buf.getvalue()


def c_macros():
    text = (ROOT / "core/app_stamp.h").read_text()
    return dict(re.findall(r'^#define (VJO_APP_\w+) "([^"]*)"', text, re.MULTILINE))


class AppStamp(unittest.TestCase):
    def test_the_stamp_names_the_api_and_the_eboot(self):
        line = app_stamp.make(EBOOT, API)
        self.assertEqual(line, f"vjoapp1 api={API} build={hashlib.sha256(EBOOT).hexdigest()[:16]}\n")
        self.assertEqual(app_stamp.api_version(ROOT / "include/vjo_api.h"), API)
        parsed = app_stamp.parse(line)
        self.assertEqual((parsed["api"], parsed["build"]), (API, hashlib.sha256(EBOOT).hexdigest()[:16]))
        self.assertEqual(app_stamp.parse(line.strip() + "\r\nmore")["line"], line.strip())
        for bad in ("", "vjoapp1 api=20", "vjoapp2 api=20 build=0123456789abcdef", "vjoapp1 api=20 build=0123",
                    "vjoapp1 api=x build=0123456789abcdef", "vjoapp1 api=20 build=0123456789ABCDEF", None):
            self.assertIsNone(app_stamp.parse(bad), bad)
        # what the Shell's in-game check and the app compare: the line, under the C side's 64-byte buffer
        self.assertLess(len(line), 64)

    def test_the_vpk_and_the_record_are_one_file(self):
        line = app_stamp.make(EBOOT, API)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Rubi.vpk"
            path.write_bytes(vpk(stamp=line))
            info = app_stamp.vpk_info(path)
            self.assertEqual(info["stamp"]["line"], line.strip())
            self.assertEqual(info["eboot_sha256"], hashlib.sha256(EBOOT).hexdigest())
            path.write_bytes(b"not a zip")
            self.assertIsNone(app_stamp.vpk_info(path))
        # the build writes the VPK's vjo_app.txt and the release's record from one file
        cmake = (ROOT / "vita/CMakeLists.txt").read_text()
        self.assertIn("${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt vjo_app.txt)", cmake)
        self.assertIn("copy ${CMAKE_CURRENT_BINARY_DIR}/vjo_app.txt ${STAGE}/ux0/data/VitaJPOverlay/", cmake)

    def test_the_verdict_for_each_installed_app(self):
        line = app_stamp.make(EBOOT, API)
        sha = hashlib.sha256(EBOOT).hexdigest()
        state, lines = app_stamp.verdict(line, line, EBOOT, sha)
        self.assertEqual(state, "ok")
        self.assertIn("Its eboot.bin is the package VPK's.", lines)
        # the stamp decides; an eboot the install changed is reported, not a mismatch
        state, lines = app_stamp.verdict(line, line, b"installed differently", sha)
        self.assertEqual(state, "ok")
        self.assertIn("not byte-identical to the VPK's (the stamp decides", lines[1])
        # #101: an app from before the stamps (test.2's and older)
        state, lines = app_stamp.verdict(line, None, b"old eboot", sha)
        self.assertEqual(state, "mismatch")
        self.assertIn("no version stamp: it is from an older build (v0.8.0-test.2 or earlier)", lines[0])
        self.assertIn("Its eboot.bin differs from the package VPK's.", lines)
        state, lines = app_stamp.verdict(line, None, None)
        self.assertEqual(state, "mismatch")
        self.assertIn("No Rubi app is installed", lines[0])
        older = app_stamp.make(b"older", API - 1)
        self.assertIn("an older build", app_stamp.verdict(line, older)[1][0])
        self.assertIn("a newer build", app_stamp.verdict(older, line)[1][0])
        self.assertIn("another build", app_stamp.verdict(line, app_stamp.make(b"other", API))[1][0])
        self.assertIn("not readable", app_stamp.verdict(line, "garbage\n")[1][0])
        self.assertEqual(app_stamp.verdict(None, line)[0], "unknown")
        self.assertEqual(app_stamp.verdict("garbage", line)[0], "unknown")

    def test_the_box_is_loud_and_fits_a_terminal(self):
        line = app_stamp.make(EBOOT, API)
        command = "bash install-vita.sh --check-app 192.168.1.50"
        out = app_stamp.report(*app_stamp.verdict(line, None, b"old"), command, True)
        rows = out.splitlines()
        self.assertTrue(all(len(r) == 78 for r in rows), [len(r) for r in rows])
        self.assertTrue(rows[0].startswith("+---") and rows[-1].startswith("+---"))
        self.assertIn("ONE MORE STEP: install the Rubi app (Rubi.vpk)", rows[1])
        flat = " ".join(r.strip("| ") for r in rows)
        self.assertIn("press OK on the FTP dialog to keep FTP running", flat)
        self.assertIn("Rubi.vpk", flat)
        self.assertIn(command, flat)
        self.assertIn("Rubi app: NOT this package's", app_stamp.report("mismatch", ["x"], command, False))
        # an app newer than the package: the advice is not "an older app"
        newer = " ".join(app_stamp.report("mismatch", ["x"], command, True, newer=True).replace("|", " ").split())
        self.assertIn("A newer app saves settings these plugins do not know yet", newer)
        self.assertNotIn("An older app", newer)
        self.assertIn("Rubi app: up to date", app_stamp.report("ok", ["x"], command, True))
        self.assertIn("Rubi app: not checked", app_stamp.report("unknown", ["x"], command, True))

    def test_the_command_line_exit_codes(self):
        line = app_stamp.make(EBOOT, API)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "eboot.bin").write_bytes(EBOOT)
            made = subprocess.run([sys.executable, str(ROOT / "tools/app_stamp.py"), "make", "--eboot",
                                   str(tmp / "eboot.bin"), "--api-header", str(ROOT / "include/vjo_api.h"), "-o",
                                   str(tmp / "vjo_app.txt")], capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(made.returncode, 0, made.stderr)
            self.assertEqual((tmp / "vjo_app.txt").read_text(), line)
            (tmp / "Rubi.vpk").write_bytes(vpk(stamp=line))

            def check(*extra):
                return subprocess.run([sys.executable, str(ROOT / "tools/app_stamp.py"), "check", "--package",
                                       str(tmp / "vjo_app.txt"), "--vpk", str(tmp / "Rubi.vpk"), *extra],
                                      capture_output=True, text=True, timeout=30, check=False)
            (tmp / "device.txt").write_text(line)
            (tmp / "device.eboot").write_bytes(EBOOT)
            r = check("--installed", str(tmp / "device.txt"), "--installed-eboot", str(tmp / "device.eboot"))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("Rubi app: up to date", r.stdout)
            r = check("--installed", str(tmp / "absent.txt"), "--installed-eboot", str(tmp / "device.eboot"))
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            r = check("--installed", str(tmp / "absent.txt"), "--after-install")
            self.assertEqual(r.returncode, 1)
            self.assertIn("ONE MORE STEP", r.stdout)
            self.assertIn("No Rubi app is installed", " ".join(r.stdout.split()))
            r = subprocess.run([sys.executable, str(ROOT / "tools/app_stamp.py"), "check", "--package",
                                str(tmp / "absent.txt"), "--installed", str(tmp / "device.txt")],
                               capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(r.returncode, 2)
            self.assertIn("Rubi app: not checked", r.stdout)
            # the package's record and its VPK disagree: no verdict
            (tmp / "Rubi.vpk").write_bytes(vpk(stamp=app_stamp.make(b"other", API)))
            r = check("--installed", str(tmp / "device.txt"))
            self.assertEqual(r.returncode, 2)
            self.assertIn("differ", r.stderr)

    def test_c_shell_and_python_name_the_same_files(self):
        m = c_macros()
        self.assertEqual(m["VJO_APP_STAMP_INSTALLED"], "ux0:app/VJOSET001/vjo_app.txt")
        self.assertEqual(m["VJO_APP_EBOOT_INSTALLED"], "ux0:app/VJOSET001/eboot.bin")
        self.assertEqual(m["VJO_APP_STAMP_PACKAGE"], "ux0:data/VitaJPOverlay/vjo_app.txt")
        self.assertEqual(m["VJO_APP_STAMP_OWN"], "app0:vjo_app.txt")
        self.assertEqual("Rubi app and plugins are different builds: open the Rubi app.", m["VJO_APP_NOTICE"])
        ftp = (ROOT / "tools/install_ftp.sh").read_text()
        for path in (m["VJO_APP_STAMP_INSTALLED"], m["VJO_APP_EBOOT_INSTALLED"], m["VJO_APP_STAMP_PACKAGE"]):
            self.assertIn(path.replace(":", ":/", 1), ftp)
        import collect_vita
        # the installed app's two files, which collect_vita.py reads as --check-app does (read_app, #138)
        self.assertEqual(m["VJO_APP_STAMP_INSTALLED"].replace(":", ":/", 1), collect_vita.APP_STAMP)
        self.assertEqual(m["VJO_APP_EBOOT_INSTALLED"].replace(":", ":/", 1), collect_vita.APP_EBOOT)
        self.assertIn(m["VJO_APP_STAMP_PACKAGE"].replace(":", ":/", 1), collect_vita.FILES)
        import package_release
        self.assertEqual("ux0/data/VitaJPOverlay/vjo_app.txt", package_release.APP_RECORD)
        self.assertEqual(app_stamp.TITLE_ID, "VJOSET001")

    def test_every_tool_the_installer_runs_ships_in_the_release(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text()
        copied = next(l for l in cmake.splitlines() if "${STAGE}/tools/" in l and "install_ftp.sh" in l)
        used = set(re.findall(r"python3 tools/(\w+\.py)", (ROOT / "tools/install_ftp.sh").read_text()))
        self.assertIn("app_stamp.py", used)
        for name in sorted(used):
            self.assertIn("${ROOT}/tools/" + name, copied, name)


if __name__ == "__main__":
    unittest.main()
