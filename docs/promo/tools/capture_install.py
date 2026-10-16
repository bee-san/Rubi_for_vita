#!/usr/bin/env python3
"""Captures what main's install-vita.sh prints during a first install, for scene 6's terminal.

    tools/capture_install.sh                      # writes src/install-transcript.txt
    tools/capture_install.sh --base-zip BASE.zip  # also checks the file list against a real base zip

The installer's output depends on the package's file list and on the console's files, not on what
the files contain. So this runs the repository's own tools/install-vita.sh, unchanged, against:

- a package with the release's file names. The text files (README, docs, tools, the config
  template) are the repository's own. The plugins, the RCO, the app's VPK, the dictionary and the
  models are small stand-ins, and package-manifest.json lists them all, as tools/package_release.py
  does;
- a local FTP server (pyftpdlib) laid out as VitaShell serves a console just set up with h-encore:
  its two stock taiHEN configs (h-encore bootstrap/bootstrap.c at 819b6cc writes
  ux0:tai/config.txt and ur0:tai/config.txt), no Overlay app, nothing of ours.

tools/capture_install.sh runs this inside a private user and network namespace (`unshare -rn`),
so the server can use the video's address, 192.168.1.42:1337. Nothing leaves the machine.
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

PROMO = Path(__file__).resolve().parent.parent
REPO = PROMO.parent.parent
HOST, PORT = "192.168.1.42", 1337
TITLE = "HSDY00001"  # the mock game's title ID

# The release's base zip (vita/CMakeLists.txt's release target, as CI built it on ac905b1): the
# repository's own files, then the stand-ins.
REPO_FILES = [
    "LICENSE", "README.md",
    "docs/JMdict-LICENSE.html", "docs/anki.md", "docs/jmdict-source.json", "docs/lens-performance.md",
    "docs/local-dictionaries.md", "docs/local-ocr.md", "docs/lookup-performance.md",
    "docs/plaintext-attribution.txt", "docs/plaintext-dictionaries.json", "docs/project-lessons.md",
    "docs/settings-png-runtime.md", "docs/settings-readability.md", "docs/todo-20261011.md",
    "docs/touch-lookup.md", "docs/vita-vn-ocr.md",
    "docs/benchmarks/daijirin-import-20261010.json", "docs/benchmarks/daijirin-v3-selected-20261011.json",
    "docs/benchmarks/kernel-startup-20261010.json", "docs/benchmarks/lens-connections-20261010.json",
    "docs/benchmarks/local-character-memory-20261011.json", "docs/benchmarks/local-lookup-jitendex.json",
    "docs/benchmarks/local-result-memory-20261011.json", "docs/benchmarks/vocr-release-20261011.json",
    "docs/benchmarks/vocr-vs-ncnn-20261010.json", "docs/benchmarks/vocr2-candidate-20261012.json",
    "third_party/README.md", "third_party/licenses/PaddleOCR.txt", "third_party/licenses/libpng.txt",
    "third_party/licenses/ncnn.txt",
    "tools/analyse_selftest.py", "tools/analyse_timeline.py", "tools/app_stamp.py", "tools/collect_vita.py",
    "tools/convert_dictionary.py", "tools/install_dictionary.py", "tools/install_ftp.sh",
    "tools/migrate_config.py", "tools/model_tiers.json", "tools/package_release.py",
    "tools/patch_tai_config.py", "tools/prepare_jmdict.py", "tools/prepare_ocr.py", "tools/prepare_plaintext.py",
]
RENAMED = {  # package name: repository file
    "config.ini": "docs/config.example.ini",
    "third_party/licenses/BearSSL-MIT.txt": "third_party/bearssl/LICENSE.txt",
    "third_party/licenses/jsmn-MIT.txt": "third_party/jsmn/LICENSE",
}
STAND_INS = [
    "JPOverlaySettings.vpk", "ur0/data/VitaJPOverlay/vitajpoverlay.rco",
    "ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_OCR.suprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
    "ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict", "ux0/data/VitaJPOverlay/vjo_app.txt",
]
# tools/package_release.py's additions: install-vita.sh, the second config.ini copy, the three
# model sizes' recognizers (tools/model_tiers.json), the four LMs and their attribution.
LMS = ["vocr_lm_o3_500k.vlm", "vocr_lm_o3_1000k.vlm", "vocr_lm_o3_2000k.vlm", "vocr_lm_o3_4000k.vlm"]

# h-encore bootstrap/bootstrap.c (819b6cc): reset_taihen_config()'s two files.
HEADER = ("# For users plugins, you must refresh taiHEN from HENkaku Settings for\n"
          "# changes to take place.\n"
          "# For kernel plugins, you must reboot for changes to take place.\n")
CONFIG = ("*KERNEL\n"
          "# henkaku.skprx is hard-coded to load and is not listed here\n"
          "*main\n"
          "# main is a special titleid for SceShell\n"
          "ur0:tai/henkaku.suprx\n"
          "*NPXS10015\n"
          "# this is for modifying the version string\n"
          "ur0:tai/henkaku.suprx\n"
          "*NPXS10016\n"
          "# this is for modifying the version string in settings widget\n"
          "ur0:tai/henkaku.suprx\n")
RECOVERY = "# This file is used as an alternative if ux0:tai/config.txt is not found.\n"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_package(root, base_names=None):
    tiers = json.loads((REPO / "tools/model_tiers.json").read_text())
    models = [t["model"] for t in tiers["tiers"]]
    base = REPO_FILES + list(RENAMED) + STAND_INS
    if base_names is not None and sorted(base) != sorted(base_names):
        sys.exit("the base zip's files differ from this list: %s" % sorted(set(base) ^ set(base_names)))
    for name in REPO_FILES:
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / name, root / name)
    for name, src in RENAMED.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / src, root / name)
    sys.path.insert(0, str(REPO / "tools"))
    import app_stamp  # the app's version stamp, as the Vita build writes it
    eboot = b"SCE\0stand-in Overlay app eboot.bin\n"
    api = app_stamp.api_version(REPO / "include/vjo_api.h")
    stamp = app_stamp.make(eboot, api)
    stamp = stamp if isinstance(stamp, str) else stamp.decode()
    for name in STAND_INS:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if name == "JPOverlaySettings.vpk":
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("eboot.bin", eboot)
                z.writestr("vjo_app.txt", stamp)
        elif name.endswith("vjo_app.txt"):
            path.write_text(stamp)
        else:
            path.write_bytes(b"SCE\0stand-in " + name.encode() + b"\n")
    extra = {"install-vita.sh": REPO / "tools/install-vita.sh",
             "ux0/data/VitaJPOverlay/config.ini": REPO / "docs/config.example.ini"}
    for name, src in extra.items():
        shutil.copyfile(src, root / name)
    ocr = root / "ux0/data/VitaJPOverlay/ocr"
    ocr.mkdir(parents=True, exist_ok=True)
    for name in models + LMS + ["LM-ATTRIBUTION.md"]:
        (ocr / name).write_bytes(b"stand-in " + name.encode() + b"\n")
    os.chmod(root / "install-vita.sh", 0o755)
    files = {}
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        files[rel] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    (root / "package-manifest.json").write_text(json.dumps({"version": "0.7.12", "files": files}, indent=2) + "\n")
    return len(files)


def make_console(ftp_root):
    for path, text in (("ux0:/tai/config.txt", HEADER + CONFIG), ("ur0:/tai/config.txt", RECOVERY + HEADER + CONFIG)):
        target = ftp_root / path.replace(":/", ":/", 1)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    (ftp_root / "ux0:/data").mkdir(parents=True, exist_ok=True)
    (ftp_root / "ur0:/data").mkdir(parents=True, exist_ok=True)


def serve(ftp_root):
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
    auth = DummyAuthorizer()
    auth.add_anonymous(str(ftp_root), perm="elradfmwMT")
    FTPHandler.authorizer = auth
    FTPHandler.banner = "VitaShell stand-in"
    server = FTPServer((HOST, PORT), FTPHandler)
    threading.Thread(target=server.serve_forever, kwargs={"timeout": 0.2}, daemon=True).start()
    return server


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-zip", type=Path, help="check the file list against this release base zip")
    ap.add_argument("-o", "--output", type=Path, default=PROMO / "src/install-transcript.txt")
    args = ap.parse_args()
    base_names = None
    if args.base_zip:
        with zipfile.ZipFile(args.base_zip) as z:
            base_names = [i.filename for i in z.infolist() if not i.is_dir()]
    import logging
    import warnings
    logging.disable(logging.CRITICAL)  # pyftpdlib's own log is not the installer's output
    warnings.filterwarnings("ignore", message="write permissions assigned to anonymous user")
    with tempfile.TemporaryDirectory(prefix="vjo-capture-") as tmp:
        tmp = Path(tmp)
        pkg, ftp_root = tmp / "VitaJPOverlay", tmp / "vita"
        pkg.mkdir()
        ftp_root.mkdir()
        count = make_package(pkg, base_names)
        make_console(ftp_root)
        server = serve(ftp_root)
        # The installer names its backup folder by `date` (install_ftp.sh): the film's time,
        # 2026-10-15 14:21:12, so it fits the story; mktemp still picks its 6-letter suffix.
        shim = tmp / "bin"
        shim.mkdir()
        real_date = shutil.which("date")
        (shim / "date").write_text("#!/bin/sh\n"
                                   '[ "$1" = "+%Y%m%d-%H%M%S" ] && { echo 20261015-142112; exit 0; }\n'
                                   f'exec {real_date} "$@"\n')
        os.chmod(shim / "date", 0o755)
        cmd = ["bash", "install-vita.sh", "--title", TITLE, HOST]
        env = dict(os.environ, VJO_NO_PROMPT="1", PATH=f"{shim}:{os.environ['PATH']}")
        run = subprocess.run(cmd, cwd=pkg, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=600)
        server.close_all()
        text = "$ " + " ".join(cmd) + "\n" + run.stdout
        args.output.write_text(text)
        uploaded = sorted(p.relative_to(ftp_root).as_posix() for p in ftp_root.rglob("*") if p.is_file())
    print(f"package: {count} files; installer exit {run.returncode}; {len(text.splitlines())} lines -> {args.output}")
    print("on the console afterwards:", len(uploaded), "files")


if __name__ == "__main__":
    main()
