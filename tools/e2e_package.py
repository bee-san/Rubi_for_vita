#!/usr/bin/env python3
"""A release package's own scripts against a simulated Vita: install, app check, self-test, collect, uninstall.

    python3 tools/e2e_package.py RELEASE.zip [--work DIR] [--keep]

RELEASE.zip is a package of tools/package_release.py or tools/package_variants.py (the plugins, the
Rubi app, the models, the LMs, the dictionary and package-manifest.json), of either build. A base
zip (the Vita build's own, CI's artifacts) has no models: package it first. The zip is unpacked into
DIR/pkg (default build/e2e-package/<the zip's name>), and each scenario runs a copy of it, the
package's own install-vita.sh and tools/install_ftp.sh, against a fake Vita's partitions
(DIR/<scenario>/device) served over FTP by this tree's tools/tests/ftp_mock.py.

DIR may hold other files (the release's out/ with the zip itself): the e2e makes, and on a later run
replaces, only its own entries (ENTRIES: pkg/, the scenarios' directories and logs) beside its marker
DIR/.e2e_package, which it writes first, and touches nothing else. It refuses a DIR that already has
one of those names without the marker, a DIR that is a file, and a package that is, or is inside, one
of its entries. After the run it checks that DIR's top-level names are its own entries and the files
that were there before (what changes inside its entries is its own).

Scenario A, the owner's case (#101): a Vita with v0.8.0-test.2 installed and test.1's Rubi app
still on it: test.2's taiHEN lines (*KERNEL, *main, *PCSG00415, the commented *ALL, Meiki and Text
lines), its model files, its VPK in ux0:data, and the config.ini the old app left (ocr_backend =
vocr2, vocr_model/vocr_fallback_model, jpdb/jiten API keys, Anki on with an address).
install-vita.sh --title PCSG00415 (a package whose worker mode is kernel, the manifest's "worker", ignores
--title with a note and removes the worker's lines: the kernel plugin loads it, docs/one-vpk-install.md;
lines mode registers it under *PCSG00415); --check-app with no app, test.2's app and this package's;
--self-test; a console session simulated in the plugins' log formats (a model size that did not
fit, an Anki save, the Rubi app's export); --collect (read-only: one zip and its analysis);
--uninstall.
Scenario B, a fresh henkaku Vita with an *ALL worker line in both ur0 and ux0: install-vita.sh --title
(kernel mode removes the *ALL worker line, lines mode comments it out).
Scenario C, an older choice migrated: vocr_model = VOCR4_W20_w8.vocr becomes vocr_tier = large,
dictionary = jpdb becomes local, and the registered game's line goes with its section (kernel mode) or
is kept without --title (lines mode).
Before them, the package itself: its manifest names the build its modules are (tools/check_variant.py's
markers, read as tools/package_release.py reads a base, and all four modules, the worker too, checked as
the Vita build checks them, against this tree's sources) and the Rubi app's version (a manifest without
app_version fails: tools/package_release.py records it with the variant), its models and LMs are this
tree's pinned files (tools/prepare_ocr.py), and ocr/ holds nothing else (the installer uploads every .vocr
and .vlm there): run the e2e from a checkout of the package's own commit.

CI runs it on both builds' packages (.github/workflows/release.yml, the `package` job). Prints a
PASS or FAIL line per check and a JSON summary; exits 1 on any failure. After a pass the copies
are removed unless --keep; DIR keeps the scenarios' logs.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/tests"))
sys.path.insert(0, str(ROOT / "tools"))
from ftp_mock import FtpMock  # noqa: E402
import package_release as release  # noqa: E402  (the packager's own reading of a package's modules and pins)
import package_variants  # noqa: E402  (module_problems: the four modules as the Vita build checks them)

FIXTURE = ROOT / "tools/tests/fixtures/selftest-v2-diagnostics.txt"
SECRET = "e2e-secret-jpdb-key"
SECRET2 = "e2e-secret-jiten-key"
AUDIO = "https://audio.example.invalid/?term={term}&token=e2e-secret-audio"
SECRETS = (SECRET, SECRET2, "e2e-secret-audio")
SCENARIOS = ("A-test2", "B-fresh", "C-migrate")
LOGS = ("A-install.log", "A-collect.log", "B-install.log", "C-install.log")
ENTRIES = ("pkg", *SCENARIOS, *LOGS)  # all the e2e makes in its working directory, and all it ever removes
MARKER = ".e2e_package"  # written before any entry: the entries beside it are the e2e's own


def config_values(text):
    return {k: v for k, v in re.findall(r"^(\w+) = ?(.*)$", text, re.M)}


def remove(path):
    """One of the e2e's own entries: a directory (never followed through a link), a file or a link."""
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def prepare_work(work, package, default=False):
    """Readies the working directory, or says why it cannot be used (audit-overlay-release-variants-d B1:
    DIR is the caller's, so only the e2e's own entries of an earlier run are removed, and only where its
    marker says it made them). default: DIR is the e2e's own default. Returns the problem, or None."""
    if work.exists() and not work.is_dir():
        return f"--work {work} is not a directory"
    marker = work / MARKER
    if marker.is_symlink() or (marker.exists() and not marker.is_file()):
        return f"{marker} is not tools/e2e_package.py's marker file: choose another --work"
    package = package.resolve()
    for name in ENTRIES:
        if package == work / name or work / name in package.parents:
            return f"the package is, or is inside, {work / name}, which the e2e replaces: choose another --work"
    found = [name for name in ENTRIES if os.path.lexists(work / name)]
    if found and not marker.is_file():
        if default:  # a run of the e2e from before the marker (#152's first head) left it
            return (f"{work} has {', '.join(found)} but no {MARKER}: an older run's? Remove it (rm -rf {work}) "
                    "or choose a --work")
        return (f"--work {work} has {', '.join(found)}, which tools/e2e_package.py did not make (no {MARKER}): "
                "choose another directory (a new one, or the default)")
    work.mkdir(parents=True, exist_ok=True)
    marker.write_text("tools/e2e_package.py's working directory; a run replaces only these entries: "
                      + ", ".join(ENTRIES) + "\n")
    for name in found:
        remove(work / name)
    return None


