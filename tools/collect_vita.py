#!/usr/bin/env python3
"""Collect everything the overlay wrote on the Vita, read-only, into one folder (install_ftp.sh --collect).

    python3 tools/collect_vita.py VITA_IP PORT OUT_DIR [--manifest package-manifest.json]

Downloads, over VitaShell's FTP (VitaShell: SELECT -> FTP), with curl as the installer does:
  ux0:data/VitaJPOverlay/  log.txt log.old.txt selftest-report.txt selftest.txt status.txt
                           status.prev.txt kernel.txt kernel.prev.txt config.ini (API keys
                           redacted before anything is written) games.ini region.ini anki_host.txt
                           anki_queue/* (every card, screenshot, temp and duplicate file)
  ur0:data/VitaJPOverlay/  boot.txt
  ur0:tai/, ux0:tai/       config.txt and the installer's config.txt.vjo-bak
  ux0:data/                every *.psp2dmp crash dump
The directories are listed (Python's ftplib, LIST as --dumps does) into listings/; the
installed plugins, the RCO, the settings VPK and every OCR model are hashed (downloaded to a
temporary file and deleted again, never kept) and compared with package-manifest.json when
given. Every file keeps its device path under OUT_DIR (ux0/data/VitaJPOverlay/log.txt, ...).
COLLECT.json lists what was fetched (bytes, sha256), what was missing and every error.

Nothing on the Vita is written, renamed or deleted: only CWD, LIST, SIZE and RETR are sent.
A file that cannot be read is recorded and skipped; the exit status is 1 only when the Vita
cannot be reached at all.
"""
import argparse
import ftplib
import hashlib
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DATA = "ux0:/data/VitaJPOverlay"
FILES = [f"{DATA}/{n}" for n in ("log.txt", "log.old.txt", "selftest-report.txt", "selftest.txt", "status.txt",
                                  "status.prev.txt", "kernel.txt", "kernel.prev.txt", "config.ini", "games.ini",
                                  "region.ini", "anki_host.txt")]
FILES += ["ur0:/data/VitaJPOverlay/boot.txt", "ur0:/tai/config.txt", "ur0:/tai/config.txt.vjo-bak",
          "ux0:/tai/config.txt", "ux0:/tai/config.txt.vjo-bak"]
# Listed, and what to do with their files: "keep" downloads them, "hash" hashes them only.
DIRS = [(f"{DATA}/anki_queue", "keep", None), (DATA, None, None), (f"{DATA}/ocr", "hash", re.compile(r"\.vocr$")),
        (f"{DATA}/dictionaries", None, None), ("ux0:/data", "keep", re.compile(r"\.psp2dmp$", re.I)),
        ("ur0:/tai", "hash", re.compile(r"^VitaJPOverlay_.*\.(skprx|suprx)$")),
        ("ur0:/data/VitaJPOverlay", "hash", re.compile(r"\.rco$")), ("ux0:/data", "hash", re.compile(r"^JPOverlaySettings\.vpk$"))]
# Device paths of the hashed files -> their package path (package-manifest.json "files")
PACKAGE_PATHS = {"ur0:/tai/": "ur0/tai/", "ur0:/data/VitaJPOverlay/": "ur0/data/VitaJPOverlay/",
                 f"{DATA}/ocr/": "ux0/data/VitaJPOverlay/ocr/", "ux0:/data/JPOverlaySettings.vpk": "JPOverlaySettings.vpk"}
SECRET = re.compile(r"^(\s*)([A-Za-z0-9_]*api_key)(\s*=\s*)(.*?)\s*$", re.I)


def local_path(out, device_path):
    dev, _, rest = device_path.partition(":/")
    return out / dev / rest


def redact(text):
    """config.ini with every *api_key value replaced by its length; the keys that were set."""
    out, keys = [], []
    for line in text.splitlines():
        m = SECRET.match(line)
        if m and m.group(4):
            keys.append(m.group(2).lower())
            line = f"{m.group(1)}{m.group(2)}{m.group(3)}<redacted: {len(m.group(4))} characters>"
        out.append(line)
    return "\n".join(out) + ("\n" if text.endswith("\n") else ""), keys


