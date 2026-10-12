"""Exercise the real package installer with a local filesystem FTP substitute."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ReleaseInstall(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "package"
        self.device = self.root / "device"
        for folder in ("tools", "ur0/tai", "ur0/data/VitaJPOverlay",
                       "ux0/data/VitaJPOverlay/ocr", "ux0/data/VitaJPOverlay/dictionaries"):
            (self.package / folder).mkdir(parents=True, exist_ok=True)
        for name in ("install_ftp.sh", "package_release.py", "prepare_ocr.py",
                     "migrate_config.py", "patch_tai_config.py"):
            shutil.copyfile(ROOT / "tools" / name, self.package / "tools" / name)
        shutil.copyfile(ROOT / "tools/install-vita.sh", self.package / "install-vita.sh")
        for name in ("config.ini", "ux0/data/VitaJPOverlay/config.ini"):
            shutil.copyfile(ROOT / "docs/config.example.ini", self.package / name)
        for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                     "ur0/data/VitaJPOverlay/vitajpoverlay.rco", "JPOverlaySettings.vpk",
                     "ur0/tai/VitaJPOverlay_OCR.suprx",
                     "ux0/data/VitaJPOverlay/ocr/FL10_w8.vocr", "ux0/data/VitaJPOverlay/ocr/H15_w8.vocr",
                     "ux0/data/VitaJPOverlay/dictionaries/jmdict.vjdict"):
            (self.package / name).write_bytes(("test payload " + name).encode())
        files = {str(p.relative_to(self.package)): {"bytes": p.stat().st_size,
                 "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in self.package.rglob("*") if p.is_file()}
        (self.package / "package-manifest.json").write_text(json.dumps({"files": files}))
        self.original_tai = b"*KERNEL\r\nur0:tai/Other.skprx\r\n*main\r\n*ALL\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n"
        for partition in ("ur0", "ux0"):
            (self.device / partition / "tai").mkdir(parents=True)
            (self.device / partition / "tai/config.txt").write_bytes(self.original_tai)
        self.data = self.device / "ux0/data/VitaJPOverlay"
        self.data.mkdir(parents=True)
        (self.data / "config.ini").write_text("dictionary = hachidori\nocr_backend = lens\nfont_size_ja = 24\njpdb_api_key = test-key\n")
        (self.data / "games.ini").write_text("keep existing choices")
        (self.data / "region.ini").write_text("keep existing region")
        binary = self.root / "bin"
        binary.mkdir()
        curl = binary / "curl"
        curl.write_text("""#!/usr/bin/env python3
import os, pathlib, shutil, sys
from urllib.parse import urlparse
a = sys.argv[1:]
p = urlparse(a[-1]).path.lstrip('/').replace(':', '')
remote = pathlib.Path(os.environ['FAKE_DEVICE']) / p
if '-T' in a:
    remote.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(a[a.index('-T') + 1], remote)
elif '-o' in a:
    if not remote.is_file(): sys.exit(22)
    out = a[a.index('-o') + 1]
    if out != '/dev/null':
        shutil.copyfile(remote, out)
        if os.environ.get('CORRUPT_READBACK') == p:
            pathlib.Path(out).write_bytes(b'corrupted transfer')