def unpack(zip_path, dest):
    """Extracts a zip, keeping each file's mode bits (zipfile's extractall drops them)."""
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            path = Path(z.extract(info, dest))
            mode = (info.external_attr >> 16) & 0o777
            if mode and not info.is_dir():
                path.chmod(mode)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class E2E:
    def __init__(self, pkg, work, package=None):
        self.pkg, self.work, self.package = pkg, work, package
        self.checks = []
        self.manifest = json.loads((pkg / "package-manifest.json").read_text())
        self.variant = self.manifest.get("variant")
        # where the OCR worker comes from (package_release.py "worker"): kernel, the kernel plugin loads it
        # and the installer writes no line for it; lines (an older package), one *TITLEID section per game
        self.worker = self.manifest.get("worker", "lines")
        self.settings = self.manifest.get("install_settings", {})
        self.shown = " ".join(f"{k}={v}" for k, v in self.settings.items())
        self.template = config_values((pkg / "config.ini").read_text())
        # the plugins' API, from the package's own record of its app (vjoapp1 api=N build=...)
        stamp = (pkg / "ux0/data/VitaJPOverlay/vjo_app.txt").read_text()
        self.api = re.search(r"\bapi=(\d+)", stamp).group(1)
        tree_api = re.search(r"#define VJO_API_VERSION (\d+)", (ROOT / "include/vjo_api.h").read_text()).group(1)
        if tree_api != self.api:
            print(f"note: the package's API {self.api} is not this tree's ({tree_api}): run the e2e of its own commit")
        # every file the installer must put on the Vita: the plugins, RCO, models, LMs, dictionary, VPK
        self.device_files = sorted(n for n in self.manifest["files"]
                                   if n.startswith(("ur0/", "ux0/")) and not n.endswith("config.ini"))

    def save_log(self, name, text):
        assert name in LOGS, name  # ENTRIES: what the e2e may make, and replace, in its working directory
        (self.work / name).write_text(text)

    def check(self, ok, what):
        self.checks.append((bool(ok), what))
        print(("PASS " if ok else "FAIL ") + what, flush=True)

    @staticmethod
    def sh(run_pkg, *args, timeout=1800):
        return subprocess.run(["bash", *args], cwd=run_pkg, capture_output=True, text=True, timeout=timeout)

    def fresh(self, name):
        assert name in SCENARIOS, name  # ENTRIES: what the e2e may make, and replace, in its working directory
        base = self.work / name
        shutil.rmtree(base, ignore_errors=True)
        dev = base / "device"
        for d in ("ur0/tai", "ux0/tai", "ux0/data/VitaJPOverlay/ocr", "ur0/data/VitaJPOverlay"):
            (dev / d).mkdir(parents=True)
        run_pkg = base / "pkg"
        shutil.copytree(self.pkg, run_pkg, symlinks=True)
        return dev, run_pkg

    def no_secret(self, text, label):
        self.check(not any(s in text for s in SECRETS), f"{label}: no credential is printed")

    def installed_right(self, dev, run_pkg, part_configs, before, label):
        check, pkg = self.check, self.pkg
        for part in part_configs:
            t = (dev / part / "tai/config.txt").read_bytes()
            lines = t.split(b"\r\n")
            if self.worker == "kernel":
                check(not re.search(rb"(?im)^[ \t]*[^#\r\n][^\r\n]*VitaJPOverlay_OCR\.suprx", t),
                      f"{label} {part}: no worker line (the kernel plugin loads the worker)")
            else:
                check(lines.count(b"ur0:tai/VitaJPOverlay_OCR.suprx") == 1
                      and b"*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx" in t,
                      f"{label} {part}: the worker once, under *PCSG00415 only")
            check(not re.search(rb"\*ALL\r\nur0:tai/VitaJPOverlay_OCR", t), f"{label} {part}: no live *ALL worker line")
            check(lines.count(b"ur0:tai/VitaJPOverlay_Kernel.skprx") == 1
                  and lines.count(b"ur0:tai/VitaJPOverlay_Shell.suprx") == 1
                  and b"henkaku" in t, f"{label} {part}: kernel and shell once each, henkaku kept, CRLF kept")
            check((dev / part / "tai/config.txt.vjo-bak").read_bytes() == before[part],
                  f"{label} {part}: config.txt.vjo-bak is the original")
            dated = sorted((dev / part / "tai").glob("config.txt.vjo-*.bak"))
            check(dated and re.fullmatch(r"config\.txt\.vjo-\d{8}-\d{6}(-\d+)?\.bak", dated[0].name)
                  and dated[0].read_bytes() == before[part], f"{label} {part}: a dated backup of the original, kept")
        payload = pkg / "payload-manifest.txt"
        if payload.is_file():  # the one-VPK install's record, the line the Rubi app writes (core/payload.h)
            sys.path.insert(0, str(ROOT / "tools"))
            import package_vpk
            record = dev / "ux0/data/VitaJPOverlay/installed.txt"
            check(record.is_file() and record.read_text() == package_vpk.installed_record(payload.read_text()),
                  f"{label} installed.txt is this package's record")
        for rel in self.device_files:
            dst = dev / ("ux0/data/Rubi.vpk" if rel == "Rubi.vpk" else rel)
            check(dst.is_file() and dst.read_bytes() == (pkg / rel).read_bytes(), f"{label} uploaded and identical: {rel}")
        check((dev / "ux0/data/Rubi.vpk").read_bytes() == (pkg / "Rubi.vpk").read_bytes(),
              f"{label} uploaded: ux0:data/Rubi.vpk (this build's app)")
        check((dev / "ur0/data/VitaJPOverlay/boot.txt").read_bytes() == b"", f"{label} boot record reset")
        backups = sorted((run_pkg / "build/device/backup").glob("*"))
        check(backups and (backups[-1] / "ur0-config.txt").read_bytes() == before["ur0"],
              f"{label} PC backup: {backups[-1].relative_to(run_pkg) if backups else None} has the ur0 config")

    def vpk_loud(self, stdout, label):
        """The VPK step is named after 'Installed.' (#101: test.2's run used the old app), in a box (#122)."""
        tail = stdout[stdout.rfind("Installed."):] if "Installed." in stdout else ""
        self.check("Rubi.vpk" in tail, f"{label}: the install's last lines name Rubi.vpk")
        self.check("ONE MORE STEP: install the Rubi app (Rubi.vpk)" in tail and "+---" in tail,
                   f"{label}: the install ends with the box 'ONE MORE STEP: install the Rubi app'")
        return tail

    def install_app(self, dev, eboot=None, stamp=None):
        """What VitaShell's VPK install leaves where the check looks: ux0:app/VJOSET001/{eboot.bin,vjo_app.txt}."""
        app = dev / "ux0/app/VJOSET001"
        shutil.rmtree(app, ignore_errors=True)
        app.mkdir(parents=True)
        with zipfile.ZipFile(self.pkg / "Rubi.vpk") as vpk:
            (app / "eboot.bin").write_bytes(eboot if eboot is not None else vpk.read("eboot.bin"))
            if stamp is None:
                stamp = vpk.read("vjo_app.txt")
        if stamp:
            (app / "vjo_app.txt").write_bytes(stamp)

    def check_app(self, run_pkg, host, want_rc, want_text, label):
        r = self.sh(run_pkg, "install-vita.sh", "--check-app", host)
        flat = " ".join(r.stdout.replace("|", " ").split())  # the box wraps its lines
        self.check(r.returncode == want_rc and want_text in flat,
                   f"{label}: install-vita.sh --check-app rc {r.returncode} (want {want_rc}), says {want_text!r}")
        return r

    # ---------------- the package itself (audit-overlay-release-variants-d notes 1 and 4, -e notes 1-3) ----------------
    def package_checks(self):
        """--verify checks the files against the manifest, not the manifest: here its variant against the four modules,
        as tools/package_release.py reads a base and as the Vita build checks them (tools/check_variant.py with this
        tree's sources, the worker too), its app version (required) against the app, its models and LMs against this
        tree's pins (a re-hashed altered model fails), and that ocr/ holds nothing else (install_ftp.sh uploads every
        .vocr and .vlm there)."""
        check, m = self.check, self.manifest
        want = m.get("variant") or "debug"  # a package from before the two builds names none: all are the debug build
        try:
            with zipfile.ZipFile(self.package) as z:
                got, app_version = release.base_variant(z)
        except (ValueError, KeyError, OSError, zipfile.BadZipFile) as exc:
            got, app_version = None, f"unreadable ({type(exc).__name__}: {exc})"
        check(got == want, f"the package's modules are the {want} build its manifest names (they read as {got})")
        try:  # the worker has no fixed marker: only its sources' debug-only strings tell its build (-e note 1)
            with zipfile.ZipFile(self.package) as z:
                problems = package_variants.module_problems(z, want, ROOT)
        except (ValueError, KeyError, OSError, zipfile.BadZipFile) as exc:
            problems = [f"unreadable ({type(exc).__name__}: {exc})"]
        check(not problems, f"the four modules pass the {want} build's own check (tools/check_variant.py --sources with "
              "this tree's sources, the worker too)" + (": " + "; ".join(problems) if problems else ""))
        recorded = m.get("app_version")  # tools/package_release.py records it with the variant (-e note 3)
        check(recorded is not None and app_version == recorded,
              f"the Rubi app is v{app_version}, as the manifest's app_version v{recorded}" if recorded is not None
              else f"the Rubi app is v{app_version}, but the manifest has no app_version (tools/package_release.py "
              "records it)")
        ocr = self.pkg / "ux0/data/VitaJPOverlay/ocr"
        models = release.shipped_models()
        for name, assets, tag, role in models:
            _, size, sha = assets[name]
            path = ocr / name
            ok = path.is_file() and path.stat().st_size == size and sha256_file(path) == sha
            check(ok, f"{name} is the pinned file ({tag}, {role}; this tree's tools/prepare_ocr.py)")
        _, size, sha = release.VOCR_LM_ATTRIBUTION_PIN[release.VOCR_LM_ATTRIBUTION]
        path = ocr / "LM-ATTRIBUTION.md"
        check(path.is_file() and path.stat().st_size == size and sha256_file(path) == sha,
              "LM-ATTRIBUTION.md is the LMs' pinned attribution")
        _, size, sha = release.VOCR_MODEL_ATTRIBUTION_PIN[release.VOCR_MODEL_ATTRIBUTION]
        path = ocr / release.VOCR_MODEL_ATTRIBUTION
        check(path.is_file() and path.stat().st_size == size and sha256_file(path) == sha,
              f"{release.VOCR_MODEL_ATTRIBUTION} is VOCR5's pinned attribution, the model sizes' base "
              f"({release.VOCR_MODEL_ATTRIBUTION_TAG})")
        _, size, sha = release.VOCR6_ATTRIBUTION_PIN[release.VOCR6_ATTRIBUTION]
        path = ocr / release.VOCR6_ATTRIBUTION
        check(path.is_file() and path.stat().st_size == size and sha256_file(path) == sha,
              f"{release.VOCR6_ATTRIBUTION} is VOCR6's pinned attribution ({release.VOCR6_TAG})")
        _, size, sha = release.VOCR_GATE_ATTRIBUTION_PIN[release.VOCR_GATE_ATTRIBUTION]
        path = ocr / release.VOCR_GATE_ATTRIBUTION
        check(path.is_file() and path.stat().st_size == size and sha256_file(path) == sha,
              f"{release.VOCR_GATE_ATTRIBUTION} is the text-presence gate's pinned attribution ({release.VOCR_GATE_TAG})")
        _, size, sha = release.VOCR_VERTICAL_ATTRIBUTION_PIN[release.VOCR_VERTICAL_ATTRIBUTION]
        path = ocr / release.VOCR_VERTICAL_ATTRIBUTION
        check(path.is_file() and path.stat().st_size == size and sha256_file(path) == sha,
              f"{release.VOCR_VERTICAL_ATTRIBUTION} is the vertical model's pinned attribution ({release.VOCR5V_TAG})")
        expected = {name for name, *_ in models} | {"LM-ATTRIBUTION.md", release.VOCR_MODEL_ATTRIBUTION,
                                                    release.VOCR6_ATTRIBUTION, release.VOCR_GATE_ATTRIBUTION,
                                                    release.VOCR_VERTICAL_ATTRIBUTION}
        # API 54: the text spotter's weights (in shipped_models) and attribution, its pin published; an unpublished
        # pin ships neither (include/vjo_vocr_sizes.h VJO_VOCR_SPOT_PUBLISHED)
        spot = ocr / release.VOCR_SPOT_ATTRIBUTION
        if release.VOCR_SPOT_PUBLISHED:
            _, size, sha = release.VOCR_SPOT_ATTRIBUTION_PIN[release.VOCR_SPOT_ATTRIBUTION]
            check(spot.is_file() and spot.stat().st_size == size and sha256_file(spot) == sha,
                  f"{release.VOCR_SPOT_ATTRIBUTION} is the text spotter's pinned attribution ({release.VOCR_SPOT_TAG})")
            expected.add(release.VOCR_SPOT_ATTRIBUTION)
        else:
            check(not spot.exists() and not (ocr / release.VOCR_SPOT_MODEL).exists(),
                  f"no text spotter while its pin is unpublished ({release.VOCR_SPOT_TAG})")
        other = sorted(rel for rel in (p.relative_to(ocr).as_posix() for p in (ocr.rglob("*") if ocr.is_dir() else ())
                                       if not p.is_dir()) if rel not in expected)
        check(not other, "ocr/ has nothing but the shipped models and their attributions"
              + (f" (other files: {', '.join(other)})" if other else ""))  # -e note 2: an extra model is uploaded too

    # ---------------- scenario A: over v0.8.0-test.2 with test.1's app (the owner's state, #101) ----------------
    def scenario_a(self):
        check, pkg, settings, template = self.check, self.pkg, self.settings, self.template
        dev, run_pkg = self.fresh("A-test2")
        tai_a = (b"*KERNEL\r\nur0:tai/henkaku.skprx\r\n*main\r\nur0:tai/henkaku.suprx\r\n# VitaJPOverlay\r\n*KERNEL\r\n"
                 b"ur0:tai/VitaJPOverlay_Kernel.skprx\r\n*main\r\nur0:tai/VitaJPOverlay_Shell.suprx\r\n*PCSG00116\r\n"
                 b"# Meiki-only test: ur0:tai/VitaJPOverlay_Text.suprx\r\n*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n"
                 b"# Disabled for official upstream: ur0:tai/VitaJPOverlay_OCR.suprx\r\n*ALL\r\n"
                 b"# Mainline OCR: ur0:tai/VitaJPOverlay_OCR.suprx\r\n")
        (dev / "ur0/tai/config.txt").write_bytes(tai_a)
        # test.2's config.ini as test.1's app rewrote it (#101: ocr_backend = vocr2)
        old_cfg = (f"dictionary = local\nhachidori_host =\nlocal_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n"
                   f"local_dictionaries = plaintext.vjdict\nlocal_dictionary_results = 1\njpdb_api_key = {SECRET}\n"
                   f"jiten_api_key = {SECRET2}\nnon_japanese_filter = lines\njapanese_only_selection = on\n"
                   f"highlight_mode = word\nfont_size_ja = 20\nfont_size_en = 14\ntoggle_button = l+r\n"
                   f"subtitle_button = select+r\ntext_source = ocr\nocr_backend = vocr2\n"
                   f"ocr_model_dir = ux0:data/VitaJPOverlay/ocr\nvocr_model = VOCR3_H15_w8.vocr\n"
                   f"vocr_fallback_model = VOCR3_FL10_w8.vocr\nocr_settle_ms = 150\nocr_settle_max_ms = 2000\n"
                   f"ocr_skip_blank_lines = off\nocr_lm = 1000k\nocr_cascade_ms = 2000\nocr_refine_lines = psp\n"
                   f"ocr_mode = auto\nlog_host =\nlog_file = on\nanki_mining = on\nanki_host = 192.168.1.23\n"
                   f"anki_audio_url = {AUDIO}\n")
        (dev / "ux0/data/VitaJPOverlay/config.ini").write_text(old_cfg)
        ocr_dir = dev / "ux0/data/VitaJPOverlay/ocr"
        for name, size in (("VOCR2_FL10_w8.vocr", 657664), ("meiki-stream-int8.mnn", 4096)):
            (ocr_dir / name).write_bytes(b"\1" * size)
        # test.2's model files: this package's (unchanged) and v0.2.0's H15/FL10, which this package no longer ships
        for name in sorted(p.name for p in (pkg / "ux0/data/VitaJPOverlay/ocr").iterdir()):
            shutil.copyfile(pkg / "ux0/data/VitaJPOverlay/ocr" / name, ocr_dir / name)
        for name in ("H15_w8.vocr", "FL10_w8.vocr"):
            (ocr_dir / name).write_bytes(b"\2" * 4096)
        # where test.2's installer put its app (every install before the Rubi rename): its banner names that file
        (dev / "ux0/data/JPOverlaySettings.vpk").write_bytes(b"test.2's VPK")
        (dev / "ur0/data/VitaJPOverlay/boot.txt").write_text(
            "vjoboot1 seq=1 mode=0 stage=4 inert=0 fault=0 failures=0 api=20 prev=0/0/0/0\n")
        before = {"ur0": tai_a}

        with FtpMock(dev) as ftp:
            host = f"127.0.0.1:{ftp.port}"
            r = self.sh(run_pkg, "install-vita.sh", "--title", "PCSG00415", host)
            log = r.stdout + r.stderr
            self.save_log("A-install.log", log)
            check(r.returncode == 0 and "Installed. Reboot the Vita" in r.stdout,
                  f"A install-vita.sh --title PCSG00415: rc {r.returncode}")
            if self.worker == "kernel":
                check("--title is not needed" in r.stdout, "A install-vita.sh says --title is not needed (kernel mode)")
            if self.shown:
                check(f"package settings: {self.shown}" in r.stdout,
                      f"A install-vita.sh names the package settings: {self.shown}")
            self.no_secret(log, "A install")
            self.vpk_loud(r.stdout, "A install")
            check(not (dev / "ux0/tai/config.txt").exists(), "A no ux0:tai/config.txt is created where there was none")
            self.installed_right(dev, run_pkg, ["ur0"], before, "A")
            check((dev / "ux0/data/JPOverlaySettings.vpk").read_bytes() == (pkg / "Rubi.vpk").read_bytes(),
                  "A ux0:data/JPOverlaySettings.vpk, the pre-rename app's, is this build's app too (its old banner names it)")
            check((dev / "ux0/data/VitaJPOverlay/vjo_app.txt").read_bytes()
                  == (pkg / "ux0/data/VitaJPOverlay/vjo_app.txt").read_bytes(),
                  "A uploaded: ux0:data/VitaJPOverlay/vjo_app.txt (the plugins' record of their app)")
            self.check_app(run_pkg, host, 1, "No Rubi app is installed on the Vita", "A no app yet")
            self.install_app(dev, eboot=b"test.2's eboot.bin", stamp=b"")  # test.2's app: no stamp
            self.check_app(run_pkg, host, 1, "from an older build (v0.8.0-test.2 or earlier)", "A test.2's app")
            self.install_app(dev)  # this build's VPK installed
            self.check_app(run_pkg, host, 0, "Rubi app: up to date", "A this build's app")
            cfg = config_values((dev / "ux0/data/VitaJPOverlay/config.ini").read_text())
            want = {"vocr_tier": "normal", "log_file": "on", "ocr_mode": "auto", "dictionary": "local",
                    "font_size_ja": "20", "ocr_lm": template["ocr_lm"], "ocr_cascade_ms": template["ocr_cascade_ms"],
                    # the old file's explicit choice stays, though the default is off since 2026-10-20
                    "japanese_only_selection": "on", **settings}
            for k in ("ocr_boost_clock", "ocr_threads", "ocr_vertical", "ocr_wait_hint"):  # #93, #99, #358: new keys,
                # the template's value
                if k in template:
                    want[k] = template[k]
            for k, v in want.items():
                check(cfg.get(k) == v, f"A config.ini: {k} = {v} (got {cfg.get(k)})")
            check("vocr_model" not in cfg and "vocr_fallback_model" not in cfg,
                  "A config.ini: vocr_model/vocr_fallback_model gone (vocr_tier)")
            check("ocr_backend" not in cfg, "A config.ini: ocr_backend gone (#130: VOCR is the only engine)")
            # a setting the template still has keeps the old value; a removed one (#117 jpdb/jiten, #126 Hachidori,
            # #125 AnkiConnect, #145 the UDP log's computer) is dropped
            for k, old_v in (("jpdb_api_key", SECRET), ("jiten_api_key", SECRET2), ("hachidori_host", ""),
                             ("anki_host", "192.168.1.23"), ("anki_audio_url", AUDIO), ("log_host", "")):
                if k in template:
                    check(cfg.get(k) == old_v, f"A config.ini: {k} kept (the template still has it)")
                else:
                    check(k not in cfg, f"A config.ini: {k} dropped (not in the template any more)")
            self.self_test(dev, run_pkg, host)
            self.session_a(dev)
            snapshot = {p: p.read_bytes() for p in dev.rglob("*") if p.is_file()}
            n = len(ftp.stored)
            r = self.sh(run_pkg, "tools/install_ftp.sh", "--collect", host)
            self.save_log("A-collect.log", r.stdout + r.stderr)
            check(r.returncode == 0, f"A --collect rc {r.returncode}")
            self.no_secret(r.stdout + r.stderr, "A --collect output")
            check(ftp.stored[n:] == [] and {p: p.read_bytes() for p in dev.rglob("*") if p.is_file()} == snapshot,
                  "A --collect wrote nothing on the Vita")
            self.collected_a(run_pkg, r.stdout)
            r = self.sh(run_pkg, "tools/install_ftp.sh", "--uninstall", host)
            t = (dev / "ur0/tai/config.txt").read_bytes()
            check(r.returncode == 0 and not any(x.startswith(b"ur0:tai/VitaJPOverlay_") for x in t.split(b"\r\n"))
                  and b"henkaku" in t, "A --uninstall removes our live lines, keeps henkaku")
            data = dev / "ux0/data/VitaJPOverlay"
            check((data / "config.ini").is_file() and (data / "log.txt").is_file(),
                  "A --uninstall keeps the data (config.ini, logs)")

    def self_test(self, dev, run_pkg, host):
        """install_ftp.sh --self-test arms the marker in either build; from the normal build's package (its manifest's
        variant) it also says that the normal build has no self-test, and from any other it does not."""
        r = self.sh(run_pkg, "tools/install_ftp.sh", "--self-test", host)
        self.check(r.returncode == 0 and (dev / "ux0/data/VitaJPOverlay/selftest.txt").read_bytes() == b"",
                   "A --self-test armed the marker")
        note = "Note: this package is the normal build, which has no self-test." in r.stdout
        self.check(note == (self.variant == "normal"),
                   f"A --self-test {'says' if self.variant == 'normal' else 'does not say'} that the normal build has no "
                   f"self-test (the {self.variant or 'unnamed'} build)")

    def session_a(self, dev):
        """A console session, simulated with the tree's self-test fixture and the plugins' log formats."""
        api = self.api
        d = dev / "ux0/data/VitaJPOverlay"
        shutil.copyfile(FIXTURE, d / "selftest-report.txt")
        (d / "selftest.txt").unlink()
        # The log lines' formats are today's; their figures are an older build's (VOCR3-H15, Normal's 8 MiB reserve
        # of API 21 until #280: 1 MiB for every size since), which the analyser reads line by line as it reads any.
        (d / "log.txt").write_text(
            f"bootstrap: shell API {api} entered\nRubi shell started (kernel API {api})\nbootstrap: worker started\n"
            "game PCSG00415 started: dictionary local (key MISSING), trigger l+r, subtitles select+r, ocr_mode auto\n"
            "tier large did not fit in PCSG00415 (need 18649088, free 12000000); using normal\n"
            "game vocr rc=0 VOCR3_H15_w8.vocr: 917x125 at 22,400 of 960x544 fmt 0x00000000, lines=3 USER workspace=188416 "
            "weights=read resident=1159168, find=9 ms load=1050 ms recognize=820 ms frames=330 io=0x00000000 "
            "stat=0x00000000 cleanup=0\n"
            "game vocr memory: free USER=91226112, needs workspace 249856 + weights 1159168 + reserve 8388608: check "
            "passed, headroom 81428480\n"
            "game vocr speed: 330 frames in 1683000 us, 5100 us per frame\n"
            "job 1 done rc=0 in 2400 ms, arena peak 65536\n"
            "anki: queue 1 card(s)\nanki: save 猫 rc=0, 0-byte screenshot, 41 ms\n"
            "Rubi app: anki: export (queued) rc=0: 2 cards, 2 moved\n", encoding="utf-8")
        # the boot the collect dates its crash dump against (docs/self-test.md#crash-dumps): it began an hour ago, after
        # the previous boot's last kernel.txt write; its ok mark came 70 s in; kernel.txt's last line, 50 minutes in,
        # carries the uptime, which checks the Vita's clock
        (d / "kernel.txt").write_text("boot guard: empty record (reset by the installer): first boot\n"
                                      "boot guard: boot 1 normal (previous: none, stage none, failures 0) write "
                                      "0x00000000\n"
                                      f"Rubi kernel {api} started (normal mode)\n"
                                      "shell registered: pid 0x118FF (was 0x0)\n"
                                      "ocr worker registered: game pid 0x20000 module 0x30000\n"
                                      "foreground 0x20000 -> 0x0 (suspend) t=3000000 ms\ngame exit\n")
        (d / "kernel.prev.txt").write_text(f"Rubi kernel {api} started (normal mode)\n")
        # the debug build's record of the game's hooks (game_ocr/hooks.c's lines, no text): --collect fetches it and
        # the package's tools/hook_request.py reads it (docs/hook-requests.md)
        (d / "hooks.log").write_text(
            "PCSG00415: text source 1\n"
            "main module game NID C5187D60 seg0 81000000+1C4000 APP_VER 0100; 1 verified, 0 Luna sites\n"
            "verified +2CA70 thumb r6 hooked, stream 1\n"
            + "".join(f"import {lib}/{nid}: {'hooked' if k < 6 else 'not hooked'}\n" for k, (lib, nid) in enumerate((
                ("BE43BB07", "7205BFDB"), ("BE43BB07", "85B924B7"), ("BE43BB07", "9F87712D"), ("BE43BB07", "AF5C218D"),
                ("CAE9ACE6", "14E9DBD7"), ("CAE9ACE6", "C458D60A"), ("CAE9ACE6", "2CDFCD1C"), ("CAE9ACE6", "AC595E68"))))
            + "sites hooked 1, refused 0; CALL imports hooked 6 of 8\n")
        (dev / "ur0/data/VitaJPOverlay/boot.txt").write_text(
            f"vjoboot1 seq=1 mode=0 stage=4 inert=0 fault=0 failures=0 api={api} prev=0/0/0/0\n")
        start = time.time() - 3600
        for path, at in ((d / "kernel.prev.txt", start - 30), (dev / "ur0/data/VitaJPOverlay/boot.txt", start + 70),
                         (d / "kernel.txt", start + 3000)):
            os.utime(path, (at, at))
        (d / "anki_queue").mkdir()
        (d / "anki_queue/0123456789abcdef0123456789abcdef.json").write_text('{"version":3,"spelling":"猫"}\n',
                                                                            encoding="utf-8")
        # #125: the app's export, two cards moved out of the queue, one package
        (d / "anki_queue/exported").mkdir()
        for n in ("1", "2"):
            (d / f"anki_queue/exported/{n * 32}.json").write_text('{"version":3,"spelling":"犬"}\n', encoding="utf-8")
        (d / "anki").mkdir()
        (d / "anki/2026-10-16_120000-2cards.apkg").write_bytes(b"PK\5\6" + b"\0" * 18)
        (dev / "ux0/data/psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp").write_bytes(b"\0" * 2048)

    def collected_a(self, run_pkg, a):
        """--collect's one zip and the analysis it printed (a)."""
        check = self.check
        zips = sorted((run_pkg / "build/device/collect").glob("vjo-collect-*.zip"))
        check(len(zips) == 1 and f"Send this one file: {zips[0]}" in a, "A one timestamped zip, named at the end")
        if len(zips) != 1:
            return
        with zipfile.ZipFile(zips[0]) as z:
            names, blob = set(z.namelist()), b"".join(z.read(x) for x in z.namelist())
        top = zips[0].stem + "/"
        for rel in ("ux0/data/VitaJPOverlay/log.txt", "ux0/data/VitaJPOverlay/selftest-report.txt",
                    "ux0/data/VitaJPOverlay/kernel.txt", "ux0/data/VitaJPOverlay/config.ini",
                    "ux0/data/VitaJPOverlay/anki_queue/0123456789abcdef0123456789abcdef.json",
                    "ux0/data/VitaJPOverlay/anki_queue/exported/" + "1" * 32 + ".json",
                    "ux0/app/VJOSET001/vjo_app.txt", "ur0/data/VitaJPOverlay/boot.txt", "ur0/tai/config.txt",
                    "ur0/tai/config.txt.vjo-bak", "ux0/data/psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp",
                    "ux0/data/VitaJPOverlay/hooks.log", "COLLECT.json", "analysis.txt", "analysis.json", "timeline.txt",
                    "hook-request.md"):
            check(top + rel in names, f"A in the zip: {rel}")
        check("hooks.log: PCSG00415, APP_VER 01.00, main module game (NID 0xC5187D60): Rubi's verified entry is installed "
              "in this game; 6 string-copy imports hooked. Report: " in a, "A --collect prints the hook request's line "
              "(the package's tools/hook_request.py)")
        check(not any(x.endswith(".apkg") for x in names) and "2 exported card(s), 1 .apkg package(s) in anki/" in a,
              "A the zip lists the .apkg packages (the analysis counts 1) but does not copy them")
        check(not any(s.encode() in blob for s in SECRETS), "A no credential is in any file of the zip")
        m = re.search(r"installed files: (\d+) of (\d+) match the package", a)
        # the hashed files (plugins, RCO, weights, LMs, the attributions; API 54: the spotter's .vtsp), the VPK, and
        # the dictionary by size
        n_hashed = sum(1 for x in self.device_files
                       if x.endswith((".vocr", ".vlm", ".vtd", ".vtsp", "-ATTRIBUTION.md", ".skprx", ".suprx", ".rco"))) + 2
        check(m and m.group(1) == m.group(2) and int(m.group(1)) == n_hashed,
              f"A analysis: {m.group(0) if m else 'no installed-files line'} (want {n_hashed} of {n_hashed})")
        check("installed files:" in a and not re.search(r"DIFFER|absent", a.split("installed files:")[1].split("\n\n")[0]),
              "A analysis: no installed file DIFFERs or is absent")
        worker_text = ("no OCR worker line (the kernel loads it)" if self.worker == "kernel"
                       else "OCR worker under ['PCSG00415']")
        install_text = ("one-VPK install (installed.txt): " if (self.pkg / "payload-manifest.txt").is_file()
                        else "one-VPK install: no installed.txt (")
        for want_text in (worker_text, install_text, "PASS: every stage completed", "memory headroom per stage",
                          "recommendation:", "not decided here",
                          # the dump (2025-10-09 by its name) is placed before this boot (#138)
                          "crash dumps: 1 on the card, none from this boot, 1 from before it;",
                          "before this boot 2025-10-09 08:53:20  an app or game (eboot.bin) pid 0x1234, 2,048 B: "
                          "psp2core-1760000000-0x0000001234-eboot.bin.psp2dmp",
                          "To archive them off the Vita: bash tools/install_ftp.sh --archive-dumps 127.0.0.1",
                          "model sizes that did not fit (log only, never shown): large in PCSG00415 x1",
                          "Rubi app: the plugins' (vjoapp1 "):
            check(want_text in a, f"A analysis says: {want_text}")
        m = re.search(r"Anki: (\d+) save\(s\)[^\n]*", a)
        check(m and m.group(1) == "1", f"A analysis: {m.group(0) if m else 'no Anki line'}")
        check("Anki exports (the Rubi app): 1 export(s): 1 succeeded (a package written and checked), 0 failed; "
              "2 card(s) in the packages" in a, "A analysis: the .apkg export counted (#125, #142)")

    # ---------------- scenario B: a fresh henkaku Vita with an *ALL worker line ----------------
    def scenario_b(self):
        check = self.check
        dev, run_pkg = self.fresh("B-fresh")
        tai_b = (b"*KERNEL\r\nur0:tai/henkaku.skprx\r\n*main\r\nur0:tai/henkaku.suprx\r\n"
                 b"*ALL\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n")
        (dev / "ur0/tai/config.txt").write_bytes(tai_b)
        (dev / "ux0/tai/config.txt").write_bytes(tai_b)
        with FtpMock(dev) as ftp:
            host = f"127.0.0.1:{ftp.port}"
            r = self.sh(run_pkg, "install-vita.sh", "--title", "PCSG00415", host)
            self.save_log("B-install.log", r.stdout + r.stderr)
            check(r.returncode == 0 and "Installed. Reboot the Vita" in r.stdout,
                  f"B install-vita.sh --title PCSG00415: rc {r.returncode}")
            self.vpk_loud(r.stdout, "B install")
            self.installed_right(dev, run_pkg, ["ur0", "ux0"], {"ur0": tai_b, "ux0": tai_b}, "B")
            check(not (dev / "ux0/data/JPOverlaySettings.vpk").exists(),
                  "B no ux0:data/JPOverlaySettings.vpk is created where there was none (the name before the rename)")
            for part in ("ur0", "ux0"):
                t = (dev / part / "tai/config.txt").read_bytes()
                if self.worker == "kernel":
                    check(b"VitaJPOverlay_OCR" not in t and t.endswith(b"*ALL\r\n"),
                          f"B {part}: the *ALL worker line is removed, the empty *ALL kept")
                else:
                    check(b"*ALL\r\n# Mainline OCR: ur0:tai/VitaJPOverlay_OCR.suprx" in t,
                          f"B {part}: the *ALL worker line is commented out")
            cfg = config_values((dev / "ux0/data/VitaJPOverlay/config.ini").read_text())
            want = {"vocr_tier": "normal", "log_file": "on", "japanese_only_selection": "off", **self.settings}
            if "ocr_boost_clock" in self.template:
                want["ocr_boost_clock"] = self.template["ocr_boost_clock"]
            check(all(cfg.get(k) == v for k, v in want.items()), f"B config.ini: {want}")

    # ---------------- scenario C: an older model choice and dictionary migrated ----------------
    def scenario_c(self):
        check = self.check
        dev, run_pkg = self.fresh("C-migrate")
        tai_c = (b"*KERNEL\r\nur0:tai/henkaku.skprx\r\nur0:tai/VitaJPOverlay_Kernel.skprx\r\n*main\r\nur0:tai/henkaku.suprx\r\n"
                 b"ur0:tai/VitaJPOverlay_Shell.suprx\r\n*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx\r\n")
        (dev / "ur0/tai/config.txt").write_bytes(tai_c)
        (dev / "ux0/data/VitaJPOverlay/config.ini").write_text(
            f"dictionary = jpdb\njpdb_api_key = {SECRET}\nocr_backend = vocr\nvocr_model = VOCR4_W20_w8.vocr\n"
            "vocr_fallback_model = VOCR3_FL10_w8.vocr\nlog_file = off\nanki_mining = off\nhighlight_mode = character\n")
        with FtpMock(dev) as ftp:
            host = f"127.0.0.1:{ftp.port}"
            r = self.sh(run_pkg, "install-vita.sh", host)
            self.save_log("C-install.log", r.stdout + r.stderr)
            check(r.returncode == 0 and "Installed. Reboot the Vita" in r.stdout,
                  f"C install-vita.sh (no --title; a registered *PCSG00415): rc {r.returncode}")
            self.no_secret(r.stdout + r.stderr, "C install")
            t = (dev / "ur0/tai/config.txt").read_bytes()
            if self.worker == "kernel":
                check(b"VitaJPOverlay_OCR" not in t and b"*PCSG00415" not in t,
                      "C the registered game's worker line goes, and the section it was alone in")
            else:
                check(t.split(b"\r\n").count(b"ur0:tai/VitaJPOverlay_OCR.suprx") == 1
                      and b"*PCSG00415\r\nur0:tai/VitaJPOverlay_OCR.suprx" in t,
                      "C the registered game is kept without --title")
            cfg = config_values((dev / "ux0/data/VitaJPOverlay/config.ini").read_text())
            want = {"vocr_tier": "large", "dictionary": "local", "log_file": "on", **self.settings}
            for k, v in want.items():
                check(cfg.get(k) == v, f"C config.ini: {k} = {v} (got {cfg.get(k)})")
            check("vocr_model" not in cfg and "vocr_fallback_model" not in cfg, "C config.ini: vocr_model -> vocr_tier = large")
            check("ocr_backend" not in cfg, "C config.ini: ocr_backend gone (#130)")
            if "jpdb_api_key" not in self.template:
                check("jpdb_api_key" not in cfg, "C config.ini: jpdb_api_key dropped (#117)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", type=Path, help="A release zip (tools/package_release.py or package_variants.py)")
    parser.add_argument("--work", type=Path, help="Working directory (default: build/e2e-package/<the zip's name>); "
                        "its other files are never touched")
    parser.add_argument("--keep", action="store_true", help="Keep the unpacked package and the fake Vitas after a pass")
    args = parser.parse_args()
    try:
        with zipfile.ZipFile(args.package) as z:
            names = set(z.namelist())
    except (OSError, zipfile.BadZipFile) as e:
        sys.exit(f"e2e_package: {args.package}: not a zip ({e})")
    if "package-manifest.json" not in names:
        sys.exit(f"e2e_package: {args.package.name} has no package-manifest.json: a base zip (the Vita build's own, "
                 "a CI artifact)? Package it with tools/package_release.py or tools/package_variants.py first.")
    default = (ROOT / "build/e2e-package" / args.package.stem).resolve()
    work = (args.work or default).resolve()
    problem = prepare_work(work, args.package, default=work == default)  # -e note 9: --work may name it too
    if problem:
        sys.exit(f"e2e_package: {problem}")
    others = {p.name for p in work.iterdir()} - set(ENTRIES) - {MARKER}  # the caller's: never touched
    pkg = work / "pkg"
    unpack(args.package, pkg)
    try:
        e = E2E(pkg, work, args.package)
    except (OSError, KeyError, ValueError, AttributeError) as exc:  # no config.ini, no app record, a bad manifest
        sys.exit(f"e2e_package: {args.package.name} is not a release package ({type(exc).__name__}: {exc}); "
                 f"its files are in {pkg}")
    m = e.manifest
    print(f"e2e_package: {args.package.name}: the {e.variant or 'unnamed'} build, v{m.get('version')}, source "
          f"{m.get('source_revision', 'not recorded')}, API {e.api}, install settings: {e.shown or 'none'}", flush=True)
    for scenario in (e.package_checks, e.scenario_a, e.scenario_b, e.scenario_c):
        try:
            scenario()
        except Exception as exc:  # a scenario that stops half way is one failure, and the next still runs
            e.check(False, f"{scenario.__name__} stopped: {type(exc).__name__}: {exc}")
    left = {p.name for p in work.iterdir()} - {MARKER}
    e.check(left - set(ENTRIES) == others,
            f"the e2e made nothing in {work.name}/ but its own entries, and left the {len(others)} other(s) there")
    failed = [w for ok, w in e.checks if not ok]
    if not failed and not args.keep:
        for name in ("pkg", *SCENARIOS):
            if os.path.lexists(work / name):
                remove(work / name)
    print(json.dumps({"package": args.package.name, "variant": e.variant, "checks": len(e.checks), "failed": failed},
                     ensure_ascii=False))
    if failed:
        print(f"e2e_package: {len(failed)} of {len(e.checks)} checks failed; kept for inspection: {work}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
