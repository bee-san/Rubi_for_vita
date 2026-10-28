"""install_ftp.sh --collect end to end: the real script, curl and ftplib against a VitaShell-like FTP
server (ftp_mock.py) over a fake device tree. Everything is fetched into one timestamped folder and
zip, config.ini's API keys never reach the folder, the Vita is never written, and the analyser
summarises the logs, the self-test, the tiers and what it cannot decide."""
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ftp_mock import FtpMock  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
SECRET = "jpdb-secret-key-1234"
SECRET2 = "jiten-secret-without-equals"

LOG = """bootstrap: shell API 19 entered
bootstrap: kernel resident, installing Paf callback
Vita JP Overlay shell started (kernel API 19)
bootstrap: worker started
config warning: ignored line 'jiten_api_key {SECRET2}'
game PCSG00415 started: dictionary Local dictionaries (key MISSING), trigger l+r, subtitles select+r, ocr_mode auto
job start (auto: screen settled)
game vocr rc=0 VOCR3_H15_w8.vocr: 917x125 at 22,400 of 960x544 fmt 0x00000000, lines=3 USER workspace=188416 weights=read resident=1159168, find=9 ms load=1050 ms recognize=820 ms frames=330 io=0x00000000 stat=0x00000000 cleanup=0
game vocr capture: hook into USER, 61440 B block, 62-row strips, 9 strips in 34 ms, 0 rows changed, sum 0x5EED0003
game vocr memory: free USER=91226112, needs workspace 249856 + weights 1159168 + reserve 4194304: check passed, headroom 85622784
game vocr speed: 330 frames in 820000 us, 2484 us per frame
game vocr cascade speed: 180 frames in 550000 us, 3055 us per frame
game vocr lines: 877x30 110f 273.3ms c912 870x30 110f 273.3ms c412* 860x30 110f 273.3ms c998 640x30 80f 273.3ms c855*^
gstep job alloc kind=user arg=capture rc=0x00000041 bytes=61440 sum=0x00000000 main=91164672 cdram=117440512 phycont=0 us=30
gstep job settle kind=- arg=4 rc=0x00000001 bytes=37 sum=0x00020006 main=0 cdram=0 phycont=0 us=183000
gstep job par kind=- arg=2 rc=0x00000002 bytes=533504 sum=0x00015A62 main=0 cdram=0 phycont=0 us=410000
gstep job clock kind=- arg=333 rc=0x00000000 bytes=444 sum=0x0000014D main=0 cdram=0 phycont=0 us=411000
gstep job pace kind=- arg=31 rc=0x0000C351 bytes=533340 sum=0x00000001 main=3000000 cdram=17000 phycont=0 us=180
gstep job blank kind=- arg=2 rc=0x00000001 bytes=19660830 sum=0x00C00014 main=0 cdram=0 phycont=0 us=310
gstep job norm kind=- arg=4 rc=0x00000000 bytes=2 sum=0x00000121 main=0 cdram=0 phycont=0 us=41
gstep job lm kind=2 arg=1 rc=0x00000000 bytes=2147549187 sum=0x0016E360 main=0 cdram=0 phycont=0 us=2100
gstep job cascade kind=1 arg=1 rc=0x00000000 bytes=2164261044 sum=0x0004B000 main=0 cdram=0 phycont=0 us=550000
gstep job refine kind=2 arg=4 rc=0x00000000 bytes=196865 sum=0x00000060 main=0 cdram=0 phycont=0 us=1400
game vocr text 0: コ一ヒ一！
game vocr text 0 shown: コーヒー!
game vocr text 1: Error 404 not found
job 0 done rc=0 in 2100 ms, arena peak 4096, dictionary page 1/1
gstep job blank kind=- arg=0 rc=0x00000006 bytes=19660830 sum=0x00C00010 main=0 cdram=0 phycont=0 us=410
gstep job blank kind=- arg=1 rc=0x00000006 bytes=19660830 sum=0x00C00011 main=0 cdram=0 phycont=0 us=410
gstep job blank kind=- arg=3 rc=0x00000006 bytes=19660830 sum=0x00C00012 main=0 cdram=0 phycont=0 us=410
gstep job blank kind=- arg=4 rc=0x00000006 bytes=19660830 sum=0x00C00013 main=0 cdram=0 phycont=0 us=410
gstep job load kind=resident arg=0 rc=0x00000000 bytes=1159168 sum=0x00000000 main=0 cdram=0 phycont=0 us=1
tier large did not fit in PCSG00415 (need 18649088, free 5000000); trying normal
tier normal did not fit in PCSG00415 (need 9801728, free 5000000); trying small
tier small did not fit in PCSG00415 (need 5050368, free 5000000); no smaller tier: not enough memory
tier large did not fit in PCSB00123 (need 19000000, free 12000000); using normal
game vocr rc=-103 VOCR3_FL10_w8.vocr (fallback, refused too): 917x125 at 22,400 of 960x544 fmt 0x00000000, lines=0 USER workspace=0 weights=none resident=0, find=0 ms load=0 ms recognize=0 ms frames=0 io=0x00000000 stat=0x00000000 cleanup=0
game vocr memory: free USER=5000000, needs workspace 245760 + weights 659456 + reserve 4194304: check refused, headroom -99520
gstep job alloc kind=user arg=workspace rc=0x80024302 bytes=188416 sum=0x00000000 main=5000000 cdram=0 phycont=0 us=40
overlay closed: 120 frames, 3 word moves, stick 0, slowest frame 33 ms
VjoNet stack: at least 5920 of 16384 B used
VjoNet stack: at least 6144 of 16384 B used
anki: Picture field off in this build: no screenshot, no kernel capture buffer
anki: save 猫 rc=0, 0-byte screenshot, 45 ms
anki: save 犬 rc=0, 12 ms
anki: queue 1 card(s)
anki: sent 0, 0 duplicates, 0 unreadable, 0 too large, 1 pending, 30 ms
overlay app: anki: export (queued) rc=0: 3 cards, 3 moved, 0 kept, 0 unreadable, 0 too large, 0 left, 40960 bytes, arena peak 1000
thread VjoAnki refused 0x80024302: 69632 B; SceShell free 28672 (0x00000000), held 389120
anki: count not started 0x80024302
""".replace("{SECRET2}", SECRET2)
# this boot's whole log (#138: its first line the boot guard's, boot 3 as boot.txt's seq=3), and its last line, a minute
# into the boot, carries the uptime: setUp writes every file at once, so the clock is checked (docs/self-test.md)
KERNEL = """boot guard: boot 3 normal (previous: normal, stage ok, failures 0) write 0x00000000
Vita JP Overlay kernel 19 started (normal mode)
hooks: display 0, input 0, procevent 0, worker loader 0 (0 = ok)
shell registered: pid 0x10000 (was 0x0)
worker for PCSG00415 (pid 0x20000): ON in games.ini: ur0:tai/VitaJPOverlay_OCR.suprx 0x00030000, start 0
game pid 0x20000 active
ocr worker registered: game pid 0x20000 module 0x30000
ocr job 1: 917x125 at 22,400 of 960x544, capture 1, flags 0x0, VOCR3_H15_w8.vocr / VOCR3_FL10_w8.vocr
foreground 0x20000 -> 0x0 (suspend) t=60000 ms
"""
CONFIG = (f"\ufeff; settings\r\nocr_backend = vocr\r\nvocr_model = VOCR3_H15_w8.vocr\r\nvocr_fallback_model = VOCR3_FL10_w8.vocr\r\n"
          f"ocr_mode = auto\r\nlog_file = on\r\nanki_mining = on\r\nanki_host =\r\njpdb_api_key = {SECRET}\r\njiten_api_key =\r\n"
          f"jiten_api_key {SECRET2}\r\n\tJPDB_API_KEY\t=\tx{SECRET}\x0cy\r\nlens_api_key = cr{SECRET}\rpart\r\n")
# the debug build's hooks.log (game_ocr/hooks.c's lines; no text): CLANNAD's verified entry hooked, the string copies
HOOKS_LOG = ("PCSG00415: text source 1\n"
             "main module game NID C5187D60 seg0 81000000+1C4000 APP_VER 0100; 1 verified, 0 Luna sites\n"
             "verified +2CA70 thumb r6 hooked, stream 1\n"
             + "".join(f"import {lib}/{nid}: hooked\n" for lib, nid in (
                 ("BE43BB07", "7205BFDB"), ("BE43BB07", "85B924B7"), ("BE43BB07", "9F87712D"), ("BE43BB07", "AF5C218D"),
                 ("CAE9ACE6", "14E9DBD7"), ("CAE9ACE6", "C458D60A"), ("CAE9ACE6", "2CDFCD1C"), ("CAE9ACE6", "AC595E68")))
             + "sites hooked 1, refused 0; CALL imports hooked 8 of 8\n")
TAI = (b"*KERNEL\r\nur0:tai/VitaJPOverlay_Kernel.skprx\r\n*main\r\nur0:tai/VitaJPOverlay_Shell.suprx\r\n*ALL\r\n"
       b"# Mainline OCR: ur0:tai/VitaJPOverlay_OCR.suprx\r\n*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n")


