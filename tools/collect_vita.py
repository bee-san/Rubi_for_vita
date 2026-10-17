#!/usr/bin/env python3
"""Collect everything the overlay wrote on the Vita, read-only, into one folder (install_ftp.sh --collect).

    python3 tools/collect_vita.py VITA_IP PORT OUT_DIR [--manifest package-manifest.json]
    python3 tools/collect_vita.py VITA_IP PORT OUT_DIR --archive-dumps [--plan]

Downloads, over VitaShell's FTP (VitaShell: SELECT button set to FTP in its settings, then SELECT),
with curl as the installer does:
  ux0:data/VitaJPOverlay/  log.txt log.old.txt selftest-report.txt selftest.txt status.txt
                           status.prev.txt kernel.txt kernel.prev.txt config.ini (API keys and
                           the audio source URL redacted before anything is written) games.ini
                           region.ini anki_queue/* (every card, screenshot, temp and duplicate file)
                           and anki_queue/exported/* (the cards the Overlay app exported); anki/ (its
                           .apkg packages) is listed only
                           vjo_app.txt (the record of the Overlay app the plugins came with)
                           installed.txt (the one-VPK install's record, core/payload.h)
  ur0:data/VitaJPOverlay/  boot.txt
  ux0:app/VJOSET001/       vjo_app.txt (the installed Overlay app's stamp, core/app_stamp.h); its eboot.bin
                           is hashed, never kept. Both are read as install_ftp.sh --check-app reads them
                           (read_app): through the folder, then by the file's full path (#138)
  ur0:tai/, ux0:tai/       config.txt, the installer's config.txt.vjo-bak, every dated backup
                           (config.txt.vjo-YYYYMMDD-HHMMSS.bak) and an interrupted swap's
                           config.txt.vjo-new / .vjo-old (settings/install.c)
  ux0:data/                every *.psp2dmp crash dump
The directories are listed (Python's ftplib, LIST as --dumps does) into listings/; the
installed plugins, the RCO, the settings VPK, every OCR model, the beam + LM decoder's LMs,
their LM-ATTRIBUTION.md and the model sizes' MODEL-ATTRIBUTION.md are hashed (downloaded to a
temporary file and deleted again, never kept) and compared with package-manifest.json when given.
Every file keeps its device path under OUT_DIR (ux0/data/VitaJPOverlay/log.txt, ...).
COLLECT.json lists what was fetched (bytes, sha256), what was missing and every error, in
"app" how each of the app's two files was read (collect_app), and each listed file's LIST time
by the Vita's clock ("date"), from which analyse_selftest.py --collect dates the crash dumps
against the boot (docs/self-test.md#crash-dumps).

Nothing on the Vita is written, renamed or deleted: only CWD, LIST, SIZE and RETR are sent.
A file that cannot be read is recorded and skipped; the exit status is 1 only when the Vita
cannot be reached at all.

--archive-dumps, run apart from a collect (install_ftp.sh asks first, or its own --archive-dumps),
is the one exception: it deletes from ux0:data the crash dumps that collect's analysis placed before
its boot, each only when the collect holds an intact copy (the bytes and SHA-256 its COLLECT.json
recorded) and the Vita's file is still that size, with DELE and nothing else, and lists ux0:data
again to see each one gone (archive_dumps). With --plan it lists them and sends nothing.
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
APPS = "ux0:/app"
APP = f"{APPS}/VJOSET001"  # the Overlay app (JPOverlaySettings.vpk)
# The installed app's stamp (kept, as FILES are) and its eboot.bin (hashed, never kept), the two files
# install_ftp.sh --check-app reads; collect_app reads them its way (read_app), not as FILES
APP_STAMP, APP_EBOOT = f"{APP}/vjo_app.txt", f"{APP}/eboot.bin"
FILES = [f"{DATA}/{n}" for n in ("log.txt", "log.old.txt", "selftest-report.txt", "selftest.txt", "status.txt",
                                  "status.prev.txt", "kernel.txt", "kernel.prev.txt", "config.ini", "games.ini",
                                  "region.ini", "vjo_app.txt", "installed.txt")]
FILES += ["ur0:/data/VitaJPOverlay/boot.txt", "ur0:/tai/config.txt", "ur0:/tai/config.txt.vjo-bak",
          "ux0:/tai/config.txt", "ux0:/tai/config.txt.vjo-bak"]
# curl's exits for a file that is not there: 9, a CWD on the way refused (VitaShell's FTP enters a folder only if
# it can open it); 19 or 78, its SIZE or RETR refused (VitaShell's FTP also answers RETR with 550 when it cannot
# allocate its transfer buffer: rare, and read as not there too)
NOT_THERE = (9, 19, 78)
# The two ways read_app and install_ftp.sh's app_check read the app's files, in this order: curl's own, a CWD into
# each folder on the way (ux0:, app, VJOSET001) before SIZE and RETR; then, when that is answered "not there", no
# CWD at all (SIZE and RETR of "ux0:/app/VJOSET001/<file>": VitaShell's FTP takes a "ux0:/..." argument as a full
# path and only opens the file). test.3's collect (#138) could not enter ux0:/app/VJOSET001, by curl's CWDs or
# ftplib's, while the Shell read the stamp in it at the game starts of the same boot.
APP_READS = (("cwd", ()), ("nocwd", ("--ftp-method", "nocwd")))
# Listed, and what to do with their files: "keep" downloads them, "hash" hashes them only.
# The ocr folder holds what the package ships there: the weights (*.vocr), the LMs (*.vlm, #89), their
# attribution, LM-ATTRIBUTION.md, and the model sizes' attribution, MODEL-ATTRIBUTION.md (package_release.py).
DIRS = [(f"{DATA}/anki_queue", "keep", None), (f"{DATA}/anki_queue/exported", "keep", None),
        (f"{DATA}/anki", None, None), (DATA, None, None),
        (f"{DATA}/ocr", "hash", re.compile(r"\.(vocr|vlm)$|^(LM|MODEL)-ATTRIBUTION\.md$")),
        (f"{DATA}/dictionaries", None, None), ("ux0:/data", "keep", re.compile(r"\.psp2dmp$", re.I)),
        ("ur0:/tai", "hash", re.compile(r"^VitaJPOverlay_.*\.(skprx|suprx)$")),
        ("ur0:/data/VitaJPOverlay", "hash", re.compile(r"\.rco$")), ("ux0:/data", "hash", re.compile(r"^JPOverlaySettings\.vpk$"))]
# the taiHEN configurations' backups (both installers: dated, kept) and an interrupted swap's leftovers
# (the Overlay app's: settings/install.c), beside each config.txt
TAI_BACKUPS = re.compile(r"^config\.txt\.vjo-.+$")  # dated, -old/-new, .vjo-bad, .vjo-new/.vjo-old (settings/install.c)
DIRS += [("ur0:/tai", "keep", TAI_BACKUPS), ("ux0:/tai", "keep", TAI_BACKUPS)]
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
# A line whose first word is one of them, whatever follows it (=, :, a space: the C parser of builds before
# audit-pr180-b's N3 fix logged a line without = whole, as "ignored line '...'"; it names the line's number now, and
# earlier builds' logs stay on the card), commented out (; or #) or not, is a secret line.
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
    def curl(self, device_path, dest, *options):
        """RETR through curl, as install_ftp.sh's get() does; (0, "") or (curl's exit, its message). The name is
        URL-quoted: curl decodes it again, so the server gets the exact name (spaces, #, ?, % included). options:
        more of curl's (read_app's --ftp-method nocwd)."""
        dest.parent.mkdir(parents=True, exist_ok=True)
        limit = DUMP_TIMEOUT if device_path.lower().endswith(".psp2dmp") else self.timeout
        url = f"ftp://{self.host}:{self.port}/" + urllib.parse.quote(device_path, safe="/:")
        r = subprocess.run(["curl", "--fail", "-s", "--show-error", "--connect-timeout", "15", "--max-time", str(limit),
                            *options, "-o", str(dest), url], capture_output=True, text=True,
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
        """VitaShell's LIST (`%c%s 1 vita vita %u %s %-2d %s %s`, libftpvita gen_list_format): the size; the time, by
        the Vita's clock, as listed ("Oct 10 06:38": the hour and minute when the file's year is the Vita's current
        one, else the year, "Oct 10 2025"); and the name, the rest after the eighth field and one space, its own
        spaces kept."""
        entries = []
        for line in lines:
            m = re.match(r"^(\S+)\s+\S+\s+\S+\s+\S+\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+) (.*)$", line.rstrip("\r\n"))
            if not m:
                continue
            day = str(int(m.group(4))) if m.group(4).isdigit() else m.group(4)
            entries.append({"name": m.group(6), "bytes": int(m.group(2)) if m.group(2).isdigit() else None,
                            "dir": line.startswith("d"), "date": f"{m.group(3)} {day} {m.group(5)}"})
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
        if rc in NOT_THERE:
            self.record["missing"].append(device_path)
        else:
            self.record["errors"].append(f"{device_path}: curl exit {rc}: {err}")
        return False

    @staticmethod
    def digest(path):
        """{"bytes", "sha256"} of a local file, read a block at a time."""
        h = hashlib.sha256()
        with path.open("rb") as s:
            for block in iter(lambda: s.read(1 << 20), b""):
                h.update(block)
        return {"bytes": path.stat().st_size, "sha256": h.hexdigest()}

    def sha256_remote(self, device_path):
        """{"bytes", "sha256"} of a device file, downloaded to a temporary file and deleted again; None (the
        error recorded) when it cannot be read."""
        with tempfile.TemporaryDirectory(prefix="vjo-collect-") as tmp:
            tmpfile = Path(tmp) / "f"
            rc, err = self.curl(device_path, tmpfile)
            if rc:
                self.record["errors"].append(f"{device_path}: not hashed, curl exit {rc}: {err}")
                return None
            return self.digest(tmpfile)

    def hash_remote(self, device_path):
        got = self.sha256_remote(device_path)
        if got:
            self.record["hashed"][device_path] = got

    def read_app(self, device_path, dest):
        """One of the installed app's two files into dest, read as install_ftp.sh's app_check reads it for
        --check-app: each way of APP_READS in turn, the next only when one is answered "not there". {"state":
        "read" | "absent" | "error", "method": the way that read it or failed with an error (None: absent),
        "rc": curl's exit for each way tried, "error": curl's message (an error)}"""
        rcs = []
        for method, options in APP_READS:
            rc, err = self.curl(device_path, dest, *options)
            rcs.append(rc)
            if rc == 0:
                return {"state": "read", "method": method, "rc": rcs}
            if rc not in NOT_THERE:
                return {"state": "error", "method": method, "rc": rcs, "error": err}
        return {"state": "absent", "method": None, "rc": rcs}

    def collect_app(self):
        """The installed Overlay app (core/app_stamp.h), its two files read as --check-app reads them (read_app),
        never through a listing of its folder: its stamp, kept as FILES are, and its eboot.bin, hashed and never
        kept (apart from "hashed", whose files the package lists one by one). When neither can be read, the
        folders say why, for the analysis: its own, listed when it can be entered (its files are then not in it),
        else ux0:/app, of which only whether it lists VJOSET001 is recorded (not the other titles)."""
        app = self.record["app"] = {"reads": {}, "eboot": None}
        dest = local_path(self.out, APP_STAMP)
        r = app["reads"]["vjo_app.txt"] = self.read_app(APP_STAMP, dest)
        if r["state"] == "read":
            self.record["files"][APP_STAMP] = {"local": str(dest.relative_to(self.out)), **self.digest(dest)}
        elif r["state"] == "error":
            self.record["errors"].append(f"{APP_STAMP}: curl exit {r['rc'][-1]}: {r['error']}")
        with tempfile.TemporaryDirectory(prefix="vjo-collect-") as tmp:
            eboot = Path(tmp) / "eboot.bin"
            r = app["reads"]["eboot.bin"] = self.read_app(APP_EBOOT, eboot)
            if r["state"] == "read":
                app["eboot"] = self.digest(eboot)
            elif r["state"] == "error":
                self.record["errors"].append(f"{APP_EBOOT}: not hashed, curl exit {r['rc'][-1]}: {r['error']}")
        if any(r["state"] == "read" for r in app["reads"].values()):
            return
        status, lines = self.listdir(APP)
        app["folder"] = status
        if status == "ok":
            entries = self.record["listings"][APP] = self.parse_list(lines)
            app["eboot_listed"] = any(e["name"] == "eboot.bin" and not e["dir"] for e in entries)
            return
        if status == "error":
            app["folder_error"] = lines
        status, lines = self.listdir(APPS)
        app["apps"] = status
        if status == "ok":
            app["in_apps"] = any(e["name"] == "VJOSET001" and e["dir"] for e in self.parse_list(lines))
        else:
            app["apps_error"] = lines

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
        self.collect_app()
        if self.ftp is not None:
            try:
                self.ftp.quit()
            except (OSError, EOFError, ftplib.Error):
                pass

    def compare(self, manifest_path):
        """Each hashed device file against the package it should come from."""
        manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
        files = manifest.get("files", {})
        self.record["package_app"] = manifest.get("app")  # the package's Overlay app (package_release.py)
        payload = Path(manifest_path).with_name("payload-manifest.txt")
        if payload.is_file():  # the one-VPK install's manifest: installed.txt names its SHA-256 (core/payload.h)
            self.record["package_payload_manifest"] = hashlib.sha256(payload.read_bytes()).hexdigest()
        self.record["package"] = {k: masked(manifest[k]) if k in ("defaults", "install_settings") else manifest[k]
                                  for k in ("version", "variant", "source_revision", "defaults", "install_settings")
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


# The only names --archive-dumps deletes: the system's crash dump names, directly in ux0:data
ARCHIVABLE = re.compile(r"^psp2core-\d{1,12}-0x[0-9A-Fa-f]{1,16}-[A-Za-z0-9._-]{1,64}\.psp2dmp$", re.I)


def file_sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as s:
        for block in iter(lambda: s.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def archive_plan(folder):
    """The crash dumps the collect in `folder` placed before its boot, placed again from the collect itself
    (analyse_selftest.py's summarise_dumps on its COLLECT.json and listings, never an analysis.json that can be edited
    or an older analyser's), each with an intact copy in that collect (the bytes and SHA-256 its COLLECT.json recorded):
    (archive, keep, problem). archive: [{"name", "bytes", "sha256", "date", "crashed", "process"}]; keep: [(name, why)],
    the dumps that stay on the Vita; problem: why none can be archived, or None."""
    import analyse_selftest  # beside this file, in the package's tools/ too
    folder = Path(folder)
    try:
        record = json.loads((folder / "COLLECT.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return [], [], f"{folder} has no readable COLLECT.json ({type(e).__name__})"
    dumps = analyse_selftest.summarise_dumps(folder, record)
    if not dumps:
        return [], [], None
    if dumps.get("unlisted"):
        return [], [], f"that collect could not list ux0:data ({dumps['unlisted']})"
    files = record.get("files") or {}
    archive, keep = [], []
    for e in dumps["entries"]:
        name = e["name"]
        got = files.get(f"ux0:/data/{name}") or {}
        copy = local_path(folder, f"ux0:/data/{name}")
        if e["when"] != "before":
            fetched = "" if got and copy.is_file() else "; this collect could not fetch it"
            keep.append((name, "from this boot" + fetched if e["when"] == "this boot" else "not placed before this boot"))
        elif not (ARCHIVABLE.match(name) and safe_name(name)):
            keep.append((name, "not a crash dump name the archive deletes"))
        elif not got or not copy.is_file():
            keep.append((name, "this collect has no copy of it"))
        elif copy.stat().st_size != got.get("bytes") or file_sha256(copy) != got.get("sha256"):
            keep.append((name, "this collect's copy is not the file it fetched (its size or SHA-256 differs)"))
        elif e["bytes"] != got.get("bytes"):
            keep.append((name, f"the collect fetched {got.get('bytes'):,} B of its {e['bytes']} B"))
        elif not e["written"]:  # the LIST time the archive compares (the collect's own listings/ux0_data.txt)
            keep.append((name, "the collect's listing of ux0:data does not have it"))
        else:
            archive.append({"name": name, "bytes": got["bytes"], "sha256": got["sha256"], "date": e["written"],
                            "crashed": e["crashed"], "process": e["process"]})
    return archive, keep, None


def archive_dumps(host, port, folder, plan=False):
    """--archive-dumps: deletes from ux0:data each crash dump of archive_plan, after a new listing shows the Vita's file
    still the size of the collect's checked copy, with DELE alone, then lists ux0:data again to see it gone; appends
    what it did to the collect's archive.log. With plan, prints them and sends nothing: exit 0 when there is one to
    archive, 3 when there is none. Else 0 when each was archived, 2 when one was not or is not known, or ux0:data
    cannot be listed."""
    folder = Path(folder)
    if not plan:
        try:
            was = json.loads((folder / "COLLECT.json").read_text(encoding="utf-8")).get("host")
        except (OSError, ValueError):
            was = None
        if not was or str(was).rpartition(":")[0] != host:  # the address: VitaShell's FTP port is always the same
            print(f"crash dumps: nothing archived: {folder.name} is a collect of {was or 'an unknown host'}, not of "
                  f"{host}; collect this Vita first")
            return 2
    archive, keep, problem = archive_plan(folder)
    if problem:
        print(f"crash dumps: nothing archived: {problem}")
        return 3 if plan else 2
    for name, why in keep:
        if why == "from this boot":
            print(f"crash dump from this boot, left on the Vita (this collect has it): {name}")
        elif why.startswith("from this boot"):
            print(f"crash dump from this boot, left on the Vita ({why[len('from this boot; '):]}): {name}")
    others = [(n, w) for n, w in keep if not w.startswith("from this boot")]
    if not plan:
        for name, why in others:
            print(f"crash dump left on the Vita ({why}): {name}")
    if plan:
        if archive:
            print(f"{len(archive)} crash dump(s) from before this boot are on the Vita "
                  f"({sum(d['bytes'] for d in archive):,} B); this collect keeps a checked copy of each "
                  f"({local_path(folder, 'ux0:/data')}):")
            for d in archive:
                print(f"  {d['name']} ({d['bytes']:,} B, {d['process']}, {d['crashed']})")
        for name, why in others:
            print(f"crash dump left on the Vita ({why}): {name}")
        return 0 if archive else 3
    if not archive:
        print("crash dumps: none from before this boot to archive" + (f" ({len(others)} left: "
              + "; ".join(f"{n}: {w}" for n, w in others) + ")" if others else ""))
        return 0
    c = Collector(host, port, folder)
    status, lines = c.listdir("ux0:/data")
    if status != "ok":
        print(f"cannot list ux0:data ({lines}): nothing deleted")
        return 2
    on_vita = {e["name"]: e for e in c.parse_list(lines)}
    print(f"archiving {len(archive)} crash dump(s) from before this boot off the Vita (this collect keeps them: "
          f"{local_path(folder, 'ux0:/data')}):")
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    log, kept, gone, sent = [f"{stamp} left {n} ({w})" for n, w in keep], 0, 0, []
    for d in archive:
        e = on_vita.get(d["name"])
        if e is None:
            print(f"  already gone: {d['name']}")
            log.append(f"{stamp} gone {d['name']} (not on the Vita any more)")
            gone += 1
            continue
        if e["dir"] or e["bytes"] != d["bytes"] or e.get("date") != d["date"]:
            why = (f"on the Vita it is {e['bytes']} B, {e.get('date')} now, not the collect's {d['bytes']:,} B, "
                   f"{d['date']}")
            print(f"  kept: {d['name']} ({why})")
            log.append(f"{stamp} kept {d['name']} ({why})")
            kept += 1
            continue
        try:
            resp = c.connect().sendcmd(f"DELE /ux0:/data/{d['name']}")  # VitaShell's FTP answers 226
        except ftplib.Error as err:  # a refusal (550): the connection is fine
            resp = f"{type(err).__name__}: {err}"
        except (OSError, EOFError) as err:  # a dropped connection: the DELE may have been done; the next one reconnects
            resp, c.ftp = None, None
            d["lost"] = f"{type(err).__name__}: {err}"
        if resp is None or resp.startswith("2"):
            sent.append(d)
        else:
            print(f"  kept: {d['name']} (DELE answered {resp})")
            log.append(f"{stamp} kept {d['name']} (DELE answered {resp})")
            kept += 1
    # the second listing, not DELE's answer, says what is gone: also of a DELE whose answer the connection lost
    status, lines = c.listdir("ux0:/data")
    left = {e["name"] for e in c.parse_list(lines)} if status == "ok" else None
    deleted = unknown = 0
    for d in sent:
        lost = d.get("lost")
        if left is not None and d["name"] in left:
            why = f"its DELE's answer was lost ({lost}), and" if lost else "DELE was answered, but"
            print(f"  kept: {d['name']} ({why} ux0:data still lists it)")
            log.append(f"{stamp} kept {d['name']} (still listed after DELE" + (f", its answer lost: {lost}" if lost
                                                                               else "") + ")")
            kept += 1
            continue
        if left is None and lost:
            print(f"  not known: {d['name']} (its DELE's answer was lost, and ux0:data could not be listed again)")
            log.append(f"{stamp} unknown {d['name']} (its DELE's answer lost: {lost}; not listed again)")
            unknown += 1
            continue
        deleted += 1
        print(f"  deleted ux0:data/{d['name']} ({d['bytes']:,} B)" + (" (ux0:data could not be listed again)"
                                                                    if left is None else
                                                                    " (its DELE's answer was lost)" if lost else ""))
        log.append(f"{stamp} deleted ux0:data/{d['name']} ({d['bytes']} B, sha256 {d['sha256']}, "
                   f"{d['process']}, crashed {d['crashed']})")
    if c.ftp is not None:
        try:
            c.ftp.quit()
        except (OSError, EOFError, ftplib.Error):
            pass
    with (folder / "archive.log").open("a", encoding="utf-8") as f:
        f.write("".join(line + "\n" for line in log))
    print(f"archived: {deleted} deleted from the Vita" + (f", {gone} already gone" if gone else "")
          + (f", {kept} left on it" if kept else "")
          + (f", {unknown} not known (a collect shows whether they are still there)" if unknown else "")
          + f"; {folder / 'archive.log'} lists them")
    return 2 if kept or unknown else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("out", type=Path)
    ap.add_argument("--manifest", type=Path, help="package-manifest.json of the installed package")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--archive-dumps", action="store_true",
                    help="delete from ux0:data the crash dumps the collect in OUT_DIR placed before its boot")
    ap.add_argument("--plan", action="store_true", help="with --archive-dumps: list them only, send nothing")
    args = ap.parse_args()
    if args.archive_dumps and args.plan:
        return archive_dumps(args.host, args.port, args.out, plan=True)
    try:
        socket.create_connection((args.host, args.port), 5).close()
    except OSError:
        print(f"Cannot reach {args.host}:{args.port}. In VitaShell press SELECT to start FTP (with SELECT button set to "
              "FTP in VitaShell's settings, Start: a Vita's default is USB), and keep the screen on.")
        return 1
    if args.archive_dumps:
        return archive_dumps(args.host, args.port, args.out)
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
