"""install_ftp.sh --collect end to end: the real script, curl and ftplib against a VitaShell-like FTP
server (ftp_mock.py) over a fake device tree. Everything is fetched into one timestamped folder and
zip, config.ini's API keys never reach the folder, the Vita is never written, and the analyser
summarises the logs, the self-test, the tiers and what it cannot decide."""
import hashlib
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

LOG = """bootstrap: shell API 19 entered
bootstrap: kernel resident, installing Paf callback
Vita JP Overlay shell started (kernel API 19)
bootstrap: worker started
game PCSG00415 started: dictionary local (key MISSING), trigger l+r, subtitles select+r, ocr_mode auto
job start (auto: screen settled)
game vocr rc=0 VOCR3_H15_w8.vocr: 917x125 at 22,400 of 960x544 fmt 0x00000000, lines=3 USER workspace=188416 weights=read resident=1159168, find=9 ms load=1050 ms recognize=820 ms frames=330 io=0x00000000 stat=0x00000000 cleanup=0
game vocr capture: hook into USER, 61440 B block, 62-row strips, 9 strips in 34 ms, 0 rows changed, sum 0x5EED0003
game vocr memory: free USER=91226112, needs workspace 249856 + weights 1159168 + reserve 4194304: check passed, headroom 85622784
game vocr speed: 330 frames in 820000 us, 2484 us per frame
game vocr lines: 877x30 110f 273.3ms 870x30 110f 273.3ms 860x30 110f 273.3ms
gstep job alloc kind=user arg=capture rc=0x00000041 bytes=61440 sum=0x00000000 main=91164672 cdram=117440512 phycont=0 us=30
job 0 done rc=0 in 2100 ms, arena peak 4096, dictionary page 1/1
game vocr rc=-103 VOCR3_FL10_w8.vocr (fallback, refused too): 917x125 at 22,400 of 960x544 fmt 0x00000000, lines=0 USER workspace=0 weights=none resident=0, find=0 ms load=0 ms recognize=0 ms frames=0 io=0x00000000 stat=0x00000000 cleanup=0
game vocr memory: free USER=5000000, needs workspace 245760 + weights 659456 + reserve 4194304: check refused, headroom -99520
gstep job alloc kind=user arg=workspace rc=0x80024302 bytes=188416 sum=0x00000000 main=5000000 cdram=0 phycont=0 us=40
overlay closed: 120 frames, 3 word moves, stick 0, slowest frame 33 ms
anki: Picture field off in this build: no screenshot, no kernel capture buffer
anki: save 猫 rc=0, 0-byte screenshot, 45 ms
anki: queue 1 card(s)
"""
KERNEL = """Vita JP Overlay kernel 19 started (normal mode)
hooks: display 0, input 0, procevent 0 (0 = ok)
shell registered: pid 0x10000 (was 0x0)
game pid 0x20000 active
ocr worker registered: game pid 0x20000 module 0x30000
ocr job 1: 917x125 at 22,400 of 960x544, capture 1, flags 0x0, VOCR3_H15_w8.vocr / VOCR3_FL10_w8.vocr
"""
CONFIG = (f"; settings\nocr_backend = vocr\nvocr_model = VOCR3_H15_w8.vocr\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n"
          f"ocr_mode = auto\nlog_file = on\nanki_mining = on\nanki_host =\njpdb_api_key = {SECRET}\njiten_api_key =\n")
TAI = (b"*KERNEL\r\nur0:tai/VitaJPOverlay_Kernel.skprx\r\n*main\r\nur0:tai/VitaJPOverlay_Shell.suprx\r\n*ALL\r\n"
       b"# Mainline OCR: ur0:tai/VitaJPOverlay_OCR.suprx\r\n*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n")