class Collect(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.package = self.root / "package"
        (self.package / "tools").mkdir(parents=True)
        for name in ("install_ftp.sh", "collect_vita.py", "analyse_selftest.py", "analyse_timeline.py", "hook_request.py",
                     "model_tiers.json", "package_release.py", "app_stamp.py",
                     "prepare_ocr.py", "prepare_plaintext.py", "migrate_config.py", "patch_tai_config.py"):
            shutil.copyfile(ROOT / "tools" / name, self.package / "tools" / name)
        shutil.copyfile(ROOT / "tools/install-vita.sh", self.package / "install-vita.sh")
        self.device = self.root / "device"
        data = self.device / "ux0/data/VitaJPOverlay"
        (data / "anki_queue").mkdir(parents=True)
        (data / "ocr").mkdir()
        (data / "dictionaries").mkdir()
        (self.device / "ur0/tai").mkdir(parents=True)
        (self.device / "ux0/tai").mkdir(parents=True)
        (self.device / "ur0/data/VitaJPOverlay").mkdir(parents=True)
        (data / "log.txt").write_text(LOG, encoding="utf-8")
        (data / "log.old.txt").write_text("bootstrap: shell API 19 entered\n", encoding="utf-8")
        (data / "kernel.txt").write_text(KERNEL)
        (data / "kernel.prev.txt").write_text("Vita JP Overlay kernel 19 started (normal mode)\n")
        (data / "hooks.log").write_text(HOOKS_LOG)
        (data / "status.txt").write_text("bootstrap: shell API 19 entered\nbootstrap: worker started\n")
        shutil.copyfile(FIXTURES / "selftest-v2-pass.txt", data / "selftest-report.txt")
        (data / "config.ini").write_text(CONFIG, encoding="utf-8")
        (data / "games.ini").write_text("default = on\n")
        (data / "ocr_line_fix.ini").write_text("ULJM05674 = all\n")  # #333
        (data / "region.ini").write_text("PCSG00415 = 1536,48188,62600,15059\n")
        (data / "hooks.log").write_text("hooks: mode 1, 0 sites\n")
        (data / "psp_probe.log").write_text("psp probe 2: process PSPEMUCFW\n")
        (data / "anki_queue/exported").mkdir()
        (data / "anki_queue/exported/00112233445566778899aabbccddeeff.json").write_text('{"version":3}\n')
        (data / "anki").mkdir()
        (data / "anki/2026-10-15_143000-3cards.apkg").write_bytes(b"PK" + bytes(40))
        (data / "anki_queue/0123456789abcdef0123456789abcdef.json").write_text('{"version":2,"spelling":"猫"}\n',
                                                                                encoding="utf-8")
        (data / "anki_queue/fedcba9876543210fedcba9876543210.json.tmp").write_text("{")
        (data / "anki_queue/a  b#c%20d?.json").write_text("{}")  # spaces, URL characters: fetched as named
        (self.device / "ur0/data/VitaJPOverlay/boot.txt").write_text(
            "vjoboot1 seq=3 mode=0 stage=4 inert=0 fault=0 failures=0 api=19 prev=0/0/0/0\n")
        (self.device / "ur0/tai/config.txt").write_bytes(TAI)
        (self.device / "ur0/tai/config.txt.vjo-bak").write_bytes(b"*KERNEL\r\n")
        # the one-VPK install (settings/install.c): its record, a dated backup, an interrupted swap's leftover
        payload = b"vjopayload 1\nversion 0.8.0-test.1\n"
        (self.package / "payload-manifest.txt").write_bytes(payload)
        (data / "installed.txt").write_text("vjoinst1 version=0.8.0-test.1 variant=debug api=22 build=0123456789abcdef "
                                            f"manifest={hashlib.sha256(payload).hexdigest()}\n")
        (self.device / "ur0/tai/config.txt.vjo-20261016-120000.bak").write_bytes(b"*KERNEL\r\n")
        (self.device / "ux0/tai/config.txt.vjo-new").write_bytes(b"*KERNEL\r\n")
        (self.device / "ux0/tai/config.txt.vjo-20261016-120000-old.bak").write_bytes(b"*KERNEL\r\n")
        (self.device / "ux0/tai/config.txt.vjo-bad").write_bytes(b"*KERN")
        (self.device / "ux0/tai/mine.txt").write_bytes(b"not fetched\n")
        (self.device / "ux0/data/psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp").write_bytes(b"\0" * 4096)
        # installed files, and the package's manifest of them (one differs)
        files = {}
        for rel, body in (("ur0/tai/VitaJPOverlay_Kernel.skprx", b"kernel"), ("ur0/tai/VitaJPOverlay_Shell.suprx", b"shell"),
                          ("ur0/tai/VitaJPOverlay_OCR.suprx", b"worker"),
                          ("ur0/data/VitaJPOverlay/vitajpoverlay.rco", b"rco"),
                          ("ux0/data/VitaJPOverlay/ocr/VOCR3_H15_w8.vocr", b"h15 weights"),
                          ("ux0/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr", b"fl10 weights"),
                          ("ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict", b"dictionary bytes")):
            (self.device / rel).write_bytes(body)
            files[rel] = {"bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()}
        files["Rubi.vpk"] = {"bytes": 3, "sha256": hashlib.sha256(b"vpk").hexdigest()}
        (self.device / "ux0/data/Rubi.vpk").write_bytes(b"vpk")
        # an install from before the Rubi rename uploaded its app as JPOverlaySettings.vpk: hashed, compared with nothing
        (self.device / "ux0/data/JPOverlaySettings.vpk").write_bytes(b"old vpk")
        files["ux0/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"]["sha256"] = "0" * 64  # the device's differs
        # a removed engine's model left on the card (VOCR2, 0.7.12 to v0.8.0-test.1): not in the package
        (self.device / "ux0/data/VitaJPOverlay/ocr/VOCR2_FL10_w8.vocr").write_bytes(b"vocr2 weights")
        (self.package / "package-manifest.json").write_text(json.dumps(
            {"version": "0.8.0-test.1", "source_revision": "a" * 40, "files": files,
             "install_settings": {"anki_mining": "on"}}))
        self.before = {p: p.read_bytes() for p in self.device.rglob("*") if p.is_file()}

    def collect(self, ftp):
        return subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--collect", f"127.0.0.1:{ftp.port}"],
                              capture_output=True, text=True, timeout=180, cwd=self.root)

    # ---- the installed Rubi app (core/app_stamp.h): --collect reads it as --check-app does (#138) ----
    EBOOT = b"SCE\0 the package's eboot.bin"
    STAMP = f"vjoapp1 api=21 build={hashlib.sha256(EBOOT).hexdigest()[:16]}"

    def stamp_package(self):
        """The package's app (its manifest's "app", its stamp and VPK, as --check-app reads them from an extracted
        package's root) and the plugins' record of it on the Vita."""
        manifest = json.loads((self.package / "package-manifest.json").read_text())
        manifest["app"] = {"stamp": self.STAMP, "title_id": "VJOSET001", "eboot_bytes": len(self.EBOOT),
                           "eboot_sha256": hashlib.sha256(self.EBOOT).hexdigest()}
        (self.package / "package-manifest.json").write_text(json.dumps(manifest))
        (self.package / "ux0/data/VitaJPOverlay").mkdir(parents=True, exist_ok=True)
        (self.package / "ux0/data/VitaJPOverlay/vjo_app.txt").write_text(self.STAMP + "\n")
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("eboot.bin", self.EBOOT)
            z.writestr("vjo_app.txt", self.STAMP + "\n")
        (self.package / "Rubi.vpk").write_bytes(buf.getvalue())
        (self.device / "ux0/data/VitaJPOverlay/vjo_app.txt").write_text(self.STAMP + "\n")

    def device_app(self, eboot=None, stamp=None, folder=True):
        app = self.device / "ux0/app/VJOSET001"
        if app.is_file():
            app.unlink()
        shutil.rmtree(app, ignore_errors=True)
        if folder:
            app.mkdir(parents=True)
        if eboot is not None:
            (app / "eboot.bin").write_bytes(eboot)
        if stamp is not None:
            (app / "vjo_app.txt").write_text(stamp + "\n")

    def check_app(self, ftp):
        return subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--check-app", f"127.0.0.1:{ftp.port}"],
                              capture_output=True, text=True, timeout=120, cwd=self.root)

    def latest(self):
        """The newest collect folder, its COLLECT.json and its analysis.txt."""
        folder = max(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        return folder, json.loads((folder / "COLLECT.json").read_text()), (folder / "analysis.txt").read_text()

    def test_collect_fetches_everything_read_only_into_one_zip(self):
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
            writes = list(ftp.stored)
            commands = {line.split()[0].upper() for line in ftp.log}
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # read-only: nothing stored, deleted or created on the Vita, and no file on it changed
        self.assertEqual(writes, [])
        self.assertFalse(commands & {"STOR", "APPE", "DELE", "MKD", "RMD", "RNFR", "RNTO", "SITE"}, commands)
        self.assertEqual({p: p.read_bytes() for p in self.device.rglob("*") if p.is_file()}, self.before)
        folders = list((self.package / "build/device/collect").glob("vjo-collect-*"))
        folder = next(f for f in folders if f.is_dir())
        zpath = Path(str(folder) + ".zip")
        self.assertTrue(zpath.is_file())
        self.assertRegex(folder.name, r"^vjo-collect-\d{8}-\d{6}Z$")
        self.assertIn(f"Send this one file: {zpath}", result.stdout)
        # the dump from before this boot: no terminal here, so no question, only the command (#138)
        self.assertIn("To archive them off the Vita: bash tools/install_ftp.sh --archive-dumps 127.0.0.1", result.stdout)
        with zipfile.ZipFile(zpath) as z:
            names = set(z.namelist())
            blob = b"".join(z.read(n) for n in names)
        d = folder.name + "/ux0/data/VitaJPOverlay/"
        for rel in ("log.txt", "log.old.txt", "selftest-report.txt", "status.txt", "kernel.txt", "kernel.prev.txt",
                    "hooks.log", "psp_probe.log", "config.ini", "games.ini", "ocr_line_fix.ini", "region.ini",
                    "anki_queue/0123456789abcdef0123456789abcdef.json",
                    "anki_queue/exported/00112233445566778899aabbccddeeff.json",
                    "anki_queue/fedcba9876543210fedcba9876543210.json.tmp"):
            self.assertIn(d + rel, names)
        for rel in ("ur0/data/VitaJPOverlay/boot.txt", "ur0/tai/config.txt", "ur0/tai/config.txt.vjo-bak",
                    "ur0/tai/config.txt.vjo-20261016-120000.bak", "ux0/tai/config.txt.vjo-new",
                    "ux0/data/VitaJPOverlay/installed.txt",
                    "ux0/data/psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp", "COLLECT.json", "analysis.txt",
                    "analysis.json", "timeline.txt", "hook-request.md"):
            self.assertIn(folder.name + "/" + rel, names)
        for rel in ("ux0/tai/config.txt.vjo-20261016-120000-old.bak", "ux0/tai/config.txt.vjo-bad"):
            self.assertIn(folder.name + "/" + rel, names)  # every copy the installers make
        self.assertNotIn(folder.name + "/ux0/tai/mine.txt", names)
        self.assertIn("sessions (game starts):", (folder / "timeline.txt").read_text())
        # hooks.log as it was on the Vita, and what a hook request reads in it (docs/hook-requests.md), its line printed
        # after the analysis's and before the zip's name
        self.assertEqual((folder / "ux0/data/VitaJPOverlay/hooks.log").read_text(), HOOKS_LOG)
        self.assertTrue((folder / "hook-request.md").read_text().startswith("# Hook request: PCSG00415\n"))
        hooks_line = ("hooks.log: PCSG00415, APP_VER 01.00, main module game (NID 0xC5187D60): Rubi's verified entry is "
                      "installed in this game; 8 string-copy imports hooked. Report: build/device/collect/")
        self.assertIn(hooks_line, result.stdout)
        self.assertLess(result.stdout.index("analysis exit "), result.stdout.index(hooks_line))
        self.assertLess(result.stdout.index(hooks_line), result.stdout.index("Send this one file: "))
        self.assertTrue(any(n.startswith(folder.name + "/listings/") for n in names))
        self.assertIn(d + "anki_queue/a  b#c%20d?.json", names)
        # no API key is in any file of the zip, also one written without = (the C parser logs that line
        # whole as a config warning) or one with a form feed in it; its length is
        self.assertNotIn(SECRET.encode(), blob)
        self.assertNotIn(SECRET2.encode(), blob)
        self.assertNotIn(SECRET2, result.stdout + result.stderr)
        config = (folder / "ux0/data/VitaJPOverlay/config.ini").read_bytes().decode()
        self.assertIn(f"jpdb_api_key = <redacted: {len(SECRET)} characters>\r\n", config)
        self.assertIn("jiten_api_key =\r\n", config)
        self.assertIn(f"jiten_api_key <redacted: {len(SECRET2)} characters>\r\n", config)
        self.assertIn(f"\tJPDB_API_KEY\t=\t<redacted: {len(SECRET) + 3} characters>\r\n", config)
        self.assertIn(f"lens_api_key = <redacted: {len(SECRET) + 7} characters>\r\n", config)  # a lone CR stays in
        self.assertIn("config warning: ignored line 'jiten_api_key <redacted>'",
                      (folder / "ux0/data/VitaJPOverlay/log.txt").read_text())
        # the weights are hashed, never kept
        self.assertFalse(any(n.endswith(".vocr") for n in names))
        record = json.loads((folder / "COLLECT.json").read_text())
        self.assertEqual(record["redacted_keys"], ["jpdb_api_key", "jiten_api_key", "jpdb_api_key", "lens_api_key"])
        self.assertEqual(record["redacted_log_lines"], {"ux0:/data/VitaJPOverlay/log.txt": 1})
        self.assertIn("ux0:/data/VitaJPOverlay/selftest.txt", record["missing"])
        self.assertEqual(record["errors"], [])
        self.assertEqual(record["hashed"]["ux0:/data/VitaJPOverlay/ocr/VOCR3_H15_w8.vocr"]["sha256"],
                         hashlib.sha256(b"h15 weights").hexdigest())
        check = record["package_check"]
        self.assertTrue(check["ur0:/tai/VitaJPOverlay_Kernel.skprx"]["match"])
        self.assertFalse(check["ux0:/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"]["match"])
        self.assertIsNone(check["ux0:/data/VitaJPOverlay/ocr/VOCR2_FL10_w8.vocr"]["match"])
        self.assertTrue(check["ux0:/data/VitaJPOverlay/dictionaries/plaintext.vjdict"]["match"])
        self.assertTrue(check["ux0:/data/Rubi.vpk"]["match"])
        self.assertEqual(record["hashed"]["ux0:/data/JPOverlaySettings.vpk"]["sha256"], hashlib.sha256(b"old vpk").hexdigest())
        self.assertIsNone(check["ux0:/data/JPOverlaySettings.vpk"]["match"])
        # the analysis: the self-test, the logs, the tiers, and what it cannot decide
        analysis = (folder / "analysis.txt").read_text()
        for text in ("installed files: 7 of 8 match the package; DIFFER or absent: ux0:/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"
                     "; not in this package: ux0:/data/JPOverlaySettings.vpk, ux0:/data/VitaJPOverlay/ocr/VOCR2_FL10_w8.vocr",
                     "API keys redacted: jpdb_api_key, jiten_api_key, jpdb_api_key, lens_api_key",
                     # the engine choice went with Google Lens: even ocr_backend = vocr is a line to delete
                     "    ocr_backend=vocr: no longer a setting (VOCR is the only engine); today's app or installer "
                     "deletes the line",
                     "games: PCSG00415 x1 (ocr_mode auto, Local dictionaries)",
                     "Shell jobs (OCR + dictionary): 1 (rc ok (rc 0) x1)",
                     "SceShell refused memory 2 time(s)",
                     "ur0:tai/config.txt: kernel under ['KERNEL'], shell under ['main'], OCR worker under ['PCSG00415'] "
                     "(an older install's line: the kernel loads the worker since #138; the Rubi app's Install removes it)",
                     # the one-VPK install: its record against the package, the backups and a leftover
                     "one-VPK install (installed.txt): v0.8.0-test.1, the debug build, API 22, app build 0123456789abcdef; "
                     "this package's payload",
                     "taiHEN backups: ur0:tai/config.txt.vjo-20261016-120000.bak, ur0:tai/config.txt.vjo-bak, "
                     "ux0:tai/config.txt.vjo-20261016-120000-old.bak, "
                     "ux0:tai/config.txt.vjo-bad; an interrupted swap's leftovers: "
                     "ux0:tai/config.txt.vjo-new (the Rubi app's Install page finishes the swap or puts it back)",
                     "boot.txt: vjoboot1 seq=3 mode=0 stage=4",
                     "worker registrations 1, OCR jobs submitted 1",
                     "    worker loader: loaded into PCSG00415",  # #138: the kernel's own load
                     "OCR jobs: 2; rc out of memory (rc -103) x1, rc ok (rc 0) x1",
                     "models VOCR3_H15_w8.vocr x1, VOCR3_FL10_w8.vocr (fallback) x1",
                     "times (ms): find 9-9, load 1,050-1,050 (1 read), recognize 820-820; per line 273-273",
                     "capture: hook into USER x1",
                     # API 21: the model sizes that did not fit, from their log lines (no UI shows them)
                     "model sizes that did not fit (log only, never shown): large in PCSG00415 x1 (need 18,649,088 B, "
                     "free as low as 5,000,000 B; then normal x1); normal in PCSG00415 x1 (need 9,801,728 B, free as low "
                     "as 5,000,000 B; then small x1); small in PCSG00415 x1 (need 5,050,368 B, free as low as 5,000,000 B; "
                     "then none x1); large in PCSB00123 x1 (need 19,000,000 B, free as low as 12,000,000 B; then normal x1)",
                     "vocr_model=VOCR3_H15_w8.vocr, vocr_fallback_model=VOCR3_FL10_w8.vocr",  # an older build's config
                     "stable-text wait: 1 job(s): rows held still in 1, the limit reached in 0, no line to wait for in 0; "
                     "waited 183-183 ms (4-4 passes); 37 row change(s) seen while waiting (text typing out); rows toggling "
                     "(an icon) in 1 job(s)",
                     "blank lines skipped: 7 (300x30, 300x30, 300x30, 300x30, 300x30; edges 16-20 per 10,000)",
                     "text normalised: 1 of 1 job(s) changed: 2 full-width form(s), 0 tilde(s), 0 dash(es), "
                     "1 ellipsis(es), 2 一->ー, 1 ー->一; 41-41 us",
                     "per CTC frame 2,484-2,484 us (the cascade's model 3,055-3,055 us); with helpers, per CTC frame is "
                     "a job's recognition time / its frames, and its lines overlap in that time",
                     "beam + LM: 1 job(s) (1000k x1), 3 line(s) decoded, 1 changed by the LM; 1 line(s) under 0.70 confidence "
                     "(median 0.91); LM read 1x (1,500-1,500 ms), decoding 2-2 ms a job",
                     "cascade: 1 job(s), 1 doubtful line(s) read again with the larger model (1 changed, 1 read as empty; its confidence "
                     "median 0.85), 0 left for the time ceiling; its weights read 1x (307-307 ms), re-reading 550-550 ms "
                     "a job",
                     "finder boxes refined: 1 job(s) (psp x1), 3 box(es) became 4 line(s) (1 split, 1 not split for the "
                     "16-line cap), 96 rows read from memory; 1,400-1,400 us a job",
                     "parallel lines: 1 job(s) with helpers 2-2; 2 line(s) on a helper, 0 helper(s) did not start, 1 "
                     "line(s) from the cache; the recognizing threads' share of a core: lowest 90-90%, mean 98-98%",
                     "ARM clock (ocr_boost_clock): 1 job(s), raised in 1; MHz before 333-333, during 444-444, after "
                     "333-333",
                     "game frames during OCR jobs: 31 in 1 job(s), longest gap 50-50 ms, 1 over 25 ms; between jobs "
                     "180 frames, longest gap 17-17 ms, 0 over 25 ms",
                     "game memory: free USER 5,000,000-91,226,112 B; headroom -99,520-85,622,784 B; refused 1 of 2",
                     "game worker steps that failed: alloc user/workspace 0x80024302",
                     "overlay opened 1x, slowest frame 33 ms",
                     "Anki: 2 save(s) (0 failed), queue 1, Picture off in this build",
                     # a successful export is reported as one (audit-test-build-3 N1), and is no problem line below
                     "Anki exports (the Rubi app): 1 export(s): 1 succeeded (a package written and checked), "
                     "0 failed; 3 card(s) in the packages",
                     "  VjoNet stack: at least 6,144 of 16,384 B used (37%)\n",
                     "anki_queue on the card: 2 card(s), 0 duplicate(s), 0 picture(s), 1 temp file(s); 1 exported card(s), "
                     "1 .apkg package(s) in anki/",
                     # #138: dated by its name (2025-10-09) before the boot the records date (now, by the mock's clock)
                     "crash dumps: 1 on the card, none from this boot, 1 from before it; this boot began between ",
                     "    before this boot 2025-10-09 08:53:20  an app or game (eboot.bin) pid 0x1234, 4,096 B: "
                     "psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp (dated before this boot began)",
                     "free game USER before the self-test's first block (selftest-report.txt): 91,226,112 B",
                     "recommendation: Large (the largest tier",
                     "free game USER the lowest any job in log.txt saw",
                     "recommendation for this game: Large (the most conservative",
                     "PASS: every stage completed",
                     "  - 1 crash dump on the card is from earlier boots (above): install_ftp.sh --collect offers to "
                     "archive them off the Vita",
                     "log.txt rotated"):
            self.assertIn(text, analysis)
        # the lowest free USER any job saw (5,000,000 B, a job the old 4 MiB reserve refused) has room for
        # Large's 4,399,104 B first job beside the 512 KiB test draft's reserve (4,923,392 B; beside #280's 1 MiB
        # only Normal's 2,449,408 B fitted): the game's recommendation is the most conservative sample's
        problems = analysis.split("lines reporting a problem", 1)[1].split("anki_queue on the card")[0]
        self.assertNotIn("key MISSING", problems)
        self.assertNotIn("anki: sent N, N duplicates, N unreadable", problems)  # a clean summary is not a problem
        self.assertNotIn("overlay app: anki: export", problems)  # nor a successful export (audit-test-build-3 N1)
        self.assertIn("thread VjoAnki refused", problems)
        self.assertIn("anki: count not started", problems)
        self.assertNotIn("Error 404", problems)  # recognized game text is never a problem line
        # the ARM clock by game window: its row names where the window starts (log.old.txt is read first)
        flat = " ".join(analysis.split())
        self.assertIn("1 log.txt:6 PCSG00415 2 1 1 333 / 444 / 333 (raised in 1); VOCR3_H15_w8.vocr 2,666 us (1 job, "
                      "4 lines)", flat)
        self.assertIn("boost effective: unclear (ratio -, n=0): no window with the boost off in this log", flat)
        summary = json.loads((folder / "analysis.json").read_text())
        self.assertEqual([(v["verdict"], v["ratio"], v["n"]) for v in summary["log"]["boost_verdicts"]],
                         [("unclear", None, 0)])
        self.assertEqual(summary["selftest"]["ocr_result"], "PASS")
        self.assertEqual([t["recommendation"] for t in summary["tiers_all"]], ["Large", "Large"])
        self.assertEqual(summary["tiers"]["recommendation"], "Large")
        # both samples priced: the self-test's and the lowest a job saw
        self.assertEqual([t["free_user"] for t in summary["tiers_all"]], [91_226_112, 5_000_000])
        self.assertEqual(summary["selftest"]["tiers"]["rows"][2]["need"], 1_720_320)

    def test_the_overlay_app_is_collected_read_only_and_compared(self):
        # The installed Rubi app (core/app_stamp.h): its stamp is fetched, its eboot.bin hashed and never kept,
        # and both are compared with the plugins' record and the package (#101). --check-app reads the same two
        # files the same way and gives the same answer (#138: test.3's --collect said "not installed" where
        # --check-app had said "up to date").
        self.stamp_package()
        eboot, other, stamp = self.EBOOT, b"SCE\0 test.2's eboot.bin", self.STAMP
        (self.device / "ux0/data/VitaJPOverlay/config.ini").write_text("ocr_backend = vocr2\n")
        (self.device / "ux0/app/PCSG00415").mkdir(parents=True)  # a game: never named by the collect
        cases = ((dict(eboot=other), ("Rubi app: NOT the plugins' (installed: an app from before the version stamps, "
                                      f"eboot.bin {hashlib.sha256(other).hexdigest()[:16]}; the plugins came with {stamp})"),
                  1, "no version stamp"),
                 (dict(eboot=eboot, stamp=stamp), f"Rubi app: the plugins' ({stamp}); its eboot.bin is the package's",
                  0, "Rubi app: up to date"),
                 # a folder without the app's files: the collect lists it to say so
                 (dict(), "Rubi app: not installed (ux0:app/VJOSET001 has no eboot.bin)", 1,
                  "No Rubi app is installed"),
                 # no folder: it cannot be entered, so ux0:app is listed, for VJOSET001 alone
                 (dict(folder=False), "Rubi app: not installed (ux0:app has no VJOSET001)", 1,
                  "No Rubi app is installed"),
                 # a file of that name is no app's folder
                 (dict(folder=False, file=True), "Rubi app: not installed (ux0:app has no VJOSET001)", 1,
                  "No Rubi app is installed"))
        for files, text, code, check_text in cases:
            with self.subTest(text=text, files=sorted(files)):
                self.device_app(**{k: v for k, v in files.items() if k != "file"})
                if files.get("file"):
                    (self.device / "ux0/app/VJOSET001").write_bytes(b"not a folder")
                before = {q: q.read_bytes() for q in self.device.rglob("*") if q.is_file()}
                with FtpMock(self.device) as ftp:
                    result = self.collect(ftp)
                    check = self.check_app(ftp)
                    writes = list(ftp.stored)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(writes, [])
                self.assertEqual({q: q.read_bytes() for q in self.device.rglob("*") if q.is_file()}, before)
                folder, record, analysis = self.latest()
                self.assertEqual(record["package_app"]["stamp"], stamp)
                body = files.get("eboot")
                self.assertEqual((record["app"]["eboot"] or {}).get("sha256"),
                                 hashlib.sha256(body).hexdigest() if body else None)
                self.assertNotIn("ux0:/app/VJOSET001/eboot.bin", record["hashed"])  # not a package file
                # each file read through its folder, as --check-app's curl does; a file that is not there is
                # asked for once more by its full path
                for name, there in (("vjo_app.txt", "stamp" in files), ("eboot.bin", body is not None)):
                    self.assertEqual(record["app"]["reads"][name]["state"], "read" if there else "absent", name)
                    self.assertEqual(record["app"]["reads"][name]["method"], "cwd" if there else None, name)
                # the app's own paths are the Rubi app line's, not "not on the Vita" (#138)
                self.assertFalse([m for m in record["missing"] if m.startswith("ux0:/app/")], record["missing"])
                self.assertNotIn("not on the Vita: ux0:/app", analysis)
                # ux0:app is listed only to find VJOSET001: no other title is kept anywhere
                self.assertNotIn(b"PCSG00415", (folder / "COLLECT.json").read_bytes())
                self.assertFalse([p.name for p in (folder / "listings").iterdir() if b"PCSG00415" in p.read_bytes()])
                self.assertNotIn("ux0:/app", record["listings"])
                with zipfile.ZipFile(Path(str(folder) + ".zip")) as z:
                    self.assertFalse(any(n.endswith("eboot.bin") for n in z.namelist()))
                self.assertIn(text, analysis)
                self.assertIn("ocr_backend=vocr2: a removed engine, which an older Rubi app writes; the plugins run "
                              "vocr, and today's app or installer deletes the line", analysis)
                self.assertNotIn("not in this package: ux0:/app", analysis)
                # --check-app, on the same Vita: the same answer
                self.assertEqual(check.returncode, code, check.stdout + check.stderr)
                self.assertIn(check_text, " ".join(check.stdout.replace("|", " ").split()))
                shutil.rmtree(folder)
                Path(str(folder) + ".zip").unlink()

    def test_the_app_is_read_by_its_full_path_when_its_folder_cannot_be_entered(self):
        """#138: test.3's --collect said "Rubi app: not installed" and listed ux0:/app/VJOSET001/ as not on the
        Vita: every CWD into that folder was refused (curl's for the stamp, ftplib's three spellings for its
        listing), while --check-app had read it after the install and the Shell read its stamp at the game starts
        of the same boot. Both tools now read each file through the folder and then by its full path, which
        VitaShell's FTP opens without entering the folder (libftpvita: SIZE and RETR take a "ux0:/..." argument
        as a full path; CWD needs the folder to open), and give the same answer."""
        self.stamp_package()
        self.device_app(self.EBOOT, self.STAMP)
        refused = {"CWD": lambda arg: "VJOSET001" in arg}
        with FtpMock(self.device, fail=refused) as ftp:
            result = self.collect(ftp)
            log = list(ftp.log)
            check = self.check_app(ftp)
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(writes, [])
        folder, record, analysis = self.latest()
        for name in ("vjo_app.txt", "eboot.bin"):
            # curl 9: the CWD into the folder refused; then one SIZE and RETR of the full path, no CWD
            self.assertEqual(record["app"]["reads"][name], {"state": "read", "method": "nocwd", "rc": [9, 0]}, name)
            self.assertIn(f"RETR ux0:/app/VJOSET001/{name}", log)
        self.assertEqual(record["app"]["eboot"]["sha256"], hashlib.sha256(self.EBOOT).hexdigest())
        self.assertEqual(record["files"]["ux0:/app/VJOSET001/vjo_app.txt"]["bytes"], len(self.STAMP) + 1)
        self.assertNotIn("folder", record["app"])  # a file was read: no folder is listed for it
        self.assertIn(f"Rubi app: the plugins' ({self.STAMP}); its eboot.bin is the package's; read by its full "
                      "path, no CWD, after the way through its folder was answered 'not there': eboot.bin (curl exit 9), "
                      "vjo_app.txt (curl exit 9)", analysis)
        self.assertNotIn("not on the Vita: ux0:/app", analysis)
        # --check-app on the same Vita: the same answer
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        flat = " ".join(check.stdout.replace("|", " ").split())
        self.assertIn("Rubi app: up to date", flat)
        self.assertIn("Its eboot.bin is the package VPK's.", flat)

    def test_an_app_that_cannot_be_read_is_never_called_not_installed(self):
        """Nothing under ux0:/app/VJOSET001 can be read either way (a hypothetical worse case than test.3's), but
        ux0:app lists the folder: the collect says it could not read it and, when the Shell checked the app at a
        game start of this boot, gives the Shell's answer. An earlier boot's check is not this boot's."""
        self.stamp_package()
        self.device_app(self.EBOOT, self.STAMP)
        (self.device / "ux0/app/PCSG00415").mkdir(parents=True)
        refused = {cmd: (lambda arg: "VJOSET001/" in arg or arg.endswith("VJOSET001")) for cmd in ("CWD", "SIZE", "RETR")}
        data = self.device / "ux0/data/VitaJPOverlay"
        check_line = (f"overlay app ok: installed '{self.STAMP}' (0x00000026, app 1), plugins' '{self.STAMP}' "
                      "(0x00000026)\n")
        boot = "Vita JP Overlay shell started (kernel API 22)\n"
        start = "game PCSG00415 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto\n"
        log = (data / "log.txt").read_text()
        cases = ((log + boot + start + check_line,
                  f"Rubi app: the plugins' ({self.STAMP}), by the Shell's check at a game start of this boot "
                  f"(log.txt:{log.count(chr(10)) + 3}); this collect could not read it over FTP: ux0:app lists VJOSET001, "
                  "but its folder could not be entered and neither its vjo_app.txt nor its eboot.bin could be read by "
                  "its full path (#138)"),
                 (log + check_line + boot,
                  "Rubi app: not checked: this collect could not read it over FTP (ux0:app lists VJOSET001, but its "
                  "folder could not be entered and neither its vjo_app.txt nor its eboot.bin could be read by its full "
                  "path), and the Shell logged no check of it in this boot; if the Rubi app is open beside VitaShell, "
                  "close it from its LiveArea page and collect again"))
        for text_log, text in cases:
            with self.subTest(text=text):
                (data / "log.txt").write_text(text_log)
                with FtpMock(self.device, fail=refused) as ftp:
                    result = self.collect(ftp)
                    writes = list(ftp.stored)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(writes, [])
                folder, record, analysis = self.latest()
                app = record["app"]
                for name in ("vjo_app.txt", "eboot.bin"):
                    # through the folder (curl 9, its CWD refused), then by its full path (curl 78, SIZE refused)
                    self.assertEqual(app["reads"][name], {"state": "absent", "method": None, "rc": [9, 78]}, name)
                self.assertEqual((app["folder"], app["apps"], app["in_apps"]), ("missing", "ok", True))
                self.assertNotIn("ux0:/app", record["listings"])
                self.assertNotIn(b"PCSG00415", (folder / "COLLECT.json").read_bytes())  # only VJOSET001 is kept
                self.assertIn(text, analysis)
                self.assertNotIn("not installed", analysis.split("Rubi app:", 1)[1].split("\n", 1)[0])
                shutil.rmtree(folder)
                Path(str(folder) + ".zip").unlink()

    def test_a_broken_transfer_is_an_error_for_both_tools_never_not_installed(self):
        """The folder's CWD refused (curl 9), then the full path's SIZE answered by a closed connection (curl 56): an
        error, never an absent file. --collect records it and says it could not read the app; --check-app cannot
        tell (exit 2) and says which file it could not read."""
        self.stamp_package()
        self.device_app(self.EBOOT, self.STAMP)
        with FtpMock(self.device, fail={"CWD": lambda arg: "VJOSET001" in arg},
                     drop={"SIZE": lambda arg: "VJOSET001/" in arg}) as ftp:
            result = self.collect(ftp)
            check = self.check_app(ftp)
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(writes, [])
        folder, record, analysis = self.latest()
        for name in ("vjo_app.txt", "eboot.bin"):
            r = record["app"]["reads"][name]
            self.assertEqual((r["state"], r["method"], r["rc"][0], len(r["rc"])), ("error", "nocwd", 9, 2), r)
            self.assertNotIn(r["rc"][1], (0, 9, 19, 78), r)
        self.assertTrue(any(e.startswith("ux0:/app/VJOSET001/vjo_app.txt: curl exit ") for e in record["errors"]),
                        record["errors"])
        self.assertTrue(any(e.startswith("ux0:/app/VJOSET001/eboot.bin: not hashed, curl exit ") for e in record["errors"]),
                        record["errors"])
        line = next(row for row in analysis.splitlines() if row.startswith("  Rubi app:"))
        self.assertTrue(line.startswith("  Rubi app: not checked: this collect could not read it over FTP (reading it "
                                        "failed: vjo_app.txt, curl exit "), line)
        self.assertIn("), and the Shell logged no check of it in this boot", line)
        self.assertEqual(check.returncode, 2, check.stdout + check.stderr)
        self.assertIn("Cannot read ux0:app/VJOSET001/vjo_app.txt from 127.0.0.1:", check.stdout)
        self.assertNotIn("No Rubi app is installed", check.stdout)

    def test_read_app_tries_the_full_path_only_after_not_there(self):
        """collect_vita.py read_app, as install_ftp.sh's app_check: the second way (no CWD) only when the first is
        answered "not there" (curl 9, 19, 78); any other exit (no connection, a timeout, a broken transfer) is an
        error, never an absent file, and no second way is tried after it."""
        sys.path.insert(0, str(ROOT / "tools"))
        import collect_vita
        cases = (([0], {"state": "read", "method": "cwd", "rc": [0]}),
                 ([9, 0], {"state": "read", "method": "nocwd", "rc": [9, 0]}),
                 ([78, 0], {"state": "read", "method": "nocwd", "rc": [78, 0]}),
                 ([9, 78], {"state": "absent", "method": None, "rc": [9, 78]}),
                 ([19, 19], {"state": "absent", "method": None, "rc": [19, 19]}),
                 ([28], {"state": "error", "method": "cwd", "rc": [28], "error": "curl: (28) timed out"}),
                 ([9, 7], {"state": "error", "method": "nocwd", "rc": [9, 7], "error": "curl: (7) timed out"}))
        for exits, want in cases:
            with self.subTest(exits=exits):
                c = collect_vita.Collector("127.0.0.1", 1, self.root / "out")
                calls, left = [], list(exits)

                def curl(device_path, dest, *options, calls=calls, left=left):
                    calls.append((device_path, options))
                    rc = left.pop(0)
                    return rc, (f"curl: ({rc}) timed out" if rc else "")
                c.curl = curl
                self.assertEqual(c.read_app(collect_vita.APP_EBOOT, self.root / "eboot.bin"), want)
                self.assertEqual(calls, [(collect_vita.APP_EBOOT, ()), (collect_vita.APP_EBOOT, ("--ftp-method", "nocwd"))]
                                 [:len(exits)])

    def test_test3s_collect_names_the_app_the_shell_checked(self):
        """The owner's test.3 collect (#138, vjo-collect-20261016-071743Z.zip, SHA-256 289ec0f9...): its COLLECT.json's
        app record, missing list and package app, the plugins' record, and its log.txt's boot, game start, Rubi app
        check and app start lines, verbatim (tools/tests/fixtures/collect-test3-app/; no game text, the host replaced by
        the docs' example address). Its collect_vita.py could not enter ux0:/app/VJOSET001; the Shell's check at this
        boot's first game start had read the stamp. The analysis gives the Shell's answer, not "not installed", and
        leaves the app's paths out of "not on the Vita"."""
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "vjo-collect-20261016-071743Z"
            shutil.copytree(FIXTURES / "collect-test3-app", folder)
            r = subprocess.run([sys.executable, str(ROOT / "tools/analyse_selftest.py"), "--collect", str(folder)],
                               capture_output=True, text=True, timeout=60)
            j = subprocess.run([sys.executable, str(ROOT / "tools/analyse_selftest.py"), "--collect", str(folder),
                                "--json"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("  not on the Vita: ux0:/data/VitaJPOverlay/log.old.txt, ux0:/data/VitaJPOverlay/selftest.txt, "
                      "ux0:/tai/config.txt, ux0:/tai/config.txt.vjo-bak\n", r.stdout)
        # a collect from after #367 (it fetches hooks.log) of a build that wrote none: named as the debug build's
        record = json.loads((FIXTURES / "collect-test3-app/COLLECT.json").read_text())
        record["missing"].append("ux0:/data/VitaJPOverlay/hooks.log")
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "vjo-collect-20261016-071743Z"
            shutil.copytree(FIXTURES / "collect-test3-app", folder)
            (folder / "COLLECT.json").write_text(json.dumps(record))
            r = subprocess.run([sys.executable, str(ROOT / "tools/analyse_selftest.py"), "--collect", str(folder)],
                               capture_output=True, text=True, timeout=60)
        self.assertIn("ux0:/tai/config.txt.vjo-bak, ux0:/data/VitaJPOverlay/hooks.log (the debug build's)\n", r.stdout)
        self.assertIn("  Rubi app: the plugins' (vjoapp1 api=22 build=c3e781ad58a5ebe3), by the Shell's check at a "
                      "game start of this boot (log.txt:5); this collect could not read it over FTP: its folder could "
                      "not be entered, and this older collect_vita.py read the app only through it (#138)\n", r.stdout)
        self.assertNotIn("not installed", r.stdout)
        self.assertIn("the Shell's Rubi app check: ok x1", r.stdout)
        app = json.loads(j.stdout)["app"]
        self.assertEqual((app["state"], app["source"], app["shell"]["where"]), ("ok", "shell", "log.txt:5"))

    def test_config_warnings_are_counted_once_each(self):
        log = (self.device / "ux0/data/VitaJPOverlay/log.txt").read_text()
        log += "config warning: unknown setting 'ocr_backend'\n" * 3 + "config warning: unknown setting 'meiki_layout'\n"
        log += ("overlay app MISMATCH: installed '' (0x80010002, app 1), plugins' 'vjoapp1 api=21 build=0123456789abcdef'"
                " (0x00000025)\n")
        # the next boot's own config load comes before its "shell started" line (shell/worker.c vjo_worker_start)
        log += "config warning: unknown setting 'ocr_settle_ms'\nVita JP Overlay shell started (kernel API 20)\n"
        (self.device / "ux0/data/VitaJPOverlay/log.txt").write_text(log)
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        analysis = (folder / "analysis.txt").read_text()
        self.assertIn("config warnings: 6 line(s), 4 distinct: unknown setting 'ocr_backend' x3; ", analysis)
        self.assertIn("    logged by builds of API 19: 5, 20: 1\n", analysis)
        self.assertIn("the Shell's Rubi app check: MISMATCH x1", analysis)

    def test_a_normal_build_package_is_named_and_its_self_test_note(self):
        """docs/release-variants.md: the analysis names the package's build; --self-test from the normal
        build's package says it has no self-test, and arms the marker for a -debug build anyway."""
        manifest = json.loads((self.package / "package-manifest.json").read_text())
        manifest["variant"] = "normal"
        (self.package / "package-manifest.json").write_text(json.dumps(manifest, indent=2))
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            armed = subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--self-test",
                                    f"127.0.0.1:{ftp.port}"], capture_output=True, text=True, timeout=60, cwd=self.root)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        self.assertIn("  package 0.8.0-test.1, the normal build (a short log; no self-test) source aaaaaaaaaaaa;",
                      (folder / "analysis.txt").read_text())
        self.assertEqual(json.loads((folder / "COLLECT.json").read_text())["package"]["variant"], "normal")
        self.assertEqual(armed.returncode, 0, armed.stdout + armed.stderr)
        self.assertIn("this package is the normal build, which has no self-test", armed.stdout)
        self.assertEqual((self.device / "ux0/data/VitaJPOverlay/selftest.txt").read_bytes(), b"")
        # the debug build's package: named, and no note
        manifest["variant"] = "debug"
        (self.package / "package-manifest.json").write_text(json.dumps(manifest, indent=2))
        with FtpMock(self.device) as ftp:
            armed = subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--self-test",
                                    f"127.0.0.1:{ftp.port}"], capture_output=True, text=True, timeout=60, cwd=self.root)
        self.assertEqual(armed.returncode, 0, armed.stdout + armed.stderr)
        self.assertNotIn("normal build", armed.stdout)

    def test_the_normal_builds_short_log_is_read(self):
        """docs/release-variants.md: the normal build's log has no job, step or timing lines; the analysis reads its
        own: each activation's first jobs and every failed one, the clock line, a refused PAF request, the self-test note,
        its boots (the config warnings' API), and its kernel.txt, which logs no job (audit-overlay-release-variants B1)."""
        data = self.device / "ux0/data/VitaJPOverlay"
        (data / "log.old.txt").unlink()
        (data / "selftest-report.txt").unlink()
        (data / "log.txt").write_text(
            "bootstrap: shell API 22 entered\n"
            "config warning: ocr_backend: VOCR2 was removed; using vocr\n"
            "Vita JP Overlay shell started (kernel API 22, the normal build: a short log)\n"
            "self-test: armed, but this is the normal build: install the -debug build to run it\n"
            "game PCSG00415 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto\n"
            "ocr rc=-117 VOCR3_H15_w8.vocr: 0 lines, 1003 ms\n"
            "ocr rc=-117 VOCR3_H15_w8.vocr: 0 lines, 1002 ms\n"
            "ocr rc=-108 VOCR3_H15_w8.vocr: 0 lines, 40 ms\n"
            "tier large did not fit in PCSG00415 (need 18649088, free 12000000); using normal\n"
            "ocr rc=0 VOCR3_H15_w8.vocr: 3 lines, 2100 ms\n"
            "clock: 333 -> 444 MHz while reading, 333 after\n"
            "game PCSE00001 started: dictionary Local dictionaries, trigger l+r, subtitles select+r, ocr_mode auto\n"
            "ocr rc=0 VOCR3_FL10_w8.vocr: 2 lines, 900 ms\n"
            "clock: 444 MHz refused 0x80010016, 333 MHz while reading\n"
            "overlay text Paf allocation refused: free=1500000 requested=20480\n"
            "lookup cursor at 3 failed: rc=-113\n"
            "anki: save rc=0, 12 ms\n"
            "anki: save rc=-110, 9 ms\n", encoding="utf-8")
        (data / "kernel.txt").write_text("Vita JP Overlay kernel 22 started (normal mode, the normal build: a short log)\n"
                                         "hooks: display 0, input 0, procevent 0 (0 = ok)\n"
                                         "ocr worker registered: game pid 0x20000 module 0x30000\n")
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        a = (folder / "analysis.txt").read_text()
        self.assertIn("1 of its 1 boot(s) the normal build's (a short log", a)
        self.assertIn("  OCR jobs in the normal build's short log (per activation of a game, a start or a resume: its first "
                      "job that read its screen and its first that found a line; and every failed one, not a cancelled one or one whose "
                      "screen changed; not the others): 5; ", a)
        self.assertRegex(a, r"rc -117\)? x2")
        self.assertRegex(a, r"rc -108\)? x1")
        self.assertIn("models VOCR3_H15_w8.vocr x4, VOCR3_FL10_w8.vocr x1; read: 2-3 lines in 900-2,100 ms in the game", a)
        self.assertIn("  ARM clock (ocr_boost_clock), the normal build's lines (each activation's first job that recognized "
                      "a line, and every change): 2; "
                      "raised in 1, left at 444 or more in 0, refused in 1 (0x80010016", a)
        self.assertIn("MHz before 333-333, during 333-444, after 333-333", a)
        self.assertIn("self-test: armed, but the normal build has none (1 boot(s) said so)", a)
        self.assertIn("OCR jobs: not logged by the normal build", a)
        self.assertIn("  ARM clock by game window: the normal build's short log (1 boot) logs no job's line or steps", a)
        self.assertNotIn("OCR jobs submitted 0", a)
        self.assertIn("    logged by builds of API 22: 1", a)  # its boot's warning, before its start line
        problems = a.split("lines reporting a problem")[1]
        for line in ("ocr rc=-N VOCRN_HN_wN.vocr: N lines, N ms", "clock: N MHz refused", "Paf allocation refused",
                     "lookup cursor at N failed"):
            self.assertIn(line, problems)
        self.assertIn("model sizes that did not fit (log only, never shown; the normal build's boots log each once per "
                      "activation and on change, so its counts are lines, not jobs): large in PCSG00415 x1", a)
        self.assertIn("  Anki: 2 save(s) (1 failed)", a)  # the normal build's save lines name no word
        record = json.loads((folder / "analysis.json").read_text())
        self.assertEqual(len(record["log"]["short"]["jobs"]), 5)
        self.assertTrue(record["log"]["shell_refusals"])
        # the timeline says it has nothing to time, rather than 0 jobs
        self.assertIn("the normal build's short log", (folder / "timeline.txt").read_text())

    def test_collect_without_a_report_or_logs_still_zips_and_says_why(self):
        data = self.device / "ux0/data/VitaJPOverlay"
        for name in ("selftest-report.txt", "log.txt", "log.old.txt"):
            (data / name).unlink()
        (data / "selftest.txt").write_text("")
        with FtpMock(self.device, fail={"LIST": lambda arg: True}) as ftp:  # listings refused: files still come
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        self.assertTrue(Path(str(folder) + ".zip").is_file())
        analysis = (folder / "analysis.txt").read_text()
        self.assertIn("self-test: no report (selftest.txt is there: armed, and it has not run yet)", analysis)
        self.assertIn("log.txt: (none: log_file off, or nothing logged)", analysis)
        self.assertIn("LIST failed", analysis)
        self.assertTrue((folder / "ur0/tai/config.txt").is_file())

    def test_a_directory_that_is_not_there_is_missing_not_an_error(self):
        # before the first × there is no anki_queue/, and a Vita without crashes has no dumps
        shutil.rmtree(self.device / "ux0/data/VitaJPOverlay/anki_queue")
        for dump in (self.device / "ux0/data").glob("*.psp2dmp"):
            dump.unlink()
        (self.device / "ur0/data/VitaJPOverlay/boot.txt").write_text("")  # what the installer leaves
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        record = json.loads((folder / "COLLECT.json").read_text())
        self.assertIn("ux0:/data/VitaJPOverlay/anki_queue/", record["missing"])
        self.assertEqual(record["errors"], [])
        analysis = (folder / "analysis.txt").read_text()
        self.assertIn("anki_queue on the card: 0 card(s)", analysis)
        self.assertIn("crash dumps: none", analysis)
        self.assertIn("boot.txt: (empty: the installer reset it", analysis)

    def test_names_from_the_server_never_leave_the_folder(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import collect_vita
        for bad in ("..", ".", "../x.json", "a/b.json", "a\\b.json", ""):
            self.assertFalse(collect_vita.safe_name(bad), bad)
        for ok in ("0123456789abcdef0123456789abcdef.json", "a  b#c%20d?.json", "psp2core-1-x.psp2dmp"):
            self.assertTrue(collect_vita.safe_name(ok), ok)
        entries = collect_vita.Collector.parse_list(["-rw-rw-rw- 1 vita vita 12 Oct 5  10:00 a  b.json\r",
                                                     "drwxrwxrwx 1 vita vita 0 Oct 14 10:00 anki_queue"])
        self.assertEqual(entries, [{"name": "a  b.json", "bytes": 12, "dir": False, "date": "Oct 5 10:00"},
                                   {"name": "anki_queue", "bytes": 0, "dir": True, "date": "Oct 14 10:00"}])
        text, keys = collect_vita.redact("\ufeffjpdb_api_key = abc\r\njpdb_api_key: d e\r\nlens_api_keyx = keep\r\n")
        self.assertEqual(text, "\ufeffjpdb_api_key = <redacted: 3 characters>\r\njpdb_api_key: <redacted: 3 characters>\r\n"
                               "lens_api_keyx = keep\r\n")
        self.assertEqual(keys, ["jpdb_api_key", "jpdb_api_key"])

    def test_commented_and_mistyped_removed_keys_are_redacted(self):
        # audit N3: a commented-out key, a key glued to its name and a longer name that starts with one
        # (the parser and the installer drop such lines); an ordinary comment and lens_api_keyx stay
        sys.path.insert(0, str(ROOT / "tools"))
        import collect_vita
        text, keys = collect_vita.redact("; jpdb_api_key = KEY1\n#jiten_api_key=KEY2\njpdb_api_keyKEY3\n"
                                         "jpdb_api_key_old = KEY4\napi_keys = KEY5\n; a comment\nlens_api_keyx = keep\n"
                                         "; jpdb.io API key (needed for dictionary = jpdb): jpdb.io -> Settings\n")
        for secret in ("KEY1", "KEY2", "KEY3", "KEY4", "KEY5"):
            self.assertNotIn(secret, text)
        self.assertEqual(text.splitlines()[:3], ["; jpdb_api_key = <redacted: 4 characters>",
                                                 "#jiten_api_key=<redacted: 4 characters>",
                                                 "jpdb_api_key<redacted: 4 characters>"])
        self.assertIn("; a comment\nlens_api_keyx = keep\n; jpdb.io API key (needed for dictionary = jpdb)", text)
        self.assertEqual(keys, ["jpdb_api_key", "jiten_api_key", "jpdb_api_key", "jpdb_api_key", "api_key"])
        # an older build's warning quoted a line without "=" whole
        data, n = collect_vita.redact_log(b"config warning: ignored line 'jpdb_api_keyKEY6'\nunknown setting 'lens_api_keyx'\n")
        self.assertNotIn(b"KEY6", data)
        self.assertIn(b"ignored line 'jpdb_api_key <redacted>'", data)
        self.assertIn(b"unknown setting 'lens_api_keyx'", data)
    def test_no_credential_reaches_the_zip(self):
        """audit-overlay-test3-cleanup B1: the zip holds no credential's value. The audio source's URL (it can
        carry a key) is redacted in config.ini and in the logs' config warnings like an API key, and the
        package's defaults and install settings (an older package's manifest holds the values) say only
        whether each is set, in COLLECT.json, analysis.txt and analysis.json."""
        url_secret, pkg_key, pkg_url = "url-secret-4711", "pkg-key-secret-1", "https://p.invalid/x?k=pkg-url-secret-2"
        data = self.device / "ux0/data/VitaJPOverlay"
        url = f"https://audio.invalid/a?term={{term}}&key={url_secret}"
        (data / "config.ini").write_text(CONFIG + f"anki_audio_url = {url}\r\n", encoding="utf-8")
        log = (data / "log.txt").read_text(encoding="utf-8")
        (data / "log.txt").write_text(log + f"config warning: ignored line 'anki_audio_url {url}'\n", encoding="utf-8")
        manifest = json.loads((self.package / "package-manifest.json").read_text())
        manifest["defaults"] = {"anki_mining": True, "jpdb_api_key": pkg_key, "jiten_api_key": "",
                                "anki_audio_url": pkg_url, "ocr_lm": "1000k"}
        manifest["install_settings"] = {"anki_mining": "on", "jpdb_api_key": pkg_key, "anki_audio_url": pkg_url}
        (self.package / "package-manifest.json").write_text(json.dumps(manifest))
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        with zipfile.ZipFile(Path(str(folder) + ".zip")) as z:
            blob = b"".join(z.read(n) for n in z.namelist())
        for secret in (SECRET, SECRET2, url_secret, "audio.invalid", pkg_key, "pkg-url-secret-2", "p.invalid"):
            self.assertNotIn(secret.encode(), blob, secret)
            self.assertNotIn(secret, result.stdout + result.stderr, secret)
        config = (folder / "ux0/data/VitaJPOverlay/config.ini").read_bytes().decode()
        self.assertIn(f"anki_audio_url = <redacted: {len(url)} characters>\r\n", config)
        self.assertIn("config warning: ignored line 'anki_audio_url <redacted>'",
                      (folder / "ux0/data/VitaJPOverlay/log.txt").read_text(encoding="utf-8"))
        record = json.loads((folder / "COLLECT.json").read_text())
        self.assertEqual(record["redacted_keys"][-1], "anki_audio_url")
        self.assertEqual(record["package"]["defaults"], {"anki_mining": True, "jpdb_api_key": "set", "jiten_api_key": "",
                                                         "anki_audio_url": "set", "ocr_lm": "1000k"})
        self.assertEqual(record["package"]["install_settings"], {"anki_mining": "on", "jpdb_api_key": "set",
                                                                 "anki_audio_url": "set"})
        self.assertIn("install settings {'anki_mining': 'on', 'jpdb_api_key': 'set', 'anki_audio_url': 'set'}",
                      (folder / "analysis.txt").read_text())
        self.assertEqual(json.loads((folder / "analysis.json").read_text())["record"]["package"]["install_settings"],
                         {"anki_mining": "on", "jpdb_api_key": "set", "anki_audio_url": "set"})
        # a COLLECT.json written before the masking (by an older collect_vita.py) is masked by the analyser
        raw = dict(record, package=manifest)
        (folder / "COLLECT.json").write_text(json.dumps(raw))
        r = subprocess.run([sys.executable, str(self.package / "tools/analyse_selftest.py"), "--collect", str(folder),
                            "--json"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn(pkg_key, r.stdout)
        self.assertNotIn("pkg-url-secret-2", r.stdout)
        self.assertEqual(json.loads(r.stdout)["record"]["package"]["defaults"]["anki_audio_url"], "set")

    def test_an_empty_report_is_a_stop_not_a_pass(self):
        (self.device / "ux0/data/VitaJPOverlay/selftest-report.txt").write_text("")
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("analysis exit 1", result.stdout)
        self.assertIn("the report is empty", result.stdout)

    def test_collect_cannot_reach_the_vita(self):
        with FtpMock(self.device) as ftp:
            port = ftp.port
        result = subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--collect", f"127.0.0.1:{port}"],
                                capture_output=True, text=True, timeout=60, cwd=self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("press SELECT to start FTP", result.stdout)
        # a Vita's VitaShell starts USB on SELECT until its SELECT button setting is FTP (#136)
        self.assertIn("SELECT button set to FTP in VitaShell's settings", result.stdout)
        self.assertIn("a Vita's default is USB", result.stdout)

    def test_the_lms_and_their_attribution_are_hashed_against_the_package(self):
        # The beam + LM decoder's LMs and their CC BY-SA attribution ship beside the weights (#89).
        # Each installed one is hashed and matches its manifest entry, a shipped one the Vita lacks
        # is absent, and none of them is kept in the zip.
        ocr, dev_ocr = "ux0/data/VitaJPOverlay/ocr/", "ux0:/data/VitaJPOverlay/ocr/"
        manifest = json.loads((self.package / "package-manifest.json").read_text())
        for name, body in (("vocr_lm6_o3_1000k.vlm", b"lm 1000k"), ("vocr_lm6_o3_4000k.vlm", b"lm 4000k"),
                           ("LM-ATTRIBUTION.md", b"# attribution"), ("MODEL-ATTRIBUTION.md", b"# the models'"),
                           ("VOCR4_W20_w8.vocr", b"w20 weights")):
            (self.device / (ocr + name)).write_bytes(body)
            manifest["files"][ocr + name] = {"bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()}
        manifest["files"][ocr + "vocr_lm_o3_500k.vlm"] = {"bytes": 2, "sha256": hashlib.sha256(b"lm").hexdigest()}
        (self.package / "package-manifest.json").write_text(json.dumps(manifest))
        # names the package never ships there: neither hashed nor read
        others = ("LM-ATTRIBUTION.md.bak", "MODEL-ATTRIBUTION.md.old", "LM-notes.txt", "meiki-stream-int8.mnn", "old.vlm.txt",
                  "VOCR3_H15_w8.vocr.part")
        for name in others:
            (self.device / (ocr + name)).write_bytes(b"not the package's")
        with FtpMock(self.device) as ftp:
            result = self.collect(ftp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        folder = next(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())
        record = json.loads((folder / "COLLECT.json").read_text())
        check = record["package_check"]
        for name in ("vocr_lm6_o3_1000k.vlm", "vocr_lm6_o3_4000k.vlm", "LM-ATTRIBUTION.md", "MODEL-ATTRIBUTION.md",
                     "VOCR4_W20_w8.vocr"):
            self.assertIn(dev_ocr + name, record["hashed"])
            self.assertTrue(check[dev_ocr + name]["match"], name)
        for name in others:
            self.assertNotIn(dev_ocr + name, record["hashed"])
        self.assertFalse(check[dev_ocr + "vocr_lm_o3_500k.vlm"]["match"])
        self.assertTrue(check[dev_ocr + "vocr_lm_o3_500k.vlm"].get("absent"))
        with zipfile.ZipFile(Path(str(folder) + ".zip")) as z:
            self.assertEqual([n for n in z.namelist() if n.endswith((".vocr", ".vlm", "-ATTRIBUTION.md"))], [])
        # setUp's 8 package files (VOCR3-FL10's differs) and these 6; VOCR2 is not in the package
        self.assertIn("installed files: 12 of 14 match the package; DIFFER or absent: "
                      "ux0:/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr, ux0:/data/VitaJPOverlay/ocr/vocr_lm_o3_500k.vlm; "
                      "not in this package: ux0:/data/JPOverlaySettings.vpk, ux0:/data/VitaJPOverlay/ocr/VOCR2_FL10_w8.vocr",
                      (folder / "analysis.txt").read_text())


class InstallRecord(unittest.TestCase):
    """The analysis of the one-VPK install's record (analyse_selftest.summarise_install): absent, another
    package's, not a record, and the taiHEN line of a configuration with no worker line (#138)."""

    def test_each_record(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import analyse_selftest as a
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            # a collect from before installed.txt was fetched says nothing of it; one that asked says it is absent
            self.assertEqual(a.render_install(a.summarise_install(folder, {})), [])
            self.assertEqual(a.render_install(a.summarise_install(
                folder, {"missing": ["ux0:/data/VitaJPOverlay/installed.txt"]})),
                ["  one-VPK install: no installed.txt (neither the Rubi app's Install page nor this install-vita.sh "
                 "has installed this Vita's overlay: an older installer did, or none)"])
            (folder / "ux0/data/VitaJPOverlay").mkdir(parents=True)
            record = folder / "ux0/data/VitaJPOverlay/installed.txt"
            record.write_text("vjoinst1 version=0.8.1 variant=normal api=22 build=fedcba9876543210 manifest=" + "a" * 64 + "\n")
            lines = a.render_install(a.summarise_install(folder, {"package_payload_manifest": "b" * 64}))
            self.assertEqual(lines, ["  one-VPK install (installed.txt): v0.8.1, the normal build, API 22, app build "
                                     "fedcba9876543210; NOT this package's payload (another build's VPK was installed)"])
            self.assertIn("not compared", a.render_install(a.summarise_install(folder, {}))[0])
            record.write_text("vjoinst0 garbage\n")
            self.assertIn("installed.txt is not a record ('vjoinst0 garbage')",
                          a.render_install(a.summarise_install(folder, {}))[0])
            (folder / "ur0/tai").mkdir(parents=True)
            (folder / "ur0/tai/config.txt").write_text("*KERNEL\nur0:tai/VitaJPOverlay_Kernel.skprx\n*main\n"
                                                       "ur0:tai/VitaJPOverlay_Shell.suprx\n")
            (folder / "ur0/tai/config.txt.vjo-20261016-120000-2.bak").write_text("*KERNEL\n")
            summary = a.summarise_collect(folder)
            text = a.render_collect(summary)
            self.assertIn("ur0:tai/config.txt: kernel under ['KERNEL'], shell under ['main'], no OCR worker line "
                          "(the kernel loads it); 0 commented line(s)", text)
            self.assertIn("taiHEN backups: ur0:tai/config.txt.vjo-20261016-120000-2.bak", text)


if __name__ == "__main__":
    unittest.main()