class Collector:
    def __init__(self, host, port, out, timeout=60):
        self.host, self.port, self.out, self.timeout = host, port, out, timeout
        self.record = {"collected_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "host": f"{host}:{port}",
                       "tool": "tools/collect_vita.py", "files": {}, "missing": [], "errors": [], "listings": {},
                       "hashed": {}, "redacted_keys": []}
        self.ftp = None

    # ---- transport ----
    def curl(self, device_path, dest):
        """RETR through curl, exactly as install_ftp.sh's get(); False when the file is absent or unreadable."""
        dest.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(["curl", "--fail", "-s", "--show-error", "--max-time", str(self.timeout), "-o", str(dest),
                            f"ftp://{self.host}:{self.port}/{device_path}"], capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, timeout=self.timeout + 30)
        if r.returncode != 0:
            dest.unlink(missing_ok=True)
            return r.returncode, r.stderr.strip()
        return 0, ""

    def connect(self):
        if self.ftp is None:
            self.ftp = ftplib.FTP()
            self.ftp.connect(self.host, self.port, timeout=20)
            self.ftp.login()
        return self.ftp

    def listdir(self, device_path):
        """("ok", LIST lines) of a directory, trying VitaShell's path spellings; ("missing", reason) when no
        spelling can be entered (the directory is not there); ("error", reason) when it was entered and could
        not be listed, or the connection failed."""
        last = None
        for attempt in range(2):
            try:
                f = self.connect()
                entered = False
                for p in ("/" + device_path, device_path, "/" + device_path + "/"):
                    lines = []
                    try:
                        f.cwd(p)
                    except ftplib.error_perm as e:
                        last = str(e)
                        continue
                    entered = True
                    try:
                        f.retrlines("LIST", lines.append)
                    except ftplib.error_perm as e:
                        last = str(e)
                        continue
                    f.cwd("/")
                    return "ok", lines
                return ("error" if entered else "missing"), last
            except (OSError, EOFError, ftplib.Error) as e:  # a dropped control connection: reconnect once
                last = f"{type(e).__name__}: {e}"
                self.ftp = None
        return "error", last

    @staticmethod
    def parse_list(lines):
        entries = []
        for l in lines:
            parts = l.split()
            if len(parts) < 9:
                continue
            size = int(parts[4]) if parts[4].isdigit() else None
            entries.append({"name": " ".join(parts[8:]), "bytes": size, "dir": l.startswith("d")})
        return entries

    # ---- collection ----
    def fetch(self, device_path):
        dest = local_path(self.out, device_path)
        if device_path.endswith("/config.ini"):
            with tempfile.TemporaryDirectory(prefix="vjo-collect-") as tmp:
                raw = Path(tmp) / "config.ini"
                rc, err = self.curl(device_path, raw)
                if rc:
                    return self.absent(device_path, rc, err)
                text, keys = redact(raw.read_text(encoding="utf-8-sig", errors="replace"))
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(text, encoding="utf-8")
            self.record["redacted_keys"] = keys
        else:
            rc, err = self.curl(device_path, dest)
            if rc:
                return self.absent(device_path, rc, err)
        data = dest.read_bytes()
        self.record["files"][device_path] = {"local": str(dest.relative_to(self.out)), "bytes": len(data),
                                             "sha256": hashlib.sha256(data).hexdigest()}
        return True

    def absent(self, device_path, rc, err):
        # curl 78 (RETR 550) / 19 / 9: the file is not there; anything else is an error worth reading
        if rc in (9, 19, 78):
            self.record["missing"].append(device_path)
        else:
            self.record["errors"].append(f"{device_path}: curl exit {rc}: {err}")
        return False

    def hash_remote(self, device_path):
        with tempfile.TemporaryDirectory(prefix="vjo-collect-") as tmp:
            tmpfile = Path(tmp) / "f"
            rc, err = self.curl(device_path, tmpfile)
            if rc:
                self.record["errors"].append(f"{device_path}: not hashed, curl exit {rc}: {err}")
                return
            h = hashlib.sha256()
            with tmpfile.open("rb") as s:
                for block in iter(lambda: s.read(1 << 20), b""):
                    h.update(block)
            self.record["hashed"][device_path] = {"bytes": tmpfile.stat().st_size, "sha256": h.hexdigest()}

    def run(self):
        for p in FILES:
            self.fetch(p)
        listing_dir = self.out / "listings"
        listing_dir.mkdir(parents=True, exist_ok=True)
        listed = {}
        for device_path, action, pattern in DIRS:
            if device_path not in listed:
                status, lines = self.listdir(device_path)
                if status == "missing":
                    self.record["missing"].append(device_path + "/")
                    listed[device_path] = None
                elif status == "error":
                    self.record["errors"].append(f"{device_path}/: LIST failed: {lines}")
                    listed[device_path] = None
                else:
                    name = re.sub(r"[^A-Za-z0-9]+", "_", device_path).strip("_") + ".txt"
                    (listing_dir / name).write_text("\n".join(lines) + "\n", encoding="utf-8")
                    listed[device_path] = self.parse_list(lines)
                    self.record["listings"][device_path] = listed[device_path]
            entries = listed[device_path] or []
            for e in entries:
                if e["dir"] or not action or (pattern and not pattern.search(e["name"])):
                    continue
                child = f"{device_path}/{e['name']}"
                if action == "keep":
                    self.fetch(child)
                else:
                    self.hash_remote(child)
        if self.ftp is not None:
            try:
                self.ftp.quit()
            except (OSError, EOFError, ftplib.Error):
                pass

    def compare(self, manifest_path):
        """Each hashed device file against the package it should come from."""
        manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
        files = manifest.get("files", {})
        self.record["package"] = {k: manifest.get(k) for k in ("version", "source_revision", "defaults", "install_settings")
                                  if k in manifest}
        checks = {}
        for device_path, got in self.record["hashed"].items():
            pkg = None
            for prefix, target in PACKAGE_PATHS.items():
                if device_path == prefix:
                    pkg = target
                elif prefix.endswith("/") and device_path.startswith(prefix):
                    pkg = target + device_path[len(prefix):]
            want = files.get(pkg) if pkg else None
            checks[device_path] = {"package_path": pkg, "expected": want,
                                   "match": None if want is None else want["sha256"] == got["sha256"]
                                   and want["bytes"] == got["bytes"]}
        # shipped files that the device lacks
        for pkg, want in files.items():
            for prefix, target in PACKAGE_PATHS.items():
                device_path = (prefix + pkg[len(target):]) if target.endswith("/") and pkg.startswith(target) else \
                    prefix if pkg == target else None
                if device_path and device_path not in self.record["hashed"]:
                    checks[device_path] = {"package_path": pkg, "expected": want, "match": False, "absent": True}
        dictionary = files.get("ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict")
        listed = {e["name"]: e for e in self.record["listings"].get(f"{DATA}/dictionaries") or []}
        if dictionary:
            got = listed.get("plaintext.vjdict")
            checks[f"{DATA}/dictionaries/plaintext.vjdict"] = {
                "package_path": "ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict", "expected": dictionary,
                "match": bool(got) and got["bytes"] == dictionary["bytes"], "by": "size (LIST)", **({} if got else {"absent": True})}
        self.record["package_check"] = checks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("out", type=Path)
    ap.add_argument("--manifest", type=Path, help="package-manifest.json of the installed package")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()
    try:
        socket.create_connection((args.host, args.port), 5).close()
    except OSError:
        print(f"Cannot reach {args.host}:{args.port}. In VitaShell press SELECT to start FTP, and keep the screen on.")
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    c = Collector(args.host, args.port, args.out, args.timeout)
    c.run()
    if args.manifest and args.manifest.is_file():
        c.compare(args.manifest)
    (args.out / "COLLECT.json").write_text(json.dumps(c.record, indent=2) + "\n", encoding="utf-8")
    r = c.record
    print(f"collected {len(r['files'])} file(s) into {args.out} ({len(r['missing'])} not on the Vita, "
          f"{len(r['errors'])} error(s)); hashed {len(r['hashed'])}; API keys redacted: "
          f"{', '.join(r['redacted_keys']) or 'none were set'}")
    for e in r["errors"]:
        print("  error: " + e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