class Collect(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.package = self.root / "package"
        (self.package / "tools").mkdir(parents=True)
        for name in ("install_ftp.sh", "collect_vita.py", "analyse_selftest.py", "package_release.py", "prepare_ocr.py",
                     "prepare_plaintext.py", "migrate_config.py", "patch_tai_config.py"):
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
        (data / "status.txt").write_text("bootstrap: shell API 19 entered\nbootstrap: worker started\n")
        shutil.copyfile(FIXTURES / "selftest-v2-pass.txt", data / "selftest-report.txt")
        (data / "config.ini").write_text(CONFIG, encoding="utf-8")
        (data / "games.ini").write_text("default = on\n")
        (data / "region.ini").write_text("PCSG00415 = 1536,48188,62600,15059\n")
        (data / "anki_host.txt").write_text("192.168.1.23:8765\n")
        (data / "anki_queue/0123456789abcdef0123456789abcdef.json").write_text('{"version":2,"spelling":"猫"}\n',
                                                                                encoding="utf-8")
        (data / "anki_queue/fedcba9876543210fedcba9876543210.json.tmp").write_text("{")
        (self.device / "ur0/data/VitaJPOverlay/boot.txt").write_text(
            "vjoboot1 seq=3 mode=0 stage=4 inert=0 fault=0 failures=0 api=19 prev=0/0/0/0\n")
        (self.device / "ur0/tai/config.txt").write_bytes(TAI)
        (self.device / "ur0/tai/config.txt.vjo-bak").write_bytes(b"*KERNEL\r\n")
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
        files["JPOverlaySettings.vpk"] = {"bytes": 3, "sha256": hashlib.sha256(b"vpk").hexdigest()}
        (self.device / "ux0/data/JPOverlaySettings.vpk").write_bytes(b"vpk")
        files["ux0/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"]["sha256"] = "0" * 64  # the device's differs
        (self.package / "package-manifest.json").write_text(json.dumps(
            {"version": "0.8.0-test.1", "source_revision": "a" * 40, "files": files,
             "install_settings": {"anki_mining": "on"}}))
        self.before = {p: p.read_bytes() for p in self.device.rglob("*") if p.is_file()}

    def collect(self, ftp):
        return subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--collect", f"127.0.0.1:{ftp.port}"],
                              capture_output=True, text=True, timeout=180, cwd=self.root)

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
        with zipfile.ZipFile(zpath) as z:
            names = set(z.namelist())
            blob = b"".join(z.read(n) for n in names)
        d = folder.name + "/ux0/data/VitaJPOverlay/"
        for rel in ("log.txt", "log.old.txt", "selftest-report.txt", "status.txt", "kernel.txt", "kernel.prev.txt",
                    "config.ini", "games.ini", "region.ini", "anki_host.txt",
                    "anki_queue/0123456789abcdef0123456789abcdef.json",
                    "anki_queue/fedcba9876543210fedcba9876543210.json.tmp"):
            self.assertIn(d + rel, names)
        for rel in ("ur0/data/VitaJPOverlay/boot.txt", "ur0/tai/config.txt", "ur0/tai/config.txt.vjo-bak",
                    "ux0/data/psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp", "COLLECT.json", "analysis.txt",
                    "analysis.json"):
            self.assertIn(folder.name + "/" + rel, names)
        self.assertTrue(any(n.startswith(folder.name + "/listings/") for n in names))
        # the API key is in no file of the zip; its length is
        self.assertNotIn(SECRET.encode(), blob)
        config = (folder / "ux0/data/VitaJPOverlay/config.ini").read_text()
        self.assertIn(f"jpdb_api_key = <redacted: {len(SECRET)} characters>\n", config)
        self.assertIn("jiten_api_key =\n", config)
        # the weights are hashed, never kept
        self.assertFalse(any(n.endswith(".vocr") for n in names))
        record = json.loads((folder / "COLLECT.json").read_text())
        self.assertEqual(record["redacted_keys"], ["jpdb_api_key"])
        self.assertIn("ux0:/data/VitaJPOverlay/selftest.txt", record["missing"])
        self.assertEqual(record["errors"], [])
        self.assertEqual(record["hashed"]["ux0:/data/VitaJPOverlay/ocr/VOCR3_H15_w8.vocr"]["sha256"],
                         hashlib.sha256(b"h15 weights").hexdigest())
        check = record["package_check"]
        self.assertTrue(check["ur0:/tai/VitaJPOverlay_Kernel.skprx"]["match"])
        self.assertFalse(check["ux0:/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"]["match"])
        self.assertTrue(check["ux0:/data/VitaJPOverlay/dictionaries/plaintext.vjdict"]["match"])
        self.assertTrue(check["ux0:/data/JPOverlaySettings.vpk"]["match"])
        # the analysis: the self-test, the logs, the tiers, and what it cannot decide
        analysis = (folder / "analysis.txt").read_text()
        for text in ("installed files: 7 of 8 match the package; DIFFER or absent: ux0:/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr",
                     "API keys redacted: jpdb_api_key",
                     "ur0:tai/config.txt: kernel under ['KERNEL'], shell under ['main'], OCR worker under ['PCSG00415']",
                     "boot.txt: vjoboot1 seq=3 mode=0 stage=4",
                     "worker registrations 1, OCR jobs submitted 1",
                     "OCR jobs: 2; rc out of memory (rc -103) x1, rc ok (rc 0) x1",
                     "models VOCR3_H15_w8.vocr x1, VOCR3_FL10_w8.vocr (fallback) x1",
                     "times (ms): find 9-9, load 1,050-1,050 (1 read), recognize 820-820; per line 273-273",
                     "capture: hook into USER x1",
                     "game memory: free USER 5,000,000-91,226,112 B; headroom -99,520-85,622,784 B; refused 1 of 2",
                     "game worker steps that failed: alloc user/workspace 0x80024302",
                     "overlay opened 1x, slowest frame 33 ms",
                     "Anki: 1 save(s) (0 failed), queue 1, 0 sent, Picture off in this build",
                     "anki_queue on the card: 1 card(s), 0 duplicate(s), 0 picture(s), 1 temp file(s); anki_host.txt 192.168.1.23:8765",
                     "crash dumps: psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp (4,096 B)",
                     "free game USER before the self-test's first block (selftest-report.txt): 91,226,112 B",
                     "recommendation: Large (the largest tier",
                     "free game USER the lowest any job in log.txt saw",
                     "recommendation: none: not even Small fits",
                     "recommendation for this game: none (not even Small fits) (the most conservative",
                     "PASS: every stage completed",
                     "a crash dump is on the card",
                     "log.txt rotated"):
            self.assertIn(text, analysis)
        # the lowest free USER any job saw (5,000,000 B, a refused job) is under Small's 4 MiB reserve +
        # 1,716,224 B first job: the game's recommendation is the most conservative sample's
        self.assertNotIn("key MISSING", analysis.split("lines reporting a problem")[-1].split("anki_queue")[0])
        summary = json.loads((folder / "analysis.json").read_text())
        self.assertEqual(summary["selftest"]["ocr_result"], "PASS")
        self.assertEqual([t["recommendation"] for t in summary["tiers_all"]], ["Large", None])
        self.assertIsNone(summary["tiers"]["recommendation"])
        self.assertEqual(summary["selftest"]["tiers"]["rows"][2]["need"], 1_716_224)

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

    def test_collect_cannot_reach_the_vita(self):
        with FtpMock(self.device) as ftp:
            port = ftp.port
        result = subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), "--collect", f"127.0.0.1:{port}"],
                                capture_output=True, text=True, timeout=60, cwd=self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("press SELECT to start FTP", result.stdout)


if __name__ == "__main__":
    unittest.main()
