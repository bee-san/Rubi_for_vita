#!/usr/bin/env python3
"""Collect everything the overlay wrote on the Vita, read-only, into one folder (install_ftp.sh --collect).

    python3 tools/collect_vita.py VITA_IP PORT OUT_DIR [--manifest package-manifest.json]

Downloads, over VitaShell's FTP (VitaShell: SELECT -> FTP), with curl as the installer does:
  ux0:data/VitaJPOverlay/  log.txt log.old.txt selftest-report.txt selftest.txt status.txt
                           status.prev.txt kernel.txt kernel.prev.txt config.ini (API keys and
                           the audio source URL redacted before anything is written) games.ini
                           region.ini anki_queue/* (every card, screenshot, temp and duplicate file)
                           and anki_queue/exported/* (the cards the Overlay app exported); anki/ (its
                           .apkg packages) is listed only
  ur0:data/VitaJPOverlay/  boot.txt
  ur0:tai/, ux0:tai/       config.txt and the installer's config.txt.vjo-bak
  ux0:data/                every *.psp2dmp crash dump
The directories are listed (Python's ftplib, LIST as --dumps does) into listings/; the
installed plugins, the RCO, the settings VPK, every OCR model, the beam + LM decoder's LMs
and their LM-ATTRIBUTION.md are hashed (downloaded to a temporary file and deleted again, never
kept) and compared with package-manifest.json when given. Every file keeps its device path
under OUT_DIR (ux0/data/VitaJPOverlay/log.txt, ...).
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
import urllib.parse
from pathlib import Path

DATA = "ux0:/data/VitaJPOverlay"
FILES = [f"{DATA}/{n}" for n in ("log.txt", "log.old.txt", "selftest-report.txt", "selftest.txt", "status.txt",
                                  "status.prev.txt", "kernel.txt", "kernel.prev.txt", "config.ini", "games.ini",
                                  "region.ini")]
FILES += ["ur0:/data/VitaJPOverlay/boot.txt", "ur0:/tai/config.txt", "ur0:/tai/config.txt.vjo-bak",
          "ux0:/tai/config.txt", "ux0:/tai/config.txt.vjo-bak"]
# Listed, and what to do with their files: "keep" downloads them, "hash" hashes them only.
# The ocr folder holds what the package ships there: the weights (*.vocr), the LMs (*.vlm, #89) and
# their attribution, LM-ATTRIBUTION.md (package_release.py).
DIRS = [(f"{DATA}/anki_queue", "keep", None), (f"{DATA}/anki_queue/exported", "keep", None),
        (f"{DATA}/anki", None, None), (DATA, None, None),
        (f"{DATA}/ocr", "hash", re.compile(r"\.(vocr|vlm)$|^LM-ATTRIBUTION\.md$")),
        (f"{DATA}/dictionaries", None, None), ("ux0:/data", "keep", re.compile(r"\.psp2dmp$", re.I)),
        ("ur0:/tai", "hash", re.compile(r"^VitaJPOverlay_.*\.(skprx|suprx)$")),
        ("ur0:/data/VitaJPOverlay", "hash", re.compile(r"\.rco$")), ("ux0:/data", "hash", re.compile(r"^JPOverlaySettings\.vpk$"))]
# Device paths of the hashed files -> their package path (package-manifest.json "files")
PACKAGE_PATHS = {"ur0:/tai/": "ur0/tai/", "ur0:/data/VitaJPOverlay/": "ur0/data/VitaJPOverlay/",
                 f"{DATA}/ocr/": "ux0/data/VitaJPOverlay/ocr/", "ux0:/data/JPOverlaySettings.vpk": "JPOverlaySettings.vpk"}
# The settings that can hold a credential: an API key, and the audio source's URL, which can carry one
# (package_release.py SECRET). anki_audio_url was removed with AnkiConnect, but a config.ini the
# installer has not rewritten can still hold it, so it stays redacted.
CREDENTIAL = re.compile(r"api_key$|^anki_audio_url$", re.I)
CREDENTIAL_NAME = r"[A-Za-z0-9_]*api_key|anki_audio_url"
# The removed online dictionaries' API keys (api_key: jpdb's older name): a word that only STARTS with
# one is theirs too (a typo, "jpdb_api_keyKEY"), as core/config.c and the installer treat it.
REMOVED_KEY = r"(?:jpdb_api_key|jiten_api_key|api_key)"
NAME = r"(" + REMOVED_KEY + r"|(?:" + CREDENTIAL_NAME + r")(?![A-Za-z0-9_]))"
# A line whose first word is one of them, whatever follows it (=, :, a space: the C parser logs a
# line without = whole, as "ignored line '...'"), commented out (; or #) or not, is a secret line.
SECRET = re.compile(r"^([\ufeff\s]*(?:[;#][;#\s]*)?)" + NAME + r"(\s*[=:]?\s*)(.*?)(\s*)$", re.I)
# In the logs: anything after such a name on the same line, up to a quote (config warnings).
LOG_SECRET = re.compile(r"(?i)((?<![A-Za-z0-9_])" + REMOVED_KEY + r"|(?:" + CREDENTIAL_NAME + r")(?![A-Za-z0-9_]))"
                        r"[^'\n]*")
LOG_FILES = re.compile(r"/(log|log\.old|status|status\.prev|kernel|kernel\.prev)\.txt$")
DUMP_TIMEOUT = 900  # a crash dump can be tens of MB over the Vita's Wi-Fi


def local_path(out, device_path):
    dev, _, rest = device_path.partition(":/")
    return out / dev / rest


def redact(text):
    """config.ini with every credential's value (SECRET) replaced by its length; the keys that were set. Lines are split
    on \\n only, as the plugin's parser does, so no other character can carry a value onto a line of its own."""
    out, keys = [], []
    for raw in text.split("\n"):
        cr = "\r" if raw.endswith("\r") else ""
        line = raw[:-1] if cr else raw
        m = SECRET.match(line)
        if m and m.group(4):
            keys.append(m.group(2).lower())
            line = f"{m.group(1)}{m.group(2)}{m.group(3)}<redacted: {len(m.group(4))} characters>"
        out.append(line + cr)
    return "\n".join(out), keys


