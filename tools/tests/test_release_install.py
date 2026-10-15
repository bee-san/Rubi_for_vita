"""Exercise the real package installer with a local filesystem FTP substitute."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import app_stamp


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
        for name in ("install_ftp.sh", "package_release.py", "prepare_ocr.py", "prepare_plaintext.py",
                     "migrate_config.py", "patch_tai_config.py", "collect_vita.py", "analyse_selftest.py",
                     "model_tiers.json", "app_stamp.py"):
            shutil.copyfile(ROOT / "tools" / name, self.package / "tools" / name)
        shutil.copyfile(ROOT / "tools/install-vita.sh", self.package / "install-vita.sh")
        for name in ("config.ini", "ux0/data/VitaJPOverlay/config.ini"):
            shutil.copyfile(ROOT / "docs/config.example.ini", self.package / name)
        for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                     "ur0/data/VitaJPOverlay/vitajpoverlay.rco", "JPOverlaySettings.vpk",
                     "ur0/tai/VitaJPOverlay_OCR.suprx",
                     "ux0/data/VitaJPOverlay/ocr/VOCR3_H15_w8.vocr", "ux0/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr",
                     "ux0/data/VitaJPOverlay/ocr/vocr_lm_o3_4000k.vlm", "ux0/data/VitaJPOverlay/ocr/LM-ATTRIBUTION.md",
                     "ux0/data/VitaJPOverlay/ocr/VOCR4_W20_w8.vocr",
                     "ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict"):
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
        # jpdb_api_key: a removed online dictionary's key (a secret) from an older build; the removed
        # Hachidori relay's choice and its computer (hachidori_host)
        (self.data / "config.ini").write_text("dictionary = hachidori\nhachidori_host = 192.168.1.20\nocr_backend = lens\n"
                                              "font_size_ja = 24\njpdb_api_key = test-key\n")
        (self.data / "games.ini").write_text("keep existing choices")
        (self.data / "region.ini").write_text("keep existing region")
        binary = self.root / "bin"
        binary.mkdir()
        curl = binary / "curl"
        curl.write_text("""#!/usr/bin/env python3
import os, pathlib, shutil, sys
from urllib.parse import urlparse
a = sys.argv[1:]
if os.environ.get('CURL_EXIT'): sys.exit(int(os.environ['CURL_EXIT']))  # 7: the Vita cannot be reached
p = urlparse(a[-1]).path.lstrip('/').replace(':', '')
remote = pathlib.Path(os.environ['FAKE_DEVICE']) / p
if '-T' in a:
    remote.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(a[a.index('-T') + 1], remote)