""")
        curl.chmod(0o755)
        self.environment = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"],
                                FAKE_DEVICE=str(self.device))
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(10)
        self.addCleanup(self.listener.close)

    def install(self, script="install-vita.sh"):
        return subprocess.run(["bash", str(self.package / script),
                               "127.0.0.1:" + str(self.listener.getsockname()[1])],
                              env=self.environment, capture_output=True, text=True, timeout=30)

    def test_update_is_verified_idempotent_and_preserves_unrelated_state(self):
        self.assertFalse((self.package / "docs/config.example.ini").exists())
        for _ in range(2):
            result = self.install()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("test-key", result.stdout)
            self.assertNotIn("migration failed", result.stdout + result.stderr)
        for partition in ("ur0", "ux0"):
            tai = (self.device / partition / "tai/config.txt").read_bytes()
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_Kernel.skprx"), 1)
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_Shell.suprx"), 1)
            self.assertIn(b"ur0:tai/Other.skprx\r\n", tai)
            self.assertIn(b"*ALL\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_OCR.suprx"), 1)
            self.assertNotIn(b"\n", tai.replace(b"\r\n", b""))
        config = (self.data / "config.ini").read_text()
        for setting in ("dictionary = local", "ocr_backend = vocr", "vocr_model = H15_w8.vocr",
                        "vocr_fallback_model = FL10_w8.vocr",
                        "local_dictionaries = jmdict.vjdict", "font_size_ja = 24", "jpdb_api_key = test-key"):
            self.assertIn(setting, config)
        self.assertEqual((self.data / "games.ini").read_text(), "keep existing choices")
        self.assertEqual((self.data / "region.ini").read_text(), "keep existing region")
        self.assertTrue((self.device / "ux0/data/JPOverlaySettings.vpk").is_file())
        self.assertEqual((self.device / "ur0/tai/VitaJPOverlay_OCR.suprx").read_bytes(),
                         (self.package / "ur0/tai/VitaJPOverlay_OCR.suprx").read_bytes())
        for name in ("dictionaries/jmdict.vjdict", "ocr/FL10_w8.vocr", "ocr/H15_w8.vocr"):
            self.assertEqual((self.data / name).read_bytes(),
                             (self.package / "ux0/data/VitaJPOverlay" / name).read_bytes())

    def test_source_install_uses_the_checkout_template(self):
        # A source checkout has its template under docs, not at the package root.
        (self.package / "config.ini").unlink()
        (self.package / "docs").mkdir()
        shutil.copyfile(ROOT / "docs/config.example.ini", self.package / "docs/config.example.ini")
        build = self.package / "build/vita"
        build.mkdir(parents=True)
        for plugin in (self.package / "ur0/tai").iterdir():
            shutil.copyfile(plugin, build / plugin.name)
        shutil.copyfile(self.package / "ur0/data/VitaJPOverlay/vitajpoverlay.rco",
                        build / "vitajpoverlay.rco")
        result = self.install("tools/install_ftp.sh")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        config = (self.data / "config.ini").read_text()
        for setting in ("ocr_backend = lens", "vocr_model = H15_w8.vocr",
                        "vocr_fallback_model = FL10_w8.vocr", "font_size_ja = 24"):
            self.assertIn(setting, config)
        self.assertNotIn("test-key", result.stdout)

    def test_missing_source_template_stops_before_uploading_plugins(self):
        result = self.install("tools/install_ftp.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing configuration template", result.stdout + result.stderr)
        self.assertFalse((self.device / "ur0/tai/VitaJPOverlay_Kernel.skprx").exists())
        self.assertEqual((self.device / "ur0/tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_migration_error_stops_before_publishing_configuration(self):
        template = self.package / "config.ini"
        template.write_bytes(b"invalid utf-8: \xff")
        # A checksum-valid package can still contain a template migration cannot read.
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["config.ini"] = {
            "bytes": template.stat().st_size,
            "sha256": hashlib.sha256(template.read_bytes()).hexdigest(),
        }
        manifest_path.write_text(json.dumps(manifest))
        result = self.install()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("config.ini migration failed", result.stdout + result.stderr)
        self.assertNotIn("config.ini is up to date", result.stdout)
        self.assertNotIn("test-key", result.stdout + result.stderr)
        for partition in ("ur0", "ux0"):
            self.assertEqual((self.device / partition / "tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_bad_readback_stops_before_publishing_configuration(self):
        self.environment["CORRUPT_READBACK"] = "ux0/data/VitaJPOverlay/dictionaries/jmdict.vjdict"
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FTP readback checksum failed", result.stdout + result.stderr)
        self.assertEqual((self.device / "ur0/tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())