def redact_log(data):
    """A log or status file: the text after a credential's name on a line masked (the parser's warnings
    quote lines)."""
    text = data.decode("utf-8", "replace")
    if not LOG_SECRET.search(text):
        return data, 0
    text, n = LOG_SECRET.subn(lambda m: m.group(1) + " <redacted>", text)
    return text.encode("utf-8"), n


def masked(settings):
    """A package's settings (its manifest's defaults or install_settings) with each credential only as
    "set" or "": COLLECT.json is sent on, and an older package's manifest holds the values."""
    if not isinstance(settings, dict):
        return settings
    return {k: ("set" if v not in ("", None) else "") if CREDENTIAL.search(str(k)) else v for k, v in settings.items()}


def safe_name(name):
    """A directory entry the server listed: never a path of its own."""
    return bool(name) and name not in (".", "..") and "/" not in name and "\\" not in name and "\0" not in name


class Collector:
    def __init__(self, host, port, out, timeout=60):
        self.host, self.port, self.out, self.timeout = host, port, out, timeout
        self.record = {"collected_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "host": f"{host}:{port}",
                       "tool": "tools/collect_vita.py", "files": {}, "missing": [], "errors": [], "listings": {},
                       "hashed": {}, "redacted_keys": []}
        self.ftp = None

    # ---- transport ----
    def curl(self, device_path, dest):
        """RETR through curl, as install_ftp.sh's get() does; (0, "") or (curl's exit, its message). The name is
        URL-quoted: curl decodes it again, so the server gets the exact name (spaces, #, ?, % included)."""
        dest.parent.mkdir(parents=True, exist_ok=True)
        limit = DUMP_TIMEOUT if device_path.lower().endswith(".psp2dmp") else self.timeout
        url = f"ftp://{self.host}:{self.port}/" + urllib.parse.quote(device_path, safe="/:")
        r = subprocess.run(["curl", "--fail", "-s", "--show-error", "--connect-timeout", "15", "--max-time", str(limit),
                            "-o", str(dest), url], capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, timeout=limit + 30)
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
        """VitaShell's LIST (`%c%s 1 vita vita %u %s %-2d %s %s`): the name is the rest after the eighth
        field and one space, its own spaces kept."""
        entries = []
        for l in lines:
            m = re.match(r"^(\S+)\s+\S+\s+\S+\s+\S+\s+(\S+)\s+\S+\s+\S+\s+\S+ (.*)$", l.rstrip("\r\n"))
            if not m:
                continue
            entries.append({"name": m.group(3), "bytes": int(m.group(2)) if m.group(2).isdigit() else None,
                            "dir": l.startswith("d")})
        return entries

    # ---- collection ----
    def fetch(self, device_path):
        dest = local_path(self.out, device_path)
        if not dest.resolve().is_relative_to(self.out.resolve()):
            self.record["errors"].append(f"{device_path}: not fetched: its name leaves the collect folder")
            return False
        if device_path.endswith("/config.ini"):
            with tempfile.TemporaryDirectory(prefix="vjo-collect-") as tmp:
                raw = Path(tmp) / "config.ini"
                rc, err = self.curl(device_path, raw)
                if rc:
                    return self.absent(device_path, rc, err)
                # bytes, not text mode: universal newlines would turn a lone \r inside a value into a line
                text, keys = redact(raw.read_bytes().decode("utf-8-sig", "replace"))
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(text.encode("utf-8"))
            self.record["redacted_keys"] = keys
        else:
            rc, err = self.curl(device_path, dest)
            if rc:
                return self.absent(device_path, rc, err)
            if LOG_FILES.search(device_path):
                data, n = redact_log(dest.read_bytes())
                if n:
                    dest.write_bytes(data)
                    self.record.setdefault("redacted_log_lines", {})[device_path] = n
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
                if not safe_name(e["name"]):
                    self.record["errors"].append(f"{device_path}/{e['name']!r}: not fetched: not a plain file name")
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
        self.record["package"] = {k: masked(manifest[k]) if k in ("defaults", "install_settings") else manifest[k]
                                  for k in ("version", "source_revision", "defaults", "install_settings") if k in manifest}
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