elif '-o' in a:
    if not remote.is_file(): sys.exit(78)  # curl's code for an FTP RETR answered 550 (not there)
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

    def install(self, script="install-vita.sh", *options):
        return subprocess.run(["bash", str(self.package / script), *options,
                               "127.0.0.1:" + str(self.listener.getsockname()[1])],
                              env=self.environment, capture_output=True, text=True, timeout=30)

    def tai(self, partition):
        return (self.device / partition / "tai/config.txt").read_bytes()

    def assert_unchanged(self, result):
        """Nothing reached the Vita: no plugin, backup, boot record or setting."""
        self.assertFalse((self.device / "ur0/tai/VitaJPOverlay_Kernel.skprx").exists(), result.stdout)
        self.assertFalse((self.device / "ur0/data/VitaJPOverlay/boot.txt").exists())
        for partition in ("ur0", "ux0"):
            self.assertEqual(self.tai(partition), self.original_tai)
            self.assertFalse((self.device / partition / "tai/config.txt.vjo-bak").exists())
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_update_is_verified_idempotent_and_preserves_unrelated_state(self):
        self.assertFalse((self.package / "docs/config.example.ini").exists())
        installed = None
        # The second update names no game: the one registered by the first is kept.
        for options in (("--title", "PCSG00415"), ()):
            result = self.install("install-vita.sh", *options)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("test-key", result.stdout)
            self.assertNotIn("migration failed", result.stdout + result.stderr)
            self.assertIn("OCR worker registered for PCSG00415", result.stdout)
            if installed is not None:
                self.assertEqual([self.tai(p) for p in ("ur0", "ux0")], installed)
            else:
                # The Vita's own backup is the configuration from before the update.
                for partition in ("ur0", "ux0"):
                    self.assertEqual((self.device / partition / "tai/config.txt.vjo-bak").read_bytes(),
                                     self.original_tai)
            installed = [self.tai(p) for p in ("ur0", "ux0")]
        for partition in ("ur0", "ux0"):
            tai = self.tai(partition)
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_Kernel.skprx"), 1)
            self.assertEqual(tai.count(b"ur0:tai/VitaJPOverlay_Shell.suprx"), 1)
            self.assertIn(b"ur0:tai/Other.skprx\r\n", tai)
            # The *ALL worker is commented out; the game's own section loads it.
            self.assertIn(b"*ALL\r\n# Mainline OCR: ur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertIn(b"\r\n*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertEqual(tai.count(b"\nur0:tai/VitaJPOverlay_OCR.suprx"), 1)
            self.assertNotIn(b"\n", tai.replace(b"\r\n", b""))
        config = (self.data / "config.ini").read_text()
        # a first install: the template's model size, Normal; no model line (API 21)
        self.assertNotIn("vocr_model", config)
        for setting in ("dictionary = local", "ocr_backend = vocr", "vocr_tier = normal",
                        "log_file = on", "anki_mining = off",
                        "local_dictionaries = plaintext.vjdict", "font_size_ja = 24",
                        "ocr_lm = 1000k", "ocr_cascade_ms = 2000"):
            self.assertIn(setting, config)
        # the removed online dictionaries' API key is gone, its value never printed; so is the relay
        self.assertNotIn("api_key", config)
        self.assertNotIn("test-key", config)
        self.assertNotIn("achidori", config)
        self.assertEqual((self.data / "games.ini").read_text(), "keep existing choices")
        self.assertEqual((self.data / "region.ini").read_text(), "keep existing region")
        self.assertTrue((self.device / "ux0/data/JPOverlaySettings.vpk").is_file())
        self.assertEqual((self.device / "ur0/tai/VitaJPOverlay_OCR.suprx").read_bytes(),
                         (self.package / "ur0/tai/VitaJPOverlay_OCR.suprx").read_bytes())
        for name in ("dictionaries/plaintext.vjdict",
                     "ocr/VOCR3_H15_w8.vocr", "ocr/VOCR3_FL10_w8.vocr", "ocr/vocr_lm_o3_4000k.vlm",
                     "ocr/LM-ATTRIBUTION.md", "ocr/VOCR4_W20_w8.vocr"):
            self.assertEqual((self.data / name).read_bytes(),
                             (self.package / "ux0/data/VitaJPOverlay" / name).read_bytes())

    # ---- the Overlay app (core/app_stamp.h): staged with the plugins, checked at the end, loudly ----
    def stamped_package(self):
        """The package with a real app VPK (an eboot.bin and its stamp) and the record beside the plugins."""
        eboot = b"SCE\0 this package's eboot.bin"
        stamp = app_stamp.make(eboot, 21)
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("eboot.bin", eboot)
            z.writestr("vjo_app.txt", stamp)
        (self.package / "JPOverlaySettings.vpk").write_bytes(buf.getvalue())
        (self.package / "ux0/data/VitaJPOverlay/vjo_app.txt").write_text(stamp)
        files = {str(p.relative_to(self.package)): {"bytes": p.stat().st_size,
                 "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in self.package.rglob("*") if p.is_file() and p.name != "package-manifest.json"}
        (self.package / "package-manifest.json").write_text(json.dumps({"files": files}))
        return eboot, stamp

    def device_app(self, eboot=None, stamp=None):
        app = self.device / "ux0/app/VJOSET001"
        shutil.rmtree(app, ignore_errors=True)
        if eboot is not None:
            app.mkdir(parents=True)
            (app / "eboot.bin").write_bytes(eboot)
            if stamp is not None:
                (app / "vjo_app.txt").write_text(stamp)

    def device_files(self):
        return {str(p.relative_to(self.device)): p.read_bytes() for p in self.device.rglob("*") if p.is_file()}

    def test_install_stages_the_app_and_ends_with_a_loud_box_until_it_is_installed(self):
        eboot, stamp = self.stamped_package()
        self.device_app(b"test.2's eboot.bin")  # #101: the app from before the stamps
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # the VPK and the record go up with the plugins
        self.assertEqual((self.device / "ux0/data/JPOverlaySettings.vpk").read_bytes(),
                         (self.package / "JPOverlaySettings.vpk").read_bytes())
        self.assertEqual((self.device / "ux0/data/VitaJPOverlay/vjo_app.txt").read_text(), stamp)
        # the last words, after "Installed.": a box, the app's state and what to do
        tail = result.stdout.split("Installed. Reboot the Vita", 1)[1]
        box = [l for l in tail.splitlines() if l.startswith(("+", "|"))]
        self.assertTrue(box and all(len(l) == 78 for l in box), box)
        flat = " ".join(" ".join(l.strip("|+- ").split()) for l in box)
        self.assertIn("ONE MORE STEP: install the Overlay app (JPOverlaySettings.vpk)", flat)
        self.assertIn("no version stamp: it is from an older build (v0.8.0-test.2 or earlier)", flat)
        self.assertIn("bash install-vita.sh --check-app 127.0.0.1", flat)
        self.assertEqual(result.stdout.rstrip().splitlines()[-1], box[-1])  # nothing can scroll it away
        # VitaShell installs the VPK: the check says so, read-only
        self.device_app(eboot, stamp)
        before = self.device_files()
        check = self.install("install-vita.sh", "--check-app")
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        self.assertIn("Overlay app: up to date", check.stdout)
        self.assertIn("Its eboot.bin is the package VPK's.", check.stdout)
        self.assertEqual(self.device_files(), before)
        # and an install over an up-to-date app says so in the same place
        result = self.install("install-vita.sh")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Overlay app: up to date", result.stdout.split("Installed. Reboot the Vita", 1)[1])

    def test_the_prompt_waits_only_in_a_terminal(self):
        """In a terminal, a missing app makes the installer wait for Enter and check again; without one
        (stdin or stdout not a tty, as in CI and every other test here) or with VJO_NO_PROMPT it never waits."""
        import pty
        import select
        eboot, stamp = self.stamped_package()
        self.device_app(b"test.2's eboot.bin")
        plain = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertNotIn("Press Enter", plain.stdout)
        # without a terminal the check runs once: no prompt, so no second check after it
        self.assertEqual(plain.stdout.count("ONE MORE STEP"), 1, plain.stdout)
        self.assertNotIn("Overlay app: NOT this package's", plain.stdout)

        def in_pty(extra_env, then_install):
            pid, fd = pty.fork()
            if pid == 0:  # the child: the installer on the pty's both ends
                env = dict(self.environment, **extra_env)
                os.execvpe("bash", ["bash", str(self.package / "install-vita.sh"),
                                    "127.0.0.1:" + str(self.listener.getsockname()[1])], env)
            out, deadline = b"", time.monotonic() + 60
            while time.monotonic() < deadline:
                ready, _, _ = select.select([fd], [], [], 0.5)
                if ready:
                    try:
                        chunk = os.read(fd, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    out += chunk
                    if b"Press Enter once the app is installed" in out and then_install:
                        then_install()
                        then_install = None
                        os.write(fd, b"\n")
            os.close(fd)
            _, status = os.waitpid(pid, 0)
            return out.decode("utf-8", "replace"), os.waitstatus_to_exitcode(status)

        out, rc = in_pty({}, lambda: self.device_app(eboot, stamp))
        self.assertEqual(rc, 0, out)
        self.assertIn("Press Enter once the app is installed", out)
        self.assertIn("Overlay app: up to date", out.split("Press Enter once the app is installed", 1)[1])
        self.device_app(b"test.2's eboot.bin")
        out, rc = in_pty({"VJO_NO_PROMPT": "1"}, None)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("Press Enter", out)
        self.assertIn("ONE MORE STEP", out)

    def test_check_app_answers_for_each_app_and_never_writes(self):
        eboot, stamp = self.stamped_package()
        cases = ((None, None, 1, "No Overlay app is installed on the Vita"),
                 (b"old eboot", None, 1, "no version stamp"),
                 (b"other eboot", stamp.replace("api=21", "api=20"), 1, "an older build"),
                 (eboot, stamp, 0, "Overlay app: up to date"))
        for app_eboot, app_stamp_text, code, text in cases:
            with self.subTest(text=text):
                self.device_app(app_eboot, app_stamp_text)
                before = self.device_files()
                for script in ("install-vita.sh", "tools/install_ftp.sh"):
                    result = self.install(script, "--check-app")
                    self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                    self.assertIn(text, " ".join(result.stdout.replace("|", " ").split()))
                    self.assertEqual(self.device_files(), before)
                    self.assertFalse((self.device / "ur0/tai/VitaJPOverlay_Kernel.skprx").exists())
        # a Vita that cannot be reached (FTP stopped, rebooted) is not one without the app
        result = subprocess.run(["bash", str(self.package / "install-vita.sh"), "--check-app",
                                 "127.0.0.1:" + str(self.listener.getsockname()[1])],
                                env=dict(self.environment, CURL_EXIT="7"), capture_output=True, text=True, timeout=30,
                                check=False)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("Cannot read ux0:app/VJOSET001/vjo_app.txt from 127.0.0.1", result.stdout)
        self.assertIn("(curl exit 7)", result.stdout)
        self.assertIn("bash install-vita.sh --check-app 127.0.0.1", result.stdout)
        self.assertNotIn("No Overlay app is installed", result.stdout)
        # nothing listening at all (VitaShell's FTP not started, a wrong address): cannot tell either
        closed = socket.socket()
        closed.bind(("127.0.0.1", 0))
        port = closed.getsockname()[1]
        closed.close()  # bound once, now nobody listens on it
        for script in ("install-vita.sh", "tools/install_ftp.sh"):
            result = subprocess.run(["bash", str(self.package / script), "--check-app", f"127.0.0.1:{port}"],
                                    env=self.environment, capture_output=True, text=True, timeout=60, check=False)
            self.assertEqual(result.returncode, 2, script + ": " + result.stdout + result.stderr)
            self.assertIn("Cannot reach 127.0.0.1", result.stdout)
        # a package from before the stamps cannot tell
        (self.package / "ux0/data/VitaJPOverlay/vjo_app.txt").unlink()
        result = self.install("install-vita.sh", "--check-app")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("Overlay app: not checked", result.stdout)

    def test_package_settings_from_the_manifest_are_applied_after_the_defaults(self):
        # A test build's own settings (package_release.py --set): anki_mining = on wins over
        # the user's earlier off, and the installer says which settings the package brings.
        (self.data / "config.ini").write_text("anki_mining = off\nvocr_model = FL10_w8.vocr\njiten_api_key = test-key\n")
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["install_settings"] = {"anki_mining": "on", "log_host": ""}
        manifest_path.write_text(json.dumps(manifest))
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("package settings: anki_mining=on log_host=\n", result.stdout)
        config = (self.data / "config.ini").read_text()
        # the older model choice became its size (FL10: Small), which the installer keeps
        for setting in ("anki_mining = on\n", "log_host =\n", "vocr_tier = small\n"):
            self.assertIn(setting, config)
        self.assertNotIn("vocr_model", config)
        self.assertNotIn("test-key", result.stdout + config)
        self.assertIn("config.ini: jiten_api_key removed", result.stdout)

    def test_an_update_keeps_the_chosen_model_size(self):
        # API 21: the Overlay app's Model size is the user's; the installer sets no size or model.
        for chosen in ("small", "large"):
            (self.data / "config.ini").write_text("ocr_backend = vocr\nvocr_tier = %s\nfont_size_ja = 24\n" % chosen)
            result = self.install("install-vita.sh", "--title", "PCSG00415")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = (self.data / "config.ini").read_text()
            self.assertIn("\nvocr_tier = %s\n" % chosen, config)
            self.assertEqual(config.count("vocr_tier ="), 1)
            self.assertNotIn("vocr_model", config)
            self.assertNotIn("config.ini: vocr_tier", result.stdout)

    def test_package_settings_are_validated_again_and_keys_are_masked(self):
        # A crafted manifest: no shell runs, a line break of any kind or an unknown key is dropped
        # (a removed online dictionary's API key too: the template has none), an API key's value
        # is never printed.
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        canary = self.root / "canary"
        manifest["install_settings"] = {
            "local_dictionary_dir": f"$(touch {canary}) `touch {canary}` 'q' \"dq\"",
            "log_host": "line1\nlog_file = off", "hachidori_host": "a\u2028anki_host = 6.6.6.6",
            "ocr_mode": "x\x85y", "bad key": "1", "not_a_setting": "on", "anki_deck": "removed",
            "anki_host": "192.168.1.5", "jpdb_api_key": "pkg-secret"}
        manifest_path.write_text(json.dumps(manifest))
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(canary.exists())
        self.assertNotIn("pkg-secret", result.stdout + result.stderr)
        shown = next(l for l in result.stdout.splitlines() if l.startswith("package settings:"))
        # (anki_host, AnkiConnect's address, and the removed online dictionaries' API keys are no
        # settings any more: dropped as unknown)
        for dropped in ("log_host", "hachidori_host", "ocr_mode", "bad key", "not_a_setting", "anki_deck",
                        "anki_host", "jpdb_api_key"):
            self.assertNotIn(dropped, shown)
        config = (self.data / "config.ini").read_text()
        self.assertIn(f"local_dictionary_dir = $(touch {canary}) `touch {canary}` 'q' \"dq\"\n", config)
        self.assertNotIn("anki_host", config)
        self.assertNotIn("pkg-secret", config)
        self.assertNotIn("api_key", config)
        self.assertIn("log_file = on\n", config)
        self.assertNotIn("6.6.6.6", config)
        self.assertNotIn("not_a_setting", config)

    def test_no_credential_is_a_package_setting_and_an_old_one_goes_unshown(self):
        """audit-overlay-test3-cleanup B1, after the removals: jpdb.io's and jiten.moe's API keys went
        with them and anki_audio_url (whose URL can carry a key) with AnkiConnect, so the template has
        no credential left. A manifest naming one "set" is ignored, and the user's old values are
        removed, each named in one line, never shown."""
        for name in ("config.ini", "ux0/data/VitaJPOverlay/config.ini"):
            text = (self.package / name).read_text()
            self.assertNotIn("api_key", text)
            self.assertNotIn("anki_audio_url", text)
        manifest_path = self.package / "package-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["install_settings"] = {"anki_mining": "on", "anki_audio_url": "set", "jpdb_api_key": "set",
                                        "jiten_api_key": ""}
        manifest_path.write_text(json.dumps(manifest))
        own = "https://own.invalid/a?term={term}&key=users-own-url"
        (self.data / "config.ini").write_text(f"anki_mining = off\njpdb_api_key = users-own-key\nanki_audio_url = {own}\n")
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        shown = next(l for l in result.stdout.splitlines() if l.startswith("package settings:"))
        self.assertEqual(shown, "package settings: anki_mining=on")
        for secret in ("users-own-key", "users-own-url", "own.invalid"):
            self.assertNotIn(secret, result.stdout + result.stderr)
        config = (self.data / "config.ini").read_text()
        for gone in ("api_key", "anki_audio_url", "users-own", "= set\n"):
            self.assertNotIn(gone, config)
        self.assertIn("\nanki_mining = on\n", config)
        self.assertIn("config.ini: jpdb_api_key removed", result.stdout)
        self.assertIn("config.ini: anki_audio_url removed (AnkiConnect was removed", result.stdout)

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
        # an earlier build's record of its app: this build has no stamp (its VPK was not built)
        (self.data / "vjo_app.txt").write_text("vjoapp1 api=20 build=0123456789abcdef\n")
        result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # the record is emptied, never left describing another build's app; the box cannot tell
        self.assertEqual((self.data / "vjo_app.txt").read_bytes(), b"")
        self.assertIn("Overlay app: not checked", result.stdout)
        config = (self.data / "config.ini").read_text()
        for setting in ("ocr_backend = lens", "vocr_tier = normal", "font_size_ja = 24"):
            self.assertIn(setting, config)
        self.assertNotIn("vocr_model", config)
        self.assertNotIn("test-key", result.stdout)

    def test_source_install_rewrites_a_removed_engine_to_vocr(self):
        # The source installer keeps the saved engine (no --set ocr_backend), except a removed
        # one: the VOCR2 trial and ncnn (host tools) become the local engine, vocr.
        (self.package / "config.ini").unlink()
        (self.package / "docs").mkdir()
        shutil.copyfile(ROOT / "docs/config.example.ini", self.package / "docs/config.example.ini")
        build = self.package / "build/vita"
        build.mkdir(parents=True)
        for plugin in (self.package / "ur0/tai").iterdir():
            shutil.copyfile(plugin, build / plugin.name)
        shutil.copyfile(self.package / "ur0/data/VitaJPOverlay/vitajpoverlay.rco", build / "vitajpoverlay.rco")
        for removed in ("vocr2", "ncnn"):
            (self.data / "config.ini").write_text(
                "ocr_backend = %s\nvocr_model = H15_w8.vocr\nfont_size_ja = 24\njpdb_api_key = test-key\n" % removed)
            result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = (self.data / "config.ini").read_text()
            self.assertIn("\nocr_backend = vocr\n", config)
            self.assertNotIn("ocr_backend = " + removed, config)
            for setting in ("vocr_tier = normal", "font_size_ja = 24"):
                self.assertIn(setting, config)
            self.assertNotIn("vocr_model", config)
            self.assertNotIn("test-key", config)
            self.assertIn("config.ini: ocr_backend = vocr", result.stdout)
            self.assertNotIn("test-key", result.stdout)

    def test_source_install_rewrites_a_removed_dictionary_to_local(self):
        # jpdb.io and jiten.moe were removed: the source installer (it keeps the saved
        # dictionary, no --set dictionary) writes local, and drops their API keys, saying so
        # without their values.
        (self.package / "config.ini").unlink()
        (self.package / "docs").mkdir()
        shutil.copyfile(ROOT / "docs/config.example.ini", self.package / "docs/config.example.ini")
        build = self.package / "build/vita"
        build.mkdir(parents=True)
        for plugin in (self.package / "ur0/tai").iterdir():
            shutil.copyfile(plugin, build / plugin.name)
        shutil.copyfile(self.package / "ur0/data/VitaJPOverlay/vitajpoverlay.rco", build / "vitajpoverlay.rco")
        for removed in ("jpdb", "JITEN"):
            (self.data / "config.ini").write_text(
                "dictionary = %s\njpdb_api_key = secret-1\njiten_api_key = secret-2\nfont_size_ja = 24\n" % removed)
            result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = (self.data / "config.ini").read_text()
            self.assertIn("\ndictionary = local\n", config)
            self.assertIn("font_size_ja = 24", config)
            self.assertNotIn("secret", config + result.stdout + result.stderr)
            self.assertNotIn("api_key", config)
            for line in ("config.ini: dictionary = local", "config.ini: jpdb_api_key removed",
                         "config.ini: jiten_api_key removed"):
                self.assertIn(line, result.stdout)

    def test_missing_source_template_stops_before_uploading_plugins(self):
        result = self.install("tools/install_ftp.sh", "--title", "PCSG00415")
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
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("config.ini migration failed", result.stdout + result.stderr)
        self.assertNotIn("config.ini is up to date", result.stdout)
        self.assertNotIn("test-key", result.stdout + result.stderr)
        for partition in ("ur0", "ux0"):
            self.assertEqual((self.device / partition / "tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_bad_readback_stops_before_publishing_configuration(self):
        self.environment["CORRUPT_READBACK"] = "ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict"
        result = self.install("install-vita.sh", "--title", "PCSG00415")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FTP readback checksum failed", result.stdout + result.stderr)
        self.assertEqual((self.device / "ur0/tai/config.txt").read_bytes(), self.original_tai)
        self.assertIn("ocr_backend = lens", (self.data / "config.ini").read_text())

    def test_without_a_title_nothing_is_changed(self):
        # The previous installer's *ALL registration names no game: refused, never kept.
        for options in ((), ("--title", "ALL"), ("--title", "NPXS10015"), ("--title", "PCSG0041")):
            with self.subTest(options=options):
                result = self.install("install-vita.sh", *options)
                self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
                self.assertIn("Nothing was changed on the Vita", result.stdout)
                self.assertIn("Correct the --title value" if options else "Run again with --title", result.stdout)
                self.assertIn("bash install-vita.sh --title PCSG00415 127.0.0.1", result.stdout)
                self.assertIn("--title" if options else "never registered under *ALL", result.stderr)
                self.assertNotIn("Installed.", result.stdout)
                self.assert_unchanged(result)

    def test_titles_are_added_and_kept_in_both_configurations(self):
        # ux0 already has one game; ur0 only the old *ALL line. Both end with the same games.
        (self.device / "ux0/tai/config.txt").write_bytes(
            b"*KERNEL\r\nur0:tai/Other.skprx\r\n*PCSE00120\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n")
        result = self.install("install-vita.sh", "--title", "NPXS10028", "--title", "pcsg00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Both configurations report the same games, in the same order.
        self.assertEqual(result.stdout.count("(OCR worker registered for NPXS10028, PCSG00415, PCSE00120)"), 2)
        for partition in ("ur0", "ux0"):
            tai = self.tai(partition)
            for title in (b"NPXS10028", b"PCSG00415", b"PCSE00120"):
                self.assertIn(b"*" + title + b"\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n", tai)
            self.assertEqual(tai.count(b"\nur0:tai/VitaJPOverlay_OCR.suprx"), 3)
            self.assertNotIn(b"*ALL\r\nur0:tai/VitaJPOverlay_OCR.suprx", tai)
        # Options after the address work too, and change nothing more.
        before = [self.tai(p) for p in ("ur0", "ux0")]
        result = subprocess.run(["bash", str(self.package / "install-vita.sh"),
                                 "127.0.0.1:" + str(self.listener.getsockname()[1]), "--title", "PCSE00120"],
                                env=self.environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([self.tai(p) for p in ("ur0", "ux0")], before)

    def test_uninstall_after_a_per_title_install(self):
        self.assertEqual(self.install("install-vita.sh", "--title", "PCSG00415").returncode, 0)
        result = self.install("tools/install_ftp.sh", "--uninstall")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for partition in ("ur0", "ux0"):
            tai = self.tai(partition)
            self.assertIn(b"ur0:tai/Other.skprx\r\n", tai)
            for line in tai.split(b"\r\n"):
                self.assertFalse(line.startswith(b"ur0:tai/VitaJPOverlay_"), tai)
        # Data stays; the boot record is reset for the next build.
        self.assertEqual((self.data / "games.ini").read_text(), "keep existing choices")
        self.assertEqual((self.device / "ur0/data/VitaJPOverlay/boot.txt").read_bytes(), b"")
        # Nothing of ours is registered any more: the next install has to name its games.
        uninstalled = [self.tai(p) for p in ("ur0", "ux0")]
        result = self.install("install-vita.sh")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertEqual([self.tai(p) for p in ("ur0", "ux0")], uninstalled)

    def test_self_test_accepts_a_title_and_leaves_the_configuration(self):
        # Issue #70's step: arming the self-test with --title changes no registration.
        result = self.install("tools/install_ftp.sh", "--self-test", "--title", "PCSG00415")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Self-test armed", result.stdout)
        # the command it names is the one that collects (install-vita.sh takes no --collect)
        self.assertIn("run: bash tools/install_ftp.sh --collect 127.0.0.1\n", result.stdout)
        self.assertNotIn("install-vita.sh --collect", result.stdout)
        self.assertIn("--title takes effect when installing (bash install-vita.sh --title PCSG00415", result.stdout)
        self.assertEqual((self.data / "selftest.txt").read_bytes(), b"")
        for partition in ("ur0", "ux0"):
            self.assertEqual(self.tai(partition), self.original_tai)
